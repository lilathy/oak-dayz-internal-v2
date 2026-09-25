#!/usr/bin/env python3
"""
Oak DayZ — stable future-proof offset dumper (v2)

Design goals:
  1) Never write false World/list hits (fail closed).
  2) Resolve every key Oak needs (runtime_offsets.cpp catalog + critical natives).
  3) Survive game updates via: RIP-xref World discovery + multi-validator scoring.
  4) Mark each key RESOLVED / VALIDATED / UNRESOLVED (no silent KEPT unless --keep-old).

Usage:
  python dump_oak_offsets.py --live --apply
  powershell -File dump_offsets.ps1 -Live -Apply

Requires DayZ_x64 running and preferably SPAWNED in-world (non-zero camera/body pos).
"""
from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import datetime as dt
import math
import os
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

# ---------------------------------------------------------------------------
# WinAPI
# ---------------------------------------------------------------------------
KERNEL32 = ctypes.WinDLL("kernel32", use_last_error=True)
PSAPI = ctypes.WinDLL("psapi", use_last_error=True)

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
TH32CS_SNAPPROCESS = 0x00000002
MEM_COMMIT = 0x1000
PAGE_NOACCESS = 0x01
PAGE_GUARD = 0x100
IMAGE_SCN_MEM_EXECUTE = 0x20000000
IMAGE_SCN_MEM_WRITE = 0x80000000

PSAPI.EnumProcessModulesEx.argtypes = [
    wt.HANDLE, ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD), wt.DWORD
]
PSAPI.EnumProcessModulesEx.restype = wt.BOOL
PSAPI.GetModuleInformation.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, wt.DWORD]
PSAPI.GetModuleInformation.restype = wt.BOOL
KERNEL32.ReadProcessMemory.argtypes = [
    wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)
]
KERNEL32.ReadProcessMemory.restype = wt.BOOL
KERNEL32.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
KERNEL32.VirtualQueryEx.restype = ctypes.c_size_t
KERNEL32.OpenProcess.restype = wt.HANDLE

DayZExeDefault = Path(r"C:\Program Files (x86)\Steam\steamapps\common\DayZ\DayZ_x64.exe")
RepoDayz = Path(__file__).resolve().parents[1]
OutGenerated = RepoDayz / "src" / "offsets_generated.hpp"
OutOffsetsTxt = RepoDayz / "offsets.txt"
OutNatives = RepoDayz / "docs" / "engine" / "NATIVE_RVAS.txt"

# Oak-needed catalog (= runtime_offsets.cpp bindings + inventory/modbase extras)
OAK_KEYS: dict[str, int] = {
    "modbase.World": 0x4262FE8,
    "modbase.FOV_Context": 0x1007C70,
    "modbase.FovBase": 0x9C4,
    "modbase.Network": 0x100EB60,
    "modbase.NetworkManager": 0x100EBD0,
    "modbase.Tick": 0xFF3928,
    "modbase.ScopeFovCtx": 0x42638B0,
    "modbase.PhysicsWorld": 0x42662E0,
    "world.Camera": 0x1B8,
    "world.LocalPlayer": 0x2960,
    "world.PlayerOn": 0x2968,
    "world.NearEntList": 0xF48,
    "world.FarEntList": 0x1090,
    "world.FarTableSize": 0x1098,
    "world.SlowEntList": 0x2010,
    "world.SlowTableSize": 0x2018,
    "world.BulletList": 0xE00,
    "world.BulletCount": 0xE08,
    "world.ItemList": 0x2060,
    "world.ItemListSize": 0x2068,
    "world.NoGrass": 0xC00,
    "world.GrassOffline": 0xBF0,
    "world.EyeAccom": 0x296C,
    "world.Hour": 0x2970,
    "world.Day": 0x2974,
    "world.DayTime": 0x2978,
    "world.WeatherController": 0x7198,
    "world.SlowEntValidCount": 0x1F90,
    "camera.InvertedViewRight": 0x8,
    "camera.InvertedViewUp": 0x14,
    "camera.InvertedViewForward": 0x20,
    "camera.InvertedViewTranslation": 0x2C,
    "camera.ViewPortSize": 0x58,
    "camera.GetProjectionD1": 0xD0,
    "camera.GetProjectionD2": 0xDC,
    "entity.Type": 0x180,
    "entity.VisualState": 0x1C8,
    "entity.FutureVisualState": 0x120,
    "entity.IsDead": 0xE2,
    "entity.NetworkId": 0x6DC,
    "entity.NetworkIdPlayer": 0x6E4,
    "entity.Stamina": 0x6A4,
    "inventory.Hands": 0x1B0,
    "player.Skeleton": 0x7E0,
    "player.Inventory": 0x650,
    "player.InputController": 0x7E8,
    "player.StatsContainer": 0x6F0,
    "player.RecordValue": 0x2C,
    "player.DamageManager": 0x188,
    "network.ScoreboardSize": 0x24,
    "network.ThirdPersonFlag": 0x9C,
    "network.ManagerNetworkClient": 0x50,
    "network.PlayerName": 0xF8,
    "network.ScoreboardPtr": 0x18,
    "network.IdentityCount": 0x1C,
    "scoreboard_identity.NetworkId": 0x30,
    "scoreboard_identity.SteamId": 0xA0,
    "scoreboard_identity.Name": 0xF8,
    "player_identity.Name": 0xB0,
    "entity_owner.Owner": 0xA0,
    "infected.Skeleton": 0x670,
    "entitytype.TypeName": 0x98,
    "entitytype.ConfigName": 0xD0,
    "entitytype.ModelName": 0xB0,
    "anim.MatrixArray": 0xBE8,
    "anim.MatrixB": 0x54,
    "anim.AnimComponent": 0x118,
    "skeleton.AnimClass1": 0x98,
    "skeleton.AnimClass2": 0x28,
    "skeleton.AnimComponent": 0x118,
}

