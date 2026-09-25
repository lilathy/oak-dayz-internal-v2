#!/usr/bin/env python3
"""Compare +0x188 across players/infected; decode GetHealth01 helpers; check server process briefly."""
import ctypes, struct, subprocess
from ctypes import wintypes

k = ctypes.WinDLL("kernel32"); p = ctypes.WinDLL("psapi")

def open_dayz(name):
    out = subprocess.check_output(['tasklist','/FI',f'IMAGENAME eq {name}','/FO','CSV','/NH'], text=True, errors='ignore')
    for line in out.splitlines():
        if name.lower() not in line.lower(): continue
        pid = int([x.strip('"') for x in line.split(",")][1])
        h = k.OpenProcess(0x410, False, pid)
        mods = (wintypes.HMODULE*4)(); n=wintypes.DWORD()
        p.EnumProcessModulesEx(h, mods, ctypes.sizeof(mods), ctypes.byref(n), 3)
        base = ctypes.cast(mods[0], ctypes.c_void_p).value
        return pid, h, base
    return None, None, None

def rpm(h,a,n):
    b=(ctypes.c_char*n)(); g=ctypes.c_size_t()
    if not k.ReadProcessMemory(h, ctypes.c_void_p(a), b, n, ctypes.byref(g)): return b""
    return bytes(b[:g.value])
def u64(h,a):
    b=rpm(h,a,8); return struct.unpack("<Q",b)[0] if len(b)==8 else 0
def i32(h,a):
    b=rpm(h,a,4); return struct.unpack("<i",b)[0] if len(b)==4 else 0
def eng(h,s):
    if not (0x100000000<s<0x7FFFFFFFFFFF): return ""
    ln=struct.unpack("<H", rpm(h,s+8,2) or b"\0\0")[0]
    if 1<=ln<=80:
        data=u64(h,s+0x10)
        if not (0x100000000<data<0x7FFFFFFFFFFF): data=s+0x10
        return rpm(h,data,ln).decode("ascii","ignore")
    raw=rpm(h,s,32)
    if raw and all(32<=x<=126 or x==0 for x in raw[:20]):
        return raw.split(b"\x00")[0].decode("ascii","ignore")
    return ""

pid,h,base = open_dayz("DayZ_x64.exe")
print(f"client pid={pid} base=0x{base:X}")
world=u64(h, base+0x4264058)
# decode helpers
rel1=struct.unpack("<i", rpm(h, base+0x923F34,4))[0]
rel2=struct.unpack("<i", rpm(h, base+0x923F41,4))[0]
print(f"helper1 target RVA=0x{(0x923F38+rel1):X} abs=0x{base+0x923F38+rel1:X}")
print(f"helper2 target RVA=0x{(0x923F45+rel2):X}")

for loff in [0xF48, 0x1090, 0x2010]:
    data=u64(h, world+loff); cnt=i32(h, world+loff+8)
    print(f"\nlist+0x{loff:X} cnt={cnt}")
    if not (0x100000000<data) or cnt<1 or cnt>5000: continue
    shown=0
    for i in range(min(cnt, 300)):
        ent=u64(h, data+i*8)
        if not (0x100000000<ent<0x7FFFFFFFFFFF): continue
        typ=u64(h, ent+0x180)
        if not (0x100000000<typ): continue
        cfg=eng(h, u64(h, typ+0xD0))
        if cfg not in ("dayzplayer","dayzinfected") and "Zmb" not in cfg and "Survivor" not in cfg and "Player" not in cfg:
            # also accept if rtti-ish via cfg empty but net id looks set
            if cfg and "zmb" not in cfg.lower() and "player" not in cfg.lower() and "infected" not in cfg.lower():
                continue
        ds=u64(h, ent+0x188)
        dm=u64(h, ent+0x700)
        net=i32(h, ent+0x6E4)
        print(f"  ent=0x{ent:X} cfg={cfg!r} net={net} ds188=0x{ds:X} dm700=0x{dm:X}")
        shown += 1
        if shown>=15: break
    print(f"  shown={shown}")

# Server-side: try same world offset briefly
spid,sh,sbase = open_dayz("DayZServer_x64.exe")
if sh:
    print(f"\nserver pid={spid} base=0x{sbase:X}")
    # world offset may differ on server — try same anyway
    for woff in [0x4264058, 0x416AC60, 0x4250000]:
        sw=u64(sh, sbase+woff)
        if 0x100000000<sw<0x7FFFFFFFFFFF:
            print(f"  try world@0x{woff:X}=0x{sw:X}")
            near=u64(sh, sw+0xF48); cnt=i32(sh, sw+0xF48+8)
            print(f"    near data=0x{near:X} cnt={cnt}")
