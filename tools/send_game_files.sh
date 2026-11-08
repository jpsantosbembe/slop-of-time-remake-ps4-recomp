#!/bin/bash
# Envia do IdeaPad ao PS4 o que o jogo precisa na pasta de dados (game_info.py --campo ps4_data_dir) além do RomFS (que já está lá): exefs/, romfs-index.bin
# (do build PS4), o cache de shaders gerado com as respostas do PS4 (tools/prepare_shaders_ps4.sh), o
# shader-prepared.bin e o totk_xlate.id. Arquivos de mesmo tamanho no console são pulados.
#   send_game_files.sh [--no-cache]
set -eo pipefail
ssh ideapad3i "NOCACHE=${1:-} python3 -" <<'PY'
import ftplib, os, time
import subprocess
H = os.path.expanduser('~/ocarina-ps4')
D = subprocess.run(['python3', '-I', H + '/game_info.py', '--campo', 'ps4_data_dir'], capture_output=True, text=True,
                   check=True).stdout.strip()
assert D.startswith('/data/') and len(D) > 6, D
M = H + '/ext/ocarina-recomp'
O = H + '/ps4-shaders'
files = [(f'{M}/build/exefs/{n}', f'{D}/exefs/{n}') for n in sorted(os.listdir(f'{M}/build/exefs'))]
files += [(H + '/ext/ocarina-recomp-orbis/build/oot111-orbis/romfs-index.bin', f'{D}/romfs-index.bin'),
          (O + '/totk_xlate.id', f'{D}/totk_xlate.id'),
          (O + '/shader-prepared.bin', f'{D}/shader-prepared.bin')]
if os.environ.get('NOCACHE') != '--no-cache':
    files.append((O + '/shader-cache/shader-cache.pack', f'{D}/shader-cache/shader-cache.pack'))
f = ftplib.FTP(); f.connect('192.168.1.99', 2121, timeout=300); f.login(); f.set_pasv(True)
for d in ('/data/slop', D, f'{D}/exefs', f'{D}/shader-cache'):
    try: f.mkd(d)
    except ftplib.error_perm: pass
for src, dst in files:
    size = os.path.getsize(src)
    try:
        # pula só arquivo grande de mesmo tamanho: um pequeno (totk_xlate.id, shader-prepared.bin) muda de conteúdo
        # sem mudar de tamanho, e mandá-lo de novo custa nada
        if size > (64 << 20) and f.size(dst) == size % (1 << 32):   # o FTP do PS4 informa o tamanho em 32 bits
            print(f'{dst}: igual, pulado'); continue
    except ftplib.error_perm:
        pass
    t = time.time()
    with open(src, 'rb') as fh:
        f.storbinary(f'STOR {dst}.tmp', fh, blocksize=1 << 20)
    try: f.delete(dst)
    except ftplib.error_perm: pass
    f.rename(f'{dst}.tmp', dst)
    got = f.size(dst)
    dt = time.time() - t
    print(f'{dst}: {got} bytes em {dt:.1f} s ({size / max(dt, 0.1) / 1e6:.0f} MB/s)')
    assert got == size % (1 << 32), f'{dst}: tamanho diferente no console'
f.quit()
PY
