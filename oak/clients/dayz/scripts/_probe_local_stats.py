#!/usr/bin/env python3
"""Dump local DayZPlayer RTTI + hunt PlayerStats / StaminaHandler / Energy/Water."""
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
LOCAL_PLAYER = 0x2960

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
    td = base + td_rva
    raw = rpm(h, td + 0x10, 96)
    if not raw or raw[0:1] not in (b".", b"?"):
        return ""
    s = raw.split(b"\0", 1)[0].decode("ascii", "ignore")
    if s.startswith(".?AV") or s.startswith(".?AU"):
        s = s[4:]
    return s.split("@", 1)[0]

def cstr(h, p, n=48):
    if not valid(p) and not (0x10000 < p < 0x7FFFFFFFFFFF):
        return ""
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

def looks_stat_label(s):
    if not s:
        return False
    sl = s.lower()
    return sl in ("energy", "water", "stamina", "heatcomfort", "diet", "tremor",
                  "toxicity", "wet", "heatbuffer", "specialty", "bloodtype",
                  "health", "blood", "shock", "globalhealth")

def dump_floats(h, obj, lim=0x80):
    hits = []
    for fo in range(0, lim, 4):
        v = f32(h, obj + fo)
        if v != v:
            continue
        if 0.0 <= v <= 100.01 or 100.0 < v <= 20000.0:
            hits.append((fo, v))
    return hits

def scan_obj_labels(h, obj, base, msize, depth=0):
    """Look for Energy/Water/Stamina strings hanging off an object."""
    found = []
    if not valid(obj) or depth > 2:
        return found
    for off in range(0, 0x80, 8):
        p = u64(h, obj + off)
        if not p:
            continue
        s = cstr(h, p, 24)
        if looks_stat_label(s):
            found.append((off, p, s, "direct"))
        if valid(p) and not (base <= p < base + msize):
            for io in range(0, 0x28, 8):
                q = u64(h, p + io)
                s2 = cstr(h, q, 24)
                if looks_stat_label(s2):
                    found.append((off, q, s2, f"via+{io:X}"))
    return found

def main():
    pid = pid_of("DayZ_x64.exe")
    h = k.OpenProcess(0x0410, False, pid)
    base, msize = module_base(h, "DayZ_x64.exe")
    world = u64(h, base + WORLD_OFF)
    local = u64(h, world + LOCAL_PLAYER)
    print(f"pid={pid} base=0x{base:X} world=0x{world:X} local=0x{local:X}")
    if not valid(local):
        print("no local")
        return 1

    print(f"\nds188=0x{u64(h, local+0x188):X} stats6F0=0x{u64(h, local+0x6F0):X} "
          f"ua6F8=0x{u64(h, local+0x6F8):X} stam6A4={f32(h, local+0x6A4):.3f} "
          f"net={i32(h, local+0x6E4)}")

    print("\n==== ALL RTTI on local+0x40..0xC00 ====")
    named = []
    for off in range(0x40, 0xC08, 8):
        p = u64(h, local + off)
        if not valid(p) or (base <= p < base + msize):
            continue
        name = rtti_name(h, p, base, msize)
        if not name:
            continue
        named.append((off, p, name))
        print(f"  +0x{off:X} {p:016X} {name}")

    keys = ("stat", "stamina", "damage", "health", "shock", "bleed", "hunger",
            "thirst", "energy", "water", "playerstat", "handler", "modifier",
            "notifiers", "transfer", "symptom")
    print("\n==== INTERESTING + 1-level children ====")
    for off, p, name in named:
        low = name.lower()
        if not any(k in low for k in keys):
            continue
        print(f"\n-- local+0x{off:X} {name} @ {p:X} --")
        fl = dump_floats(h, p, 0xC0)
        if fl:
            print("  floats:", ", ".join(f"+{fo:X}={v:.3f}" for fo, v in fl[:20]))
        labs = scan_obj_labels(h, p, base, msize)
        for lo, lp, s, how in labs:
            print(f"  label +0x{lo:X} {s!r} ({how})")
        # child pointers with RTTI
        for co in range(0, 0x100, 8):
            c = u64(h, p + co)
            if not valid(c) or (base <= c < base + msize) or c == p:
                continue
            cn = rtti_name(h, c, base, msize)
            if not cn:
                continue
            print(f"  child+0x{co:X} {c:X} {cn}")
            cl = scan_obj_labels(h, c, base, msize)
            for lo, lp, s, how in cl:
                print(f"    label +0x{lo:X} {s!r} ({how})")
            # array-of-ptrs pattern (PlayerStats m_PlayerStats)
            arr = c
            cnt = i32(h, p + co + 8)
            if 1 <= cnt <= 32:
                print(f"    maybe array count={cnt}")
                for i in range(min(cnt, 16)):
                    rec = u64(h, arr + i * 8)
                    if not valid(rec):
                        continue
                    rn = rtti_name(h, rec, base, msize)
                    labs2 = scan_obj_labels(h, rec, base, msize)
                    val = f32(h, rec + 0x2C)
                    print(f"      [{i}] {rec:X} {rn or '?'} val@2C={val:.3f} labels={labs2}")

    print("\n==== BFS labels under every named child (depth 1) ====")
    for off, p, name in named:
        labs = scan_obj_labels(h, p, base, msize)
        if labs:
            print(f"  local+0x{off:X} {name}: {labs}")

    # Script objects often lack native RTTI — brute label scan all heap ptrs
    print("\n==== brute Energy/Water/Stamina string refs on local slots ====")
    for off in range(0x40, 0xC08, 8):
        p = u64(h, local + off)
        if not valid(p) or (base <= p < base + msize):
            continue
        labs = scan_obj_labels(h, p, base, msize)
        if labs:
            print(f"  +0x{off:X} {p:X} {rtti_name(h, p, base, msize) or '?'} {labs}")
        # walk array at p if count nearby
        for cnt_off in (0x8, 0x10, 0x18, 0x20):
            cnt = i32(h, p + cnt_off)
            if not (2 <= cnt <= 24):
                continue
            arr = u64(h, p) if cnt_off != 0 else p
            if not valid(arr):
                arr = p
            hits = []
            for i in range(cnt):
                rec = u64(h, arr + i * 8)
                if not valid(rec):
                    continue
                labs2 = scan_obj_labels(h, rec, base, msize)
                if labs2:
                    hits.append((i, rec, f32(h, rec + 0x2C), labs2))
            if hits:
                print(f"  ARRAY local+0x{off:X} cnt@{cnt_off:X}={cnt} hits={hits}")

    return 0

if __name__ == "__main__":
    raise SystemExit(main())
