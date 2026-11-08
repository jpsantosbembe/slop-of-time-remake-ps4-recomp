# Frente G: pasta de dados derivada do jogo

Escrito em 2026-11-07. Hoje a pasta gravável do port no console é fixa como `/data/ocarina` em três lugares (runtime,
carregador e scripts do Mac). Este desenho tira o nome do código: a pasta vem da identidade do jogo.

## Convenção

- Pasta: `/data/slop/<title id do Switch>/`, com o title ID em 16 dígitos hexadecimais minúsculos (o mesmo formato de
  `game_info.py` e de `jogos/<title id>/`). Para o OoT: `/data/slop/0400486024dec000/`.
  `slop` é o prefixo do projeto (o mesmo dos IDs SLOPnnnnn do PS4); um jogo novo ganha a própria pasta, sem colisão.
- O ELF do jogo no console passa a se chamar `game.elf` (não `ocarina.elf`). O nome do arquivo do build no IdeaPad
  (`build/oot111-orbis/ocarina.elf`) não muda; só o nome no console.
- Dentro da pasta, nada muda: `args.txt`, `dev-host.txt`, `exefs/`, `romfs/`, `romfs-index.bin`, `shader-cache/`,
  `logs/`, `tmp/` etc.

## Quem descobre o title ID, e como

1. **Fonte única para os scripts e o carregador:** `game_info.py` ganha o campo `ps4_data_dir`
   (`/data/slop/<title_id>`), calculado do title ID que ele já lê do `.nup`. A convenção mora só ali.
2. **Carregador** (`loader/loader.c`): recebe a pasta na compilação, como o `pkg_jogo.sh` faz com o nome.
   `loader/build.sh` roda `game_info.py --campo ps4_data_dir` e passa `-DSLOP_DATA_DIR="..."`; sem a definição, o
   `loader.c` não compila (`#error`). O carregador é um PKG por jogo de todo modo (TITLE_ID e nome do jogo), então não
   perde generalidade. `GAME_PATH` = `SLOP_DATA_DIR "/game.elf"`, `HOST_PATH` = `SLOP_DATA_DIR "/dev-host.txt"`.
   Ao pular no `_start`, o carregador passa `argv[0]` = o caminho completo do `game.elf` (hoje passa `"eboot.bin"`).
3. **Runtime** (`runtime/platform/orbis.c`): a pasta de dados é `dirname(argv[0])` quando `argv[0]` é um caminho
   absoluto em `/data/` (o carregador de dev); senão (o runtime como eboot direto, PKG teste/final, onde `argv[0]` é
   `eboot.bin`), vale `rt_orbis_data_dir`, gerado pelo `tools/build_game.py` a partir do Program ID do `exefs/main.npdm`
   do build (ACI0 + 0x10; confere com o title ID do `.nup`: 0400486024dec000). O `ORBIS_DATA_DIR` vira a função
   `orbis_data_dir()`, preenchida no `main` do `orbis.c` antes de tudo (antes do `args.txt`, do stderr e do log). Os
   `rt_default_exefs/romfs/profile` gerados passam a usar a mesma pasta derivada; e como `plat_exe_path` devolve
   `<pasta>/eboot.bin`, o `portable_paths` acha `<pasta>/exefs` e afins mesmo se o carregador apontar outra pasta.
   O `probe_orbis.c` usa `orbis_data_dir()` no lugar de `/data/ocarina`.
4. **Scripts do Mac** (`dev_push.sh`, `dev_reload.py`, `send_game_files.sh`, `send_romfs.sh`): pegam a pasta de
   `game_info.py --campo ps4_data_dir` (por `ssh ideapad3i`, ou direto quando o trecho já roda no IdeaPad), com
   `SLOP_DATA_DIR` no ambiente para pular o ssh.

Alternativas descartadas: ler o title ID do `param.sfo` (o TITLE_ID do PS4 é um hash, não volta ao do Switch; daria
para pôr no rótulo do CONTENT_ID, mas é mais frágil que a constante de compilação); o runtime ler o `main.npdm` (fica
dentro da própria pasta que se quer achar); variável de ambiente do carregador (o jogo tem a própria musl estática e
não vê o `environ` da libc do sistema).

## Migração da pasta atual (sem copiar os 26 GB)

`tools/migrar_pasta_dados.py` (Mac): por FTP, `MKD /data/slop`, `RNFR /data/ocarina` `RNTO /data/slop/<title id>` e,
dentro dela, `ocarina.elf` para `game.elf`. É um rename no mesmo sistema de arquivos (`/data`): instantâneo, nada é
copiado. Sem `--executar` só mostra o que faria; recusa se o destino já existir; `--desfazer` volta ao nome antigo.

## Onde está a implementação

- Runtime: branch `frente/g` (worktree `~/ocarina-ps4/ext/frente-g` no IdeaPad), commit "PS4: pasta de dados derivada
  do jogo": `runtime/platform/orbis.c`, `runtime/probe_orbis.c`, comentários em `main.c` e `gfx_shader.c`,
  `tools/build_game.py` (`orbis_data_dir`, gera `rt_orbis_data_dir`).
- `tools/game_info.py` (Mac e cópia no IdeaPad): campo `ps4_data_dir` já aplicado (só acrescenta, não muda nada).
- Carregador e scripts do Mac: as versões novas estão em `frente-G-pronto/` (mesmos caminhos: `loader/`, `tools/`),
  e NÃO no lugar, porque os scripts do Mac valem na hora e quebrariam o fluxo atual antes da migração. Aplicar com
  `cp -R frente-G-pronto/* .` na raiz do repositório no passo 3 abaixo, e apagar a pasta.
- `tools/migrar_pasta_dados.py`: novo, inofensivo sem `--executar`.

Ordem (só o principal executa, e a instalação do PKG é com o João):

1. Merge de `frente/g` em `ps4-orbis`; build do ELF (`build_ps4.sh --no-pkg`).
2. Copiar `frente-G-pronto/loader/{loader.c,build.sh}` para `~/ocarina-ps4/loader` no IdeaPad e rodar
   `loader/build.sh` (PKG "<jogo> (dev)" novo, mesmo TITLE_ID SLOP de dev; já compila e liga, conferido).
3. Fechar o app no console; aplicar `frente-G-pronto/` no Mac (`cp -R frente-G-pronto/* .`).
4. `python3 tools/migrar_pasta_dados.py --executar` (sem a opção, só confere; conferido em 2026-11-07).
   NÃO testado: o RNFR/RNTO de uma pasta pelo FTP do PS4. Antes de mexer em `/data/ocarina`, confirmar com uma pasta
   descartável (`MKD /data/slop/teste`, `RNFR /data/slop/teste RNTO /data/slop/teste2`, `RMD`). Se o servidor recusar,
   o plano B continua sendo um rename no mesmo sistema de arquivos por outro canal (ps4debug), nunca uma cópia.
5. O João instala o PKG novo do carregador (substitui o antigo, mesmo TITLE_ID).
6. `tools/dev_push.sh --no-reload` (sobe o `game.elf` novo) e abrir o app. Conferir no klog
   `[ocl] início: /data/slop/0400486024dec000/game.elf` e o log em `/data/slop/0400486024dec000/logs/totk.log`.

O carregador velho com a pasta nova (ou o novo com a velha) não acha o ELF e só fica em "erro-carga", sem estragar
nada. Para voltar: `migrar_pasta_dados.py --desfazer` e reinstalar o PKG antigo.
