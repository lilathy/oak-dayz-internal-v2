"use client";

import { useEffect, useState } from "react";
import { api, friendlyError } from "@/lib/api";

type Product = {
  latestVersion: string;
  description: string;
  changelog?: string;
};

export default function ChangelogPage() {
  const [product, setProduct] = useState<Product | null>(null);
  const [siteNotes, setSiteNotes] = useState("");
  const [err, setErr] = useState("");

  useEffect(() => {
    Promise.all([
      api<{ product: Product }>("/v1/products/public/dayz"),
      api<{ changelogExtra?: string }>("/v1/site/bootstrap"),
    ])
      .then(([prod, boot]) => {
        setProduct(prod.product);
        setSiteNotes(boot.changelogExtra ?? "");
      })
      .catch((e) => setErr(friendlyError(e)));
  }, []);

  return (
    <>
      <header className="page-head">
        <h1>Changelog</h1>
        <p>What changed in the DayZ client — and any site notes from admin.</p>
      </header>

      {err ? <p className="error">{err}</p> : null}
      {!product && !err ? <p className="muted">Loading…</p> : null}

      {product ? (
        <div className="block">
          <h2>Client {product.latestVersion}</h2>
          <p className="muted">{product.description}</p>
          <pre className="notes">{product.changelog || "—"}</pre>
        </div>
      ) : null}

      {siteNotes ? (
        <div className="block">
          <h2>Site notes</h2>
          <pre className="notes">{siteNotes}</pre>
        </div>
      ) : null}
    </>
  );
}
