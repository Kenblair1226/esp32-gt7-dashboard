private:
	FirmwareUpdate::Status firmwareUpdateStatus;
	FirmwareUpdate::Status renderedFirmwareUpdateStatus;
	FirmwareUpdate::Action firmwareUpdateAction = FirmwareUpdate::Action::None;
	bool firmwareUpdateConfirmInstall = false;
	bool firmwareUpdateCancelPending = false;
	bool firmwareUpdateLastGameActive = false;
	bool firmwareUpdateRenderValid = false;
	bool renderedFirmwareUpdateConfirmation = false;
	bool renderedFirmwareUpdateCancellation = false;
	bool renderedFirmwareUpdateGameActive = false;

	static constexpr int FIRMWARE_BUTTON_X = 25;
	static constexpr int FIRMWARE_BUTTON_WIDTH = 270;
	static constexpr int FIRMWARE_BUTTON_HEIGHT = 44;
	static constexpr int FIRMWARE_BUTTON_TOP = 142;
	static constexpr int FIRMWARE_BUTTON_PITCH = 48;

	bool firmwareUpdateBusy() const
	{
		return FirmwareUpdate::isBusy(firmwareUpdateStatus.state);
	}

	void resetFirmwareUpdateInactivity()
	{
		const unsigned long now = millis();
		settingsLastInteractionTime = now;
		gameStoppedTime = now;
		gameStoppedTimerStarted = !previousGameRunning;
	}

	void resetFirmwareUpdateTouch()
	{
		settingsPressedButton = -1;
		waitForReleaseAfterScreenChange = true;
	}

	static void firmwareUpdateUpperText(const char *text, char *buffer, size_t capacity)
	{
		size_t index = 0;
		for (; text && text[index] && index + 1 < capacity; ++index)
		{
			const unsigned char character = static_cast<unsigned char>(text[index]);
			buffer[index] = character >= 'a' && character <= 'z'
				? character - 'a' + 'A'
				: (character == '\n' || (character >= ' ' && character <= '~')
					? character : '?');
		}
		buffer[index] = '\0';
	}

	uint8_t wrapFirmwareUpdateMessage(const char *text, char lines[4][49],
		uint8_t font, uint8_t maxLines, bool &complete)
	{
		tft.setTextFont(font);
		uint8_t count = 0;
		while (*text && count < maxLines)
		{
			while (*text == ' ') ++text;
			if (!*text) break;
			char *line = lines[count];
			size_t length = 0;
			size_t lastSpace = 0;
			while (text[length] && text[length] != '\n' && length < 48)
			{
				line[length] = text[length];
				line[length + 1] = '\0';
				if (tft.textWidth(line) > SCREEN_WIDTH - 32) break;
				if (text[length] == ' ') lastSpace = length;
				++length;
			}
			if (text[length] && text[length] != '\n' && lastSpace > 0)
				length = lastSpace;
			if (length == 0 && *text != '\n') length = 1;
			line[length] = '\0';
			text += length;
			if (*text == '\n') ++text;
			++count;
		}
		while (*text == ' ') ++text;
		complete = *text == '\0';
		return count;
	}

	void drawFirmwareUpdateMessage(const char *message, uint16_t color = TFT_LIGHTGREY)
	{
		char text[193];
		char lines[4][49] = {};
		firmwareUpdateUpperText(message, text, sizeof(text));
		bool complete = false;
		uint8_t font = 2;
		uint8_t count = wrapFirmwareUpdateMessage(text, lines, font, 3, complete);
		if (!complete)
		{
			font = 1;
			count = wrapFirmwareUpdateMessage(text, lines, font, 4, complete);
		}
		if (!complete && count > 0)
		{
			char *last = lines[count - 1];
			const size_t length = strlen(last);
			if (length >= 3) memcpy(last + length - 3, "...", 3);
		}
		const int lineHeight = font == 2 ? 18 : 12;
		const int firstY = 79 + (54 - count * lineHeight) / 2 + lineHeight / 2;
		tft.setTextDatum(MC_DATUM);
		tft.setTextColor(color, TFT_BLACK);
		for (uint8_t line = 0; line < count; ++line)
			tft.drawString(lines[line], X_CENTER, firstY + line * lineHeight, font);
	}

	void drawFirmwareUpdateIdentity(const char *label, const char *value, int y)
	{
		char text[64];
		char uppercase[64];
		snprintf(text, sizeof(text), "%s: %s", label, value);
		firmwareUpdateUpperText(text, uppercase, sizeof(uppercase));
		tft.setTextFont(2);
		const uint8_t font = tft.textWidth(uppercase) <= SCREEN_WIDTH - 24 ? 2 : 1;
		tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
		tft.setTextDatum(MC_DATUM);
		tft.drawString(uppercase, X_CENTER, y, font);
	}

	bool firmwareUpdateButtonEnabled(int button) const
	{
		if (button < 0 || button > 1) return false;
		if (firmwareUpdateBusy())
			return button == 1 && !firmwareUpdateCancelPending &&
				firmwareUpdateStatus.state != FirmwareUpdate::State::Restarting;
		if (firmwareUpdateConfirmInstall)
			return button == 0 || (!previousGameRunning &&
				firmwareUpdateStatus.state == FirmwareUpdate::State::Available &&
				firmwareUpdateStatus.availableVersion[0] != '\0');
		return button == 1 || (!previousGameRunning &&
			(firmwareUpdateStatus.state != FirmwareUpdate::State::Available ||
				firmwareUpdateStatus.availableVersion[0] != '\0'));
	}

	const char *firmwareUpdateButtonLabel(int button) const
	{
		using State = FirmwareUpdate::State;
		if (firmwareUpdateBusy())
		{
			if (button == 0) return nullptr;
			if (firmwareUpdateStatus.state == State::Restarting) return "RESTARTING";
			return firmwareUpdateCancelPending ? "CANCELLING" : "CANCEL";
		}
		if (firmwareUpdateConfirmInstall)
			return button == 0 ? "NOT NOW" : "INSTALL";
		if (button == 1) return "BACK";
		if (firmwareUpdateStatus.state == State::Available) return "REVIEW UPDATE";
		if (firmwareUpdateStatus.state == State::Failed ||
			firmwareUpdateStatus.state == State::Cancelled) return "RETRY CHECK";
		return "CHECK FOR UPDATE";
	}

	void drawFirmwareUpdateButton(int button, bool pressed = false)
	{
		if (button < 0 || button > 1) return;
		const char *label = firmwareUpdateButtonLabel(button);
		if (!label) return;
		const int y = FIRMWARE_BUTTON_TOP + button * FIRMWARE_BUTTON_PITCH;
		if (firmwareUpdateButtonEnabled(button))
		{
			drawSettingsButton(FIRMWARE_BUTTON_X, y, FIRMWARE_BUTTON_WIDTH,
				FIRMWARE_BUTTON_HEIGHT, label, pressed,
				firmwareUpdateConfirmInstall && button == 1);
		}
		else
		{
			const uint16_t fill = tft.color565(24, 26, 30);
			tft.fillRoundRect(FIRMWARE_BUTTON_X, y, FIRMWARE_BUTTON_WIDTH,
				FIRMWARE_BUTTON_HEIGHT, 7, fill);
			tft.drawRoundRect(FIRMWARE_BUTTON_X, y, FIRMWARE_BUTTON_WIDTH,
				FIRMWARE_BUTTON_HEIGHT, 7, TFT_DARKGREY);
			tft.setTextColor(TFT_DARKGREY, fill);
			tft.setTextDatum(MC_DATUM);
			tft.drawString(label, X_CENTER, y + FIRMWARE_BUTTON_HEIGHT / 2, 2);
		}
		tft.setTextDatum(TL_DATUM);
	}

	int firmwareUpdateButtonAtTouch() const
	{
		for (int button = 0; button < 2; ++button)
			if (firmwareUpdateButtonEnabled(button) &&
				touchInside(FIRMWARE_BUTTON_X,
					FIRMWARE_BUTTON_TOP + button * FIRMWARE_BUTTON_PITCH,
					FIRMWARE_BUTTON_WIDTH, FIRMWARE_BUTTON_HEIGHT))
				return button;
		return -1;
	}

	void drawFirmwareUpdateProgress()
	{
		tft.fillRect(25, 102, 270, 37, TFT_BLACK);
		tft.drawRoundRect(25, 104, 270, 12, 3, TFT_LIGHTGREY);
		const int width = 266 * firmwareUpdateStatus.progress / 100;
		if (width > 0) tft.fillRect(27, 106, width, 8, TFT_CYAN);
		char percentage[8];
		snprintf(percentage, sizeof(percentage), "%u%%",
			static_cast<unsigned int>(firmwareUpdateStatus.progress));
		tft.setTextDatum(MC_DATUM);
		tft.setTextColor(TFT_WHITE, TFT_BLACK);
		tft.drawString(percentage, X_CENTER, 128, 2);
	}

	void drawFirmwareUpdateStatus()
	{
		if (settingsScreen != SettingsScreen::FirmwareUpdate || screenSleeping) return;
		using State = FirmwareUpdate::State;
		const bool contentChanged = !firmwareUpdateRenderValid ||
			renderedFirmwareUpdateStatus.state != firmwareUpdateStatus.state ||
			renderedFirmwareUpdateStatus.error != firmwareUpdateStatus.error ||
			strcmp(renderedFirmwareUpdateStatus.availableVersion,
				firmwareUpdateStatus.availableVersion) != 0 ||
			renderedFirmwareUpdateConfirmation != firmwareUpdateConfirmInstall ||
			renderedFirmwareUpdateCancellation != firmwareUpdateCancelPending ||
			renderedFirmwareUpdateGameActive != previousGameRunning;
		if (contentChanged)
		{
			tft.fillRect(0, 76, SCREEN_WIDTH, SCREEN_HEIGHT - 76, TFT_BLACK);
			char message[193];
			if (firmwareUpdateCancelPending)
				drawFirmwareUpdateMessage("CANCELLING UPDATE\nWAITING FOR CONFIRMATION");
			else if (previousGameRunning && !firmwareUpdateBusy())
				drawFirmwareUpdateMessage("UPDATES DISABLED ON TRACK\nLEAVE TRACK TO CHECK OR INSTALL");
			else
			{
				switch (firmwareUpdateStatus.state)
				{
				case State::SyncingClock:
					drawFirmwareUpdateMessage("SYNCING SECURE CLOCK\nPLEASE WAIT");
					break;
				case State::Checking:
					drawFirmwareUpdateMessage("CHECKING FOR UPDATE\nPLEASE WAIT");
					break;
				case State::UpToDate:
					drawFirmwareUpdateMessage("FIRMWARE IS UP TO DATE", TFT_CYAN);
					break;
				case State::Available:
					snprintf(message, sizeof(message), firmwareUpdateConfirmInstall
						? "INSTALL VERSION %s?\nDEVICE WILL RESTART"
						: "UPDATE AVAILABLE\nVERSION %s",
						firmwareUpdateStatus.availableVersion);
					drawFirmwareUpdateMessage(message, TFT_CYAN);
					break;
				case State::Downloading:
					tft.setTextColor(TFT_WHITE, TFT_BLACK);
					tft.setTextDatum(MC_DATUM);
					tft.drawString("DOWNLOADING FIRMWARE", X_CENTER, 86, 2);
					drawFirmwareUpdateProgress();
					break;
				case State::Verifying:
					drawFirmwareUpdateMessage("VERIFYING FIRMWARE\nCHECKING DOWNLOAD");
					break;
				case State::ReadyToRestart:
					drawFirmwareUpdateMessage("UPDATE VERIFIED\nPREPARING RESTART", TFT_CYAN);
					break;
				case State::Restarting:
					drawFirmwareUpdateMessage("UPDATE COMPLETE\nRESTARTING", TFT_CYAN);
					break;
				case State::Cancelled:
					snprintf(message, sizeof(message), "UPDATE CANCELLED\n%s",
						firmwareUpdateStatus.error == FirmwareUpdate::Error::None
							? "CURRENT FIRMWARE UNCHANGED"
							: FirmwareUpdate::errorText(firmwareUpdateStatus.error));
					drawFirmwareUpdateMessage(message);
					break;
				case State::Failed:
					snprintf(message, sizeof(message), "UPDATE FAILED\n%s",
						FirmwareUpdate::errorText(firmwareUpdateStatus.error));
					drawFirmwareUpdateMessage(message, TFT_ORANGE);
					break;
				case State::Idle:
				default:
					drawFirmwareUpdateMessage("INTERNET WI-FI REQUIRED\nNO AUTOMATIC INSTALLATION");
					break;
				}
			}
			if (firmwareUpdateBusy())
			{
				tft.setTextDatum(MC_DATUM);
				tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
				tft.drawString("KEEP DEVICE POWERED", X_CENTER, 164, 2);
			}
			drawFirmwareUpdateButton(0);
			drawFirmwareUpdateButton(1);
		}
		else if (firmwareUpdateStatus.state == State::Downloading &&
			!firmwareUpdateCancelPending &&
			renderedFirmwareUpdateStatus.progress != firmwareUpdateStatus.progress)
		{
			drawFirmwareUpdateProgress();
		}
		renderedFirmwareUpdateStatus = firmwareUpdateStatus;
		renderedFirmwareUpdateConfirmation = firmwareUpdateConfirmInstall;
		renderedFirmwareUpdateCancellation = firmwareUpdateCancelPending;
		renderedFirmwareUpdateGameActive = previousGameRunning;
		firmwareUpdateRenderValid = true;
		tft.setTextFont(1);
		tft.setTextDatum(TL_DATUM);
	}

	void drawFirmwareUpdateScreen()
	{
		tft.setTextColor(TFT_WHITE, TFT_BLACK);
		tft.setTextDatum(MC_DATUM);
		tft.drawString("FIRMWARE UPDATE", X_CENTER, 18, 4);
		drawFirmwareUpdateIdentity("CURRENT", GT7_DASH_VERSION, 43);
		drawFirmwareUpdateIdentity("PANEL", FirmwareUpdateConfig::VARIANT, 62);
		tft.drawFastHLine(20, 74, SCREEN_WIDTH - 40, TFT_DARKGREY);
		firmwareUpdateRenderValid = false;
		drawFirmwareUpdateStatus();
	}

	void queueFirmwareUpdateAction(FirmwareUpdate::Action action)
	{
		using Action = FirmwareUpdate::Action;
		if (settingsScreen != SettingsScreen::FirmwareUpdate ||
			firmwareUpdateAction != Action::None || wifiResetRequested)
			return;
		if (action == Action::Cancel)
		{
			if (!firmwareUpdateButtonEnabled(1) || !firmwareUpdateBusy()) return;
			firmwareUpdateCancelPending = true;
		}
		else
		{
			if (firmwareUpdateBusy() || previousGameRunning ||
				(action != Action::Check && action != Action::Install))
				return;
			if (action == Action::Install &&
				(!firmwareUpdateConfirmInstall ||
					firmwareUpdateStatus.state != FirmwareUpdate::State::Available))
				return;
			// Cover the interval before the parent consumes this request.
			firmwareUpdateStatus.state = action == Action::Check
				? FirmwareUpdate::State::Checking : FirmwareUpdate::State::Downloading;
			firmwareUpdateStatus.error = FirmwareUpdate::Error::None;
			firmwareUpdateStatus.progress = 0;
			firmwareUpdateStatus.receivedBytes = 0;
			firmwareUpdateStatus.totalBytes = 0;
			firmwareUpdateStatus.httpStatus = 0;
			if (action == Action::Check) firmwareUpdateStatus.availableVersion[0] = '\0';
			firmwareUpdateConfirmInstall = false;
			firmwareUpdateCancelPending = false;
		}
		firmwareUpdateAction = action;
		resetFirmwareUpdateInactivity();
		resetFirmwareUpdateTouch();
		drawFirmwareUpdateStatus();
	}

	void activateFirmwareUpdateButton(int button)
	{
		if (!firmwareUpdateButtonEnabled(button)) return;
		if (firmwareUpdateBusy())
			queueFirmwareUpdateAction(FirmwareUpdate::Action::Cancel);
		else if (firmwareUpdateConfirmInstall)
		{
			if (button == 1)
				queueFirmwareUpdateAction(FirmwareUpdate::Action::Install);
			else
			{
				firmwareUpdateConfirmInstall = false;
				resetFirmwareUpdateTouch();
				drawFirmwareUpdateStatus();
			}
		}
		else if (button == 1)
			showSettingsScreen(SettingsScreen::DeviceSettings);
		else if (firmwareUpdateStatus.state == FirmwareUpdate::State::Available)
		{
			// INSTALL is on the opposite row, never under the review-opening tap.
			firmwareUpdateConfirmInstall = true;
			resetFirmwareUpdateTouch();
			drawFirmwareUpdateStatus();
		}
		else
			queueFirmwareUpdateAction(FirmwareUpdate::Action::Check);
	}

	void refreshFirmwareUpdateScreen()
	{
		const bool gameStarted = previousGameRunning && !firmwareUpdateLastGameActive;
		firmwareUpdateLastGameActive = previousGameRunning;
		if (settingsScreen != SettingsScreen::FirmwareUpdate) return;
		if (previousGameRunning && !firmwareUpdateBusy() &&
			(gameStarted || firmwareUpdateStatus.state == FirmwareUpdate::State::Cancelled))
		{
			closeSettings();
			return;
		}
		drawFirmwareUpdateStatus();
	}

