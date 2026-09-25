#!/usr/bin/env python3
"""Cycle server m_HealthLevel and diff client DayZPlayer ints to find netsync offset."""
import importlib.util, struct, time, sys
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


def snap_ints(h, local, lo=0x600, hi=0x1800):
    out = {}
    for off in range(lo, hi, 4):
        out[off] = p.i32(h, local + off)
    return out


def set_hl(level: int):
    FORCE.write_text(str(level) + "\n", encoding="ascii")
    # wait for server pulse (~2s)
    time.sleep(3.5)
    if VITALS.exists():
        print("vitals:", VITALS.read_text(encoding="utf-8", errors="ignore").strip())


def main():
    levels = [0, 1, 2, 3, 4, 1, 0]
    if len(sys.argv) > 1:
        levels = [int(x) for x in sys.argv[1:]]

    pid = p.find_pid("DayZ_x64.exe")
    if not pid:
        print("no DayZ")
        return 1
    h = p.open_proc(pid)
    base, msize = p.module_base(h, "DayZ_x64.exe")
    world, local, near, nsz = p.resolve_local(h, base)
    print(f"local=0x{local:X} nsz={nsz}")
    if not p.valid(local):
        return 1

    snaps = []
    for lv in levels:
        print(f"\n=== force hl={lv} ===")
        set_hl(lv)
        world, local, near, nsz = p.resolve_local(h, base)
        s = snap_ints(h, local)
        snaps.append((lv, local, s))
        # show small ints
        small = {hex(o): v for o, v in s.items() if 0 <= v <= 4}
        print("small0..4 count", len(small))

    # offsets that track the forced level across snaps
    print("\n=== tracking offsets ===")
    base_snap = snaps[0][2]
    track = {}
    for off in base_snap:
        series = [s[2][off] for s in snaps]
        want = [s[0] for s in snaps]
        if series == want:
            track[off] = series
    print(f"exact trackers: {len(track)}")
    for off, series in sorted(track.items()):
        print(f"  +0x{off:X} -> {series}")

    # near-track: matches at least 4/len
    print("\n=== partial trackers (match >= len-1) ===")
    for off in base_snap:
        series = [s[2][off] for s in snaps]
        want = [s[0] for s in snaps]
        miss = sum(1 for a, b in zip(series, want) if a != b)
        if miss <= 1 and any(0 <= v <= 4 for v in series) and len(set(series)) > 1:
            if off not in track:
                print(f"  +0x{off:X} series={series} want={want} miss={miss}")

    p.k.CloseHandle(h)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
