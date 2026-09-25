import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { config } from "./config.js";
import { cryptoRandomId, db, nowIso, type HwidRow, type UserRow } from "./db.js";
import { getCurrentClient, type ClientReleaseRow } from "./routes/releases.js";
import { getHwid, syncLicenseStatus } from "./license.js";
import { isMaintenance } from "./analytics.js";

export const BOOTSTRAP_TTL_SECONDS = 60;
export const LEASE_TTL_SECONDS = 5 * 60;
export const LEASE_RENEW_AFTER_SECONDS = 90;

export type RuntimePackageRow = {
  id: string;
  product_slug: string;
  client_release_id: string | null;
  version: string;
  payload_json: string;
  payload_sha256: string;
  signature: string;
  status: "staged" | "active" | "revoked";
  created_by: string | null;
  created_at: string;
  activated_at: string | null;
  revoked_at: string | null;
};

type BootstrapTicketRow = {
  id: string;
  token_hash: string;
  user_id: string;
  session_id: string;
  hwid_id: string;
  hwid_hash: string;
  product_slug: string;
  client_release_id: string;
  expires_at: string;
  consumed_at: string | null;
  revoked_at: string | null;
};

type LeaseRow = {
  id: string;
  token_hash: string;
  user_id: string;
  session_id: string;
  hwid_id: string;
  hwid_hash: string;
  product_slug: string;
  client_release_id: string;
  runtime_package_id: string | null;
  client_nonce_hash: string;
  renew_challenge: string;
  expires_at: string;
  last_renewed_at: string;
  revoked_at: string | null;
  revoke_reason: string | null;
  created_at: string;
};

export function ensureProtectionSchema(): void {
  db.exec(`
CREATE TABLE IF NOT EXISTS runtime_packages (
  id                TEXT PRIMARY KEY,
  product_slug      TEXT NOT NULL REFERENCES products(slug) ON DELETE CASCADE,
  client_release_id TEXT REFERENCES client_releases(id) ON DELETE SET NULL,
  version           TEXT NOT NULL,
  payload_json      TEXT NOT NULL,
  payload_sha256    TEXT NOT NULL,
  signature         TEXT NOT NULL DEFAULT '',
  status            TEXT NOT NULL DEFAULT 'staged'
                    CHECK(status IN ('staged','active','revoked')),
  created_by        TEXT REFERENCES users(id) ON DELETE SET NULL,
  created_at        TEXT NOT NULL,
  activated_at      TEXT,
  revoked_at        TEXT
);
CREATE UNIQUE INDEX IF NOT EXISTS idx_runtime_package_active
  ON runtime_packages(product_slug) WHERE status = 'active';

CREATE TABLE IF NOT EXISTS bootstrap_tickets (
  id                TEXT PRIMARY KEY,
  token_hash        TEXT NOT NULL UNIQUE,
  user_id           TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  session_id        TEXT NOT NULL REFERENCES refresh_tokens(id) ON DELETE CASCADE,
  hwid_id           TEXT NOT NULL REFERENCES hwids(id) ON DELETE CASCADE,
  hwid_hash         TEXT NOT NULL,
  product_slug      TEXT NOT NULL REFERENCES products(slug) ON DELETE CASCADE,
  client_release_id TEXT NOT NULL REFERENCES client_releases(id) ON DELETE CASCADE,
  expires_at        TEXT NOT NULL,
  consumed_at       TEXT,
  revoked_at        TEXT,
  created_at        TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_bootstrap_user ON bootstrap_tickets(user_id, expires_at);

CREATE TABLE IF NOT EXISTS client_leases (
  id                 TEXT PRIMARY KEY,
  token_hash         TEXT NOT NULL UNIQUE,
  user_id            TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  session_id         TEXT NOT NULL REFERENCES refresh_tokens(id) ON DELETE CASCADE,
  hwid_id            TEXT NOT NULL REFERENCES hwids(id) ON DELETE CASCADE,
  hwid_hash          TEXT NOT NULL,
  product_slug       TEXT NOT NULL REFERENCES products(slug) ON DELETE CASCADE,
  client_release_id  TEXT NOT NULL REFERENCES client_releases(id) ON DELETE CASCADE,
  runtime_package_id TEXT REFERENCES runtime_packages(id) ON DELETE SET NULL,
  client_nonce_hash  TEXT NOT NULL,
  renew_challenge    TEXT NOT NULL DEFAULT '',
  expires_at         TEXT NOT NULL,
  last_renewed_at    TEXT NOT NULL,
  revoked_at         TEXT,
  revoke_reason      TEXT,
  created_at         TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_lease_user ON client_leases(user_id, product_slug, expires_at);
CREATE INDEX IF NOT EXISTS idx_lease_session ON client_leases(session_id, expires_at);

CREATE TABLE IF NOT EXISTS client_distributions (
  id                TEXT PRIMARY KEY,
  user_id           TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  product_slug      TEXT NOT NULL REFERENCES products(slug) ON DELETE CASCADE,
  client_release_id TEXT NOT NULL REFERENCES client_releases(id) ON DELETE CASCADE,
  created_at        TEXT NOT NULL,
  ip                TEXT
);
CREATE INDEX IF NOT EXISTS idx_distribution_user ON client_distributions(user_id, created_at);
`);
  try {
    db.exec(`ALTER TABLE runtime_packages ADD COLUMN signature TEXT NOT NULL DEFAULT ''`);
  } catch {
    // Existing databases already have the column.
  }
  try {
    db.exec(`ALTER TABLE client_leases ADD COLUMN renew_challenge TEXT NOT NULL DEFAULT ''`);
  } catch {
    // Existing databases already have the column.
  }
}

