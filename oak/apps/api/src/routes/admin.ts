import { Router } from "express";
import { z } from "zod";
import { createRedeemCode, publicRedeemCode, revokeCode, type RedeemCodeRow } from "../codes.js";
import { audit, db, nowIso, type UserRow } from "../db.js";
import {
  addLicenseDays,
  getHwid,
  getLicense,
  publicHwid,
  publicLicense,
  publicUser,
  syncLicenseStatus,
} from "../license.js";
import { isPlanId, listPlans } from "../plans.js";
import { clientIp, requireAdmin, type AuthedRequest } from "../middleware/auth.js";
import { analyticsSummary } from "../analytics.js";
import { getCurrentClient, getCurrentLauncher } from "./releases.js";
import { revokeAllRefreshTokens } from "../tokens.js";
import { revokeProtectionForUser } from "../protection.js";
import { alertCounts } from "../securityAlerts.js";
import { listPurchases } from "../purchases.js";

export const adminRouter = Router();
adminRouter.use(requireAdmin);

adminRouter.get("/overview", (_req, res) => {
  const users = (db.prepare(`SELECT COUNT(*) AS c FROM users`).get() as { c: number }).c;
  const active = (
    db.prepare(`SELECT COUNT(*) AS c FROM licenses WHERE status = 'active'`).get() as { c: number }
  ).c;
  const banned = (db.prepare(`SELECT COUNT(*) AS c FROM users WHERE banned = 1`).get() as { c: number }).c;
  const bound = (db.prepare(`SELECT COUNT(*) AS c FROM hwids`).get() as { c: number }).c;
  const unusedCodes = (
    db.prepare(`SELECT COUNT(*) AS c FROM redeem_codes WHERE status = 'unused'`).get() as { c: number }
  ).c;
  const recent = db
    .prepare(
      `SELECT id, actor_id, action, target_id, meta_json, ip, created_at
       FROM audit_log ORDER BY created_at DESC LIMIT 50`
    )
    .all();
  const client = getCurrentClient("dayz");
  const launcher = getCurrentLauncher();
  res.json({
    stats: { users, activeLicenses: active, bannedUsers: banned, hwidBound: bound, unusedCodes },
    analytics: analyticsSummary(7),
    alerts: alertCounts(),
    plans: listPlans(),
    currentClient: client
      ? { version: client.version, uploadedAt: client.created_at, sha256: client.sha256 }
      : null,
    currentLauncher: launcher
      ? { version: launcher.version, uploadedAt: launcher.created_at }
      : null,
    recentAudit: recent,
  });
});

adminRouter.get("/users", (req, res) => {
  const q = typeof req.query.q === "string" ? req.query.q.trim() : "";
  const rows = q
    ? (db
        .prepare(
          `SELECT * FROM users
           WHERE email LIKE ? OR username LIKE ?
           ORDER BY created_at DESC LIMIT 100`
        )
        .all(`%${q}%`, `%${q}%`) as UserRow[])
    : (db.prepare(`SELECT * FROM users ORDER BY created_at DESC LIMIT 100`).all() as UserRow[]);

  res.json({
    users: rows.map((u) => ({
      ...publicUser(u),
      license: publicLicense(syncLicenseStatus(u.id)),
      hwid: publicHwid(getHwid(u.id)),
    })),
  });
});

adminRouter.get("/users/:id", (req, res) => {
  const u = db.prepare(`SELECT * FROM users WHERE id = ?`).get(req.params.id) as UserRow | undefined;
  if (!u) {
    res.status(404).json({ error: "not_found" });
    return;
  }
  res.json({
    user: publicUser(u),
    license: publicLicense(syncLicenseStatus(u.id)),
    hwid: publicHwid(getHwid(u.id)),
  });
});

