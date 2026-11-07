#!/usr/bin/env bash
# PS4: o eboot.bin do OoT recompilado (runtime inteiro + C gerado) e o PKG de teste, no IdeaPad. Não envia nada ao
# console.
#
#   tools/orbis/build_ps4.sh             compila e liga (tools/build_game.py --target orbis), depois o PKG
#   tools/orbis/build_ps4.sh --bg        o mesmo em segundo plano (nohup setsid), com o log em $LOG; "fim rc=N" no fim
#   tools/orbis/build_ps4.sh --no-pkg    só o eboot.bin
#   tools/orbis/build_ps4.sh -- --modules subsdk0 --compile-only    argumentos depois de -- vão para o build_game.py
#
# O C gerado, o ExeFS e o RomFS (só o index.json, para o romfs-index.bin) são LIDOS da árvore principal
# ($OCARINA_MAIN, padrão ~/ocarina-ps4/ext/ocarina-recomp), com o tools/oot/patch_generated.py já aplicado lá. A saída
# vai para build/oot111-orbis desta árvore: ocarina.elf, eboot.bin, romfs-index.bin e o PKG em pkg/.
# Variáveis: JOBS (padrão 8), NICE (padrão 10), OUT, LOG, TITLE_ID (OCAR00001), PKG_TITLE ("Ocarina PS4 (teste)").
set -u
R=$(cd "$(dirname "$0")/../.." && pwd)
MAIN=${OCARINA_MAIN:-$HOME/ocarina-ps4/ext/ocarina-recomp}
OUT=${OUT:-$R/build/oot111-orbis}
LOG=${LOG:-$HOME/ocarina-ps4/oot-orbis-build.log}
JOBS=${JOBS:-8}
NICE=${NICE:-10}
TITLE_ID=${TITLE_ID:-OCAR00001}
PKG_TITLE=${PKG_TITLE:-Ocarina PS4 (teste)}
MAKE_PKG=${MAKE_PKG:-$HOME/ocarina-ps4/make_pkg.sh}

bg=0 pkg=1
extra=()
while [ $# -gt 0 ]; do
    case "$1" in
    --bg) bg=1 ;;
    --no-pkg) pkg=0 ;;
    --) shift; extra=("$@"); break ;;
    *) echo "uso: build_ps4.sh [--bg] [--no-pkg] [-- <argumentos do build_game.py>]" >&2; exit 2 ;;
    esac
    shift
done

if [ $bg = 1 ]; then
    args=()
    [ $pkg = 0 ] && args+=(--no-pkg)
    [ ${#extra[@]} -gt 0 ] && args+=(-- "${extra[@]}")
    nohup setsid "$0" ${args[@]+"${args[@]}"} > "$LOG" 2>&1 < /dev/null &
    echo "build_ps4: em segundo plano (pid $!), log em $LOG"
    exit 0
fi

cd "$R" || exit 1
for d in "$MAIN/generated/oot111" "$MAIN/build/exefs" "$MAIN/build/romfs"; do
    [ -e "$d" ] || { echo "build_ps4: falta $d (árvore principal em OCARINA_MAIN)" >&2; exit 2; }
done
echo "build_ps4: $(date '+%F %T') início, $JOBS jobs, nice $NICE"
nice -n "$NICE" python3 tools/build_game.py --target orbis --version oot111 --gen "$MAIN/generated/oot111" \
    --out "$OUT" --expected tools/data/expected-oot111.json --exefs "$MAIN/build/exefs" --romfs "$MAIN/build/romfs" \
    --jobs "$JOBS" ${extra[@]+"${extra[@]}"}
rc=$?
if [ $rc = 0 ] && [ $pkg = 1 ] && [ -f "$OUT/eboot.bin" ] && [ ${#extra[@]} = 0 ]; then
    bash "$MAKE_PKG" "$OUT/eboot.bin" "$OUT/pkg" "$TITLE_ID" "$PKG_TITLE" neo
    rc=$?
fi
echo "build_ps4: $(date '+%F %T') fim"
echo "fim rc=$rc"
exit $rc
