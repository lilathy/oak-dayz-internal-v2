#!/usr/bin/env python3
"""Find server PlayerBase via known HP float, then diff ints while forcing m_HealthLevel."""
import importlib.util, struct, time, sys, ctypes
from ctypes import wintypes
from pathlib import Path
import re

spec = importlib.util.spec_from_file_location(
    "p",
    r"..\..\..\clients\dayz\scripts\_probe_vitals_netsync.py",
)
p = importlib.util.module_from_spec(spec)
spec.loader.exec_module(p)

PROFILES = Path(r"C:\Program Files (x86)\Steam\steamapps\common\DayZServer\oak_profiles")
FORCE = PROFILES / "oak_force_hl.txt"
VITALS = PROFILES / "oak_vitals.txt"

MEM_COMMIT = 0x1000
MEM_PRIVATE = 0x20000
PAGE_READWRITE = 0x04
PAGE_EXECUTE_READWRITE = 0x40
PAGE_READONLY = 0x02
PAGE_READWRITE_FLAGS = {0x02, 0x04, 0x08, 0x40, 0x80, 0x10, 0x20}


class MEMORY_BASIC_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("BaseAddress", ctypes.c_void_p),
        ("AllocationBase", ctypes.c_void_p),
        ("AllocationProtect", wintypes.DWORD),
        ("RegionSize", ctypes.c_size_t),
        ("State", wintypes.DWORD),
        ("Protect", wintypes.DWORD),
        ("Type", wintypes.DWORD),
    ]


k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.VirtualQueryEx.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.POINTER(MEMORY_BASIC_INFORMATION), ctypes.c_size_t]
k32.VirtualQueryEx.restype = ctypes.c_size_t


def parse_vitals():
    txt = VITALS.read_text(encoding="utf-8", errors="ignore") if VITALS.exists() else ""
    m = re.search(
        r"player=(\S+) hp=([0-9.]+) blood=([0-9.]+) shock=([0-9.]+) hp01=([0-9.]+) healthLevel=(\d+)",
        txt,
    )
    if not m:
        return None
    return {
        "name": m.group(1),
        "hp": float(m.group(2)),
        "blood": float(m.group(3)),
        "shock": float(m.group(4)),
        "hp01": float(m.group(5)),
        "hl": int(m.group(6)),
        "raw": txt.strip(),
    }


def iter_regions(h, max_addr=0x7FFFFFFFFFFF):
    addr = 0
    mbi = MEMORY_BASIC_INFORMATION()
    while addr < max_addr:
        got = k32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi))
        if not got:
            break
        base = mbi.BaseAddress or 0
        size = mbi.RegionSize or 0
        if mbi.State == MEM_COMMIT and (mbi.Protect & 0xFF) in PAGE_READWRITE_FLAGS and size > 0:
            # skip huge
            if size <= 64 * 1024 * 1024:
                yield base, size
        nxt = base + size
        if nxt <= addr:
            break
        addr = nxt


def find_float_addrs(h, target, tol=1e-3, limit=40):
    """Scan committed RW memory for float approx target. Returns list of addresses."""
    needle = struct.pack("<f", target)
    # Also search exact bit pattern
    hits = []
    scanned = 0
    for base, size in iter_regions(h):
        # read in chunks
        off = 0
        while off < size and len(hits) < limit:
            chunk = min(0x100000, size - off)
            buf = p.rpm(h, base + off, chunk)
            scanned += len(buf)
            if len(buf) < 4:
                break
            # exact bits first
            idx = 0
            while len(hits) < limit:
                i = buf.find(needle, idx)
                if i < 0:
                    break
                if i % 4 == 0:
                    hits.append(base + off + i)
                idx = i + 4
            off += chunk
        if len(hits) >= limit:
            break
    print(f"scanned~{scanned/1e6:.1f}MB hits={len(hits)} for float {target}")
    return hits


def score_player_candidate(h, base_mod, hp_addr):
    """Given address of an HP float inside DS or entity, walk back to find DayZPlayer-like object."""
    # Try: hp is at dmg+0x10 (common) → dmg ptr stored in player at +0x188
    cands = []
    # Assume hp_addr is inside some object; check nearby for blood=5000
    for back in range(0, 0x80, 4):
        obj = hp_addr - back
        # look for blood 5000 within +0..0x40 of hp
        blood_near = False
        for fo in range(0, 0x40, 4):
            fv = p.f32(h, obj + fo)
            if abs(fv - 5000.0) < 0.1:
                blood_near = True
                break
        if blood_near:
            cands.append(("zoneish", obj))
    return cands


