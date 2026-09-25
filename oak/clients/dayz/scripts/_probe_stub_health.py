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
    if not (0x100000000<obj<0x7FFFFFFFFFFF): return ""
    vt=u64(obj)
    if not (base<=vt<base+0x8000000): return ""
    col=u64(vt-8)
    if not (base<=col<base+0x8000000): return ""
    td=base+(i32(col+0xC)&0xFFFFFFFF)
    return rpm(td+0x10,80).split(b"\0")[0].decode("ascii","ignore")

world=u64(base+0x4264058)
stub=u64(world+0x2960)
ent=u64(u64(world+0xF48))
print(f"stub=0x{stub:X} rtti={rtti(stub)}")
print(f"ent =0x{ent:X} rtti={rtti(ent)}")
print(f"ent+0xB0=0x{u64(ent+0xB0):X} (==stub? {u64(ent+0xB0)==stub})")

for label,obj in [("stub",stub),("ent",ent),("ent.B0",u64(ent+0xB0))]:
    if not (0x100000000<obj<0x7FFFFFFFFFFF): 
        print(f"\n{label}: invalid"); continue
    print(f"\n==== {label} 0x{obj:X} rtti={rtti(obj)} ====")
    for off in [0xA8,0xB0,0x108,0x180,0x188,0x190,0x650,0x6F0,0x6F8,0x700]:
        v=u64(obj+off)
        extra=""
        if 0x100000000<v<0x7FFFFFFFFFFF:
            if base<=v<base+0x8000000: extra=" [MOD]"
            else: extra=f" rtti={rtti(v)}"
        print(f"  +0x{off:X}=0x{v:X}{extra}")

# Find GetHealthLevel string + native
img=rpm(base,0x6000000)
idx=img.find(b"GetHealthLevel\x00")
print(f"\nGetHealthLevel str@0x{base+idx:X}" if idx>=0 else "no GetHealthLevel")
# lea refs
refs=[]
if idx>=0:
    target=base+idx
    for i in range(0,min(len(img),0xA00000)-7):
        if img[i] in (0x48,0x4C) and img[i+1]==0x8D and (img[i+2]&0xC7)==0x05:
            disp=struct.unpack_from("<i",img,i+3)[0]
            if base+i+7+disp==target:
                refs.append(base+i)
    print(f"LEA refs: {len(refs)}")
    for r in refs[:5]:
        print(f"  0x{r:X} RVA=0x{r-base:X}")
        # look nearby for function ptr lea into .text
        off=r-base
        for j in range(max(0,off-0x100), off+0x20):
            if img[j] in (0x48,0x4C) and img[j+1]==0x8D and (img[j+2]&0xC7)==0x05:
                d=struct.unpack_from("<i",img,j+3)[0]
                t=base+j+7+d
                if base<t<base+0xA00000:
                    print(f"    code LEA RVA 0x{j:X} -> 0x{t-base:X}")
                    b=rpm(t,32)
                    print("     ", " ".join(f"{x:02X}" for x in b))

# IsDamageDestroyed / dead-related
for name in [b"IsDamageDestroyed\x00", b"GetHealthLevel\x00"]:
    i=img.find(name)
    print(f"{name[:-1].decode()} @ 0x{base+i:X}" if i>=0 else f"missing {name}")
