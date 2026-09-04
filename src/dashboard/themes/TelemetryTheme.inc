	void drawTelemetryCard(int x, int y, int width, int height,
		uint16_t background, uint16_t border, uint16_t accent)
	{
		tft.fillRoundRect(x, y, width, height, 5, background);
		tft.drawRoundRect(x, y, width, height, 5, border);
		tft.fillRoundRect(x + 7, y + 1, min(34, width - 14), 3, 1, accent);
	}

	void drawTelemetryValue(int x, int y, int width, int height,
		const String &value, const String &id, uint16_t color,
		uint16_t background, uint8_t font, textdatum_t datum,
		bool forceUpdate)
	{
		const String cacheState = value + ":" + String(color);
		if (!forceUpdate && prevData[id] == cacheState) return;

		tft.fillRect(x, y, width, height, background);
		tft.setTextColor(color, background);
		tft.setTextDatum(datum);
		tft.setTextSize(1.0f);
		const int textX = datum == MR_DATUM ? x + width - 1
			: (datum == MC_DATUM ? x + width / 2 : x + 1);
		tft.drawString(value, textX, y + height / 2, font);
		tft.setTextDatum(TL_DATUM);
		prevData[id] = cacheState;
	}

	void drawTelemetryValueBuffered(int x, int y, int width, int height,
		const String &value, const String &id, uint16_t color,
		uint16_t background, uint8_t font, textdatum_t datum,
		bool forceUpdate, LGFX_Sprite &sprite, bool &spriteCreated)
	{
		const String cacheState = value + ":" + String(color);
		if (!forceUpdate && prevData[id] == cacheState) return;
		if (!spriteCreated)
		{
			sprite.setColorDepth(16);
			spriteCreated = sprite.createSprite(width, height) != nullptr;
		}
		if (!spriteCreated)
		{
			drawTelemetryValue(x, y, width, height, value, id, color,
				background, font, datum, true);
			return;
		}

		sprite.fillSprite(background);
		sprite.setTextColor(color, background);
		sprite.setTextDatum(datum);
		sprite.setTextSize(1.0f);
		const int textX = datum == MR_DATUM ? width - 1
			: (datum == MC_DATUM ? width / 2 : 1);
		sprite.drawString(value, textX, height / 2, font);
		sprite.pushSprite(x, y);
		prevData[id] = cacheState;
		releaseSpriteAfterThemePreview(sprite, spriteCreated);
	}

	void drawTelemetryGear(const String &gear, uint16_t foreground,
		uint16_t background, bool forceUpdate)
	{
		if (!forceUpdate && prevData["telemetryGear"] == gear) return;

		static constexpr int X = 91;
		static constexpr int Y = 57;
		static constexpr int WIDTH = 57;
		static constexpr int HEIGHT = 68;
		tft.fillRect(X, Y, WIDTH, HEIGHT, background);
		tft.setTextColor(foreground, background);
		tft.setTextDatum(MC_DATUM);
		if (gear.length() == 1 && gear[0] >= '0' && gear[0] <= '9')
		{
			tft.setTextSize(1.0f);
			tft.drawString(gear, X + WIDTH / 2, Y + HEIGHT / 2, 7);
		}
		else
		{
			tft.setTextSize(2.0f);
			tft.drawString(gear, X + WIDTH / 2, Y + HEIGHT / 2, 4);
			tft.setTextSize(1.0f);
		}
		tft.setTextDatum(TL_DATUM);
		prevData["telemetryGear"] = gear;
	}

	void drawTelemetryRpm(const DashboardState &state, uint16_t background,
		bool forceUpdate)
	{
		static constexpr int SEGMENT_COUNT = 27;
		static constexpr int SEGMENT_X = 74;
		static constexpr int SEGMENT_Y = 9;
		static constexpr int SEGMENT_PITCH = 9;
		static constexpr int SEGMENT_WIDTH = 6;
		static constexpr int SEGMENT_HEIGHT = 14;
		static constexpr uint32_t FRAME_MS = 24;
		static int previousActive = -1;
		static bool previousPulse = false;
		static uint32_t pulseStart = 0;
		static uint32_t lastFrame = 0;
		static uint8_t previousMix = 0;

		const int active = constrain(
			(state.rpmPercent * SEGMENT_COUNT + 99) / 100, 0, SEGMENT_COUNT);
		const bool warning = state.rpmAlertRangeValid &&
			state.rpmPercent >= state.rpmRedLineSetting;
		const bool pulse = warning || state.revLimitAlertActive;
		const bool pulseStarted = pulse && !previousPulse;
		const uint32_t now = millis();
		if (pulseStarted) pulseStart = now;

		uint8_t whiteMix = 0;
		bool pulseFrame = false;
		if (pulse && (pulseStarted || now - lastFrame >= FRAME_MS))
		{
			float peak = warning ? 0.48f : 0.35f;
			if (state.revLimitAlertActive) peak = 0.70f;
			const float elapsed = (now - pulseStart) / 1000.0f;
			const float wave = 0.5f + 0.5f * cosf(TWO_PI * 3.0f * elapsed);
			whiteMix = static_cast<uint8_t>(lroundf(255.0f * peak * wave));
			pulseFrame = pulseStarted || whiteMix != previousMix;
			lastFrame = now;
		}
		else if (pulse)
		{
			whiteMix = previousMix;
		}

		const String rangeState = String(state.rpmRedLineSetting) + ":" +
			String(state.rpmAlertRangeValid ? 1 : 0);
		const bool rangeChanged = prevData["telemetryRpmRange"] != rangeState;
		const bool pulseEnded = !pulse && previousPulse;
		if (!forceUpdate && active == previousActive && !rangeChanged &&
			!pulseFrame && !pulseEnded)
			return;

		const uint16_t inactive = tft.color565(31, 43, 58);
		const uint16_t cyan = tft.color565(47, 213, 255);
		const uint16_t violet = tft.color565(145, 92, 246);
		const uint16_t rose = tft.color565(255, 76, 126);
		const float alertPosition = state.rpmAlertRangeValid
			? constrain(state.rpmRedLineSetting / 100.0f, 0.40f, 0.98f)
			: 0.82f;

		for (int i = 0; i < SEGMENT_COUNT; ++i)
		{
			const bool fillChanged = previousActive < 0 ||
				(i >= min(active, previousActive) && i < max(active, previousActive));
			const bool pulseChanged = i < active && (pulseFrame || pulseEnded);
			if (!forceUpdate && !rangeChanged && !fillChanged && !pulseChanged)
				continue;

			uint16_t color = inactive;
			if (i < active)
			{
				const float position = i / static_cast<float>(SEGMENT_COUNT - 1);
				if (position >= alertPosition)
					color = rose;
				else
				{
					const uint8_t mix = static_cast<uint8_t>(constrain(
						position / max(0.01f, alertPosition) * 255.0f,
						0.0f, 255.0f));
					color = blendRgb565(cyan, violet, mix);
				}
				if (pulse) color = blendRgb565(color, TFT_WHITE, whiteMix);
			}
			tft.fillRect(SEGMENT_X + i * SEGMENT_PITCH, SEGMENT_Y,
				SEGMENT_WIDTH, SEGMENT_HEIGHT, color);
		}

		if (forceUpdate || rangeChanged)
		{
			tft.fillRect(SEGMENT_X - 3, 24,
				SEGMENT_PITCH * (SEGMENT_COUNT - 1) + SEGMENT_WIDTH + 6,
				6, background);
			if (state.rpmAlertRangeValid)
			{
				const int markerX = SEGMENT_X + lroundf(
					alertPosition * SEGMENT_PITCH * (SEGMENT_COUNT - 1)) +
					SEGMENT_WIDTH / 2;
				tft.fillTriangle(markerX - 3, 29, markerX + 3, 29,
					markerX, 24, rose);
			}
		}

		previousActive = active;
		previousPulse = pulse;
		previousMix = pulse ? whiteMix : 0;
		prevData["telemetryRpmRange"] = rangeState;
	}

	void drawTelemetryFuel(const DashboardState &state, uint16_t foreground,
		uint16_t muted, uint16_t background, uint16_t accent,
		bool forceUpdate)
	{
		const String cacheState = state.fuelLabel + ":" +
			state.fuelDisplayValue + ":" + String(state.fuelProgressPercent) +
			":" + String(state.fuelValueValid ? 1 : 0) + ":" +
			state.tyrePressureFrontLeft;
		if (!forceUpdate && prevData["telemetryFuel"] == cacheState) return;

		tft.fillRect(246, 42, 64, 79, background);
		tft.setTextDatum(TL_DATUM);
		tft.setTextSize(1.0f);
		tft.setTextColor(muted, background);
		tft.drawString(state.fuelLabel, 248, 43, 1);
		tft.setTextDatum(MC_DATUM);
		tft.setTextColor(foreground, background);
		String fuelValue = state.fuelDisplayValue;
		if (state.fuelValueValid && !state.fuelIsEV) fuelValue += "%";
		tft.drawString(fuelValue, 278, 63, 2);

		const uint16_t empty = tft.color565(36, 49, 65);
		tft.drawRoundRect(248, 75, 60, 7, 2, empty);
		if (state.fuelValueValid)
		{
			const int fillWidth = constrain(
				state.fuelProgressPercent * 56 / 100, 0, 56);
			if (fillWidth > 0)
				tft.fillRoundRect(250, 77, fillWidth, 3, 1,
					state.fuelProgressPercent <= 15
						? tft.color565(255, 76, 126) : accent);
		}

		tft.setTextDatum(TL_DATUM);
		tft.setTextColor(muted, background);
		tft.drawString("REM LAPS", 248, 89, 1);
		tft.setTextDatum(MC_DATUM);
		tft.setTextColor(foreground, background);
		tft.drawString(state.tyrePressureFrontLeft, 278, 108, 2);
		tft.setTextDatum(TL_DATUM);
		prevData["telemetryFuel"] = cacheState;
	}

	void drawTelemetryStatusBadge(int x, const char *label, bool active,
		const String &id, uint16_t activeColor, uint16_t foreground,
		uint16_t muted, uint16_t background, bool forceUpdate)
	{
		const String cacheState = active ? "1" : "0";
		if (!forceUpdate && prevData[id] == cacheState) return;

		const uint16_t fill = active
			? blendRgb565(background, activeColor, 72)
			: tft.color565(22, 30, 42);
		tft.fillRoundRect(x, 124, 29, 17, 4, fill);
		tft.drawRoundRect(x, 124, 29, 17, 4, active ? activeColor : muted);
		tft.setTextColor(active ? foreground : muted, fill);
		tft.setTextDatum(MC_DATUM);
		tft.setTextSize(1.0f);
		tft.drawString(label, x + 14, 132, 1);
		tft.setTextDatum(TL_DATUM);
		prevData[id] = cacheState;
	}

	void drawTelemetryPedals(const DashboardState &state, uint16_t background,
		uint16_t border, bool forceUpdate, LGFX_Sprite &sprite,
		bool &spriteCreated)
	{
		const int brakeInput = constrain(state.absLevel.toInt(), 0, 100);
		const int brakeApplied = constrain(state.absFilteredLevel.toInt(), 0, 100);
		const int throttleInput = constrain(state.tcLevel.toInt(), 0, 100);
		const int throttleApplied = constrain(state.tcFilteredLevel.toInt(), 0, 100);
		const String cacheState = String(brakeInput) + ":" +
			String(brakeApplied) + ":" + String(throttleInput) + ":" +
			String(throttleApplied);
		if (!forceUpdate && prevData["telemetryPedals"] == cacheState) return;

		if (!spriteCreated)
		{
			sprite.setColorDepth(16);
			spriteCreated = sprite.createSprite(140, 31) != nullptr;
		}

		const uint16_t inputColors[] = {
			tft.color565(101, 42, 66), tft.color565(27, 82, 92)};
		const uint16_t appliedColors[] = {
			tft.color565(255, 76, 126), tft.color565(47, 213, 187)};
		const int values[2][2] = {
			{brakeInput, brakeApplied}, {throttleInput, throttleApplied}};
		const char *labels[] = {"B", "T"};

		if (spriteCreated)
		{
			sprite.fillSprite(background);
			sprite.setTextColor(tft.color565(180, 190, 202), background);
			sprite.setTextDatum(ML_DATUM);
			sprite.setTextSize(1.0f);
			for (int row = 0; row < 2; ++row)
			{
				const int y = 2 + row * 15;
				sprite.drawString(labels[row], 1, y + 5, 1);
				sprite.drawRoundRect(13, y, 126, 11, 3, border);
				const int inputWidth = 122 * values[row][0] / 100;
				const int appliedWidth = 122 * values[row][1] / 100;
				if (inputWidth > 0)
					sprite.fillRoundRect(15, y + 2, inputWidth, 7, 2,
						inputColors[row]);
				if (appliedWidth > 0)
					sprite.fillRoundRect(15, y + 2, appliedWidth, 7, 2,
						appliedColors[row]);
			}
			sprite.pushSprite(170, 166);
			releaseSpriteAfterThemePreview(sprite, spriteCreated);
		}
		else
		{
			tft.fillRect(170, 166, 140, 31, background);
			tft.setTextColor(tft.color565(180, 190, 202), background);
			tft.setTextDatum(ML_DATUM);
			for (int row = 0; row < 2; ++row)
			{
				const int y = 168 + row * 15;
				tft.drawString(labels[row], 171, y + 5, 1);
				tft.drawRoundRect(183, y, 126, 11, 3, border);
				const int inputWidth = 122 * values[row][0] / 100;
				const int appliedWidth = 122 * values[row][1] / 100;
				if (inputWidth > 0)
					tft.fillRoundRect(185, y + 2, inputWidth, 7, 2,
						inputColors[row]);
				if (appliedWidth > 0)
					tft.fillRoundRect(185, y + 2, appliedWidth, 7, 2,
						appliedColors[row]);
			}
			tft.setTextDatum(TL_DATUM);
		}
		prevData["telemetryPedals"] = cacheState;
	}

	void drawTelemetryTyres(const DashboardState &state, uint16_t background,
		uint16_t muted, bool forceUpdate, LGFX_Sprite &sprite,
		bool &spriteCreated)
	{
		String cacheState;
		for (int i = 0; i < 4; ++i)
		{
			if (i > 0) cacheState += ":";
			cacheState += isfinite(state.tyreTemperatures[i])
				? String(lroundf(state.tyreTemperatures[i])) : String("--");
		}
		if (!forceUpdate && prevData["telemetryTyres"] == cacheState) return;

		if (!spriteCreated)
		{
			sprite.setColorDepth(16);
			spriteCreated = sprite.createSprite(98, 28) != nullptr;
		}

		const char *labels[] = {"FL", "FR", "RL", "RR"};
		if (spriteCreated)
		{
			sprite.fillSprite(background);
			sprite.setTextDatum(MC_DATUM);
			sprite.setTextSize(1.0f);
			for (int i = 0; i < 4; ++i)
			{
				const int centerX = 12 + i * 24;
				const bool valid = isfinite(state.tyreTemperatures[i]) &&
					state.tyreTemperatures[i] > 0.0f;
				sprite.setTextColor(muted, background);
				sprite.drawString(labels[i], centerX, 5, 1);
				sprite.setTextColor(valid ? TFT_WHITE : muted, background);
				sprite.drawString(valid
					? String(lroundf(state.tyreTemperatures[i])) : "--",
					centerX, 17, 1);
				sprite.fillRoundRect(centerX - 7, 24, 14, 3, 1,
					valid ? tyreTemperatureColor(state.tyreTemperatures[i])
						: tft.color565(36, 49, 65));
			}
			sprite.pushSprite(213, 205);
			releaseSpriteAfterThemePreview(sprite, spriteCreated);
		}
		else
		{
			tft.fillRect(213, 205, 98, 28, background);
			tft.setTextDatum(MC_DATUM);
			for (int i = 0; i < 4; ++i)
			{
				const int centerX = 225 + i * 24;
				const bool valid = isfinite(state.tyreTemperatures[i]) &&
					state.tyreTemperatures[i] > 0.0f;
				tft.setTextColor(muted, background);
				tft.drawString(labels[i], centerX, 210, 1);
				tft.setTextColor(valid ? TFT_WHITE : muted, background);
				tft.drawString(valid
					? String(lroundf(state.tyreTemperatures[i])) : "--",
					centerX, 222, 1);
				tft.fillRoundRect(centerX - 7, 229, 14, 3, 1,
					valid ? tyreTemperatureColor(state.tyreTemperatures[i])
						: tft.color565(36, 49, 65));
			}
			tft.setTextDatum(TL_DATUM);
		}
		prevData["telemetryTyres"] = cacheState;
	}

	void drawTelemetryDashboard(const DashboardState &state, bool forceUpdate)
	{
		static LGFX_Sprite speedSprite(&tft);
		static LGFX_Sprite currentSprite(&tft);
		static LGFX_Sprite pedalSprite(&tft);
		static LGFX_Sprite tyreSprite(&tft);
		static bool speedSpriteCreated = false;
		static bool currentSpriteCreated = false;
		static bool pedalSpriteCreated = false;
		static bool tyreSpriteCreated = false;

		const uint16_t canvas = tft.color565(7, 10, 16);
		const uint16_t card = tft.color565(15, 21, 31);
		const uint16_t border = tft.color565(42, 55, 72);
		const uint16_t muted = tft.color565(137, 150, 166);
		const uint16_t cyan = tft.color565(47, 213, 255);
		const uint16_t violet = tft.color565(145, 92, 246);
		const uint16_t green = tft.color565(47, 213, 187);
		const uint16_t rose = tft.color565(255, 76, 126);

		if (forceUpdate)
		{
			tft.fillScreen(canvas);
			tft.setTextPadding(0);
			tft.setTextSize(1.0f);
			tft.setTextDatum(TL_DATUM);

			drawTelemetryCard(4, 4, 312, 29, card, border, cyan);
			drawTelemetryCard(4, 37, 76, 108, card, border, violet);
			drawTelemetryCard(84, 37, 152, 108, card, border, cyan);
			drawTelemetryCard(240, 37, 76, 108, card, border, green);
			drawTelemetryCard(4, 149, 156, 87, card, border, violet);
			drawTelemetryCard(164, 149, 152, 87, card, border, green);

			tft.setTextColor(muted, card);
			tft.drawString("RPM", 9, 11, 1);
			tft.drawFastHLine(9, 73, 66, border);
			tft.drawFastHLine(9, 108, 66, border);
			tft.drawString("LAP", 9, 42, 1);
			tft.drawString("POSITION", 9, 77, 1);
			tft.drawString("DELTA", 9, 112, 1);

			tft.drawFastVLine(153, 45, 92, border);
			tft.setTextDatum(MC_DATUM);
			tft.drawString("GEAR", 119, 47, 1);
			tft.drawString("SPEED", 194, 47, 1);
			tft.drawString("km/h", 194, 130, 1);
			tft.setTextDatum(TL_DATUM);

			tft.drawFastHLine(9, 177, 146, border);
			tft.drawFastHLine(9, 204, 146, border);
			tft.drawString("CURRENT", 10, 157, 1);
			tft.drawString("BEST", 10, 184, 1);
			tft.drawString("LAST", 10, 211, 1);

			tft.drawFastHLine(169, 201, 142, border);
			tft.drawString("PEDALS", 170, 155, 1);
			tft.drawString("TYRES C", 170, 209, 1);
		}

		const int displayedRpm = ((state.engineRpm + 25) / 50) * 50;
		drawTelemetryValue(31, 7, 39, 21, String(displayedRpm),
			"telemetryRpmValue",
			state.revLimitAlertActive ? rose : TFT_WHITE,
			card, 1, MC_DATUM, forceUpdate);
		drawTelemetryRpm(state, card, forceUpdate);

		drawTelemetryValue(8, 51, 68, 20, state.tyrePressureRearLeft,
			"telemetryLap", TFT_WHITE, card, 2, MC_DATUM, forceUpdate);
		drawTelemetryValue(8, 86, 68, 20, state.tyrePressureFrontRight,
			"telemetryPosition", TFT_WHITE, card, 2, MC_DATUM, forceUpdate);
		uint16_t deltaColor = TFT_WHITE;
		if (state.sessionBestLiveDeltaSeconds.startsWith("-"))
			deltaColor = green;
		else if (state.sessionBestLiveDeltaSeconds.startsWith("+") &&
			state.sessionBestLiveDeltaSeconds != "+0.000")
			deltaColor = rose;
		drawTelemetryValue(8, 121, 68, 20,
			state.sessionBestLiveDeltaSeconds, "telemetryDelta", deltaColor,
			card, 2, MC_DATUM, forceUpdate);

		drawTelemetryGear(state.gear, TFT_WHITE, card, forceUpdate);
		drawTelemetryValueBuffered(159, 65, 70, 49, state.speed,
			"telemetrySpeed", TFT_WHITE, card, 4, MC_DATUM, forceUpdate,
			speedSprite, speedSpriteCreated);

		drawTelemetryFuel(state, TFT_WHITE, muted, card, green, forceUpdate);
		drawTelemetryStatusBadge(247, "ABS", isActiveValue(state.absActive),
			"telemetryAbs", rose, TFT_WHITE, muted, card, forceUpdate);
		drawTelemetryStatusBadge(280, "TCS", isActiveValue(state.tcActive),
			"telemetryTcs", cyan, TFT_WHITE, muted, card, forceUpdate);

		drawTelemetryValueBuffered(59, 154, 96, 21, state.currentLapTime,
			"telemetryCurrent",
			state.lapInvalidated == "True" ? rose : TFT_WHITE,
			card, 2, MR_DATUM, forceUpdate, currentSprite,
			currentSpriteCreated);
		drawTelemetryValue(59, 181, 96, 21, state.bestLapTime,
			"telemetryBest", TFT_WHITE, card, 2, MR_DATUM, forceUpdate);
		drawTelemetryValue(59, 208, 96, 21, state.lastLapTime,
			"telemetryLast", TFT_WHITE, card, 2, MR_DATUM, forceUpdate);

		drawTelemetryPedals(state, card, border, forceUpdate, pedalSprite,
			pedalSpriteCreated);
		drawTelemetryTyres(state, card, muted, forceUpdate, tyreSprite,
			tyreSpriteCreated);
	}
