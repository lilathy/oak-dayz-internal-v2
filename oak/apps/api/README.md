# Oak Auth / License / HWID API (localhost)

Bound to `127.0.0.1` only. SQLite file in `data/oak.sqlite`.

## Quick start

```powershell
cd oak\apps\api
copy .env.example .env
# edit .env — set JWT_* secrets and ADMIN_PASSWORD
npm install
npm run seed
npm run dev
```

Health: http://127.0.0.1:8787/health  
Customer site (Next.js): http://127.0.0.1:3000 — `cd oak\apps\web && npm run dev`  
Admin: http://127.0.0.1:8787/admin/

## Endpoints

### Auth
- `POST /v1/auth/register` `{ email, username, password, captchaToken? }` — rejects
  reserved usernames and passwords that contain the account name or email
- `POST /v1/auth/login` `{ login, password, hwid?, captchaToken? }`
- `POST /v1/auth/refresh` `{ refreshToken }` **or** the refresh cookie
- `POST /v1/auth/logout` Bearer + optional `{ refreshToken }`
- `POST /v1/auth/logout-all` Bearer — revokes every session
- `GET  /v1/auth/sessions` Bearer — active sessions (IP is truncated)
- `DELETE /v1/auth/sessions/:id` Bearer — revoke one session
- `POST /v1/auth/verify/send` Bearer — resend the confirmation email
- `POST /v1/auth/verify` `{ token }` — confirm an email address
- `GET  /v1/auth/captcha` — whether captcha is configured
- `GET  /v1/auth/me` Bearer

**Browser vs launcher.** A request carrying `X-Oak-Client: web` gets its refresh
token as an httpOnly, `SameSite`, `Path=/v1/auth` cookie and *not* in the JSON
body. The launcher omits the header and keeps using bearer tokens. The site and
the API must therefore share a registrable domain.

Changing an email address requires the current password and drops the account
back to unverified.

### Forum
Reads are public; writes need a signed-in account with a verified email.

- `GET  /v1/forum/categories` — sections plus recent activity
- `GET  /v1/forum/categories/:slug?page=` — threads, pinned first
- `POST /v1/forum/categories/:slug/threads` `{ title, body, captchaToken? }`
- `GET  /v1/forum/threads/:id?page=` — thread with posts and viewer state
- `POST /v1/forum/threads/:id/posts` `{ body }`
- `PATCH /v1/forum/posts/:id` `{ body }` — author within 30 minutes, or admin
- `DELETE /v1/forum/posts/:id` — author or admin (soft delete)
- `POST|DELETE /v1/forum/posts/:id/react` — one like per user per post
- `POST /v1/forum/reports` `{ targetType, targetId, reason, note? }`
- `GET  /v1/forum/search?q=` — minimum 3 characters, LIKE wildcards escaped
- `GET  /v1/forum/members/:username` — public profile

Moderation (admin):
- `PATCH /v1/admin/forum/threads/:id` `{ pinned?, locked?, deleted?, title?, categorySlug? }`
- `GET  /v1/admin/forum/reports?status=open|actioned|dismissed`
- `PATCH /v1/admin/forum/reports/:id` `{ status }`
- `POST /v1/admin/forum/users/:id/mute` `{ hours, reason? }` — `hours: 0` unmutes
- `POST /v1/admin/forum/categories`, `PATCH /v1/admin/forum/categories/:slug`

**Content safety.** Post bodies are stored raw and rendered server side into
`body_html` by `renderPostHtml` in `src/forum.ts`: the input is HTML-escaped
first and every tag in the output is emitted by that function, so no third-party
sanitiser is involved and author markup can never reach the page. Links must be
absolute `http(s)`, get `rel="nofollow ugc noopener noreferrer"`, and any URL
containing a smuggled entity is left as plain text.

**Anti-spam** (`FORUM_LIMITS`, enforced against the database so restarts do not
reset them):

| Guard | Default |
| --- | --- |
| Verified email required | yes |
| Minimum account age before first post | 15 minutes |
| Cooldown between posts | 20 seconds |
| New threads per hour | 5 |
| Posts per hour | 20 |
| Links allowed after | 5 posts or 24 hours |
| Maximum links per post | 5 |
| Identical body rejected within | 10 minutes |
| Author edit window | 30 minutes |

Reports are deduplicated per reporter per item. Deleting a post decrements the
author's post count so removed spam does not buy link privileges.

