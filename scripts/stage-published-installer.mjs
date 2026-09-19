#!/usr/bin/env node

import { spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { copyFile, lstat, mkdir, mkdtemp, readFile, readdir, rm, writeFile } from "node:fs/promises";
import { dirname, isAbsolute, join, relative, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";
import { compareVersions, parseVersion, RELEASE_FILES, validateReleaseDirectory } from "./firmware-release.mjs";
import { PANELS, readReleaseIndex, validateInstaller } from "./validate-installer.mjs";

export const DEFAULT_REPOSITORY = "Kenblair1226/esp32-gt7-dashboard";
export const PUBLISHED_FILES = Object.freeze([...RELEASE_FILES, "checksums.sha256"]);
const binaryFiles = ["bootloader.bin", "partitions.bin", "boot_app0.bin", "firmware-ili9341.bin", "firmware-st7789.bin"];
const legacyFiles = [...binaryFiles, ...PANELS.map((panel) => `manifest-${panel}.json`)];
const managedFiles = new Set([...PUBLISHED_FILES, ...legacyFiles, "firmware.bin", "usb-installer.zip", "manifest.json", "releases.json", "firmware", "versions"]);
const maximumPages = 20;

function sha256(bytes) {
  return createHash("sha256").update(bytes).digest("hex");
}

function repositoryName(repository) {
  if (!/^[A-Za-z0-9][A-Za-z0-9-]{0,38}\/[A-Za-z0-9_.-]{1,100}$/.test(repository) ||
      [".", ".."].includes(repository.split("/")[1])) {
    throw new Error("Expected a GitHub owner/repository name.");
  }
  return repository;
}

export function createGitHubClient(repository = DEFAULT_REPOSITORY) {
  repository = repositoryName(repository);
  const prefix = `repos/${repository}`;
  function gh(argumentsList, input) {
    const result = spawnSync("gh", argumentsList, {
      input,
      maxBuffer: 32 * 1024 * 1024,
      timeout: 120_000,
      env: { ...process.env, GH_HOST: "github.com", GH_PROMPT_DISABLED: "1" },
    });
    if (result.error) throw result.error;
    if (result.status !== 0) {
      throw new Error(`GitHub request failed: ${result.stderr?.toString().trim() || `exit ${result.status}`}`);
    }
    return result.stdout;
  }
  function json(path, method = "GET", body) {
    const args = ["api", "--hostname", "github.com", `${prefix}/${path}`, "--method", method,
      "-H", "Accept: application/vnd.github+json", "-H", "X-GitHub-Api-Version: 2022-11-28"];
    if (body) args.push("--input", "-");
    return JSON.parse(gh(args, body ? JSON.stringify(body) : undefined).toString());
  }
  return {
    repository,
    listPage: async (page) => json(`releases?per_page=100&page=${page}`),
    latest: async () => json("releases/latest"),
    release: async (id) => json(`releases/${id}`),
    download: async (asset) => gh(["api", "--hostname", "github.com", `${prefix}/releases/assets/${asset.id}`,
      "-H", "Accept: application/octet-stream", "-H", "X-GitHub-Api-Version: 2022-11-28"]),
    async tagCommit(tag) {
      let object = json(`git/ref/tags/${encodeURIComponent(tag)}`).object;
      for (let depth = 0; depth < 5; depth += 1) {
        if (!/^[a-f0-9]{40}$/.test(object?.sha)) throw new Error("Invalid GitHub tag object.");
        if (object.type === "commit") return object.sha;
        if (object.type !== "tag") break;
        object = json(`git/tags/${object.sha}`).object;
      }
      throw new Error("Tag does not resolve to a commit within the annotated-tag limit.");
    },
    createDraft: async (body) => json("releases", "POST", body),
    upload: async (tag, path) => gh(["release", "upload", tag, path, "--repo", repository]),
  };
}

export async function listReleases(client) {
  const releases = [];
  for (let page = 1; page <= maximumPages; page += 1) {
    const batch = await client.listPage(page);
    if (!Array.isArray(batch) || batch.length > 100) throw new Error("Invalid GitHub releases response.");
    for (const release of batch) {
      if (typeof release?.draft !== "boolean" || typeof release?.prerelease !== "boolean") {
        throw new Error("GitHub release metadata is missing its channel flags.");
      }
    }
    releases.push(...batch);
    if (batch.length < 100) return releases;
  }
  throw new Error(`GitHub release pagination exceeded ${maximumPages} pages; refusing an incomplete history.`);
}

function stableVersion(release) {
  if (
    !Number.isSafeInteger(release?.id) || release.id <= 0 ||
    release.draft !== false || release.prerelease !== false ||
    !release.published_at || !Number.isFinite(Date.parse(release.published_at)) ||
    typeof release.tag_name !== "string" || !release.tag_name.startsWith("v")
  ) {
    throw new Error("Invalid published stable release metadata.");
  }
  const version = release.tag_name.slice(1);
  parseVersion(version);
  return version;
}

function assetSizeLimit(name) {
  if (name === "usb-installer.zip") return 16 * 1024 * 1024;
  if (name === "checksums.sha256") return 16 * 1024;
  if (name === "release.json") return 64 * 1024;
  if (name.endsWith(".json")) return 8192;
  if (name.startsWith("firmware-")) return 0x1E0000;
  return 1024 * 1024;
}

function assetMap(release, required = PUBLISHED_FILES) {
  if (!Array.isArray(release.assets)) throw new Error(`Missing assets for ${release.tag_name}`);
  const assets = new Map();
  const ids = new Set();
  for (const name of required) {
    const matches = release.assets.filter((asset) => asset.name === name);
    if (matches.length !== 1) throw new Error(`Missing or duplicate release asset ${name} in ${release.tag_name}`);
    const asset = matches[0];
    if (
      !Number.isSafeInteger(asset.id) || asset.id <= 0 || ids.has(asset.id) ||
      !Number.isSafeInteger(asset.size) || asset.size <= 0 || asset.size > assetSizeLimit(name) ||
      asset.state !== "uploaded" ||
      (asset.digest != null && !/^sha256:[a-f0-9]{64}$/.test(asset.digest))
    ) {
      throw new Error(`Invalid release asset metadata: ${name}`);
    }
    ids.add(asset.id);
    assets.set(name, asset);
  }
  return assets;
}

function releaseIdentity(release) {
  if (release === null) return null;
  stableVersion(release);
  return {
    id: release.id,
    tag: release.tag_name,
    publishedAt: release.published_at,
    assets: [...assetMap(release)].sort(([left], [right]) => left < right ? -1 : left > right ? 1 : 0).map(([name, asset]) => ({
      name, id: asset.id, size: asset.size, updatedAt: asset.updated_at ?? null, digest: asset.digest ?? null,
    })),
  };
}

export async function resolvePublishedReleases(client) {
  const stable = (await listReleases(client)).filter((release) => !release.draft && !release.prerelease);
  for (const release of stable) stableVersion(release);
  // Only a successful, complete listing with no stable release permits legacy bootstrap.
  if (stable.length === 0) return { latest: null, stable: [] };
  const latest = await client.latest();
  stableVersion(latest);
  if (!stable.some((release) => release.id === latest.id && release.tag_name === latest.tag_name)) {
    throw new Error("The current stable release changed during discovery; rerun this deployment.");
  }
  return { latest, stable: stable.map((release) => release.id === latest.id ? latest : release) };
}

export function mergeReleaseHistory(latest, published, legacy) {
  const choices = new Map();
  for (const entry of legacy) {
    parseVersion(entry.version);
    if (entry.ota !== true && !choices.has(entry.version)) {
      choices.set(entry.version, { version: entry.version, source: "legacy", release: entry });
    }
  }
  for (const release of published) {
    if (release.draft || release.prerelease) continue;
    const version = stableVersion(release);
    choices.set(version, { version, source: "published", release });
  }
  if (latest !== null) {
    const version = stableVersion(latest);
    choices.set(version, { version, source: "published", release: latest });
  }
  const currentVersion = latest === null ? null : stableVersion(latest);
  const sorted = [...choices.values()].sort((left, right) => {
    if (left.version === currentVersion) return -1;
    if (right.version === currentVersion) return 1;
    return compareVersions(right.version, left.version);
  });
  if (sorted.length === 0) throw new Error("No published stable firmware or committed legacy installer is available.");
  return sorted.slice(0, 10);
}

async function copyTree(source, destination, excluded = new Set()) {
  if (!(await lstat(source)).isDirectory()) throw new Error(`Expected a regular directory: ${source}`);
  await mkdir(destination, { recursive: true });
  for (const entry of await readdir(source, { withFileTypes: true })) {
    if (excluded.has(entry.name)) continue;
    const from = join(source, entry.name);
    const to = join(destination, entry.name);
    if (entry.isDirectory()) await copyTree(from, to);
    else if (entry.isFile()) await copyFile(from, to);
    else throw new Error(`Symlinks and special files are not allowed in installer staging: ${from}`);
  }
}

async function downloadAsset(client, asset) {
  const bytes = await client.download(asset);
  if (
    !Buffer.isBuffer(bytes) || bytes.length !== asset.size ||
    (asset.digest && asset.digest !== `sha256:${sha256(bytes)}`)
  ) {
    throw new Error(`Downloaded release asset has an incorrect size or SHA-256: ${asset.name}`);
  }
  return bytes;
}

function isWithin(root, path) {
  const part = relative(root, path);
  return !isAbsolute(part) && part !== ".." && !part.startsWith(`..${sep}`);
}

export async function stageInstaller(source, output, client = createGitHubClient()) {
  source = resolve(source);
  output = resolve(output);
  if (isWithin(source, output) || isWithin(output, source)) {
    throw new Error("Source installer and staged output must be separate directories.");
  }
  const { versions: legacy } = await readReleaseIndex(source);
  const snapshot = await resolvePublishedReleases(client);
  const selected = mergeReleaseHistory(snapshot.latest, snapshot.stable, legacy);
  const state = { schemaVersion: 1, repository: repositoryName(client.repository), latest: releaseIdentity(snapshot.latest) };
  await mkdir(dirname(output), { recursive: true });
  await mkdir(output); // Never delete or overwrite an existing caller-owned directory.
  let scratch;
  try {
    scratch = await mkdtemp(join(dirname(output), ".installer-assets-"));
    await copyTree(source, output, managedFiles);
    const versions = [];
    for (const choice of selected) {
      const destination = join(output, "versions", choice.version);
      if (choice.source === "legacy") {
        await copyTree(join(source, "versions", choice.version), destination);
        versions.push(choice.release);
        continue;
      }
      const downloaded = join(scratch, choice.version);
      await mkdir(downloaded);
      for (const [name, asset] of assetMap(choice.release)) {
        await writeFile(join(downloaded, name), await downloadAsset(client, asset), { flag: "wx" });
      }
      const { release } = await validateReleaseDirectory(downloaded, choice.version);
      await copyTree(downloaded, destination);
      versions.push({
        version: choice.version, notes: release.notes, ota: true,
        manifests: Object.fromEntries(PANELS.map((panel) => [panel, `versions/${choice.version}/manifest-${panel}.json`])),
      });
    }
    const latest = versions[0];
    const archived = join(output, "versions", latest.version);
    for (const name of latest.ota === true ? PUBLISHED_FILES : legacyFiles) {
      await copyFile(join(archived, name), join(output, name));
    }
    await copyFile(join(output, "manifest-ili9341.json"), join(output, "manifest.json"));
    await mkdir(join(output, "firmware"));
    for (const name of binaryFiles) {
      await copyFile(join(output, name), join(output, "firmware", name));
    }
    await copyFile(join(output, "firmware-ili9341.bin"), join(output, "firmware", "firmware.bin"));
    await writeFile(join(output, "releases.json"), `${JSON.stringify({ versions }, null, 2)}\n`);
    const result = await validateInstaller(output);
    return { ...result, state };
  } catch (error) {
    await rm(output, { recursive: true, force: true });
    throw error;
  } finally {
    if (scratch) await rm(scratch, { recursive: true, force: true });
  }
}

export async function assertCurrentRelease(state, client = createGitHubClient(state?.repository)) {
  if (state?.schemaVersion !== 1 || repositoryName(state.repository) !== client.repository || !Object.hasOwn(state, "latest")) {
    throw new Error("Invalid staged release identity.");
  }
  const { latest } = await resolvePublishedReleases(client);
  if (JSON.stringify(state.latest) !== JSON.stringify(releaseIdentity(latest))) {
    throw new Error("The current stable release or its assets changed after staging; rerun instead of deploying stale firmware.");
  }
}

export function assertDraftRelease(release, tag, commit) {
  if (
    !Number.isSafeInteger(release?.id) || release.id <= 0 ||
    release.tag_name !== tag || release.draft !== true || release.prerelease !== false || release.published_at ||
    !Array.isArray(release.assets)
  ) {
    throw new Error(`Refusing to modify ${tag}: it is not an unpublished stable draft.`);
  }
  if (release.target_commitish !== commit) throw new Error(`Draft ${tag} was created for a different commit.`);
}

export async function publishDraft(directory, tag, commit, client = createGitHubClient()) {
  const version = tag?.startsWith("v") ? tag.slice(1) : "";
  parseVersion(version);
  if (!/^[a-f0-9]{40}$/.test(commit)) throw new Error("Expected the full tagged commit SHA.");
  const { release: metadata } = await validateReleaseDirectory(directory, version);
  const files = [...PUBLISHED_FILES, "usb-installer.zip"];
  const contents = new Map();
  for (const name of files) {
    const path = join(directory, name);
    const details = await lstat(path);
    if (!details.isFile() || details.size === 0 || details.size > assetSizeLimit(name)) {
      throw new Error(`Invalid draft asset: ${name}`);
    }
    contents.set(name, await readFile(path));
  }
  if (await client.tagCommit(tag) !== commit) throw new Error("The remote tag no longer points at the built commit.");
  const matches = (await listReleases(client)).filter((release) => release.tag_name === tag);
  if (matches.length > 1) throw new Error(`Ambiguous release tag: ${tag}`);
  let release = matches[0];
  if (release) assertDraftRelease(release, tag, commit);
  else {
    release = await client.createDraft({
      tag_name: tag, target_commitish: commit, name: tag, draft: true, prerelease: false,
      body: `${metadata.notes.map((note) => `- ${note}`).join("\n")}\n\nHardware acceptance required for both panels using these exact assets. Publish this draft manually only after testing; CI does not publish it.\n`,
    });
    assertDraftRelease(release, tag, commit);
  }
  async function checkExistingAsset(name, bytes) {
    if (release.assets.some((asset) => asset.name === name)) {
      const asset = assetMap(release, [name]).get(name);
      if (!(await downloadAsset(client, asset)).equals(bytes)) {
        throw new Error(`Draft asset ${name} differs from this build. No assets were overwritten; resolve the draft explicitly.`);
      }
      return true;
    }
    return false;
  }
  release = await client.release(release.id);
  assertDraftRelease(release, tag, commit);
  const missing = [];
  for (const [name, bytes] of contents) {
    if (!(await checkExistingAsset(name, bytes))) missing.push(name);
  }
  for (const name of missing) {
    release = await client.release(release.id);
    assertDraftRelease(release, tag, commit);
    if (!(await checkExistingAsset(name, contents.get(name)))) {
      // No --clobber: even publication racing this check cannot overwrite accepted bytes.
      await client.upload(tag, join(directory, name));
    }
  }
  release = await client.release(release.id);
  assertDraftRelease(release, tag, commit);
  for (const [name, asset] of assetMap(release, files)) {
    const bytes = contents.get(name);
    if (asset.size !== bytes.length || (asset.digest && asset.digest !== `sha256:${sha256(bytes)}`)) {
      throw new Error(`Draft asset changed during publication: ${name}`);
    }
  }
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try {
    const [command, ...args] = process.argv.slice(2);
    const client = createGitHubClient(process.env.GH_REPO ?? DEFAULT_REPOSITORY);
    if (command === "stage" && args.length === 3) {
      const [source, output, statePath] = args.map((path) => resolve(path));
      if (isWithin(source, statePath) || isWithin(output, statePath)) {
        throw new Error("The release identity file must be outside the source and deployed installer.");
      }
      const result = await stageInstaller(source, output, client);
      await mkdir(dirname(statePath), { recursive: true });
      await writeFile(statePath, `${JSON.stringify(result.state, null, 2)}\n`, { flag: "wx" });
      console.log(`Staged ${result.versionCount} versions; current ${result.version} (${result.state.latest ? "published" : "legacy bootstrap"}).`);
    } else if (command === "check-current" && args.length === 1) {
      await assertCurrentRelease(JSON.parse(await readFile(args[0], "utf8")), client);
      console.log("Current stable release identity is unchanged.");
    } else if (command === "draft" && args.length === 3) {
      await publishDraft(resolve(args[0]), args[1], args[2], client);
      console.log(`Draft ${args[1]} is ready for hardware acceptance. No release was published.`);
    } else {
      throw new Error("Usage: node scripts/stage-published-installer.mjs stage <source> <output> <state> | check-current <state> | draft <release-directory> <tag> <commit>");
    }
  } catch (error) {
    console.error(`Firmware release synchronization failed: ${error.message}`);
    process.exitCode = 1;
  }
}
