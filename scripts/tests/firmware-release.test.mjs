import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { copyFile, lstat, mkdir, mkdtemp, readFile, readdir, rm, symlink, writeFile } from "node:fs/promises";
import { dirname, join, resolve } from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";
import {
  compareVersions,
  createReleaseDirectory,
  parseVersion,
  RELEASE_FILES,
  validateReleaseDirectory,
  verifyTagVersion,
  versionHeaderContents,
  writeChecksums,
} from "../firmware-release.mjs";

const projectDirectory = resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");
const VERSION = "1.9.0";
const PANELS = ["ili9341", "st7789"];
const SLOT_BYTES = 0x1e0000;

function hash(bytes, algorithm = "sha256") {
  return createHash(algorithm).update(bytes).digest("hex");
}

function image({ panel, version = VERSION, identity, payloadLength = 256, appendHash = true } = {}) {
  const data = Buffer.alloc(payloadLength, 0x42);
  if (panel || identity) {
    data.write(identity ?? `GT7DASH-OTA:1|esp32-gt7-dashboard|${version}|${panel}|esp32-4mb-min-spiffs-v1|END\0`);
  }
  const imageEnd = Math.floor((32 + data.length) / 16) * 16 + 16;
  const bytes = Buffer.alloc(imageEnd + (appendHash ? 32 : 0));
  bytes[0] = 0xe9;
  bytes[1] = 1;
  bytes[2] = 2;
  bytes[3] = 0x20;
  bytes.writeUInt32LE(0x40080000, 4);
  bytes[8] = 0xee;
  bytes.writeUInt16LE(0xffff, 17);
  bytes[23] = appendHash ? 1 : 0;
  bytes.writeUInt32LE(0x3f400020, 24);
  bytes.writeUInt32LE(data.length, 28);
  data.copy(bytes, 32);
  bytes[imageEnd - 1] = data.reduce((checksum, byte) => checksum ^ byte, 0xef);
  if (appendHash) createHash("sha256").update(bytes.subarray(0, imageEnd)).digest().copy(bytes, imageEnd);
  return bytes;
}

function partitions() {
  const bytes = Buffer.alloc(0xc00, 0xff);
  const entries = [
    ["nvs", 1, 2, 0x9000, 0x5000],
    ["otadata", 1, 0, 0xe000, 0x2000],
    ["app0", 0, 0x10, 0x10000, SLOT_BYTES],
    ["app1", 0, 0x11, 0x1f0000, SLOT_BYTES],
    ["spiffs", 1, 0x82, 0x3d0000, 0x20000],
    ["coredump", 1, 3, 0x3f0000, 0x10000],
  ];
  for (const [index, [name, type, subtype, offset, size]] of entries.entries()) {
    const entry = Buffer.alloc(32);
    entry.writeUInt16LE(0x50aa, 0);
    entry[2] = type;
    entry[3] = subtype;
    entry.writeUInt32LE(offset, 4);
    entry.writeUInt32LE(size, 8);
    entry.write(name, 12);
    entry.copy(bytes, index * 32);
  }
  bytes.writeUInt16LE(0xebeb, 192);
  createHash("md5").update(bytes.subarray(0, 192)).digest().copy(bytes, 208);
  return bytes;
}

function bootApp() {
  const bytes = Buffer.alloc(8192, 0xff);
  bytes.writeUInt32LE(1, 0);
  bytes.writeUInt32LE(0x4743989a, 28);
  bytes.writeUInt32LE(0, 4096);
  return bytes;
}

async function fixture(t) {
  // Fixtures stay in the ignored worktree build area, never the system temp directory.
  const buildRoot = join(projectDirectory, ".pio");
  await mkdir(buildRoot, { recursive: true });
  const directory = await mkdtemp(join(buildRoot, "release-tests-"));
  t.after(() => rm(directory, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 }));
  return directory;
}

async function put(directory, filename, contents) {
  const path = join(directory, filename);
  await mkdir(dirname(path), { recursive: true });
  await writeFile(path, contents);
}

async function json(directory, filename) {
  return JSON.parse(await readFile(join(directory, filename), "utf8"));
}

async function putJson(directory, filename, value) {
  await put(directory, filename, `${JSON.stringify(value, null, 2)}\n`);
}

async function absent(path) {
  await assert.rejects(lstat(path), { code: "ENOENT" });
}

async function buildFixture(directory, version = VERSION) {
  const buildDirectories = Object.fromEntries(PANELS.map((panel) => [panel, join(directory, "build", panel)]));
  for (const panel of PANELS) {
    await put(buildDirectories[panel], "firmware.bin", image({ panel, version }));
    await put(buildDirectories[panel], "bootloader.bin", image());
    await put(buildDirectories[panel], "partitions.bin", partitions());
  }
  const bootAppPath = join(directory, "framework", "boot_app0.bin");
  await put(dirname(bootAppPath), "boot_app0.bin", bootApp());
  return { version, notes: ["Enable verified OTA updates."], buildDirectories, bootAppPath };
}

