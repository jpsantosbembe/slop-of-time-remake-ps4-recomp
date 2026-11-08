#!/bin/bash
# Frente N: compila arquivos do runtime da worktree frente-memoria com os flags do build do principal (linux e orbis).
# Uso: memoria_cc.sh [linux|orbis|ambos] arquivo.c ...  Objetos em ~/ocarina-ps4/memoria-obj/<alvo>/
W=~/ocarina-ps4/ext/frente-memoria; P=~/ocarina-ps4/ext/ocarina-recomp-orbis
which=$1; shift
case $which in linux) T="oot111";; orbis) T="oot111-orbis";; *) T="oot111 oot111-orbis";; esac
for tgt in $T; do
  mkdir -p ~/ocarina-ps4/memoria-obj/$tgt
  for f in "$@"; do
    rel=${f#runtime/}; o=build/$tgt/obj/runtime/${rel%.*}.o
    cmd=$(cd $P && ninja -f build/$tgt/build.ninja -t commands $o 2>/dev/null | tail -1)
    [ -z "$cmd" ] && { echo "$tgt $f: sem comando"; continue; }
    out=~/ocarina-ps4/memoria-obj/$tgt/$(echo ${rel%.*} | tr / _).o
    cmd=$(echo "$cmd" | sed -E "s# build/$tgt# $P/build/$tgt#g; s# toolchain/# $P/toolchain/#g; s# -o [^ ]+# -o $out#; s# -MF [^ ]+##; s# -MD##")
    (cd $W && nice -n 10 bash -c "$cmd") > /tmp/memoria_cc.log 2>&1 && echo "$tgt $f ok ($(grep -c warning: /tmp/memoria_cc.log) avisos)" || { echo "$tgt $f FALHOU"; grep -m10 -E "error|warning" /tmp/memoria_cc.log; }
    grep -m5 "warning:" /tmp/memoria_cc.log
  done
done
