import { Router } from "express";
import { z } from "zod";
import rateLimit from "express-rate-limit";
import { config } from "../config.js";
import { hashPassword, verifyPassword, hashHwid, hwidHint } from "../crypto.js";
import { audit, cryptoRandomId, db, nowIso, type UserRow } from "../db.js";
import {
  getHwid,
  getLicense,
  publicHwid,
  publicLicense,
  publicUser,
  syncLicenseStatus,
} from "../license.js";
import { trackEvent, touchLastSeen } from "../analytics.js";
import { raiseSecurityAlert } from "../securityAlerts.js";
import { sendMail } from "../mail.js";
import {
  captchaEnabled,
  clearAdminAccessCookie,
  clearRefreshCookie,
  consumeEmailVerification,
  createEmailVerification,
  isAdminClient,
  isWebClient,
  resetEmailVerification,
  setAdminAccessCookie,
  setRefreshCookie,
  verifyCaptcha,
  type AccountUser,
} from "../accounts.js";
import { consumePasswordResetToken, createPasswordResetToken } from "../support.js";
import { clientIp, requireAuth, type AuthedRequest } from "../middleware/auth.js";
import {
  consumeRefreshToken,
  issueRefreshToken,
  revokeAllRefreshTokens,
  revokeRefreshSession,
  signAccessToken,
} from "../tokens.js";
import { revokeProtectionForUser } from "../protection.js";

export const authRouter = Router();

const authLimiter = rateLimit({
  windowMs: 15 * 60 * 1000,
  max: 40,
  standardHeaders: true,
  legacyHeaders: false,
  message: { error: "rate_limited" },
});

const registerSchema = z.object({
  email: z.string().email().max(190),
  username: z
    .string()
    .min(3)
    .max(32)
    .regex(/^[a-zA-Z0-9_]+$/),
  password: z.string().min(10).max(128),
  captchaToken: z.string().max(4096).optional(),
});

/**
 * Rejects the passwords that show up first in every credential-stuffing list.
 * Length is already enforced by the schema; this catches the low-effort ones.
 */
const WEAK_PASSWORDS = new Set([
  "password12",
  "password123",
  "password1234",
  "1234567890",
  "12345678910",
  "qwertyuiop",
  "letmein123",
  "iloveyou12",
  "adminadmin",
  "welcome123",
  "changeme12",
]);

function isWeakPassword(password: string, email: string, username: string): boolean {
  const p = password.toLowerCase();
  if (WEAK_PASSWORDS.has(p)) return true;
  if (p.includes(username.toLowerCase())) return true;
  const localPart = email.split("@")[0]?.toLowerCase() ?? "";
  if (localPart.length >= 4 && p.includes(localPart)) return true;
  // A single repeated character or a straight run of digits is not a password.
  if (/^(.)\1+$/.test(p)) return true;
  if (/^0?123456789/.test(p)) return true;
  return false;
}

/** Usernames that would let someone impersonate the team. */
const RESERVED_USERNAMES = new Set([
  "admin", "administrator", "oak", "oakstaff", "staff", "moderator", "mod",
  "support", "system", "root", "owner", "official", "team", "help", "billing",
  "security", "null", "undefined", "me", "everyone", "here",
]);

/** Issues the session pair and, for browsers, stores the refresh token in a cookie. */
function issueSession(
  req: import("express").Request,
  res: import("express").Response,
  user: UserRow,
  familyId?: string
): { accessToken: string; refreshToken: string | undefined } {
  const refresh = issueRefreshToken(
    user.id,
    {
      ip: clientIp(req),
      userAgent: req.get("user-agent"),
    },
    familyId
  );
  const accessToken = signAccessToken(user.id, user.role, refresh.id);
  const browser = isWebClient(req) || isAdminClient(req);
  if (browser) {
    setRefreshCookie(res, refresh.raw, config.refreshTtlSec);
  }
  if (isAdminClient(req) && user.role === "admin") {
    setAdminAccessCookie(res, accessToken, config.accessTtlSec);
  }
  if (browser) {
    return { accessToken, refreshToken: undefined };
  }
  return { accessToken, refreshToken: refresh.raw };
}

