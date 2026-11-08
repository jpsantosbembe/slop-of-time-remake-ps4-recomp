#!/bin/bash
# Fase 1C: bibliotecas do runtime compiladas para o PS4 (estáticas, -march=btver2), com o bundle orbis-sdk-v1.
#   zstd 1.5.7 (RomFS e cache), opus 1.5.2 (áudio), dav1d 1.5.1 (filmes AV1, com o asm x86 via nasm).
# Fontes em ~/ocarina-ps4/deps-src, instaladas em ~/ocarina-ps4/deps-ps4/{include,lib}. Log: ~/ocarina-ps4/deps-ps4.log
set -eo pipefail
R=~/ocarina-ps4; S=$R/sdk/orbis-sdk-v1; SRC=$R/deps-src; OUT=$R/deps-ps4
TC=$S/toolchain/orbis-sdk.cmake
mkdir -p "$SRC" "$OUT"
exec > "$R/deps-ps4.log" 2>&1
fetch() {   # url arquivo
  [ -f "$SRC/$2" ] || curl -sfL -o "$SRC/$2" "$1"
}
fetch https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz zstd-1.5.7.tar.gz
fetch https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz opus-1.5.2.tar.gz
fetch https://downloads.videolan.org/pub/videolan/dav1d/1.5.1/dav1d-1.5.1.tar.xz dav1d-1.5.1.tar.xz
cd "$SRC"
for t in zstd-1.5.7.tar.gz opus-1.5.2.tar.gz dav1d-1.5.1.tar.xz; do d=${t%.tar.*}; [ -d "$d" ] || tar -xf "$t"; done

echo "== zstd"
cmake -S zstd-1.5.7/build/cmake -B build-zstd -G Ninja -DCMAKE_TOOLCHAIN_FILE=$TC -DCMAKE_INSTALL_PREFIX=$OUT \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS="-march=btver2" -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_SHARED=OFF \
  -DZSTD_BUILD_STATIC=ON -DZSTD_MULTITHREAD_SUPPORT=OFF -DZSTD_BUILD_TESTS=OFF
nice -n 10 cmake --build build-zstd -j4
cmake --install build-zstd

echo "== opus"
cmake -S opus-1.5.2 -B build-opus -G Ninja -DCMAKE_TOOLCHAIN_FILE=$TC -DCMAKE_INSTALL_PREFIX=$OUT \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS="-march=btver2" -DOPUS_BUILD_SHARED_LIBRARY=OFF -DOPUS_BUILD_TESTING=OFF \
  -DOPUS_BUILD_PROGRAMS=OFF
nice -n 10 cmake --build build-opus -j4
cmake --install build-opus

echo "== dav1d"
. "$S/env.sh" >/dev/null
# o env.sh deixa o compilador como @ORBIS_CC@ etc.; preenche como o build.sh do mesa-ps4 faz
CROSS=$R/deps-src/orbis-cross.ini
sed -e "s|@ORBIS_CC@|clang|g" -e "s|@ORBIS_CXX@|clang++|g" -e "s|@ORBIS_AR@|llvm-ar|g" -e "s|@ORBIS_STRIP@|llvm-strip|g" \
    "$ORBIS_MESON_CROSS" > "$CROSS"
# os testes de link do meson usam só c_link_args, que não trazem os -isystem/-isysroot do SDK: copia os de c_args
python3 - "$CROSS" <<'PY'
import ast, re, sys
p = sys.argv[1]; s = open(p).read()
ca = ast.literal_eval(re.search(r"^c_args\s*=\s*(\[.*?\])\s*$", s, re.M | re.S).group(1))
inc = []
for i, a in enumerate(ca):
    if a in ('-isystem', '-isysroot', '-I', '-include'): inc += [a, ca[i + 1]]
    elif a.startswith(('-isystem', '-I', '-D')) and len(a) > 2 and a not in ('-isystem',): inc.append(a)
def add(m):
    lst = ast.literal_eval(m.group(2)); return m.group(1) + repr(lst + [x for x in inc if x not in lst])
s = re.sub(r"^(c_link_args\s*=\s*)(\[.*?\])", add, s, count=1, flags=re.M | re.S)
open(p, 'w').write(s)
PY
ORBIS_MESON_CROSS=$CROSS
# o has_function do meson monta um executável inteiro, e o link do SDK do PS4 não passa nesse teste; o clock_gettime
# existe (overlay orbis-compat), então o erro vira aviso
sed -i "s/error('clock_gettime not found')/message('clock_gettime: assumido no PS4 (overlay orbis-compat)')/" dav1d-1.5.1/meson.build
rm -rf build-dav1d
meson setup build-dav1d dav1d-1.5.1 --cross-file "$ORBIS_MESON_CROSS" --prefix "$OUT" --buildtype release \
  --default-library static -Denable_tools=false -Denable_tests=false -Denable_examples=false -Denable_asm=true \
  -Dc_args="-march=btver2" -Dc_std=c11 --wipe 2>/dev/null || \
meson setup build-dav1d dav1d-1.5.1 --cross-file "$ORBIS_MESON_CROSS" --prefix "$OUT" --buildtype release \
  --default-library static -Denable_tools=false -Denable_tests=false -Denable_examples=false -Denable_asm=true \
  -Dc_args="-march=btver2" -Dc_std=c11
# os has_function que falharam pelo mesmo motivo: o musl do SDK tem aligned_alloc (C11, sempre declarado)
sed -i -e "s/#define HAVE_ALIGNED_ALLOC 0/#define HAVE_ALIGNED_ALLOC 1/" \
    build-dav1d/config.h
nice -n 10 ninja -C build-dav1d -j4
ninja -C build-dav1d install

echo "== pronto"
ls -la "$OUT/lib"
echo "fim rc=0"
