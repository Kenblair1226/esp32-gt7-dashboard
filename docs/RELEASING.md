# Releasing firmware for OTA and the Web Installer

Firmware releases come from
[`Kenblair1226/esp32-gt7-dashboard`](https://github.com/Kenblair1226/esp32-gt7-dashboard/releases).
GitHub Actions builds both display-controller variants at a version tag and creates a
**draft** release. A maintainer accepts those exact binaries on hardware and publishes
the draft manually. Devices and the Web Installer then use the same published bytes.

Pages deployment does not rebuild firmware. Installer-only changes on `main` reuse
accepted release firmware instead of replacing it with older committed binaries.
Local builds and packaging remain available for development.

## Version and hardware identity

`VERSION` is the manually controlled version source. The publishing helper generates
`include/version.h` and synchronizes the installer manifests from it. Use canonical
numeric `major.minor.patch` versions without leading zeroes; each component must fit an
unsigned 32-bit integer and the complete version must fit 32 characters. The matching
tag is `vX.Y.Z`.

Each application embeds its version, product, display variant, and
`esp32-4mb-min-spiffs-v1` layout identity. Packaging checks that identity against the
actual image, not only the name of the file. CI rejects a tag that disagrees with the
version/header/release notes before running the publisher. Do not reuse an existing
published tag or overwrite a published asset.

OTA offers only strictly newer stable versions. It does not offer a draft, prerelease,
same-version reinstall, downgrade, or a manually selected panel. The compiled
`DISPLAY_PANEL_ST7789` flag determines which image the device accepts.

## Flash layout and capacity

Both `esp32` (ILI9341) and `esp32-st7789` use the classic ESP32 target, Arduino framework,
`espressif32@6.9.0`, and the framework's `min_spiffs.csv`. Each of the two application
slots is **1,966,080 bytes (`0x1E0000`)**. Release packaging must reject an application
that does not fit either slot; do not remove themes or change the layout silently to
make an oversized release pass.

| Partition | Offset | Size |
| --- | --- | --- |
| NVS | `0x9000` | `0x5000` |
| OTA data | `0xE000` | `0x2000` |
| Application 0 | `0x10000` | `0x1E0000` |
| Application 1 | `0x1F0000` | `0x1E0000` |
| SPIFFS | `0x3D0000` | `0x20000` |
| Core dump | `0x3F0000` | `0x10000` |

The USB bundle uses these offsets:

| Offset | File | Build source |
| --- | --- | --- |
| `0x1000` | `bootloader.bin` | `.pio/build/esp32/bootloader.bin` |
| `0x8000` | `partitions.bin` | `.pio/build/esp32/partitions.bin` |
| `0xE000` | `boot_app0.bin` | Framework `tools/partitions/boot_app0.bin` |
| `0x10000` | `firmware-ili9341.bin` | `.pio/build/esp32/firmware.bin` |
| `0x10000` | `firmware-st7789.bin` | `.pio/build/esp32-st7789/firmware.bin` |

Choose one application image, not both. Both builds must have identical shared boot
files and partition tables. Revisit all packaging, compatibility checks, manifests,
and migration instructions together if the platform or flash layout changes.

## One-time migration and USB recovery

Older firmware uses `huge_app.csv`, which has only one application slot. Such an
installation cannot receive an ordinary OTA update: it needs one complete USB
installation of an OTA-capable release.

Use the Web Installer after that release is published, or extract the release's
`usb-installer.zip` and flash its complete controller-specific manifest with ESP Web
Tools or esptool. The bundle includes the bootloader, partition table, OTA-data
initializer, and both application variants. Installing only `firmware-*.bin` onto an
old layout is insufficient.

The NVS offset and size are unchanged, so flashing without an erase preserves saved
Wi-Fi, theme, brightness, and touch orientation. Choosing the installer's erase option
clears those settings; complete Wi-Fi setup again afterward.

Archived USB-only releases retain their original partition images. The installer warns
when one is selected because installing it removes the second OTA slot. Another USB
migration is needed before updating wirelessly again.

An interrupted or rejected OTA transfer leaves the existing boot selection intact.
This is not boot-failure rollback: the stock bootloader is not configured here to
automatically recover from a newly installed application that crashes on startup.
Recover a bad boot by flashing a known-good complete USB release.

## Prepare and build locally

1. Add short, nonempty release notes for the chosen version to
   `installer/release-notes.json`.
2. Synchronize and build both variants:

   ```powershell
   npm run publish:firmware -- 1.9.0
   ```

   Replace `1.9.0` with the intended new version. Omitting the version reads `VERSION`.
   The helper builds both targets, checks the actual images/partitions, stages the local
   installer, archives up to ten versions, and writes flat release files under
   `.pio/release/`.
3. For version preparation only, without builds or release assets:

   ```powershell
   npm run publish:firmware -- 1.9.0 --skip-build
   ```

   This only synchronizes version files. It does **not** make old binaries into a
   release. Do not publish sync-only installer manifests with mismatched binaries.
   CI does not use this option.
4. To build without staging or changing installer/version files:

   ```powershell
   pio run -e esp32 -e esp32-st7789
   ```

Use `PLATFORMIO_CMD` when PlatformIO is not on `PATH`, `PLATFORMIO_CORE_DIR` for a custom
core installation, and the existing `PLATFORMIO_ENV`/`PLATFORMIO_ST7789_ENV` overrides
only for equivalent compatible targets. A custom environment still has to pass all
panel, partition, image, and size checks.

The local publisher never commits, tags, pushes, creates a GitHub Release, or deploys
Pages. Do not commit `.pio/`. CI-generated release binaries do not need to be committed
to the repository.

## Release assets

The flat package contains:

| Asset | Purpose |
| --- | --- |
| `ota-manifest.json` | Bounded, versioned metadata with per-panel size and SHA-256 |
| `firmware-ili9341.bin`, `firmware-st7789.bin` | Application-only OTA and USB images |
| `bootloader.bin`, `partitions.bin`, `boot_app0.bin` | Complete USB installation/migration |
| `manifest-ili9341.json`, `manifest-st7789.json` | ESP Web Tools manifests with USB offsets |
| `release.json` | Version, release notes, OTA capability, and manifest names |
| `checksums.sha256` | SHA-256 checksums of the packaged files |
| `usb-installer.zip` | CI-created archive of the nine payload files above, excluding checksums |

CI creates the ZIP from already validated files and then refreshes the external
checksums to include the ZIP. There is no second compilation for USB.

The OTA manifest declares schema version `1`, product `esp32-gt7-dashboard`, chip family
`ESP32`, layout `esp32-4mb-min-spiffs-v1`, the numeric version, matching release tag, and
both panel assets. Filenames are fixed; arbitrary URLs are not accepted. The device
fetches a small manifest through `releases/latest/download/ota-manifest.json`, then
downloads the selected application from its immutable tag-specific URL. A changing
`latest` release therefore cannot silently mix metadata and application versions.

Useful local validation commands:

```powershell
node scripts\firmware-release.mjs verify-tag v1.9.0
node scripts\firmware-release.mjs validate .pio\release 1.9.0
npm run test:release
```

Host regressions use Node's built-in runner and `g++` with AddressSanitizer and
UndefinedBehaviorSanitizer. Ubuntu CI uses its existing compiler; Windows defaults
to the checkout's WSL toolchain, or `CXX` can name a sanitizer-capable compiler.
Missing compilers fail explicitly. The tests include the reviewed MIT-licensed cJSON
1.7.17 source, so they do not download dependencies or require a prebuilt firmware SDK.

## Publish a release

1. Merge the accepted source/workflow changes into `main`. Commit the intended
   `VERSION`, generated version header, and release notes. Never commit version-only
   installer changes as though corresponding binaries had been rebuilt.
2. Create and push an annotated tag on that accepted commit:

   ```powershell
   git tag -a v1.9.0 -m "Release 1.9.0"
   git push origin v1.9.0
   ```

   Pushing a branch does not push a newly created tag. The tag and version must agree.
3. The firmware-release workflow builds both targets, validates all payloads, uploads
   an Actions artifact, and creates a GitHub **draft** release. A failed build or package
   does not create an installable release. Published releases cannot be overwritten by
   rerunning the workflow. A partial draft resumes only when its existing assets match
   byte-for-byte. If a rebuild produces different bytes, resolve that unpublished draft
   explicitly; the workflow will not replace its assets silently.
4. Download that draft's exact assets. Flash the complete USB bundle on each supported
   panel and perform the hardware acceptance below; do not substitute a local rebuild
   with the same version number.
5. Publish the accepted draft manually as a stable release in GitHub. The public
   `latest` channel must identify the intended release.
6. The Pages workflow stages and validates the accepted release files without rebuilding.
   Confirm its deployment completes and that the selected version and application
   hashes match the published release.

Publishing makes the OTA assets available before the subsequent Pages deployment
finishes. If Pages fails, fix and rerun that deployment rather than modifying accepted
firmware assets in place.

## Pages behavior and repository settings

Set **Repository > Settings > Pages > Build and deployment > Source** to
**GitHub Actions**. Allow Actions to build the public repository and create draft
releases. The build jobs need read access; release publication needs `contents: write`,
while Pages uses its separate Pages/id-token permissions. No GitHub token is embedded
in firmware or release payloads.

If the `github-pages` environment restricts deployment branches or tags, allow `main`
and accepted `v*` tags: a release-publication event runs with a tag ref even when its
installer frontend is checked out from `main`.

The intended site is
<https://kenblair1226.github.io/esp32-gt7-dashboard/>. Enabling these workflows does not
mean the site or an OTA release has already been deployed.

Before the first stable OTA release, Pages can deploy the committed legacy installer.
Afterward, the published release assets are the firmware source of truth. Installer
frontend changes on `main` and manual deployments reuse the current stable firmware.
Missing assets, malformed metadata, download failures, and hash mismatches must fail
deployment rather than fall back to old binaries.

The version index retains up to ten selectable stable/legacy versions, excludes drafts
and prereleases, and merges accepted GitHub release history with existing legacy
archives. A late workflow for an older release must not change which firmware is
advertised as current. Root `manifest.json` remains the ILI9341 alias and
`manifest-st7789.json` remains the ST7789 alias.

To stage a local, read-only preview using GitHub CLI authentication:

```powershell
node scripts\stage-published-installer.mjs stage installer .pio\pages-preview .pio\pages-preview-state.json
node scripts\stage-published-installer.mjs check-current .pio\pages-preview-state.json
```

Both output paths must be fresh. These commands download and validate release assets
but do not deploy Pages or modify releases. `GH_REPO` defaults to this fork; automation
uses `GH_TOKEN`. The identity file stays outside the served directory.

## Device behavior and troubleshooting

Use 1.9.1 or newer for OTA. Version 1.9.0 can exhaust the heap during certificate
verification or reject a fragmented heap before downloading. A device already on
that version may require a USB installation of 1.9.1; the fix cannot repair the
running downloader through a transfer it cannot complete.

Use **DEVICE SETTINGS > FIRMWARE UPDATE** while not on track. Checking never installs
an image; installation requires confirmation. There are no automatic startup checks,
scheduled checks, or unattended updates. The updater keeps the screen awake, supports
cancellation before activation, and cancels if gameplay resumes before activation.

The device validates HTTPS certificates/hostnames, permits only expected GitHub HTTPS
redirects, and requires a valid clock. SHA-256 and image identity/size checks protect
against corrupt, stale, or wrong-panel content. Authenticity relies on verified HTTPS
and control of the selected GitHub repository; this is not an independent firmware
signature, secure-boot, or eFuse policy.

| Problem | Action |
| --- | --- |
| USB upgrade required | Install the complete dual-slot USB bundle, not only the app. |
| Wi-Fi, clock, or secure connection unavailable | Restore internet/time-server access, then retry. Do not disable TLS verification. |
| No compatible release available | Publish a valid stable release with all expected assets. Drafts remain invisible to devices. |
| Size, image, identity, or hash failure | Keep the current firmware; correct the package and publish a new version. |
| Update cancelled when driving resumes | Leave the on-track session and check again. |
| Preferences cannot be saved | Resolve the storage issue before restarting into another image. |
| New firmware cannot boot | Restore a known-good complete release over USB. |

Normal local GT7 telemetry does not need internet. TLS trust roots and redirect-host
policy are maintained in the firmware service; if GitHub changes its trust chain, update
those roots in a release rather than adding an insecure fallback.

The updater allocates manifest storage only after the TLS redirect handshakes and
sizes it to the bounded response length. HTTP header scratch uses the reserved
worker stack rather than occupying TLS heap. Body reads tolerate up to 60 seconds
without progress, still within the five-minute installation deadline; cancellation
and on-track checks continue during that wait. Request writes have a separate
five-second limit.

## Hardware acceptance

For both ILI9341 and ST7789, exercise fresh boot, Wi-Fi setup, PS5 discovery, telemetry,
every theme, brightness, touch calibration, Settings Back/reset confirmation, sleep and
wake, and repeated checks without heap degradation.

Migrate an old single-slot installation over USB, then install two successive OTA
versions to exercise both slots. Confirm the expected panel/version after each restart
and that Wi-Fi, theme, brightness, and touch rotation survive.

Exercise no-internet/clock/TLS failures, a missing release, HTTP/redirect failures,
cancel, Wi-Fi interruption, truncated/corrupt/wrong-panel images, oversized images,
flash-write errors, and power loss before activation. The current boot selection must
remain intact on rejected or incomplete updates. Check that gameplay, touch cancellation,
Settings timeout, auto-sleep, and renderer restoration interact correctly.

Keep hardware, real tag-triggered CI, and deployed Pages acceptance explicitly pending
when the required devices or publishing access are unavailable. A local compiler or
host test result alone does not establish those outcomes.
