import struct, sys, json
d = open(sys.argv[1], 'rb').read()
assert d[:2] == b'YB'
ver, kt, st, root = struct.unpack_from('<HIII', d, 2)
def strtab(o):
    n = struct.unpack_from('<I', d, o)[0] >> 8
    offs = struct.unpack_from(f'<{n}I', d, o + 4)
    return [d[o + x:].split(b'\0')[0].decode() for x in offs]
keys = strtab(kt); strs = strtab(st) if st else []
def node(t, v):
    if t == 0xc1:
        n = struct.unpack_from('<I', d, v)[0] >> 8; r = {}
        for i in range(n):
            e = struct.unpack_from('<I', d, v + 4 + 8 * i)[0]; tt = d[v + 4 + 8 * i + 3]; vv = struct.unpack_from('<I', d, v + 8 + 8 * i)[0]
            r[keys[e & 0xffffff]] = node(tt, vv)
        return r
    if t == 0xc0:
        n = struct.unpack_from('<I', d, v)[0] >> 8; ts = d[v + 4:v + 4 + n]; base = v + 4 + ((n + 3) & ~3)
        return [node(ts[i], struct.unpack_from('<I', d, base + 4 * i)[0]) for i in range(n)]
    if t == 0xa0: return strs[v]
    if t == 0xd1: return struct.unpack('<i', struct.pack('<I', v))[0]
    if t == 0xd0: return bool(v)
    if t == 0xd2: return struct.unpack('<f', struct.pack('<I', v))[0]
    if t == 0xd3: return v
    if t == 0xd4: return struct.unpack_from('<q', d, v)[0]
    if t == 0xd5: return struct.unpack_from('<Q', d, v)[0]
    return f'?{t:#x}:{v:#x}'
rt = d[root]
print(json.dumps(node(rt, root), indent=1, ensure_ascii=False))
