#!/bin/bash
# Recompila o mesa-ps4 (RADV para o PS4) no IdeaPad, com o build.sh do próprio mesa-ps4.
# O build.sh pede o nix só para abrir um ambiente com as dependências do Mesa ("nix develop nixpkgs#mesa --command X");
# aqui um nix de mentira executa X direto, e um venv dá o que o Fedora não tem (mako, pyyaml, packaging).
# Toolchain, overlay e kit: os do bundle orbis-sdk-v1, os mesmos com que o Mesa do bundle foi feito.
set -euo pipefail
R=~/ocarina-ps4
S=$R/sdk/orbis-sdk-v1
V=$R/mesa-venv
SHIM=$R/nixshim
if [ ! -x "$V/bin/python3" ]; then
  python3 -m venv "$V"
  "$V/bin/pip" install -q mako pyyaml packaging
fi
mkdir -p "$SHIM"
cat > "$SHIM/nix" <<'EOF'
#!/bin/bash
# nix de mentira: "nix develop <x> --command CMD..." executa CMD...; o resto não é suportado
if [ "$1" = develop ]; then
  while [ $# -gt 0 ] && [ "$1" != --command ]; do shift; done
  shift
  exec "$@"
fi
echo "nixshim: só 'develop --command' é suportado ($*)" >&2
exit 1
EOF
chmod +x "$SHIM/nix"
export PATH="$SHIM:$V/bin:$PATH"
export ORBIS_COMPAT_DIR=$S/orbis-compat ORBIS_KIT_DIR=$R/ext/orbis-porting-kit
cd $R/ext/mesa-ps4
nice -n 10 bash build-support/orbis/build.sh --sdk "$S/sdk" --work "$R/mesa-work" 2>&1 | tail -40
ls -la build-orbis/src/amd/vulkan/libvulkan_radeon.a 2>/dev/null
