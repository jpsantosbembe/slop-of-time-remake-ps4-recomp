#!/usr/bin/env python3
"""Abre um app instalado no PS4 pela rede, sem tocar no console. Roda no Mac, só com a biblioteca padrão.

    ps4_lancar.py [TITLE_ID] [--ps4 192.168.1.99] [--seco] [--proc SceShellUI] [--espera 30]

Sem TITLE_ID, pega o do carregador de dev com `ssh ideapad3i 'python3 ~/ocarina-ps4/game_info.py --campo ps4_tid_dev'`.

Como funciona: pelo ps4debug (porta 744) acha o SceShellUI e a base do libSceSystemService.sprx nele; baixa o mesmo
.sprx pelo FTP do GoldHEN (2121, que entrega o SELF já decriptado), acha na tabela dinâmica o deslocamento de
sceLncUtilLaunchApp pelo NID e confere os primeiros bytes na memória do processo. Então instala (ou reaproveita) o
stub de RPC do ps4debug no ShellUI, pede o usuário em primeiro plano (sceUserServiceGetForegroundUser), grava o title
ID e um LncAppParam num buffer alocado no processo e chama sceLncUtilLaunchApp(tid, NULL, &param). Depois espera
aparecer um processo com esse title ID. Sai com 0 quando o app abriu (ou já estava aberto).

--seco: resolve tudo e imprime o que chamaria, sem chamar nada no console.
"""
import argparse
import base64
import ftplib
import hashlib
import io
import json
import os
import socket
import struct
import subprocess
import sys
import time

CMD_MAGIC = 0xFFAABBCC
CMD_VERSION = 0xBD000001
CMD_PROC_LIST = 0xBDAA0001
CMD_PROC_READ = 0xBDAA0002
CMD_PROC_WRITE = 0xBDAA0003
CMD_PROC_MAPS = 0xBDAA0004
CMD_PROC_INSTALL = 0xBDAA0005
CMD_PROC_CALL = 0xBDAA0006
CMD_PROC_INFO = 0xBDAA000A
CMD_PROC_ALLOC = 0xBDAA000B
CMD_PROC_FREE = 0xBDAA000C
OK = 0x80000000

CACHE = os.path.expanduser('~/.cache/ps4_lancar.json')


