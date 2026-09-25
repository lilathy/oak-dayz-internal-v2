"use client";

import { API_URL } from "@/lib/api";

const ALLOWED = new Set([
  "web.page_view",
  "web.signup_start",
  "web.signup_ok",
  "web.signup_fail",
  "web.login_ok",
  "web.login_fail",
  "web.redeem_ok",
  "web.redeem_fail",
  "web.checkout_start",
  "web.checkout_ok",
  "web.checkout_fail",
  "web.download_click",
  "web.support_ticket_open",
]);

/**
 * Best-effort first-party telemetry. Never throws; never blocks UX.
 * Uses keepalive so navigations still deliver the beacon.
 */
export function track(
  event: string,
  opts: { path?: string; meta?: Record<string, string | number | boolean> } = {}
): void {
  if (!ALLOWED.has(event)) return;
  try {
    const body = JSON.stringify({
      event,
      path: opts.path?.slice(0, 200),
      meta: opts.meta,
    });
    const headers: Record<string, string> = {
      "Content-Type": "application/json",
      "X-Oak-Client": "web",
    };
    // Prefer sendBeacon when available (no auth header — optionalAuth on API).
    if (typeof navigator !== "undefined" && typeof navigator.sendBeacon === "function") {
      const blob = new Blob([body], { type: "application/json" });
      navigator.sendBeacon(`${API_URL}/v1/telemetry/web`, blob);
      return;
    }
    void fetch(`${API_URL}/v1/telemetry/web`, {
      method: "POST",
      headers,
      body,
      credentials: "include",
      keepalive: true,
    }).catch(() => {});
  } catch {
    /* ignore */
  }
}
