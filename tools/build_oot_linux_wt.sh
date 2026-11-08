#!/bin/bash
# Build Linux do OoT a partir da worktree ps4-orbis (runtime dela), reaproveitando o C gerado, o ExeFS e o RomFS da
# árvore principal. Log em ~/ocarina-ps4/oot-linux-wt.log; "fim rc=N" no fim.
M=~/ocarina-ps4/ext/ocarina-recomp; W=~/ocarina-ps4/ext/ocarina-recomp-orbis
cd "$W" || exit 1
{
  nice -n 10 "$M/.venv/bin/python" tools/build_game.py --version oot111 --gen "$M/generated/oot111" --out build/oot111 \
     --expected tools/data/expected-oot111.json --exefs "$M/build/exefs" --romfs "$M/build/romfs" --jobs 6
  echo "fim rc=$?"
} > ~/ocarina-ps4/oot-linux-wt.log 2>&1
