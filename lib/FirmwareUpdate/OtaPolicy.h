#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ota_config.h"

namespace FirmwareUpdate
{
namespace Policy
{
constexpr size_t MAX_URL_BYTES = 2048;
constexpr size_t MAX_IDENTITY_BYTES = 160;
constexpr uint8_t MAX_REDIRECTS = 4;

struct Version
{
	uint32_t part[3] = {};
};

inline bool parseVersion(const char *text, Version &version)
{
	if (!text || !*text || strlen(text) >= FirmwareUpdateConfig::VERSION_CAPACITY)
		return false;
	Version parsed;
	for (size_t component = 0; component < 3; ++component)
	{
		if (*text < '0' || *text > '9')
			return false;
		const bool zero = *text == '0';
		size_t digits = 0;
		uint32_t value = 0;
		while (*text >= '0' && *text <= '9')
		{
			const uint32_t digit = static_cast<uint32_t>(*text++ - '0');
			if ((zero && digits != 0) || value > (UINT32_MAX - digit) / 10)
				return false;
			value = value * 10 + digit;
			++digits;
		}
		parsed.part[component] = value;
		if (component != 2 && *text++ != '.')
			return false;
	}
	if (*text != '\0')
		return false;
	version = parsed;
	return true;
}

inline int compare(const Version &left, const Version &right)
{
	for (size_t i = 0; i < 3; ++i)
	{
		if (left.part[i] != right.part[i])
			return left.part[i] > right.part[i] ? 1 : -1;
	}
	return 0;
}

inline bool parseSha256(const char *hex, uint8_t digest[32])
{
	if (!hex || strlen(hex) != 64)
		return false;
	for (size_t i = 0; i < 32; ++i)
	{
		uint8_t value = 0;
		for (size_t j = 0; j < 2; ++j)
		{
			const char c = hex[i * 2 + j];
			if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
				return false;
			value = static_cast<uint8_t>((value << 4) |
				(c <= '9' ? c - '0' : c - 'a' + 10));
		}
		digest[i] = value;
	}
	return true;
}

inline bool parseLength(const char *text, uint32_t &result)
{
	if (!text || !*text)
		return false;
	uint32_t value = 0;
	do
	{
		if (*text < '0' || *text > '9')
			return false;
		const uint32_t digit = static_cast<uint32_t>(*text++ - '0');
		if (value > (UINT32_MAX - digit) / 10)
			return false;
		value = value * 10 + digit;
	} while (*text);
	result = value;
	return true;
}

struct Url
{
	char host[48] = {};
	const char *path = nullptr;
};

inline bool parseUrl(const char *text, Url &url)
{
	if (!text || strlen(text) > MAX_URL_BYTES || strncmp(text, "https://", 8) != 0)
		return false;
	for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); *p; ++p)
	{
		if (*p <= 32 || *p >= 127 || *p == '\\' || *p == '#')
			return false;
	}
	const char *host = text + 8;
	const char *path = strchr(host, '/');
	if (!path || path == host || static_cast<size_t>(path - host) >= sizeof(url.host))
		return false;
	const size_t length = static_cast<size_t>(path - host);
	memcpy(url.host, host, length);
	url.host[length] = '\0';
	if (strcmp(url.host, "github.com") != 0 &&
		strcmp(url.host, "release-assets.githubusercontent.com") != 0 &&
		strcmp(url.host, "objects.githubusercontent.com") != 0)
		return false;
	url.path = path;
	return true;
}

inline bool githubPathAllowed(const Url &url, const char *filename, const char *releaseTag)
{
	if (strcmp(url.host, "github.com") != 0)
		return true;
	char prefix[160];
	const int length = snprintf(prefix, sizeof(prefix), "/%s/releases/",
		FirmwareUpdateConfig::REPOSITORY);
	if (length < 0 || static_cast<size_t>(length) >= sizeof(prefix) ||
		strncmp(url.path, prefix, static_cast<size_t>(length)) != 0)
		return false;
	const char *path = url.path + length;
	if (!releaseTag && strncmp(path, "latest/download/", 16) == 0)
		return strcmp(path + 16, filename) == 0;
	if (strncmp(path, "download/v", 10) != 0)
		return false;
	path += 9;
	const char *slash = strchr(path, '/');
	if (!slash || strcmp(slash + 1, filename) != 0)
		return false;
	const size_t tagLength = static_cast<size_t>(slash - path);
	if (tagLength < 2 || tagLength > FirmwareUpdateConfig::VERSION_CAPACITY)
		return false;
	char tag[FirmwareUpdateConfig::VERSION_CAPACITY + 1] = {};
	memcpy(tag, path, tagLength);
	Version version;
	return parseVersion(tag + 1, version) && (!releaseTag || strcmp(tag, releaseTag) == 0);
}

