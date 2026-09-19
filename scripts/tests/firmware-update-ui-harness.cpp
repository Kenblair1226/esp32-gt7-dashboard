// The Node runner inserts current production members at the markers below.
// Only hardware/rendering dependencies are faked; touch and OTA UI logic is not copied.
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using String = std::string;

template <typename A, typename B, typename C>
A constrain(A value, B lower, C upper)
{
	return std::max(static_cast<A>(lower), std::min(value, static_cast<A>(upper)));
}

uint32_t clockNow = 1000;
unsigned long millis() { return clockNow; }

enum
{
	TFT_BLACK, TFT_WHITE, TFT_CYAN, TFT_LIGHTGREY, TFT_DARKGREY, TFT_ORANGE,
	TL_DATUM, MC_DATUM,
};

struct FakeTft
{
	bool touched = false;
	uint16_t x = 0, y = 0, brightness = 0;
	int font = 1, datum = MC_DATUM, draws = 0, clears = 0;
	std::vector<std::string> texts;

	void setBrightness(int value) { brightness = value; }
	bool getTouch(uint16_t *resultX, uint16_t *resultY)
	{
		*resultX = x;
		*resultY = y;
		return touched;
	}
	uint16_t color565(int, int, int) { return 10; }
	void setTextFont(int value) { font = value; }
	void setTextColor(int, int) {}
	void setTextPadding(int) {}
	void setTextDatum(int value) { datum = value; }
	int textWidth(const char *text)
	{
		return strlen(text) * (font == 1 ? 6 : font == 2 ? 8 : 13);
	}
	void checkRect(int left, int top, int width, int height)
	{
		assert(left >= 0 && top >= 0 && left + width <= 320 && top + height <= 240);
		++draws;
	}
	void fillScreen(int) { ++clears; ++draws; }
	void fillRect(int left, int top, int width, int height, int)
	{
		checkRect(left, top, width, height);
	}
	void drawRoundRect(int left, int top, int width, int height, int, int)
	{
		checkRect(left, top, width, height);
	}
	void fillRoundRect(int left, int top, int width, int height, int, int)
	{
		checkRect(left, top, width, height);
	}
	void drawFastHLine(int left, int top, int width, int)
	{
		checkRect(left, top, width, 1);
	}
	void drawString(const std::string &text, int centerX, int centerY, int textFont)
	{
		font = textFont;
		assert(centerY >= 0 && centerY <= 240);
		if (datum == MC_DATUM)
		{
			const int width = textWidth(text.c_str());
			assert(centerX - width / 2 >= 0 && centerX + width / 2 <= 320);
		}
		++draws;
		texts.push_back(text);
	}
} tft;

struct Preferences
{
	bool success = true;
	int writes = 0;
	std::map<std::string, uint8_t> saved;

	size_t putUChar(const char *key, uint8_t value)
	{
		++writes;
		if (!success) return 0;
		saved[key] = value;
		return 1;
	}
};

struct FakeSerial
{
	std::vector<std::string> messages;
	void println(const char *message) { messages.push_back(message); }
} Serial;

constexpr int SCREEN_WIDTH = 320, SCREEN_HEIGHT = 240, X_CENTER = 160;
bool forceUpdate = false;
struct DashboardState {};

// @production:DECLARATIONS

namespace FirmwareUpdate
{
// @production:ERROR_TEXT
}

class UI
{
	friend struct UiCases;

private:
// @production:FIELDS

public:
	int rendererInvalidations = 0, calibrationDraws = 0;
	void invalidateDashboardRenderer() { ++rendererInvalidations; forceUpdate = true; }
	void redrawAfterWifiResetDialog() { invalidateDashboardRenderer(); }
	void renderThemePreview() {}
	void drawTouchCalibrationScreen(int = -1) { ++calibrationDraws; }
	void drawDeviceBrightnessValue() {}
	void drawTouchCalibrationHint(bool) {}
	void fadeScreenOn()
	{
		currentBrightness = normalBrightness();
		tft.setBrightness(currentBrightness);
	}
	bool selectDashboardTheme(DashboardTheme selected, bool)
	{
		activeDashboardTheme = selected;
		return true;
	}
	static size_t dashboardThemeIndex(DashboardTheme theme) { return static_cast<size_t>(theme); }
	bool touchInside(int left, int top, int width, int height) const
	{
		return touchX >= left && touchX < left + width &&
			touchY >= top && touchY < top + height;
	}

// @production:FIRMWARE_SCREEN

// @production:SETTINGS_METHODS
};