/**
 * Local development uses an ephemeral signing key by design. Recreate a
 * compatible package on boot / after client upload so local Release builds can
 * exercise signature verification and offset application. Production never
 * auto-publishes policy.
 */
export function publishDevelopmentRuntimePackage(productSlug: string, clientReleaseId: string): string {
  let runtimeValues: Record<string, string> = {};
  try {
    const valuesPath = path.join(
      path.dirname(fileURLToPath(import.meta.url)),
      "..",
      "data",
      "default-runtime-values.json"
    );
    runtimeValues = JSON.parse(fs.readFileSync(valuesPath, "utf8")) as Record<string, string>;
  } catch {
    console.warn("[oak-api] default-runtime-values.json missing — dev runtime package will fail offset load.");
  }
  const id = cryptoRandomId();
  const issuedAt = nowIso();
  const payloadJson = JSON.stringify({
    schemaVersion: 1,
    productSlug,
    clientReleaseId,
    runtimePackageId: id,
    issuedAt,
    expiresAt: new Date(Date.now() + 7 * 24 * 60 * 60 * 1000).toISOString(),
    enabled: true,
    featureCompatibilityFlags: { enabled: true },
    runtimeValues,
  });
  const tx = db.transaction(() => {
    db.prepare(`UPDATE runtime_packages SET status = 'staged' WHERE product_slug = ? AND status = 'active'`)
      .run(productSlug);
    db.prepare(
      `INSERT INTO runtime_packages
       (id, product_slug, client_release_id, version, payload_json, payload_sha256, signature,
        status, created_by, created_at, activated_at, revoked_at)
       VALUES (?, ?, ?, ?, ?, ?, ?, 'active', NULL, ?, ?, NULL)`
    ).run(
      id,
      productSlug,
      clientReleaseId,
      `dev-${issuedAt.replace(/[^0-9]/g, "").slice(0, 14)}`,
      payloadJson,
      sha256(payloadJson),
      signRuntimePackage(payloadJson),
      issuedAt,
      issuedAt
    );
  });
  tx();
  return id;
}