async function releaseFixture(t) {
  const root = await fixture(t);
  const build = await buildFixture(root);
  const directory = join(root, "release");
  await createReleaseDirectory(directory, build);
  return { root, directory, build };
}

async function changeJson(directory, filename, mutate) {
  const value = await json(directory, filename);
  mutate(value);
  await putJson(directory, filename, value);
  await rm(join(directory, "checksums.sha256"), { force: true });
}

async function replaceApplication(directory, panel, bytes) {
  await put(directory, `firmware-${panel}.bin`, bytes);
  await changeJson(directory, "ota-manifest.json", (manifest) => {
    manifest.variants[panel].size = bytes.length;
    manifest.variants[panel].sha256 = hash(bytes);
  });
}

async function projectFixture(t) {
  const directory = await fixture(t);
  await put(directory, "VERSION", `${VERSION}\n`);
  await put(directory, join("include", "version.h"), versionHeaderContents(VERSION));
  await putJson(directory, join("installer", "release-notes.json"), { [VERSION]: ["Enable OTA updates."] });
  return directory;
}

async function publisherFixture(t) {
  const directory = await projectFixture(t);
  const source = await buildFixture(directory);
  for (const filename of ["publish-firmware.mjs", "firmware-release.mjs"]) {
    await mkdir(join(directory, "scripts"), { recursive: true });
    await copyFile(join(projectDirectory, "scripts", filename), join(directory, "scripts", filename));
  }
  const environmentNames = ["esp32", "esp32-st7789"];
  for (const [index, panel] of PANELS.entries()) {
    const buildDirectory = join(directory, ".pio", "build", environmentNames[index]);
    for (const filename of ["firmware.bin", "bootloader.bin", "partitions.bin"]) {
      await put(buildDirectory, filename, await readFile(join(source.buildDirectories[panel], filename)));
    }
    await putJson(directory, join("installer", index === 0 ? "manifest.json" : "manifest-st7789.json"), {
      name: `ESP32 GT7 Dashboard (${panel.toUpperCase()})`,
      version: VERSION,
      new_install_prompt_erase: true,
      builds: [{
        chipFamily: "ESP32",
        parts: [
          { path: "firmware/bootloader.bin", offset: 0x1000 },
          { path: "firmware/partitions.bin", offset: 0x8000 },
          { path: "firmware/boot_app0.bin", offset: 0xe000 },
          { path: `firmware/firmware-${panel}.bin`, offset: 0x10000 },
        ],
      }],
    });
  }
  const core = join(directory, "core");
  await put(core, join("packages", "framework-arduinoespressif32", "tools", "partitions", "boot_app0.bin"), bootApp());
  await putJson(directory, join("installer", "releases.json"), {
    versions: [{ version: "1.8.0", notes: ["Legacy USB-only release."], manifests: {} }],
  });
  await put(directory, join("installer", "versions", "1.8.0", "legacy.bin"), "unchanged legacy bytes");
  await put(directory, "run", `require("node:fs").appendFileSync("build-calls", process.argv.slice(2).join(" ") + "\\n");\n`);
  return {
    directory,
    run: (...args) => spawnSync(process.execPath, [join(directory, "scripts", "publish-firmware.mjs"), ...args], {
      cwd: directory,
      encoding: "utf8",
      timeout: 30000,
      env: {
        ...process.env,
        PLATFORMIO_CMD: process.execPath,
        PLATFORMIO_CORE_DIR: core,
        PLATFORMIO_ENV: environmentNames[0],
        PLATFORMIO_ST7789_ENV: environmentNames[1],
      },
    }),
  };
}

test("canonical versions use bounded uint32 components and numeric ordering", () => {
  assert.deepEqual(parseVersion("0.0.0"), [0, 0, 0]);
  assert.deepEqual(parseVersion("4294967295.4294967295.4294967295"), [0xffffffff, 0xffffffff, 0xffffffff]);
  for (const invalid of ["01.2.3", "1.02.3", "1.2.03", "1.2", "v1.2.3", "1.2.3-beta", "1.2.3+build",
    " 1.2.3", "1.2.3\n", "-1.2.3", "1.2.4294967296", "4294967296.0.0", "11111111111111111111111111111111.0.0", 123, null]) {
    assert.throws(() => parseVersion(invalid), /Invalid version/);
  }
  for (const [older, newer] of [["1.9.9", "1.10.0"], ["9.999.999", "10.0.0"], ["0.0.9", "0.0.10"], ["1.2.3", "1.2.4294967295"]]) {
    assert.equal(compareVersions(older, newer), -1);
    assert.equal(compareVersions(newer, older), 1);
  }
  assert.equal(compareVersions("1.2.3", "1.2.3"), 0);
  assert.throws(() => compareVersions("01.2.3", "1.2.3"), /Invalid version/);
});

