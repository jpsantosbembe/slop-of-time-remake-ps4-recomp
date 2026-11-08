#!/usr/bin/env python3
"""Migra a pasta de dados do port no PS4 de /data/ocarina para a pasta derivada do jogo (docs/frente-G.md). Roda no Mac.

    migrar_pasta_dados.py [--executar | --desfazer] [--ps4 192.168.1.99] [--dados /data/slop/<title id>]

Por FTP: MKD /data/slop, RNFR /data/ocarina RNTO <pasta nova> e, dentro dela, ocarina.elf -> game.elf. É um rename no
mesmo sistema de arquivos (/data), instantâneo: o RomFS (26 GB) não é copiado. Sem --executar só confere e mostra o
que faria (a leitura por FTP é livre). --desfazer volta tudo ao nome antigo. Recusa se o destino já existir.
Feche o app no console antes: com ele aberto, o processo ainda grava logs na pasta.
Pasta nova: --dados, SLOP_DATA_DIR, ou game_info.py --campo ps4_data_dir no IdeaPad.
"""
import argparse
import ftplib
import os
import posixpath
import subprocess
import sys

OLD = '/data/ocarina'
OLD_ELF, NEW_ELF = 'ocarina.elf', 'game.elf'


def data_dir(arg):
    d = arg or os.environ.get('SLOP_DATA_DIR') or subprocess.run(
        ['ssh', 'ideapad3i', 'python3 -I ~/ocarina-ps4/game_info.py --campo ps4_data_dir'],
        capture_output=True, text=True, check=True).stdout.strip()
    if not d.startswith('/data/slop/') or d.count('/') != 3:
        sys.exit(f'pasta de dados inesperada: {d!r}')
    return d


def exists(f, path):
    parent, name = posixpath.split(path)
    try:
        return name in [posixpath.basename(n) for n in f.nlst(parent)]
    except ftplib.error_perm:
        return False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group()
    g.add_argument('--executar', action='store_true')
    g.add_argument('--desfazer', action='store_true')
    ap.add_argument('--ps4', default='192.168.1.99')
    ap.add_argument('--dados')
    a = ap.parse_args()
    new = data_dir(a.dados)
    src, dst = (new, OLD) if a.desfazer else (OLD, new)
    elf_from, elf_to = (NEW_ELF, OLD_ELF) if a.desfazer else (OLD_ELF, NEW_ELF)

    f = ftplib.FTP()
    f.connect(a.ps4, 2121, timeout=30)
    f.login()
    f.set_pasv(True)
    if not exists(f, src):
        sys.exit(f'{src} não existe no console; nada a fazer')
    if exists(f, dst):
        sys.exit(f'{dst} já existe no console; não sobrescrevo (confira à mão)')
    has_elf = exists(f, f'{src}/{elf_from}')
    plan = [f'RNFR {src} RNTO {dst}']
    if has_elf:
        plan.append(f'RNFR {dst}/{elf_from} RNTO {dst}/{elf_to}')
    if not a.desfazer:
        plan.insert(0, 'MKD /data/slop (se faltar)')
    print('plano:\n  ' + '\n  '.join(plan))
    if not (a.executar or a.desfazer):
        print('(só conferência; rode com --executar para migrar, com o app FECHADO no console)')
        f.quit()
        return
    if not a.desfazer and not exists(f, '/data/slop'):
        f.mkd('/data/slop')
    f.rename(src, dst)
    if has_elf:
        f.rename(f'{dst}/{elf_from}', f'{dst}/{elf_to}')
    print('feito. conteúdo de', dst + ':', ', '.join(sorted(posixpath.basename(n) for n in f.nlst(dst))))
    f.quit()


if __name__ == '__main__':
    main()
