# Oak DayZ Internal

DayZ internal client stack. See **[docs/multi-product-structure.md](docs/multi-product-structure.md)**.

## Layout

- `apps/web` — website  
- `apps/api` — API + admin  
- `apps/launcher` — shared launcher  
- `packages/protect` — OakProtect  
- `clients/dayz` — DayZ DLL / driver / inject scripts  

## Building

```powershell
cd oak
.\scripts\build_all.ps1
```

Needs Visual Studio (C++ + WDK for the driver), CMake, .NET 8, and Node.js (watermark mutate).

## DayZ BattlEye path (short)

1. Build via `scripts\build_all.ps1` (or DayZ DLL-only under `clients\dayz`).
2. Admin: `clients\dayz\scripts\setup_be_path.ps1` then reboot if blocklist changed.
3. Prefer launcher Launch, or `clients\dayz\scripts\be_quiet_entry.ps1`.
4. Stage dir is **`C:\oak\dayz\`** (`dayz_internal.dll`, loader, sys, inject.log).
5. Menu key: **K**.

Full guide: `docs/AI_LAUNCH_GUIDE.md`.

## Docs

- [docs/README.md](docs/README.md)  
- [docs/PRE_PUBLISH_CHECKLIST.md](docs/PRE_PUBLISH_CHECKLIST.md)  
- [AGENTS.md](AGENTS.md)
