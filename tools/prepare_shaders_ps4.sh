#!/bin/bash
# Gera no IdeaPad o cache de shaders do PS4: traduz a semente (seed-programs.bin + seed-answers.bin da árvore principal)
# com as respostas da GPU do PS4 (f64=1 sbuf=16 sg=64 gather=8; o console roda com TOTK_VK_SSBO_ALIGN=16), pelo build Linux da
# worktree ps4-orbis. Saída em ~/ocarina-ps4/ps4-shaders/: shader-cache/shader-cache.pack, shader-prepared.bin e o
# totk_xlate.id. Log: ~/ocarina-ps4/prepare-ps4.log (+ -err.log); "fim rc=N" no fim.
M=~/ocarina-ps4/ext/ocarina-recomp; W=~/ocarina-ps4/ext/ocarina-recomp-orbis; O=~/ocarina-ps4/ps4-shaders
B=$W/build/oot111
mkdir -p "$O/shader-cache"
ln -sf "$M/build/oot111/seed-programs.bin" "$B/seed-programs.bin"
ln -sf "$M/build/oot111/seed-answers.bin" "$B/seed-answers.bin"
export XDG_RUNTIME_DIR=/run/user/$(id -u) WAYLAND_DISPLAY=wayland-0 DISPLAY=:0
export __NV_PRIME_RENDER_OFFLOAD=1 __VK_LAYER_NV_optimus=NVIDIA_only TOTK_GPU_NAME=NVIDIA
export TOTK_GPL=0 TOTK_ASYNC_PREEMPT=0
export DKB_SKIP_CS="3bdfe449de832903,909139c13b510f1f,753f8ab880fe7923,c968ad44272dcbb4,785a41d2df6dc095,db8c39bdb420e068,dec1feb3c7fa967a,09931d0c57c6b67f,13cab66ee383c4f4,3f6f2b1d671badcd"
export TOTK_PROFILE_DIR=$W/perfil
export TOTK_XLATE=$M/build/xlate/totk_xlate TOTK_SHADER_CACHE=$O/shader-cache
# sbuf=16: o PS4 roda com TOTK_VK_SSBO_ALIGN=16 (no alinhamento 4 da AMD, leituras de 64/128 bits de SSBO pegam a
# palavra errada: Lichii PR #295, docs/nx2/amd.md)
export OCARINA_SHADER_SG=64 TOTK_GATHER_BIAS=8 OCARINA_SHADER_SBUF=16
cd "$B" || exit 1
start=$(date +%s)
nice -n 10 ./ocarina --prepare-shaders > ~/ocarina-ps4/prepare-ps4.log 2> ~/ocarina-ps4/prepare-ps4-err.log
rc=$?
cp -f "$B/shader-prepared.bin" "$M/build/xlate/totk_xlate.id" "$O/" 2>/dev/null
echo "fim rc=$rc em $(( $(date +%s) - start )) s" >> ~/ocarina-ps4/prepare-ps4.log
