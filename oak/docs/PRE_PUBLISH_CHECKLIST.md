# Pre-publish / go-live checklist

**Single ops checklist for everything still required before a public website + paid product.**  
Agents: also follow `oak/AGENTS.md` and `.cursor/rules/oak-publish.mdc`. **Never skip the ship gate for client uploads.**

Product-feature work (ESP, aim, freecam, etc.) lives in [MASTER_CHECKLIST.md](./MASTER_CHECKLIST.md). This file is **infra, business, security, packaging, and go-live smoke**.

Last updated: 2026-08-03

---

## How to use

| Track | Meaning |
|-------|---------|
| **A — Site live** | Domain, Cloudflare, website + API on HTTPS, auth/email/captcha, admin locked down |
| **B — Take money** | NOWPayments live, plans priced, fulfill → license works |
| **C — Sell DayZ** | Launcher + DLL + driver package, pins baked, lease path works without Debug unlock |
| **D — Stay alive** | Backups, alerts, rollback, support, legal |

You can soft-open **A** (marketing / register) before **C**, but do **not** advertise “buy & inject” until **B + C** pass.

---

## 0. Stop-ship (any true = do not publish)

- [ ] `oak_dev_unlock` / `localClientDll` still used on the build you plan to upload
- [ ] PDB / `.map` / `offsets_generated.hpp` / offset dumper next to customer artifacts
- [ ] Expecting `PAYMENT_MOCK` to mint licenses in production (it cannot and must not)
- [ ] Absolute developer paths inside shipped `oak.launcher.json`
- [ ] Ship gate fails (`ok: false`)
- [ ] Default admin password still `OakAdmin!ChangeMe` (or anything weak/public)
- [ ] API bound publicly without TLS / without Cloudflare or reverse-proxy TLS
- [ ] Production secrets still the `.env.example` placeholders
- [ ] Turnstile secret missing in production (register/login fail-closed or bots open)
- [ ] Customer package still contains OakPanel, PS1 sprawl, AI guides, or private scripts

---

## 1. Business / legal / brand

- [ ] Final product / brand name locked (site title, emails, Discord, receipts)
- [ ] Business entity / DBA decided (sole prop vs LLC) for payments KYB and ToS
- [ ] Support contact decided (email and/or Discord) — same string everywhere
- [ ] Terms of Service page written and linked in footer + checkout
- [ ] Privacy Policy page written (accounts, HWID hash, payments via NOWPayments, logs/retention)
- [ ] Refund / chargeback / ban policy written (crypto + card/fiat)
- [ ] Acceptable-use / cheat-ToS language reviewed (honest about risks; no “undetectable” claims)
- [ ] Age / jurisdiction restrictions decided (if any) and stated
- [ ] DMCA / abuse contact if you host UGC (forum)
- [ ] Trademark / domain brand conflict quick check

---

## 2. Domain + Cloudflare + DNS

- [ ] Buy primary domain (registrar of choice)
- [ ] Cloudflare account created; domain added; nameservers switched at registrar
- [ ] SSL/TLS mode: **Full (strict)** once origin has a real cert (or Cloudflare Origin Cert)
- [ ] Always Use HTTPS **On**
- [ ] Minimum TLS 1.2+
- [ ] HSTS enabled (after confirming HTTPS works end-to-end)
- [ ] DNS records planned:

| Host | Type | Points to | Notes |
|------|------|-----------|-------|
| `@` / `www` | A/AAAA or CNAME | Website origin (Vercel/VPS/etc.) | Proxied (orange cloud) |
| `api` | A/AAAA or CNAME | API origin | Proxied; TLS to origin |
| `mail` / MX | MX | Email provider | Usually DNS-only |
| SPF / DKIM / DMARC | TXT | Email provider | Required for deliverability |

- [ ] Decide hostnames and lock them (example):
  - Site: `https://oak.example`
  - API: `https://api.oak.example`
  - Admin: `https://api.oak.example/admin/` (not a separate public marketing URL)
