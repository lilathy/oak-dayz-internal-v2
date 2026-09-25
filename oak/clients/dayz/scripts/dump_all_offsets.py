#!/usr/bin/env python3
"""
Oak DayZ — FULL catalog offset dumper + validator (fail-closed)

Dumps and validates EVERY key in offsets_generated.hpp / recipes old_values:
  1) Module RVAs  — RIP path signatures (old→new) + unanimous delta fill
  2) Struct fields — code access-path learning (old→new) + live rediscovery
  3) Gate natives  — unique cmp [rax+imm] bodies
  4) Code sites    — 3PP writer/copies, AimingModel recoil epilogue

Writes hpp ONLY when every REQUIRED key PASSes live validation.

Usage:
  # After a Steam update (DayZ running in-world):
  python dump_all_offsets.py --full --allow-warn-write

  # One-time after a PASS dump — locks current build as next migrate baseline:
  python dump_all_offsets.py --snapshot

  # Stepwise:
  python dump_all_offsets.py --learn
  python dump_all_offsets.py --apply
  python dump_all_offsets.py --live-validate --write-hpp --allow-warn-write
"""
from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import datetime as dt
import json
import math
import re
import struct
import sys
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
REPO = Path(__file__).resolve().parents[1]
DEFAULT_OLD = Path(r"C:\oak\dayz\DayZ_x64_OLD.exe")
DEFAULT_BASELINE = Path(r"C:\oak\dayz\DayZ_x64_BASELINE.exe")
DEFAULT_NEW = Path(r"C:\Program Files (x86)\Steam\steamapps\common\DayZ\DayZ_x64.exe")
DEFAULT_HPP = REPO / "src" / "offsets_generated.hpp"
DEFAULT_RECIPES = REPO / "docs" / "engine" / "offset_recipes_full.json"
DEFAULT_MIGRATE = REPO / "docs" / "engine" / "offset_recipes.json"  # prior module recipes
OUT_REPORT = REPO / "docs" / "engine" / "FULL_DUMP_REPORT.txt"
OUT_JSON = REPO / "docs" / "engine" / "full_dump_results.json"
OUT_NATIVES = REPO / "docs" / "engine" / "NATIVE_RVAS.txt"
OUT_SITES = REPO / "docs" / "engine" / "CODE_SITES.txt"

IMAGE_SCN_MEM_EXECUTE = 0x20000000
IMAGE_SCN_MEM_WRITE = 0x80000000

# Keys that MUST pass live validation before --write-hpp succeeds.
REQUIRED = {
    "modbase.World",
    "modbase.Network",
    "modbase.FOV_Context",
    "modbase.PhysicsWorld",
    "world.Camera",
    "world.LocalPlayer",
    "world.NearEntList",
    "world.FarEntList",
    "world.SlowEntList",
    "world.ItemList",
    "camera.InvertedViewTranslation",
    "camera.InvertedViewRight",
    "camera.InvertedViewUp",
    "camera.InvertedViewForward",
    "camera.GetProjectionD1",
    "camera.GetProjectionD2",
    "entity.Type",
    "entity.VisualState",
    "entity.FutureVisualState",
    "entity.IsDead",
    "player.Inventory",
    "player.Skeleton",
    "inventory.Hands",
    "network.ThirdPersonFlag",
    "native.Is3rdPersonDisabled",
    "native.IsCrosshairDisabled",
    "code.ThirdPersonAllowWriter",
}

MODULE_KEYS = {
    "modbase.World",
    "modbase.FOV_Context",
    "modbase.Network",
    "modbase.NetworkManager",
    "modbase.Tick",
    "modbase.ScopeFovCtx",
    "modbase.PhysicsWorld",
}

WORLD_FIELDS = [
    "world.Camera",
    "world.LocalPlayer",
    "world.PlayerOn",
    "world.NearEntList",
    "world.FarEntList",
    "world.FarTableSize",
    "world.SlowEntList",
    "world.SlowTableSize",
    "world.BulletList",
    "world.BulletCount",
    "world.ItemList",
    "world.ItemListSize",
    "world.NoGrass",
    "world.GrassOffline",
    "world.EyeAccom",
    "world.Hour",
    "world.Day",
    "world.DayTime",
    "world.WeatherController",
    "world.SlowEntValidCount",
]

# Known-good on DayZ 1.29.0.163709 (seeds for path migrate + pattern fallback).
# After each Steam update: --snapshot once, then next patch uses that baseline as --old-exe.
CODE_SITES_CURRENT = {
    "code.ThirdPersonAllowWriter": 0xA85292,
    "code.ThirdPersonAllowCopy0": 0x53BA14,
    "code.ThirdPersonAllowCopy1": 0x5F4A34,
    "code.AimingModelRecoil": 0x4E9126,
    "code.CameraFovUpdate": 0x7A0330,
    "code.DayZPlayerGetName": 0x4E5120,
    "code.AllocCollisionBuffer": 0x98390,
}

# Pre-163709 seeds kept only for migrating an old baseline → 163709.
CODE_SITES_OLD = {
    "code.ThirdPersonAllowWriter": 0xA85DC2,
    "code.ThirdPersonAllowCopy0": 0x53C604,
    "code.ThirdPersonAllowCopy1": 0x5F5624,
    "code.AimingModelRecoil": 0x4E9D16,
}

# ---------------------------------------------------------------------------
# PE
# ---------------------------------------------------------------------------


class PeImage:
    def __init__(self, path: Path):
        self.path = path
        self.data = path.read_bytes()
        pe = struct.unpack_from("<I", self.data, 0x3C)[0]
        num = struct.unpack_from("<H", self.data, pe + 6)[0]
        opt = struct.unpack_from("<H", self.data, pe + 20)[0]
        sec_off = pe + 24 + opt
        self.secs = []
        for i in range(num):
            o = sec_off + i * 40
            name = self.data[o : o + 8].split(b"\0", 1)[0]
            vs, va, rs, raw = struct.unpack_from("<IIII", self.data, o + 8)
            chars = struct.unpack_from("<I", self.data, o + 36)[0]
            self.secs.append((name, va, vs, raw, rs, chars))
        self.size = max((va + max(vs, rs) for _n, va, vs, _r, rs, _c in self.secs), default=len(self.data))

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

    def is_writable(self, rva: int) -> bool:
        for _n, va, vs, _r, _rs, chars in self.secs:
            if va <= rva < va + max(vs, 1):
                return (chars & IMAGE_SCN_MEM_WRITE) != 0
        return False

    def exec_ranges(self):
        for n, va, vs, raw, rs, chars in self.secs:
            if chars & IMAGE_SCN_MEM_EXECUTE:
                yield n, va, vs, raw, rs, chars


def parse_hpp_offsets(path: Path) -> dict[str, int]:
    text = path.read_text(encoding="utf-8", errors="replace")
    out: dict[str, int] = {}
    ns = None
    for line in text.splitlines():
        m = re.match(r"\s*namespace\s+(\w+)\s*\{", line)
        if m:
            ns = m.group(1)
            continue
        m = re.search(r"OAK_RUNTIME_OFFSET\(\s*(\w+)\s*,\s*(0x[0-9A-Fa-f]+|\d+)\s*\)", line)
        if m and ns and ns != "oak_offsets":
            out[f"{ns}.{m.group(1)}"] = int(m.group(2), 0)
    return out


# ---------------------------------------------------------------------------
# Static: RIP / sig / field access / natives / code sites
# ---------------------------------------------------------------------------

RIP_PREFS = {
    b"\x48\x8B\x05",
    b"\x48\x8B\x0D",
    b"\x48\x8B\x15",
    b"\x48\x8B\x1D",
    b"\x48\x8B\x3D",
    b"\x4C\x8B\x05",
    b"\x4C\x8B\x0D",
    b"\x4C\x8B\x15",
    b"\x4C\x8B\x1D",
    b"\x48\x8D\x05",
    b"\x48\x8D\x0D",
    b"\x48\x8D\x15",
    b"\x4C\x8D\x05",
    b"\x4C\x8D\x0D",
}


def find_rip_xrefs_to(pe: PeImage, target_rva: int) -> list[int]:
    hits = []
    data = pe.data
    for _n, _va, _vs, raw, rs, chars in pe.exec_ranges():
        end = raw + rs - 7
        i = raw
        while i < end:
            if data[i : i + 3] in RIP_PREFS:
                disp = struct.unpack_from("<i", data, i + 3)[0]
                instr = pe.off_to_rva(i)
                if instr is not None and instr + 7 + disp == target_rva:
                    hits.append(instr)
                i += 7
                continue
            i += 1
    return hits


def extract_path_sig(pe: PeImage, instr_rva: int, before: int = 12, after: int = 8) -> dict | None:
    off = pe.rva_to_off(instr_rva - before)
    if off is None:
        return None
    total = before + 7 + after
    raw = pe.data[off : off + total]
    if len(raw) < total:
        return None
    return {
        "before": before,
        "after": after,
        "bytes": raw.hex(),
        "wildcards": list(range(before + 3, before + 7)),
        "old_instr_rva": instr_rva,
    }


def match_sig(pe: PeImage, sig: dict) -> list[tuple[int, int]]:
    needle = bytes.fromhex(sig["bytes"])
    wild = set(sig["wildcards"])
    before = sig["before"]
    first_fixed = next((j for j in range(len(needle)) if j not in wild), None)
    if first_fixed is None:
        return []
    best = (first_fixed, 1)
    run_start = None
    run_len = 0
    for j in range(len(needle)):
        if j not in wild:
            if run_start is None:
                run_start, run_len = j, 1
            else:
                run_len += 1
            if run_len > best[1]:
                best = (run_start, run_len)
        else:
            run_start, run_len = None, 0
    seed_off, seed_n = best
    seed = needle[seed_off : seed_off + max(seed_n, 1)]
    hits = []
    data = pe.data
    for _n, _va, _vs, raw, rs, chars in pe.exec_ranges():
        end = raw + rs
        pos = raw
        while pos < end:
            i = data.find(seed, pos, end)
            if i < 0:
                break
            start = i - seed_off
            pos = i + 1
            if start < raw or start + len(needle) > end:
                continue
            window = data[start : start + len(needle)]
            if any(j not in wild and window[j] != needle[j] for j in range(len(needle))):
                continue
            instr_off = start + before
            instr_rva = pe.off_to_rva(instr_off)
            if instr_rva is None:
                continue
            disp = struct.unpack_from("<i", data, instr_off + 3)[0]
            hits.append((instr_rva, instr_rva + 7 + disp))
    return hits