using State = FirmwareUpdate::State;
using Error = FirmwareUpdate::Error;
using Action = FirmwareUpdate::Action;

static_assert(std::is_same<decltype(&UI::takeFirmwareUpdateAction), Action (UI::*)()>::value,
	"The action hook must be public with the coordinator's signature");
static_assert(std::is_same<decltype(&UI::setFirmwareUpdateStatus),
	void (UI::*)(const FirmwareUpdate::Status &)>::value,
	"The snapshot hook must be public with the coordinator's signature");
static_assert(std::is_same<decltype(&UI::isFirmwareUpdateGameActive), bool (UI::*)() const>::value,
	"The activity hook must remain public and const");
static_assert(std::is_same<decltype(&UI::prepareFirmwareUpdateRestart), bool (UI::*)()>::value,
	"The preference-flush hook must remain public");
static_assert(DASHBOARD_THEME_COUNT == 7, "The existing seven themes must be retained");

struct TestCase
{
	const char *name;
	void (*run)();
};

struct UiCases
{
	using Screen = UI::SettingsScreen;

	static void release(UI &ui)
	{
		tft.touched = false;
		++clockNow;
		ui.readTouch();
	}
	static void press(UI &ui, int x, int y)
	{
		tft.x = x;
		tft.y = y;
		tft.touched = true;
		++clockNow;
		ui.readTouch();
	}
	static void tap(UI &ui, int y, int x = 160)
	{
		release(ui);
		press(ui, x, y);
		release(ui);
		release(ui);
	}
	static void page(UI &ui)
	{
		ui.showSettingsScreen(Screen::FirmwareUpdate);
		release(ui);
	}
	static FirmwareUpdate::Status status(State state, Error error = Error::None, int progress = 0)
	{
		FirmwareUpdate::Status snapshot;
		snapshot.state = state;
		snapshot.error = error;
		snapshot.progress = progress;
		if (state == State::Available) strcpy(snapshot.availableVersion, "1.9.0");
		return snapshot;
	}

