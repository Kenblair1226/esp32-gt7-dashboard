#pragma once

#include <stdint.h>
#include "ota_config.h"

namespace FirmwareUpdate
{
enum class State : uint8_t
{
	Idle,
	SyncingClock,
	Checking,
	UpToDate,
	Available,
	Downloading,
	Verifying,
	ReadyToRestart,
	Restarting,
	Cancelled,
	Failed,
};

enum class Error : uint8_t
{
	None,
	Busy,
	WifiUnavailable,
	GameActive,
	ClockUnavailable,
	ConnectionFailed,
	HttpFailed,
	ReleaseUnavailable,
	MetadataTooLarge,
	InvalidManifest,
	IncompatibleFirmware,
	ImageTooLarge,
	UsbUpgradeRequired,
	OutOfMemory,
	TimedOut,
	TruncatedImage,
	HashMismatch,
	WriteFailed,
	InvalidImage,
	ActivationFailed,
	PreferencesFailed,
};

enum class Action : uint8_t
{
	None,
	Check,
	Install,
	Cancel,
};

struct Status
{
	State state = State::Idle;
	Error error = Error::None;
	uint8_t progress = 0;
	uint32_t receivedBytes = 0;
	uint32_t totalBytes = 0;
	int httpStatus = 0;
	char availableVersion[FirmwareUpdateConfig::VERSION_CAPACITY] = {};
};

inline bool isBusy(State state)
{
	return state == State::SyncingClock || state == State::Checking ||
		state == State::Downloading || state == State::Verifying ||
		state == State::ReadyToRestart || state == State::Restarting;
}

bool begin();
Status snapshot();
void poll(bool gameActive);
bool requestCheck(bool gameActive);
bool requestInstall(bool gameActive);
void requestCancel();
// Selects the verified partition; the main loop owns the subsequent restart.
bool activate(bool gameActive);
void rejectActivation(Error error);
const char *errorText(Error error);
}
