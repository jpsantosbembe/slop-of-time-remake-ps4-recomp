#!/bin/bash
# Empacota um eboot.bin com a identidade e a arte do jogo, lidas dos arquivos do jogo (nada fixo no código). Roda no
# IdeaPad.
#   pkg_jogo.sh <eboot.bin> <dir_saida> <teste|dev|final>
# Título: o do pacote base do .nup (game_info.py), com " (teste)" ou " (dev)" nas variantes de desenvolvimento.
# TITLE_ID do PS4: derivado do title ID do Switch (SLOPnnnnn; teste, dev e final não colidem).
# Arte: tools/arte/arte_pkg.py com a receita jogos/<title id>/arte.json, a partir do RomFS do jogo; sem receita, sem arte.
# Variáveis: GAME_NUP (o .nup), ROMFS_DIR (romfs.bin + index.json), NEO (padrão neo; vazio desliga o modo Pro).
set -eo pipefail
EBOOT=$1 OUT=$2 VAR=$3
[ -f "$EBOOT" ] && [ -n "$OUT" ] && [[ "$VAR" =~ ^(teste|dev|final)$ ]] || { echo "uso: pkg_jogo.sh <eboot> <saida> <teste|dev|final>" >&2; exit 2; }
R=~/ocarina-ps4
PY=$R/arte/venv/bin/python
INFO=$($PY -I $R/game_info.py)
campo() { $PY -c "import json,sys; print(json.load(sys.stdin)['$1'])" <<<"$INFO"; }
TITLE=$(campo title) TID_NX=$(campo title_id)
case $VAR in
  teste) TID=$(campo ps4_tid_teste) SUF=" (teste)" ;;
  dev) TID=$(campo ps4_tid_dev) SUF=" (dev)" ;;
  final) TID=$(campo ps4_tid) SUF="" ;;
esac
ROMFS_DIR=${ROMFS_DIR:-$R/ext/ocarina-recomp/build/romfs}
RECEITA=$R/jogos/$TID_NX/arte.json
ARTE=$(mktemp -d); trap 'rm -rf "$ARTE"' EXIT
if [ -f "$RECEITA" ]; then
  nice -n 10 $PY -I $R/arte-tools/arte_pkg.py --romfs "$ROMFS_DIR" --receita "$RECEITA" --out "$ARTE" >/dev/null
else
  echo "pkg_jogo: sem receita de arte em $RECEITA; PKG sem ícone" >&2
fi
echo "pkg_jogo: \"$TITLE$SUF\" ($TID, jogo $TID_NX)"
ARTE=$ARTE bash $R/make_pkg.sh "$EBOOT" "$OUT" "$TID" "$TITLE$SUF" ${NEO-neo}
