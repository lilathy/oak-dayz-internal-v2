import { Router } from "express";
import { z } from "zod";
import {
  analyticsSummary,
  getAllSettings,
  getSetting,
  setSetting,
  trackEvent,
} from "../analytics.js";
import { clientIp, requireAdmin, type AuthedRequest } from "../middleware/auth.js";
import { getCurrentClient, getCurrentLauncher, publicClientMeta } from "./releases.js";
import { db } from "../db.js";

export const siteRouter = Router();

/**
 * Only absolute https URLs are ever handed to the browser. Anything else is
 * dropped so a compromised admin account cannot turn a settings field into a
 * script injection on every visitor's page.
 */
function safeHttpsUrl(value: string | undefined): string {
  if (!value) return "";
  try {
    const url = new URL(value.trim());
    return url.protocol === "https:" ? url.toString() : "";
  } catch {
    return "";
  }
}

/** Public site bootstrap: announcement, maintenance, discord, analytics, changelog. */
siteRouter.get("/bootstrap", (_req, res) => {
  const s = getAllSettings();
  const dayz = getCurrentClient("dayz");
  const launcher = getCurrentLauncher();
  const analyticsSrc = safeHttpsUrl(s.analytics_src);
  res.json({
    announcement: s.announcement || "",
    maintenance: s.maintenance === "1" || s.maintenance === "true",
    discordUrl: safeHttpsUrl(s.discord_url),
    analytics: analyticsSrc
      ? { src: analyticsSrc, domain: (s.analytics_domain || "").slice(0, 190) }
      : null,
    changelogExtra: s.changelog_extra || "",
    dayz: publicClientMeta(dayz),
    launcher: launcher
      ? {
          version: launcher.version,
          sizeBytes: launcher.size_bytes,
          uploadedAt: launcher.created_at,
        }
      : null,
  });
});

siteRouter.post("/event", (req, res) => {
  const parsed = z
    .object({
      event: z.enum(["page_view", "click"]),
      path: z.string().max(200).optional(),
    })
    .safeParse(req.body);
  if (!parsed.success) {
    res.status(400).json({ error: "invalid_body" });
    return;
  }
  trackEvent(parsed.data.event, {
    path: parsed.data.path ?? null,
    ip: clientIp(req as AuthedRequest),
  });
  res.json({ ok: true });
});

export const adminSiteRouter = Router();
adminSiteRouter.use(requireAdmin);

adminSiteRouter.get("/settings", (_req, res) => {
  res.json({ settings: getAllSettings() });
});

adminSiteRouter.patch("/settings", (req: AuthedRequest, res) => {
  const parsed = z
    .object({
      announcement: z.string().max(2000).optional(),
      maintenance: z.union([z.boolean(), z.enum(["0", "1", "true", "false"])]).optional(),
      discord_url: z.union([z.literal(""), z.string().url().max(500)]).optional(),
      // A script URL, not markup: the tag is built by the site, not the admin.
      analytics_src: z.union([z.literal(""), z.string().url().max(500)]).optional(),
      analytics_domain: z.string().max(190).optional(),
      changelog_extra: z.string().max(20000).optional(),
      alert_discord_webhook: z.union([z.literal(""), z.string().url().max(500)]).optional(),
      alert_min_severity: z.enum(["low", "medium", "high", "critical"]).optional(),
    })
    .safeParse(req.body);
  if (!parsed.success) {
    res.status(400).json({ error: "invalid_body", details: parsed.error.flatten() });
    return;
  }
  const d = parsed.data;
  for (const [key, value] of [
    ["discord_url", d.discord_url],
    ["analytics_src", d.analytics_src],
  ] as const) {
    if (value === undefined) continue;
    if (value !== "" && !safeHttpsUrl(value)) {
      res.status(400).json({ error: "https_url_required", field: key });
      return;
    }
  }
  if (d.alert_discord_webhook !== undefined && d.alert_discord_webhook !== "") {
    try {
      const u = new URL(d.alert_discord_webhook);
      const host = u.hostname.toLowerCase();
      const okHost =
        host === "discord.com" || host === "discordapp.com" || host.endsWith(".discord.com");
      if (u.protocol !== "https:" || !okHost || !u.pathname.startsWith("/api/webhooks/")) {
        res.status(400).json({ error: "invalid_discord_webhook" });
        return;
      }
    } catch {
      res.status(400).json({ error: "invalid_discord_webhook" });
      return;
    }
  }

  if (d.announcement !== undefined) setSetting("announcement", d.announcement);
  if (d.maintenance !== undefined) {
    const on = d.maintenance === true || d.maintenance === "1" || d.maintenance === "true";
    setSetting("maintenance", on ? "1" : "0");
  }
  if (d.discord_url !== undefined) setSetting("discord_url", d.discord_url);
  if (d.analytics_src !== undefined) setSetting("analytics_src", d.analytics_src);
  if (d.analytics_domain !== undefined)
    setSetting("analytics_domain", d.analytics_domain.replace(/[^a-zA-Z0-9.\-_,]/g, ""));
  if (d.changelog_extra !== undefined) setSetting("changelog_extra", d.changelog_extra);
  if (d.alert_discord_webhook !== undefined) setSetting("alert_discord_webhook", d.alert_discord_webhook);
  if (d.alert_min_severity !== undefined) setSetting("alert_min_severity", d.alert_min_severity);
  res.json({ settings: getAllSettings() });
});

adminSiteRouter.get("/analytics", (req, res) => {
  const days = Math.min(Number(req.query.days ?? 7) || 7, 90);
  res.json({ summary: analyticsSummary(days) });
});

adminSiteRouter.get("/online", (_req, res) => {
  const since = new Date(Date.now() - 15 * 60 * 1000).toISOString();
  const rows = db
    .prepare(
      `SELECT id, username, email, role, last_seen_at FROM users
       WHERE last_seen_at >= ? ORDER BY last_seen_at DESC LIMIT 100`
    )
    .all(since);
  res.json({ online: rows, windowMinutes: 15 });
});

adminSiteRouter.get("/downloads", (req, res) => {
  const limit = Math.min(Number(req.query.limit ?? 100) || 100, 500);
  const rows = db
    .prepare(
      `SELECT id, kind, user_id, product_slug, version, ip, created_at
       FROM download_log ORDER BY created_at DESC LIMIT ?`
    )
    .all(limit);
  res.json({ downloads: rows });
});

export function publicSetting(key: string): string {
  return getSetting(key);
}