test("tag verification is read-only and rejects VERSION/header/notes disagreements", async (t) => {
  const directory = await projectFixture(t);
  assert.equal(await verifyTagVersion(directory, `v${VERSION}`), VERSION);
  for (const tag of ["1.9.0", "v01.9.0", "v1.9.0-beta", "v1.9.1"]) {
    await assert.rejects(verifyTagVersion(directory, tag), /tag|version|VERSION/i);
  }
  const badHeader = versionHeaderContents("1.8.0");
  await put(directory, join("include", "version.h"), badHeader);
  await assert.rejects(verifyTagVersion(directory, `v${VERSION}`), /include\/version.h/);
  assert.equal(await readFile(join(directory, "include", "version.h"), "utf8"), badHeader);
  assert.equal(await readFile(join(directory, "VERSION"), "utf8"), `${VERSION}\n`);
  await put(directory, join("include", "version.h"), `#pragma once\n\ninline constexpr char GT7_DASH_VERSION[] = "${VERSION}";\n`);
  await assert.rejects(verifyTagVersion(directory, `v${VERSION}`), /header format/);
  await put(directory, join("include", "version.h"), versionHeaderContents(VERSION).replaceAll("\n", "\r\n"));
  for (const notes of [{}, { [VERSION]: [] }, { [VERSION]: [" "] }, { [VERSION]: [42] }]) {
    await putJson(directory, join("installer", "release-notes.json"), notes);
    await assert.rejects(verifyTagVersion(directory, `v${VERSION}`), /release notes/i);
  }
});

test("valid release packages contain exact OTA/USB bytes, metadata and checksums", async (t) => {
  const { directory, build } = await releaseFixture(t);
  const { manifest, release } = await validateReleaseDirectory(directory, VERSION);
  assert.equal(RELEASE_FILES.length, 9);
  assert.deepEqual((await readdir(directory)).sort(), [...RELEASE_FILES, "checksums.sha256"].sort());
  assert.equal(manifest.releaseTag, `v${VERSION}`);
  assert.equal(release.ota, true);
  for (const panel of PANELS) {
    const bytes = await readFile(join(directory, `firmware-${panel}.bin`));
    assert.deepEqual(bytes, await readFile(join(build.buildDirectories[panel], "firmware.bin")));
    assert.equal(manifest.variants[panel].size, bytes.length);
    assert.equal(manifest.variants[panel].sha256, hash(bytes));
    const usb = await json(directory, release.manifests[panel]);
    assert.deepEqual(usb.builds[0].parts.map((part) => part.offset), [0x1000, 0x8000, 0xe000, 0x10000]);
  }
  await assert.rejects(validateReleaseDirectory(directory, "1.10.0"), /expected version/);
});

test("repackaging removes a stale generated ZIP without deleting unrelated output entries", async (t) => {
  const { directory, build } = await releaseFixture(t);
  await put(directory, "usb-installer.zip", "archive for the old release");
  await writeChecksums(directory);
  const nextVersion = "1.10.0";
  for (const panel of PANELS) {
    await put(build.buildDirectories[panel], "firmware.bin", image({ panel, version: nextVersion }));
  }
  await put(directory, "maintainer.zip", "unrelated archive must not be deleted");
  await assert.rejects(createReleaseDirectory(directory, { ...build, version: nextVersion }), /Unexpected or unsafe/);
  assert.equal(await readFile(join(directory, "maintainer.zip"), "utf8"), "unrelated archive must not be deleted");
  assert.equal(await readFile(join(directory, "usb-installer.zip"), "utf8"), "archive for the old release");
  await rm(join(directory, "maintainer.zip"));

  await createReleaseDirectory(directory, { ...build, version: nextVersion });
  await validateReleaseDirectory(directory, nextVersion);
  await absent(join(directory, "usb-installer.zip"));
  const checksums = await readFile(join(directory, "checksums.sha256"), "utf8");
  assert.doesNotMatch(checksums, /usb-installer\.zip/);
  assert.equal(checksums.trimEnd().split("\n").length, 9);
});

