import crypto from "node:crypto";
import { Router } from "express";
import rateLimit from "express-rate-limit";
import { z } from "zod";
import { isMaintenance } from "../analytics.js";
import { hashHwid } from "../crypto.js";
import { audit, cryptoRandomId, db, nowIso } from "../db.js";
import {
  createBootstrapTicket,
  getActiveRuntimePackage,
  ProtectionError,
  renewLease,
  runtimePackageForLease,
  signRuntimePackage,
  consumeBootstrapTicket,
  type RuntimePackageRow,
} from "../protection.js";
import { config } from "../config.js";
import { clientIp, requireAdmin, requireAuth, type AuthedRequest } from "../middleware/auth.js";
import { alertFromProtectionFailure } from "../securityAlerts.js";

export const protectionRouter = Router();
export const adminProtectionRouter = Router();

const protectionLimiter = rateLimit({
  windowMs: 10 * 60 * 1000,
  max: 30,
  standardHeaders: true,
  legacyHeaders: false,
  keyGenerator: (req) => (req as AuthedRequest).user?.id ?? clientIp(req),
  message: { error: "rate_limited" },
});

function protectionError(
  res: import("express").Response,
  err: unknown,
  ctx: { userId?: string | null; ip?: string | null; path?: string; slug?: string } = {}
): boolean {
  if (!(err instanceof ProtectionError)) return false;
  alertFromProtectionFailure({
    error: err.error,
    userId: ctx.userId,
    slug: ctx.slug,
    ip: ctx.ip,
    path: ctx.path,
  });
  res.status(err.status).json({ error: err.error });
  return true;
}

/**
 * Launcher-only: a normal authenticated launcher session requests a short,
 * single-use handoff ticket after proving its current HWID.
 */
protectionRouter.post("/:slug/bootstrap", requireAuth, protectionLimiter, (req: AuthedRequest, res) => {
  const parsed = z.object({ hwid: z.string().min(8).max(256) }).safeParse(req.body);
  if (!parsed.success || !req.authSessionId) {
    res.status(400).json({ error: "invalid_body" });
    return;
  }
  try {
    const created = createBootstrapTicket({
      user: req.user!,
      sessionId: req.authSessionId,
      slug: req.params.slug,
      hwidHash: hashHwid(parsed.data.hwid, config.hwidPepper),
    });
    audit("protection.bootstrap_issue", {
      actorId: req.user!.id,
      targetId: created.release.id,
      meta: { slug: req.params.slug, sessionId: req.authSessionId, expiresAt: created.expiresAt },
      ip: clientIp(req),
    });
    res.json({
      bootstrapTicket: created.ticket,
      expiresAt: created.expiresAt,
      release: { id: created.release.id, version: created.release.version },
      runtimePackageVersion: created.runtimePackageVersion,
    });
  } catch (err) {
    if (
      !protectionError(res, err, {
        userId: req.user?.id,
        ip: clientIp(req),
        path: "bootstrap",
        slug: req.params.slug,
      })
    )
      throw err;
  }
});

/** Client bootstrap only. This is intentionally not a bearer-token endpoint. */
protectionRouter.post("/:slug/lease", protectionLimiter, (req, res) => {
  const parsed = z
    .object({
      bootstrapTicket: z.string().min(32).max(512),
      hwid: z.string().min(8).max(256),
      clientNonce: z.string().min(16).max(256),
    })
    .safeParse(req.body);
  if (!parsed.success) {
    res.status(400).json({ error: "invalid_body" });
    return;
  }
  try {
    const lease = consumeBootstrapTicket({
      ticket: parsed.data.bootstrapTicket,
      slug: req.params.slug,
      hwidHash: hashHwid(parsed.data.hwid, config.hwidPepper),
      clientNonce: parsed.data.clientNonce,
    });
    audit("protection.lease_issue", {
      targetId: lease.leaseId,
      meta: { slug: req.params.slug, expiresAt: lease.expiresAt },
      ip: clientIp(req),
    });
    res.json(lease);
  } catch (err) {
    if (
      !protectionError(res, err, {
        ip: clientIp(req),
        path: "lease",
        slug: req.params.slug,
      })
    )
      throw err;
  }
});

