/**
 * End-to-end checks for live-session JWTs, HWID-bound tickets, leases, and
 * signed runtime packages. Run while the API is listening:
 *   node scripts/smoke-protection.mjs
 *
 * The script creates a uniquely named local test account and promotes it in
 * the local test database only to exercise the admin package routes.
 */
import Database from "better-sqlite3";
import fs from "node:fs";
import path from "node:path";
import { createHash, createHmac } from "node:crypto";
import { fileURLToPath } from "node:url";

const BASE = process.env.SMOKE_BASE ?? "http://127.0.0.1:8787";
const __dirname = path.dirname(fileURLToPath(import.meta.url));
const db = new Database(path.join(__dirname, "..", "data", "oak.sqlite"));
db.pragma("foreign_keys = ON");
// Recover cleanly from a prior interrupted local smoke run.
db.prepare(`DELETE FROM runtime_packages WHERE version LIKE 'smoke-%'`).run();
for (const row of db.prepare(`SELECT id FROM users WHERE username LIKE 'protect_%'`).all()) {
  db.prepare(`DELETE FROM users WHERE id = ?`).run(row.id);
}
const runtimeValues = JSON.parse(
  fs.readFileSync(path.join(__dirname, "..", "data", "default-runtime-values.json"), "utf8")
);
let pass = 0;
let fail = 0;

function check(name, ok, detail = "") {
  if (ok) {
    pass++;
    console.log(`  ok   ${name}`);
  } else {
    fail++;
    console.log(`  FAIL ${name} ${detail}`);
  }
}

async function call(pathname, { method = "GET", body, token } = {}) {
  const headers = { "X-Oak-Client": "launcher" };
  if (body) headers["Content-Type"] = "application/json";
  if (token) headers.Authorization = `Bearer ${token}`;
  const res = await fetch(`${BASE}${pathname}`, {
    method,
    headers,
    body: body ? JSON.stringify(body) : undefined,
  });
  const text = await res.text();
  let json = {};
  try { json = text ? JSON.parse(text) : {}; } catch { json = { raw: text }; }
  return { status: res.status, json };
}

const rand = Math.random().toString(36).slice(2, 10);
const username = `protect_${rand}`;
const email = `${username}@example.test`;
const password = "Correct-Horse-9182!";
const hwid = `v2:smoke-${rand}-machine`;
let testUserId = "";
let packageId = "";

function cleanup() {
  if (packageId) db.prepare(`DELETE FROM runtime_packages WHERE id = ?`).run(packageId);
  if (testUserId) db.prepare(`DELETE FROM users WHERE id = ?`).run(testUserId);
}

console.log("\n== protection authorization ==");
const reg = await call("/v1/auth/register", {
  method: "POST",
  body: { email, username, password },
});
check("register test account", reg.status === 201, JSON.stringify(reg.json));
if (reg.status !== 201) process.exit(1);
testUserId = reg.json.user.id;

// Local-only test setup. Entitlement and role manipulation are deliberately
// not public API behavior.
db.prepare(`UPDATE licenses SET status = 'active', expires_at = ?, updated_at = ? WHERE user_id = ?`)
  .run(new Date(Date.now() + 86_400_000).toISOString(), new Date().toISOString(), reg.json.user.id);
db.prepare(`UPDATE users SET role = 'admin' WHERE id = ?`).run(reg.json.user.id);

const login = await call("/v1/auth/login", {
  method: "POST",
  body: { login: email, password, hwid },
});
check("launcher login gets session-bound JWT", login.status === 200 && !!login.json.accessToken);
const access = login.json.accessToken;
const release = db
  .prepare(`SELECT id FROM client_releases WHERE product_slug = 'dayz' AND is_current = 1 LIMIT 1`)
  .get();
if (!release) {
  console.log("  SKIP no current DayZ client release; upload one to test ticket/lease paths.");
  console.log(`\n${pass} passed, ${fail} failed`);
  cleanup();
  process.exit(fail ? 1 : 0);
}

const packageStage = await call("/v1/admin/protection/runtime-packages/dayz", {
  method: "POST",
  token: access,
  body: {
    version: `smoke-${rand}`,
    clientReleaseId: release.id,
    payload: {
      schemaVersion: 1,
      productSlug: "dayz",
      clientReleaseId: release.id,
      enabled: true,
      expiresAt: new Date(Date.now() + 86400_000).toISOString(),
      runtimeValues,
      revision: rand,
    },
  },
});
check(
  "stage signed runtime package",
  packageStage.status === 201 && !!packageStage.json.signature && !!packageStage.json.sha256,
  JSON.stringify(packageStage.json)
);
packageId = packageStage.json.id;
const activated = await call(`/v1/admin/protection/runtime-packages/${packageId}/activate`, {
  method: "POST",
  token: access,
});
check("activate runtime package", activated.status === 200, JSON.stringify(activated.json));

