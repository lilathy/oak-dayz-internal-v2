# Oak customer site (Next.js)

Customer-facing portal: accounts, licenses, downloads, support tickets and the
community forum.

## Run

```powershell
# terminal 1 — API
cd oak\api
npm run start

# terminal 2 — site
cd oak\web
npm run dev
```

Open **http://localhost:3000**.

> Use `localhost`, not `127.0.0.1`. The session refresh cookie is `SameSite`
> scoped, so the site and the API must share a host name. `localhost:3000` and
> `localhost:8787` count as the same site; `127.0.0.1:3000` and `localhost:8787`
> do not, and sessions would not survive a page reload.

Admin panel lives on the API: `http://localhost:8787/admin/`.

## Configuration (`.env.local`)

| Variable | Purpose |
| --- | --- |
| `NEXT_PUBLIC_API_URL` | API base URL. Must share a registrable domain with the site. |
| `NEXT_PUBLIC_TURNSTILE_SITE_KEY` | Optional Cloudflare Turnstile key. Blank disables captcha widgets. |
| `NEXT_PUBLIC_ANALYTICS_ORIGIN` | Optional origin of an external analytics script, added to the CSP allowlist. |

In production also set `NEXT_PUBLIC_API_URL` to an https URL — the CSP then adds
`upgrade-insecure-requests` automatically.

## Session handling

- The access token is held **in memory only**. It is never written to
  `localStorage` or `sessionStorage`, so an injected script cannot persist a
  stolen session past a reload.
- The refresh token is an **httpOnly, SameSite, path-scoped cookie** set by the
  API, unreadable from JavaScript.
- `api()` transparently refreshes once on a 401 and replays the request; parallel
  401s share a single refresh so the rotating token is not raced.
- Refresh tokens are single use. Replaying one fails and clears the session.
- Users can list and revoke individual sessions, or sign out everywhere, from
  `/account`.

## Security headers

Set in `next.config.ts` for every route:

- Content-Security-Policy: no framing, no plugins, `script-src`/`connect-src`
  limited to this origin plus the API, Turnstile and the configured analytics
  origin.
- `X-Content-Type-Options`, `X-Frame-Options`, `Referrer-Policy`,
  `Permissions-Policy`, `Cross-Origin-Opener-Policy`, HSTS.
- `poweredByHeader` is off.

Pages are statically prerendered, so the CSP is an origin allowlist rather than a
nonce policy — a nonce cannot be embedded in prerendered HTML.

## Forum

| Route | Purpose |
| --- | --- |
| `/forum` | Sections, recent activity, search |
| `/forum/c/[slug]` | Threads in a section, paginated |
| `/forum/c/[slug]/new` | Thread composer |
| `/forum/t/[id]` | Thread with replies, reactions, reporting, moderation |
| `/forum/search` | Title and post-body search |
| `/forum/members/[username]` | Public member profile |

Guests can read. Posting requires a signed-in account with a verified email.
Post bodies are rendered to HTML by the API from a strict Markdown subset built
on an escaped copy of the input, so `bodyHtml` never contains author-controlled
markup. See `oak/apps/api/README.md` for the anti-spam rules and moderation API.
