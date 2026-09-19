#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "../FirmwareUpdate.h"
#include "../FirmwareManifest.h"
// Expose only test-state access; the production API header is copied unmodified.
#define private public
#include "../OtaTransport.h"
#undef private

struct Host
{
	uint32_t now = 0;
	unsigned delays = 0;
	unsigned checkpoints = 0;
	unsigned drainAttempts = 0;
	std::function<void()> onDelay;
	std::vector<std::string> cleanup;
	std::vector<int> closedDescriptors;
	size_t freeHeap = 87000;
	size_t largestBlock = 45044;
	size_t advertisedCapacity = 535;
	size_t allocatedManifest = 0;
	size_t lastAllocation = 0;
	unsigned downloadDestructions = 0;
	bool tlsOpen = false;
	bool failOpen = false;
	bool failAllocation = false;
	bool compatibleLayout = true;
	FirmwareUpdate::Error manifestError = FirmwareUpdate::Error::None;
	std::string manifestBody = std::string(535, 'x');
};

Host host;
constexpr int MALLOC_CAP_8BIT = 1;
constexpr size_t MBEDTLS_SSL_IN_BUFFER_LEN = 16717;
constexpr size_t MBEDTLS_SSL_OUT_BUFFER_LEN = 16717;
size_t heap_caps_get_free_size(int) { return host.freeHeap; }
size_t heap_caps_get_largest_free_block(int) { return host.largestBlock; }

uint32_t millis() { return host.now; }
uint32_t pdMS_TO_TICKS(uint32_t milliseconds) { return milliseconds; }
void vTaskDelay(uint32_t ticks)
{
	assert(++host.delays < 10000);
	host.now += ticks;
	if (host.onDelay)
		host.onDelay();
}

namespace FirmwareUpdate
{
// @production:READ_TIMEOUT
// @production:MEMORY_LIMITS
// @production:MEMORY_GUARD
// @production:REMAINING
// @production:FAIL

// Only the board/game/Wi-Fi checkpoint boundary is simulated. The reader and
// its timeout/failure helpers below are extracted from the production sources.
bool Operation::checkpoint()
{
	assert(++host.checkpoints < 10000);
	if (cancelled || error != Error::None)
		return false;
	return remaining() ? true : fail(Error::TimedOut);
}

class WiFiClientSecure
{
public:
	struct Context { int socket = -1; } context;
	Context *sslclient = &context;
	virtual ~WiFiClientSecure() { stop(); }
	virtual void stop()
	{
		if (sslclient->socket >= 0) host.closedDescriptors.push_back(sslclient->socket);
		// The pinned SDK's stop_ssl_socket clears the structure to zero.
		sslclient->socket = 0;
	}
};

class VerifiedClient : public WiFiClientSecure
{
public:
	// @production:TLS_STOP
};

struct FakeClient
{
	std::vector<uint8_t> bytes;
	size_t cursor = 0;
	bool remoteOpen = true;
	bool stopped = false;
	unsigned availableCalls = 0;
	unsigned readCalls = 0;
	unsigned connectedCalls = 0;
	unsigned stalledReads = 0;
	int stalledResult = -1;
	uint32_t readAdvance = 0;

	int available()
	{
		++availableCalls;
		return stopped ? 0 : static_cast<int>(bytes.size() - cursor);
	}

	int read(uint8_t *buffer, size_t capacity)
	{
		assert(!stopped);
		++readCalls;
		host.now += readAdvance;
		if (stalledReads)
		{
			--stalledReads;
			return stalledResult;
		}
		const size_t count = std::min(capacity, bytes.size() - cursor);
		std::copy_n(bytes.data() + cursor, count, buffer);
		cursor += count;
		return static_cast<int>(count);
	}

	bool connected()
	{
		++connectedCalls;
		return remoteOpen && !stopped;
	}

	void stop()
	{
		host.cleanup.emplace_back("stop");
		stopped = true;
	}
};

struct FakeRequest
{
	explicit FakeRequest(FakeClient &transport) : client(transport) {}
	FakeClient &client;

	void end()
	{
		host.cleanup.emplace_back("end");
		if (client.connected() && client.available() > 0)
		{
			// Model HTTP cleanup touching an unread response if TLS is still open.
			++host.drainAttempts;
			uint8_t discarded;
			client.read(&discarded, 1);
		}
	}
};

struct HttpsDownload::Impl
{
	explicit Impl(Operation &context) : operation(context), request(client) {}
	~Impl() { close(); }

