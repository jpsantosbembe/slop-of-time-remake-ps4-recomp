#!/usr/bin/env python3
"""Gera um overlay de RomFS a partir de uma receita da camada do jogo (jogos/<title id>/romfs-mods.json).

    romfs_mods.py <receita> <pasta de saída> [--romfs-index ...] [--romfs ...] [--jogo ...]
    romfs_mods.py --listar

Os originais saem da cópia do jogo do usuário (romfs.bin + romfs-index.bin). Cada chave de nível raiz de um BYML little
endian solto é trocada no lugar, sem mudar o tamanho (bool, int, uint, float). A pasta de saída recebe os arquivos
nos caminhos do RomFS e o manifest.tsv (caminho, tamanho, SHA-256 do overlay, SHA-256 do original), que o runtime lê por
TOTK_ROMFS_MODS=<pasta>. Nada do jogo fica no repositório: só a receita. Roda no IdeaPad.
"""
import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
from pathlib import Path

HOME = Path.home() / 'ocarina-ps4'


def romfs_files(index):
    d = index.read_bytes()
    _, n, so = struct.unpack_from('<8sQQ', d, 0)
    out = {}
    for i in range(n):
        off, size, no, nl = struct.unpack_from('<QQII', d, 24 + 24 * i)
        out[d[so + no:so + no + nl - 1].decode()] = (off, size)
    return out


def byml_set(b, sets):
    """Troca valores escalares de nível raiz num BYML LE (YB) no lugar. Devolve as linhas de log."""
    if b[:2] != b'YB':
        sys.exit('romfs_mods: só BYML little endian solto (YB)')
    keys, _, root = struct.unpack_from('<III', b, 4)

    def key(k):
        a = struct.unpack_from('<I', b, keys + 4 + 4 * k)[0]
        e = b.index(0, keys + a)
        return b[keys + a:e].decode()

    h = struct.unpack_from('<I', b, root)[0]
    if h & 0xff != 0xc1:
        sys.exit('romfs_mods: a raiz do BYML não é um dicionário')
    ent = {}
    for i in range(h >> 8):
        e = struct.unpack_from('<I', b, root + 4 + 8 * i)[0]
        ent[key(e & 0xffffff)] = (e >> 24, root + 8 + 8 * i)
    log = []
    for k, v in sets.items():
        if k not in ent:
            sys.exit(f'romfs_mods: a chave {k} não está na raiz')
        t, at = ent[k]
        old = bytes(b[at:at + 4])
        v = str(v)
        if t == 0xd0:
            struct.pack_into('<I', b, at, 1 if v.lower() in ('1', 'true') else 0)
        elif t == 0xd1:
            struct.pack_into('<i', b, at, int(v))
        elif t == 0xd3:
            struct.pack_into('<I', b, at, int(v))
        elif t == 0xd2:
            struct.pack_into('<f', b, at, float(v))
        else:
            sys.exit(f'romfs_mods: tipo {t:#x} de {k} não suportado')
        log.append(f'  {k}: {old.hex()} -> {bytes(b[at:at + 4]).hex()}')
    return log


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('receita', nargs='?')
    ap.add_argument('saida', nargs='?', type=Path)
    ap.add_argument('--listar', action='store_true')
    ap.add_argument('--romfs-index', type=Path, default=HOME / 'ext/ocarina-recomp-orbis/build/oot111-orbis/romfs-index.bin')
    ap.add_argument('--romfs', type=Path, default=HOME / 'ext/ocarina-recomp/build/romfs/romfs.bin')
    ap.add_argument('--jogo', type=Path, default=None)
    a = ap.parse_args()
    jogo = a.jogo
    if jogo is None:
        tid = subprocess.run([sys.executable, '-I', str(HOME / 'game_info.py'), '--campo', 'title_id'],
                             capture_output=True, text=True, check=True).stdout.strip()
        jogo = Path(__file__).resolve().parent.parent / 'jogos' / tid
    receitas = {k: v for k, v in json.loads((jogo / 'romfs-mods.json').read_text()).items() if not k.startswith('_')}
    if a.listar or not a.receita:
        for k, v in receitas.items():
            print(f'{k}: {v["descricao"]}')
        return
    if a.receita not in receitas or a.saida is None:
        sys.exit(f'romfs_mods: receita desconhecida ou sem saída; há {", ".join(receitas)}')
    files = romfs_files(a.romfs_index)
    rows = []
    with open(a.romfs, 'rb') as rom:
        for path, sets in receitas[a.receita]['arquivos'].items():
            if path not in files:
                sys.exit(f'romfs_mods: {path} não está no RomFS')
            off, size = files[path]
            rom.seek(off)
            orig = rom.read(size)
            b = bytearray(orig)
            print(path)
            print('\n'.join(byml_set(b, sets)))
            dst = a.saida / path
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(b)
            rows.append(f'{path}\t{len(b)}\t{hashlib.sha256(b).hexdigest()}\t{hashlib.sha256(orig).hexdigest()}\n')
    (a.saida / 'manifest.tsv').write_text(''.join(sorted(rows)))
    print(f'{a.saida}/manifest.tsv: {len(rows)} arquivo(s)')


if __name__ == '__main__':
    main()
