#!/bin/bash
# Frente D: roda o OoT Linux (cópia em ~/ocarina-ps4/dieta-run) com as opções do PS4 e mede a memória do host:
# malloc por pilha (malloc_census.so) + smaps a cada 5 s. Uso: dieta_medir.sh TAG [segundos] [VAR=valor ...]
TAG=${1:?tag}; SECS=${2:-150}; shift 2
D=~/ocarina-ps4/dieta-run; O=~/ocarina-ps4/dieta-out/$TAG
rm -rf "$O"; mkdir -p "$O" "$D/perfil"
export XDG_RUNTIME_DIR=/run/user/$(id -u) WAYLAND_DISPLAY=wayland-0 DISPLAY=:0
export __NV_PRIME_RENDER_OFFLOAD=1 __VK_LAYER_NV_optimus=NVIDIA_only TOTK_GPU_NAME=NVIDIA
export TOTK_GPL=0 TOTK_ASYNC_PREEMPT=0
export DKB_SKIP_CS="3bdfe449de832903,909139c13b510f1f,753f8ab880fe7923,c968ad44272dcbb4,785a41d2df6dc095,db8c39bdb420e068,dec1feb3c7fa967a,09931d0c57c6b67f,13cab66ee383c4f4,3f6f2b1d671badcd"
export TOTK_PROFILE_DIR=$D/perfil TOTK_VRAM_HEADROOM=0 TOTK_LIB_PREBUILD=0 TOTK_ASTC=cpu
export __GL_SHADER_DISK_CACHE=0
for kv in "$@"; do export "$kv"; done
cd "$D" || exit 1
MC_OUT=$O/mc MC_PERIOD=10 LD_PRELOAD=$HOME/ocarina-ps4/malloc_census.so \
  nice -n 10 timeout -k 10 "$SECS" ./ocarina > "$O/stdout.log" 2> "$O/stderr.log" &
TPID=$!; sleep 2; PID=$(pgrep -P $TPID | head -1)
i=0
while kill -0 "$PID" 2>/dev/null; do
  { echo "== t=$((i*5))"; cat /proc/$PID/smaps_rollup 2>/dev/null | grep -E "^(Rss|Pss_Anon|Pss_File|Anonymous)"; } >> "$O/rollup.txt"
  [ $((i % 6)) = 0 ] && cp /proc/$PID/smaps "$O/smaps.$((i*5))" 2>/dev/null
  i=$((i+1)); sleep 5
done
wait
cp "$D/logs/totk.log" "$O/totk.log" 2>/dev/null
echo "fim $TAG" > "$O/done"
