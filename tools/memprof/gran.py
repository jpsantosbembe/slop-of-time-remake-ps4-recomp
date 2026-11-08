import sys, numpy as np
for p in sys.argv[1:]:
    z = np.load(p); A = z['addr']; ex = z['excl']; nz = z['nz']
    out = [p.split('/')[-1]]
    for sh in (14, 16, 18, 20, 21):
        w = len(np.unique(A[ex] >> np.uint64(sh))) << sh >> 20
        n = len(np.unique(A[nz] >> np.uint64(sh))) << sh >> 20
        out.append(f'{1 << sh >> 10}K: escritas {w} não zero {n}')
    print(' | '.join(out))
