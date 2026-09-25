import crypto from "node:crypto";
import { Router } from "express";
import rateLimit from "express-rate-limit";
import { z } from "zod";
import { config } from "../config.js";
import { db, type UserRow } from "../db.js";
import { clientIp, requireAuth, type AuthedRequest } from "../middleware/auth.js";
import { isPlanId, PLAN_PRICE_USD, PLANS, type PlanId } from "../plans.js";
import {
  createNowPaymentsInvoice,
  createPendingPurchase,
  emailPurchaseReceipt,
  fulfillPaidPurchase,
  getPurchase,
  resolveDefaultProvider,
  setPurchaseProviderRef,
  verifyNowPaymentsIpn,
  type PaymentProvider,
} from "../purchases.js";
import { raiseSecurityAlert } from "../securityAlerts.js";
import { trackEvent } from "../analytics.js";

export const purchasesRouter = Router();

const checkoutLimiter = rateLimit({
  windowMs: 15 * 60 * 1000,
  max: 20,
  standardHeaders: true,
  legacyHeaders: false,
  keyGenerator: (req) => (req as AuthedRequest).user?.id ?? clientIp(req),
  message: { error: "rate_limited" },
});

function requireFulfillSecret(req: import("express").Request, res: import("express").Response): boolean {
  const secret = config.fulfillSecret;
  if (!secret) {
    res.status(503).json({ error: "fulfill_not_configured" });
    return false;
  }
  const hdr = req.get("X-Oak-Fulfill-Secret") ?? "";
  const a = crypto.createHash("sha256").update(hdr).digest();
  const b = crypto.createHash("sha256").update(secret).digest();
  if (a.length !== b.length || !crypto.timingSafeEqual(a, b)) {
    res.status(401).json({ error: "unauthorized" });
    return false;
  }
  return true;
}

async function completeFulfill(opts: {
  purchaseId: string;
  planId: PlanId;
  buyerUserId: string;
  provider: PaymentProvider;
  providerRef?: string | null;
  note?: string | null;
  autoRedeem?: boolean;
  ip?: string | null;
}) {
  const buyer = db.prepare(`SELECT * FROM users WHERE id = ?`).get(opts.buyerUserId) as UserRow | undefined;
  if (!buyer) throw new Error("buyer_not_found");
  const result = fulfillPaidPurchase(opts);
  if (!result.alreadyFulfilled && result.rawCode) {
    await emailPurchaseReceipt({
      user: buyer,
      planId: result.planId,
      durationDays: result.durationDays,
      rawCode: result.rawCode,
      autoRedeemed: result.autoRedeemed,
    });
  }
  trackEvent("purchase", {
    userId: opts.buyerUserId,
    meta: { planId: opts.planId, provider: opts.provider, autoRedeem: result.autoRedeemed },
    ip: opts.ip ?? undefined,
  });
  return result;
}

/** Public catalog for the plans page. Optional ?product=dayz|rust|eft */
purchasesRouter.get("/catalog", (req, res) => {
  const product =
    typeof req.query.product === "string" && req.query.product.trim()
      ? req.query.product.trim().toLowerCase()
      : "";
  const provider = resolveDefaultProvider();
  const planIds = (Object.keys(PLANS) as PlanId[]).filter(
    (id) => !product || PLANS[id].product_slug === product
  );
  res.json({
    provider,
    mock: provider === "mock",
    methods: provider === "mock" ? ["mock"] : ["crypto", "card", "fiat"],
    product: product || null,
    plans: planIds.map((id) => ({
      id,
      name: PLANS[id].name,
      durationDays: PLANS[id].duration_days,
      priceUsd: PLAN_PRICE_USD[id],
      productSlug: PLANS[id].product_slug,
    })),
  });
});

/**
 * Start checkout for the signed-in user. Register-before-buy keeps HWID/abuse model clean.
 */
