import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import Database from "better-sqlite3";
import { config } from "./config.js";

fs.mkdirSync(path.dirname(config.databasePath), { recursive: true });

export const db = new Database(config.databasePath);
db.pragma("journal_mode = WAL");
db.pragma("foreign_keys = ON");
db.pragma("busy_timeout = 5000");

db.exec(`
CREATE TABLE IF NOT EXISTS users (
  id            TEXT PRIMARY KEY,
  email         TEXT NOT NULL UNIQUE COLLATE NOCASE,
  username      TEXT NOT NULL UNIQUE COLLATE NOCASE,
  password_hash TEXT NOT NULL,
  role          TEXT NOT NULL DEFAULT 'user' CHECK(role IN ('user','admin')),
  banned        INTEGER NOT NULL DEFAULT 0,
  ban_reason    TEXT,
  created_at    TEXT NOT NULL,
  updated_at    TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS licenses (
  id            TEXT PRIMARY KEY,
  user_id       TEXT NOT NULL UNIQUE REFERENCES users(id) ON DELETE CASCADE,
  status        TEXT NOT NULL DEFAULT 'inactive'
                  CHECK(status IN ('inactive','active','expired','banned')),
  plan          TEXT NOT NULL DEFAULT 'none',
  expires_at    TEXT,
  created_at    TEXT NOT NULL,
  updated_at    TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS hwids (
  id            TEXT PRIMARY KEY,
  user_id       TEXT NOT NULL UNIQUE REFERENCES users(id) ON DELETE CASCADE,
  hwid_hash     TEXT NOT NULL,
  hwid_hint     TEXT NOT NULL,
  bound_at      TEXT NOT NULL,
  last_seen_at  TEXT NOT NULL,
  reset_count   INTEGER NOT NULL DEFAULT 0,
  last_reset_at TEXT
);

CREATE TABLE IF NOT EXISTS refresh_tokens (
  id            TEXT PRIMARY KEY,
  user_id       TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  token_hash    TEXT NOT NULL UNIQUE,
  expires_at    TEXT NOT NULL,
  revoked       INTEGER NOT NULL DEFAULT 0,
  created_at    TEXT NOT NULL,
  user_agent    TEXT,
  ip            TEXT,
  family_id     TEXT
);

CREATE TABLE IF NOT EXISTS audit_log (
  id            TEXT PRIMARY KEY,
  actor_id      TEXT,
  action        TEXT NOT NULL,
  target_id     TEXT,
  meta_json     TEXT,
  ip            TEXT,
  created_at    TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_refresh_user ON refresh_tokens(user_id);
CREATE INDEX IF NOT EXISTS idx_audit_created ON audit_log(created_at);
`);

// Existing DBs created before family_id: add the column and backfill.
try {
  db.exec(`ALTER TABLE refresh_tokens ADD COLUMN family_id TEXT`);
} catch {
  /* already present */
}
db.exec(`UPDATE refresh_tokens SET family_id = id WHERE family_id IS NULL OR family_id = ''`);
try {
  db.exec(`CREATE INDEX IF NOT EXISTS idx_refresh_family ON refresh_tokens(family_id)`);
} catch {
  /* ignore */
}

export type UserRow = {
  id: string;
  email: string;
  username: string;
  password_hash: string;
  role: "user" | "admin";
  banned: number;
  ban_reason: string | null;
  created_at: string;
  updated_at: string;
};

export type LicenseRow = {
  id: string;
  user_id: string;
  status: "inactive" | "active" | "expired" | "banned";
  plan: string;
  expires_at: string | null;
  created_at: string;
  updated_at: string;
};

export type HwidRow = {
  id: string;
  user_id: string;
  hwid_hash: string;
  hwid_hint: string;
  bound_at: string;
  last_seen_at: string;
  reset_count: number;
  last_reset_at: string | null;
};

export function nowIso(): string {
  return new Date().toISOString();
}

export function cryptoRandomId(): string {
  return crypto.randomUUID();
}

export function audit(
  action: string,
  opts: { actorId?: string | null; targetId?: string | null; meta?: unknown; ip?: string | null } = {}
): void {
  db.prepare(
    `INSERT INTO audit_log (id, actor_id, action, target_id, meta_json, ip, created_at)
     VALUES (@id, @actor_id, @action, @target_id, @meta_json, @ip, @created_at)`
  ).run({
    id: cryptoRandomId(),
    actor_id: opts.actorId ?? null,
    action,
    target_id: opts.targetId ?? null,
    meta_json: opts.meta == null ? null : JSON.stringify(opts.meta),
    ip: opts.ip ?? null,
    created_at: nowIso(),
  });
}
