#!/usr/bin/env python3
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

STAT = {"Energy", "Water", "Stamina", "HeatComfort", "Diet", "Tremor", "Toxicity",
        "Wet", "HeatBuffer", "Specialty", "BloodType", "Health", "Blood", "Shock"}

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

def cstr(h, p, n=32):
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

def labels_on(h, obj, base, msize):
    found = []
    if not valid(obj):
        return found
    for lo in range(0, 0x90, 8):
        q = u64(h, obj + lo)
        s = cstr(h, q, 24)
        if s in STAT:
            found.append((lo, s))
        if valid(q) and not (base <= q < base + msize):
            for io in (0, 8, 0x10, 0x18, 0x20):
                s2 = cstr(h, u64(h, q + io), 24)
                if s2 in STAT:
                    found.append((lo, s2))
    return found

def main():
    pid = pid_of("DayZ_x64.exe")
    h = k.OpenProcess(0x0410, False, pid)
    base, msize = module_base(h, "DayZ_x64.exe")
    world = u64(h, base + 0x4264058)
    near = u64(h, world + 0xF48)
    ent = u64(h, near)
    print(f"base={base:X} player={ent:X} rtti={rtti(h, ent, base, msize)}")
    print(f"net6E4={i32(h, ent+0x6E4)} net6DC={i32(h, ent+0x6DC)}")
    print(f"+188={u64(h, ent+0x188):X} {rtti(h, u64(h, ent+0x188), base, msize)}")
    print(f"+6F0={u64(h, ent+0x6F0):X} {rtti(h, u64(h, ent+0x6F0), base, msize)}")
    print(f"+6A4={f32(h, ent+0x6A4):.3f}")
    vs = u64(h, ent + 0x1C8)
    if valid(vs):
        print(f"pos={f32(h, vs+0x2C):.1f},{f32(h, vs+0x30):.1f},{f32(h, vs+0x34):.1f}")

    print("\n==== RTTI ====")
    named = []
    for off in range(0x40, 0xC08, 8):
        p = u64(h, ent + off)
        if not valid(p) or (base <= p < base + msize):
            continue
        n = rtti(h, p, base, msize)
        if n:
            named.append((off, p, n))
            print(f"  +0x{off:X} {n} {p:X}")

    print("\n==== labels / arrays on all heap slots ====")
    for off in range(0x40, 0xC08, 8):
        p = u64(h, ent + off)
        if not valid(p) or (base <= p < base + msize):
            continue
        labs = labels_on(h, p, base, msize)
        if labs:
            print(f"  labels +0x{off:X} {rtti(h, p, base, msize) or '?'} {labs} val2C={f32(h, p+0x2C):.3f}")
        for aoff in (0, 8, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38):
            arr = u64(h, p + aoff)
            cnt = i32(h, p + aoff + 8)
            if not valid(arr) or not (3 <= cnt <= 24):
                continue
            hits = []
            for i in range(cnt):
                rec = u64(h, arr + i * 8)
                if not valid(rec):
                    continue
                rl = labels_on(h, rec, base, msize)
                if rl:
                    floats = [(fo, f32(h, rec + fo)) for fo in range(0x10, 0x48, 4)]
                    floats = [(fo, v) for fo, v in floats if v == v and -1 <= v <= 20000]
                    hits.append((i, rec, rl, f"2C={f32(h, rec+0x2C):.3f}", floats[:8]))
            if hits:
                print(f"  ARRAY +0x{off:X}+0x{aoff:X} cnt={cnt} parent={rtti(h, p, base, msize)}")
                for hit in hits:
                    print("   ", hit)
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
