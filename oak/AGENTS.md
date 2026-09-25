# Oak agent instructions

This file is for coding agents (Cursor, etc.) working in the Oak tree (`oak/`).

## Monorepo layout

- Shared: `apps/web`, `apps/api`, `apps/launcher`, `packages/protect`
- Clients: `clients/dayz`
- See `docs/multi-product-structure.md`

## Always-on obfuscation

Every client DLL build must be watermark-mutated. DayZ CMake POST_BUILD runs:

`node packages/protect/tools/ship_security_gate.mjs --dll <built.dll> --mutate`

Or: `oak/scripts/obfuscate_client.ps1 -Slug dayz`

## Before publishing a new version

1. `node oak/packages/protect/tools/ship_security_gate.mjs --dll <path\to\client.dll> --mutate`
2. Confirm JSON `"ok": true`
3. Walk `oak/docs/PRE_PUBLISH_CHECKLIST.md`
4. Upload via Admin with the correct **product slug**

## Key docs

| Doc | Purpose |
|-----|---------|
| `docs/multi-product-structure.md` | Folder + slug map |
| `docs/PRE_PUBLISH_CHECKLIST.md` | Ship gate |
| `docs/features-*.md` | Feature maps |
| `docs/AI_LAUNCH_GUIDE.md` | DayZ BE kernel inject |
