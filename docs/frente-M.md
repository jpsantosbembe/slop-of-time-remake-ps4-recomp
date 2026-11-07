# Frente M: mesh shaders emulados em compute

Escrito em 2026-11-07. A GPU do PS4 (GCN GFX7 pelo mesa-ps4/RADV) não tem `VK_EXT_mesh_shader`, e o OoT do Switch 2
desenha parte da cena com task/mesh shaders do NVN2. Este documento descreve como o runtime converte cada desenho de
mesh em compute + desenho indexado comum.

## O que já existe e o que mostra o censo

O tradutor (patch `tools/xlate/patches/0019-switch2-mesh-stage.patch`) traduz o programa de mesh como um vertex
program cujo "stage buffer" (ISBE) é um array `Workgroup` de uint. A semântica de mesh vive só no epílogo gerado
(`GenerateMeshEntry`): zera o contador, roda o programa, copia o array compartilhado para as saídas `Output`
(`Position`, `Location n`, `PrimitiveTriangleIndicesEXT`), chama `OpSetMeshOutputsEXT` e, havendo task, lê o payload
de `TaskPayloadWorkgroupEXT`. O task faz o inverso: copia o payload e termina em `OpEmitMeshTasksEXT(count, 1, 1)`.
O corpo do programa já é, na prática, um compute.

Censo do cache de shaders do Linux (pack de 7,2 GB, 753 módulos de mesh/task):

- 736 mesh, 17 task. Todos com `LocalSize 32 1 1` (um task tem `LocalSize 1`) e `OutputTrianglesEXT`.
- (maxV, maxP): (64, 126) em 601; (128, 64) 48; (96, 64) 47; (96, 32) 23; (160, 128) 8; (96, 96) 6; (32, 32) 3.
- Builtins: só `LocalInvocationId`, `WorkgroupId`, `SubgroupLocalInvocationId`, `SubgroupLtMask`, `Position`,
  `PrimitiveTriangleIndicesEXT`. Nenhuma saída por primitiva, nada de `PrimitiveId`, `ViewportIndex`, `CullPrimitive`.
- Saídas genéricas: `Location 0..20`, todas `vec4 float`. Payload do task: até 1600 words.
- Subgrupo (ballot, shuffle, vote) em todos: com workgroup de 32 numa wave de 64 do GCN, as 32 lanes ativas veem os
  mesmos valores (o tradutor usa só a word baixa das máscaras).
- 22 módulos usam o set 2 (bindless).

## Precedentes

- Asahi (Apple AGX, sem hardware de geometry shader): GS e tesselação emulados em compute que escreve vértices e
  índices num buffer, seguido de um desenho comum que lê esse buffer. É exatamente o formato deste desenho.
- lavapipe: mesh/task implementados como variantes de compute dentro do llvmpipe.
- vkd3d-proton, RADV e MoltenVK não emulam: exigem mesh nativo (RADV só tem mesh em GFX10.3+ por NGG; o MoltenVK mapeia
  para o mesh do Metal 3). yuzu/Eden não têm o que emular (o Switch 1 não tem mesh).

## Abordagem: reescrita do SPIR-V no runtime, não no tradutor

Mudar o tradutor troca a identidade dele e invalida o cache inteiro (o PS4 não roda o tradutor, depende do pack). O
runtime já reescreve SPIR-V em C (`spirv_fixup`, `spirv_add_outputs`, `spirv_rebind`, `spirv_set`,
`spirv_alpha_test` em `vk_pipeline.c`). A conversão entra ali, num arquivo novo `runtime/vk_mesh_emu.c`:

`spirv_mesh_to_compute(w, n, kind)` (kind mesh ou task):

1. `OpEntryPoint MeshEXT/TaskEXT` vira `GLCompute`; saem `OpCapability MeshShadingEXT`, a extensão e os modos
   `OutputVertices`, `OutputPrimitivesEXT`, `OutputTrianglesEXT`. `LocalSize` fica.
2. As variáveis `Output` (Position, Locations, índices) e a `TaskPayloadWorkgroupEXT` saem, com suas decorações. Entram
   três SSBOs no set 3 (abaixo). Os `OpAccessChain` sobre as variáveis removidas viram acessos ao SSBO com o índice
   linear calculado (vértice, componente, slot do workgroup); o `OpStore` do uvec3 de índices vira três stores de uint
   já somados à base de vértices do workgroup.
3. `OpSetMeshOutputsEXT` some. No lugar, cada invocação zera as primitivas `inv + k*32` do seu slab (o slab tem
   `roundup(maxP, 32)` primitivas): as que o programa não escreve ficam degeneradas `{0,0,0}`. Mesma invocação, mesma
   ordem de programa que a escrita real, então sem corrida.
4. Mesh: prólogo calcula o slot do workgroup e sai cedo (uniforme no workgroup, antes de qualquer barreira) quando o
   slot passa da capacidade ou, com task, quando `WorkgroupId.x >= count[WorkgroupId.y]`. O programa só lê
   `WorkgroupId.x` (o `CtaId` do tradutor), então o grid (meshes, tasks) não muda o que ele vê.
5. Task: payload vai para `ctrl.payload[t * PW + i]`; `OpEmitMeshTasksEXT(n, ...)` vira `count[t] = n`,
   `atomicMax(ctrl.maxMeshes, n)` e `OpReturn`.

