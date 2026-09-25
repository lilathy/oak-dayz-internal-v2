#!/usr/bin/env node
/**
 * Pre-ship security gate for Oak client artifacts.
 *
 * Usage:
 *   node oak/protect/tools/ship_security_gate.mjs --dll path\to\dayz_internal.dll [--mutate]
 *
 * Checks:
 *   - DLL exists and is non-trivial
 *   - No oak_dev_unlock / protect_auth.flag next to artifact
 *   - Optional: run mutate_release watermark rewrite
 *   - Refuse shipping if a PDB sits beside the DLL
 */
import fs from "node:fs";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const args = process.argv.slice(2);
function arg(name) {
  const i = args.indexOf(name);
  return i >= 0 ? args[i + 1] : undefined;
}
const wantMutate = args.includes("--mutate");
const dllPath = arg("--dll");
if (!dllPath) {
  console.error("Usage: node ship_security_gate.mjs --dll <client.dll> [--mutate]");
  process.exit(1);
}

const abs = path.resolve(dllPath);
const failures = [];

if (!fs.existsSync(abs)) failures.push(`missing dll: ${abs}`);
else {
  const st = fs.statSync(abs);
  if (st.size < 50_000) failures.push(`dll suspiciously small (${st.size} bytes)`);
}

const dir = path.dirname(abs);
const base = path.basename(abs, path.extname(abs));
for (const bad of [`${base}.pdb`, "oak_dev_unlock", "protect_auth.flag", "inject.log"]) {
  const p = path.join(dir, bad);
  if (fs.existsSync(p)) failures.push(`refuse ship: found ${p}`);
}

// Absolute developer paths must not appear as plaintext UTF-16/UTF-8 in Release DLLs.
if (fs.existsSync(abs)) {
  const bin = fs.readFileSync(abs);
  const hay = bin.toString("latin1").toLowerCase();
  for (const needle of ["onedrive\\desktop", "oak_dev_unlock", "protect_auth.flag"]) {
    if (hay.includes(needle)) failures.push(`dll contains sensitive string: ${needle}`);
  }
}

if (wantMutate && failures.length === 0) {
  const here = path.dirname(fileURLToPath(import.meta.url));
  const mutate = path.join(here, "mutate_release.mjs");
  const r = spawnSync(process.execPath, [mutate, "--dll", abs, "--out", abs], { encoding: "utf8" });
  if (r.status !== 0) {
    failures.push(`mutate_release failed: ${r.stderr || r.stdout}`);
  } else {
    console.log(r.stdout.trim());
  }
}

if (failures.length) {
  console.error(JSON.stringify({ ok: false, failures }, null, 2));
  process.exit(2);
}

console.log(JSON.stringify({ ok: true, dll: abs, size: fs.statSync(abs).size }, null, 2));
