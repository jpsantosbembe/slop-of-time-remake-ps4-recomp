#!/usr/bin/env python3
"""Cruza o trace de pools (NPOOL init/fini do gdb) com um retrato do memwin: quanto da memória escrita/não zero do
guest está em pools (por flags) e quanto fora deles, por região. Uso: pools_report.py trace.log win.npz [win_antes.npz]"""
import sys, re, collections
import numpy as np

live = {}
for ln in open(sys.argv[1], errors='replace'):
    m = re.match(r'NPOOL (init|fini) (0x[0-9a-f]+) (0x[0-9a-f]+) (0x[0-9a-f]+)', ln)
    if not m: continue
    op, a, n, f = m.group(1), int(m.group(2), 16), int(m.group(3), 16), int(m.group(4), 16)
    if op == 'init': live[a] = (n, f)
    else: live.pop(a, None)
NAMES = {0x1: 'CPU_NO_ACCESS', 0x2: 'CPU_UNCACHED', 0x4: 'CPU_CACHED', 0x8: 'GPU_NO_ACCESS', 0x10: 'GPU_UNCACHED',
         0x20: 'GPU_CACHED', 0x40: 'SHADER_CODE', 0x80: 'COMPRESSIBLE', 0x100: 'PHYSICAL', 0x200: 'VIRTUAL'}
def fname(f): return '|'.join(v for k, v in NAMES.items() if f & k) or hex(f)

def load(p):
    z = np.load(p)
    return z['addr'], z['excl'], z['zp'], z['nz']
low = 0x100000000; ALIAS = 0x500000000
def reg(a): return 'alias' if ALIAS <= a < ALIAS + (64 << 30) else 'heap' if 0x130000000 <= a < low + (16 << 30) else 'outra'

A, ex, zp, nz = load(sys.argv[2])
inpool = np.zeros(len(A), dtype=np.int32) - 1
keys = sorted(live)
starts = np.array(keys, dtype=np.uint64); ends = np.array([k + live[k][0] for k in keys], dtype=np.uint64)
i = np.searchsorted(starts, A, side='right') - 1
ok = (i >= 0) & (A < ends[np.clip(i, 0, None)])
flags = np.array([live[k][1] for k in keys])
pf = np.where(ok, flags[np.clip(i, 0, None)], -1)
M = 256  # páginas por MiB
agg = collections.defaultdict(lambda: [0, 0, 0, 0, 0])
for f in sorted(set(pf.tolist())):
    m = pf == f
    for r in ('heap', 'alias', 'outra'):
        lo, hi = {'heap': (0x130000000, low + (16 << 30)), 'alias': (ALIAS, ALIAS + (64 << 30)), 'outra': (0, 0x130000000)}[r]
        mm = m & (A >= np.uint64(lo)) & (A < np.uint64(hi))
        if not mm.any(): continue
        k = ('fora de pool' if f < 0 else fname(f), r)
        agg[k][0] += int(mm.sum()); agg[k][1] += int((ex & mm).sum()); agg[k][2] += int((zp & mm).sum()); agg[k][3] += int((nz & mm).sum())
tot = collections.Counter()
for k, f in live.items(): tot[(fname(f[1]), reg(k))] += f[0]
print(f'pools vivos: {len(live)}, {sum(v[0] for v in live.values()) >> 20} MiB')
print(f'{"classe":50s} {"região":6s} {"pool MiB":>9s} {"commit":>8s} {"escritas":>9s} {"lidas-0":>8s} {"não zero":>9s}')
for k, v in sorted(agg.items(), key=lambda kv: -kv[1][1]):
    print(f'{k[0]:50s} {k[1]:6s} {tot.get(k, 0) / 1048576:9.0f} {v[0] / M:8.0f} {v[1] / M:9.0f} {v[2] / M:8.0f} {v[3] / M:9.0f}')