/** Client-only short lease renewal. */
protectionRouter.post("/:slug/lease/renew", protectionLimiter, (req, res) => {
  const parsed = z
    .object({
      leaseToken: z.string().min(32).max(512),
      clientNonce: z.string().min(16).max(256),
      challengeResponse: z.string().regex(/^[0-9a-fA-F]{64}$/),
    })
    .safeParse(req.body);
  if (!parsed.success) {
    res.status(400).json({ error: "invalid_body" });
    return;
  }
  try {
    const lease = renewLease({ ...parsed.data, slug: req.params.slug });
    audit("protection.lease_renew", {
      targetId: lease.leaseId,
      meta: { slug: req.params.slug, expiresAt: lease.expiresAt },
      ip: clientIp(req),
    });
    res.json(lease);
  } catch (err) {
    if (
      !protectionError(res, err, {
        ip: clientIp(req),
        path: "lease_renew",
        slug: req.params.slug,
      })
    )
      throw err;
  }
});

/** Client-only current package endpoint. */
protectionRouter.post("/:slug/runtime-package", protectionLimiter, (req, res) => {
  const parsed = z.object({ leaseToken: z.string().min(32).max(512) }).safeParse(req.body);
  if (!parsed.success) {
    res.status(400).json({ error: "invalid_body" });
    return;
  }
  try {
    res.json({ runtimePackage: runtimePackageForLease(parsed.data.leaseToken, req.params.slug) });
  } catch (err) {
    if (
      !protectionError(res, err, {
        ip: clientIp(req),
        path: "runtime_package",
        slug: req.params.slug,
      })
    )
      throw err;
  }
});

/** Public verification key lets the client authenticate signed runtime packages. */
protectionRouter.get("/:slug/runtime-package-key", (_req, res) => {
  res.json({
    algorithm: "ecdsa-p256-sha256",
    keyId: config.runtimePackageKeyId,
    publicKey: config.runtimePackagePublicKey,
  });
});

// ------------------------------------------------------------------ admin

adminProtectionRouter.use(requireAdmin);

adminProtectionRouter.get("/protection/runtime-packages/:slug", (req, res) => {
  const packages = db
    .prepare(
      `SELECT id, product_slug, client_release_id, version, payload_sha256, signature, status,
              created_by, created_at, activated_at, revoked_at
       FROM runtime_packages WHERE product_slug = ? ORDER BY created_at DESC LIMIT 50`
    )
    .all(req.params.slug);
  res.json({ packages, active: getActiveRuntimePackage(req.params.slug) ?? null });
});

adminProtectionRouter.post("/protection/runtime-packages/:slug", (req: AuthedRequest, res) => {
  const parsed = z
    .object({
      version: z.string().trim().min(1).max(64),
      payload: z.record(z.string(), z.unknown()),
      clientReleaseId: z.string().uuid().optional(),
    })
    .safeParse(req.body);
  if (!parsed.success) {
    res.status(400).json({ error: "invalid_body", details: parsed.error.flatten() });
    return;
  }
  const product = db.prepare(`SELECT 1 FROM products WHERE slug = ?`).get(req.params.slug);
  if (!product) {
    res.status(404).json({ error: "not_found" });
    return;
  }
  if (parsed.data.clientReleaseId) {
    const release = db
      .prepare(`SELECT 1 FROM client_releases WHERE id = ? AND product_slug = ?`)
      .get(parsed.data.clientReleaseId, req.params.slug);
    if (!release) {
      res.status(400).json({ error: "invalid_release" });
      return;
    }
  }
  const id = cryptoRandomId();
  const issuedAt = nowIso();
  const clientReleaseId =
    parsed.data.clientReleaseId ??
    ((db
      .prepare(`SELECT id FROM client_releases WHERE product_slug = ? AND is_current = 1 LIMIT 1`)
      .get(req.params.slug) as { id: string } | undefined)?.id ?? "");
  if (!clientReleaseId) {
    res.status(400).json({ error: "no_current_release" });
    return;
  }
  // Sign the complete compatibility envelope, not just free-form values, so a
  // valid package cannot be replayed across products or client releases.
  const payloadJson = JSON.stringify({
    schemaVersion: 1,
    productSlug: req.params.slug,
    clientReleaseId,
    runtimePackageId: id,
    issuedAt,
    expiresAt: new Date(Date.now() + 30 * 24 * 60 * 60 * 1000).toISOString(),
    featureCompatibilityFlags: { enabled: true },
    runtimeValues: parsed.data.payload,
  });
  const sha256 = crypto.createHash("sha256").update(payloadJson).digest("hex");
  const signature = signRuntimePackage(payloadJson);
  db.prepare(
    `INSERT INTO runtime_packages
     (id, product_slug, client_release_id, version, payload_json, payload_sha256, signature, status,
      created_by, created_at, activated_at, revoked_at)
     VALUES (?, ?, ?, ?, ?, ?, ?, 'staged', ?, ?, NULL, NULL)`
  ).run(
    id,
    req.params.slug,
    clientReleaseId,
    parsed.data.version,
    payloadJson,
    sha256,
    signature,
    req.user!.id,
    issuedAt
  );
  audit("protection.runtime_package_stage", {
    actorId: req.user!.id,
    targetId: id,
    meta: { slug: req.params.slug, version: parsed.data.version, sha256 },
    ip: clientIp(req),
  });
  res.status(201).json({ id, version: parsed.data.version, sha256, signature, status: "staged" });
});