### HWID
- `POST /v1/hwid/bind` `{ hwid }`
- `POST /v1/hwid/migrate` `{ from, to }` — rebinds without spending a reset, but
  only for a caller that can prove it owns the currently bound fingerprint. Lets
  the launcher change its HWID recipe without locking anyone out.
- `POST /v1/hwid/reset` (72h cooldown for users)
- `GET  /v1/hwid/status`

### Products / launcher
- `GET  /v1/products/public` — public catalog (no auth, no inject paths)
- `GET  /v1/products/public/:slug` — public DayZ status/version
- `GET  /v1/products` Bearer
- `GET  /v1/products/:slug` Bearer
- `GET  /v1/products/:slug/launch-info` Bearer — version, status, license (incl. remaining time), HWID, inject paths
- `PATCH /v1/products/:slug` Admin — update version/status/paths/changelog

Customer site (bare HTML): http://127.0.0.1:8787/ — register, login, account, redeem, plans placeholders, download notes.  
WPF launcher: `oak\launcher` → `oak\bin\OakLauncher.exe`.

### Releases / downloads
- Admin upload DayZ **client DLL**: `POST /v1/admin/releases/client/dayz` multipart `file` + `version` (launcher-only; no public URL)
- Launcher fetch: `GET /v1/products/:slug/client` Bearer + active license + HWID (streams DLL)
- Meta: `GET /v1/products/:slug/client/meta`
- Website launcher: `GET /v1/downloads/launcher` (public) + `GET /v1/downloads/launcher/meta`
- Admin upload launcher: `POST /v1/admin/releases/launcher` multipart `file` + `version`

### Client authorization and runtime packages
- `POST /v1/products/:slug/bootstrap` Bearer + `{ hwid }` — launcher-only,
  returns a one-time, 60-second ticket bound to the live refresh session, HWID,
  product, and current release.
- `POST /v1/products/:slug/lease` `{ bootstrapTicket, hwid, clientNonce }` —
  client-only; consumes a ticket and returns a renewable five-minute lease.
- `POST /v1/products/:slug/lease/renew` `{ leaseToken, clientNonce, challengeResponse }`
  - `challengeResponse` = hex(HMAC-SHA256(key=SHA256(leaseToken|clientNonce), data=renewChallenge))
- `POST /v1/products/:slug/runtime-package` `{ leaseToken }`
- `GET /v1/products/:slug/runtime-package-key` — ECDSA P-256 public key.
- Admin: stage/activate/revoke packages at
  `POST /v1/admin/protection/runtime-packages/:slug`,
  `POST /v1/admin/protection/runtime-packages/:id/activate`, and
  `POST /v1/admin/protection/runtime-packages/:id/revoke`; inspect/revoke
  active leases at `GET /v1/admin/protection/leases` and
  `POST /v1/admin/protection/leases/:id/revoke`.

Production requires stable `PROTECTION_PEPPER`, `RUNTIME_PACKAGE_PRIVATE_KEY`,
and `RUNTIME_PACKAGE_PUBLIC_KEY` values. PEM values may use `\n` escapes in
the environment. Never rotate the protection pepper without intentionally
invalidating all outstanding tickets and leases.

Build a DayZ runtime package from current offsets with:
`node scripts/build-runtime-package.mjs`
Activate it with an admin access token:
`node scripts/build-runtime-package.mjs --activate --token <accessJwt>`

