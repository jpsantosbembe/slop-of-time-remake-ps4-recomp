#!/usr/bin/env python3
"""Fase 0, prova 4: escolhe funções do OoT já traduzidas para C pelo lifter (dkb-pc-recomp/recomp/gen.py) que rodam
sozinhas, e gera lift_tests.c para a sonda: o mesmo arquivo roda no IdeaPad (x86-64-v2) e no PS4 (btver2), e os
resultados (hash do estado da CPU e da memória depois da função) têm de ser iguais.

Sozinha quer dizer: não chama outra função do jogo (nenhum f_xxx(c)), não usa serviço do runtime (rt_svc, rt_dispatch,
rt_spin_gate, rt_brk, acesso exclusivo, contador do sistema) e não tem laço (nenhum goto para um rótulo já visto),
porque os registradores de entrada são artificiais e um laço com contagem lixo não termina.
  make_lift_test.py <saida_do_gen> <lift_tests.c> [máximo]"""
import re, sys
from pathlib import Path

out_dir, dst = Path(sys.argv[1]), Path(sys.argv[2])
limit = int(sys.argv[3]) if len(sys.argv) > 3 else 400

FN = re.compile(r'^void (f_[0-9a-f]+)\(Cpu \*c\) \{\n(.*?)^\}\n', re.M | re.S)
CALL = re.compile(r'\bf_[0-9a-f]+\(c\)')
BAD = re.compile(r'\brt_(svc|dispatch|spin_gate|brk|ticks|tick_hz|ns_to|get_fp|set_fp)|ldx|stx|excl|tpidr|cntv|cntp|'
                 r'host_|mem_|g_[a-z]|GUEST_|\bc->pc\b')
LABEL = re.compile(r'^\s*(L_[0-9a-f]+):', re.M)
GOTO = re.compile(r'goto (L_[0-9a-f]+);')

picked, seen = [], 0
for chunk in sorted(out_dir.glob('*/*_[0-9][0-9][0-9][0-9].c')):
    text = chunk.read_text()
    for m in FN.finditer(text):
        name, body = m.group(1), m.group(2)
        seen += 1
        lines = body.count('\n')
        if not (12 <= lines <= 220) or CALL.search(body) or BAD.search(body):
            continue
        # laço = goto para um rótulo que já apareceu antes do goto
        labels_before, loop = set(), False
        for tok in re.finditer(r'(L_[0-9a-f]+):|goto (L_[0-9a-f]+);', body):
            if tok.group(1):
                labels_before.add(tok.group(1))
            elif tok.group(2) in labels_before:
                loop = True
                break
        if loop or 'return;' not in body:
            continue
        uses_mem = bool(re.search(r'\b(LD|ST)(8|16|32|64|128)\b', body))
        uses_simd = 'v[' in body or 'simd' in body or 'fp' in body
        picked.append((name, body, uses_mem, uses_simd, chunk.parent.name))
        if len(picked) >= limit:
            break
    if len(picked) >= limit:
        break

with open(dst, 'w') as f:
    f.write('// Gerado por tools/make_lift_test.py a partir de código do jogo traduzido. Não versionar.\n')
    f.write('#include "cpu.h"\n#include "simd.h"\n\n')
    for name, body, *_ in picked:
        f.write(f'static void {name}(Cpu *c) {{\n{body}}}\n\n')
    f.write('typedef void (*LiftFn)(Cpu *);\n')
    f.write(f'const int lift_count = {len(picked)};\n')
    f.write('const LiftFn lift_fns[] = {\n' + ''.join(f'    {n},\n' for n, *_ in picked) + '};\n')
    f.write('const char *const lift_names[] = {\n' + ''.join(f'    "{mod}:{n}",\n' for n, _, _, _, mod in picked) + '};\n')
mem = sum(p[2] for p in picked); simd = sum(p[3] for p in picked)
print(f'{len(picked)} funções escolhidas de {seen} (com memória {mem}, com SIMD/FP {simd}) -> {dst}')
