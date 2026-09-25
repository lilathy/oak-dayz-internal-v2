#!/usr/bin/env python3
"""BFS-diff client DayZPlayer (+1-hop heap) for m_HealthLevel while server forces it."""
import importlib.util, time, sys
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


def set_hl(level: int):
    FORCE.write_text(str(level) + "\n", encoding="ascii")
    time.sleep(4.0)
    txt = VITALS.read_text(encoding="utf-8", errors="ignore").strip() if VITALS.exists() else ""
    print("vitals:", txt)
    # parse reported hl
    hl = None
    if "healthLevel=" in txt:
        try:
            hl = int(txt.split("healthLevel=")[1].split()[0])
        except Exception:
            pass
    return hl


def collect_regions(h, local, base, msize):
    """(label, base_addr) regions to scan for ints."""
    regs = [("self", local)]
    for off in range(0x8, 0xA00, 8):
        ptr = p.u64(h, local + off)
        if not p.valid(ptr):
            continue
        # skip module
        if base <= ptr < base + msize:
            continue
        name = p.rtti_name(h, ptr, base, msize) or ""
        # skip noisy known non-script blobs
        skip = ("WeakPtrTracker", "DayZPlayerClass", "DayZInfectedClass", "DayZAnimalClass")
        if any(s in name for s in skip):
            continue
        regs.append((f"+0x{off:X}:{name[:40]}", ptr))
    return regs


def snap_region(h, addr, size=0x800):
    out = {}
    for off in range(0, size, 4):
        out[off] = p.i32(h, addr + off)
    return out


def main():
    levels = [0, 2, 4, 1, 3, 0, 4]
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
    if not p.valid(local):
        return 1

    # Also include other near entities (remote/AI players)
    if p.valid(near) and 0 < nsz < 128:
        for i in range(nsz):
            ent = p.u64(h, near + i * 8)
            if p.valid(ent) and ent != local:
                rtti = p.rtti_name(h, ent, base, msize)
                print(f"near[{i}]={ent:#x} {rtti}")
                # will add as root if DayZPlayer
                if rtti and "DayZPlayer" in rtti:
                    # treat as extra root via collecting from it each snap
                    pass

    regs0 = collect_regions(h, local, base, msize)
    print(f"regions: {len(regs0)}")
    for lab, addr in regs0[:25]:
        print(f"  {lab} @ {addr:#x}")

    # snaps[lab] = list of (want_lv, server_hl, snapdict)
    snaps = {lab: [] for lab, _ in regs0}
    # also snap other DayZPlayers in near
    extra_roots = []
    if p.valid(near) and 0 < nsz < 128:
        for i in range(min(nsz, 32)):
            ent = p.u64(h, near + i * 8)
            if p.valid(ent) and ent != local:
                rtti = p.rtti_name(h, ent, base, msize) or ""
                if "DayZPlayer" in rtti or "Player" in rtti:
                    extra_roots.append((f"nearPlayer[{i}]", ent))
                    snaps[f"nearPlayer[{i}]"] = []

    for lv in levels:
        print(f"\n=== force hl={lv} ===")
        srv_hl = set_hl(lv)
        # refresh local
        world, local, near, nsz = p.resolve_local(h, base)
        regs = collect_regions(h, local, base, msize)
        # map by label
        bylab = {lab: addr for lab, addr in regs}
        for lab in list(snaps.keys()):
            if lab.startswith("nearPlayer"):
                continue
            addr = bylab.get(lab)
            if not addr:
                # try self
                if lab == "self":
                    addr = local
                else:
                    continue
            snaps[lab].append((lv, srv_hl, snap_region(h, addr)))
        # extras
        if p.valid(near) and 0 < nsz < 128:
            for i in range(min(nsz, 32)):
                lab = f"nearPlayer[{i}]"
                if lab not in snaps:
                    continue
                ent = p.u64(h, near + i * 8)
                if p.valid(ent):
                    snaps[lab].append((lv, srv_hl, snap_region(h, ent)))

    print("\n========== RESULTS ==========")
    for lab, series_list in snaps.items():
        if len(series_list) < 3:
            continue
        base_snap = series_list[0][2]
        exact = []
        for off in base_snap:
            series = [s[2][off] for s in series_list]
            want = [s[0] for s in series_list]
            # also try matching server-reported hl (print-before-force lag: use previous?)
            if series == want:
                exact.append((off, series, "want"))
            srv = [s[1] if s[1] is not None else -999 for s in series_list]
            if series == srv and len(set(series)) > 1:
                exact.append((off, series, "server_hl"))
        if exact:
            print(f"\n{lab}: {len(exact)} trackers")
            for off, series, kind in exact[:20]:
                print(f"  +0x{off:X} ({kind}) -> {series}")

    p.k.CloseHandle(h)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
