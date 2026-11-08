#!/usr/bin/env python3
"""Identidade do jogo lida dos arquivos dele (nada fixo no código): título, title ID e versão, do bloco de metadados
do pacote base do .nup, mais os IDs de PKG do PS4 derivados do title ID do Switch.

    game_info.py [<jogo>.nup] [--campo title|title_id|version|ps4_tid_dev|ps4_tid_teste|ps4_tid|ps4_data_dir]

Sem caminho, usa o único .nup em ~/ocarina-ps4/game (ou GAME_NUP). Sem --campo, imprime tudo em JSON.

IDs do PS4: 4 letras + 5 dígitos. As letras são "SLOP" (o projeto); os dígitos vêm do SHA-256 do title ID do Switch,
então cada jogo tem os seus, sempre os mesmos. Três variantes não colidem: teste = n, dev = n + 1, final = n + 2
(n múltiplo de 3, de 0 a 99999).

Pasta de dados no console (ps4_data_dir): /data/slop/<title id do Switch>. O carregador de dev a recebe na
compilação (loader/build.sh) e os scripts do Mac a usam; o runtime chega à mesma pasta pelo main.npdm (build_game.py).

O leitor do .nup é o tools/extract_nup.py do ocarina-recomp (OCARINA_MAIN, padrão ~/ocarina-ps4/ext/ocarina-recomp).
"""
import argparse
import hashlib
import json
import os
import sys
from pathlib import Path


def find_nup(arg):
    if arg:
        return Path(arg)
    if os.environ.get('GAME_NUP'):
        return Path(os.environ['GAME_NUP'])
    nups = sorted((Path.home() / 'ocarina-ps4/game').glob('*.nup'))
    if len(nups) != 1:
        sys.exit(f'game_info: esperava um .nup em ~/ocarina-ps4/game, achei {len(nups)}; passe o caminho ou GAME_NUP')
    return nups[0]


def info(nup_path):
    main = Path(os.environ.get('OCARINA_MAIN', Path.home() / 'ocarina-ps4/ext/ocarina-recomp'))
    sys.path.insert(0, str(main / 'tools'))
    import extract_nup   # noqa: E402 (o leitor do formato mora no ocarina-recomp)
    nup = extract_nup.Nup(str(nup_path))
    try:
        base = next((p for p in nup.packages if p.kind == 0), None)
        if base is None:
            sys.exit(f'game_info: {nup_path} não tem pacote base')
        title = base.title.strip()
        if not title or title == '-':
            sys.exit(f'game_info: o pacote base de {nup_path} não traz o título')
        upd = [p for p in nup.packages if p.kind == 1]
        version = max([p.version for p in upd] + [base.version])
        title_id = f'{base.title_id:016x}'
    finally:
        nup.close()
    n = int.from_bytes(hashlib.sha256(title_id.encode()).digest()[:8], 'little') % 33333 * 3
    return {'title': title, 'title_id': title_id, 'version': version, 'display_version': base.display_version,
            'ps4_tid_teste': f'SLOP{n:05d}', 'ps4_tid_dev': f'SLOP{n + 1:05d}', 'ps4_tid': f'SLOP{n + 2:05d}',
            'ps4_data_dir': f'/data/slop/{title_id}'}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('nup', nargs='?')
    ap.add_argument('--campo')
    a = ap.parse_args()
    d = info(find_nup(a.nup))
    if a.campo:
        if a.campo not in d:
            sys.exit(f'game_info: campo desconhecido {a.campo}; há {", ".join(d)}')
        print(d[a.campo])
    else:
        print(json.dumps(d, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
