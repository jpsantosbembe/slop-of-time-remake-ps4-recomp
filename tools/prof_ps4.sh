#!/bin/bash
# Perfilador por amostragem do PS4 (TOTK_PROFILE, runtime/platform/prof_orbis.c): baixa os perfis do console por FTP
# (só leitura) e gera o relatório simbolizado no IdeaPad com o tools/prof_report.py e o ocarina.elf NÃO stripado.
#   prof_ps4.sh                o arquivo de quadro lento mais recente (<perfil>.f<quadro>), ou o perfil agregado
#   prof_ps4.sh todos          todos os arquivos do perfil, um relatório por arquivo
#   prof_ps4.sh <arquivo>      um arquivo de <pasta de dados>/logs/ (ex.: prof.txt.f1463)
# Variáveis:
#   PROF_NOME     o valor de TOTK_PROFILE no args.txt (padrão prof.txt)
#   SLOP_DATA_DIR a pasta de dados no console (padrão: game_info.py --campo ps4_data_dir no IdeaPad)
#   PROF_ELF      o ELF no IdeaPad (padrão: o do build do principal). Tem de ser o MESMO build que rodou; o report avisa
#                 quando o tamanho do código não bate.
#   PROF_REPO     a worktree com o tools/prof_report.py (padrão: a do principal, ou a frente-sampler se a do principal
#                 ainda não lê ELF)
# Relatórios em out/prof/<data-hora>/ no Mac; o do arquivo mais recente também sai na tela.
set -eo pipefail
H=ideapad3i; PS4=192.168.1.99
L=$(cd "$(dirname "$0")/.." && pwd)
NOME=${PROF_NOME:-prof.txt}
QUAL=${1:-ultimo}
DATA=${SLOP_DATA_DIR:-$(ssh $H "python3 -I ~/ocarina-ps4/game_info.py --campo ps4_data_dir")}
[[ "$DATA" == /data/?* ]] || { echo "pasta de dados inválida: '$DATA'" >&2; exit 1; }
OUT="$L/out/prof/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT"

python3 -I - "$PS4" "$DATA/logs" "$NOME" "$QUAL" "$OUT" <<'PY'
import ftplib, re, sys
ps4, logs, nome, qual, out = sys.argv[1:]
f = ftplib.FTP(); f.connect(ps4, 2121, timeout=60); f.login(); f.set_pasv(True)
f.cwd(logs)
nomes = [n.rsplit('/', 1)[-1] for n in f.nlst()]
quad = sorted((int(m.group(1)), n) for n in nomes if (m := re.fullmatch(re.escape(nome) + r'\.f(\d+)', n)))
if qual == 'todos':
    pegar = [n for _, n in quad] + ([nome] if nome in nomes else [])
elif qual == 'ultimo':
    pegar = [quad[-1][1]] if quad else [nome] if nome in nomes else []
else:
    pegar = [qual] if qual in nomes else []
if not pegar:
    sys.exit(f'nada em {logs} para {qual!r} (perfil {nome!r}); arquivos: {" ".join(sorted(nomes))}')
for n in pegar:
    with open(f'{out}/{n}', 'wb') as fh:
        f.retrbinary(f'RETR {n}', fh.write)
    print(f'baixado {logs}/{n}')
f.quit()
PY

R="ocarina-ps4/prof/$(basename "$OUT")"
ssh $H "mkdir -p ~/$R"
scp -q "$OUT"/* "$H:$R/"
ssh $H "R=~/$R PROF_ELF='${PROF_ELF:-}' PROF_REPO='${PROF_REPO:-}' bash -s" <<'REMOTE'
set -eo pipefail
E=${PROF_ELF:-~/ocarina-ps4/ext/ocarina-recomp-orbis/build/oot111-orbis/ocarina.elf}
REPO=${PROF_REPO:-~/ocarina-ps4/ext/ocarina-recomp-orbis}
grep -q text_start "$REPO/tools/prof_report.py" 2>/dev/null || REPO=~/ocarina-ps4/ext/frente-sampler
for p in "$R"/*; do
  case "$p" in *.report) continue ;; esac
  python3 "$REPO/tools/prof_report.py" "$p" "$E" > "$p.report" 2>&1 || echo "prof_report falhou em $p (ver $p.report)"
done
REMOTE
scp -q "$H:$R/*.report" "$OUT/"
echo "relatórios em $OUT"
ultimo=$(ls -t "$OUT"/*.report | head -1)
cat "$ultimo"
