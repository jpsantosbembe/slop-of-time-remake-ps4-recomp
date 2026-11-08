#!/usr/bin/env python3
"""Muda a cena e o ponto de partida de um slot de save, para os testes começarem numa cena leve (docs/saves.md).

    editar_save.py <slot de entrada> <slot de saída> --cena <cena> [--start <nome do StartPos>] [--listar]

A posição e o ângulo vêm do ator StartPos da própria cena (Banc/<grupo>/<cena>_{Dynamic,Static}.bcett.byml.zs no
RomFS), e não de números fixos. --listar mostra os StartPos da cena e sai. Os hashes dos campos do save são do jogo e
ficam em jogos/<title id>/save.json. Roda no IdeaPad, onde ficam o RomFS e o zstd:
  --romfs-index (padrão: build/oot111-orbis/romfs-index.bin da worktree orbis), --romfs (padrão: build/romfs/romfs.bin
  do ocarina-recomp), --jogo (padrão: jogos/<title id do game_info.py> ao lado de tools/).
O save sai numa pasta nova; a de entrada não muda.
"""
import argparse
import json
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import byml  # noqa: E402

HOME = Path.home() / 'ocarina-ps4'


def romfs_files(index):
    d = index.read_bytes()
    _, n, so = struct.unpack_from('<8sQQ', d, 0)
    out = {}
    for i in range(n):
        off, size, no, nl = struct.unpack_from('<QQII', d, 24 + 24 * i)
        out[d[so + no:so + no + nl - 1].decode()] = (off, size)
    return out


def read_romfs(rom, files, name):
    off, size = files[name]
    with open(rom, 'rb') as f:
        f.seek(off)
        return f.read(size)


def zstd(data, dic=None):
    with tempfile.TemporaryDirectory() as t:
        src, dst = Path(t) / 'in.zs', Path(t) / 'out'
        src.write_bytes(data)
        cmd = ['zstd', '-dqf', str(src), '-o', str(dst)]
        if dic:
            dp = Path(t) / 'dic'
            dp.write_bytes(dic)
            cmd += ['-D', str(dp)]
        subprocess.run(cmd, check=True)
        return dst.read_bytes()


def sarc(data):
    assert data[:4] == b'SARC'
    e = '<' if data[6:8] == b'\xff\xfe' else '>'
    _, _, _, dstart = struct.unpack_from(e + 'HHII', data, 4)
    _, _, count, _ = struct.unpack_from(e + '4sHHI', data, 0x14)
    names = 0x14 + 12 + 16 * count + 8
    out = {}
    for i in range(count):
        _, attr, s, en = struct.unpack_from(e + 'IIII', data, 0x14 + 12 + 16 * i)
        name = data[names + (attr & 0xffff) * 4:].split(b'\0')[0].decode()
        out[name] = data[dstart + s:dstart + en]
    return out


def starts(rom, files, cena):
    """Os StartPos da cena: [(nome, (x, y, z), ângulo)]. Ângulo: o yaw que o save guarda (rotação Y; quando X e Z são ±π,
    a mesma orientação é π - Y, como no StartPos da demonstração: Rotate (-π, 1.2467, -π) e ângulo salvo 1.8949)."""
    dic = sarc(zstd(read_romfs(rom, files, 'Pack/ZsDic.pack.zs')))['bcett.byml.zsdic']
    bancs = [n for n in files if n.startswith('Banc/') and n.split('/')[-1] in
             (f'{cena}_Dynamic.bcett.byml.zs', f'{cena}_Static.bcett.byml.zs')]
    if not bancs:
        sys.exit(f'editar_save: a cena {cena} não tem Banc no RomFS')
    out = []
    for n in sorted(bancs):
        root = byml.Byml(zstd(read_romfs(rom, files, n), dic)).root()
        for a in root.get('Actors', []):
            if a.get('Gyaml') != 'StartPos':
                continue
            x, y, z = a.get('Rotate', [0.0, 0.0, 0.0])
            yaw = math.pi - y if abs(abs(x) - math.pi) < 1e-3 and abs(abs(z) - math.pi) < 1e-3 else y
            yaw = math.atan2(math.sin(yaw), math.cos(yaw))
            out.append((a.get('Name') or '', tuple(a['Translate']), yaw, n.split('/')[1]))
    return out