function sendVerificationEmail(user: { id: string; email: string; username: string }): void {
  const raw = createEmailVerification(user.id, user.email);
  const link = `${config.publicSiteUrl}/verify-email?token=${encodeURIComponent(raw)}`;
  void sendMail({
    to: user.email,
    subject: "Confirm your Oak account",
    text:
      `Hi ${user.username},\n\n` +
      `Confirm your email address to finish setting up your Oak account (link valid 24 hours):\n\n` +
      `${link}\n\n` +
      `If you did not create this account, ignore this email.`,
  });
}

const loginSchema = z.object({
  login: z.string().min(3).max(190), // email or username
  password: z.string().min(1).max(128),
  hwid: z.string().min(8).max(256).optional(),
});

function ensureLicenseRow(userId: string): void {
  const existing = getLicense(userId);
  if (existing) return;
  const t = nowIso();
  db.prepare(
    `INSERT INTO licenses (id, user_id, status, plan, expires_at, created_at, updated_at)
     VALUES (?, ?, 'inactive', 'none', NULL, ?, ?)`
  ).run(cryptoRandomId(), userId, t, t);
}

authRouter.post("/register", authLimiter, async (req, res) => {
  const parsed = registerSchema.safeParse(req.body);
  if (!parsed.success) {
    res.status(400).json({ error: "invalid_body", details: parsed.error.flatten() });
    return;
  }
  const { email, username, password } = parsed.data;

  if (!(await verifyCaptcha(parsed.data.captchaToken, clientIp(req)))) {
    res.status(400).json({ error: "captcha_failed" });
    return;
  }
  if (RESERVED_USERNAMES.has(username.toLowerCase())) {
    res.status(409).json({ error: "username_reserved" });
    return;
  }
  if (isWeakPassword(password, email, username)) {
    res.status(400).json({ error: "weak_password" });
    return;
  }

  const exists = db
    .prepare(`SELECT id FROM users WHERE email = ? OR username = ?`)
    .get(email, username);
  if (exists) {
    res.status(409).json({ error: "already_exists" });
    return;
  }

  const id = cryptoRandomId();
  const t = nowIso();
  const password_hash = await hashPassword(password);
  db.prepare(
    `INSERT INTO users (id, email, username, password_hash, role, banned, created_at, updated_at)
     VALUES (?, ?, ?, ?, 'user', 0, ?, ?)`
  ).run(id, email.toLowerCase(), username, password_hash, t, t);
  ensureLicenseRow(id);
  audit("user.register", { actorId: id, targetId: id, ip: clientIp(req) });

  const user = db.prepare(`SELECT * FROM users WHERE id = ?`).get(id) as UserRow;
  sendVerificationEmail(user);
  const session = issueSession(req, res, user);

  touchLastSeen(id);
  trackEvent("register", { userId: id, ip: clientIp(req) });

  res.status(201).json({
    user: publicUser(user),
    license: publicLicense(syncLicenseStatus(id)),
    hwid: publicHwid(getHwid(id)),
    accessToken: session.accessToken,
    refreshToken: session.refreshToken,
    expiresIn: config.accessTtlSec,
  });
});

