#!/bin/bash
# Fase 1A: abre o OoT recompilado (ocarina-recomp) na tela do IdeaPad (sessão Wayland do usuário, GPU NVIDIA).
# Mesmas variáveis do jogar/jogar-ocarina.ps1 do autor. Uso: jogar_linux.sh [segundos]  (padrão 180; encerra sozinho)
# Logs: ~/ocarina-ps4/oot-run.log (saída) e ~/ocarina-ps4/oot-run-err.log (erros)
R=~/ocarina-ps4/ext/ocarina-recomp
export XDG_RUNTIME_DIR=/run/user/$(id -u) WAYLAND_DISPLAY=wayland-0 DISPLAY=:0
export __NV_PRIME_RENDER_OFFLOAD=1 __VK_LAYER_NV_optimus=NVIDIA_only TOTK_GPU_NAME=NVIDIA
export TOTK_GPL=0 TOTK_ASYNC_PREEMPT=0
export DKB_SKIP_CS="3bdfe449de832903,909139c13b510f1f,753f8ab880fe7923,c968ad44272dcbb4,785a41d2df6dc095,db8c39bdb420e068,dec1feb3c7fa967a,09931d0c57c6b67f,13cab66ee383c4f4,3f6f2b1d671badcd"
export TOTK_PROFILE_DIR=$R/perfil
mkdir -p "$TOTK_PROFILE_DIR"
cd "$R/build/oot111" || exit 1
timeout -k 10 "${1:-180}" ./ocarina > ~/ocarina-ps4/oot-run.log 2> ~/ocarina-ps4/oot-run-err.log
echo "saiu rc=$?" >> ~/ocarina-ps4/oot-run.log
