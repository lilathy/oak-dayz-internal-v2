#!/usr/bin/env python3
"""Find m_HealthLevel / netsync strings and probe server+client for player health-ish ints."""
import ctypes, struct, subprocess
from ctypes import wintypes

k = ctypes.WinDLL("kernel32"); p = ctypes.WinDLL("psapi")

class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p),
                ("AllocationProtect", wintypes.DWORD), ("RegionSize", ctypes.c_size_t),
                ("State", wintypes.DWORD), ("Protect", wintypes.DWORD), ("Type", wintypes.DWORD)]

def open_proc(name):
    out = subprocess.check_output(['tasklist','/FI',f'IMAGENAME eq {name}','/FO','CSV','/NH'], text=True, errors='ignore')
    for line in out.splitlines():
        if name.lower() not in line.lower(): continue
        pid = int([x.strip('"') for x in line.split(',')][1])
        h = k.OpenProcess(0x410, False, pid)
        mods = (wintypes.HMODULE*4)(); n=wintypes.DWORD()
        p.EnumProcessModulesEx(h, mods, ctypes.sizeof(mods), ctypes.byref(n), 3)
        base = ctypes.cast(mods[0], ctypes.c_void_p).value
        return pid, h, base
    return None, None, None

def rpm(h,a,n):
    b=(ctypes.c_char*n)(); g=ctypes.c_size_t()
    if not k.ReadProcessMemory(h, ctypes.c_void_p(a), b, n, ctypes.byref(g)): return b''
    return bytes(b[:g.value])

def u64(h,a):
    b=rpm(h,a,8); return struct.unpack('<Q',b)[0] if len(b)==8 else 0
def i32(h,a):
    b=rpm(h,a,4); return struct.unpack('<i',b)[0] if len(b)==4 else 0
def f32(h,a):
    b=rpm(h,a,4); return struct.unpack('<f',b)[0] if len(b)==4 else float('nan')
def eng(h,s):
    if not (0x100000000<s<0x7FFFFFFFFFFF): return ''
    ln=struct.unpack('<H', rpm(h,s+8,2) or b'\0\0')[0]
    if 1<=ln<=80:
        data=u64(h,s+0x10)
        if not (0x100000000<data): data=s+0x10
        return rpm(h,data,ln).decode('ascii','ignore')
    return ''

def find_cstr(img, base, s):
    needle=s.encode()+b'\0'; out=[]; i=0
    while True:
        j=img.find(needle,i)
        if j<0: break
        out.append(base+j); i=j+1
    return out

def lea_refs(img, base, target, limit=None):
    limit = limit or len(img)
    refs=[]
    for i in range(0, min(limit,len(img))-7):
        if img[i] not in (0x48,0x4C) or img[i+1]!=0x8D: continue
        if (img[i+2]&0xC7)!=0x05: continue
        disp=struct.unpack_from('<i',img,i+3)[0]
        if base+i+7+disp==target:
            refs.append(base+i)
    return refs

# --- Client or Server ---
for pname in ['DayZ_x64.exe','DayZServer_x64.exe']:
    pid,h,base = open_proc(pname)
    if not h:
        print(f'{pname}: not running'); continue
    print(f'\n======== {pname} pid={pid} base=0x{base:X} ========')
    img = rpm(h, base, 0x6000000)
    for s in ['m_HealthLevel','m_ShockSimplified','m_CurrentShock','m_BleedingBits','m_TrasferValues','TransferValues']:
        addrs=find_cstr(img, base, s)
        print(f'  {s}: {len(addrs)} -> {[hex(a) for a in addrs[:3]]}')
        for a in addrs[:1]:
            refs=lea_refs(img, base, a, 0xA00000)
            print(f'    LEA refs in first 10MB: {len(refs)}')
            for r in refs[:4]:
                print(f'      @0x{r:X} RVA=0x{r-base:X}')

    # Try common world offsets for server too
    world_offs = [0x4264058] if 'DayZ_x64' in pname else [0x4264058, 0x4250000, 0x4270000, 0x4240000, 0x416AC60]
    world=0
    for wo in world_offs:
        w=u64(h, base+wo)
        if 0x100000000<w<0x7FFFFFFFFFFF:
            near=u64(h, w+0xF48); cnt=i32(h, w+0xF48+8)
            if 0x100000000<near and 0<cnt<5000:
                world=w; print(f'  world@0x{wo:X}=0x{w:X} nearCnt={cnt}'); break
    if not world:
        print('  no world found'); continue

    # Enumerate dayzplayers and dump candidate ints/floats for netsync
    players=[]
    for loff in [0xF48,0x1090,0x2010]:
        data=u64(h, world+loff); cnt=i32(h, world+loff+8)
        if not (0x100000000<data) or cnt<1 or cnt>5000: continue
        for i in range(min(cnt,400)):
            ent=u64(h, data+i*8)
            if not (0x100000000<ent): continue
            typ=u64(h, ent+0x180)
            if not (0x100000000<typ): continue
            cfg=eng(h, u64(h, typ+0xD0))
            if 'player' not in cfg.lower() and cfg!='dayzplayer': continue
            if ent not in players: players.append(ent)
    print(f'  players={len(players)}')
    for ent in players[:4]:
        ds=u64(h, ent+0x188)
        print(f'  ent=0x{ent:X} ds188=0x{ds:X}')
        # dump small ints 0..4 and floats in player body that look like shock/health
        print('    int candidates (0..63):')
        for off in range(0x200, 0x900, 4):
            v=i32(h, ent+off)
            if 0<=v<=63:
                # skip zeros mostly — show non-zero or health-level-ish
                if v!=0 or off%0x40==0:
                    if 0<=v<=4 or (1<=v<=63):
                        if v>0:
                            print(f'      +0x{off:X}={v}')
        print('    float candidates:')
        for off in range(0x200, 0x900, 4):
            v=f32(h, ent+off)
            if v!=v: continue
            if (0.05<v<=1.0) or (90<=v<=100) or (4000<=v<=5500) or (20<=v<=100 and v==int(v)):
                print(f'      +0x{off:X}={v:.3f}')
        if 0x100000000<ds<0x7FFFFFFFFFFF and not (base<=ds<base+0x8000000):
            print(f'    DS f10={f32(h,ds+0x10):.3f} arr={u64(h,ds+0x28):X} c={i32(h,ds+0x34)}')