- [ ] Cloudflare **WAF** basics: managed ruleset on; bot fight / Super Bot Fight as appropriate
- [ ] Rate limiting / under-attack mode plan documented (who can flip it)
- [ ] Turnstile widget created in Cloudflare → site key (web) + secret (API)
- [ ] Optional: Cloudflare Access or IP allowlist for `/admin/` (extra layer; still rotate admin password)
- [ ] Optional: Page Rules / Cache Rules — **do not cache** API or authenticated HTML
- [ ] Optional: redirect apex ↔ www (pick one canonical)

---

## 3. Hosting / deploy targets

### Website (`apps/web`)

- [ ] Host chosen (Vercel / Cloudflare Pages / Node on VPS — pick one)
- [ ] Production build succeeds (`next build`)
- [ ] Env set: `NEXT_PUBLIC_API_URL=https://api.<domain>`
- [ ] Env set: Turnstile **site** key (`NEXT_PUBLIC_TURNSTILE_*` or whatever the web app expects)
- [ ] No secrets in any `NEXT_PUBLIC_*` var
- [ ] Custom domain attached; HTTPS green
- [ ] Preview/staging URL separated from production (optional but recommended)

### API (`apps/api`)

- [ ] Host chosen (VPS recommended for SQLite simplicity, or container + volume)
- [ ] Node LTS installed; process manager (systemd / pm2 / Docker restart policy)
- [ ] `NODE_ENV=production`
- [ ] Listen on loopback or private interface; **Cloudflare / nginx / Caddy** terminates TLS in front
- [ ] `TRUST_PROXY` set only if behind that real proxy (e.g. `1` or `loopback`)
- [ ] `HOST` / reverse-proxy config correct; `/health` returns minimal JSON in prod
- [ ] Firewall: only 80/443 (and SSH locked down) exposed publicly
- [ ] Automatic restart on reboot
- [ ] Log rotation configured
- [ ] Time sync (NTP) confirmed — JWT/lease expiry depends on it

### Data

- [ ] Production `DATABASE_PATH` on persistent disk (not ephemeral container FS)
- [ ] Scheduled SQLite backup (file copy + off-box) — daily minimum
- [ ] Backup restore tested once
- [ ] Disk space alert / monitoring

### Email (SMTP)

- [ ] Transactional mail provider (Resend / Postmark / SES / SMTP from Google Workspace / etc.)
- [ ] Domain verified; SPF + DKIM + DMARC pass
- [ ] `SMTP_*` + `SMTP_FROM` set on API
- [ ] `PUBLIC_SITE_URL` / `PUBLIC_API_URL` match real HTTPS URLs
- [ ] Smoke: register verify email, password reset, purchase receipt, HWID-reset confirm

---

## 4. Vendor accounts & money

### NOWPayments (required to take money)

