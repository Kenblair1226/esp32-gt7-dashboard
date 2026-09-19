#pragma once

#include "FirmwareUpdate.h"

namespace FirmwareUpdate
{
struct Release
{
	char version[FirmwareUpdateConfig::VERSION_CAPACITY] = {};
	char tag[FirmwareUpdateConfig::VERSION_CAPACITY + 1] = {};
	uint32_t size = 0;
	uint8_t sha256[32] = {};
};

Error parseManifest(const char *json, size_t size, Release &release, bool &newer);
}