def iter_disp_loads(pe: PeImage, start_rva: int, span: int = 0xC0):
    """Yield (instr_rva, disp, size) for [reg+disp8/32] loads in window."""
    off0 = pe.rva_to_off(start_rva)
    if off0 is None:
        return
    data = pe.data
    end = min(off0 + span, len(data) - 8)
    i = off0
    while i < end:
        b0 = data[i]
        # REX.W mov r64, r/m64
        if b0 in (0x48, 0x4C) and data[i + 1] == 0x8B:
            modrm = data[i + 2]
            mod = modrm >> 6
            rm = modrm & 7
            if rm == 4:  # SIB — skip for simplicity
                i += 1
                continue
            if mod == 1:  # disp8
                disp = struct.unpack_from("<b", data, i + 3)[0]
                rva = pe.off_to_rva(i)
                if rva is not None:
                    yield rva, disp & 0xFFFFFFFF if disp < 0 else disp, 1
                i += 4
                continue
            if mod == 2:  # disp32
                disp = struct.unpack_from("<i", data, i + 3)[0]
                rva = pe.off_to_rva(i)
                if rva is not None:
                    yield rva, disp & 0xFFFFFFFF, 4
                i += 7
                continue
        # movss/movsd xmm, [reg+disp] — 0xF3/F2 0x0F 0x10
        if data[i] in (0xF3, 0xF2) and data[i + 1] == 0x0F and data[i + 2] == 0x10:
            modrm = data[i + 3]
            mod = modrm >> 6
            rm = modrm & 7
            if rm != 4 and mod == 2:
                disp = struct.unpack_from("<i", data, i + 4)[0]
                rva = pe.off_to_rva(i)
                if rva is not None:
                    yield rva, disp & 0xFFFFFFFF, 4
                i += 8
                continue
            if rm != 4 and mod == 1:
                disp = struct.unpack_from("<b", data, i + 4)[0]
                rva = pe.off_to_rva(i)
                if rva is not None:
                    yield rva, disp & 0xFFFFFFFF if disp >= 0 else disp, 1
                i += 5
                continue
        i += 1


def extract_field_sig(pe: PeImage, instr_rva: int, disp_size: int, before: int = 10, after: int = 6) -> dict | None:
    """Signature around a [reg+disp] load with the displacement bytes wildcarded."""
    off = pe.rva_to_off(instr_rva)
    if off is None:
        return None
    # locate disp offset inside instruction
    b0 = pe.data[off]
    if b0 in (0x48, 0x4C) and pe.data[off + 1] == 0x8B:
        disp_off_in_instr = 3
        instr_len = 3 + disp_size
    elif pe.data[off] in (0xF3, 0xF2) and pe.data[off + 1] == 0x0F:
        disp_off_in_instr = 4
        instr_len = 4 + disp_size
    else:
        return None
    start = pe.rva_to_off(instr_rva - before)
    if start is None:
        return None
    total = before + instr_len + after
    raw = pe.data[start : start + total]
    if len(raw) < total:
        return None
    wild = list(range(before + disp_off_in_instr, before + disp_off_in_instr + disp_size))
    return {
        "kind": "field_disp",
        "before": before,
        "instr_len": instr_len,
        "disp_off": before + disp_off_in_instr,
        "disp_size": disp_size,
        "bytes": raw.hex(),
        "wildcards": wild,
        "old_instr_rva": instr_rva,
    }


def match_field_sig(pe: PeImage, sig: dict) -> list[tuple[int, int]]:
    """Return (instr_rva, new_disp)."""
    needle = bytes.fromhex(sig["bytes"])
    wild = set(sig["wildcards"])
    before = sig["before"]
    disp_off = sig["disp_off"]
    disp_size = sig["disp_size"]
    first_fixed = next((j for j in range(len(needle)) if j not in wild), None)
    if first_fixed is None:
        return []
    # longest fixed run
    best = (first_fixed, 1)
    run_start = None
    run_len = 0
    for j in range(len(needle)):
        if j not in wild:
            if run_start is None:
                run_start, run_len = j, 1
            else:
                run_len += 1
            if run_len > best[1]:
                best = (run_start, run_len)
        else:
            run_start, run_len = None, 0
    seed_off, seed_n = best
    seed = needle[seed_off : seed_off + max(seed_n, 1)]
    hits = []
    data = pe.data
    for _n, _va, _vs, raw, rs, chars in pe.exec_ranges():
        end = raw + rs
        pos = raw
        while pos < end:
            i = data.find(seed, pos, end)
            if i < 0:
                break
            start = i - seed_off
            pos = i + 1
            if start < raw or start + len(needle) > end:
                continue
            window = data[start : start + len(needle)]
            if any(j not in wild and window[j] != needle[j] for j in range(len(needle))):
                continue
            instr_off = start + before
            instr_rva = pe.off_to_rva(instr_off)
            if instr_rva is None:
                continue
            if disp_size == 1:
                disp = struct.unpack_from("<b", data, start + disp_off)[0] & 0xFF
            else:
                disp = struct.unpack_from("<i", data, start + disp_off)[0] & 0xFFFFFFFF
            hits.append((instr_rva, disp))
    return hits


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
    if name == "Is3rdPersonDisabled":
        g = find_gate_by_field(pe, 0x74)
        if g is not None:
            return g
    if name == "IsCrosshairDisabled":
        g = find_gate_by_field(pe, 0x78)
        if g is not None:
            return g
    enc = name.encode("ascii")
    off = pe.data.find(enc + b"\0")
    if off < 0:
        off = pe.data.find(enc)
    if off < 0:
        return None
    str_rva = pe.off_to_rva(off)
    if str_rva is None:
        return None
    for _n, _va, _vs, raw, rs, chars in pe.exec_ranges():
        for i in range(raw, raw + rs - 7):
            if pe.data[i] not in (0x48, 0x4C) or pe.data[i + 1] != 0x8D:
                continue
            if pe.data[i + 2] not in (0x05, 0x0D, 0x15, 0x1D, 0x25, 0x2D, 0x35, 0x3D):
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


def migrate_code_site(old: PeImage, new: PeImage, old_rva: int) -> int | None:
    ooff = old.rva_to_off(old_rva)
    if ooff is None:
        return None
    for n in (24, 20, 16, 12, 10):
        exact = old.data[ooff : ooff + n]
        idxs = []
        start = 0
        while True:
            i = new.data.find(exact, start)
            if i < 0:
                break
            idxs.append(i)
            start = i + 1
        if len(idxs) == 1:
            return new.off_to_rva(idxs[0])
    # wildcard E8/RIP
    pat = bytearray(old.data[ooff : ooff + 28])
    mask = bytearray([0xFF] * len(pat))
    i = 0
    while i < len(pat) - 4:
        if pat[i] == 0xE8:
            for j in range(1, 5):
                mask[i + j] = 0
            i += 5
            continue
        if pat[i] in (0x48, 0x4C) and i + 6 < len(pat) and pat[i + 1] in (0x8B, 0x8D):
            for j in range(3, 7):
                mask[i + j] = 0
            i += 7
            continue
        i += 1
    matches = []
    for _n, va, _vs, raw, rs, chars in new.exec_ranges():
        chunk = new.data[raw : raw + rs]
        for i in range(len(chunk) - len(pat)):
            if all(not mask[j] or chunk[i + j] == pat[j] for j in range(len(pat))):
                matches.append(va + i)
    return matches[0] if len(matches) == 1 else None


# ---------------------------------------------------------------------------
# Learn / Apply
# ---------------------------------------------------------------------------


def load_catalog(recipes_prior: Path | None, hpp: Path) -> dict[str, int]:
    """Prefer prior recipes old_values (true old build); else hpp."""
    if recipes_prior and recipes_prior.exists():
        prior = json.loads(recipes_prior.read_text(encoding="utf-8"))
        ov = prior.get("old_values") or {}
        if ov.get("modbase.World") == 0x4264058 or (ov and "modbase.World" in ov):
            # If recipes already applied once, old_values still hold OLD catalog — good.
            # But if someone re-learned from new hpp, World would be new. Prefer migrate recipes.
            return {k: int(v) for k, v in ov.items()}
    return parse_hpp_offsets(hpp)


def index_disp32_movs(pe: PeImage) -> dict[int, list[int]]:
    """Map disp32 -> list of instruction RVAs for REX mov r64,[reg+disp32]."""
    out: dict[int, list[int]] = {}
    data = pe.data
    for _n, va, _vs, raw, rs, chars in pe.exec_ranges():
        end = raw + rs - 7
        i = raw
        while i < end:
            if data[i] in (0x48, 0x4C) and data[i + 1] == 0x8B:
                modrm = data[i + 2]
                mod = modrm >> 6
                rm = modrm & 7
                if mod == 2 and rm != 4:
                    disp = struct.unpack_from("<i", data, i + 3)[0] & 0xFFFFFFFF
                    rva = pe.off_to_rva(i)
                    if rva is not None:
                        out.setdefault(disp, []).append(rva)
                    i += 7
                    continue
            i += 1
    return out


