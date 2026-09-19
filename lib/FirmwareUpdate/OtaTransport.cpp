#include "OtaTransport.h"
#include "OtaPolicy.h"
#include "TrustedRoots.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <lwip/dns.h>
#include <lwip/tcpip.h>
#include <mbedtls/ssl_internal.h>
#include <new>
#include <strings.h>

namespace FirmwareUpdate
{
namespace
{
constexpr uint32_t DNS_TIMEOUT_MS = 10000;
constexpr uint32_t CONNECT_TIMEOUT_MS = 10000;
constexpr uint32_t HANDSHAKE_TIMEOUT_MS = 10000;
constexpr uint32_t WRITE_TIMEOUT_MS = 5000;
constexpr uint32_t READ_TIMEOUT_MS = 60000;
constexpr uint32_t HEADER_TIMEOUT_MS = 15000;
constexpr size_t MAX_HEADER_BYTES = 16384;
constexpr size_t MAX_HEADER_LINE = 4096;
constexpr size_t MIN_TLS_HEAP_BYTES = 64 * 1024;
constexpr size_t MIN_TLS_HEAP_BLOCK = MBEDTLS_SSL_IN_BUFFER_LEN > MBEDTLS_SSL_OUT_BUFFER_LEN
	? MBEDTLS_SSL_IN_BUFFER_LEN : MBEDTLS_SSL_OUT_BUFFER_LEN;

bool hasTlsMemory()
{
	// TLS needs separate record buffers, not one contiguous 48 KiB allocation.
	return heap_caps_get_free_size(MALLOC_CAP_8BIT) >= MIN_TLS_HEAP_BYTES &&
		heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) >= MIN_TLS_HEAP_BLOCK;
}

uint32_t smaller(uint32_t a, uint32_t b) { return a < b ? a : b; }

// The callback arguments have process lifetime: cancellation/timeouts never
// leave lwIP holding a pointer into a finished worker operation's stack.
struct DnsLookup
{
	DnsLookup(const char *hostname) : host(hostname) {}

	const char *host;
	bool pending = false;
	bool done = false;
	err_t result = ERR_INPROGRESS;
	ip_addr_t address = {};
};

portMUX_TYPE dnsLock = portMUX_INITIALIZER_UNLOCKED;
DnsLookup dnsLookups[] = {
	{"github.com"},
	{"release-assets.githubusercontent.com"},
	{"objects.githubusercontent.com"}
};

void dnsCompleted(const char *, const ip_addr_t *address, void *argument)
{
	DnsLookup *lookup = static_cast<DnsLookup *>(argument);
	portENTER_CRITICAL(&dnsLock);
	if (address)
		lookup->address = *address;
	lookup->result = address && IP_IS_V4(address) && !ip4_addr_isany_val(*ip_2_ip4(address))
		? ERR_OK : ERR_VAL;
	lookup->done = true;
	lookup->pending = false;
	portEXIT_CRITICAL(&dnsLock);
}

void startDns(void *argument)
{
	DnsLookup *lookup = static_cast<DnsLookup *>(argument);
	ip_addr_t address = {};
	const err_t result = dns_gethostbyname_addrtype(lookup->host, &address,
		dnsCompleted, lookup, LWIP_DNS_ADDRTYPE_IPV4);
	if (result == ERR_OK)
		dnsCompleted(nullptr, &address, lookup);
	else if (result != ERR_INPROGRESS)
		dnsCompleted(nullptr, nullptr, lookup);
}

bool resolveHost(Operation &operation, const char *host, IPAddress &address)
{
	DnsLookup *lookup = nullptr;
	for (DnsLookup &candidate : dnsLookups)
		if (strcmp(candidate.host, host) == 0)
			lookup = &candidate;
	if (!lookup || !operation.checkpoint())
		return false;
	bool start;
	portENTER_CRITICAL(&dnsLock);
	start = !lookup->pending;
	if (start)
	{
		lookup->pending = true;
		lookup->done = false;
	}
	portEXIT_CRITICAL(&dnsLock);
	if (start && tcpip_try_callback(startDns, lookup) != ERR_OK)
	{
		portENTER_CRITICAL(&dnsLock);
		lookup->pending = false;
		portEXIT_CRITICAL(&dnsLock);
		return operation.fail(Error::OutOfMemory);
	}
	const uint32_t started = millis();
	while (operation.checkpoint())
	{
		bool done;
		err_t result;
		ip_addr_t resolved;
		portENTER_CRITICAL(&dnsLock);
		done = lookup->done;
		result = lookup->result;
		resolved = lookup->address;
		portEXIT_CRITICAL(&dnsLock);
		if (done)
		{
			if (result != ERR_OK)
				return operation.fail(Error::ConnectionFailed);
			address = ip4_addr_get_u32(ip_2_ip4(&resolved));
			return true;
		}
		if (millis() - started >= DNS_TIMEOUT_MS)
			return operation.fail(Error::TimedOut);
		vTaskDelay(pdMS_TO_TICKS(10));
	}
	return false;
}

class VerifiedClient : public WiFiClientSecure
{
public:
	explicit VerifiedClient(Operation &operation) : operation_(operation) {}

