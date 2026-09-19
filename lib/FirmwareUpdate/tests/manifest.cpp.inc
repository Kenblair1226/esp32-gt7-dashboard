// Host-only test; link FirmwareManifest.cpp with the framework's cJSON version.
#include "../FirmwareManifest.h"

#include <assert.h>
#include <iostream>
#include <string>

using namespace FirmwareUpdate;

std::string manifest(const std::string &version)
{
	return R"({"schemaVersion":1,"product":"esp32-gt7-dashboard","chipFamily":"ESP32",)"
		R"("layout":"esp32-4mb-min-spiffs-v1","version":")" + version +
		R"(","releaseTag":"v)" + version +
		R"(","variants":{"ili9341":{"filename":"firmware-ili9341.bin","size":300,"sha256":")" +
		std::string(64, 'a') +
		R"("},"st7789":{"filename":"firmware-st7789.bin","size":304,"sha256":")" +
		std::string(64, 'b') + R"("}}})";
}

std::string replace(std::string input, const std::string &before, const std::string &after)
{
	const auto index = input.find(before);
	assert(index != std::string::npos);
	input.replace(index, before.size(), after);
	return input;
}

void rejected(const std::string &json)
{
	Release release;
	release.size = 123;
	bool newer = true;
	assert(parseManifest(json.data(), json.size(), release, newer) != Error::None);
	assert(release.size == 0 && !newer);
}

int main()
{
	const std::string valid = manifest("1.9.0");
	Release release;
	bool newer = false;
	assert(parseManifest(valid.data(), valid.size(), release, newer) == Error::None && newer);
	assert(std::string(release.version) == "1.9.0" && std::string(release.tag) == "v1.9.0");
	const bool st7789 = std::string(FirmwareUpdateConfig::VARIANT) == "st7789";
	assert(release.size == (st7789 ? 304U : 300U));
	for (const auto value : release.sha256)
		assert(value == (st7789 ? 0xbb : 0xaa));
	for (const char *older : {"0.0.0", "1.7.99", "1.8.0"})
	{
		const auto json = manifest(older);
		assert(parseManifest(json.data(), json.size(), release, newer) == Error::None && !newer);
	}
	for (const char *later : {"1.8.1", "1.10.0", "2.0.0", "4294967295.4294967295.4294967295"})
	{
		const auto json = manifest(later);
		assert(parseManifest(json.data(), json.size(), release, newer) == Error::None && newer);
	}
	for (const char *invalid : {"v1.9.0", "01.9.0", "1.09.0", "1.9.00", "1.9.0-rc1",
		"1.9.0+test", "1.9", "-1.9.0", "4294967296.0.0", "1.9.0\\u0000ignored"})
		rejected(manifest(invalid));
	rejected(replace(valid, "\"schemaVersion\":1", "\"schemaVersion\":2"));
	rejected(replace(valid, "\"schemaVersion\":1", "\"schemaVersion\":\"1\""));
	rejected(replace(valid, "esp32-gt7-dashboard", "other-product"));
	rejected(replace(valid, "\"ESP32\"", "\"ESP32-S3\""));
	rejected(replace(valid, "esp32-4mb-min-spiffs-v1", "huge-app"));
	rejected(replace(valid, "\"releaseTag\":\"v1.9.0\"", "\"releaseTag\":\"v1.9.1\""));
	rejected(replace(valid, "\"layout\":", "\"product\":")); // Duplicate key, same node count.
	rejected(replace(valid, "\"ili9341\":", "\"otherPanel\":"));
	rejected(replace(valid, "firmware-ili9341.bin", "../firmware-ili9341.bin"));
	rejected(replace(valid, "firmware-st7789.bin", "https://evil.invalid/firmware.bin"));
	for (const char *size : {"0", "31", "-1", "300.5", "\"300\"", "1966081", "1e99", "null"})
		rejected(replace(valid, "\"size\":300", std::string("\"size\":") + size));
	for (const std::string &hash : {std::string(63, 'a'), std::string(65, 'a'),
		std::string(64, 'A'), std::string(64, 'g')})
		rejected(replace(valid, std::string(64, 'a'), hash));
	rejected(replace(valid, "\"size\":300", "\"filename\":300"));
	rejected(valid.substr(0, valid.size() - 1));
	rejected(valid + "false");
	rejected(valid + std::string("\0", 1));
	rejected(replace(valid, "\"size\":300", "\"size\":[[[[300]]]]"));
	std::string maximum = valid;
	maximum.append(FirmwareUpdateConfig::MAX_MANIFEST_BYTES - maximum.size(), ' ');
	assert(parseManifest(maximum.data(), maximum.size(), release, newer) == Error::None && newer);
	maximum.push_back(' ');
	assert(parseManifest(maximum.data(), maximum.size(), release, newer) == Error::MetadataTooLarge);
	std::cout << "OTA strict manifest tests passed (" << FirmwareUpdateConfig::VARIANT << ")\n";
}