test("application validation rejects bad headers, wrong panel, stale or absent identities and invalid images", async (t) => {
  const cases = [
    ["wrong panel", () => image({ panel: "st7789" }), /identity/],
    ["stale version", () => image({ panel: "ili9341", version: "1.8.0" }), /identity/],
    ["missing identity", () => image(), /identity/],
    ["wrong layout", () => image({ identity: `GT7DASH-OTA:1|esp32-gt7-dashboard|${VERSION}|ili9341|legacy|END\0` }), /identity/],
    ["wrong product", () => image({ identity: `GT7DASH-OTA:1|other-product|${VERSION}|ili9341|esp32-4mb-min-spiffs-v1|END\0` }), /identity/],
    ["bad ESP magic", () => { const bytes = image({ panel: "ili9341" }); bytes[0] = 0; return bytes; }, /header/],
    ["other chip family", () => { const bytes = image({ panel: "ili9341" }); bytes[12] = 9; return bytes; }, /header/],
    ["wrong flash size", () => { const bytes = image({ panel: "ili9341" }); bytes[3] = 0x10; return bytes; }, /header/],
    ["invalid segment count", () => { const bytes = image({ panel: "ili9341" }); bytes[1] = 0; return bytes; }, /header/],
    ["truncated segment", () => { const bytes = image({ panel: "ili9341" }); bytes.writeUInt32LE(4096, 28); return bytes; }, /segment/],
    ["corrupt ESP checksum", () => { const bytes = image({ panel: "ili9341" }); bytes[bytes.length - 33] ^= 1; return bytes; }, /checksum/],
    ["corrupt ESP digest", () => { const bytes = image({ panel: "ili9341" }); bytes[bytes.length - 1] ^= 1; return bytes; }, /digest/],
    ["identity appended outside a real image", () => Buffer.concat([image(), Buffer.from(`GT7DASH-OTA:1|esp32-gt7-dashboard|${VERSION}|ili9341|esp32-4mb-min-spiffs-v1|END\0`)]), /trailing data/],
  ];
  for (const [name, bytes, error] of cases) {
    await t.test(name, async (t) => {
      const { directory } = await releaseFixture(t);
      await replaceApplication(directory, "ili9341", bytes());
      await assert.rejects(validateReleaseDirectory(directory), error);
    });
  }
});

test("image scanner permits runtime prefixes and identical copies but rejects conflicting identities", async (t) => {
  const { directory } = await releaseFixture(t);
  const identity = `GT7DASH-OTA:1|esp32-gt7-dashboard|${VERSION}|ili9341|esp32-4mb-min-spiffs-v1|END\0`;
  const bytes = image({
    identity: `GT7DASH-OTA:\0GT7DASH-OTA:1|\0GT7DASH-OTA:1|%s|%s|%s|%s|END\0${identity}${identity}`,
    payloadLength: 512,
  });
  await replaceApplication(directory, "ili9341", bytes);
  await writeChecksums(directory);
  await validateReleaseDirectory(directory, VERSION);
  await replaceApplication(directory, "ili9341", image({
    identity: `${identity}${identity.replace(VERSION, "9.9.9")}`, payloadLength: 512,
  }));
  await assert.rejects(validateReleaseDirectory(directory), /stale or incompatible firmware identity/);
});

test("images must fit both OTA slots, including the full boundary size", async (t) => {
  const { directory } = await releaseFixture(t);
  const fullSlot = image({ panel: "ili9341", payloadLength: SLOT_BYTES - 80 });
  assert.equal(fullSlot.length, SLOT_BYTES);
  await replaceApplication(directory, "ili9341", fullSlot);
  await writeChecksums(directory);
  await validateReleaseDirectory(directory);
  for (const panel of PANELS) {
    await t.test(`oversized ${panel}`, async (t) => {
      const { directory, build } = await releaseFixture(t);
      const oversized = Buffer.alloc(SLOT_BYTES + 1);
      await put(build.buildDirectories[panel], "firmware.bin", oversized);
      await assert.rejects(createReleaseDirectory(join(dirname(directory), "oversized-release"), build), /file size/);
      await replaceApplication(directory, panel, oversized);
      await assert.rejects(validateReleaseDirectory(directory), /file size|both .*OTA slots/);
    });
  }
});

