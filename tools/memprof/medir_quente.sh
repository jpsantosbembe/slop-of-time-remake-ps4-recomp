#!/bin/bash
# Frente N: conjunto quente depois do New Game. Roda o jogo (in1: A no título e no New Game), limpa os bits de acesso
# em LIMPA s e lê o Referenced em intervalos. Uso: medir_quente.sh TAG [VAR=valor ...]
TAG=$1; shift
D=~/ocarina-ps4/memoria-run; O=~/ocarina-ps4/memoria-out/$TAG; H=~/ocarina-ps4/memoria-tools/hotset.py
rm -rf "$O"; mkdir -p "$O"; rm -rf "$D/perfil/save" "$D/perfil/save-tx" "$D/perfil/save-backup" "$D/perfil/temp"
export XDG_RUNTIME_DIR=/run/user/$(id -u) WAYLAND_DISPLAY=wayland-0 DISPLAY=:0
export __NV_PRIME_RENDER_OFFLOAD=1 __VK_LAYER_NV_optimus=NVIDIA_only TOTK_GPU_NAME=NVIDIA
export TOTK_GPL=0 TOTK_ASYNC_PREEMPT=0
export DKB_SKIP_CS="3bdfe449de832903,909139c13b510f1f,753f8ab880fe7923,c968ad44272dcbb4,785a41d2df6dc095,db8c39bdb420e068,dec1feb3c7fa967a,09931d0c57c6b67f,13cab66ee383c4f4,3f6f2b1d671badcd"
export TOTK_PROFILE_DIR=$D/perfil TOTK_VRAM_HEADROOM=0 TOTK_LIB_PREBUILD=0 TOTK_ASTC=cpu TOTK_VK_SSBO_ALIGN=16
export TOTK_PERF_OVERLAY=fps __GL_SHADER_DISK_CACHE=0 TOTK_INPUT_SCRIPT=~/ocarina-ps4/memoria-tools/in1.txt
for kv in "$@"; do export "$kv"; done
cd "$D" || exit 1
nice -n 10 timeout -k 10 150 ./ocarina > "$O/stdout.log" 2> "$O/stderr.log" &
sleep 4; PID=$(pgrep -n -x ocarina); t0=$(date +%s)
el() { echo $(( $(date +%s) - t0 + 4 )); }
until [ "$(el)" -ge 45 ]; do sleep 1; done
python3 -I $H $PID limpar; echo "limpou em t=$(el)" >> "$O/quente.txt"
for alvo in 50 60 75 100 140; do
  until [ "$(el)" -ge $alvo ] || ! kill -0 $PID 2>/dev/null; do sleep 1; done
  kill -0 $PID 2>/dev/null || break
  { echo "t=$(el) (desde a limpeza)"; python3 -I $H $PID ler; } >> "$O/quente.txt"
done
wait; echo fim > "$O/done"
