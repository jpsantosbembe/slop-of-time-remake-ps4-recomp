#!/usr/bin/env python3
"""Relatório dos retratos do memwin.py: por região do guest, o que o PS4 pagaria (janelas de 2 MiB tocadas, leitura ou
escrita), quanto disso tem conteúdo não zero, e a união acumulada no tempo (o PS4 nunca devolve uma janela).
Uso: memwin_report.py DIR [low_hex]"""
import sys, glob, re, os
import numpy as np

d = sys.argv[1]
files = sorted(glob.glob(f'{d}/win_t*.npz'), key=lambda f: int(re.search(r'_t(\d+)', f).group(1)))
low = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x100000000
GiB = 1 << 30
STACK = (low + 0x10000000, low + 0x20000000)
HEAP = low + 0x30000000
ALIAS = ((low + 16 * GiB + 4 * GiB - 1) // (4 * GiB)) * 4 * GiB
MODS = 0x7000000000

def cls(a):
    if a >= MODS: return 'módulos'
    if ALIAS <= a < ALIAS + 64 * GiB: return 'alias'
    if STACK[0] <= a < STACK[1]: return 'pilhas'
    if HEAP <= a < low + 16 * GiB: return 'heap'
    return 'outra'

M = 1 << 20
cum = {}
print(f'low {low:#x} heap {HEAP:#x} alias {ALIAS:#x}')
for f in files:
    t = int(re.search(r'_t(\d+)', f).group(1))
    z = np.load(f)
    A = z['addr']; ex = z['excl']; zp = z['zp']; nz = z['nz']
    W = (A >> np.uint64(21))
    touched = ex | zp
    tw = np.unique(W[touched]); nzw = np.unique(W[nz]); exw = np.unique(W[ex])
    nz64 = np.unique(A[nz] >> np.uint64(16)); t64 = np.unique(A[touched] >> np.uint64(16))
    rows = {}
    for name, arr, unit in (('jan2M_tocadas', tw, 2), ('jan2M_escritas', exw, 2), ('jan2M_nao_zero', nzw, 2),
                            ('jan64K_tocadas', t64, 1 / 16), ('jan64K_nao_zero', nz64, 1 / 16)):
        shift = 21 if unit == 2 else 16
        for w in arr:
            c = cls(int(w) << shift)
            rows.setdefault(c, {}).setdefault(name, 0)
            rows[c][name] += unit
    for name, mask in (('pag_escritas', ex), ('pag_zero_lidas', zp), ('pag_nao_zero', nz)):
        for c in set(cls(int(a)) for a in A[mask][::256]) | set(rows):
            pass
    # páginas por classe
    pc = {}
    for c in ('pilhas', 'heap', 'alias', 'módulos', 'outra'):
        lo, hi = {'pilhas': STACK, 'heap': (HEAP, low + 16 * GiB), 'alias': (ALIAS, ALIAS + 64 * GiB),
                  'módulos': (MODS, 1 << 39), 'outra': (0, 0)}[c]
        m = (A >= np.uint64(lo)) & (A < np.uint64(hi)) if hi else np.zeros(len(A), bool)
        pc[c] = (int((ex & m).sum()) * 4 / 1024, int((zp & m).sum()) * 4 / 1024, int((nz & m).sum()) * 4 / 1024)
    for w in tw: cum[int(w)] = cum.get(int(w), t)
    print(f'== t={t}s  janelas 2M tocadas {len(tw) * 2} MiB (só escritas {len(exw) * 2}, com não zero {len(nzw) * 2}); '
          f'64K: tocadas {len(t64) / 16:.0f} MiB, não zero {len(nz64) / 16:.0f} MiB; união acumulada 2M {len(cum) * 2} MiB')
    for c in sorted(rows, key=lambda c: -rows[c].get('jan2M_tocadas', 0)):
        r = rows[c]; p = pc.get(c, (0, 0, 0))
        print(f'   {c:8s} 2M tocadas {r.get("jan2M_tocadas", 0):7.0f}  escritas {r.get("jan2M_escritas", 0):7.0f}  '
              f'não zero {r.get("jan2M_nao_zero", 0):7.0f} | 64K tocadas {r.get("jan64K_tocadas", 0):7.0f} não zero '
              f'{r.get("jan64K_nao_zero", 0):7.0f} | págs escritas {p[0]:7.0f} lidas-zero {p[1]:7.0f} não zero {p[2]:7.0f}')