test("partition layout and boot artifacts are validated, not just application metadata", async (t) => {
  for (const [name, offset, value] of [
    ["NVS offset", 4, 0xa000],
    ["OTA-data offset", 32 + 4, 0xd000],
    ["app0 offset", 64 + 4, 0x20000],
    ["app0 size", 64 + 8, SLOT_BYTES - 0x10000],
    ["app1 offset", 96 + 4, 0x200000],
    ["app1 size", 96 + 8, SLOT_BYTES - 0x10000],
    ["SPIFFS offset", 128 + 4, 0x310000],
  ]) {
    await t.test(name, async (t) => {
      const { directory } = await releaseFixture(t);
      const bytes = partitions();
      bytes.writeUInt32LE(value, offset);
      createHash("md5").update(bytes.subarray(0, 192)).digest().copy(bytes, 208);
      await put(directory, "partitions.bin", bytes);
      await assert.rejects(validateReleaseDirectory(directory), /partition layout/);
    });
  }
  for (const [name, filename, bytes, error] of [
    ["partition MD5", "partitions.bin", (() => { const bytes = partitions(); bytes[208] ^= 1; return bytes; })(), /MD5/],
    ["partition truncation", "partitions.bin", partitions().subarray(0, 224), /partition table/],
    ["bootloader header", "bootloader.bin", Buffer.alloc(256), /image header/],
    ["bootloader overlaps partition table", "bootloader.bin", Buffer.alloc(0x7001), /file size/],
    ["invalid OTA data initializer", "boot_app0.bin", Buffer.alloc(8192), /OTA-data initializer/],
    ["oversized OTA data initializer", "boot_app0.bin", Buffer.alloc(8193), /file size/],
  ]) {
    await t.test(name, async (t) => {
      const { directory } = await releaseFixture(t);
      await put(directory, filename, bytes);
      await assert.rejects(validateReleaseDirectory(directory), error);
    });
  }
});

test("source builds must agree on shared bootloader and partition bytes before packaging", async (t) => {
  for (const filename of ["bootloader.bin", "partitions.bin"]) {
    await t.test(filename, async (t) => {
      const root = await fixture(t);
      const build = await buildFixture(root);
      const path = join(build.buildDirectories.st7789, filename);
      const bytes = await readFile(path);
      bytes[bytes.length - 1] ^= 1;
      await writeFile(path, bytes);
      const directory = join(root, "release");
      await assert.rejects(createReleaseDirectory(directory, build), new RegExp(`differing shared ${filename}`));
      await absent(directory);
    });
  }
});

test("missing files, truncation, corrupted downloads, and malformed SHA-256 fail validation", async (t) => {
  for (const filename of RELEASE_FILES) {
    await t.test(`missing ${filename}`, async (t) => {
      const { directory } = await releaseFixture(t);
      await rm(join(directory, filename));
      await assert.rejects(validateReleaseDirectory(directory), /Missing required release file/);
    });
  }
  for (const kind of ["truncated", "empty", "corrupted"]) {
    await t.test(kind, async (t) => {
      const { directory } = await releaseFixture(t);
      const filename = "firmware-ili9341.bin";
      const bytes = await readFile(join(directory, filename));
      if (kind === "corrupted") bytes[80] ^= 1;
      await put(directory, filename, kind === "truncated" ? bytes.subarray(0, -1) : kind === "empty" ? Buffer.alloc(0) : bytes);
      await assert.rejects(validateReleaseDirectory(directory), /size|SHA-256/);
    });
  }
  for (const sha256 of ["A".repeat(64), "g".repeat(64), "0".repeat(63), 123]) {
    await t.test(`invalid hash ${sha256}`, async (t) => {
      const { directory } = await releaseFixture(t);
      await changeJson(directory, "ota-manifest.json", (manifest) => { manifest.variants.ili9341.sha256 = sha256; });
      await assert.rejects(validateReleaseDirectory(directory), /Invalid SHA-256/);
    });
  }
});

test("OTA schema and release metadata reject incompatible values and oversized metadata", async (t) => {
  const cases = [
    ["unsupported schema", "ota-manifest.json", (value) => { value.schemaVersion = 2; }, /schema/],
    ["wrong product", "ota-manifest.json", (value) => { value.product = "another-dashboard"; }, /product/],
    ["wrong chip", "ota-manifest.json", (value) => { value.chipFamily = "ESP32-S3"; }, /chip/],
    ["wrong layout", "ota-manifest.json", (value) => { value.layout = "huge_app"; }, /layout/],
    ["noncanonical version", "ota-manifest.json", (value) => { value.version = "01.9.0"; }, /Invalid version/],
    ["tag disagreement", "ota-manifest.json", (value) => { value.releaseTag = "v1.8.0"; }, /releaseTag/],
    ["unknown panel", "ota-manifest.json", (value) => { value.variants.extra = value.variants.ili9341; }, /variants/],
    ["missing panel", "ota-manifest.json", (value) => { delete value.variants.st7789; }, /variants/],
    ["arbitrary URL", "ota-manifest.json", (value) => { value.variants.ili9341.filename = "https://example.com/firmware.bin"; }, /filename/],
    ["extra URL field", "ota-manifest.json", (value) => { value.variants.ili9341.url = "https://example.com"; }, /exactly/],
    ["negative size", "ota-manifest.json", (value) => { value.variants.ili9341.size = -1; }, /size/],
    ["fractional size", "ota-manifest.json", (value) => { value.variants.ili9341.size = 100.5; }, /size/],
    ["release version", "release.json", (value) => { value.version = "1.8.0"; }, /version/],
    ["OTA disabled", "release.json", (value) => { value.ota = false; }, /OTA capability/],
    ["blank note", "release.json", (value) => { value.notes = [" "]; }, /notes/],
    ["nonstrings in notes", "release.json", (value) => { value.notes = [123]; }, /notes/],
    ["unsafe release manifest", "release.json", (value) => { value.manifests.ili9341 = "../manifest.json"; }, /unsafe manifest path/],
  ];
  for (const [name, filename, mutate, error] of cases) {
    await t.test(name, async (t) => {
      const { directory } = await releaseFixture(t);
      await changeJson(directory, filename, mutate);
      await assert.rejects(validateReleaseDirectory(directory), error);
    });
  }
  const { directory } = await releaseFixture(t);
  await put(directory, "ota-manifest.json", Buffer.alloc(8193, 0x20));
  await assert.rejects(validateReleaseDirectory(directory), /file size/);
  await put(directory, "ota-manifest.json", "{bad json");
  await assert.rejects(validateReleaseDirectory(directory), /not valid JSON/);
});

