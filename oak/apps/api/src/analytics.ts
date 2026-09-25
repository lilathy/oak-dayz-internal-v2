import { cryptoRandomId, db, nowIso } from "./db.js";

export function ensureAnalyticsSchema(): void {
  db.exec(`
CREATE TABLE IF NOT EXISTS analytics_events (
  id          TEXT PRIMARY KEY,
  event       TEXT NOT NULL,
  user_id     TEXT,
  path        TEXT,
  meta_json   TEXT,
  ip          TEXT,
  created_at  TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_analytics_event ON analytics_events(event);
CREATE INDEX IF NOT EXISTS idx_analytics_created ON analytics_events(created_at);

CREATE TABLE IF NOT EXISTS download_log (
  id          TEXT PRIMARY KEY,
  kind        TEXT NOT NULL CHECK(kind IN ('launcher','client')),
  user_id     TEXT,
  product_slug TEXT,
  version     TEXT,
  ip          TEXT,
  created_at  TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_download_created ON download_log(created_at);

CREATE TABLE IF NOT EXISTS site_settings (
  key         TEXT PRIMARY KEY,
  value       TEXT NOT NULL DEFAULT '',
  updated_at  TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS client_releases (
  id            TEXT PRIMARY KEY,
  product_slug  TEXT NOT NULL,
  version       TEXT NOT NULL,
  filename      TEXT NOT NULL,
  storage_path  TEXT NOT NULL,
  sha256        TEXT NOT NULL,
  size_bytes    INTEGER NOT NULL,
  uploaded_by   TEXT,
  created_at    TEXT NOT NULL,
  is_current    INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_client_slug ON client_releases(product_slug);

CREATE TABLE IF NOT EXISTS launcher_releases (
  id            TEXT PRIMARY KEY,
  version       TEXT NOT NULL,
  filename      TEXT NOT NULL,
  storage_path  TEXT NOT NULL,
  sha256        TEXT NOT NULL,
  size_bytes    INTEGER NOT NULL,
  uploaded_by   TEXT,
  created_at    TEXT NOT NULL,
  is_current    INTEGER NOT NULL DEFAULT 1
);
`);

  // users.last_seen_at
  const cols = db.prepare(`PRAGMA table_info(users)`).all() as { name: string }[];
  if (!cols.some((c) => c.name === "last_seen_at")) {
    db.exec(`ALTER TABLE users ADD COLUMN last_seen_at TEXT`);
  }

  const defaults: Record<string, string> = {
    announcement: "",
    maintenance: "0",
    discord_url: "",
    analytics_script: "",
    changelog_extra: "",
    alert_discord_webhook: "",
    alert_min_severity: "high",
    alert_cooldown_seconds: "60",
  };
  const upsert = db.prepare(
    `INSERT INTO site_settings (key, value, updated_at) VALUES (?, ?, ?)
     ON CONFLICT(key) DO NOTHING`
  );
  const t = nowIso();
  for (const [k, v] of Object.entries(defaults)) {
    upsert.run(k, v, t);
  }
}

export function trackEvent(
  event: string,
  opts: { userId?: string | null; path?: string | null; meta?: unknown; ip?: string | null } = {}
): void {
  db.prepare(
    `INSERT INTO analytics_events (id, event, user_id, path, meta_json, ip, created_at)
     VALUES (?, ?, ?, ?, ?, ?, ?)`
  ).run(
    cryptoRandomId(),
    event,
    opts.userId ?? null,
    opts.path ?? null,
    opts.meta == null ? null : JSON.stringify(opts.meta),
    opts.ip ?? null,
    nowIso()
  );
}

export function logDownload(opts: {
  kind: "launcher" | "client";
  userId?: string | null;
  productSlug?: string | null;
  version?: string | null;
  ip?: string | null;
}): void {
  db.prepare(
    `INSERT INTO download_log (id, kind, user_id, product_slug, version, ip, created_at)
     VALUES (?, ?, ?, ?, ?, ?, ?)`
  ).run(
    cryptoRandomId(),
    opts.kind,
    opts.userId ?? null,
    opts.productSlug ?? null,
    opts.version ?? null,
    opts.ip ?? null,
    nowIso()
  );
}

export function touchLastSeen(userId: string): void {
  db.prepare(`UPDATE users SET last_seen_at = ? WHERE id = ?`).run(nowIso(), userId);
}

export function getSetting(key: string): string {
  const row = db.prepare(`SELECT value FROM site_settings WHERE key = ?`).get(key) as
    | { value: string }
    | undefined;
  return row?.value ?? "";
}

export function setSetting(key: string, value: string): void {
  db.prepare(
    `INSERT INTO site_settings (key, value, updated_at) VALUES (?, ?, ?)
     ON CONFLICT(key) DO UPDATE SET value = excluded.value, updated_at = excluded.updated_at`
  ).run(key, value, nowIso());
}

export function getAllSettings(): Record<string, string> {
  const rows = db.prepare(`SELECT key, value FROM site_settings`).all() as {
    key: string;
    value: string;
  }[];
  const out: Record<string, string> = {};
  for (const r of rows) out[r.key] = r.value;
  return out;
}

export function isMaintenance(): boolean {
  return getSetting("maintenance") === "1" || getSetting("maintenance") === "true";
}

export function analyticsSummary(days = 7) {
  const since = new Date(Date.now() - days * 86400000).toISOString();
  const count = (event: string) =>
    (
      db
        .prepare(
          `SELECT COUNT(*) AS c FROM analytics_events WHERE event = ? AND created_at >= ?`
        )
        .get(event, since) as { c: number }
    ).c;
  const downloads = {
    launcher: (
      db
        .prepare(
          `SELECT COUNT(*) AS c FROM download_log WHERE kind = 'launcher' AND created_at >= ?`
        )
        .get(since) as { c: number }
    ).c,
    client: (
      db
        .prepare(
          `SELECT COUNT(*) AS c FROM download_log WHERE kind = 'client' AND created_at >= ?`
        )
        .get(since) as { c: number }
    ).c,
  };
  const onlineWindow = new Date(Date.now() - 15 * 60 * 1000).toISOString();
  const onlineNow = (
    db
      .prepare(`SELECT COUNT(*) AS c FROM users WHERE last_seen_at >= ?`)
      .get(onlineWindow) as { c: number }
  ).c;
  return {
    days,
    pageViews: count("page_view") + count("web.page_view"),
    logins: count("login") + count("web.login_ok"),
    registers: count("register") + count("web.signup_ok"),
    redeems: count("redeem") + count("web.redeem_ok"),
    launcherInjectOk: count("launcher.inject_ok"),
    launcherInjectFail: count("launcher.inject_fail"),
    downloads,
    onlineNow,
  };
}
