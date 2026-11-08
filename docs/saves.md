# Saves para os testes (começar no mundo, sem o New Game)

Hoje os testes no console começam um New Game: o roteiro de input (`TOTK_INPUT_SCRIPT`, `input-ng.txt`) aperta A três
vezes e o jogo passa pela abertura e pelo vídeo inicial antes de carregar a Floresta Kokiri. Isso custa minutos por
teste. Com um slot de save gravado depois da abertura, o teste vai da tela de título direto para o mundo.

## O editor (onif.info/ocarina)

- Endereço: https://onif.info/ocarina/ ("Ocarina SAVE EDITOR for Switch 2", de rastapasta, não oficial). É feito para o
  save do OoT do Switch 2, o mesmo jogo deste port.
- Abre uma pasta `slot_XX` com `caption.sav` e `progress.sav`, ou os dois arquivos juntos.
- **O processamento é no servidor, não no browser.** O `app.js` manda os arquivos em base64 para `/api/import`, e a
  exportação vem de `/api/export` como `slot_XX-edited.zip`. Ou seja, o save sai da máquina. Não é dado sensível, mas
  fica registrado.
- As APIs, lidas do `app.js` em 2026-11-08:
  - `POST /api/demo {}`: um save de demonstração completo, com os arquivos em base64;
  - `POST /api/import {files:[{path,data}]}`: lê um save;
  - `POST /api/export {files, edit_token, changes}`: devolve o zip editado;
  - `GET /api/item-limits`.
- Campos editáveis, em 9 categorias (link, inventory, equipment, quests, dungeons, statistics, world, history,
  details):
  - corações, magia, carteira e rupias;
  - itens, munição e garrafas;
  - equipamento;
  - canções e itens de missão;
  - estado das masmorras;
  - lugares visitados;
  - posição de retorno (`Saved position`, Vector3 dentro da cena, e `Saved facing angle`);
  - data e rótulo do slot (`Location Name`, que é só o texto; a cena fica em outro lugar);
  - o histórico de eventos e conversas (2662 eventos jogados), que o editor não deixa editar.
- A demonstração ("Load demo") é um `slot_00` na **Floresta Kokiri**, com 210 s de jogo, 3 corações, 0 rupias, autosave
  e data 2026-11-05. Ou seja, logo depois da abertura: é o ponto que os testes precisam.
  - No browser do cmux o botão não carregou nada, sem erro visível. Por isso o save foi tirado direto da API (`/api/demo`).

## Formato

- O mesmo contêiner do `account.sav` que o nosso runtime já grava no PS4: cabeçalho `04 03 02 01 eb 80 2d 00`. É a
  tabela de gamedata por hash, a mesma família do formato do TotK.
- Tamanhos do slot de demonstração: `progress.sav` 120572 bytes, `caption.sav` 40035 bytes. O caption traz a miniatura
  JPEG e o rótulo do slot.

## Onde os saves ficam

- **Fora do repositório**, porque é dado derivado do jogo: no IdeaPad, em `~/ocarina-ps4/saves/<nome>/slot_XX/`.
  - `~/ocarina-ps4/saves/demo-kokiri/slot_00/`: a demonstração do editor, decodificada sem mudanças.
- No console, o runtime monta o save em `<pasta de dados>/profile/save/<uid>/`, com o `account.sav` e as pastas
  `slot_XX/`.
  - `<uid>` é a conta criada na primeira execução; no PS4 de testes é `8f5f32fbc56e9242d0155b3cc5c0070d`.
  - O `save-tx/work` é a cópia de trabalho; o runtime a apaga ao abrir e a refaz a partir de `save/<uid>`
    (`runtime/fs_host.c`). Por isso, um slot copiado para `save/<uid>/` vale na próxima execução.
  - As versões anteriores ficam em `profile/save-backup/<hora UTC>/` (as três últimas).

## Trocar a cena e o ponto de partida (`tools/editar_save.py`)

