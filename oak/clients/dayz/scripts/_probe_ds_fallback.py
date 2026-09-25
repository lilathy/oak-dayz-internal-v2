#!/usr/bin/env python3
import ctypes, struct, subprocess
from ctypes import wintypes

k = ctypes.WinDLL("kernel32"); p = ctypes.WinDLL("psapi")
out = subprocess.check_output(['tasklist','/FI','IMAGENAME eq DayZ_x64.exe','/FO','CSV','/NH'], text=True)
pid = int([x.strip('"') for x in out.split(",")][1])
h = k.OpenProcess(0x410, False, pid)
mods = (wintypes.HMODULE*4)(); n=wintypes.DWORD()
p.EnumProcessModulesEx(h, mods, ctypes.sizeof(mods), ctypes.byref(n), 3)
base = ctypes.cast(mods[0], ctypes.c_void_p).value

def rpm(a,n):
    b=(ctypes.c_char*n)(); g=ctypes.c_size_t()
    k.ReadProcessMemory(h, ctypes.c_void_p(a), b, n, ctypes.byref(g)); return bytes(b[:g.value])
def u64(a):
    b=rpm(a,8); return struct.unpack("<Q",b)[0] if len(b)==8 else 0
def i32(a):
    b=rpm(a,4); return struct.unpack("<i",b)[0] if len(b)==4 else 0
def f32(a):
    b=rpm(a,4); return struct.unpack("<f",b)[0] if len(b)==4 else float("nan")
def rtti(obj):
    vt=u64(obj)
    if not (base <= vt < base+0x8000000): return ""
    col=u64(vt-8)
    if not (base <= col < base+0x8000000): return ""
    rva=i32(col+0x0C)
    td=base+(rva & 0xFFFFFFFF)
    raw=rpm(td+0x10, 80)
    return raw.split(b"\x00")[0].decode("ascii","ignore")

world=u64(base+0x4264058); near=u64(world+0xF48); ent=u64(near)
typ=u64(ent+0x180)
print(f"ent=0x{ent:X} typ=0x{typ:X} rtti={rtti(ent)}")
print(f"ent+0xA8=0x{u64(ent+0xA8):X} rtti={rtti(u64(ent+0xA8)) if u64(ent+0xA8)>0x100000000 else ''}")
print(f"typ+0x148 flag@+1={rpm(typ+0x149,1)[0] if typ else -1}")
print(f"typ+0x118=0x{u64(typ+0x118):X}")

# dump entity+0xA0..0x120
print("\nentity pointers 0xA0..0x1C0:")
for off in range(0xA0, 0x1C0, 8):
    v=u64(ent+off)
    if 0x100000000 < v < 0x7FFFFFFFFFFF and not (base <= v < base+0x8000000):
        print(f"  +0x{off:X}=0x{v:X} rtti={rtti(v)}")

def dump_zones(obj, tag):
    if not (0x100000000 < obj < 0x7FFFFFFFFFFF):
        print(f"[{tag}] null"); return
    print(f"[{tag}] 0x{obj:X} rtti={rtti(obj)} f10={f32(obj+0x10):.3f}")
    for toff,coff in [(0x28,0x34),(0x20,0x28),(0x18,0x24)]:
        arr=u64(obj+toff); c=i32(obj+coff)
        if 0x100000000 < arr < 0x7FFFFFFFFFFF and 1 <= c <= 64:
            print(f"  table+0x{toff:X} c@+0x{coff:X}={c}")
            for i in range(min(c,12)):
                slot=arr+i*0x10; np=u64(slot); fv=f32(slot+8)
                name=""
                if 0x100000000 < np:
                    raw=rpm(np,32)
                    if raw and all(32<=b<=126 or b==0 for b in raw[:20]):
                        name=raw.split(b"\x00")[0].decode("ascii","ignore")
                    else:
                        ln=struct.unpack_from("<H", raw, 8)[0] if len(raw)>10 else 0
                        if 1<=ln<=40:
                            data=u64(np+0x10)
                            if not (0x100000000<data<0x7FFFFFFFFFFF): data=np+0x10
                            name=rpm(data,ln).decode("ascii","ignore")
                z0=f32(np) if np else 0; z4=f32(np+4) if np else 0
                print(f"    [{i}] '{name}' f={fv:.3f} z={z0:.3f}/{z4:.3f}")
            return

dump_zones(u64(ent+0xA8), "ent+0xA8")
# EntityType embedded DamageSystemData (UC edit2)
print(f"\nEntityType+0x118 as inline:")
typ=u64(ent+0x180)
for off in [0x100,0x110,0x118,0x120,0x128,0x140,0x148,0x150]:
    f4=f32(typ+off+4); arr=u64(typ+off+0x18); c=i32(typ+off+0x24)
    ptr=u64(typ+off)
    print(f"  typ+0x{off:X} ptr=0x{ptr:X} f4={f4:.3f} arr18=0x{arr:X} c24={c}")
    if 0x100000000 < ptr < 0x7FFFFFFFFFFF:
        dump_zones(ptr, f"*(typ+0x{off:X})")

# Follow GetHealth01 helper calls
# E8 13 02 DE FF from 0x923F33 -> 
call1 = base + 0x923F33 + 5 + struct.unpack("<i", rpm(base+0x923F34,4))[0]
call2 = base + 0x923F40 + 5 + struct.unpack("<i", rpm(base+0x923F41,4))[0]
print(f"\nGetHealth01 helper1=0x{call1:X} RVA=0x{call1-base:X}")
print(f"GetHealth01 helper2=0x{call2:X} RVA=0x{call2-base:X}")
for addr,name in [(call1,"h1"),(call2,"h2")]:
    print(f"--- {name} ---")
    b=rpm(addr,48)
    for i in range(0,len(b),16):
        print(" ", " ".join(f"{x:02X}" for x in b[i:i+16]))

# Call target from success path E8 05 E7 AF FF at 0x923F96
ct = base + 0x923F96 + 5 + struct.unpack("<i", rpm(base+0x923F97,4))[0]
print(f"\nGetHealth01 DS call target=0x{ct:X} RVA=0x{ct-base:X}")
