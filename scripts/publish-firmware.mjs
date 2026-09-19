#!/usr/bin/env node

import { createHash } from "node:crypto";
import { copyFile, mkdir, readFile, rm, stat, writeFile } from "node:fs/promises";
import { homedir } from "node:os";
import { dirname, join, relative, resolve } from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import {
  createReleaseDirectory,
  parseVersion,
  readReleaseNotes,
  RELEASE_FILES,
  validateInstallerManifest,
  validateReleaseDirectory,
  verifyProjectVersion,
  versionHeaderContents,
} from "./firmware-release.mjs";

const scriptDirectory = dirname(fileURLToPath(import.meta.url));
const projectDirectory = resolve(scriptDirectory, "..");
const variants = [
  {
    id: "ili9341",
    environment: process.env.PLATFORMIO_ENV ?? "esp32",
    firmwareName: "firmware-ili9341.bin",
  },
  {
    id: "st7789",
    environment: process.env.PLATFORMIO_ST7789_ENV ?? "esp32-st7789",
    firmwareName: "firmware-st7789.bin",
  },
];
const firmwareDirectory = join(projectDirectory, "installer", "firmware");
const versionPath = join(projectDirectory, "VERSION");
const versionHeaderPath = join(projectDirectory, "include", "version.h");
const manifestPath = join(projectDirectory, "installer", "manifest.json");
const manifestPaths = [
  manifestPath,
  join(projectDirectory, "installer", "manifest-st7789.json"),
];
const releasesPath = join(projectDirectory, "installer", "releases.json");
const versionsDirectory = join(projectDirectory, "installer", "versions");
const releaseDirectory = join(projectDirectory, ".pio", "release");
const retainedReleaseCount = 10;

function displayPath(path) {
  return relative(projectDirectory, path).replaceAll("\\", "/");
}

async function fileContents(path) {
  try {
    return await readFile(path, "utf8");
  } catch (error) {
    if (error.code === "ENOENT") {
      return null;
    }
    throw error;
  }
}

async function writeIfChanged(path, contents, modifiedFiles) {
  if ((await fileContents(path)) === contents) {
    return;
  }

  await mkdir(dirname(path), { recursive: true });
  await writeFile(path, contents, "utf8");
  modifiedFiles.push(displayPath(path));
}

function parseArguments() {
  const argumentsList = process.argv.slice(2);
  const skipBuild = argumentsList.includes("--skip-build");
  const positional = argumentsList.filter((argument) => argument !== "--skip-build");

  if (positional.length > 1 || argumentsList.some((argument) => argument.startsWith("--") && argument !== "--skip-build")) {
    throw new Error("Usage: node scripts/publish-firmware.mjs [x.y.z] [--skip-build]");
  }

  return { suppliedVersion: positional[0], skipBuild };
}

async function resolveVersion(suppliedVersion) {
  const version = suppliedVersion ?? (await readFile(versionPath, "utf8")).trim();
  parseVersion(version);
  return version;
}

async function synchronizeVersion(version) {
  const modifiedFiles = [];

  await writeIfChanged(versionPath, `${version}\n`, modifiedFiles);
  await writeIfChanged(
    versionHeaderPath,
    versionHeaderContents(version),
    modifiedFiles,
  );

  for (const currentManifestPath of manifestPaths) {
    const manifest = JSON.parse(await readFile(currentManifestPath, "utf8"));
    manifest.version = version;
    await writeIfChanged(
      currentManifestPath,
      `${JSON.stringify(manifest, null, 2)}\n`,
      modifiedFiles,
    );
  }

  return modifiedFiles;
}

async function verifySynchronizedVersion(expectedVersion) {
  const version = await verifyProjectVersion(projectDirectory, expectedVersion);
  const manifests = [];
  for (const [index, currentManifestPath] of manifestPaths.entries()) {
    const manifest = JSON.parse(await readFile(currentManifestPath, "utf8"));
    validateInstallerManifest(manifest, variants[index].id, version, "firmware/");
    manifests.push({ path: currentManifestPath, manifest });
  }

  return manifests;
}