def find_player_via_hp(h, base_mod, hp):
    hits = find_float_addrs(h, hp, limit=60)
    players = []
    for ha in hits:
        # Check if this looks like DS: f10~hp, and pointer from somewhere
        # Search for pointer value to (ha-0x10) as DS object start if hp at +0x10
        for ds_off in (0x10, 0x0, 0x14, 0x18, 0x20):
            ds = ha - ds_off
            if ds < 0x10000:
                continue
            # scan a limited set of regions for pointer to ds — too slow.
            # Instead: if blood 5000 nearby, treat as zone holder; then find who points to it later
            for fo in range(0, 0x60, 4):
                if abs(p.f32(h, ds + fo) - 5000.0) < 0.5:
                    players.append(ds)
                    break
    # unique
    uniq = sorted(set(players))
    print(f"zone-holder candidates: {len(uniq)}")
    return uniq[:20], hits[:20]


def set_hl(level):
    FORCE.write_text(str(level) + "\n", encoding="ascii")
    time.sleep(3.8)
    v = parse_vitals()
    print("vitals:", v["raw"] if v else None)
    return v


def snap_ints(h, addr, lo=0, hi=0x1000):
    return {off: p.i32(h, addr + off) for off in range(lo, hi, 4)}


def main():
    pid = p.find_pid("DayZServer_x64.exe")
    if not pid:
        print("no server")
        return 1
    # need VM_READ already; VirtualQueryEx needs PROCESS_QUERY_INFORMATION
    h = p.open_proc(pid)
    base, msize = p.module_base(h, "DayZServer_x64.exe")
    print(f"server {base:#x}")

    v0 = parse_vitals()
    if not v0:
        print("no vitals yet — is client connected?")
        return 1
    print("start", v0)

    zones, hp_hits = find_player_via_hp(h, base, v0["hp"])
    print("hp_hits sample", [hex(x) for x in hp_hits[:8]])
    print("zones sample", [hex(x) for x in zones[:8]])

    # Also: for each hp hit, treat (hit-0x10) as DS and search client-style player by
    # scanning for ptr == ds at known slots is hard. Diff ALL zone objects for int trackers.
    targets = []
    for z in zones[:12]:
        targets.append(("zone", z))
    # Also include ± windows around hp hits as possible script objects containing both
    for ha in hp_hits[:8]:
        for delta in (0, -0x100, -0x200, -0x400, -0x800):
            targets.append((f"hp{delta}", ha + delta))

    # dedupe addrs
    seen = set()
    uniq_targets = []
    for lab, addr in targets:
        if addr in seen or addr < 0x10000:
            continue
        seen.add(addr)
        uniq_targets.append((lab, addr))
    print(f"diff targets: {len(uniq_targets)}")

    levels = [0, 2, 4, 1, 3, 0, 4]
    if len(sys.argv) > 1:
        levels = [int(x) for x in sys.argv[1:]]

    snaps = {addr: [] for _, addr in uniq_targets}
    labels = {addr: lab for lab, addr in uniq_targets}

    for lv in levels:
        print(f"\n=== force {lv} ===")
        v = set_hl(lv)
        srv_hl = v["hl"] if v else None
        for addr in snaps:
            snaps[addr].append((lv, srv_hl, snap_ints(h, addr, 0, 0xC00)))

    print("\n========== TRACKERS ==========")
    for addr, series_list in snaps.items():
        base_snap = series_list[0][2]
        for off in base_snap:
            series = [s[2][off] for s in series_list]
            want = [s[0] for s in series_list]
            if series == want and len(set(series)) > 1:
                print(f"{labels[addr]}@{addr:#x} +0x{off:X} EXACT {series}")
            srv = [s[1] if s[1] is not None else -1 for s in series_list]
            if series == srv and len(set([x for x in series if x is not None])) > 1:
                print(f"{labels[addr]}@{addr:#x} +0x{off:X} SERVER {series}")

    p.k.CloseHandle(h)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
