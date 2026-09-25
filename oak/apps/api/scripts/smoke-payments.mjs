/**
 * Checkout + fulfill smoke (mock provider).
 * Run while the API is listening: node scripts/smoke-payments.mjs
 */
const BASE = process.env.SMOKE_BASE ?? "http://127.0.0.1:8787";
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

async function call(pathname, { method = "GET", body, token, headers } = {}) {
  const h = { ...(headers || {}) };
  if (body) h["Content-Type"] = "application/json";
  if (token) h.Authorization = `Bearer ${token}`;
  const res = await fetch(`${BASE}${pathname}`, {
    method,
    headers: h,
    body: body ? JSON.stringify(body) : undefined,
    redirect: "manual",
  });
  const text = await res.text();
  let json = {};
  try {
    json = text ? JSON.parse(text) : {};
  } catch {
    json = { raw: text.slice(0, 200) };
  }
  return { status: res.status, json, text };
}

const suf = Date.now().toString(36);
const email = `pay_${suf}@test.local`;
const username = `pay_${suf}`;
const password = "OakPay!Test99xx";

console.log("\n== payments ==");

const catalog = await call("/v1/purchases/catalog");
check("catalog lists plans", catalog.status === 200 && catalog.json.plans?.length === 3);
check("dev uses mock provider", catalog.json.provider === "mock" || catalog.json.mock === true);

const guest = await call("/v1/purchases/checkout", {
  method: "POST",
  body: { planId: "day1" },
});
check("checkout requires auth", guest.status === 401);

const reg = await call("/v1/auth/register", {
  method: "POST",
  body: { email, username, password },
});
check("register", reg.status === 201 && !!reg.json.accessToken);
const token = reg.json.accessToken;

const checkout = await call("/v1/purchases/checkout", {
  method: "POST",
  token,
  body: { planId: "day7", provider: "mock" },
});
check(
  "checkout creates session",
  checkout.status === 201 && !!checkout.json.checkoutUrl && !!checkout.json.purchaseId,
  JSON.stringify(checkout.json)
);

const pay = await call(
  `/v1/purchases/mock/pay?purchaseId=${encodeURIComponent(checkout.json.purchaseId)}`
);
check("mock pay fulfills", pay.status === 200 && /Payment mock OK/i.test(pay.text));

const me = await call("/v1/auth/me", { token });
check(
  "license auto-redeemed",
  me.status === 200 && me.json.license?.status === "active" && me.json.license?.plan === "day7",
  JSON.stringify(me.json.license)
);

const replay = await call(
  `/v1/purchases/mock/pay?purchaseId=${encodeURIComponent(checkout.json.purchaseId)}`
);
check("replay mock pay is harmless", replay.status === 404 || /already|Invalid/i.test(replay.text));

const admin = await call("/v1/auth/login", {
  method: "POST",
  body: { login: "admin@oak.local", password: "OakAdmin!ChangeMe" },
});
const purchases = await call("/v1/admin/purchases", { token: admin.json.accessToken });
check(
  "admin sees purchases",
  purchases.status === 200 &&
    (purchases.json.purchases || []).some((p) => p.id === checkout.json.purchaseId)
);

const badSig = await call("/v1/purchases/webhooks/nowpayments", {
  method: "POST",
  body: { order_id: checkout.json.purchaseId, payment_status: "finished" },
  headers: { "x-nowpayments-sig": "deadbeef" },
});
check("nowpayments bad signature rejected", badSig.status === 401);

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