function platformioCandidates() {
  const candidates = [];

  if (process.env.PLATFORMIO_CMD) {
    candidates.push(process.env.PLATFORMIO_CMD);
  }

  candidates.push("pio", "platformio");

  if (process.platform === "win32") {
    candidates.push(
      join(homedir(), ".platformio", "penv", "Scripts", "pio.exe"),
      join(homedir(), ".platformio", "penv", "Scripts", "platformio.exe"),
    );
  } else {
    candidates.push(
      join(homedir(), ".platformio", "penv", "bin", "pio"),
      join(homedir(), ".platformio", "penv", "bin", "platformio"),
    );
  }

  return [...new Set(candidates)];
}

function runBuild(environment) {
  for (const command of platformioCandidates()) {
    const result = spawnSync(command, ["run", "-e", environment], {
      cwd: projectDirectory,
      encoding: "utf8",
      stdio: "inherit",
      shell: false,
    });

    if (result.error?.code === "ENOENT") {
      continue;
    }

    if (result.error) {
      throw result.error;
    }

    if (result.status !== 0) {
      throw new Error(`PlatformIO exited with status ${result.status}.`);
    }

    return command;
  }

  throw new Error(
    "PlatformIO was not found. Install PlatformIO, add pio to PATH, or set PLATFORMIO_CMD.",
  );
}

function frameworkBootAppPath() {
  const coreDirectory = process.env.PLATFORMIO_CORE_DIR
    ? resolve(process.env.PLATFORMIO_CORE_DIR)
    : join(homedir(), ".platformio");

  return join(
    coreDirectory,
    "packages",
    "framework-arduinoespressif32",
    "tools",
    "partitions",
    "boot_app0.bin",
  );
}

async function verifyNonEmpty(path) {
  const details = await stat(path);

  if (!details.isFile() || details.size === 0) {
    throw new Error(`Expected a non-empty binary file: ${path}`);
  }

  return details.size;
}

async function sha256(path) {
  const contents = await readFile(path);
  return createHash("sha256").update(contents).digest("hex");
}

async function verifyManifestBinaries(manifests) {
  for (const { path: currentManifestPath, manifest } of manifests) {
    const parts = manifest.builds?.flatMap((build) => build.parts ?? []) ?? [];

    if (parts.length === 0) {
      throw new Error(`${displayPath(currentManifestPath)} does not reference any binary files.`);
    }

    for (const part of parts) {
      const binaryPath = resolve(dirname(currentManifestPath), part.path);
      await verifyNonEmpty(binaryPath);
    }
  }
}

function printSummary({ version, skipBuild, copiedFiles, modifiedFiles }) {
  console.log("\nRelease summary");
  console.log(`  Version: ${version}`);
  console.log(`  PlatformIO environments: ${variants.map((variant) => variant.environment).join(", ")}`);
  console.log(`  Manifests: ${manifestPaths.map(displayPath).join(", ")}`);
  console.log(`  Build: ${skipBuild ? "skipped" : "successful"}`);
  console.log(`  Release package: ${skipBuild ? "not generated (--skip-build)" : displayPath(releaseDirectory)}`);
  if (skipBuild) {
    console.log("  Existing release assets were not refreshed or validated; build before publishing.");
  }
  console.log(`  Copied release files: ${copiedFiles.length ? copiedFiles.join(", ") : "none (--skip-build)"}`);
  console.log(`  Modified files: ${modifiedFiles.length ? modifiedFiles.join(", ") : "none"}`);
}

async function copyVerifiedBinary(source, destinationName, copiedFiles) {
  await verifyNonEmpty(source);
  const destination = join(firmwareDirectory, destinationName);
  await copyFile(source, destination);
  const size = await verifyNonEmpty(destination);
  const hash = await sha256(destination);
  if (hash !== await sha256(source)) {
    throw new Error(`Copied firmware SHA-256 mismatch: ${destinationName}`);
  }
  copiedFiles.push(displayPath(destination));
  console.log(`${destinationName}: ${size} bytes, SHA-256 ${hash}`);
}

