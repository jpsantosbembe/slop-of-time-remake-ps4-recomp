#!/bin/bash
# Um teste no console do começo ao fim, para cortar as idas e voltas: envia (opcionalmente) o ELF e o save, troca o
# args.txt, recarrega o app, espera o log novo, acompanha, baixa o log e as últimas capturas e escreve um resumo.
# Roda no Mac; fala com o PS4 por FTP (192.168.1.99:2121, sem senha) e com o carregador pelo dev_reload.py.
#   teste_console.sh <args.txt local> [--save <pasta slot no IdeaPad>] [--elf] [--minutos N]
#                    [--quadros-de-captura K] [--saida <pasta local>]
#     --save                   antes do reload, instala o slot no console (tools/instalar_save.sh)
#     --elf                    antes de tudo, manda o game.elf do IdeaPad (tools/dev_push.sh --no-reload)
#     --minutos N              teto do acompanhamento, em minutos (padrão 10; aceita decimal)
#     --quadros-de-captura K   quantas capturas shot_N.png baixar (padrão 4; 0 = nenhuma)
#     --saida <pasta>          onde gravar log, capturas e resumo (padrão out/testes/<data-hora>)
#
# Pasta de dados: /data/slop/0400486024dec000 (ou SLOP_DATA_DIR). Log: logs/totk.log; capturas: logs/shot_N.png.
# Se o carregador não responder "ok reload", o app é aberto pelo ps4_lancar.py SLOP78577.
# O teste termina no primeiro ^FATAL, no primeiro "preparar-compute: fim", em N minutos ou com o log parado por 4 min.
# Nunca lista shader-cache, mesa-cache, romfs ou profile, e não apaga nada no console (o args.txt.tmp pode ficar).
# Código de saída: 0 sem FATAL, 1 com FATAL, 2 erro de operação (console fora, sem log novo, uso inválido).
set -uo pipefail

PS4=192.168.1.99
TID=SLOP78577
DATA_PADRAO=/data/slop/0400486024dec000
L=$(cd "$(dirname "$0")/.." && pwd)

uso() {
  cat <<'EOF'
uso: teste_console.sh <args.txt local> [--save <pasta slot no IdeaPad>] [--elf] [--minutos N]
                       [--quadros-de-captura K] [--saida <pasta local>]

  <args.txt local>        arquivo que será enviado para <dados>/args.txt no console
  --save <pasta>          instala o slot (tools/instalar_save.sh) antes do reload
  --elf                   manda o game.elf do IdeaPad antes de tudo (tools/dev_push.sh --no-reload)
  --minutos N             teto do acompanhamento (padrão 10; aceita decimal)
  --quadros-de-captura K  quantas capturas baixar (padrão 4; 0 = nenhuma)
  --saida <pasta>         pasta local do log, das capturas e do resumo (padrão out/testes/<data-hora>)
  -h, --help              mostra esta ajuda

Termina no primeiro ^FATAL, no primeiro "preparar-compute: fim", em N minutos ou com o log parado por 4 minutos.
Código de saída: 0 sem FATAL, 1 com FATAL, 2 erro de operação.
EOF
}

erro() {
  echo "teste_console: $*" >&2
  exit 2
}

ARGS_TXT=""
SAVE=""
ELF=0
MINUTOS=10
CAPTURAS=4
SAIDA=""

while [ $# -gt 0 ]; do
  case "$1" in
    -h|--help)
      uso
      exit 0
      ;;
    --save)
      [ $# -ge 2 ] && [ -n "$2" ] || erro "--save precisa de uma pasta de slot"
      SAVE=$2
      shift 2
      ;;
    --elf)
      ELF=1
      shift
      ;;
    --minutos)
      [ $# -ge 2 ] || erro "--minutos precisa de um número"
      MINUTOS=$2
      shift 2
      ;;
    --quadros-de-captura)
      [ $# -ge 2 ] || erro "--quadros-de-captura precisa de um número"
      CAPTURAS=$2
      shift 2
      ;;
    --saida)
      [ $# -ge 2 ] && [ -n "$2" ] || erro "--saida precisa de uma pasta"
      SAIDA=$2
      shift 2
      ;;
    --)
      shift
      break
      ;;
    --*)
      erro "opção desconhecida: $1 (veja --help)"
      ;;
    *)
      [ -z "$ARGS_TXT" ] || erro "argumento a mais: $1"
      ARGS_TXT=$1
      shift
      ;;
  esac
