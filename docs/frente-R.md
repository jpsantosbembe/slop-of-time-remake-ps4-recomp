# Frente R: repositório e empacotamento

Escrito em 2026-11-07. Árvore montada em `/Users/joaobembe/Workspace/slop-of-time-remake-ps4-recomp` (branch `main`,
local, sem push). Repositório no GitHub: `jpsantosbembe/slop-of-time-remake-ps4-recomp`, privado e vazio.

## A estrutura: um repositório só

A auditoria de licenças (`licencas.md`) sugeria três repositórios (fork do ocarina-recomp, ferramentas do Mac, fork do
mesa-ps4). O João escolheu um nome só, então tudo vai num repositório, com a licença separada por pasta:

```
/                         fork do ocarina-recomp: histórico inteiro do upstream + a branch ps4-orbis (GPL-3.0)
  LICENSE, LICENSE-MIT    herdados, intactos (GPL-3.0; MIT do TotK Recomp)
  THIRD_PARTY_NOTICES.md  herdado + seção "PlayStation 4 port" (dav1d, opus, zstd, zlib, zbic, OpenOrbis, bundle
                          orbis-sdk-v1, orbis-tls.ld, orbis-compat, orbis-porting-kit, mesa-ps4, LibOrbisPkg)
  NOTICE                  resumo: aviso de IA, não afiliação, licença por parte, bases e terceiros
  REUSE.toml              licença por caminho dos arquivos do port, sem editar os arquivos da ps4-orbis
  README.md, README.en.md os nossos (aviso de IA, não afiliação, estado, requisitos, passo a passo)
  docs/ocarina-recomp/    os README do upstream, movidos da raiz
  runtime/, recomp/, tools/  o código do fork (com o port de PS4 da ps4-orbis)
  ps4/                    as ferramentas do Mac: loader/, probe/, bench/, tools/, docs/, jogos/<title id>/
                          (GPL-3.0-or-later OR MIT; ps4/LICENSE-MIT)
  patches/mesa-ps4/       os 2 commits do mesa-ps4 como git format-patch, sobre 1a59cc1 (MIT; LICENSE próprio)
```

Por quê:

- **Raiz = fork com histórico.** A GPL pede os avisos e a origem; o histórico do upstream (5,4 MB) mostra quem fez o
  quê e deixa o `git merge upstream/main` funcionar. Herda de graça o `LICENSE`, o `LICENSE-MIT`, o
  `THIRD_PARTY_NOTICES.md`, o `.gitignore` e o `tools/check_content.py` com o CI `content-guard`.
- **`ps4/` separado do código do fork.** As ferramentas do Mac são nossas e podem sair também sob MIT; numa pasta
  própria, a licença dupla é uma linha do `REUSE.toml`, e os merges do upstream nunca tocam nelas.
- **mesa-ps4 como patches, não como fork.** São 2 commits em 3 arquivos MIT. Como patches, ficam MIT sem misturar com
  a GPL, e voltam ao upstream (o mesa-ps4 não está arquivado) com `git am`. O build aponta para o clone do usuário.
- **Nada vendorizado do SDK.** O bundle `orbis-sdk-v1` (284 MB, stubs "UNRESOLVED", `orbis-tls.ld` GPL-3.0-only) fica
  referenciado por release e SHA-256.
- **Licença por caminho no `REUSE.toml`** em vez de cabeçalho SPDX nos 28 arquivos da `ps4-orbis`: editar esses
  arquivos aqui geraria conflito com o trabalho do principal no IdeaPad. O cabeçalho por arquivo fica para o principal
  aplicar na `ps4-orbis` (proposta: `SPDX-License-Identifier: GPL-3.0-or-later` e
  `SPDX-FileCopyrightText: 2026 João Paulo Santos Bembe`; `platform/orbis.c` só GPL, porque parte do `linux.c`).

Limite do GitHub: um repositório **privado não pode ser fork** de um público, e não vira fork depois. A relação com o
upstream fica no histórico, no remote `upstream` documentado e no README. Se um dia ficar público, dá para recriar
como fork do GitHub e empurrar a mesma `main`.

## O que foi feito na árvore

Commits locais na `main` (sobre `ps4-orbis` @ `a909add`, que está 42 commits à frente do upstream v0.2.4 `1c362d7`):

1. `ps4/`: cópia das ferramentas do Mac (`loader/`, `probe/`, `bench/`, `tools/`, `jogos/`, e `docs/PLANO.md` e
   `docs/fase1-orbis.md`). Ficaram de fora: `out/`, `__pycache__`, `docs/frentes.md` e este arquivo (coordenação
   interna, com caminhos do scratchpad), `tools/xlate_bridge.py` (idêntico ao `tools/oot/xlate_bridge.py` do fork),
   `tools/nso_plain.py` (dependia de `~/Workspace/nxnative`; obsoleto, o `compilar.sh` usa o `unzbic`) e o
   `payload_lancar/lancar.elf`. O `deps_ps4.sh` veio do IdeaPad (o do Mac estava atrasado: sem a correção das
   `CFLAGS`); o `loader.c` veio do Mac (mais novo).
