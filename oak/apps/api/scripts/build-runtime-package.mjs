/**
 * Builds a signed-ready runtime package payload from offsets_generated.hpp
 * and optionally stages/activates it through the local admin API.
 *
 * Usage:
 *   node scripts/build-runtime-package.mjs
 *   node scripts/build-runtime-package.mjs --activate --token <accessJwt>
 */
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import Database from "better-sqlite3";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const oakRoot = path.resolve(__dirname, "..", "..", "..");
const offsetsPath = path.join(oakRoot, "clients", "dayz", "src", "offsets_generated.hpp");
const BASE = process.env.SMOKE_BASE ?? "http://127.0.0.1:8787";
const args = new Set(process.argv.slice(2));
const tokenIdx = process.argv.indexOf("--token");
const token = tokenIdx >= 0 ? process.argv[tokenIdx + 1] : process.env.OAK_ADMIN_TOKEN;

const text = fs.readFileSync(offsetsPath, "utf8");
const runtimeValues = {};
let ns = "";
for (const line of text.split(/\r?\n/)) {
  const nsMatch = line.match(/namespace\s+(\w+)\s*\{/);
  if (nsMatch && !["oak_offsets"].includes(nsMatch[1])) {
    ns = nsMatch[1];
    continue;
  }
  const off = line.match(/OAK_RUNTIME_OFFSET\(\s*(\w+)\s*,\s*(0x[0-9A-Fa-f]+|\d+)\s*\)/);
  if (off && ns) runtimeValues[`${ns}.${off[1]}`] = off[2];
}

const db = new Database(path.join(__dirname, "..", "data", "oak.sqlite"));
const release = db
  .prepare(`SELECT id FROM client_releases WHERE product_slug = 'dayz' AND is_current = 1 LIMIT 1`)
  .get();
if (!release) {
  console.error("No current DayZ client release. Upload one first.");
  process.exit(1);
}

const expiresAt = new Date(Date.now() + 365 * 86400_000).toISOString();
const payload = {
  schemaVersion: 1,
  productSlug: "dayz",
  clientReleaseId: release.id,
  enabled: true,
  expiresAt,
  runtimeValues,
};

const outPath = path.join(__dirname, "..", "data", "runtime-package-dayz.json");
fs.mkdirSync(path.dirname(outPath), { recursive: true });
fs.writeFileSync(outPath, JSON.stringify(payload, null, 2));
console.log(`Wrote ${outPath} (${Object.keys(runtimeValues).length} offsets)`);

if (!args.has("--activate")) process.exit(0);
if (!token) {
  console.error("Provide --token <accessJwt> or OAK_ADMIN_TOKEN to activate.");
  process.exit(1);
}

const version = `offsets-${new Date().toISOString().slice(0, 10)}-${Date.now().toString(36)}`;
const stage = await fetch(`${BASE}/v1/admin/protection/runtime-packages/dayz`, {
  method: "POST",
  headers: {
    Authorization: `Bearer ${token}`,
    "Content-Type": "application/json",
    "X-Oak-Client": "launcher",
  },
  // Admin route wraps this as payload.runtimeValues — send the flat offset map
  // only. Passing the full envelope double-nests runtimeValues.
  body: JSON.stringify({ version, clientReleaseId: release.id, payload: runtimeValues }),
});
const staged = await stage.json();
if (!stage.ok) {
  console.error("stage failed", staged);
  process.exit(1);
}
const activate = await fetch(`${BASE}/v1/admin/protection/runtime-packages/${staged.id}/activate`, {
  method: "POST",
  headers: { Authorization: `Bearer ${token}`, "X-Oak-Client": "launcher" },
});
const activated = await activate.json();
if (!activate.ok) {
  console.error("activate failed", activated);
  process.exit(1);
}
console.log(`Activated runtime package ${staged.id} (${version})`);
