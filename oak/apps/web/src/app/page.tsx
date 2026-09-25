"use client";

import Link from "next/link";
import { useEffect, useState } from "react";
import { api } from "@/lib/api";

type Product = {
  name: string;
  description: string;
  status: string;
  latestVersion: string;
  minLauncher: string;
  updatedAt?: string;
  changelog?: string;
};

export default function HomePage() {
  const [product, setProduct] = useState<Product | null>(null);
  const [err, setErr] = useState("");

  useEffect(() => {
    api<{ product: Product }>("/v1/products/public/dayz")
      .then((d) => setProduct(d.product))
      .catch((e) => setErr(e.message));
  }, []);

  const stClass =
    product?.status === "online"
      ? "status-online"
      : product?.status === "maintenance"
        ? "status-maintenance"
        : "status-offline";

  return (
    <>
      <header className="hero">
        <h1 className="hero-brand">
          OAK<em> / DayZ</em>
        </h1>
        <p>Internal client for DayZ. Sign in, redeem time, run the launcher.</p>
        <div className="cta-row">
          <Link className="btn btn-primary" href="/plans">
            View plans
          </Link>
          <Link className="btn btn-ghost" href="/download">
            Get launcher
          </Link>
        </div>
      </header>

      <section className="section">
        <p className="section-label">How it works</p>
        <h2>Three steps</h2>
        <p className="lead">Buy time, install the launcher, play.</p>
        <div className="home-path">
          <Link href="/plans">
            <div className="k">01 · Plans</div>
            <p>Pick a DayZ plan and check out. Time stacks when you redeem more.</p>
          </Link>
          <Link href="/download">
            <div className="k">02 · Download</div>
            <p>Grab OakLauncher. It pulls the client for you — no public DLL links.</p>
          </Link>
          <Link href="/account">
            <div className="k">03 · Account</div>
            <p>Redeem codes, check status, reset HWID, and manage sessions.</p>
          </Link>
        </div>
      </section>

      <section className="section">
        <p className="section-label">Product</p>
        <h2>DayZ status</h2>
        <p className="lead">Live from the API — version and service state.</p>
        {err ? (
          <p className="error">{err}</p>
        ) : !product ? (
          <p className="muted">Loading…</p>
        ) : (
          <div className="status-strip">
            <div className="status-cell">
              <div className="k">Status</div>
              <div className={`v ${stClass}`}>{product.status.toUpperCase()}</div>
            </div>
            <div className="status-cell">
              <div className="k">Version</div>
              <div className="v">{product.latestVersion}</div>
            </div>
            <div className="status-cell">
              <div className="k">Min launcher</div>
              <div className="v">{product.minLauncher}</div>
            </div>
            <div className="status-cell">
              <div className="k">Updated</div>
              <div className="v">{(product.updatedAt || "—").slice(0, 10)}</div>
            </div>
          </div>
        )}
        <p className="muted" style={{ marginTop: 20 }}>
          Full notes on the <Link href="/changelog">changelog</Link>.
        </p>
      </section>
    </>
  );
}