	// @production:CLOSE

	Operation &operation;
	FakeClient client;
	FakeRequest request;
	uint32_t length = 4;
	uint32_t maximum = 4;
	uint32_t received = 0;
	uint32_t lastRead = millis();
	bool hasLength = true;
	bool metadata = false;
};

// @production:CONSTRUCTOR
// @production:DESTRUCTOR
// @production:BODY_CAPACITY
// @production:READ

struct esp_partition_t {};
const esp_partition_t *inactivePartition()
{
	static const esp_partition_t partition;
	return host.compatibleLayout ? &partition : nullptr;
}
bool synchronizeClock(Operation &operation) { return operation.checkpoint(); }
void publishState(const Operation &, State) {}
constexpr size_t TRANSFER_BYTES = 1024;

class ManifestDownload
{
public:
	explicit ManifestDownload(Operation &operation) : operation_(operation) {}
	~ManifestDownload()
	{
		host.tlsOpen = false;
		++host.downloadDestructions;
	}
	bool open(const char *filename, const char *tag, uint32_t maximum)
	{
		assert(host.allocatedManifest == 0);
		assert(std::string(filename) == "ota-manifest.json" && tag == nullptr);
		assert(maximum == FirmwareUpdateConfig::MAX_MANIFEST_BYTES);
		if (host.failOpen) return operation_.fail(Error::ConnectionFailed);
		host.tlsOpen = true;
		return true;
	}
	size_t bodyCapacity() const { return host.advertisedCapacity; }
	int read(uint8_t *destination, size_t capacity)
	{
		assert(host.tlsOpen && host.allocatedManifest != 0);
		const size_t bytes = std::min(capacity, host.manifestBody.size() - offset_);
		std::memcpy(destination, host.manifestBody.data() + offset_, bytes);
		offset_ += bytes;
		return static_cast<int>(bytes);
	}

private:
	Operation &operation_;
	size_t offset_ = 0;
};

void *allocateManifest(size_t bytes)
{
	assert(host.tlsOpen && host.allocatedManifest == 0);
	host.lastAllocation = bytes;
	if (host.failAllocation) return nullptr;
	void *result = std::malloc(bytes);
	assert(result);
	host.allocatedManifest = bytes;
	return result;
}
void freeManifest(void *data)
{
	assert(host.allocatedManifest != 0);
	host.allocatedManifest = 0;
	std::free(data);
}
Error parseManifest(const char *json, size_t size, Release &, bool &newer)
{
	assert(!host.tlsOpen && host.downloadDestructions == 1);
	assert(host.allocatedManifest != 0 && json[size] == '\0');
	assert(std::string(json, size) == host.manifestBody);
	newer = host.manifestError == Error::None;
	return host.manifestError;
}

#define HttpsDownload ManifestDownload
#define malloc allocateManifest
#define free freeManifest
// @production:DISCOVER
#undef free
#undef malloc
#undef HttpsDownload

struct Fixture
{
	Fixture() : operation(1, millis(), READ_TIMEOUT_MS + 60000), download(operation)
	{
		download.impl_ = new HttpsDownload::Impl(operation);
	}

	HttpsDownload::Impl &state() { return *download.impl_; }
	FakeClient &client() { return state().client; }
	Operation operation;
	HttpsDownload download;
	std::array<uint8_t, 8> output = {};

