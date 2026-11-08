# Frentes paralelas do port (contrato comum)

Escrito em 2026-11-07. Cada frente é um agente com escopo próprio. O agente principal (a conversa com o João) integra,
monta o ELF do PS4 e é o ÚNICO que roda testes no console.

## Regras para todas as frentes

1. **Console (192.168.1.99): ninguém mexe além do principal.** Nada de subir ELF, trocar `/data/ocarina/args.txt`,
   mandar reload ou abrir app. Para testar no console, peça ao principal (`SendMessage` para `main`) com: branch e commit,
   o que mudar no `args.txt` e o que procurar no log. A leitura por FTP (`curl ftp://192.168.1.99:2121/...`) é livre.
2. **Código:** cada frente trabalha numa branch própria, numa worktree própria no IdeaPad:
   `git -C ~/ocarina-ps4/ext/ocarina-recomp worktree add ~/ocarina-ps4/ext/frente-<nome> -b frente/<nome> ps4-orbis`.
   Não edite `~/ocarina-ps4/ext/ocarina-recomp-orbis` (a worktree do principal) nem a branch `ps4-orbis`. O principal faz
   o merge da sua branch quando você disser que está pronta.
3. **Build:** IdeaPad (`ssh ideapad3i`) com poucos recursos: `nice -n 10` e no máximo `-j3`, um build pesado por frente.
   Um build PS4 ou Linux completo numa worktree nova recompila 1,2 GB de C gerado (demora); prefira compilar só os
   arquivos do runtime que você mudou, com os mesmos flags do `build.ninja` da worktree do principal
   (`~/ocarina-ps4/ext/ocarina-recomp-orbis/build/oot111-orbis/build.ninja` para PS4, `build/oot111/build.ninja` para
   Linux). Só faça um build completo se o seu teste exigir rodar o jogo no Linux.
4. **Nada derivado do jogo no repositório** (C gerado, ExeFS, RomFS, cache de shaders, arte, logs com endereços do jogo).
   Nada específico do jogo fixo no código: dados do jogo vêm de `~/ocarina-ps4/game_info.py` ou de `jogos/<title id>/`.
5. **Commits:** em português, sem `Co-Authored-By`. Sem push para o GitHub (só o principal, com o OK do João).
6. **Comunicação:** precisando de algo de outra frente ou de uma camada acima, mande `SendMessage` para `main` com a
   pergunta; o principal repassa ou responde. Registre decisões de interface aqui, na seção da sua frente.
7. **Entrega:** commits na sua branch e um resumo curto: o que mudou, como testar, riscos e o que falta.

## Estado comum (o que todas sabem)

- Runtime: `runtime/platform/orbis.c` (camada do PS4), `runtime/vk_*.c` (Vulkan), `runtime/gfx_shader.c` (tradutor e
  cache). No PS4: commit preguiçoso da memória do jogo, coringa da GPU nos pools importados, pilhas preguiçosas numa
  área fixa, notificações na tela, monitor de memória ("orbis mem" no `totk.log`), vigia de GPU travada (breadcrumbs).
- GPU do PS4 Pro: GCN GFX7 (Liverpool/Gladius) pelo mesa-ps4 (RADV), Vulkan 1.3, sem `VK_EXT_mesh_shader`,
  subgrupo 64, sem ASTC nativo.
- Memória: 5120 MiB de memória direta no total. No último boot, o jogo tinha 2,48 GB mapeados, o VMA ~570 MB, o malloc
  do runtime e do Mesa ~800 MB, as pilhas 55 MB; acabou no compilador de shaders do Mesa (malloc NULL).
- Logs do console: `ftp://192.168.1.99:2121/data/ocarina/logs/totk.log` e `stderr.log` (depois da migração da frente G:
  `/data/slop/0400486024dec000/logs/`); klog em
  `/Users/joaobembe/Workspace/ocarina-ps4/out/teste1/klog5.txt` (Mac).

## Frentes

### M — Mesh shaders emulados
Dono: `runtime/vk_*` (caminho de desenho de mesh/task) e `runtime/shaders/` (compute de emulação). Entrega: quando o
device não tem `VK_EXT_mesh_shader`, um desenho de mesh (com task opcional) vira compute (task+mesh escrevendo
vértices e índices num buffer) + desenho indexado comum, com o mesmo resultado. Testável no Linux forçando o caminho
emulado (ex.: `OCARINA_MESH_EMULATE=1`) mesmo com a NVIDIA tendo mesh nativo.