inline bool resolveRedirect(const Url &previous, const char *location, char *result, size_t capacity)
{
	if (!location || !*location || strlen(location) > MAX_URL_BYTES)
		return false;
	int length;
	if (location[0] == '/' && location[1] != '/')
		length = snprintf(result, capacity, "https://%s%s", previous.host, location);
	else
		length = snprintf(result, capacity, "%s", location);
	Url parsed;
	return length > 0 && static_cast<size_t>(length) < capacity && parseUrl(result, parsed);
}

// Bound cJSON's allocations/depth before parsing. The release schema needs no
// arrays, escaped strings, non-ASCII text, or more than these fifteen members.
inline bool manifestEnvelope(const char *json, size_t size)
{
	if (!json || !size || size > FirmwareUpdateConfig::MAX_MANIFEST_BYTES)
		return false;
	bool quoted = false;
	unsigned depth = 0, objects = 0, members = 0;
	for (size_t i = 0; i < size; ++i)
	{
		const unsigned char c = static_cast<unsigned char>(json[i]);
		if (!c || c >= 127)
			return false;
		if (c == '"')
			quoted = !quoted;
		else if (quoted)
		{
			if (c < 32 || c == '\\')
				return false;
		}
		else if (c == '{')
		{
			if (++depth > 3 || ++objects > 4)
				return false;
		}
		else if (c == '}')
		{
			if (!depth)
				return false;
			--depth;
		}
		else if (c == ':')
		{
			if (++members > 15)
				return false;
		}
		else if (c == '[' || c == ']')
			return false;
	}
	return !quoted && depth == 0 && objects == 4 && members == 15;
}

// The matched-prefix table retains overlap across arbitrarily split chunks,
// without keeping a second image buffer or rescanning every previous byte.
class IdentityMatcher
{
public:
	bool begin(const char *version)
	{
		length_ = 0;
		prefixLength_ = 0;
		matched_ = 0;
		candidate_ = 0;
		found_ = false;
		conflict_ = false;
		Version parsed;
		if (!parseVersion(version, parsed))
			return false;
		const int prefixSize = snprintf(expected_, sizeof(expected_),
			"GT7DASH-OTA:%u|%s|", 1U, FirmwareUpdateConfig::PRODUCT);
		if (prefixSize <= 0 || static_cast<size_t>(prefixSize) >= sizeof(expected_))
			return false;
		prefixLength_ = static_cast<size_t>(prefixSize);
		const int suffixSize = snprintf(expected_ + prefixLength_, sizeof(expected_) - prefixLength_,
			"%s|%s|%s|END", version, FirmwareUpdateConfig::VARIANT,
			FirmwareUpdateConfig::LAYOUT);
		if (suffixSize <= 0 || static_cast<size_t>(suffixSize) >= sizeof(expected_) - prefixLength_)
			return false;
		length_ = prefixLength_ + static_cast<size_t>(suffixSize) + 1;
		overlap_[0] = 0;
		for (size_t i = 1, prefix = 0; i < prefixLength_; ++i)
		{
			while (prefix && expected_[i] != expected_[prefix])
				prefix = overlap_[prefix - 1];
			if (expected_[i] == expected_[prefix])
				++prefix;
			overlap_[i] = static_cast<uint8_t>(prefix);
		}
		return true;
	}

	void feed(const uint8_t *data, size_t size)
	{
		if (!length_ || conflict_)
			return;
		for (size_t i = 0; i < size; ++i)
		{
			if (candidate_)
			{
				// Every product-qualified marker must match, including its NUL.
				// Identical copies are harmless; conflicting or partial ones are not.
				if (data[i] != static_cast<uint8_t>(expected_[candidate_]))
				{
					conflict_ = true;
					return;
				}
				if (++candidate_ == length_)
				{
					found_ = true;
					candidate_ = 0;
				}
			}
			while (matched_ && data[i] != static_cast<uint8_t>(expected_[matched_]))
				matched_ = overlap_[matched_ - 1];
			if (data[i] == static_cast<uint8_t>(expected_[matched_]))
				++matched_;
			if (matched_ == prefixLength_)
			{
				candidate_ = prefixLength_;
				matched_ = overlap_[matched_ - 1];
			}
		}
	}

	bool valid() const { return found_ && !conflict_ && candidate_ == 0; }

private:
	char expected_[MAX_IDENTITY_BYTES] = {};
	uint8_t overlap_[MAX_IDENTITY_BYTES] = {};
	size_t length_ = 0;
	size_t prefixLength_ = 0;
	size_t matched_ = 0;
	size_t candidate_ = 0;
	bool found_ = false;
	bool conflict_ = false;
};
}
}