authRouter.post("/login", authLimiter, async (req, res) => {
  const parsed = loginSchema.safeParse(req.body);
  if (!parsed.success) {
    res.status(400).json({ error: "invalid_body", details: parsed.error.flatten() });
    return;
  }
  const { login, password, hwid } = parsed.data;
  const user = db
    .prepare(`SELECT * FROM users WHERE email = ? OR username = ?`)
    .get(login.toLowerCase(), login) as UserRow | undefined;

  // Constant-ish failure
  if (!user || !(await verifyPassword(user.password_hash, password))) {
    audit("user.login_fail", { meta: { login }, ip: clientIp(req) });
    trackEvent("web.login_fail", { path: "/login", ip: clientIp(req) });
    res.status(401).json({ error: "invalid_credentials" });
    return;
  }
  if (user.banned) {
    res.status(403).json({ error: "banned", reason: user.ban_reason ?? "banned" });
    return;
  }

  ensureLicenseRow(user.id);
  const license = syncLicenseStatus(user.id)!;

  if (hwid) {
    const h = hashHwid(hwid, config.hwidPepper);
    const bound = getHwid(user.id);
    if (!bound) {
      const t = nowIso();
      db.prepare(
        `INSERT INTO hwids (id, user_id, hwid_hash, hwid_hint, bound_at, last_seen_at, reset_count)
         VALUES (?, ?, ?, ?, ?, ?, 0)`
      ).run(cryptoRandomId(), user.id, h, hwidHint(hwid), t, t);
      audit("hwid.bind", { actorId: user.id, targetId: user.id, ip: clientIp(req) });
    } else if (bound.hwid_hash !== h) {
      audit("hwid.mismatch", { actorId: user.id, targetId: user.id, ip: clientIp(req) });
      raiseSecurityAlert({
        severity: "high",
        category: "hwid",
        title: "HWID mismatch on login",
        summary: "Bound machine does not match presented fingerprint",
        dedupeKey: `hwid_mismatch:login:${user.id}:${new Date().toISOString().slice(0, 10)}`,
        userId: user.id,
        hwidId: bound.id,
        evidence: { path: "auth.login" },
        ip: clientIp(req),
      });
      res.status(403).json({ error: "hwid_mismatch", hint: bound.hwid_hint });
      return;
    } else {
      db.prepare(`UPDATE hwids SET last_seen_at = ? WHERE user_id = ?`).run(nowIso(), user.id);
    }
  }

  audit("user.login", { actorId: user.id, targetId: user.id, ip: clientIp(req) });
  touchLastSeen(user.id);
  trackEvent("login", { userId: user.id, ip: clientIp(req) });
  const session = issueSession(req, res, user);

  res.json({
    user: publicUser(user),
    license: publicLicense(license),
    hwid: publicHwid(getHwid(user.id)),
    accessToken: session.accessToken,
    refreshToken: session.refreshToken,
    expiresIn: config.accessTtlSec,
  });
});

authRouter.post("/refresh", authLimiter, (req, res) => {
  // Browsers send the token in an httpOnly cookie; the launcher posts it.
  const body = z.object({ refreshToken: z.string().min(20).optional() }).safeParse(req.body ?? {});
  const cookieToken = (req as { cookies?: Record<string, string> }).cookies?.[config.cookieName];
  const token = (body.success ? body.data.refreshToken : undefined) ?? cookieToken;
  if (!token) {
    res.status(401).json({ error: "invalid_refresh" });
    return;
  }
  const consumed = consumeRefreshToken(token);
  if (!consumed.ok) {
    if (consumed.reason === "reuse" && consumed.userId) {
      revokeProtectionForUser(consumed.userId, "refresh_reuse");
      raiseSecurityAlert({
        severity: "critical",
        category: "auth",
        title: "Refresh token reuse",
        summary: "A rotated refresh token was presented again — session family revoked",
        dedupeKey: `refresh_reuse:${consumed.userId}:${new Date().toISOString().slice(0, 13)}`,
        userId: consumed.userId,
        evidence: { path: "auth.refresh", familyId: consumed.familyId ?? "" },
        ip: clientIp(req),
      });
    }
    clearRefreshCookie(res);
    clearAdminAccessCookie(res);
    res.status(401).json({
      error: consumed.reason === "reuse" ? "refresh_reuse" : "invalid_refresh",
    });
    return;
  }
  const user = db.prepare(`SELECT * FROM users WHERE id = ?`).get(consumed.userId) as UserRow | undefined;
  if (!user || user.banned) {
    clearRefreshCookie(res);
    clearAdminAccessCookie(res);
    res.status(401).json({ error: "invalid_refresh" });
    return;
  }
  const session = issueSession(req, res, user, consumed.familyId);
  touchLastSeen(user.id);
  res.json({
    user: publicUser(user),
    accessToken: session.accessToken,
    refreshToken: session.refreshToken,
    expiresIn: config.accessTtlSec,
  });
});

authRouter.post("/logout", requireAuth, (req: AuthedRequest, res) => {
  if (req.user && req.authSessionId) revokeRefreshSession(req.authSessionId, req.user.id);
  else if (req.user) revokeAllRefreshTokens(req.user.id);
  if (req.user) revokeProtectionForUser(req.user.id, "logout");
  clearRefreshCookie(res);
  clearAdminAccessCookie(res);
  audit("user.logout", { actorId: req.user?.id, ip: clientIp(req) });
  res.json({ ok: true });
});

