#!/usr/bin/env python3
"""Deep-dive enf::Instance at player+0x6F0 for Health/Blood/Shock variables."""
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
def cstr(a,n=64):
    raw=rpm(a,n)
    if not raw: return ""
    if all(32<=x<=126 or x==0 for x in raw[:min(32,len(raw))]):
        return raw.split(b"\0")[0].decode("ascii","ignore")
    return ""
def eng(s):
    if not (0x100000000<s<0x7FFFFFFFFFFF): return ""
    # try module cstring
    if base<=s<base+0x8000000:
        return cstr(s)
    ln=struct.unpack("<H", rpm(s+8,2) or b"\0\0")[0]
    if 1<=ln<=80:
        data=u64(s+0x10)
        if not (0x100000000<data<0x7FFFFFFFFFFF): data=s+0x10
        return rpm(data,ln).decode("ascii","ignore")
    return cstr(s)

world=u64(base+0x4264058)
ent=u64(u64(world+0xF48))
inst=u64(ent+0x6F0)
print(f"ent=0x{ent:X} inst=0x{inst:X}")

# dump instance pointer fanout
print("instance slots:")
for off in range(0, 0x100, 8):
    v=u64(inst+off)
    if 0x100000000<v<0x7FFFFFFFFFFF:
        s=eng(v)
        mark=f" '{s}'" if s and len(s)<40 else ""
        print(f"  +0x{off:X}=0x{v:X}{mark}")

# BFS from instance depth 3 looking for health-ish names or blood floats
want_names={"Health","Blood","Shock","Energy","Water","Stamina","GlobalHealth","health","blood","shock"}
from collections import deque
q=deque([(inst,0,"I")])
seen={inst}
found=0
while q and found<40 and len(seen)<2000:
    obj,d,path=q.popleft()
    if d>3: continue
    lim=0x200 if d==0 else 0x100
    for off in range(0,lim,8):
        p=u64(obj+off)
        # name at this slot?
        if base<=p<base+0x8000000:
            nm=cstr(p)
            if nm in want_names or any(nm==x for x in want_names):
                fv=f32(obj+off+8)
                print(f"NAMEHIT {path}+0x{off:X} '{nm}' f+8={fv:.3f}")
                found+=1
        if not (0x100000000<p<0x7FFFFFFFFFFF): continue
        if base<=p<base+0x8000000: continue
        nm=eng(p)
        if nm in want_names:
            fv=f32(obj+off+8)
            print(f"ENGHIT {path}+0x{off:X} '{nm}' f+8={fv:.3f}")
            # dump neighbors
            for dlt in range(-16,32,4):
                print(f"  d{dlt}: f={f32(obj+off+dlt):.3f}")
            found+=1
        if d<3 and p not in seen and len(seen)<2000:
            seen.add(p)
            q.append((p,d+1,f"{path}+0x{off:X}"))

print(f"done found={found} seen={len(seen)}")

# Also try Inventory and UAInterface briefly
for off,tag in [(0x650,"inv"),(0x6F8,"ua")]:
    obj=u64(ent+off)
    print(f"\n{tag}=0x{obj:X}")
    if not (0x100000000<obj): continue
    for o in range(0,0x80,8):
        v=u64(obj+o)
        if 0x100000000<v<0x7FFFFFFFFFFF and not (base<=v<base+0x8000000):
            nm=eng(v)
            if nm and any(k.lower() in nm.lower() for k in ("health","blood","shock","stamina","damage")):
                print(f"  +0x{o:X} -> '{nm}'")
