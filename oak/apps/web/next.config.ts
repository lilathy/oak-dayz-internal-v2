import type { NextConfig } from "next";

const API_ORIGIN = (() => {
  try {
    return new URL(process.env.NEXT_PUBLIC_API_URL ?? "http://localhost:8787").origin;
  } catch {
    return "http://localhost:8787";
  }
})();

const TURNSTILE = "https://challenges.cloudflare.com";
const ANALYTICS_ORIGIN = process.env.NEXT_PUBLIC_ANALYTICS_ORIGIN ?? "";
const isProd = process.env.NODE_ENV === "production";

/**
 * Content Security Policy.
 *
 * Pages are statically prerendered, so their script tags cannot carry a
 * per-request nonce; `script-src` is therefore an origin allowlist rather than
 * a nonce policy. `'unsafe-inline'` covers the inline hydration payload Next
 * emits. Everything else is locked down: no framing, no plugins, no arbitrary
 * origins for scripts, connections, styles or fonts.
 */
const csp = [
  `default-src 'self'`,
  `base-uri 'self'`,
  `object-src 'none'`,
  `frame-ancestors 'none'`,
  `form-action 'self'`,
  `img-src 'self' data: blob:`,
  `font-src 'self' https://fonts.gstatic.com`,
  `style-src 'self' 'unsafe-inline' https://fonts.googleapis.com`,
  `script-src 'self' 'unsafe-inline' ${TURNSTILE} ${ANALYTICS_ORIGIN}${
    isProd ? "" : " 'unsafe-eval'"
  }`.trim(),
  `connect-src 'self' ${API_ORIGIN} ${TURNSTILE} ${ANALYTICS_ORIGIN}`.trim(),
  `frame-src ${TURNSTILE}`,
  `manifest-src 'self'`,
  // Only force https once the API is actually served over https, otherwise a
  // local production build could no longer reach an http API.
  isProd && API_ORIGIN.startsWith("https:") ? "upgrade-insecure-requests" : "",
]
  .filter(Boolean)
  .join("; ");

const securityHeaders = [
  { key: "Content-Security-Policy", value: csp },
  { key: "X-Content-Type-Options", value: "nosniff" },
  { key: "X-Frame-Options", value: "DENY" },
  { key: "Referrer-Policy", value: "strict-origin-when-cross-origin" },
  { key: "X-DNS-Prefetch-Control", value: "off" },
  {
    key: "Permissions-Policy",
    value: "camera=(), microphone=(), geolocation=(), payment=(), usb=(), interest-cohort=()",
  },
  { key: "Cross-Origin-Opener-Policy", value: "same-origin" },
  { key: "Strict-Transport-Security", value: "max-age=63072000; includeSubDomains; preload" },
];

const nextConfig: NextConfig = {
  reactStrictMode: true,
  // Do not advertise the framework version to scanners.
  poweredByHeader: false,
  async headers() {
    return [{ source: "/:path*", headers: securityHeaders }];
  },
};

export default nextConfig;