purchasesRouter.post("/checkout", requireAuth, checkoutLimiter, async (req: AuthedRequest, res) => {
  const parsed = z
    .object({
      planId: z.string().min(1).max(32),
      provider: z.enum(["nowpayments", "mock"]).optional(),
    })
    .safeParse(req.body);
  if (!parsed.success) {
    res.status(400).json({ error: "invalid_body", details: parsed.error.flatten() });
    return;
  }
  if (!isPlanId(parsed.data.planId)) {
    res.status(400).json({ error: "invalid_plan" });
    return;
  }
  if (req.user!.banned) {
    res.status(403).json({ error: "banned" });
    return;
  }

  let provider: PaymentProvider = parsed.data.provider ?? resolveDefaultProvider();
  if (provider === "nowpayments" && (!config.nowpaymentsApiKey || config.paymentMock)) {
    provider = config.paymentMock ? "mock" : provider;
  }
  if (provider === "nowpayments" && !config.nowpaymentsApiKey) {
    res.status(503).json({ error: "nowpayments_not_configured" });
    return;
  }

  const purchase = createPendingPurchase({
    userId: req.user!.id,
    planId: parsed.data.planId,
    provider,
  });

  try {
    let checkoutUrl = "";
    if (provider === "mock") {
      checkoutUrl = `${config.publicApiUrl}/v1/purchases/mock/pay?purchaseId=${encodeURIComponent(purchase.id)}`;
    } else if (provider === "nowpayments") {
      const inv = await createNowPaymentsInvoice({
        purchaseId: purchase.id,
        planId: parsed.data.planId,
        amountUsd: purchase.amount_usd,
      });
      setPurchaseProviderRef(purchase.id, inv.providerRef);
      checkoutUrl = inv.url;
    } else {
      res.status(503).json({ error: "provider_unavailable" });
      return;
    }

    trackEvent("checkout_start", {
      userId: req.user!.id,
      meta: { planId: parsed.data.planId, provider },
      ip: clientIp(req),
    });

    res.status(201).json({
      purchaseId: purchase.id,
      provider,
      checkoutUrl,
      amountUsd: purchase.amount_usd,
      planId: parsed.data.planId,
    });
  } catch (e) {
    const msg = e instanceof Error ? e.message : "checkout_failed";
    res.status(502).json({ error: msg });
  }
});

/**
 * Dev-only mock pay page: completes a pending purchase without a real processor.
 */
purchasesRouter.get("/mock/pay", async (req, res) => {
  // Mock checkout is development-only — never honor PAYMENT_MOCK in production.
  if (config.isProd || !config.paymentMock) {
    res.status(404).json({ error: "not_found" });
    return;
  }
  const purchaseId = typeof req.query.purchaseId === "string" ? req.query.purchaseId : "";
  const purchase = purchaseId ? getPurchase(purchaseId) : undefined;
  if (!purchase || purchase.status !== "pending") {
    res.status(404).type("html").send("<h1>Invalid or already paid purchase</h1>");
    return;
  }
  try {
    await completeFulfill({
      purchaseId: purchase.id,
      planId: purchase.plan_id as PlanId,
      buyerUserId: purchase.user_id,
      provider: "mock",
      providerRef: "mock",
      autoRedeem: true,
      ip: clientIp(req),
    });
    trackEvent("checkout_ok", {
      userId: purchase.user_id,
      meta: { planId: purchase.plan_id, provider: "mock" },
      ip: clientIp(req),
    });
    res
      .status(200)
      .type("html")
      .send(
        `<!doctype html><meta charset="utf-8"><title>Oak mock pay</title>
         <h1>Payment mock OK</h1>
         <p>License applied for plan <code>${purchase.plan_id}</code>.</p>
         <p><a href="${config.publicSiteUrl}/account">Back to Account</a></p>`
      );
  } catch (e) {
    const msg = e instanceof Error ? e.message : "error";
    res.status(500).type("html").send(`<h1>Fulfill failed</h1><pre>${msg}</pre>`);
  }
});

