import { Router } from "express";
import rateLimit from "express-rate-limit";
import { z } from "zod";
import { trackEvent, touchLastSeen } from "../analytics.js";
import { db } from "../db.js";
import { clientIp, optionalAuth, requireAuth, requireAdmin, type AuthedRequest } from "../middleware/auth.js";
import {
  alertCounts,
  alertFromProtectionFailure,
  ensureSecurityAlertsSchema,
  getSecurityAlert,
  listSecurityAlerts,
  raiseSecurityAlert,
  sanitizeEvidence,
  updateAlertStatus,
  type AlertSeverity,
  type AlertStatus,
} from "../securityAlerts.js";
import { getActiveLeaseByToken, ProtectionError } from "../protection.js";
import { audit } from "../db.js";

export { ensureSecurityAlertsSchema };

const WEB_EVENTS = [
  "web.page_view",
  "web.signup_start",
  "web.signup_ok",
  "web.signup_fail",
  "web.login_ok",
  "web.login_fail",
  "web.redeem_ok",
  "web.redeem_fail",
  "web.checkout_start",
  "web.checkout_ok",
  "web.checkout_fail",
  "web.download_click",
  "web.support_ticket_open",
] as const;

const LAUNCHER_EVENTS = [
  "launcher.start",
  "launcher.login_ok",
  "launcher.login_fail",
  "launcher.launch_start",
  "launcher.stage_ok",
  "launcher.stage_fail",
  "launcher.kernel_start",
  "launcher.dayz_start",
  "launcher.inject_ok",
  "launcher.inject_fail",
  "launcher.bootstrap_ok",
  "launcher.bootstrap_fail",
  "launcher.cancel",
] as const;

const CLIENT_EVENTS = [
  "client.boot_ok",
  "client.lease_renew_ok",
  "client.lease_renew_fail",
  "client.feature_gate_deny",
  "client.integrity_warn",
  "client.integrity_fail",
  "client.heartbeat",
] as const;

const metaSchema = z
  .record(z.union([z.string().max(200), z.number(), z.boolean()]))
  .optional()
  .refine((m) => !m || Object.keys(m).length <= 16, { message: "meta_too_large" });

function scrubMeta(meta: Record<string, string | number | boolean> | undefined) {
  return sanitizeEvidence(meta ?? {});
}

const webLimiter = rateLimit({
  windowMs: 60 * 1000,
  max: 60,
  standardHeaders: true,
  legacyHeaders: false,
  keyGenerator: (req) => clientIp(req),
  message: { error: "rate_limited" },
});

const launcherLimiter = rateLimit({
  windowMs: 60 * 1000,
  max: 40,
  standardHeaders: true,
  legacyHeaders: false,
  keyGenerator: (req) => (req as AuthedRequest).user?.id ?? clientIp(req),
  message: { error: "rate_limited" },
});

const clientLimiter = rateLimit({
  windowMs: 60 * 1000,
  max: 30,
  standardHeaders: true,
  legacyHeaders: false,
  keyGenerator: (req) => clientIp(req),
  message: { error: "rate_limited" },
});

export const telemetryRouter = Router();
export const adminAlertsRouter = Router();

telemetryRouter.post("/web", optionalAuth, webLimiter, (req: AuthedRequest, res) => {
  const parsed = z
    .object({
      event: z.enum(WEB_EVENTS),
      path: z.string().max(200).optional(),
      meta: metaSchema,
    })
    .safeParse(req.body);
  if (!parsed.success) {
    res.status(400).json({ error: "invalid_body" });
    return;
  }
  // Never trust a client-supplied user id — only the optional session.
  trackEvent(parsed.data.event, {
    userId: req.user?.id ?? null,
    path: parsed.data.path ?? null,
    meta: scrubMeta(parsed.data.meta),
    ip: clientIp(req),
  });
  if (req.user) touchLastSeen(req.user.id);
  res.json({ ok: true });
});

