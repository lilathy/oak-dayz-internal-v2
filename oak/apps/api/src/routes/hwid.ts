import { Router } from "express";
import { z } from "zod";
import rateLimit from "express-rate-limit";
import { config } from "../config.js";
import { hashHwid, hwidHint, verifyPassword } from "../crypto.js";
import { audit, cryptoRandomId, db, nowIso } from "../db.js";
import { getHwid, publicHwid } from "../license.js";
import { sendMail } from "../mail.js";
import { clientIp, requireAuth, type AuthedRequest } from "../middleware/auth.js";
import { revokeProtectionForUser } from "../protection.js";
import { raiseSecurityAlert } from "../securityAlerts.js";
import { consumeHwidResetToken, createHwidResetToken } from "../support.js";

export const hwidRouter = Router();

const limiter = rateLimit({
  windowMs: 60 * 60 * 1000,
  max: 20,
  standardHeaders: true,
  legacyHeaders: false,
  message: { error: "rate_limited" },
});

/** User self-reset cooldown (admin bypasses via /admin). */
const RESET_COOLDOWN_MS = 72 * 60 * 60 * 1000;

function lastSelfResetAt(userId: string): Date | null {
  const row = db
    .prepare(
      `SELECT created_at FROM audit_log
       WHERE action = 'hwid.reset_self' AND actor_id = ?
       ORDER BY created_at DESC LIMIT 1`
    )
    .get(userId) as { created_at: string } | undefined;
  return row ? new Date(row.created_at) : null;
}

function assertCooldown(userId: string, res: import("express").Response): boolean {
  const last = lastSelfResetAt(userId);
  if (!last) return true;
  const elapsed = Date.now() - last.getTime();
  if (elapsed < RESET_COOLDOWN_MS) {
    res.status(429).json({
      error: "reset_cooldown",
      retryAfterSec: Math.ceil((RESET_COOLDOWN_MS - elapsed) / 1000),
    });
    return false;
  }
  return true;
}

hwidRouter.post("/bind", requireAuth, limiter, (req: AuthedRequest, res) => {
  const parsed = z.object({ hwid: z.string().min(8).max(256) }).safeParse(req.body);
  if (!parsed.success) {
    res.status(400).json({ error: "invalid_body" });
    return;
  }
  const userId = req.user!.id;
  const existing = getHwid(userId);
  const h = hashHwid(parsed.data.hwid, config.hwidPepper);
  if (existing) {
    if (existing.hwid_hash !== h) {
      raiseSecurityAlert({
        severity: "high",
        category: "hwid",
        title: "HWID mismatch on bind",
        summary: "Caller presented a different fingerprint than the bound HWID",
        dedupeKey: `hwid_mismatch:bind:${userId}:${new Date().toISOString().slice(0, 10)}`,
        userId,
        hwidId: existing.id,
        evidence: { path: "hwid.bind" },
        ip: clientIp(req),
      });
      res.status(403).json({ error: "hwid_mismatch", hint: existing.hwid_hint });
      return;
    }
    db.prepare(`UPDATE hwids SET last_seen_at = ? WHERE user_id = ?`).run(nowIso(), userId);
    res.json({ hwid: publicHwid(getHwid(userId)) });
    return;
  }
  const t = nowIso();
  db.prepare(
    `INSERT INTO hwids (id, user_id, hwid_hash, hwid_hint, bound_at, last_seen_at, reset_count)
     VALUES (?, ?, ?, ?, ?, ?, 0)`
  ).run(cryptoRandomId(), userId, h, hwidHint(parsed.data.hwid), t, t);
  audit("hwid.bind", { actorId: userId, targetId: userId, ip: clientIp(req) });
  res.status(201).json({ hwid: publicHwid(getHwid(userId)) });
});

/**
 * Rebind to a new fingerprint without burning a self-reset, but only for a
 * caller that can already prove it owns the currently bound one. Lets the
 * launcher change its HWID algorithm without locking existing users out.
 */
hwidRouter.post("/migrate", requireAuth, limiter, (req: AuthedRequest, res) => {
  const parsed = z
    .object({
      from: z.string().min(8).max(256),
      to: z.string().min(8).max(256),
    })
    .safeParse(req.body);
  if (!parsed.success) {
    res.status(400).json({ error: "invalid_body" });
    return;
  }
  const userId = req.user!.id;
  const existing = getHwid(userId);
  if (!existing) {
    res.status(404).json({ error: "not_bound" });
    return;
  }
  if (existing.hwid_hash !== hashHwid(parsed.data.from, config.hwidPepper)) {
    res.status(403).json({ error: "hwid_mismatch", hint: existing.hwid_hint });
    return;
  }
  db.prepare(`UPDATE hwids SET hwid_hash = ?, hwid_hint = ?, last_seen_at = ? WHERE user_id = ?`).run(
    hashHwid(parsed.data.to, config.hwidPepper),
    hwidHint(parsed.data.to),
    nowIso(),
    userId
  );
  revokeProtectionForUser(userId, "hwid_migrated");
  audit("hwid.migrate", {
    actorId: userId,
    targetId: userId,
    meta: { previousHint: existing.hwid_hint },
    ip: clientIp(req),
  });
  res.json({ hwid: publicHwid(getHwid(userId)) });
});