public:
	FirmwareUpdate::Action takeFirmwareUpdateAction()
	{
		using Action = FirmwareUpdate::Action;
		const Action action = firmwareUpdateAction;
		firmwareUpdateAction = Action::None;
		if (action == Action::Check || action == Action::Install)
		{
			if (settingsScreen != SettingsScreen::FirmwareUpdate ||
				previousGameRunning || wifiResetRequested)
			{
				FirmwareUpdate::Status cancelled = firmwareUpdateStatus;
				cancelled.state = FirmwareUpdate::State::Cancelled;
				cancelled.error = previousGameRunning
					? FirmwareUpdate::Error::GameActive : FirmwareUpdate::Error::None;
				setFirmwareUpdateStatus(cancelled);
				return Action::None;
			}
		}
		else if (action == Action::Cancel &&
			(!firmwareUpdateBusy() ||
				firmwareUpdateStatus.state == FirmwareUpdate::State::Restarting))
		{
			firmwareUpdateCancelPending = false;
			return Action::None;
		}
		return action;
	}

	void setFirmwareUpdateStatus(const FirmwareUpdate::Status &status)
	{
		// An unconsumed touch request owns its immediate busy presentation.
		if (firmwareUpdateAction == FirmwareUpdate::Action::Check ||
			firmwareUpdateAction == FirmwareUpdate::Action::Install)
			return;
		FirmwareUpdate::Status next = status;
		next.progress = next.progress > 100 ? 100 : next.progress;
		next.availableVersion[sizeof(next.availableVersion) - 1] = '\0';
		for (size_t index = 0; next.availableVersion[index]; ++index)
		{
			const char character = next.availableVersion[index];
			if ((character < '0' || character > '9') && character != '.')
				next.availableVersion[index] = '?';
		}
		const bool wasBusy = firmwareUpdateBusy();
		const bool controlsChanged = next.state != firmwareUpdateStatus.state ||
			next.error != firmwareUpdateStatus.error ||
			strcmp(next.availableVersion, firmwareUpdateStatus.availableVersion) != 0;
		firmwareUpdateStatus = next;
		if (controlsChanged || next.state != FirmwareUpdate::State::Available)
			firmwareUpdateConfirmInstall = false;
		const bool busy = firmwareUpdateBusy();
		if (!busy)
		{
			firmwareUpdateCancelPending = false;
			firmwareUpdateAction = FirmwareUpdate::Action::None;
		}
		if (wasBusy != busy) resetFirmwareUpdateInactivity();
		if (busy)
		{
			wifiResetRequested = false;
			if (settingsScreen != SettingsScreen::FirmwareUpdate || screenSleeping)
				showSettingsScreen(SettingsScreen::FirmwareUpdate);
		}
		else if (wasBusy && previousGameRunning &&
			settingsScreen == SettingsScreen::FirmwareUpdate)
		{
			closeSettings();
			return;
		}
		if (settingsScreen == SettingsScreen::FirmwareUpdate)
		{
			if (controlsChanged) resetFirmwareUpdateTouch();
			drawFirmwareUpdateStatus();
		}
	}

	bool isFirmwareUpdateGameActive() const
	{
		return previousGameRunning;
	}

	bool prepareFirmwareUpdateRestart()
	{
		const bool brightnessSaved = savePendingBrightnessPreference();
		const bool themeSaved = savePendingDashboardPreference(
			"theme", static_cast<uint8_t>(activeDashboardTheme), themeSavePending);
		const bool touchSaved = savePendingDashboardPreference(
			"touchRot", static_cast<uint8_t>(touchRotation), touchRotationSavePending);
		if (!brightnessSaved || !themeSaved || !touchSaved)
		{
			Serial.println("OTA restart aborted: pending dashboard preferences could not be saved.");
			return false;
		}
		return true;
	}
