import { cryptoRandomId, db, nowIso, type LicenseRow } from "./db.js";
import { getPlan } from "./plans.js";

export type ProductLicenseRow = {
  id: string;
  user_id: string;
  product_slug: string;
  status: string;
  plan: string;
  expires_at: string | null;
  created_at: string;
  updated_at: string;
};

export function getLicense(userId: string): LicenseRow | undefined {
  return db.prepare(`SELECT * FROM licenses WHERE user_id = ?`).get(userId) as LicenseRow | undefined;
}

export function getProductLicense(userId: string, productSlug: string): ProductLicenseRow | undefined {
  return db
    .prepare(`SELECT * FROM product_licenses WHERE user_id = ? AND product_slug = ?`)
    .get(userId, productSlug) as ProductLicenseRow | undefined;
}

export function ensureProductLicenseRow(userId: string, productSlug: string): ProductLicenseRow {
  let row = getProductLicense(userId, productSlug);
  if (row) return row;
  const t = nowIso();
  db.prepare(
    `INSERT INTO product_licenses
     (id, user_id, product_slug, status, plan, expires_at, created_at, updated_at)
     VALUES (?, ?, ?, 'inactive', 'none', NULL, ?, ?)`
  ).run(cryptoRandomId(), userId, productSlug, t, t);
  return getProductLicense(userId, productSlug)!;
}

export function syncProductLicenseStatus(userId: string, productSlug: string): ProductLicenseRow | undefined {
  const row = getProductLicense(userId, productSlug);
  if (!row) return undefined;
  if (row.status === "active" && row.expires_at && new Date(row.expires_at).getTime() < Date.now()) {
    db.prepare(
      `UPDATE product_licenses SET status = 'expired', updated_at = ? WHERE user_id = ? AND product_slug = ?`
    ).run(nowIso(), userId, productSlug);
    return getProductLicense(userId, productSlug);
  }
  return row;
}

/** True if the user may launch this product (per-product row, else legacy DayZ license). */
export function hasProductEntitlement(userId: string, productSlug: string): boolean {
  const productLic = syncProductLicenseStatus(userId, productSlug);
  if (productLic?.status === "active") return true;
  if (productSlug === "dayz") {
    const legacy = syncLicenseStatus(userId);
    return legacy?.status === "active";
  }
  return false;
}

/**
 * Any active paid product OR legacy DayZ license.
 * Used to gate forum writing — free accounts can browse, not post.
 */
export function hasAnyActiveSubscription(userId: string): boolean {
  const legacy = syncLicenseStatus(userId);
  if (legacy?.status === "active") return true;
  const rows = db
    .prepare(`SELECT product_slug FROM product_licenses WHERE user_id = ? AND status = 'active'`)
    .all(userId) as { product_slug: string }[];
  for (const r of rows) {
    const synced = syncProductLicenseStatus(userId, r.product_slug);
    if (synced?.status === "active") return true;
  }
  return false;
}

export function addProductLicenseDays(
  userId: string,
  productSlug: string,
  days: number,
  planLabel: string
): ProductLicenseRow {
  const lic = ensureProductLicenseRow(userId, productSlug);
  if (lic.status === "banned") throw new Error("license_banned");
  const now = Date.now();
  const currentExp = lic.expires_at ? new Date(lic.expires_at).getTime() : 0;
  const base = Math.max(now, currentExp);
  const expiresAt = new Date(base + days * 24 * 60 * 60 * 1000).toISOString();
  const t = nowIso();
  db.prepare(
    `UPDATE product_licenses SET status = 'active', plan = ?, expires_at = ?, updated_at = ?
     WHERE user_id = ? AND product_slug = ?`
  ).run(planLabel, expiresAt, t, userId, productSlug);
  return syncProductLicenseStatus(userId, productSlug)!;
}

export function ensureLicenseRow(userId: string): LicenseRow {
  let lic = getLicense(userId);
  if (lic) return lic;
  const t = nowIso();
  db.prepare(
    `INSERT INTO licenses (id, user_id, status, plan, expires_at, created_at, updated_at)
     VALUES (?, ?, 'inactive', 'none', NULL, ?, ?)`
  ).run(cryptoRandomId(), userId, t, t);
  return getLicense(userId)!;
}

/** Refresh expired → expired status if past expires_at. */
export function syncLicenseStatus(userId: string): LicenseRow | undefined {
  const lic = getLicense(userId);
  if (!lic) return undefined;
  if (lic.status === "active" && lic.expires_at && new Date(lic.expires_at).getTime() < Date.now()) {
    db.prepare(
      `UPDATE licenses SET status = 'expired', updated_at = ? WHERE user_id = ?`
    ).run(nowIso(), userId);
    return getLicense(userId);
  }
  return lic;
}