### A — ASTC na GPU sem travar o PS4
Dono: `runtime/vk_astc.c`, `runtime/shaders/astc*` e `bc3_encode.comp`. Entrega: o decodificador de ASTC em compute e
o codificador BC3 rodando no GCN GFX7 sem travar a GPU (hipóteses, correção e um teste isolado pelo modo
`--probe-astc` ou um novo, para o principal rodar no console).

Estado (branch `frente/astc`, commits 3e91ee4 e b0b8a37): os shaders compilam limpos para GFX7 no ACO (RADV null
device, `RADV_FORCE_FAMILY=hawaii`: scratch 0, sem spill, LDS 2 KiB, sem 64 bits, sem subgrupo, sem imagem de
storage). Interfaces novas: `TOTK_ASTC_BAND` (texels por despacho, padrão 1048576; 0 = fatia inteira) com um
`VK_CRUMB_ASTC` por faixa; `TOTK_ASTC_TRACE=1` (cada faixa no log com o número do crumb); `--probe-astc-gpu` (etapas
com linha de início e fim em `/data/ocarina/probe-astc.txt`); `TOTK_CRUMBS=draw` (crumb `VK_CRUMB_DRAW` antes de cada
draw, dispatch e mesh) com `TOTK_CRUMBS_LOG=1`; o vigia também dispara com alcançado == terminado quando a CPU já
gravou crumbs depois deles.

### D — Dieta de memória (análise e cortes do lado do host)
Dono: análise; mudanças pequenas em knobs (`runtime/vk_memory.c`, tamanhos de bloco do VMA, pools de staging,
caches do pipeline). Entrega: um mapa de quem gasta a memória do host (malloc do runtime e do Mesa, VMA, cache de
pipeline, cópias de texturas) medido no Linux com as mesmas opções do PS4, e cortes com o ganho estimado.

Decisões (frente/dieta, 8cdfcb2): knobs `OCARINA_VMA_BLOCK_MIB` (PS4 64), `OCARINA_STG_MIB`/`OCARINA_STG_MIN` (PS4
32 x 1), `OCARINA_PIPE_WORKERS` (PS4 1), `OCARINA_PREWARM_THREADS` (PS4 2), `OCARINA_AV1_THREADS` (PS4 2); o
`DKB_SKIP_CS` pula antes do `shader_for` (as origens do filme 54b0/3f6f seguem o caminho antigo). Medidores em
`tools/memprof/` (malloc_census.so, mc_report.py, pagemap_windows.py, smaps_sum.py, medir_linux.sh).

### P — Desempenho na CPU Jaguar
Dono: análise; flags de compilação (`tools/build_game.py`, alvo orbis) e caminhos quentes do runtime. Entrega: perfil no
Linux (perf) dos caminhos quentes, com os ganhos esperados no Jaguar (8 núcleos de 1,6 a 2,1 GHz, sem AVX2), e
propostas medidas.

### G — Pasta de dados derivada do jogo
Dono: `ORBIS_DATA_DIR` em `runtime/platform/orbis.c`, `loader/loader.c` (cópia no Mac,
`/Users/joaobembe/Workspace/ocarina-ps4/loader/`) e os scripts `tools/*.sh` e `tools/*.py` do Mac que usam `/data/ocarina`.
Entrega: a pasta passa a vir do jogo (por exemplo `/data/slop/<title id>/`), com migração da pasta atual descrita, sem
quebrar o fluxo de dev. Só depois de combinar a mudança do carregador com o principal (reinstalar o carregador exige o
João).
Decisão (2026-11-07, `docs/frente-G.md`): pasta `/data/slop/<title id do Switch>/`, ELF no console `game.elf`; o
carregador recebe a pasta na compilação (`game_info.py --campo ps4_data_dir`) e passa o caminho do ELF no `argv[0]`;
o runtime usa `dirname(argv[0])` ou, sem carregador, `rt_orbis_data_dir` (do `main.npdm`, gerado pelo `build_game.py`).

