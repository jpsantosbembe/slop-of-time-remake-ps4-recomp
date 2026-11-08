#!/bin/bash
# Compila o carregador de desenvolvimento (loader.c) e empacota o PKG "<título do jogo> (dev)" (tools/pkg_jogo.sh: nome,
# ID e arte lidos do jogo). Roda no IdeaPad,
# em ~/ocarina-ps4/loader. Confere o TLS (reserva sem dados iniciais e maior que o do jogo) e os módulos que o jogo
# pede. Não envia nada ao console.
#   build.sh [ELF do jogo para conferir]
# A pasta de dados no console (SLOP_DATA_DIR) vem de game_info.py --campo ps4_data_dir (/data/slop/<title id>).
set -eo pipefail
D=$(cd "$(dirname "$0")" && pwd)
S=~/ocarina-ps4/sdk/orbis-sdk-v1
OO=$S/sdk
GAME=${1:-$HOME/ocarina-ps4/ext/ocarina-recomp-orbis/build/oot111-orbis/ocarina.elf}
O=$D/build
DATA=$(python3 -I ~/ocarina-ps4/game_info.py --campo ps4_data_dir)
[[ "$DATA" =~ ^/data/slop/[0-9a-f]{16}$ ]] || { echo "ERRO: pasta de dados inesperada: '$DATA'" >&2; exit 1; }
echo "pasta de dados: $DATA"
rm -rf "$O"; mkdir -p "$O"
clang --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -O2 -march=btver2 -std=gnu11 -Wall -D__ORBIS__ \
  -DSLOP_DATA_DIR="\"$DATA\"" \
  -isysroot "$OO" -isystem "$OO/include" -c "$D/loader.c" -o "$O/loader.o"
ld.lld -m elf_x86_64 -pie --script "$S/toolchain/orbis-tls.ld" --eh-frame-hdr --no-rosegment -L"$OO/lib" \
  "$O/loader.o" -o "$O/loader.elf" -lc -lkernel -lSceGnmDriver -lSceVideoOut -lScePad -lSceAudioOut -lSceUserService \
  -lSceNet -lSceRandom -lSceSysmodule -lSceSystemService "$OO/lib/crt1.o"

tls() { llvm-readelf -lW "$1" | awk '$1=="TLS"{print $5, $6, $NF}'; }   # filesz memsz align
read -r lf lm la <<<"$(tls "$O/loader.elf")"
echo "TLS do carregador: filesz=$lf memsz=$lm align=$la"
[ "$lf" = 0x000000 ] || { echo "ERRO: o TLS do carregador tem dados iniciais" >&2; exit 1; }
if [ -f "$GAME" ]; then
  read -r gf gm ga <<<"$(tls "$GAME")"
  echo "TLS do jogo:       filesz=$gf memsz=$gm align=$ga"
  [ $((gm)) -le $((lm)) ] || { echo "ERRO: o TLS do jogo é maior que a reserva" >&2; exit 1; }
  need() { llvm-readelf -dW "$1" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' | sort; }
  falta=$(comm -23 <(need "$GAME") <(need "$O/loader.elf"))
  [ -z "$falta" ] || echo "aviso: o jogo pede módulos que o carregador não importa (ele tenta carregar na hora): $falta"
fi
(cd "$O" && OO_PS4_TOOLCHAIN=$OO "$OO/bin/linux/create-fself" -in=loader.elf -out=loader.oelf --eboot eboot.bin \
   --paid 0x3800000000000011 >/dev/null)
bash ~/ocarina-ps4/pkg_jogo.sh "$O/eboot.bin" "$O/pkg" dev
ls -la "$O"/pkg/*.pkg
echo "fim rc=0"
