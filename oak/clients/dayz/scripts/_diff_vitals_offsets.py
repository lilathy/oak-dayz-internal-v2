#!/usr/bin/env python3
"""Diff-scan DayZPlayer for m_HealthLevel by comparing before/after HP change."""
import ctypes, struct, time, sys, importlib.util
spec = importlib.util.spec_from_file_location('p', r'..\..\..\clients\dayz\scripts\_probe_vitals_netsync.py')
p = importlib.util.module_from_spec(spec); spec.loader.exec_module(p)

def snap(h, local, lo=0x600, hi=0x1800):
    out = {}
    for off in range(lo, hi, 4):
        out[off] = p.i32(h, local + off)
    return out

def main():
    pid = p.find_pid('DayZ_x64.exe')
    h = p.open_proc(pid)
    base, msize = p.module_base(h, 'DayZ_x64.exe')
    world, local, near, nsz = p.resolve_local(h, base)
    print(f'local=0x{local:X} nsz={nsz} — snapshot A. Take damage from infected (or wait).')
    a = snap(h, local)
    # also float snap for TransferValues-like
    fa = {off: p.f32(h, local+off) for off in range(0x600, 0x1800, 4)}
    time.sleep(float(sys.argv[1]) if len(sys.argv)>1 else 25.0)
    # refresh local in case entity moved
    world, local2, near, nsz = p.resolve_local(h, base)
    if local2 != local:
        print(f'local changed 0x{local:X} -> 0x{local2:X}')
        local = local2
    b = snap(h, local)
    fb = {off: p.f32(h, local+off) for off in range(0x600, 0x1800, 4)}
    print('=== int diffs (interesting) ===')
    for off in sorted(a):
        if a[off] == b[off]:
            continue
        av, bv = a[off], b[off]
        # health level 0..4, shock 0..63, bleeding bits, etc
        interesting = (
            (0 <= av <= 63 and 0 <= bv <= 63) or
            (abs(av) < 100000 and abs(bv) < 100000)
        )
        if interesting:
            tag = ''
            if 0 <= av <= 4 and 0 <= bv <= 4: tag = ' LEVEL?'
            if 0 <= av <= 63 and 0 <= bv <= 63 and (av > 4 or bv > 4): tag += ' shock?'
            print(f'  +0x{off:X}: {av} -> {bv}{tag}')
    print('=== float diffs (0..1.05 or 0..100) ===')
    for off in sorted(fa):
        if fa[off] != fa[off] or fb[off] != fb[off]:
            continue
        if abs(fa[off] - fb[off]) < 1e-6:
            continue
        av, bv = fa[off], fb[off]
        if (0 <= av <= 1.05 and 0 <= bv <= 1.05) or (0 <= av <= 100 and 0 <= bv <= 100):
            print(f'  +0x{off:X}: {av} -> {bv}')
    p.k.CloseHandle(h)

if __name__ == '__main__':
    main()
