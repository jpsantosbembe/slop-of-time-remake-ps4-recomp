#!/usr/bin/env python3
"""Relatório do malloc_census: simboliza as pilhas (addr2line) e soma os bytes vivos (ou o pico) por subsistema.
Uso: mc_report.py ARQUIVO_MC [--peak] [--top N]
Subsistema = a primeira moldura do runtime (arquivo .c do runtime/) da pilha, depois de pular os embrulhos de alocação;
pilhas sem moldura do runtime vão pelo módulo (driver, libc, ...). A moldura mais funda de driver também é mostrada."""
import collections, os, re, subprocess, sys

args = sys.argv[1:]
peak = '--peak' in args
top = int(args[args.index('--top') + 1]) if '--top' in args else 40
path = [a for a in args if not a.startswith('--') and not a.isdigit()][0]

lines = open(path).read().splitlines()
head = lines[0]
rows = []
for ln in lines[1:]:
    tag = 'P' if peak else 'L'
    if not ln.startswith(tag + ' '):
        continue
    meta, _, st = ln.partition('|')
    f = meta.split()
    a = int(f[1]); b = int(f[3]); n = int(f[5])
    frames = []
    for tok in st.split():
        m = re.match(r'(.*)\+(0x[0-9a-f]+)$', tok)
        frames.append((m.group(1), int(m.group(2), 16)) if m else (tok, 0))
    rows.append((a, b, n, frames))

# addr2line por módulo, em lote
need = collections.defaultdict(set)
for _, _, _, fr in rows:
    for mod, off in fr:
        if mod.startswith('/') or mod.startswith('./'):
            need[mod].add(off)
sym = {}
for mod, offs in need.items():
    offs = sorted(offs)
    exe = mod
    base = 0
    if os.path.basename(mod) == 'ocarina':   # ET_EXEC: dladdr deu o offset a partir do 1o LOAD (0x200000)
        exe = os.environ.get('MC_EXE', os.path.expanduser('~/ocarina-ps4/dieta-run/ocarina')); base = 0x200000
    try:
        out = subprocess.run(['addr2line', '-f', '-C', '-s', '-e', exe] + [hex(o - 1 + base) for o in offs],
                             capture_output=True, text=True, timeout=300).stdout.splitlines()
    except Exception:
        out = []
    for i, o in enumerate(offs):
        fn = out[2 * i] if 2 * i < len(out) else '??'
        fl = out[2 * i + 1] if 2 * i + 1 < len(out) else '??'
        sym[(mod, o)] = (fn, fl.split(' ')[0].rsplit(':', 1)[0])

WRAP = re.compile(r'^(rt_|plat_|xmalloc|xcalloc|xrealloc|ralloc|rzalloc|linear_|gc_|util_|os_|vk_alloc|vk_zalloc|'
                  r'_mesa_hash|_mesa_set|u_vector|u_dynarray|stb_|cJSON|vk_object|vk_multialloc|__libc|operator new|'
                  r'std::|_ZNSt|_ZN9__gnu|malloc|calloc|realloc|plat_file_read|read_file|vma_|Vma|pack_read)')

def label(fr):
    names = []
    for mod, off in fr:
        fn, fl = sym.get((mod, off), ('??', '??'))
        names.append((os.path.basename(mod), fn, fl))
    # primeira moldura em arquivo do runtime (ocarina) que não seja embrulho
    for m, fn, fl in names:
        if m == 'ocarina' and fl.endswith(('.c', '.cpp')) and not WRAP.match(fn) and not fl.startswith('vk_mem_alloc'):
            return f'{fl}:{fn}'
    for m, fn, fl in names:
        if m != 'libc.so.6' and m != 'malloc_census.so':
            return f'[{m}] {fn}'
    return '[?]'

def driver_frame(fr):
    for mod, off in fr:
        b = os.path.basename(mod)
        if 'nvidia' in b or 'vulkan' in b or 'radv' in b or 'GLX' in b:
            fn, fl = sym.get((mod, off), ('??', '??'))
            return f'{b}:{fn}'
    return ''

by = collections.Counter(); by_mod = collections.Counter(); ex = {}
for a, b, n, fr in rows:
    l = label(fr)
    by[l] += a
    d = driver_frame(fr)
    by_mod['driver' if d else ('runtime' if any(os.path.basename(m) == 'ocarina' for m, _ in fr) else 'outro')] += a
    if l not in ex:
        ex[l] = ' <- '.join(f'{sym.get((m, o), ("?", "?"))[0]}' for m, o in fr[:10])
print(head)
print('por origem (MiB):', {k: round(v / 2**20, 1) for k, v in by_mod.items()})
tot = sum(by.values())
print(f'{"MiB":>8} {"%":>5}  {"pico" if peak else "vivo"} por subsistema (top {top}, soma {tot/2**20:.1f} MiB das 400 pilhas)')
for l, v in by.most_common(top):
    print(f'{v/2**20:8.1f} {100*v/tot:5.1f}  {l}')
    print(f'{"":15}{ex[l][:230]}')