/**
 * Step 1: prove password ownership and email a one-time confirm link.
 * Session alone is no longer enough to move a license to a new machine.
 */
hwidRouter.post("/reset/request", requireAuth, limiter, async (req: AuthedRequest, res) => {
  const parsed = z.object({ password: z.string().min(1).max(128) }).safeParse(req.body);
  if (!parsed.success) {
    res.status(400).json({ error: "invalid_body" });
    return;
  }
  const user = req.user!;
  const existing = getHwid(user.id);
  if (!existing) {
    res.status(404).json({ error: "not_bound" });
    return;
  }
  if (!assertCooldown(user.id, res)) return;
  if (!(await verifyPassword(user.password_hash, parsed.data.password))) {
    res.status(403).json({ error: "wrong_password" });
    return;
  }

  const { raw } = createHwidResetToken(user.id);
  const link = `${config.publicSiteUrl}/account?hwid_reset=${encodeURIComponent(raw)}`;
  await sendMail({
    to: user.email,
    subject: "Oak HWID reset confirmation",
    text:
      `Confirm your Oak hardware ID reset (valid 1 hour):\n\n${link}\n\n` +
      `If you did not request this, change your password and contact support.`,
  });
  audit("hwid.reset_request", {
    actorId: user.id,
    targetId: user.id,
    meta: { previousHint: existing.hwid_hint },
    ip: clientIp(req),
  });
  raiseSecurityAlert({
    severity: "medium",
    category: "hwid",
    title: "HWID reset requested",
    summary: "Account requested an email-confirmed HWID clear",
    dedupeKey: `hwid_reset_req:${user.id}:${new Date().toISOString().slice(0, 10)}`,
    userId: user.id,
    hwidId: existing.id,
    evidence: { path: "hwid.reset.request" },
    ip: clientIp(req),
  });
  res.json({ ok: true, message: "If eligible, a confirmation email was sent." });
});

/** Step 2: consume the emailed token and clear the binding. */
hwidRouter.post("/reset/confirm", limiter, (req, res) => {
  const parsed = z.object({ token: z.string().min(20).max(256) }).safeParse(req.body);
  if (!parsed.success) {
    res.status(400).json({ error: "invalid_body" });
    return;
  }
  const userId = consumeHwidResetToken(parsed.data.token);
  if (!userId) {
    res.status(400).json({ error: "invalid_or_expired_token" });
    return;
  }
  const existing = getHwid(userId);
  if (!existing) {
    res.status(404).json({ error: "not_bound" });
    return;
  }
  if (!assertCooldown(userId, res)) return;

  const resetTx = db.transaction(() => {
    revokeProtectionForUser(userId, "hwid_reset");
    db.prepare(`DELETE FROM hwids WHERE user_id = ?`).run(userId);
  });
  resetTx();
  audit("hwid.reset_self", {
    actorId: userId,
    targetId: userId,
    meta: { previousHint: existing.hwid_hint, resetCount: existing.reset_count + 1 },
    ip: clientIp(req),
  });
  raiseSecurityAlert({
    severity: "high",
    category: "hwid",
    title: "HWID self-reset completed",
    summary: "Hardware binding cleared after email confirmation — rebind on next login",
    dedupeKey: `hwid_reset_ok:${userId}:${new Date().toISOString().slice(0, 13)}`,
    userId,
    evidence: { path: "hwid.reset.confirm", previousHint: existing.hwid_hint },
    ip: clientIp(req),
  });
  res.json({ ok: true, message: "hwid_cleared_bind_on_next_login" });
});

/**
 * Legacy path — disabled. Callers must use /reset/request + /reset/confirm.
 */
hwidRouter.post("/reset", requireAuth, limiter, (_req, res) => {
  res.status(410).json({
    error: "use_email_confirm",
    message: "POST /v1/hwid/reset/request then confirm via emailed link",
  });
});

hwidRouter.get("/status", requireAuth, (req: AuthedRequest, res) => {
  res.json({ hwid: publicHwid(getHwid(req.user!.id)) });
});