	void stop() override
	{
		WiFiClientSecure::stop();
		// Arduino 2.0.17 zeroes the context during cleanup. Repeated HTTP/TLS
		// cleanup must not close the unrelated descriptor 0.
		sslclient->socket = -1;
	}

	int connect(const char *host, uint16_t port, int32_t) override
	{
		if (port != 443 || !operation_.checkpoint())
			return 0;
		IPAddress address;
		if (!resolveHost(operation_, host, address))
			return 0;
		const uint32_t remaining = operation_.remaining();
		if (remaining < 2)
		{
			operation_.fail(Error::TimedOut);
			return 0;
		}
		const uint32_t connectBudget = smaller(CONNECT_TIMEOUT_MS, remaining / 2);
		_timeout = static_cast<int>(connectBudget);
		sslclient->handshake_timeout = smaller(HANDSHAKE_TIMEOUT_MS, remaining - connectBudget);
		const char *roots = strcmp(host, "github.com") == 0
			? Trust::GITHUB_ROOTS : Trust::ASSET_ROOTS;
		setCACert(roots);
		// This SDK overload preserves SNI and certificate hostname validation while
		// using the separately resolved address; connecting by IP alone would not.
		const int connected = WiFiClientSecure::connect(address, port, host, roots, nullptr, nullptr);
		if (!operation_.checkpoint())
		{
			stop();
			return 0;
		}
		if (!connected)
			operation_.fail(Error::ConnectionFailed);
		return connected;
	}

	size_t write(const uint8_t *buffer, size_t length) override
	{
		if (!operation_.checkpoint())
			return 0;
		sslclient->socket_timeout = smaller(WRITE_TIMEOUT_MS, operation_.remaining());
		return WiFiClientSecure::write(buffer, length);
	}

private:
	Operation &operation_;
};

class RequestClient : public HTTPClient
{
public:
	bool start(Operation &operation, VerifiedClient &client, const Policy::Url &url)
	{
		setReuse(false);
		useHTTP10(true);
		setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
		setConnectTimeout(CONNECT_TIMEOUT_MS);
		setTimeout(READ_TIMEOUT_MS);
		setUserAgent("GT7Dash-OTA/1");
		begin(client, url.host, 443, url.path, true);
		if (_host != url.host || _uri != url.path || _userAgent != "GT7Dash-OTA/1")
			return operation.fail(Error::OutOfMemory);
		if (!connect() || !operation.checkpoint() || !sendHeader("GET"))
			return operation.fail(Error::ConnectionFailed);
		return operation.checkpoint();
	}
};

bool isRedirect(int code)
{
	return code == 301 || code == 302 || code == 303 || code == 307 || code == 308;
}
}

struct HttpsDownload::Impl
{
	explicit Impl(Operation &context) : operation(context), client(context) {}
	~Impl() { close(); }

	void close()
	{
		// Do not let HTTPClient drain a rejected or cancelled response body.
		client.stop();
		request.end();
	}

