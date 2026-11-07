#!/usr/bin/env bash
# Verifica a compilação do runtime para o PS4, sem ligar nada: as mesmas flags que o toolchain/orbis-sdk.cmake do
# bundle orbis-sdk-v1 passa (ninja -t commands de um projeto mínimo), mais -march=btver2 (Jaguar) e as do RT_FLAGS do
# tools/build_game.py.
#
#   tools/orbis/check_platform.sh            só runtime/platform/orbis.c (tem de passar sem erro e sem declaração
#                                             implícita)
#   tools/orbis/check_platform.sh --all      também cada runtime/*.c que não é de outro SO (-fsyntax-only), listando
#                                             as três primeiras linhas de erro de cada um que quebra
#
# O bundle vem de ORBIS_SDK (padrão: ~/ocarina-ps4/sdk/orbis-sdk-v1). CC escolhe o clang (padrão: clang).
#
# ⚠ -nostdlibinc é obrigatório aqui: o driver FreeBSD do clang ignora -isysroot para os includes padrão e procura
# também no /usr/include do HOST, então um header que falta no SDK (SDL3, opus, zstd...) seria achado no Fedora sem
# aviso. O toolchain/orbis-sdk.cmake do bundle tem esse vazamento.
#
# Só no --all, para a lista mostrar o que quebra no CÓDIGO e não apenas os headers que faltam:
#   - as bibliotecas de terceiros sem versão PS4 ainda (SDL3, opus, vpx, dav1d, zstd, VMA) entram por um diretório
#     temporário com links só para os headers delas no host (HOST_INC, padrão /usr/include); não substituem as libs;
#   - os headers gerados pelo build do jogo (layout.h, romfs_config.h) vêm de GEN_DIR e GEN_BUILD_DIR, quando existem
#     (padrões: a árvore de trabalho ocarina-recomp vizinha, só leitura).
set -u
cd "$(dirname "$0")/../.."

B=${ORBIS_SDK:-$HOME/ocarina-ps4/sdk/orbis-sdk-v1}
CC=${CC:-clang}
if [ ! -f "$B/BUNDLE.txt" ]; then
    echo "check_platform: bundle orbis-sdk não encontrado em $B (defina ORBIS_SDK)" >&2
    exit 2
fi

# A ordem dos -isystem importa: o overlay orbis-compat antes do SDK (README do bundle).
FLAGS=(--target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -D__PS4__ -DPS4 -D__ORBIS__ -D_BSD_SOURCE=1
       -nostdlibinc -isysroot "$B/sdk" -isystem "$B/orbis-compat/include" -isystem "$B/sdk/include"
       -march=btver2 -O2 -g -Wall -fno-strict-aliasing
       -Werror=implicit-function-declaration -Werror=implicit-int -Werror=int-conversion)

echo "== runtime/platform/orbis.c"
if ! "$CC" "${FLAGS[@]}" -c runtime/platform/orbis.c -o /dev/null; then
    echo "check_platform: orbis.c NÃO compila" >&2
    exit 1
fi
echo "ok"

[ "${1:-}" = "--all" ] || exit 0

# O resto do runtime: o que não é de outro SO (nem *_win, *_sdl, win32.c, linux.c), com os headers do Vulkan do Mesa
# do bundle. Só sintaxe: o objetivo é a lista do que quebra sob __ORBIS__, não um objeto.
HOST_INC=${HOST_INC:-/usr/include}
GEN_DIR=${GEN_DIR:-$HOME/ocarina-ps4/ext/ocarina-recomp/generated/oot111}
GEN_BUILD_DIR=${GEN_BUILD_DIR:-$HOME/ocarina-ps4/ext/ocarina-recomp/build/oot111}
shim=$(mktemp -d)
trap 'rm -rf "$shim"' EXIT
for h in SDL3 opus vpx dav1d zstd.h zstd_errors.h vk_mem_alloc.h; do
    [ -e "$HOST_INC/$h" ] && ln -s "$HOST_INC/$h" "$shim/$h"
done
EXTRA=(-isystem "$B/mesa/include" -Iruntime -isystem "$shim")
[ -f "$GEN_DIR/layout.h" ] && EXTRA+=(-I "$GEN_DIR")
[ -f "$GEN_BUILD_DIR/romfs_config.h" ] && EXTRA+=(-I "$GEN_BUILD_DIR")

ok=0
bad=0
broken=()
for f in runtime/*.c runtime/platform/common.c; do
    n=$(basename "$f" .c)
    case "$n" in *_win|*_sdl) continue ;; esac
    out=$("$CC" "${FLAGS[@]}" "${EXTRA[@]}" -fsyntax-only "$f" 2>&1)
    if [ $? -eq 0 ]; then
        ok=$((ok + 1))
    else
        bad=$((bad + 1))
        broken+=("$f")
        echo "== $f"
        echo "$out" | grep -E 'error:' | head -3
    fi
done
echo
echo "check_platform: $ok arquivos de runtime/*.c compilam, $bad não"
for f in ${broken[@]+"${broken[@]}"}; do echo "  $f"; done
exit 0
