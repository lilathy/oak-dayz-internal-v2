#!/usr/bin/env python3
"""Diff DayZServer player ints while forcing m_HealthLevel — layout matches client."""
import importlib.util, struct, time, sys, ctypes
from ctypes import wintypes
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "p",
    r"..\..\..\clients\dayz\scripts\_probe_vitals_netsync.py",
)
p = importlib.util.module_from_spec(spec)
spec.loader.exec_module(p)

PROFILES = Path(r"C:\Program Files (x86)\Steam\steamapps\common\DayZServer\oak_profiles")
FORCE = PROFILES / "oak_force_hl.txt"
VITALS = PROFILES / "oak_vitals.txt"

# Server World / LocalPlayer offsets often match client; NearEntList for players.
# We'll also scan for DayZPlayer by walking near list if local fails.


def set_hl(level: int):
    FORCE.write_text(str(level) + "\n", encoding="ascii")
    time.sleep(3.5)
    if VITALS.exists():
        print("vitals:", VITALS.read_text(encoding="utf-8", errors="ignore").strip())


def find_players(h, base):
    """Return list of (label, ptr) candidate player entities on server."""
    found = []
    # Try client-matching world chain
    world = p.u64(h, base + p.WORLD_OFF)
    print(f"world@+{p.WORLD_OFF:#x} = {world:#x} valid={p.valid(world)}")
    if p.valid(world):
        local = p.u64(h, world + p.LOCAL_PLAYER)
        print(f"  local@+{p.LOCAL_PLAYER:#x} = {local:#x}")
        if p.valid(local):
            found.append(("local", local))
        near = p.u64(h, world + p.NEAR_LIST)
        nsz = p.i32(h, world + p.NEAR_SIZE)
        print(f"  near={near:#x} nsz={nsz}")
        if p.valid(near) and 0 < nsz < 256:
            for i in range(nsz):
                ent = p.u64(h, near + i * 8)
                if p.valid(ent) and all(ent != x for _, x in found):
                    found.append((f"near[{i}]", ent))
    return found


def snap_ints(h, ent, lo=0x80, hi=0x2000):
    out = {}
    for off in range(lo, hi, 4):
        out[off] = p.i32(h, ent + off)
    return out


def main():
    levels = [0, 1, 2, 3, 4, 0, 2]
    if len(sys.argv) > 1:
        levels = [int(x) for x in sys.argv[1:]]

    pid = p.find_pid("DayZServer_x64.exe")
    if not pid:
        print("no DayZServer")
        return 1
    h = p.open_proc(pid)
    base, msize = p.module_base(h, "DayZServer_x64.exe")
    print(f"server base={base:#x} size={msize:#x}")

    players = find_players(h, base)
    if not players:
        print("no players found via world chain — abort")
        return 1
    for lab, ptr in players:
        print(f"  candidate {lab}={ptr:#x}")

    # Prefer first valid; if multiple, track all
    targets = players[:4]
    snaps = {lab: [] for lab, _ in targets}

    for lv in levels:
        print(f"\n=== force hl={lv} ===")
        set_hl(lv)
        # re-resolve in case of streaming
        players2 = {lab: ptr for lab, ptr in find_players(h, base)}
        for lab, ptr0 in targets:
            ptr = players2.get(lab, ptr0)
            s = snap_ints(h, ptr)
            snaps[lab].append((lv, ptr, s))
            small = sum(1 for v in s.values() if 0 <= v <= 4)
            print(f"  {lab}@{ptr:#x} small0..4={small}")

    for lab, series_list in snaps.items():
        print(f"\n=== {lab} exact trackers ===")
        if len(series_list) < 2:
            continue
        base_snap = series_list[0][2]
        track = []
        for off in base_snap:
            series = [s[2][off] for s in series_list]
            want = [s[0] for s in series_list]
            if series == want:
                track.append((off, series))
        print(f"exact: {len(track)}")
        for off, series in track:
            print(f"  +0x{off:X} -> {series}")
        # partial
        print(f"=== {lab} partial (miss<=1, varies) ===")
        for off in base_snap:
            series = [s[2][off] for s in series_list]
            want = [s[0] for s in series_list]
            miss = sum(1 for a, b in zip(series, want) if a != b)
            if miss <= 1 and len(set(series)) > 1 and any(0 <= v <= 4 for v in series):
                if not any(off == t[0] for t in track):
                    print(f"  +0x{off:X} series={series} want={want} miss={miss}")

    p.k.CloseHandle(h)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