	// HTTPClient's stock header reader grows Strings until newline and has only
	// an idle timeout. Keep its request/TLS implementation, but parse the small
	// response header with explicit line, aggregate, and wall-clock limits.
	bool readLine(char (&line)[MAX_HEADER_LINE], size_t &headerBytes, uint32_t headerStarted)
	{
		size_t length = 0;
		while (operation.checkpoint())
		{
			if (millis() - headerStarted >= HEADER_TIMEOUT_MS)
				return operation.fail(Error::TimedOut);
			if (client.available() <= 0)
			{
				if (!client.connected())
					return operation.fail(Error::HttpFailed);
				if (millis() - lastRead >= READ_TIMEOUT_MS)
					return operation.fail(Error::TimedOut);
				vTaskDelay(pdMS_TO_TICKS(2));
				continue;
			}
			const int value = client.read();
			if (value < 0)
				continue;
			lastRead = millis();
			if (++headerBytes > MAX_HEADER_BYTES || length + 1 >= sizeof(line))
				return operation.fail(Error::HttpFailed);
			if (value == '\n')
			{
				if (!length || line[length - 1] != '\r')
					return operation.fail(Error::HttpFailed);
				line[length - 1] = '\0';
				return true;
			}
			if ((value < 32 && value != '\r' && value != '\t') || value > 126)
				return operation.fail(Error::HttpFailed);
			line[length++] = static_cast<char>(value);
		}
		return false;
	}

	bool headers()
	{
		// Use the reserved worker stack after TLS, not heap needed by its verifier.
		char line[MAX_HEADER_LINE];
		hasLength = false;
		length = 0;
		location[0] = '\0';
		received = 0;
		bool sawEncoding = false, sawTransfer = false, sawLocation = false;
		size_t headerBytes = 0;
		const uint32_t started = millis();
		lastRead = started;
		if (!readLine(line, headerBytes, started))
			return false;
		if ((strncmp(line, "HTTP/1.0 ", 9) != 0 && strncmp(line, "HTTP/1.1 ", 9) != 0) ||
			strlen(line) < 12 || line[9] < '1' || line[9] > '5' ||
			line[10] < '0' || line[10] > '9' || line[11] < '0' || line[11] > '9' ||
			(line[12] && line[12] != ' '))
			return operation.fail(Error::HttpFailed);
		operation.httpStatus = (line[9] - '0') * 100 + (line[10] - '0') * 10 + line[11] - '0';
		while (readLine(line, headerBytes, started))
		{
			if (!line[0])
				return true;
			char *colon = strchr(line, ':');
			if (!colon || colon == line || line[0] == ' ' || line[0] == '\t')
				return operation.fail(Error::HttpFailed);
			for (char *p = line; p < colon; ++p)
				if (*p <= 32 || *p >= 127)
					return operation.fail(Error::HttpFailed);
			*colon++ = '\0';
			while (*colon == ' ' || *colon == '\t')
				++colon;
			char *end = colon + strlen(colon);
			while (end > colon && (end[-1] == ' ' || end[-1] == '\t'))
				*--end = '\0';
			if (strchr(colon, '\r'))
				return operation.fail(Error::HttpFailed);
			if (strcasecmp(line, "Content-Length") == 0)
			{
				if (hasLength || !Policy::parseLength(colon, length))
					return operation.fail(Error::HttpFailed);
				hasLength = true;
			}
			else if (strcasecmp(line, "Location") == 0)
			{
				if (sawLocation || strlen(colon) > Policy::MAX_URL_BYTES)
					return operation.fail(Error::HttpFailed);
				sawLocation = true;
				memcpy(location, colon, strlen(colon) + 1);
			}
			else if (strcasecmp(line, "Transfer-Encoding") == 0)
			{
				// HTTP/1.0 + Connection: close requests an unencoded body. Reject
				// unexpected framing instead of hashing/writing chunk delimiters.
				if (sawTransfer || strcasecmp(colon, "identity") != 0)
					return operation.fail(Error::HttpFailed);
				sawTransfer = true;
			}
			else if (strcasecmp(line, "Content-Encoding") == 0)
			{
				if (sawEncoding || strcasecmp(colon, "identity") != 0)
					return operation.fail(Error::HttpFailed);
				sawEncoding = true;
			}
		}
		return false;
	}

	Operation &operation;
	VerifiedClient client;
	RequestClient request;
	char url[Policy::MAX_URL_BYTES + 1] = {};
	char location[Policy::MAX_URL_BYTES + 1] = {};
	uint32_t length = 0;
	uint32_t maximum = 0;
	uint32_t received = 0;
	uint32_t lastRead = 0;
	bool hasLength = false;
	bool metadata = false;
};

