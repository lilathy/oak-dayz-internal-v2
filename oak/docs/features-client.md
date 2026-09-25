# In-game client features (`dayz_internal.dll`)

Built from `oak/clients/dayz/src` (+ ImGui). Menu toggle: **K**. Overlay uses Present hook + ImGui.

Release builds define `OAK_REQUIRE_PROTECTION` — features stay off until lease/runtime package authorizes.

DayZ-specific docs: [`clients/dayz/docs/README.md`](../clients/dayz/docs/README.md).  
Open backlog: [`clients/dayz/docs/remaining.txt`](../clients/dayz/docs/remaining.txt).  
Shipped map: [`clients/dayz/docs/SHIPPED.md`](../clients/dayz/docs/SHIPPED.md).

## Visual / ESP

- Master ESP toggle; skeleton; chams
- Names, distance, player box
- Filters: players, zombies, animals, items, vehicles
- Containers, corpses, traps
- Loot categories: weapons, ammo, medical, food, clothing, tools, other
- Weapon ESP; health / blood / shock / stamina / hunger / thirst bars
- Local vitals HUD; local weapon ammo
- Grenade trajectory; crosshair; bullet tracers; impact / shot / hit markers
- Fullbright; no grass (misc-adjacent)
- **Batch4:** container contents, contamination zones, looking-at-me, player threat-gear viewer, item quantity, visibility-color slots (occluded path incomplete)

## Combat / aim

- Aimbot (enable, players/zombies, FOV draw, FOV size, smooth, bone, key, max distance)
- Fast bullets; no dispersion; perfect ballistics
- Magic bullet
- **Batch4:** silent aim, triggerbot, no recoil/sway, custom silent/aim binds, auto-fire while locked

## World / camera / overlay

- Daytime lock; clear weather
- FOV changer
- Soft third-person shoulder-cam (locked final design)
- Stream proof (`WDA_EXCLUDEFROMCAPTURE`)
- Freecam (+ move body, speed)
- Waypoints; Steam names / avatars
- Panic hide; disable overlays

## Misc / exploits

- Middle-click despawn (range)
- Loot magnet / container magnet (ranges) — “loot through walls” UI is magnet-only today
- Lag switch / warp (shipped)
- Door unlock (partial — local NoBE + natives; see door docs)
- Grenade through walls / loot living / mug / dupe — partial or stubs (see remaining)

## Protection hooks

- `OakProtectionAuthorize()` before enabling product behavior
- `OakProtectionIsAuthorized()` gates hot paths
- Prefers launcher-prefetched lease file (no in-process WinHttp under BE)
- Runtime offsets applied from signed package payload

## Build / obfuscation

Every CMake build runs:

```text
node packages/protect/tools/ship_security_gate.mjs --dll <out.dll> --mutate
```

Watermark lives in `packages/protect` (`OAKWMARK…` placeholder). See [features-protect.md](./features-protect.md).

## Staging

Launcher stages to `C:\oak\dayz\dayz_internal.dll` (+ `.oak-bootstrap`, `.oak-lease`).
