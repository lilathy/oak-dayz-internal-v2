import crypto from "node:crypto";
import { config } from "./config.js";
import { createRedeemCode } from "./codes.js";
import { audit, cryptoRandomId, db, nowIso, type UserRow } from "./db.js";
import { addLicenseDays, publicLicense, syncLicenseStatus } from "./license.js";
import { sendMail } from "./mail.js";
import { isPlanId, PLAN_PRICE_USD, type PlanId } from "./plans.js";
import { raiseSecurityAlert } from "./securityAlerts.js";

export type PaymentProvider = "nowpayments" | "mock" | "internal";

export type PurchaseRow = {
  id: string;
  user_id: string;
  plan_id: string;
  provider: string;
  status: "pending" | "paid" | "failed" | "refunded";
  amount_usd: number;
  provider_ref: string | null;
  created_at: string;
  paid_at: string | null;
  meta_json: string | null;
};

export function ensurePurchasesSchema(): void {
  db.exec(`
CREATE TABLE IF NOT EXISTS purchases (
  id            TEXT PRIMARY KEY,
  user_id       TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  plan_id       TEXT NOT NULL,
  provider      TEXT NOT NULL,
  status        TEXT NOT NULL DEFAULT 'pending'
                  CHECK(status IN ('pending','paid','failed','refunded')),
  amount_usd    REAL NOT NULL,
  provider_ref  TEXT,
  created_at    TEXT NOT NULL,
  paid_at       TEXT,
  meta_json     TEXT
);
CREATE INDEX IF NOT EXISTS idx_purchases_user ON purchases(user_id);
CREATE INDEX IF NOT EXISTS idx_purchases_status ON purchases(status);
CREATE INDEX IF NOT EXISTS idx_purchases_provider_ref ON purchases(provider_ref);
`);
}

export function getPurchase(id: string): PurchaseRow | undefined {
  return db.prepare(`SELECT * FROM purchases WHERE id = ?`).get(id) as PurchaseRow | undefined;
}

export function listPurchases(limit = 100): Array<PurchaseRow & { email?: string; username?: string }> {
  return db
    .prepare(
      `SELECT p.*, u.email, u.username
       FROM purchases p
       JOIN users u ON u.id = p.user_id
       ORDER BY p.created_at DESC
       LIMIT ?`
    )
    .all(Math.min(limit, 500)) as Array<PurchaseRow & { email?: string; username?: string }>;
}

export function createPendingPurchase(opts: {
  userId: string;
  planId: PlanId;
  provider: PaymentProvider;
  meta?: Record<string, unknown>;
}): PurchaseRow {
  const id = cryptoRandomId();
  const t = nowIso();
  db.prepare(
    `INSERT INTO purchases
       (id, user_id, plan_id, provider, status, amount_usd, provider_ref, created_at, paid_at, meta_json)
     VALUES (?, ?, ?, ?, 'pending', ?, NULL, ?, NULL, ?)`
  ).run(
    id,
    opts.userId,
    opts.planId,
    opts.provider,
    PLAN_PRICE_USD[opts.planId],
    t,
    opts.meta ? JSON.stringify(opts.meta) : null
  );
  return getPurchase(id)!;
}

export function setPurchaseProviderRef(purchaseId: string, providerRef: string): void {
  db.prepare(`UPDATE purchases SET provider_ref = ? WHERE id = ?`).run(providerRef, purchaseId);
}

export type FulfillResult = {
  alreadyFulfilled: boolean;
  purchaseId: string;
  codeId: string;
  rawCode: string;
  planId: string;
  durationDays: number;
  buyerUserId: string;
  autoRedeemed: boolean;
  license: ReturnType<typeof publicLicense>;
};

/**
 * Idempotent fulfill: buyer-bound code + optional auto-redeem into license days.
 */
export function fulfillPaidPurchase(opts: {
  purchaseId: string;
  planId: PlanId;
  buyerUserId: string;
  provider: PaymentProvider;
  providerRef?: string | null;
  note?: string | null;
  autoRedeem?: boolean;
  ip?: string | null;
}): FulfillResult {
  const autoRedeem = opts.autoRedeem !== false;
  const existingCode = db
    .prepare(`SELECT id, code_hint, plan_id, duration_days, status FROM redeem_codes WHERE purchase_id = ?`)
    .get(opts.purchaseId) as
    | {
        id: string;
        code_hint: string;
        plan_id: string;
        duration_days: number;
        status: string;
      }
    | undefined;

  if (existingCode) {
    const purchase = getPurchase(opts.purchaseId);
    return {
      alreadyFulfilled: true,
      purchaseId: opts.purchaseId,
      codeId: existingCode.id,
      rawCode: "",
      planId: existingCode.plan_id,
      durationDays: existingCode.duration_days,
      buyerUserId: opts.buyerUserId,
      autoRedeemed: existingCode.status === "redeemed",
      license: publicLicense(syncLicenseStatus(opts.buyerUserId)),
    };
  }

  const result = db.transaction(() => {
    const { row, raw } = createRedeemCode({
      planId: opts.planId,
      createdBy: "purchase",
      createdByUserId: null,
      buyerUserId: opts.buyerUserId,
      purchaseId: opts.purchaseId,
      note: opts.note ?? `provider:${opts.provider}`,
    });

    let license = syncLicenseStatus(opts.buyerUserId);
    if (autoRedeem) {
      db.prepare(
        `UPDATE redeem_codes
         SET status = 'redeemed', redeemed_by_user_id = ?, redeemed_at = ?
         WHERE id = ? AND status = 'unused'`
      ).run(opts.buyerUserId, nowIso(), row.id);
      license = addLicenseDays(opts.buyerUserId, row.duration_days, row.plan_id);
    }

    const purchase = getPurchase(opts.purchaseId);
    if (purchase) {
      db.prepare(
        `UPDATE purchases
         SET status = 'paid', paid_at = ?, provider_ref = COALESCE(?, provider_ref)
         WHERE id = ?`
      ).run(nowIso(), opts.providerRef ?? null, opts.purchaseId);
    } else {
      // Internal fulfill without a prior checkout row — insert a paid row for audit.
      db.prepare(
        `INSERT INTO purchases
           (id, user_id, plan_id, provider, status, amount_usd, provider_ref, created_at, paid_at, meta_json)
         VALUES (?, ?, ?, ?, 'paid', ?, ?, ?, ?, NULL)`
      ).run(
        opts.purchaseId,
        opts.buyerUserId,
        opts.planId,
        opts.provider,
        PLAN_PRICE_USD[opts.planId],
        opts.providerRef ?? null,
        nowIso(),
        nowIso()
      );
    }

    return { row, raw, license };
  })();

  audit("purchase.fulfill", {
    actorId: null,
    targetId: opts.buyerUserId,
    meta: {
      planId: opts.planId,
      purchaseId: opts.purchaseId,
      codeId: result.row.id,
      provider: opts.provider,
      autoRedeem,
    },
    ip: opts.ip ?? null,
  });

  return {
    alreadyFulfilled: false,
    purchaseId: opts.purchaseId,
    codeId: result.row.id,
    rawCode: result.raw,
    planId: result.row.plan_id,
    durationDays: result.row.duration_days,
    buyerUserId: opts.buyerUserId,
    autoRedeemed: autoRedeem,
    license: publicLicense(result.license),
  };
}

