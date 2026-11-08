#!/bin/bash
# Instala um slot de save do jogo no console, para os testes começarem num ponto do jogo e não no New Game.
# Copia <pasta>/caption.sav e progress.sav para <pasta de dados>/profile/save/<uid>/<nome do slot>/ por FTP.
# <uid> é a única subpasta de profile/save (a conta que o runtime criou na primeira execução). O runtime refaz
# save-tx/work a partir de save/<uid> ao abrir, então o slot vale na próxima execução. Rode com o jogo fechado ou
# antes de um reload: o jogo aberto pode gravar por cima.
#   instalar_save.sh <pasta slot_XX> [nome do slot no console, padrão o da pasta]
# A pasta é lida no IdeaPad (os saves ficam lá, fora do repositório: ~/ocarina-ps4/saves/). docs/saves.md.
set -eo pipefail
H=ideapad3i; PS4=192.168.1.99
[ $# -ge 1 ] || { echo "uso: instalar_save.sh <pasta slot_XX no IdeaPad> [nome]" >&2; exit 2; }
SRC=$1; NAME=${2:-$(basename "$SRC")}
[[ "$NAME" =~ ^slot_[0-9]{2}$ ]] || { echo "nome do slot inválido: '$NAME' (slot_00 a slot_99)" >&2; exit 2; }
DATA=${SLOP_DATA_DIR:-$(ssh $H "python3 -I ~/ocarina-ps4/game_info.py --campo ps4_data_dir")}
[[ "$DATA" == /data/?* ]] || { echo "pasta de dados inválida: '$DATA'" >&2; exit 1; }
ssh $H "PS4=$PS4 DATA=$DATA SRC=$SRC NAME=$NAME python3 -" <<'PY'
import ftplib, os, sys
ps4, data, name = os.environ['PS4'], os.environ['DATA'], os.environ['NAME']
src = os.path.expanduser(os.environ['SRC'])
files = ['caption.sav', 'progress.sav']
for n in files:
    if not os.path.isfile(f'{src}/{n}'):
        sys.exit(f'instalar_save: falta {src}/{n}')
f = ftplib.FTP(); f.connect(ps4, 2121, timeout=60); f.login(); f.set_pasv(True)
root = f'{data}/profile/save'
uids = [n for n, fa in f.mlsd(root) if fa.get('type') == 'dir']
if len(uids) != 1:
    sys.exit(f'instalar_save: esperava uma conta em {root}, achei {uids}; rode o jogo uma vez antes')
dst = f'{root}/{uids[0]}/{name}'
# A pasta da conta é do app (uid 1, 0755) e o FTP não escreve nela: SITE CHMOD 777 antes. O slot e os arquivos saem 777/666
# para o app (outro uid) poder regravar e trocar o save depois.
f.sendcmd(f'SITE CHMOD 777 {root}/{uids[0]}')
try:
    f.mkd(dst)
except ftplib.error_perm:
    pass   # já existe: os arquivos são substituídos
f.sendcmd(f'SITE CHMOD 777 {dst}')
for n in files:
    with open(f'{src}/{n}', 'rb') as fh:
        f.storbinary(f'STOR {dst}/{n}.tmp', fh)
    try:
        f.delete(f'{dst}/{n}')
    except ftplib.error_perm:
        pass
    f.rename(f'{dst}/{n}.tmp', f'{dst}/{n}')
    f.sendcmd(f'SITE CHMOD 666 {dst}/{n}')
    print(f'{dst}/{n}: {f.size(f"{dst}/{n}")} bytes (local {os.path.getsize(f"{src}/{n}")})')
f.quit()
PY
