#!/usr/bin/env python3
"""Find real DayZPlayer in near/far/slow lists and hunt PlayerStats."""
import ctypes, struct, subprocess
from ctypes import wintypes

k = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
k.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
k.OpenProcess.restype = wintypes.HANDLE
k.ReadProcessMemory.argtypes = [wintypes.HANDLE, wintypes.LPCVOID, wintypes.LPVOID, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k.ReadProcessMemory.restype = wintypes.BOOL

class MODULEINFO(ctypes.Structure):
    _fields_ = [("lpBaseOfDll", wintypes.LPVOID), ("SizeOfImage", wintypes.DWORD), ("EntryPoint", wintypes.LPVOID)]

psapi.EnumProcessModulesEx.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.HMODULE), wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), wintypes.DWORD]
psapi.GetModuleInformation.argtypes = [wintypes.HANDLE, wintypes.HMODULE, ctypes.POINTER(MODULEINFO), wintypes.DWORD]
psapi.GetModuleBaseNameW.argtypes = [wintypes.HANDLE, wintypes.HMODULE, wintypes.LPWSTR, wintypes.DWORD]

WORLD_OFF = 0x4264058
LOCAL_OFF = 0x2960
CAM_OFF = 0x1B8
NEAR = 0xF48
FAR = 0x1090
SLOW = 0x2010
ITEM = 0x2060
VS = 0x1C8
TYPE = 0x180
CFG = 0xD0

def pid_of(name):
    out = subprocess.check_output(["tasklist", "/FI", f"IMAGENAME eq {name}", "/FO", "CSV", "/NH"], text=True, errors="ignore")
    for line in out.splitlines():
        if name.lower() not in line.lower():
            continue
        return int([p.strip('"') for p in line.split(",")][1])
    raise SystemExit("DayZ not running")

def rpm(h, addr, n):
    buf = (ctypes.c_char * n)()
    got = ctypes.c_size_t()
    if not k.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)):
        return b""
    return bytes(buf[:got.value])

def u64(h, a):
    b = rpm(h, a, 8)
    return struct.unpack("<Q", b)[0] if len(b) == 8 else 0

def u16(h, a):
    b = rpm(h, a, 2)
    return struct.unpack("<H", b)[0] if len(b) == 2 else 0

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
    psapi.EnumProcessModulesEx(h, mods, ctypes.sizeof(mods), ctypes.byref(needed), 0x03)
    count = needed.value // ctypes.sizeof(wintypes.HMODULE)
    for i in range(count):
        name = ctypes.create_unicode_buffer(260)
        psapi.GetModuleBaseNameW(h, mods[i], name, 260)
        if name.value.lower() == want.lower():
            mi = MODULEINFO()
            psapi.GetModuleInformation(h, mods[i], ctypes.byref(mi), ctypes.sizeof(mi))
            return ctypes.cast(mi.lpBaseOfDll, ctypes.c_void_p).value or 0, mi.SizeOfImage
    return 0, 0

def engstr(h, obj):
    if not valid(obj):
        return ""
    ln = u16(h, obj + 8)
    if not (1 <= ln <= 80):
        return ""
    raw = rpm(h, obj + 0x10, ln)
    try:
        s = raw.decode("ascii", "ignore")
    except Exception:
        return ""
    if not all(32 <= ord(c) <= 126 for c in s):
        return ""
    return s

def cstr(h, p, n=40):
    raw = rpm(h, p, n)
    if not raw:
        return ""
    out = []
    for b in raw:
        if b == 0:
            break
        if b < 32 or b > 126:
            return ""
        out.append(chr(b))
    return "".join(out)

def rtti_name(h, obj, base, msize):
    if not valid(obj):
        return ""
    vt = u64(h, obj)
    if not (base <= vt < base + msize):
        return ""
    col = u64(h, vt - 8)
    if not valid(col):
        return ""
    td_rva = i32(h, col + 0x0C) & 0xFFFFFFFF
    raw = rpm(h, base + td_rva + 0x10, 96)
    if not raw or raw[:1] not in (b".", b"?"):
        return ""
    s = raw.split(b"\0", 1)[0].decode("ascii", "ignore")
    if s.startswith(".?AV") or s.startswith(".?AU"):
        s = s[4:]
    return s.split("@", 1)[0]

def cfg_name(h, ent):
    typ = u64(h, ent + TYPE)
    if not valid(typ):
        return ""
    cfg = u64(h, typ + CFG)
    return engstr(h, cfg)

def list_ents(h, world, off, cap):
    head = u64(h, world + off)
    cnt = i32(h, world + off + 8)
    if not valid(head) or not (0 < cnt < cap):
        return []
    out = []
    for i in range(cnt):
        e = u64(h, head + i * 8)
        if valid(e):
            out.append(e)
    return out

STAT_NAMES = {"Energy", "Water", "Stamina", "HeatComfort", "Diet", "Tremor",
              "Toxicity", "Wet", "HeatBuffer", "Specialty", "BloodType",
              "Health", "Blood", "Shock", "GlobalHealth"}