export async function emailPurchaseReceipt(opts: {
  user: UserRow;
  planId: string;
  durationDays: number;
  rawCode: string;
  autoRedeemed: boolean;
}): Promise<void> {
  const accountUrl = `${config.publicSiteUrl}/account`;
  const lines = [
    `Thanks for your Oak purchase (${opts.planId}, ${opts.durationDays} day(s)).`,
    "",
    opts.autoRedeemed
      ? "License time was applied to your account automatically."
      : "Redeem this code on your Account page:",
    opts.rawCode ? `Code: ${opts.rawCode}` : "(code already issued — check Account)",
    "",
    `Account: ${accountUrl}`,
    "",
    "Keep this email. Codes are bound to your account and cannot be gifted.",
  ];
  await sendMail({
    to: opts.user.email,
    subject: "Oak purchase receipt",
    text: lines.join("\n"),
  });
}

/**
 * NOWPayments IPN: HMAC-SHA512 of JSON.stringify(body, Object.keys(body).sort())
 * compared to header x-nowpayments-sig.
 */
export function verifyNowPaymentsIpn(
  body: Record<string, unknown>,
  ipnSecret: string,
  headerSig: string
): boolean {
  if (!ipnSecret || !headerSig) return false;
  const sorted = JSON.stringify(body, Object.keys(body).sort());
  const expected = crypto.createHmac("sha512", ipnSecret).update(sorted).digest("hex");
  const a = Buffer.from(expected);
  const b = Buffer.from(headerSig.trim());
  try {
    return a.length === b.length && crypto.timingSafeEqual(a, b);
  } catch {
    return false;
  }
}

export function resolveDefaultProvider(): PaymentProvider {
  if (config.paymentMock) return "mock";
  if (config.nowpaymentsApiKey && config.nowpaymentsIpnSecret) return "nowpayments";
  // Dev without keys → mock. Production without keys still reports nowpayments so checkout 503s
  // instead of accidentally offering free mock pay.
  return config.isProd ? "nowpayments" : "mock";
}

export async function createNowPaymentsInvoice(opts: {
  purchaseId: string;
  planId: PlanId;
  amountUsd: number;
}): Promise<{ url: string; providerRef: string }> {
  if (!config.nowpaymentsApiKey) {
    throw new Error("nowpayments_not_configured");
  }
  // Omit pay_currency so the invoice lets the buyer pick crypto *or* fiat/card
  // (fiat on-ramp appears once enabled in the NOWPayments dashboard).
  const payload = {
    price_amount: opts.amountUsd,
    price_currency: "usd",
    order_id: opts.purchaseId,
    order_description: `Oak ${opts.planId}`,
    ipn_callback_url:
      config.nowpaymentsCallbackUrl ||
      `${config.publicApiUrl}/v1/purchases/webhooks/nowpayments`,
    success_url: `${config.publicSiteUrl}/account?purchase=ok`,
    cancel_url: `${config.publicSiteUrl}/plans?purchase=cancel`,
  };
  const res = await fetch("https://api.nowpayments.io/v1/invoice", {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      "x-api-key": config.nowpaymentsApiKey,
    },
    body: JSON.stringify(payload),
    signal: AbortSignal.timeout(15000),
  });
  const json = (await res.json()) as {
    id?: string | number;
    invoice_url?: string;
    message?: string;
  };
  if (!res.ok || !json.invoice_url) {
    raiseSecurityAlert({
      severity: "high",
      category: "payment",
      title: "NOWPayments invoice failed",
      summary: json.message || `HTTP ${res.status}`,
      dedupeKey: `nowpayments_invoice_fail:${new Date().toISOString().slice(0, 13)}`,
      evidence: { path: "purchases.nowpayments", planId: opts.planId },
    });
    throw new Error("nowpayments_invoice_failed");
  }
  return { url: json.invoice_url, providerRef: String(json.id ?? opts.purchaseId) };
}

export function assertPlanId(v: string): PlanId {
  if (!isPlanId(v)) throw new Error("invalid_plan");
  return v;
}
