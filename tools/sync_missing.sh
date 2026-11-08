#!/bin/bash
# Modo "sincronizar" do cache de shaders (roda no Mac): o runtime do PS4 anota em <cache>/missing/<chave>.code e .req
# as variantes que faltaram no cache (ele não tem tradutor). Este script as traduz no IdeaPad, com o mesmo pedido, e
# devolve <chave>.spv e <chave>.res.json soltos no cache do console (o runtime lê o pack e depois os soltos). As
# variantes sincronizadas saem de missing/. Na próxima execução do jogo elas já estão lá.
#   sync_missing.sh
set -eo pipefail
H=ideapad3i; PS4=192.168.1.99
DATA=${SLOP_DATA_DIR:-$(ssh $H "python3 -I ~/ocarina-ps4/game_info.py --campo ps4_data_dir")}
[[ "$DATA" == /data/?* ]] || { echo "pasta de dados inválida: '$DATA'" >&2; exit 1; }
ssh $H "PS4=$PS4 DATA=$DATA python3 -" <<'PY'
import ftplib, io, json, os, subprocess, sys
ps4 = os.environ['PS4']
base = os.environ['DATA'] + '/shader-cache'
work = os.path.expanduser('~/ocarina-ps4/sync-missing')
xlate = os.path.expanduser('~/ocarina-ps4/ext/ocarina-recomp/build/xlate/totk_xlate')
os.makedirs(f'{work}/in', exist_ok=True)
os.makedirs(f'{work}/out', exist_ok=True)
f = ftplib.FTP(); f.connect(ps4, 2121, timeout=60); f.login(); f.set_pasv(True)
# as faltas da última execução (a pasta missing/ pode ter milhares de restos, e o FTP do PS4 não lista tudo)
lst = io.BytesIO()
try:
    f.retrbinary(f'RETR {base}/missing.txt', lst.write)
except ftplib.all_errors:
    pass
keys = sorted({ln.split()[0] for ln in lst.getvalue().decode(errors='replace').splitlines() if len(ln.split()[0:1][0] if ln.split() else '') == 64})
print(f'{len(keys)} variantes faltando no console')
ok = 0
for k in keys:
    req = io.BytesIO(); f.retrbinary(f'RETR {base}/missing/{k}.req', req.write)
    with open(f'{work}/in/{k}.code', 'wb') as out:
        f.retrbinary(f'RETR {base}/missing/{k}.code', out.write)
    d = json.loads(req.getvalue())
    d['job']['code'] = f'{work}/in/{k}.code'
    d['out'] = f'{work}/out'
    r = subprocess.run([xlate, '--serve'], input=(json.dumps(d, separators=(',', ':')) + '\n').encode(),
                       capture_output=True, timeout=300)
    line = r.stdout.decode(errors='replace').strip().splitlines()[:1]
    if not line or not line[0].startswith('ok'):
        print(f'{k}: o tradutor falhou: {line[0] if line else r.stderr.decode(errors="replace")[-200:]}')
        continue
    for ext in ('spv', 'res.json'):
        p = f'{work}/out/{k}.{ext}'
        if os.path.exists(p):
            with open(p, 'rb') as src:
                f.storbinary(f'STOR {base}/{k}.{ext}', src)
    for ext in ('req', 'code'):
        try: f.delete(f'{base}/missing/{k}.{ext}')
        except ftplib.error_perm: pass
    ok += 1
    print(f'{k}: traduzida e enviada')
f.quit()
print(f'fim: {ok} de {len(keys)} sincronizadas')
PY
