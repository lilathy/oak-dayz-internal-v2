import struct
from pathlib import Path

new = Path(r"C:\Program Files (x86)\Steam\steamapps\common\DayZ\DayZ_x64.exe").read_bytes()
e = struct.unpack_from("<I", new, 0x3C)[0]
coff = e + 4
nsec = struct.unpack_from("<H", new, coff + 2)[0]
opt = coff + 20
size_opt = struct.unpack_from("<H", new, coff + 16)[0]
sec = opt + size_opt
secs = []
for i in range(nsec):
    off = sec + i * 40
    vsz, va, rsz, raw = struct.unpack_from("<IIII", new, off + 8)
    secs.append((va, vsz, raw, rsz))


def rva_off(rva):
    for va, vsz, raw, rsz in secs:
        if va <= rva < va + max(vsz, rsz):
            return raw + (rva - va)
    return None


o = rva_off(0x8ED160)
b = new[o : o + 40]
print("gate", b.hex())
checks = [
    (0, 0x48),
    (1, 0x83),
    (2, 0xEC),
    (3, 0x28),
    (17, 0x48),
    (18, 0x85),
    (19, 0xC0),
    (22, 0x83),
    (23, 0x78),
    (24, 0x74),
    (25, 0x00),
    (26, 0x74),
    (27, 0x07),
    (28, 0xB0),
    (29, 0x01),
]
ok_all = True
for i, v in checks:
    ok = b[i] == v
    ok_all &= ok
    print(f"  [{i}]=0x{b[i]:02X} expect 0x{v:02X} {'OK' if ok else 'FAIL'}")
print("gate_match", ok_all)

o2 = rva_off(0xA85292)
print("writer", new[o2 : o2 + 0x20].hex())
print("xor_ok", new[o2] == 0x32 and new[o2 + 1] == 0xC9)
print("store", new[o2 + 0x1A : o2 + 0x1E].hex())
