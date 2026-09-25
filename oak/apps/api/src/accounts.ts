import crypto from "node:crypto";
import type { Response } from "express";
import { config } from "./config.js";
import { sha256 } from "./crypto.js";
import { cryptoRandomId, db, nowIso, type UserRow } from "./db.js";

/**
 * Account-level schema that grew after the initial release. Kept as ALTERs so
 * existing databases upgrade in place.
 */
export function ensureAccountSchema(): void {
  db.exec(`
CREATE TABLE IF NOT EXISTS email_verifications (
  id          TEXT PRIMARY KEY,
  user_id     TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  token_hash  TEXT NOT NULL UNIQUE,
  email       TEXT NOT NULL,
  expires_at  TEXT NOT NULL,
  used_at     TEXT,
  created_at  TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_emailverify_user ON email_verifications(user_id);
`);

  for (const col of [
    `ALTER TABLE users ADD COLUMN email_verified_at TEXT`,
    `ALTER TABLE users ADD COLUMN forum_post_count INTEGER NOT NULL DEFAULT 0`,
    `ALTER TABLE users ADD COLUMN forum_muted_until TEXT`,
    `ALTER TABLE users ADD COLUMN signature TEXT`,
  ]) {
    try {
      db.exec(col);
    } catch {
      // Column already exists — nothing to migrate.
    }
  }
}

export type AccountUser = UserRow & {
  email_verified_at?: string | null;
  forum_post_count?: number;
  forum_muted_until?: string | null;
};

export function isEmailVerified(user: AccountUser): boolean {
  return !!user.email_verified_at;
}

export function isMuted(user: AccountUser): boolean {
  const until = user.forum_muted_until;
  return !!until && new Date(until).getTime() > Date.now();
}

/** Age of the account in milliseconds — used to slow down throwaway spam accounts. */
export function accountAgeMs(user: UserRow): number {
  return Date.now() - new Date(user.created_at).getTime();
}

// ------------------------------------------------------------ verification

const VERIFY_TTL_MS = 24 * 60 * 60 * 1000;

export function createEmailVerification(userId: string, email: string): string {
  // Only one live token per account, so re-sending invalidates the previous link.
  db.prepare(`UPDATE email_verifications SET used_at = ? WHERE user_id = ? AND used_at IS NULL`)
    .run(nowIso(), userId);

  const raw = crypto.randomBytes(32).toString("base64url");
  db.prepare(
    `INSERT INTO email_verifications (id, user_id, token_hash, email, expires_at, used_at, created_at)
     VALUES (?, ?, ?, ?, ?, NULL, ?)`
  ).run(
    cryptoRandomId(),
    userId,
    sha256(raw),
    email.toLowerCase(),
    new Date(Date.now() + VERIFY_TTL_MS).toISOString(),
    nowIso()
  );
  return raw;
}

export function consumeEmailVerification(raw: string): string | null {
  const row = db
    .prepare(
      `SELECT id, user_id, email, expires_at, used_at FROM email_verifications WHERE token_hash = ?`
    )
    .get(sha256(raw)) as
    | { id: string; user_id: string; email: string; expires_at: string; used_at: string | null }
    | undefined;
  if (!row || row.used_at) return null;
  if (new Date(row.expires_at).getTime() < Date.now()) return null;

  const user = db.prepare(`SELECT email FROM users WHERE id = ?`).get(row.user_id) as
    | { email: string }
    | undefined;
  // The address may have been changed after the link was sent.
  if (!user || user.email.toLowerCase() !== row.email) return null;

  const t = nowIso();
  db.prepare(`UPDATE email_verifications SET used_at = ? WHERE id = ?`).run(t, row.id);
  db.prepare(`UPDATE users SET email_verified_at = ?, updated_at = ? WHERE id = ?`)
    .run(t, t, row.user_id);
  return row.user_id;
}

/** Called when someone changes their address: the new one is unverified again. */
export function resetEmailVerification(userId: string): void {
  db.prepare(`UPDATE users SET email_verified_at = NULL WHERE id = ?`).run(userId);
  db.prepare(`UPDATE email_verifications SET used_at = ? WHERE user_id = ? AND used_at IS NULL`)
    .run(nowIso(), userId);
}

// ----------------------------------------------------------------- captcha

/**
 * Verifies a Cloudflare Turnstile token.
 * - Dev without TURNSTILE_SECRET: skipped (returns true).
 * - Production without TURNSTILE_SECRET: fail closed (returns false).
 * - Configured: siteverify; network/outage fail-closed in production only.
 */
export async function verifyCaptcha(token: unknown, ip?: string | null): Promise<boolean> {
  // Dev stays usable without Turnstile. Production must configure it so bots
  // cannot skip the check by omitting the env var.
  if (!config.turnstileSecret) return !config.isProd;
  if (typeof token !== "string" || token.length < 10 || token.length > 4096) return false;
  try {
    const body = new URLSearchParams({ secret: config.turnstileSecret, response: token });
    if (ip) body.set("remoteip", ip);
    const res = await fetch("https://challenges.cloudflare.com/turnstile/v0/siteverify", {
      method: "POST",
      body,
      signal: AbortSignal.timeout(5000),
    });
    const json = (await res.json()) as { success?: boolean };
    return json.success === true;
  } catch {
    // Fail closed in production: an outage must not become an open door for bots.
    // Local/dev can keep registering while Turnstile is unreachable.
    return !config.isProd;
  }
}

export const captchaEnabled = () => !!config.turnstileSecret;

// ------------------------------------------------------------------ cookie

const COOKIE_PATH = "/v1/auth";

/**
 * Stores the refresh token in an httpOnly cookie for browsers so a cross-site
 * script cannot read it. The launcher keeps using the JSON body instead.
 */
export function setRefreshCookie(res: Response, token: string, maxAgeSec: number): void {
  res.cookie(config.cookieName, token, {
    ...cookieOptions(),
    maxAge: maxAgeSec * 1000,
  });
}

export function clearRefreshCookie(res: Response): void {
  res.clearCookie(config.cookieName, cookieOptions());
}

/** Access JWT for the static /admin UI — never exposed to JS. */
export function setAdminAccessCookie(res: Response, accessToken: string, maxAgeSec: number): void {
  res.cookie(config.adminCookieName, accessToken, {
    ...adminCookieOptions(),
    maxAge: maxAgeSec * 1000,
  });
}

export function clearAdminAccessCookie(res: Response): void {
  res.clearCookie(config.adminCookieName, adminCookieOptions());
}

function cookieOptions() {
  return {
    httpOnly: true,
    secure: config.isProd,
    // The site and the API must share a registrable domain for this to be sent
    // (e.g. oak.gg + api.oak.gg, or localhost:3000 + localhost:8787).
    sameSite: config.isProd ? ("strict" as const) : ("lax" as const),
    path: COOKIE_PATH,
    domain: config.cookieDomain,
  };
}

function adminCookieOptions() {
  return {
    httpOnly: true,
    secure: config.isProd,
    sameSite: "strict" as const,
    // Path `/` so /admin pages and /v1/* API calls both receive it.
    path: "/",
    domain: config.cookieDomain,
  };
}

/** True when the caller is the website rather than the desktop launcher. */
export function isWebClient(req: { get(name: string): string | undefined }): boolean {
  return (req.get("X-Oak-Client") ?? "").toLowerCase() === "web";
}

/** True when the caller is the static /admin console. */
export function isAdminClient(req: { get(name: string): string | undefined }): boolean {
  return (req.get("X-Oak-Client") ?? "").toLowerCase() === "admin";
}