def learn_all(old: PeImage, catalog: dict[str, int]) -> dict:
    recipes: dict = {
        "version": 2,
        "module_paths": {},
        "field_paths": {},
        "native_paths": {},
        "code_sites": {},
        "old_values": catalog,
    }

    # Modules
    for key in sorted(MODULE_KEYS):
        if key not in catalog:
            continue
        rva = catalog[key]
        xrefs = find_rip_xrefs_to(old, rva)
        print(f"[learn] {key} old=0x{rva:X} xrefs={len(xrefs)}", flush=True)
        sigs = []
        for xr in xrefs[:16]:
            sig = extract_path_sig(old, xr)
            if sig:
                sigs.append(sig)
        recipes["module_paths"][key] = {
            "old_rva": rva,
            "xref_count": len(xrefs),
            "signatures": sigs[:8],
        }
        print(f"        sigs={len(recipes['module_paths'][key]['signatures'])}", flush=True)

    print("[*] indexing disp32 movs on old exe...", flush=True)
    disp_index = index_disp32_movs(old)
    print(f"[+] indexed {len(disp_index)} unique displacements", flush=True)

    world_rva = catalog.get("modbase.World")
    world_xrefs = find_rip_xrefs_to(old, world_rva) if world_rva else []
    world_windows = [(wx, wx + 0xC0) for wx in world_xrefs[:60]]

    def pick_field_sigs(off: int, prefer_near_world: bool) -> list[dict]:
        sigs: list[dict] = []
        sites = disp_index.get(off & 0xFFFFFFFF, [])
        if prefer_near_world and world_windows:
            near = [r for r in sites if any(a <= r < b for a, b in world_windows)]
            if near:
                sites = near
        for instr in sites[:8]:
            sig = extract_field_sig(old, instr, 4)
            if sig:
                sigs.append(sig)
            if len(sigs) >= 4:
                break
        return sigs

    for key in WORLD_FIELDS:
        if key not in catalog:
            continue
        off = catalog[key]
        sigs = pick_field_sigs(off, prefer_near_world=True)
        recipes["field_paths"][key] = {
            "parent": "modbase.World",
            "old_offset": off,
            "signatures": sigs,
        }
        print(f"[learn] {key} off=0x{off:X} field_sigs={len(sigs)}", flush=True)

    for key, val in sorted(catalog.items()):
        if key.startswith("modbase.") or key in WORLD_FIELDS:
            continue
        if key.startswith("native.") or key.startswith("code."):
            continue
        sigs = pick_field_sigs(val, prefer_near_world=False) if val >= 0x80 else []
        recipes["field_paths"][key] = {
            "parent": key.split(".", 1)[0],
            "old_offset": val,
            "signatures": sigs,
        }
        print(f"[learn] {key} off=0x{val:X} field_sigs={len(sigs)}", flush=True)

    for key, name in (
        ("native.Is3rdPersonDisabled", "Is3rdPersonDisabled"),
        ("native.IsCrosshairDisabled", "IsCrosshairDisabled"),
        ("native.CameraViewChanged", "CameraViewChanged"),
    ):
        h = find_native_handler(old, name)
        recipes["native_paths"][key] = {"string": name, "old_rva": h}
        print(f"[learn] {key} -> {hex(h) if h else None}", flush=True)

    for key, rva in CODE_SITES_CURRENT.items():
        recipes["code_sites"][key] = {"old_rva": rva}
        print(f"[learn] {key} old=0x{rva:X}", flush=True)

    return recipes


def apply_all(new: PeImage, recipes: dict) -> dict[str, dict]:
    results: dict[str, dict] = {}

    # Modules
    for key, rec in recipes.get("module_paths", {}).items():
        votes: Counter[int] = Counter()
        for sig in rec.get("signatures", []):
            for _i, tgt in match_sig(new, sig):
                votes[tgt] += 1
        if votes:
            resolved = votes.most_common(1)[0][0]
            results[key] = {
                "status": "RESOLVED",
                "old": rec.get("old_rva"),
                "new": resolved,
                "votes": votes[resolved],
                "candidates": len(votes),
            }
            print(f"[apply] {key}: 0x{rec.get('old_rva'):X} -> 0x{resolved:X}")
        else:
            results[key] = {"status": "UNRESOLVED", "old": rec.get("old_rva"), "new": None}
            print(f"[apply] {key}: UNRESOLVED")

    deltas = [
        int(r["new"]) - int(r["old"])
        for r in results.values()
        if r.get("status") == "RESOLVED" and r.get("old") is not None and r.get("new") is not None
    ]
    if deltas and len(set(deltas)) == 1:
        delta = deltas[0]
        for key, r in list(results.items()):
            if r.get("status") == "UNRESOLVED" and key.startswith("modbase.") and r.get("old") is not None:
                guess = int(r["old"]) + delta
                results[key] = {"status": "RESOLVED_DELTA", "old": r["old"], "new": guess, "delta": delta}
                print(f"[apply] {key}: delta -> 0x{guess:X}")

    # Fields via access sigs; else keep old pending live
    for key, rec in recipes.get("field_paths", {}).items():
        votes: Counter[int] = Counter()
        for sig in rec.get("signatures", []):
            for _i, disp in match_field_sig(new, sig):
                votes[disp] += 1
        old_off = int(rec["old_offset"])
        if votes:
            resolved = votes.most_common(1)[0][0]
            results[key] = {
                "status": "RESOLVED_FIELD",
                "old": old_off,
                "new": resolved,
                "votes": votes[resolved],
                "candidates": len(votes),
            }
            print(f"[apply] {key}: 0x{old_off:X} -> 0x{resolved:X} (field)")
        else:
            results[key] = {
                "status": "PENDING_LIVE",
                "old": old_off,
                "new": old_off,
                "note": "no unique field sig; must live-validate",
            }

    # FovBase is a constant field of FOV context object
    if "modbase.FovBase" in recipes.get("old_values", {}):
        results["modbase.FovBase"] = {
            "status": "PENDING_LIVE",
            "old": recipes["old_values"]["modbase.FovBase"],
            "new": recipes["old_values"]["modbase.FovBase"],
        }

    # Natives
    for key, rec in recipes.get("native_paths", {}).items():
        h = find_native_handler(new, rec["string"])
        results[key] = {
            "status": "RESOLVED" if h else "UNRESOLVED",
            "old": rec.get("old_rva"),
            "new": h,
            "string": rec["string"],
        }
        print(f"[apply] {key}: {hex(h) if h else None}")

    # Code sites — need old pe; apply expects recipes only. Re-open via optional path later.
    # Here we only mark pending; --apply with --old-exe migrates them.
    for key, rec in recipes.get("code_sites", {}).items():
        results[key] = {"status": "PENDING_CODE", "old": rec["old_rva"], "new": None}

    return results


def discover_code_sites(pe: PeImage) -> dict[str, int]:
    """Pattern-resolve code sites on a single PE (no old exe required)."""
    out: dict[str, int] = {}
    data = pe.data

    # 3PP allow writer: xor cl,cl / mov cl,1 near World+0x2984 store
    hits = []
    start = 0
    while True:
        i = data.find(b"\x32\xc9\xeb\x02\xb1\x01", start)
        if i < 0:
            break
        # nearby 88 88 84 29 00 00 (mov [rax+0x2984], cl) within +0x30
        window = data[i : i + 0x30]
        if b"\x88\x88\x84\x29\x00\x00" in window or b"\x88\x8b\x84\x29\x00\x00" in window:
            r = pe.off_to_rva(i)
            if r is not None:
                hits.append(r)
        start = i + 1
    if len(hits) == 1:
        out["code.ThirdPersonAllowWriter"] = hits[0]

    for key, needle in (
        ("code.ThirdPersonAllowCopy0", b"\x40\x88\xa9\x84\x29\x00\x00"),
        ("code.ThirdPersonAllowCopy1", b"\x40\x88\xab\x84\x29\x00\x00"),
    ):
        idxs = []
        start = 0
        while True:
            i = data.find(needle, start)
            if i < 0:
                break
            r = pe.off_to_rva(i)
            if r is not None:
                idxs.append(r)
            start = i + 1
        if len(idxs) == 1:
            out[key] = idxs[0]

    # AimingModel recoil epilogue: mov eax,[rbx+0x2760] then +0x10 = hook site
    hits = []
    start = 0
    needle = bytes.fromhex("8b8360270000")
    while True:
        i = data.find(needle, start)
        if i < 0:
            break
        r = pe.off_to_rva(i)
        if r is not None:
            hits.append(r + 0x10)
        start = i + 1
    if len(hits) == 1:
        out["code.AimingModelRecoil"] = hits[0]

    # Camera FOV update entry: prologue + movss xmm1,[rcx+0x194]
    pat = bytes.fromhex("488bc448895808488970104889781855")
    start = 0
    fov_hits = []
    while True:
        i = data.find(pat, start)
        if i < 0:
            break
        chunk = data[i : i + 80]
        if b"\xf3\x0f\x10\x89\x94\x01\x00\x00" in chunk:
            r = pe.off_to_rva(i)
            if r is not None:
                fov_hits.append(r)
        start = i + 1
    if len(fov_hits) == 1:
        out["code.CameraFovUpdate"] = fov_hits[0]
    elif CODE_SITES_CURRENT["code.CameraFovUpdate"] not in fov_hits and fov_hits:
        # Prefer site near 0x7A0000 when ambiguous
        near = [r for r in fov_hits if 0x790000 <= r <= 0x7B0000]
        if len(near) == 1:
            out["code.CameraFovUpdate"] = near[0]

    # AllocCollisionBuffer: mov [rsp+8],rbx; ... mov ecx,0xB8
    alloc_pat = bytes.fromhex("48895c24084889742410574883ec20488bd9498bf8b9b8000000")
    i = data.find(alloc_pat)
    if i >= 0:
        r = pe.off_to_rva(i)
        if r is not None:
            out["code.AllocCollisionBuffer"] = r

    # DayZPlayer::GetName — keep seed if entry looks like a real prologue
    gn = CODE_SITES_CURRENT["code.DayZPlayerGetName"]
    off = pe.rva_to_off(gn)
    if off is not None:
        raw = data[off : off + 16]
        if raw[:3] in (b"\x48\x89\x5c", b"\x48\x8b\xc4", b"\x40\x53", b"\x48\x83\xec"):
            out["code.DayZPlayerGetName"] = gn

    # Fill any remaining from current seeds if bytes match expected shape
    for key, rva in CODE_SITES_CURRENT.items():
        if key in out:
            continue
        off = pe.rva_to_off(rva)
        if off is None:
            continue
        raw = data[off : off + 8]
        if raw:
            out[key] = rva
    return out


