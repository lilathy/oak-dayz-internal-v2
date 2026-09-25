import { db } from "./db.js";

/** Legacy DayZ ids plus per-product plan ids for rust/eft/cs2. */
export type PlanId =
  | "day1"
  | "day7"
  | "day30"
  | "rust-day1"
  | "rust-day7"
  | "rust-day30"
  | "eft-day1"
  | "eft-day7"
  | "eft-day30"
  | "cs2-day1"
  | "cs2-day7"
  | "cs2-day30";

export type PlanRow = {
  id: PlanId;
  name: string;
  duration_days: number;
  product_slug: string;
};

export const PLANS: Record<PlanId, PlanRow> = {
  day1: { id: "day1", name: "DayZ — 1 Day", duration_days: 1, product_slug: "dayz" },
  day7: { id: "day7", name: "DayZ — 7 Day", duration_days: 7, product_slug: "dayz" },
  day30: { id: "day30", name: "DayZ — 30 Day", duration_days: 30, product_slug: "dayz" },
  "rust-day1": { id: "rust-day1", name: "Rust — 1 Day", duration_days: 1, product_slug: "rust" },
  "rust-day7": { id: "rust-day7", name: "Rust — 7 Day", duration_days: 7, product_slug: "rust" },
  "rust-day30": { id: "rust-day30", name: "Rust — 30 Day", duration_days: 30, product_slug: "rust" },
  "eft-day1": { id: "eft-day1", name: "EFT — 1 Day", duration_days: 1, product_slug: "eft" },
  "eft-day7": { id: "eft-day7", name: "EFT — 7 Day", duration_days: 7, product_slug: "eft" },
  "eft-day30": { id: "eft-day30", name: "EFT — 30 Day", duration_days: 30, product_slug: "eft" },
  "cs2-day1": { id: "cs2-day1", name: "CS2 — 1 Day", duration_days: 1, product_slug: "cs2" },
  "cs2-day7": { id: "cs2-day7", name: "CS2 — 7 Day", duration_days: 7, product_slug: "cs2" },
  "cs2-day30": { id: "cs2-day30", name: "CS2 — 30 Day", duration_days: 30, product_slug: "cs2" },
};

/** List prices in USD for checkout invoices. */
export const PLAN_PRICE_USD: Record<PlanId, number> = {
  day1: 4.99,
  day7: 14.99,
  day30: 39.99,
  "rust-day1": 4.99,
  "rust-day7": 14.99,
  "rust-day30": 39.99,
  "eft-day1": 4.99,
  "eft-day7": 14.99,
  "eft-day30": 39.99,
  "cs2-day1": 4.99,
  "cs2-day7": 14.99,
  "cs2-day30": 39.99,
};

export function ensurePlansSchema(): void {
  db.exec(`
CREATE TABLE IF NOT EXISTS plans (
  id              TEXT PRIMARY KEY,
  name            TEXT NOT NULL,
  duration_days   INTEGER NOT NULL CHECK(duration_days > 0)
);

CREATE TABLE IF NOT EXISTS redeem_codes (
  id                    TEXT PRIMARY KEY,
  code_hash             TEXT NOT NULL UNIQUE,
  code_hint             TEXT NOT NULL,
  plan_id               TEXT NOT NULL REFERENCES plans(id),
  duration_days         INTEGER NOT NULL,
  status                TEXT NOT NULL DEFAULT 'unused'
                          CHECK(status IN ('unused','redeemed','revoked')),
  created_by            TEXT NOT NULL CHECK(created_by IN ('admin','purchase')),
  created_by_user_id    TEXT,
  buyer_user_id         TEXT,
  redeemed_by_user_id   TEXT,
  redeemed_at           TEXT,
  purchase_id           TEXT,
  note                  TEXT,
  created_at            TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_redeem_status ON redeem_codes(status);
CREATE INDEX IF NOT EXISTS idx_redeem_plan ON redeem_codes(plan_id);
CREATE INDEX IF NOT EXISTS idx_redeem_buyer ON redeem_codes(buyer_user_id);
`);

  // Additive column for existing DBs.
  const cols = db.prepare(`PRAGMA table_info(plans)`).all() as { name: string }[];
  if (!cols.some((c) => c.name === "product_slug")) {
    db.exec(`ALTER TABLE plans ADD COLUMN product_slug TEXT NOT NULL DEFAULT 'dayz'`);
  }

  const upsert = db.prepare(
    `INSERT INTO plans (id, name, duration_days, product_slug) VALUES (?, ?, ?, ?)
     ON CONFLICT(id) DO UPDATE SET
       name = excluded.name,
       duration_days = excluded.duration_days,
       product_slug = excluded.product_slug`
  );
  for (const p of Object.values(PLANS)) {
    upsert.run(p.id, p.name, p.duration_days, p.product_slug);
  }
}

export function getPlan(planId: string): PlanRow | undefined {
  const row = db
    .prepare(`SELECT id, name, duration_days, product_slug FROM plans WHERE id = ?`)
    .get(planId) as PlanRow | undefined;
  return row;
}

export function isPlanId(v: string): v is PlanId {
  return Object.prototype.hasOwnProperty.call(PLANS, v);
}

export function listPlans(productSlug?: string): PlanRow[] {
  if (productSlug) {
    return db
      .prepare(
        `SELECT id, name, duration_days, product_slug FROM plans
         WHERE product_slug = ? ORDER BY duration_days ASC`
      )
      .all(productSlug) as PlanRow[];
  }
  return db
    .prepare(`SELECT id, name, duration_days, product_slug FROM plans ORDER BY product_slug, duration_days ASC`)
    .all() as PlanRow[];
}
