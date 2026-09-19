import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { randomUUID } from "node:crypto";
import { mkdirSync, readFileSync, rmSync } from "node:fs";
import { basename, dirname, join, resolve } from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const project = resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");
const source = readFileSync(join(project, "src", "SHCustomProtocol.h"), "utf8");
const fragment = readFileSync(join(project, "src", "dashboard", "FirmwareUpdateScreen.inc"), "utf8");

function between(text, start, end) {
  const first = text.indexOf(start);
  const last = text.indexOf(end, first + start.length);
  assert(first >= 0 && last > first, `Cannot locate production UI section: ${start}`);
  return text.slice(first, last);
}

function extractFunction(text, signature, name) {
  const match = signature.exec(text);
  assert(match, `Cannot locate production function: ${name}`);
  const masked = text.replace(
    /\/\/[^\n]*|\/\*[\s\S]*?\*\/|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'/g,
    (token) => token.replace(/[^\n]/g, " "),
  );
  const opening = masked.indexOf("{", match.index);
  assert(opening >= 0, `Missing function body: ${name}`);
  let depth = 1;
  let end = opening + 1;
  while (depth > 0 && end < masked.length) {
    if (masked[end] === "{") ++depth;
    if (masked[end] === "}") --depth;
    ++end;
  }
  assert.equal(depth, 0, `Unterminated function body: ${name}`);
  return text.slice(match.index, end);
}

function method(name) {
  return extractFunction(source,
    new RegExp(`^\\t(?:static )?(?:void|bool|uint8_t|int) ${name}\\(`, "m"), name);
}

function header(path, localInclude) {
  return readFileSync(join(project, ...path), "utf8")
    .replace(/^#pragma once\s*$/gm, "")
    .replace(localInclude ?? /$^/, "");
}

function translationUnit() {
  const members = [
    "normalBrightness", "scheduleBrightnessSave", "saveBrightnessIfDue",
    "savePendingBrightnessPreference", "savePendingDashboardPreference",
    "applyTouchRotation", "calibrationRotationForTouch", "drawSettingsButton",
    "drawSettingsScreen", "redrawSettingsButton", "showSettingsScreen", "closeSettings",
    "showTouchCalibration", "showWifiResetConfirm", "showWifiResettingScreen",
    "takeWifiResetRequest", "settingsButtonAtTouch", "activateSettingsButton", "readTouch",
  ].map(method).join("\n");
  const backend = readFileSync(join(project, "lib", "FirmwareUpdate", "FirmwareUpdate.cpp"), "utf8");
  const errors = extractFunction(backend, /^const char \*errorText\(Error error\)/m, "errorText");
  const replacements = {
    DECLARATIONS: between(source, "enum class DashboardTheme", "// Phase 1 renderer") +
      header(["include", "version.h"]) +
      header(["include", "ota_config.h"], '#include "version.h"') +
      header(["lib", "FirmwareUpdate", "FirmwareUpdate.h"], '#include "ota_config.h"'),
    ERROR_TEXT: errors,
    FIELDS: between(source, "\tPreferences dashboardPreferences;",
      "\tstatic bool isValidDashboardTheme"),
    FIRMWARE_SCREEN: fragment,
    SETTINGS_METHODS: members,
  };
  let harness = readFileSync(join(project, "scripts", "tests", "firmware-update-ui-harness.cpp"), "utf8");
  for (const [name, content] of Object.entries(replacements)) {
    const marker = `// @production:${name}`;
    assert.equal(harness.split(marker).length, 2, `Missing or repeated harness marker: ${marker}`);
    harness = harness.replace(marker, () => content);
  }
  return harness;
}

function checkedRun(command, args, { cwd, input, prerequisite = "" }) {
  const result = spawnSync(command, args, {
    cwd,
    input,
    encoding: "utf8",
    timeout: 60000,
    maxBuffer: 4 * 1024 * 1024,
    env: { ...process.env, TMPDIR: cwd, TMP: cwd, TEMP: cwd },
  });
  const details = [
    prerequisite,
    result.error?.message,
    result.signal ? `Terminated by ${result.signal}` : "",
    result.stderr,
    result.stdout,
  ].filter(Boolean).join("\n");
  assert.equal(result.status, 0, `${command} failed (exit ${result.status}).\n${details}`);
  return result.stdout;
}

test("firmware update touchscreen regressions", { timeout: 180000 }, async (t) => {
  assert.match(source, /if \(!firmwareUpdateBusy\(\) && !screenSleeping &&/);
  assert.match(source, /if \(!firmwareUpdateBusy\(\) && !isTouched && !touchWasPressed &&/);
  assert.doesNotMatch(fragment,
    /FirmwareUpdate::(?:begin|snapshot|poll|requestCheck|requestInstall|requestCancel|activate|rejectActivation)\(/);
  assert.doesNotMatch(fragment, /esp_ota_|ESP\.restart/);

  // CXX is one host compiler executable/path, not a shell command with flags.
  // Windows without CXX runs the existing WSL g++, never a silently skipped test.
  const cxx = process.env.CXX?.trim();
  const wsl = process.platform === "win32" && !cxx;
  const build = join(project, ".pio", `firmware-ui-tests-${process.pid}-${randomUUID()}`);
  mkdirSync(build, { recursive: true });
  t.after(() => {
    // Remove WSL-owned compiler output through WSL, not Windows UNC permissions.
    if (wsl) {
      checkedRun("wsl.exe", ["--exec", "rm", "--recursive", "--force", "--", basename(build)], {
        cwd: dirname(build),
      });
    } else {
      rmSync(build, { recursive: true, force: true, maxRetries: 3, retryDelay: 100 });
    }
  });

  const executable = `firmware-update-ui-harness${process.platform === "win32" && !wsl ? ".exe" : ""}`;
  const compiler = wsl ? "wsl.exe" : cxx || "g++";
  const prefix = wsl ? ["--exec", "env", "TMPDIR=.", "TMP=.", "TEMP=.", "g++"] : [];
  const prerequisite = "These tests require a host C++ compiler. Install g++ or set CXX to its executable path. " +
    "On Windows, unset CXX to use an installed WSL distribution with g++. Missing compilers fail, not skip.";
  // Match the pinned Arduino build dialect, including its existing inline-variable extensions.
  checkedRun(compiler, [...prefix, "-std=gnu++11", "-w", "-x", "c++", "-", "-o", executable], {
    cwd: build, input: translationUnit(), prerequisite,
  });
  const run = (args) => wsl
    ? checkedRun("wsl.exe", ["--exec", "env", "PATH=.", executable, ...args], { cwd: build })
    : checkedRun(join(build, executable), args, { cwd: build });
  const cases = run(["--list"]).trim().split(/\r?\n/);
  assert(cases.length >= 18, "The original 17 cases and brightness failure notification case must run");
  assert.equal(new Set(cases).size, cases.length, "Regression case names must be unique");
  for (const name of cases) {
    await t.test(name, () => {
      assert.equal(run([name]).trim(), `PASS ${name}`);
    });
  }
});
