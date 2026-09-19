import assert from "node:assert/strict";
import { X509Certificate } from "node:crypto";
import { readFile } from "node:fs/promises";
import test from "node:test";

test("OTA trust roots have the reviewed public identities and valid self-signatures", async () => {
  const source = await readFile(new URL("../TrustedRoots.cpp", import.meta.url), "utf8");
  const roots = [...source.matchAll(/-----BEGIN CERTIFICATE-----[\s\S]+?-----END CERTIFICATE-----/g)]
    .map(([pem]) => new X509Certificate(pem));
  const expected = new Set([
    "4FF460D54B9C86DABFBCFC5712E0400D2BED3FBC4D4FBDAA86E06ADCD2A9AD7A",
    "C90F26F0FB1B4018B22227519B5CA2B53E2CA5B3BE5CF18EFE1BEF47380C5383",
    "96BCEC06264976F37460779ACF28C5A7CFE8A3C0AAE11A8FFCEE05C0BDDF08C6",
    "69729B8E15A86EFC177A57AFB7171DFC64ADD28C2FCA8CF1507E34453CCB1470",
    "CB3CCBB76031E5E0138F8DD39A23F9DE47FFC35E43C1144CEA27D46A5AB1CB5F",
  ]);
  assert.equal(roots.length, expected.size);
  const reviewedAt = Date.parse("2026-09-09T00:00:00Z");
  for (const root of roots) {
    assert.equal(root.ca, true);
    assert.equal(root.subject, root.issuer);
    assert.equal(root.verify(root.publicKey), true);
    assert(Date.parse(root.validFrom) <= reviewedAt && reviewedAt < Date.parse(root.validTo));
    assert(expected.delete(root.fingerprint256.replaceAll(":", "")), root.subject);
  }
  assert.equal(expected.size, 0);
});