export function ensureDevelopmentRuntimePackages(): void {
  if (config.isProd) return;
  const releases = db
    .prepare(`SELECT id, product_slug FROM client_releases WHERE is_current = 1`)
    .all() as { id: string; product_slug: string }[];
  for (const release of releases) {
    const matched = getActiveRuntimePackage(release.product_slug, release.id);
    if (!matched) publishDevelopmentRuntimePackage(release.product_slug, release.id);
  }
}

/** Non-prod self-heal used when a new client release is uploaded after boot. */
export function ensureRuntimePackageForCurrentRelease(slug: string): void {
  if (config.isProd) return;
  const release = getCurrentClient(slug);
  if (!release) return;
  if (!getActiveRuntimePackage(slug, release.id)) {
    publishDevelopmentRuntimePackage(slug, release.id);
  }
}

function opaqueToken(): string {
  return crypto.randomBytes(32).toString("base64url");
}

function renewChallenge(): string {
  return crypto.randomBytes(24).toString("base64url");
}

function hashToken(raw: string): string {
  return crypto.createHmac("sha256", config.protectionPepper).update(raw).digest("hex");
}

function hashNonce(nonce: string): string {
  return crypto.createHmac("sha256", config.protectionPepper).update(`nonce:${nonce}`).digest("hex");
}

/** Client proves possession of the lease material without sending a static secret. */
export function verifyChallengeResponse(
  leaseToken: string,
  clientNonce: string,
  challenge: string,
  responseHex: string
): boolean {
  if (!/^[0-9a-f]{64}$/i.test(responseHex)) return false;
  const key = crypto.createHash("sha256").update(`${leaseToken}|${clientNonce}`).digest();
  const expected = crypto.createHmac("sha256", key).update(challenge).digest("hex");
  try {
    return crypto.timingSafeEqual(Buffer.from(expected, "hex"), Buffer.from(responseHex, "hex"));
  } catch {
    return false;
  }
}

function sha256(value: string): string {
  return crypto.createHash("sha256").update(value).digest("hex");
}

function isoAfter(seconds: number): string {
  return new Date(Date.now() + seconds * 1000).toISOString();
}

export function getActiveRuntimePackage(
  slug: string,
  clientReleaseId?: string
): RuntimePackageRow | undefined {
  return db
    .prepare(
      `SELECT * FROM runtime_packages
       WHERE product_slug = ? AND status = 'active'
         AND (? IS NULL OR client_release_id = ?)
       LIMIT 1`
    )
    .get(slug, clientReleaseId ?? null, clientReleaseId ?? null) as RuntimePackageRow | undefined;
}

export function parseRuntimePackage(row: RuntimePackageRow): unknown {
  return JSON.parse(row.payload_json) as unknown;
}

export function signRuntimePackage(payload: string): string {
  return crypto
    .sign("sha256", Buffer.from(payload, "utf8"), {
      key: config.runtimePackagePrivateKey,
      dsaEncoding: "ieee-p1363",
    })
    .toString("base64");
}

function requireLaunchEntitlement(user: UserRow, slug: string): {
  hwid: HwidRow;
  release: ClientReleaseRow;
} {
  if (user.banned) throw new ProtectionError("banned", 403);
  if (isMaintenance()) throw new ProtectionError("maintenance", 503);
  if (syncLicenseStatus(user.id)?.status !== "active") {
    throw new ProtectionError("license_inactive", 403);
  }
  const hwid = getHwid(user.id);
  if (!hwid) throw new ProtectionError("hwid_unbound", 403);
  const product = db.prepare(`SELECT status FROM products WHERE slug = ?`).get(slug) as
    | { status: string }
    | undefined;
  if (!product) throw new ProtectionError("not_found", 404);
  if (product.status !== "online") throw new ProtectionError(`product_${product.status}`, 503);
  const release = getCurrentClient(slug);
  if (!release) throw new ProtectionError("no_release", 404);
  return { hwid, release };
}

