import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { copyFile, lstat, mkdir, mkdtemp, readFile, readdir, rm, rmdir, symlink, writeFile } from "node:fs/promises";
import { basename, dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import test, { after } from "node:test";
import { RELEASE_FILES, writeChecksums } from "../firmware-release.mjs";
import {
  assertCurrentRelease, assertDraftRelease, DEFAULT_REPOSITORY, listReleases, mergeReleaseHistory,
  PUBLISHED_FILES, publishDraft, resolvePublishedReleases, stageInstaller,
} from "../stage-published-installer.mjs";
import { installerPath, validateInstaller } from "../validate-installer.mjs";

const project = resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");
const fixtureParent = join(project, ".pio", "published-installer-tests");
const panels = ["ili9341", "st7789"];
const binaries = ["bootloader.bin", "partitions.bin", "boot_app0.bin", "firmware-ili9341.bin", "firmware-st7789.bin"];
const commit = "a".repeat(40);
const date = "2026-09-01T00:00:00Z";
const hash = (bytes, algorithm = "sha256") => createHash(algorithm).update(bytes).digest("hex");

async function fixture(t) {
  await mkdir(fixtureParent, { recursive: true });
  const root = await mkdtemp(join(fixtureParent, "case-"));
  t.after(() => rm(root, { recursive: true, force: true }));
  return root;
}

after(async () => {
  try {
    await rmdir(fixtureParent);
  } catch (error) {
    if (!["ENOENT", "ENOTEMPTY", "EEXIST"].includes(error.code)) throw error;
  }
});

async function json(path, value) {
  await writeFile(path, `${JSON.stringify(value, null, 2)}\n`);
}

function manifest(version, panel, prefix = "") {
  return {
    name: `ESP32 GT7 Dashboard (${panel.toUpperCase()})`, version, new_install_prompt_erase: true,
    builds: [{
      chipFamily: "ESP32",
      parts: ["bootloader.bin", "partitions.bin", "boot_app0.bin", `firmware-${panel}.bin`].map((name, index) => ({
        path: `${prefix}${name}`, offset: [4096, 32768, 57344, 65536][index],
      })),
    }],
  };
}

function legacyEntry(version) {
  return {
    version, notes: [`Legacy ${version}`, "保留既有版本說明。"],
    manifests: Object.fromEntries(panels.map((panel) => [panel, `versions/${version}/manifest-${panel}.json`])),
  };
}

async function legacyInstaller(root, versions = ["1.8.0", "1.7.3"]) {
  const source = join(root, "installer");
  await mkdir(source);
  await writeFile(join(source, "index.html"), "<html lang=\"en\">Current main frontend / 中文</html>");
  await writeFile(join(source, "image.png"), "Existing frontend image");
  await json(join(source, "release-notes.json"), { notes: "Keep frontend source notes unchanged" });
  for (const version of versions) {
    const directory = join(source, "versions", version);
    await mkdir(directory, { recursive: true });
    for (const name of binaries) await writeFile(join(directory, name), `legacy:${version}:${name}`);
    for (const panel of panels) await json(join(directory, `manifest-${panel}.json`), manifest(version, panel));
  }
  await mkdir(join(source, "firmware"));
  for (const name of binaries) {
    await copyFile(join(source, "versions", versions[0], name), join(source, "firmware", name));
  }
  await copyFile(join(source, "firmware", "firmware-ili9341.bin"), join(source, "firmware", "firmware.bin"));
  await json(join(source, "manifest.json"), manifest(versions[0], "ili9341", "firmware/"));
  await json(join(source, "manifest-st7789.json"), manifest(versions[0], "st7789", "firmware/"));
  await json(join(source, "releases.json"), { versions: versions.map(legacyEntry) });
  return source;
}

function image(text) {
  const body = Buffer.from(`${text}\0`);
  const segmentSize = Math.ceil(body.length / 4) * 4;
  const position = 32 + segmentSize;
  const imageEnd = Math.floor(position / 16) * 16 + 16;
  const bytes = Buffer.alloc(imageEnd);
  bytes[0] = 0xe9;
  bytes[1] = 1;
  bytes[3] = 0x20;
  bytes.writeUInt32LE(0x3ffb0000, 24);
  bytes.writeUInt32LE(segmentSize, 28);
  body.copy(bytes, 32);
  let checksum = 0xef;
  for (const byte of bytes.subarray(32, position)) checksum ^= byte;
  bytes[imageEnd - 1] = checksum;
  return bytes;
}

function partitions() {
  const entries = [
    ["nvs", 1, 2, 0x9000, 0x5000], ["otadata", 1, 0, 0xe000, 0x2000],
    ["app0", 0, 0x10, 0x10000, 0x1e0000], ["app1", 0, 0x11, 0x1f0000, 0x1e0000],
    ["spiffs", 1, 0x82, 0x3d0000, 0x20000], ["coredump", 1, 3, 0x3f0000, 0x10000],
  ];
  const bytes = Buffer.alloc(0xc00, 0xff);
  for (const [index, [name, type, subtype, offset, size]] of entries.entries()) {
    const entry = bytes.subarray(index * 32, (index + 1) * 32);
    entry.fill(0);
    entry.writeUInt16LE(0x50aa);
    entry[2] = type;
    entry[3] = subtype;
    entry.writeUInt32LE(offset, 4);
    entry.writeUInt32LE(size, 8);
    entry.write(name, 12);
  }
  const end = entries.length * 32;
  bytes.writeUInt16LE(0xebeb, end);
  Buffer.from(hash(bytes.subarray(0, end), "md5"), "hex").copy(bytes, end + 16);
  return bytes;
}

async function modernDirectory(root, version, withZip = false) {
  const directory = join(root, `release-${version}`);
  await mkdir(directory);
  const ota = {
    schemaVersion: 1, product: "esp32-gt7-dashboard", chipFamily: "ESP32",
    layout: "esp32-4mb-min-spiffs-v1", version, releaseTag: `v${version}`, variants: {},
  };
  for (const panel of panels) {
    const name = `firmware-${panel}.bin`;
    const bytes = image(`GT7DASH-OTA:1|esp32-gt7-dashboard|${version}|${panel}|esp32-4mb-min-spiffs-v1|END`);
    ota.variants[panel] = { filename: name, size: bytes.length, sha256: hash(bytes) };
    await writeFile(join(directory, name), bytes);
    await json(join(directory, `manifest-${panel}.json`), manifest(version, panel));
  }
  await writeFile(join(directory, "bootloader.bin"), image("fixture bootloader"));
  await writeFile(join(directory, "partitions.bin"), partitions());
  const bootApp = Buffer.alloc(0x2000, 0xff);
  bootApp.writeUInt32LE(1, 0);
  bootApp.writeUInt32LE(0x4743989a, 28);
  bootApp.writeUInt32LE(0, 0x1000);
  await writeFile(join(directory, "boot_app0.bin"), bootApp);
  await json(join(directory, "ota-manifest.json"), ota);
  await json(join(directory, "release.json"), {
    version, notes: [`Published ${version}`, "已通過硬體驗收。"], ota: true,
    manifests: { ili9341: "manifest-ili9341.json", st7789: "manifest-st7789.json" },
  });
  if (withZip) await writeFile(join(directory, "usb-installer.zip"), "Fixture bytes for draft-asset immutability tests");
  await writeChecksums(directory);
  return directory;
}

function metadata(version, id = 1, overrides = {}) {
  return {
    id, tag_name: `v${version}`, draft: false, prerelease: false, published_at: date,
    target_commitish: commit, assets: [], ...overrides,
  };
}

async function remoteRelease(root, version, id = 1, withZip = false) {
  const directory = await modernDirectory(root, version, withZip);
  const release = metadata(version, id);
  for (const [index, name] of [...PUBLISHED_FILES, ...(withZip ? ["usb-installer.zip"] : [])].entries()) {
    const bytes = await readFile(join(directory, name));
    release.assets.push({
      id: id * 100 + index + 1, name, size: bytes.length, state: "uploaded", updated_at: date,
      digest: `sha256:${hash(bytes)}`, bytes,
    });
  }
  return { directory, release };
}

function fakeClient(releases = [], latest = releases.find((release) => !release.draft && !release.prerelease)) {
  const calls = [];
  const client = {
    repository: DEFAULT_REPOSITORY, releases, current: latest, calls,
    async listPage(page) { calls.push(["list", page]); return this.releases.slice((page - 1) * 100, page * 100); },
    async latest() { calls.push(["latest"]); return this.current; },
    async download(asset) { calls.push(["download", asset.name, asset.id]); return Buffer.from(asset.bytes); },
    async tagCommit() { return commit; },
    async release(id) { calls.push(["release", id]); return this.releases.find((release) => release.id === id); },
    async createDraft(body) {
      calls.push(["create", body]);
      const release = { ...body, id: 9999, assets: [], published_at: null };
      this.releases.push(release);
      return release;
    },
    async upload(tag, path) {
      calls.push(["upload", basename(path)]);
      const release = this.releases.find((entry) => entry.tag_name === tag);
      const bytes = await readFile(path);
      release.assets.push({
        id: 999900 + release.assets.length, name: basename(path), size: bytes.length,
        state: "uploaded", digest: `sha256:${hash(bytes)}`, bytes,
      });
    },
  };
  return client;
}

async function treeHashes(directory, prefix = "") {
  const hashes = {};
  for (const entry of await readdir(directory, { withFileTypes: true })) {
    const key = `${prefix}${entry.name}`;
    if (entry.isDirectory()) Object.assign(hashes, await treeHashes(join(directory, entry.name), `${key}/`));
    else hashes[key] = hash(await readFile(join(directory, entry.name)));
  }
  return hashes;
}

test("successful empty release discovery bootstraps legacy without altering source bytes", async (t) => {
  const root = await fixture(t);
  const source = await legacyInstaller(root);
  const before = await treeHashes(source);
  const client = fakeClient();
  const output = join(root, "staged");
  const result = await stageInstaller(source, output, client);
  assert.equal(result.version, "1.8.0");
  assert.equal(result.state.latest, null);
  assert.deepEqual(client.calls, [["list", 1]]);
  assert.deepEqual(await treeHashes(source), before);
  for (const name of ["index.html", "image.png", "release-notes.json"]) {
    assert.deepEqual(await readFile(join(output, name)), await readFile(join(source, name)));
  }
  assert.deepEqual(await readFile(join(output, "versions", "1.8.0", "partitions.bin")),
    await readFile(join(source, "versions", "1.8.0", "partitions.bin")));
  await assertCurrentRelease(result.state, client);
});

test("transport, authentication and latest lookup errors never fall back to legacy", async (t) => {
  const root = await fixture(t);
  const source = await legacyInstaller(root);
  for (const message of ["network timeout", "HTTP 401", "HTTP 404", "HTTP 500"]) {
    const client = fakeClient();
    client.listPage = async () => { throw new Error(message); };
    await assert.rejects(stageInstaller(source, join(root, message.replaceAll(" ", "-")), client), new RegExp(message));
  }
  const client = fakeClient([metadata("2.0.0")]);
  client.latest = async () => { throw new Error("latest HTTP 404"); };
  await assert.rejects(stageInstaller(source, join(root, "missing-latest"), client), /latest HTTP 404/);
  assert.equal((await readdir(root)).some((name) => name.startsWith(".installer-assets-")), false);
});

test("drafts and prereleases do not enter the installer or disable legitimate bootstrap", async (t) => {
  const root = await fixture(t);
  const source = await legacyInstaller(root);
  const client = fakeClient([
    metadata("99.0.0", 1, { draft: true, published_at: null }),
    metadata("3.0.0-rc.1", 2, { prerelease: true }),
  ]);
  const result = await stageInstaller(source, join(root, "staged"), client);
  assert.equal(result.state.latest, null);
  assert.equal(result.versionCount, 2);
  assert.deepEqual(client.calls, [["list", 1]]);
});

test("release history is bounded, deduplicated, numerically ordered and latest-first", () => {
  const latest = metadata("2.0.0", 1);
  const published = [latest, metadata("2.1.0", 2), metadata("1.10.0", 3), metadata("99.0.0", 4, { draft: true })];
  const legacy = [...Array.from({ length: 10 }, (_, index) => legacyEntry(`1.${index}.0`)), legacyEntry("1.10.0"),
    { ...legacyEntry("100.0.0"), ota: true }];
  const selected = mergeReleaseHistory(latest, published, legacy);
  assert.equal(selected.length, 10);
  assert.deepEqual(selected.slice(0, 5).map((choice) => choice.version), ["2.0.0", "2.1.0", "1.10.0", "1.9.0", "1.8.0"]);
  assert.equal(selected.find((choice) => choice.version === "1.10.0").source, "published");
  assert.equal(new Set(selected.map((choice) => choice.version)).size, 10);
  assert.equal(selected.some((choice) => ["99.0.0", "100.0.0"].includes(choice.version)), false);
  assert.throws(() => mergeReleaseHistory(null, [], [{ ...legacyEntry("2.0.0"), ota: true }]), /No published stable/);
});

test("pagination is bounded and malformed release metadata is not interpreted as an empty channel", async () => {
  let calls = 0;
  await assert.rejects(listReleases({
    async listPage() { calls += 1; return Array.from({ length: 100 }, () => metadata("2.0.0", 1, { draft: true })); },
  }), /pagination exceeded/);
  assert.equal(calls, 20);
  await assert.rejects(resolvePublishedReleases(fakeClient([{ id: 1 }])), /channel flags/);
  await assert.rejects(resolvePublishedReleases(fakeClient([metadata("02.0.0")])), /Invalid version/);
  const client = fakeClient([metadata("2.0.0", 1)], metadata("2.1.0", 2));
  await assert.rejects(resolvePublishedReleases(client), /changed during discovery/);
});

test("published history is reconstructed from immutable assets and current main frontend is retained", async (t) => {
  const root = await fixture(t);
  const source = await legacyInstaller(root);
  const before = await treeHashes(source);
  const current = await remoteRelease(root, "2.1.0", 21, true);
  const previous = await remoteRelease(root, "2.0.0", 20, true);
  const client = fakeClient([previous.release, current.release], current.release);
  const output = join(root, "staged");
  const result = await stageInstaller(source, output, client);
  assert.equal(result.version, "2.1.0");
  assert.deepEqual(await treeHashes(source), before);
  const index = JSON.parse(await readFile(join(output, "releases.json"), "utf8"));
  assert.deepEqual(index.versions.map((release) => release.version), ["2.1.0", "2.0.0", "1.8.0", "1.7.3"]);
  assert.equal(index.versions[0].ota, true);
  assert.equal(index.versions[2].ota, undefined);
  assert.deepEqual(index.versions[0].notes, ["Published 2.1.0", "已通過硬體驗收。"]);
  for (const name of PUBLISHED_FILES) {
    assert.deepEqual(await readFile(join(output, name)), await readFile(join(current.directory, name)));
    assert.deepEqual(await readFile(join(output, "versions", "2.0.0", name)), await readFile(join(previous.directory, name)));
  }
  assert.deepEqual(await readFile(join(output, "manifest.json")), await readFile(join(current.directory, "manifest-ili9341.json")));
  for (const name of binaries) {
    assert.deepEqual(await readFile(join(output, "firmware", name)), await readFile(join(current.directory, name)));
  }
  assert.deepEqual(await readFile(join(output, "firmware", "firmware.bin")),
    await readFile(join(current.directory, "firmware-ili9341.bin")));
  assert.deepEqual(await readFile(join(output, "index.html")), await readFile(join(source, "index.html")));
  assert.equal(client.calls.filter(([operation]) => operation === "download").length, PUBLISHED_FILES.length * 2);
  assert.equal(client.calls.some((call) => call[1] === "usb-installer.zip"), false);
  await assertCurrentRelease(result.state, client);
});

test("missing, corrupt and incompatible published payloads are hard failures with targeted cleanup", async (t) => {
  const root = await fixture(t);
  const source = await legacyInstaller(root);
  for (const [index, failure] of ["missing", "duplicate", "truncated", "hash", "layout", "version"].entries()) {
    const version = `2.0.${index}`;
    const { release } = await remoteRelease(root, version, index + 1);
    if (failure === "missing") release.assets = release.assets.filter((asset) => asset.name !== "partitions.bin");
    if (failure === "duplicate") release.assets.push(release.assets[0]);
    if (failure === "truncated") release.assets[1].bytes = Buffer.from("truncated");
    if (failure === "hash") release.assets[1].bytes[32] ^= 1;
    if (failure === "layout" || failure === "version") {
      const asset = release.assets.find((entry) => entry.name === "ota-manifest.json");
      const ota = JSON.parse(asset.bytes.toString());
      if (failure === "layout") ota.layout = "legacy-huge-app";
      else ota.version = "999.0.0";
      asset.bytes = Buffer.from(JSON.stringify(ota));
      asset.size = asset.bytes.length;
      asset.digest = `sha256:${hash(asset.bytes)}`;
    }
    const output = join(root, `staged-${failure}`);
    await assert.rejects(stageInstaller(source, output, fakeClient([release])), /asset|layout|version/i);
    await assert.rejects(lstat(output), { code: "ENOENT" });
  }
  assert.equal((await readdir(root)).some((name) => name.startsWith(".installer-assets-")), false);
  assert.equal((await readFile(join(source, "index.html"), "utf8")).includes("Current main"), true);
});

test("a corrupt retained historical release is not silently dropped or replaced by a source archive", async (t) => {
  const root = await fixture(t);
  const source = await legacyInstaller(root, ["2.0.0", "1.8.0"]);
  const current = await remoteRelease(root, "2.1.0", 2);
  const previous = await remoteRelease(root, "2.0.0", 1);
  previous.release.assets = previous.release.assets.filter((asset) => asset.name !== "bootloader.bin");
  const output = join(root, "staged");
  await assert.rejects(stageInstaller(source, output, fakeClient([current.release, previous.release])), /Missing.*bootloader.bin/);
  await assert.rejects(lstat(output), { code: "ENOENT" });
  assert.equal((await readdir(root)).some((name) => name.startsWith(".installer-assets-")), false);
});

test("staging rejects overlapping or existing output instead of cleaning caller-owned paths", async (t) => {
  const root = await fixture(t);
  const source = await legacyInstaller(root);
  const before = await treeHashes(source);
  for (const output of [source, root, join(source, "staged")]) {
    await assert.rejects(stageInstaller(source, output, fakeClient()), /separate directories/);
  }
  const output = join(root, "existing");
  await mkdir(output);
  await writeFile(join(output, "keep.txt"), "Keep me");
  await assert.rejects(stageInstaller(source, output, fakeClient()), { code: "EEXIST" });
  assert.equal(await readFile(join(output, "keep.txt"), "utf8"), "Keep me");
  assert.deepEqual(await treeHashes(source), before);
});

test("release-index manifest paths cannot escape, use URLs, or target another version", async (t) => {
  const root = await fixture(t);
  const source = await legacyInstaller(root);
  for (const path of ["../outside.json", "/outside.json", "C:/outside.json", "C:\\outside.json",
    "\\\\server\\share\\outside.json", "https://example.com/file.json", "versions/%2e%2e/outside.json",
    "versions/1.8.0/../../outside.json", "versions/1.7.3/manifest-ili9341.json"]) {
    const entry = legacyEntry("1.8.0");
    entry.manifests.ili9341 = path;
    await json(join(source, "releases.json"), { versions: [entry] });
    await assert.rejects(validateInstaller(source), /path/i);
    await assert.rejects(stageInstaller(source, join(root, "staged"), fakeClient()), /path/i);
  }
  for (const path of ["", "../bad", "..\\bad", "https://example.com/file.bin", "file.bin?x=1"]) {
    assert.throws(() => installerPath(source, source, path), /path/i);
  }
});

test("USB manifests require complete fixed offsets, safe binary paths and matching root bytes", async (t) => {
  const root = await fixture(t);
  const source = await legacyInstaller(root);
  for (const change of [
    (value) => value.builds[0].parts.pop(),
    (value) => { value.builds[0].parts[0].offset = 0; },
    (value) => { value.builds[0].parts[0].path = "../bootloader.bin"; },
    (value) => { value.builds[0].parts[3].path = "firmware/firmware-st7789.bin"; },
    (value) => { value.version = "1.7.3"; },
  ]) {
    const value = manifest("1.8.0", "ili9341", "firmware/");
    change(value);
    await json(join(source, "manifest.json"), value);
    await assert.rejects(validateInstaller(source), /manifest|path|binary|offset|version/i);
  }
  await json(join(source, "manifest.json"), manifest("1.8.0", "ili9341", "firmware/"));
  await writeFile(join(source, "firmware", "partitions.bin"), "stale layout");
  await assert.rejects(validateInstaller(source), /Root installer bytes differ/);
});

test("symlinked version directories cannot bypass path containment", async (t) => {
  const root = await fixture(t);
  const source = await legacyInstaller(root, ["1.8.0"]);
  const outside = join(root, "outside");
  await mkdir(outside);
  await json(join(outside, "manifest-ili9341.json"), manifest("1.8.0", "ili9341"));
  await rm(join(source, "versions", "1.8.0"), { recursive: true });
  try {
    await symlink(outside, join(source, "versions", "1.8.0"), process.platform === "win32" ? "junction" : "dir");
  } catch (error) {
    if (["EPERM", "EACCES", "ENOTSUP"].includes(error.code) || (process.platform === "win32" && error.code === "EISDIR")) {
      return t.skip("Filesystem does not permit test symlinks.");
    }
    throw error;
  }
  await assert.rejects(validateInstaller(source), /outside its directory/);
  await assert.rejects(stageInstaller(source, join(root, "staged"), fakeClient()), /regular directory/);
});

test("the deployment identity guard rejects new releases, asset replacements and disappearing stable releases", async (t) => {
  const root = await fixture(t);
  const source = await legacyInstaller(root);
  const initial = await stageInstaller(source, join(root, "bootstrap"), fakeClient());
  const old = await remoteRelease(root, "2.0.0", 1);
  const newer = await remoteRelease(root, "2.1.0", 2);
  await assert.rejects(assertCurrentRelease(initial.state, fakeClient([old.release])), /changed after staging/);
  const client = fakeClient([old.release]);
  const staged = await stageInstaller(source, join(root, "published"), client);
  await assert.rejects(assertCurrentRelease(staged.state, fakeClient()), /changed after staging/);
  client.releases.push(newer.release);
  client.current = newer.release;
  await assert.rejects(assertCurrentRelease(staged.state, client), /changed after staging/);
  client.current = old.release;
  old.release.assets[0].id += 5000;
  await assert.rejects(assertCurrentRelease(staged.state, client), /changed after staging/);
  await assert.rejects(assertCurrentRelease({ ...staged.state, repository: "other/repo" }, client), /identity/);
});

test("modern root payloads must be identical to the validated current archive", async (t) => {
  const root = await fixture(t);
  const source = await legacyInstaller(root);
  const { release } = await remoteRelease(root, "2.0.0");
  const output = join(root, "staged");
  await stageInstaller(source, output, fakeClient([release]));
  await writeFile(join(output, "ota-manifest.json"), "{}");
  await assert.rejects(validateInstaller(output), /Root release payload differs/);
});

test("the local publisher's firmware-prefixed root validates without rewriting unused legacy aliases", async (t) => {
  const root = await fixture(t);
  const source = await legacyInstaller(root, ["1.8.0"]);
  const directory = await modernDirectory(root, "2.0.0");
  const archive = join(source, "versions", "2.0.0");
  await mkdir(archive);
  for (const name of PUBLISHED_FILES) await copyFile(join(directory, name), join(archive, name));
  for (const name of binaries) await copyFile(join(directory, name), join(source, "firmware", name));
  await json(join(source, "manifest.json"), manifest("2.0.0", "ili9341", "firmware/"));
  await json(join(source, "manifest-st7789.json"), manifest("2.0.0", "st7789", "firmware/"));
  const release = JSON.parse(await readFile(join(directory, "release.json"), "utf8"));
  release.manifests = legacyEntry("2.0.0").manifests;
  await json(join(source, "releases.json"), { versions: [release, legacyEntry("1.8.0")] });
  const before = await treeHashes(source);
  assert.equal((await validateInstaller(source)).version, "2.0.0");
  assert.deepEqual(await treeHashes(source), before);
});

test("published releases, prereleases and wrong commits cannot be resumed as writable drafts", () => {
  const release = metadata("2.0.0");
  assert.throws(() => assertDraftRelease(release, "v2.0.0", commit), /unpublished stable draft/);
  release.draft = true;
  release.published_at = null;
  assertDraftRelease(release, "v2.0.0", commit);
  assert.throws(() => assertDraftRelease(release, "v2.1.0", commit), /unpublished stable draft/);
  assert.throws(() => assertDraftRelease(release, "v2.0.0", "b".repeat(40)), /different commit/);
  release.prerelease = true;
  assert.throws(() => assertDraftRelease(release, "v2.0.0", commit), /unpublished stable draft/);
});

test("draft creation uploads the fixed assets and identical reruns never overwrite them", async (t) => {
  const root = await fixture(t);
  const directory = await modernDirectory(root, "2.0.0", true);
  const client = fakeClient();
  await publishDraft(directory, "v2.0.0", commit, client);
  const creation = client.calls.find(([operation]) => operation === "create")[1];
  assert.equal(creation.draft, true);
  assert.equal(creation.prerelease, false);
  assert.equal(Object.hasOwn(creation, "make_latest"), false);
  assert.match(creation.body, /Published 2.0.0/);
  assert.match(creation.body, /Hardware acceptance/);
  assert.deepEqual(client.calls.filter(([operation]) => operation === "upload").map((call) => call[1]),
    [...RELEASE_FILES, "checksums.sha256", "usb-installer.zip"]);
  client.calls.length = 0;
  await publishDraft(directory, "v2.0.0", commit, client);
  assert.equal(client.calls.some(([operation]) => ["create", "upload"].includes(operation)), false);
  const asset = client.releases[0].assets[1];
  asset.bytes = Buffer.from(asset.bytes);
  asset.bytes[32] ^= 1;
  asset.digest = `sha256:${hash(asset.bytes)}`;
  await assert.rejects(publishDraft(directory, "v2.0.0", commit, client), /differs from this build/);
  assert.equal(client.calls.some(([operation]) => operation === "upload"), false);
});

test("published reruns, moved tags and publication during a draft rerun stop before asset mutation", async (t) => {
  const root = await fixture(t);
  const { release, directory } = await remoteRelease(root, "2.0.0", 1, true);
  const client = fakeClient([release]);
  await assert.rejects(publishDraft(directory, "v2.0.0", commit, client), /unpublished stable draft/);
  assert.equal(client.calls.some(([operation]) => ["upload", "create"].includes(operation)), false);
  client.tagCommit = async () => "b".repeat(40);
  await assert.rejects(publishDraft(directory, "v2.0.0", commit, client), /remote tag/);
  const racing = fakeClient([{ ...release, draft: true, published_at: null }]);
  racing.release = async () => release;
  await assert.rejects(publishDraft(directory, "v2.0.0", commit, racing), /unpublished stable draft/);
  assert.equal(racing.calls.some(([operation]) => ["upload", "create"].includes(operation)), false);
});

test("partial draft reruns verify every existing asset before adding missing ones", async (t) => {
  const root = await fixture(t);
  const { directory, release } = await remoteRelease(root, "2.0.0", 1, true);
  release.draft = true;
  release.published_at = null;
  release.assets = release.assets.filter((asset) => !["checksums.sha256", "usb-installer.zip"].includes(asset.name));
  const client = fakeClient([release]);
  await publishDraft(directory, "v2.0.0", commit, client);
  assert.deepEqual(client.calls.filter(([operation]) => operation === "upload").map((call) => call[1]),
    ["checksums.sha256", "usb-installer.zip"]);
  release.assets = release.assets.filter((asset) => asset.name !== "ota-manifest.json");
  const existing = release.assets.find((asset) => asset.name === "firmware-ili9341.bin");
  existing.bytes[32] ^= 1;
  existing.digest = `sha256:${hash(existing.bytes)}`;
  client.calls.length = 0;
  await assert.rejects(publishDraft(directory, "v2.0.0", commit, client), /differs from this build/);
  assert.equal(client.calls.some(([operation]) => operation === "upload"), false);
});

test("manual publication while uploading stops subsequent draft writes", async (t) => {
  const root = await fixture(t);
  const directory = await modernDirectory(root, "2.0.0", true);
  const client = fakeClient();
  const getRelease = client.release.bind(client);
  let probes = 0;
  client.release = async (id) => {
    const release = await getRelease(id);
    probes += 1;
    if (probes === 3) {
      release.draft = false;
      release.published_at = date;
    }
    return release;
  };
  await assert.rejects(publishDraft(directory, "v2.0.0", commit, client), /unpublished stable draft/);
  assert.equal(client.calls.filter(([operation]) => operation === "upload").length, 1);
});
