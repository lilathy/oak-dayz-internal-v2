import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import { Router } from "express";
import multer from "multer";
import { z } from "zod";
import { logDownload, touchLastSeen, isMaintenance, trackEvent } from "../analytics.js";
import { audit, cryptoRandomId, db, nowIso } from "../db.js";
import { getHwid, syncLicenseStatus } from "../license.js";
import { clientIp, requireAdmin, requireAuth, type AuthedRequest } from "../middleware/auth.js";
import { clientsDir, ensureReleaseDirs, launchersDir } from "../paths.js";

ensureReleaseDirs();

export type ClientReleaseRow = {
  id: string;
  product_slug: string;
  version: string;
  filename: string;
  storage_path: string;
  sha256: string;
  size_bytes: number;
  uploaded_by: string | null;
  created_at: string;
  is_current: number;
};

export type LauncherReleaseRow = {
  id: string;
  version: string;
  filename: string;
  storage_path: string;
  sha256: string;
  size_bytes: number;
  uploaded_by: string | null;
  created_at: string;
  is_current: number;
};

const upload = multer({
  storage: multer.memoryStorage(),
  limits: { fileSize: 80 * 1024 * 1024 },
});

function sha256buf(buf: Buffer): string {
  return crypto.createHash("sha256").update(buf).digest("hex");
}

export function getCurrentClient(slug: string): ClientReleaseRow | undefined {
  return db
    .prepare(
      `SELECT * FROM client_releases WHERE product_slug = ? AND is_current = 1 ORDER BY created_at DESC LIMIT 1`
    )
    .get(slug) as ClientReleaseRow | undefined;
}

export function getCurrentLauncher(): LauncherReleaseRow | undefined {
  return db
    .prepare(`SELECT * FROM launcher_releases WHERE is_current = 1 ORDER BY created_at DESC LIMIT 1`)
    .get() as LauncherReleaseRow | undefined;
}

export function publicClientMeta(row: ClientReleaseRow | undefined) {
  if (!row) return null;
  return {
    id: row.id,
    version: row.version,
    sha256: row.sha256,
    sizeBytes: row.size_bytes,
    uploadedAt: row.created_at,
    filename: row.filename,
  };
}

/** Admin upload routes mounted under /v1/admin */
export const adminReleasesRouter = Router();
adminReleasesRouter.use(requireAdmin);

adminReleasesRouter.get("/releases/client/:slug", (req, res) => {
  const rows = db
    .prepare(
      `SELECT id, product_slug, version, filename, sha256, size_bytes, uploaded_by, created_at, is_current
       FROM client_releases WHERE product_slug = ? ORDER BY created_at DESC LIMIT 50`
    )
    .all(req.params.slug);
  res.json({ releases: rows, current: publicClientMeta(getCurrentClient(req.params.slug)) });
});

adminReleasesRouter.post(
  "/releases/client/:slug",
  upload.single("file"),
  (req: AuthedRequest, res) => {
    const parsed = z
      .object({ version: z.string().min(1).max(64) })
      .safeParse(req.body);
    if (!parsed.success || !req.file) {
      res.status(400).json({ error: "invalid_body", hint: "multipart: file + version" });
      return;
    }
    const slug = req.params.slug;
    const product = db.prepare(`SELECT id FROM products WHERE slug = ?`).get(slug);
    if (!product) {
      res.status(404).json({ error: "product_not_found" });
      return;
    }
    const ext = path.extname(req.file.originalname || "").toLowerCase() || ".dll";
    if (![".dll", ".bin"].includes(ext)) {
      res.status(400).json({ error: "invalid_file_type", hint: "upload .dll" });
      return;
    }
    const id = cryptoRandomId();
    const filename = `${slug}-${parsed.data.version.replace(/[^a-zA-Z0-9._-]/g, "_")}-${id.slice(0, 8)}${ext}`;
    const storagePath = path.join(clientsDir, filename);
    fs.writeFileSync(storagePath, req.file.buffer);
    const hash = sha256buf(req.file.buffer);
    const t = nowIso();
    db.prepare(`UPDATE client_releases SET is_current = 0 WHERE product_slug = ?`).run(slug);
    db.prepare(
      `INSERT INTO client_releases
       (id, product_slug, version, filename, storage_path, sha256, size_bytes, uploaded_by, created_at, is_current)
       VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, 1)`
    ).run(
      id,
      slug,
      parsed.data.version,
      filename,
      storagePath,
      hash,
      req.file.size,
      req.user!.id,
      t
    );
    // Keep product.latest_version in sync
    db.prepare(`UPDATE products SET latest_version = ?, updated_at = ? WHERE slug = ?`).run(
      parsed.data.version,
      t,
      slug
    );
    audit("admin.client_upload", {
      actorId: req.user!.id,
      meta: { slug, version: parsed.data.version, sha256: hash, size: req.file.size },
      ip: clientIp(req),
    });
    res.status(201).json({
      release: publicClientMeta(getCurrentClient(slug)),
    });
  }
);

adminReleasesRouter.get("/releases/launcher", (_req, res) => {
  const rows = db
    .prepare(
      `SELECT id, version, filename, sha256, size_bytes, uploaded_by, created_at, is_current
       FROM launcher_releases ORDER BY created_at DESC LIMIT 20`
    )
    .all();
  const cur = getCurrentLauncher();
  res.json({
    releases: rows,
    current: cur
      ? {
          version: cur.version,
          sha256: cur.sha256,
          sizeBytes: cur.size_bytes,
          uploadedAt: cur.created_at,
          filename: cur.filename,
        }
      : null,
  });
});

