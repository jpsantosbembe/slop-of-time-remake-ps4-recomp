#!/bin/bash
# Frente N: liga o ocarina Linux com os objetos do principal, trocando os do runtime que a frente compilou
# (~/ocarina-ps4/memoria-obj/oot111/*.o). Saída: ~/ocarina-ps4/memoria-run/ocarina
P=~/ocarina-ps4/ext/ocarina-recomp-orbis; OBJ=~/ocarina-ps4/memoria-obj/oot111
cd $P || exit 1
ninja -f build/oot111/build.ninja -t query build/oot111/ocarina | awk '/^  input:/{f=1;next} /^  outputs:/{f=0} f&&/^    /{print $1}' | grep -v "^|" > /tmp/memoria.rsp; wc -l < /tmp/memoria.rsp
for o in $OBJ/*.o; do
  b=$(basename $o .o); rel=build/oot111/obj/runtime/$(echo $b | sed 's#_#/#; t; s#$##').o
  [ -f "build/oot111/obj/runtime/$b.o" ] && rel=build/oot111/obj/runtime/$b.o
  grep -q "$rel" /tmp/memoria.rsp || { echo "sem $rel no rsp"; exit 1; }
  sed -i "s#$rel#$o#" /tmp/memoria.rsp
done
nice -n 10 /usr/lib64/ccache/clang -fuse-ld=lld -no-pie -Wl,-z,noexecstack '-Wl,-rpath,$ORIGIN' @/tmp/memoria.rsp -lvulkan -lSDL3 -lopus -ldav1d -lvpx -lzstd -l:libstdc++.a -lm -lpthread -ldl -o ~/ocarina-ps4/memoria-run/ocarina.novo && mv ~/ocarina-ps4/memoria-run/ocarina.novo ~/ocarina-ps4/memoria-run/ocarina && echo "ligado $(date +%T)"
