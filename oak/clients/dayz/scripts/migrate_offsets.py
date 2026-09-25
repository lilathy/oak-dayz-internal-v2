#!/usr/bin/env python3
"""
Oak DayZ — path-based offset migrator

Idea (what you asked for):
  1) Take the OLD DayZ_x64.exe + the known-good offsets that worked on it.
  2) For every MODULE RVA (globals / functions), find the code sites that
     reference that RVA (RIP-relative mov/lea), and record a unique byte PATH
     (signature with the disp32 wildcarded).
  3) On the NEW DayZ_x64.exe, search for those same paths and resolve the new
     disp32 → new RVA.
  4) Struct-field offsets (World+Camera, etc.) are recorded as "field" recipes
     and re-validated live; many survive patches unchanged.

Without --old-exe this cannot invent paths (Steam already overwrote the old
client on this machine). Put the previous build at e.g.:
  C:\\oak\\dayz\\DayZ_x64_OLD.exe
then:

  python migrate_offsets.py --old-exe C:\\oak\\dayz\\DayZ_x64_OLD.exe ^
    --new-exe "C:\\Program Files (x86)\\Steam\\steamapps\\common\\DayZ\\DayZ_x64.exe" ^
    --old-hpp ..\\src\\offsets_generated.hpp ^
    --learn recipes.json --apply

Future updates only need:
  python migrate_offsets.py --new-exe <new> --recipes recipes.json --apply
"""
from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
DEFAULT_HPP = REPO / "src" / "offsets_generated.hpp"
DEFAULT_NEW = Path(r"C:\Program Files (x86)\Steam\steamapps\common\DayZ\DayZ_x64.exe")
DEFAULT_RECIPES = REPO / "scripts" / "offset_recipes.json"

IMAGE_SCN_MEM_EXECUTE = 0x20000000
IMAGE_SCN_MEM_WRITE = 0x80000000

# Keys that are module RVAs (migrate via code xref signatures).
MODULE_KEYS = {
    "modbase.World",
    "modbase.FOV_Context",
    "modbase.Network",
    "modbase.NetworkManager",
    "modbase.Tick",
    "modbase.ScopeFovCtx",
    "modbase.PhysicsWorld",
}

# Keys that are struct fields (path = parent + relationship; often stable).
FIELD_KEYS_PARENT = {
    "world.Camera": "modbase.World",
    "world.LocalPlayer": "modbase.World",
    "world.PlayerOn": "modbase.World",
    "world.NearEntList": "modbase.World",
    "world.FarEntList": "modbase.World",
    "world.FarTableSize": "modbase.World",
    "world.SlowEntList": "modbase.World",
    "world.SlowTableSize": "modbase.World",
    "world.BulletList": "modbase.World",
    "world.BulletCount": "modbase.World",
    "world.ItemList": "modbase.World",
    "world.ItemListSize": "modbase.World",
    "world.NoGrass": "modbase.World",
    "world.GrassOffline": "modbase.World",
    "world.EyeAccom": "modbase.World",
    "world.Hour": "modbase.World",
    "world.Day": "modbase.World",
    "world.DayTime": "modbase.World",
    "world.WeatherController": "modbase.World",
    "world.SlowEntValidCount": "modbase.World",
    "camera.InvertedViewRight": "world.Camera",
    "camera.InvertedViewUp": "world.Camera",
    "camera.InvertedViewForward": "world.Camera",
    "camera.InvertedViewTranslation": "world.Camera",
    "camera.ViewPortSize": "world.Camera",
    "camera.GetProjectionD1": "world.Camera",
    "camera.GetProjectionD2": "world.Camera",
    "entity.Type": "entity",
    "entity.VisualState": "entity",
    "entity.FutureVisualState": "entity",
    "entity.IsDead": "entity",
    "entity.NetworkId": "entity",
    "entity.NetworkIdPlayer": "entity",
    "entity.Stamina": "entity",
    "player.Skeleton": "player",
    "player.Inventory": "player",
    "player.InputController": "player",
    "player.StatsContainer": "player",
    "player.RecordValue": "player",
    "player.DamageManager": "player",
    "inventory.Hands": "inventory",
    "network.ThirdPersonFlag": "modbase.Network",
    "network.ScoreboardSize": "modbase.Network",
    "network.ManagerNetworkClient": "modbase.Network",
    "network.PlayerName": "modbase.Network",
    "network.ScoreboardPtr": "modbase.Network",
    "network.IdentityCount": "modbase.Network",
}

# String → native handler paths (no old exe required).
NATIVE_STRING_PATHS = {
    "native.Is3rdPersonDisabled": "Is3rdPersonDisabled",
    "native.IsCrosshairDisabled": "IsCrosshairDisabled",
    "native.CameraViewChanged": "CameraViewChanged",
}


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


