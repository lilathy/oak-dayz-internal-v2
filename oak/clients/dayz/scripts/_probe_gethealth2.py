#!/usr/bin/env python3
"""Find DamageSystem::GetHealth implementation and DayZPlayer vtable slots."""
import ctypes
from ctypes import wintypes
import struct
import subprocess

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
OpenProcess = kernel32.OpenProcess
OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
OpenProcess.restype = wintypes.HANDLE
ReadProcessMemory = kernel32.ReadProcessMemory
ReadProcessMemory.argtypes = [wintypes.HANDLE, wintypes.LPCVOID, wintypes.LPVOID, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
ReadProcessMemory.restype = wintypes.BOOL
CloseHandle = kernel32.CloseHandle
class MODULEINFO(ctypes.Structure):
    _fields_ = [("lpBaseOfDll", wintypes.LPVOID), ("SizeOfImage", wintypes.DWORD), ("EntryPoint", wintypes.LPVOID)]
EnumProcessModulesEx = psapi.EnumProcessModulesEx
EnumProcessModulesEx.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.HMODULE), wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), wintypes.DWORD]
EnumProcessModulesEx.restype = wintypes.BOOL
GetModuleInformation = psapi.GetModuleInformation
GetModuleInformation.argtypes = [wintypes.HANDLE, wintypes.HMODULE, ctypes.POINTER(MODULEINFO), wintypes.DWORD]
GetModuleInformation.restype = wintypes.BOOL

def find_pid(name):
    out = subprocess.check_output(["tasklist", "/FI", f"IMAGENAME eq {name}", "/FO", "CSV", "/NH"], text=True, errors="ignore")
    for line in out.splitlines():
        if name.lower() in line.lower():
            return int([p.strip('"') for p in line.split(",")][1])
    raise SystemExit("not running")

def rpm(h, addr, n):
    buf = (ctypes.c_char * n)()
    got = ctypes.c_size_t(0)
    if not ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)):
        return b""
    return bytes(buf[:got.value])

def u64(h, a):
    b = rpm(h, a, 8)
    return struct.unpack("<Q", b)[0] if len(b)==8 else 0

def hexdump(b, addr):
    for i in range(0, len(b), 16):
        print(f"  {addr+i:X}  " + " ".join(f"{x:02X}" for x in b[i:i+16]))

def lea_refs(img, base, target):
    refs = []
    for i in range(0, len(img) - 7):
        if img[i] not in (0x48, 0x4C) or img[i+1] != 0x8D:
            continue
        if (img[i+2] & 0xC7) != 0x05:
            continue
        disp = struct.unpack_from("<i", img, i+3)[0]
        instr = base + i
        if instr + 7 + disp == target:
            refs.append(instr)
    return refs

def find_func_start(img, offset):
    for i in range(offset, max(0, offset-0x600), -1):
        if i > 0 and img[i-1] == 0xCC and img[i] != 0xCC:
            return i
    return max(0, offset-0x40)

def main():
    pid = find_pid("DayZ_x64.exe")
    h = OpenProcess(0x0410, False, pid)
    mods = (wintypes.HMODULE * 4)()
    needed = wintypes.DWORD()
    EnumProcessModulesEx(h, mods, ctypes.sizeof(mods), ctypes.byref(needed), 3)
    base = ctypes.cast(mods[0], ctypes.c_void_p).value
    mi = MODULEINFO(); GetModuleInformation(h, mods[0], ctypes.byref(mi), ctypes.sizeof(mi))
    img = rpm(h, base, min(mi.SizeOfImage, 0x6000000))
    print(f"base=0x{base:X}")

    # substring search
    sub = b"[DamageSystem::GetHealth]"
    idx = img.find(sub)
    print(f"format substr idx={idx} addr=0x{base+idx:X}" if idx>=0 else "format not found")
    if idx >= 0:
        # show full cstring
        end = img.find(b"\x00", idx)
        print("full:", img[idx:end].decode("ascii", "ignore"))
        target = base + idx
        refs = lea_refs(img, base, target)
        print(f"LEA refs: {len(refs)}")
        for instr in refs[:10]:
            off = instr - base
            fs = find_func_start(img, off)
            print(f"\nref RVA=0x{off:X} func~0x{fs:X}")
            hexdump(img[fs:fs+128], base+fs)
            print("around:")
            hexdump(img[max(0,off-48):off+64], base+max(0,off-48))

    # Also GetHealth01 registration — walk BACKWARD from GetHealth01 lea to find function ptr arg
    gh01 = img.find(b"GetHealth01\x00")
    print(f"\nGetHealth01 @ 0x{base+gh01:X}")
    # find LEA to it
    for instr in lea_refs(img, base, base+gh01)[:3]:
        off = instr - base
        print(f"reg LEA RVA=0x{off:X}")
        # dump 0x200 bytes before — look for mov r8/r9 imm or lea of function
        start = max(0, off - 0x200)
        region = img[start:off+0x40]
        # find absolute addresses in module that look like code pointers near here
        for i in range(0, len(region)-8, 1):
            # look for LEA r64, [rip+disp] targeting .text
            if region[i] in (0x48, 0x4C) and region[i+1] == 0x8D and (region[i+2] & 0xC7) == 0x05:
                disp = struct.unpack_from("<i", region, i+3)[0]
                abs_instr = base + start + i
                tgt = abs_instr + 7 + disp
                if base < tgt < base + 0xA00000:
                    print(f"  nearby LEA code? @RVA 0x{start+i:X} -> RVA 0x{tgt-base:X}")

    # DayZPlayer vtable dump around 0x380 and 0x770
    world = u64(h, base + 0x4264058)
    near = u64(h, world + 0xF48)
    ent = u64(h, near)
    vt = u64(h, ent)
    print(f"\nent=0x{ent:X} vt=0x{vt:X}")
    for slot in [0x370, 0x378, 0x380, 0x388, 0x390, 0x760, 0x768, 0x770, 0x778, 0x780]:
        fn = u64(h, vt + slot)
        rva = fn - base if base <= fn < base + mi.SizeOfImage else 0
        print(f"  vt+0x{slot:X} = 0x{fn:X} RVA=0x{rva:X}")
        if rva:
            hexdump(rpm(h, fn, 32), fn)

    # Search for "Blood" next to GlobalHealth and find functions comparing both
    gh = img.find(b"GlobalHealth\x00")
    print(f"\nAround GlobalHealth strings:")
    region = img[gh-64:gh+512]
    j = 0
    while j < len(region)-3:
        if 32 <= region[j] <= 126:
            k = j
            while k < len(region) and 32 <= region[k] <= 126:
                k += 1
            if k > j+3 and (k>=len(region) or region[k]==0):
                s = region[j:k].decode()
                print(f"  {j-64:+d}: {s}")
            j = k+1
        else:
            j += 1

    CloseHandle(h)

if __name__ == "__main__":
    main()
