#!/usr/bin/env python3
import ctypes, struct, subprocess
from ctypes import wintypes
k=ctypes.WinDLL("kernel32"); p=ctypes.WinDLL("psapi")
out=subprocess.check_output(['tasklist','/FI','IMAGENAME eq DayZ_x64.exe','/FO','CSV','/NH'],text=True)
pid=int([x.strip('"') for x in out.split(",")][1])
h=k.OpenProcess(0x410,False,pid)
mods=(wintypes.HMODULE*4)(); n=wintypes.DWORD()
p.EnumProcessModulesEx(h,mods,ctypes.sizeof(mods),ctypes.byref(n),3)
base=ctypes.cast(mods[0],ctypes.c_void_p).value
def rpm(a,n):
    b=(ctypes.c_char*n)(); g=ctypes.c_size_t()
    k.ReadProcessMemory(h,ctypes.c_void_p(a),b,n,ctypes.byref(g)); return bytes(b[:g.value])
def u64(a):
    b=rpm(a,8); return struct.unpack("<Q",b)[0] if len(b)==8 else 0
def i32(a):
    b=rpm(a,4); return struct.unpack("<i",b)[0] if len(b)==4 else 0
def f32(a):
    b=rpm(a,4); return struct.unpack("<f",b)[0] if len(b)==4 else float("nan")
def rtti(obj):
    vt=u64(obj)
    if not (base<=vt<base+0x8000000): return ""
    col=u64(vt-8)
    if not (base<=col<base+0x8000000): return ""
    td=base+(i32(col+0xC)&0xFFFFFFFF)
    return rpm(td+0x10,80).split(b"\0")[0].decode("ascii","ignore")
def eng(s):
    if not (0x100000000<s<0x7FFFFFFFFFFF): return ""
    ln=struct.unpack("<H", rpm(s+8,2) or b"\0\0")[0]
    if 1<=ln<=80:
        data=u64(s+0x10)
        if not (0x100000000<data): data=s+0x10
        return rpm(data,ln).decode("ascii","ignore")
    raw=rpm(s,32)
    if raw and all(32<=x<=126 or x==0 for x in raw[:24]):
        return raw.split(b"\0")[0].decode("ascii","ignore")
    return ""

def dump_ds(ent):
    typ=u64(ent+0x180)
    cfg=eng(u64(typ+0xD0)) if 0x100000000<typ else ""
    tn=eng(u64(typ+0x98)) if 0x100000000<typ else ""
    ds=u64(ent+0x188)
    print(f"\nent=0x{ent:X} rtti={rtti(ent)} cfg={cfg!r} typeName={tn!r}")
    print(f"  ds=0x{ds:X} rtti={rtti(ds)}")
    if not (0x100000000<ds): return
    arr=u64(ds+0x28); c=i32(ds+0x34)
    if not (1<=c<=64): c=i32(ds+0x30)
    print(f"  f10={f32(ds+0x10):.3f} arr=0x{arr:X} c={c}")
    if 0x100000000<arr and 1<=c<=64:
        for i in range(min(c,12)):
            slot=arr+i*0x10; np=u64(slot); fv=f32(slot+8)
            name=eng(np)
            if not name and 0x100000000<np:
                raw=rpm(np,24)
                if raw and all(32<=x<=126 or x==0 for x in raw[:20]):
                    name=raw.split(b"\0")[0].decode("ascii","ignore")
            z0=f32(np) if np else 0; z4=f32(np+4) if np else 0
            print(f"    [{i}] '{name}' f={fv:.3f} z={z0:.3f}/{z4:.3f}")

world=u64(base+0x4264058)
# Near players
data=u64(world+0xF48); cnt=i32(world+0xF48+8)
print(f"NEAR cnt={cnt}")
for i in range(min(cnt,20)):
    ent=u64(data+i*8)
    if 0x100000000<ent:
        dump_ds(ent)
        print(f"  +0x188={u64(ent+0x188):X} +0x700={u64(ent+0x700):X}")

# Known hits
for ent in [0x167037D4380, 0x16703EE3200]:
    dump_ds(ent)