/** Signs out everywhere — the recovery action after a stolen password. */
authRouter.post("/logout-all", requireAuth, (req: AuthedRequest, res) => {
  revokeAllRefreshTokens(req.user!.id);
  revokeProtectionForUser(req.user!.id, "logout_all");
  clearRefreshCookie(res);
  clearAdminAccessCookie(res);
  audit("user.logout_all", { actorId: req.user!.id, ip: clientIp(req) });
  res.json({ ok: true });
});

authRouter.get("/sessions", requireAuth, (req: AuthedRequest, res) => {
  const rows = db
    .prepare(
      `SELECT id, created_at, expires_at, user_agent, ip
       FROM refresh_tokens
       WHERE user_id = ? AND revoked = 0 AND expires_at > ?
       ORDER BY created_at DESC LIMIT 25`
    )
    .all(req.user!.id, nowIso()) as {
    id: string;
    created_at: string;
    expires_at: string;
    user_agent: string | null;
    ip: string | null;
  }[];
  res.json({
    sessions: rows.map((r) => ({
      id: r.id,
      createdAt: r.created_at,
      expiresAt: r.expires_at,
      userAgent: r.user_agent,
      // Only the prefix, so a leaked response cannot fully locate someone.
      ip: r.ip ? r.ip.replace(/\.\d+$/, ".x").replace(/:[0-9a-f]*$/i, ":x") : null,
    })),
  });
});

authRouter.delete("/sessions/:id", requireAuth, (req: AuthedRequest, res) => {
  const result = db
    .prepare(`UPDATE refresh_tokens SET revoked = 1 WHERE id = ? AND user_id = ?`)
    .run(req.params.id, req.user!.id);
  if (result.changes !== 1) {
    res.status(404).json({ error: "not_found" });
    return;
  }
  audit("user.session_revoke", { actorId: req.user!.id, targetId: req.params.id, ip: clientIp(req) });
  res.json({ ok: true });
});

// ------------------------------------------------------------ verification

const verifyLimiter = rateLimit({
  windowMs: 60 * 60 * 1000,
  max: 6,
  standardHeaders: true,
  legacyHeaders: false,
  keyGenerator: (req) => (req as AuthedRequest).user?.id ?? clientIp(req),
  message: { error: "rate_limited" },
});

authRouter.post("/verify/send", requireAuth, verifyLimiter, (req: AuthedRequest, res) => {
  const user = req.user as AccountUser;
  if (user.email_verified_at) {
    res.json({ ok: true, alreadyVerified: true });
    return;
  }
  sendVerificationEmail(user);
  res.json({ ok: true });
});

authRouter.post("/verify", authLimiter, (req, res) => {
  const parsed = z.object({ token: z.string().min(20).max(512) }).safeParse(req.body);
  if (!parsed.success) {
    res.status(400).json({ error: "invalid_body" });
    return;
  }
  const userId = consumeEmailVerification(parsed.data.token);
  if (!userId) {
    res.status(400).json({ error: "invalid_or_expired_token" });
    return;
  }
  audit("user.email_verified", { actorId: userId, targetId: userId, ip: clientIp(req) });
  trackEvent("email_verified", { userId, ip: clientIp(req) });
  res.json({ ok: true });
});

authRouter.get("/captcha", (_req, res) => {
  res.json({ enabled: captchaEnabled() });
});

authRouter.get("/me", requireAuth, (req: AuthedRequest, res) => {
  const u = req.user!;
  touchLastSeen(u.id);
  res.json({
    user: publicUser(u),
    license: publicLicense(syncLicenseStatus(u.id)),
    hwid: publicHwid(getHwid(u.id)),
  });
});

authRouter.post("/forgot-password", authLimiter, async (req, res) => {
  const parsed = z
    .object({ email: z.string().email(), captchaToken: z.string().max(4096).optional() })
    .safeParse(req.body);
  if (!parsed.success) {
    res.status(400).json({ error: "invalid_body" });
    return;
  }
  if (!(await verifyCaptcha(parsed.data.captchaToken, clientIp(req)))) {
    res.status(400).json({ error: "captcha_failed" });
    return;
  }
  // Always 200 to avoid email enumeration
  const user = db
    .prepare(`SELECT * FROM users WHERE email = ?`)
    .get(parsed.data.email.toLowerCase()) as UserRow | undefined;
  if (user && !user.banned) {
    const { raw } = createPasswordResetToken(user.id);
    const link = `${config.publicSiteUrl}/reset-password?token=${encodeURIComponent(raw)}`;
    await sendMail({
      to: user.email,
      subject: "Oak password reset",
      text: `Reset your Oak password (valid 1 hour):\n\n${link}\n\nIf you did not request this, ignore this email.`,
    });
    audit("user.forgot_password", { actorId: user.id, ip: clientIp(req) });
  }
  res.json({ ok: true, message: "If that email exists, a reset link was sent." });
});