NATIVE_STRINGS = [
    "Is3rdPersonDisabled",
    "IsCrosshairDisabled",
    "CameraViewChanged",
]


@dataclass
class Hit:
    key: str
    value: int
    status: str  # RESOLVED | VALIDATED | UNRESOLVED | KEPT
    note: str = ""
    confidence: int = 0  # 0..100


@dataclass
class PeImage:
    data: bytes
    secs: list[tuple[bytes, int, int, int, int, int]]  # name,va,vs,raw,rs,chars

    def rva_to_off(self, rva: int) -> int | None:
        for _n, va, vs, raw, rs, _c in self.secs:
            if va <= rva < va + max(vs, rs, 1):
                return raw + (rva - va)
        return None

    def off_to_rva(self, off: int) -> int | None:
        for _n, va, vs, raw, rs, _c in self.secs:
            if raw <= off < raw + rs:
                return va + (off - raw)
        return None

    def is_writable_rva(self, rva: int) -> bool:
        for _n, va, vs, _raw, _rs, chars in self.secs:
            if va <= rva < va + max(vs, 1):
                return (chars & IMAGE_SCN_MEM_WRITE) != 0
        return False


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
        ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)), ("th32ModuleID", wt.DWORD),
        ("cntThreads", wt.DWORD), ("th32ParentProcessID", wt.DWORD),
        ("pcPriClassBase", ctypes.c_long), ("dwFlags", wt.DWORD),
        ("szExeFile", ctypes.c_wchar * 260),
    ]


class MODULEINFO(ctypes.Structure):
    _fields_ = [
        ("lpBaseOfDll", ctypes.c_void_p),
        ("SizeOfImage", wt.DWORD),
        ("EntryPoint", ctypes.c_void_p),
    ]


class MEMORY_BASIC_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p),
        ("AllocationProtect", wt.DWORD), ("PartitionId", wt.WORD),
        ("RegionSize", ctypes.c_size_t), ("State", wt.DWORD),
        ("Protect", wt.DWORD), ("Type", wt.DWORD),
    ]


def load_pe(path: Path) -> PeImage:
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    num = struct.unpack_from("<H", data, pe + 6)[0]
    opt = struct.unpack_from("<H", data, pe + 20)[0]
    sec_off = pe + 24 + opt
    secs = []
    for i in range(num):
        o = sec_off + i * 40
        name = data[o : o + 8].split(b"\0", 1)[0]
        vs, va, rs, raw = struct.unpack_from("<IIII", data, o + 8)
        chars = struct.unpack_from("<I", data, o + 36)[0]
        secs.append((name, va, vs, raw, rs, chars))
    return PeImage(data, secs)


def find_pid(name: str = "DayZ_x64.exe") -> int:
    snap = KERNEL32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    pe = PROCESSENTRY32W()
    pe.dwSize = ctypes.sizeof(pe)
    if not KERNEL32.Process32FirstW(snap, ctypes.byref(pe)):
        KERNEL32.CloseHandle(snap)
        return 0
    pid = 0
    while True:
        if pe.szExeFile.lower() == name.lower():
            pid = int(pe.th32ProcessID)
            break
        if not KERNEL32.Process32NextW(snap, ctypes.byref(pe)):
            break
    KERNEL32.CloseHandle(snap)
    return pid


def open_process(pid: int):
    return KERNEL32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)