	static const TestCase *cases(size_t &count)
	{
		static const TestCase tests[] = {
			{"idle, no startup action and request busy protection", [] {
				UI ui;
				assert(ui.takeFirmwareUpdateAction() == Action::None);
				page(ui);
				assert(!ui.firmwareUpdateBusy());
				tap(ui, 164);
				assert(ui.firmwareUpdateStatus.state == State::Checking && ui.firmwareUpdateBusy());
				ui.setFirmwareUpdateStatus(status(State::Idle));
				assert(ui.firmwareUpdateStatus.state == State::Checking);
				clockNow += 310000;
				release(ui);
				assert(ui.settingsScreen == Screen::FirmwareUpdate);
				assert(ui.takeFirmwareUpdateAction() == Action::Check);
				assert(ui.takeFirmwareUpdateAction() == Action::None);
				ui.showSettingsScreen(Screen::Main);
				ui.closeSettings();
				assert(ui.settingsScreen == Screen::FirmwareUpdate);
				ui.wifiResetRequested = true;
				assert(!ui.takeWifiResetRequest() && !ui.wifiResetRequested);
			}},
			{"separate available-version install confirmation", [] {
				UI ui;
				page(ui);
				ui.setFirmwareUpdateStatus(status(State::Available));
				release(ui);
				tap(ui, 164);
				assert(ui.firmwareUpdateConfirmInstall && ui.takeFirmwareUpdateAction() == Action::None);
				tap(ui, 164);
				assert(!ui.firmwareUpdateConfirmInstall && ui.takeFirmwareUpdateAction() == Action::None);
				tap(ui, 164);
				tap(ui, 212);
				assert(ui.firmwareUpdateStatus.state == State::Downloading);
				assert(ui.takeFirmwareUpdateAction() == Action::Install);
				assert(ui.takeFirmwareUpdateAction() == Action::None);
			}},
			{"back from available does not install or preserve queued actions", [] {
				UI ui;
				page(ui);
				ui.setFirmwareUpdateStatus(status(State::Available));
				release(ui);
				tap(ui, 212);
				assert(ui.settingsScreen == Screen::DeviceSettings);
				assert(ui.takeFirmwareUpdateAction() == Action::None);
			}},
			{"held touch cannot cross asynchronous version/state change", [] {
				UI ui;
				page(ui);
				ui.setFirmwareUpdateStatus(status(State::Available));
				release(ui);
				tap(ui, 164);
				press(ui, 160, 212);
				auto snapshot = status(State::Available);
				strcpy(snapshot.availableVersion, "1.10.0");
				ui.setFirmwareUpdateStatus(snapshot);
				release(ui);
				release(ui);
				assert(!ui.firmwareUpdateConfirmInstall && ui.takeFirmwareUpdateAction() == Action::None);
			}},
			{"cancel waits for backend acknowledgement through ready-to-restart", [] {
				UI ui;
				page(ui);
				ui.setFirmwareUpdateStatus(status(State::Downloading, Error::None, 20));
				release(ui);
				tap(ui, 212);
				assert(ui.firmwareUpdateCancelPending && ui.firmwareUpdateBusy());
				assert(ui.takeFirmwareUpdateAction() == Action::Cancel);
				assert(ui.takeFirmwareUpdateAction() == Action::None);
				ui.setFirmwareUpdateStatus(status(State::ReadyToRestart));
				assert(ui.firmwareUpdateBusy() && ui.firmwareUpdateCancelPending);
				ui.showSettingsScreen(Screen::WifiResetConfirmation);
				assert(ui.settingsScreen == Screen::FirmwareUpdate);
				clockNow += 310000;
				release(ui);
				assert(ui.settingsScreen == Screen::FirmwareUpdate);
				ui.setFirmwareUpdateStatus(status(State::Cancelled));
				assert(!ui.firmwareUpdateBusy() && !ui.firmwareUpdateCancelPending);
				assert(ui.settingsLastInteractionTime == clockNow && ui.gameStoppedTime == clockNow);
				release(ui);
				clockNow += 14900;
				release(ui);
				assert(ui.settingsScreen == Screen::FirmwareUpdate);
				clockNow += 101;
				release(ui);
				assert(ui.settingsScreen == Screen::Closed && ui.rendererInvalidations > 0);
			}},
			{"ready is cancellable but restarting is not", [] {
				UI ui;
				ui.setFirmwareUpdateStatus(status(State::ReadyToRestart));
				assert(ui.firmwareUpdateBusy() && ui.firmwareUpdateButtonEnabled(1));
				ui.setFirmwareUpdateStatus(status(State::Restarting));
				assert(ui.firmwareUpdateBusy() && !ui.firmwareUpdateButtonEnabled(1));
				clockNow += 310000;
				release(ui);
				release(ui);
				assert(ui.settingsScreen == Screen::FirmwareUpdate);
			}},
			{"gameplay guards consume no stale action and return telemetry", [] {
				UI ui;
				page(ui);
				tap(ui, 164);
				ui.previousGameRunning = true;
				assert(ui.takeFirmwareUpdateAction() == Action::None);
				assert(ui.firmwareUpdateStatus.error == Error::GameActive);
				assert(ui.settingsScreen == Screen::Closed);
				ui.setFirmwareUpdateStatus(status(State::Idle));
				page(ui);
				assert(!ui.firmwareUpdateButtonEnabled(0));
				tap(ui, 164);
				assert(ui.takeFirmwareUpdateAction() == Action::None);
				ui.previousGameRunning = false;
				ui.refreshFirmwareUpdateScreen();
				ui.setFirmwareUpdateStatus(status(State::Downloading));
				release(ui);
				ui.previousGameRunning = true;
				ui.refreshFirmwareUpdateScreen();
				assert(ui.settingsScreen == Screen::FirmwareUpdate);
				ui.setFirmwareUpdateStatus(status(State::Cancelled, Error::GameActive));
				assert(ui.settingsScreen == Screen::Closed);
			}},
			{"available screen yields to newly resumed gameplay", [] {
				UI ui;
				page(ui);
				ui.setFirmwareUpdateStatus(status(State::Available));
				release(ui);
				ui.previousGameRunning = true;
				ui.refreshFirmwareUpdateScreen();
				assert(ui.settingsScreen == Screen::Closed);
			}},
			{"byte-only snapshots draw nothing, percentage redraws no full screen", [] {
				UI ui;
				page(ui);
				auto snapshot = status(State::Downloading, Error::None, 12);
				ui.setFirmwareUpdateStatus(snapshot);
				const int draws = tft.draws, clears = tft.clears;
				for (int index = 0; index < 100; ++index)
				{
					snapshot.receivedBytes += 512;
					snapshot.totalBytes += 512;
					ui.setFirmwareUpdateStatus(snapshot);
					ui.refreshFirmwareUpdateScreen();
				}
				assert(tft.draws == draws && tft.clears == clears);
				snapshot.progress = 13;
				ui.setFirmwareUpdateStatus(snapshot);
				assert(tft.draws > draws && tft.clears == clears);
			}},
			{"long typed errors fit bounded layout and allow retry/back", [] {
				UI ui;
				page(ui);
				for (int value = static_cast<int>(Error::Busy);
					value <= static_cast<int>(Error::PreferencesFailed); ++value)
				{
					ui.setFirmwareUpdateStatus(status(State::Failed, static_cast<Error>(value)));
					release(ui);
					assert(ui.firmwareUpdateButtonEnabled(0) && ui.firmwareUpdateButtonEnabled(1));
				}
				ui.drawFirmwareUpdateMessage(
					"USB UPGRADE REQUIRED. INSTALL THE COMPLETE OTA-CAPABLE FIRMWARE LAYOUT FIRST. "
					"KEEP YOUR SETTINGS BY FLASHING WITHOUT ERASE. RETURN TO DEVICE SETTINGS AND RETRY.");
				tap(ui, 164);
				assert(ui.takeFirmwareUpdateAction() == Action::Check);
			}},
			{"preference failures retained; activation preparation flushes all", [] {
				UI ui;
				ui.dashboardPreferencesReady = true;
				ui.dashboardPreferences.success = false;
				ui.brightnessSavePending = true;
				ui.themeSavePending = true;
				ui.touchRotationSavePending = true;
				clockNow += 1000;
				ui.saveBrightnessIfDue();
				assert(ui.brightnessSavePending && !ui.prepareFirmwareUpdateRestart());
				assert(!Serial.messages.empty());
				ui.dashboardPreferences.success = true;
				assert(ui.prepareFirmwareUpdateRestart());
				assert(!ui.brightnessSavePending && !ui.themeSavePending && !ui.touchRotationSavePending);
				assert(ui.dashboardPreferences.saved["brightness"] == 80);
				assert(ui.dashboardPreferences.saved["theme"] == static_cast<uint8_t>(DashboardTheme::GT3));
				UI unopened;
				unopened.brightnessSavePending = true;
				assert(!unopened.prepareFirmwareUpdateRestart());
			}},
			{"brightness failure warns once, retries, and re-arms after either save path", [] {
				UI ui;
				ui.dashboardPreferencesReady = true;
				ui.dashboardPreferences.success = false;
				ui.scheduleBrightnessSave();
				ui.saveBrightnessIfDue();
				assert(Serial.messages.empty() && ui.dashboardPreferences.writes == 0);
				for (int retry = 0; retry < 8; ++retry)
				{
					clockNow += UI::BRIGHTNESS_SAVE_DELAY_MS;
					ui.saveBrightnessIfDue();
				}
				assert(ui.brightnessSavePending && ui.dashboardPreferences.writes == 8);
				assert(Serial.messages.size() == 1);
				assert(Serial.messages[0].find("Brightness preference save failed") != std::string::npos);
				assert(Serial.messages[0].find("retrying") != std::string::npos);
				assert(Serial.messages[0].find("Keep power on") != std::string::npos);
				ui.dashboardPreferences.success = true;
				clockNow += UI::BRIGHTNESS_SAVE_DELAY_MS;
				ui.saveBrightnessIfDue();
				assert(!ui.brightnessSavePending && !ui.brightnessSaveFailureReported);
				ui.dashboardPreferences.success = false;
				ui.scheduleBrightnessSave();
				clockNow += UI::BRIGHTNESS_SAVE_DELAY_MS;
				ui.saveBrightnessIfDue();
				assert(Serial.messages.size() == 2);
				ui.dashboardPreferences.success = true;
				assert(ui.prepareFirmwareUpdateRestart());
				assert(!ui.brightnessSaveFailureReported);
				ui.dashboardPreferences.success = false;
				ui.scheduleBrightnessSave();
				clockNow += UI::BRIGHTNESS_SAVE_DELAY_MS;
				ui.saveBrightnessIfDue();
				assert(Serial.messages.size() == 3);
			}},
			{"brightness clamp/default, same-screen taps and deferred save", [] {
				UI ui;
				ui.dashboardPreferencesReady = true;
				ui.showSettingsScreen(Screen::DeviceSettings);
				release(ui);
				assert(ui.userBrightnessPercent == 80);
				for (int index = 0; index < 10; ++index) tap(ui, 85, 50);
				assert(ui.userBrightnessPercent == 20 && !ui.waitForReleaseAfterScreenChange);
				for (int index = 0; index < 10; ++index) tap(ui, 85, 260);
				assert(ui.userBrightnessPercent == 100 && ui.currentBrightness == 255);
				assert(ui.brightnessSavePending);
				assert(ui.prepareFirmwareUpdateRestart());
				assert(ui.dashboardPreferences.saved["brightness"] == 100);
			}},
			{"separate WIFI confirmation and consistent device hit targets", [] {
				UI ui;
				ui.showSettingsScreen(Screen::DeviceSettings);
				release(ui);
				tap(ui, 140);
				assert(ui.settingsScreen == Screen::FirmwareUpdate);
				tap(ui, 212);
				assert(ui.settingsScreen == Screen::DeviceSettings);
				tap(ui, 204, 60);
				assert(ui.settingsScreen == Screen::WifiResetConfirmation);
				assert(!ui.takeWifiResetRequest());
				tap(ui, 165, 225);
				assert(ui.takeWifiResetRequest() && !ui.takeWifiResetRequest());
			}},
			{"first touch wakes only and restores existing brightness", [] {
				UI ui;
				ui.screenSleeping = true;
				ui.userBrightnessPercent = 60;
				tap(ui, 100);
				assert(!ui.screenSleeping && ui.settingsScreen == Screen::Closed);
				assert(ui.currentBrightness == 153 && ui.takeFirmwareUpdateAction() == Action::None);
				tap(ui, 100);
				assert(ui.settingsScreen == Screen::Main);
			}},
			{"held settings touch release refreshes timeout before same-screen action", [] {
				UI ui;
				ui.showSettingsScreen(Screen::DeviceSettings);
				release(ui);
				press(ui, 260, 85);
				clockNow += 16000;
				release(ui);
				assert(ui.settingsScreen == Screen::DeviceSettings && ui.userBrightnessPercent == 90);
				assert(!ui.waitForReleaseAfterScreenChange);
			}},
			{"touch rotation calibration still requires verification before saving", [] {
				UI ui;
				ui.dashboardPreferencesReady = true;
				ui.showTouchCalibration(UI::TouchRotation::Deg0);
				release(ui);
				tap(ui, 195, 220);
				assert(ui.settingsScreen == Screen::TouchCalibration);
				tap(ui, 104, 252);
				assert(ui.touchCalibrationVerified);
				tap(ui, 195, 220);
				assert(ui.settingsScreen == Screen::Closed);
				assert(ui.dashboardPreferences.saved["touchRot"] == 0);
			}},
			{"version snapshot is bounded and not raw remote text", [] {
				UI ui;
				page(ui);
				auto snapshot = status(State::Available);
				memset(snapshot.availableVersion, '<', sizeof(snapshot.availableVersion));
				ui.setFirmwareUpdateStatus(snapshot);
				assert(ui.firmwareUpdateStatus.availableVersion[32] == '\0');
				assert(strlen(ui.firmwareUpdateStatus.availableVersion) == 32);
				assert(ui.firmwareUpdateStatus.availableVersion[0] == '?');
			}},
		};
		count = sizeof(tests) / sizeof(tests[0]);
		return tests;
	}
};

int main(int argc, char **argv)
{
	size_t count = 0;
	const TestCase *cases = UiCases::cases(count);
	if (argc == 2 && strcmp(argv[1], "--list") == 0)
	{
		for (size_t index = 0; index < count; ++index)
			std::cout << cases[index].name << '\n';
		return 0;
	}
	if (argc == 2)
	{
		for (size_t index = 0; index < count; ++index)
		{
			if (strcmp(argv[1], cases[index].name) != 0) continue;
			tft = FakeTft{};
			clockNow = 1000;
			Serial.messages.clear();
			cases[index].run();
			std::cout << "PASS " << cases[index].name << '\n';
			return 0;
		}
	}
	std::cerr << "Expected --list or an exact regression case name.\n";
	return 2;
}
