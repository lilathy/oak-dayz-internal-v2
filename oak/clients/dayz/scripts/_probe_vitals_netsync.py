#!/usr/bin/env python3
"""External probe: local DayZPlayer + TransferValues/netsync candidates + server DS."""
import ctypes, struct, sys, time
from ctypes import wintypes

k = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)

PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400

k.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
k.OpenProcess.restype = wintypes.HANDLE
k.ReadProcessMemory.argtypes = [wintypes.HANDLE, wintypes.LPCVOID, wintypes.LPVOID, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k.ReadProcessMemory.restype = wintypes.BOOL
k.CloseHandle.argtypes = [wintypes.HANDLE]

class MODULEINFO(ctypes.Structure):
    _fields_ = [("lpBaseOfDll", wintypes.LPVOID), ("SizeOfImage", wintypes.DWORD), ("EntryPoint", wintypes.LPVOID)]

psapi.EnumProcessModulesEx.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.HMODULE), wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), wintypes.DWORD]
psapi.GetModuleInformation.argtypes = [wintypes.HANDLE, wintypes.HMODULE, ctypes.POINTER(MODULEINFO), wintypes.DWORD]
psapi.GetModuleBaseNameW.argtypes = [wintypes.HANDLE, wintypes.HMODULE, wintypes.LPWSTR, wintypes.DWORD]

def find_pid(name):
    import subprocess
    out = subprocess.check_output(["tasklist", "/FI", f"IMAGENAME eq {name}", "/FO", "CSV", "/NH"], text=True, errors="ignore")
    for line in out.splitlines():
        if not line.strip() or line.startswith("INFO:"):
            continue
        parts = [p.strip('"') for p in line.split(",")]
        if len(parts) >= 2 and parts[0].lower() == name.lower():
            return int(parts[1])
    return None

def open_proc(pid):
    return k.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)

def rpm(h, addr, n):
    buf = (ctypes.c_char * n)()
    got = ctypes.c_size_t()
    if not k.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)):
        return b""
    return bytes(buf[: got.value])

def u64(h, a):
    b = rpm(h, a, 8)
    return struct.unpack("<Q", b)[0] if len(b) == 8 else 0

def i32(h, a):
    b = rpm(h, a, 4)
    return struct.unpack("<i", b)[0] if len(b) == 4 else 0

def f32(h, a):
    b = rpm(h, a, 4)
    return struct.unpack("<f", b)[0] if len(b) == 4 else float("nan")

def valid(p):
    return 0x100000000 < p < 0x7FFFFFFFFFFF

def module_base(h, want):
    mods = (wintypes.HMODULE * 512)()
    needed = wintypes.DWORD()
    if not psapi.EnumProcessModulesEx(h, mods, ctypes.sizeof(mods), ctypes.byref(needed), 0x03):
        return 0, 0
    count = needed.value // ctypes.sizeof(wintypes.HMODULE)
    for i in range(count):
        name = ctypes.create_unicode_buffer(260)
        psapi.GetModuleBaseNameW(h, mods[i], name, 260)
        if name.value.lower() == want.lower():
            mi = MODULEINFO()
            psapi.GetModuleInformation(h, mods[i], ctypes.byref(mi), ctypes.sizeof(mi))
            return ctypes.cast(mi.lpBaseOfDll, ctypes.c_void_p).value or 0, mi.SizeOfImage
    return 0, 0

def rtti_name(h, obj, mod_base, mod_size):
    """MSVC RTTI: obj->vtable[-1] = RTTICompleteObjectLocator -> type desc name."""
    if not valid(obj):
        return ""
    vt = u64(h, obj)
    if not (mod_base <= vt < mod_base + mod_size):
        return ""
    col = u64(h, vt - 8)
    if not valid(col):
        return ""
    # COL: signature, offset, cdOffset, typeDescriptor (x64 often RVA-based)
    # Try absolute typeDescriptor at +0x10
    td = u64(h, col + 0x10)
    name_addr = 0
    if valid(td):
        name_addr = td + 0x10
    else:
        # RVA style: imageBase from COL+0x14, td RVA at +0x0C
        td_rva = i32(h, col + 0x0C)
        ib_rva = i32(h, col + 0x14)
        if ib_rva:
            ib = col - ib_rva  # rough
            # better: module base
            ib = mod_base
            td = ib + (td_rva & 0xFFFFFFFF)
            if valid(td):
                name_addr = td + 0x10
    if not name_addr:
        return ""
    raw = rpm(h, name_addr, 64)
    if not raw:
        return ""
    # decorated .?AVTransferValues@@ etc
    try:
        s = raw.split(b"\0", 1)[0].decode("ascii", "ignore")
    except Exception:
        return ""
    return s