adminRouter.post("/users/:id/ban", (req: AuthedRequest, res) => {
  const body = z.object({ reason: z.string().max(500).optional() }).safeParse(req.body ?? {});
  const id = req.params.id;
  const u = db.prepare(`SELECT * FROM users WHERE id = ?`).get(id) as UserRow | undefined;
  if (!u) {
    res.status(404).json({ error: "not_found" });
    return;
  }
  db.prepare(`UPDATE users SET banned = 1, ban_reason = ?, updated_at = ? WHERE id = ?`).run(
    body.success ? body.data.reason ?? "banned" : "banned",
    nowIso(),
    id
  );
  db.prepare(`UPDATE licenses SET status = 'banned', updated_at = ? WHERE user_id = ?`).run(nowIso(), id);
  revokeAllRefreshTokens(id);
  revokeProtectionForUser(id, "banned");
  audit("admin.ban", {
    actorId: req.user!.id,
    targetId: id,
    meta: { reason: body.success ? body.data.reason : null },
    ip: clientIp(req),
  });
  res.json({ ok: true });
});

adminRouter.post("/users/:id/unban", (req: AuthedRequest, res) => {
  const id = req.params.id;
  const u = db.prepare(`SELECT * FROM users WHERE id = ?`).get(id) as UserRow | undefined;
  if (!u) {
    res.status(404).json({ error: "not_found" });
    return;
  }
  db.prepare(`UPDATE users SET banned = 0, ban_reason = NULL, updated_at = ? WHERE id = ?`).run(
    nowIso(),
    id
  );
  const lic = getLicense(id);
  if (lic?.status === "banned") {
    const status =
      lic.expires_at && new Date(lic.expires_at).getTime() > Date.now() ? "active" : "inactive";
    db.prepare(`UPDATE licenses SET status = ?, updated_at = ? WHERE user_id = ?`).run(
      status,
      nowIso(),
      id
    );
  }
  audit("admin.unban", { actorId: req.user!.id, targetId: id, ip: clientIp(req) });
  res.json({ ok: true });
});

adminRouter.post("/users/:id/license", (req: AuthedRequest, res) => {
  const parsed = z
    .object({
      plan: z.string().min(1).max(64).optional(),
      planId: z.string().optional(),
      days: z.number().int().min(1).max(3650).optional(),
      status: z.enum(["inactive", "active", "expired", "banned"]).optional(),
    })
    .safeParse(req.body);
  if (!parsed.success) {
    res.status(400).json({ error: "invalid_body", details: parsed.error.flatten() });
    return;
  }
  const id = req.params.id;
  const u = db.prepare(`SELECT id FROM users WHERE id = ?`).get(id);
  if (!u) {
    res.status(404).json({ error: "not_found" });
    return;
  }

  let days = parsed.data.days;
  let planLabel = parsed.data.plan ?? "manual";
  if (parsed.data.planId && isPlanId(parsed.data.planId)) {
    const fromPlan = parsed.data.planId;
    days = days ?? (fromPlan === "day1" ? 1 : fromPlan === "day7" ? 7 : 30);
    planLabel = fromPlan;
  }
  days = days ?? 30;

  try {
    const license = addLicenseDays(id, days, planLabel);
    if (parsed.data.status) {
      db.prepare(`UPDATE licenses SET status = ?, updated_at = ? WHERE user_id = ?`).run(
        parsed.data.status,
        nowIso(),
        id
      );
    }
    audit("admin.license_set", {
      actorId: req.user!.id,
      targetId: id,
      meta: { ...parsed.data, days, plan: planLabel },
      ip: clientIp(req),
    });
    res.json({ license: publicLicense(syncLicenseStatus(id) ?? license) });
  } catch (e) {
    const msg = e instanceof Error ? e.message : "error";
    if (msg === "license_banned") {
      res.status(403).json({ error: "license_banned" });
      return;
    }
    throw e;
  }
});

adminRouter.post("/users/:id/hwid/reset", (req: AuthedRequest, res) => {
  const id = req.params.id;
  const existing = getHwid(id);
  const resetTx = db.transaction(() => {
    revokeProtectionForUser(id, "admin_hwid_reset");
    db.prepare(`DELETE FROM hwids WHERE user_id = ?`).run(id);
  });
  resetTx();
  audit("admin.hwid_reset", {
    actorId: req.user!.id,
    targetId: id,
    meta: { previousHint: existing?.hwid_hint ?? null },
    ip: clientIp(req),
  });
  res.json({ ok: true });
});

