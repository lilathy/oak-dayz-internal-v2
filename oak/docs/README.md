# Oak documentation index

Start here. Feature maps, ship checklists, security, and agent guides.

## Must-read for shipping

| Doc | When |
|-----|------|
| [PRE_PUBLISH_CHECKLIST.md](./PRE_PUBLISH_CHECKLIST.md) | Full go-live: domain, Cloudflare, hosting, payments, secrets, ship gates, smoke |
| [multi-product-structure.md](./multi-product-structure.md) | Folder layout + product slugs |
| [AGENTS.md](../AGENTS.md) | Rules for AI/agents (always mutate before publish) |
| [AI_LAUNCH_GUIDE.md](./AI_LAUNCH_GUIDE.md) | → DayZ inject guide |

## Feature documentation

| Doc | Covers |
|-----|--------|
| [features-overview.md](./features-overview.md) | Whole stack at a glance |
| [features-website.md](./features-website.md) | Next.js customer site |
| [features-api-admin.md](./features-api-admin.md) | API + admin panel |
| [features-launcher.md](./features-launcher.md) | WPF launcher + kernel path |
| [features-client.md](./features-client.md) | In-game DLL / menu |
| [features-protect.md](./features-protect.md) | Licenses, leases, watermark, mutate |

## Security & payments

| Doc | Covers |
|-----|--------|
| [anti-piracy-plan.md](./anti-piracy-plan.md) | Commercial protection design |
| [security-hardening-2026-08-03.md](./security-hardening-2026-08-03.md) | Latest hardening remediations |
| [security-audit-2026-08-03.md](./security-audit-2026-08-03.md) | Earlier audit + residual items |
| [payments.md](./payments.md) | NOWPayments only |
| [telemetry-abuse-plan.md](./telemetry-abuse-plan.md) | Abuse / alerts posture |

## Component READMEs

- `oak/apps/api/README.md` — API setup
- `oak/apps/web/README.md` — website setup
- `oak/apps/launcher/README.md` — launcher
- `oak/packages/protect/README.md` — OakProtect SDK
- `oak/docs/multi-product-structure.md` — monorepo + product slugs