def apply_code_sites(old: PeImage | None, new: PeImage, recipes: dict, results: dict) -> None:
    # Always pattern-scan the new PE first (survives missing old exe).
    for key, rva in discover_code_sites(new).items():
        results[key] = {"status": "RESOLVED", "old": recipes.get("code_paths", {}).get(key, {}).get("old_rva"), "new": rva}
        print(f"[apply] {key}: pattern -> 0x{rva:X}")

    if old is None:
        return
    # Legacy path migrate for any still missing
    _apply_code_sites_migrate(old, new, recipes, results)


def _apply_code_sites_migrate(old: PeImage, new: PeImage, recipes: dict, results: dict) -> None:
    for key, rec in recipes.get("code_sites", {}).items():
        if results.get(key, {}).get("new"):
            continue  # pattern discovery already owns this
        old_rva = int(rec["old_rva"])
        # Special-case 3PP writer: find xor cl,cl before store 88 ?? 84 29
        if key == "code.ThirdPersonAllowWriter":
            hits = []
            for _n, va, _vs, raw, rs, chars in new.exec_ranges():
                chunk = new.data[raw : raw + rs]
                for i in range(len(chunk) - 6):
                    if chunk[i] == 0x88 and chunk[i + 2 : i + 6] == bytes.fromhex("84290000"):
                        win = chunk[max(0, i - 0x30) : i]
                        rel = win.rfind(b"\x32\xc9")
                        if rel >= 0:
                            hits.append(va + i - (len(win) - rel))
            if len(hits) == 1:
                results[key] = {"status": "RESOLVED", "old": old_rva, "new": hits[0]}
                print(f"[apply] {key}: -> 0x{hits[0]:X}")
                continue
        if key.startswith("code.ThirdPersonAllowCopy"):
            # 40 88 A9/AB 84 29 00 00
            want = b"\x40\x88\xa9\x84\x29\x00\x00" if key.endswith("0") else b"\x40\x88\xab\x84\x29\x00\x00"
            idxs = []
            start = 0
            while True:
                i = new.data.find(want, start)
                if i < 0:
                    break
                r = new.off_to_rva(i)
                if r is not None:
                    idxs.append(r)
                start = i + 1
            if len(idxs) == 1:
                results[key] = {"status": "RESOLVED", "old": old_rva, "new": idxs[0]}
                print(f"[apply] {key}: -> 0x{idxs[0]:X}")
                continue
        m = migrate_code_site(old, new, old_rva)
        if m is not None:
            results[key] = {"status": "RESOLVED", "old": old_rva, "new": m}
            print(f"[apply] {key}: 0x{old_rva:X} -> 0x{m:X}")
        elif key == "code.AimingModelRecoil":
            # Distinctive: 8B 83 60 27 00 00 ... then 48 8B 08 83 39 00
            needle = bytes.fromhex("8b8360270000")
            start = 0
            hits = []
            while True:
                i = new.data.find(needle, start)
                if i < 0:
                    break
                r = new.off_to_rva(i)
                # epilogue site is typically +0x10 from this mov
                if r is not None:
                    hits.append(r + 0x10)
                start = i + 1
            if len(hits) == 1:
                results[key] = {"status": "RESOLVED", "old": old_rva, "new": hits[0]}
                print(f"[apply] {key}: pattern -> 0x{hits[0]:X}")
            else:
                results[key] = {"status": "UNRESOLVED", "old": old_rva, "new": None}
                print(f"[apply] {key}: UNRESOLVED hits={hits}")
        else:
            results[key] = {"status": "UNRESOLVED", "old": old_rva, "new": None}
            print(f"[apply] {key}: UNRESOLVED")


# ---------------------------------------------------------------------------
# Live validation
# ---------------------------------------------------------------------------

KERNEL32 = ctypes.WinDLL("kernel32", use_last_error=True)
PSAPI = ctypes.WinDLL("psapi", use_last_error=True)
PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400
TH32CS_SNAPPROCESS = 0x2
MEM_COMMIT = 0x1000

KERNEL32.ReadProcessMemory.argtypes = [
    wt.HANDLE,
    ctypes.c_void_p,
    ctypes.c_void_p,
    ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_size_t),
]
KERNEL32.ReadProcessMemory.restype = wt.BOOL
KERNEL32.OpenProcess.restype = wt.HANDLE
KERNEL32.GetProcessId.argtypes = [wt.HANDLE]
KERNEL32.GetProcessId.restype = wt.DWORD
PSAPI.EnumProcessModulesEx.argtypes = [
    wt.HANDLE, ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD), wt.DWORD
]
PSAPI.EnumProcessModulesEx.restype = wt.BOOL
PSAPI.GetModuleInformation.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, wt.DWORD]
PSAPI.GetModuleInformation.restype = wt.BOOL
PSAPI.GetModuleBaseNameW.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_wchar_p, wt.DWORD]
PSAPI.GetModuleBaseNameW.restype = wt.DWORD


class MODULEINFO(ctypes.Structure):
    _fields_ = [
        ("lpBaseOfDll", ctypes.c_void_p),
        ("SizeOfImage", wt.DWORD),
        ("EntryPoint", ctypes.c_void_p),
    ]


def find_pid(name: str = "DayZ_x64.exe") -> int | None:
    class PE32W(ctypes.Structure):
        _fields_ = [
            ("dwSize", wt.DWORD),
            ("cntUsage", wt.DWORD),
            ("th32ProcessID", wt.DWORD),
            ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)),
            ("th32ModuleID", wt.DWORD),
            ("cntThreads", wt.DWORD),
            ("th32ParentProcessID", wt.DWORD),
            ("pcPriClassBase", ctypes.c_long),
            ("dwFlags", wt.DWORD),
            ("szExeFile", ctypes.c_wchar * 260),
        ]

    snap = KERNEL32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    pe = PE32W()
    pe.dwSize = ctypes.sizeof(PE32W)
    if not KERNEL32.Process32FirstW(snap, ctypes.byref(pe)):
        KERNEL32.CloseHandle(snap)
        return None
    while True:
        if pe.szExeFile.lower() == name.lower():
            pid = int(pe.th32ProcessID)
            KERNEL32.CloseHandle(snap)
            return pid
        if not KERNEL32.Process32NextW(snap, ctypes.byref(pe)):
            break
    KERNEL32.CloseHandle(snap)
    return None


def open_process(pid: int):
    # 0x0410 = VM_READ|QUERY_INFORMATION; 0x1000 = QUERY_LIMITED (PEB on newer Win)
    rights = PROCESS_VM_READ | PROCESS_QUERY_INFORMATION | 0x1000
    h = KERNEL32.OpenProcess(rights, False, pid)
    if h:
        return h
    return KERNEL32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)


