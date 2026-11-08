#!/bin/bash
# Teste da ponte do tradutor (modo "on the fly pela rede") no IdeaPad: o OoT da worktree ps4-orbis fala com o
# tradutor por TCP (OCARINA_XLATE_REMOTE), como o PS4 vai fazer, em vez de abrir o processo.
#   jogar_linux_ponte.sh [segundos]   (padrão 180)
# Logs: ~/ocarina-ps4/ponte.log (a ponte), ~/ocarina-ps4/oot-ponte-err.log (o jogo)
W=~/ocarina-ps4/ext/ocarina-recomp-orbis
python3 ~/ocarina-ps4/xlate_bridge.py --port 9050 > ~/ocarina-ps4/ponte.log 2>&1 &
BRIDGE=$!
sleep 1
export XDG_RUNTIME_DIR=/run/user/$(id -u) WAYLAND_DISPLAY=wayland-0 DISPLAY=:0
export __NV_PRIME_RENDER_OFFLOAD=1 __VK_LAYER_NV_optimus=NVIDIA_only TOTK_GPU_NAME=NVIDIA
export TOTK_GPL=0 TOTK_ASYNC_PREEMPT=0
export DKB_SKIP_CS="3bdfe449de832903,909139c13b510f1f,753f8ab880fe7923,c968ad44272dcbb4,785a41d2df6dc095,db8c39bdb420e068,dec1feb3c7fa967a,09931d0c57c6b67f,13cab66ee383c4f4,3f6f2b1d671badcd"
export TOTK_PROFILE_DIR=$W/perfil
export OCARINA_XLATE_REMOTE=127.0.0.1:9050
mkdir -p "$TOTK_PROFILE_DIR"
cd "$W/build/oot111" || { kill $BRIDGE; exit 1; }
timeout -k 10 "${1:-180}" ./ocarina > ~/ocarina-ps4/oot-ponte.log 2> ~/ocarina-ps4/oot-ponte-err.log
echo "saiu rc=$?" >> ~/ocarina-ps4/oot-ponte.log
kill $BRIDGE
