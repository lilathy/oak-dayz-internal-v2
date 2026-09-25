"use client";

import Link from "next/link";
import { useEffect, useState } from "react";
import { API_URL, api } from "@/lib/api";
import { track } from "@/lib/track";

export default function DownloadPage() {
  const [meta, setMeta] = useState("Checking…");
  const [hasRelease, setHasRelease] = useState(false);

  useEffect(() => {
    api<{ launcher: { version: string; sizeBytes: number; uploadedAt?: string } | null }>(
      "/v1/downloads/launcher/meta"
    )
      .then((data) => {
        if (data.launcher) {
          setHasRelease(true);
          setMeta(
            `Version ${data.launcher.version} · ${(data.launcher.sizeBytes / 1024 / 1024).toFixed(2)} MB · uploaded ${(data.launcher.uploadedAt || "").slice(0, 19)}`
          );
        } else {
          setMeta("No launcher uploaded yet — use the local build or wait for an admin upload.");
        }
      })
      .catch((e) => setMeta(e.message));
  }, []);

  return (
    <>
      <header className="page-head">
        <h1>Download</h1>
        <p>Get OakLauncher. It signs in, pulls the DayZ client, and injects — no public DLL links.</p>
      </header>
      <ol className="steps">
        <li>
          <strong>Create an account</strong> — <Link href="/register">Register</Link> or{" "}
          <Link href="/login">login</Link>.
        </li>
        <li>
          <strong>Get time</strong> — redeem a code on <Link href="/account">Account</Link>.
        </li>
        <li>
          <strong>Download the launcher</strong> below, sign in, pick DayZ, Inject.
        </li>
      </ol>
      <div style={{ marginTop: 28, paddingTop: 20, borderTop: "1px solid var(--line)" }}>
        <p className="section-label">Launcher release</p>
        <p className="muted">{meta}</p>
        <p style={{ marginTop: 16 }}>
          <a
            className="btn btn-primary"
            href={`${API_URL}/v1/downloads/launcher`}
            style={{ opacity: hasRelease ? 1 : 0.45 }}
            onClick={(e) => {
              if (!hasRelease) {
                e.preventDefault();
                alert("No launcher release on the API yet.");
                return;
              }
              track("web.download_click", { path: "/download" });
            }}
          >
            Download launcher
          </a>
        </p>
      </div>
    </>
  );
}