def scan_labels(h, obj, base, msize):
    found = []
    if not valid(obj):
        return found
    for off in range(0, 0x90, 8):
        p = u64(h, obj + off)
        if not p:
            continue
        s = cstr(h, p, 24)
        if s in STAT_NAMES:
            found.append((off, s, f32(h, obj + 0x2C)))
        if valid(p) and not (base <= p < base + msize):
            s2 = engstr(h, p)
            if s2 in STAT_NAMES:
                found.append((off, s2, f32(h, obj + 0x2C)))
            for io in (0, 8, 0x10, 0x18):
                q = u64(h, p + io)
                s3 = cstr(h, q, 24)
                if s3 in STAT_NAMES:
                    found.append((off, s3, f32(h, obj + 0x2C)))
                s4 = engstr(h, q)
                if s4 in STAT_NAMES:
                    found.append((off, s4, f32(h, obj + 0x2C)))
    return found

def dump_player(h, ent, base, msize, tag):
    cfg = cfg_name(h, ent)
    vs = u64(h, ent + VS)
    pos = (f32(h, vs + 0x2C), f32(h, vs + 0x30), f32(h, vs + 0x34)) if valid(vs) else (0, 0, 0)
    net = i32(h, ent + 0x6E4)
    net2 = i32(h, ent + 0x6DC)
    print(f"\n######## {tag} ent=0x{ent:X} cfg={cfg!r} net6E4={net} net6DC={net2} pos=({pos[0]:.1f},{pos[1]:.1f},{pos[2]:.1f})")
    print(f"  +0x188=0x{u64(h, ent+0x188):X} rtti={rtti_name(h, u64(h, ent+0x188), base, msize)}")
    print(f"  +0x6F0=0x{u64(h, ent+0x6F0):X} rtti={rtti_name(h, u64(h, ent+0x6F0), base, msize)}")
    print(f"  +0x6A4 stam={f32(h, ent+0x6A4):.3f}")

    print("  RTTI slots:")
    for off in range(0x40, 0xC08, 8):
        p = u64(h, ent + off)
        if not valid(p) or (base <= p < base + msize):
            continue
        name = rtti_name(h, p, base, msize)
        if not name:
            continue
        print(f"    +0x{off:X} {name} @ {p:X}")

    print("  label brute:")
    for off in range(0x40, 0xC08, 8):
        p = u64(h, ent + off)
        if not valid(p) or (base <= p < base + msize):
            continue
        labs = scan_labels(h, p, base, msize)
        if labs:
            print(f"    +0x{off:X} {rtti_name(h, p, base, msize) or '?'} {labs}")
        # try as stats container: pointer table
        for aoff in (0x0, 0x8, 0x10, 0x18, 0x20, 0x28):
            arr = u64(h, p + aoff)
            cnt = i32(h, p + aoff + 8)
            if not valid(arr) or not (3 <= cnt <= 24):
                continue
            hits = []
            for i in range(cnt):
                rec = u64(h, arr + i * 8)
                if not valid(rec):
                    continue
                labs2 = scan_labels(h, rec, base, msize)
                if labs2:
                    hits.append((i, rec, labs2, f32(h, rec + 0x2C)))
            if hits:
                print(f"    ARRAY +0x{off:X}+0x{aoff:X} cnt={cnt} {hits}")

def main():
    pid = pid_of("DayZ_x64.exe")
    h = k.OpenProcess(0x0410, False, pid)
    base, msize = module_base(h, "DayZ_x64.exe")
    world = u64(h, base + WORLD_OFF)
    stub = u64(h, world + LOCAL_OFF)
    cam = u64(h, world + CAM_OFF)
    print(f"pid={pid} world=0x{world:X} stub=0x{stub:X} stubCfg={cfg_name(h, stub)!r} cam=0x{cam:X}")

    lists = [
        ("near", list_ents(h, world, NEAR, 4000)),
        ("far", list_ents(h, world, FAR, 8000)),
        ("slow", list_ents(h, world, SLOW, 8000)),
        ("item", list_ents(h, world, ITEM, 8000)),
    ]
    players = []
    for tag, ents in lists:
        cfgs = {}
        for e in ents:
            c = cfg_name(h, e)
            if not c:
                continue
            cfgs[c] = cfgs.get(c, 0) + 1
            cl = c.lower()
            if cl == "dayzplayer" or "survivor" in cl or cl == "player":
                players.append((tag, e, c))
        print(f"{tag}: n={len(ents)} types={sorted(cfgs.items(), key=lambda x: -x[1])[:12]}")

    print(f"\nplayers found: {len(players)}")
    seen = set()
    for tag, e, c in players:
        if e in seen:
            continue
        seen.add(e)
        dump_player(h, e, base, msize, f"{tag}")

    if not players:
        print("\nNo dayzplayer in lists — dumping first 8 near ents with any cfg")
        near = lists[0][1]
        shown = 0
        for e in near:
            c = cfg_name(h, e)
            if not c:
                continue
            print(f"  near {e:X} cfg={c!r} typeRtti={rtti_name(h, u64(h, e+TYPE), base, msize)}")
            shown += 1
            if shown >= 12:
                break
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