test("USB manifests enforce fixed contained paths, panel, chip family and all four boot offsets", async (t) => {
  for (const path of ["../bootloader.bin", "/bootloader.bin", "..\\bootloader.bin", "C:\\bootloader.bin",
    "//example.com/bootloader.bin", "https://example.com/bootloader.bin", "./bootloader.bin", "firmware-st7789.bin"]) {
    await t.test(`path ${path}`, async (t) => {
      const { directory } = await releaseFixture(t);
      await changeJson(directory, "manifest-ili9341.json", (manifest) => { manifest.builds[0].parts[0].path = path; });
      await assert.rejects(validateReleaseDirectory(directory), /unsafe, unexpected, or duplicate path/);
    });
  }
  for (let index = 0; index < 4; index++) {
    await t.test(`offset ${index}`, async (t) => {
      const { directory } = await releaseFixture(t);
      await changeJson(directory, "manifest-ili9341.json", (manifest) => { manifest.builds[0].parts[index].offset += 4096; });
      await assert.rejects(validateReleaseDirectory(directory), /flash offset/);
    });
  }
  for (const [name, mutate, error] of [
    ["wrong panel name", (value) => { value.name = "ESP32 GT7 Dashboard (ST7789)"; }, /panel name/],
    ["wrong USB version", (value) => { value.version = "1.8.0"; }, /version/],
    ["wrong chip", (value) => { value.builds[0].chipFamily = "ESP8266"; }, /ESP32 build/],
    ["missing part", (value) => { value.builds[0].parts.pop(); }, /four flash parts/],
    ["duplicate part", (value) => { value.builds[0].parts[1] = value.builds[0].parts[0]; }, /duplicate path/],
    ["extra build", (value) => { value.builds.push(value.builds[0]); }, /one ESP32 build/],
  ]) {
    await t.test(name, async (t) => {
      const { directory } = await releaseFixture(t);
      await changeJson(directory, "manifest-ili9341.json", mutate);
      await assert.rejects(validateReleaseDirectory(directory), error);
    });
  }
});

test("checksums include only fixed payloads and an optional USB archive", async (t) => {
  const { directory } = await releaseFixture(t);
  const original = await readFile(join(directory, "checksums.sha256"), "utf8");
  assert.equal(original.trim().split("\n").length, 9);
  await put(directory, "usb-installer.zip", "archive bytes are additionally validated by the installer staging tool");
  await assert.rejects(validateReleaseDirectory(directory), /every payload/);
  const regenerated = await writeChecksums(directory);
  assert.equal(regenerated.trim().split("\n").length, 10);
  assert.match(regenerated, /  usb-installer\.zip\n/);
  assert.doesNotMatch(regenerated, /  checksums\.sha256/);
  await validateReleaseDirectory(directory);
  await put(directory, "checksums.sha256", regenerated.replace(/^[a-f0-9]{64}/, "0".repeat(64)));
  await assert.rejects(validateReleaseDirectory(directory), /Checksum mismatch/);
  await writeChecksums(directory);
  await put(directory, "unexpected.bin", "unsafe extra file");
  await assert.rejects(writeChecksums(directory), /Unexpected or unsafe release payload entry/);
  await rm(join(directory, "unexpected.bin"));
  await mkdir(join(directory, "nested"));
  await assert.rejects(validateReleaseDirectory(directory), /Unexpected or unsafe release payload entry/);
});