authRouter.post("/reset-password", authLimiter, async (req, res) => {
  const parsed = z
    .object({
      token: z.string().min(20),
      password: z.string().min(10).max(128),
    })
    .safeParse(req.body);
  if (!parsed.success) {
    res.status(400).json({ error: "invalid_body" });
    return;
  }
  const userId = consumePasswordResetToken(parsed.data.token);
  if (!userId) {
    res.status(400).json({ error: "invalid_or_expired_token" });
    return;
  }
  const resetUser = db.prepare(`SELECT email, username FROM users WHERE id = ?`).get(userId) as
    | { email: string; username: string }
    | undefined;
  if (
    resetUser &&
    isWeakPassword(parsed.data.password, resetUser.email, resetUser.username)
  ) {
    res.status(400).json({ error: "weak_password" });
    return;
  }
  const password_hash = await hashPassword(parsed.data.password);
  db.prepare(`UPDATE users SET password_hash = ?, updated_at = ? WHERE id = ?`).run(
    password_hash,
    nowIso(),
    userId
  );
  revokeAllRefreshTokens(userId);
  revokeProtectionForUser(userId, "password_reset");
  audit("user.reset_password", { actorId: userId, targetId: userId, ip: clientIp(req) });
  res.json({ ok: true });
});

authRouter.patch("/profile", requireAuth, async (req: AuthedRequest, res) => {
  const parsed = z
    .object({
      email: z.string().email().max(190).optional(),
      currentPassword: z.string().min(1).max(128).optional(),
      newPassword: z.string().min(10).max(128).optional(),
    })
    .safeParse(req.body);
  if (!parsed.success) {
    res.status(400).json({ error: "invalid_body", details: parsed.error.flatten() });
    return;
  }
  const u = db.prepare(`SELECT * FROM users WHERE id = ?`).get(req.user!.id) as UserRow;
  const d = parsed.data;

  if (d.newPassword) {
    if (!d.currentPassword || !(await verifyPassword(u.password_hash, d.currentPassword))) {
      res.status(403).json({ error: "wrong_password" });
      return;
    }
    if (isWeakPassword(d.newPassword, u.email, u.username)) {
      res.status(400).json({ error: "weak_password" });
      return;
    }
    const password_hash = await hashPassword(d.newPassword);
    db.prepare(`UPDATE users SET password_hash = ?, updated_at = ? WHERE id = ?`).run(
      password_hash,
      nowIso(),
      u.id
    );
    revokeAllRefreshTokens(u.id);
    revokeProtectionForUser(u.id, "password_changed");
  }

  if (d.email && d.email.toLowerCase() !== u.email.toLowerCase()) {
    // Changing the address requires the current password too, otherwise a
    // hijacked session could quietly move the account to another mailbox.
    if (!d.currentPassword || !(await verifyPassword(u.password_hash, d.currentPassword))) {
      res.status(403).json({ error: "wrong_password" });
      return;
    }
    const taken = db
      .prepare(`SELECT id FROM users WHERE email = ? AND id != ?`)
      .get(d.email.toLowerCase(), u.id);
    if (taken) {
      res.status(409).json({ error: "email_taken" });
      return;
    }
    db.prepare(`UPDATE users SET email = ?, updated_at = ? WHERE id = ?`).run(
      d.email.toLowerCase(),
      nowIso(),
      u.id
    );
    resetEmailVerification(u.id);
    sendVerificationEmail({ id: u.id, email: d.email.toLowerCase(), username: u.username });
  }

  const updated = db.prepare(`SELECT * FROM users WHERE id = ?`).get(u.id) as UserRow;
  audit("user.profile_update", {
    actorId: u.id,
    meta: { email: !!d.email, password: !!d.newPassword },
    ip: clientIp(req),
  });
  res.json({ user: publicUser(updated) });
});
