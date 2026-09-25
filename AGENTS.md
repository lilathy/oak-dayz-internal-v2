# Agent instructions

The Oak DayZ project lives in **`oak/`** (public source release).

Read and follow:

- [`oak/AGENTS.md`](./oak/AGENTS.md)
- [`oak/docs/multi-product-structure.md`](./oak/docs/multi-product-structure.md)
- [`oak/docs/PRE_PUBLISH_CHECKLIST.md`](./oak/docs/PRE_PUBLISH_CHECKLIST.md)

**Before publishing any client DLL:**

```powershell
node oak/packages/protect/tools/ship_security_gate.mjs --dll <path\to\client.dll> --mutate
```

**Never commit** local config (`.env`, `oak.launcher.json`), databases (`data/`), or build outputs — see `.gitignore`.
