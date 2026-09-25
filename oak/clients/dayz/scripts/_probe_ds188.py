#!/usr/bin/env python3
import ctypes, struct, subprocess
from ctypes import wintypes

k = ctypes.WinDLL("kernel32")
p = ctypes.WinDLL("psapi")
out = subprocess.check_output(
    ['tasklist', '/FI', 'IMAGENAME eq DayZ_x64.exe', '/FO', 'CSV', '/NH'], text=True)
pid = int([x.strip('"') for x in out.split(",")][1])
h = k.OpenProcess(0x410, False, pid)
mods = (wintypes.HMODULE * 4)()
n = wintypes.DWORD()
p.EnumProcessModulesEx(h, mods, ctypes.sizeof(mods), ctypes.byref(n), 3)
base = ctypes.cast(mods[0], ctypes.c_void_p).value

def rpm(a, n):
    b = (ctypes.c_char * n)()
    g = ctypes.c_size_t()
    k.ReadProcessMemory(h, ctypes.c_void_p(a), b, n, ctypes.byref(g))
    return bytes(b[: g.value])

def u64(a):
    b = rpm(a, 8)
    return struct.unpack("<Q", b)[0] if len(b) == 8 else 0

def i32(a):
    b = rpm(a, 4)
    return struct.unpack("<i", b)[0] if len(b) == 4 else 0

def f32(a):
    b = rpm(a, 4)
    return struct.unpack("<f", b)[0] if len(b) == 4 else float("nan")

world = u64(base + 0x4264058)
near = u64(world + 0xF48)
ent = u64(near)
print(f"ent=0x{ent:X}")
for off in [0x100, 0x108, 0x180, 0x188, 0x190, 0x198, 0x1A0, 0x700, 0x6F0, 0x6F8]:
    print(f"  +0x{off:X}=0x{u64(ent+off):X}")

ds = u64(ent + 0x188)
print(f"DamageSystem@188=0x{ds:X}")
if 0x100000000 < ds < 0x7FFFFFFFFFFF:
    print(f"  f10={f32(ds+0x10):.3f} arr28=0x{u64(ds+0x28):X} c34={i32(ds+0x34)} c30={i32(ds+0x30)}")
    arr = u64(ds + 0x28)
    c = i32(ds + 0x34)
    if not (1 <= c <= 64):
        c = i32(ds + 0x30)
    if 0x100000000 < arr < 0x7FFFFFFFFFFF and 1 <= c <= 64:
        for i in range(min(c, 16)):
            slot = arr + i * 0x10
            np = u64(slot)
            fv = f32(slot + 8)
            name = ""
            if 0x100000000 < np:
                raw = rpm(np, 48)
                if raw:
                    # module cstring or engstring
                    if all(32 <= b <= 126 or b == 0 for b in raw[:24]):
                        name = raw.split(b"\x00")[0].decode("ascii", "ignore")
                    else:
                        # engstring: len@+8 chars@+0x10
                        ln = struct.unpack_from("<H", raw, 8)[0] if len(raw) > 10 else 0
                        if 1 <= ln <= 40:
                            data = u64(np + 0x10)
                            if not (0x100000000 < data < 0x7FFFFFFFFFFF):
                                data = np + 0x10
                            name = rpm(data, ln).decode("ascii", "ignore")
            z0 = f32(np) if np else 0
            z4 = f32(np + 4) if np else 0
            print(f"  [{i}] name={name!r} f={fv:.3f} z0={z0:.3f} z4={z4:.3f} np=0x{np:X}")

# Dump GetHealth01 full after HasDamage path
print("\nGetHealth01 @ 0x923F20:")
img = rpm(base + 0x923F20, 0xA0)
for i in range(0, len(img), 16):
    print(" ", f"{base+0x923F20+i:X}", " ".join(f"{x:02X}" for x in img[i:i+16]))

# Decode: after mov rcx,[rbx+188], what next?
print("\nGetHealth @ 0x923FB0 (likely abs GetHealth):")
img = rpm(base + 0x923FB0, 0xA0)
for i in range(0, len(img), 16):
    print(" ", f"{base+0x923FB0+i:X}", " ".join(f"{x:02X}" for x in img[i:i+16]))
