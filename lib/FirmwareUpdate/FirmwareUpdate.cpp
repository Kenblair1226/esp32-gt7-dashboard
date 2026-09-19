#include "FirmwareUpdate.h"
#include "FirmwareManifest.h"
#include "OtaPolicy.h"
#include "OtaTransport.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_app_format.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_sntp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/sha256.h>
#include <memory>
#include <stdlib.h>
#include <time.h>

namespace FirmwareUpdate
{
namespace
{
constexpr uint32_t CHECK_TIMEOUT_MS = 90000;
constexpr uint32_t INSTALL_TIMEOUT_MS = 300000;
constexpr uint32_t CLOCK_TIMEOUT_MS = 20000;
constexpr time_t MIN_VALID_CLOCK = 1735689600; // 2025-01-01 UTC; TLS also checks validity.
constexpr size_t WORKER_STACK_BYTES = 16384;
constexpr size_t TRANSFER_BYTES = 1024;

struct Request
{
	Action action = Action::None;
	uint32_t generation = 0;
	Release release;
};

portMUX_TYPE stateLock = portMUX_INITIALIZER_UNLOCKED;
Status status;
Request mailbox;
Release offer;
TaskHandle_t workerTask = nullptr;
const esp_partition_t *pendingPartition = nullptr;
uint32_t generation = 0;
bool initialized = false;
bool inFlight = false;
bool hasOffer = false;
bool gameIsActive = false;
bool cancellationPending = false;
State cancellationState = State::Cancelled;
Error cancellationError = Error::None;

void clearOfferLocked()
{
	offer = Release{};
	hasOffer = false;
	pendingPartition = nullptr;
}

void invalidateLocked(State state, Error error)
{
	++generation;
	clearOfferLocked();
	status.error = error;
	if (inFlight)
	{
		// The UI keeps its timeout/sleep guard until the worker has actually
		// released its network and OTA resources, not merely received Cancel.
		cancellationPending = true;
		cancellationState = state;
		cancellationError = error;
	}
	else
		status.state = state;
}

void acknowledgeCancellationLocked()
{
	if (cancellationPending)
	{
		status.state = cancellationState;
		status.error = cancellationError;
		cancellationPending = false;
	}
}

void rejectRequestLocked(Error error)
{
	clearOfferLocked();
	status = Status{};
	status.state = State::Failed;
	status.error = error;
}

void publishState(const Operation &operation, State state)
{
	portENTER_CRITICAL(&stateLock);
	if (operation.generation == generation && !gameIsActive)
	{
		status.state = state;
		status.error = Error::None;
		status.httpStatus = operation.httpStatus;
	}
	portEXIT_CRITICAL(&stateLock);
}

void publishProgress(const Operation &operation, uint32_t received, uint32_t total)
{
	portENTER_CRITICAL(&stateLock);
	if (operation.generation == generation && !gameIsActive)
	{
		status.receivedBytes = received;
		status.totalBytes = total;
		status.progress = total ? static_cast<uint8_t>((received * 100UL) / total) : 0;
		status.httpStatus = operation.httpStatus;
	}
	portEXIT_CRITICAL(&stateLock);
}

bool clockValid() { return time(nullptr) >= MIN_VALID_CLOCK; }

bool synchronizeClock(Operation &operation)
{
	if (!operation.checkpoint())
		return false;
	if (clockValid())
		return true;
	publishState(operation, State::SyncingClock);
	const bool startedHere = !esp_sntp_enabled();
	if (startedHere)
	{
		esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
		esp_sntp_setservername(0, "time.cloudflare.com");
		esp_sntp_setservername(1, "pool.ntp.org");
		esp_sntp_init();
	}
	const uint32_t started = millis();
	while (operation.checkpoint() && !clockValid())
	{
		if (millis() - started >= CLOCK_TIMEOUT_MS)
		{
			operation.fail(Error::ClockUnavailable);
			break;
		}
		vTaskDelay(pdMS_TO_TICKS(50));
	}
	if (startedHere)
		esp_sntp_stop();
	return operation.checkpoint() && clockValid();
}

const esp_partition_t *inactivePartition()
{
	struct ExpectedPartition
	{
		esp_partition_type_t type;
		esp_partition_subtype_t subtype;
		uint32_t address;
		uint32_t size;
	};
	static const ExpectedPartition expected[] = {
		{ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, 0x9000, 0x5000},
		{ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, 0xE000, 0x2000},
		{ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, 0x10000, FirmwareUpdateConfig::SLOT_BYTES},
		{ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, 0x1F0000, FirmwareUpdateConfig::SLOT_BYTES},
		{ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, 0x3D0000, 0x20000},
		{ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, 0x3F0000, 0x10000}
	};
	if (ESP.getFlashChipSize() < 0x400000)
		return nullptr;
	uint8_t seen = 0;
	for (esp_partition_iterator_t it = esp_partition_find(
			ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
		it; it = esp_partition_next(it))
	{
		const esp_partition_t *partition = esp_partition_get(it);
		size_t i = 0;
		for (; i < sizeof(expected) / sizeof(expected[0]); ++i)
		{
			const ExpectedPartition &required = expected[i];
			if (partition->type == required.type && partition->subtype == required.subtype &&
				partition->address == required.address && partition->size == required.size &&
				!partition->encrypted)
				break;
		}
		if (i == sizeof(expected) / sizeof(expected[0]) || (seen & (1U << i)))
		{
			esp_partition_iterator_release(it);
			return nullptr;
		}
		seen |= 1U << i;
	}
	if (seen != 0x3F)
		return nullptr;
	const esp_partition_t *running = esp_ota_get_running_partition();
	const esp_partition_t *boot = esp_ota_get_boot_partition();
	if (!running || !boot || boot->address != running->address ||
		(running->address != 0x10000 && running->address != 0x1F0000))
		return nullptr;
	const esp_partition_t *next = esp_ota_get_next_update_partition(running);
	if (!next || next->address == running->address || next->size != FirmwareUpdateConfig::SLOT_BYTES ||
		(next->address != 0x10000 && next->address != 0x1F0000))
		return nullptr;
	return next;
}

bool discoverRelease(Operation &operation, Release &release, bool &newer)
{
	if (!inactivePartition())
		return operation.fail(Error::UsbUpgradeRequired);
	if (!synchronizeClock(operation))
		return false;
	publishState(operation, State::Checking);
	std::unique_ptr<char, decltype(&free)> json(nullptr, free);
	size_t used = 0;
	{
		HttpsDownload download(operation);
		if (!download.open("ota-manifest.json", nullptr, FirmwareUpdateConfig::MAX_MANIFEST_BYTES))
			return false;
		// Leave heap available for certificate verification across all redirects.
		const size_t capacity = download.bodyCapacity();
		if (!capacity || capacity > FirmwareUpdateConfig::MAX_MANIFEST_BYTES)
			return operation.fail(Error::InvalidManifest);
		json.reset(static_cast<char *>(malloc(capacity + 1)));
		if (!json)
			return operation.fail(Error::OutOfMemory);
		uint8_t buffer[TRANSFER_BYTES];
		int count;
		while ((count = download.read(buffer, sizeof(buffer))) > 0)
		{
			if (used + static_cast<size_t>(count) > capacity)
				return operation.fail(Error::MetadataTooLarge);
			memcpy(json.get() + used, buffer, static_cast<size_t>(count));
			used += static_cast<size_t>(count);
		}
		if (count < 0)
			return false;
	}
	if (!operation.checkpoint())
		return false;
	json.get()[used] = '\0';
	if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < 4096)
		return operation.fail(Error::OutOfMemory);
	const Error error = parseManifest(json.get(), used, release, newer);
	return error == Error::None ? operation.checkpoint() : operation.fail(error);
}

class OtaWriter
{
public:
	~OtaWriter()
	{
		if (open_)
			esp_ota_abort(handle_);
		mbedtls_sha256_free(&hash_);
	}

