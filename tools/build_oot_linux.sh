#!/bin/bash
# Fase 1A: o OoT recompilado (ocarina-recomp, branch ps4) para Linux x86-64, no IdeaPad.
#   build_oot_linux.sh compile   compila o C gerado e o runtime (a etapa longa), sem ligar
#   build_oot_linux.sh link      liga o executável (precisa de build/exefs e build/romfs)
# Log em ~/ocarina-ps4/oot-linux-build.log; "fim rc=N" na última linha.
set -o pipefail
R=~/ocarina-ps4/ext/ocarina-recomp
cd "$R" || exit 1
case "$1" in
compile) EXTRA=--compile-only ;;
link)    EXTRA= ;;
*) echo "uso: build_oot_linux.sh compile|link"; exit 2 ;;
esac
{
  nice -n 10 .venv/bin/python tools/build_game.py --version oot111 --gen generated/oot111 --out build/oot111 \
     --expected tools/data/expected-oot111.json --exefs build/exefs --romfs build/romfs --jobs 6 $EXTRA
  echo "fim rc=$?"
} > ~/ocarina-ps4/oot-linux-build.log 2>&1