adminProtectionRouter.post("/protection/runtime-packages/:id/activate", (req: AuthedRequest, res) => {
  const pkg = db.prepare(`SELECT * FROM runtime_packages WHERE id = ?`).get(req.params.id) as
    | RuntimePackageRow
    | undefined;
  if (!pkg || pkg.status === "revoked") {
    res.status(404).json({ error: "not_found" });
    return;
  }
  const t = nowIso();
  const tx = db.transaction(() => {
    db.prepare(`UPDATE runtime_packages SET status = 'staged' WHERE product_slug = ? AND status = 'active'`)
      .run(pkg.product_slug);
    db.prepare(`UPDATE runtime_packages SET status = 'active', activated_at = ?, revoked_at = NULL WHERE id = ?`)
      .run(t, pkg.id);
  });
  tx();
  audit("protection.runtime_package_activate", {
    actorId: req.user!.id,
    targetId: pkg.id,
    meta: { slug: pkg.product_slug, version: pkg.version },
    ip: clientIp(req),
  });
  res.json({ ok: true });
});

adminProtectionRouter.post("/protection/runtime-packages/:id/revoke", (req: AuthedRequest, res) => {
  const result = db
    .prepare(`UPDATE runtime_packages SET status = 'revoked', revoked_at = ? WHERE id = ? AND status != 'revoked'`)
    .run(nowIso(), req.params.id);
  if (result.changes !== 1) {
    res.status(404).json({ error: "not_found" });
    return;
  }
  audit("protection.runtime_package_revoke", {
    actorId: req.user!.id,
    targetId: req.params.id,
    ip: clientIp(req),
  });
  res.json({ ok: true });
});

adminProtectionRouter.post("/protection/leases/:id/revoke", (req: AuthedRequest, res) => {
  const result = db
    .prepare(`UPDATE client_leases SET revoked_at = ?, revoke_reason = 'admin' WHERE id = ? AND revoked_at IS NULL`)
    .run(nowIso(), req.params.id);
  if (result.changes !== 1) {
    res.status(404).json({ error: "not_found" });
    return;
  }
  audit("protection.lease_revoke", { actorId: req.user!.id, targetId: req.params.id, ip: clientIp(req) });
  res.json({ ok: true });
});

adminProtectionRouter.get("/protection/leases", (req, res) => {
  const limit = Math.min(Number(req.query.limit ?? 100) || 100, 500);
  const leases = db
    .prepare(
      `SELECT id, user_id, hwid_id, product_slug, client_release_id, runtime_package_id,
              expires_at, last_renewed_at, revoked_at, revoke_reason, created_at
       FROM client_leases ORDER BY created_at DESC LIMIT ?`
    )
    .all(limit);
  res.json({ leases, maintenance: isMaintenance() });
});
