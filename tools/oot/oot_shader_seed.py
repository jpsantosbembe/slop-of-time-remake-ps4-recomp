"""Semente offline do cache de shaders do Ocarina of Time (Switch 2, 1.1.1), sem ninguém precisar jogar.

O jogo entrega ao runtime cada estágio NVN 2 como (bloco de controle, código Ampere) por nvnProgramSetShaders /
nvnProgramBuilderSetShaders (runtime/nvn_api_obj.c prog_set_shaders). O runtime rebaixa o estágio para Maxwell
(runtime/nvn2_lower.cpp) e só então o tradutor o vê; os registros de programa de seed-programs.bin são os de DEPOIS do
rebaixamento. Por isso esta ferramenta só faz o inventário (acha os estágios crus nos arquivos do jogo, exatamente os
bytes que o jogo passaria) e grava seed-nvn2.bin; o próprio executável (`ocarina --seed-nvn2`,
runtime/gfx_shader.c gfx_shader_seed_nvn2) rebaixa cada estágio com o código do runtime e escreve seed-programs.bin e
seed-answers.bin com a variante padrão (todos os slots de textura 2D, entradas de vértice e saídas de cor float). Depois,
`ocarina --prepare-shaders` traduz tudo para o cache e tools/pack_shader_cache.py o empacota.

Onde estão os estágios (RomFS 1.1.1; o main e os outros módulos não têm nenhum):
    BNSH          dentro de BFSHA (Shader/, Layout2Shader/, os .bfsha do Shader/ApplicationPackage) e dos efeitos
                  (Effect/*.vsetb.byml.zs); programa -> estágios em +8 (vértice), +40 (fragmento), +48 (compute),
                  +64 (task), +72 (mesh); ShaderCode +8: offset do controle, offset do código, tamanho do código, tamanho do controle
    sharcb (BAHS) pacotes agl/gsys/terreno/folhagem; o registro <u64 offset do código no pool, u32 tamanho do controle,
                  i32 0x68> fica 0x68 bytes antes de cada controle
    NDSR          agl_ndsr*.pck: tabela de entradas <u32, offset do controle, u32, offset do código> em +0x10
    NDVE          Movie/Ndve: o código segue o controle quando o controle traz o próprio tamanho (+0x30)
Conferido contra um DKB_SHADER_DUMP de uma jogada (--compare-dump): o que o jogo registra e não está aqui é contado.

seed-nvn2.bin: "OOTNVN2S", u32 versão 1, u32 contagem; registros: u32 tamanho do controle, u32 tamanho do código,
u8 prioridade (0 título, 1 aquecimento, 2 resto), 3 bytes zero, controle, código. Dado do jogo: fica em build/.

    .venv/bin/python tools/oot/oot_shader_seed.py build/oot111 [--romfs build/romfs] [--no-run]
    .venv/bin/python tools/oot/oot_shader_seed.py build/oot111 --compare-dump <pasta do DKB_SHADER_DUMP>

Sem --no-run, roda `ocarina --seed-nvn2` na pasta do executável (gera seed-programs.bin e seed-answers.bin) e, sem
--no-play, junta a elas as variantes que jogadas já gravaram ali (shader-programs.bin + shader-answers.bin).
"""
import argparse
import compression.zstd as zstd
import hashlib
import json
import struct
import subprocess
import sys
import time
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from shader_census import sarc  # noqa: E402

MAGIC = struct.pack('<II', 0x98761234, 0xC9)   # runtime/gfx_shader.c gfx_shader_is_nvn2
ZSTD = b'\x28\xb5\x2f\xfd'
DIRS = ('Shader/', 'Layout2Shader/', 'Effect/', 'Lib/', 'Movie/')   # os únicos com controles NVN 2 (varredura completa)
STAGE_SLOTS = {8: 'vs', 40: 'ps', 48: 'cs', 64: 'task', 72: 'mesh'}  # ShaderProgram: ponteiro por estágio; 16-32, 56 nunca
SEED_MAGIC, SEED_VERSION = b'OOTNVN2S', 1