adminReleasesRouter.post("/releases/launcher", upload.single("file"), (req: AuthedRequest, res) => {
  const parsed = z.object({ version: z.string().min(1).max(64) }).safeParse(req.body);
  if (!parsed.success || !req.file) {
    res.status(400).json({ error: "invalid_body", hint: "multipart: file + version" });
    return;
  }
  const ext = path.extname(req.file.originalname || "").toLowerCase() || ".exe";
  if (![".exe", ".zip"].includes(ext)) {
    res.status(400).json({ error: "invalid_file_type", hint: "upload .exe or .zip" });
    return;
  }
  const id = cryptoRandomId();
  const filename = `OakLauncher-${parsed.data.version.replace(/[^a-zA-Z0-9._-]/g, "_")}-${id.slice(0, 8)}${ext}`;
  const storagePath = path.join(launchersDir, filename);
  fs.writeFileSync(storagePath, req.file.buffer);
  const hash = sha256buf(req.file.buffer);
  const t = nowIso();
  db.prepare(`UPDATE launcher_releases SET is_current = 0`).run();
  db.prepare(
    `INSERT INTO launcher_releases
     (id, version, filename, storage_path, sha256, size_bytes, uploaded_by, created_at, is_current)
     VALUES (?, ?, ?, ?, ?, ?, ?, ?, 1)`
  ).run(id, parsed.data.version, filename, storagePath, hash, req.file.size, req.user!.id, t);
  audit("admin.launcher_upload", {
    actorId: req.user!.id,
    meta: { version: parsed.data.version, sha256: hash, size: req.file.size },
    ip: clientIp(req),
  });
  res.status(201).json({
    release: {
      version: parsed.data.version,
      sha256: hash,
      sizeBytes: req.file.size,
      uploadedAt: t,
      filename,
    },
  });
});

/**
 * Launcher-only client download — NOT for browsers/customers.
 * Requires auth + active license + HWID + product online + not maintenance.
 */
export const clientDownloadRouter = Router();

clientDownloadRouter.get("/:slug/client/meta", requireAuth, (req: AuthedRequest, res) => {
  touchLastSeen(req.user!.id);
  const cur = getCurrentClient(req.params.slug);
  if (!cur) {
    res.status(404).json({ error: "no_release" });
    return;
  }
  res.json({ client: publicClientMeta(cur) });
});

clientDownloadRouter.get("/:slug/client", requireAuth, (req: AuthedRequest, res) => {
  const u = req.user!;
  touchLastSeen(u.id);
  if (u.banned) {
    res.status(403).json({ error: "banned" });
    return;
  }
  if (isMaintenance()) {
    res.status(503).json({ error: "maintenance" });
    return;
  }
  const license = syncLicenseStatus(u.id);
  if (license?.status !== "active") {
    res.status(403).json({ error: "license_inactive" });
    return;
  }
  if (!getHwid(u.id)) {
    res.status(403).json({ error: "hwid_unbound" });
    return;
  }
  const product = db
    .prepare(`SELECT status FROM products WHERE slug = ?`)
    .get(req.params.slug) as { status: string } | undefined;
  if (!product) {
    res.status(404).json({ error: "not_found" });
    return;
  }
  if (product.status !== "online") {
    res.status(503).json({ error: `product_${product.status}` });
    return;
  }
  const cur = getCurrentClient(req.params.slug);
  if (!cur || !fs.existsSync(cur.storage_path)) {
    res.status(404).json({ error: "no_release" });
    return;
  }
  const distributionId = cryptoRandomId();
  db.prepare(
    `INSERT INTO client_distributions (id, user_id, product_slug, client_release_id, created_at, ip)
     VALUES (?, ?, ?, ?, ?, ?)`
  ).run(distributionId, u.id, req.params.slug, cur.id, nowIso(), clientIp(req));
  logDownload({
    kind: "client",
    userId: u.id,
    productSlug: req.params.slug,
    version: cur.version,
    ip: clientIp(req),
  });
  trackEvent("client_download", {
    userId: u.id,
    meta: { slug: req.params.slug, version: cur.version, distributionId },
    ip: clientIp(req),
  });
  res.setHeader("Content-Type", "application/octet-stream");
  res.setHeader("Content-Disposition", `attachment; filename="${cur.filename}"`);
  res.setHeader("X-Oak-Client-Version", cur.version);
  res.setHeader("X-Oak-Client-Sha256", cur.sha256);
  res.setHeader("X-Oak-Client-Release-Id", cur.id);
  res.setHeader("X-Oak-Distribution-Id", distributionId);
  fs.createReadStream(cur.storage_path).pipe(res);
});

/** Public launcher download for the website (the shared loader). */
export function registerLauncherDownload(app: import("express").Express): void {
  app.get("/v1/downloads/launcher", (req, res) => {
    const cur = getCurrentLauncher();
    if (!cur || !fs.existsSync(cur.storage_path)) {
      res.status(404).json({ error: "no_launcher_release" });
      return;
    }
    logDownload({
      kind: "launcher",
      userId: null,
      version: cur.version,
      ip: clientIp(req as AuthedRequest),
    });
    trackEvent("launcher_download", { path: "/v1/downloads/launcher", ip: clientIp(req as AuthedRequest) });
    res.setHeader("Content-Type", "application/octet-stream");
    res.setHeader("Content-Disposition", `attachment; filename="${cur.filename}"`);
    res.setHeader("X-Oak-Launcher-Version", cur.version);
    fs.createReadStream(cur.storage_path).pipe(res);
  });

  app.get("/v1/downloads/launcher/meta", (_req, res) => {
    const cur = getCurrentLauncher();
    if (!cur) {
      res.json({ launcher: null });
      return;
    }
    res.json({
      launcher: {
        version: cur.version,
        sha256: cur.sha256,
        sizeBytes: cur.size_bytes,
        uploadedAt: cur.created_at,
        filename: cur.filename,
      },
    });
  });
}
