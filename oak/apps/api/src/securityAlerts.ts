import { cryptoRandomId, db, nowIso, audit } from "./db.js";
import { getSetting } from "./analytics.js";

export type AlertSeverity = "low" | "medium" | "high" | "critical";
export type AlertCategory =
  | "auth"
  | "hwid"
  | "protection"
  | "integrity"
  | "leak"
  | "payment"
  | "telemetry";
export type AlertStatus = "open" | "acked" | "resolved" | "dismissed";

const DEDUPE_WINDOW_MS = 30 * 60 * 1000;
const SEVERITY_RANK: Record<AlertSeverity, number> = {
  low: 1,
  medium: 2,
  high: 3,
  critical: 4,
};

export function ensureSecurityAlertsSchema(): void {
  db.exec(`
CREATE TABLE IF NOT EXISTS security_alerts (
  id              TEXT PRIMARY KEY,
  severity        TEXT NOT NULL CHECK(severity IN ('low','medium','high','critical')),
  category        TEXT NOT NULL,
  title           TEXT NOT NULL,
  summary         TEXT NOT NULL DEFAULT '',
  user_id         TEXT,
  hwid_id         TEXT,
  release_id      TEXT,
  distribution_id TEXT,
  evidence_json   TEXT,
  status          TEXT NOT NULL DEFAULT 'open'
                  CHECK(status IN ('open','acked','resolved','dismissed')),
  dedupe_key      TEXT NOT NULL,
  notify_state    TEXT NOT NULL DEFAULT 'pending'
                  CHECK(notify_state IN ('pending','sent','suppressed')),
  count           INTEGER NOT NULL DEFAULT 1,
  created_at      TEXT NOT NULL,
  updated_at      TEXT NOT NULL,
  resolved_at     TEXT,
  resolved_by     TEXT,
  resolve_note    TEXT
);
CREATE INDEX IF NOT EXISTS idx_alerts_status ON security_alerts(status, severity, created_at);
CREATE INDEX IF NOT EXISTS idx_alerts_dedupe ON security_alerts(dedupe_key, status, updated_at);
CREATE INDEX IF NOT EXISTS idx_alerts_user ON security_alerts(user_id, created_at);
`);

  const defaults: Record<string, string> = {
    alert_discord_webhook: "",
    alert_min_severity: "high",
    alert_cooldown_seconds: "60",
  };
  const upsert = db.prepare(
    `INSERT INTO site_settings (key, value, updated_at) VALUES (?, ?, ?)
     ON CONFLICT(key) DO NOTHING`
  );
  const t = nowIso();
  for (const [k, v] of Object.entries(defaults)) upsert.run(k, v, t);
}

/** Flat, bounded evidence bag — never store tokens, raw HWID, or free-form dumps. */
export function sanitizeEvidence(input: unknown): Record<string, string | number | boolean> {
  if (!input || typeof input !== "object" || Array.isArray(input)) return {};
  const out: Record<string, string | number | boolean> = {};
  let n = 0;
  for (const [rawKey, value] of Object.entries(input as Record<string, unknown>)) {
    if (n >= 24) break;
    if (!/^[a-zA-Z][a-zA-Z0-9_]{0,39}$/.test(rawKey)) continue;
    const key = rawKey.toLowerCase();
    if (
      key.includes("token") ||
      key.includes("password") ||
      key.includes("secret") ||
      key.includes("authorization") ||
      key.includes("cookie") ||
      key.includes("refresh") ||
      key.includes("bearer") ||
      key.includes("session") ||
      key.includes("private") ||
      key.includes("apikey") ||
      key.includes("api_key") ||
      key.includes("lease") ||
      key.includes("bootstrap") ||
      key.includes("pem") ||
      key.includes("signature") ||
      key === "hwid" ||
      key === "rawhwid" ||
      key === "code" ||
      key === "rawcode" ||
      key === "email" ||
      key === "body" ||
      key === "subject" ||
      key === "stack" ||
      key === "filepath" ||
      key === "localclientdll"
    ) {
      continue;
    }
    if (typeof value === "string" && value.length <= 240) {
      const lower = value.toLowerCase();
      // Drop absolute developer paths and credential-shaped strings.
      if (
        /[a-z]:\\users\\/i.test(value) ||
        lower.includes("onedrive") ||
        lower.includes("begin private") ||
        lower.includes("bearer ") ||
        /eyj[a-z0-9_-]{20,}\./i.test(value)
      ) {
        continue;
      }
      out[rawKey] = value;
      n++;
    } else if (typeof value === "number" && Number.isFinite(value)) {
      out[rawKey] = value;
      n++;
    } else if (typeof value === "boolean") {
      out[rawKey] = value;
      n++;
    }
  }
  return out;
}

