import sys, re, numpy as np
live = {}
for ln in open(sys.argv[1], errors='replace'):
    m = re.match(r'NPOOL (init|fini) (0x[0-9a-f]+) (0x[0-9a-f]+) (0x[0-9a-f]+)', ln)
    if not m: continue
    a, n, f = (int(m.group(i), 16) for i in (2, 3, 4))
    if m.group(1) == 'init': live[a] = (n, f)
    else: live.pop(a, None)
snaps = []
for p in sys.argv[2:]:
    z = np.load(p); A = z['addr']; o = np.argsort(A)
    snaps.append((A[o], z['excl'][o], z['nz'][o]))
def cnt(s, a, n):
    A, ex, nz = s; i, j = np.searchsorted(A, [a, a + n])
    return int(ex[i:j].sum()) / 256, int(nz[i:j].sum()) / 256
print('pool              MiB  flags   ' + '  '.join(f'esc/nz@{i}' for i in range(len(snaps))))
small = {}
for a, (n, f) in sorted(live.items()):
    if n >= 32 << 20:
        print(f'{a:#014x} {n / 1048576:8.0f}  {f:#6x}  ' + '  '.join('%7.0f/%-7.0f' % cnt(s, a, n) for s in snaps))
    else:
        k = (f, 'alias' if a >= 0x500000000 else 'heap'); small.setdefault(k, [0, 0] + [0.0] * len(snaps)); small[k][0] += 1; small[k][1] += n
        for i, s in enumerate(snaps): small[k][2 + i] += cnt(s, a, n)[0]
for k, v in sorted(small.items(), key=lambda kv: -kv[1][1]):
    print(f'pequenos {k[0]:#6x} {k[1]:5s}: {v[0]} pools, {v[1] / 1048576:.0f} MiB; escritos ' + ' '.join(f'{x:.0f}' for x in v[2:]))