HttpsDownload::HttpsDownload(Operation &operation) : operation_(operation) {}
HttpsDownload::~HttpsDownload() { delete impl_; }

bool HttpsDownload::open(const char *filename, const char *releaseTag, uint32_t maximumBytes)
{
	if (!operation_.checkpoint())
		return false;
	if (!hasTlsMemory())
		return operation_.fail(Error::OutOfMemory);
	impl_ = new (std::nothrow) Impl(operation_);
	if (!impl_)
		return operation_.fail(Error::OutOfMemory);
	impl_->maximum = maximumBytes;
	impl_->metadata = releaseTag == nullptr;
	int size;
	if (releaseTag)
		size = snprintf(impl_->url, sizeof(impl_->url),
			"https://github.com/%s/releases/download/%s/%s",
			FirmwareUpdateConfig::REPOSITORY, releaseTag, filename);
	else
		size = snprintf(impl_->url, sizeof(impl_->url),
			"https://github.com/%s/releases/latest/download/%s",
			FirmwareUpdateConfig::REPOSITORY, filename);
	if (size <= 0 || static_cast<size_t>(size) >= sizeof(impl_->url))
		return operation_.fail(Error::InvalidManifest);
	for (unsigned redirects = 0; operation_.checkpoint(); ++redirects)
	{
		Policy::Url parsed;
		if (!Policy::parseUrl(impl_->url, parsed) ||
			!Policy::githubPathAllowed(parsed, filename, releaseTag))
			return operation_.fail(Error::HttpFailed);
		if (!impl_->request.start(operation_, impl_->client, parsed) || !impl_->headers())
			return false;
		if (isRedirect(operation_.httpStatus))
		{
			// Location and the old path occupy separate buffers; the bounded
			// result may safely replace the old URL after extracting its host.
			if (redirects >= Policy::MAX_REDIRECTS ||
				!Policy::resolveRedirect(parsed, impl_->location, impl_->url, sizeof(impl_->url)))
				return operation_.fail(Error::HttpFailed);
			impl_->close();
			continue;
		}
		if (operation_.httpStatus == 404)
			return operation_.fail(Error::ReleaseUnavailable);
		if (operation_.httpStatus != 200)
			return operation_.fail(Error::HttpFailed);
		if (impl_->hasLength && impl_->length > maximumBytes)
			return operation_.fail(impl_->metadata ? Error::MetadataTooLarge : Error::ImageTooLarge);
		if (!impl_->metadata && (!impl_->hasLength || impl_->length != maximumBytes))
			return operation_.fail(impl_->hasLength ? Error::TruncatedImage : Error::HttpFailed);
		return true;
	}
	return false;
}

size_t HttpsDownload::bodyCapacity() const
{
	return impl_ ? (impl_->hasLength ? impl_->length : impl_->maximum) : 0;
}

int HttpsDownload::read(uint8_t *buffer, size_t capacity)
{
	if (!impl_ || !buffer || !capacity)
	{
		operation_.fail(Error::HttpFailed);
		return -1;
	}
	while (operation_.checkpoint())
	{
		const int available = impl_->client.available();
		if (available > 0)
		{
			const size_t wanted = static_cast<size_t>(available) < capacity
				? static_cast<size_t>(available) : capacity;
			const int count = impl_->client.read(buffer, wanted);
			if (count <= 0)
				continue;
			impl_->lastRead = millis();
			const uint32_t total = impl_->received + static_cast<uint32_t>(count);
			if (total > impl_->maximum || (impl_->hasLength && total > impl_->length))
			{
				operation_.fail(impl_->metadata ? Error::MetadataTooLarge : Error::ImageTooLarge);
				return -1;
			}
			impl_->received = total;
			return count;
		}
		if (impl_->hasLength && impl_->received == impl_->length)
			return 0;
		if (!impl_->client.connected())
		{
			if (impl_->hasLength && impl_->received != impl_->length)
			{
				operation_.fail(Error::TruncatedImage);
				return -1;
			}
			return 0;
		}
		if (millis() - impl_->lastRead >= READ_TIMEOUT_MS)
		{
			operation_.fail(Error::TimedOut);
			return -1;
		}
		vTaskDelay(pdMS_TO_TICKS(2));
	}
	return -1;
}
}
