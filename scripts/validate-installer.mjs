#!/usr/bin/env node

import { createHash } from "node:crypto";
import { lstat, readFile, readdir, realpath } from "node:fs/promises";
import { basename, dirname, isAbsolute, join, relative, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";
import { parseVersion, RELEASE_FILES, validateReleaseDirectory } from "./firmware-release.mjs";

export const PANELS = ["ili9341", "st7789"];
const offsets = [4096, 32768, 57344, 65536];

function isContained(root, path) {
  const child = relative(root, path);
  return !isAbsolute(child) && child !== ".." && !child.startsWith(`..${sep}`);
}

export function installerPath(root, directory, value) {
  if (
    typeof value !== "string" ||
    !value.split("/").every((part) => /^[a-zA-Z0-9._-]+$/.test(part) && part !== "." && part !== "..")
  ) {
    throw new Error(`Unsafe installer path: ${JSON.stringify(value)}`);
  }
  const path = resolve(directory, value);
  if (!isContained(root, path)) {
    throw new Error(`Installer path escapes its directory: ${value}`);
  }
  return path;
}

export async function requireInstallerFile(root, path, maximumSize = Infinity) {
  const details = await lstat(path);
  if (!details.isFile() || details.size === 0 || details.size > maximumSize) {
    throw new Error(`Expected a non-empty regular installer file of at most ${maximumSize} bytes: ${path}`);
  }
  if (!isContained(await realpath(root), await realpath(path))) {
    throw new Error(`Installer file resolves outside its directory: ${path}`);
  }
  return details;
}

async function readJson(root, path, maximumSize = 64 * 1024) {
  await requireInstallerFile(root, path, maximumSize);
  return JSON.parse(await readFile(path, "utf8"));
}

export async function readReleaseIndex(root) {
  root = resolve(root);
  const index = await readJson(root, join(root, "releases.json"), 1024 * 1024);
  if (!Array.isArray(index.versions) || index.versions.length === 0 || index.versions.length > 10) {
    throw new Error("Installer releases.json must contain between 1 and 10 versions.");
  }
  const seen = new Set();
  for (const release of index.versions) {
    parseVersion(release?.version);
    if (seen.has(release.version)) throw new Error(`Duplicate installer version: ${release.version}`);
    seen.add(release.version);
    if (
      !Array.isArray(release.notes) ||
      release.notes.length === 0 ||
      release.notes.some((note) => typeof note !== "string" || !note.trim()) ||
      (release.ota !== undefined && typeof release.ota !== "boolean")
    ) {
      throw new Error(`Invalid installer notes or OTA capability for ${release.version}`);
    }
    if (Object.keys(release.manifests ?? {}).sort().join(",") !== PANELS.join(",")) {
      throw new Error(`Both display manifests are required for ${release.version}`);
    }
    for (const panel of PANELS) {
      const path = release.manifests[panel];
      installerPath(root, root, path);
      if (path !== `versions/${release.version}/manifest-${panel}.json`) {
        throw new Error(`Unexpected version manifest path: ${path}`);
      }
    }
  }
  return index;
}

async function validateWebManifest(root, path, version, panel) {
  const manifest = await readJson(root, path);
  if (
    manifest.version !== version ||
    typeof manifest.name !== "string" || !manifest.name.toUpperCase().includes(panel.toUpperCase()) ||
    !Array.isArray(manifest.builds) ||
    manifest.builds.length !== 1 ||
    manifest.builds[0]?.chipFamily !== "ESP32"
  ) {
    throw new Error(`Incorrect version or ESP32 build in ${path}`);
  }
  const parts = manifest.builds[0].parts;
  if (!Array.isArray(parts) || parts.length !== offsets.length) {
    throw new Error(`A complete four-part USB manifest is required: ${path}`);
  }
  const files = new Map();
  const names = ["bootloader.bin", "partitions.bin", "boot_app0.bin", `firmware-${panel}.bin`];
  for (const part of parts) {
    const index = offsets.indexOf(part?.offset);
    if (index === -1 || files.has(part.offset)) throw new Error(`Invalid flash offsets in ${path}`);
    const binary = installerPath(root, dirname(path), part.path);
    if (basename(binary) !== names[index]) throw new Error(`Incorrect binary for offset ${part.offset}: ${path}`);
    await requireInstallerFile(root, binary);
    files.set(part.offset, binary);
  }
  return files;
}

export async function validateModernDirectory(root, directory, version) {
  for (const name of [...RELEASE_FILES, "checksums.sha256"]) {
    await requireInstallerFile(root, join(directory, name));
  }
  return validateReleaseDirectory(directory, version);
}

async function digest(path) {
  return createHash("sha256").update(await readFile(path)).digest("hex");
}

export async function validateInstaller(directory) {
  const root = resolve(directory);
  await requireInstallerFile(root, join(root, "index.html"));
  const { versions } = await readReleaseIndex(root);
  const archived = new Map();
  let manifestCount = 0;
  for (const release of versions) {
    const binaries = {};
    for (const panel of PANELS) {
      const path = installerPath(root, root, release.manifests[panel]);
      binaries[panel] = await validateWebManifest(root, path, release.version, panel);
      manifestCount += 1;
    }
    if (release.ota === true) {
      const validated = await validateModernDirectory(root, join(root, "versions", release.version), release.version);
      if (JSON.stringify(release.notes) !== JSON.stringify(validated.release.notes)) {
        throw new Error(`Release notes differ from the published metadata for ${release.version}`);
      }
    }
    archived.set(release.version, binaries);
  }

  const latest = versions[0];
  const rootFiles = await readdir(root);
  const rootNames = rootFiles.filter((name) => /^manifest(?:-[a-z0-9-]+)?\.json$/i.test(name));
  for (const required of ["manifest.json", "manifest-st7789.json"]) {
    if (!rootNames.includes(required)) throw new Error(`Missing root installer alias: ${required}`);
  }
  for (const name of rootNames) {
    if (!["manifest.json", "manifest-ili9341.json", "manifest-st7789.json"].includes(name)) {
      throw new Error(`Unknown root installer manifest: ${name}`);
    }
    const panel = name === "manifest-st7789.json" ? "st7789" : "ili9341";
    const files = await validateWebManifest(root, join(root, name), latest.version, panel);
    for (const [offset, path] of files) {
      if (await digest(path) !== await digest(archived.get(latest.version)[panel].get(offset))) {
        throw new Error(`Root installer bytes differ from latest version ${latest.version}: ${name}`);
      }
    }
    manifestCount += 1;
  }
  if (rootFiles.includes("ota-manifest.json")) {
    if (latest.ota !== true) throw new Error("Legacy latest version cannot expose an OTA manifest.");
    for (const name of [...RELEASE_FILES, "checksums.sha256"]) {
      const path = join(root, name);
      await requireInstallerFile(root, path);
      if (await digest(path) !== await digest(join(root, "versions", latest.version, name))) {
        throw new Error(`Root release payload differs from latest version ${latest.version}: ${name}`);
      }
    }
  }
  if (rootFiles.includes("firmware")) {
    const firmwareNames = await readdir(join(root, "firmware"));
    for (const panel of PANELS) {
      for (const [offset, archivedPath] of archived.get(latest.version)[panel]) {
        const path = join(root, "firmware", basename(archivedPath));
        await requireInstallerFile(root, path);
        if (await digest(path) !== await digest(archivedPath)) {
          throw new Error(`Root firmware alias differs from latest version ${latest.version}: ${path}`);
        }
        if (rootFiles.includes("ota-manifest.json") && panel === "ili9341" && offset === 65536 && firmwareNames.includes("firmware.bin")) {
          const alias = join(root, "firmware", "firmware.bin");
          await requireInstallerFile(root, alias);
          if (await digest(alias) !== await digest(path)) throw new Error("The firmware.bin alias must match ILI9341.");
        }
      }
    }
  }
  return { version: latest.version, versionCount: versions.length, manifestCount };
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try {
    if (process.argv.length > 3) throw new Error("Usage: node scripts/validate-installer.mjs [installer-directory]");
    const result = await validateInstaller(process.argv[2] ?? "installer");
    console.log(`Validated installer ${result.version}, ${result.versionCount} versions and ${result.manifestCount} manifests.`);
  } catch (error) {
    console.error(`Installer validation failed: ${error.message}`);
    process.exitCode = 1;
  }
}