adminRouter.get("/plans", (_req, res) => {
  res.json({ plans: listPlans() });
});

adminRouter.post("/codes", (req: AuthedRequest, res) => {
  const parsed = z
    .object({
      planId: z.enum(["day1", "day7", "day30"]),
      count: z.number().int().min(1).max(100).default(1),
      note: z.string().max(500).optional(),
      /** When set, only this account may redeem (gift/resale blocked). */
      buyerUserId: z.string().uuid().optional(),
    })
    .safeParse(req.body);
  if (!parsed.success) {
    res.status(400).json({ error: "invalid_body", details: parsed.error.flatten() });
    return;
  }
  const { planId, count, note, buyerUserId } = parsed.data;
  if (buyerUserId) {
    const buyer = db.prepare(`SELECT id FROM users WHERE id = ?`).get(buyerUserId);
    if (!buyer) {
      res.status(404).json({ error: "buyer_not_found" });
      return;
    }
  }
  const created: { id: string; code: string; hint: string; planId: string; durationDays: number }[] =
    [];
  for (let i = 0; i < count; i++) {
    const { row, raw } = createRedeemCode({
      planId,
      createdBy: "admin",
      createdByUserId: req.user!.id,
      buyerUserId: buyerUserId ?? null,
      note: note ?? null,
    });
    created.push({
      id: row.id,
      code: raw,
      hint: row.code_hint,
      planId: row.plan_id,
      durationDays: row.duration_days,
    });
  }
  audit("admin.codes_create", {
    actorId: req.user!.id,
    meta: { planId, count, note: note ?? null, buyerUserId: buyerUserId ?? null },
    ip: clientIp(req),
  });
  res.status(201).json({ codes: created });
});

adminRouter.get("/codes", (req, res) => {
  const status = typeof req.query.status === "string" ? req.query.status : "";
  const planId = typeof req.query.planId === "string" ? req.query.planId : "";
  const limit = Math.min(Number(req.query.limit ?? 100) || 100, 500);

  let sql = `SELECT * FROM redeem_codes WHERE 1=1`;
  const params: unknown[] = [];
  if (status === "unused" || status === "redeemed" || status === "revoked") {
    sql += ` AND status = ?`;
    params.push(status);
  }
  if (planId && isPlanId(planId)) {
    sql += ` AND plan_id = ?`;
    params.push(planId);
  }
  sql += ` ORDER BY created_at DESC LIMIT ?`;
  params.push(limit);

  const rows = db.prepare(sql).all(...params) as RedeemCodeRow[];
  res.json({ codes: rows.map(publicRedeemCode) });
});

adminRouter.post("/codes/:id/revoke", (req: AuthedRequest, res) => {
  const before = db
    .prepare(`SELECT * FROM redeem_codes WHERE id = ?`)
    .get(req.params.id) as RedeemCodeRow | undefined;
  if (!before) {
    res.status(404).json({ error: "not_found" });
    return;
  }
  if (before.status !== "unused") {
    res.status(409).json({ error: "not_revokable", code: publicRedeemCode(before) });
    return;
  }
  const row = revokeCode(req.params.id, req.user!.id, clientIp(req));
  res.json({ code: publicRedeemCode(row!) });
});

adminRouter.get("/audit", (req, res) => {
  const limit = Math.min(Number(req.query.limit ?? 100) || 100, 500);
  const rows = db
    .prepare(
      `SELECT id, actor_id, action, target_id, meta_json, ip, created_at
       FROM audit_log ORDER BY created_at DESC LIMIT ?`
    )
    .all(limit);
  res.json({ audit: rows });
});

adminRouter.get("/purchases", (req, res) => {
  const limit = Math.min(Number(req.query.limit ?? 100) || 100, 500);
  res.json({
    purchases: listPurchases(limit).map((p) => ({
      id: p.id,
      userId: p.user_id,
      email: p.email,
      username: p.username,
      planId: p.plan_id,
      provider: p.provider,
      status: p.status,
      amountUsd: p.amount_usd,
      providerRef: p.provider_ref,
      createdAt: p.created_at,
      paidAt: p.paid_at,
    })),
  });
});