export function createBootstrapTicket(input: {
  user: UserRow;
  sessionId: string;
  slug: string;
  hwidHash: string;
}): {
  ticket: string;
  expiresAt: string;
  release: ClientReleaseRow;
  runtimePackageVersion: string | null;
} {
  const { hwid, release } = requireLaunchEntitlement(input.user, input.slug);
  if (!crypto.timingSafeEqual(Buffer.from(hwid.hwid_hash, "hex"), Buffer.from(input.hwidHash, "hex"))) {
    throw new ProtectionError("hwid_mismatch", 403);
  }
  // Self-heal mismatched packages after a client upload in local development.
  ensureRuntimePackageForCurrentRelease(input.slug);

  const ticket = opaqueToken();
  const t = nowIso();
  const expiresAt = isoAfter(BOOTSTRAP_TTL_SECONDS);
  db.prepare(
    `INSERT INTO bootstrap_tickets
     (id, token_hash, user_id, session_id, hwid_id, hwid_hash, product_slug, client_release_id,
      expires_at, consumed_at, revoked_at, created_at)
     VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, NULL, NULL, ?)`
  ).run(
    cryptoRandomId(),
    hashToken(ticket),
    input.user.id,
    input.sessionId,
    hwid.id,
    hwid.hwid_hash,
    input.slug,
    release.id,
    expiresAt,
    t
  );

  return {
    ticket,
    expiresAt,
    release,
    runtimePackageVersion: getActiveRuntimePackage(input.slug, release.id)?.version ?? null,
  };
}

function getBootstrapTicket(raw: string): BootstrapTicketRow | undefined {
  return db
    .prepare(`SELECT * FROM bootstrap_tickets WHERE token_hash = ?`)
    .get(hashToken(raw)) as BootstrapTicketRow | undefined;
}

function getLease(raw: string): LeaseRow | undefined {
  return db.prepare(`SELECT * FROM client_leases WHERE token_hash = ?`).get(hashToken(raw)) as
    | LeaseRow
    | undefined;
}

/** Active (non-revoked, non-expired) lease lookup for lease-authenticated endpoints. */
export function getActiveLeaseByToken(raw: string): LeaseRow {
  const lease = getLease(raw);
  if (!lease || lease.revoked_at || new Date(lease.expires_at).getTime() <= Date.now()) {
    throw new ProtectionError("invalid_lease", 401);
  }
  return lease;
}

function requireLiveLeaseContext(input: {
  userId: string;
  sessionId: string;
  hwidId: string;
  hwidHash: string;
  slug: string;
  releaseId: string;
}): void {
  const user = db.prepare(`SELECT * FROM users WHERE id = ?`).get(input.userId) as UserRow | undefined;
  if (!user) throw new ProtectionError("user_gone", 401);
  const session = db
    .prepare(
      `SELECT 1 FROM refresh_tokens WHERE id = ? AND user_id = ? AND revoked = 0 AND expires_at > ?`
    )
    .get(input.sessionId, input.userId, nowIso());
  if (!session) throw new ProtectionError("session_revoked", 401);
  const hwid = getHwid(input.userId);
  if (!hwid || hwid.id !== input.hwidId || hwid.hwid_hash !== input.hwidHash) {
    throw new ProtectionError("hwid_mismatch", 403);
  }
  const { release } = requireLaunchEntitlement(user, input.slug);
  if (release.id !== input.releaseId) throw new ProtectionError("release_changed", 409);
}

function leaseResponse(row: LeaseRow, rawLease: string) {
  const pkg = getActiveRuntimePackage(row.product_slug, row.client_release_id);
  return {
    leaseToken: rawLease,
    leaseId: row.id,
    expiresAt: row.expires_at,
    renewAfterSeconds: LEASE_RENEW_AFTER_SECONDS,
    renewChallenge: row.renew_challenge,
    runtimePackage: pkg
      ? {
          id: pkg.id,
          version: pkg.version,
          sha256: pkg.payload_sha256,
          signature: pkg.signature,
          signingKeyId: config.runtimePackageKeyId,
          // Include the verify key here so the client does not need a second
          // round-trip on every lease or renewal.
          publicKey: config.runtimePackagePublicKey,
          payloadBase64: Buffer.from(pkg.payload_json, "utf8").toString("base64"),
          payload: parseRuntimePackage(pkg),
        }
      : null,
  };
}

