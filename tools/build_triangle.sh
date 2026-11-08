#!/bin/bash
# Fase 0B: compila o triângulo Vulkan de exemplo do orbis-porting-kit com o bundle orbis-sdk-v1 (roda no IdeaPad).
set -e
R=~/ocarina-ps4; K=$R/ext/orbis-porting-kit; S=$R/sdk/orbis-sdk-v1
rm -rf $R/build/triangle; mkdir -p $R/build
cmake -S $K/examples/triangle -B $R/build/triangle -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$S/toolchain/orbis-sdk.cmake -DORBIS_KIT_DIR=$K > $R/build/triangle.cfg.log 2>&1 || { tail -20 $R/build/triangle.cfg.log; exit 1; }
nice -n 10 cmake --build $R/build/triangle -j4 > $R/build/triangle.build.log 2>&1 || { tail -30 $R/build/triangle.build.log; exit 1; }
tail -3 $R/build/triangle.build.log; ls -la $R/build/triangle | grep -iE "eboot|oelf|\.pkg"
