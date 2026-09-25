import Database from "better-sqlite3";
import fs from "node:fs";
import path from "node:path";
import crypto from "node:crypto";
import { fileURLToPath } from "node:url";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const apiRoot = path.resolve(__dirname, "..");
const dll = process.argv[2];
if (!dll || !fs.existsSync(dll)) {
  console.error("usage: node publish_local_client.mjs <dll-path>");
  process.exit(1);
}
const buf = fs.readFileSync(dll);
const sha = crypto.createHash("sha256").update(buf).digest("hex");
const id = crypto.randomUUID();
const ver = "1.4.12-auth-mark";
const filename = `dayz-${ver}-${id.slice(0, 8)}.dll`;
const clientsDir = path.join(apiRoot, "data", "releases", "clients");
fs.mkdirSync(clientsDir, { recursive: true });
const storage = path.join(clientsDir, filename);
fs.copyFileSync(dll, storage);

const db = new Database(path.join(apiRoot, "data", "oak.sqlite"));
const t = new Date().toISOString();
db.prepare(`UPDATE client_releases SET is_current = 0 WHERE product_slug = 'dayz'`).run();
db.prepare(
  `INSERT INTO client_releases
   (id, product_slug, version, filename, storage_path, sha256, size_bytes, uploaded_by, created_at, is_current)
   VALUES (?, 'dayz', ?, ?, ?, ?, ?, 'local-agent', ?, 1)`
).run(id, ver, filename, storage, sha, buf.length, t);
console.log(
  JSON.stringify(
    db.prepare(`SELECT version, sha256, size_bytes, is_current, filename FROM client_releases WHERE is_current=1`).get(),
    null,
    2
  )
);
