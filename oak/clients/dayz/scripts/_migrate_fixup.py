"""Post-apply: delta-fill unresolved modbase + remap 3PP/recoil hard RVAs."""
from __future__ import annotations

import json
import re
import struct
from pathlib import Path

OLD = Path(r"C:\oak\dayz\DayZ_x64_OLD.exe")
NEW = Path(r"C:\Program Files (x86)\Steam\steamapps\common\DayZ\DayZ_x64.exe")
HPP = Path(__file__).resolve().parents[1] / "src" / "offsets_generated.hpp"
APPLY = Path(__file__).resolve().parents[1] / "docs" / "engine" / "offset_recipes.apply.json"
NATIVE = Path(__file__).resolve().parents[1] / "docs" / "engine" / "NATIVE_RVAS.txt"
MISC = Path(__file__).resolve().parents[1] / "src" / "misc_impl.inl"
ENGINE = Path(__file__).resolve().parents[1] / "src" / "oak_engine.cpp"
COMBAT = Path(__file__).resolve().parents[1] / "src" / "oak_batch4_combat.inl"
AMMO = Path(__file__).resolve().parents[1] / "src" / "ammo_probe.cpp"


def load_pe(p: Path):
    data = p.read_bytes()
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    coff = e_lfanew + 4
    nsec = struct.unpack_from("<H", data, coff + 2)[0]
    opt = coff + 20
    size_opt = struct.unpack_from("<H", data, coff + 16)[0]
    sec = opt + size_opt
    sections = []
    for i in range(nsec):
        off = sec + i * 40
        name = data[off : off + 8].split(b"\0", 1)[0].decode("ascii", "ignore")
        vsz, va, rsz, raw = struct.unpack_from("<IIII", data, off + 8)
        sections.append((name, va, vsz, raw, rsz))
    return data, sections


def rva_to_off(sections, rva):
    for name, va, vsz, raw, rsz in sections:
        if va <= rva < va + max(vsz, rsz):
            return raw + (rva - va)
    return None


def off_to_rva(sections, off):
    for name, va, vsz, raw, rsz in sections:
        if raw <= off < raw + rsz:
            return va + (off - raw)
    return None


def count_rip_refs(data, sections, target_rva, text_only=True):
    hits = 0
    for name, va, vsz, raw, rsz in sections:
        if text_only and name not in (".text", "CODE"):
            # DayZ uses .text typically
            if name != ".text":
                continue
        end = raw + rsz
        i = raw
        while i + 7 <= end:
            b = data[i]
            # mov/lea r64,[rip+disp] : 48/4C 8B/8D 05/0D/15/1D/25/2D/35/3D
            if b in (0x48, 0x4C) and data[i + 1] in (0x8B, 0x8D) and data[i + 2] in (
                0x05,
                0x0D,
                0x15,
                0x1D,
                0x25,
                0x2D,
                0x35,
                0x3D,
            ):
                disp = struct.unpack_from("<i", data, i + 3)[0]
                instr_rva = off_to_rva(sections, i)
                if instr_rva is not None:
                    tgt = instr_rva + 7 + disp
                    if tgt == target_rva:
                        hits += 1
                i += 7
                continue
            i += 1
    return hits


