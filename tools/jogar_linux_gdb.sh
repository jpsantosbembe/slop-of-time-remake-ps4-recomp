#!/bin/bash
# Fase 1A: o mesmo que jogar_linux.sh, mas dentro do gdb (não interativo): na primeira falha de segmentação imprime a
# pilha, os registradores e a biblioteca do endereço. Saída em ~/ocarina-ps4/oot-gdb.log
R=~/ocarina-ps4/ext/ocarina-recomp
export XDG_RUNTIME_DIR=/run/user/$(id -u) WAYLAND_DISPLAY=wayland-0 DISPLAY=:0
export __NV_PRIME_RENDER_OFFLOAD=1 __VK_LAYER_NV_optimus=NVIDIA_only TOTK_GPU_NAME=NVIDIA
export TOTK_GPL=0 TOTK_ASYNC_PREEMPT=0
export DKB_SKIP_CS="3bdfe449de832903,909139c13b510f1f,753f8ab880fe7923,c968ad44272dcbb4,785a41d2df6dc095,db8c39bdb420e068,dec1feb3c7fa967a,09931d0c57c6b67f,13cab66ee383c4f4,3f6f2b1d671badcd"
export TOTK_PROFILE_DIR=$R/perfil
cd "$R/build/oot111" || exit 1
# o runtime usa SIGSEGV de propósito (write-watch); só para quando a falha não é tratada: "handle SIGSEGV nostop"
# não serve, então o gdb para em toda SIGSEGV e continua até a que mata (as tratadas não chegam ao fim do comando)
timeout -k 10 "${1:-300}" gdb -batch -nx \
  -ex "set pagination off" -ex "set confirm off" \
  -ex "handle SIGSEGV stop print" -ex "handle SIGUSR1 SIGUSR2 SIG34 SIG35 nostop noprint pass" \
  -ex "run" \
  -ex "info symbol \$pc" -ex "x/4i \$pc" -ex "info registers rsp rbp rip" -ex "bt 25" \
  -ex "info sharedlibrary" \
  ./ocarina > ~/ocarina-ps4/oot-gdb.log 2>&1
echo "saiu rc=$?" >> ~/ocarina-ps4/oot-gdb.log