2. Parametrização: `ideapad3i` → `OCARINA_BUILD_HOST`, `192.168.1.99` → `OCARINA_PS4_HOST`, lidos de
   `ps4/config.local.sh` (ignorado; modelo em `ps4/config.example.sh`) por `ps4/tools/ambiente.sh` (shell) e
   `ps4/tools/slop_config.py` (Python). `~/ocarina-ps4` ficou como a área de trabalho documentada (o
   `tools/build_game.py --target orbis` também usa esse padrão); `/data/ocarina` não foi mexido (é da frente G).
3. Saneamento: `args-jogo.txt` virou `ps4/jogos/0400486024dec000/args.txt` (é do jogo), sem o offset `main+0x…` no
   comentário; `PLANO.md` sem o endereço `main:f_7000…`, sem IP e alias, e sem duas perguntas pessoais (disco do Mac,
   paradeiro do galaxy-recomp). O `DKB_SKIP_CS` (hashes de shaders) ficou: o `jogar/jogar-ocarina.sh` do upstream
   publica a mesma lista.
4. `ps4/tools/preparar_area.sh`: monta `~/ocarina-ps4` com links para o clone (os scripts esperam esse layout) e lista o
   que falta.
5. `.gitignore` e `tools/check_content.py`: barram também `.nup .pkg .elf .oelf .self .sprx .prx`, `eboot.bin`,
   `romfs-index.bin`, `shader-prepared.bin`, `totk_xlate.id`, `seed-*.bin`, `/out/`, `klog*.txt`, `lift_*.txt`, os
   `build/` de `ps4/` e o `config.local.sh`. `tests/check_content_test.py` passa.
6. README PT/EN, `NOTICE`, seção nova no `THIRD_PARTY_NOTICES.md`, `REUSE.toml`, `patches/mesa-ps4/`.

Conferências: `python3 tools/check_content.py` limpo; nenhum arquivo acima de 1 MiB; busca por `192.168.`,
`/Users/joaobembe`, `/home/jpbembe`, `ideapad3i`, `prod.keys` e blocos de chave sem ocorrência fora dos exemplos.

Histórico: os 42 commits da `ps4-orbis` sobre o upstream também foram conferidos (o push leva o histórico, não só a
árvore): nenhuma mensagem com `main+0x…`, `f_7…`, IP ou caminho pessoal; nenhum blob acima de 1 MiB nem com
extensão barrada; os arquivos adicionados são só os do port (`runtime/*_orbis*`, `platform/orbis.c`, `vpx_none.h`,
`tools/orbis/*`, `tools/oot/{oot_shader_seed,xlate_bridge}.py`).

Correção feita no caminho: com um clone só, o `prepare_shaders_ps4.sh` faria `ln -sf` da semente sobre ela mesma
(as duas árvores apontam para o mesmo lugar); agora só liga quando as árvores são diferentes.

Pendências pequenas herdadas: o `.githooks/pre-commit` do upstream é 644 e chama `python` (no Mac só há `python3`),
então o hook não roda aqui; os README do upstream em `docs/ocarina-recomp/` têm links relativos que agora quebram; e
`docs/architecture.md`, `docs/build-by-hand.md` e `docs/compilar-manual.md` apontam para o README da raiz, que agora é
o nosso.

## Sincronia depois do primeiro push

A árvore é uma foto de 2026-11-07. Até o primeiro push, o trabalho continua onde está (IdeaPad `ps4-orbis` e Mac
`~/Workspace/ocarina-ps4`). Para atualizar: `git fetch ideapad && git merge ideapad/ps4-orbis` e copiar de novo as
pastas do Mac para `ps4/` (repassando a parametrização). Proposta para depois do push: o Mac passa a trabalhar direto
em `ps4/` do repositório, e o IdeaPad adiciona o GitHub como remote da `ps4-orbis`.

## Decisões pendentes (do João)

1. Primeiro push: `git push -u origin main` (sem as branches `frente/*`, que são só remote-tracking do clone).
2. README do upstream fora da raiz (feito) ou o nosso em `ps4/README.md` com o do upstream na raiz.
3. "GPL-3.0" do ocarina-recomp é "only" ou "or later"? Perguntar ao sostenesfreitas. O mesmo para o zbic (GPL-2.0).
4. Licença dupla MIT também para os arquivos do port escritos do zero em `runtime/` (`input_orbis.c`,
   `swkbd_orbis.c`, `probe_orbis.c`, `vpx_none.h`, `gfx_window_orbis.c`, `tools/orbis/*`)? Hoje o `REUSE.toml` os deixa
   só GPL-3.0-or-later, o mais conservador.
5. `ps4/docs/PLANO.md` cita o nxnative (fork privado) e o histórico pessoal do projeto: publicar assim ou resumir.
6. Nome no README: o título "Slop of Time Remake" usa só o trocadilho; "Zelda" e "Ocarina of Time" aparecem só na
   descrição e no aviso de marca.
