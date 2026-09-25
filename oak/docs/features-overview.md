# Oak — product overview

Commercial DayZ / Rust / EFT stack: one website + API + launcher; per-game clients under `clients/<slug>`.

```text
Customer browser  →  apps/web (:3000)  →  apps/api (:8787)
Customer PC       →  apps/launcher     →  API (auth, download, bootstrap/lease)
                  →  product launch    →  C:\oak\<slug>\ + game process
Staff             →  http://127.0.0.1:8787/admin/
```

See [multi-product-structure.md](./multi-product-structure.md).

## Artifacts

| Artifact | Role |
|----------|------|
| `dayz_internal.dll` | In-game product (ImGui overlay + features) |
| `OakLauncher.exe` | Sign-in, download encrypted client, bootstrap, kernel launch |
| `oak_loader.exe` + `oak.sys` | Kernel manual-map path under BattlEye |
| Oak API | Auth, licenses, HWID, payments, forum, tickets, releases, protection |
| `oak/web` | Public marketing + account + forum + support |
| Admin UI | Staff operations at `/admin/` |

## Runtime folders

| Path | Purpose |
|------|---------|
| `C:\oak\<slug>\` | Per-product stage (DLL, handoff, inject markers) |
| `%LOCALAPPDATA%\Oak\` | Launcher session, encrypted client cache, `launch.log` |
| `%LOCALAPPDATA%\DayZ\oak_imgui.log` | Overlay diagnostics (dev) |

## Always-on obfuscation

Every CMake build of `dayz_internal.dll` runs `packages/protect/tools/ship_security_gate.mjs --mutate`. See [features-protect.md](./features-protect.md), [multi-product-structure.md](./multi-product-structure.md), and [PRE_PUBLISH_CHECKLIST.md](./PRE_PUBLISH_CHECKLIST.md).

## Payments

NOWPayments only (crypto + card/fiat). See [payments.md](./payments.md).