def rpm(h, addr: int, n: int) -> bytes | None:
    buf = (ctypes.c_ubyte * n)()
    got = ctypes.c_size_t(0)
    if not KERNEL32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)):
        return None
    if got.value != n:
        return None
    return bytes(buf)


def rpm_u64(h, addr: int) -> int | None:
    b = rpm(h, addr, 8)
    return struct.unpack("<Q", b)[0] if b else None


def rpm_u32(h, addr: int) -> int | None:
    b = rpm(h, addr, 4)
    return struct.unpack("<I", b)[0] if b else None


def rpm_f32(h, addr: int) -> float | None:
    b = rpm(h, addr, 4)
    return struct.unpack("<f", b)[0] if b else None


def rpm_vec3(h, addr: int) -> tuple[float, float, float] | None:
    b = rpm(h, addr, 12)
    if not b:
        return None
    return struct.unpack("<fff", b)


def is_user_ptr(p: int) -> bool:
    return 0x10000 <= p < 0x00007FFFFFFFFFFF


def committed(h, p: int) -> bool:
    if not is_user_ptr(p):
        return False
    mbi = MEMORY_BASIC_INFORMATION()
    if not KERNEL32.VirtualQueryEx(h, ctypes.c_void_p(p), ctypes.byref(mbi), ctypes.sizeof(mbi)):
        return False
    if mbi.State != MEM_COMMIT:
        return False
    if mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD):
        return False
    return True


def module_base_size(h) -> tuple[int, int]:
    mods = (ctypes.c_void_p * 1024)()
    needed = wt.DWORD()
    if not PSAPI.EnumProcessModulesEx(h, ctypes.byref(mods), ctypes.sizeof(mods), ctypes.byref(needed), 0x03):
        return 0, 0
    info = MODULEINFO()
    if not PSAPI.GetModuleInformation(h, ctypes.c_void_p(mods[0]), ctypes.byref(info), ctypes.sizeof(info)):
        return 0, 0
    return int(info.lpBaseOfDll or 0), int(info.SizeOfImage or 0)


def finite(f: float | None) -> bool:
    return f is not None and math.isfinite(f) and abs(f) < 1.0e8


def sane_world_pos(v: tuple[float, float, float] | None, allow_origin: bool = False) -> bool:
    if not v or not all(finite(x) for x in v):
        return False
    x, y, z = v
    if not allow_origin and abs(x) < 0.5 and abs(y) < 0.5 and abs(z) < 0.5:
        return False
    # Chernarus / Livonia-ish bounds (loose)
    if abs(x) > 60000 or abs(z) > 60000:
        return False
    if y < -100 or y > 2500:
        return False
    return True


