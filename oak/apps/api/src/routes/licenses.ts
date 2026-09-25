import { Router } from "express";
import { z } from "zod";
import rateLimit from "express-rate-limit";
import { findCodeByRaw } from "../codes.js";
import { audit, db, nowIso } from "../db.js";
import { addLicenseDays, publicLicense, syncLicenseStatus } from "../license.js";
import { trackEvent } from "../analytics.js";
import { clientIp, requireAuth, type AuthedRequest } from "../middleware/auth.js";

export const licensesRouter = Router();

// Keyed on the account, not the IP: rotating IPs must not buy extra guesses
// against the redeem-code space.
const redeemLimiter = rateLimit({
  windowMs: 15 * 60 * 1000,
  max: 15,
  standardHeaders: true,
  legacyHeaders: false,
  keyGenerator: (req) => (req as AuthedRequest).user?.id ?? clientIp(req),
  message: { error: "rate_limited" },
});

licensesRouter.get("/me", requireAuth, (req: AuthedRequest, res) => {
  res.json({ license: publicLicense(syncLicenseStatus(req.user!.id)) });
});

licensesRouter.post("/redeem", requireAuth, redeemLimiter, (req: AuthedRequest, res) => {
  const parsed = z.object({ code: z.string().min(8).max(64) }).safeParse(req.body);
  if (!parsed.success) {
    res.status(400).json({ error: "invalid_body" });
    return;
  }
  const userId = req.user!.id;
  if (req.user!.banned) {
    res.status(403).json({ error: "banned" });
    return;
  }

  const row = findCodeByRaw(parsed.data.code);
  if (!row) {
    res.status(404).json({ error: "invalid_code" });
    return;
  }
  if (row.status === "revoked") {
    res.status(410).json({ error: "code_revoked" });
    return;
  }
  if (row.status === "redeemed") {
    res.status(409).json({ error: "code_already_used" });
    return;
  }
  // Paid codes are bound to the buyer at fulfill time — block gift/resale to
  // other accounts (admin-minted codes leave buyer_user_id null).
  if (row.buyer_user_id && row.buyer_user_id !== userId) {
    res.status(403).json({ error: "code_not_for_this_account" });
    return;
  }

  try {
    const tx = db.transaction(() => {
      const updated = db
        .prepare(
          `UPDATE redeem_codes
           SET status = 'redeemed', redeemed_by_user_id = ?, redeemed_at = ?
           WHERE id = ? AND status = 'unused'`
        )
        .run(userId, nowIso(), row.id);
      if (updated.changes !== 1) {
        throw new Error("code_race");
      }
      return addLicenseDays(userId, row.duration_days, row.plan_id);
    });
    const license = tx();
    audit("license.redeem", {
      actorId: userId,
      targetId: row.id,
      meta: { planId: row.plan_id, days: row.duration_days },
      ip: clientIp(req),
    });
    trackEvent("redeem", {
      userId,
      meta: { planId: row.plan_id, days: row.duration_days },
      ip: clientIp(req),
    });
    res.json({
      ok: true,
      creditedDays: row.duration_days,
      planId: row.plan_id,
      license: publicLicense(license),
    });
  } catch (e) {
    const msg = e instanceof Error ? e.message : "error";
    if (msg === "license_banned") {
      res.status(403).json({ error: "license_banned" });
      return;
    }
    if (msg === "code_race") {
      res.status(409).json({ error: "code_already_used" });
      return;
    }
    console.error(e);
    res.status(500).json({ error: "internal" });
  }
});