def module_base_size(h) -> tuple[int, int]:
    # Prefer Toolhelp module walk — more reliable than EnumProcessModulesEx ctypes wiring.
    class MODULEENTRY32W(ctypes.Structure):
        _fields_ = [
            ("dwSize", wt.DWORD),
            ("th32ModuleID", wt.DWORD),
            ("th32ProcessID", wt.DWORD),
            ("GlblcntUsage", wt.DWORD),
            ("ProccntUsage", wt.DWORD),
            ("modBaseAddr", ctypes.c_void_p),
            ("modBaseSize", wt.DWORD),
            ("hModule", wt.HMODULE),
            ("szModule", ctypes.c_wchar * 256),
            ("szExePath", ctypes.c_wchar * 260),
        ]

    TH32CS_SNAPMODULE = 0x00000008
    TH32CS_SNAPMODULE32 = 0x00000010
    pid = KERNEL32.GetProcessId(h)
    snap = KERNEL32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
    INVALID = ctypes.c_void_p(-1).value
    if snap not in (0, INVALID, wt.HANDLE(-1).value):
        me = MODULEENTRY32W()
        me.dwSize = ctypes.sizeof(me)
        ok = KERNEL32.Module32FirstW(snap, ctypes.byref(me))
        base = size = 0
        while ok:
            name = (me.szModule or "").lower()
            if name == "dayz_x64.exe" or name.endswith("\\dayz_x64.exe"):
                base = int(me.modBaseAddr or 0)
                size = int(me.modBaseSize or 0)
                break
            ok = KERNEL32.Module32NextW(snap, ctypes.byref(me))
        KERNEL32.CloseHandle(snap)
        if base:
            return base, size

    # Fallback: EnumProcessModulesEx (LIST_MODULES_ALL) — Toolhelp is often blocked under BE.
    LIST_MODULES_ALL = 0x03
    needed = wt.DWORD(0)
    PSAPI.EnumProcessModulesEx(h, None, 0, ctypes.byref(needed), LIST_MODULES_ALL)
    if needed.value:
        count = max(needed.value // ctypes.sizeof(ctypes.c_void_p), 256)
        arr = (ctypes.c_void_p * count)()
        if PSAPI.EnumProcessModulesEx(
            h, arr, ctypes.sizeof(arr), ctypes.byref(needed), LIST_MODULES_ALL
        ):
            n = min(needed.value // ctypes.sizeof(ctypes.c_void_p), count)
            mi = MODULEINFO()
            name_buf = ctypes.create_unicode_buffer(260)
            for i in range(n):
                mod = arr[i]
                if not mod:
                    continue
                if PSAPI.GetModuleBaseNameW(h, mod, name_buf, 260):
                    if name_buf.value.lower() == "dayz_x64.exe":
                        if PSAPI.GetModuleInformation(h, mod, ctypes.byref(mi), ctypes.sizeof(mi)):
                            return int(mi.lpBaseOfDll or 0), int(mi.SizeOfImage or 0)

    # Last resort: PEB ImageBaseAddress via NtQueryInformationProcess.
    class PROCESS_BASIC_INFORMATION(ctypes.Structure):
        _fields_ = [
            ("Reserved1", ctypes.c_void_p),
            ("PebBaseAddress", ctypes.c_void_p),
            ("Reserved2", ctypes.c_void_p * 2),
            ("UniqueProcessId", ctypes.POINTER(ctypes.c_ulong)),
            ("Reserved3", ctypes.c_void_p),
        ]

    ntdll = ctypes.WinDLL("ntdll")
    pbi = PROCESS_BASIC_INFORMATION()
    ret_len = ctypes.c_ulong(0)
    st = ntdll.NtQueryInformationProcess(h, 0, ctypes.byref(pbi), ctypes.sizeof(pbi), ctypes.byref(ret_len))
    if st == 0 and pbi.PebBaseAddress:
        # PEB+0x10 = ImageBaseAddress on x64
        img = (ctypes.c_ulonglong * 1)()
        br = ctypes.c_size_t()
        if KERNEL32.ReadProcessMemory(
            h,
            ctypes.c_void_p(int(pbi.PebBaseAddress) + 0x10),
            img,
            8,
            ctypes.byref(br),
        ):
            base = int(img[0])
            if base and rpm(h, base, 2) == b"MZ":
                # Size from PE OptionalHeader.SizeOfImage
                e_lfanew = struct.unpack_from("<I", rpm(h, base + 0x3C, 4) or b"\0\0\0\0")[0]
                pe = rpm(h, base + e_lfanew, 0x58) or b""
                if len(pe) >= 0x50 and pe[:4] == b"PE\0\0":
                    size = struct.unpack_from("<I", pe, 0x50)[0]
                    return base, size
                return base, 0x2000000

    raise OSError("DayZ_x64.exe module not found (Toolhelp/EnumModules/PEB all failed)")


def rpm(h, addr: int, n: int) -> bytes | None:
    if not addr:
        return None
    buf = (ctypes.c_ubyte * n)()
    br = ctypes.c_size_t()
    if not KERNEL32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n, ctypes.byref(br)):
        return None
    return bytes(buf[: br.value])


def ru64(h, addr: int) -> int | None:
    b = rpm(h, addr, 8)
    return None if not b or len(b) < 8 else struct.unpack("<Q", b)[0]


def ru32(h, addr: int) -> int | None:
    b = rpm(h, addr, 4)
    return None if not b or len(b) < 4 else struct.unpack("<I", b)[0]


def rf32(h, addr: int) -> float | None:
    b = rpm(h, addr, 4)
    return None if not b or len(b) < 4 else struct.unpack("<f", b)[0]


def is_user_ptr(p: int | None) -> bool:
    return bool(p) and 0x10000 < p < 0x00007FFFFFFFFFFF


def committed(h, addr: int) -> bool:
    class MBI(ctypes.Structure):
        _fields_ = [
            ("BaseAddress", ctypes.c_void_p),
            ("AllocationBase", ctypes.c_void_p),
            ("AllocationProtect", wt.DWORD),
            ("RegionSize", ctypes.c_size_t),
            ("State", wt.DWORD),
            ("Protect", wt.DWORD),
            ("Type", wt.DWORD),
        ]

    mbi = MBI()
    if KERNEL32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)) == 0:
        return False
    return mbi.State == MEM_COMMIT and (mbi.Protect & 0xFF) not in (0x01, 0x00)


def sane_pos(pos: tuple[float, float, float] | None, allow_origin=False) -> bool:
    if not pos:
        return False
    x, y, z = pos
    if not all(math.isfinite(v) for v in pos):
        return False
    if abs(x) < 1 and abs(z) < 1 and not allow_origin:
        return False
    return -100 < y < 800 and -20000 < x < 40000 and -20000 < z < 40000


def entity_pos(h, ent: int, vs_off: int = 0x1C8) -> tuple[float, float, float] | None:
    vs = ru64(h, ent + vs_off)
    if not vs or not committed(h, vs):
        return None
    b = rpm(h, vs + 0x2C, 12)
    if not b:
        return None
    return struct.unpack("<fff", b)


def looks_camera(h, cam: int) -> bool:
    if not is_user_ptr(cam) or not committed(h, cam):
        return False
    t = rpm(h, cam + 0x2C, 12)
    if not t:
        return False
    pos = struct.unpack("<fff", t)
    return sane_pos(pos)


def enfusion_string_ok(h, sp: int) -> bool:
    """Enfusion string often: ptr to length-prefixed or char*."""
    if not is_user_ptr(sp) or not committed(h, sp):
        return False
    # try raw cstring
    raw = rpm(h, sp, 64)
    if raw and 3 <= raw.find(b"\0") <= 60:
        s = raw.split(b"\0", 1)[0]
        if all(32 <= c < 127 for c in s) and len(s) >= 3:
            return True
    # try ptr->cstring
    p2 = ru64(h, sp)
    if p2 and committed(h, p2):
        raw = rpm(h, p2, 64)
        if raw and 3 <= raw.find(b"\0") <= 60:
            s = raw.split(b"\0", 1)[0]
            if all(32 <= c < 127 for c in s) and len(s) >= 3:
                return True
    return False


@dataclass
class Check:
    key: str
    value: int | None
    status: str  # PASS / FAIL / WARN / SKIP
    note: str = ""


def values_from_results(results: dict) -> dict[str, int]:
    out = {}
    for k, r in results.items():
        if r.get("new") is not None:
            out[k] = int(r["new"])
    return out


