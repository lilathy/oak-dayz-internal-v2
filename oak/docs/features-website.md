# Website features (`oak/web`)

Next.js app (default `http://127.0.0.1:3000`). Talks to the API via `NEXT_PUBLIC_API_URL`.

## Pages

| Route | Feature |
|-------|---------|
| `/` | Marketing home / status strip |
| `/plans` | License plans + checkout entry |
| `/login` | Sign in (optional Turnstile); `?next=` same-origin only |
| `/register` | Create account + captcha when configured |
| `/verify-email` | Email verification completion |
| `/forgot-password` | Request reset email |
| `/reset-password` | Apply reset token |
| `/account` | Profile, license status, HWID, redeem, session |
| `/download` | Download current launcher build (gated by auth/license as API allows) |
| `/changelog` | Public changelog from site content API |
| `/faq` | FAQ content |
| `/tos` | Terms of service |
| `/support` | Customer support tickets (create / view / reply — owner only) |
| `/forum` | Forum home / categories |
| `/forum/c/[slug]` | Category thread list |
| `/forum/c/[slug]/new` | New thread |
| `/forum/t/[id]` | Thread + posts (Markdown → sanitized HTML from API) |
| `/forum/search` | Search threads/posts |
| `/forum/members/[username]` | Public member profile |

## Behaviors

- Browser refresh token stored in **httpOnly** cookie (not readable by page JS).
- Forum post bodies use API-rendered HTML (`PostBody`); authors never inject raw HTML.
- Captcha enabled when Turnstile site key + API secret are set (production fail-closed on API).

## Env (website)

See `oak/apps/web/.env.local` pattern:

- `NEXT_PUBLIC_API_URL`
- `NEXT_PUBLIC_TURNSTILE_SITE_KEY` (optional locally)
- `NEXT_PUBLIC_ANALYTICS_ORIGIN` (optional)

No secrets belong in `NEXT_PUBLIC_*` variables.