	int read() { return download.read(output.data(), output.size()); }
};

void declaredBodyEof()
{
	Fixture fixture;
	fixture.client().bytes = {1, 2, 3, 4};
	assert(fixture.read() == 4);
	assert(fixture.state().received == 4);
	host.now += READ_TIMEOUT_MS;
	assert(fixture.read() == 0);
	assert(fixture.client().remoteOpen && fixture.client().connectedCalls == 0);
	assert(fixture.operation.error == Error::None && host.delays == 0);
}

void zeroDeclaredBody()
{
	Fixture fixture;
	fixture.state().length = 0;
	fixture.state().metadata = true;
	assert(fixture.read() == 0);
	assert(fixture.client().connectedCalls == 0 && host.delays == 0);
	assert(fixture.operation.error == Error::None);
}

void truncatedBody()
{
	Fixture fixture;
	fixture.client().bytes = {1, 2, 3};
	fixture.client().remoteOpen = false;
	assert(fixture.read() == 3);
	assert(fixture.read() == -1);
	assert(fixture.state().received == 3);
	assert(fixture.operation.error == Error::TruncatedImage);
}

void declaredOverflow()
{
	for (bool metadata : {false, true})
	{
		Fixture fixture;
		fixture.state().metadata = metadata;
		fixture.state().maximum = 8;
		fixture.client().bytes = {1, 2, 3, 4, 5};
		assert(fixture.read() == -1);
		assert(fixture.state().received == 0);
		assert(fixture.operation.error == (metadata ? Error::MetadataTooLarge : Error::ImageTooLarge));
	}
}

void trailingOverflow()
{
	Fixture fixture;
	fixture.client().bytes = {1, 2, 3, 4, 5};
	assert(fixture.download.read(fixture.output.data(), 4) == 4);
	assert(fixture.state().received == 4);
	assert(fixture.read() == -1);
	assert(fixture.operation.error == Error::ImageTooLarge);
	assert(fixture.state().received == 4);
}

void metadataEofOnClose()
{
	Fixture fixture;
	fixture.state().hasLength = false;
	fixture.state().metadata = true;
	fixture.client().bytes = {1, 2, 3, 4};
	assert(fixture.read() == 4);
	host.onDelay = [&]() {
		if (host.delays == 2)
			fixture.client().remoteOpen = false;
	};
	assert(fixture.read() == 0);
	assert(host.delays == 2 && !fixture.client().remoteOpen);
	assert(fixture.operation.error == Error::None && fixture.state().received == 4);
}

void metadataOverflow()
{
	Fixture fixture;
	fixture.state().hasLength = false;
	fixture.state().metadata = true;
	fixture.client().bytes = {1, 2, 3, 4, 5};
	assert(fixture.read() == -1);
	assert(fixture.operation.error == Error::MetadataTooLarge);
}

void idleTimeout()
{
	for (bool hasLength : {false, true})
	{
		Fixture fixture;
		fixture.state().hasLength = hasLength;
		fixture.state().metadata = !hasLength;
		host.now += READ_TIMEOUT_MS;
		assert(fixture.read() == -1);
		assert(fixture.operation.error == Error::TimedOut);
		assert(fixture.client().remoteOpen && host.delays == 0);
	}
}

void cancelBeforeRead()
{
	Fixture fixture;
	fixture.client().bytes = {1, 2, 3, 4};
	fixture.operation.cancelled = true;
	assert(fixture.read() == -1);
	assert(fixture.client().readCalls == 0 && fixture.client().availableCalls == 0);
	assert(fixture.operation.error == Error::None && host.delays == 0);
}

void cancelDuringWait()
{
	Fixture fixture;
	host.onDelay = [&]() { fixture.operation.cancelled = true; };
	assert(fixture.read() == -1);
	assert(host.delays == 1 && fixture.client().readCalls == 0);
	assert(fixture.operation.cancelled && fixture.operation.error == Error::None);
}

void deadlineBeforeRead()
{
	Fixture fixture;
	fixture.client().bytes = {1, 2, 3, 4};
	host.now += fixture.operation.budget;
	assert(fixture.read() == -1);
	assert(fixture.operation.error == Error::TimedOut);
	assert(fixture.client().readCalls == 0 && fixture.client().availableCalls == 0);
}

void deadlineDuringWait()
{
	for (uint32_t start : {0U, UINT32_MAX - 1})
	{
		host.now = start;
		host.delays = 0;
		Fixture fixture;
		fixture.operation.budget = 3;
		assert(fixture.read() == -1);
		assert(fixture.operation.error == Error::TimedOut && host.delays == 2);
		assert(millis() - start == 4);
	}
}

void stalledReadDeadline()
{
	for (int result : {-1, 0})
	{
		Fixture fixture;
		fixture.client().bytes = {1, 2, 3, 4};
		fixture.client().stalledReads = 100;
		fixture.client().stalledResult = result;
		fixture.client().readAdvance = 1;
		fixture.operation.budget = 3;
		assert(fixture.read() == -1);
		assert(fixture.operation.error == Error::TimedOut);
		assert(fixture.client().readCalls == 3 && fixture.state().received == 0);
	}
}

void closeWithoutDrain()
{
	Fixture fixture;
	fixture.client().bytes = {1, 2, 3, 4, 5};
	fixture.state().close();
	assert((host.cleanup == std::vector<std::string>{"stop", "end"}));
	assert(host.drainAttempts == 0 && fixture.client().readCalls == 0);
	assert(fixture.client().stopped);
	fixture.state().close();
	assert((host.cleanup == std::vector<std::string>{"stop", "end", "stop", "end"}));
	assert(host.drainAttempts == 0 && fixture.client().readCalls == 0);
}

void destructorWithoutDrain()
{
	{
		Fixture fixture;
		fixture.client().bytes = {1, 2, 3, 4};
		fixture.operation.cancelled = true;
		assert(fixture.read() == -1);
	}
	assert((host.cleanup == std::vector<std::string>{"stop", "end"}));
	assert(host.drainAttempts == 0);
}

void invalidArguments()
{
	uint8_t output;
	Operation operation(1, millis(), 60000);
	HttpsDownload unopened(operation);
	assert(unopened.read(&output, 1) == -1 && operation.error == Error::HttpFailed);
	Fixture fixture;
	assert(fixture.download.read(nullptr, 1) == -1);
	assert(fixture.operation.error == Error::HttpFailed);
	fixture.operation.error = Error::None;
	assert(fixture.download.read(&output, 0) == -1);
	assert(fixture.operation.error == Error::HttpFailed);
	assert(fixture.client().availableCalls == 0 && fixture.client().readCalls == 0);
}

void boundedRead()
{
	Fixture fixture;
	fixture.client().bytes = {1, 2, 3, 4};
	uint8_t tiny[2] = {};
	assert(fixture.download.read(tiny, sizeof(tiny)) == 2);
	assert(tiny[0] == 1 && tiny[1] == 2 && fixture.state().received == 2);
	assert(fixture.download.read(tiny, sizeof(tiny)) == 2);
	assert(tiny[0] == 3 && tiny[1] == 4 && fixture.state().received == 4);
	assert(fixture.download.read(tiny, sizeof(tiny)) == 0);
}

void fragmentedTlsMemory()
{
	assert(host.largestBlock < 48000 && hasTlsMemory());
	host.freeHeap = MIN_TLS_HEAP_BYTES - 1;
	assert(!hasTlsMemory());
	host.freeHeap = MIN_TLS_HEAP_BYTES;
	host.largestBlock = MIN_TLS_HEAP_BLOCK - 1;
	assert(!hasTlsMemory());
	host.largestBlock = MIN_TLS_HEAP_BLOCK;
	assert(hasTlsMemory());
}

void exactBodyCapacity()
{
	Fixture fixture;
	fixture.state().length = 535;
	fixture.state().maximum = FirmwareUpdateConfig::MAX_MANIFEST_BYTES;
	assert(fixture.download.bodyCapacity() == 535);
	fixture.state().hasLength = false;
	assert(fixture.download.bodyCapacity() == FirmwareUpdateConfig::MAX_MANIFEST_BYTES);
}

void manifestAllocationLifetime()
{
	Operation operation(1, millis(), 60000);
	Release release;
	bool newer = false;
	assert(discoverRelease(operation, release, newer));
	assert(newer && host.lastAllocation == 536);
	assert(host.allocatedManifest == 0 && !host.tlsOpen);
	assert(host.downloadDestructions == 1);
}

void manifestUnknownLength()
{
	host.advertisedCapacity = FirmwareUpdateConfig::MAX_MANIFEST_BYTES;
	Operation operation(1, millis(), 60000);
	Release release;
	bool newer = false;
	assert(discoverRelease(operation, release, newer));
	assert(host.lastAllocation == FirmwareUpdateConfig::MAX_MANIFEST_BYTES + 1);
	assert(host.allocatedManifest == 0 && !host.tlsOpen);
}

void manifestFailureCleanup()
{
	for (int scenario = 0; scenario < 6; ++scenario)
	{
		host = Host{};
		Error expected = Error::None;
		switch (scenario)
		{
		case 0: host.failOpen = true; expected = Error::ConnectionFailed; break;
		case 1: host.failAllocation = true; expected = Error::OutOfMemory; break;
		case 2: host.advertisedCapacity = 0; expected = Error::InvalidManifest; break;
		case 3: host.advertisedCapacity = 8193; expected = Error::InvalidManifest; break;
		case 4: host.advertisedCapacity = 4; expected = Error::MetadataTooLarge; break;
		case 5: host.manifestError = Error::InvalidManifest; expected = Error::InvalidManifest; break;
		}
		Operation operation(1, millis(), 60000);
		Release release;
		bool newer = false;
		assert(!discoverRelease(operation, release, newer));
		assert(operation.error == expected);
		assert(host.allocatedManifest == 0 && !host.tlsOpen && host.downloadDestructions == 1);
	}
}

void delayedBodyResumes()
{
	Fixture fixture;
	host.onDelay = [&]() {
		if (host.now >= 9000) fixture.client().bytes = {1, 2, 3, 4};
	};
	assert(fixture.read() == 4);
	assert(host.now == 9000 && fixture.operation.error == Error::None);
}

void repeatedSecureCleanup()
{
	{
		VerifiedClient client;
		client.sslclient->socket = 54;
		client.stop();
		assert(client.sslclient->socket == -1);
		WiFiClientSecure &httpReference = client;
		httpReference.stop();
		client.sslclient->socket = 55;
		httpReference.stop();
		httpReference.stop();
	}
	assert((host.closedDescriptors == std::vector<int>{54, 55}));
}
}

