#!/usr/bin/env python3
"""Probe DayZ HealthCalc / GlobalHealth xrefs via RPM."""
import ctypes
from ctypes import wintypes
import struct
import sys

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)

PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400

OpenProcess = kernel32.OpenProcess
OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
OpenProcess.restype = wintypes.HANDLE

ReadProcessMemory = kernel32.ReadProcessMemory
ReadProcessMemory.argtypes = [wintypes.HANDLE, wintypes.LPCVOID, wintypes.LPVOID, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
ReadProcessMemory.restype = wintypes.BOOL

CloseHandle = kernel32.CloseHandle

class MODULEINFO(ctypes.Structure):
    _fields_ = [("lpBaseOfDll", wintypes.LPVOID),
                ("SizeOfImage", wintypes.DWORD),
                ("EntryPoint", wintypes.LPVOID)]

EnumProcessModulesEx = psapi.EnumProcessModulesEx
EnumProcessModulesEx.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.HMODULE), wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), wintypes.DWORD]
EnumProcessModulesEx.restype = wintypes.BOOL

GetModuleInformation = psapi.GetModuleInformation
GetModuleInformation.argtypes = [wintypes.HANDLE, wintypes.HMODULE, ctypes.POINTER(MODULEINFO), wintypes.DWORD]
GetModuleInformation.restype = wintypes.BOOL

def find_pid(name: str) -> int:
    import subprocess
    out = subprocess.check_output(["tasklist", "/FI", f"IMAGENAME eq {name}", "/FO", "CSV", "/NH"], text=True, errors="ignore")
    for line in out.splitlines():
        if name.lower() not in line.lower():
            continue
        parts = [p.strip('"') for p in line.split(",")]
        if len(parts) >= 2:
            return int(parts[1])
    raise SystemExit(f"{name} not running")

def rpm(h, addr, n):
    buf = (ctypes.c_char * n)()
    got = ctypes.c_size_t(0)
    if not ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)):
        return b""
    return bytes(buf[:got.value])

def u64(h, addr):
    b = rpm(h, addr, 8)
    return struct.unpack("<Q", b)[0] if len(b) == 8 else 0

def i32(h, addr):
    b = rpm(h, addr, 4)
    return struct.unpack("<i", b)[0] if len(b) == 4 else 0

def f32(h, addr):
    b = rpm(h, addr, 4)
    return struct.unpack("<f", b)[0] if len(b) == 4 else float("nan")

def hexdump(b, addr):
    for i in range(0, len(b), 16):
        chunk = b[i:i+16]
        hx = " ".join(f"{x:02X}" for x in chunk)
        print(f"  {addr+i:X}  {hx}")

def main():
    pid = find_pid("DayZ_x64.exe")
    h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
    mods = (wintypes.HMODULE * 4)()
    needed = wintypes.DWORD()
    EnumProcessModulesEx(h, mods, ctypes.sizeof(mods), ctypes.byref(needed), 3)
    base = mods[0]
    mi = MODULEINFO()
    GetModuleInformation(h, base, ctypes.byref(mi), ctypes.sizeof(mi))
    base = ctypes.cast(base, ctypes.c_void_p).value
    size = mi.SizeOfImage
    print(f"PID={pid} base=0x{base:X} size=0x{size:X}")

    rvas = {
        "HealthCalc_1": 0x962C0,
        "HealthCalc_6": 0x4197D0,
        "HealthCalc_8": 0x780750,
        "HealthCalc_10": 0x7A08D0,
        "Blood_Func": 0x4AA100,
        "DayZPlayer_GetName": 0x4E5130,
    }
    for name, rva in rvas.items():
        addr = base + rva
        b = rpm(h, addr, 96)
        print(f"\n==== {name} @ 0x{addr:X} RVA 0x{rva:X} ====")
        hexdump(b, addr)

    # Find GlobalHealth strings in image
    img = rpm(h, base, min(size, 0x6000000))
    needle = b"GlobalHealth\x00"
    gh = []
    start = 0
    while True:
        i = img.find(needle, start)
        if i < 0:
            break
        gh.append(base + i)
        start = i + 1
    print(f"\nGlobalHealth strings ({len(gh)}):")
    for a in gh:
        print(f"  0x{a:X}")

    # Scan for RIP-relative LEA to those strings in first 12MB of image
    scan_end = min(len(img), 0xC00000)
    refs = []
    for i in range(0, scan_end - 7):
        b0, b1, b2 = img[i], img[i+1], img[i+2]
        if b0 not in (0x48, 0x4C) or b1 != 0x8D:
            continue
        if (b2 & 0xC7) != 0x05:
            continue
        disp = struct.unpack_from("<i", img, i + 3)[0]
        instr = base + i
        target = instr + 7 + disp
        if target in gh:
            refs.append((instr, instr - base, target, b2))
    print(f"\nLEA refs to GlobalHealth: {len(refs)}")
    for instr, rva, target, modrm in refs[:30]:
        reg = (modrm >> 3) & 7
        print(f"  LEA @0x{instr:X} RVA=0x{rva:X} reg={reg} -> 0x{target:X}")
        # dump -48..+48
        off = rva - 48
        if off < 0:
            off = 0
        ctx = img[off:off+120]
        print("  context:")
        hexdump(ctx, base + off)

    # Also find xref to "Blood\0" near GlobalHealth page
    blood_near = []
    for g in gh:
        page = img[g - base - 0x200:g - base + 0x400]
        # just list nearby cstrings
        pass
    for g in gh[:1]:
        region = img[max(0, g-base-0x100): g-base+0x200]
        print(f"\nNearby strings around first GH:")
        # crude cstring scan
        j = 0
        while j < len(region) - 2:
            if 32 <= region[j] <= 126:
                k = j
                while k < len(region) and 32 <= region[k] <= 126:
                    k += 1
                if k > j + 2 and (k >= len(region) or region[k] == 0):
                    s = region[j:k].decode("ascii", "ignore")
                    if s.isprintable():
                        print(f"  +{j-0x100:+d}: '{s}'")
                j = k + 1
            else:
                j += 1

    # Dump player DM-ish slots + HasDamageSystem-related floats on entity
    world = u64(h, base + 0x4264058)
    near = u64(h, world + 0xF48)
    ent = u64(h, near)
    print(f"\nplayer ent=0x{ent:X}")
    for off in range(0x80, 0x820, 8):
        p = u64(h, ent + off)
        if not (0x100000000 < p < 0x7FFFFFFFFFFF):
            continue
        if base <= p < base + size:
            continue
        f10 = f32(h, p + 0x10)
        if 0.01 < f10 <= 1.05:
            print(f"  ent+0x{off:X} -> 0x{p:X} f10={f10:.3f}")

    CloseHandle(h)

if __name__ == "__main__":
    main()
