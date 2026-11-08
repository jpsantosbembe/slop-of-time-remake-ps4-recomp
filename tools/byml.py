#!/usr/bin/env python3
"""Leitor de BYML (v2 a v7, little ou big endian) para Python puro: dict, list, str, int, float, bool, bytes, None.

    byml.py <arquivo.byml> [--json] [--depth N]

Sem --json, imprime a estrutura resumida (chaves e tipos até --depth, padrão 3). Usado para ler a lista de gamedata do
jogo (docs/saves.md); não traz nada do jogo.
"""
import json
import struct
import sys


class Byml:
    def __init__(self, data):
        self.d = data
        if data[:2] == b'BY':
            self.e = '>'
        elif data[:2] == b'YB':
            self.e = '<'
        else:
            raise ValueError(f'não é BYML: {data[:4]!r}')
        self.version, self.keys_off, self.strs_off, self.root_off = struct.unpack_from(self.e + 'HIII', data, 2)
        self.keys = self._table(self.keys_off) if self.keys_off else []
        self.strs = self._table(self.strs_off) if self.strs_off else []

    def u32(self, o):
        return struct.unpack_from(self.e + 'I', self.d, o)[0]

    def u24(self, o):
        b = self.d[o:o + 3]
        return int.from_bytes(b, 'big' if self.e == '>' else 'little')

    def _table(self, o):
        assert self.d[o] == 0xC2, hex(self.d[o])
        n = self.u24(o + 1)
        out = []
        for i in range(n):
            s = o + self.u32(o + 4 + 4 * i)
            out.append(self.d[s:self.d.index(b'\0', s)].decode('utf-8', 'replace'))
        return out

    def root(self):
        return self.node(self.d[self.root_off], self.root_off) if self.root_off else None

    def value(self, t, raw_off):
        """Valor de um item de array ou dict: raw_off aponta para os 4 bytes do item."""
        v = self.u32(raw_off)
        if t == 0xA0:
            return self.strs[v]
        if t == 0xD0:
            return bool(v)
        if t == 0xD1:
            return struct.unpack_from(self.e + 'i', self.d, raw_off)[0]
        if t == 0xD2:
            return struct.unpack_from(self.e + 'f', self.d, raw_off)[0]
        if t == 0xD3:
            return v
        if t in (0xD4, 0xD5, 0xD6):
            fmt = {0xD4: 'q', 0xD5: 'Q', 0xD6: 'd'}[t]
            return struct.unpack_from(self.e + fmt, self.d, v)[0]
        if t == 0xA1:   # binário: tamanho + dados
            n = self.u32(v)
            return bytes(self.d[v + 4:v + 4 + n])
        if t == 0xA2:   # binário com alinhamento: tamanho, alinhamento, dados
            n = self.u32(v)
            return bytes(self.d[v + 8:v + 8 + n])
        if t == 0xFF:
            return None
        return self.node(t, v)

    def node(self, t, o):
        if t == 0xC0:   # array
            n = self.u24(o + 1)
            types = self.d[o + 4:o + 4 + n]
            base = o + 4 + ((n + 3) & ~3)
            return [self.value(types[i], base + 4 * i) for i in range(n)]
        if t == 0xC1:   # dict
            n = self.u24(o + 1)
            out = {}
            for i in range(n):
                p = o + 4 + 8 * i
                k = self.u24(p) if self.e == '<' else int.from_bytes(self.d[p:p + 3], 'big')
                vt = self.d[p + 3]
                out[self.keys[k]] = self.value(vt, p + 4)
            return out
        if t in (0x20, 0x21):   # dict por hash (v7): 32 ou 64 bits de hash por entrada
            n = self.u24(o + 1)
            hs = 4 if t == 0x20 else 8
            ent = hs + 4
            types = self.d[o + 4 + ent * n:o + 4 + ent * n + n]
            out = {}
            for i in range(n):
                p = o + 4 + ent * i
                h = int.from_bytes(self.d[p:p + hs], 'big' if self.e == '>' else 'little')
                out[f'#{h:0{hs * 2}x}'] = self.value(types[i], p + hs)
            return out
        raise ValueError(f'tipo BYML {t:#x} em {o:#x}')


def summary(v, depth, ind=0, out=None):
    out = out if out is not None else []
    pad = '  ' * ind
    if isinstance(v, dict):
        for k, x in list(v.items())[:40]:
            out.append(f'{pad}{k}: {kind(x)}')
            if depth > 1:
                summary(x, depth - 1, ind + 1, out)
        if len(v) > 40:
            out.append(f'{pad}... {len(v)} chaves')
    elif isinstance(v, list) and v:
        out.append(f'{pad}[{len(v)}] de {kind(v[0])}')
        if depth > 1:
            summary(v[0], depth - 1, ind + 1, out)
    return out


def kind(x):
    if isinstance(x, dict):
        return f'dict({len(x)})'
    if isinstance(x, list):
        return f'list({len(x)})'
    if isinstance(x, (bytes, bytearray)):
        return f'bytes({len(x)})'
    return repr(x)[:60]


def main():
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__)
    depth = int(a[a.index('--depth') + 1]) if '--depth' in a else 3
    v = Byml(open(a[0], 'rb').read()).root()
    if '--json' in a:
        json.dump(v, sys.stdout, ensure_ascii=False, default=lambda b: b.hex())
    else:
        print('\n'.join(summary(v, depth)))


if __name__ == '__main__':
    main()