function minSeverity(): AlertSeverity {
  const raw = (getSetting("alert_min_severity") || "high").toLowerCase();
  if (raw === "low" || raw === "medium" || raw === "high" || raw === "critical") return raw;
  return "high";
}

function discordWebhookUrl(): string | null {
  const raw = (getSetting("alert_discord_webhook") || "").trim();
  if (!raw) return null;
  try {
    const url = new URL(raw);
    if (url.protocol !== "https:") return null;
    // Strict host allowlist — prevents SSRF via admin settings.
    const host = url.hostname.toLowerCase();
    if (host !== "discord.com" && host !== "discordapp.com" && !host.endsWith(".discord.com")) {
      return null;
    }
    if (!url.pathname.startsWith("/api/webhooks/")) return null;
    return url.toString();
  } catch {
    return null;
  }
}

async function notifyDiscord(alert: {
  id: string;
  severity: AlertSeverity;
  category: string;
  title: string;
  summary: string;
  count: number;
  userId?: string | null;
}): Promise<boolean> {
  if (SEVERITY_RANK[alert.severity] < SEVERITY_RANK[minSeverity()]) return false;
  const webhook = discordWebhookUrl();
  if (!webhook) return false;

  const content = [
    `**Oak alert (${alert.severity})**`,
    `\`${alert.category}\` — ${alert.title}`,
    alert.summary ? alert.summary.slice(0, 300) : "",
    alert.userId ? `user: \`${alert.userId.slice(0, 8)}…\`` : "",
    alert.count > 1 ? `count: ${alert.count}` : "",
    `id: \`${alert.id}\``,
  ]
    .filter(Boolean)
    .join("\n");

  try {
    const res = await fetch(webhook, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ content: content.slice(0, 1800) }),
      signal: AbortSignal.timeout(5000),
    });
    return res.ok || res.status === 204;
  } catch {
    return false;
  }
}

