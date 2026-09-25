import { Router } from "express";
import { z } from "zod";
import { audit, cryptoRandomId, db, nowIso } from "../db.js";
import { requireAdmin, requireAuth, type AuthedRequest, clientIp } from "../middleware/auth.js";
import { getHwid, hasProductEntitlement, publicHwid, publicLicense, syncLicenseStatus, syncProductLicenseStatus } from "../license.js";
import { isMaintenance } from "../analytics.js";
import { getCurrentClient, publicClientMeta } from "./releases.js";

export const productsRouter = Router();

export type ProductRow = {
  id: string;
  slug: string;
  name: string;
  description: string;
  status: "online" | "maintenance" | "offline";
  latest_version: string;
  min_launcher: string;
  changelog: string;
  dll_path: string;
  loader_path: string;
  created_at: string;
  updated_at: string;
};

export function ensureProductsSchema(): void {
  db.exec(`
CREATE TABLE IF NOT EXISTS products (
  id              TEXT PRIMARY KEY,
  slug            TEXT NOT NULL UNIQUE COLLATE NOCASE,
  name            TEXT NOT NULL,
  description     TEXT NOT NULL DEFAULT '',
  status          TEXT NOT NULL DEFAULT 'online'
                    CHECK(status IN ('online','maintenance','offline')),
  latest_version  TEXT NOT NULL,
  min_launcher    TEXT NOT NULL DEFAULT '1.0.0',
  changelog       TEXT NOT NULL DEFAULT '',
  dll_path        TEXT NOT NULL DEFAULT '',
  loader_path     TEXT NOT NULL DEFAULT '',
  created_at      TEXT NOT NULL,
  updated_at      TEXT NOT NULL
);
`);

  db.exec(`
CREATE TABLE IF NOT EXISTS product_licenses (
  id            TEXT PRIMARY KEY,
  user_id       TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  product_slug  TEXT NOT NULL REFERENCES products(slug) ON DELETE CASCADE,
  status        TEXT NOT NULL DEFAULT 'inactive'
                  CHECK(status IN ('inactive','active','expired','banned')),
  plan          TEXT NOT NULL DEFAULT 'none',
  expires_at    TEXT,
  created_at    TEXT NOT NULL,
  updated_at    TEXT NOT NULL,
  UNIQUE(user_id, product_slug)
);
CREATE INDEX IF NOT EXISTS idx_product_licenses_user ON product_licenses(user_id);
`);

  const dayz = db.prepare(`SELECT id FROM products WHERE slug = 'dayz'`).get();
  if (!dayz) {
    const t = nowIso();
    db.prepare(
      `INSERT INTO products
       (id, slug, name, description, status, latest_version, min_launcher, changelog, dll_path, loader_path, created_at, updated_at)
       VALUES (?, 'dayz', ?, ?, 'online', ?, '1.0.0', ?, ?, ?, ?, ?)`
    ).run(
      cryptoRandomId(),
      "DayZ",
      "Oak DayZ internal — ESP, combat, misc, freecam.",
      "1.4.0-panel",
      "In-game ImGui menu, auth-ready loader path, soak FPS fixes.",
      "C:\\oak\\dayz\\dayz_internal.dll",
      "C:\\oak\\dayz\\oak_loader.exe",
      t,
      t
    );
  } else {
    // Keep staging paths on the multi-product layout without clobbering custom overrides
    // that already point under C:\oak\dayz\.
    db.prepare(
      `UPDATE products SET dll_path = ?, loader_path = ?, updated_at = ?
       WHERE slug = 'dayz' AND (dll_path = '' OR dll_path LIKE 'C:\\oak\\dayz_internal.dll%')`
    ).run("C:\\oak\\dayz\\dayz_internal.dll", "C:\\oak\\dayz\\oak_loader.exe", nowIso());
  }

  seedProductIfMissing("rust", "Rust", "Oak Rust client — coming soon.", "offline");
  seedProductIfMissing("eft", "Escape from Tarkov", "Oak EFT client — coming soon.", "offline");
  seedProductIfMissing("cs2", "CS2", "Oak CS2 client — Neverlose-parity roadmap.", "online");
}

function seedProductIfMissing(slug: string, name: string, description: string, status: string): void {
  const row = db.prepare(`SELECT id FROM products WHERE slug = ?`).get(slug);
  if (row) return;
  const t = nowIso();
  const dllName = slug === "eft" ? "eft_internal.dll" : `${slug}_internal.dll`;
  db.prepare(
    `INSERT INTO products
     (id, slug, name, description, status, latest_version, min_launcher, changelog, dll_path, loader_path, created_at, updated_at)
     VALUES (?, ?, ?, ?, ?, '0.0.0', '1.0.0', '', ?, ?, ?, ?)`
  ).run(
    cryptoRandomId(),
    slug,
    name,
    description,
    status,
    `C:\\oak\\${slug}\\${dllName}`,
    `C:\\oak\\${slug}\\oak_loader.exe`,
    t,
    t
  );
}

function publicProduct(p: ProductRow) {
  return {
    id: p.id,
    slug: p.slug,
    name: p.name,
    description: p.description,
    status: p.status,
    latestVersion: p.latest_version,
    minLauncher: p.min_launcher,
    changelog: p.changelog,
    updatedAt: p.updated_at,
  };
}

