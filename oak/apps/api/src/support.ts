import crypto from "node:crypto";
import { cryptoRandomId, db, nowIso } from "./db.js";
import { sha256 } from "./crypto.js";

export function ensureSupportSchema(): void {
  db.exec(`
CREATE TABLE IF NOT EXISTS password_resets (
  id          TEXT PRIMARY KEY,
  user_id     TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  token_hash  TEXT NOT NULL UNIQUE,
  expires_at  TEXT NOT NULL,
  used_at     TEXT,
  created_at  TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_pwreset_user ON password_resets(user_id);

CREATE TABLE IF NOT EXISTS tickets (
  id            TEXT PRIMARY KEY,
  user_id       TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  category      TEXT NOT NULL
                  CHECK(category IN ('general','hwid_reset','billing','technical','other')),
  subject       TEXT NOT NULL,
  status        TEXT NOT NULL DEFAULT 'open'
                  CHECK(status IN ('open','pending','resolved','closed')),
  created_at    TEXT NOT NULL,
  updated_at    TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_tickets_user ON tickets(user_id);
CREATE INDEX IF NOT EXISTS idx_tickets_status ON tickets(status);

CREATE TABLE IF NOT EXISTS ticket_messages (
  id            TEXT PRIMARY KEY,
  ticket_id     TEXT NOT NULL REFERENCES tickets(id) ON DELETE CASCADE,
  author_id     TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  is_staff      INTEGER NOT NULL DEFAULT 0,
  body          TEXT NOT NULL,
  created_at    TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_ticket_msgs ON ticket_messages(ticket_id);

CREATE TABLE IF NOT EXISTS hwid_resets (
  id          TEXT PRIMARY KEY,
  user_id     TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  token_hash  TEXT NOT NULL UNIQUE,
  expires_at  TEXT NOT NULL,
  used_at     TEXT,
  created_at  TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_hwidreset_user ON hwid_resets(user_id);
`);
}

export function createPasswordResetToken(userId: string): { raw: string; expiresAt: string } {
  const raw = crypto.randomBytes(32).toString("base64url");
  const token_hash = sha256(raw);
  const expiresAt = new Date(Date.now() + 60 * 60 * 1000).toISOString(); // 1h
  db.prepare(
    `INSERT INTO password_resets (id, user_id, token_hash, expires_at, used_at, created_at)
     VALUES (?, ?, ?, ?, NULL, ?)`
  ).run(cryptoRandomId(), userId, token_hash, expiresAt, nowIso());
  return { raw, expiresAt };
}

export function consumePasswordResetToken(raw: string): string | null {
  const hash = sha256(raw);
  const row = db
    .prepare(
      `SELECT id, user_id, expires_at, used_at FROM password_resets WHERE token_hash = ?`
    )
    .get(hash) as
    | { id: string; user_id: string; expires_at: string; used_at: string | null }
    | undefined;
  if (!row || row.used_at) return null;
  if (new Date(row.expires_at).getTime() < Date.now()) return null;
  db.prepare(`UPDATE password_resets SET used_at = ? WHERE id = ?`).run(nowIso(), row.id);
  return row.user_id;
}

export function createHwidResetToken(userId: string): { raw: string; expiresAt: string } {
  const raw = crypto.randomBytes(32).toString("base64url");
  const token_hash = sha256(raw);
  const expiresAt = new Date(Date.now() + 60 * 60 * 1000).toISOString();
  db.prepare(
    `INSERT INTO hwid_resets (id, user_id, token_hash, expires_at, used_at, created_at)
     VALUES (?, ?, ?, ?, NULL, ?)`
  ).run(cryptoRandomId(), userId, token_hash, expiresAt, nowIso());
  return { raw, expiresAt };
}

export function consumeHwidResetToken(raw: string): string | null {
  const hash = sha256(raw);
  const row = db
    .prepare(`SELECT id, user_id, expires_at, used_at FROM hwid_resets WHERE token_hash = ?`)
    .get(hash) as
    | { id: string; user_id: string; expires_at: string; used_at: string | null }
    | undefined;
  if (!row || row.used_at) return null;
  if (new Date(row.expires_at).getTime() < Date.now()) return null;
  db.prepare(`UPDATE hwid_resets SET used_at = ? WHERE id = ?`).run(nowIso(), row.id);
  return row.user_id;
}
