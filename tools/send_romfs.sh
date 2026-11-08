#!/bin/bash
# Envia o RomFS do jogo (romfs.bin + index.json) do IdeaPad para <pasta de dados>/romfs/ no PS4, por FTP, com retomada.
# Log em ~/ocarina-ps4/send-romfs.log; "fim rc=N" no fim.
R=~/ocarina-ps4/ext/ocarina-recomp/build/romfs
D=$(python3 -I ~/ocarina-ps4/game_info.py --campo ps4_data_dir)
[[ "$D" == /data/?* ]] || { echo "pasta de dados inválida: '$D'" >&2; exit 1; }
F=ftp://192.168.1.99:2121$D/romfs
{
  curl -sS --ftp-create-dirs -T "$R/index.json" "$F/index.json" &&
  curl -sS -C - --ftp-create-dirs -T "$R/romfs.bin" "$F/romfs.bin"
  echo "fim rc=$?"
} > ~/ocarina-ps4/send-romfs.log 2>&1