def parse_hpp_offsets(path: Path) -> dict[str, int]:
    text = path.read_text(encoding="utf-8", errors="replace")
    out: dict[str, int] = {}
    ns = None
    for line in text.splitlines():
        m = re.match(r"\s*namespace\s+(\w+)\s*\{", line)
        if m:
            ns = m.group(1)
            continue
        if re.match(r"\s*\}", line) and ns and "namespace" not in line:
            # closing namespace — heuristic ok
            pass
        m = re.search(r"OAK_RUNTIME_OFFSET\(\s*(\w+)\s*,\s*(0x[0-9A-Fa-f]+|\d+)\s*\)", line)
        if m and ns and ns != "oak_offsets":
            out[f"{ns}.{m.group(1)}"] = int(m.group(2), 0)
    return out


def find_rip_xrefs_to(pe: PeImage, target_rva: int) -> list[int]:
    """Return instruction RVAs of mov/lea that RIP-reference target_rva."""
    hits = []
    prefs = {
        b"\x48\x8B\x05", b"\x48\x8B\x0D", b"\x48\x8B\x15", b"\x48\x8B\x1D", b"\x48\x8B\x3D",
        b"\x4C\x8B\x05", b"\x4C\x8B\x0D", b"\x4C\x8B\x15", b"\x4C\x8B\x1D",
        b"\x48\x8D\x05", b"\x48\x8D\x0D", b"\x48\x8D\x15",
        b"\x4C\x8D\x05", b"\x4C\x8D\x0D",
    }
    for _n, _va, _vs, raw, rs, chars in pe.secs:
        if not (chars & IMAGE_SCN_MEM_EXECUTE):
            continue
        data = pe.data
        end = raw + rs - 7
        i = raw
        while i < end:
            if data[i : i + 3] in prefs:
                disp = struct.unpack_from("<i", data, i + 3)[0]
                instr = pe.off_to_rva(i)
                if instr is not None and instr + 7 + disp == target_rva:
                    hits.append(instr)
                i += 7
                continue
            i += 1
    return hits


def extract_path_sig(pe: PeImage, instr_rva: int, before: int = 12, after: int = 8) -> dict | None:
    """
    Signature covering [instr-before, instr+7+after] with the 4-byte RIP disp wildcarded.
    """
    off = pe.rva_to_off(instr_rva - before)
    if off is None:
        return None
    total = before + 7 + after
    raw = pe.data[off : off + total]
    if len(raw) < total:
        return None
    # wildcard indices for disp32 inside the 7-byte rip instr at offset `before`
    wild = list(range(before + 3, before + 7))
    return {
        "before": before,
        "after": after,
        "bytes": raw.hex(),
        "wildcards": wild,
        "old_instr_rva": instr_rva,
    }


def match_sig(pe: PeImage, sig: dict) -> list[tuple[int, int]]:
    """Return list of (instr_rva, resolved_target_rva)."""
    needle = bytes.fromhex(sig["bytes"])
    wild = set(sig["wildcards"])
    before = sig["before"]
    hits = []
    data = pe.data
    # Only search executable sections
    for _n, _va, _vs, raw, rs, chars in pe.secs:
        if not (chars & IMAGE_SCN_MEM_EXECUTE):
            continue
        start, end = raw, raw + rs - len(needle)
        for i in range(start, max(start, end) + 1):
            window = data[i : i + len(needle)]
            if len(window) < len(needle):
                break
            ok = True
            for j, b in enumerate(needle):
                if j in wild:
                    continue
                if window[j] != b:
                    ok = False
                    break
            if not ok:
                continue
            instr_off = i + before
            instr_rva = pe.off_to_rva(instr_off)
            if instr_rva is None:
                continue
            disp = struct.unpack_from("<i", data, instr_off + 3)[0]
            target = instr_rva + 7 + disp
            hits.append((instr_rva, target))
    return hits


def find_gate_by_field(pe: PeImage, field_imm: int) -> int | None:
    """Find Enfusion Is*Disabled gate: cmp dword [rax+imm8],0; je; mov al,1."""
    # 83 78 <imm> 00 74 07 B0 01
    needle = bytes([0x83, 0x78, field_imm & 0xFF, 0x00, 0x74, 0x07, 0xB0, 0x01])
    start = 0
    hits: list[int] = []
    while True:
        i = pe.data.find(needle, start)
        if i < 0:
            break
        # prologue 48 83 EC 28 within 0x30 bytes back
        for back in range(0, 0x30):
            if pe.data[i - back : i - back + 4] == b"\x48\x83\xEC\x28":
                r = pe.off_to_rva(i - back)
                if r is not None:
                    hits.append(r)
                break
        start = i + 1
    return hits[0] if len(hits) == 1 else None


