#!/usr/bin/env python3
"""Reescreve NSO de Switch 2 (segmentos ZBIC, a variante de zstd da Nintendo) como NSO sem compressão, que o lifter do
TotK/DKB Recomp lê (ele só conhece LZ4). Decodifica com o dm/tools/nso.py do nxnative (pacote zbic).
  nso_plain.py <exefs_entrada> <exefs_saida> [módulos...]   (padrão: main subsdk0 sdk)"""
import struct, sys, os
from pathlib import Path
sys.path.insert(0, str(Path.home() / 'Workspace/nxnative/dm/tools'))
import nso

src, dst = Path(sys.argv[1]), Path(sys.argv[2])
mods = sys.argv[3:] or ['main', 'subsdk0', 'sdk']
dst.mkdir(parents=True, exist_ok=True)
for name in mods:
    d = (src / name).read_bytes()
    m = nso.load(d, name)
    hdr = bytearray(d[:0x100])
    struct.pack_into('<I', hdr, 0xC, 0)            # sem compressão e sem hash: o lifter lê os segmentos como estão
    out, off = bytearray(), 0x100
    for i, (mem, size) in enumerate(m.segments):
        seg = bytes(m.image[mem:mem + size])
        struct.pack_into('<III', hdr, 0x10 + i * 0x10, off, mem, size)
        struct.pack_into('<I', hdr, 0x60 + i * 4, size)
        out += seg
        off += size
    (dst / name).write_bytes(bytes(hdr) + out)
    print(f'{name}: {len(d)} -> {0x100 + len(out)} bytes, build id {m.build_id[:16].hex()}')
