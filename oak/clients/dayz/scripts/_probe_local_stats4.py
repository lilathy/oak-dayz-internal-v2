#!/usr/bin/env python3
"""Scan real DayZPlayer for hl=4 / shock=100 / blood=5000 / energy-water-stam."""
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

def pid_of(name):
    out = subprocess.check_output(["tasklist", "/FI", f"IMAGENAME eq {name}", "/FO", "CSV", "/NH"], text=True, errors="ignore")
    for line in out.splitlines():
        if name.lower() in line.lower():
            return int([p.strip('"') for p in line.split(",")][1])
    raise SystemExit("not running")

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

def rtti(h, obj, base, msize):
    if not valid(obj):
        return ""
    vt = u64(h, obj)
    if not (base <= vt < base + msize):
        return ""
    col = u64(h, vt - 8)
    if not valid(col):
        return ""
    td = base + (i32(h, col + 0x0C) & 0xFFFFFFFF)
    raw = rpm(h, td + 0x10, 80)
    if not raw:
        return ""
    s = raw.split(b"\x00", 1)[0].decode("ascii", "ignore")
    if s.startswith(".?AV") or s.startswith(".?AU"):
        s = s[4:]
    return s.split("@", 1)[0]

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

def scan_range(h, obj, lo, hi, tag):
    print(f"\n==== {tag} {obj:X} +{lo:X}..+{hi:X} ====")
    for off in range(lo, hi, 4):
        iv = i32(h, obj + off)
        fv = f32(h, obj + off)
        if iv == 4:
            print(f"  i32 +0x{off:X} = 4  (hl?)")
        if iv == 100:
            print(f"  i32 +0x{off:X} = 100 (shockS?)")
        if fv == fv and abs(fv - 100.0) < 0.01:
            print(f"  f32 +0x{off:X} = {fv:.3f} (shock?)")
        if fv == fv and abs(fv - 5000.0) < 0.5:
            print(f"  f32 +0x{off:X} = {fv:.3f} (blood/energy max?)")
        if fv == fv and abs(fv - 14.6973) < 0.05:
            print(f"  f32 +0x{off:X} = {fv:.4f} (hp abs?)")
        if fv == fv and abs(fv - 0.146973) < 0.002:
            print(f"  f32 +0x{off:X} = {fv:.6f} (hp01?)")
        if fv == fv and 200 <= fv <= 20000 and abs(fv - round(fv)) < 0.01:
            # plausible energy/water
            if fv in (1250, 3750, 5000, 20000) or (1000 <= fv <= 20000 and off % 4 == 0):
                pass

def dump_interesting_floats(h, obj, lo, hi, tag):
    print(f"\n==== floats {tag} ====")
    for off in range(lo, hi, 4):
        fv = f32(h, obj + off)
        if not (fv == fv):
            continue
        if 0.05 < fv <= 1.05 or 1.1 < fv <= 100.01 or 100.5 < fv <= 20000:
            print(f"  +0x{off:X} = {fv:.4f}")

def main():
    pid = pid_of("DayZ_x64.exe")
    h = k.OpenProcess(0x0410, False, pid)
    base, msize = module_base(h, "DayZ_x64.exe")
    world = u64(h, base + 0x4264058)
    ent = u64(h, u64(h, world + 0xF48))
    print(f"player={ent:X}")

    scan_range(h, ent, 0x600, 0x1400, "player body")
    dump_interesting_floats(h, ent, 0x600, 0x1400, "player 0x600-0x1400")

    inst = u64(h, ent + 0x6F0)
    ua = u64(h, ent + 0x6F8)
    inv = u64(h, ent + 0x650)
    print(f"\nInstance={inst:X} {rtti(h, inst, base, msize)}")
    print(f"UA={ua:X} {rtti(h, ua, base, msize)}")
    print(f"Inv={inv:X} {rtti(h, inv, base, msize)}")

    for name, obj in (("Instance", inst), ("UA", ua), ("Inv", inv)):
        if not valid(obj):
            continue
        scan_range(h, obj, 0, 0x200, name)
        dump_interesting_floats(h, obj, 0, 0x200, name)
        print(f"\n  {name} RTTI children:")
        for off in range(0, 0x200, 8):
            p = u64(h, obj + off)
            if not valid(p) or (base <= p < base + msize):
                continue
            n = rtti(h, p, base, msize)
            if n:
                print(f"    +0x{off:X} {n} {p:X}")
            s = cstr(h, p, 24)
            if s:
                print(f"    +0x{off:X} cstr {s!r}")

    # Wider player RTTI
    print("\n==== player RTTI 0xC00-0x1800 ====")
    for off in range(0xC00, 0x1808, 8):
        p = u64(h, ent + off)
        if not valid(p) or (base <= p < base + msize):
            continue
        n = rtti(h, p, base, msize)
        if n:
            print(f"  +0x{off:X} {n} {p:X}")

    # Search Energy/Water C-strings in DayZ module, then scan player graph for pointers to them
    print("\n==== module strings Energy/Water/Stamina ====")
    img = rpm(h, base, min(msize, 0x3000000))
    for s in (b"Energy\x00", b"Water\x00", b"Stamina\x00"):
        idx = 0
        hits = []
        while True:
            i = img.find(s, idx)
            if i < 0:
                break
            hits.append(base + i)
            idx = i + 1
            if len(hits) >= 8:
                break
        print(f"  {s[:-1]!r} -> " + ", ".join(f"{x:X}" for x in hits))

    return 0

if __name__ == "__main__":
    raise SystemExit(main())