/** Stack days onto max(now, current expires_at). Also mirrors into product_licenses when plan is known. */
export function addLicenseDays(userId: string, days: number, planLabel: string): LicenseRow {
  const lic = ensureLicenseRow(userId);
  if (lic.status === "banned") {
    throw new Error("license_banned");
  }
  if (lic.plan === "admin" && lic.expires_at == null && lic.status === "active") {
    db.prepare(`UPDATE licenses SET updated_at = ? WHERE user_id = ?`).run(nowIso(), userId);
    return syncLicenseStatus(userId)!;
  }

  const now = Date.now();
  const currentExp = lic.expires_at ? new Date(lic.expires_at).getTime() : 0;
  const base = Math.max(now, currentExp);
  const expiresAt = new Date(base + days * 24 * 60 * 60 * 1000).toISOString();
  const t = nowIso();
  db.prepare(
    `UPDATE licenses SET status = 'active', plan = ?, expires_at = ?, updated_at = ? WHERE user_id = ?`
  ).run(planLabel, expiresAt, t, userId);

  const plan = getPlan(planLabel);
  const slug = plan?.product_slug ?? "dayz";
  addProductLicenseDays(userId, slug, days, planLabel);

  return syncLicenseStatus(userId)!;
}

export function publicLicense(lic: LicenseRow | undefined) {
  if (!lic) return null;
  const expiresAt = lic.expires_at;
  let remainingSeconds: number | null = null;
  let remainingDays: number | null = null;
  if (lic.status === "active" && !expiresAt) {
    // Lifetime / admin
    remainingSeconds = null;
    remainingDays = null;
  } else if (expiresAt) {
    const ms = new Date(expiresAt).getTime() - Date.now();
    remainingSeconds = Math.max(0, Math.floor(ms / 1000));
    remainingDays = ms > 0 ? Math.ceil(ms / (24 * 60 * 60 * 1000)) : 0;
  } else {
    remainingSeconds = 0;
    remainingDays = 0;
  }
  return {
    status: lic.status,
    plan: lic.plan,
    expiresAt,
    remainingSeconds,
    remainingDays,
    lifetime: lic.status === "active" && expiresAt == null,
  };
}

export function getHwid(userId: string): import("./db.js").HwidRow | undefined {
  return db.prepare(`SELECT * FROM hwids WHERE user_id = ?`).get(userId) as
    | import("./db.js").HwidRow
    | undefined;
}

export function publicHwid(row: import("./db.js").HwidRow | undefined) {
  if (!row) return null;
  return {
    bound: true,
    hint: row.hwid_hint,
    boundAt: row.bound_at,
    lastSeenAt: row.last_seen_at,
    resetCount: row.reset_count,
    lastResetAt: row.last_reset_at,
  };
}

import {
  avatarPublicUrl,
  computePresence,
  escapePlainText,
  type ComputedPresence,
} from "./forumProfile.js";

export function publicUser(u: {
  id: string;
  email: string;
  username: string;
  role: string;
  banned: number;
  ban_reason: string | null;
  created_at: string;
  email_verified_at?: string | null;
  forum_post_count?: number;
  forum_muted_until?: string | null;
  bio?: string | null;
  avatar_ext?: string | null;
  presence?: string | null;
  last_seen_at?: string | null;
}) {
  const mutedUntil =
    u.forum_muted_until && new Date(u.forum_muted_until).getTime() > Date.now()
      ? u.forum_muted_until
      : null;
  const presence = computePresence({
    preference: u.presence,
    lastSeenAt: u.last_seen_at,
    self: true,
  });
  return {
    id: u.id,
    email: u.email,
    username: u.username,
    role: u.role,
    banned: !!u.banned,
    banReason: u.ban_reason,
    createdAt: u.created_at,
    emailVerified: !!u.email_verified_at,
    forumPostCount: u.forum_post_count ?? 0,
    forumMutedUntil: mutedUntil,
    bio: u.bio ?? "",
    avatarUrl: avatarPublicUrl(u.id, u.avatar_ext),
    presence,
  };
}

/** Author card shown next to forum posts — never leaks the email address. */
export function publicAuthor(
  u: {
    id: string;
    username: string;
    role: string;
    created_at: string;
    forum_post_count?: number;
    bio?: string | null;
    avatar_ext?: string | null;
    presence?: string | null;
    last_seen_at?: string | null;
  },
  opts?: { self?: boolean }
): {
  id: string;
  username: string;
  role: string;
  createdAt: string;
  postCount: number;
  bio: string;
  bioHtml: string;
  avatarUrl: string | null;
  presence: ComputedPresence;
} {
  const presence = computePresence({
    preference: u.presence,
    lastSeenAt: u.last_seen_at,
    self: !!opts?.self,
  });
  const bio = (u.bio ?? "").slice(0, 280);
  return {
    id: u.id,
    username: u.username,
    role: u.role,
    createdAt: u.created_at,
    postCount: u.forum_post_count ?? 0,
    bio,
    bioHtml: escapePlainText(bio).replace(/\n/g, "<br>"),
    avatarUrl: avatarPublicUrl(u.id, u.avatar_ext),
    presence,
  };
}
