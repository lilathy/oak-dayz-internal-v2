# API & admin features (`oak/api`)

Express + SQLite API (default `http://127.0.0.1:8787`). Admin static UI at `/admin/`.

## Public / customer API (high level)

| Group | Capabilities |
|-------|----------------|
| `/v1/auth` | Register, login, refresh (reuse detection), logout, password reset, email verify, captcha status |
| `/v1/session/validate` | License + HWID snapshot for launcher/site |
| `/v1/hwid` | Bind HWID; password+email confirm reset (legacy reset disabled) |
| `/v1/licenses` | Status + redeem (buyer-bound codes) |
| `/v1/purchases` | Plans catalog, checkout (NOWPayments), IPN webhook, mock pay (**dev only**), internal fulfill |
| `/v1/products` | Product catalog, launch-info, client download (licensed), protection lease/bootstrap/runtime key |
| `/v1/forum` | Categories, threads, posts, reactions, search, reports |
| `/v1/tickets` | Customer support tickets (owner or admin) |
| `/v1/site` | Public site settings / changelog / FAQ-style content |
| `/v1/telemetry` | Launcher/client events; feeds security alerts |
| `/health` | Liveness (minimal in production) |

## Admin API (`/v1/admin/*`)

Role-gated. Admin browser uses httpOnly `oak_admin_at` cookie (`X-Oak-Client: admin`).

Typical capabilities:

- Overview / analytics
- Security alerts (ack / resolve)
- Upload DayZ client DLL + launcher builds
- Site settings (maintenance blocks inject)
- Forum moderation (reports, mute, pin/lock via admin forum routes)
- Mint redeem codes (optional buyer binding)
- Purchases list
- Support ticket staff reply / status
- Users: ban, adjust time, inspect
- Protection / runtime package admin tools

## Admin UI panels (`/admin/`)

| Panel | What staff do |
|-------|----------------|
| Overview | Counts (users, codes, banned, views) |
| Security alerts | Triage abuse / auth anomalies |
| Analytics (7d) | Traffic-style stats |
| Upload DayZ client | Publish `dayz_internal.dll` for launcher download |
| Upload launcher | Publish `OakLauncher` for website download |
| Site settings | Maintenance, Discord webhook, etc. |
| Forum reports / mute | Moderation |
| Generate redeem codes | Mint + optional buyer bind |
| Purchases | Payment rows |
| Codes | Filter unused/used by plan |
| Support tickets | Staff thread + reply |
| Users / remaining time | Ban, grant time |

## Security notes

- CORS allowlist; trust-proxy parsed safely
- Helmet + admin CSP
- Production: no mock checkout; captcha fail-closed without Turnstile
- Secrets live in `oak/apps/api/.env` (gitignored)

See also [payments.md](./payments.md), [security-hardening-2026-08-03.md](./security-hardening-2026-08-03.md).
