#pragma once

#include <stddef.h>
#include <stdint.h>
#include "version.h"

#ifndef GT7_OTA_REPOSITORY
#define GT7_OTA_REPOSITORY "Kenblair1226/esp32-gt7-dashboard"
#endif

#define GT7_OTA_PRODUCT "esp32-gt7-dashboard"
#define GT7_OTA_LAYOUT "esp32-4mb-min-spiffs-v1"

#if defined(DISPLAY_PANEL_ST7789)
#define GT7_OTA_PANEL "st7789"
#else
#define GT7_OTA_PANEL "ili9341"
#endif

namespace FirmwareUpdateConfig
{
inline constexpr char REPOSITORY[] = GT7_OTA_REPOSITORY;
inline constexpr char PRODUCT[] = GT7_OTA_PRODUCT;
inline constexpr char CHIP_FAMILY[] = "ESP32";
inline constexpr char LAYOUT[] = GT7_OTA_LAYOUT;
inline constexpr char VARIANT[] = GT7_OTA_PANEL;
inline constexpr char FIRMWARE_FILENAME[] = "firmware-" GT7_OTA_PANEL ".bin";
inline constexpr uint32_t SCHEMA_VERSION = 1;
inline constexpr uint32_t SLOT_BYTES = 0x1E0000;
inline constexpr size_t MAX_MANIFEST_BYTES = 8192;
inline constexpr size_t VERSION_CAPACITY = 33;

// Packaging and the streamed-image verifier use the same embedded identity.
inline constexpr char BUILD_IDENTITY[] =
	"GT7DASH-OTA:1|" GT7_OTA_PRODUCT "|" GT7_DASH_VERSION_LITERAL "|"
	GT7_OTA_PANEL "|" GT7_OTA_LAYOUT "|END";
}
