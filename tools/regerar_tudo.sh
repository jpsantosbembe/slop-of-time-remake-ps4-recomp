#!/bin/bash
# Depois de incorporar o upstream (IdeaPad): tradutor, build Linux da worktree, cache de shaders com as respostas do PS4,
# pacote do cache e ELF do PS4, em série e com poucos recursos. --do-cache começa no cache. Log: ~/ocarina-ps4/regerar.log; "fim rc=N" no fim.
set -o pipefail
R=~/ocarina-ps4; M=$R/ext/ocarina-recomp; W=$R/ext/ocarina-recomp-orbis
exec > $R/regerar.log 2>&1
passo() { echo "== $(date +%T) $*"; }
if [ "$1" != "--do-cache" ]; then
passo "tradutor"
(cd $M && nice -n 10 .venv/bin/python tools/build_xlate.py) || { echo "fim rc=1"; exit 1; }
cat $M/build/xlate/totk_xlate.id
passo "build Linux da worktree"
bash $R/build_oot_linux_wt.sh; tail -1 $R/oot-linux-wt.log | grep -q "fim rc=0" || { echo "fim rc=2"; exit 2; }
fi
passo "cache de shaders (respostas do PS4)"
rm -rf $R/ps4-shaders.old; [ -d $R/ps4-shaders ] && mv $R/ps4-shaders $R/ps4-shaders.old
rm -f $R/prepare-ps4.log $R/prepare-ps4-err.log   # senão o laço abaixo vê o "prepared" da geração anterior
timeout 1800 bash $R/prepare_shaders_ps4.sh &
PREP=$!
# o --prepare-shaders segue para o jogo depois de preparar: espera o "prepared" e encerra
until grep -q "prepared [0-9]* shader variants" $R/prepare-ps4.log 2>/dev/null || ! kill -0 $PREP 2>/dev/null; do sleep 10; done
sleep 5
pkill -f "ocarina --prepare-shaders"; sleep 3; pkill -9 -f "ocarina --prepare-shaders"
for p in $(pgrep -f "build/xlate/totk_xlate --serve"); do kill $p; done
wait $PREP 2>/dev/null
grep "prepared [0-9]* shader variants" $R/prepare-ps4.log || { echo "fim rc=3"; exit 3; }
cp -f $W/build/oot111/shader-prepared.bin $M/build/xlate/totk_xlate.id $R/ps4-shaders/
passo "pacote do cache"
(cd $W && nice -n 10 python3 tools/pack_shader_cache.py $R/ps4-shaders/shader-cache) || { echo "fim rc=4"; exit 4; }
passo "ELF do PS4"
(cd $W && bash tools/orbis/build_ps4.sh --no-pkg 2>&1 | tail -3)
grep -q "fim rc=0" $R/oot-orbis-build.log 2>/dev/null || true
passo "pronto"
echo "fim rc=0"
