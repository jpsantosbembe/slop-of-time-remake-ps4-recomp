#!/bin/bash
# Frente N: roda o OoT Linux (~/ocarina-ps4/memoria-run) com as opções do PS4, aperta botões por TOTK_INPUT_SCRIPT e
# tira retratos da memória do guest (memwin.py) a cada PASSO segundos, mais as imagens da tela.
# Uso: medir_memoria.sh TAG SEGUNDOS PASSO SCRIPT_DE_INPUT [VAR=valor ...]
TAG=${1:?tag}; SECS=${2:-240}; STEP=${3:-15}; INP=${4:?input}; shift 4
D=~/ocarina-ps4/memoria-run; O=~/ocarina-ps4/memoria-out/$TAG; PY=~/ocarina-ps4/ext/ocarina-recomp/.venv/bin/python
MW=~/ocarina-ps4/memoria-tools/memwin.py
rm -rf "$O"; mkdir -p "$O"
rm -rf "$D/perfil/save" "$D/perfil/save-tx" "$D/perfil/save-backup" "$D/perfil/temp" "$D/logs"
export XDG_RUNTIME_DIR=/run/user/$(id -u) WAYLAND_DISPLAY=wayland-0 DISPLAY=:0
export __NV_PRIME_RENDER_OFFLOAD=1 __VK_LAYER_NV_optimus=NVIDIA_only TOTK_GPU_NAME=NVIDIA
export TOTK_GPL=0 TOTK_ASYNC_PREEMPT=0
export DKB_SKIP_CS="3bdfe449de832903,909139c13b510f1f,753f8ab880fe7923,c968ad44272dcbb4,785a41d2df6dc095,db8c39bdb420e068,dec1feb3c7fa967a,09931d0c57c6b67f,13cab66ee383c4f4,3f6f2b1d671badcd"
export TOTK_PROFILE_DIR=$D/perfil TOTK_VRAM_HEADROOM=0 TOTK_LIB_PREBUILD=0 TOTK_ASTC=cpu TOTK_VK_SSBO_ALIGN=16
export TOTK_PERF_OVERLAY=fps OCARINA_SHOT_EVERY=${OCARINA_SHOT_EVERY:-60} __GL_SHADER_DISK_CACHE=0
export TOTK_INPUT_SCRIPT=$INP
for kv in "$@"; do export "$kv"; done
cd "$D" || exit 1
if [ -n "$GDBX" ]; then
  nice -n 10 timeout -k 10 "$SECS" gdb -q -batch -x "$GDBX" --args ./ocarina > "$O/stdout.log" 2> "$O/stderr.log" &
else
  nice -n 10 timeout -k 10 "$SECS" ./ocarina > "$O/stdout.log" 2> "$O/stderr.log" &
fi
TPID=$!; sleep 4; PID=$(pgrep -n -x ocarina)
echo "pid $PID" > "$O/info"
t0=$(date +%s); next=$STEP
while kill -0 "$PID" 2>/dev/null; do
  t=$(( $(date +%s) - t0 ))
  for s in "$D"/logs/shot_*.png; do [ -f "$s" ] && [ "$s" -nt "$O/.last" ] && cp "$s" "$O/shot_t${t}_$(basename "$s")"; done
  touch "$O/.last"
  if [ "$t" -ge "$next" ]; then
    nice -n 10 "$PY" -I "$MW" "$PID" "$O/win_t$t.npz" >> "$O/memwin.log" 2>&1
    grep -E "orbis mem|frame step|input:|scene|movie" "$D/logs/totk.log" 2>/dev/null | tail -3 | sed "s/^/  t=$t /" >> "$O/memwin.log"
    next=$((t + STEP))
  fi
  sleep 3
done
wait
cp "$D/logs/totk.log" "$O/totk.log" 2>/dev/null
echo "fim $TAG" > "$O/done"
