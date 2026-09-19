#include "FirmwareManifest.h"
#include "OtaPolicy.h"

#include <cJSON.h>
#include <math.h>
#include <memory>

namespace FirmwareUpdate
{
namespace
{
bool exactMembers(const cJSON *object, const char *const *names, size_t count)
{
	if (!cJSON_IsObject(object))
		return false;
	uint32_t seen = 0;
	for (const cJSON *item = object->child; item; item = item->next)
	{
		size_t i = 0;
		for (; i < count; ++i)
			if (item->string && strcmp(item->string, names[i]) == 0)
				break;
		if (i == count || (seen & (1U << i)))
			return false;
		seen |= 1U << i;
	}
	return seen == (1U << count) - 1;
}

const char *stringValue(const cJSON *object, const char *name)
{
	const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
	return cJSON_IsString(item) ? item->valuestring : nullptr;
}

bool equals(const cJSON *object, const char *name, const char *expected)
{
	const char *actual = stringValue(object, name);
	return actual && strcmp(actual, expected) == 0;
}

bool unsignedValue(const cJSON *object, const char *name, uint32_t &value)
{
	const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
	if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
		item->valuedouble < 0 || item->valuedouble > UINT32_MAX ||
		floor(item->valuedouble) != item->valuedouble)
		return false;
	value = static_cast<uint32_t>(item->valuedouble);
	return true;
}
}

Error parseManifest(const char *json, size_t size, Release &release, bool &newer)
{
	release = Release{};
	newer = false;
	if (size > FirmwareUpdateConfig::MAX_MANIFEST_BYTES)
		return Error::MetadataTooLarge;
	if (!Policy::manifestEnvelope(json, size))
		return Error::InvalidManifest;
	const char *end = nullptr;
	std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(
		cJSON_ParseWithLengthOpts(json, size, &end, false), cJSON_Delete);
	if (!root)
		return Error::InvalidManifest;
	while (end && end < json + size &&
		(*end == ' ' || *end == '\r' || *end == '\n' || *end == '\t'))
		++end;
	if (end != json + size)
		return Error::InvalidManifest;
	static const char *const fields[] = {
		"schemaVersion", "product", "chipFamily", "layout", "version", "releaseTag", "variants"
	};
	static const char *const panels[] = {"ili9341", "st7789"};
	static const char *const assetFields[] = {"filename", "size", "sha256"};
	uint32_t schema;
	if (!exactMembers(root.get(), fields, 7) ||
		!unsignedValue(root.get(), "schemaVersion", schema) ||
		schema != FirmwareUpdateConfig::SCHEMA_VERSION)
		return Error::InvalidManifest;
	if (!equals(root.get(), "product", FirmwareUpdateConfig::PRODUCT) ||
		!equals(root.get(), "chipFamily", FirmwareUpdateConfig::CHIP_FAMILY) ||
		!equals(root.get(), "layout", FirmwareUpdateConfig::LAYOUT))
		return Error::IncompatibleFirmware;
	const char *version = stringValue(root.get(), "version");
	const char *tag = stringValue(root.get(), "releaseTag");
	Policy::Version candidate, running;
	if (!Policy::parseVersion(version, candidate) ||
		!Policy::parseVersion(GT7_DASH_VERSION_LITERAL, running) ||
		!tag || tag[0] != 'v' || strcmp(tag + 1, version) != 0)
		return Error::InvalidManifest;
	const cJSON *variants = cJSON_GetObjectItemCaseSensitive(root.get(), "variants");
	if (!exactMembers(variants, panels, 2))
		return Error::IncompatibleFirmware;
	Release validated;
	for (const char *panel : panels)
	{
		const cJSON *asset = cJSON_GetObjectItemCaseSensitive(variants, panel);
		char filename[40];
		snprintf(filename, sizeof(filename), "firmware-%s.bin", panel);
		uint32_t bytes;
		uint8_t hash[32];
		if (!exactMembers(asset, assetFields, 3) || !equals(asset, "filename", filename) ||
			!unsignedValue(asset, "size", bytes) || bytes < 32 ||
			!Policy::parseSha256(stringValue(asset, "sha256"), hash))
			return Error::InvalidManifest;
		if (bytes > FirmwareUpdateConfig::SLOT_BYTES)
			return Error::ImageTooLarge;
		if (strcmp(panel, FirmwareUpdateConfig::VARIANT) == 0)
		{
			validated.size = bytes;
			memcpy(validated.sha256, hash, sizeof(hash));
		}
	}
	if (!validated.size)
		return Error::IncompatibleFirmware;
	memcpy(validated.version, version, strlen(version) + 1);
	memcpy(validated.tag, tag, strlen(tag) + 1);
	release = validated;
	newer = Policy::compare(candidate, running) > 0;
	return Error::None;
}
}
