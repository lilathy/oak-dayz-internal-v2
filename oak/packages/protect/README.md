# OakProtect SDK

Reusable commercial protection for every Oak product client (DayZ now, others later).

## What this protects

- Bootstrap ticket consumption
- Lease + challenge-response renewal
- Signed runtime-package verification
- Multi-token authorization gate
- Per-release watermark (`g_OakBuildMark`)

## Always-on obfuscation (required)

Every `dayz_internal.dll` build — **Debug and Release** — must pass the ship gate with mutate.

CMake POST_BUILD runs this automatically. Agents must still re-run it before any publish/upload:

```bat
node oak\packages\protect\tools\ship_security_gate.mjs --dll path\to\dayz_internal.dll --mutate
```

Manual watermark-only tool (gate preferred):

```bat
node oak\packages\protect\tools\mutate_release.mjs --dll path\to\client.dll
```

See `oak/docs/PRE_PUBLISH_CHECKLIST.md` and `oak/AGENTS.md`.

## What this deliberately does not do

- Pack/virtualize the whole feature DLL (breaks manual-map inject and adds latency)
- Anti-cheat evasion
- Claim uncrackability

## Wire a new client

1. Compile with `OAK_REQUIRE_PROTECTION` in Release.
2. Define `OAK_PRODUCT_SLUG` to that product's slug.
3. Link:
   - `oak/clients/<slug>/src/protection_client.cpp` (or a product-specific thin wrapper)
   - `oak/packages/protect/src/oak_watermark.cpp`
   - `oak/clients/<slug>/src/runtime_offsets.cpp` (or product equivalent)
4. Call `OakProtectionAuthorize()` before enabling features.
5. Gate hot paths with `OakProtectionIsAuthorized()`.
6. Ensure CMake (or CI) always runs `ship_security_gate.mjs --mutate` POST_BUILD.

## Optional commercial packer

If you later buy VMProtect/Themida, protect **only** the auth object file / static lib, never the whole game-facing DLL. Keep an unmarked rollback build.

## Challenge renew

Renewals require:

```text
HMAC-SHA256(key = SHA256(leaseToken|clientNonce), data = renewChallenge)
```

Patching a local `authorized = true` flag is insufficient; renewals still fail without lease material.
