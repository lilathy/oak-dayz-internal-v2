import jwt from "jsonwebtoken";
import { config } from "./config.js";
import { db, cryptoRandomId, nowIso } from "./db.js";
import { newRefreshToken, sha256 } from "./crypto.js";

export type AccessClaims = {
  sub: string;
  role: "user" | "admin";
  /** Refresh-token session that issued this access JWT. */
  sid: string;
  typ: "access";
};

export function signAccessToken(userId: string, role: "user" | "admin", sessionId: string): string {
  const claims: AccessClaims = { sub: userId, role, sid: sessionId, typ: "access" };
  return jwt.sign(claims, config.jwtAccessSecret, {
    expiresIn: config.accessTtlSec,
    issuer: "oak-api",
    audience: "oak-clients",
  });
}

export function verifyAccessToken(token: string): AccessClaims {
  const decoded = jwt.verify(token, config.jwtAccessSecret, {
    issuer: "oak-api",
    audience: "oak-clients",
  }) as AccessClaims;
  if (
    decoded.typ !== "access" ||
    typeof decoded.sub !== "string" ||
    !/^[0-9a-f-]{36}$/i.test(decoded.sid ?? "") ||
    (decoded.role !== "user" && decoded.role !== "admin")
  ) {
    throw new Error("invalid access token");
  }
  return decoded;
}

export function issueRefreshToken(
  userId: string,
  meta: { ip?: string | null; userAgent?: string | null },
  familyId?: string
): { id: string; raw: string; expiresAt: string; familyId: string } {
  const raw = newRefreshToken();
  const id = cryptoRandomId();
  const family = familyId || cryptoRandomId();
  const expiresAt = new Date(Date.now() + config.refreshTtlSec * 1000).toISOString();
  db.prepare(
    `INSERT INTO refresh_tokens
       (id, user_id, token_hash, expires_at, revoked, created_at, user_agent, ip, family_id)
     VALUES (@id, @user_id, @token_hash, @expires_at, 0, @created_at, @user_agent, @ip, @family_id)`
  ).run({
    id,
    user_id: userId,
    token_hash: sha256(raw),
    expires_at: expiresAt,
    created_at: nowIso(),
    user_agent: meta.userAgent ?? null,
    ip: meta.ip ?? null,
    family_id: family,
  });
  return { id, raw, expiresAt, familyId: family };
}

export function revokeRefreshToken(raw: string): void {
  db.prepare(`UPDATE refresh_tokens SET revoked = 1 WHERE token_hash = ?`).run(sha256(raw));
}

export function revokeAllRefreshTokens(userId: string): void {
  db.prepare(`UPDATE refresh_tokens SET revoked = 1 WHERE user_id = ?`).run(userId);
}

export function revokeRefreshFamily(familyId: string): void {
  db.prepare(`UPDATE refresh_tokens SET revoked = 1 WHERE family_id = ?`).run(familyId);
}

export function revokeRefreshSession(sessionId: string, userId: string): void {
  db.prepare(`UPDATE refresh_tokens SET revoked = 1 WHERE id = ? AND user_id = ?`).run(sessionId, userId);
}

export function getActiveRefreshSession(sessionId: string, userId: string): boolean {
  const row = db
    .prepare(
      `SELECT 1 FROM refresh_tokens
       WHERE id = ? AND user_id = ? AND revoked = 0 AND expires_at > ?`
    )
    .get(sessionId, userId, nowIso());
  return !!row;
}

export type ConsumeRefreshOk = {
  ok: true;
  userId: string;
  sessionId: string;
  familyId: string;
};

export type ConsumeRefreshFail = {
  ok: false;
  reason: "missing" | "expired" | "reuse";
  userId?: string;
  familyId?: string;
};

/**
 * Rotates a refresh token. Presenting an already-revoked token is treated as
 * theft (reuse) and revokes the entire session family.
 */
export function consumeRefreshToken(raw: string): ConsumeRefreshOk | ConsumeRefreshFail {
  const row = db
    .prepare(
      `SELECT id, user_id, expires_at, revoked, family_id FROM refresh_tokens WHERE token_hash = ?`
    )
    .get(sha256(raw)) as
    | {
        id: string;
        user_id: string;
        expires_at: string;
        revoked: number;
        family_id: string | null;
      }
    | undefined;

  if (!row) return { ok: false, reason: "missing" };

  const familyId = row.family_id || row.id;

  if (row.revoked) {
    revokeRefreshFamily(familyId);
    // Belt-and-suspenders for pre-family rows that share no family_id.
    revokeAllRefreshTokens(row.user_id);
    return { ok: false, reason: "reuse", userId: row.user_id, familyId };
  }

  if (new Date(row.expires_at).getTime() < Date.now()) {
    db.prepare(`UPDATE refresh_tokens SET revoked = 1 WHERE id = ?`).run(row.id);
    return { ok: false, reason: "expired", userId: row.user_id, familyId };
  }

  db.prepare(`UPDATE refresh_tokens SET revoked = 1 WHERE id = ?`).run(row.id);
  return { ok: true, userId: row.user_id, sessionId: row.id, familyId };
}
