#!/usr/bin/env python3
"""Find DamageSystem::GetHealth via error string xrefs and dump candidate GetHealth funcs."""
import ctypes
from ctypes import wintypes
import struct
import subprocess

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
        if name.lower() not in line.lower():
            continue
        parts = [p.strip('"') for p in line.split(",")]
        return int(parts[1])
    raise SystemExit("not running")

def rpm(h, addr, n):
    buf = (ctypes.c_char * n)()
    got = ctypes.c_size_t(0)
    if not ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)):
        return b""
    return bytes(buf[:got.value])

def hexdump(b, addr):
    for i in range(0, len(b), 16):
        chunk = b[i:i+16]
        print(f"  {addr+i:X}  " + " ".join(f"{x:02X}" for x in chunk))

def find_cstr(img, base, s):
    needle = s.encode("ascii") + b"\x00"
    out = []
    start = 0
    while True:
        i = img.find(needle, start)
        if i < 0:
            break
        out.append(base + i)
        start = i + 1
    return out

def lea_refs(img, base, targets, scan_limit=None):
    if scan_limit is None:
        scan_limit = len(img)
    scan_limit = min(scan_limit, len(img))
    refs = []
    tgt = set(targets)
    for i in range(0, scan_limit - 7):
        b0, b1, b2 = img[i], img[i+1], img[i+2]
        if b0 not in (0x48, 0x4C) or b1 != 0x8D:
            continue
        if (b2 & 0xC7) != 0x05:
            continue
        disp = struct.unpack_from("<i", img, i + 3)[0]
        instr = base + i
        target = instr + 7 + disp
        if target in tgt:
            refs.append((instr, i, target))
    return refs

def find_func_start(img, offset, max_back=0x400):
    # walk back for common prologues / INT3 padding
    start = max(0, offset - max_back)
    # prefer CC CC CC then non-CC, or 40 53 / 48 89 5C / 48 83 EC / 55 48 8B EC
    for i in range(offset, start, -1):
        if i >= 5 and img[i-1] == 0xCC and img[i] != 0xCC:
            return i
        if i >= 4 and img[i:i+3] in (b"\x40\x53\x48", b"\x48\x89\x5C", b"\x48\x83\xEC", b"\x55\x48\x8B") and (i == 0 or img[i-1] in (0xCC, 0xC3, 0xC2)):
            return i
    return max(0, offset - 0x80)

def main():
    pid = find_pid("DayZ_x64.exe")
    h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
    mods = (wintypes.HMODULE * 4)()
    needed = wintypes.DWORD()
    EnumProcessModulesEx(h, mods, ctypes.sizeof(mods), ctypes.byref(needed), 3)
    base = ctypes.cast(mods[0], ctypes.c_void_p).value
    mi = MODULEINFO()
    GetModuleInformation(h, mods[0], ctypes.byref(mi), ctypes.sizeof(mi))
    size = mi.SizeOfImage
    print(f"PID={pid} base=0x{base:X} size=0x{size:X}")
    img = rpm(h, base, min(size, 0x6000000))

    needles = [
        "[DamageSystem::GetHealth]",
        "Could not find health type",
        "Could not find global health type",
        "DamageSystem::GetHealth",
        "HasDamageSystem",
        "GetHealth01",
    ]
    for n in needles:
        addrs = find_cstr(img, base, n)
        print(f"\n'{n}' -> {len(addrs)} hits")
        for a in addrs[:5]:
            print(f"  0x{a:X}")
            refs = lea_refs(img, base, [a], scan_limit=0xC00000)
            # also scan full image in chunks for refs if none in first 12MB
            if not refs:
                refs = lea_refs(img, base, [a])
            print(f"  LEA refs: {len(refs)}")
            for instr, off, tgt in refs[:8]:
                fs = find_func_start(img, off)
                print(f"    ref@0x{instr:X} RVA=0x{instr-base:X} funcStart~RVA=0x{fs:X}")
                # dump func start
                hexdump(img[fs:fs+96], base + fs)
                # dump around ref
                print("    around ref:")
                a0 = max(0, off - 32)
                hexdump(img[a0:off+48], base + a0)

    # Specifically analyze first GlobalHealth-using function cluster around 0x421900
    print("\n==== Candidate GetHealth near 0x4218F0 ====")
    rva = 0x4218F0
    # find start
    fs = find_func_start(img, rva)
    print(f"funcStart RVA=0x{fs:X}")
    hexdump(img[fs:fs+160], base + fs)

    # Look for MOVSS XMM0 returns near GlobalHealth compare functions — find CALLERS of strcmp after GH lea
    # Decode call target after GH lea at 0x42193F: E8 D2 80 D1 FF
    def decode_call(off):
        if img[off] != 0xE8:
            return None
        rel = struct.unpack_from("<i", img, off + 1)[0]
        return base + off + 5 + rel

    for rva_lea in [0x42193F, 0x4226EE, 0x42295E, 0x42654E, 0x427B32, 0x427F5E, 0x817D5E]:
        # after lea is often mov rcx; call
        off = rva_lea
        # find E8 within next 16 bytes
        for j in range(off, off + 20):
            if img[j] == 0xE8:
                tgt = decode_call(j)
                print(f"LEA@0x{rva_lea:X} call@0x{j:X} -> 0x{tgt:X} RVA=0x{tgt-base:X}")
                break

    # Find vtable entries? Search for functions that take 3 string args pattern.
    # Dump HasDamageSystem if found
    hd = find_cstr(img, base, "HasDamageSystem")
    print(f"\nHasDamageSystem strings: {hd}")

    CloseHandle(h)

if __name__ == "__main__":
    main()