async function archiveRelease(version, notes, copiedFiles, modifiedFiles) {
  const existingReleases = JSON.parse(
    (await fileContents(releasesPath)) ?? '{"versions":[]}',
  );
  if (!Array.isArray(existingReleases.versions)) {
    throw new Error("installer/releases.json must contain a versions array.");
  }
  for (const entry of existingReleases.versions) parseVersion(entry.version);

  const versionDirectory = join(versionsDirectory, version);
  await mkdir(versionDirectory, { recursive: true });
  await rm(join(versionDirectory, "usb-installer.zip"), { force: true });

  for (const filename of [...RELEASE_FILES, "checksums.sha256"]) {
    const source = join(releaseDirectory, filename);
    const destination = join(versionDirectory, filename);
    await copyFile(source, destination);
    await verifyNonEmpty(destination);
    copiedFiles.push(displayPath(destination));
  }
  await validateReleaseDirectory(versionDirectory, version);

  const archivedManifests = Object.fromEntries(variants.map((variant) =>
    [variant.id, `versions/${version}/manifest-${variant.id}.json`]));
  const releaseEntry = {
    version,
    notes,
    ota: true,
    manifests: archivedManifests,
  };
  const versions = [
    releaseEntry,
    ...(existingReleases.versions ?? []).filter((entry) => entry.version !== version),
  ].slice(0, retainedReleaseCount);

  await writeIfChanged(
    releasesPath,
    `${JSON.stringify({ versions }, null, 2)}\n`,
    modifiedFiles,
  );

  const retainedVersions = new Set(versions.map((entry) => entry.version));
  for (const previousRelease of existingReleases.versions ?? []) {
    if (!retainedVersions.has(previousRelease.version)) {
      await rm(join(versionsDirectory, previousRelease.version), {
        recursive: true,
        force: true,
      });
    }
  }
}

async function main() {
  const { suppliedVersion, skipBuild } = parseArguments();
  const version = await resolveVersion(suppliedVersion);
  const modifiedFiles = await synchronizeVersion(version);
  let manifests = await verifySynchronizedVersion(version);

  if (skipBuild) {
    printSummary({ version, skipBuild, copiedFiles: [], modifiedFiles });
    return;
  }

  const notes = await readReleaseNotes(projectDirectory, version);
  for (const variant of variants) {
    console.log(`Building ${variant.id} with PlatformIO environment: ${variant.environment}`);
    const command = runBuild(variant.environment);
    console.log(`Build completed with: ${command}`);
  }

  await verifySynchronizedVersion(version);
  await createReleaseDirectory(releaseDirectory, {
    version,
    notes,
    buildDirectories: Object.fromEntries(variants.map((variant) => [
      variant.id, join(projectDirectory, ".pio", "build", variant.environment),
    ])),
    bootAppPath: frameworkBootAppPath(),
  });
  await mkdir(firmwareDirectory, { recursive: true });

  const copiedFiles = [];
  console.log(`Copying firmware to: ${firmwareDirectory}`);

  for (const filename of [
    "bootloader.bin", "partitions.bin", "boot_app0.bin",
    ...variants.map((variant) => variant.firmwareName),
  ]) {
    await copyVerifiedBinary(
      join(releaseDirectory, filename),
      filename,
      copiedFiles,
    );
  }

  manifests = await verifySynchronizedVersion(version);
  await verifyManifestBinaries(manifests);
  await archiveRelease(version, notes, copiedFiles, modifiedFiles);
  printSummary({ version, skipBuild, copiedFiles, modifiedFiles });
}

main().catch((error) => {
  console.error(`publish-firmware: ${error.message}`);
  process.exitCode = 1;
});
