#!/usr/bin/env python3
"""Conjunto quente do guest no Linux: soma Rss/Referenced do smaps das regiões abaixo de 2^39 (o guest), por região.
'limpar' zera os bits de acesso (clear_refs 1); depois, Referenced = o que a CPU tocou desde então.
Uso: hotset.py PID limpar | hotset.py PID ler"""
import sys, re, collections
pid = int(sys.argv[1])
if sys.argv[2] == 'limpar':
    open(f'/proc/{pid}/clear_refs', 'w').write('1'); sys.exit()
def cls(a):
    if a >= 0x7000000000: return 'módulos'
    if 0x500000000 <= a < 0x500000000 + (64 << 30): return 'alias'
    if 0x130000000 <= a < 0x500000000: return 'heap'
    return 'outra'
agg = collections.defaultdict(lambda: [0, 0, 0]); cur = None
for ln in open(f'/proc/{pid}/smaps'):
    m = re.match(r'^([0-9a-f]+)-([0-9a-f]+) ', ln)
    if m:
        a = int(m.group(1), 16); cur = cls(a) if a < 1 << 39 and a >= 0x100000000 else None; continue
    if cur is None: continue
    if ln.startswith('Rss:'): agg[cur][0] += int(ln.split()[1])
    elif ln.startswith('Referenced:'): agg[cur][1] += int(ln.split()[1])
    elif ln.startswith('Anonymous:'): agg[cur][2] += int(ln.split()[1])
t = [0, 0, 0]
for k, v in sorted(agg.items()):
    print(f'  {k:8s} rss {v[0] >> 10:6d} MiB  referenciada {v[1] >> 10:6d} MiB  anônima {v[2] >> 10:6d} MiB')
    t = [x + y for x, y in zip(t, v)]
print(f'  total    rss {t[0] >> 10:6d} MiB  referenciada {t[1] >> 10:6d} MiB')