class PS4Debug:
    """Cliente mínimo do protocolo do ps4debug 1.3: uma conexão TCP por comando."""

    def __init__(self, host, port=744):
        self.host, self.port = host, port

    def _cmd(self, code, body=b'', data=b'', timeout=15):
        s = socket.create_connection((self.host, self.port), timeout=timeout)
        s.sendall(struct.pack('<III', CMD_MAGIC, code, len(body)) + body)
        st = struct.unpack('<I', self._rx(s, 4))[0]
        if st != OK:
            s.close()
            raise RuntimeError(f'ps4debug: comando {code:#x} falhou com {st:#x}')
        if data:
            s.sendall(data)
            st = struct.unpack('<I', self._rx(s, 4))[0]
            if st != OK:
                s.close()
                raise RuntimeError(f'ps4debug: dados do comando {code:#x} recusados ({st:#x})')
        return s

    @staticmethod
    def _rx(s, n):
        b = b''
        while len(b) < n:
            c = s.recv(n - len(b))
            if not c:
                raise EOFError('ps4debug fechou a conexão')
            b += c
        return b

    def version(self):
        s = socket.create_connection((self.host, self.port), timeout=10)
        s.sendall(struct.pack('<III', CMD_MAGIC, CMD_VERSION, 0))
        n = struct.unpack('<I', self._rx(s, 4))[0]
        v = self._rx(s, n).decode(errors='replace')
        s.close()
        return v

    def procs(self):
        s = self._cmd(CMD_PROC_LIST)
        n = struct.unpack('<I', self._rx(s, 4))[0]
        out = []
        for _ in range(n):
            e = self._rx(s, 36)
            out.append((struct.unpack('<I', e[32:])[0], e[:32].split(b'\0')[0].decode(errors='replace')))
        s.close()
        return out

    def info(self, pid):
        s = self._cmd(CMD_PROC_INFO, struct.pack('<I', pid))
        e = self._rx(s, 188)
        s.close()
        z = lambda b: b.split(b'\0')[0].decode(errors='replace')
        return {'pid': struct.unpack_from('<I', e)[0], 'name': z(e[4:44]), 'path': z(e[44:108]),
                'title_id': z(e[108:124]), 'content_id': z(e[124:188])}

    def maps(self, pid):
        s = self._cmd(CMD_PROC_MAPS, struct.pack('<I', pid))
        n = struct.unpack('<I', self._rx(s, 4))[0]
        out = []
        for _ in range(n):
            e = self._rx(s, 58)
            st, en, off, prot = struct.unpack('<QQQH', e[32:])
            out.append((e[:32].split(b'\0')[0].decode(errors='replace'), st, en, off, prot))
        s.close()
        return out

    def read(self, pid, addr, n):
        s = self._cmd(CMD_PROC_READ, struct.pack('<IQI', pid, addr, n))
        b = self._rx(s, n)
        s.close()
        return b

    def write(self, pid, addr, data):
        self._cmd(CMD_PROC_WRITE, struct.pack('<IQI', pid, addr, len(data)), data).close()

    def alloc(self, pid, n):
        s = self._cmd(CMD_PROC_ALLOC, struct.pack('<II', pid, n))
        a = struct.unpack('<Q', self._rx(s, 8))[0]
        s.close()
        return a

    def free(self, pid, addr, n):
        self._cmd(CMD_PROC_FREE, struct.pack('<IQI', pid, addr, n)).close()

    def install_rpc(self, pid):
        s = self._cmd(CMD_PROC_INSTALL, struct.pack('<I', pid))
        a = struct.unpack('<Q', self._rx(s, 8))[0]
        s.close()
        return a

    def call(self, pid, stub, rip, *args, timeout=60):
        regs = list(args) + [0] * (6 - len(args))
        s = self._cmd(CMD_PROC_CALL, struct.pack('<IQQ6Q', pid, stub, rip, *regs), timeout=timeout)
        _pid, rax = struct.unpack('<IQ', self._rx(s, 12))
        s.close()
        return rax


# --- ELF do PS4 (SELF já decriptado pelo FTP do GoldHEN) ---------------------------------------------------------

NID_SUFFIX = bytes.fromhex('518D64A635DED8C1E6B039B1C3E55230')


def nid(name):
    # 8 primeiros bytes do SHA-1(nome + sufixo), lidos como u64 little-endian e codificados big-endian em base64
    # (alfabeto '+-'). Conferido contra a tabela de exports do próprio .sprx: só essa ordem bate.
    h = hashlib.sha1(name.encode() + NID_SUFFIX).digest()[:8][::-1]
    return base64.b64encode(h, altchars=b'+-').decode().rstrip('=')


def sprx_exports(d):
    """{nid: (vaddr, file_offset)} das funções exportadas."""
    phoff = struct.unpack_from('<Q', d, 0x20)[0]
    phnum = struct.unpack_from('<H', d, 0x38)[0]
    ph = [struct.unpack_from('<IIQQQQQQ', d, phoff + i * 56) for i in range(phnum)]
    dyn = next(p for p in ph if p[0] == 2)
    dld = next(p for p in ph if p[0] == 0x61000000)
    loads = [p for p in ph if p[0] == 1]
    tags = {}
    for i in range(0, dyn[5], 16):
        t, v = struct.unpack_from('<QQ', d, dyn[2] + i)
        tags.setdefault(t, v)
    strt = dld[2] + tags[0x61000035]
    symt = dld[2] + tags[0x61000039]
    out = {}
    for i in range(0, tags[0x6100003F], 24):
        nm, info, _o, shn, val, _sz = struct.unpack_from('<IBBHQQ', d, symt + i)
        if not shn or not val or (info & 15) != 2:
            continue
        name = d[strt + nm:d.index(b'\0', strt + nm)].decode(errors='replace').split('#')[0]
        fo = next((p[2] + val - p[3] for p in loads if p[3] <= val < p[3] + p[5]), None)
        out[name] = (val, fo)
    return out