def find_store_2984(data, sections):
    """Find World+0x2984 stores: 88 ?? 84 29 00 00 with nearby xor cl,cl or mov cl,1."""
    needle = bytes.fromhex("8884 290000")  # 88 ?? 84 29 00 00 — need flexible mid byte
    # Use regex-like scan for 88 ?? 84 29 00 00
    hits = []
    for name, va, vsz, raw, rsz in sections:
        if name != ".text":
            continue
        chunk = data[raw : raw + rsz]
        for i in range(len(chunk) - 6):
            if chunk[i] == 0x88 and chunk[i + 2 : i + 6] == bytes.fromhex("84290000"):
                off = raw + i
                rva = va + i
                win = chunk[max(0, i - 0x40) : i + 8]
                has_xor = b"\x32\xc9" in win
                has_mov = b"\xb1\x01" in win
                # also 40 88 A9/AB 84 29 00 00 style (REX + mov r/m8)
                hits.append(
                    {
                        "rva": rva,
                        "modrm": chunk[i + 1],
                        "has_xor": has_xor,
                        "has_mov": has_mov,
                        "prev7": chunk[max(0, i - 7) : i].hex(),
                    }
                )
            # REX.W/R forms: 40 88 A9/AB 84 29 00 00
            if (
                chunk[i] == 0x40
                and chunk[i + 1] == 0x88
                and chunk[i + 3 : i + 7] == bytes.fromhex("84290000")
            ):
                hits.append(
                    {
                        "rva": va + i,
                        "kind": "rex_mov",
                        "bytes": chunk[i : i + 7].hex(),
                        "reg": chunk[i + 2],
                    }
                )
    return hits


def migrate_code_site(old_data, osecs, new_data, nsecs, old_rva, window=0x30, wild_imm=True):
    """Build signature from old site and find unique match on new."""
    ooff = rva_to_off(osecs, old_rva)
    if ooff is None:
        return None
    # take bytes from old_rva-8 .. old_rva+window
    start = ooff - 8
    blob = old_data[start : ooff + window]
    # wildcard RIP-relative disp32 after 48/4C 8B/8D ?? 
    # simpler: exact match of distinctive opcode run at site
    # Prefer exact 24-byte sequence at old_rva
    exact = old_data[ooff : ooff + 24]
    # find all exact in new
    idxs = []
    start_i = 0
    while True:
        i = new_data.find(exact, start_i)
        if i < 0:
            break
        idxs.append(i)
        start_i = i + 1
    if len(idxs) == 1:
        return off_to_rva(nsecs, idxs[0])
    # try shorter unique prefix
    for n in (20, 16, 12, 10, 8):
        exact = old_data[ooff : ooff + n]
        idxs = []
        start_i = 0
        while True:
            i = new_data.find(exact, start_i)
            if i < 0:
                break
            idxs.append(i)
            start_i = i + 1
        if len(idxs) == 1:
            return off_to_rva(nsecs, idxs[0])
    # try context with wildcards for relative calls (E8 xx xx xx xx)
    pat = bytearray(old_data[ooff - 4 : ooff + 28])
    # mark E8 rel32
    i = 0
    mask = bytearray([0xFF] * len(pat))
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
    for name, va, vsz, raw, rsz in nsecs:
        if name != ".text":
            continue
        chunk = new_data[raw : raw + rsz]
        for i in range(len(chunk) - len(pat)):
            ok = True
            for j, b in enumerate(pat):
                if mask[j] and chunk[i + j] != b:
                    ok = False
                    break
            if ok:
                matches.append(va + i + 4)  # pat started 4 before old_rva
    if len(matches) == 1:
        return matches[0]
    return matches  # may be list


