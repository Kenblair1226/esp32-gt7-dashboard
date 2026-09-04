# EV Telemetry Support Plan

Status: implemented on the EV feature branch; awaiting in-game validation.

## Scope and expected change size

This is a small-to-medium change rather than a dashboard redesign. The GT7 UDP parser
already exposes `fuelLevel` and `fuelCapacity`, and every theme already has a fuel value
or fuel progress presentation. The implementation should mainly add one normalized fuel
display model in the dashboard data layer and make the existing theme renderers consume
that model.

Expected touch points:

- one GT7 telemetry-to-dashboard mapping function;
- the dashboard state and fixed Theme Preview mock data;
- the existing fuel label/value/progress paths in all seven themes;
- the remaining-laps (`REM`/`LEFT`) output;
- focused ICE, EV, invalid-data and vehicle-transition tests.

No new image assets, framebuffer, vehicle database or telemetry packet format is needed.
The main implementation risk is stale text when a running session changes between `FUEL`
and `EV`, because several themes currently draw static labels only during a full redraw.
The EV flag must therefore participate in the relevant redraw cache or request a safe full
theme redraw when the powertrain classification changes.

## Telemetry interpretation

Use the following GT7-specific interpretation:

- `fuelCapacity >= 0.1f`: combustion or hybrid vehicle with normal fuel data;
- `fuelCapacity < 0.1f`: EV candidate;
- for an EV candidate, `fuelLevel` is treated as remaining energy in kWh.

The kWh interpretation is a project assumption that must be verified against the GT7 HUD
on multiple EVs before release. `energyRecovery` is an instantaneous recovery value and
must not be used as battery state of charge.

Powertrain classification uses only the capacity field at packet offset `0x48`. Keep
energy validity separate so an invalid energy sample cannot make an EV label flicker back
to `FUEL`:

```cpp
const bool validCapacity = isfinite(fuelCapacity) && fuelCapacity >= 0.0f;
const bool isEV = validCapacity && fuelCapacity < 0.1f;
const bool validEvEnergy = isfinite(fuelLevel) && fuelLevel > 0.0f && fuelLevel < 500.0f;
```

Kart capacity is expected to be around 5 and therefore remains on the normal fuel path.
Hybrids with a non-zero fuel capacity also remain on the normal fuel path.

## Normalized dashboard data

Compute the EV behavior once below the theme renderers. Themes must not independently
interpret raw `fuelLevel` or `fuelCapacity`.

The normalized state should expose equivalent fields to:

```cpp
bool fuelIsEV;
bool fuelValueValid;
String fuelLabel;          // "FUEL" or "EV"
String fuelDisplayValue;   // ICE: percentage, EV: remaining kWh without suffix
int fuelProgressPercent;   // clamped 0..100 visual progress
String remainingLaps;      // EV always "--"
```

Existing legacy protocol field names may remain internally for compatibility, but theme
code should receive distinct display value and progress semantics. A single string must
not simultaneously represent raw EV energy and a percentage.

## Display rules

### Combustion and hybrid vehicles

- Label: `FUEL`
- Value: existing fuel percentage behavior, including `%` where the theme currently uses it
- Progress: `fuelLevel / fuelCapacity`
- Remaining laps: retain the existing fuel-per-lap estimator
- Low-fuel status: retain existing percentage thresholds

### Electric vehicles

- Label: `EV`
- Value: rounded whole-number `fuelLevel`, with no `%` or `kWh` suffix
- Icon: retain the theme's existing fuel-pump icon and layout
- Remaining laps (`REM` or `LEFT`): always `--`
- The fuel-per-lap estimator must not consume EV energy data
- Reset or invalidate the estimator when classification changes between EV and non-EV

Example:

```text
EV 42
REM --
```

### Invalid or transitional data

- Value: `--`
- Remaining laps: `--`
- Do not update the normal fuel estimator
- Preserve the theme's current safe progress fallback unless testing identifies a clearer
  common fallback

An EV `fuelLevel` of zero is treated as transitional/invalid while this behavior is being
validated, preventing the initialization packet from briefly displaying `0.0` before the
car's energy value becomes available.

## EV visual progress

GT7 does not currently provide a confirmed battery capacity or battery SOC field. Use a
fixed visual reference only:

```cpp
constexpr float EV_VISUAL_FULL_KWH = 60.0f;

const int progressPercent = constrain(
    lroundf(fuelLevel / EV_VISUAL_FULL_KWH * 100.0f),
    0,
    100
);
```

Examples:

| Remaining EV energy | Visual progress |
| ---: | ---: |
| 80 kWh | 100% |
| 60 kWh | 100% |
| 45 kWh | 75% |
| 30 kWh | 50% |
| 15 kWh | 25% |

This progress is a relative energy indication, not true battery SOC. Documentation must
not describe it as an exact percentage. Values above 60 kWh are intentionally clamped to
full.

In Model 3 testing, GT7 updated EV energy at a visibly lower cadence than the main
telemetry stream, producing stepwise changes separated by tens of seconds. This cadence
is an observed game behavior, not an official fixed interval. The dashboard presents the
latest raw value without interpolation or an artificial consumption estimate.

## Theme integration

All seven themes should preserve their current geometry, fonts, icon shapes and colors.
Only the following content becomes dynamic:

- `FUEL` changes to `EV` for an EV;
- the displayed value uses `fuelDisplayValue`;
- segmented or filled gauges use `fuelProgressPercent`;
- `REM`/`LEFT` displays `--` for an EV.

Theme Preview should include an explicit EV mock-data option during development, but the
normal default preview may remain an ICE vehicle. Label redraws must be tested without
requiring a theme switch or reboot.

## Acceptance checks

1. ICE with valid fuel capacity behaves exactly as before.
2. Hybrid with valid fuel capacity remains `FUEL`, not `EV`.
3. Kart with capacity near 5 remains `FUEL`.
4. EV shows `EV`, a suffix-free rounded energy value and visual progress based on 60 kWh.
5. EV always shows `REM --` or `LEFT --` and does not update the fuel estimator.
6. Invalid values show `--` without NaN, overflow or a misleading low-fuel warning.
7. Switching ICE -> EV -> ICE refreshes labels, values, progress and remaining laps without
   stale pixels or a full-screen flicker.
8. All seven themes preserve their existing layout at one-, two- and three-digit EV values.
9. At least two GT7 EVs are compared with the native HUD to validate that `fuelLevel`
   behaves like remaining kWh before the feature is released.