/** NOWPayments IPN — HMAC-SHA512 in x-nowpayments-sig. */
purchasesRouter.post("/webhooks/nowpayments", async (req, res) => {
  const body = (req.body ?? {}) as Record<string, unknown>;
  const sig = req.get("x-nowpayments-sig") || "";
  if (!verifyNowPaymentsIpn(body, config.nowpaymentsIpnSecret, sig)) {
    raiseSecurityAlert({
      severity: "critical",
      category: "payment",
      title: "NOWPayments webhook bad signature",
      summary: "Rejected IPN with invalid x-nowpayments-sig",
      dedupeKey: `nowpayments_bad_sig:${new Date().toISOString().slice(0, 13)}`,
      evidence: { path: "webhooks.nowpayments" },
      ip: clientIp(req),
    });
    res.status(401).json({ error: "invalid_signature" });
    return;
  }

  const status = String(body.payment_status ?? body.status ?? "").toLowerCase();
  // finished = fully paid; confirmed = on-chain confirmed (also safe to fulfill)
  const paid = status === "finished" || status === "confirmed";
  if (!paid) {
    res.json({ ok: true, ignored: true, status });
    return;
  }

  const purchaseId = String(body.order_id ?? "");
  const purchase = getPurchase(purchaseId);
  if (!purchase) {
    res.status(404).json({ error: "purchase_not_found" });
    return;
  }
  if (!isPlanId(purchase.plan_id)) {
    res.status(400).json({ error: "invalid_plan" });
    return;
  }

  try {
    const result = await completeFulfill({
      purchaseId: purchase.id,
      planId: purchase.plan_id,
      buyerUserId: purchase.user_id,
      provider: "nowpayments",
      providerRef:
        body.payment_id != null
          ? String(body.payment_id)
          : body.invoice_id != null
            ? String(body.invoice_id)
            : null,
      autoRedeem: true,
      ip: clientIp(req),
    });
    res.json({ ok: true, alreadyFulfilled: result.alreadyFulfilled });
  } catch (e) {
    const msg = e instanceof Error ? e.message : "error";
    raiseSecurityAlert({
      severity: "high",
      category: "payment",
      title: "NOWPayments fulfill failed",
      summary: msg,
      dedupeKey: `nowpayments_fulfill_fail:${purchaseId}`,
      userId: purchase.user_id,
      evidence: { path: "webhooks.nowpayments" },
      ip: clientIp(req),
    });
    res.status(500).json({ error: msg });
  }
});

/**
 * Internal fulfillment (admin tooling / migration). Prefer signed provider webhooks in production.
 */
purchasesRouter.post("/fulfill", async (req, res) => {
  if (!requireFulfillSecret(req, res)) return;

  const parsed = z
    .object({
      planId: z.string(),
      buyerUserId: z.string().uuid().optional(),
      buyerEmail: z.string().email().optional(),
      purchaseId: z.string().min(1).max(128).optional(),
      note: z.string().max(500).optional(),
      autoRedeem: z.boolean().optional(),
    })
    .safeParse(req.body);

  if (!parsed.success) {
    res.status(400).json({ error: "invalid_body", details: parsed.error.flatten() });
    return;
  }
  const { planId, buyerUserId, buyerEmail, purchaseId, note, autoRedeem } = parsed.data;
  if (!isPlanId(planId)) {
    res.status(400).json({ error: "invalid_plan" });
    return;
  }
  if (!buyerUserId && !buyerEmail) {
    res.status(400).json({ error: "buyer_required" });
    return;
  }

  let buyer: UserRow | undefined;
  if (buyerUserId) {
    buyer = db.prepare(`SELECT * FROM users WHERE id = ?`).get(buyerUserId) as UserRow | undefined;
  } else if (buyerEmail) {
    buyer = db
      .prepare(`SELECT * FROM users WHERE email = ?`)
      .get(buyerEmail.toLowerCase()) as UserRow | undefined;
  }
  if (!buyer) {
    res.status(404).json({ error: "buyer_not_found" });
    return;
  }

  const id = purchaseId ?? crypto.randomUUID();
  try {
    const result = await completeFulfill({
      purchaseId: id,
      planId,
      buyerUserId: buyer.id,
      provider: "internal",
      note: note ?? null,
      autoRedeem: autoRedeem !== false,
      ip: clientIp(req),
    });
    res.status(result.alreadyFulfilled ? 200 : 201).json({
      ok: true,
      alreadyFulfilled: result.alreadyFulfilled,
      code: result.rawCode || undefined,
      planId: result.planId,
      durationDays: result.durationDays,
      buyerUserId: result.buyerUserId,
      autoRedeemed: result.autoRedeemed,
      license: result.license,
      purchaseId: result.purchaseId,
    });
  } catch (e) {
    const msg = e instanceof Error ? e.message : "error";
    if (msg === "license_banned") {
      res.status(403).json({ error: "license_banned" });
      return;
    }
    res.status(500).json({ error: config.isProd ? "internal" : msg });
  }
});
