#!/usr/bin/env python3
"""Fala com o carregador de desenvolvimento ("<jogo> (dev)") no PS4. Roda no Mac.

    dev_reload.py [reload|ping] [--ps4 192.168.1.99] [--port 9026] [--wait 15] [--dados /data/slop/<title id>]

Grava <pasta de dados>/dev-host.txt no console ("ip_do_Mac porta") e escuta a porta: o carregador conecta a cada 2 s,
manda uma linha de estado e recebe o comando. "reload" reinicia o app, que relê o <pasta de dados>/game.elf.
Pasta de dados: --dados, SLOP_DATA_DIR, ou game_info.py --campo ps4_data_dir no IdeaPad.
Se o app não estiver aberto, nada conecta e o script avisa.
"""
import argparse
import ftplib
import io
import os
import socket
import subprocess
import sys


def data_dir(arg):
    d = arg or os.environ.get('SLOP_DATA_DIR') or subprocess.run(
        ['ssh', 'ideapad3i', 'python3 -I ~/ocarina-ps4/game_info.py --campo ps4_data_dir'],
        capture_output=True, text=True, check=True).stdout.strip()
    if not d.startswith('/data/') or len(d) <= 6:
        sys.exit(f'pasta de dados inválida: {d!r}')
    return d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cmd', nargs='?', default='reload', choices=['reload', 'ping'])
    ap.add_argument('--ps4', default='192.168.1.99')
    ap.add_argument('--port', type=int, default=9026)
    ap.add_argument('--wait', type=float, default=15)
    ap.add_argument('--dados')
    a = ap.parse_args()
    data = data_dir(a.dados)

    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.connect((a.ps4, 9))
    me = u.getsockname()[0]
    u.close()
    f = ftplib.FTP()
    f.connect(a.ps4, 2121, timeout=30)
    f.login()
    f.storbinary(f'STOR {data}/dev-host.txt', io.BytesIO(f'{me} {a.port}\n'.encode()))
    f.quit()

    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('0.0.0.0', a.port))
    srv.listen(1)
    srv.settimeout(a.wait)
    try:
        c, peer = srv.accept()
    except socket.timeout:
        sys.exit(f'o carregador não conectou em {a.wait:.0f} s: o app do carregador ("<jogo> (dev)") está aberto?')
    finally:
        srv.close()
    c.settimeout(5)
    print('carregador:', c.recv(200).decode(errors='replace').strip())
    c.sendall((a.cmd + '\n').encode())
    try:
        print('resposta:', c.recv(200).decode(errors='replace').strip())
    except socket.timeout:
        print('resposta: (nenhuma)')
    c.close()


if __name__ == '__main__':
    main()
