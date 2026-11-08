"""Leitura de arquivos individuais do romfs empacotado (romfs.bin + index.json), por offset, sem copiar o romfs.

Arquivos .zs são zstd; alguns usam dicionário, que vem de Pack/ZsDic.pack.zs (um SARC de dicionários), como em
ext/ocarina-recomp/tools/make_romfs_patches.py. Usa compression.zstd (Python 3.14).
"""
import json
import struct
import sys
from pathlib import Path
from compression import zstd


def sarc_members(data):
    """nome -> bytes de cada membro de um SARC."""
    assert data[:4] == b"SARC", "não é SARC"
    hdr = struct.unpack_from("<H", data, 4)[0]
    data_start = struct.unpack_from("<I", data, 0x0c)[0]
    sfat = hdr
    assert data[sfat:sfat + 4] == b"SFAT"
    count = struct.unpack_from("<H", data, sfat + 6)[0]
    nodes = sfat + 0x0c
    sfnt = nodes + count * 0x10
    assert data[sfnt:sfnt + 4] == b"SFNT"
    names = sfnt + 8
    out = {}
    for i in range(count):
        _, attr, lo, hi = struct.unpack_from("<IIII", data, nodes + i * 0x10)
        name = f"#{i}"
        if attr & 0x01000000:
            p = names + (attr & 0xffff) * 4
            name = data[p:data.index(b"\0", p)].decode("utf-8")
        out[name] = data[data_start + lo:data_start + hi]
    return out


class Romfs:
    def __init__(self, romfs_dir):
        d = Path(romfs_dir)
        self.bin = open(d / "romfs.bin", "rb")
        self.idx = {f["path"]: (f["offset"], f["size"]) for f in json.load(open(d / "index.json"))["files"]}
        self._dicts = None

    def span(self, path):
        return self.idx[path]

    def raw(self, path):
        off, size = self.idx[path]
        self.bin.seek(off)
        b = self.bin.read(size)
        assert len(b) == size
        return b

    def dicts(self):
        if self._dicts is None:
            self._dicts = {}
            for b in sarc_members(zstd.decompress(self.raw("Pack/ZsDic.pack.zs"))).values():
                d = zstd.ZstdDict(b)
                self._dicts[d.dict_id] = d
        return self._dicts

    def read(self, path):
        b = self.raw(path)
        if not path.endswith(".zs"):
            return b
        did = zstd.get_frame_info(b).dictionary_id
        return zstd.decompress(b, zstd_dict=self.dicts()[did]) if did else zstd.decompress(b)


if __name__ == "__main__":
    r = Romfs(sys.argv[1])
    Path(sys.argv[3]).write_bytes(r.read(sys.argv[2]))