O mesh sem task que declara payload lê zeros (região zerada), como seria indefinido no Vulkan.

## Onde entra no runtime

- `vk_device.c`/`vk.h`: `mesh_emulate = !has_mesh_shader || OCARINA_MESH_EMULATE=1`, separado de `has_mesh_shader`.
- `vk_exec_draw.c`, `op_draw_mesh` e `op_draw_mesh_ind`: com `mesh_emulate`, chamam `mesh_emu_draw()` em vez de
  `rec_mesh*`. Sequência: resolve os estágios como hoje; `vk_exec_barrier` (fecha o passe); bind do pipeline de
  compute do mesh com os MESMOS descritores do estágio de mesh (`bind_resources` com o bind point de compute e o
  `mesh_task_draw` ligado, para os slots do estágio 4/3); dispatch; depois `prepare_draw` reabre o passe (a abertura já
  faz a barreira completa) com o pipeline gráfico de pull; `vkCmdBindIndexBuffer` + `vkCmdDrawIndexedIndirect`.
- `vk_pipeline.c`: um bit `mesh_emu` no fim da chave do pipeline (os campos legados não mudam). Com ele, o layout do
  mesh ganha o estágio COMPUTE nas bindings de mesh/task, VERTEX na do pull, e o set 3; os módulos de mesh/task passam
  por `spirv_mesh_to_compute` e viram dois pipelines de compute; o gráfico troca mesh/task pelo VS de pull, sem vertex
  input, topologia lista de triângulos, sem primitive restart.
- `runtime/shaders/mesh_pull.vert` (estático): lê o vértice `gl_VertexIndex` do SSBO, escreve `Position` (Invariant:
  passthrough bit a bit, o depth EQUAL da folhagem depende disso) e as Locations 0..20 com o default (0,0,0,1) nas que
  o mesh não escreve. Stride e offset por Location vêm do bloco de controle do desenho (sem push constants no layout).
- `runtime/shaders/mesh_scan.comp` (fase 2, task): prefix sum sobre `count[]`, escreve o dispatch indireto do mesh e o
  draw indireto.

## Formatos (set 3, três `STORAGE_BUFFER_DYNAMIC`, offsets por desenho)

- binding 0, vértices `float[]`: slot s, vértice v: `(s*maxV + v) * stride`, `stride = 4 * (1 + L)` floats (Position e
  depois as L Locations do módulo em ordem crescente).
- binding 1, índices `uint[]`: slot s tem `roundup(maxP,32) * 3` índices, valores globais `s*maxV + local`.
- binding 2, controle `uint[]`: `{VkDrawIndexedIndirectCommand (5), VkDispatchIndirectCommand (3), capacidade em slots,
  task count, maxMeshes, stride, loc_off[21], ...}`, depois `count[T]`, `offset[T]` e o payload `T*PW`.

Mesh sem task, direto: `vkCmdDispatch(N,1,1)`, slot = `WorkgroupId.x`. Indireto: o registro `{count,1,1}` que
`mesh_args` já monta vira o `VkDispatchIndirectCommand`; o workgroup 0 escreve `indexCount = min(NumWorkGroups.x, cap)
* roundup(maxP,32) * 3`. Determinístico e sem compactação: a ordem das primitivas é a do hardware.

## Limites

- maxV ≤ 256, maxP ≤ 256, L ≤ 21, LocalSize ≤ 128 (o censo fica bem abaixo). Fora disso o desenho é pulado com log.
- Memória: um ring device-local de 32 MiB por padrão (`OCARINA_MESH_EMU_MB`), partido por frame em voo. Por workgroup:
  64·16·(1+L) B de vértices (L=6: 7 KiB) + 128·12 B de índices. A capacidade em slots vai no controle; acima dela o
  workgroup não escreve e o desenho perde triângulos, com um log único. Nada de malloc do host além dos módulos.
- Custo no GCN: o VS de pull exporta todas as Locations que o FS lê; geometria degenerada no fim de cada slab.

## Fases e teste

1. Reescritor + `--probe-mesh-emu`: converte os 753 módulos extraídos e passa `spirv-val --target-env vulkan1.3`.
2. Mesh sem task (736/753) no Linux com `OCARINA_MESH_EMULATE=1`; comparar com o nativo no mesmo frame (`OOT_ONLY_MESH=1`,
   `vk_dump`).
3. Task (17/753): task compute, scan, mesh indireto em grid (maxMeshes, T).

Fora por enquanto: tirar os dispatches de dentro do passe (cada desenho de mesh fecha e reabre o passe), compactação de
primitivas, VS de pull gerado por módulo (menos exports).

## Arquivos tocados (para evitar conflito com A e D)

Novos: `runtime/vk_mesh_emu.c`, `runtime/shaders/mesh_pull.vert`, `runtime/shaders/mesh_scan.comp` e os `*_spv.h`.
Ganchos curtos: `op_draw_mesh`/`op_draw_mesh_ind` (`vk_exec_draw.c`), `create_graphics` e a chave (`vk_pipeline.c`),
`vk_device.c`/`vk.h` (flag). Não toca `vk_astc.c`, `vk_memory.c` nem os knobs do VMA (o ring usa `vmaCreateBuffer`).
