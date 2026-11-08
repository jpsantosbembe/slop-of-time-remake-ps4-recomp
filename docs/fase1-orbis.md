# Fase 1B: a camada de plataforma do PS4 (`runtime/platform/orbis.c`)

Base: `sostenesfreitas/ocarina-recomp`, branch local `ps4` no IdeaPad (`~/ocarina-ps4/ext/ocarina-recomp`). A interface é
`runtime/platform/platform.h`: cerca de 120 funções, com uma implementação por sistema (`win32.c` e `linux.c`). O
`orbis.c` parte do `linux.c`, porque o PS4 é um FreeBSD com musl e usa a mesma ABI SysV x86-64. O toolchain é o bundle
`orbis-sdk-v1` (OpenOrbis 0.5.4 + overlay `orbis-compat`), e o alvo é `-march=btver2`.

Escrito em 2026-11-07, a partir da Fase 0 (sonda `probe/`), que já provou no console tudo o que está marcado como
**provado**.

## Por grupo

| Grupo | No PS4 | Estado |
|---|---|---|
| **Memória: reserva fixa abaixo de 2^39** | `sceKernelReserveVirtualRange(&a, n, MAP_FIXED\|NO_OVERWRITE)`. As regiões do runtime estão livres: módulos em 0x7000000000, kernel em 0x7010000000, host em 0x7020000000, bloco de 16 GiB em 0x280000000 e alias de 64 GiB em 0x1000000000 | **provado** |
| **Memória: commit** | memória **direta** WB_ONION, mapeada em endereço fixo com CPU_RW+GPU_RW, porque os pools do jogo precisam ser importáveis pela GPU (`VK_EXT_external_memory_host` do mesa-ps4 corrigido). Alocar a direta em blocos (2 MiB), não página a página | **provado** |
| **Página de 16 KiB** | o runtime trabalha com 4 KiB (página do Switch, `memory.c` `PAGE`). Commit arredonda para fora; decommit só libera uma página de 16 KiB quando as 4 subpáginas foram liberadas, e senão zera a parte pedida; proteção arredonda para dentro (nunca protege mais do que o pedido); `plat_mem_state` responde por subpágina, com uma tabela própria | a fazer |
| **Write-watch** (`plat_mem_written`) | não há userfaultfd. Usar `sceKernelMprotect` para CPU somente leitura (GPU continua RW), com SIGSEGV marcando a página de 16 KiB como escrita e liberando. O `si_addr` fica no offset 24 do siginfo do FreeBSD, e o tratador é instalado com `SA_SIGINFO\|SA_NODEFER` do overlay | **provado** |
| **Gancho de falta e crash** | `sigaction` para SIGSEGV(11) e SIGBUS(10, numeração do FreeBSD), via overlay. Nos registradores do ucontext do Orbis, o rip fica em uc[28] e o rsp em uc[31] (anotado no galaxy-recomp) | parcial |
| **Threads** | pthread do overlay (`orbis_thread`), tamanho de pilha e nome (`pthread_rename_np`). Afinidade e prioridade via `sceKernelSetCpumask`/`scePthreadSetprio`, ou no-op no começo | a fazer |
| **Preempção assíncrona** (`plat_intr_*`) | desligada no começo (`TOTK_ASYNC_PREEMPT=0`, que o lançador do autor já usa) | stub |
| **Fibras** | o mesmo asm do `linux.c`, **com a correção de alinhamento** (commit da branch `ps4`) e a pilha por `mmap` | copiar |
| **Tempo** | `clock_gettime` corrigido pelo overlay (`orbis_clock`), ou `sceKernelGetProcessTime`/`sceKernelReadTsc` | a fazer |
| **Arquivos** | POSIX via overlay. Somente leitura em `/app0`; dados graváveis em `/data/ocarina/` (saves, cache, logs). O RomFS (24,5 GB) fica em `/data/ocarina/romfs/`, enviado por FTP | a fazer |
| **Processos filhos** (`plat_child_*`) | **não existem**. Shaders só vêm do cache gerado no PC (modo cache-only) | stub NULL |
| **`plat_reexec`, `plat_run_self`** | fatal / -1. As regiões fixas estão livres (Fase 0) | stub |
| **Bibliotecas dinâmicas, clipboard, pasta, fontes, carga de GPU** | stubs (NULL / -1 / 0) | stub |
| **Idioma e região** | `sceSystemServiceParamGetInt(LANG)` mapeado para BCP-47 | a fazer |

## Além do `platform.h`

- **Janela** (`gfx_window_sdl.c` → `gfx_window_orbis.c`): `VK_EXT_headless_surface`, que o mesa-ps4 apresenta via `sceVideoOutSubmitFlip`. Mais `sceGnmSubmitDone()` a cada quadro e no ocioso, senão o fechamento vira crash (0xa0d0c00a).
- **Controle** (`input_sdl.c` → `input_orbis.c`): `scePadOpen`/`scePadReadState`.
- **Áudio** (o mixer usa SDL): `sceAudioOutOpen`/`sceAudioOutOutput`, a 48 kHz.
- **Teclado virtual** (`swkbd_sdl.c`): stub, ou o diálogo IME do sistema depois.
- **Build:** um alvo `orbis` no `tools/build_game.py`, com o toolchain do bundle, `-march=btver2`, o mesa-ps4 corrigido, o vkloader do kit e as bibliotecas em versão estática para PS4: opus, dav1d, zstd e VMA (só header). O libvpx não é necessário, porque os filmes do OoT são AV1.
- **Shaders:** modo cache-only. O cache vem do PC (`--prepare-shaders` + `pack_shader_cache.py`), e quando falta uma variante, o runtime usa a padrão.
- **Modos de shader no PS4** (pedido do João, 2026-11-07):
  1. **pré-compilado** (padrão): só o cache. A variante que falta usa a padrão mais próxima, e o pedido fica anotado (o runtime já grava "answers met in play");
  2. **sincronizar:** a lista anotada vai por FTP para o PC, que traduz só o que faltou e devolve o cache. Dá para jogar direto no PS4;
  3. **on the fly pela rede:** o canal `plat_child_*` com o `totk_xlate --serve` vira uma conexão TCP do PS4 para o tradutor rodando no PC. O app pode conectar, só não pode escutar. Serve para desenvolvimento e para quem tem PC na rede;
  4. **on the fly nativo** (etapa OBRIGATÓRIA depois que o OoT rodar no PS4, decidida em 2026-11-07 para o PS4 não depender do PC): trocar o tradutor .NET (Ryujinx) pelo recompilador de shaders C++ do Eden/Eglantine, que tem Ampere/NVN2, compilado para o PS4. Exige adaptar o runtime ao formato de saída e de reflexão dele.
- **Mesh shaders:** a GPU GFX7 não tem `VK_EXT_mesh_shader`. Emular com compute + draw comum. É o maior item gráfico, e fica para depois do primeiro quadro.

## Ordem

1. `orbis.c` compilando contra o bundle, só compilação.
2. **Programa de teste do runtime** no PS4: sobe a memória, as threads, as fibras, o tempo e os arquivos, e apresenta quadros pela janela orbis.
3. O OoT com o cache de shaders do PC, até o filme de abertura no PS4.