telemetryRouter.post("/launcher", requireAuth, launcherLimiter, (req: AuthedRequest, res) => {
  const parsed = z
    .object({
      event: z.enum(LAUNCHER_EVENTS),
      meta: metaSchema,
    })
    .safeParse(req.body);
  if (!parsed.success) {
    res.status(400).json({ error: "invalid_body" });
    return;
  }
  const meta = scrubMeta(parsed.data.meta);
  trackEvent(parsed.data.event, {
    userId: req.user!.id,
    path: "launcher",
    meta,
    ip: clientIp(req),
  });
  touchLastSeen(req.user!.id);

  if (parsed.data.event === "launcher.inject_fail") {
    const reason = String(meta.reasonCode || meta.reason || "unknown").slice(0, 80);
    // Reliability noise stays low unless it looks like protection abuse.
    if (/integrity|challenge|hwid|blocked|tamper/i.test(reason)) {
      raiseSecurityAlert({
        severity: "medium",
        category: "telemetry",
        title: "Launcher inject failure (security-related)",
        summary: reason,
        dedupeKey: `launcher.inject_fail:${req.user!.id}:${new Date().toISOString().slice(0, 10)}`,
        userId: req.user!.id,
        evidence: meta,
        ip: clientIp(req),
      });
    }
  }

  res.json({ ok: true });
});

telemetryRouter.post("/client", clientLimiter, (req, res) => {
  const parsed = z
    .object({
      leaseToken: z.string().min(32).max(512),
      event: z.enum(CLIENT_EVENTS),
      meta: metaSchema,
    })
    .safeParse(req.body);
  if (!parsed.success) {
    res.status(400).json({ error: "invalid_body" });
    return;
  }

  let lease;
  try {
    lease = getActiveLeaseByToken(parsed.data.leaseToken);
  } catch (err) {
    if (err instanceof ProtectionError) {
      res.status(err.status).json({ error: err.error });
      return;
    }
    throw err;
  }

  const meta = scrubMeta(parsed.data.meta);
  trackEvent(parsed.data.event, {
    userId: lease.user_id,
    path: `client:${lease.product_slug}`,
    meta: { ...meta, releaseId: lease.client_release_id },
    ip: clientIp(req),
  });
  touchLastSeen(lease.user_id);

  if (parsed.data.event === "client.integrity_fail") {
    raiseSecurityAlert({
      severity: "high",
      category: "integrity",
      title: "Client integrity failure",
      summary: String(meta.reasonCode || meta.reason || "integrity_fail").slice(0, 200),
      dedupeKey: `client.integrity_fail:${lease.user_id}:${new Date().toISOString().slice(0, 10)}`,
      userId: lease.user_id,
      hwidId: lease.hwid_id,
      releaseId: lease.client_release_id,
      evidence: meta,
      ip: clientIp(req),
    });
  } else if (parsed.data.event === "client.integrity_warn") {
    raiseSecurityAlert({
      severity: "medium",
      category: "integrity",
      title: "Client integrity warning",
      summary: String(meta.reasonCode || meta.reason || "integrity_warn").slice(0, 200),
      dedupeKey: `client.integrity_warn:${lease.user_id}:${new Date().toISOString().slice(0, 10)}`,
      userId: lease.user_id,
      hwidId: lease.hwid_id,
      releaseId: lease.client_release_id,
      evidence: meta,
      ip: clientIp(req),
    });
  } else if (parsed.data.event === "client.lease_renew_fail") {
    alertFromProtectionFailure({
      error: String(meta.reasonCode || "lease_renew_fail"),
      userId: lease.user_id,
      hwidId: lease.hwid_id,
      releaseId: lease.client_release_id,
      slug: lease.product_slug,
      ip: clientIp(req),
      path: "telemetry.client",
    });
  }

  res.json({ ok: true });
});

// ------------------------------------------------------------------ admin

adminAlertsRouter.use(requireAdmin);

adminAlertsRouter.get("/alerts", (req, res) => {
  const status = typeof req.query.status === "string" ? req.query.status : "open";
  const severity = typeof req.query.severity === "string" ? req.query.severity : "all";
  const limit = Number(req.query.limit ?? 50);
  const offset = Number(req.query.offset ?? 0);
  const allowedStatus = ["open", "acked", "resolved", "dismissed", "all"] as const;
  const allowedSeverity = ["low", "medium", "high", "critical", "all"] as const;
  if (!allowedStatus.includes(status as (typeof allowedStatus)[number])) {
    res.status(400).json({ error: "invalid_status" });
    return;
  }
  if (!allowedSeverity.includes(severity as (typeof allowedSeverity)[number])) {
    res.status(400).json({ error: "invalid_severity" });
    return;
  }
  res.json({
    counts: alertCounts(),
    alerts: listSecurityAlerts({
      status: status as AlertStatus | "all",
      severity: severity as AlertSeverity | "all",
      limit,
      offset,
    }),
  });
});