def ftp_get(host, path):
    f = ftplib.FTP()
    f.connect(host, 2121, timeout=30)
    f.login()
    b = io.BytesIO()
    f.retrbinary('RETR ' + path, b.write)
    f.quit()
    return b.getvalue()


def module_base(maps, name):
    segs = [m for m in maps if m[0] == name]
    if not segs:
        raise RuntimeError(f'{name} não está carregado no processo')
    return min(m[1] for m in segs)


def resolve(ps4, dbg, pid, maps, module, funcs):
    d = ftp_get(ps4, '/system/common/lib/' + module)
    if d[:4] != b'\x7fELF':
        raise RuntimeError(f'{module} não veio decriptado pelo FTP')
    ex = sprx_exports(d)
    base = module_base(maps, module)
    out = {}
    for fn in funcs:
        n = nid(fn)
        if n not in ex:
            raise RuntimeError(f'{fn} (NID {n}) não está nos exports de {module}')
        va, fo = ex[n]
        addr = base + va
        live = dbg.read(pid, addr, 32)
        if live != d[fo:fo + 32]:
            raise RuntimeError(f'{fn}: bytes em {addr:#x} não batem com o {module} do FTP')
        out[fn] = addr
    return base, out


def running_with_tid(dbg, tid, pids=None):
    """(pid, nome) do processo com esse title ID, olhando todos os pids (ou só os dados) em paralelo."""
    from concurrent.futures import ThreadPoolExecutor
    # cada comando do ps4debug custa ~0,25 s e o servidor atende um por vez; os apps abrem depois do SceShellUI, então
    # só olha os pids acima do dele (e qualquer eboot), em vez dos ~60 processos do sistema
    allp = dbg.procs()
    shell = max((p for p, n in allp if n == 'SceShellUI'), default=0)
    procs = [(p, n) for p, n in allp if (pids is None and (p > shell or 'eboot' in n)) or (pids and p in pids)]

    def tid_of(pn):
        try:
            return dbg.info(pn[0])['title_id']
        except Exception:
            return None
    with ThreadPoolExecutor(8) as ex:
        for pn, t in zip(procs, ex.map(tid_of, procs)):
            if t == tid:
                return pn
    return None


def load_cache():
    try:
        with open(CACHE) as f:
            return json.load(f)
    except Exception:
        return {}


def get_stub(dbg, ps4, pid):
    """Reaproveita o stub de RPC de uma execução anterior (mesmo pid) para não criar uma thread nova por chamada."""
    c = load_cache()
    k = f'{ps4}:{pid}'
    a = c.get(k)
    if a:
        try:
            if dbg.read(pid, a, 4) == b'RSTB':
                return a, False
        except Exception:
            pass
    a = dbg.install_rpc(pid)
    c = {k2: v for k2, v in c.items() if not k2.startswith(ps4 + ':')}
    c[k] = a
    os.makedirs(os.path.dirname(CACHE), exist_ok=True)
    with open(CACHE, 'w') as f:
        json.dump(c, f)
    return a, True


def default_tid():
    r = subprocess.run(['ssh', '-o', 'ConnectTimeout=8', 'ideapad3i',
                        'python3 ~/ocarina-ps4/game_info.py --campo ps4_tid_dev'],
                       capture_output=True, text=True, timeout=30)
    tid = r.stdout.strip()
    if r.returncode or not tid:
        sys.exit(f'não consegui o title ID pelo ideapad3i ({r.stderr.strip()}); passe como argumento')
    return tid