- [ ] Account at [nowpayments.io](https://nowpayments.io)
- [ ] API key generated
- [ ] IPN secret generated
- [ ] KYC/KYB completed for withdrawals
- [ ] Fiat / card on-ramp enabled if you want card (dashboard **Fiat operations**)
- [ ] Desired coins / fiat currencies enabled
- [ ] `.env`: `NOWPAYMENTS_API_KEY`, `NOWPAYMENTS_IPN_SECRET`
- [ ] `PUBLIC_API_URL` + `PUBLIC_SITE_URL` production HTTPS
- [ ] IPN callback reaches: `https://api.<domain>/v1/purchases/webhooks/nowpayments`
- [ ] Test invoice (sandbox or small live) → IPN → fulfill → buyer-bound code
- [ ] `FULFILL_SECRET` long random (internal tooling only — not a substitute for IPN)
- [ ] Plans/prices in admin match what the site shows (`dayz` + any stub products you hide)

### Cloudflare Turnstile

- [ ] Widget for production domain(s)
- [ ] Site key on website
- [ ] `TURNSTILE_SECRET` on API
- [ ] Confirm register/login blocked when captcha missing/invalid in production

### Discord (ops)

- [ ] Private staff / alerts server or channel
- [ ] Webhook URL is `discord.com` only (API allowlist)
- [ ] Alert categories you care about enabled (refresh-reuse, HWID anomalies, etc.)
- [ ] Public community / support server optional — invite linked from site if used

### Optional accounts

- [ ] Error monitoring (Sentry or similar) for web/API — optional
- [ ] Uptime monitor on `/health` + homepage
- [ ] GitHub private repo access locked to you / staff only

---

## 5. Production secrets & env (API)

Generate fresh values; store offline (password manager). **Never commit `.env`.**

- [ ] `JWT_ACCESS_SECRET` (long random)
- [ ] `JWT_REFRESH_SECRET` (long random)
- [ ] `CODE_PEPPER` — set once; never rotate casually (invalidates codes)
- [ ] `HWID_PEPPER` — set once; never rotate casually (invalidates HWID binds)
- [ ] `PROTECTION_PEPPER` (bootstrap/lease HMAC)
- [ ] `RUNTIME_PACKAGE_PRIVATE_KEY` / `RUNTIME_PACKAGE_PUBLIC_KEY` (ECDSA P-256 PEM) — private key **offline backup**
- [ ] `FULFILL_SECRET`
- [ ] `TURNSTILE_SECRET`
- [ ] `NOWPAYMENTS_*`
- [ ] `ADMIN_EMAIL` / `ADMIN_USERNAME` / strong `ADMIN_PASSWORD` — then rotate again after first login
- [ ] `CORS_ORIGINS` = real site origins only (https apex + www if both)
- [ ] `COOKIE_DOMAIN` / `SESSION_COOKIE_DOMAIN` if site and API are sibling subdomains (e.g. `.oak.example`)
- [ ] `PAYMENT_MOCK` unset / ignored in production
- [ ] Confirm no `.env` in git (`git status` / `.gitignore`)

Website:

- [ ] `NEXT_PUBLIC_API_URL` → production API
- [ ] Turnstile site key matches API secret pair
- [ ] No secrets in client-exposed env

---

## 6. Website content & UX (before public traffic)

- [ ] Home / brand / hero copy final (no placeholder Lorem)
- [ ] `/plans` shows correct products/prices; hide Rust/EFT if not selling yet
- [ ] `/download` serves current launcher only after upload
- [ ] Login / register / verify-email / forgot-password flows work on real domain
- [ ] Dashboard: licenses, HWID status, tickets entry points clear
- [ ] Forum enabled or deliberately disabled/hidden
- [ ] Maintenance messaging path tested (admin maintenance blocks inject)
- [ ] Favicon / Open Graph / basic SEO title+description
- [ ] 404 / error pages not leaking stack traces
- [ ] Mobile layout smoke on phone-width
- [ ] Footer: ToS, Privacy, Support
- [ ] Remove or gate any “dev / mock pay” UI in production builds

---

## 7. Admin / ops console

- [ ] Admin reachable only over HTTPS
- [ ] Default password gone; unique staff accounts if multi-admin
- [ ] Confirm admin auth is **httpOnly cookie only** (no `oak_admin_token` in sessionStorage)
- [ ] CSP headers present on `/admin/`
- [ ] Can upload DayZ client (`product` slug `dayz`)
- [ ] Can upload launcher
- [ ] Can create/edit plans and redeem codes
- [ ] Can ban / HWID reset / revoke sessions
- [ ] Can mark release current / roll back
- [ ] Ticket reply path works
- [ ] Security alerts visible

---

## 8. Launcher + loader + driver (customer package)

### Build

- [ ] Release `OakLauncher.exe` from `oak/apps/launcher` (`DebugType=none` — no PDB in ship folder)
- [ ] Release `dayz_internal.dll` from `oak/clients/dayz` (CMake POST_BUILD mutate)
- [ ] **Re-run ship gate before every upload:**

```powershell
cd <repo>\oak
node .\packages\protect\tools\ship_security_gate.mjs --dll .\clients\dayz\build\Release\dayz_internal.dll --mutate
```

- [ ] Gate JSON `"ok": true`; record `watermark` + `sha256`
- [ ] `oak_loader.exe` + `oak.sys` built and staged; hashes recorded
- [ ] Customer package = **only** what they need (G0): launcher + DLL path via API + loader/sys as designed — **no** OakPanel, no AI guides, no dumper, no PS1 sprawl

### Config pins (public builds)

- [ ] Shipped `oak.launcher.json` has production `https://` API URL
- [ ] `expectedLoaderSha256` / `expectedSysSha256` filled
- [ ] No `localClientDll` key
- [ ] Bake `OAK_RUNTIME_SPKI_PIN` into Release DLL when production runtime keys are stable:

```powershell
# SPKI pin = API runtimePackageKeyId hex
cmake -S . -B build -DOAK_RUNTIME_SPKI_PIN=<hex>
cmake --build build --config Release --target DayZInternal
node .\packages\protect\tools\ship_security_gate.mjs --dll .\build\Release\dayz_internal.dll --mutate
```

### Upload / distribute

- [ ] Upload DLL via Admin → client upload (slug `dayz`) — not Discord/file share
- [ ] Upload launcher via Admin → launcher upload
- [ ] Website `/download` serves the new launcher
- [ ] Download smoke from a **clean** launcher cache / second Windows user

### Kernel path ops note

- [ ] Vulnerable driver blocklist policy documented for customers/ops (kdmapper / iqvw64e) — see DayZ `AI_LAUNCH_GUIDE` (private)
- [ ] One clean-machine BE smoke: map → inject → overlay (**K**) → RESULT line

---

## 9. Security bake & residual ship items

From [security-hardening-2026-08-03.md](./security-hardening-2026-08-03.md):

- [ ] **S1** Production PEMs + Turnstile + NOWPayments + long `FULFILL_SECRET`
- [ ] **S2** Bake `OAK_RUNTIME_SPKI_PIN`
- [ ] **S3** Loader/sys SHA pins in launcher config
- [ ] **S4** Ship gate + mutate every Release upload
- [ ] **S5** Signature/lease verify works **without** Debug unlock (`prefetch_verify_failed` gone)
- [ ] **S6** API behind TLS reverse proxy; `TRUST_PROXY` only when appropriate
- [ ] **S7** Never distribute Debug launchers or `localClientDll` configs
- [ ] Discord webhook host allowlist respected
- [ ] Read [security-audit-2026-08-03.md](./security-audit-2026-08-03.md) residuals once more

Anti-piracy / protect (must work for paid open — see [anti-piracy-plan.md](./anti-piracy-plan.md)):

- [ ] Bootstrap ticket → lease → runtime package path green in production
- [ ] Encrypted `.oakc` cache path used by Release launcher
- [ ] Ban / HWID reset revokes leases within grace window
- [ ] Release revoke / replace without loader change tested
- [ ] Watermark / distribution attribution recorded per upload

---

## 10. Functional smoke (minimum before “open”)

### Website + API

- [ ] Register / verify email / login / refresh
- [ ] Logout revokes session as designed
- [ ] Forgot password email works
- [ ] Checkout (NOWPayments) creates buyer-bound code
- [ ] Redeem on buying account only; other account rejected
- [ ] Ticket create (user) + staff reply (admin)
- [ ] Forum post + report path (if forum enabled)
- [ ] Captcha required on sensitive routes in production
- [ ] CORS rejects unknown origins

### Launcher + client

- [ ] Launcher login → `launch-info` `canInject` → download → inject → overlay (**K**)
- [ ] Release authorize works **without** Debug unlock
- [ ] Maintenance mode blocks inject
- [ ] Wrong HWID / banned account fails clearly
- [ ] Panic / exit / reinject same boot (basic stability)

### API scripts (from `oak/apps/api`)

```powershell
node .\scripts\smoke-protection.mjs
node .\scripts\smoke-telemetry.mjs
node .\scripts\smoke-forum.mjs
node .\scripts\smoke-payments.mjs
```

---

## 11. Product readiness (paid DayZ v1)

Do **not** claim a full paid feature set until [MASTER_CHECKLIST.md](./MASTER_CHECKLIST.md) ship gates and Pass A–C (or your agreed subset) are honest.

**Hard ship gates from master checklist (block “product is ready”):**

- [ ] **G0** One clean customer package
- [ ] **G1** Build id / version in menu + logs + launcher verify
- [ ] **G2** No stub toggles in Release UI
- [ ] **G3** Config persists
- [ ] **G4** Clean exit / reinject
- [ ] **G5** Panic fully unwinds
- [ ] **G6** Quiet Release logs
- [ ] **G7** Release folder has zero junk
- [ ] **G8** Smoke: inject → attach → present# → PASS/FAIL

**Also before charging for DayZ:**

- [ ] **L1–L6** Launcher/distribution hygiene
- [ ] **S2–S4** Debug hotkeys / unfinished ads stripped from Release
- [ ] Menu key **K** documented on customer card only
- [ ] Customer one-pager: run launcher → press K → support contact (no AI_LAUNCH_GUIDE)

Website-only soft launch can proceed without finishing every ESP/combat item — but marketing must not promise unfinished toggles.

---

## 12. Multi-product honesty

- [ ] Rust / EFT shown as Coming soon (or hidden) — not purchasable until real
- [ ] Plans seeded with correct `product_slug` (`dayz` vs `rust-*` / `eft-*`)
- [ ] Admin client upload always picks the right slug
- [ ] Launcher product picker does not offer inject for unavailable products

---

## 13. After first publish (ops runbook)

- [ ] Save release SHA-256 + watermark id + runtime key id in private ops notes
- [ ] Keep an unmarked rollback DLL offline (never public)
- [ ] Watch Discord/security alerts (refresh-reuse, HWID, lease anomalies)
- [ ] Document: how to rotate runtime package keys (and that SPKI pin requires client rebuild)
- [ ] Document: how to revoke a leaked release and push a new one
- [ ] Document: NOWPayments dispute / partial payment handling
- [ ] On-call: who responds to tickets in first 48h
- [ ] Calendar reminder: game update → offset dump → rebuild → re-gate → re-upload

---

## 14. Suggested order of work (ops)

1. **Domain + Cloudflare + Turnstile**  
2. **VPS/API + website host + TLS + SMTP**  
3. **All production secrets + admin password**  
4. **NOWPayments KYB + test payment**  
5. **Bake SPKI + loader/sys pins; Release launcher/DLL; ship gate; admin upload**  
6. **Full smoke (section 10)**  
7. **Legal pages + support channel**  
8. **Open site** → then **open paid DayZ** only when section 11 gates are honest  

---

## Quick copy-paste (DLL ship)

```powershell
$dll = "C:\path\to\oak\clients\dayz\build\Release\dayz_internal.dll"
node C:\path\to\oak\packages\protect\tools\ship_security_gate.mjs --dll $dll --mutate
# Upload $dll in Admin only if ok:true (product slug: dayz)
```

---

## Related docs

| Doc | Role |
|-----|------|
| [MASTER_CHECKLIST.md](./MASTER_CHECKLIST.md) | In-game / launcher product features |
| [payments.md](./payments.md) | NOWPayments-only decisions |
| [anti-piracy-plan.md](./anti-piracy-plan.md) | Lease / runtime package design |
| [security-hardening-2026-08-03.md](./security-hardening-2026-08-03.md) | What was fixed vs residual S1–S7 |
| [multi-product-structure.md](./multi-product-structure.md) | Monorepo + slugs |
| `clients/dayz/docs/AI_LAUNCH_GUIDE.md` | Private inject / kdmapper ops (not for customers) |