def fields(b):
    """{hash: (posição da entrada, valor u32)} da tabela de campos (do cabeçalho até o início dos dados)."""
    data = struct.unpack_from('<I', b, 8)[0]
    return {struct.unpack_from('<I', b, p)[0]: (p, struct.unpack_from('<I', b, p + 4)[0]) for p in range(0x20, data, 8)}


def put_str(b, f, h, text, size):
    p, v = f[h]
    raw = text.encode()
    if len(raw) >= size:
        sys.exit(f'editar_save: "{text}" não cabe em {size} bytes')
    b[v:v + size] = raw + b'\0' * (size - len(raw))


def put_vec(b, f, h, xyz):
    p, v = f[h]
    struct.pack_into('<fff', b, v, *xyz)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('entrada', type=Path)
    ap.add_argument('saida', type=Path)
    ap.add_argument('--cena', required=True)
    ap.add_argument('--start', default=None, help='nome do StartPos (padrão: o primeiro com nome)')
    ap.add_argument('--listar', action='store_true')
    ap.add_argument('--romfs-index', type=Path, default=HOME / 'ext/ocarina-recomp-orbis/build/oot111-orbis/romfs-index.bin')
    ap.add_argument('--romfs', type=Path, default=HOME / 'ext/ocarina-recomp/build/romfs/romfs.bin')
    ap.add_argument('--jogo', type=Path, default=None)
    a = ap.parse_args()

    files = romfs_files(a.romfs_index)
    st = starts(a.romfs, files, a.cena)
    if a.listar or not st:
        for nome, pos, yaw, grupo in st:
            print(f'{nome or "(sem nome)":40s} {pos[0]:9.3f} {pos[1]:9.3f} {pos[2]:9.3f}  ângulo {yaw:7.4f}  ({grupo})')
        return
    if a.start:
        st = [s for s in st if s[0] == a.start]
        if not st:
            sys.exit(f'editar_save: a cena {a.cena} não tem StartPos "{a.start}"; veja --listar')
    else:
        st = [s for s in st if s[0]] or st
    nome, pos, yaw, grupo = st[0]

    jogo = a.jogo
    if jogo is None:
        tid = subprocess.run([sys.executable, '-I', str(HOME / 'game_info.py'), '--campo', 'title_id'],
                             capture_output=True, text=True, check=True).stdout.strip()
        jogo = Path(__file__).resolve().parent.parent / 'jogos' / tid
    cfg = json.loads((jogo / 'save.json').read_text())

    a.saida.mkdir(parents=True, exist_ok=True)
    pr = bytearray((a.entrada / 'progress.sav').read_bytes())
    f = fields(pr)
    for h in cfg['progress']['cena_string32']:
        put_str(pr, f, h, a.cena, 32)
    for h in cfg['progress']['cena_string64']:
        put_str(pr, f, h, a.cena, 64)
    for h in cfg['progress']['posicao']:
        put_vec(pr, f, h, pos)
    for h in cfg['progress']['angulo']:
        p, _ = f[h]
        struct.pack_into('<f', pr, p + 4, yaw)
    (a.saida / 'progress.sav').write_bytes(pr)

    ca = bytearray((a.entrada / 'caption.sav').read_bytes())
    f = fields(ca)
    for h in cfg['caption']['posicao']:
        put_vec(ca, f, h, pos)
    for h in cfg['caption']['rotulo_local_string64']:
        put_str(ca, f, h, grupo, 64)
    for h in cfg['caption']['rotulo_cena_string64']:
        put_str(ca, f, h, f'{grupo}/{a.cena}', 64)
    (a.saida / 'caption.sav').write_bytes(ca)
    print(f'{a.saida}: cena {a.cena} ({grupo}), StartPos "{nome}" em {pos}, ângulo {yaw:.4f}')


if __name__ == '__main__':
    main()
