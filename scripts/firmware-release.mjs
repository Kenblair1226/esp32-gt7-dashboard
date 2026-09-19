#!/usr/bin/env node

import { createHash } from "node:crypto";
import { lstat, mkdir, readFile, readdir, rm, writeFile } from "node:fs/promises";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const PRODUCT = "esp32-gt7-dashboard";
const LAYOUT = "esp32-4mb-min-spiffs-v1";
const PANELS = ["ili9341", "st7789"];
const SLOT_BYTES = 0x1e0000;
const MAX_MANIFEST_BYTES = 8192;
const CHECKSUM_FILE = "checksums.sha256";
const USB_ARCHIVE = "usb-installer.zip";
const PARTITIONS = [
  { name: "nvs", type: 1, subtype: 2, offset: 0x9000, size: 0x5000 },
  { name: "otadata", type: 1, subtype: 0, offset: 0xe000, size: 0x2000 },
  { name: "app0", type: 0, subtype: 0x10, offset: 0x10000, size: SLOT_BYTES },
  { name: "app1", type: 0, subtype: 0x11, offset: 0x1f0000, size: SLOT_BYTES },
  { name: "spiffs", type: 1, subtype: 0x82, offset: 0x3d0000, size: 0x20000 },
  { name: "coredump", type: 1, subtype: 3, offset: 0x3f0000, size: 0x10000 },
];

export const RELEASE_FILES = Object.freeze([
  "ota-manifest.json",
  "firmware-ili9341.bin",
  "firmware-st7789.bin",
  "bootloader.bin",
  "partitions.bin",
  "boot_app0.bin",
  "manifest-ili9341.json",
  "manifest-st7789.json",
  "release.json",
]);

function requireCondition(condition, message) {
  if (!condition) throw new Error(message);
}

