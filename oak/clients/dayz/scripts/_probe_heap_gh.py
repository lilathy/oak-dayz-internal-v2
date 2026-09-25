#!/usr/bin/env python3
"""Heap-scan for GlobalHealth zone tables (any object), and dump GetHealth01/GetHealth abs variants."""
import ctypes, struct, subprocess
from ctypes import wintypes

k = ctypes.WinDLL("kernel32"); p = ctypes.WinDLL("psapi")
class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p),
                ("AllocationProtect", wintypes.DWORD), ("RegionSize", ctypes.c_size_t),
                ("State", wintypes.DWORD), ("Protect", wintypes.DWORD), ("Type", wintypes.DWORD)]

out = subprocess.check_output(['tasklist','/FI','IMAGENAME eq DayZ_x64.exe','/FO','CSV','/NH'], text=True)
pid = int([x.strip('"') for x in out.split(",")][1])
h = k.OpenProcess(0x410, False, pid)
mods = (wintypes.HMODULE*4)(); n=wintypes.DWORD()
p.EnumProcessModulesEx(h, mods, ctypes.sizeof(mods), ctypes.byref(n), 3)
base = ctypes.cast(mods[0], ctypes.c_void_p).value
mi_size = 0x4407000

def rpm(a,n):
    b=(ctypes.c_char*n)(); g=ctypes.c_size_t()
    if not k.ReadProcessMemory(h, ctypes.c_void_p(a), b, n, ctypes.byref(g)): return b""
    return bytes(b[:g.value])
def u64(a):
    b=rpm(a,8); return struct.unpack("<Q",b)[0] if len(b)==8 else 0
def f32(a):
    b=rpm(a,4); return struct.unpack("<f",b)[0] if len(b)==4 else float("nan")

# find GlobalHealth
img = rpm(base, min(mi_size, 0x6000000))
gh = base + img.find(b"GlobalHealth\x00")
health = base + img.find(b"Health\x00", img.find(b"GlobalHealth\x00")-16)
print(f"GH=0x{gh:X} HealthNearby=0x{health:X}")

# scan heap for qword == gh
hits = []
addr = 0x100000000
mbi = MBI()
scanned = 0
while addr < 0x7FFFFFFFFFFF and len(hits) < 30 and scanned < 1200:
    if not k.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
        break
    baseA = mbi.BaseAddress or 0
    size = mbi.RegionSize or 0
    nxt = baseA + size
    if nxt <= addr: break
    readable = (mbi.State == 0x1000) and (mbi.Protect & 0xEE) and not (mbi.Protect & 0x100)
    if readable and 0 < size < 0x2000000 and not (base <= baseA < base + mi_size):
        scanned += 1
        # read in 512k chunks
        off = 0
        while off < size and len(hits) < 30:
            n = min(0x80000, size - off)
            buf = rpm(baseA + off, n)
            for i in range(0, len(buf) - 8, 8):
                val = struct.unpack_from("<Q", buf, i)[0]
                if val == gh or val == health:
                    loc = baseA + off + i
                    fv = struct.unpack_from("<f", buf, i+8)[0] if i+12 <= len(buf) else float("nan")
                    hits.append((loc, val, fv))
            off += n
    addr = nxt

print(f"heap string-ptr hits={len(hits)} regions={scanned}")
for loc, val, fv in hits[:25]:
    which = "GH" if val == gh else "Health"
    print(f"  @0x{loc:X} {which} f+8={fv:.3f}")
    # try interpret as zone table entry: look back for table base by checking neighbors
    # print container candidate = loc & ~0xF aligned back
    for back in range(0, 0x80, 8):
        cand = loc - back
        # if cand-0x28 style: check if some object points here at +0x28
        pass

# Also: call path RVA 0x4226A0 (GetHealth01 on DS)
print("\nDamageSystem GetHealth01-ish @ 0x4226A0:")
b = rpm(base + 0x4226A0, 64)
for i in range(0, len(b), 16):
    print(" ", f"{base+0x4226A0+i:X}", " ".join(f"{x:02X}" for x in b[i:i+16]))

# Check dead flag and network on player
world = u64(base + 0x4264058)
ent = u64(u64(world + 0xF48))
print(f"\nent=0x{ent:X} dead@E2={rpm(ent+0xE2,1)[0]} net@6E4={struct.unpack('<i',rpm(ent+0x6E4,4))[0]}")
print(f"ent+0x188={u64(ent+0x188):X} ent+0x190 as float={f32(ent+0x190):.3f}")
# 0x190 was 0xBF800000 = -1.0f

# Scan player for any float that looks like blood (4500-5000) or health 0.9-1.0
print("player float scan 0x40..0x900:")
for off in range(0x40, 0x900, 4):
    v = f32(ent + off)
    if v != v: continue
    if (0.2 < v <= 1.0) or (90 <= v <= 100) or (4000 <= v <= 5500) or (1000 <= v <= 1500):
        print(f"  +0x{off:X} = {v:.3f}")