def find_native_handler(pe: PeImage, name: str) -> int | None:
    # Script-native string xref can land on a different wrapper than the C++ gate
    # that Oak patches. Prefer the unique cmp [rax+imm] body for these two.
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
    for _n, _va, _vs, raw, rs, chars in pe.secs:
        if not (chars & IMAGE_SCN_MEM_EXECUTE):
            continue
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


def learn_recipes(old: PeImage, offsets: dict[str, int]) -> dict:
    recipes = {"version": 1, "module_paths": {}, "field_paths": {}, "native_paths": {}, "old_values": offsets}
    for key in sorted(MODULE_KEYS):
        if key not in offsets:
            continue
        rva = offsets[key]
        xrefs = find_rip_xrefs_to(old, rva)
        print(f"[learn] {key} old=0x{rva:X} xrefs={len(xrefs)}")
        sigs = []
        for xr in xrefs[:12]:
            sig = extract_path_sig(old, xr)
            if not sig:
                continue
            # Prefer signatures that uniquely resolve back to the same RVA on old
            matches = match_sig(old, sig)
            targets = {t for _i, t in matches}
            if targets == {rva}:
                sigs.append(sig)
            elif rva in targets and len(targets) <= 3:
                sig["ambiguous"] = sorted(targets)
                sigs.append(sig)
        recipes["module_paths"][key] = {
            "old_rva": rva,
            "xref_count": len(xrefs),
            "signatures": sigs[:6],
        }
        print(f"        unique-ish sigs kept={len(recipes['module_paths'][key]['signatures'])}")

    for key, parent in FIELD_KEYS_PARENT.items():
        if key not in offsets:
            continue
        recipes["field_paths"][key] = {
            "parent": parent,
            "old_offset": offsets[key],
            "note": "struct field; migrate by keeping value then live-validate",
        }

    for key, name in NATIVE_STRING_PATHS.items():
        h = find_native_handler(old, name)
        recipes["native_paths"][key] = {
            "string": name,
            "old_rva": h,
            "path": "lea_r9_handler_then_lea_name",
        }
        print(f"[learn] {key} string={name} old_handler={hex(h) if h else None}")

    return recipes


def apply_recipes(new: PeImage, recipes: dict) -> dict[str, dict]:
    results = {}
    for key, rec in recipes.get("module_paths", {}).items():
        resolved = None
        votes: dict[int, int] = {}
        for sig in rec.get("signatures", []):
            for _instr, tgt in match_sig(new, sig):
                if not new.is_writable(tgt) and "World" in key:
                    # World must be writable global; others often writable too
                    pass
                votes[tgt] = votes.get(tgt, 0) + 1
        if votes:
            resolved = sorted(votes.items(), key=lambda kv: (-kv[1], kv[0]))[0][0]
            results[key] = {
                "status": "RESOLVED",
                "old": rec.get("old_rva"),
                "new": resolved,
                "votes": votes.get(resolved, 0),
                "candidates": len(votes),
            }
            print(f"[apply] {key}: 0x{rec.get('old_rva'):X} -> 0x{resolved:X} (votes={votes[resolved]})")
        else:
            results[key] = {"status": "UNRESOLVED", "old": rec.get("old_rva"), "new": None}
            print(f"[apply] {key}: UNRESOLVED")

    # Fail-soft: if every signature-resolved modbase shares one delta, fill the rest.
    deltas = [
        int(r["new"]) - int(r["old"])
        for r in results.values()
        if r.get("status") == "RESOLVED" and r.get("old") is not None and r.get("new") is not None
    ]
    if deltas and len(set(deltas)) == 1:
        delta = deltas[0]
        for key, r in list(results.items()):
            if r.get("status") != "UNRESOLVED" or r.get("old") is None:
                continue
            if not key.startswith("modbase."):
                continue
            guess = int(r["old"]) + delta
            results[key] = {
                "status": "RESOLVED_DELTA",
                "old": r["old"],
                "new": guess,
                "delta": delta,
            }
            print(f"[apply] {key}: 0x{r['old']:X} -> 0x{guess:X} (delta={delta:#x})")

    for key, rec in recipes.get("field_paths", {}).items():
        results[key] = {
            "status": "KEPT_FIELD",
            "old": rec["old_offset"],
            "new": rec["old_offset"],
            "note": "field path — verify live after module migrate",
        }

    for key, rec in recipes.get("native_paths", {}).items():
        h = find_native_handler(new, rec["string"])
        results[key] = {
            "status": "RESOLVED" if h else "UNRESOLVED",
            "old": rec.get("old_rva"),
            "new": h,
            "string": rec["string"],
        }
        print(f"[apply] {key}: {rec['string']} -> {hex(h) if h else None}")

    return results


