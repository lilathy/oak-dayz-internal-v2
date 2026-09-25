import crypto from "node:crypto";
import dotenv from "dotenv";
import path from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
dotenv.config({ path: path.join(__dirname, "..", ".env") });

const isProd = (process.env.NODE_ENV ?? "development") === "production";

/** Parse TRUST_PROXY so "false"/"0" disable forwarding (string "false" is truthy). */
function parseTrustProxy(raw: string): boolean | number | string {
  const v = raw.trim();
  if (!v) return false;
  const lower = v.toLowerCase();
  if (lower === "0" || lower === "false" || lower === "no" || lower === "off") return false;
  if (lower === "1" || lower === "true" || lower === "yes" || lower === "on") return true;
  const n = Number(v);
  if (Number.isInteger(n) && n >= 0) return n;
  return v;
}

function req(name: string, fallback?: string): string {
  const v = process.env[name];
  if (v) return v;
  // A generated fallback changes on every boot: tokens, redeem codes and HWID
  // bindings would all silently break. Never allow that outside development.
  if (isProd) throw new Error(`Missing env ${name}`);
  if (!fallback) throw new Error(`Missing env ${name}`);
  console.warn(`[oak-api] ${name} not set — using an ephemeral dev value.`);
  return fallback;
}

const jwtAccessSecret = req("JWT_ACCESS_SECRET", crypto.randomBytes(48).toString("hex"));
const devRuntimeSigningPair = !isProd
  ? crypto.generateKeyPairSync("ec", { namedCurve: "prime256v1" })
  : null;
const runtimePackagePrivateKey =
  process.env.RUNTIME_PACKAGE_PRIVATE_KEY ??
  (devRuntimeSigningPair
    ? devRuntimeSigningPair.privateKey.export({ type: "pkcs8", format: "pem" }).toString()
    : req("RUNTIME_PACKAGE_PRIVATE_KEY")).replace(/\\n/g, "\n");
const runtimePackagePublicKey =
  process.env.RUNTIME_PACKAGE_PUBLIC_KEY ??
  (devRuntimeSigningPair
    ? devRuntimeSigningPair.publicKey.export({ type: "spki", format: "pem" }).toString()
    : req("RUNTIME_PACKAGE_PUBLIC_KEY")).replace(/\\n/g, "\n");
const runtimePrivateKeyObject = crypto.createPrivateKey(runtimePackagePrivateKey);
const runtimePublicKeyObject = crypto.createPublicKey(runtimePackagePublicKey);
if (
  runtimePrivateKeyObject.asymmetricKeyType !== "ec" ||
  runtimePublicKeyObject.asymmetricKeyType !== "ec"
) {
  throw new Error("Runtime package signing keys must be ECDSA P-256 PEM keys.");
}
const runtimePackagePublicDer = runtimePublicKeyObject.export({ type: "spki", format: "der" });

