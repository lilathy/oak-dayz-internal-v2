import express from "express";
import cookieParser from "cookie-parser";
import cors from "cors";
import helmet from "helmet";
import rateLimit from "express-rate-limit";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { config } from "./config.js";
import "./db.js";
import { ensureAnalyticsSchema, isMaintenance, touchLastSeen } from "./analytics.js";
import { ensureReleaseDirs } from "./paths.js";
import { authRouter } from "./routes/auth.js";
import { hwidRouter } from "./routes/hwid.js";
import { adminRouter } from "./routes/admin.js";
import { productsRouter, ensureProductsSchema } from "./routes/products.js";
import { licensesRouter } from "./routes/licenses.js";
import { purchasesRouter } from "./routes/purchases.js";
import { ensurePlansSchema } from "./plans.js";
import { ensurePurchasesSchema } from "./purchases.js";
import {
  adminReleasesRouter,
  clientDownloadRouter,
  registerLauncherDownload,
} from "./routes/releases.js";
import { adminSiteRouter, siteRouter } from "./routes/site.js";
import { adminForumRouter, forumRouter } from "./routes/forum.js";
import { adminTicketsRouter, ticketsRouter } from "./routes/tickets.js";
import { ensureSupportSchema } from "./support.js";
import { ensureAccountSchema } from "./accounts.js";
import { ensureForumSchema } from "./forum.js";
import {
  ensureAvatarsDir,
  ensureForumProfileSchema,
  ensureProfileRateSchema,
} from "./forumProfile.js";
import { ensureDevelopmentRuntimePackages, ensureProtectionSchema } from "./protection.js";
import { adminProtectionRouter, protectionRouter } from "./routes/protection.js";
import { adminAlertsRouter, ensureSecurityAlertsSchema, telemetryRouter } from "./routes/telemetry.js";
import { requireAuth, type AuthedRequest } from "./middleware/auth.js";
import { getHwid, publicHwid, publicLicense, publicUser, syncLicenseStatus } from "./license.js";

ensurePlansSchema();
ensurePurchasesSchema();
ensureProductsSchema();
ensureAnalyticsSchema();
ensureSecurityAlertsSchema();
ensureReleaseDirs();
ensureSupportSchema();
ensureAccountSchema();
ensureForumSchema();
ensureForumProfileSchema();
ensureProfileRateSchema();
ensureAvatarsDir();
ensureProtectionSchema();
ensureDevelopmentRuntimePackages();

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const publicDir = path.join(__dirname, "..", "public");

const app = express();

app.disable("x-powered-by");
app.set("trust proxy", config.trustProxy);

app.use(
  helmet({
    // Global API responses: no HTML UI except /admin (CSP applied there).
    contentSecurityPolicy: false,
    crossOriginEmbedderPolicy: false,
    referrerPolicy: { policy: "no-referrer" },
    frameguard: { action: "deny" },
  })
);

app.use(
  cors({
    origin(origin, cb) {
      if (!origin) return cb(null, true);
      if (config.corsOrigins.includes(origin)) return cb(null, true);
      return cb(new Error("cors_blocked"));
    },
    credentials: true,
  })
);

app.use(cookieParser());
app.use(express.json({ limit: "64kb" }));

app.use(
  rateLimit({
    windowMs: 60 * 1000,
    max: 180,
    standardHeaders: true,
    legacyHeaders: false,
    message: { error: "rate_limited" },
  })
);

app.get("/health", (_req, res) => {
  // Production health checks must not advertise bind address or NODE_ENV.
  if (config.isProd) {
    res.json({ ok: true, maintenance: isMaintenance() });
    return;
  }
  res.json({
    ok: true,
    service: "oak-api",
    env: config.nodeEnv,
    bind: `${config.host}:${config.port}`,
    maintenance: isMaintenance(),
  });
});

app.use("/v1/auth", authRouter);
app.use("/v1/hwid", hwidRouter);
app.use("/v1/admin", adminRouter);
app.use("/v1/admin", adminReleasesRouter);
app.use("/v1/admin", adminSiteRouter);
app.use("/v1/admin", adminAlertsRouter);
app.use("/v1/telemetry", telemetryRouter);
app.use("/v1/products", clientDownloadRouter);
app.use("/v1/products", productsRouter);
app.use("/v1/products", protectionRouter);
app.use("/v1/licenses", licensesRouter);
app.use("/v1/purchases", purchasesRouter);
app.use("/v1/site", siteRouter);
app.use("/v1/forum", forumRouter);
app.use("/v1/admin", adminForumRouter);
app.use("/v1/tickets", ticketsRouter);
app.use("/v1/admin", adminTicketsRouter);
app.use("/v1/admin", adminProtectionRouter);
registerLauncherDownload(app);

// Admin UI: tight CSP (inline script/style required by the single-file panel).
app.use(
  "/admin",
  helmet({
    contentSecurityPolicy: {
      useDefaults: false,
      directives: {
        defaultSrc: ["'self'"],
        baseUri: ["'self'"],
        formAction: ["'self'"],
        frameAncestors: ["'none'"],
        imgSrc: ["'self'", "data:"],
        styleSrc: ["'self'", "'unsafe-inline'"],
        scriptSrc: ["'self'", "'unsafe-inline'"],
        connectSrc: ["'self'"],
        objectSrc: ["'none'"],
      },
    },
    referrerPolicy: { policy: "no-referrer" },
    frameguard: { action: "deny" },
  }),
  express.static(path.join(publicDir, "admin"))
);

// Customer site is Next.js (oak/web) on :3000 — API only serves /admin static here.

app.get("/v1/session/validate", requireAuth, (req: AuthedRequest, res) => {
  const u = req.user!;
  touchLastSeen(u.id);
  if (isMaintenance()) {
    res.status(503).json({
      ok: false,
      reason: "maintenance",
      user: publicUser(u),
      license: publicLicense(syncLicenseStatus(u.id)),
      hwid: publicHwid(getHwid(u.id)),
    });
    return;
  }
  const license = syncLicenseStatus(u.id);
  const active = license?.status === "active";
  res.status(active ? 200 : 403).json({
    ok: active,
    user: publicUser(u),
    license: publicLicense(license),
    hwid: publicHwid(getHwid(u.id)),
    reason: active ? undefined : "license_inactive",
  });
});

app.use((err: unknown, _req: express.Request, res: express.Response, _next: express.NextFunction) => {
  const msg = err instanceof Error ? err.message : "error";
  if (msg === "cors_blocked") {
    res.status(403).json({ error: "cors_blocked" });
    return;
  }
  if (msg === "File too large") {
    res.status(413).json({ error: "file_too_large" });
    return;
  }
  console.error(err);
  res.status(500).json({ error: "internal" });
});

app.listen(config.port, config.host, () => {
  console.log(`oak-api listening on http://${config.host}:${config.port}`);
  console.log(`admin:  http://${config.host}:${config.port}/admin/`);
  console.log(`health: http://${config.host}:${config.port}/health`);
  console.log(`site:   Next.js oak/web → http://127.0.0.1:3000`);
});