export function consumeBootstrapTicket(input: {
  ticket: string;
  slug: string;
  hwidHash: string;
  clientNonce: string;
}) {
  const ticket = getBootstrapTicket(input.ticket);
  if (!ticket) {
    throw new ProtectionError("invalid_bootstrap", 401);
  }
  if (ticket.consumed_at) {
    throw new ProtectionError("bootstrap_reused", 401);
  }
  if (ticket.revoked_at || new Date(ticket.expires_at).getTime() <= Date.now()) {
    throw new ProtectionError("invalid_bootstrap", 401);
  }
  if (ticket.product_slug !== input.slug) throw new ProtectionError("invalid_bootstrap", 401);
  if (ticket.hwid_hash !== input.hwidHash) throw new ProtectionError("hwid_mismatch", 403);
  requireLiveLeaseContext({
    userId: ticket.user_id,
    sessionId: ticket.session_id,
    hwidId: ticket.hwid_id,
    hwidHash: ticket.hwid_hash,
    slug: ticket.product_slug,
    releaseId: ticket.client_release_id,
  });

  const rawLease = opaqueToken();
  const t = nowIso();
  const expiresAt = isoAfter(LEASE_TTL_SECONDS);
  const runtimePackage = getActiveRuntimePackage(ticket.product_slug, ticket.client_release_id);
  const challenge = renewChallenge();
  const lease: LeaseRow = {
    id: cryptoRandomId(),
    token_hash: hashToken(rawLease),
    user_id: ticket.user_id,
    session_id: ticket.session_id,
    hwid_id: ticket.hwid_id,
    hwid_hash: ticket.hwid_hash,
    product_slug: ticket.product_slug,
    client_release_id: ticket.client_release_id,
    runtime_package_id: runtimePackage?.id ?? null,
    client_nonce_hash: hashNonce(input.clientNonce),
    renew_challenge: challenge,
    expires_at: expiresAt,
    last_renewed_at: t,
    revoked_at: null,
    revoke_reason: null,
    created_at: t,
  };

  const tx = db.transaction(() => {
    const consumed = db
      .prepare(`UPDATE bootstrap_tickets SET consumed_at = ? WHERE id = ? AND consumed_at IS NULL`)
      .run(t, ticket.id);
    if (consumed.changes !== 1) throw new ProtectionError("invalid_bootstrap", 401);
    // New launch supersedes stale product leases for this session/HWID.
    db.prepare(
      `UPDATE client_leases SET revoked_at = ?, revoke_reason = 'superseded'
       WHERE user_id = ? AND product_slug = ? AND hwid_id = ? AND revoked_at IS NULL`
    ).run(t, ticket.user_id, ticket.product_slug, ticket.hwid_id);
    db.prepare(
      `INSERT INTO client_leases
       (id, token_hash, user_id, session_id, hwid_id, hwid_hash, product_slug, client_release_id,
        runtime_package_id, client_nonce_hash, renew_challenge, expires_at, last_renewed_at,
        revoked_at, revoke_reason, created_at)
       VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, NULL, NULL, ?)`
    ).run(
      lease.id,
      lease.token_hash,
      lease.user_id,
      lease.session_id,
      lease.hwid_id,
      lease.hwid_hash,
      lease.product_slug,
      lease.client_release_id,
      lease.runtime_package_id,
      lease.client_nonce_hash,
      lease.renew_challenge,
      lease.expires_at,
      lease.last_renewed_at,
      lease.created_at
    );
  });
  tx();
  return leaseResponse(lease, rawLease);
}

