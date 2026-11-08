#!/usr/bin/env python3
"""Frente N: o que o commit preguiçoso do PS4 pagaria pela memória do jogo, medido no Linux.
Para cada página de 4 KiB das regiões commitadas do guest (anônimas, abaixo de 2^39): presente exclusiva (escrita ou
puxada), presente só como página zero (lida, nunca escrita) e, das exclusivas, se o conteúdo é todo zero (lê
/proc/PID/mem). Agrega por janela de 2 MiB (a do PS4) e de 64 KiB, por região (pilhas, heap, alias, módulos, outra).
Uso: memwin.py PID SAIDA.npz [heap_base_hex]"""
import os, re, sys, json
import numpy as np

pid = int(sys.argv[1]); out = sys.argv[2]
LIM = 1 << 39
regs = []
for ln in open(f'/proc/{pid}/maps'):
    f = ln.split()
    a, b = (int(x, 16) for x in f[0].split('-'))
    if b > LIM or a < 0x10000000: continue
    if len(f) > 5: continue                 # com nome (arquivo, [stack]...)
    if f[1].startswith('---'): continue     # só reservada
    regs.append((a, b, f[1]))
pm = os.open(f'/proc/{pid}/pagemap', os.O_RDONLY)
mem = os.open(f'/proc/{pid}/mem', os.O_RDONLY)
PG = 4096
addrs, excl, zp, nz = [], [], [], []
for a, b, perm in regs:
    n = (b - a) // PG
    raw = os.pread(pm, n * 8, a // PG * 8)
    e = np.frombuffer(raw, dtype='<u8')
    if len(e) < n: e = np.concatenate([e, np.zeros(n - len(e), dtype='<u8')])
    present = (e >> np.uint64(63)) & np.uint64(1)
    swapped = (e >> np.uint64(62)) & np.uint64(1)
    exclusive = (e >> np.uint64(56)) & np.uint64(1)
    ex = ((present & exclusive) | swapped).astype(bool)
    z = (present.astype(bool) & ~exclusive.astype(bool))
    nzv = np.zeros(n, dtype=bool)
    # conteúdo das exclusivas, em trechos contíguos
    idx = np.flatnonzero(ex)
    if len(idx):
        brk = np.flatnonzero(np.diff(idx) != 1)
        starts = np.concatenate([[idx[0]], idx[brk + 1]]); ends = np.concatenate([idx[brk], [idx[-1]]]) + 1
        for s, t in zip(starts, ends):
            for c0 in range(s, t, 4096):          # 16 MiB por leitura
                c1 = min(t, c0 + 4096)
                try:
                    d = os.pread(mem, (c1 - c0) * PG, a + c0 * PG)
                except OSError:
                    continue
                m = np.frombuffer(d, dtype='<u8')
                k = len(m) // 512
                nzv[c0:c0 + k] = m[:k * 512].reshape(k, 512).any(axis=1)
    addrs.append(np.arange(a, b, PG, dtype=np.uint64)); excl.append(ex); zp.append(z); nz.append(nzv)
A = np.concatenate(addrs) if addrs else np.zeros(0, np.uint64)
np.savez_compressed(out, addr=A, excl=np.concatenate(excl), zp=np.concatenate(zp), nz=np.concatenate(nz),
                    regs=np.array([(a, b) for a, b, _ in regs], dtype=np.uint64))
print(f'{out}: {len(regs)} regiões, {len(A) * PG >> 20} MiB commitados, exclusivas {int(np.concatenate(excl).sum()) * PG >> 20} MiB, '
      f'página zero {int(np.concatenate(zp).sum()) * PG >> 20} MiB, não zero {int(np.concatenate(nz).sum()) * PG >> 20} MiB')