done
[ $# -eq 0 ] || erro "argumento a mais: $1"
[ -n "$ARGS_TXT" ] || { uso >&2; exit 2; }
[ -f "$ARGS_TXT" ] || erro "args.txt não encontrado: $ARGS_TXT"
[[ "$MINUTOS" =~ ^[0-9]+([.][0-9]+)?$ ]] || erro "--minutos inválido: '$MINUTOS'"
python3 -c 'import sys; sys.exit(0 if float(sys.argv[1]) > 0 else 1)' "$MINUTOS" \
  || erro "--minutos precisa ser maior que zero: '$MINUTOS'"
[[ "$CAPTURAS" =~ ^[0-9]+$ ]] || erro "--quadros-de-captura inválido: '$CAPTURAS'"

DATA=${SLOP_DATA_DIR:-$DATA_PADRAO}
[[ "$DATA" == /data/?* ]] || erro "pasta de dados inválida: '$DATA'"

ARGS_TXT=$(python3 -c 'import os, sys; print(os.path.abspath(sys.argv[1]))' "$ARGS_TXT")
if [ -z "$SAIDA" ]; then
  SAIDA="$L/out/testes/$(date +%Y-%m-%d_%H-%M-%S)"
elif [[ "$SAIDA" != /* ]]; then
  SAIDA="$PWD/$SAIDA"
fi
mkdir -p "$SAIDA" || erro "não consegui criar a pasta de saída: $SAIDA"

WORK=$(mktemp -d "${TMPDIR:-/tmp}/teste-console.XXXXXX") || erro "não consegui criar a pasta temporária"
trap 'rm -rf "$WORK"' EXIT

cat > "$WORK/console.py" <<'PY'
"""Parte FTP do teste_console.sh. Só biblioteca padrão; roda no Mac."""
import ftplib
import hashlib
import io
import json
import os
import re
import sys
import time

BOOT = b'boot: guest starts'
RE_FATAL = re.compile(rb'(?m)^FATAL')
USO = 'uso interno: console.py <enviar|estado|monitorar|finalizar> ...'


def conectar(ps4):
    f = ftplib.FTP()
    f.connect(ps4, 2121, timeout=60)
    f.login()
    f.set_pasv(True)
    return f


def fechar(f):
    try:
        f.quit()
    except Exception:
        try:
            f.close()
        except Exception:
            pass


def tamanho(f, caminho):
    try:
        n = f.size(caminho)
    except ftplib.all_errors:
        return 0
    return 0 if n is None else n


def baixar(f, caminho):
    b = io.BytesIO()
    f.retrbinary('RETR ' + caminho, b.write)
    return b.getvalue()


def ler_inicio(f, caminho, n):
    """Lê só os primeiros n bytes do arquivo, sem baixar o log inteiro."""
    conn = f.transfercmd('RETR ' + caminho)
    b = bytearray()
    try:
        while len(b) < n:
            c = conn.recv(min(65536, n - len(b)))
            if not c:
                break
            b.extend(c)
    finally:
        try:
            conn.close()
        except Exception:
            pass
        try:
            f.voidresp()
        except Exception:
            pass
    return bytes(b)


def cmd_enviar(ps4, dados, origem):
    destino = f'{dados}/args.txt'
    with open(origem, 'rb') as fh:
        conteudo = fh.read()
    f = conectar(ps4)
    try:
        f.storbinary(f'STOR {destino}.tmp', io.BytesIO(conteudo))
        modo = '.tmp + rename'
        try:
            f.rename(f'{destino}.tmp', destino)
        except ftplib.all_errors as e:
            # Não apagamos nada no console: se o rename for recusado, reenvia por cima do args.txt.
            print(f'[args] rename recusado ({e}); reenviando por cima, sem apagar nada')
            f.storbinary(f'STOR {destino}', io.BytesIO(conteudo))
            modo = 'STOR direto'
        n = tamanho(f, destino)
    finally:
        fechar(f)
    if n != len(conteudo):
        raise RuntimeError(f'{destino} ficou com {n} bytes, esperava {len(conteudo)}')
    print(f'[args] {destino}: {n} bytes ({modo})')


def cmd_estado(ps4, dados, work):
    caminho = f'{dados}/logs/totk.log'
    f = conectar(ps4)
    try:
        n = tamanho(f, caminho)
        prefixo = ler_inicio(f, caminho, min(n, 4096)) if n else b''
    finally:
        fechar(f)
    estado = {'tamanho': n, 'prefixo_n': len(prefixo),
              'prefixo_sha': hashlib.sha256(prefixo).hexdigest()}
    with open(os.path.join(work, 'estado.json'), 'w') as fh:
        json.dump(estado, fh)
    print(f'[estado] log antigo: {n} bytes'
          + (f', guardei o prefixo de {len(prefixo)} B para detectar o log novo' if prefixo else ''))


def cmd_monitorar(ps4, dados, work, minutos, intervalo=20.0, ocioso=240.0):
    caminho = f'{dados}/logs/totk.log'
    with open(os.path.join(work, 'estado.json')) as fh:
        est = json.load(fh)
    base = est['tamanho']
    prefixo_n = est['prefixo_n']
    prefixo_sha = est['prefixo_sha']

    total = float(minutos) * 60.0
    boot_limite = min(total, 240.0)
    inicio = time.time()
    novo = False
    motivo = None
    ultimo_cambio = inicio
    assinatura = None
    erros = 0
    caminho_novo = os.path.join(work, 'novo.log')

    while True:
        agora = time.time()
        decorrido = agora - inicio

        try:
            f = conectar(ps4)
            try:
                texto = baixar(f, caminho)
            finally:
                fechar(f)
            erros = 0
        except Exception as e:
            erros += 1
            print(f'[monitor] {decorrido:.0f} s: FTP falhou ({erros}/10): {e}', flush=True)
            if erros >= 10:
                motivo = 'ftp'
                break
            time.sleep(max(1.0, min(intervalo, total - decorrido)))
            continue

        tamanho_atual = len(texto)
        encolheu = tamanho_atual < base
        prefixo_mudou = bool(prefixo_n and tamanho_atual >= prefixo_n
                             and hashlib.sha256(texto[:prefixo_n]).hexdigest() != prefixo_sha)
        pos_boot = texto.rfind(BOOT)
        boot_novo = pos_boot >= 0 and (base == 0 or pos_boot >= base)

        if not novo:
            if encolheu or prefixo_mudou or boot_novo:
                novo = True
                como = ('o arquivo encolheu' if encolheu else
                        'o começo do arquivo mudou' if prefixo_mudou else 'a linha de boot apareceu')
                print(f'[monitor] log novo detectado em {decorrido:.0f} s ({como})', flush=True)
                ultimo_cambio = agora

        if novo:
            if pos_boot >= 0:
                trecho = texto[pos_boot:]
            elif encolheu or prefixo_mudou:
                trecho = texto
            else:
                trecho = texto[base:]
            with open(caminho_novo, 'wb') as fh:
                fh.write(trecho)

            if RE_FATAL.search(trecho):
                motivo = 'fatal'
                print(f'[monitor] FATAL encontrado em {decorrido:.0f} s', flush=True)
                break
            if b'preparar-compute: fim' in trecho:
                motivo = 'preparo'
                print(f'[monitor] preparar-compute: fim em {decorrido:.0f} s', flush=True)
                break

            assinatura_agora = hashlib.sha256(texto).digest()
            if assinatura_agora != assinatura:
                assinatura = assinatura_agora
                ultimo_cambio = agora
            parado = agora - ultimo_cambio
            print(f'[monitor] {decorrido:.0f} s: log {tamanho_atual} B, trecho novo {len(trecho)} B, '
                  f'sem FATAL (parado há {parado:.0f} s)', flush=True)
            if parado >= ocioso:
                motivo = 'parado'
                print(f'[monitor] o log parou por {parado:.0f} s', flush=True)
                break
            if decorrido >= total:
                motivo = 'tempo'
                print(f'[monitor] teto de {total / 60:.1f} min atingido', flush=True)
                break
        else:
            print(f'[monitor] {decorrido:.0f} s: log {tamanho_atual} B, aguardando o log novo', flush=True)
            if decorrido >= boot_limite:
                motivo = 'sem-log-novo'
                print(f'[monitor] o log novo não apareceu em {boot_limite:.0f} s', flush=True)
                break

        restante = total - (time.time() - inicio)
        if restante <= 0:
            motivo = motivo or 'tempo'
            break
        time.sleep(max(1.0, min(intervalo, restante)))

    fim = {
        'motivo': motivo or 'tempo',
        'segundos': round(time.time() - inicio, 1),
        'novo': novo,
        'bytes_novo': os.path.getsize(caminho_novo) if os.path.exists(caminho_novo) else 0,
    }
    with open(os.path.join(work, 'fim.json'), 'w') as fh:
        json.dump(fim, fh)
    print(f'[monitor] fim: {fim["motivo"]} em {fim["segundos"]:.0f} s', flush=True)
    return 0


def baixar_capturas(ps4, dados, saida, quantas):
    if quantas <= 0:
        return []
    f = conectar(ps4)
    try:
        entradas = []
        for nome, fat in f.mlsd(f'{dados}/logs'):
            m = re.fullmatch(r'shot_(\d+)\.png', nome)
            if m:
                entradas.append((fat.get('modify') or '', int(m.group(1)), nome))
        entradas.sort()
        baixadas = []
        for modifica, _numero, nome in entradas[-quantas:]:
            destino = os.path.join(saida, nome)
            with open(destino, 'wb') as fh:
                f.retrbinary(f'RETR {dados}/logs/{nome}', fh.write)
            baixadas.append((nome, os.path.getsize(destino), modifica))
        return baixadas
    finally:
        fechar(f)


def analisar(log_bytes, novo_bytes):
    texto = log_bytes.decode('utf-8', errors='replace') if log_bytes else ''
    novo = novo_bytes.decode('utf-8', errors='replace') if novo_bytes else ''
    linhas = texto.splitlines()
    indices = [i for i, l in enumerate(linhas) if 'boot: guest starts' in l]
    if indices:
        ini = indices[-1]
        return (linhas[ini:],
                f"análise a partir da última linha 'boot: guest starts' (linha {ini + 1} do log baixado)",
                ini)
    if novo.strip():
        return (novo.splitlines(),
                'análise do log novo acumulado (não achei a linha de boot no log baixado)', 0)
    return linhas, 'análise do log inteiro (não achei a linha de boot)', 0


def montar_resumo(linhas, nota, deslocamento, fim, capturas, args_txt, saida):
    def numero(i):
        return i + 1 + deslocamento

    def secao(titulo, itens):
        partes.append('')
        partes.append('== ' + titulo + ' ==')
        partes.extend(itens if itens else ['(nenhum)'])

    partes = ['Resumo do teste no console']
    partes.append('data: ' + time.strftime('%Y-%m-%d %H:%M:%S'))
    motivo = fim.get('motivo', 'desconhecido')
    segundos = fim.get('segundos')
    quando = f' ({segundos:.0f} s)' if isinstance(segundos, (int, float)) else ''
    partes.append(f'motivo do fim: {motivo}{quando}')
    partes.append('args: ' + args_txt)
    partes.append('pasta: ' + saida)
    partes.append('log: ' + nota)

    fatais = []
    for i, linha in enumerate(linhas):
        if linha.startswith('FATAL'):
            fatais.append((i, max(0, i - 5)))
    if fatais:
        partes.append('')
        plural = 's' if len(fatais) > 1 else ''
        partes.append(f'== FATAL ({len(fatais)} ocorrência{plural}) ==')
        for i, inicio in fatais:
            partes.append(f'--- FATAL na linha {numero(i)} (com as 5 linhas anteriores) ---')
            for j in range(inicio, i + 1):
                partes.append(f'{numero(j)}: {linhas[j]}')
            partes.append('')
    else:
        secao('FATAL', ['nenhuma linha FATAL'])

    ft = [l for l in linhas if 'frametime: 60 s' in l][-3:]
    secao('frametime: 60 s (últimas 3)', ['  ' + l for l in ft])

    mem = [l for l in linhas if 'orbis mem' in l]
    secao('orbis mem (último)', ['  ' + mem[-1]] if mem else [])

    partes.append('')
    partes.append('== contadores ==')
    for chave in ('GPU PARADA', 'translation failed', 'image creation failed', 'device lost'):
        n = sum(l.count(chave) for l in linhas)
        partes.append(f'  {chave}: {n}')

    shots = [l for l in linhas if 'gfx: shot' in l and 'brilho médio' in l][-6:]
    secao('gfx: shot ... brilho médio (últimas 6)', ['  ' + l for l in shots])

    preparo = [l for l in linhas if 'preparar-compute: fim' in l]
    gigantes = [l for l in linhas if 'big compute shader' in l and '(compilado)' in l]
    partes.append('')
    partes.append('== preparo ==')
    partes.append(f'preparar-compute: fim: {len(preparo)}')
    for l in preparo[-10:]:
        partes.append('  ' + l)
    partes.append(f'big compute shader ... (compilado): {len(gigantes)}')
    for l in gigantes[-20:]:
        partes.append('  ' + l)

    partes.append('')
    partes.append('== capturas baixadas ==')
    if capturas:
        for nome, n, modifica in capturas:
            extra = f', {modifica}' if modifica else ''
            partes.append(f'  {nome} ({n} bytes{extra})')
    else:
        partes.append('(nenhuma)')

    return '\n'.join(partes).rstrip() + '\n', bool(fatais)


def cmd_finalizar(ps4, dados, saida, work, quantas, args_txt):
    os.makedirs(saida, exist_ok=True)

    log_bytes = None
    erro_log = None
    for _ in range(3):
        try:
            f = conectar(ps4)
            try:
                log_bytes = baixar(f, f'{dados}/logs/totk.log')
            finally:
                fechar(f)
            break
        except Exception as e:
            erro_log = e
            time.sleep(2)
    if log_bytes is not None:
        with open(os.path.join(saida, 'totk.log'), 'wb') as fh:
            fh.write(log_bytes)
        print(f'[saída] totk.log: {len(log_bytes)} bytes')
    else:
        print(f'[saída] aviso: não baixei o totk.log: {erro_log}', file=sys.stderr)

    novo_bytes = b''
    caminho_novo = os.path.join(work, 'novo.log')
    if os.path.exists(caminho_novo):
        with open(caminho_novo, 'rb') as fh:
            novo_bytes = fh.read()

    try:
        capturas = baixar_capturas(ps4, dados, saida, int(quantas))
    except Exception as e:
        print(f'[saída] aviso: capturas: {e}', file=sys.stderr)
        capturas = []
    for nome, n, _modifica in capturas:
        print(f'[saída] {nome}: {n} bytes')

    try:
        with open(os.path.join(work, 'fim.json')) as fh:
            fim = json.load(fh)
    except Exception:
        fim = {}

    linhas, nota, deslocamento = analisar(log_bytes, novo_bytes)
    resumo, tem_fatal = montar_resumo(linhas, nota, deslocamento, fim, capturas, args_txt, saida)
    with open(os.path.join(saida, 'resumo.txt'), 'w', encoding='utf-8') as fh:
        fh.write(resumo)
    print()
    print(resumo, end='')

    if log_bytes is None and not novo_bytes:
        print('\n[saída] sem log para analisar', file=sys.stderr)
        return 2
    return 1 if tem_fatal else 0


def principal(argv):
    if len(argv) < 2:
        print(USO, file=sys.stderr)
        return 2
    cmd = argv[1]
    if cmd == 'enviar' and len(argv) == 5:
        cmd_enviar(argv[2], argv[3], argv[4])
        return 0
    if cmd == 'estado' and len(argv) == 5:
        cmd_estado(argv[2], argv[3], argv[4])
        return 0
    if cmd == 'monitorar' and len(argv) >= 6:
        intervalo = float(argv[6]) if len(argv) > 6 else 20.0
        ocioso = float(argv[7]) if len(argv) > 7 else 240.0
        return cmd_monitorar(argv[2], argv[3], argv[4], argv[5], intervalo, ocioso)
    if cmd == 'finalizar' and len(argv) == 8:
        return cmd_finalizar(argv[2], argv[3], argv[4], argv[5], argv[6], argv[7])
    print(USO, file=sys.stderr)
    return 2


if __name__ == '__main__':
    try:
        sys.exit(principal(sys.argv))
    except KeyboardInterrupt:
        sys.exit(130)
    except Exception as e:
        print(f'teste_console: erro: {e}', file=sys.stderr)
        sys.exit(2)
PY

echo "[teste] console $PS4 (FTP 2121), dados $DATA"
echo "[teste] args: $ARGS_TXT"
echo "[teste] saída: $SAIDA"

if [ "$ELF" = 1 ]; then
  echo "[teste] 1/7 ELF: tools/dev_push.sh --no-reload"
  bash "$L/tools/dev_push.sh" --no-reload || erro "dev_push.sh falhou; nada foi testado"
fi

if [ -n "$SAVE" ]; then
  echo "[teste] 2/7 save: $SAVE"
  bash "$L/tools/instalar_save.sh" "$SAVE" || erro "instalar_save.sh falhou; nada foi testado"
fi

echo "[teste] 3/7 args.txt -> $DATA/args.txt"
python3 "$WORK/console.py" enviar "$PS4" "$DATA" "$ARGS_TXT" || erro "não consegui enviar o args.txt"

echo "[teste] estado do log antes do reload"
python3 "$WORK/console.py" estado "$PS4" "$DATA" "$WORK" || erro "não consegui ler o estado do log"

echo "[teste] 4/7 reload"
reload_saida=$(python3 "$L/tools/dev_reload.py" reload 2>&1)
reload_rc=$?
printf '%s\n' "$reload_saida"
if [ $reload_rc -ne 0 ] || ! printf '%s' "$reload_saida" | grep -q 'ok reload'; then
  echo "[teste] o carregador não respondeu 'ok reload'; usando tools/ps4_lancar.py $TID"
  python3 "$L/tools/ps4_lancar.py" "$TID" || erro "não consegui recarregar nem abrir o app"
fi

echo "[teste] 5/7 esperando o log novo e acompanhando (a cada 20 s)"
python3 "$WORK/console.py" monitorar "$PS4" "$DATA" "$WORK" "$MINUTOS" 20 240 \
  || erro "o acompanhamento do log falhou"

echo "[teste] 6/7 baixando o log e as últimas capturas"
echo "[teste] 7/7 gravando o resumo"
if python3 "$WORK/console.py" finalizar "$PS4" "$DATA" "$SAIDA" "$WORK" "$CAPTURAS" "$ARGS_TXT"; then
  rc=0
else
  rc=$?
fi

motivo=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1])).get("motivo", ""))' \
  "$WORK/fim.json" 2>/dev/null || true)

echo
echo "[teste] pasta de saída: $SAIDA"
if [ "$rc" = 0 ] && { [ "$motivo" = "sem-log-novo" ] || [ "$motivo" = "ftp" ]; }; then
  echo "[teste] aviso: o teste terminou sem um log novo ($motivo); o resultado não é confiável" >&2
  exit 2
fi
exit "$rc"
