# OakProtect features (`oak/protect` + API protection)

Commercial license boundary — not anti-cheat evasion.

## What it does

| Piece | Role |
|-------|------|
| Bootstrap ticket | One-time, short-lived launch authorization from API |
| Lease | Session token + renew challenge |
| Runtime package | Server-signed offsets / policy payload |
| Multi-token auth | Several in-memory tokens must agree |
| `OAK_ENC_STR` | Compile-time XOR for sensitive path literals |
| Watermark | Per-build `OAKWMARK…` stamp for attribution |
| SPKI pin (optional) | Bake API public-key fingerprint into DLL for production |

## Always-on mutate (required)

**Every** `dayz_internal.dll` build (Debug and Release) must be mutated.

CMake POST_BUILD (automatic):

```bat
node oak\packages\protect\tools\ship_security_gate.mjs --dll path\to\dayz_internal.dll --mutate
```

Manual / publish re-check (agents must run before upload):

```powershell
node oak/packages/protect/tools/ship_security_gate.mjs --dll <absolute-dll> --mutate
```

Gate refuses ship if PDBs / unlock files sit beside the DLL or sensitive plaintext strings appear.

## OAK_RUNTIME_SPKI_PIN (production only)

Fingerprint of the API’s runtime-package **public** key. Empty for local/dev is OK. Set when you have stable production PEM keys:

```bat
cmake -DOAK_RUNTIME_SPKI_PIN=<sha256-hex> ...
```

Matches API `config.runtimePackageKeyId`.

## Dev vs Release

| | Debug | Release |
|--|-------|---------|
| Watermark mutate | Yes (always) | Yes (always) |
| `OAK_REQUIRE_PROTECTION` | No (auto-authorize for local) | Yes |
| `localClientDll` / unlock | Allowed in Debug launcher | Ignored / refused |

## Related docs

- [anti-piracy-plan.md](./anti-piracy-plan.md)
- [PRE_PUBLISH_CHECKLIST.md](./PRE_PUBLISH_CHECKLIST.md)
- [multi-product-structure.md](./multi-product-structure.md)
- `oak/packages/protect/README.md`