def live_validate(h, base: int, size: int, pe: PeImage, values: dict[str, int]) -> list[Check]:
    checks: list[Check] = []

    def add(key, val, status, note=""):
        checks.append(Check(key, val, status, note))

    # --- World ---
    wrva = values.get("modbase.World")
    world = ru64(h, base + wrva) if wrva is not None else None
    if not (wrva and world and is_user_ptr(world) and committed(h, world) and not (base <= world < base + size)):
        add("modbase.World", wrva, "FAIL", f"bad world ptr {world:#x}" if world else "null")
        # Still emit FAILs for dependents
        for k in values:
            if k != "modbase.World" and k.startswith(("world.", "camera.", "entity.", "player.", "inventory.", "anim.", "skeleton.", "infected.", "entitytype.")):
                add(k, values.get(k), "FAIL", "no world")
        # continue with globals/natives
    else:
        add("modbase.World", wrva, "PASS", f"ptr={world:#x}")

        cam_off = values.get("world.Camera", 0x1B8)
        cam = ru64(h, world + cam_off)
        if looks_camera(h, cam or 0):
            add("world.Camera", cam_off, "PASS", f"cam={cam:#x}")
            # camera fields
            for key, off in (
                ("camera.InvertedViewRight", values.get("camera.InvertedViewRight", 0x8)),
                ("camera.InvertedViewUp", values.get("camera.InvertedViewUp", 0x14)),
                ("camera.InvertedViewForward", values.get("camera.InvertedViewForward", 0x20)),
                ("camera.InvertedViewTranslation", values.get("camera.InvertedViewTranslation", 0x2C)),
                ("camera.ViewPortSize", values.get("camera.ViewPortSize", 0x58)),
                ("camera.GetProjectionD1", values.get("camera.GetProjectionD1", 0xD0)),
                ("camera.GetProjectionD2", values.get("camera.GetProjectionD2", 0xDC)),
            ):
                raw = rpm(h, cam + off, 12 if "Projection" in key or "View" in key or "Translation" in key else 8)
                if raw:
                    if key == "camera.InvertedViewTranslation":
                        pos = struct.unpack("<fff", raw[:12])
                        add(key, off, "PASS" if sane_pos(pos) else "FAIL", f"pos={pos}")
                    else:
                        add(key, off, "PASS", "readable")
                else:
                    add(key, off, "FAIL", "unreadable")
        else:
            add("world.Camera", cam_off, "FAIL", f"cam={cam}")
            for k in list(values):
                if k.startswith("camera."):
                    add(k, values[k], "FAIL", "no camera")

        # Local player — accept catalog off or rediscover near camera / near-list
        lp_off = values.get("world.LocalPlayer", 0x2960)
        lp = ru64(h, world + lp_off)
        vs_guess = values.get("entity.VisualState", 0x1C8)
        lp_pos = entity_pos(h, lp, vs_guess) if lp else None
        if not (lp and is_user_ptr(lp) and committed(h, lp) and sane_pos(lp_pos)):
            found = None
            cam_pos = None
            if cam and looks_camera(h, cam):
                raw = rpm(h, cam + 0x2C, 12)
                if raw:
                    cam_pos = struct.unpack("<fff", raw)
            # Prefer near-table entries close to camera
            near_off = values.get("world.NearEntList", 0xF48)
            near_p = ru64(h, world + near_off)
            near_c = ru32(h, world + near_off + 8) or 0
            cands = []
            if near_p and committed(h, near_p) and 0 < near_c < 64:
                for i in range(min(near_c, 16)):
                    ent = ru64(h, near_p + i * 8)
                    if ent and committed(h, ent):
                        cands.append(ent)
            # Also scan World local-ish slots
            for off in list(range(0x2880, 0x2A80, 8)):
                ent = ru64(h, world + off)
                if ent and committed(h, ent):
                    cands.append(ent)
            best = None
            for ent in cands:
                pos = entity_pos(h, ent, vs_guess)
                if not sane_pos(pos):
                    continue
                score = 10
                if cam_pos and pos:
                    dx, dy, dz = pos[0] - cam_pos[0], pos[1] - cam_pos[1], pos[2] - cam_pos[2]
                    dist = math.sqrt(dx * dx + dy * dy + dz * dz)
                    if dist < 5:
                        score = 100
                    elif dist < 80:
                        score = 50
                # Prefer entity whose type name looks like dayzplayer
                typ = ru64(h, ent + values.get("entity.Type", 0x180))
                if typ and committed(h, typ):
                    score += 5
                if best is None or score > best[0]:
                    # find which world offset holds this ent if any
                    hold = None
                    for off in list(range(0x2880, 0x2A80, 8)):
                        if ru64(h, world + off) == ent:
                            hold = off
                            break
                    best = (score, hold if hold is not None else lp_off, ent, pos)
            if best and best[0] >= 50:
                lp_off, lp, lp_pos = best[1], best[2], best[3]
                add("world.LocalPlayer", lp_off, "PASS", f"rediscovered ptr={lp:#x} score={best[0]} pos={lp_pos}")
            else:
                add("world.LocalPlayer", values.get("world.LocalPlayer", 0x2960), "FAIL", "no local")
                lp = None
        else:
            add("world.LocalPlayer", lp_off, "PASS", f"ptr={lp:#x} pos={lp_pos}")

        if lp_off:
            po = values.get("world.PlayerOn", lp_off + 8)
            add("world.PlayerOn", po, "PASS" if rpm(h, world + po, 8) else "FAIL", "cluster")

        # Lists (NearEnt count is typically ptr+8; catalog has no NearTableSize key)
        near_ptr_off = values.get("world.NearEntList", 0xF48)
        for name_p, name_c, po, co, mx in [
            ("world.NearEntList", None, near_ptr_off, near_ptr_off + 8, 5000),
            ("world.FarEntList", "world.FarTableSize", values.get("world.FarEntList", 0x1090), values.get("world.FarTableSize", 0x1098), 20000),
            ("world.SlowEntList", "world.SlowTableSize", values.get("world.SlowEntList", 0x2010), values.get("world.SlowTableSize", 0x2018), 20000),
            ("world.BulletList", "world.BulletCount", values.get("world.BulletList", 0xE00), values.get("world.BulletCount", 0xE08), 5000),
            ("world.ItemList", "world.ItemListSize", values.get("world.ItemList", 0x2060), values.get("world.ItemListSize", 0x2068), 20000),
        ]:
            p = ru64(h, world + po) if po is not None else None
            c = ru32(h, world + co) if co is not None else None
            ok = (
                bool(p)
                and is_user_ptr(p)
                and committed(h, p)
                and (p >> 40) == (world >> 40)
                and c is not None
                and 0 <= c < mx
            )
            # Empty tables may have null ptr + count 0 — still a valid layout
            # (far/slow often empty until entities stream in; bullets/items same).
            if not ok and name_p in (
                "world.BulletList",
                "world.ItemList",
                "world.FarEntList",
                "world.SlowEntList",
            ) and c == 0:
                ok = True
            add(name_p, po, "PASS" if ok else "FAIL", f"ptr={p} count={c}")
            if name_c:
                add(name_c, co, "PASS" if (ok or c == 0) else "FAIL", f"count={c}")

        # Time / eye / grass / weather
        for key, typ in (
            ("world.EyeAccom", "f32"),
            ("world.Hour", "f32"),
            ("world.Day", "f32"),
            ("world.DayTime", "f32"),
            ("world.NoGrass", "u8"),
            ("world.GrassOffline", "u8"),
            ("world.SlowEntValidCount", "u32"),
        ):
            off = values.get(key)
            if off is None:
                add(key, None, "FAIL", "missing")
                continue
            if typ == "f32":
                v = rf32(h, world + off)
                add(key, off, "PASS" if v is not None and math.isfinite(v) else "FAIL", f"v={v}")
            elif typ == "u8":
                b = rpm(h, world + off, 1)
                add(key, off, "PASS" if b is not None else "FAIL", f"v={b[0] if b else None}")
            else:
                v = ru32(h, world + off)
                add(key, off, "PASS" if v is not None else "FAIL", f"v={v}")

        wc_off = values.get("world.WeatherController", 0x7198)
        wc = ru64(h, world + wc_off)
        add(
            "world.WeatherController",
            wc_off,
            "PASS" if wc and is_user_ptr(wc) and committed(h, wc) else "WARN",
            f"ptr={wc}",
        )

        # Entity / player / inventory from local
        if lp:
            vs_off = values.get("entity.VisualState", 0x1C8)
            fvs_off = values.get("entity.FutureVisualState", 0x120)
            typ_off = values.get("entity.Type", 0x180)
            vs = ru64(h, lp + vs_off)
            fvs = ru64(h, lp + fvs_off)
            typ = ru64(h, lp + typ_off)
            add("entity.VisualState", vs_off, "PASS" if vs and committed(h, vs) and entity_pos(h, lp, vs_off) else "FAIL", f"vs={vs}")
            add("entity.FutureVisualState", fvs_off, "PASS" if fvs and committed(h, fvs) else "WARN", f"fvs={fvs}")
            add("entity.Type", typ_off, "PASS" if typ and committed(h, typ) else "FAIL", f"type={typ}")

            dead_off = values.get("entity.IsDead", 0xE2)
            add("entity.IsDead", dead_off, "PASS" if rpm(h, lp + dead_off, 1) is not None else "FAIL", "")

            for key in ("entity.NetworkId", "entity.NetworkIdPlayer", "entity.Stamina"):
                off = values.get(key)
                add(key, off, "PASS" if off is not None and rpm(h, lp + off, 4) else "FAIL", "")

            inv_off = values.get("player.Inventory", 0x650)
            inv = ru64(h, lp + inv_off)
            add("player.Inventory", inv_off, "PASS" if inv and committed(h, inv) else "FAIL", f"inv={inv}")
            hands_off = values.get("inventory.Hands", 0x1B0)
            if inv and committed(h, inv):
                hands = ru64(h, inv + hands_off)
                add("inventory.Hands", hands_off, "PASS" if hands is not None else "FAIL", f"hands={hands}")
            else:
                add("inventory.Hands", hands_off, "FAIL", "no inv")

            for key in ("player.Skeleton", "player.InputController", "player.StatsContainer", "player.DamageManager"):
                off = values.get(key)
                p = ru64(h, lp + off) if off is not None else None
                # DamageManager can be null depending on player state
                if key == "player.DamageManager":
                    add(key, off, "PASS" if (p and committed(h, p)) else "WARN", f"p={p}")
                else:
                    add(key, off, "PASS" if p and committed(h, p) else "FAIL", f"p={p}")

            rv = values.get("player.RecordValue", 0x2C)
            add("player.RecordValue", rv, "PASS" if rv is not None and rpm(h, lp + rv, 4) else "WARN", "scalar")

            # entitytype via Type
            if typ and committed(h, typ):
                for key in ("entitytype.TypeName", "entitytype.ConfigName", "entitytype.ModelName"):
                    off = values.get(key)
                    sp = ru64(h, typ + off) if off is not None else None
                    ok = enfusion_string_ok(h, sp or 0)
                    add(key, off, "PASS" if ok else "WARN", f"sp={sp}")

            # skeleton / anim chain
            sk_off = values.get("player.Skeleton", 0x7E0)
            sk = ru64(h, lp + sk_off)
            if sk and committed(h, sk):
                for key in ("skeleton.AnimClass1", "skeleton.AnimClass2", "skeleton.AnimComponent", "anim.AnimComponent"):
                    off = values.get(key)
                    p = ru64(h, sk + off) if off is not None else None
                    # AnimComponent often on skeleton-related object
                    add(key, off, "PASS" if (p and committed(h, p)) or (off is not None and rpm(h, sk + off, 8)) else "WARN", f"p={p}")
                anim = None
                for off in (
                    values.get("anim.AnimComponent"),
                    values.get("skeleton.AnimComponent"),
                    values.get("skeleton.AnimClass1"),
                ):
                    if off is None:
                        continue
                    cand = ru64(h, sk + off)
                    if cand and committed(h, cand):
                        mats = ru64(h, cand + values.get("anim.MatrixArray", 0xBE8))
                        if mats and committed(h, mats):
                            anim = cand
                            break
                if anim:
                    add("anim.MatrixArray", values.get("anim.MatrixArray"), "PASS", f"anim={anim:#x}")
                    add("anim.MatrixB", values.get("anim.MatrixB"), "PASS" if rpm(h, anim + values.get("anim.MatrixB", 0x54), 4) else "WARN", "")
                else:
                    add("anim.MatrixArray", values.get("anim.MatrixArray"), "WARN", "chain incomplete")
                    add("anim.MatrixB", values.get("anim.MatrixB"), "WARN", "chain incomplete")
            else:
                for key in ("skeleton.AnimClass1", "skeleton.AnimClass2", "skeleton.AnimComponent", "anim.AnimComponent", "anim.MatrixArray", "anim.MatrixB"):
                    add(key, values.get(key), "WARN", "no skeleton")

            # infected skeleton — soft: just check offset readable on local (may not apply)
            add("infected.Skeleton", values.get("infected.Skeleton"), "WARN", "needs infected entity")
            add("entity_owner.Owner", values.get("entity_owner.Owner"), "WARN", "needs owned entity")
            add("player_identity.Name", values.get("player_identity.Name"), "WARN", "needs identity obj")
            for key in ("scoreboard_identity.Name", "scoreboard_identity.NetworkId", "scoreboard_identity.SteamId"):
                add(key, values.get(key), "WARN", "needs scoreboard entry")

    # --- Globals ---
    for key in (
        "modbase.Network",
        "modbase.NetworkManager",
        "modbase.PhysicsWorld",
        "modbase.FOV_Context",
        "modbase.Tick",
        "modbase.ScopeFovCtx",
    ):
        if any(c.key == key for c in checks):
            continue
        rva = values.get(key)
        if rva is None:
            add(key, None, "FAIL", "missing")
            continue
        obj = ru64(h, base + rva)
        # Tick / Scope may be non-pointer
        if key in ("modbase.Tick",):
            add(key, rva, "PASS" if rpm(h, base + rva, 8) else "FAIL", f"raw ok")
            continue
        ok = obj and is_user_ptr(obj) and committed(h, obj) and not (base <= obj < base + size)
        # NetworkManager global is often a non-heap / coded value; Network is the live object.
        if key == "modbase.NetworkManager":
            raw_ok = rpm(h, base + rva, 8) is not None
            st = "PASS" if ok else ("WARN" if raw_ok else "FAIL")
            add(key, rva, st, f"obj={obj:#x}" if obj else "null")
            continue
        # ScopeFovCtx sometimes empty
        st = "PASS" if ok else ("WARN" if key == "modbase.ScopeFovCtx" else "FAIL")
        add(key, rva, st, f"obj={obj:#x}" if obj else "null")

    add("modbase.FovBase", values.get("modbase.FovBase", 0x9C4), "PASS", "scalar field of FOV ctx")

    # Network fields
    net_rva = values.get("modbase.Network")
    net = ru64(h, base + net_rva) if net_rva else None
    if net and committed(h, net):
        for key in (
            "network.ThirdPersonFlag",
            "network.ScoreboardPtr",
            "network.IdentityCount",
            "network.ScoreboardSize",
            "network.PlayerName",
            "network.ManagerNetworkClient",
        ):
            off = values.get(key)
            add(key, off, "PASS" if off is not None and rpm(h, net + off, 4) else "FAIL", "")
        # scoreboard soft validate
        sb = ru64(h, net + values.get("network.ScoreboardPtr", 0x18))
        if sb and committed(h, sb):
            add("scoreboard_identity.NetworkId", values.get("scoreboard_identity.NetworkId"), "PASS", "scoreboard present")
            add("scoreboard_identity.SteamId", values.get("scoreboard_identity.SteamId"), "PASS", "scoreboard present")
            add("scoreboard_identity.Name", values.get("scoreboard_identity.Name"), "PASS", "scoreboard present")
    else:
        for key in (
            "network.ThirdPersonFlag",
            "network.ScoreboardPtr",
            "network.IdentityCount",
            "network.ScoreboardSize",
            "network.PlayerName",
            "network.ManagerNetworkClient",
        ):
            add(key, values.get(key), "FAIL", "no network")

    # Natives — prefer PE file bytes (process may be patched by Oak 3PP gate).
    for key, name in (
        ("native.Is3rdPersonDisabled", "Is3rdPersonDisabled"),
        ("native.IsCrosshairDisabled", "IsCrosshairDisabled"),
        ("native.CameraViewChanged", "CameraViewChanged"),
    ):
        rva = values.get(key)
        if rva is None:
            hnd = find_native_handler(pe, name)
            if hnd:
                rva = hnd
                values[key] = hnd
        if rva is None:
            add(key, None, "FAIL", "missing")
            continue
        off = pe.rva_to_off(rva)
        raw = pe.data[off : off + 32] if off is not None else b""
        live = rpm(h, base + rva, 8) or b""
        if key == "native.Is3rdPersonDisabled":
            ok = (raw[0:4] == b"\x48\x83\xEC\x28" and b"\x83\x78\x74\x00" in raw) or live[0:3] == b"\x32\xc0\xc3"
            add(key, rva, "PASS" if ok else "FAIL", raw[:16].hex())
        elif key == "native.IsCrosshairDisabled":
            ok = raw[0:4] == b"\x48\x83\xEC\x28" and b"\x83\x78\x78\x00" in raw
            add(key, rva, "PASS" if ok else "FAIL", raw[:16].hex())
        else:
            add(key, rva, "PASS" if raw else "FAIL", raw[:8].hex() if raw else "")

    # Code sites (PE file bytes — Oak may have patched process memory)
    for key in (
        "code.ThirdPersonAllowWriter",
        "code.ThirdPersonAllowCopy0",
        "code.ThirdPersonAllowCopy1",
        "code.AimingModelRecoil",
        "code.CameraFovUpdate",
        "code.DayZPlayerGetName",
        "code.AllocCollisionBuffer",
    ):
        rva = values.get(key)
        if rva is None:
            add(key, None, "WARN" if key.startswith("code.DayZ") or "Alloc" in key else "FAIL", "missing")
            continue
        off = pe.rva_to_off(rva)
        raw = pe.data[off : off + 16] if off is not None else b""
        if key == "code.ThirdPersonAllowWriter":
            ok = len(raw) >= 2 and (raw[0:2] in (b"\x32\xc9", b"\xb1\x01") or b"\x32\xc9" in raw[:8])
            add(key, rva, "PASS" if ok else "FAIL", raw[:8].hex())
        elif key.startswith("code.ThirdPersonAllowCopy"):
            ok = len(raw) >= 7 and raw[0:2] == b"\x40\x88" and raw[3:7] == bytes.fromhex("84290000")
            add(key, rva, "PASS" if ok else "WARN", raw[:7].hex())
        elif key == "code.CameraFovUpdate":
            ok = raw[:3] == b"\x48\x8b\xc4" and b"\xf3\x0f\x10\x89\x94\x01" in pe.data[off : off + 80]
            add(key, rva, "PASS" if ok else "FAIL", raw[:8].hex())
        elif key == "code.AimingModelRecoil":
            add(key, rva, "PASS" if raw else "WARN", raw[:12].hex() if raw else "")
        else:
            add(key, rva, "PASS" if raw else "WARN", raw[:8].hex() if raw else "")

    return checks