function isRecord(value) {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

function requireKeys(value, keys, label) {
  requireCondition(
    isRecord(value) && Object.keys(value).length === keys.length &&
      keys.every((key) => Object.hasOwn(value, key)),
    `${label} must contain exactly: ${keys.join(", ")}.`,
  );
}

function digest(bytes, algorithm = "sha256") {
  return createHash(algorithm).update(bytes).digest("hex");
}

export function parseVersion(version) {
  requireCondition(
    typeof version === "string" && version.length <= 32 &&
      /^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$/.test(version),
    `Invalid version "${version}": expected canonical x.y.z (at most 32 characters, no leading zeros).`,
  );
  const components = version.split(".").map(Number);
  requireCondition(
    components.every((component) => Number.isInteger(component) && component <= 0xffffffff),
    `Invalid version "${version}": every component must fit uint32.`,
  );
  return components;
}

export function compareVersions(left, right) {
  const leftParts = parseVersion(left);
  const rightParts = parseVersion(right);
  for (let index = 0; index < leftParts.length; index++) {
    if (leftParts[index] !== rightParts[index]) {
      return leftParts[index] < rightParts[index] ? -1 : 1;
    }
  }
  return 0;
}

export function versionHeaderContents(version) {
  parseVersion(version);
  return `#pragma once\n\n#define GT7_DASH_VERSION_LITERAL "${version}"\ninline constexpr char GT7_DASH_VERSION[] = GT7_DASH_VERSION_LITERAL;\n`;
}

export async function verifyProjectVersion(projectDirectory, expectedVersion) {
  const version = (await readFile(join(projectDirectory, "VERSION"), "utf8")).trim();
  parseVersion(version);
  requireCondition(version === expectedVersion, `VERSION "${version}" does not match expected version "${expectedVersion}".`);
  const header = await readFile(join(projectDirectory, "include", "version.h"), "utf8");
  requireCondition(
    header.replaceAll("\r\n", "\n") === versionHeaderContents(version),
    `include/version.h does not match VERSION "${version}" or the GT7_DASH_VERSION_LITERAL header format.`,
  );
  return version;
}

function validateNotes(notes, version) {
  requireCondition(
    Array.isArray(notes) && notes.length > 0 &&
      notes.every((note) => typeof note === "string" && note.trim().length > 0),
    `Release notes for ${version} must be a non-empty array of non-empty strings.`,
  );
}

export async function readReleaseNotes(projectDirectory, version) {
  parseVersion(version);
  const notesByVersion = parseJson(
    await readFile(join(projectDirectory, "installer", "release-notes.json")),
    "installer/release-notes.json",
  );
  requireCondition(isRecord(notesByVersion) && Object.hasOwn(notesByVersion, version),
    `Add release notes for ${version} to installer/release-notes.json.`);
  validateNotes(notesByVersion[version], version);
  return notesByVersion[version];
}

export async function verifyTagVersion(projectDirectory, tag) {
  requireCondition(typeof tag === "string" && tag.startsWith("v"),
    `Invalid release tag "${tag}": expected vx.y.z.`);
  const version = tag.slice(1);
  parseVersion(version);
  await verifyProjectVersion(projectDirectory, version);
  await readReleaseNotes(projectDirectory, version);
  return version;
}

function parseJson(bytes, label) {
  try {
    return JSON.parse(bytes.toString("utf8"));
  } catch (error) {
    throw new Error(`${label} is not valid JSON: ${error.message}`);
  }
}

async function readRegularFile(path, maximumBytes) {
  let details;
  try {
    details = await lstat(path);
  } catch (error) {
    if (error.code === "ENOENT") throw new Error(`Missing required file: ${path}`);
    throw error;
  }
  requireCondition(details.isFile() && !details.isSymbolicLink(),
    `Expected a regular file, not a symlink or directory: ${path}`);
  requireCondition(details.size > 0 && details.size <= maximumBytes,
    `Invalid file size for ${path}: ${details.size} bytes; expected 1..${maximumBytes}.`);
  const bytes = await readFile(path);
  requireCondition(bytes.length === details.size, `File changed while reading: ${path}`);
  return bytes;
}

function validateEsp32Image(bytes, label, version, panel) {
  requireCondition(
    bytes.length >= 48 && bytes[0] === 0xe9 && bytes[1] >= 1 && bytes[1] <= 16 &&
      bytes[2] <= 3 && (bytes[3] >> 4) === 2 &&
      [0, 1, 2, 0xf].includes(bytes[3] & 0xf) &&
      bytes.readUInt16LE(12) === 0 && bytes[23] <= 1,
    `${label} has an invalid ESP32 4 MiB image header.`,
  );
  const segments = [];
  let position = 24;
  let checksum = 0xef;
  for (let index = 0; index < bytes[1]; index++) {
    requireCondition(position + 8 <= bytes.length, `${label} has a truncated ESP32 segment header.`);
    const length = bytes.readUInt32LE(position + 4);
    position += 8;
    requireCondition(length % 4 === 0 && length <= bytes.length - position,
      `${label} has a malformed or truncated ESP32 segment.`);
    const segment = bytes.subarray(position, position + length);
    for (const byte of segment) checksum ^= byte;
    segments.push(segment);
    position += length;
  }
  const checksumOffset = Math.floor(position / 16) * 16 + 15;
  const imageEnd = checksumOffset + 1;
  requireCondition(
    bytes.length === imageEnd + (bytes[23] === 1 ? 32 : 0) &&
      bytes.subarray(position, checksumOffset).every((byte) => byte === 0) &&
      bytes[checksumOffset] === checksum,
    `${label} has a truncated image, invalid ESP32 checksum/padding, or unexpected trailing data.`,
  );
  if (bytes[23] === 1) {
    requireCondition(digest(bytes.subarray(0, imageEnd)) === bytes.subarray(imageEnd).toString("hex"),
      `${label} has an invalid ESP32 image SHA-256 digest.`);
  }
  if (panel !== undefined) {
    const prefix = Buffer.from(`GT7DASH-OTA:1|${PRODUCT}|`);
    const identity = Buffer.from(`GT7DASH-OTA:1|${PRODUCT}|${version}|${panel}|${LAYOUT}|END\0`);
    let matches = 0;
    for (const segment of segments) {
      let offset = segment.indexOf(prefix);
      while (offset !== -1) {
        requireCondition(segment.subarray(offset, offset + identity.length).equals(identity),
          `${label} has a stale or incompatible firmware identity (expected ${version}, ${panel}, ${LAYOUT}).`);
        matches += 1;
        offset = segment.indexOf(prefix, offset + prefix.length);
      }
    }
    requireCondition(matches > 0, `${label} is missing its embedded firmware identity for ${version}/${panel}/${LAYOUT}.`);
  }
}

function validatePartitions(bytes, label) {
  requireCondition(bytes.length === 0xc00, `${label} must be a complete 3072-byte ESP32 partition table.`);
  for (const [index, expected] of PARTITIONS.entries()) {
    const entry = bytes.subarray(index * 32, (index + 1) * 32);
    const name = Buffer.alloc(16);
    name.write(expected.name);
    requireCondition(
      entry.readUInt16LE(0) === 0x50aa && entry[2] === expected.type && entry[3] === expected.subtype &&
        entry.readUInt32LE(4) === expected.offset && entry.readUInt32LE(8) === expected.size &&
        entry.subarray(12, 28).equals(name) && entry.readUInt32LE(28) === 0,
      `${label} has an incompatible partition layout at ${expected.name}; expected offset 0x${expected.offset.toString(16)}, size 0x${expected.size.toString(16)} (${LAYOUT}).`,
    );
  }
  const trailerOffset = PARTITIONS.length * 32;
  const trailer = bytes.subarray(trailerOffset, trailerOffset + 32);
  requireCondition(
    trailer.readUInt16LE(0) === 0xebeb && trailer.subarray(2, 16).every((byte) => byte === 0xff) &&
      trailer.subarray(16).toString("hex") === digest(bytes.subarray(0, trailerOffset), "md5") &&
      bytes.subarray(trailerOffset + 32).every((byte) => byte === 0xff),
    `${label} has an invalid partition-table MD5 trailer or unexpected partitions/padding.`,
  );
}

function validateBootApp(bytes) {
  const expected = Buffer.alloc(0x2000, 0xff);
  expected.writeUInt32LE(1, 0);
  expected.writeUInt32LE(0x4743989a, 28);
  expected.writeUInt32LE(0, 0x1000);
  requireCondition(bytes.equals(expected),
    "boot_app0.bin must be the pinned Arduino ESP32 2.0.17 8192-byte OTA-data initializer.");
}

function installerParts(panel, prefix = "") {
  return [
    { path: `${prefix}bootloader.bin`, offset: 0x1000 },
    { path: `${prefix}partitions.bin`, offset: 0x8000 },
    { path: `${prefix}boot_app0.bin`, offset: 0xe000 },
    { path: `${prefix}firmware-${panel}.bin`, offset: 0x10000 },
  ];
}

export function validateInstallerManifest(manifest, panel, version, prefix = "") {
  requireCondition(PANELS.includes(panel), `Unsupported display panel "${panel}".`);
  requireCondition(
    isRecord(manifest) && manifest.version === version && typeof manifest.name === "string" &&
      manifest.name.toUpperCase().includes(panel.toUpperCase()),
    `USB manifest for ${panel} has an inconsistent version or panel name.`,
  );
  requireCondition(Array.isArray(manifest.builds) && manifest.builds.length === 1 &&
    manifest.builds[0]?.chipFamily === "ESP32", `USB manifest for ${panel} must contain exactly one ESP32 build.`);
  const parts = manifest.builds[0].parts;
  requireCondition(Array.isArray(parts) && parts.length === 4,
    `USB manifest for ${panel} must contain exactly four flash parts.`);
  const expectedParts = installerParts(panel, prefix);
  const seen = new Set();
  for (const part of parts) {
    const expected = expectedParts.find((entry) => entry.path === part?.path);
    requireCondition(expected && !seen.has(part.path),
      `USB manifest for ${panel} contains an unsafe, unexpected, or duplicate path: ${part?.path}`);
    requireCondition(Number.isInteger(part.offset) && part.offset === expected.offset,
      `USB manifest for ${panel}: ${part.path} must use flash offset 0x${expected.offset.toString(16)}.`);
    seen.add(part.path);
  }
}

function validateOtaManifest(manifest, expectedVersion) {
  requireKeys(manifest, ["schemaVersion", "product", "chipFamily", "layout", "version", "releaseTag", "variants"],
    "ota-manifest.json");
  requireCondition(manifest.schemaVersion === 1 && manifest.product === PRODUCT &&
    manifest.chipFamily === "ESP32" && manifest.layout === LAYOUT,
  "ota-manifest.json has an unsupported schema, product, chip family, or layout.");
  parseVersion(manifest.version);
  if (expectedVersion !== undefined) {
    parseVersion(expectedVersion);
    requireCondition(manifest.version === expectedVersion,
      `Release version "${manifest.version}" does not match expected version "${expectedVersion}".`);
  }
  requireCondition(manifest.releaseTag === `v${manifest.version}`, "ota-manifest.json releaseTag does not match its version.");
  requireKeys(manifest.variants, PANELS, "ota-manifest.json variants");
  for (const panel of PANELS) {
    const variant = manifest.variants[panel];
    requireKeys(variant, ["filename", "size", "sha256"], `ota-manifest.json ${panel}`);
    requireCondition(variant.filename === `firmware-${panel}.bin`, `Invalid firmware filename for ${panel}.`);
    requireCondition(Number.isInteger(variant.size) && variant.size > 0 && variant.size <= SLOT_BYTES,
      `Firmware size for ${panel} must fit both ${SLOT_BYTES}-byte OTA slots.`);
    requireCondition(typeof variant.sha256 === "string" && /^[a-f0-9]{64}$/.test(variant.sha256),
      `Invalid SHA-256 for ${panel}: expected 64 lowercase hexadecimal characters.`);
  }
}

async function payloadEntries(directory, requireFiles = true) {
  const details = await lstat(directory);
  requireCondition(details.isDirectory() && !details.isSymbolicLink(),
    `Release directory must be a real directory, not a symlink: ${directory}`);
  const allowed = new Set([...RELEASE_FILES, CHECKSUM_FILE, USB_ARCHIVE]);
  const entries = await readdir(directory, { withFileTypes: true });
  for (const entry of entries) {
    requireCondition(allowed.has(entry.name) && entry.isFile() && !entry.isSymbolicLink(),
      `Unexpected or unsafe release payload entry: ${entry.name}`);
  }
  const names = new Set(entries.map((entry) => entry.name));
  if (requireFiles) {
    for (const filename of RELEASE_FILES) {
      requireCondition(names.has(filename), `Missing required release file: ${filename}`);
    }
  }
  return names;
}

function maximumFileBytes(filename) {
  if (filename === "ota-manifest.json" || filename === CHECKSUM_FILE) return MAX_MANIFEST_BYTES;
  if (filename === USB_ARCHIVE) return 16 * 1024 * 1024;
  if (filename.endsWith(".json")) return 64 * 1024;
  if (filename === "bootloader.bin") return 0x7000;
  if (filename === "partitions.bin") return 0xc00;
  if (filename === "boot_app0.bin") return 0x2000;
  return SLOT_BYTES;
}

function verifyChecksums(bytes, files) {
  const lines = bytes.toString("utf8").trimEnd().split(/\r?\n/);
  const seen = new Set();
  requireCondition(
    lines.length === files.size || (!files.has(USB_ARCHIVE) && lines.length === files.size + 1),
    "checksums.sha256 must list every payload exactly once.",
  );
  for (const line of lines) {
    const match = /^([a-f0-9]{64})  ([a-z0-9._-]+)$/.exec(line);
    requireCondition(match && (files.has(match[2]) || match[2] === USB_ARCHIVE) && !seen.has(match[2]),
      `checksums.sha256 contains an unsafe, unexpected, or duplicate entry: ${line}`);
    // Pages downloads the fixed payloads, but preserves the release's optional ZIP checksum.
    if (files.has(match[2])) {
      requireCondition(digest(files.get(match[2])) === match[1], `Checksum mismatch for ${match[2]}.`);
    }
    seen.add(match[2]);
  }
  requireCondition([...files.keys()].every((filename) => seen.has(filename)),
    "checksums.sha256 must list every payload exactly once.");
}

async function readReleaseDirectory(directory, expectedVersion, checkHashes) {
  const names = await payloadEntries(directory);
  const files = new Map(await Promise.all(
    [...RELEASE_FILES, ...(names.has(USB_ARCHIVE) ? [USB_ARCHIVE] : [])].map(async (filename) =>
      [filename, await readRegularFile(join(directory, filename), maximumFileBytes(filename))]),
  ));
  const manifest = parseJson(files.get("ota-manifest.json"), "ota-manifest.json");
  validateOtaManifest(manifest, expectedVersion);
  const release = parseJson(files.get("release.json"), "release.json");
  requireKeys(release, ["version", "notes", "ota", "manifests"], "release.json");
  requireCondition(release.version === manifest.version && release.ota === true,
    "release.json version/OTA capability does not match ota-manifest.json.");
  validateNotes(release.notes, manifest.version);
  requireKeys(release.manifests, PANELS, "release.json manifests");
  for (const panel of PANELS) {
    const filename = `manifest-${panel}.json`;
    requireCondition(release.manifests[panel] === filename, `release.json contains an unsafe manifest path for ${panel}.`);
    validateInstallerManifest(parseJson(files.get(filename), filename), panel, manifest.version);
    const variant = manifest.variants[panel];
    const bytes = files.get(variant.filename);
    requireCondition(bytes.length === variant.size, `Firmware size mismatch for ${panel}.`);
    requireCondition(digest(bytes) === variant.sha256, `Firmware SHA-256 mismatch for ${panel}.`);
    validateEsp32Image(bytes, variant.filename, manifest.version, panel);
  }
  validateEsp32Image(files.get("bootloader.bin"), "bootloader.bin");
  validatePartitions(files.get("partitions.bin"), "partitions.bin");
  validateBootApp(files.get("boot_app0.bin"));
  if (checkHashes) {
    requireCondition(names.has(CHECKSUM_FILE), `Missing required release file: ${CHECKSUM_FILE}`);
    verifyChecksums(await readRegularFile(join(directory, CHECKSUM_FILE), MAX_MANIFEST_BYTES), files);
  }
  return { manifest, release, files };
}

export async function validateReleaseDirectory(directory, expectedVersion) {
  const { manifest, release } = await readReleaseDirectory(resolve(directory), expectedVersion, true);
  return { manifest, release };
}

export async function writeChecksums(directory) {
  const { files } = await readReleaseDirectory(resolve(directory), undefined, false);
  const contents = [...files.keys()].sort().map((filename) => `${digest(files.get(filename))}  ${filename}\n`).join("");
  await writeFile(join(directory, CHECKSUM_FILE), contents, "utf8");
  return contents;
}

export async function createReleaseDirectory(directory, { version, notes, buildDirectories, bootAppPath }) {
  parseVersion(version);
  validateNotes(notes, version);
  requireKeys(buildDirectories, PANELS, "Build directories");
  const files = new Map();
  const manifest = {
    schemaVersion: 1,
    product: PRODUCT,
    chipFamily: "ESP32",
    layout: LAYOUT,
    version,
    releaseTag: `v${version}`,
    variants: {},
  };
  for (const panel of PANELS) {
    for (const filename of ["bootloader.bin", "partitions.bin"]) {
      const bytes = await readRegularFile(join(buildDirectories[panel], filename), maximumFileBytes(filename));
      if (files.has(filename)) {
        requireCondition(bytes.equals(files.get(filename)), `Builds have differing shared ${filename} binaries.`);
      } else {
        if (filename === "bootloader.bin") validateEsp32Image(bytes, filename);
        else validatePartitions(bytes, filename);
        files.set(filename, bytes);
      }
    }
    const filename = `firmware-${panel}.bin`;
    const bytes = await readRegularFile(join(buildDirectories[panel], "firmware.bin"), SLOT_BYTES);
    validateEsp32Image(bytes, filename, version, panel);
    files.set(filename, bytes);
    manifest.variants[panel] = { filename, size: bytes.length, sha256: digest(bytes) };
    files.set(`manifest-${panel}.json`, Buffer.from(`${JSON.stringify({
      name: `ESP32 GT7 Dashboard (${panel.toUpperCase()})`,
      version,
      new_install_prompt_erase: true,
      builds: [{ chipFamily: "ESP32", parts: installerParts(panel) }],
    }, null, 2)}\n`));
  }
  const bootApp = await readRegularFile(bootAppPath, 0x2000);
  validateBootApp(bootApp);
  files.set("boot_app0.bin", bootApp);
  files.set("ota-manifest.json", Buffer.from(`${JSON.stringify(manifest, null, 2)}\n`));
  files.set("release.json", Buffer.from(`${JSON.stringify({
    version,
    notes,
    ota: true,
    manifests: Object.fromEntries(PANELS.map((panel) => [panel, `manifest-${panel}.json`])),
  }, null, 2)}\n`));

  // Validate every build before replacing an existing package or installer binary.
  await mkdir(directory, { recursive: true });
  await payloadEntries(directory, false);
  await rm(join(directory, USB_ARCHIVE), { force: true });
  await rm(join(directory, CHECKSUM_FILE), { force: true });
  for (const [filename, bytes] of files) await writeFile(join(directory, filename), bytes);
  await writeChecksums(directory);
  return validateReleaseDirectory(directory, version);
}

async function main() {
  const [command, argument, expectedVersion, ...extra] = process.argv.slice(2);
  if (command === "verify-tag" && argument !== undefined && expectedVersion === undefined) {
    const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
    console.log(`Verified release tag v${await verifyTagVersion(root, argument)}.`);
  } else if (command === "validate" && argument !== undefined && extra.length === 0) {
    const { manifest } = await validateReleaseDirectory(argument, expectedVersion);
    console.log(`Validated OTA/USB release ${manifest.releaseTag}: ${resolve(argument)}`);
  } else if (command === "checksums" && argument !== undefined && expectedVersion === undefined) {
    await writeChecksums(argument);
    console.log(`Wrote ${join(argument, CHECKSUM_FILE)}.`);
  } else {
    throw new Error("Usage: firmware-release.mjs verify-tag <vx.y.z> | validate <directory> [x.y.z] | checksums <directory>");
  }
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main().catch((error) => {
    console.error(`firmware-release: ${error.message}`);
    process.exitCode = 1;
  });
}
