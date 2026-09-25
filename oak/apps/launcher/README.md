# Oak Launcher

WPF desktop client. Signs the user in, lists products (DayZ / Rust / EFT),
downloads the current client for the selected slug, and launches via a
product-specific backend (DayZ = kernel/BE path under `C:\oak\dayz\`).

## Build

```powershell
cd oak\apps\launcher
dotnet build OakLauncher.csproj -c Release
```

Output: `bin\Release\net8.0-windows\OakLauncher.exe`.

## Multi-product

See `oak/docs/multi-product-structure.md`. Rust/EFT show in the product list when
seeded in the API; launch returns “not available yet” until their backends exist.

For a single-file build to hand to customers:

```powershell
dotnet publish OakLauncher.csproj -c Release -r win-x64 --self-contained false `
  -p:PublishSingleFile=true -o publish
```

## Configuration

The API endpoint is resolved in this order:

1. `OAK_API_URL` environment variable
2. `oak.launcher.json` next to the executable
3. the built-in default (`http://127.0.0.1:8787`)

```json
{ "apiUrl": "https://api.yourdomain.com" }
```

Nothing else needs to be configured — product, loader path, client build and
version all come from the API.

## Runtime layout

| Path | Contents |
| --- | --- |
| `%LOCALAPPDATA%\Oak\session.bin` | Refresh token, DPAPI-encrypted for the current Windows user |
| `%LOCALAPPDATA%\Oak\clients\<slug>\` | Downloaded client builds |
| `%LOCALAPPDATA%\Oak\launcher.log` | Crash log |

## Elevation

The launcher ships with a `requireAdministrator` manifest. The loader injects
with `OpenProcess` + `CreateRemoteThread` against `DayZ_x64.exe`, which needs
administrator rights, and it runs as a child of the launcher — so elevating once
at startup avoids a confusing failure at inject time.

## Injection flow

The injection method is unchanged: the launcher shells out to
`OakImGuiOverlayLoader.exe "<dll path>"`.

1. Re-fetch `launch-info` so a lapsed licence or new build is caught.
2. Resolve the loader — the API's configured path first, then a copy next to the
   launcher (the API stores a build-machine path that will not exist on a
   customer PC).
3. Download the client only if the local copy's SHA-256 differs from the one the
   API reported, verify the hash, and swap the file in atomically.
4. Run the loader, streaming its output into the status line.

Loader exit codes are translated to plain messages: `1` client unreadable,
`2` DayZ never started, `3` injection blocked (usually missing elevation).

## HWID

The fingerprint is `SHA-256` over the Windows `MachineGuid`, the system volume
serial, and the CPU identifier. It deliberately excludes the OS version, which
changes on every Windows update.

When the recipe changes, the launcher proves ownership of the old binding via
`POST /v1/hwid/migrate` and rebinds silently, so users are not locked out. A
manual reset is available from the account panel once every 72 hours.
