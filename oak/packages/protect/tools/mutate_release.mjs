/**
 * Per-release mutation for OakProtect builds.
 *
 * Rewrites the g_OakBuildMark placeholder inside a built client DLL with an
 * opaque watermark. This is intentionally cheap and does not change inject
 * timing. Point it at any OakProtect-linked product DLL.
 *
 * Usage:
 *   node protect/tools/mutate_release.mjs --dll path\to\dayz_internal.dll
 *   node protect/tools/mutate_release.mjs --dll path\to\client.dll --mark <opaqueId>
 */
import fs from "node:fs";
import crypto from "node:crypto";
import path from "node:path";

const args = process.argv.slice(2);
function arg(name) {
  const i = args.indexOf(name);
  return i >= 0 ? args[i + 1] : undefined;
}

const dllPath = arg("--dll");
if (!dllPath) {
  console.error("Usage: node mutate_release.mjs --dll <client.dll> [--mark <opaqueId>]");
  process.exit(1);
}

const markerPrefix = Buffer.from("OAKWMARK");
const totalLen = 80;
const mark =
  arg("--mark") ||
  crypto.randomBytes(24).toString("base64url").replace(/[^A-Za-z0-9]/g, "X").slice(0, 64);

let payload = `OAKWMARK${mark}`;
if (payload.length > totalLen) payload = payload.slice(0, totalLen);
while (payload.length < totalLen) payload += "_";

const bin = fs.readFileSync(dllPath);
const idx = bin.indexOf(markerPrefix);
if (idx < 0) {
  console.error("Build mark placeholder not found. Is this an OakProtect-linked Release DLL?");
  process.exit(1);
}

const next = Buffer.from(bin);
Buffer.from(payload, "ascii").copy(next, idx, 0, totalLen);
const out = arg("--out") || dllPath.replace(/(\.dll)?$/i, `.wm.dll`);
fs.writeFileSync(out, next);

const sha = crypto.createHash("sha256").update(next).digest("hex");
console.log(JSON.stringify({
  input: path.resolve(dllPath),
  output: path.resolve(out),
  watermark: payload.slice(0, 72),
  sha256: sha,
}, null, 2));
