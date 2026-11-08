#!/bin/bash
# Fase 0: sonda. Sincroniza probe/ e o gerador, gera lift_tests.c, compila a referência Linux e o eboot do PS4 (com o
# Mesa do patch Gladius) e empacota dois PKGs (base e Pro). Não envia nada ao console.
set -e
H=ideapad3i; L=$(cd "$(dirname "$0")/.." && pwd)
rsync -a --delete "$L/probe/" $H:ocarina-ps4/probe/
scp -q "$L/tools/make_lift_test.py" "$L/tools/make_pkg.sh" $H:ocarina-ps4/
ssh $H "PROBE_TID=${PROBE_TID:-OCPR00002} PROBE_TITLE='${PROBE_TITLE:-Ocarina Sonda Pro}' bash -s" <<'REMOTE'
set -e; cd ~/ocarina-ps4
  python3 make_lift_test.py lift-test probe-gen/lift_tests.c 400
  RT=$PWD/ext/dkb-pc-recomp/runtime LT=$PWD/probe-gen/lift_tests.c
  cmake -S probe -B build/probe-linux -G Ninja -DCMAKE_C_COMPILER=clang -DRUNTIME_DIR=$RT -DLIFT_TESTS=$LT > build/probe-linux.cfg.log 2>&1
  nice -n 10 cmake --build build/probe-linux -j4 2>&1 | grep -E "error|undefined" | head -20 || true
  (cd build/probe-linux && ./probe lift_linux.txt)
  M=$PWD/ext/mesa-ps4 K=$PWD/ext/orbis-porting-kit S=$PWD/sdk/orbis-sdk-v1
  cmake -S probe -B build/probe-ps4 -G Ninja -DCMAKE_TOOLCHAIN_FILE=$S/toolchain/orbis-sdk.cmake -DORBIS_KIT_DIR=$K \
        -DORBIS_MESA_SRC=$M -DORBIS_MESA_BUILD=$M/build-orbis -DRUNTIME_DIR=$RT -DLIFT_TESTS=$LT > build/probe-ps4.cfg.log 2>&1 \
        || { tail -20 build/probe-ps4.cfg.log; exit 1; }
  nice -n 10 cmake --build build/probe-ps4 -j4 2>&1 | grep -E "error|undefined|warning: implicit" | head -30 || true
  ls -la build/probe-ps4/eboot.bin
  true
  bash make_pkg.sh build/probe-ps4/eboot.bin out $PROBE_TID "$PROBE_TITLE" neo | head -1
REMOTE