export function raiseSecurityAlert(input: {
  severity: AlertSeverity;
  category: AlertCategory;
  title: string;
  summary?: string;
  dedupeKey: string;
  userId?: string | null;
  hwidId?: string | null;
  releaseId?: string | null;
  distributionId?: string | null;
  evidence?: unknown;
  ip?: string | null;
}): { id: string; created: boolean; count: number } {
  const t = nowIso();
  const evidence = sanitizeEvidence({
    ...(typeof input.evidence === "object" && input.evidence ? input.evidence : {}),
    ...(input.ip ? { ip: String(input.ip).slice(0, 64) } : {}),
  });
  const dedupeKey = input.dedupeKey.slice(0, 190);
  const since = new Date(Date.now() - DEDUPE_WINDOW_MS).toISOString();

  const existing = db
    .prepare(
      `SELECT * FROM security_alerts
       WHERE dedupe_key = ? AND status = 'open' AND updated_at >= ?
       ORDER BY updated_at DESC LIMIT 1`
    )
    .get(dedupeKey, since) as
    | {
        id: string;
        severity: AlertSeverity;
        count: number;
        evidence_json: string | null;
      }
    | undefined;

  if (existing) {
    const nextCount = existing.count + 1;
    const mergedEvidence = {
      ...sanitizeEvidence(existing.evidence_json ? JSON.parse(existing.evidence_json) : {}),
      ...evidence,
      lastSeenAt: t,
    };
    const nextSeverity =
      SEVERITY_RANK[input.severity] > SEVERITY_RANK[existing.severity]
        ? input.severity
        : existing.severity;
    db.prepare(
      `UPDATE security_alerts
       SET count = ?, severity = ?, evidence_json = ?, updated_at = ?, summary = ?
       WHERE id = ?`
    ).run(
      nextCount,
      nextSeverity,
      JSON.stringify(mergedEvidence),
      t,
      (input.summary || "").slice(0, 500),
      existing.id
    );
    return { id: existing.id, created: false, count: nextCount };
  }

  const id = cryptoRandomId();
  db.prepare(
    `INSERT INTO security_alerts
     (id, severity, category, title, summary, user_id, hwid_id, release_id, distribution_id,
      evidence_json, status, dedupe_key, notify_state, count, created_at, updated_at)
     VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 'open', ?, 'pending', 1, ?, ?)`
  ).run(
    id,
    input.severity,
    input.category,
    input.title.slice(0, 160),
    (input.summary || "").slice(0, 500),
    input.userId ?? null,
    input.hwidId ?? null,
    input.releaseId ?? null,
    input.distributionId ?? null,
    JSON.stringify(evidence),
    dedupeKey,
    t,
    t
  );

  audit("security.alert_raised", {
    actorId: input.userId ?? null,
    targetId: id,
    meta: { category: input.category, severity: input.severity, dedupeKey },
    ip: input.ip ?? null,
  });

  // Fire-and-forget notification — never block the request path.
  void (async () => {
    const sent = await notifyDiscord({
      id,
      severity: input.severity,
      category: input.category,
      title: input.title,
      summary: input.summary || "",
      count: 1,
      userId: input.userId,
    });
    db.prepare(`UPDATE security_alerts SET notify_state = ?, updated_at = ? WHERE id = ? AND notify_state = 'pending'`).run(
      sent ? "sent" : "suppressed",
      nowIso(),
      id
    );
  })();

  return { id, created: true, count: 1 };
}

export function listSecurityAlerts(opts: {
  status?: AlertStatus | "all";
  severity?: AlertSeverity | "all";
  limit?: number;
  offset?: number;
}) {
  const limit = Math.min(Math.max(opts.limit ?? 50, 1), 200);
  const offset = Math.min(Math.max(opts.offset ?? 0, 0), 5000);
  const status = opts.status && opts.status !== "all" ? opts.status : null;
  const severity = opts.severity && opts.severity !== "all" ? opts.severity : null;

  let sql = `SELECT * FROM security_alerts WHERE 1=1`;
  const params: unknown[] = [];
  if (status) {
    sql += ` AND status = ?`;
    params.push(status);
  }
  if (severity) {
    sql += ` AND severity = ?`;
    params.push(severity);
  }
  sql += ` ORDER BY
    CASE status WHEN 'open' THEN 0 WHEN 'acked' THEN 1 ELSE 2 END,
    CASE severity WHEN 'critical' THEN 0 WHEN 'high' THEN 1 WHEN 'medium' THEN 2 ELSE 3 END,
    updated_at DESC
    LIMIT ? OFFSET ?`;
  params.push(limit, offset);

  const rows = db.prepare(sql).all(...params) as Record<string, unknown>[];
  return rows.map(publicAlert);
}

export function getSecurityAlert(id: string) {
  const row = db.prepare(`SELECT * FROM security_alerts WHERE id = ?`).get(id) as
    | Record<string, unknown>
    | undefined;
  return row ? publicAlert(row) : null;
}

