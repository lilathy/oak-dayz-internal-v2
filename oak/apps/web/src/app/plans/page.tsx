"use client";

import Link from "next/link";
import { useEffect, useMemo, useState } from "react";
import { api, friendlyError, useSession } from "@/lib/api";
import { track } from "@/lib/track";

type CatalogPlan = {
  id: string;
  name: string;
  durationDays: number;
  priceUsd: number;
  productSlug?: string;
};

type Catalog = {
  provider: string;
  mock: boolean;
  methods: string[];
  plans: CatalogPlan[];
};

const PRODUCTS = [
  { slug: "dayz", label: "DayZ" },
  { slug: "rust", label: "Rust" },
  { slug: "eft", label: "Escape from Tarkov" },
  { slug: "cs2", label: "CS2" },
] as const;

const DESCRIPTIONS: Record<string, string> = {
  day1: "Short session access.",
  day7: "One week of access.",
  day30: "Full month. Stacks if you redeem more time.",
  "rust-day1": "Short session access.",
  "rust-day7": "One week of access.",
  "rust-day30": "Full month. Stacks if you redeem more time.",
  "eft-day1": "Short session access.",
  "eft-day7": "One week of access.",
  "eft-day30": "Full month. Stacks if you redeem more time.",
  "cs2-day1": "Short session access.",
  "cs2-day7": "One week of access.",
  "cs2-day30": "Full month. Stacks if you redeem more time.",
};

export default function PlansPage() {
  const { authed, ready } = useSession();
  const [product, setProduct] = useState<string>("dayz");
  const [catalog, setCatalog] = useState<Catalog | null>(null);
  const [err, setErr] = useState("");
  const [busy, setBusy] = useState<string | null>(null);

  useEffect(() => {
    setCatalog(null);
    void api<Catalog>(`/v1/purchases/catalog?product=${encodeURIComponent(product)}`)
      .then(setCatalog)
      .catch((e) => setErr(friendlyError(e)));
  }, [product]);

  const productLabel = useMemo(
    () => PRODUCTS.find((p) => p.slug === product)?.label ?? product,
    [product]
  );

  async function buy(planId: string) {
    if (!authed) {
      window.location.href = `/login?next=${encodeURIComponent("/plans")}`;
      return;
    }
    setErr("");
    setBusy(planId);
    try {
      track("web.checkout_start", { path: "/plans", meta: { planId, product } });
      const res = await api<{ checkoutUrl: string; provider: string }>("/v1/purchases/checkout", {
        method: "POST",
        body: JSON.stringify({ planId }),
      });
      window.location.href = res.checkoutUrl;
    } catch (e) {
      track("web.checkout_fail", { path: "/plans", meta: { planId, product } });
      setErr(friendlyError(e));
      setBusy(null);
    }
  }

  const plans = catalog?.plans ?? [];

  return (
    <>
      <header className="page-head">
        <h1>Plans</h1>
        <p>
          Choose a product, then pick how long you need.{" "}
          {catalog?.mock
            ? "Local mock checkout is on — payments credit your license immediately."
            : "Pay with crypto or card via NOWPayments."}
        </p>
      </header>

      <div className="product-tabs" role="tablist" aria-label="Product">
        {PRODUCTS.map((p) => (
          <button
            key={p.slug}
            type="button"
            role="tab"
            aria-selected={product === p.slug}
            className={`btn btn-ghost${product === p.slug ? " is-on" : ""}`}
            onClick={() => {
              setErr("");
              setProduct(p.slug);
            }}
          >
            {p.label}
          </button>
        ))}
      </div>
      <p className="muted" style={{ marginTop: -8, marginBottom: 20 }}>
        Showing <strong>{productLabel}</strong> plans.
      </p>

      {err ? <p className="error">{err}</p> : null}
      <div className="plan-list">
        {plans.map((p) => (
          <div className="plan-row" key={p.id}>
            <div>
              <h3>{p.name}</h3>
              <p>{DESCRIPTIONS[p.id] ?? ""}</p>
            </div>
            <div className="plan-meta">
              ${p.priceUsd.toFixed(2)} USD
              <div>{p.id}</div>
            </div>
            <button
              className="btn btn-primary"
              type="button"
              disabled={!ready || busy === p.id}
              onClick={() => buy(p.id)}
            >
              {!ready ? "…" : busy === p.id ? "Starting…" : authed ? "Buy" : "Sign in to buy"}
            </button>
          </div>
        ))}
      </div>
      <p className="muted" style={{ marginTop: 32 }}>
        Have a key already? <Link href="/account">Redeem on Account</Link>
      </p>
    </>
  );
}
