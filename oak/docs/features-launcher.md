# Launcher features (`oak/launcher`)

WPF app (`OakLauncher.exe`). Config: `oak.launcher.json` beside the exe (or `OAK_API_URL`).

## User flows

| Feature | Behavior |
|---------|----------|
| Sign in | Username/email + password against API |
| Stay signed in | Refresh token in DPAPI-protected `%LOCALAPPDATA%\Oak\session.bin` |
| Session restore | Silent refresh on start |
| Product list | DayZ / Rust / EFT from API (`product_slug`) |
| Product / launch-info | Checks per-product entitlement, HWID, maintenance, current client |
| Download client | Encrypted cache under `%LOCALAPPDATA%\Oak\clients\<slug>\` |
| Bootstrap | Short-lived ticket + **prefetched lease** (so DayZ never WinHttp) |
| Kernel launch (DayZ) | Stage `C:\oak\dayz\`, start `oak_loader` + `DayZ_BE`, wait for inject success |
| Other products | “Not available yet” until backends land |
| Telemetry | Launch success/fail events to API |
| Elevation | Requires admin for kernel path |

## Launch path (BattlEye)

1. Ensure client (download or cache hit)
2. Materialize plaintext DLL to stage dir
3. Write `.oak-bootstrap` + `.oak-lease` (hidden)
4. `oak_loader.exe` + `oak.sys` → map into game
5. Delayed secure wipe of staged client + handoff files

Usermode `CreateRemoteThread` injectors are **not** the supported BE path.

## Config keys (`oak.launcher.json`)

| Key | Purpose |
|-----|---------|
| `apiUrl` | API base (Release requires HTTPS except loopback) |
| `expectedLoaderSha256` | Optional pin for `oak_loader.exe` |
| `expectedSysSha256` | Optional pin for `oak.sys` |
| `localClientDll` | **Debug builds only** — plaintext override path |

## Logs

- `%LOCALAPPDATA%\Oak\launch.log` — scrubbed/truncated in Release
- `C:\oak\inject.log` — loader evidence; wiped after successful map

## Related

- Kernel details: [AI_LAUNCH_GUIDE.md](./AI_LAUNCH_GUIDE.md)
- Protection handoff: [features-protect.md](./features-protect.md)