def rt_sizes(control):
    """gfx_shader.c nvn2_sizes: o tamanho do controle e do código (com as constantes c[0x1]) que o runtime lê."""
    ctl_size, length, c1_end = struct.unpack_from('<I', control, 0x30)[0], *struct.unpack_from('<I', control, 0x38), \
        struct.unpack_from('<I', control, 0x44)[0]
    ctl_size = ctl_size or 0xd8
    if c1_end > length and c1_end - length <= 0x10000:
        length = c1_end
    if not (0x40 <= ctl_size <= 0x10000 and 0 < length <= 0x400000):
        return None
    return ctl_size, length


def stage_key(control, code):
    """O que identifica um estágio para o runtime (os bytes que gfx_shader_nvn2_hash cobre)."""
    ctl_size, length = rt_sizes(control)
    return hashlib.sha1(control[:ctl_size] + code[:length]).digest()


class Inventory:
    def __init__(self):
        self.stages = {}            # chave -> [controle, código, prioridade, origem]
        self.found = Counter()      # por tipo de contêiner: estágios achados (com repetição)
        self.bad = Counter()

    def add(self, kind, origin, prio, control, code):
        sizes = rt_sizes(control) if len(control) >= 0x48 else None
        if not sizes or control[:8] != MAGIC:
            self.bad[kind + ': controle inválido'] += 1
            return
        ctl_size, length = sizes
        if len(control) < ctl_size or len(code) < length:
            self.bad[kind + ': fora do arquivo'] += 1
            return
        self.found[kind] += 1
        key = stage_key(control, code)
        s = self.stages.get(key)
        if s is None:
            self.stages[key] = [control, code[:length], prio, origin]
        elif prio < s[2]:
            s[2], s[3] = prio, origin

    def bnsh(self, origin, prio, data):
        """Um BNSH completo; offsets relativos a ele."""
        if data[12:14] != b'\xff\xfe' or data[96:100] != b'grsc':
            self.bad['bnsh: cabeçalho'] += 1
            return
        size = struct.unpack_from('<I', data, 28)[0]
        data = data[:size]
        count, variations = struct.unpack_from('<IQ', data, 124)
        for i in range(count):
            program = struct.unpack_from('<Q', data, variations + i * 64 + 16)[0]
            for slot in range(8, 80, 8):
                at = struct.unpack_from('<Q', data, program + slot)[0]
                if not at:
                    continue
                if slot not in STAGE_SLOTS:
                    self.bad[f'bnsh: estágio em +{slot}'] += 1
                    continue
                ctl_off, code_off, code_size, ctl_size = struct.unpack_from('<QQII', data, at + 8)
                control = data[ctl_off:ctl_off + max(ctl_size, 0x100)]
                need = rt_sizes(control)[1] if control[:8] == MAGIC and rt_sizes(control) else code_size
                self.add('bnsh', origin, prio, control, data[code_off:code_off + max(code_size, need)])

    def sharcb(self, origin, prio, data):
        """Pacote agl (BAHS): registro <u64 código no pool, u32 tamanho do controle, i32 +0x68> antes de cada controle."""
        start, pool_size, pool = struct.unpack_from('<3I', data, 20)
        at = start
        while (at := data.find(MAGIC, at, pool)) >= 0:
            rec = at - 0x68
            code_off, ctl_size, rel = struct.unpack_from('<QIi', data, rec)
            if rel != 0x68 or code_off >= pool_size:
                self.bad['sharcb: sem registro'] += 1
            else:
                control = data[at:at + max(ctl_size, 0x100)]
                need = (rt_sizes(control) or (0, 0))[1]
                self.add('sharcb', origin, prio, control, data[pool + code_off:min(pool + code_off + need, pool + pool_size)])
            at += 8

    def ndsr(self, origin, prio, data):
        """Pacote NDSR (agl_ndsr*.pck): u32 contagem em +0xc, entradas de 16 bytes em +0x10 <u32, u32 offset do
        controle, u32, u32 offset do código>; as que não apontam para um controle NVN 2 são outros recursos."""
        count = struct.unpack_from('<I', data, 12)[0]
        for i in range(min(count, (len(data) - 16) // 16)):
            _, ctl_off, _, code_off = struct.unpack_from('<4I', data, 16 + 16 * i)
            control = data[ctl_off:ctl_off + 0x100]
            if control[:8] == MAGIC and rt_sizes(control):
                self.add('ndsr', origin, prio, control, data[code_off:code_off + rt_sizes(control)[1]])

    def ndve(self, origin, prio, data):
        """Pacote NDVE dos filmes (Movie/Ndve): o código segue o controle, quando o controle diz o próprio tamanho (+0x30).
        Os controles sem tamanho não têm código achável aqui."""
        at = 0
        while (at := data.find(MAGIC, at)) >= 0:
            control = data[at:at + 0x100]
            ctl_size = struct.unpack_from('<I', control, 0x30)[0]
            if ctl_size and rt_sizes(control):
                control = data[at:at + ctl_size]
                self.add('ndve', origin, prio, control, data[at + ctl_size:at + ctl_size + rt_sizes(control)[1]])
            else:
                self.bad['ndve: controle sem tamanho'] += 1
            at += 8

    def visit(self, origin, prio, data, decode, depth=0):
        if data[:4] == b'SARC' and depth < 4:
            for member, raw in sarc(data).items():
                self.visit(origin + '/' + member, prio, decode(raw) if raw[:4] == ZSTD else raw, decode, depth + 1)
            return
        if data[:4] == b'BAHS':
            self.sharcb(origin, prio, data)
            return
        if data[:4] in (b'NDSR', b'NDVE'):
            (self.ndsr if data[:4] == b'NDSR' else self.ndve)(origin, prio, data)
            return
        if MAGIC not in data:
            return
        pos, inside = 0, 0
        while (pos := data.find(b'BNSH\0\0\0\0', pos)) >= 0:
            size = struct.unpack_from('<I', data, pos + 28)[0]
            self.bnsh(origin, prio, data[pos:pos + size])
            inside += data[pos:pos + size].count(MAGIC)
            pos += max(size, 8)
        if data.count(MAGIC) > inside:   # controles fora de BNSH (NDSR, NDVE): sem formato conhecido aqui
            self.bad[f'fora de BNSH/sharcb ({data[:4].decode("latin-1")})'] += data.count(MAGIC) - inside


def priority(name):
    """0: pacotes que o boot e a tela-título carregam; 1: o resto dos pacotes de sistema e UI; 2: modelos e efeitos."""
    if name.startswith(('Shader/ApplicationPackage', 'Lib/', 'Layout2Shader/', 'Movie/')):
        return 0
    if name.startswith('Shader/') and 'Model.' not in name:
        return 1
    return 2


def inventory(romfs):
    index = json.loads((romfs / 'index.json').read_text(encoding='utf-8'))
    entries = {r['path']: r for r in index['files']}
    inv = Inventory()
    with (romfs / 'romfs.bin').open('rb') as stream:
        def read(name):
            row = entries[name]
            stream.seek(row['offset'])
            return stream.read(row['size'])

        dictionaries = {}
        for raw in sarc(zstd.decompress(read('Pack/ZsDic.pack.zs'))).values():
            d = zstd.ZstdDict(raw)
            dictionaries[d.dict_id] = d

        def decode(raw):
            info = zstd.get_frame_info(raw)
            return zstd.decompress(raw, zstd_dict=dictionaries[info.dictionary_id] if info.dictionary_id else None)

        for name in sorted(entries):
            if name.startswith(DIRS):
                raw = read(name)
                inv.visit(name, priority(name), decode(raw) if raw[:4] == ZSTD else raw, decode)
    return inv


def write_seed(inv, path):
    order = sorted(inv.stages.values(), key=lambda s: (s[2], s[3]))
    with open(path, 'wb') as f:
        f.write(SEED_MAGIC + struct.pack('<II', SEED_VERSION, len(order)))
        for control, code, prio, _ in order:
            f.write(struct.pack('<IIB3x', len(control), len(code), prio) + control + code)
    return Counter(s[2] for s in order)


def compare_dump(inv, folder):
    """Quantos estágios que o jogo registrou de fato (DKB_SHADER_DUMP: <fnv>.ctl + <fnv>.bin) o inventário tem."""
    missing = []
    dumps = sorted(folder.glob('*.ctl'))
    for p in dumps:
        control, code = p.read_bytes(), p.with_suffix('.bin').read_bytes()
        if stage_key(control, code) not in inv.stages:
            missing.append(p.stem)
    print(f'dump: {len(dumps)} estágios registrados pelo jogo, {len(dumps) - len(missing)} no inventário, '
          f'{len(missing)} fora')
    for m in missing[:40]:
        print('  fora:', m)
    return missing


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('build', type=Path, help='pasta do executável (build/oot111)')
    ap.add_argument('--romfs', type=Path, default=ROOT / 'build' / 'romfs', help='romfs.bin + index.json')
    ap.add_argument('--compare-dump', type=Path, help='pasta de um DKB_SHADER_DUMP para conferir a cobertura')
    ap.add_argument('--no-run', action='store_true', help='só grava seed-nvn2.bin')
    ap.add_argument('--no-play', action='store_true', help='não junta as variantes gravadas em jogo (shader-answers.bin)')
    a = ap.parse_args()
    t0 = time.time()
    inv = inventory(a.romfs)
    print(f'inventário: {len(inv.stages)} estágios distintos ({dict(inv.found)} com repetição) em '
          f'{time.time() - t0:.1f} s')
    for k, v in inv.bad.most_common():
        print(f'  ignorados: {v} {k}')
    if a.compare_dump:
        compare_dump(inv, a.compare_dump)
        return 0
    by = write_seed(inv, a.build / 'seed-nvn2.bin')
    print(f'{a.build / "seed-nvn2.bin"}: prioridade 0 {by[0]}, 1 {by[1]}, 2 {by[2]}')
    if a.no_run:
        return 0
    exe = a.build / ('ocarina.exe' if sys.platform == 'win32' else 'ocarina')
    rc = subprocess.call([str(exe.resolve()), '--seed-nvn2'], cwd=a.build)
    if rc == 0 and not a.no_play:
        add_play(a.build)
    return rc


def add_play(build):
    """Junta à semente as variantes que uma jogada já gravou (shader-programs.bin + shader-answers.bin, mesmos formatos,
    gfx_shader.c "answers met in play"): as respostas reais delas (tipos de textura e imagem, atributos, saídas) que a
    variante padrão não cobre. Os registros de jogada vêm com prioridade 0."""
    play_p, play_a = build / 'shader-programs.bin', build / 'shader-answers.bin'
    if not (play_p.is_file() and play_a.is_file()):
        return
    p, ans = play_p.read_bytes(), play_a.read_bytes()
    seed_a = (build / 'seed-answers.bin').read_bytes()
    if p[:12] != b'TOTKSPRG' + struct.pack('<I', 1) or ans[:16] != seed_a[:16]:
        print(f'{play_a}: outra versão de registro; a jogada fica fora da semente')
        return
    rec = struct.unpack_from('<I', ans, 12)[0]
    body = ans[16:len(ans) - (len(ans) - 16) % rec]
    with open(build / 'seed-programs.bin', 'ab') as f:
        f.write(p[16:])
    with open(build / 'seed-answers.bin', 'ab') as f:
        f.write(body)
    print(f'jogada: +{len(body) // rec} variantes gravadas em jogo ({play_a.name})')


if __name__ == '__main__':
    sys.exit(main())