/** Logged-out safe: no inject paths / secrets. */
function publicCatalogProduct(p: ProductRow) {
  return {
    slug: p.slug,
    name: p.name,
    description: p.description,
    status: p.status,
    latestVersion: p.latest_version,
    minLauncher: p.min_launcher,
    changelog: p.changelog,
    updatedAt: p.updated_at,
  };
}

productsRouter.get("/public", (_req, res) => {
  const rows = db
    .prepare(`SELECT * FROM products ORDER BY name ASC`)
    .all() as ProductRow[];
  res.json({ products: rows.map(publicCatalogProduct) });
});

productsRouter.get("/public/:slug", (req, res) => {
  const p = db
    .prepare(`SELECT * FROM products WHERE slug = ?`)
    .get(req.params.slug) as ProductRow | undefined;
  if (!p) {
    res.status(404).json({ error: "not_found" });
    return;
  }
  res.json({ product: publicCatalogProduct(p) });
});

productsRouter.get("/", requireAuth, (_req, res) => {
  const rows = db
    .prepare(`SELECT * FROM products ORDER BY name ASC`)
    .all() as ProductRow[];
  res.json({ products: rows.map(publicProduct) });
});

productsRouter.get("/:slug", requireAuth, (req, res) => {
  const p = db
    .prepare(`SELECT * FROM products WHERE slug = ?`)
    .get(req.params.slug) as ProductRow | undefined;
  if (!p) {
    res.status(404).json({ error: "not_found" });
    return;
  }
  res.json({ product: publicProduct(p) });
});

/** Combined product + account entitlement for the launcher detail page. */
productsRouter.get("/:slug/launch-info", requireAuth, (req: AuthedRequest, res) => {
  const p = db
    .prepare(`SELECT * FROM products WHERE slug = ?`)
    .get(req.params.slug) as ProductRow | undefined;
  if (!p) {
    res.status(404).json({ error: "not_found" });
    return;
  }
  const license = syncLicenseStatus(req.user!.id);
  const productLicense = syncProductLicenseStatus(req.user!.id, p.slug);
  const hwid = getHwid(req.user!.id);
  const licenseOk = hasProductEntitlement(req.user!.id, p.slug);
  const productOk = p.status === "online";
  const hwidOk = !!hwid;
  const maintenance = isMaintenance();
  const client = getCurrentClient(p.slug);
  const clientOk = !!client;
  const canInject =
    licenseOk && productOk && hwidOk && clientOk && !maintenance && !req.user!.banned;

  res.json({
    product: publicProduct(p),
    license: publicLicense(license),
    productLicense: productLicense
      ? {
          status: productLicense.status,
          plan: productLicense.plan,
          expiresAt: productLicense.expires_at,
          productSlug: productLicense.product_slug,
        }
      : null,
    hwid: publicHwid(hwid),
    client: publicClientMeta(client),
    launch: {
      canInject,
      reasons: [
        !licenseOk ? "license_inactive" : null,
        !productOk ? `product_${p.status}` : null,
        req.user!.banned ? "banned" : null,
        !hwidOk ? "hwid_unbound" : null,
        !clientOk ? "no_client_release" : null,
        maintenance ? "maintenance" : null,
      ].filter(Boolean),
      dllPath: p.dll_path,
      loaderPath: p.loader_path,
      clientVersion: client?.version ?? p.latest_version,
      fetchClient: true,
    },
  });
});

productsRouter.patch("/:slug", requireAdmin, (req: AuthedRequest, res) => {
  const parsed = z
    .object({
      name: z.string().min(1).max(64).optional(),
      description: z.string().max(2000).optional(),
      status: z.enum(["online", "maintenance", "offline"]).optional(),
      latestVersion: z.string().min(1).max(64).optional(),
      minLauncher: z.string().min(1).max(32).optional(),
      changelog: z.string().max(8000).optional(),
      dllPath: z.string().max(512).optional(),
      loaderPath: z.string().max(512).optional(),
    })
    .safeParse(req.body);
  if (!parsed.success) {
    res.status(400).json({ error: "invalid_body", details: parsed.error.flatten() });
    return;
  }
  const p = db
    .prepare(`SELECT * FROM products WHERE slug = ?`)
    .get(req.params.slug) as ProductRow | undefined;
  if (!p) {
    res.status(404).json({ error: "not_found" });
    return;
  }
  const d = parsed.data;
  db.prepare(
    `UPDATE products SET
      name = COALESCE(?, name),
      description = COALESCE(?, description),
      status = COALESCE(?, status),
      latest_version = COALESCE(?, latest_version),
      min_launcher = COALESCE(?, min_launcher),
      changelog = COALESCE(?, changelog),
      dll_path = COALESCE(?, dll_path),
      loader_path = COALESCE(?, loader_path),
      updated_at = ?
     WHERE slug = ?`
  ).run(
    d.name ?? null,
    d.description ?? null,
    d.status ?? null,
    d.latestVersion ?? null,
    d.minLauncher ?? null,
    d.changelog ?? null,
    d.dllPath ?? null,
    d.loaderPath ?? null,
    nowIso(),
    req.params.slug
  );
  audit("admin.product_update", {
    actorId: req.user!.id,
    targetId: p.id,
    meta: d,
    ip: clientIp(req),
  });
  const updated = db
    .prepare(`SELECT * FROM products WHERE slug = ?`)
    .get(req.params.slug) as ProductRow;
  res.json({ product: publicProduct(updated) });
});
