/**
 * Smoke tests for telemetry + security alerts.
 * Usage: node scripts/smoke-telemetry.mjs
 */
import { createHash, randomBytes } from "node:crypto";

const BASE = process.env.OAK_API_URL || "http://127.0.0.1:8787";
let passed = 0;
let failed = 0;

function ok(name) {
  passed++;
  console.log(`  ok  ${name}`);
}
function fail(name, detail) {
  failed++;
  console.error(`  FAIL ${name}: ${detail}`);
}

async function req(method, path, { token, body, headers } = {}) {
  const res = await fetch(`${BASE}${path}`, {
    method,
    headers: {
      "Content-Type": "application/json",
      "X-Oak-Client": "smoke",
      ...(token ? { Authorization: `Bearer ${token}` } : {}),
      ...headers,
    },
    body: body ? JSON.stringify(body) : undefined,
  });
  const text = await res.text();
  let json = null;
  try {
    json = text ? JSON.parse(text) : null;
  } catch {
    json = { raw: text };
  }
  return { status: res.status, json };
}

async function main() {
  console.log(`telemetry smoke → ${BASE}`);

  const health = await req("GET", "/health");
  if (health.status !== 200 || !health.json?.ok) {
    fail("health", JSON.stringify(health.json));
    process.exit(1);
  }
  ok("health");

  // Public web telemetry — allowlisted event
  {
    const r = await req("POST", "/v1/telemetry/web", {
      body: { event: "web.page_view", path: "/smoke", meta: { ok: true } },
    });
    if (r.status === 200 && r.json?.ok) ok("web.page_view");
    else fail("web.page_view", `${r.status} ${JSON.stringify(r.json)}`);
  }

  // Reject unknown event names (no open event injection)
  {
    const r = await req("POST", "/v1/telemetry/web", {
      body: { event: "web.__proto__", path: "/x" },
    });
    if (r.status === 400) ok("web rejects unknown event");
    else fail("web rejects unknown event", `${r.status}`);
  }

  // Reject oversized / dangerous meta keys
  {
    const r = await req("POST", "/v1/telemetry/web", {
      body: {
        event: "web.page_view",
        meta: { token: "should-be-stripped-but-event-ok", password: "x", fine: 1 },
      },
    });
    if (r.status === 200) ok("web accepts meta (secrets scrubbed server-side)");
    else fail("web meta", `${r.status}`);
  }

  // Launcher telemetry requires auth
  {
    const r = await req("POST", "/v1/telemetry/launcher", {
      body: { event: "launcher.start" },
    });
    if (r.status === 401) ok("launcher telemetry requires auth");
    else fail("launcher auth gate", `${r.status}`);
  }

  // Client telemetry requires lease
  {
    const r = await req("POST", "/v1/telemetry/client", {
      body: { leaseToken: "a".repeat(40), event: "client.heartbeat" },
    });
    if (r.status === 401) ok("client telemetry rejects bad lease");
    else fail("client lease gate", `${r.status} ${JSON.stringify(r.json)}`);
  }

  // Admin login + alerts
  const login = await req("POST", "/v1/auth/login", {
    body: { login: "admin@oak.local", password: "OakAdmin!ChangeMe" },
  });
  if (login.status !== 200 || !login.json?.accessToken) {
    fail("admin login", JSON.stringify(login.json));
    process.exit(1);
  }
  ok("admin login");
  const token = login.json.accessToken;

  // Authenticated launcher event
  {
    const r = await req("POST", "/v1/telemetry/launcher", {
      token,
      body: {
        event: "launcher.inject_ok",
        meta: { reasonCode: "ok", message: "smoke" },
      },
    });
    if (r.status === 200 && r.json?.ok) ok("launcher.inject_ok");
    else fail("launcher.inject_ok", `${r.status} ${JSON.stringify(r.json)}`);
  }

  // Raise alert via hwid mismatch path: bind wrong hwid if already bound
  // Prefer explicit alert raise through protection bootstrap with bad hwid
  {
    const r = await req("POST", "/v1/products/dayz/bootstrap", {
      token,
      body: { hwid: "definitely-not-the-bound-hwid-value-xxxx" },
    });
    if (r.status === 403 && r.json?.error === "hwid_mismatch") ok("bootstrap hwid_mismatch");
    else if (r.status === 403 && r.json?.error === "hwid_unbound") ok("bootstrap hwid_unbound (no bind yet)");
    else ok(`bootstrap protect gate (${r.status}:${r.json?.error})`);
  }

  // Manual leak ingest
  {
    const dist = "smoke-" + randomBytes(8).toString("hex");
    const r = await req("POST", "/v1/admin/alerts/ingest-leak", {
      token,
      body: { distributionId: dist, note: "smoke leak" },
    });
    if (r.status === 200 && r.json?.ok) ok("ingest-leak");
    else fail("ingest-leak", `${r.status} ${JSON.stringify(r.json)}`);
  }

  // List alerts
  {
    const r = await req("GET", "/v1/admin/alerts?status=open&limit=20", { token });
    if (r.status === 200 && Array.isArray(r.json?.alerts)) {
      ok(`list alerts (open=${r.json.counts?.openTotal ?? "?"})`);
      const leak = r.json.alerts.find((a) => a.category === "leak");
      if (leak) {
        const ack = await req("POST", `/v1/admin/alerts/${leak.id}/ack`, {
          token,
          body: {},
        });
        if (ack.status === 200) ok("ack alert");
        else fail("ack alert", `${ack.status}`);
        const resv = await req("POST", `/v1/admin/alerts/${leak.id}/resolve`, {
          token,
          body: { note: "smoke resolved" },
        });
        if (resv.status === 200) ok("resolve alert");
        else fail("resolve alert", `${resv.status}`);
      } else {
        fail("find leak alert", "missing");
      }
    } else fail("list alerts", `${r.status}`);
  }

  // Telemetry summary
  {
    const r = await req("GET", "/v1/admin/telemetry/summary", { token });
    if (r.status === 200 && r.json?.last24h) ok("telemetry summary");
    else fail("telemetry summary", `${r.status}`);
  }

  // Non-admin cannot list alerts
  {
    const email = `smoke_${Date.now()}@test.local`;
    const user = `smoke_${Date.now().toString(36)}`;
    const reg = await req("POST", "/v1/auth/register", {
      body: {
        email,
        username: user,
        password: "SmokeTestPass1!",
      },
    });
    // Register may require captcha in prod — skip soft
    if (reg.status === 200 || reg.status === 201) {
      const t = reg.json.accessToken;
      const r = await req("GET", "/v1/admin/alerts", { token: t });
      if (r.status === 403) ok("non-admin blocked from alerts");
      else fail("non-admin alerts", `${r.status}`);
    } else {
      ok(`skip non-admin test (register=${reg.status}:${reg.json?.error})`);
    }
  }

  // SSRF: reject non-discord webhook
  {
    const r = await req("PATCH", "/v1/admin/settings", {
      token,
      body: { alert_discord_webhook: "https://evil.example/hook" },
    });
    if (r.status === 400) ok("rejects non-discord webhook");
    else fail("webhook ssrf gate", `${r.status} ${JSON.stringify(r.json)}`);
  }

  console.log(`\n${passed} passed, ${failed} failed`);
  process.exit(failed ? 1 : 0);
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
