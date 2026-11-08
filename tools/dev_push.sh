#!/bin/bash
# Ciclo de desenvolvimento sem reinstalar: o IdeaPad tira a depuração do ocarina.elf do build PS4 e sobe direto para o
# console (<pasta de dados>/game.elf, via .tmp + rename), e o Mac pede reload ao carregador de dev.
# Pasta de dados: SLOP_DATA_DIR, ou game_info.py --campo ps4_data_dir no IdeaPad (/data/slop/<title id>).
#   dev_push.sh [--build] [--no-reload]
#     --build      antes, recompila o eboot no IdeaPad (tools/orbis/build_ps4.sh --no-pkg)
#     --no-reload  só envia o ELF (o app pega na próxima abertura)
set -eo pipefail
H=ideapad3i; PS4=192.168.1.99
L=$(cd "$(dirname "$0")/.." && pwd)
build=0 reload=1
for a in "$@"; do
  case "$a" in --build) build=1 ;; --no-reload) reload=0 ;; *) echo "uso: dev_push.sh [--build] [--no-reload]" >&2; exit 2 ;; esac
done
if [ $build = 1 ]; then
  ssh $H 'cd ~/ocarina-ps4/ext/ocarina-recomp-orbis && bash tools/orbis/build_ps4.sh --no-pkg 2>&1 | tail -5' \
    | tee /dev/stderr | grep -q "fim rc=0" || { echo "build falhou; nada enviado" >&2; exit 1; }
fi
DATA=${SLOP_DATA_DIR:-$(ssh $H "python3 -I ~/ocarina-ps4/game_info.py --campo ps4_data_dir")}
[[ "$DATA" == /data/?* ]] || { echo "pasta de dados inválida: '$DATA'" >&2; exit 1; }
export SLOP_DATA_DIR=$DATA   # o dev_reload.py usa a mesma
ssh $H "PS4=$PS4 DATA=$DATA bash -s" <<'REMOTE'
set -eo pipefail
E=~/ocarina-ps4/ext/ocarina-recomp-orbis/build/oot111-orbis/ocarina.elf
T=$(mktemp /tmp/ocarina-dev.XXXXXX)
trap 'rm -f "$T"' EXIT
# O TLS do jogo tem de caber na reserva do carregador instalado (loader.c TLS_RESERVE, 0x2000); senão ele recusa o ELF
TLS=$(llvm-readelf -lW "$E" | awk '$1=="TLS"{print $6}')
[ $((TLS)) -le $((0x2000)) ] || { echo "dev_push: TLS do jogo $TLS passa da reserva do carregador (0x2000); nada enviado" >&2; exit 1; }
# ... e sem dados iniciais (.tdata): o carregador só zera a reserva. Uma __thread com valor inicial diferente de zero quebra isso.
TDATA=$(llvm-readelf -lW "$E" | awk '$1=="TLS"{print $5}')
[ $((TDATA)) -eq 0 ] || { echo "dev_push: o TLS do jogo tem $TDATA bytes de dados iniciais (.tdata); o carregador recusa. Procure uma __thread com valor inicial; nada enviado" >&2; exit 1; }
llvm-objcopy --strip-all "$E" "$T"
python3 - "$T" "$PS4" "$DATA" <<'PY'
import ftplib, os, sys, time
src, ps4, data = sys.argv[1], sys.argv[2], sys.argv[3]
dst = f'{data}/game.elf'
f = ftplib.FTP(); f.connect(ps4, 2121, timeout=120); f.login(); f.set_pasv(True)
t = time.time()
with open(src, 'rb') as fh:
    f.storbinary(f'STOR {dst}.tmp', fh, blocksize=1 << 20)
try:
    f.delete(dst)
except ftplib.error_perm:
    pass
f.rename(f'{dst}.tmp', dst)
n = f.size(dst)
print(f'{dst}: {n} bytes em {time.time() - t:.1f} s (local {os.path.getsize(src)})')
assert n == os.path.getsize(src), 'tamanho diferente no console'
f.quit()
PY
REMOTE
[ $reload = 1 ] && python3 "$L/tools/dev_reload.py" reload
true
