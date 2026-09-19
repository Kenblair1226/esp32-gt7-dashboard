import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { createHash, randomUUID } from "node:crypto";
import { copyFileSync, existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { basename, dirname, join, resolve } from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

import "../../lib/FirmwareUpdate/tests/trust.test.mjs";

const project = resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");
const library = join(project, "lib", "FirmwareUpdate");
const fixtures = join(project, "scripts", "tests", "fixtures", "cjson-1.7.17");

// Unmodified MIT-licensed upstream sources; each file retains its license.
// https://github.com/DaveGamble/cJSON/tree/87d8f0961a01bf09bef98ff89bae9fdec42181ee
// This is v1.7.17, also used by the pinned Arduino ESP32 2.0.17 SDK.
// Vendoring both files keeps CI offline, including its pre-PlatformIO test step.
const cjsonHashes = {
  "cJSON.c": "de63e951ce3bc9b6938c7635575a6c90c3b364595b0b0f4c5ae8f9c83a43c17d",
  "cJSON.h": "c01a8ca5609bb2c956dd1ae836d5d926ec68153ecebc5a75ea0febc2e076da8d",
};

function normalizedSource(path) {
  return readFileSync(path, "utf8").replaceAll("\r\n", "\n");
}

function sourceHash(source) {
  return createHash("sha256").update(source).digest("hex");
}

function extractFunction(source, signature, name) {
  const matches = [...source.matchAll(new RegExp(signature.source, "gm"))];
  assert.equal(matches.length, 1, `Expected one production function: ${name}`);
  const start = matches[0].index;
  const masked = source.replace(
    /\/\/[^\n]*|\/\*[\s\S]*?\*\/|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'/g,
    (token) => token.replace(/[^\n]/g, " "),
  );
  const opening = masked.indexOf("{", start);
  assert(opening >= 0, `Missing function body: ${name}`);
  let depth = 1;
  let end = opening + 1;
  while (depth && end < masked.length) {
    if (masked[end] === "{") ++depth;
    if (masked[end] === "}") --depth;
    ++end;
  }
  assert.equal(depth, 0, `Unterminated function body: ${name}`);
  return source.slice(start, end);
}

function transportTranslationUnit() {
  const transport = normalizedSource(join(library, "OtaTransport.cpp"));
  const service = normalizedSource(join(library, "FirmwareUpdate.cpp"));
  const timeout = transport.match(/^constexpr uint32_t READ_TIMEOUT_MS = [^;\r\n]+;$/m);
  assert(timeout, "Cannot locate production transport idle timeout");
  const memoryLimits = transport.match(
    /^constexpr size_t MIN_TLS_HEAP_BYTES = [^;]+;\r?\nconstexpr size_t MIN_TLS_HEAP_BLOCK = [\s\S]+?;/m,
  );
  assert(memoryLimits, "Cannot locate production TLS memory limits");
  const headers = extractFunction(transport, /^\s*bool headers\(\)/, "HttpsDownload::Impl::headers");
  assert.match(headers, /char line\[MAX_HEADER_LINE\];/);
  const layout = transport.slice(transport.indexOf("\tOperation &operation;", transport.indexOf("struct HttpsDownload::Impl")));
  assert.doesNotMatch(layout, /char line\[MAX_HEADER_LINE\]/, "Header scratch must not occupy handshake heap");
  const replacements = {
    READ_TIMEOUT: timeout[0],
    MEMORY_LIMITS: memoryLimits[0],
    MEMORY_GUARD: extractFunction(transport, /^bool hasTlsMemory\(\)/, "hasTlsMemory"),
    TLS_STOP: extractFunction(transport, /^\s*void stop\(\) override/, "VerifiedClient::stop"),
    DISCOVER: extractFunction(service, /^bool discoverRelease\(/, "discoverRelease"),
    BODY_CAPACITY: extractFunction(transport, /^size_t HttpsDownload::bodyCapacity\(\) const/, "bodyCapacity"),
    CLOSE: extractFunction(transport, /^\s*void close\(\)/, "HttpsDownload::Impl::close"),
    READ: extractFunction(transport, /^int HttpsDownload::read\(/, "HttpsDownload::read"),
    CONSTRUCTOR: extractFunction(transport, /^HttpsDownload::HttpsDownload\(/, "HttpsDownload constructor"),
    DESTRUCTOR: extractFunction(transport, /^HttpsDownload::~HttpsDownload\(/, "HttpsDownload destructor"),
    REMAINING: extractFunction(service, /^uint32_t Operation::remaining\(\) const/, "Operation::remaining"),
    FAIL: extractFunction(service, /^bool Operation::fail\(/, "Operation::fail"),
  };
  let harness = normalizedSource(join(project, "scripts", "tests", "firmware-update-transport-harness.cpp"));
  for (const [name, implementation] of Object.entries(replacements)) {
    const marker = `// @production:${name}`;
    assert.equal(harness.split(marker).length, 2, `Missing or repeated harness marker: ${marker}`);
    harness = harness.replace(marker, () => implementation);
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
    env: {
      ...process.env,
      TMPDIR: cwd,
      TMP: cwd,
      TEMP: cwd,
      ASAN_OPTIONS: "detect_leaks=1:halt_on_error=1",
      UBSAN_OPTIONS: "halt_on_error=1:print_stacktrace=1",
    },
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

test("embedded OTA policy, real cJSON manifest and transport regressions (ASan + UBSan)",
  { timeout: 240000 }, async (t) => {
    for (const [name, expected] of Object.entries(cjsonHashes)) {
      assert.equal(sourceHash(normalizedSource(join(fixtures, name))), expected,
        `${name} must remain the reviewed, unmodified cJSON 1.7.17 test source`);
    }
    const sdkHeader = join(project, ".pio", "core", "packages", "framework-arduinoespressif32",
      "tools", "sdk", "esp32", "include", "json", "cJSON", "cJSON.h");
    if (existsSync(sdkHeader)) {
      assert.equal(sourceHash(normalizedSource(sdkHeader)), cjsonHashes["cJSON.h"],
        "Restored framework cJSON header differs from the pinned native test header");
    }

    // CXX names one executable, not a shell command. Windows defaults to WSL;
    // select the owning distribution for a \\wsl.localhost\Distro\... checkout.
    const cxx = process.env.CXX?.trim();
    const wsl = process.platform === "win32" && !cxx;
    const distro = wsl && project.match(/^\\\\(?:wsl\.localhost|wsl\$)\\([^\\]+)\\/i)?.[1];
    const wslPrefix = distro ? ["--distribution", distro, "--exec"] : ["--exec"];
    const build = join(project, ".pio", `firmware-policy-tests-${process.pid}-${randomUUID()}`);
    const working = join(build, "tests");
    mkdirSync(working, { recursive: true });
    t.after(() => {
      if (wsl) {
        checkedRun("wsl.exe", [...wslPrefix, "rm", "--recursive", "--force", "--", basename(build)], {
          cwd: dirname(build),
        });
      } else {
        rmSync(build, { recursive: true, force: true, maxRetries: 3, retryDelay: 100 });
      }
    });

    for (const name of ["OtaPolicy.h", "FirmwareManifest.h", "FirmwareUpdate.h", "OtaTransport.h"]) {
      copyFileSync(join(library, name), join(build, name));
    }
    copyFileSync(join(library, "FirmwareManifest.cpp"), join(working, "FirmwareManifest.cpp"));
    copyFileSync(join(project, "include", "ota_config.h"), join(build, "ota_config.h"));
    copyFileSync(join(fixtures, "cJSON.h"), join(build, "cJSON.h"));

    // Existing C++ cases model a running 1.8.0 device. Isolate that baseline so
    // a maintainer's next version bump does not invalidate the regression cases.
    // Only this generated test header changes; production files are read-only.
    const versionHeader = normalizedSource(join(project, "include", "version.h"));
    const versionDefine = /^#define GT7_DASH_VERSION_LITERAL "[^"\r\n]+"$/gm;
    assert.equal([...versionHeader.matchAll(versionDefine)].length, 1,
      "Expected exactly one production version literal");
    writeFileSync(join(build, "version.h"),
      versionHeader.replace(versionDefine, '#define GT7_DASH_VERSION_LITERAL "1.8.0"'));

    const compiler = wsl ? "wsl.exe" : cxx || "g++";
    const compilerPrefix = wsl
      ? [...wslPrefix, "env", "TMPDIR=.", "TMP=.", "TEMP=.", "g++"]
      : [];
    const prerequisite = "OTA native tests require g++ with AddressSanitizer and UndefinedBehaviorSanitizer. " +
      "On Windows, unset CXX to use WSL with g++, or set CXX to a sanitizer-capable native compiler. " +
      "No dependency downloads, firmware builds, or silently skipped tests are performed.";
    const flags = [
      "-I..", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
      "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
      "-DCJSON_HIDE_SYMBOLS",
    ];
    // Fixed executable placement avoids sporadic PIE/ASan shadow collisions on
    // Linux hosts with aggressive ASLR. Native Windows toolchains do not use it.
    const noPie = wsl || process.platform === "linux";
    if (noPie) flags.push("-fno-pie");
    const compile = (args, input) => checkedRun(compiler, [...compilerPrefix, ...flags, ...args], {
      cwd: working, input, prerequisite,
    });
    compile(["-std=c99", "-x", "c", "-", "-c", "-o", "cjson.o"],
      normalizedSource(join(fixtures, "cJSON.c")));
    const run = (executable, args = []) => wsl
      ? checkedRun("wsl.exe", [...wslPrefix, "env",
        "ASAN_OPTIONS=detect_leaks=1:halt_on_error=1",
        "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1",
        "TMPDIR=.", "TMP=.", "TEMP=.", `./${executable}`, ...args], { cwd: working })
      : checkedRun(join(working, executable), args, { cwd: working });
    const suffix = process.platform === "win32" && !wsl ? ".exe" : "";

    for (const panel of ["ili9341", "st7789"]) {
      const panelFlag = panel === "st7789" ? ["-DDISPLAY_PANEL_ST7789=1"] : [];
      for (const kind of ["policy", "manifest"]) {
        await t.test(`${kind}: ${panel}`, () => {
          const executable = `${kind}-${panel}${suffix}`;
          const sources = kind === "manifest"
            ? ["FirmwareManifest.cpp", "-x", "none", "cjson.o"]
            : [];
          compile(["-std=c++17", ...panelFlag, "-x", "c++", "-", ...sources,
            ...(noPie ? ["-no-pie"] : []), "-o", executable],
          normalizedSource(join(library, "tests", `${kind}.cpp.inc`)));
          const label = kind === "policy" ? "policy and streaming identity" : "strict manifest";
          assert.equal(run(executable).trim(), `OTA ${label} tests passed (${panel})`);
        });
      }
    }

    const transportExecutable = `transport${suffix}`;
    compile(["-std=c++17", "-x", "c++", "-", ...(noPie ? ["-no-pie"] : []),
      "-o", transportExecutable], transportTranslationUnit());
    const transportCases = run(transportExecutable, ["--list"]).trim().split(/\r?\n/);
    assert(transportCases.length >= 24, "Transport lifecycle and TLS memory regressions must run");
    assert.equal(new Set(transportCases).size, transportCases.length);
    // Batch native cases to avoid a WSL/sanitizer startup for every assertion group.
    const transportResults = run(transportExecutable, ["--all"]).trim().split(/\r?\n/);
    assert.equal(transportResults.length, transportCases.length);
    for (const name of transportCases) {
      await t.test(`transport: ${name}`, () => {
        assert.equal(transportResults.filter((line) => line === `PASS ${name}`).length, 1);
      });
    }
  });
