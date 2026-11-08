# Patches das dependências

As dependências do port que não são deste repositório, com as nossas mudanças.

## `mesa-ps4/`

Série de `git format-patch` sobre o `orbis-ports/mesa-ps4` (branch `orbis`, commit em `BASE`). A nossa branch de
trabalho no IdeaPad é `ocarina-gladius`. Para aplicar:

```
git -C mesa-ps4 checkout -b ocarina-gladius $(cat patches/mesa-ps4/BASE)
git -C mesa-ps4 am patches/mesa-ps4/*.patch
```

O que entra:
- o Gladius em modo Neo;
- o external memory host;
- o IA_MULTI_VGT_PARAM do Gladius, que corrige o travamento de GPU no desenho instanciado;
- a devolução do físico sob o GARLIC;
- a arena preguiçosa (`ORBIS_ARENA_LAZY`, `ORBIS_LAZY_RECLAIM`);
- o `ORBIS_BIG_NIR`;
- o `ORBIS_ARENA_VA`;
- o staging de envio de 8 MiB, com o envio grande dividido em vários DCBs.

## `orbis-compat/`

O orbis-compat do SDK (`orbis-sdk-v1/orbis-compat`) não tem git, então vão os arquivos inteiros que mudamos:
- **`src/orbis_mmap.cpp`:**
  - carve-outs numa faixa própria de endereço;
  - registro da pilha de pedidos anônimos de 64 MiB ou mais;
  - **devolução ao kernel de carve vazio**.
- **`src/orbis_expand_heap.c`:** o `__expand_heap` do musl com crescimento limitado a 8 MiB. O original dobra a cada duas expansões e chegava a pedir 256 e 512 MiB.
- **`orbis_mmap.desde-antes-da-faixa.diff`:** o diff do `orbis_mmap.cpp` desde antes da faixa de endereço.

O `.a` é montado recompilando só esses objetos com os flags do `build.sh` (`-DORBIS_MMAP_MAX_CARVES=32`) e trocando-os com `llvm-ar r` (`docs/frentes.md`).

## Runtime

O runtime (fork do `sostenesfreitas/ocarina-recomp`, GPL-3.0) não está aqui. Vai como branch `runtime/ps4-orbis` neste mesmo repositório do GitHub.