### R — Repositório e empacotamento
Dono: o repositório `slop-of-time-remake-ps4-recomp` (GitHub, privado por enquanto) e a documentação de uso. Entrega:
estrutura do repositório conforme a auditoria (`/private/tmp/claude-501/-Users-joaobembe-Workspace/6a6e6fe1-cf22-4ae3-b13a-126a7f92243d/scratchpad/licencas.md`):
licença GPL-3.0-or-later, NOTICE/créditos, `.gitignore` que barra tudo derivado do jogo, README com o aviso "100% feito
por IA" e de não afiliação, e o passo a passo de build a partir da cópia do jogo do usuário. Prepara tudo localmente
e cria o repositório PRIVADO, sem push de código.

### S — Perfil por amostragem no PS4
Dono: `runtime/platform/prof_orbis.c`, o gancho `prof_sampler_thread` (prof.h/perf.c), `orbis_lazy_stack_span`
(orbis.c), `tools/prof_report.py` (leitura do ELF do PS4) e `tools/prof_ps4.sh` (Mac). Branch `frente/sampler`.
Mecanismo: thread "prof" manda SIGPROF (27, FreeBSD; `TOTK_PROFILE_SIG` troca) por `pthread_kill` a cada 1 ms às
threads main/render/replay; o tratador lê rip/rsp do ucontext e copia até 8 KiB de pilha mapeada; a cadeia sai dos
endereços de retorno conferidos contra a instrução de chamada. Uso: `TOTK_PROFILE=prof.txt` (vai para
`<pasta de dados>/logs/`), `TOTK_PROFILE_SLOW=<ms>` (um `prof.txt.f<quadro>` por quadro lento, no máximo 64),
`TOTK_PROFILE_MAIN=1`/`TOTK_PROFILE_RENDER=1` restringem; `bash tools/prof_ps4.sh` baixa e gera o relatório.

### L — Reabrir o app pela rede (já em andamento)
`tools/ps4_lancar.py` via ps4debug.

### H — GPU travada no desenho instanciado (concluída)
Causa: o mesa-ps4 declara o PS4 Pro em modo Neo como CHIP_GLADIUS (4 SE), mas as regras do Hawaii
(WD_SWITCH_ON_EOP com instâncias, PARTIAL_VS_WAVE_ON) só valiam para CHIP_HAWAII, e o IA_MULTI_VGT_PARAM ia só para o
context reg 0x28AA8. Correção: mesa-ps4 `ocarina-gladius` 2b683c6 (regras do Hawaii no Gladius + uconfig R_030960),
`ORBIS_NEO_VGT=0|1|2` (0 = sem a correção). Contraprova em 2026-11-07: com 0 trava no quadro ~1550; com 1 roda.
`ORBIS_NEO_GLADIUS=0` (Liverpool) não resolve.

### N — Memória na entrada do jogo (em andamento)
No "New Game" o jogo vai de ~1,2 GB a 3,2 GB tocados e a memória direta acaba. Mede no Linux e corta.
Decisões (branch `frente/memoria`, 69a779a .. 9981517):
- Medido no Linux (janelas de 2 MiB escritas): título 2200 MiB; New Game 5084 MiB, ~4,2 GB de dado não zero. Janela de
  64 KiB economiza 752 MiB e janelas zeradas ~130 MiB; nenhum dos dois faz caber. CPU quente: 70 MiB/95 s no filme,
  ~840 MiB com o mundo rodando.
- `OCARINA_HEAPDEF=<heap>=<MiB>,...` reescreve a HeapDef do jogo, achada no RomFS. O TextureHeap é só teto.
  VirtualAddressMemory abaixo do padrão aborta no nnMain.
- Paginador no `orbis.c`: janelas frias vão comprimidas para `<pasta de dados>/swap.bin` e voltam no toque ou no
  prefault. `OCARINA_SWAP=0` desliga; `OCARINA_GUEST_MIB` (padrão 2560) é o orçamento. `OCARINA_WIN_KEEP=1` e
  `OCARINA_DMEM_HIGH=0` desligam a devolução e a alocação no alto. `OCARINA_WW_PAGE=1` volta à vigia por página.
  `OCARINA_MEM_INVENTORY=0` desliga o inventário.
- Ferramentas em `tools/memprof/`: memwin.py, gran.py, hotset.py, compress_sample.py, gdb_pools.gdb, medir_memoria.sh etc.

### S — Perfil por amostragem no PS4 (em andamento)
Equivalente ao `prof_win.c` (TOTK_PROFILE_SLOW) para achar os quadros de ~750 ms a cada 32 na tela de título (não é o
dav1d: 6 threads não mudaram nada).

