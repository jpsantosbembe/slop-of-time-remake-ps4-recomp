#!/usr/bin/env python3
"""Ponte do tradutor de shaders (ocarina-ps4, modo "on the fly pela rede").

O PS4 não roda .NET nem cria processos, então o runtime do jogo, com OCARINA_XLATE_REMOTE=ip:porta, conecta por TCP
aqui em vez de abrir o tradutor. Para cada conexão esta ponte abre um "totk_xlate --serve" e repassa os bytes nos dois
sentidos, sem interpretar: o protocolo (uma linha de pedido, uma linha de resposta) é o mesmo do processo filho.

    xlate_bridge.py [--port 9050] [--xlate <caminho do totk_xlate>]

Escuta em todas as interfaces. Conexões e quedas vão para a saída padrão.
"""
import argparse
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

DEFAULT_XLATE = Path.home() / 'ocarina-ps4/ext/ocarina-recomp/build/xlate/totk_xlate'


def pump(src_read, dst_write, close_dst):
    """Copia até o fim de um lado e fecha o outro."""
    try:
        while True:
            data = src_read(65536)
            if not data:
                break
            dst_write(data)
    except OSError:
        pass
    finally:
        try:
            close_dst()
        except OSError:
            pass


def serve(conn, addr, xlate, n):
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    t0 = time.time()
    proc = subprocess.Popen([str(xlate), '--serve'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, bufsize=0)
    print(f'[{n}] {addr[0]}:{addr[1]} conectou; tradutor pid {proc.pid}', flush=True)

    def to_proc(data):
        proc.stdin.write(data)
        proc.stdin.flush()

    a = threading.Thread(target=pump, args=(conn.recv, to_proc, proc.stdin.close), daemon=True)
    b = threading.Thread(target=pump, args=(proc.stdout.read, conn.sendall,
                                            lambda: conn.shutdown(socket.SHUT_WR)), daemon=True)
    a.start(), b.start()
    a.join(), b.join()
    proc.kill()
    proc.wait()
    conn.close()
    print(f'[{n}] {addr[0]} desconectou depois de {time.time() - t0:.0f} s', flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', type=int, default=9050)
    ap.add_argument('--xlate', type=Path, default=DEFAULT_XLATE)
    args = ap.parse_args()
    if not args.xlate.is_file():
        sys.exit(f'tradutor não encontrado: {args.xlate}')
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('0.0.0.0', args.port))
    srv.listen(16)
    print(f'ponte do tradutor na porta {args.port} ({args.xlate})', flush=True)
    n = 0
    while True:
        conn, addr = srv.accept()
        n += 1
        threading.Thread(target=serve, args=(conn, addr, args.xlate, n), daemon=True).start()


if __name__ == '__main__':
    main()
