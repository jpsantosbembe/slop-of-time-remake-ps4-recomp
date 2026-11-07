#!/usr/bin/env python3
"""Folga das janelas de 2 MiB do commit preguiçoso do PS4, medida no Linux: lê /proc/PID/pagemap (bit present) das
regiões anônimas do espaço do jogo e compara as páginas de 4 KiB tocadas com as janelas de 2 MiB (e 64/256 KiB) que
o PS4 mapearia no primeiro toque. Uso: pagemap_windows.py PID [lo_hex hi_hex]"""
import re, struct, sys, collections
pid = int(sys.argv[1])
lo_lim = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x10000000
hi_lim = int(sys.argv[3], 16) if len(sys.argv) > 3 else 1 << 39
regs = []
for ln in open(f'/proc/{pid}/maps'):
    f = ln.split()
    a, b = (int(x, 16) for x in f[0].split('-'))
    if len(f) > 5 and f[5] not in ('',):  # só anônimas sem nome
        continue
    if a < lo_lim or b > hi_lim:
        continue
    regs.append((a, b, f[1]))
# vizinhas viram um trecho; só os trechos de >= MIN_SPAN (a reserva do jogo, 16 GiB no Linux) contam
MIN_SPAN = int(sys.argv[4], 16) if len(sys.argv) > 4 else 1 << 30
spans = []
for a, b, p in regs:
    if spans and spans[-1][1] == a: spans[-1][1] = b; spans[-1][2].append((a, b, p))
    else: spans.append([a, b, [(a, b, p)]])
regs = [r for sp in spans if sp[1] - sp[0] >= MIN_SPAN for r in sp[2]]
print('trechos do jogo:', ', '.join(f'{sp[0]:#x}+{(sp[1]-sp[0])>>20} MiB' for sp in spans if sp[1] - sp[0] >= MIN_SPAN))
pm = open(f'/proc/{pid}/pagemap', 'rb')
present = 0
win = {s: set() for s in (16 << 10, 64 << 10, 256 << 10, 2 << 20)}
by_reg = collections.Counter()
CH = 1 << 15
for a, b, perm in regs:
    for base in range(a, b, CH * 4096):
        n = min(CH, (b - base) // 4096)
        pm.seek(base // 4096 * 8)
        data = pm.read(n * 8)
        for i, (e,) in enumerate(struct.iter_unpack('<Q', data)):
            if (e >> 63 & 1 and e >> 56 & 1) or e >> 62 & 1:  # presente e exclusiva (a página zero compartilhada das leituras não conta) ou swap
                va = base + i * 4096
                present += 1
                by_reg[a] += 1
                for s in win:
                    win[s].add(va // s)
M = 1 << 20
print(f'regiões anônimas no espaço do jogo: {len(regs)}, {sum(b - a for a, b, _ in regs) / M:.0f} MiB de endereço')
print(f'páginas 4 KiB tocadas: {present * 4096 / M:.1f} MiB')
for s, v in sorted(win.items()):
    print(f'janelas de {s >> 10:5d} KiB tocadas: {len(v) * s / M:8.1f} MiB (folga {(len(v) * s - present * 4096) / M:.1f} MiB)')
for a, n in by_reg.most_common(8):
    print(f'  região {a:#x}: {n * 4096 / M:.1f} MiB tocados')