### V — Memória dentro do driver de vídeo (mesa-ps4)
Branch `frente/v` do mesa-ps4, worktree `~/ocarina-ps4/ext/mesa-frente-v` (build `build_mesa_v.sh`, -j3). Knobs:
- `ORBIS_GARLIC_RELEASE` (55d44ea, ligado; =0 caminho antigo): o físico da arena por baixo de cada BO em GARLIC volta ao
  sistema (consulta por VirtualQuery, nunca a aritmética); no put_back volta ao mesmo físico, ou a outro, ou o GARLIC
  fica de lastro. Provado no console: "sem dono" 0, 305 MiB devolvidos no título.
- `ORBIS_ARENA_LAZY=1` (25cb6d4, desligado): a arena vira reserva de endereço; físico em pedaços de 2 MiB no 1º map.
  Com ASTC=gpu o boot travou (hipótese: o CP lê endereço sem mapa e trava). `ORBIS_ARENA_LAZY=2` (d45c118): a janela
  fica mapeada PROT_NONE sobre um ralo de `ORBIS_ARENA_SINK_MIB` (32), como na arena antiga. Com o lazy, ORBIS_ARENA_MIB
  pode subir (2048) sem custo físico; a 1 GiB o VMA esgotou o VA (764 local).
- `ORBIS_BIG_NIR=<instr>` (4dfa5d1, padrão 100000; 0 desliga): vars_to_ssa antes do nir_opt_copy_prop_vars nos
  compute gigantes (o pico que derrubou o console com OOT_BIG_CS_OPT=1).
- d449f3c: linha "radv: orbis: chaves do cache" (pipelineCacheUUID e device cache_hash) e `ORBIS_HOST_NEO=1` no build
  host-orbis (Gladius no Linux), base para gerar o pipeline-cache.bin fora do console. c15409e: o valor do ORBIS_BIG_NIR
  entra no UUID do cache (o mesa-cache do console não contamina o A/B).
- Medidor: `~/ocarina-ps4/frente-v-aco/cscomp` (um compute SPIR-V, pico de RSS e tempo) e os SPIR-V gigantes em `spv/`.
  Sem otimizador o ee7764a6 sai com 2,28 MB de código e o RADV não aloca (crash); o pico do ACO (~1 GiB) vem do
  ajudante de memória global do tradutor inlinado até 243 vezes.

### T — Tradutor de shaders (compute gigantes)
Branch `frente/xlate` (worktree `~/ocarina-ps4/ext/frente-xlate`, de `ps4-orbis`), commit 8d757c6. Decisões:
- Patch 0039 (array de storage buffers guardados; sozinho não muda nenhuma tradução) + 0040 (laço): programas NVN 2
  compute com 64 ou mais acessos globais testam a lista de buffers alcançáveis (c0 + 0x900, 32 entradas) num laço
  DontUnroll sobre um binding `cp_dN[32]`, em vez de 32 candidatos desenrolados por ajudante. Mesma ordem, primeiro em
  range vence. `build_xlate.py --no-guarded-loop` deixa o 0040 de fora (corpus byte a byte igual ao tradutor antigo).
- Interface: entrada de `sbufs` com `"array": 32, "cb_stride": 4` e `storage_candidates_version` 2 no `.res.json`
  desses shaders (runtime antigo recusa). `GfxShaderRes.stride` (uint8, no padding); `bind_resources` liga o elemento
  k como o candidato de `cb_offset + k * stride`.
- RADV host (hawaii, noopt), pico: c7f13b5d 1415 → 135 MiB, 4b03b304 1385 → 134, 5a1f77c2 1497 → 162, a54f19e9 527 →
  247, ee7764a6 crash (~1,49 GiB) → 643. O ee7764a6 segue grande: candidatos explícitos de c5 (fase 2, contrato novo).
- Ferramentas no IdeaPad: `~/ocarina-ps4/xlate-cap/` (captura de pedidos `jobs/`, `tools/traduz.py`, `mede.sh`,
  `compara.py`, `jogar.sh`).

### O — Orçamento de memória de vídeo
Branch `frente/orcamento` (worktree `~/ocarina-ps4/ext/frente-orcamento`). ccc63d1 entrou em ps4-orbis (c73ec62);
5999776 por cima. Knobs:
- `OCARINA_VRAM_CENSUS=<quadros>` (`_MIB` passo de crescimento, padrão 64, 0 = sempre; `_TOP` padrão 40): imagens por
  classe (astc, rt, depth, tex; vivas e soltas), as maiores, VMA local em blocos x alocações, pools físicos/espelhos.