WORLD_OFF = 0x4264058
LOCAL_PLAYER = 0x2960
NEAR_LIST = 0xF48
NEAR_SIZE = 0xF50  # often +8 from list; updater says FarTableSize pattern — try near+8
# From offsets: NearEntList 0xF48 — size commonly at 0xF50

def resolve_local(h, base):
    world = u64(h, base + WORLD_OFF)
    if not valid(world):
        return 0, 0, 0
    local = u64(h, world + LOCAL_PLAYER)
    near = u64(h, world + NEAR_LIST)
    nsz = i32(h, world + NEAR_LIST + 8)
    return world, local, near, nsz

def scan_player(h, base, msize, player, tag):
    print(f"\n=== {tag} player=0x{player:X} ===")
    if not valid(player):
        print("invalid")
        return
    ds188 = u64(h, player + 0x188)
    dm700 = u64(h, player + 0x700)
    print(f"ds188=0x{ds188:X} dm700=0x{dm700:X}")
    # pointer slot RTTI scan
    hits = []
    for off in range(0x80, 0xA00, 8):
        p = u64(h, player + off)
        if not valid(p):
            continue
        if base <= p < base + msize:
            continue
        name = rtti_name(h, p, base, msize)
        if not name:
            continue
        low = name.lower()
        if any(k in low for k in ("transfer", "damage", "health", "bleed", "shock", "stat", "injury")):
            hits.append((off, p, name))
    print(f"rtti interesting slots: {len(hits)}")
    for off, p, name in hits[:40]:
        print(f"  +0x{off:X} -> 0x{p:X} {name}")
        # dump floats 0..0x80
        floats = []
        for fo in range(0, 0x80, 4):
            v = f32(h, p + fo)
            if v == v and 0.0 <= v <= 1.05:
                floats.append((fo, v))
        if floats:
            print("    unit floats:", ", ".join(f"+{fo:X}={v:.3f}" for fo, v in floats[:12]))
    # netsync-ish ints on player itself
    small = []
    for off in range(0x700, 0xC00, 4):
        v = i32(h, player + off)
        if 0 <= v <= 4:
            small.append((off, v, "lvl?"))
        elif 0 <= v <= 63:
            small.append((off, v, "shock?"))
    print("small ints 0x700..0xC00 (0..63):", len(small))
    for off, v, kind in small[:30]:
        print(f"  +0x{off:X} = {v} ({kind})")
    # also 0x180..0x280
    small2 = []
    for off in range(0x180, 0x300, 4):
        v = i32(h, player + off)
        if 0 <= v <= 4:
            small2.append((off, v))
    print("tiny 0..4 in 0x180..0x300:", small2[:20])

def main():
    target = sys.argv[1] if len(sys.argv) > 1 else "DayZ_x64.exe"
    pid = find_pid(target)
    if not pid:
        print("no process", target)
        return 1
    h = open_proc(pid)
    if not h:
        print("OpenProcess failed", ctypes.get_last_error())
        return 1
    base, msize = module_base(h, target)
    print(f"{target} pid={pid} base=0x{base:X} size=0x{msize:X}")
    world, local, near, nsz = resolve_local(h, base)
    print(f"world=0x{world:X} local=0x{local:X} near=0x{near:X} nsz={nsz}")
    if valid(local):
        scan_player(h, base, msize, local, "LOCAL")
    # near entities
    if valid(near) and 0 < nsz < 4096:
        shown = 0
        for i in range(min(nsz, 256)):
            ent = u64(h, near + i * 8)
            if not valid(ent) or ent == local:
                continue
            # cheap type check via vtable in module
            vt = u64(h, ent)
            if not (base <= vt < base + msize):
                continue
            ds = u64(h, ent + 0x188)
            # print a few with ds or without
            if shown < 8:
                print(f"near[{i}]=0x{ent:X} ds188=0x{ds:X}")
                shown += 1
            # if looks like player-sized object with stats @6F0
            st = u64(h, ent + 0x6F0)
            if valid(st) and shown < 12:
                scan_player(h, base, msize, ent, f"NEAR[{i}]")
    k.CloseHandle(h)
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
