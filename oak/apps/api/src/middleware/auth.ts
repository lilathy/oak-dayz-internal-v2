import type { NextFunction, Request, Response } from "express";
import { touchLastSeen } from "../analytics.js";
import { config } from "../config.js";
import { db, type UserRow } from "../db.js";
import { getActiveRefreshSession, verifyAccessToken } from "../tokens.js";

export type AuthedRequest = Request & {
  user?: UserRow;
  /** Live refresh-token session behind the bearer token. */
  authSessionId?: string;
};

const lastSeenThrottle = new Map<string, number>();

function maybeTouchLastSeen(userId: string): void {
  const now = Date.now();
  const prev = lastSeenThrottle.get(userId) ?? 0;
  if (now - prev < 60_000) return;
  lastSeenThrottle.set(userId, now);
  try {
    touchLastSeen(userId);
  } catch {
    /* ignore */
  }
  // Bound map growth
  if (lastSeenThrottle.size > 5000) {
    for (const [k, t] of lastSeenThrottle) {
      if (now - t > 10 * 60_000) lastSeenThrottle.delete(k);
    }
  }
}

function extractAccessToken(req: Request): string | null {
  const header = req.headers.authorization;
  if (typeof header === "string" && header.startsWith("Bearer ")) {
    return header.slice(7);
  }
  const cookies = (req as { cookies?: Record<string, string> }).cookies;
  const fromAdmin = cookies?.[config.adminCookieName];
  if (typeof fromAdmin === "string" && fromAdmin.length > 20) return fromAdmin;
  return null;
}

export function requireAuth(req: AuthedRequest, res: Response, next: NextFunction): void {
  const token = extractAccessToken(req);
  if (!token) {
    res.status(401).json({ error: "missing_token" });
    return;
  }
  try {
    const claims = verifyAccessToken(token);
    if (!getActiveRefreshSession(claims.sid, claims.sub)) {
      res.status(401).json({ error: "session_revoked" });
      return;
    }
    const user = db.prepare(`SELECT * FROM users WHERE id = ?`).get(claims.sub) as UserRow | undefined;
    if (!user) {
      res.status(401).json({ error: "user_gone" });
      return;
    }
    if (user.banned) {
      res.status(403).json({ error: "banned", reason: user.ban_reason ?? "banned" });
      return;
    }
    req.user = user;
    req.authSessionId = claims.sid;
    maybeTouchLastSeen(user.id);
    next();
  } catch {
    res.status(401).json({ error: "invalid_token" });
  }
}

/**
 * Attaches the user when a valid token is present but never rejects. Used by
 * read-only forum routes so guests can browse while members see their own
 * reactions and moderation controls.
 */
export function optionalAuth(req: AuthedRequest, _res: Response, next: NextFunction): void {
  const token = extractAccessToken(req);
  if (!token) {
    next();
    return;
  }
  try {
    const claims = verifyAccessToken(token);
    if (!getActiveRefreshSession(claims.sid, claims.sub)) {
      next();
      return;
    }
    const user = db.prepare(`SELECT * FROM users WHERE id = ?`).get(claims.sub) as UserRow | undefined;
    if (user && !user.banned) {
      req.user = user;
      req.authSessionId = claims.sid;
    }
  } catch {
    // An expired token just means "treat as a guest" here.
  }
  next();
}

export function requireAdmin(req: AuthedRequest, res: Response, next: NextFunction): void {
  requireAuth(req, res, () => {
    if (!req.user || req.user.role !== "admin") {
      res.status(403).json({ error: "admin_only" });
      return;
    }
    next();
  });
}

/**
 * X-Forwarded-For is client-controlled, so it is only honoured when a proxy is
 * explicitly configured (TRUST_PROXY). Otherwise audit/rate-limit records could
 * be poisoned by anyone sending the header.
 */
export function clientIp(req: Request): string {
  if (config.trustProxy) {
    const xf = req.headers["x-forwarded-for"];
    if (typeof xf === "string" && xf.length) return xf.split(",")[0]!.trim();
  }
  return req.socket.remoteAddress ?? "unknown";
}
