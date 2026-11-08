#!/bin/bash
# Fase 0B: sincroniza bench/ do Mac para o IdeaPad e compila/roda lá.
#   bash tools/bench.sh linux [segundos]   compila para Linux e roda na GTX 1650 (validação)
#   bash tools/bench.sh ps4                compila o eboot do PS4 (não envia nada ao console)
set -e
H=ideapad3i; L=$(cd "$(dirname "$0")/.." && pwd)
rsync -a --delete "$L/bench/" $H:ocarina-ps4/bench/
case "$1" in
linux)
  ssh $H "cd ~/ocarina-ps4 && cmake -S bench -B build/bench-linux -G Ninja -DCMAKE_BUILD_TYPE=Release > build/bench-linux.cfg.log 2>&1 || { tail -20 build/bench-linux.cfg.log; exit 1; }
          nice -n 10 cmake --build build/bench-linux -j4 2>&1 | grep -E 'error|warning|FAILED' | head -30
          cd build/bench-linux && rm -f results.csv && ./bench ${2:-8} 2 2>&1 | tail -20 && column -s, -t results.csv" ;;
ps4)
  ssh $H "cd ~/ocarina-ps4 && K=\$PWD/ext/orbis-porting-kit S=\$PWD/sdk/orbis-sdk-v1 &&
          cmake -S bench -B build/bench-ps4 -G Ninja -DCMAKE_TOOLCHAIN_FILE=\$S/toolchain/orbis-sdk.cmake -DORBIS_KIT_DIR=\$K > build/bench-ps4.cfg.log 2>&1 || { tail -20 build/bench-ps4.cfg.log; exit 1; }
          nice -n 10 cmake --build build/bench-ps4 -j4 2>&1 | grep -E 'error|warning|FAILED' | head -30
          ls -la build/bench-ps4 | grep -E 'eboot|oelf'" ;;
*) echo "uso: bench.sh linux [segundos] | ps4"; exit 2 ;;
esac