	OtaWriter() { mbedtls_sha256_init(&hash_); }

	bool begin(Operation &operation, const esp_partition_t *partition)
	{
		if (!operation.checkpoint())
			return false;
		if (mbedtls_sha256_starts_ret(&hash_, 0) != 0)
			return operation.fail(Error::OutOfMemory);
		// Erase incrementally, rather than holding up cancellation for a complete
		// slot erase. Every write is separately bounded by the accepted image size.
		const esp_err_t result = esp_ota_begin(partition, OTA_WITH_SEQUENTIAL_WRITES, &handle_);
		if (result != ESP_OK)
		{
			Serial.printf("[OTA] esp_ota_begin: %s\n", esp_err_to_name(result));
			return operation.fail(result == ESP_ERR_NO_MEM ? Error::OutOfMemory : Error::WriteFailed);
		}
		open_ = true;
		return operation.checkpoint();
	}

	bool write(Operation &operation, const uint8_t *buffer, size_t size)
	{
		if (!operation.checkpoint())
			return false;
		if (mbedtls_sha256_update_ret(&hash_, buffer, size) != 0)
			return operation.fail(Error::InvalidImage);
		const esp_err_t result = esp_ota_write(handle_, buffer, size);
		if (result != ESP_OK)
		{
			Serial.printf("[OTA] esp_ota_write: %s\n", esp_err_to_name(result));
			return operation.fail(result == ESP_ERR_OTA_VALIDATE_FAILED ? Error::InvalidImage : Error::WriteFailed);
		}
		return operation.checkpoint();
	}