def main():
    old, osecs = load_pe(OLD)
    new, nsecs = load_pe(NEW)
    apply = json.loads(APPLY.read_text(encoding="utf-8"))

    deltas = []
    for k, v in apply.items():
        if k.startswith("modbase.") and v.get("status") == "RESOLVED" and v.get("old") and v.get("new"):
            deltas.append(int(v["new"]) - int(v["old"]))
    assert deltas, "no resolved modbase"
    # require unanimous delta
    if len(set(deltas)) != 1:
        print("[!] non-unanimous deltas", set(hex(d) for d in deltas))
        # use mode
        from collections import Counter

        delta = Counter(deltas).most_common(1)[0][0]
    else:
        delta = deltas[0]
    print(f"[*] unanimous-ish module delta = {delta:#x} ({delta})")

    filled = {}
    for k, v in apply.items():
        if not k.startswith("modbase."):
            continue
        if v.get("status") == "RESOLVED":
            filled[k] = int(v["new"])
            continue
        old_rva = int(v["old"])
        guess = old_rva + delta
        refs = count_rip_refs(new, nsecs, guess)
        filled[k] = guess
        apply[k] = {
            "status": "RESOLVED_DELTA",
            "old": old_rva,
            "new": guess,
            "delta": delta,
            "rip_refs": refs,
        }
        print(f"[delta] {k}: {old_rva:#x} -> {guess:#x} rip_refs={refs}")

    # Patch hpp modbase lines
    text = HPP.read_text(encoding="utf-8")
    for k, val in filled.items():
        ns, name = k.split(".", 1)
        if ns != "modbase":
            continue
        text, n = re.subn(
            rf"(OAK_RUNTIME_OFFSET\({name},\s*)0x[0-9A-Fa-f]+(\);)",
            rf"\g<1>0x{val:X}\2",
            text,
            count=1,
        )
        if n != 1:
            print(f"[!] hpp replace failed for {name}")
    # stamp note
    text = re.sub(
        r"// Auto-generated by migrate_offsets\.py — [^\n]+",
        "// Auto-generated by migrate_offsets.py + delta-fill — 2026-08-12",
        text,
        count=1,
    )
    HPP.write_text(text, encoding="utf-8")
    APPLY.write_text(json.dumps(apply, indent=2), encoding="utf-8")
    print(f"[+] updated {HPP}")

    # --- 3PP allow writer ---
    print("\n=== 3PP World+0x2984 stores ===")
    old_hits = find_store_2984(old, osecs)
    new_hits = find_store_2984(new, nsecs)
    print(f"old hits={len(old_hits)} new hits={len(new_hits)}")
    old_xor = [h for h in old_hits if h.get("has_xor")]
    new_xor = [h for h in new_hits if h.get("has_xor")]
    old_rex = [h for h in old_hits if h.get("kind") == "rex_mov"]
    new_rex = [h for h in new_hits if h.get("kind") == "rex_mov"]
    print("old xor-context stores:", [hex(h["rva"]) for h in old_xor])
    print("new xor-context stores:", [hex(h["rva"]) for h in new_xor])
    print("old rex copies:", [(hex(h["rva"]), h.get("bytes")) for h in old_rex])
    print("new rex copies:", [(hex(h["rva"]), h.get("bytes")) for h in new_rex])

    # Map old writer 0xA85DC2
    writer_new = migrate_code_site(old, osecs, new, nsecs, 0xA85DC2)
    print("writer_new from sig:", writer_new if not isinstance(writer_new, list) else [hex(x) for x in writer_new[:10]])

    # Prefer new_xor site: distance from store back to xor cl,cl
    writer_rva = None
    if isinstance(writer_new, int):
        writer_rva = writer_new
    elif new_xor:
        # find xor cl,cl before store
        for h in new_xor:
            o = rva_to_off(nsecs, h["rva"])
            win = new[o - 0x30 : o]
            rel = win.rfind(b"\x32\xc9")
            if rel >= 0:
                writer_rva = h["rva"] - (0x30 - rel)
                print(f" derived writer from store {h['rva']:#x} -> {writer_rva:#x}")
                break

    copy_rvas = []
    for h in new_rex:
        # match known old patterns 40 88 A9 / 40 88 AB
        if h.get("bytes", "").startswith("4088a9") or h.get("bytes", "").startswith("4088ab"):
            copy_rvas.append(h["rva"])
    # if not found, migrate old copy sites
    for old_c in (0x53C604, 0x5F5624):
        m = migrate_code_site(old, osecs, new, nsecs, old_c)
        print(f"copy migrate {old_c:#x} ->", hex(m) if isinstance(m, int) else m)
        if isinstance(m, int):
            copy_rvas.append(m)
    copy_rvas = sorted(set(copy_rvas))
    print("copy_rvas final:", [hex(x) for x in copy_rvas])

    # Recoil hook 0x4E9D16
    recoil = migrate_code_site(old, osecs, new, nsecs, 0x4E9D16)
    print("recoil 0x4E9D16 ->", hex(recoil) if isinstance(recoil, int) else recoil)

    # Verify Is3rdPersonDisabled bytes at 0x8F5F40
    native = 0x8F5F40
    no = rva_to_off(nsecs, native)
    nb = new[no : no + 32]
    print("Is3rdPersonDisabled@0x8F5F40:", nb.hex())

    # Update misc_impl.inl constants
    misc = MISC.read_text(encoding="utf-8")
    if writer_rva is not None:
        misc2, n = re.subn(
            r"static constexpr UINT_PTR k3ppWorldAllowWriterRva = 0x[0-9A-Fa-f]+ull;",
            f"static constexpr UINT_PTR k3ppWorldAllowWriterRva = 0x{writer_rva:X}ull;",
            misc,
            count=1,
        )
        print(f"writer RVA update n={n} -> {writer_rva:#x}")
        misc = misc2
    if len(copy_rvas) >= 2:
        # replace array contents
        arr = ",\n    ".join(f"0x{r:X}ull" for r in copy_rvas[:2])
        misc2, n = re.subn(
            r"static constexpr UINT_PTR k3ppWorldAllowCopyRvas\[\] = \{[^}]+\};",
            "static constexpr UINT_PTR k3ppWorldAllowCopyRvas[] = {\n    " + arr + ",\n};",
            misc,
            count=1,
            flags=re.S,
        )
        print(f"copy RVAs update n={n}")
        misc = misc2
    # comment refresh
    misc = misc.replace(
        "// 1) Patch Is3rdPersonDisabled @ RVA 0x8EDC90 → xor al,al; ret",
        "// 1) Patch Is3rdPersonDisabled @ RVA 0x8F5F40 → xor al,al; ret",
    )
    MISC.write_text(misc, encoding="utf-8")

    # oak_engine hardcoded FOV
    eng = ENGINE.read_text(encoding="utf-8")
    fov = filled.get("modbase.FOV_Context")
    if fov:
        eng2, n = re.subn(r"g_GameBase \+ 0x1008CE0", f"g_GameBase + 0x{fov:X}", eng)
        print(f"oak_engine FOV hardcode n={n} -> {fov:#x}")
        ENGINE.write_text(eng2, encoding="utf-8")

    # ammo_probe World
    world = filled.get("modbase.World")
    if world and AMMO.exists():
        ammo = AMMO.read_text(encoding="utf-8")
        ammo2, n = re.subn(r"base \+ 0x4264058", f"base + 0x{world:X}", ammo)
        print(f"ammo_probe World n={n} -> {world:#x}")
        AMMO.write_text(ammo2, encoding="utf-8")

    # combat recoil comment / constant if present
    if isinstance(recoil, int) and COMBAT.exists():
        combat = COMBAT.read_text(encoding="utf-8")
        if "0x4E9D16" in combat:
            combat2 = combat.replace("0x4E9D16", f"0x{recoil:X}")
            COMBAT.write_text(combat2, encoding="utf-8")
            print(f"combat recoil -> {recoil:#x}")

    # natives sidecar already good; append delta note
    note = Path(__file__).resolve().parents[1] / "docs" / "engine" / "MIGRATE_RESULT.txt"
    lines = [
        f"module_delta={delta:#x}",
        *[f"{k}=0x{v:X}" for k, v in sorted(filled.items())],
        f"k3ppWorldAllowWriterRva={writer_rva:#x}" if writer_rva else "writer=UNRESOLVED",
        f"copy_rvas={[hex(x) for x in copy_rvas]}",
        f"recoil={hex(recoil) if isinstance(recoil,int) else recoil}",
        f"Is3rdPersonDisabled=0x8F5F40",
        f"IsCrosshairDisabled=0x8ED160",
        f"CameraViewChanged=0x324560",
    ]
    note.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"[+] {note}")


if __name__ == "__main__":
    main()