O editor web só muda a posição DENTRO da cena. Ele não troca a cena, porque o rótulo `Location Name` é só texto. Para
os testes começarem numa cena leve, o save foi lido direto. Como:
- A lista de campos é `GameData/GameDataList.Product.110.byml.zs` (zstd com o `zs.zsdic` de `Pack/ZsDic.pack.zs`; BYML
  v7 big-endian; `tools/byml.py`).
  - Os campos aparecem só como hash, com tipo, valor padrão e `SaveFileIndex`: 0 é o progress, 2 é o caption e 3 é o
    account.
  - O `MetaData.SaveDirectory` confirma os arquivos: `slot_%02d/progress.sav`, `option.sav`, `slot_%02d/caption.sav` e
    `account.sav`.
- O save tem um cabeçalho de 0x20 bytes, uma tabela de pares (hash u32, valor u32) até o início dos dados (u32 em +8) e
  depois os dados.
  - Strings e vetores são offsets para a área de dados; float e int ficam no próprio par.
  - **Não há checksum**: a exportação do editor só mudou os bytes do valor.
- Campos de local na demonstração:
  - a cena `KokiriForest_Child` em dois campos de texto (String32 e String64);
  - a posição em três Vector3 iguais;
  - o ângulo em um Float;
  - no caption, a posição, o rótulo `KokiriForest` e `KokiriForest/KokiriForest_Child`.
  - Os hashes ficam em `jogos/0400486024dec000/save.json`.
- A posição da demonstração é exatamente o `StartPos` sem nome da Kokiri, em
  `Banc/KokiriForest/KokiriForest_Child_Dynamic.bcett.byml.zs` (zstd com o `bcett.byml.zsdic`).
  - O ângulo salvo é o yaw do ator: com Rotate (±π, Y, ±π), o yaw é π − Y. Na demonstração, (−π, 1,2467, −π) dá
    1,8949, o valor gravado.
- O `editar_save.py` lê o `StartPos` da cena pedida e grava a cena, a posição, o ângulo e os rótulos. Roda no IdeaPad,
  onde ficam o RomFS e o zstd.

```
python3 tools/editar_save.py saves/demo-kokiri/slot_00 /tmp/x --cena InsideTheDekuTree --listar
python3 tools/editar_save.py saves/demo-kokiri/slot_00 saves/deku-entrada/slot_00 --cena InsideTheDekuTree --start Entrance
```

### Cenas leves

São 80 cenas, cada uma com um Banc (`Banc/<grupo>/<cena>_{Static,Dynamic}`). As mais leves pelo tamanho do Banc e dos
probes de luz:
- as arenas `DreamWorld/*Stage`: DinolfosStage 2 KB, GohmaStage 6 KB, BossRushIntervalRoom 13 KB;
- as masmorras pequenas: InsideTheDekuTree 122 KB e 4,6 MB de probes; BottomOfTheWell 66 KB e 2 MB;
- para comparar, KokiriForest_Child tem 235 KB e 39 MB de probes, e HyruleField_Child 356 KB e 114 MB.

A primeira escolhida foi **InsideTheDekuTree, StartPos "Entrance"**. É coerente com o save (Link criança, objetivo
gravado `Meet_DekuTree`): `saves/deku-entrada/slot_00`.

## Instalar um slot no console

```
bash tools/instalar_save.sh ~/ocarina-ps4/saves/demo-kokiri/slot_00
```

- Lê a pasta no IdeaPad e manda por FTP para `profile/save/<uid>/slot_00/` (`.tmp` + rename).
- A pasta `profile` é criada pelo app (dono uid 1, modo 0755), e o FTP do GoldHEN não escreve nela: o STOR e o MKD dão
  550. O script faz `SITE CHMOD 777` na pasta da conta. O slot fica com modo 777 e os arquivos com 666, para o app
  poder regravá-los.
- Rode antes de um reload ou com o jogo fechado: o jogo aberto pode gravar por cima.
- O roteiro de input muda: com um slot gravado, a tela de título leva à escolha de arquivo em vez do New Game.
  O roteiro de "continuar" fica registrado abaixo, depois de testado.

## Registro

- 2026-11-08:
  - Editor examinado. O save de demonstração foi baixado e guardado em `saves/demo-kokiri`. Criado o
    `tools/instalar_save.sh`.
  - Lidos o formato e os campos de local. Criados o `tools/editar_save.py`, o `tools/byml.py` e o `jogos/<tid>/save.json`.
    Gerado o `saves/deku-entrada`.
  - Ainda falta o teste no console.