test("release validation requires the checksum inventory", async (t) => {
  const { directory } = await releaseFixture(t);
  await rm(join(directory, "checksums.sha256"));
  await assert.rejects(validateReleaseDirectory(directory), /Missing required release file: checksums.sha256/);
  await writeChecksums(directory);
  await validateReleaseDirectory(directory, VERSION);
});

test("published checksums may list an unfetched USB archive but cannot omit a required payload", async (t) => {
  const { directory } = await releaseFixture(t);
  const original = await readFile(join(directory, "checksums.sha256"), "utf8");
  const external = `${original}${"a".repeat(64)}  usb-installer.zip\n`;
  await put(directory, "checksums.sha256", external);
  await validateReleaseDirectory(directory, VERSION);
  assert.equal(await readFile(join(directory, "checksums.sha256"), "utf8"), external);

  await put(directory, "checksums.sha256", external.replace(/^[a-f0-9]{64}  bootloader\.bin\n/m, ""));
  await assert.rejects(validateReleaseDirectory(directory), /every payload/);
  await put(directory, "checksums.sha256", external.replace("  usb-installer.zip", "  ../usb-installer.zip"));
  await assert.rejects(validateReleaseDirectory(directory), /unsafe, unexpected, or duplicate entry/);
  await put(directory, "checksums.sha256", external);
  await put(directory, "usb-installer.zip", "downloaded archive with a different hash");
  await assert.rejects(validateReleaseDirectory(directory), /Checksum mismatch for usb-installer.zip/);
  await rm(join(directory, "usb-installer.zip"));
  assert.equal(await writeChecksums(directory), original);
});

test("release payload symlinks cannot escape the package directory", async (t) => {
  const { root, directory } = await releaseFixture(t);
  await put(root, "outside.bin", image({ panel: "ili9341" }));
  await rm(join(directory, "firmware-ili9341.bin"));
  try {
    await symlink(join(root, "outside.bin"), join(directory, "firmware-ili9341.bin"), "file");
  } catch (error) {
    if (["EPERM", "EACCES", "ENOTSUP", "UNKNOWN"].includes(error.code)) {
      t.skip(`This host does not permit creating fixture symlinks (${error.code}).`);
      return;
    }
    throw error;
  }
  await assert.rejects(validateReleaseDirectory(directory), /unsafe release payload entry/);
  await assert.rejects(writeChecksums(directory), /unsafe release payload entry/);
});

test("publisher --skip-build synchronizes versions only and never relabels stale binaries as a release", async (t) => {
  const { directory, run } = await publisherFixture(t);
  await put(directory, join("installer", "firmware", "firmware-ili9341.bin"), "legacy firmware must not be repackaged");
  const releasesBefore = await readFile(join(directory, "installer", "releases.json"));
  const result = run("1.10.0", "--skip-build");
  assert.equal(result.status, 0, result.stderr || result.stdout);
  assert.match(result.stdout, /not generated \(--skip-build\)/);
  assert.equal(await readFile(join(directory, "include", "version.h"), "utf8"), versionHeaderContents("1.10.0"));
  assert.equal((await json(directory, join("installer", "manifest.json"))).version, "1.10.0");
  assert.equal(await readFile(join(directory, "installer", "firmware", "firmware-ili9341.bin"), "utf8"),
    "legacy firmware must not be repackaged");
  assert.deepEqual(await readFile(join(directory, "installer", "releases.json")), releasesBefore);
  await absent(join(directory, "build-calls"));
  await absent(join(directory, ".pio", "release"));
  await absent(join(directory, "installer", "versions", "1.10.0"));

  await put(directory, join(".pio", "release", "release.json"), "existing release is not touched");
  const second = run("--skip-build");
  assert.equal(second.status, 0, second.stderr || second.stdout);
  assert.equal(await readFile(join(directory, ".pio", "release", "release.json"), "utf8"), "existing release is not touched");

  const staleBuild = await buildFixture(join(directory, "stale-build"));
  const releaseDirectory = join(directory, ".pio", "release");
  await createReleaseDirectory(releaseDirectory, staleBuild);
  const previousManifest = await readFile(join(releaseDirectory, "ota-manifest.json"));
  const third = run("--skip-build");
  assert.equal(third.status, 0, third.stderr || third.stdout);
  assert.match(third.stdout, /not refreshed or validated/);
  assert.deepEqual(await readFile(join(releaseDirectory, "ota-manifest.json")), previousManifest);
  await validateReleaseDirectory(releaseDirectory, VERSION);
  await assert.rejects(validateReleaseDirectory(releaseDirectory, "1.10.0"), /expected version/);
  await absent(join(directory, "build-calls"));
});

