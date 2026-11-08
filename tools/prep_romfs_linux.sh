#!/bin/bash
# Fase 1A: RomFS do OoT para o build Linux, a partir do .nup copiado para o IdeaPad.
# extract_nup.py extrai e confere cada arquivo pelo SHA-256 do contêiner; pack_romfs.py faz o romfs.bin + índice que o
# runtime lê, apagando cada arquivo extraído ao empacotar (--consume), para não ocupar o dobro de disco.
# Log em ~/ocarina-ps4/oot-romfs.log; "fim rc=N" na última linha.
set -o pipefail
R=~/ocarina-ps4/ext/ocarina-recomp
NUP=~/ocarina-ps4/game/oot.nup
cd "$R" || exit 1
{
  .venv/bin/python tools/extract_nup.py "$NUP" --list &&
  nice -n 10 .venv/bin/python tools/extract_nup.py "$NUP" --out build/dump --only romfs --overlay-update &&
  nice -n 10 .venv/bin/python tools/pack_romfs.py --dir build/dump/romfs --out build/romfs --consume
  echo "fim rc=$?"
} > ~/ocarina-ps4/oot-romfs.log 2>&1