- `OCARINA_TEX_MIP_DROP=<n>` + `_MIB` (padrão 16): ASTC preenchida pela CPU acima do limite nasce sem os n níveis
  maiores (`VkTexImage.lod_drop`, `vk_tex_level` mapeia em views, alvos, cópias, Read/WriteTexels).
  `OCARINA_TEX_OOM_DROP=<n>`: sem memória, tenta de novo com mais níveis a menos.
- `OCARINA_DEPTH_SCALE=<2|4>` + `_MIN` (padrão 8192): profundidade grande do jogo em 1/escala; o passe divide viewport,
  scissor e o viewport do support buffer (`VkExec.pass_div`). Passe com escalas misturadas desenha em escala cheia (log).
- `OCARINA_VRAM_BUDGET_MIB`: orçamento da política `TOTK_TEX_VRAM_PRESSURE` contra os blocos locais do VMA;
  `OCARINA_TEX_PRESSURE_RT_AGE=<submits>`: sob pressão, soltas escritas pela GPU também saem.
- Linux, mesmo ponto do mundo (~690 texturas ASTC): texturas 2606 → 1504 MiB (ASTC 1279 → 452; soltas 133 → 1).
  Falta: alvos D32S8 ~1900² das sombras locais que ficam vivos (sem Finalize, ~14 MiB cada) e 4096² R32F nunca escrito.
- Ferramentas no IdeaPad: `~/ocarina-ps4/orcamento-tools/` (`medir_vram.sh`, `orc_cc.sh`, `orc_link.sh`, `rf_get.py`
  extrai do RomFS, `sarc.py`).

### Notas do principal (2026-11-07)
- Pasta de dados migrada para `/data/slop/0400486024dec000` (frente G, merge bb5178a; carregador novo instalado).
- Upscaler do filme `3f6f2b1d671badcd` fora do `DKB_SKIP_CS` no PS4: tela preta e ~400 ms de GPU por quadro (2 fps);
  segue pulado (o blit no lugar dele serrilha o logo: o jogo desenha em 1706x960).
- Cache solto: runtime b8cdd5b lê a tradução sincronizada pela chave do pedido (`sync_missing.sh`).
- Picos de ~750 ms na tela de título: a thread do filme (movie::, prioridade 45, núcleo 5) segurava o núcleo virtual
  durante a decodificação AV1 no host (TOTK_CORE_DIAG: espera "host" de 104 ms/quadro). Correção e66cd35 (ov_codec solta o
  núcleo): título de 26 para 57 fps, 1% low de 3,9 para 43 fps. A preempção assíncrona por sinal (frente S,
  `frente/preempt` bfaeaef) fica guardada: a classe "game" é ~1 ms/quadro.
- Vídeo claro demais: o blit do filme UNORM -> janela SRGB aplicava a curva duas vezes; agora passa por uma imagem SRGB.

### Notas do principal (2026-11-08)
- Os cortes da frente O resolveram a falta de VRAM na entrada do mundo. No run lz6 o VMA chegou a 2226 MiB; no o1 ficou em 545 MiB, sem nenhuma "image creation failed".
- O kernel limita o total de memória direta MAPEADA. A arena preguiçosa (`ORBIS_ARENA_LAZY=2`, janela coberta por visões do ralo) disputa esse limite com os carve-outs do malloc: com arena 2560 cabem 8 carves e o 9º dá EINVAL no map, mesmo FIXO na faixa própria; com arena 1536 o 9º entra.
- O mesa-ps4 é compilado com `radv-build-id` fixo, então o pipelineCacheUUID e a chave do mesa-cache não mudam quando o mesa é recompilado. A execução lia do `mesa-cache` um objeto de pipeline de outro build: `shaders[0]` lixo, #GP no `lock incl` de `radv_compute_pipeline_cache_search` (aparece como bus error com o endereço igual ao PC).
  - Correção no runtime: bd8033b e 51741b4. O `build_game.py` gera `mesa_id.c` com o SHA-256 do `libvulkan_radeon.a`; o mesa-cache vai para `mesa-cache/<id>` e o cache de pipelines para `pipeline-cache-<id>.bin`.
  - As pastas e arquivos de builds antigos ficam no console (`mesa-cache.o4`, `pipeline-cache.bin*`); apagar à mão.