def main():
    ap = argparse.ArgumentParser(description='Abre um app do PS4 pela rede via ps4debug.')
    ap.add_argument('tid', nargs='?')
    ap.add_argument('--ps4', default='192.168.1.99')
    ap.add_argument('--proc', default='SceShellUI', help='processo do sistema onde chamar (padrão SceShellUI)')
    ap.add_argument('--seco', action='store_true', help='só resolve e imprime, sem chamar nada')
    ap.add_argument('--espera', type=float, default=30, help='segundos esperando o app aparecer')
    a = ap.parse_args()
    tid = (a.tid or default_tid()).strip()
    if len(tid) != 9 or not tid.isalnum():
        sys.exit(f'title ID estranho: {tid!r}')

    dbg = PS4Debug(a.ps4)
    print(f'ps4debug {dbg.version()} em {a.ps4}; title ID {tid}')

    already = running_with_tid(dbg, tid)
    if already:
        print(f'{tid} já está rodando (pid {already[0]}, {already[1]}); nada a fazer')
        return 0

    pid = next((p for p, n in dbg.procs() if n == a.proc), None)
    if pid is None:
        sys.exit(f'processo {a.proc} não encontrado')
    maps = dbg.maps(pid)
    _, ss = resolve(a.ps4, dbg, pid, maps, 'libSceSystemService.sprx', ['sceLncUtilLaunchApp'])
    _, us = resolve(a.ps4, dbg, pid, maps, 'libSceUserService.sprx', ['sceUserServiceGetForegroundUser'])
    launch, fguser = ss['sceLncUtilLaunchApp'], us['sceUserServiceGetForegroundUser']
    print(f'{a.proc} pid {pid}: sceLncUtilLaunchApp={launch:#x} sceUserServiceGetForegroundUser={fguser:#x}')

    if a.seco:
        print(f'[seco] chamaria: sceUserServiceGetForegroundUser(&u); '
              f'sceLncUtilLaunchApp("{tid}", NULL, &{{sz=32, user_id=u, app_opt=0, crash_report=0, check_flag=0}})')
        return 0

    antes = {q for q, _ in dbg.procs()}
    stub, novo = get_stub(dbg, a.ps4, pid)
    print(f'stub RPC em {stub:#x} ({"instalado agora" if novo else "reaproveitado"})')
    buf = dbg.alloc(pid, 0x1000)
    try:
        # buf+0x000: int user; buf+0x100: title ID; buf+0x200: LncAppParam (32 bytes com o alinhamento do clang)
        dbg.write(pid, buf, b'\xff' * 4)
        r = dbg.call(pid, stub, fguser, buf) & 0xFFFFFFFF
        user = struct.unpack('<I', dbg.read(pid, buf, 4))[0]
        if r != 0 or user in (0xFFFFFFFF, 0):
            raise RuntimeError(f'sceUserServiceGetForegroundUser -> {r:#x}, user {user:#x} (ninguém logado?)')
        print(f'usuário em primeiro plano: {user:#x}')
        dbg.write(pid, buf + 0x100, tid.encode() + b'\0')
        dbg.write(pid, buf + 0x200, struct.pack('<IIIIQII', 32, user, 0, 0, 0, 0, 0))
        r = dbg.call(pid, stub, launch, buf + 0x100, 0, buf + 0x200) & 0xFFFFFFFF
    finally:
        try:
            dbg.free(pid, buf, 0x1000)
        except Exception as e:
            print(f'aviso: não liberei o buffer {buf:#x}: {e}')
    print(f'sceLncUtilLaunchApp("{tid}") -> {r:#x}')
    if r & 0x80000000:
        print('erro do LncUtil (0x8094000x é comum quando o app não pode abrir agora)')
        return 1

    fim = time.time() + a.espera
    while time.time() < fim:
        p = running_with_tid(dbg, tid, {q for q, _ in dbg.procs()} - antes)
        if p:
            print(f'{tid} abriu: pid {p[0]} ({p[1]}), appId {r:#x}')
            return 0
        time.sleep(1)
    print(f'a chamada retornou {r:#x}, mas nenhum processo de {tid} apareceu em {a.espera:.0f} s')
    return 1


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (RuntimeError, OSError, EOFError) as e:
        sys.exit(f'falhou: {e}')