def write_hpp(path: Path, values: dict[str, int], stamp: str) -> None:
    groups: dict[str, list[tuple[str, int]]] = {}
    for key, val in values.items():
        if key.startswith(("native.", "code.")):
            continue
        if "." not in key:
            continue
        ns, name = key.split(".", 1)
        groups.setdefault(ns, []).append((name, val))
    order = [
        "modbase",
        "world",
        "camera",
        "entity",
        "inventory",
        "player",
        "network",
        "scoreboard_identity",
        "player_identity",
        "entity_owner",
        "infected",
        "entitytype",
        "anim",
        "skeleton",
    ]
    lines = [
        "#pragma once",
        f"// Auto-generated by dump_all_offsets.py — {stamp}",
        "// Full catalog: static path migrate + live validation PASS required.",
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


def write_report(checks: list[Check], values: dict[str, int], stamp: str) -> None:
    lines = [f"[FULL-DUMP] {stamp}", f"keys_in_values={len(values)} checks={len(checks)}", ""]
    by = {"PASS": 0, "FAIL": 0, "WARN": 0, "SKIP": 0}
    for c in checks:
        by[c.status] = by.get(c.status, 0) + 1
        lines.append(f"[{c.status:4}] {c.key:42} -> {('0x%X'%c.value) if c.value is not None else 'None':12}  {c.note}")
    lines.append("")
    lines.append(f"SUMMARY PASS={by.get('PASS',0)} FAIL={by.get('FAIL',0)} WARN={by.get('WARN',0)}")
    req_fail = [c for c in checks if c.key in REQUIRED and c.status == "FAIL"]
    lines.append(f"REQUIRED_FAILS={len(req_fail)} {[c.key for c in req_fail]}")
    OUT_REPORT.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"[+] report {OUT_REPORT}")
    print(f"SUMMARY PASS={by.get('PASS',0)} FAIL={by.get('FAIL',0)} WARN={by.get('WARN',0)} REQUIRED_FAILS={len(req_fail)}")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def main() -> int:
    ap = argparse.ArgumentParser(description="Full Oak DayZ offset dump + validate")
    ap.add_argument("--old-exe", type=Path, default=DEFAULT_OLD)
    ap.add_argument("--new-exe", type=Path, default=DEFAULT_NEW)
    ap.add_argument("--baseline", type=Path, default=DEFAULT_BASELINE)
    ap.add_argument("--hpp", type=Path, default=DEFAULT_HPP)
    ap.add_argument("--recipes", type=Path, default=DEFAULT_RECIPES)
    ap.add_argument("--prior-recipes", type=Path, default=DEFAULT_MIGRATE)
    ap.add_argument("--learn", action="store_true")
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--live-validate", action="store_true")
    ap.add_argument("--write-hpp", action="store_true")
    ap.add_argument("--snapshot", action="store_true",
                    help="Copy new-exe to baseline and re-learn recipes from CURRENT build")
    ap.add_argument("--full", action="store_true",
                    help="apply+live-validate+write-hpp; learn only if recipes missing")
    ap.add_argument("--allow-warn-write", action="store_true")
    args = ap.parse_args()

    if args.full:
        args.apply = args.live_validate = args.write_hpp = True
        if not args.recipes.exists():
            args.learn = True

    if args.snapshot:
        if not args.new_exe.exists():
            print(f"[!] missing {args.new_exe}", file=sys.stderr)
            return 3
        import shutil
        args.baseline.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(args.new_exe, args.baseline)
        print(f"[+] snapshot baseline -> {args.baseline}")
        args.old_exe = args.baseline
        args.learn = True

    if not (args.learn or args.apply or args.live_validate):
        print(__doc__)
        return 2

    recipes: dict = {}
    results: dict = {}

    if args.learn:
        old_path = args.old_exe if args.old_exe.exists() else args.baseline
        if not old_path.exists():
            print(
                f"[!] missing old/baseline exe at {args.old_exe} / {args.baseline}\n"
                f"    Run --snapshot on a known-good build, or place previous DayZ_x64.exe at --old-exe.",
                file=sys.stderr,
            )
            return 3
        catalog = parse_hpp_offsets(args.hpp) if args.hpp.exists() else {}
        if args.prior_recipes.exists() and not args.snapshot:
            prior = json.loads(args.prior_recipes.read_text(encoding="utf-8"))
            for k, v in (prior.get("old_values") or {}).items():
                catalog.setdefault(k, int(v))
            for k, rec in (prior.get("module_paths") or {}).items():
                if rec.get("old_rva"):
                    catalog[k] = int(rec["old_rva"])
            for k, rec in (prior.get("field_paths") or {}).items():
                if rec.get("old_offset") is not None:
                    catalog[k] = int(rec["old_offset"])
        for k, v in CODE_SITES_CURRENT.items():
            catalog.setdefault(k, v)
        print(f"[*] learn catalog keys={len(catalog)} World=0x{catalog.get('modbase.World', 0):X} from {old_path.name}")
        recipes = learn_all(PeImage(old_path), catalog)
        args.recipes.parent.mkdir(parents=True, exist_ok=True)
        args.recipes.write_text(json.dumps(recipes, indent=2), encoding="utf-8")
        print(f"[+] wrote {args.recipes}")
        if args.snapshot and not args.apply and not args.live_validate:
            return 0

    if args.apply:
        if not recipes:
            if not args.recipes.exists():
                print(f"[!] run --learn/--snapshot first ({args.recipes})", file=sys.stderr)
                return 4
            recipes = json.loads(args.recipes.read_text(encoding="utf-8"))
        if not args.new_exe.exists():
            print(f"[!] missing {args.new_exe}", file=sys.stderr)
            return 5
        new = PeImage(args.new_exe)
        print(f"[*] apply -> {args.new_exe.name}")
        results = apply_all(new, recipes)
        old_pe = None
        if args.old_exe.exists():
            old_pe = PeImage(args.old_exe)
        elif args.baseline.exists():
            old_pe = PeImage(args.baseline)
        apply_code_sites(old_pe, new, recipes, results)
        OUT_JSON.write_text(json.dumps(results, indent=2), encoding="utf-8")
        print(f"[+] wrote {OUT_JSON}")
        OUT_NATIVES.write_text(
            "\n".join(
                f"{k} = 0x{int(r['new']):X}"
                for k, r in sorted(results.items())
                if k.startswith("native.") and r.get("new")
            )
            + "\n",
            encoding="utf-8",
        )
        OUT_SITES.write_text(
            "\n".join(
                f"{k} = 0x{int(r['new']):X}"
                for k, r in sorted(results.items())
                if k.startswith("code.") and r.get("new")
            )
            + "\n",
            encoding="utf-8",
        )
        print(f"[+] wrote {OUT_NATIVES}")
        print(f"[+] wrote {OUT_SITES}")

    if args.live_validate:
        if not results:
            if OUT_JSON.exists():
                results = json.loads(OUT_JSON.read_text(encoding="utf-8"))
            elif args.recipes.exists():
                recipes = json.loads(args.recipes.read_text(encoding="utf-8"))
                results = apply_all(PeImage(args.new_exe), recipes)
                old_pe = (
                    PeImage(args.old_exe)
                    if args.old_exe.exists()
                    else (PeImage(args.baseline) if args.baseline.exists() else None)
                )
                apply_code_sites(old_pe, PeImage(args.new_exe), recipes, results)
            else:
                hppv = parse_hpp_offsets(args.hpp)
                results = {k: {"status": "FROM_HPP", "new": v, "old": v} for k, v in hppv.items()}
                pe0 = PeImage(args.new_exe)
                for k, v in discover_code_sites(pe0).items():
                    results[k] = {"status": "RESOLVED", "new": v}
                for name, key in (
                    ("Is3rdPersonDisabled", "native.Is3rdPersonDisabled"),
                    ("IsCrosshairDisabled", "native.IsCrosshairDisabled"),
                    ("CameraViewChanged", "native.CameraViewChanged"),
                ):
                    hnd = find_native_handler(pe0, name)
                    if hnd:
                        results[key] = {"status": "RESOLVED", "new": hnd}

        values = values_from_results(results)
        pe = PeImage(args.new_exe)
        for name, key in (
            ("Is3rdPersonDisabled", "native.Is3rdPersonDisabled"),
            ("IsCrosshairDisabled", "native.IsCrosshairDisabled"),
            ("CameraViewChanged", "native.CameraViewChanged"),
        ):
            if not values.get(key):
                hnd = find_native_handler(pe, name)
                if hnd:
                    values[key] = hnd
                    results[key] = {"status": "RESOLVED", "new": hnd}
        for k, v in discover_code_sites(pe).items():
            if not values.get(k):
                values[k] = v
                results[k] = {"status": "RESOLVED", "new": v}

        pid = find_pid()
        if not pid:
            print("[!] DayZ_x64.exe not running — launch NoBE in-world first", file=sys.stderr)
            return 6
        h = open_process(pid)
        if not h:
            print("[!] OpenProcess failed", file=sys.stderr)
            return 7
        base, size = module_base_size(h)
        print(f"[*] live validate pid={pid} base=0x{base:X}")
        checks = live_validate(h, base, size, pe, values)
        KERNEL32.CloseHandle(h)

        for c in checks:
            if c.status == "PASS" and c.value is not None:
                values[c.key] = c.value
                results[c.key] = {
                    **results.get(c.key, {}),
                    "status": "LIVE_PASS",
                    "new": c.value,
                    "note": c.note,
                }

        stamp = dt.datetime.now().isoformat(timespec="seconds")
        write_report(checks, values, stamp)
        OUT_JSON.write_text(json.dumps(results, indent=2), encoding="utf-8")
        OUT_NATIVES.write_text(
            "\n".join(
                f"{k} = 0x{int(values[k]):X}"
                for k in sorted(values)
                if k.startswith("native.") and values[k]
            )
            + "\n",
            encoding="utf-8",
        )
        OUT_SITES.write_text(
            "\n".join(
                f"{k} = 0x{int(values[k]):X}"
                for k in sorted(values)
                if k.startswith("code.") and values[k]
            )
            + "\n",
            encoding="utf-8",
        )

        req_fail = [c for c in checks if c.key in REQUIRED and c.status == "FAIL"]
        hard_fail = [c for c in checks if c.status == "FAIL"]
        if req_fail:
            print("[!] REQUIRED keys failed — refusing --write-hpp")
            for c in req_fail:
                print(f"    FAIL {c.key}: {c.note}")
            if args.write_hpp:
                return 8

        if args.write_hpp:
            if hard_fail and not args.allow_warn_write:
                print(f"[!] {len(hard_fail)} FAIL checks — use --allow-warn-write (REQUIRED still blocked)")
                return 9
            write_hpp(args.hpp, values, stamp)
            print(f"[+] wrote {args.hpp}")
            misc = REPO / "src" / "misc_impl.inl"
            if misc.exists() and values.get("native.Is3rdPersonDisabled") and values.get("code.ThirdPersonAllowWriter"):
                t = misc.read_text(encoding="utf-8", errors="replace")
                t2 = re.sub(
                    r"k3ppIs3rdPersonDisabledRva = 0x[0-9A-Fa-f]+ull;",
                    f"k3ppIs3rdPersonDisabledRva = 0x{values['native.Is3rdPersonDisabled']:X}ull;",
                    t,
                    count=1,
                )
                t2 = re.sub(
                    r"k3ppWorldAllowWriterRva = 0x[0-9A-Fa-f]+ull;",
                    f"k3ppWorldAllowWriterRva = 0x{values['code.ThirdPersonAllowWriter']:X}ull;",
                    t2,
                    count=1,
                )
                if values.get("code.ThirdPersonAllowCopy0") and values.get("code.ThirdPersonAllowCopy1"):
                    t2 = re.sub(
                        r"static constexpr UINT_PTR k3ppWorldAllowCopyRvas\[\] = \{[^}]+\};",
                        "static constexpr UINT_PTR k3ppWorldAllowCopyRvas[] = {\n"
                        f"    0x{values['code.ThirdPersonAllowCopy0']:X}ull,\n"
                        f"    0x{values['code.ThirdPersonAllowCopy1']:X}ull,\n"
                        "};",
                        t2,
                        count=1,
                        flags=re.S,
                    )
                if t2 != t:
                    misc.write_text(t2, encoding="utf-8")
                    print(f"[+] updated 3PP RVAs in {misc.name}")
            b4 = REPO / "src" / "oak_batch4_misc.inl"
            if b4.exists() and values.get("code.CameraFovUpdate"):
                t = b4.read_text(encoding="utf-8", errors="replace")
                t2 = re.sub(
                    r"kCamUpdateRva = 0x[0-9A-Fa-f]+ull",
                    f"kCamUpdateRva = 0x{values['code.CameraFovUpdate']:X}ull",
                    t,
                    count=1,
                )
                if t2 != t:
                    b4.write_text(t2, encoding="utf-8")
                    print(f"[+] updated FOV entry RVA in {b4.name}")
            combat = REPO / "src" / "oak_batch4_combat.inl"
            if combat.exists() and values.get("code.AimingModelRecoil"):
                t = combat.read_text(encoding="utf-8", errors="replace")
                t2 = re.sub(
                    r"modBase \+ 0x[0-9A-Fa-f]+ull",
                    f"modBase + 0x{values['code.AimingModelRecoil']:X}ull",
                    t,
                    count=1,
                )
                if t2 != t:
                    combat.write_text(t2, encoding="utf-8")
                    print(f"[+] updated recoil site in {combat.name}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