export function renewLease(input: {
  leaseToken: string;
  slug: string;
  clientNonce: string;
  challengeResponse: string;
}) {
  const lease = getLease(input.leaseToken);
  if (!lease) {
    throw new ProtectionError("invalid_lease", 401);
  }
  if (lease.revoked_at) {
    throw new ProtectionError("lease_revoked", 401);
  }
  if (new Date(lease.expires_at).getTime() <= Date.now()) {
    throw new ProtectionError("invalid_lease", 401);
  }
  if (lease.product_slug !== input.slug) throw new ProtectionError("invalid_lease", 401);
  if (lease.client_nonce_hash !== hashNonce(input.clientNonce)) {
    throw new ProtectionError("invalid_lease", 401);
  }
  if (
    !lease.renew_challenge ||
    !verifyChallengeResponse(
      input.leaseToken,
      input.clientNonce,
      lease.renew_challenge,
      input.challengeResponse
    )
  ) {
    throw new ProtectionError("invalid_challenge", 401);
  }
  requireLiveLeaseContext({
    userId: lease.user_id,
    sessionId: lease.session_id,
    hwidId: lease.hwid_id,
    hwidHash: lease.hwid_hash,
    slug: lease.product_slug,
    releaseId: lease.client_release_id,
  });

  const expiresAt = isoAfter(LEASE_TTL_SECONDS);
  const nextChallenge = renewChallenge();
  const pkg = getActiveRuntimePackage(lease.product_slug, lease.client_release_id);
  db.prepare(
    `UPDATE client_leases
     SET expires_at = ?, last_renewed_at = ?, runtime_package_id = ?, renew_challenge = ?
     WHERE id = ?`
  ).run(expiresAt, nowIso(), pkg?.id ?? null, nextChallenge, lease.id);
  return leaseResponse(
    {
      ...lease,
      expires_at: expiresAt,
      runtime_package_id: pkg?.id ?? null,
      renew_challenge: nextChallenge,
    },
    input.leaseToken
  );
}

export function runtimePackageForLease(leaseToken: string, slug: string): {
  id: string;
  version: string;
  sha256: string;
  signature: string;
  signingKeyId: string;
  publicKey: string;
  payloadBase64: string;
  payload: unknown;
} {
  const lease = getLease(leaseToken);
  if (!lease || lease.revoked_at || new Date(lease.expires_at).getTime() <= Date.now()) {
    throw new ProtectionError("invalid_lease", 401);
  }
  if (lease.product_slug !== slug) throw new ProtectionError("invalid_lease", 401);
  requireLiveLeaseContext({
    userId: lease.user_id,
    sessionId: lease.session_id,
    hwidId: lease.hwid_id,
    hwidHash: lease.hwid_hash,
    slug: lease.product_slug,
    releaseId: lease.client_release_id,
  });
  const pkg = getActiveRuntimePackage(lease.product_slug, lease.client_release_id);
  if (!pkg) throw new ProtectionError("runtime_package_unavailable", 503);
  return {
    id: pkg.id,
    version: pkg.version,
    sha256: pkg.payload_sha256,
    signature: pkg.signature,
    signingKeyId: config.runtimePackageKeyId,
    publicKey: config.runtimePackagePublicKey,
    payloadBase64: Buffer.from(pkg.payload_json, "utf8").toString("base64"),
    payload: parseRuntimePackage(pkg),
  };
}

export function revokeProtectionForUser(userId: string, reason: string): void {
  const t = nowIso();
  db.prepare(`UPDATE bootstrap_tickets SET revoked_at = ? WHERE user_id = ? AND consumed_at IS NULL AND revoked_at IS NULL`)
    .run(t, userId);
  db.prepare(`UPDATE client_leases SET revoked_at = ?, revoke_reason = ? WHERE user_id = ? AND revoked_at IS NULL`)
    .run(t, reason, userId);
}

export class ProtectionError extends Error {
  constructor(
    public readonly error: string,
    public readonly status: number
  ) {
    super(error);
  }
}
