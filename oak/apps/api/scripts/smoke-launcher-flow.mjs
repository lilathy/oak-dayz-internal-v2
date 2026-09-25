/**
 * Launcher-facing integration check against a live API.
 * Mirrors the WPF launcher sequence: login → HWID bind → products →
 * launch-info → bootstrap → client lease/challenge → encrypted-download meta.
 *
 *   node scripts/smoke-launcher-flow.mjs
 */
import Database from "better-sqlite3";
import fs from "node:fs";
import path from "node:path";
import { createHash, createHmac, randomBytes } from "node:crypto";
import { fileURLToPath } from "node:url";

const BASE = process.env.SMOKE_BASE ?? "http://127.0.0.1:8787";
const __dirname = path.dirname(fileURLToPath(import.meta.url));
const db = new Database(path.join(__dirname, "..", "data", "oak.sqlite"));
db.pragma("foreign_keys = ON");

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
  const headers = { "X-Oak-Client": "launcher", "User-Agent": "OakLauncher/1.0.0" };
  if (body) headers["Content-Type"] = "application/json";
  if (token) headers.Authorization = `Bearer ${token}`;
  const res = await fetch(`${BASE}${pathname}`, {
    method,
    headers,
    body: body ? JSON.stringify(body) : undefined,
  });
  const text = await res.text();
  let json = {};
  try {
    json = text ? JSON.parse(text) : {};
  } catch {
    json = { raw: text.slice(0, 200) };
  }
  return { status: res.status, json, headers: res.headers };
}

const rand = randomBytes(4).toString("hex");
const username = `launch_${rand}`;
const email = `${username}@example.test`;
const password = "Correct-Horse-9182!";
const hwid = `v2:launcher-flow-${rand}`;
let userId = "";

console.log("\n== launcher ↔ API flow ==");

const health = await call("/health");
check("API health", health.status === 200 && health.json.ok === true, JSON.stringify(health.json));

const reg = await call("/v1/auth/register", {
  method: "POST",
  body: { email, username, password },
});
check("launcher-style register returns tokens", reg.status === 201 && !!reg.json.accessToken && !!reg.json.refreshToken, JSON.stringify(reg.json));
if (reg.status !== 201) {
  console.log(`\n${pass} passed, ${fail} failed`);
  process.exit(1);
}
userId = reg.json.user.id;

db.prepare(`UPDATE licenses SET status = 'active', plan = 'day30', expires_at = ?, updated_at = ? WHERE user_id = ?`).run(
  new Date(Date.now() + 30 * 86400_000).toISOString(),
  new Date().toISOString(),
  userId
);

const login = await call("/v1/auth/login", {
  method: "POST",
  body: { login: email, password },
});
check("login without HWID succeeds for launcher bind-after-auth", login.status === 200 && !!login.json.accessToken);
let access = login.json.accessToken;
let refresh = login.json.refreshToken;

const bind = await call("/v1/hwid/bind", {
  method: "POST",
  token: access,
  body: { hwid },
});
check("HWID bind", bind.status === 201 || bind.status === 200, JSON.stringify(bind.json));

const products = await call("/v1/products", { token: access });
check(
  "products list includes dayz",
  products.status === 200 && products.json.products?.some((p) => p.slug === "dayz"),
  JSON.stringify(products.json)
);

const launch = await call("/v1/products/dayz/launch-info", { token: access });
check("launch-info returns", launch.status === 200, JSON.stringify(launch.json));
check("launch-info has client id+sha for encrypted cache", !!(launch.json.client?.id && launch.json.client?.sha256), JSON.stringify(launch.json.client));
check("launch canInject true with license+hwid+release", launch.json.launch?.canInject === true, JSON.stringify(launch.json.launch));

const release = db.prepare(`SELECT id, storage_path FROM client_releases WHERE product_slug='dayz' AND is_current=1`).get();
if (!release || !fs.existsSync(release.storage_path)) {
  check("current client release file exists", false, "upload a DayZ client DLL before testing downloads");
} else {
  // Ensure an active runtime package exists for this release (dev auto-publish may have one).
  const activePkg = db
    .prepare(`SELECT id FROM runtime_packages WHERE product_slug='dayz' AND status='active' AND client_release_id=?`)
    .get(release.id);
  check("active runtime package for current release", !!activePkg, "dev package missing");

  const meta = await call("/v1/products/dayz/client/meta", { token: access });
  check("client meta", meta.status === 200 && !!meta.json.client?.sha256);

  const dl = await call("/v1/products/dayz/client", { token: access });
  // fetch through call() buffers JSON — for binary we need raw fetch
  const raw = await fetch(`${BASE}/v1/products/dayz/client`, {
    headers: { Authorization: `Bearer ${access}`, "X-Oak-Client": "launcher", "User-Agent": "OakLauncher/1.0.0" },
  });
  const buf = Buffer.from(await raw.arrayBuffer());
  const sha = createHash("sha256").update(buf).digest("hex");
  const hdrSha = raw.headers.get("x-oak-client-sha256") || "";
  const dist = raw.headers.get("x-oak-distribution-id") || "";
  check("client download succeeds", raw.status === 200 && buf.length > 0, `status=${raw.status} size=${buf.length}`);
  check("client sha header matches body", sha.toLowerCase() === hdrSha.toLowerCase(), `${sha} vs ${hdrSha}`);
  check("distribution id stamped", !!dist);

  const boot = await call("/v1/products/dayz/bootstrap", {
    method: "POST",
    token: access,
    body: { hwid },
  });
  check("bootstrap ticket issued", boot.status === 200 && !!boot.json.bootstrapTicket && !!boot.json.release?.id, JSON.stringify(boot.json));

  const nonce = `nonce-${rand}-0123456789abcdef`;
  const lease = await call("/v1/products/dayz/lease", {
    method: "POST",
    body: { bootstrapTicket: boot.json.bootstrapTicket, hwid, clientNonce: nonce },
  });
  check(
    "client lease + renewChallenge + package",
    lease.status === 200 &&
      !!lease.json.leaseToken &&
      !!lease.json.renewChallenge &&
      !!lease.json.runtimePackage?.signature,
    JSON.stringify(lease.json)
  );

  if (lease.status === 200) {
    const key = createHash("sha256").update(`${lease.json.leaseToken}|${nonce}`).digest();
    const proof = createHmac("sha256", key).update(lease.json.renewChallenge).digest("hex");
    const renew = await call("/v1/products/dayz/lease/renew", {
      method: "POST",
      body: { leaseToken: lease.json.leaseToken, clientNonce: nonce, challengeResponse: proof },
    });
    check("challenge renew works", renew.status === 200 && !!renew.json.renewChallenge, JSON.stringify(renew.json));
  }

  const refreshed = await call("/v1/auth/refresh", {
    method: "POST",
    body: { refreshToken: refresh },
  });
  check("launcher refresh rotates tokens", refreshed.status === 200 && !!refreshed.json.accessToken && !!refreshed.json.refreshToken);
  access = refreshed.json.accessToken;
  refresh = refreshed.json.refreshToken;

  const me = await call("/v1/auth/me", { token: access });
  check("me after refresh", me.status === 200 && me.json.user?.username === username);

  const logout = await call("/v1/auth/logout", { method: "POST", token: access });
  check("logout", logout.status === 200);
  const dead = await call("/v1/auth/me", { token: access });
  check("post-logout access rejected", dead.status === 401);
}

// cleanup test user
try {
  db.prepare(`DELETE FROM users WHERE id = ?`).run(userId);
} catch {
  /* ignore */
}

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
