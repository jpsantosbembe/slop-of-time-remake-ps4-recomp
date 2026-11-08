#!/bin/bash
# Empacota um eboot.bin num PKG fake-signed (roda no IdeaPad). Segue a receita do OpenOrbis, como o make-pkg.sh do
# orbis-porting-kit, mas com o PkgTool .NET 8 do LibOrbisPkg já compilado no IdeaPad (sem o shim de OpenSSL 1.x).
#   make_pkg.sh <eboot.bin> <dir_saida> <TITLE_ID> "<título>" [neo]
#   TITLE_ID: 4 letras + 5 dígitos. "neo" liga o bit NEO (0x00800000) no ATTRIBUTE: no PS4 Pro o app roda no modo Pro.
set -e
EBOOT=$1 OUT=$2 TID=$3 TITLE=$4 NEO=$5
[ -f "$EBOOT" ] && [ -n "$OUT" ] && [[ "$TID" =~ ^[A-Z]{4}[0-9]{5}$ ]] && [ -n "$TITLE" ] || { echo "uso: make_pkg.sh <eboot> <saida> <TITLE_ID> <título> [neo]"; exit 2; }
PKGTOOL=~/LibOrbisPkg/PkgTool/bin/Release/net8.0/PkgTool
OO=~/ps4toolchain/OpenOrbis/PS4Toolchain
MODS=$OO/samples/graphics/sce_module
LABEL=$(printf '%-16s' "$(echo "$TITLE" | tr -cd 'A-Za-z0-9' | tr a-z A-Z | cut -c1-16)" | tr ' ' 0)
CID="IV0000-${TID}_00-${LABEL}"
ATTR=0; [ "$NEO" = neo ] && ATTR=8388608
STAGE=$(mktemp -d); trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/sce_sys" "$STAGE/sce_module" "$OUT"
cp "$EBOOT" "$STAGE/eboot.bin"
cp "$MODS/libc.prx" "$MODS/libSceFios2.prx" "$STAGE/sce_module/"
SFO=$STAGE/sce_sys/param.sfo
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1
$PKGTOOL sfo_new "$SFO" >/dev/null
s() { $PKGTOOL sfo_setentry "$SFO" "$1" --type "$2" --maxsize "$3" --value "$4" >/dev/null; }
s APP_TYPE Integer 4 1; s APP_VER Utf8 8 01.00; s ATTRIBUTE Integer 4 $ATTR; s CATEGORY Utf8 4 gd
s CONTENT_ID Utf8 48 "$CID"; s DOWNLOAD_DATA_SIZE Integer 4 0; s SYSTEM_VER Integer 4 0
s TITLE Utf8 128 "$TITLE"; s TITLE_ID Utf8 12 "$TID"; s VERSION Utf8 8 01.00
# Arte (ícone e fundos) gerada da cópia do jogo de quem builda (arte/tools/arte_pkg.py); nunca vem do repositório.
# ARTE=<dir> com icon0.png (512x512) e pic0.png/pic1.png (1920x1080); sem o diretório, o PKG sai sem arte.
ARTE=${ARTE:-$HOME/ocarina-ps4/arte/pkg}
FILES="eboot.bin sce_sys/param.sfo sce_module/libc.prx sce_module/libSceFios2.prx"
for f in icon0.png pic0.png pic1.png; do
  [ -f "$ARTE/$f" ] && cp "$ARTE/$f" "$STAGE/sce_sys/$f" && FILES="$FILES sce_sys/$f"
done
( cd "$STAGE" && $OO/bin/linux/create-gp4 -out pkg.gp4 --content-id="$CID" --files "$FILES" >/dev/null &&
  $PKGTOOL pkg_build pkg.gp4 . >/dev/null )
cp "$STAGE/$CID.pkg" "$OUT/"
echo "$OUT/$CID.pkg (ATTRIBUTE=$ATTR)"; ls -la "$OUT/$CID.pkg"