const bootstrap = await call("/v1/products/dayz/bootstrap", {
  method: "POST",
  token: access,
  body: { hwid },
});
check(
  "issue HWID-bound one-time bootstrap",
  bootstrap.status === 200 && !!bootstrap.json.bootstrapTicket,
  JSON.stringify(bootstrap.json)
);

const badHwid = await call("/v1/products/dayz/lease", {
  method: "POST",
  body: { bootstrapTicket: bootstrap.json.bootstrapTicket, hwid: `${hwid}-wrong`, clientNonce: `nonce-${rand}-0123456789` },
});
check("reject mismatched HWID lease", badHwid.status === 403 && badHwid.json.error === "hwid_mismatch", JSON.stringify(badHwid.json));

const leased = await call("/v1/products/dayz/lease", {
  method: "POST",
  body: { bootstrapTicket: bootstrap.json.bootstrapTicket, hwid, clientNonce: `nonce-${rand}-0123456789` },
});
check(
  "consume ticket and issue lease",
  leased.status === 200 &&
    !!leased.json.leaseToken &&
    !!leased.json.renewChallenge &&
    !!leased.json.runtimePackage?.signature &&
    !!leased.json.runtimePackage?.publicKey &&
    !!leased.json.runtimePackage?.payloadBase64,
  JSON.stringify(leased.json)
);

const replay = await call("/v1/products/dayz/lease", {
  method: "POST",
  body: { bootstrapTicket: bootstrap.json.bootstrapTicket, hwid, clientNonce: `second-${rand}-0123456789` },
});
check("reject bootstrap replay", replay.status === 401, JSON.stringify(replay.json));

function challengeResponse(leaseToken, clientNonce, challenge) {
  const key = createHash("sha256").update(`${leaseToken}|${clientNonce}`).digest();
  return createHmac("sha256", key).update(challenge).digest("hex");
}

const proof = challengeResponse(
  leased.json.leaseToken,
  `nonce-${rand}-0123456789`,
  leased.json.renewChallenge
);
const badProof = await call("/v1/products/dayz/lease/renew", {
  method: "POST",
  body: {
    leaseToken: leased.json.leaseToken,
    clientNonce: `nonce-${rand}-0123456789`,
    challengeResponse: "0".repeat(64),
  },
});
check("reject bad renew challenge", badProof.status === 401, JSON.stringify(badProof.json));

const renewed = await call("/v1/products/dayz/lease/renew", {
  method: "POST",
  body: {
    leaseToken: leased.json.leaseToken,
    clientNonce: `nonce-${rand}-0123456789`,
    challengeResponse: proof,
  },
});
check(
  "renew lease with challenge proof",
  renewed.status === 200 &&
    !!renewed.json.renewChallenge &&
    renewed.json.renewChallenge !== leased.json.renewChallenge,
  JSON.stringify(renewed.json)
);

const packageFetch = await call("/v1/products/dayz/runtime-package", {
  method: "POST",
  body: { leaseToken: leased.json.leaseToken },
});
check(
  "fetch signed package through active lease",
  packageFetch.status === 200 && packageFetch.json.runtimePackage?.id === packageId,
  JSON.stringify(packageFetch.json)
);

const logout = await call("/v1/auth/logout", { method: "POST", token: access });
check("logout revokes current session", logout.status === 200);
const oldJwt = await call("/v1/auth/me", { token: access });
check("revoked session invalidates old JWT immediately", oldJwt.status === 401 && oldJwt.json.error === "session_revoked", JSON.stringify(oldJwt.json));
const postLogoutProof = challengeResponse(
  leased.json.leaseToken,
  `nonce-${rand}-0123456789`,
  renewed.json.renewChallenge
);
const invalidLease = await call("/v1/products/dayz/lease/renew", {
  method: "POST",
  body: {
    leaseToken: leased.json.leaseToken,
    clientNonce: `nonce-${rand}-0123456789`,
    challengeResponse: postLogoutProof,
  },
});
check(
  "session revocation invalidates lease",
  invalidLease.status === 401 &&
    (invalidLease.json.error === "session_revoked" ||
      invalidLease.json.error === "invalid_lease" ||
      invalidLease.json.error === "lease_revoked"),
  JSON.stringify(invalidLease.json)
);

console.log(`\n${pass} passed, ${fail} failed`);
cleanup();
process.exit(fail ? 1 : 0);