def write_hpp(path: Path, results: dict, recipes: dict, stamp: str) -> None:
    # Start from old values, overlay resolved module + kept fields
    values = dict(recipes.get("old_values", {}))
    for key, r in results.items():
        if r.get("new") is None:
            continue
        if key.startswith("native."):
            continue
        values[key] = int(r["new"])

    groups: dict[str, list[tuple[str, int]]] = {}
    for key, val in values.items():
        if "." not in key:
            continue
        ns, name = key.split(".", 1)
        groups.setdefault(ns, []).append((name, val))

    order = [
        "modbase", "world", "camera", "entity", "inventory", "player", "network",
        "scoreboard_identity", "player_identity", "entity_owner", "infected",
        "entitytype", "anim", "skeleton",
    ]
    lines = [
        "#pragma once",
        f"// Auto-generated by migrate_offsets.py — {stamp}",
        "// Module RVAs via path signatures learned on old build; fields kept pending live validate.",
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


def main() -> int:
    ap = argparse.ArgumentParser(description="Path-based DayZ offset migrator")
    ap.add_argument("--old-exe", type=Path, help="Previous DayZ_x64.exe (required to --learn)")
    ap.add_argument("--new-exe", type=Path, default=DEFAULT_NEW)
    ap.add_argument("--old-hpp", type=Path, default=DEFAULT_HPP, help="Known-good offsets for old build")
    ap.add_argument("--recipes", type=Path, default=DEFAULT_RECIPES)
    ap.add_argument("--learn", action="store_true", help="Learn paths from old-exe + old-hpp")
    ap.add_argument("--apply", action="store_true", help="Apply recipes to new-exe and write hpp")
    ap.add_argument("--out-hpp", type=Path, default=DEFAULT_HPP)
    args = ap.parse_args()

    if not args.learn and not args.apply:
        print("Specify --learn and/or --apply", file=sys.stderr)
        print(__doc__)
        return 2

    recipes = {}
    if args.learn:
        if not args.old_exe or not args.old_exe.exists():
            print(
                "[!] --learn needs the PREVIOUS DayZ_x64.exe.\n"
                "    Steam already replaced the install on this PC.\n"
                "    Place a copy at e.g. C:\\oak\\dayz\\DayZ_x64_OLD.exe and pass --old-exe.\n"
                "    Or download the prior depot/build, then re-run --learn.",
                file=sys.stderr,
            )
            return 3
        if not args.old_hpp.exists():
            print(f"[!] missing {args.old_hpp}", file=sys.stderr)
            return 3
        old = PeImage(args.old_exe)
        offsets = parse_hpp_offsets(args.old_hpp)
        print(f"[*] learning from {args.old_exe.name} offsets={len(offsets)}")
        recipes = learn_recipes(old, offsets)
        args.recipes.write_text(json.dumps(recipes, indent=2), encoding="utf-8")
        print(f"[+] wrote recipes {args.recipes}")

    if args.apply:
        if not recipes:
            if not args.recipes.exists():
                print(f"[!] no recipes at {args.recipes} — run --learn first", file=sys.stderr)
                return 4
            recipes = json.loads(args.recipes.read_text(encoding="utf-8"))
        if not args.new_exe.exists():
            print(f"[!] missing new exe {args.new_exe}", file=sys.stderr)
            return 5
        new = PeImage(args.new_exe)
        print(f"[*] applying recipes to {args.new_exe.name}")
        results = apply_recipes(new, recipes)
        report = args.recipes.with_suffix(".apply.json")
        report.write_text(json.dumps(results, indent=2), encoding="utf-8")
        print(f"[+] wrote {report}")
        if args.out_hpp:
            import datetime as dt
            stamp = dt.datetime.now().isoformat(timespec="seconds")
            write_hpp(args.out_hpp, results, recipes, stamp)
            print(f"[+] wrote {args.out_hpp}")
            # Also dump natives sidecar
            natives = {k: v["new"] for k, v in results.items() if k.startswith("native.") and v.get("new")}
            side = REPO / "docs" / "engine" / "NATIVE_RVAS.txt"
            side.parent.mkdir(parents=True, exist_ok=True)
            side.write_text("\n".join(f"{k} = 0x{int(v):X}" for k, v in sorted(natives.items())) + "\n", encoding="utf-8")

    return 0


if __name__ == "__main__":
    sys.exit(main())
