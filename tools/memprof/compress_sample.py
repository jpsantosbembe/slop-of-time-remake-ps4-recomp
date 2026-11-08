#!/usr/bin/env python3
"""Compressibilidade da memória escrita do guest, por classe: amostra até N janelas de 2 MiB com página exclusiva de
cada classe, despeja em arquivos e mede zstd -1 e -3 (CLI). Uso: compress_sample.py PID DIR [N] [tex_lo tex_hi]"""
import os, sys, subprocess, random
import numpy as np
pid = int(sys.argv[1]); out = sys.argv[2]; N = int(sys.argv[3]) if len(sys.argv) > 3 else 96
tex = (int(sys.argv[4], 16), int(sys.argv[5], 16)) if len(sys.argv) > 5 else (0x1bf470000, 0x1bf470000 + (2432 << 20))
os.makedirs(out, exist_ok=True)
pm = os.open(f'/proc/{pid}/pagemap', os.O_RDONLY); mem = os.open(f'/proc/{pid}/mem', os.O_RDONLY)
wins = {}
for ln in open(f'/proc/{pid}/maps'):
    f = ln.split(); a, b = (int(x, 16) for x in f[0].split('-'))
    if b > 1 << 39 or a < 0x100000000 or len(f) > 5 or f[1].startswith('---'): continue
    n = (b - a) >> 12
    e = np.frombuffer(os.pread(pm, n * 8, (a >> 12) * 8), dtype='<u8')
    ex = ((e >> np.uint64(63)) & np.uint64(1)) & ((e >> np.uint64(56)) & np.uint64(1))
    for p in np.flatnonzero(ex):
        va = a + (int(p) << 12); w = va >> 21
        c = 'texturas' if tex[0] <= va < tex[1] else 'alias' if va >= 0x500000000 else 'heap'
        wins.setdefault(c, set()).add(w)
random.seed(1)
for c, ws in sorted(wins.items()):
    pick = random.sample(sorted(ws), min(N, len(ws)))
    path = f'{out}/{c}.bin'
    with open(path, 'wb') as fo:
        for w in pick:
            try: fo.write(os.pread(mem, 2 << 20, w << 21))
            except OSError: pass
    raw = os.path.getsize(path)
    r = {}
    for lv in ('-1', '-3'):
        z = subprocess.run(['zstd', lv, '-q', '-c', path], capture_output=True).stdout
        r[lv] = len(z)
    print(f'{c:9s} {len(ws) * 2:6d} MiB em janelas; amostra {raw >> 20} MiB: zstd -1 {r["-1"] / raw:.2f}, -3 {r["-3"] / raw:.2f}')
    os.remove(path)