- Teto físico: o run o6 (arena 2048) morreu com device lost e só 20 MiB de memória direta livre, dos 5120. O uso era VMA 1702, carves 1408, guest 1432 e pilhas 59.
- PENDENTE (estrutural, não começar agora): os carve-outs do malloc nunca voltam. O pico transitório do ACO num compute gigante (hash de 128 a 256 MiB no spill) vira heap permanente, e cada shader gigante novo no gameplay pode custar mais um carve. Saídas possíveis:
  - compilar os gigantes antes, no host, e levar o mesa-cache pronto (UUID agora por build);
  - um carve-out que volte para o kernel quando esvazia;
  - limitar o spill do ACO.

### E — Configurações da engine no RomFS (concluída, 2026-11-08)
- Não há perfil de qualidade pronto. A engine só separa `Handheld` e `Console`, e o que muda entre eles pesa em GPU (resolução, resolução dinâmica, LOD de foliage, névoa), não em memória.
- **Sombra estática off: −128 MiB de VRAM** (medido no Linux). São `CreateStaticDepthShadow=false` em `gsys/ModelSceneConfigParam/Main` e `IsUseStaticDepthShadow=false` em `agl/ShadowPrePassParam/MainField`.
  - Receita `sombra-estatica-off` em `jogos/<tid>/romfs-mods.json`. O `tools/romfs_mods.py` gera o overlay a partir do RomFS do usuário, e o runtime o carrega por `TOTK_ROMFS_MODS=<pasta>`.
  - Variante `sombra-estatica-2048`, não medida.
- O atlas D16 12288² (288 MiB) não vem de parâmetro do RomFS. Próximo passo é no runtime: logar quem cria uma textura com lado ≥ 8192.
- Ainda a medir:
  - o pool de partículas (`ParticleConfigParam` no Bootup, 88+50 MiB?);
  - o heap `Movie` (206 MiB) via `OCARINA_HEAPDEF`;
  - as cascatas de sombra, o SSAO e o foliage (GPU).
- Relatório completo no IdeaPad: `~/ocarina-ps4/engine-cfg/RELATORIO.md`. As medições do `medir_vram.sh` apagam `orcamento-run/perfil/save*`.

### Notas do principal (2026-11-08, tarde)
- Deku Tree por save (docs/saves.md): a carga leva ~3 min, sem a abertura.
- Device lost na entrada da Deku Tree: uma submissão achatada passava de 2 MiB, e o orbis-drm a recusava a cada quadro. Mesa d06e369 muda o staging para 8 slots de 8 MiB.
- Filme a ~20 imagens/s (AV1 1080p60) com o padrão de 2 threads do dav1d no PS4: testar `OCARINA_AV1_THREADS=6`.
- `OCARINA_SHOT_EVERY=300` congela ~250 ms a cada 5 s; usar 1200 ou desligar.
- Pedidos anônimos de 256 e 512 MiB esgotaram os carves uma vez (o11) e não se repetiram. O orbis-compat agora loga a pilha de todo mmap ≥ 64 MiB (`.a` com backup `.faixa-antes-pilha`).
- Devolução de carves (orbis-compat, `src/orbis_mmap.cpp`; o orbis-compat não tem git). Backups: `.cpp.antes-devolver` e `build/liborbis-compat.a.pilha-antes-devolver`.
  - Um carve (fora o 0) que esvazia por completo volta ao kernel, com `sceKernelMunmap` e `ReleaseDirectMemory`. O endereço dele é reaproveitado pelo próximo carve que caiba.
  - O `.a` é recompilado só com `orbis_mmap.o`, com `-DORBIS_MMAP_MAX_CARVES=32`, e trocado com `llvm-ar r`.
- O malloc logando o chamador (runtime f8c3d82) mostrou que os pedidos grandes vêm do ACO, que compila shaders gigantes, e do `operator new`, nunca do `vk_astc`. A varredura de pilha do orbis-compat apontava lixo.
- Frente V2 (mesa 55500a5, já em `ocarina-gladius`):
  - O `ORBIS_LAZY_RECLAIM` devolve ao ralo os pedaços de 2 MiB da arena que ficam sem VA vivo. Há também uma linha `CENSO`.
  - O "com físico" da arena era o pico de VA tocado, e o VMA do relatório estava velho: na Deku, a GPU passava de ~1,6 GiB.
  - Ainda em aberto:
    - uma válvula de dreno da fila de retiro;
    - o VMA começar com blocos de 64 MiB;
    - o `vk_vma_usage` no relatório periódico.