export function updateAlertStatus(input: {
  id: string;
  status: AlertStatus;
  actorId: string;
  note?: string;
}): boolean {
  const row = db.prepare(`SELECT id, status FROM security_alerts WHERE id = ?`).get(input.id) as
    | { id: string; status: string }
    | undefined;
  if (!row) return false;
  const t = nowIso();
  const resolved = input.status === "resolved" || input.status === "dismissed";
  db.prepare(
    `UPDATE security_alerts
     SET status = ?, updated_at = ?, resolved_at = ?, resolved_by = ?, resolve_note = ?
     WHERE id = ?`
  ).run(
    input.status,
    t,
    resolved ? t : null,
    resolved ? input.actorId : null,
    resolved ? (input.note || "").slice(0, 500) : null,
    input.id
  );
  audit("security.alert_update", {
    actorId: input.actorId,
    targetId: input.id,
    meta: { status: input.status },
  });
  return true;
}

export function alertCounts() {
  const open = db
    .prepare(
      `SELECT severity, COUNT(*) AS c FROM security_alerts WHERE status IN ('open','acked') GROUP BY severity`
    )
    .all() as { severity: string; c: number }[];
  const bySeverity: Record<string, number> = { low: 0, medium: 0, high: 0, critical: 0 };
  for (const r of open) bySeverity[r.severity] = r.c;
  const openTotal = Object.values(bySeverity).reduce((a, b) => a + b, 0);
  return { openTotal, bySeverity };
}

function publicAlert(row: Record<string, unknown>) {
  let evidence: unknown = {};
  try {
    evidence = row.evidence_json ? JSON.parse(String(row.evidence_json)) : {};
  } catch {
    evidence = {};
  }
  return {
    id: row.id,
    severity: row.severity,
    category: row.category,
    title: row.title,
    summary: row.summary,
    userId: row.user_id,
    hwidId: row.hwid_id,
    releaseId: row.release_id,
    distributionId: row.distribution_id,
    evidence: sanitizeEvidence(evidence),
    status: row.status,
    dedupeKey: row.dedupe_key,
    notifyState: row.notify_state,
    count: row.count,
    createdAt: row.created_at,
    updatedAt: row.updated_at,
    resolvedAt: row.resolved_at,
    resolvedBy: row.resolved_by,
    resolveNote: row.resolve_note,
  };
}

/** Map known protection/auth failure codes to alerts. */
export function alertFromProtectionFailure(input: {
  error: string;
  userId?: string | null;
  hwidId?: string | null;
  releaseId?: string | null;
  slug?: string | null;
  ip?: string | null;
  path?: string | null;
}): void {
  const map: Record<
    string,
    { severity: AlertSeverity; category: AlertCategory; title: string }
  > = {
    bootstrap_reused: {
      severity: "high",
      category: "protection",
      title: "Bootstrap ticket reuse",
    },
    hwid_mismatch: {
      severity: "high",
      category: "hwid",
      title: "HWID mismatch on protection path",
    },
    invalid_package_signature: {
      severity: "critical",
      category: "protection",
      title: "Runtime package signature rejected",
    },
    lease_revoked: {
      severity: "medium",
      category: "protection",
      title: "Renew after lease revocation",
    },
    challenge_failed: {
      severity: "high",
      category: "protection",
      title: "Lease renew challenge failed",
    },
    invalid_challenge: {
      severity: "high",
      category: "protection",
      title: "Lease renew challenge failed",
    },
  };
  const hit = map[input.error];
  if (!hit) return;
  const day = new Date().toISOString().slice(0, 10);
  raiseSecurityAlert({
    severity: hit.severity,
    category: hit.category,
    title: hit.title,
    summary: `${input.error}${input.slug ? ` (${input.slug})` : ""}`,
    dedupeKey: `${input.error}:${input.userId || "anon"}:${day}`,
    userId: input.userId,
    hwidId: input.hwidId,
    releaseId: input.releaseId,
    evidence: { error: input.error, path: input.path || "", slug: input.slug || "" },
    ip: input.ip,
  });
}
