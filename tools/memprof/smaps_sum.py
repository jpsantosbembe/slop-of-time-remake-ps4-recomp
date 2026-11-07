import sys, re, collections
cur=None; agg=collections.Counter(); anon=collections.Counter(); big=[]
for ln in open(sys.argv[1]):
    m=re.match(r'^([0-9a-f]+)-([0-9a-f]+) (\S+) \S+ \S+ \S+\s*(.*)$', ln)
    if m:
        lo,hi=int(m.group(1),16),int(m.group(2),16); name=m.group(4) or '[anon]'
        name=re.sub(r'/home/\S+/','~/',name)
        cur=[name,lo,hi,m.group(3),0,0]; big.append(cur); continue
    if ln.startswith('Rss:'): cur[4]=int(ln.split()[1])
    if ln.startswith('Anonymous:'): cur[5]=int(ln.split()[1])
for name,lo,hi,perm,rss,an in big:
    key=name
    if name=='[anon]':
        sz=(hi-lo)>>20
        key='[anon] '+('guest>=0x7000000000' if lo>=0x7000000000 and lo<0x8000000000 else ('>=1GiB' if sz>=1024 else ('128M..1G' if sz>=128 else ('8M..128M' if sz>=8 else '<8M'))))
    agg[key]+=rss; anon[key]+=an
print('MiB rss / anon por mapeamento')
for k,v in agg.most_common(25): print(f'{v/1024:9.1f} {anon[k]/1024:9.1f}  {k}')