- o15: o FATAL ocorreu na thread `big compute` e o processo seguiu vivo, entrando na dungeon (com música) com a tela preta. Os shaders gigantes nunca ficaram prontos, a GPU caiu para 0,1 fps e a memória direta livre ficou em 4 MiB.

### Notas do principal (2026-11-08, noite)
- **Lentidão na dungeon resolvida.** Com `OCARINA_GUEST_MIB=2048`, o paginador despejava buffers que o replay lê a cada quadro: o profiler mostrou `orbis_mem_prefault < vk_mem_resolve` e quadros de até 47 s. Com 2560 a Deku Tree foi para 8,6 fps estáveis.
  - Falta proteger da troca as janelas que o replay ou a GPU usaram há pouco.
- **Os pedidos de 256 e 512 MiB que esgotavam a memória vinham do `__expand_heap` do musl do SDK.** Sem brk, cada expansão do heap é um mmap de 16 KiB << (mmap_step/2), e o tamanho dobra a cada duas expansões.
  - Correção: `orbis-compat/src/orbis_expand_heap.c`, que substitui o original e limita o crescimento a `ORBIS_HEAP_STEP_MAX` = 8 MiB. Backup do .a: `liborbis-compat.a.antes-expand-heap`.
- **Modo de preparo (frente C).** A primeira rodada no console deixou 30 shaders gigantes prontos, 22 deles já no cache (prova de que a chave bate). Com `OCARINA_PRECOMP_MAX_WORDS=500000` foram 36. Ficaram de fora 6, de 737 mil a 7,2 milhões de palavras, que a Deku Tree não usa.
- **Cena 3D preta na Deku Tree:** o HUD aparece e a GPU gasta 54 ms por quadro. O próximo passo é um `TOTK_DUMP=5000` (com PNG por passe e `images.txt`) e o mesmo save no Linux (frente do OpenCode).
- **OpenCode** (DeepSeek V4.1 Flash, via OpenCode Go):
  - o mapa de esperas CPU/GPU está em `~/ocarina-ps4/opencode/sync-mapa.md`;
  - o filme com frame threading está em `frente/filme` 1010b9f (`OCARINA_AV1_FRAME_DELAY`, ainda não testado no console).

### Notas do principal (2026-11-08, fim do dia)
- **A Deku Tree aparece no PS4**, a ~8 fps, carregada pelo save `deku-entrada`.
  - A tela preta vinha de memória de imagem não zerada: lixo virava NaN nos volumes temporais da névoa. O `TOTK_FIX_ZERO` agora é padrão no PS4 (runtime d16ac7d).
  - Com o mesmo save, o Linux renderiza a dungeon completa (frente OpenCode `linux-deku`).
- **Blocos pretos remanescentes.** O compute da névoa (`ee7764a6b1ef459c`, 115 mil palavras) gera NaN em ~16% dos froxels só na GCN (despejo o31). Pular esse compute (o32) NÃO tirou os blocos, então há outra fonte. Ele custa ~28 ms de GPU por quadro.
  - `OCARINA_NAN_MINMAX_CS`, que troca FMin/FMax/FClamp por NMin/NMax/NClamp nos compute, piorou: a cena voltou a ficar toda preta (o33). Ficou desligado por padrão (43bd10a).
  - O próximo passo é fazer a troca só onde o NaN nasce, ou guardar com OpIsNan. O relatório do OpenCode está em `~/ocarina-ps4/opencode/nan-nevoa/RELATORIO.md` (as 14 guardas FMax(x,-1022) antes de Exp2).
- `ORBIS_BIG_NIR=0` esgota os 4 GiB de malloc no `nir_opt_copy_prop_vars` mesmo no modo de preparo: inviável no console.
- **Upstream** (`sostenesfreitas/ocarina-recomp`, a nossa base, GPL-3.0): está 25 commits à frente (v0.2.7, com #119 amd-spirv-fixes e 8 dos 10 compute pulados de volta). O merge está em andamento na frente OpenCode `frente/upstream-027`, na worktree `~/ocarina-ps4/ext/frente-upstream`.
- **Melhor configuração no console:** `out/args-melhor-2026-11-08.txt` (fora do repositório; é o o30).