export const config = {
  port: Number(process.env.PORT ?? 8787),
  host: process.env.HOST ?? "127.0.0.1",
  nodeEnv: process.env.NODE_ENV ?? "development",
  isProd,
  jwtAccessSecret,
  jwtRefreshSecret: req("JWT_REFRESH_SECRET", crypto.randomBytes(48).toString("hex")),
  /**
   * Peppers for redeem-code and HWID hashes. Kept separate from the JWT secret
   * so rotating signing keys does not invalidate every issued code / binding.
   * Default to the JWT secret for backwards compatibility with existing rows.
   */
  codePepper: process.env.CODE_PEPPER || jwtAccessSecret,
  hwidPepper: process.env.HWID_PEPPER || jwtAccessSecret,
  /**
   * HMAC key for one-time bootstrap tickets and short-lived product leases.
   * Separate from JWT and HWID keys so rotations stay scoped.
   */
  protectionPepper: req("PROTECTION_PEPPER", crypto.randomBytes(48).toString("hex")),
  /** Ed25519 signing keypair for server-issued runtime packages. */
  runtimePackagePrivateKey,
  runtimePackagePublicKey,
  runtimePackageKeyId: crypto.createHash("sha256").update(runtimePackagePublicDer).digest("hex"),
  /**
   * Express trust-proxy setting. Strings like "false" must not be treated as
   * truthy — that would honour attacker-controlled X-Forwarded-For.
   */
  trustProxy: parseTrustProxy(process.env.TRUST_PROXY ?? ""),
  accessTtlSec: Number(process.env.ACCESS_TTL_SEC ?? 900),
  refreshTtlSec: Number(process.env.REFRESH_TTL_SEC ?? 604800),
  adminEmail: process.env.ADMIN_EMAIL ?? "admin@oak.local",
  adminPassword: req("ADMIN_PASSWORD", "OakAdmin!ChangeMe"),
  adminUsername: process.env.ADMIN_USERNAME ?? "admin",
  corsOrigins: (process.env.CORS_ORIGINS ??
    "http://127.0.0.1:8787,http://localhost:8787,http://127.0.0.1:3000,http://localhost:3000")
    .split(",")
    .map((s) => s.trim())
    .filter(Boolean),
  databasePath: path.resolve(
    process.env.DATABASE_PATH ?? path.join(__dirname, "..", "data", "oak.sqlite")
  ),
  /** Shared secret for POST /v1/purchases/fulfill (internal / admin tooling). */
  fulfillSecret: process.env.FULFILL_SECRET ?? "",
  /**
   * When true (or when crypto keys are unset in development), checkout uses the
   * local mock pay page instead of NOWPayments.
   * Never enabled in production — PAYMENT_MOCK=1 must not mint free licenses live.
   */
  paymentMock:
    !isProd &&
    (process.env.PAYMENT_MOCK === "1" ||
      process.env.PAYMENT_MOCK === "true" ||
      !process.env.NOWPAYMENTS_API_KEY),
  /** NOWPayments API key (dashboard) + IPN secret (Payment Settings). */
  nowpaymentsApiKey: process.env.NOWPAYMENTS_API_KEY ?? "",
  nowpaymentsIpnSecret: process.env.NOWPAYMENTS_IPN_SECRET ?? "",
  nowpaymentsCallbackUrl: process.env.NOWPAYMENTS_CALLBACK_URL ?? "",
  /** Public API origin used in mock checkout redirects and IPN callbacks. */
  publicApiUrl: (process.env.PUBLIC_API_URL ?? `http://127.0.0.1:${process.env.PORT ?? 8787}`).replace(
    /\/$/,
    ""
  ),
  /**
   * Cloudflare Turnstile. When the secret is unset, captcha checks are skipped
   * so local development and the launcher keep working.
   */
  turnstileSecret: process.env.TURNSTILE_SECRET ?? "",
  /** Refresh-token cookie used by the website (the launcher stays on bearer). */
  cookieName: process.env.SESSION_COOKIE_NAME ?? "oak_rt",
  /** Access JWT cookie for the /admin UI (httpOnly — never sessionStorage). */
  adminCookieName: process.env.ADMIN_COOKIE_NAME ?? "oak_admin_at",
  cookieDomain: process.env.COOKIE_DOMAIN || process.env.SESSION_COOKIE_DOMAIN || undefined,
  /** Public site origin for password-reset links (Next.js) */
  publicSiteUrl: (process.env.PUBLIC_SITE_URL ?? "http://127.0.0.1:3000").replace(/\/$/, ""),
  smtpHost: process.env.SMTP_HOST ?? "",
  smtpPort: Number(process.env.SMTP_PORT ?? 587),
  smtpSecure: (process.env.SMTP_SECURE ?? "false") === "true",
  smtpUser: process.env.SMTP_USER ?? "",
  smtpPass: process.env.SMTP_PASS ?? "",
  smtpFrom: process.env.SMTP_FROM ?? "Oak <noreply@oak.local>",
};