def dist3(a: tuple[float, float, float], b: tuple[float, float, float]) -> float:
    return math.sqrt((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2 + (a[2] - b[2]) ** 2)


def looks_like_camera(h, cam: int) -> bool:
    if not committed(h, cam) or not committed(h, cam + 0xE0):
        return False
    # basis + translation block
    for off in (0x8, 0x14, 0x20, 0x2C):
        v = rpm_vec3(h, cam + off)
        if not v or not all(finite(x) for x in v):
            return False
    # right/up/forward should be roughly unit-ish (not all zero)
    right = rpm_vec3(h, cam + 0x8)
    assert right
    rn = math.sqrt(right[0] ** 2 + right[1] ** 2 + right[2] ** 2)
    if rn < 0.5 or rn > 1.5:
        return False
    proj = rpm_f32(h, cam + 0xD0)
    if not finite(proj) or abs(proj) < 1e-6:
        return False
    return True


def entity_pos(h, ent: int, vs_off: int = 0x1C8) -> tuple[float, float, float] | None:
    if not committed(h, ent):
        return None
    vs = rpm_u64(h, ent + vs_off)
    if not vs or not committed(h, vs):
        return None
    return rpm_vec3(h, vs + 0x2C)


def table_contains_ptr(h, table: int, count: int, needle: int, max_check: int = 256) -> bool:
    if not table or count <= 0:
        return False
    n = min(count, max_check)
    for i in range(n):
        e = rpm_u64(h, table + i * 8)
        if e == needle:
            return True
    return False


def find_gate_by_field(pe: PeImage, field_imm: int) -> int | None:
    needle = bytes([0x83, 0x78, field_imm & 0xFF, 0x00, 0x74, 0x07, 0xB0, 0x01])
    start = 0
    hits: list[int] = []
    while True:
        i = pe.data.find(needle, start)
        if i < 0:
            break
        for back in range(0, 0x30):
            if pe.data[i - back : i - back + 4] == b"\x48\x83\xEC\x28":
                r = pe.off_to_rva(i - back)
                if r is not None:
                    hits.append(r)
                break
        start = i + 1
    return hits[0] if len(hits) == 1 else None


def find_native_handler(pe: PeImage, name: str) -> int | None:
    # Prefer unique C++ gate bodies for patch targets (string regs can wrap elsewhere).
    if name == "Is3rdPersonDisabled":
        g = find_gate_by_field(pe, 0x74)
        if g is not None:
            return g
    if name == "IsCrosshairDisabled":
        g = find_gate_by_field(pe, 0x78)
        if g is not None:
            return g

    needle = name.encode("ascii") + b"\0"
    off = pe.data.find(needle)
    if off < 0:
        off = pe.data.find(name.encode("ascii"))
        if off < 0:
            return None
    str_rva = pe.off_to_rva(off)
    if str_rva is None:
        return None
    text = next((s for s in pe.secs if s[0] == b".text"), None)
    if not text:
        return None
    _n, _va, _vs, raw, rs, _c = text
    for i in range(raw, raw + rs - 7):
        b0, b1, b2 = pe.data[i], pe.data[i + 1], pe.data[i + 2]
        if b0 not in (0x48, 0x4C) or b1 != 0x8D:
            continue
        if b2 not in (0x05, 0x0D, 0x15, 0x1D, 0x25, 0x2D, 0x35, 0x3D):
            continue
        disp = struct.unpack_from("<i", pe.data, i + 3)[0]
        instr = pe.off_to_rva(i)
        if instr is None or instr + 7 + disp != str_rva:
            continue
        back = max(raw, i - 0x40)
        for j in range(back, i - 6):
            if pe.data[j : j + 3] != b"\x4C\x8D\x0D":
                continue
            hinstr = pe.off_to_rva(j)
            hdisp = struct.unpack_from("<i", pe.data, j + 3)[0]
            handler = hinstr + 7 + hdisp
            if pe.rva_to_off(handler) is not None:
                return handler
    return None


def collect_rip_targets(pe: PeImage) -> dict[int, int]:
    """Map writable RVA -> xref count from .text mov/lea rip-rel."""
    counts: dict[int, int] = {}
    prefs = (
        b"\x48\x8B\x05", b"\x48\x8B\x0D", b"\x48\x8B\x15", b"\x48\x8B\x1D", b"\x48\x8B\x3D",
        b"\x4C\x8B\x05", b"\x4C\x8B\x0D", b"\x4C\x8B\x15", b"\x4C\x8B\x1D",
    )
    for _n, _va, _vs, raw, rs, chars in pe.secs:
        if not (chars & IMAGE_SCN_MEM_EXECUTE):
            continue
        data = pe.data
        end = raw + rs - 7
        i = raw
        while i < end:
            chunk = data[i : i + 3]
            if chunk in prefs:
                disp = struct.unpack_from("<i", data, i + 3)[0]
                instr = pe.off_to_rva(i)
                if instr is not None:
                    tgt = instr + 7 + disp
                    if pe.is_writable_rva(tgt):
                        counts[tgt] = counts.get(tgt, 0) + 1
                i += 7
                continue
            i += 1
    return counts


@dataclass
class WorldEval:
    rva: int
    ptr: int
    xrefs: int
    score: int
    fields: dict[str, int]
    cam_pos: tuple[float, float, float] | None = None
    lp_pos: tuple[float, float, float] | None = None
    reasons: list[str] | None = None


def evaluate_world(h, base: int, size: int, rva: int, xrefs: int) -> WorldEval | None:
    world = rpm_u64(h, base + rva)
    if not world or not is_user_ptr(world) or (base <= world < base + size):
        return None
    if not committed(h, world) or not committed(h, world + 0x3000):
        return None

    reasons: list[str] = []
    fields: dict[str, int] = {}
    score = 0

    # --- Camera ---
    cam = None
    cam_off = 0
    for off in (0x1B8, 0x1A8, 0x1C0, 0x1B0, 0x1C8, 0x200):
        c = rpm_u64(h, world + off)
        if c and not (base <= c < base + size) and looks_like_camera(h, c):
            cam = c
            cam_off = off
            break
    if not cam:
        return None
    fields["world.Camera"] = cam_off
    score += 30
    cam_pos = rpm_vec3(h, cam + 0x2C)
    if sane_world_pos(cam_pos):
        score += 40
        reasons.append("cam_pos_ok")
    else:
        # Fail closed for APPLY-quality: zero/garbage camera = menu or false World
        reasons.append("cam_pos_bad")
        score -= 20

    # --- Local player ---
    lp = None
    lp_off = 0
    lp_pos = None
    cand_lp = [0x2960, 0x28D0, 0x28D8, 0x2958, 0x2940]
    cand_lp += list(range(0x2880, 0x2A40, 8))
    for off in cand_lp:
        ent = rpm_u64(h, world + off)
        if not ent or not is_user_ptr(ent) or (base <= ent < base + size) or not committed(h, ent):
            continue
        pos = entity_pos(h, ent, 0x1C8)
        if not sane_world_pos(pos):
            # allow scanning further; don't accept zero-pos as local
            continue
        # Prefer proximity to camera
        if cam_pos and sane_world_pos(cam_pos) and dist3(pos, cam_pos) < 80.0:
            lp, lp_off, lp_pos = ent, off, pos
            score += 50
            reasons.append(f"lp_near_cam@{off:X}")
            break
        if lp is None:
            lp, lp_off, lp_pos = ent, off, pos
            score += 20
            reasons.append(f"lp_pos_ok@{off:X}")
    if not lp:
        reasons.append("no_local")
        score -= 30
    else:
        fields["world.LocalPlayer"] = lp_off
        fields["world.PlayerOn"] = lp_off + 8
        fields["entity.VisualState"] = 0x1C8
        # classic time/eye cluster relative to OLD local; also try lp+0xC/+0x10
        for key, delta in (("world.EyeAccom", 0xC), ("world.Hour", 0x10), ("world.Day", 0x14), ("world.DayTime", 0x18)):
            fields[key] = lp_off + delta

    # --- Entity lists: require same heap niche + optional contains local ---
    list_specs = [
        ("world.NearEntList", 0xF48, "world.NearTableSize", 0xF50, 5000),
        ("world.FarEntList", 0x1090, "world.FarTableSize", 0x1098, 20000),
        ("world.SlowEntList", 0x2010, "world.SlowTableSize", 0x2018, 20000),
        ("world.BulletList", 0xE00, "world.BulletCount", 0xE08, 5000),
        ("world.ItemList", 0x2060, "world.ItemListSize", 0x2068, 20000),
    ]
    lists_ok = 0
    contains_lp = False
    for pk, po, ck, co, mx in list_specs:
        p = rpm_u64(h, world + po)
        c = rpm_u32(h, world + co)
        if p is None or c is None:
            continue
        if not is_user_ptr(p) or (base <= p < base + size) or not committed(h, p):
            continue
        if (p >> 40) != (world >> 40):
            continue
        if not (0 <= c < mx):
            continue
        # Peek first entry: should look like entity with VS when count>0
        if c > 0:
            e0 = rpm_u64(h, p)
            if not e0 or not committed(h, e0):
                continue
            if not entity_pos(h, e0):
                # still accept empty-ish structure if count==0 path; here count>0 so require VS
                continue
        fields[pk] = po
        fields[ck] = co
        lists_ok += 1
        score += 12
        if lp and c > 0 and table_contains_ptr(h, p, c, lp):
            contains_lp = True
            score += 40
            reasons.append(f"list_has_lp@{po:X}")

    if lists_ok == 0:
        score -= 25
        reasons.append("no_lists")

    # xref bonus
    score += min(xrefs, 40)

    # Hard reject: no camera, or (no local AND no lists)
    if "cam_pos_bad" in reasons and "no_local" in reasons:
        return None
    if score < 80:
        return None

    return WorldEval(rva, world, xrefs, score, fields, cam_pos, lp_pos, reasons)


def discover_world(h, base: int, size: int, pe: PeImage) -> WorldEval | None:
    print("[*] collecting RIP-relative writable targets...")
    counts = collect_rip_targets(pe)
    print(f"[+] unique rip targets: {len(counts)}")

    # Seed with prior defaults / neighbors
    seeds = [
        OAK_KEYS["modbase.World"],
        OAK_KEYS["modbase.World"] - 0x2000,
        OAK_KEYS["modbase.World"] + 0x2000,
        0xFF2540,
        0x1007900,
        OAK_KEYS["modbase.ScopeFovCtx"],
        OAK_KEYS["modbase.PhysicsWorld"],
    ]
    for r in seeds:
        counts.setdefault(r, 0)

    # Evaluate high-xref first
    ranked = sorted(counts.items(), key=lambda kv: (-kv[1], kv[0]))
    best: WorldEval | None = None
    checked = 0
    for rva, xref in ranked:
        if rva < 0x1000 or rva + 8 > size:
            continue
        if not pe.is_writable_rva(rva) and rva not in seeds:
            continue
        ev = evaluate_world(h, base, size, rva, xref)
        checked += 1
        if not ev:
            continue
        if best is None or ev.score > best.score or (ev.score == best.score and ev.xrefs > best.xrefs):
            best = ev
            print(f"  candidate RVA=0x{rva:X} score={ev.score} xrefs={ev.xrefs} reasons={ev.reasons}")
        # Early exit on excellent hit
        if ev.score >= 180 and "cam_pos_ok" in (ev.reasons or []) and "list_has_lp" in (ev.reasons or []):
            break
        if checked > 8000 and best and best.score >= 140:
            break

    print(f"[*] evaluated {checked} candidates")
    return best


def validate_entity_fields(h, lp: int) -> dict[str, Hit]:
    out: dict[str, Hit] = {}
    checks = [
        ("entity.Type", 0x180, True),
        ("entity.VisualState", 0x1C8, True),
        ("entity.FutureVisualState", 0x120, True),
        ("player.Inventory", 0x650, True),
        ("player.Skeleton", 0x7E0, True),
        ("player.InputController", 0x7E8, True),
        ("player.StatsContainer", 0x6F0, True),
        ("player.DamageManager", 0x188, True),
        ("entity.NetworkId", 0x6DC, False),
        ("entity.Stamina", 0x6A4, False),
        ("inventory.Hands", 0x1B0, True),  # via inventory object — validated loosely
    ]
    # inventory.Hands is on inventory object
    inv = rpm_u64(h, lp + 0x650)
    for key, off, as_ptr in checks:
        if key == "inventory.Hands":
            if inv and committed(h, inv):
                hands = rpm_u64(h, inv + off)
                if hands is not None:
                    out[key] = Hit(key, off, "VALIDATED", "inv+Hands readable", 70)
            continue
        if as_ptr:
            p = rpm_u64(h, lp + off)
            if p and is_user_ptr(p) and committed(h, p):
                out[key] = Hit(key, off, "VALIDATED", "local ptr ok", 80)
        else:
            if rpm(h, lp + off, 4):
                out[key] = Hit(key, off, "VALIDATED", "local readable", 60)
    if rpm(h, lp + 0xE2, 1):
        out["entity.IsDead"] = Hit("entity.IsDead", 0xE2, "VALIDATED", "byte readable", 60)
    return out


def validate_camera_fields(h, cam: int) -> dict[str, Hit]:
    out: dict[str, Hit] = {}
    if not looks_like_camera(h, cam):
        return out
    for key, off in (
        ("camera.InvertedViewRight", 0x8),
        ("camera.InvertedViewUp", 0x14),
        ("camera.InvertedViewForward", 0x20),
        ("camera.InvertedViewTranslation", 0x2C),
        ("camera.ViewPortSize", 0x58),
        ("camera.GetProjectionD1", 0xD0),
        ("camera.GetProjectionD2", 0xDC),
    ):
        out[key] = Hit(key, off, "VALIDATED", "live camera", 90)
    return out


def validate_globals(h, base: int, size: int) -> dict[str, Hit]:
    out: dict[str, Hit] = {}
    for key in (
        "modbase.Network",
        "modbase.NetworkManager",
        "modbase.PhysicsWorld",
        "modbase.Tick",
        "modbase.FOV_Context",
    ):
        rva = OAK_KEYS[key]
        if rva + 8 > size:
            continue
        obj = rpm_u64(h, base + rva)
        if obj and is_user_ptr(obj) and committed(h, obj) and not (base <= obj < base + size):
            out[key] = Hit(key, rva, "VALIDATED", "live global ptr", 75)
    if "modbase.Network" in out:
        net = rpm_u64(h, base + out["modbase.Network"].value)
        if net and rpm(h, net + 0x9C, 1):
            out["network.ThirdPersonFlag"] = Hit("network.ThirdPersonFlag", 0x9C, "VALIDATED", "", 70)
            for key, off in (
                ("network.ScoreboardPtr", 0x18),
                ("network.IdentityCount", 0x1C),
                ("network.ScoreboardSize", 0x24),
                ("network.PlayerName", 0xF8),
                ("network.ManagerNetworkClient", 0x50),
            ):
                if rpm(h, net + off, 4):
                    out[key] = Hit(key, off, "VALIDATED", "net field readable", 55)
    # FovBase is a small struct offset constant
    out["modbase.FovBase"] = Hit("modbase.FovBase", OAK_KEYS["modbase.FovBase"], "VALIDATED", "stable scalar", 50)
    return out


def write_generated_hpp(path: Path, hits: dict[str, Hit], stamp: str, product: str) -> None:
    groups: dict[str, list[tuple[str, int]]] = {}
    for key, hit in hits.items():
        if "." not in key or hit.status == "UNRESOLVED":
            continue
        ns, name = key.split(".", 1)
        groups.setdefault(ns, []).append((name, hit.value))
    order = [
        "modbase", "world", "camera", "entity", "inventory", "player", "network",
        "scoreboard_identity", "player_identity", "entity_owner", "infected",
        "entitytype", "anim", "skeleton",
    ]
    lines = [
        "#pragma once",
        f"// Auto-generated by dump_oak_offsets.py v2 — {stamp}",
        f"// DayZ {product}",
        "// Only RESOLVED/VALIDATED/KEPT keys; UNRESOLVED omitted (kept prior via --keep-old).",
        "",
        "#if defined(OAK_REQUIRE_PROTECTION)",
        "#define OAK_RUNTIME_OFFSET(name, developmentValue) inline uintptr_t name = developmentValue",
        "#else",
        "#define OAK_RUNTIME_OFFSET(name, developmentValue) inline uintptr_t name = developmentValue",
        "#endif",
        "",
        "namespace oak_offsets {",
    ]
    for ns in order:
        if ns not in groups:
            continue
        lines.append(f"namespace {ns} {{")
        for name, val in sorted(groups[ns], key=lambda t: t[0].lower()):
            lines.append(f"    OAK_RUNTIME_OFFSET({name}, 0x{val:X});")
        lines.append("}")
    lines += ["}", "", "#undef OAK_RUNTIME_OFFSET", ""]
    path.write_text("\n".join(lines), encoding="utf-8")


def write_report(path: Path, hits: dict[str, Hit], natives: dict[str, int], stamp: str, product: str, world: WorldEval | None) -> None:
    lines = [
        f"[OAK-DUMP-v2] === {stamp} product={product} ===",
        f"[OAK-DUMP-v2] keys={len(hits)} natives={len(natives)}",
    ]
    if world:
        lines.append(
            f"[OAK-DUMP-v2] World RVA=0x{world.rva:X} ptr=0x{world.ptr:X} score={world.score} "
            f"xrefs={world.xrefs} cam={world.cam_pos} lp={world.lp_pos} reasons={world.reasons}"
        )
    lines.append("")
    for key in sorted(hits.keys()):
        h = hits[key]
        lines.append(f"[{h.status:10}] conf={h.confidence:3d} {key:40} -> 0x{h.value:X}  {h.note}".rstrip())
    lines.append("")
    lines.append("[OAK-DUMP-v2] === natives ===")
    for n, rva in sorted(natives.items()):
        lines.append(f"[NATIVE   ] {n:40} -> 0x{rva:X}")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", type=Path, default=DayZExeDefault)
    ap.add_argument("--live", action="store_true")
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--keep-old", action="store_true", default=False,
                    help="Fill UNRESOLVED with prior OAK_KEYS defaults (marked KEPT)")
    ap.add_argument("--allow-menu", action="store_true",
                    help="Allow dump when camera/body at origin (NOT recommended)")
    args = ap.parse_args()

    if not args.exe.exists():
        print(f"[!] missing {args.exe}", file=sys.stderr)
        return 1

    pe = load_pe(args.exe)
    product = dt.datetime.fromtimestamp(args.exe.stat().st_mtime).isoformat(sep=" ", timespec="seconds")
    stamp = dt.datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    hits: dict[str, Hit] = {}
    natives: dict[str, int] = {}

    print(f"[*] PE size={len(pe.data)} product={product}")
    for name in NATIVE_STRINGS:
        hnd = find_native_handler(pe, name)
        if hnd is not None:
            natives[name] = hnd
            print(f"[+] native {name} -> 0x{hnd:X}")
        else:
            print(f"[!] native {name} UNRESOLVED")

    world_ev: WorldEval | None = None

    if args.live:
        pid = find_pid()
        if not pid:
            print("[!] DayZ_x64.exe not running", file=sys.stderr)
            return 2
        h = open_process(pid)
        if not h:
            print("[!] OpenProcess failed (elevate?)", file=sys.stderr)
            return 3
        base, size = module_base_size(h)
        print(f"[+] live pid={pid} base=0x{base:X} size=0x{size:X}")

        world_ev = discover_world(h, base, size, pe)
        if not world_ev:
            print("[!] No trustworthy World candidate. Spawn in-world and retry.", file=sys.stderr)
            KERNEL32.CloseHandle(h)
            return 5

        # Quality gate
        cam_ok = sane_world_pos(world_ev.cam_pos, allow_origin=args.allow_menu)
        lp_ok = sane_world_pos(world_ev.lp_pos, allow_origin=args.allow_menu)
        if not cam_ok or not lp_ok:
            print(
                f"[!] World found but positions look like menu/false positive "
                f"(cam={world_ev.cam_pos} lp={world_ev.lp_pos}). "
                f"Spawn in-world or pass --allow-menu (discouraged).",
                file=sys.stderr,
            )
            if not args.allow_menu:
                print(f"[!] best rejected RVA=0x{world_ev.rva:X} score={world_ev.score} reasons={world_ev.reasons}")
                KERNEL32.CloseHandle(h)
                return 6

        conf = min(100, world_ev.score)
        hits["modbase.World"] = Hit("modbase.World", world_ev.rva, "RESOLVED",
                                    f"ptr=0x{world_ev.ptr:X} xrefs={world_ev.xrefs}", conf)
        print(f"[+] World RVA=0x{world_ev.rva:X} score={world_ev.score} xrefs={world_ev.xrefs}")

        for k, v in world_ev.fields.items():
            # Skip NearTableSize if not in OAK_KEYS — still record useful extras
            status = "VALIDATED"
            hits[k] = Hit(k, v, status, "world eval", conf)

        cam = rpm_u64(h, world_ev.ptr + world_ev.fields["world.Camera"])
        if cam:
            hits.update(validate_camera_fields(h, cam))

        lp_off = world_ev.fields.get("world.LocalPlayer")
        if lp_off is not None:
            lp = rpm_u64(h, world_ev.ptr + lp_off)
            if lp:
                hits.update(validate_entity_fields(h, lp))

        hits.update(validate_globals(h, base, size))

        # Stable-ish struct constants if not otherwise set
        for key in ("anim.MatrixArray", "anim.MatrixB", "anim.AnimComponent",
                    "skeleton.AnimClass1", "skeleton.AnimClass2", "skeleton.AnimComponent",
                    "infected.Skeleton", "entitytype.TypeName", "entitytype.ConfigName",
                    "entitytype.ModelName", "player.RecordValue", "entity_owner.Owner",
                    "player_identity.Name", "scoreboard_identity.NetworkId",
                    "scoreboard_identity.SteamId", "scoreboard_identity.Name",
                    "world.NoGrass", "world.GrassOffline", "world.WeatherController",
                    "world.SlowEntValidCount", "world.BulletList", "world.BulletCount",
                    "world.ItemList", "world.ItemListSize", "world.FarEntList", "world.FarTableSize"):
            if key not in hits and key in world_ev.fields:
                hits[key] = Hit(key, world_ev.fields[key], "VALIDATED", "world field", conf)
            elif key not in hits and args.keep_old and key in OAK_KEYS:
                # only keep if we at least have a good World (session is real)
                hits[key] = Hit(key, OAK_KEYS[key], "KEPT", "prior; not revalidated this run", 20)

        KERNEL32.CloseHandle(h)
    else:
        print("[*] offline-only (natives). Pass --live for World/structs.")

    # Fill remaining
    for key, default in OAK_KEYS.items():
        if key in hits:
            continue
        if args.keep_old:
            hits[key] = Hit(key, default, "KEPT", "prior default", 10)
        else:
            hits[key] = Hit(key, default, "UNRESOLVED", "not proven this run", 0)

    local = Path(os.environ.get("LOCALAPPDATA", ".")) / "DayZ" / "oak_sessions"
    local.mkdir(parents=True, exist_ok=True)
    session = local / f"offsets_v2_{stamp}.txt"
    write_report(session, hits, natives, stamp, product, world_ev)
    print(f"[+] session {session}")

    n_good = sum(1 for h in hits.values() if h.status in ("RESOLVED", "VALIDATED"))
    n_kept = sum(1 for h in hits.values() if h.status == "KEPT")
    n_bad = sum(1 for h in hits.values() if h.status == "UNRESOLVED")
    print(f"[*] summary good={n_good} kept={n_kept} unresolved={n_bad} natives={len(natives)}")

    if args.apply:
        if args.live and n_good < 10:
            print("[!] refusing --apply: too few validated keys", file=sys.stderr)
            return 7
        # For apply: omit UNRESOLVED (use keep-old KEPT if requested)
        apply_hits = {k: v for k, v in hits.items() if v.status != "UNRESOLVED"}
        # Ensure full catalog present when keep-old
        if args.keep_old:
            for k, d in OAK_KEYS.items():
                apply_hits.setdefault(k, Hit(k, d, "KEPT", "fill", 10))
        write_generated_hpp(OutGenerated, apply_hits, stamp, product)
        write_report(OutOffsetsTxt, hits, natives, stamp, product, world_ev)
        OutNatives.parent.mkdir(parents=True, exist_ok=True)
        OutNatives.write_text("\n".join(f"{k} = 0x{v:X}" for k, v in sorted(natives.items())) + "\n", encoding="utf-8")
        print(f"[+] wrote {OutGenerated}")
        print(f"[+] wrote {OutOffsetsTxt}")
        print(f"[+] wrote {OutNatives}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
