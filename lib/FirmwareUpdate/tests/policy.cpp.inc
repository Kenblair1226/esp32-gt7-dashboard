// Host-only .inc keeps PlatformIO's firmware source discovery from compiling
// this test main. Compile with g++ -x c++ -std=c++17 and the project include paths.
#include "../OtaPolicy.h"

#include <algorithm>
#include <assert.h>
#include <iostream>
#include <string>
#include <vector>

using namespace FirmwareUpdate::Policy;

int main()
{
	Version version, other;
	for (const char *good : {"0.0.0", "1.8.0", "1.10.0", "4294967295.4294967295.4294967295"})
		assert(parseVersion(good, version));
	for (const char *bad : {"", "1", "1.2", "1.2.3.4", "1.2.", "v1.2.3",
		"01.2.3", "1.02.3", "1.2.03", "4294967296.0.0", "999999999999.0.0",
		"-1.2.3", "1.2.3-rc1", "1.2.3+build", " 1.2.3", "1.2.3\n"})
		assert(!parseVersion(bad, version));
	assert(!parseVersion(nullptr, version));
	assert(parseVersion("1.10.0", version) && parseVersion("1.9.99", other));
	assert(compare(version, other) > 0 && compare(other, version) < 0);
	assert(compare(version, version) == 0);
	assert(parseVersion("4294967295.0.0", version) && parseVersion("2147483648.0.0", other));
	assert(compare(version, other) > 0);

	uint8_t hash[32];
	assert(parseSha256(std::string(64, 'a').c_str(), hash));
	for (uint8_t value : hash)
		assert(value == 0xaa);
	assert(!parseSha256(std::string(64, 'A').c_str(), hash));
	assert(!parseSha256(std::string(64, 'g').c_str(), hash));
	assert(!parseSha256(std::string(63, 'a').c_str(), hash));
	assert(!parseSha256(std::string(65, 'a').c_str(), hash));
	assert(!parseSha256(nullptr, hash));
	uint32_t length = 0;
	assert(parseLength("4294967295", length) && length == UINT32_MAX);
	for (const char *bad : {"", "-1", "+1", "4294967296", "1 2", "1.0", "5x", " 7"})
		assert(!parseLength(bad, length));

	const std::string repository = FirmwareUpdateConfig::REPOSITORY;
	const std::string latest = "https://github.com/" + repository + "/releases/latest/download/ota-manifest.json";
	const std::string immutable = "https://github.com/" + repository + "/releases/download/v1.9.0/firmware-ili9341.bin";
	Url url;
	assert(parseUrl(latest.c_str(), url) && githubPathAllowed(url, "ota-manifest.json", nullptr));
	assert(parseUrl(immutable.c_str(), url) && githubPathAllowed(url, "firmware-ili9341.bin", "v1.9.0"));
	assert(!githubPathAllowed(url, "firmware-st7789.bin", "v1.9.0"));
	assert(!githubPathAllowed(url, "firmware-ili9341.bin", "v1.10.0"));
	for (const char *bad : {
		"http://github.com/path", "HTTPS://github.com/path",
		"https://github.com.evil.invalid/path", "https://github.com@evil.invalid/path",
		"https://evil.invalid@github.com/path", "https://github.com:443/path",
		"https://github.com:80/path", "https://github.com./path", "https://github.com",
		"https://github.com/path#fragment", "https://github.com/path\r\nInjected: header",
		"https://github.com\\evil.invalid/path", "https://github.com/a b",
		"https://raw.githubusercontent.com/path", "https://127.0.0.1/path",
		"https://[::1]/path", "//github.com/path", "/relative"})
		assert(!parseUrl(bad, url));
	const std::string oversized = "https://github.com/" + std::string(MAX_URL_BYTES, 'a');
	assert(!parseUrl(oversized.c_str(), url));
	const std::string otherRepo = "https://github.com/not-this/repo/releases/download/v1.9.0/ota-manifest.json";
	assert(parseUrl(otherRepo.c_str(), url) && !githubPathAllowed(url, "ota-manifest.json", nullptr));
	assert(parseUrl("https://release-assets.githubusercontent.com/assets?id=token", url));
	assert(parseUrl("https://objects.githubusercontent.com/assets?id=token", url));
	char redirect[MAX_URL_BYTES + 1];
	assert(parseUrl(latest.c_str(), url));
	assert(resolveRedirect(url, "/a", redirect, sizeof(redirect)));
	assert(strcmp(redirect, "https://github.com/a") == 0);
	assert(!resolveRedirect(url, "//evil.invalid/a", redirect, sizeof(redirect)));
	assert(!resolveRedirect(url, "http://github.com/a", redirect, sizeof(redirect)));
	assert(!resolveRedirect(url, "https://evil.invalid/a", redirect, sizeof(redirect)));
	assert(!resolveRedirect(url, "/a", redirect, 4));
	assert(!resolveRedirect(url, nullptr, redirect, sizeof(redirect)));

	const std::string json =
		R"({"schemaVersion":1,"product":"esp32-gt7-dashboard","chipFamily":"ESP32",)"
		R"("layout":"esp32-4mb-min-spiffs-v1","version":"1.9.0","releaseTag":"v1.9.0",)"
		R"("variants":{"ili9341":{"filename":"firmware-ili9341.bin","size":300,"sha256":")" +
		std::string(64, 'a') +
		R"("},"st7789":{"filename":"firmware-st7789.bin","size":300,"sha256":")" +
		std::string(64, 'b') + R"("}}})";
	assert(manifestEnvelope(json.data(), json.size()));
	assert(!manifestEnvelope(json.data(), FirmwareUpdateConfig::MAX_MANIFEST_BYTES + 1));
	assert(!manifestEnvelope("{}", 2));
	assert(!manifestEnvelope("{\"a\":[[[[1]]]]}", 15));
	std::string escaped = json;
	escaped.replace(escaped.find("1.9.0"), 5, "1.9.0\\u0000ignored");
	assert(!manifestEnvelope(escaped.data(), escaped.size()));
	std::string embeddedNull = json;
	embeddedNull[10] = '\0';
	assert(!manifestEnvelope(embeddedNull.data(), embeddedNull.size()));

	const std::string identity = std::string("GT7DASH-OTA:") + "1|" +
		FirmwareUpdateConfig::PRODUCT + "|1.9.0|" + FirmwareUpdateConfig::VARIANT + "|" +
		FirmwareUpdateConfig::LAYOUT + "|END";
	std::vector<uint8_t> image(1100, 0xaa);
	image.insert(image.end(), identity.begin(), identity.end());
	image.push_back(0);
	image.insert(image.end(), 1100, 0xbb);
	// All two-chunk boundaries, including every byte of the actual marker.
	for (size_t split = 0; split <= image.size(); ++split)
	{
		IdentityMatcher matcher;
		assert(matcher.begin("1.9.0"));
		matcher.feed(image.data(), split);
		matcher.feed(image.data() + split, image.size() - split);
		assert(matcher.valid());
	}
	for (size_t chunk : {1U, 2U, 7U, 64U, 512U, 1024U})
	{
		IdentityMatcher matcher;
		assert(matcher.begin("1.9.0"));
		for (size_t offset = 0; offset < image.size(); offset += chunk)
			matcher.feed(image.data() + offset, std::min(chunk, image.size() - offset));
		assert(matcher.valid());
	}
	IdentityMatcher wrongVersion;
	assert(wrongVersion.begin("1.9.1"));
	wrongVersion.feed(image.data(), image.size());
	assert(!wrongVersion.valid());
	for (const char *wrongField : {FirmwareUpdateConfig::VARIANT, FirmwareUpdateConfig::LAYOUT,
		FirmwareUpdateConfig::PRODUCT})
	{
		std::string wrong = identity;
		const auto field = wrong.find(wrongField);
		assert(field != std::string::npos);
		wrong[field] = '!';
		wrong.push_back('\0');
		IdentityMatcher matcher;
		assert(matcher.begin("1.9.0"));
		matcher.feed(reinterpret_cast<const uint8_t *>(wrong.data()), wrong.size());
		assert(!matcher.valid());
	}
	IdentityMatcher truncated;
	assert(truncated.begin("1.9.0"));
	truncated.feed(reinterpret_cast<const uint8_t *>(identity.data()), identity.size());
	assert(!truncated.valid()); // A prefix without its terminating NUL is not an identity.
	IdentityMatcher repeated;
	assert(repeated.begin("1.9.0"));
	repeated.feed(image.data(), image.size());
	repeated.feed(image.data(), image.size());
	assert(repeated.valid());
	const char shortLiterals[] = "GT7DASH-OTA:\0GT7DASH-OTA:1|\0";
	const std::string literals = std::string(shortLiterals, sizeof(shortLiterals) - 1) +
		"GT7DASH-OTA:%u|%s|%s|%s|%s|END";
	std::vector<uint8_t> duplicateImage(literals.begin(), literals.end());
	duplicateImage.push_back(0);
	duplicateImage.insert(duplicateImage.end(), image.begin(), image.end());
	duplicateImage.insert(duplicateImage.end(), image.begin(), image.end());
	for (size_t split = 0; split <= duplicateImage.size(); ++split)
	{
		IdentityMatcher matcher;
		assert(matcher.begin("1.9.0"));
		matcher.feed(duplicateImage.data(), split);
		matcher.feed(duplicateImage.data() + split, duplicateImage.size() - split);
		assert(matcher.valid());
	}
	for (const char *wrongField : {"1.9.0", FirmwareUpdateConfig::VARIANT, FirmwareUpdateConfig::LAYOUT})
	{
		std::string wrong = identity;
		wrong[wrong.find(wrongField)] = '!';
		wrong.push_back('\0');
		for (bool wrongFirst : {false, true})
		{
			IdentityMatcher matcher;
			assert(matcher.begin("1.9.0"));
			const auto feedWrong = [&]() {
				for (size_t i = 0; i < wrong.size(); ++i)
					matcher.feed(reinterpret_cast<const uint8_t *>(wrong.data() + i), 1);
			};
			if (wrongFirst)
				feedWrong();
			matcher.feed(image.data(), image.size());
			if (!wrongFirst)
				feedWrong();
			assert(!matcher.valid());
		}
	}
	IdentityMatcher partialAfterValid;
	assert(partialAfterValid.begin("1.9.0"));
	partialAfterValid.feed(image.data(), image.size());
	partialAfterValid.feed(reinterpret_cast<const uint8_t *>(identity.data()), identity.size());
	assert(!partialAfterValid.valid());
	IdentityMatcher reset;
	assert(reset.begin("1.9.0"));
	reset.feed(image.data(), image.size() / 2);
	assert(reset.begin("1.9.0"));
	assert(!reset.valid());
	reset.feed(image.data(), image.size());
	assert(reset.valid());
	assert(!reset.begin("01.9.0"));
	assert(!reset.valid());
	std::cout << "OTA policy and streaming identity tests passed (" <<
		FirmwareUpdateConfig::VARIANT << ")\n";
}
