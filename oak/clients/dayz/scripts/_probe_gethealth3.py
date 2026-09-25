#!/usr/bin/env python3
"""Dump GetHealth / GetHealth01 wrappers and find callers."""
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

def hexdump(b, addr):
    for i in range(0, len(b), 16):
        print(f"  {addr+i:X}  " + " ".join(f"{x:02X}" for x in b[i:i+16]))

def main():
    pid = find_pid("DayZ_x64.exe")
    h = OpenProcess(0x0410, False, pid)
    mods = (wintypes.HMODULE * 4)()
    needed = wintypes.DWORD()
    EnumProcessModulesEx(h, mods, ctypes.sizeof(mods), ctypes.byref(needed), 3)
    base = ctypes.cast(mods[0], ctypes.c_void_p).value
    mi = MODULEINFO(); GetModuleInformation(h, mods[0], ctypes.byref(mi), ctypes.sizeof(mi))
    img = rpm(h, base, min(mi.SizeOfImage, 0x6000000))

    targets = {
        "DamageSystem_GetHealth": 0x422920,
        "GetHealth01_native?": 0x923F20,
        "nearby_923FB0": 0x923FB0,
        "nearby_924450": 0x924450,
        "HasDamageSystem?": 0x925E60,
        "GetHealth_reg?": 0x923840,
    }
    for name, rva in targets.items():
        print(f"\n==== {name} RVA 0x{rva:X} ====")
        hexdump(img[rva:rva+96], base+rva)

    # Find E8 calls to 0x422920
    dest = base + 0x422920
    calls = []
    for i in range(0, len(img)-5):
        if img[i] != 0xE8:
            continue
        rel = struct.unpack_from("<i", img, i+1)[0]
        if base + i + 5 + rel == dest:
            calls.append(base + i)
    print(f"\nDirect CALL sites to DamageSystem::GetHealth: {len(calls)}")
    for c in calls[:20]:
        print(f"  call @0x{c:X} RVA=0x{c-base:X}")
        # dump 32 before
        off = c - base
        hexdump(img[max(0,off-40):off+16], base+max(0,off-40))

    # Map GetHealth / GetMaxHealth / GetHealth01 string registration order near 0x92CE87
    print("\n==== Native name table near registration ====")
    for name in [b"GetHealth\x00", b"GetHealth01\x00", b"GetMaxHealth\x00", b"SetHealth\x00", b"HasDamageSystem\x00", b"IsDamageDestroyed\x00"]:
        i = 0
        hits = []
        while True:
            j = img.find(name, i)
            if j < 0:
                break
            hits.append(base+j)
            i = j+1
            if len(hits) > 3:
                break
        print(f"  {name[:-1].decode()}: {[hex(x) for x in hits]}")

    CloseHandle(h)

if __name__ == "__main__":
    main()