adminAlertsRouter.get("/alerts/:id", (req, res) => {
  const alert = getSecurityAlert(req.params.id);
  if (!alert) {
    res.status(404).json({ error: "not_found" });
    return;
  }
  res.json({ alert });
});

adminAlertsRouter.post("/alerts/:id/ack", (req: AuthedRequest, res) => {
  if (!updateAlertStatus({ id: req.params.id, status: "acked", actorId: req.user!.id })) {
    res.status(404).json({ error: "not_found" });
    return;
  }
  res.json({ ok: true, alert: getSecurityAlert(req.params.id) });
});

adminAlertsRouter.post("/alerts/:id/resolve", (req: AuthedRequest, res) => {
  const parsed = z.object({ note: z.string().max(500).optional() }).safeParse(req.body ?? {});
  if (!parsed.success) {
    res.status(400).json({ error: "invalid_body" });
    return;
  }
  if (
    !updateAlertStatus({
      id: req.params.id,
      status: "resolved",
      actorId: req.user!.id,
      note: parsed.data.note,
    })
  ) {
    res.status(404).json({ error: "not_found" });
    return;
  }
  res.json({ ok: true, alert: getSecurityAlert(req.params.id) });
});

adminAlertsRouter.post("/alerts/:id/dismiss", (req: AuthedRequest, res) => {
  const parsed = z.object({ note: z.string().max(500).optional() }).safeParse(req.body ?? {});
  if (!parsed.success) {
    res.status(400).json({ error: "invalid_body" });
    return;
  }
  if (
    !updateAlertStatus({
      id: req.params.id,
      status: "dismissed",
      actorId: req.user!.id,
      note: parsed.data.note,
    })
  ) {
    res.status(404).json({ error: "not_found" });
    return;
  }
  res.json({ ok: true, alert: getSecurityAlert(req.params.id) });
});

adminAlertsRouter.get("/telemetry/summary", (_req, res) => {
  const since = new Date(Date.now() - 24 * 3600 * 1000).toISOString();
  const count = (event: string) =>
    (
      db
        .prepare(`SELECT COUNT(*) AS c FROM analytics_events WHERE event = ? AND created_at >= ?`)
        .get(event, since) as { c: number }
    ).c;
  const injectOk = count("launcher.inject_ok");
  const injectFail = count("launcher.inject_fail");
  const launches = count("launcher.launch_start");
  res.json({
    last24h: {
      pageViews: count("web.page_view") + count("page_view"),
      logins: count("web.login_ok") + count("login"),
      registers: count("web.signup_ok") + count("register"),
      redeems: count("web.redeem_ok") + count("redeem"),
      launcherStarts: count("launcher.start"),
      launches,
      injectOk,
      injectFail,
      injectSuccessRate:
        injectOk + injectFail > 0 ? Number((injectOk / (injectOk + injectFail)).toFixed(3)) : null,
      clientHeartbeats: count("client.heartbeat"),
      integrityFails: count("client.integrity_fail"),
    },
    alerts: alertCounts(),
  });
});

adminAlertsRouter.post("/alerts/ingest-leak", (req: AuthedRequest, res) => {
  const parsed = z
    .object({
      distributionId: z.string().min(8).max(128),
      note: z.string().max(500).optional(),
      releaseId: z.string().max(64).optional(),
    })
    .safeParse(req.body);
  if (!parsed.success) {
    res.status(400).json({ error: "invalid_body" });
    return;
  }
  const raised = raiseSecurityAlert({
    severity: "critical",
    category: "leak",
    title: "Manual leak report",
    summary: parsed.data.note || "distributionId submitted for review",
    dedupeKey: `leak:${parsed.data.distributionId}`,
    distributionId: parsed.data.distributionId,
    releaseId: parsed.data.releaseId,
    evidence: { distributionId: parsed.data.distributionId, source: "admin_ingest" },
    ip: clientIp(req),
  });
  audit("security.leak_ingest", {
    actorId: req.user!.id,
    targetId: raised.id,
    meta: { distributionId: parsed.data.distributionId },
    ip: clientIp(req),
  });
  res.json({ ok: true, alertId: raised.id, created: raised.created });
});
