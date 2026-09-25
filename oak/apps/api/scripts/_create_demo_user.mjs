import Database from "better-sqlite3";
import path from "node:path";
import { fileURLToPath } from "node:url";

const email = process.argv[2];
const username = process.argv[3];
const password = process.argv[4];

const BASE = process.env.SMOKE_BASE ?? "http://127.0.0.1:8787";
const __dirname = path.dirname(fileURLToPath(import.meta.url));
const dbPath = path.join(__dirname, "..", "data", "oak.sqlite");

async function call(pathname, { method = "POST", body } = {}) {
  const headers = { "X-Oak-Client": "web", "Content-Type": "application/json" };
  const res = await fetch(`${BASE}${pathname}`, {
    method,
    headers,
    body: body ? JSON.stringify(body) : undefined,
  });
  const text = await res.text();
  let json = {};
  try {
    json = text ? JSON.parse(text) : {};
  } catch {
    json = { raw: text };
  }
  return { status: res.status, json };
}

const reg = await call("/v1/auth/register", {
  body: { email, username, password, acceptTos: true },
});
console.error("register", reg.status, JSON.stringify(reg.json).slice(0, 200));

const login = await call("/v1/auth/login", {
  body: { login: email, password },
});
if (login.status !== 200) {
  console.error("login failed", login.status, JSON.stringify(login.json));
  process.exit(1);
}

const userId = login.json.user.id;
const db = new Database(dbPath);
const now = new Date().toISOString();
const exp = new Date(Date.now() + 7 * 86400000).toISOString();
db.prepare(`UPDATE users SET email_verified_at = COALESCE(email_verified_at, ?) WHERE id = ?`).run(now, userId);
// Ensure a license row exists then activate
const existing = db.prepare(`SELECT user_id FROM licenses WHERE user_id = ?`).get(userId);
if (!existing) {
  db.prepare(
    `INSERT INTO licenses (user_id, status, plan, expires_at, created_at, updated_at)
     VALUES (?, 'active', 'day7', ?, ?, ?)`
  ).run(userId, exp, now, now);
} else {
  db.prepare(
    `UPDATE licenses SET status = 'active', plan = 'day7', expires_at = ?, updated_at = ? WHERE user_id = ?`
  ).run(exp, now, userId);
}
const row = db
  .prepare(`SELECT username, email, role, email_verified_at FROM users WHERE id = ?`)
  .get(userId);
const lic = db.prepare(`SELECT status, plan, expires_at FROM licenses WHERE user_id = ?`).get(userId);
console.log(JSON.stringify({ ok: true, user: row, license: lic, login: { email, username, password } }, null, 2));