test("publisher packages both builds, keeps legacy archives, and marks only the verified entry OTA capable", async (t) => {
  const { directory, run } = await publisherFixture(t);
  const result = run();
  assert.equal(result.status, 0, result.stderr || result.stdout);
  assert.equal(await readFile(join(directory, "build-calls"), "utf8"), "-e esp32\n-e esp32-st7789\n");
  await validateReleaseDirectory(join(directory, ".pio", "release"), VERSION);
  await validateReleaseDirectory(join(directory, "installer", "versions", VERSION), VERSION);
  const releases = (await json(directory, join("installer", "releases.json"))).versions;
  assert.equal(releases[0].ota, true);
  assert.equal(releases[0].version, VERSION);
  assert.equal(releases[1].version, "1.8.0");
  assert.equal(releases[1].ota, undefined);
  assert.equal(await readFile(join(directory, "installer", "versions", "1.8.0", "legacy.bin"), "utf8"), "unchanged legacy bytes");
  for (const panel of PANELS) {
    assert.deepEqual(await readFile(join(directory, "installer", "firmware", `firmware-${panel}.bin`)),
      await readFile(join(directory, ".pio", "release", `firmware-${panel}.bin`)));
  }
});

test("publisher rejects stale build identities without overwriting installer binaries or creating packages", async (t) => {
  const { directory, run } = await publisherFixture(t);
  await put(directory, join(".pio", "build", "esp32-st7789", "firmware.bin"), image({ panel: "st7789", version: "1.8.0" }));
  await put(directory, join("installer", "firmware", "firmware-ili9341.bin"), "original installer image");
  const result = run();
  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /stale or incompatible firmware identity/);
  assert.equal(await readFile(join(directory, "installer", "firmware", "firmware-ili9341.bin"), "utf8"), "original installer image");
  await absent(join(directory, ".pio", "release"));
  await absent(join(directory, "installer", "versions", VERSION));
});

test("publisher retains ten releases and never prunes outside canonical version directories", async (t) => {
  const { directory, run } = await publisherFixture(t);
  const versions = Array.from({ length: 10 }, (_, index) => ({ version: `1.0.${9 - index}`, notes: ["Legacy."], manifests: {} }));
  await putJson(directory, join("installer", "releases.json"), { versions });
  for (const { version } of versions) await put(directory, join("installer", "versions", version, "legacy.bin"), version);
  const result = run();
  assert.equal(result.status, 0, result.stderr || result.stdout);
  const retained = (await json(directory, join("installer", "releases.json"))).versions;
  assert.equal(retained.length, 10);
  assert.equal(retained[0].version, VERSION);
  assert.equal(retained.at(-1).version, "1.0.1");
  await absent(join(directory, "installer", "versions", "1.0.0"));
  await putJson(directory, join("installer", "releases.json"), { versions: [{ version: "../../outside" }] });
  await put(directory, "outside", "must stay");
  const unsafe = run();
  assert.notEqual(unsafe.status, 0);
  assert.match(unsafe.stderr, /Invalid version/);
  assert.equal(await readFile(join(directory, "outside"), "utf8"), "must stay");
});

test("release CLI supports validate/checksums/verify-tag and import has no side effects", async (t) => {
  const { root, directory } = await releaseFixture(t);
  const cli = join(projectDirectory, "scripts", "firmware-release.mjs");
  for (const args of [["validate", directory, VERSION], ["checksums", directory]]) {
    const result = spawnSync(process.execPath, [cli, ...args], { encoding: "utf8", timeout: 30000 });
    assert.equal(result.status, 0, result.stderr || result.stdout);
  }
  const copiedProject = await projectFixture(t);
  await mkdir(join(copiedProject, "scripts"));
  const copiedCli = join(copiedProject, "scripts", "firmware-release.mjs");
  await copyFile(cli, copiedCli);
  const verified = spawnSync(process.execPath, [copiedCli, "verify-tag", `v${VERSION}`], { encoding: "utf8", timeout: 30000 });
  assert.equal(verified.status, 0, verified.stderr || verified.stdout);
  const mismatch = spawnSync(process.execPath, [copiedCli, "verify-tag", "v1.8.0"], { encoding: "utf8", timeout: 30000 });
  assert.notEqual(mismatch.status, 0);
  assert.match(mismatch.stderr, /VERSION/);
  const before = await readdir(root);
  const imported = spawnSync(process.execPath, ["--input-type=module", "-e", `await import(${JSON.stringify(new URL("../firmware-release.mjs", import.meta.url).href)})`],
    { cwd: root, encoding: "utf8", timeout: 30000 });
  assert.equal(imported.status, 0, imported.stderr || imported.stdout);
  assert.equal(imported.stdout, "");
  assert.deepEqual(await readdir(root), before);
});