	bool finish(Operation &operation, const uint8_t expectedHash[32])
	{
		if (!operation.checkpoint())
			return false;
		uint8_t actual[32];
		if (mbedtls_sha256_finish_ret(&hash_, actual) != 0)
			return operation.fail(Error::InvalidImage);
		uint8_t difference = 0;
		for (size_t i = 0; i < sizeof(actual); ++i)
			difference |= actual[i] ^ expectedHash[i];
		if (difference)
			return operation.fail(Error::HashMismatch);
		if (!operation.checkpoint())
			return false;
		open_ = false; // esp_ota_end consumes the handle on both success and failure.
		const esp_err_t result = esp_ota_end(handle_);
		if (result != ESP_OK)
		{
			Serial.printf("[OTA] esp_ota_end: %s\n", esp_err_to_name(result));
			return operation.fail(Error::InvalidImage);
		}
		return operation.checkpoint();
	}

private:
	esp_ota_handle_t handle_ = 0;
	mbedtls_sha256_context hash_;
	bool open_ = false;
};

bool installRelease(Operation &operation, const Release &release, const esp_partition_t *&ready)
{
	ready = nullptr;
	Policy::Version candidate, running;
	if (!Policy::parseVersion(release.version, candidate) ||
		!Policy::parseVersion(GT7_DASH_VERSION_LITERAL, running) ||
		Policy::compare(candidate, running) <= 0)
		return operation.fail(Error::IncompatibleFirmware);
	const esp_partition_t *partition = inactivePartition();
	if (!partition)
		return operation.fail(Error::UsbUpgradeRequired);
	if (!release.size || release.size > partition->size)
		return operation.fail(Error::ImageTooLarge);
	if (!synchronizeClock(operation))
		return false;
	publishState(operation, State::Downloading);
	Policy::IdentityMatcher identity;
	if (!identity.begin(release.version))
		return operation.fail(Error::InvalidManifest);
	OtaWriter writer;
	uint32_t received = 0;
	{
		HttpsDownload download(operation);
		if (!download.open(FirmwareUpdateConfig::FIRMWARE_FILENAME, release.tag, release.size))
			return false;
		uint8_t buffer[TRANSFER_BYTES];
		size_t prefix = 0;
		while (prefix < sizeof(esp_image_header_t))
		{
			const int count = download.read(buffer + prefix, sizeof(esp_image_header_t) - prefix);
			if (count <= 0)
				return count < 0 ? false : operation.fail(Error::TruncatedImage);
			prefix += static_cast<size_t>(count);
		}
		esp_image_header_t header;
		memcpy(&header, buffer, sizeof(header));
		if (header.magic != ESP_IMAGE_HEADER_MAGIC || header.chip_id != ESP_CHIP_ID_ESP32 ||
			!header.segment_count || header.segment_count > ESP_IMAGE_MAX_SEGMENTS ||
			header.spi_size != ESP_IMAGE_FLASH_SIZE_4MB || header.hash_appended != 1)
			return operation.fail(Error::InvalidImage);
		if (!writer.begin(operation, partition) || !writer.write(operation, buffer, prefix))
			return false;
		identity.feed(buffer, prefix);
		received = static_cast<uint32_t>(prefix);
		uint32_t lastProgress = 0;
		int count;
		while ((count = download.read(buffer, sizeof(buffer))) > 0)
		{
			if (static_cast<uint32_t>(count) > release.size - received)
				return operation.fail(Error::ImageTooLarge);
			if (!writer.write(operation, buffer, static_cast<size_t>(count)))
				return false;
			identity.feed(buffer, static_cast<size_t>(count));
			received += static_cast<uint32_t>(count);
			const uint32_t now = millis();
			if (received == release.size || now - lastProgress >= 100)
			{
				publishProgress(operation, received, release.size);
				lastProgress = now;
			}
			vTaskDelay(1);
		}
		if (count < 0)
			return false;
	}
	if (received != release.size)
		return operation.fail(Error::TruncatedImage);
	if (!operation.checkpoint())
		return false;
	publishState(operation, State::Verifying);
	if (!identity.valid())
		return operation.fail(Error::IncompatibleFirmware);
	if (!writer.finish(operation, release.sha256))
		return false;
	ready = partition;
	return true;
}

void worker(void *)
{
	for (;;)
	{
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		Request request;
		portENTER_CRITICAL(&stateLock);
		request = mailbox;
		portEXIT_CRITICAL(&stateLock);
		Operation operation{request.generation, millis(),
			request.action == Action::Install ? INSTALL_TIMEOUT_MS : CHECK_TIMEOUT_MS};
		Release found;
		bool newer = false;
		const esp_partition_t *ready = nullptr;
		bool success = false;
		if (operation.checkpoint())
		{
			if (request.action == Action::Check)
				success = discoverRelease(operation, found, newer);
			else if (request.action == Action::Install)
				success = installRelease(operation, request.release, ready);
		}
		if (!success && operation.error == Error::None && !operation.cancelled)
			operation.fail(Error::ConnectionFailed);

		portENTER_CRITICAL(&stateLock);
		if (operation.generation == generation)
		{
			status.httpStatus = operation.httpStatus;
			if (!success || gameIsActive)
			{
				clearOfferLocked();
				status.state = operation.cancelled || gameIsActive ? State::Cancelled : State::Failed;
				status.error = gameIsActive ? Error::GameActive : operation.error;
			}
			else if (request.action == Action::Check)
			{
				clearOfferLocked();
				status.error = Error::None;
				status.state = newer ? State::Available : State::UpToDate;
				memcpy(status.availableVersion, found.version, sizeof(status.availableVersion));
				if (newer)
				{
					offer = found;
					hasOffer = true;
				}
			}
			else
			{
				pendingPartition = ready;
				status.error = Error::None;
				status.state = State::ReadyToRestart;
				status.progress = 100;
			}
		}
		else
			acknowledgeCancellationLocked();
		mailbox = Request{};
		inFlight = false;
		portEXIT_CRITICAL(&stateLock);
		if (operation.error != Error::None)
			Serial.printf("[OTA] %s (HTTP %d)\n", errorText(operation.error), operation.httpStatus);
	}
}

bool requestOperation(Action action, bool gameActive)
{
	begin();
	const bool wifi = WiFi.status() == WL_CONNECTED;
	TaskHandle_t task;
	uint32_t acceptedGeneration;
	portENTER_CRITICAL(&stateLock);
	if (inFlight || isBusy(status.state))
	{
		if (!isBusy(status.state))
			status.error = Error::Busy;
		portEXIT_CRITICAL(&stateLock);
		return false;
	}
	if (gameActive || gameIsActive || !wifi)
	{
		rejectRequestLocked(gameActive || gameIsActive ? Error::GameActive : Error::WifiUnavailable);
		portEXIT_CRITICAL(&stateLock);
		return false;
	}
	if (action == Action::Install && (status.state != State::Available || !hasOffer))
	{
		rejectRequestLocked(Error::ReleaseUnavailable);
		portEXIT_CRITICAL(&stateLock);
		return false;
	}
	acceptedGeneration = ++generation;
	mailbox = Request{};
	mailbox.action = action;
	mailbox.generation = acceptedGeneration;
	status = Status{};
	if (action == Action::Install)
	{
		mailbox.release = offer;
		memcpy(status.availableVersion, offer.version, sizeof(status.availableVersion));
		status.totalBytes = offer.size;
		status.state = State::Downloading;
	}
	else
	{
		clearOfferLocked();
		status.state = State::Checking;
	}
	inFlight = true;
	task = workerTask;
	portEXIT_CRITICAL(&stateLock);
	if (!task)
	{
		if (xTaskCreate(worker, "gt7-ota", WORKER_STACK_BYTES, nullptr, 1, &task) != pdPASS)
		{
			portENTER_CRITICAL(&stateLock);
			inFlight = false;
			mailbox = Request{};
			if (generation == acceptedGeneration)
				rejectRequestLocked(Error::OutOfMemory);
			else
				acknowledgeCancellationLocked();
			portEXIT_CRITICAL(&stateLock);
			Serial.println("[OTA] Worker allocation failed; dashboard remains available");
			return false;
		}
		portENTER_CRITICAL(&stateLock);
		workerTask = task;
		portEXIT_CRITICAL(&stateLock);
	}
	// A single typed mailbox + notification is a bounded queue with no queue
	// allocation. inFlight remains set during cancellation cleanup.
	xTaskNotifyGive(task);
	return true;
}
}

bool Operation::fail(Error reason)
{
	if (error == Error::None && !cancelled)
		error = reason;
	return false;
}

uint32_t Operation::remaining() const
{
	const uint32_t elapsed = millis() - started;
	return elapsed < budget ? budget - elapsed : 0;
}

bool Operation::checkpoint()
{
	bool invalidated, active;
	Error reason;
	State state;
	portENTER_CRITICAL(&stateLock);
	invalidated = generation != FirmwareUpdate::generation;
	active = gameIsActive;
	reason = cancellationPending ? cancellationError : status.error;
	state = cancellationPending ? cancellationState : status.state;
	portEXIT_CRITICAL(&stateLock);
	if (invalidated)
	{
		cancelled = state == State::Cancelled;
		error = reason;
		return false;
	}
	if (active)
	{
		cancelled = true;
		error = Error::GameActive;
		return false;
	}
	if (error != Error::None || cancelled)
		return false;
	if (WiFi.status() != WL_CONNECTED)
		return fail(Error::WifiUnavailable);
	if (!remaining())
		return fail(Error::TimedOut);
	return true;
}

bool begin()
{
	portENTER_CRITICAL(&stateLock);
	const bool first = !initialized;
	initialized = true;
	portEXIT_CRITICAL(&stateLock);
	if (first)
		Serial.printf("[OTA] %s\n", FirmwareUpdateConfig::BUILD_IDENTITY);
	return true;
}

Status snapshot()
{
	portENTER_CRITICAL(&stateLock);
	const Status result = status;
	portEXIT_CRITICAL(&stateLock);
	return result;
}

void poll(bool gameActive)
{
	const bool wifi = WiFi.status() == WL_CONNECTED;
	portENTER_CRITICAL(&stateLock);
	gameIsActive = gameActive;
	if (status.state != State::Restarting && isBusy(status.state) && !cancellationPending)
	{
		if (gameActive)
			invalidateLocked(State::Cancelled, Error::GameActive);
		else if (!wifi)
			invalidateLocked(State::Failed, Error::WifiUnavailable);
	}
	portEXIT_CRITICAL(&stateLock);
}

bool requestCheck(bool gameActive) { return requestOperation(Action::Check, gameActive); }
bool requestInstall(bool gameActive) { return requestOperation(Action::Install, gameActive); }

void requestCancel()
{
	portENTER_CRITICAL(&stateLock);
	if (status.state != State::Restarting)
		invalidateLocked(State::Cancelled, Error::None);
	portEXIT_CRITICAL(&stateLock);
}

void rejectActivation(Error error)
{
	portENTER_CRITICAL(&stateLock);
	if (status.state == State::ReadyToRestart)
		invalidateLocked(State::Failed, error == Error::None ? Error::ActivationFailed : error);
	portEXIT_CRITICAL(&stateLock);
}

bool activate(bool gameActive)
{
	uint32_t expectedGeneration;
	const esp_partition_t *expected;
	portENTER_CRITICAL(&stateLock);
	expectedGeneration = generation;
	expected = pendingPartition;
	const bool ready = status.state == State::ReadyToRestart && !inFlight && hasOffer && expected;
	portEXIT_CRITICAL(&stateLock);
	if (!ready)
		return false;
	Error error = Error::None;
	if (gameActive)
		error = Error::GameActive;
	else if (WiFi.status() != WL_CONNECTED)
		error = Error::WifiUnavailable;
	else if (!clockValid())
		error = Error::ClockUnavailable;
	else if (inactivePartition() != expected)
		error = Error::UsbUpgradeRequired;

	portENTER_CRITICAL(&stateLock);
	if (generation != expectedGeneration || status.state != State::ReadyToRestart ||
		pendingPartition != expected || inFlight || !hasOffer)
	{
		portEXIT_CRITICAL(&stateLock);
		return false;
	}
	if (gameIsActive)
		error = Error::GameActive;
	if (error != Error::None)
	{
		invalidateLocked(error == Error::GameActive ? State::Cancelled : State::Failed, error);
		portEXIT_CRITICAL(&stateLock);
		return false;
	}
	// This is the final, non-cancellable activation boundary. Serialize it with
	// cancel/new requests using the state transition, never a flash operation
	// inside a critical section. The main loop has already flushed Preferences.
	status.state = State::Restarting;
	status.error = Error::None;
	++generation;
	clearOfferLocked();
	portEXIT_CRITICAL(&stateLock);

	const esp_err_t result = esp_ota_set_boot_partition(expected);
	if (result != ESP_OK)
	{
		Serial.printf("[OTA] esp_ota_set_boot_partition: %s\n", esp_err_to_name(result));
		portENTER_CRITICAL(&stateLock);
		status.state = State::Failed;
		status.error = Error::ActivationFailed;
		portEXIT_CRITICAL(&stateLock);
		return false;
	}
	return true;
}

const char *errorText(Error error)
{
	switch (error)
	{
	case Error::None: return "";
	case Error::Busy: return "Update busy; wait or cancel";
	case Error::WifiUnavailable: return "Connect to Wi-Fi and retry";
	case Error::GameActive: return "Exit the track before updating";
	case Error::ClockUnavailable: return "Clock sync failed; check internet";
	case Error::ConnectionFailed: return "Secure connection failed; retry";
	case Error::HttpFailed: return "Release server response rejected";
	case Error::ReleaseUnavailable: return "No published OTA release found";
	case Error::MetadataTooLarge: return "Release metadata exceeds limit";
	case Error::InvalidManifest: return "Invalid release metadata";
	case Error::IncompatibleFirmware: return "Firmware identity does not match";
	case Error::ImageTooLarge: return "Firmware exceeds expected size";
	case Error::UsbUpgradeRequired: return "USB OTA-layout upgrade required";
	case Error::OutOfMemory: return "Not enough memory; restart/retry";
	case Error::TimedOut: return "Update timed out; check internet";
	case Error::TruncatedImage: return "Download incomplete; retry";
	case Error::HashMismatch: return "Firmware checksum mismatch";
	case Error::WriteFailed: return "Flash write failed; retry";
	case Error::InvalidImage: return "Invalid ESP32 firmware image";
	case Error::ActivationFailed: return "Boot selection failed; use USB";
	case Error::PreferencesFailed: return "Settings save failed; not rebooted";
	}
	return "Firmware update failed";
}
}
