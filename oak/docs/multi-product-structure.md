# Multi-product Oak structure

Oak is one commercial stack for multiple games. Shared control plane + one launcher; native cheats live per slug.

## Layout

```text
oak/
  apps/
    web/          # Next.js site (all products)
    api/          # Express API (product_slug everywhere)
    launcher/     # Shared OakLauncher
  packages/
    protect/      # OakProtect SDK + ship_security_gate
  clients/
    dayz/         # DayZ DLL, driver, inject scripts, docs
    cs2/          # CS2 DLL, driver, offset tools, docs
    rust/         # Stub (slug rust)
    eft/          # Stub (slug eft)
  docs/           # Cross-product docs
  scripts/        # Repo-wide build / obfuscate
  bin/            # Staged shared binaries for local ops
```

## Product slugs

| Slug | Display | Stage dir | DLL |
|------|---------|-----------|-----|
| `dayz` | DayZ | `C:\oak\dayz\` | `dayz_internal.dll` |
| `rust` | Rust | `C:\oak\rust\` | `rust_internal.dll` |
| `eft` | Escape from Tarkov | `C:\oak\eft\` | `eft_internal.dll` |
| `cs2` | CS2 | `C:\oak\cs2\` | `cs2_internal.dll` |

## Shared vs per-client

**Shared:** website, API, launcher, protect, account/forum/support/payments.

**Per client:** game DLL sources, driver/inject path, offsets, in-game UI, product CMake, DayZ-only scripts under `clients/dayz/scripts`.

## Licenses

Plans are per product (`day1`/`day7`/`day30` = DayZ; `rust-day7`, `eft-day30`, `cs2-day7`, …). Checkout/redeem writes `product_licenses` for that slug. DayZ still honors the legacy single-row `licenses` table for compatibility.

## Publish

```powershell
node oak/packages/protect/tools/ship_security_gate.mjs --dll oak/clients/dayz/build/Release/dayz_internal.dll --mutate
```

See [PRE_PUBLISH_CHECKLIST.md](./PRE_PUBLISH_CHECKLIST.md) and [AGENTS.md](../AGENTS.md).
