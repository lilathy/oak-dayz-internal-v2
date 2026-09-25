#!/usr/bin/env python3
"""Find InjuryAnimationHandler on local player and diff floats/ints while forcing injury level."""
import importlib.util, time, sys, struct
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

# InjuryAnimValues: PRISTINE=0 WORN=0.3 DAMAGED=0.6 BADLY=0.8 RUINED=1.0
ANIM = {0: 0.0, 1: 0.3, 2: 0.6, 3: 0.8, 4: 1.0}


def set_hl(level: int):
    FORCE.write_text(str(level) + "\n", encoding="ascii")
    time.sleep(4.2)
    txt = VITALS.read_text(encoding="utf-8", errors="ignore").strip() if VITALS.exists() else ""
    print("vitals:", txt)
    return txt


def find_injury_handlers(h, local, base, msize):
    hits = []
    for off in range(0x8, 0x1200, 8):
        ptr = p.u64(h, local + off)
        if not p.valid(ptr):
            continue
        if base <= ptr < base + msize:
            continue
        name = p.rtti_name(h, ptr, base, msize) or ""
        if "Injury" in name or "injury" in name:
            hits.append((off, ptr, name))
        # also 1-hop
        for off2 in range(0x0, 0x40, 8):
            ptr2 = p.u64(h, ptr + off2)
            if not p.valid(ptr2) or base <= ptr2 < base + msize:
                continue
            name2 = p.rtti_name(h, ptr2, base, msize) or ""
            if "Injury" in name2:
                hits.append((off, ptr2, f"via+0x{off:X}->{name2}"))
    return hits


def snap_floats(h, addr, size=0x100):
    out = {}
    for off in range(0, size, 4):
        out[off] = p.f32(h, addr + off)
    return out


def snap_ints(h, addr, size=0x100):
    out = {}
    for off in range(0, size, 4):
        out[off] = p.i32(h, addr + off)
    return out


def near_eq(a, b, eps=0.02):
    return a == a and b == b and abs(a - b) <= eps


def main():
    levels = [0, 1, 2, 3, 4, 2, 0]
    if len(sys.argv) > 1:
        levels = [int(x) for x in sys.argv[1:]]

    pid = p.find_pid("DayZ_x64.exe")
    if not pid:
        print("no DayZ")
        return 1
    h = p.open_proc(pid)
    base, msize = p.module_base(h, "DayZ_x64.exe")
    world, local, near, nsz = p.resolve_local(h, base)
    print(f"local={local:#x} nsz={nsz}")

    # Also try near[0] DayZPlayer twin
    roots = [("local", local)]
    if p.valid(near) and nsz > 0:
        e0 = p.u64(h, near)
        if p.valid(e0) and e0 != local:
            roots.append(("near0", e0))

    for rlab, root in roots:
        hits = find_injury_handlers(h, root, base, msize)
        print(f"\n{rlab} injury RTTI hits: {len(hits)}")
        for off, ptr, name in hits[:12]:
            print(f"  +0x{off:X} -> {ptr:#x} {name}")

    # Broad: any heap child with a float in {0,.3,.6,.8,1} that will track
    # Collect candidate (root, off_to_obj, obj) for objects containing anim-like floats
    handlers = []
    for rlab, root in roots:
        for off in range(0x8, 0x1000, 8):
            ptr = p.u64(h, root + off)
            if not p.valid(ptr) or base <= ptr < base + msize:
                continue
            # small object scan
            for fo in range(0, 0x80, 4):
                fv = p.f32(h, ptr + fo)
                if any(near_eq(fv, a) for a in ANIM.values()):
                    handlers.append((rlab, off, ptr))
                    break
    # unique ptrs
    seen = set()
    uniq = []
    for item in handlers:
        if item[2] in seen:
            continue
        seen.add(item[2])
        uniq.append(item)
    print(f"\ncandidate handler objs: {len(uniq)}")

    snaps = {ptr: [] for _, _, ptr in uniq[:40]}
    meta = {ptr: (rlab, off) for rlab, off, ptr in uniq[:40]}

    for lv in levels:
        print(f"\n=== force hl={lv} anim={ANIM[lv]} ===")
        set_hl(lv)
        world, local, near, nsz = p.resolve_local(h, base)
        for ptr in list(snaps.keys()):
            snaps[ptr].append((lv, snap_floats(h, ptr, 0xC0), snap_ints(h, ptr, 0xC0)))

    print("\n========== FLOAT TRACKERS (anim values) ==========")
    for ptr, series_list in snaps.items():
        base_f = series_list[0][1]
        for off in base_f:
            series = [s[1][off] for s in series_list]
            want = [ANIM[s[0]] for s in series_list]
            if all(near_eq(a, b) for a, b in zip(series, want)) and len(set(round(x, 2) for x in series)) > 1:
                rlab, poff = meta[ptr]
                print(f"{rlab}+0x{poff:X} obj={ptr:#x} float+0x{off:X} -> {[round(x,2) for x in series]}")

    print("\n========== INT TRACKERS (0..4) ==========")
    for ptr, series_list in snaps.items():
        base_i = series_list[0][2]
        for off in base_i:
            series = [s[2][off] for s in series_list]
            want = [s[0] for s in series_list]
            if series == want and len(set(series)) > 1:
                rlab, poff = meta[ptr]
                print(f"{rlab}+0x{poff:X} obj={ptr:#x} int+0x{off:X} -> {series}")

    # Also diff root player ints again (maybe juncture writes m_HealthLevel on owner now)
    print("\n========== ROOT INT TRACKERS ==========")
    root_snaps = {lab: [] for lab, _ in roots}
    # re-run short cycle on roots only
    for lv in levels:
        set_hl(lv)
        world, local, near, nsz = p.resolve_local(h, base)
        mapping = {"local": local}
        if p.valid(near) and nsz > 0:
            mapping["near0"] = p.u64(h, near)
        for lab in root_snaps:
            addr = mapping.get(lab)
            if p.valid(addr):
                root_snaps[lab].append((lv, {o: p.i32(h, addr + o) for o in range(0x100, 0x1800, 4)}))

    for lab, series_list in root_snaps.items():
        if len(series_list) < 3:
            continue
        base_snap = series_list[0][1]
        for off in base_snap:
            series = [s[1][off] for s in series_list]
            want = [s[0] for s in series_list]
            if series == want and len(set(series)) > 1:
                print(f"{lab}+0x{off:X} -> {series}")

    p.k.CloseHandle(h)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
