# Plano: Ocarina of Time (Switch 2) nativo no PS4 Pro

Escrito em 2026-11-06. É um projeto de pesquisa de longo prazo, e o plano muda conforme os testes.

## A ideia

O mesmo caminho do nxnative: partir de uma recompilação de Switch 1 que já funciona e adaptar até o Switch 2. Desta vez, porém, o código do jogo é **traduzido de ARM64 para C antes de rodar** e compilado para a CPU x86 do PS4. O Mac executa o ARM64 direto; o PS4 não consegue, por isso a tradução.

## As bases (pesquisa de 2026-11-06)

| Projeto | O que é | Papel aqui |
|---|---|---|
| [valentderah/totk-pc-recomp](https://github.com/valentderah/totk-pc-recomp) (MIT, cerca de 5,4 mil estrelas) | Recompilação estática do *Tears of the Kingdom* (Switch 1) para Windows x86-64 com Vulkan: o lifter ARM64 → C vem do LAS Recomp, e o host Horizon vem do MacNativeGames, os dois MIT | **Base principal.** O motor do OoT de Switch 2 é da mesma família do TotK (sead, agl, gsys) |
| [Lucas-blip-png/dkb-pc-recomp](https://github.com/Lucas-blip-png/dkb-pc-recomp) (GPL-3.0) | A mesma base adaptada para um jogo de **Switch 2**, o *Donkey Kong Bananza*: NVN 2 com 700 entradas, shaders Ampere → Maxwell → SPIR-V, mesh shaders. Mostra a cena 3D, mas ainda não é jogável | **O mapa da adaptação Switch 1 → Switch 2.** É a única recompilação estática pública de jogo de Switch 2 |
| [Muhammad1259-sigma/totk-recomp](https://github.com/Muhammad1259-sigma/totk-recomp) (MIT) | Outra recompilação do TotK, para Linux x86-64 e Steam Deck, a cerca de 60 fps | Referência para Linux, que é o parente mais próximo do sistema do PS4 (FreeBSD) |
| [scarletnx/ScarletNX](https://github.com/scarletnx/ScarletNX) (GPL-3.0) | Recompilador estático de ARM64 para x86 via LLVM, usado por Bayonetta 3 (B3-Express) e Splatoon 3 (Inkypaint) | Plano B para a CPU, se o lifter em C não servir |
| [orbis-ports](https://github.com/orbis-ports) (`mesa-ps4`, `orbis-compat`, SDL2) | **Vulkan no PS4** (o driver RADV do Mesa, com suporte a Liverpool e Gladius, isto é, PS4 e PS4 Pro), mais os shims de libc e pthread. O OpenGothic roda nele a 45–60 fps | **A camada gráfica no console.** O RADV traz o compilador de shaders para o console, o que resolve a falta de compilador no firmware |
| nxnative, branch `oot-upstream` (nosso, MIT) | O OoT já roda no Mac: perfil, HLE de serviços do SDK 22, NDVE, mod `video`, tudo o que se sabe do jogo | **O conhecimento do jogo.** Código MIT pode vir para cá; o caminho contrário é proibido |

Licença: a base é MIT. Se trouxermos código do dkb-pc-recomp ou do ScarletNX, o projeto inteiro passa a ser GPL-3.0, o que é aceitável por ser um projeto pessoal e privado. O nxnative continua em clean room: nada daqui volta para lá.

## A arquitetura no PS4

```
dump do OoT (main, sdk, subsdk0 em ARM64)
  → lifter → C → clang do OpenOrbis (-march=btver2, a CPU Jaguar, sem AVX2) → eboot do PS4
runtime:
  host Horizon (threads, memória, arquivos, áudio, controle) → runtime/platform/orbis.c (libkernel do PS4)
  NVN 2 → Vulkan → mesa-ps4 (RADV na GPU Gladius do PS4 Pro)
  shaders: Ampere → Maxwell → SPIR-V → compilador do RADV no console, com cache
```

## Por que pode dar certo, e onde mora o risco

- **GPU, provavelmente OK.** O PS4 Pro tem 4,2 TFLOPS, contra uns 3 do Switch 2 no dock. O Ampere rende mais por TFLOP, mas renderizar a 720p–900p em vez de 1152p dá margem. O God of War e o Shadow of the Colossus mostram o que essa GPU entrega com código nativo.
- **CPU, apertada.** O Switch 2 tem 8 núcleos A78C a cerca de 1 GHz; o PS4 Pro, 8 núcleos Jaguar a 2,13 GHz. Por núcleo, ficam mais ou menos empatados. Só que o código traduzido roda mais devagar que o original, então vai ser preciso otimizar o lifter e os trechos quentes.
- **Memória, a parede principal.** O PS4 Pro dá uns 5,5 GB para o jogo (o número exato será medido). O OoT pede um heap de 6,2 GiB na partida. No Mac, o 3D ocupa 8,2–8,5 GB, com duplicações que o PS4, de memória unificada como o Switch, não precisa ter. Para caber, as alavancas são:
  - resolução interna menor;
  - texturas com o mip maior removido no romfs, o que corta cerca de 75% de cada uma;
  - os tamanhos de heap do jogo ajustados por patch;
  - nada de cópias duplicadas entre CPU e GPU (`VK_EXT_external_memory_host`, como já faz o TotK Recomp).

## Dois consoles: PS4 e PS4 Pro

O projeto atende os dois, e quem usar escolhe o PKG do console que tem (pedido do João, 2026-11-06):

| Alvo | PKG | Como roda | Orçamento |
|---|---|---|---|
| **PS4** (base, Slim, e o Pro em modo base) | `ATTRIBUTE` 0 | GPU de 18 CUs | 720p/30 como meta |
| **PS4 Pro** | bit NEO (`0x00800000`) no `ATTRIBUTE` | GPU de 36 CUs, saída 4K possível | 900p–1080p/30, ou 720p/60 |

- O mesmo código serve os dois. O que muda é o PKG, mais uma tabela de qualidade por alvo: resolução interna, mips de textura removidos e sombras.
- No Pro do João dá para testar os dois, já que o modo base do Pro se comporta como um PS4 comum.
- `tools/make_pkg.sh` gera os dois: o quinto argumento `neo` liga o modo Pro.
- Na bateria, os IDs são `OCRB00001` (base) e `OCRB00002` (Pro), para os dois ficarem instalados juntos.
- **Armadilhas do modo Pro**, já vistas no galaxy-recomp:
  - sem `WD_SWITCH_ON_EOP`, a GPU trava no primeiro draw grande;
  - o VideoOut em 4K exige tiles com ALT_TILE;
  - a profundidade em P16.
- O mesa-ps4 só foi validado no modo base. O modo Pro é a primeira coisa a confirmar nele.

**Resultado da bateria Vulkan no modo base (2026-11-06, `out/run1`):**
- **CPU:** 0,40–0,54 µs por draw, gravar mais enviar. Os ~1.030 draws do OoT custam ~0,5 ms.
- **GPU:** o quadro completo leva 3,4 ms em 720p, 4,6 ms em 900p e 6,1 ms em 1080p, contra 2,1 e 3,7 ms na GTX 1650.

**Bateria Vulkan no modo Pro (2026-11-06, `out/results_todas.csv`), FULL-1080p, GPU por passe:**

| Variante | Total | Geometria | Luz (pixel) | Pós (compute) |
|---|---|---|---|---|
| base | 6,13 ms | 1,67 | 2,09 | 2,35 |
| Pro (driver como veio) | 5,96 ms | 1,56 | 2,04 | 2,35 |
| Pro com `ORBIS_NUM_SE=4` | 4,91 ms | 1,46 | 2,04 | 1,39 |
| **Pro com o patch Gladius** | **3,93 ms** | 1,35 | **1,18** | **1,39** |

- O mesa-ps4 trata todo console como Liverpool: `device_id` 0x9920 e 2 motores fixos em `ac_orbis_drm.c`.
- Com a chave `ORBIS_NUM_SE=4`, o compute passa a usar a GPU inteira, 1,7 vez mais rápido, sem travar.
- O pixel não mudou. O `PA_SC_RASTER_CONFIG` continua o do Liverpool (0x2a00161a, forma de 2 motores), então os pixels só vão para 2 dos 4 motores.
- **Feito (patch Gladius, branch `ocarina-gladius` do mesa-ps4):** um patch no mesa-ps4 que, no modo Neo (`sceKernelIsNeoMode`), declare Gladius:
  - `device_id` 0x9924, 4 motores e 16 RBs;
  - raster config de 4 motores e 16 RBs: 0x3a00161a / 0x2e, como no Hawaii.
- **Erro ao fechar ou apagar o app:** a suspensão espera `sceGnmSubmitDone()`, e sem ele vem a exceção 0xa0d0c00a. A bateria agora chama a função a cada quadro e no ocioso. Validado: o app fecha pelo botão PS sem erro.
- **Decisão da Fase 0B:** Vulkan pelo mesa-ps4, com o patch Gladius. O Pro rende 1,56x o PS4 comum no quadro completo. O GNM fica descartado como camada principal.

## Fases

Cada fase termina num critério claro. Os testes no PS4 só começam depois de avisar o João.

**Fase 0: provas no console, antes de qualquer código do jogo.** São quatro testes pequenos, um de cada vez:
1. Quanta memória um homebrew consegue alocar de fato no Pro, com GoldHEN.
2. Um triângulo Vulkan com o `mesa-ps4` no Pro, mais a lista de extensões que o RADV oferece na Gladius comparada com a que o back end do TotK Recomp exige.
3. Mapear memória em endereço fixo abaixo de 2^39. O runtime usa endereço do jogo igual a endereço do host.
4. Um bloco de C gerado pelo lifter, de uma função qualquer de exemplo, compilado para a Jaguar e rodando no console.

Critério: se os quatro passarem, o caminho é viável. Se a memória vier muito abaixo de 5 GB, replanejar antes de seguir.

**Resultado da Fase 0 (2026-11-06, sonda `probe/`, `out/probe1/`), igual nos dois modos, exceto a memória:**

| Prova | Base | Pro |
|---|---|---|
| Memória direta total, alocável de fato com GPU | 4,50 GiB, 3,25 GiB | 5,00 GiB, 3,75 GiB |
| Memória flexível | 384 MiB | 384 MiB |
| Regiões fixas: módulos 0x7000000000, kernel 0x7010000000, host 0x7020000000, 16 GiB em 0x280000000, alias de 64 GiB em 0x1000000000 | livres | livres |
| Memória direta com GPU em endereço fixo | ok | ok |
| Write-watch (mprotect + SIGSEGV com si_addr no offset 24) | ok | ok |
| `VK_EXT_external_memory_host` no mesa-ps4 | **ausente** | **ausente** |
| C do lifter (dkb-pc-recomp) na Jaguar: funções do OoT idênticas ao IdeaPad | 96/97 | 96/97 |

- **Lifter:**
  - os NSO do Switch 2 vêm em ZBIC, e `tools/nso_plain.py` os descomprime;
  - em 8.000 funções, o `main` não tem nenhuma instrução sem suporte, e o `sdk` tem 28, da classe `dp2` (provavelmente CRC32);
  - a função divergente é `main:f_70001417a0`, só de comparações de float, com código de máquina idêntico. Não é DAZ/FTZ. Fica para a Fase 2.
- **Feito (2026-11-06, commit `5118b66` na branch `ocarina-gladius`):** `VK_EXT_external_memory_host` no mesa-ps4. O BO importado mora no próprio ponteiro, desde que a memória esteja mapeada com GPU_RW (conferido com `sceKernelVirtualQuery`). A sonda 2 confirmou no Pro: a GPU escreve na memória do guest, lê o que a CPU escreveu, e memória sem GPU é recusada. Antes era a primeira tarefa da Fase 1: No PS4, a GPU usa o mesmo endereço virtual que a CPU para memória mapeada com GPU_RW, então importar um ponteiro do guest é quase só repassá-lo.

**Fase 0B: bateria de desempenho, GNM nativo × Vulkan do `mesa-ps4`.** O pedido do João: comprovar o desempenho antes de escolher a camada gráfica.

O mesmo programa de teste (`bench/`) tem dois back ends:
- **Vulkan:** pelo `mesa-ps4`.
- **GNM:** pacotes PM4 escritos por nós e enviados com o `sceGnmSubmitCommandBuffers`, como fez o renderer do galaxy-recomp.

Para comparar só a API e o driver, os dois usam **os mesmos shaders**:
- um único fonte vira SPIR-V;
- o Vulkan compila no console, pelo ACO do RADV;
- o GNM usa o binário GCN que o mesmo ACO gera offline no IdeaPad, com o RADV fingindo a GPU do PS4 Pro (null winsys).

As cenas imitam o que o OoT faz, com números medidos no Mac (`jogar.log` do build `_ootup`):

| Cena | O que mede | Carga, a partir do OoT |
|---|---|---|
| S1 tempestade de draws | custo de CPU por draw, que é o ponto fraco da Jaguar | 1.000, 2.500 e 5.000 draws por quadro (o OoT faz cerca de 1.030); constantes por draw; ~3.600 texturas bindless (o OoT tem 3.618 no pool) |
| S2 G-buffer | preenchimento e banda da GPU | deferred com 4 render targets, ~2 M triângulos, texturas BC, em 1280×720, 1600×900 e 1920×1080 |
| S3 pós-processamento em compute | compute em tela cheia | névoa volumétrica, bloom e um TAA simples |
| S4 sombras | passes só de profundidade | 3 cascatas de 2048² |
| S5 substituto de mesh shader | o caminho que o OoT vai precisar: a GPU do PS4 não tem mesh nem task shader | culling em compute + draw indireto simples (o `DRAW_*_INDIRECT_MULTI` trava o CP do PS4, como o mesa-ps4 descobriu) |
| S6 memória | estabilidade com o orçamento cheio | 4,5 GB de texturas e buffers vivos, trocados ao longo de 10 minutos |

Medidas por cena e por API:
- tempo de CPU para gravar e enviar o quadro;
- tempo de GPU por timestamps;
- fps;
- memória usada;
- travamentos.

Cada cena roda 60 s depois de 10 s de aquecimento, três vezes.

Antes do console, o mesmo programa roda no IdeaPad (Linux, GTX 1650) só pelo lado Vulkan, para depurar sem gastar ciclos no PS4.

**Regra de decisão:**
- **Vulkan:** se o Vulkan ficar a até ~15% do GNM no tempo de GPU e custar menos de ~5 µs de CPU por draw, ele vence. O TotK Recomp e o DKB Recomp já falam Vulkan, o que economiza meses.
- **GNM:** se o GNM for muito melhor, o back end NVN 2 → GNM é escrito direto, e o Vulkan fica como referência de correção.
- **Híbrido:** em último caso, Vulkan no geral e GNM só nos passes quentes.

**Fase 1: a base de Switch 1 no PS4.**
- Portar o runtime do TotK Recomp para o PS4: escrever `runtime/platform/orbis.c` ao lado de `win32.c` e `linux.c`, o vídeo e o controle pelo SDL2 orbis ou direto pelo VideoOut e pelo scePad, e o Vulkan pelo `mesa-ps4`.
- Validar com um programa de teste próprio, sem jogo. Opcionalmente, com o próprio TotK, se o João tiver o dump.

**Fase 1, refinada depois de ler o runtime (2026-11-06):**
- **O runtime do dkb-pc-recomp:** 54,6 mil linhas de C. A camada de plataforma é `platform/platform.h` (395 linhas), com `linux.c` (1.650) e `win32.c`. O build (`tools/build_game.py`) já faz Linux, com `*_sdl.*` e `linux.c`.
- **Dependências:** SDL3, Vulkan com VMA, zstd, libvpx (filmes VP9; os do OoT são AV1) e opus.
- **Os shaders são traduzidos em tempo de execução** por um processo filho .NET (`totk_xlate --serve`, `gfx_shader.c:384`), com cache em disco por sha256. No PS4 não há processos filhos nem .NET. O cache é gerado no PC e o runtime do PS4 só o lê (modo cache-only).
- **`plat_reexec`** (`memory.c:213`) relança o processo quando as regiões fixas estão ocupadas. No PS4 elas estão livres (Fase 0), e isso vira erro fatal.
- **A ordem:**
  1. **1A, OoT no IdeaPad:** o build Linux do runtime com o OoT inteiro traduzido. Mostra até onde o OoT vai sobre o host Switch 2 do DKB e gera o cache de shaders que o PS4 vai usar. Precisa de SDL3, libvpx, opus, VMA e .NET 10 no IdeaPad.
  - **1A, resultado (2026-11-07):** com a base ocarina-recomp, o OoT abre no IdeaPad e o filme de abertura toca, a ~35 fps de média. Foram 2 correções na branch `ps4`: o dav1d no build Linux e a pilha das fibras alinhada a 16, que valia também para o PS4.
  2. **1B, `runtime/platform/orbis.c`:** threads (pthread do overlay), memória (`sceKernelReserveVirtualRange` e `MapDirectMemory` em endereço fixo), write-watch (já provado), tempo (`sceKernelGetProcessTime`), arquivos (`/data/...` e `/app0`), sem processos filhos.
  3. **1C, janela, controle e áudio do PS4:** a janela é a superfície headless do mesa-ps4, o controle vai por `scePad` e o áudio por `sceAudioOut`. Mais `sceGnmSubmitDone()` a cada quadro.
  4. **1D, programa de teste no PS4:** o runtime sobe e apresenta quadros, e só depois vem o OoT.

**Fase 2: lifter no OoT, só a CPU.**
- Extrair `main`, `subsdk0` e `sdk` do `.nup` (o nxnative já lê `.nup`) e passar pelo lifter, com as extensões de ARMv8.3 que o DKB Recomp acrescentou.
- Critério: zero instruções sem suporte, e os testes diferenciais contra o Unicorn passando.

**Fase 3: a adaptação para o Switch 2,** seguindo o mapa do DKB Recomp e o conhecimento do nxnative:
- os serviços do SDK 22;
- o NVN 2;
- os shaders Ampere, com os mesh e task nos slots 7 e 8;
- o NDVE dos filmes, desligado pelo equivalente do mod `video`;
- o filme de abertura.

Critério: a tela de título aparece no PS4.

**Fase 4: o 3D e a dieta de memória.** Kokiri Forest renderizando, com medição de memória, resolução reduzida e texturas cortadas. Critério: chegar ao controle do Link sem estourar a memória.

**Fase 5: desempenho.** Medir CPU e GPU por quadro e atacar os trechos quentes do código traduzido e da GPU. A meta inicial é 30 fps estável.

## Infraestrutura

- **Build: no IdeaPad, pelo alias SSH `ideapad3i`.**
  - Máquina: Fedora 43 x86_64, i7-10750H com 12 threads (AVX2), 31 GB de RAM, 227 GB livres e uma GTX 1650 com Vulkan.
  - Já tem clang 21, lld, cmake, ninja, dotnet, o OpenOrbis completo (`~/ps4toolchain/OpenOrbis/PS4Toolchain`, com `create-fself`, `create-gp4` e `PkgTool.Core`), o LibOrbisPkg, o `ps4-payload-sdk` e o shadPS4.
  - Por ser x86 com Vulkan, as fases 2 e 3 podem ser desenvolvidas primeiro **rodando no próprio IdeaPad** (Linux nativo, como o totk-recomp), e só depois ir para o PS4.
  - O C gerado é enorme: o TotK leva uns 17 minutos num PC e precisa de 45 GB de disco no pico.
  - O Orange Pi fica de reserva.
- **Console:** o ciclo do galaxy-recomp. PS4 Pro em 192.168.1.99, GoldHEN, loader QVOX00002, FTP, klog na porta 3232, reload remoto. O JIT não é necessário, porque tudo é compilado antes.
- **Referência viva:** o OoT rodando no Mac pelo nxnative, para comparar logs e imagens quadro a quadro.

## Perguntas em aberto

- O código do galaxy-recomp (o renderer PM4/GNM do Pro, o loader QVOX00002 e o `push_game.sh`) não está mais em `~/Workspace/galaxy-recomp`. Descobrir se existe cópia. Se não existir, o conhecimento está nas memórias e no histórico das sessões.
- O `cas.db` de 13 GB do OoT no Mac: pode ser apagado para liberar disco?
- O firmware do PS4 Pro e a versão do GoldHEN: confirmar no primeiro teste.
