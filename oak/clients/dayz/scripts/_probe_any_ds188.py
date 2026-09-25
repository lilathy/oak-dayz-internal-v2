#!/usr/bin/env python3
"""Find ANY entity with non-null +0x188 DamageSystem on client."""
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
def eng(s):
    if not (0x100000000<s<0x7FFFFFFFFFFF): return ""
    ln=struct.unpack("<H", rpm(s+8,2) or b"\0\0")[0]
    if 1<=ln<=64:
        data=u64(s+0x10)
        if not (0x100000000<data): data=s+0x10
        return rpm(data,ln).decode("ascii","ignore")
    return ""

world=u64(base+0x4264058)
hits=0
scanned=0
for loff in [0xF48,0x1090,0x2010,0x2060]:
    data=u64(world+loff); cnt=i32(world+loff+8)
    if not (0x100000000<data) or cnt<1 or cnt>8000: 
        print(f"list+0x{loff:X} skip cnt={cnt}")
        continue
    print(f"list+0x{loff:X} cnt={cnt}")
    for i in range(min(cnt, 2000)):
        ent=u64(data+i*8)
        if not (0x100000000<ent<0x7FFFFFFFFFFF): continue
        scanned+=1
        ds=u64(ent+0x188)
        if not (0x100000000<ds<0x7FFFFFFFFFFF): continue
        if base<=ds<base+0x8000000: continue
        typ=u64(ent+0x180)
        cfg=eng(u64(typ+0xD0)) if 0x100000000<typ else "?"
        print(f"  HIT ent=0x{ent:X} cfg={cfg!r} ds=0x{ds:X}")
        hits+=1
        if hits>=20: break
    if hits>=20: break
print(f"scanned={scanned} hits={hits}")