The launcher keeps client DLLs in an AES-GCM + DPAPI encrypted `.oakc` cache
under `%LOCALAPPDATA%\Oak\clients\`. Plaintext is materialized only for the
injection window, then deleted. A copied cache is useless on another Windows
user / HWID.

### Site
- `GET /v1/site/bootstrap` — announcement, maintenance, discord, analytics, versions
  - `analytics` is `{ src, domain }` with an https-validated script **URL**. Free
    form HTML is deliberately not accepted: it would let one admin account inject
    script into every visitor's browser.
- `POST /v1/site/event` — `{ event: page_view|click, path? }`
- Admin: `GET/PATCH /v1/admin/settings`, `GET /v1/admin/analytics`, `GET /v1/admin/online`, `GET /v1/admin/downloads`
- Plans: `day1` (1d), `day7` (7d), `day30` (30d) — redeem stacks onto `max(now, expires_at)`
- `GET  /v1/licenses/me` Bearer — license + `remainingSeconds` / `remainingDays`
- `POST /v1/licenses/redeem` `{ code }` Bearer — single-use key → add time

### Telemetry & security alerts
- `POST /v1/telemetry/web` — allowlisted website events; optional session; IP rate limit
- `POST /v1/telemetry/launcher` — Bearer JWT; allowlisted launcher events
- `POST /v1/telemetry/client` — `{ leaseToken, event, meta? }` lease-auth only (not account JWT)
- Meta is scrubbed (no tokens/passwords/raw HWID); unknown event names are rejected
- Protection failures (`bootstrap_reused`, `hwid_mismatch`, `lease_revoked`, …) raise `security_alerts`
- Admin: `GET /v1/admin/alerts`, ack/resolve/dismiss, `POST /v1/admin/alerts/ingest-leak`
- Admin: `GET /v1/admin/telemetry/summary` — 24h funnel + inject success rate
- Discord webhook setting `alert_discord_webhook` (https discord.com webhooks only — SSRF gated)
- Smoke: `node scripts/smoke-telemetry.mjs`

### Purchases (payment-ready)
- `POST /v1/purchases/fulfill` header `X-Oak-Fulfill-Secret: $FULFILL_SECRET`
  - body `{ planId, buyerUserId | buyerEmail, purchaseId?, note? }`
  - creates one redeem code for the buyer (raw code returned once)

### Launcher gate
- `GET /v1/session/validate` Bearer — 200 if license active

### Admin (role=admin)
- UI: http://127.0.0.1:8787/admin/
- `GET  /v1/admin/overview`
- `GET  /v1/admin/plans`
- `POST /v1/admin/codes` `{ planId, count?, note? }` — returns raw codes once
- `GET  /v1/admin/codes?status=&planId=`
- `POST /v1/admin/codes/:id/revoke`
- `GET  /v1/admin/users?q=`
- `GET  /v1/admin/users/:id`
- `POST /v1/admin/users/:id/ban` `{ reason? }`
- `POST /v1/admin/users/:id/unban`
- `POST /v1/admin/users/:id/license` `{ planId? | plan?, days?, status? }`
- `POST /v1/admin/users/:id/hwid/reset`
- `GET  /v1/admin/audit`

## Security defaults
- Argon2id passwords, with weak and account-derived passwords rejected
- Access JWT (15m) + rotating single-use refresh tokens (hashed in DB, revocable);
  every access JWT is tied to its live refresh-session ID, so a revoked session
  immediately loses API and lease access
- Browser sessions use an httpOnly `SameSite` cookie; the token never reaches JS
- Email verification gates forum posting
- Optional Cloudflare Turnstile on register, login recovery and thread creation
  (set `TURNSTILE_SECRET`; unset means the checks are skipped)
- HWID stored hashed only (hint shown)
- Separate peppers for codes (`CODE_PEPPER`) and HWIDs (`HWID_PEPPER`) so
  rotating the JWT secret does not invalidate them
- Rate limits on auth / HWID / forum writes, keyed per account where possible
- Helmet + CORS allowlist; `TRUST_PROXY` controls whether `X-Forwarded-For` is
  believed, so client IPs cannot be spoofed by default
- Audit log for login/bind/ban/license and every moderation action

Run `node scripts/smoke-forum.mjs` against a live API to exercise the auth flow,
the Markdown sanitiser and every forum abuse guard.
Run `node scripts/smoke-protection.mjs` to exercise ticket replay prevention,
HWID enforcement, lease renewal/revocation, and signed package delivery.
Run `node scripts/smoke-payments.mjs` for mock checkout → auto-redeem → admin inbox.

### Purchases / checkout
See [`oak/docs/payments.md`](../docs/payments.md) for locked platform decisions.

- `GET  /v1/purchases/catalog` — plans, prices, active provider
- `POST /v1/purchases/checkout` Bearer `{ planId, provider? }` — returns `checkoutUrl`
- `GET  /v1/purchases/mock/pay?purchaseId=` — dev mock completion (auto-redeems)
- `POST /v1/purchases/webhooks/nowpayments` — NOWPayments IPN (`x-nowpayments-sig`; crypto + fiat/card)
- `POST /v1/purchases/fulfill` — internal tooling (`X-Oak-Fulfill-Secret`)
- `GET  /v1/admin/purchases` — admin purchase inbox

## Example (PowerShell)

```powershell
# login as admin
$body = @{ login = "admin"; password = "OakAdmin!ChangeMe" } | ConvertTo-Json
$r = Invoke-RestMethod http://127.0.0.1:8787/v1/auth/login -Method POST -ContentType application/json -Body $body
$r.accessToken

# overview
Invoke-RestMethod http://127.0.0.1:8787/v1/admin/overview -Headers @{ Authorization = "Bearer $($r.accessToken)" }
```
