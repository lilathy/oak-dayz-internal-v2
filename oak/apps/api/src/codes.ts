import crypto from "node:crypto";
import { config } from "./config.js";
import { sha256 } from "./crypto.js";
import { audit, cryptoRandomId, db, nowIso } from "./db.js";
import { getPlan, type PlanId } from "./plans.js";

export type RedeemCodeRow = {
  id: string;
  code_hash: string;
  code_hint: string;
  plan_id: string;
  duration_days: number;
  status: "unused" | "redeemed" | "revoked";
  created_by: "admin" | "purchase";
  created_by_user_id: string | null;
  buyer_user_id: string | null;
  redeemed_by_user_id: string | null;
  redeemed_at: string | null;
  purchase_id: string | null;
  note: string | null;
  created_at: string;
};

/** Raw format: OAK-XXXX-XXXX-XXXX-XXXX (64 bits of entropy) */
export function generateRawCode(): string {
  const seg = () => crypto.randomBytes(2).toString("hex").toUpperCase();
  return `OAK-${seg()}-${seg()}-${seg()}-${seg()}`;
}

export function normalizeCode(raw: string): string {
  return raw.trim().toUpperCase().replace(/\s+/g, "");
}

export function hashRedeemCode(raw: string): string {
  return sha256(`${config.codePepper}:code:${normalizeCode(raw)}`);
}

export function codeHint(raw: string): string {
  const n = normalizeCode(raw);
  if (n.length < 8) return "OAK-****";
  return `${n.slice(0, 4)}-****-${n.slice(-4)}`;
}

export type CreateCodeOpts = {
  planId: PlanId;
  createdBy: "admin" | "purchase";
  createdByUserId?: string | null;
  buyerUserId?: string | null;
  purchaseId?: string | null;
  note?: string | null;
};

export function createRedeemCode(opts: CreateCodeOpts): { row: RedeemCodeRow; raw: string } {
  const plan = getPlan(opts.planId);
  if (!plan) throw new Error("invalid_plan");

  // Retry a few times on extremely rare hash collision.
  for (let i = 0; i < 5; i++) {
    const raw = generateRawCode();
    const hash = hashRedeemCode(raw);
    const id = cryptoRandomId();
    const t = nowIso();
    try {
      db.prepare(
        `INSERT INTO redeem_codes
         (id, code_hash, code_hint, plan_id, duration_days, status, created_by, created_by_user_id,
          buyer_user_id, redeemed_by_user_id, redeemed_at, purchase_id, note, created_at)
         VALUES (?, ?, ?, ?, ?, 'unused', ?, ?, ?, NULL, NULL, ?, ?, ?)`
      ).run(
        id,
        hash,
        codeHint(raw),
        plan.id,
        plan.duration_days,
        opts.createdBy,
        opts.createdByUserId ?? null,
        opts.buyerUserId ?? null,
        opts.purchaseId ?? null,
        opts.note ?? null,
        t
      );
      const row = db.prepare(`SELECT * FROM redeem_codes WHERE id = ?`).get(id) as RedeemCodeRow;
      return { row, raw };
    } catch (e) {
      const msg = e instanceof Error ? e.message : String(e);
      if (msg.includes("UNIQUE")) continue;
      throw e;
    }
  }
  throw new Error("code_gen_failed");
}

export function publicRedeemCode(row: RedeemCodeRow) {
  return {
    id: row.id,
    hint: row.code_hint,
    planId: row.plan_id,
    durationDays: row.duration_days,
    status: row.status,
    createdBy: row.created_by,
    createdByUserId: row.created_by_user_id,
    buyerUserId: row.buyer_user_id,
    redeemedByUserId: row.redeemed_by_user_id,
    redeemedAt: row.redeemed_at,
    purchaseId: row.purchase_id,
    note: row.note,
    createdAt: row.created_at,
  };
}

export function findCodeByRaw(raw: string): RedeemCodeRow | undefined {
  return db
    .prepare(`SELECT * FROM redeem_codes WHERE code_hash = ?`)
    .get(hashRedeemCode(raw)) as RedeemCodeRow | undefined;
}

export function revokeCode(id: string, actorId: string, ip?: string | null): RedeemCodeRow | null {
  const row = db.prepare(`SELECT * FROM redeem_codes WHERE id = ?`).get(id) as RedeemCodeRow | undefined;
  if (!row) return null;
  if (row.status !== "unused") return row;
  db.prepare(`UPDATE redeem_codes SET status = 'revoked' WHERE id = ?`).run(id);
  audit("admin.code_revoke", { actorId, targetId: id, ip: ip ?? null });
  return db.prepare(`SELECT * FROM redeem_codes WHERE id = ?`).get(id) as RedeemCodeRow;
}