struct TestCase
{
	const char *name;
	void (*run)();
};

const TestCase cases[] = {
	{"declared body ends without socket close", FirmwareUpdate::declaredBodyEof},
	{"zero declared body ends immediately", FirmwareUpdate::zeroDeclaredBody},
	{"truncated declared body fails", FirmwareUpdate::truncatedBody},
	{"buffered declared overflow fails", FirmwareUpdate::declaredOverflow},
	{"buffered trailing bytes fail after complete length", FirmwareUpdate::trailingOverflow},
	{"lengthless metadata waits for socket close", FirmwareUpdate::metadataEofOnClose},
	{"lengthless metadata rejects surplus", FirmwareUpdate::metadataOverflow},
	{"open incomplete response hits idle timeout", FirmwareUpdate::idleTimeout},
	{"cancellation prevents a buffered read", FirmwareUpdate::cancelBeforeRead},
	{"cancellation interrupts waiting", FirmwareUpdate::cancelDuringWait},
	{"deadline prevents a buffered read", FirmwareUpdate::deadlineBeforeRead},
	{"deadline interrupts waiting and wraps millis", FirmwareUpdate::deadlineDuringWait},
	{"stalled reads obey operation deadline", FirmwareUpdate::stalledReadDeadline},
	{"close stops before HTTP cleanup", FirmwareUpdate::closeWithoutDrain},
	{"destructor closes without draining", FirmwareUpdate::destructorWithoutDrain},
	{"invalid read arguments fail", FirmwareUpdate::invalidArguments},
	{"read respects caller buffer capacity", FirmwareUpdate::boundedRead},
	{"TLS admission permits separate record buffers in fragmented heap", FirmwareUpdate::fragmentedTlsMemory},
	{"body capacity uses Content-Length or a bounded fallback", FirmwareUpdate::exactBodyCapacity},
	{"manifest is allocated exactly once after TLS and released after parse", FirmwareUpdate::manifestAllocationLifetime},
	{"unknown manifest length remains bounded", FirmwareUpdate::manifestUnknownLength},
	{"manifest allocation and parse failures clean up TLS and heap", FirmwareUpdate::manifestFailureCleanup},
	{"body can resume after a nine-second pause within the operation deadline", FirmwareUpdate::delayedBodyResumes},
	{"repeated TLS cleanup never closes an unrelated descriptor", FirmwareUpdate::repeatedSecureCleanup},
};

int main(int argc, char **argv)
{
	if (argc == 2 && std::string(argv[1]) == "--list")
	{
		for (const auto &item : cases)
			std::cout << item.name << '\n';
		return 0;
	}
	if (argc == 2 && std::string(argv[1]) == "--all")
	{
		for (const auto &item : cases)
		{
			host = Host{};
			item.run();
			std::cout << "PASS " << item.name << std::endl;
		}
		return 0;
	}
	if (argc == 2)
	{
		for (const auto &item : cases)
		{
			if (std::string(argv[1]) == item.name)
			{
				host = Host{};
				item.run();
				std::cout << "PASS " << item.name << '\n';
				return 0;
			}
		}
	}
	std::cerr << "Pass --list, --all, or a transport regression case name\n";
	return 2;
}
