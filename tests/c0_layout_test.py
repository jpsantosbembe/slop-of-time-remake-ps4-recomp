"""Exercise the real C0 writer/slot mapper with authored data; no game, Vulkan or SDL.

python tests/c0_layout_test.py --cc clang
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, declaration):
    start = source.index(declaration)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='clang')
    args = parser.parse_args()
    source = (ROOT / 'runtime/vk_exec_draw.c').read_text(encoding='utf-8')
    constants = '\n'.join(re.findall(r'^#define (?:DYN_C0|DYN_N|NVN_STAGE_\w+) .+$', source, re.M))
    fixture = '''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "gfx.h"
#define VK_C0_BYTES NVN2_C0_BYTES
static const Nvn2C0Layout *c0_lay;
static const uint32_t (*c0_push)[NVN2_ROOT_WORDS];
static void rt_fatal(const char *fmt, ...) { (void)fmt; abort(); }
'''.replace('static void rt_fatal', 'void rt_fatal') + constants + '''
static struct { uint64_t addr, size; } dyn_lists[6][DYN_N];
'''
    fixture += '\n'.join(function(source, name) for name in
                         ('static void c0_table(', 'void vk_c0_fill(', 'static int ssbo_slot('))
    fixture += '''
int main(void) {
    p_bind_buf storage[16] = {0}, uniform[16] = {0};
    uint32_t dispatch[3] = {1, 2, 3};
    unsigned checks = 0;
    const int stages[] = {3, 4, NVN_STAGE_VS};
    for (unsigned phase = 0; phase < 2; phase++) for (unsigned t = 0; t < 3; t++) {
        int stage = stages[t];
        memset(dyn_lists, 0, sizeof dyn_lists);
        for (int i = 0; i < 16; i++)
            storage[i] = (p_bind_buf){stage, i, 0x123400000000ull + (uint64_t)i * 65536, 0x2000 + i * 32};
        if (phase) for (int i = 0; i < DYN_N; i++) {
            dyn_lists[stage][i].addr = 0x567800000000ull + (uint64_t)i * 65536;
            dyn_lists[stage][i].size = 0x8000 + i * 64;
        }
        uint8_t bank[VK_C0_BYTES];
        vk_c0_fill(bank, stage, storage, uniform, dispatch, 0);
        unsigned base = stage == 3 ? 0x670 : 0x770;
        for (int i = 0; i < 16; i++) {
            uint64_t address; uint32_t size;
            memcpy(&address, bank + base + 16 * i, 8);
            memcpy(&size, bank + base + 16 * i + 8, 4);
            assert(address == storage[i].addr); assert(size == storage[i].size); checks += 2;
            GfxShaderRes res = {.cbuf_slot = 0, .handle = (base + 16 * i) / 4}; int ubo;
            assert(ssbo_slot(&res, stage, &ubo) == i && !ubo); checks++;
        }
        if (phase) {
            assert(memcmp(bank + DYN_C0, dyn_lists[stage], sizeof dyn_lists[stage]) == 0); checks++;
        }
    }
    for (int stage = 0; stage <= 4; stage += 4) {
        GfxShaderRes res = {.cbuf_slot = 0, .handle = DYN_C0 / 4}; int ubo;
        assert(ssbo_slot(&res, stage, &ubo) == -1); checks++;
    }
    // A valid native compute block fits before the list; an oversized one is rejected without writing.
    Nvn2C0Layout layout = {.ubos = 15};
    uint32_t roots[NVN2_ROOT_TABLES][NVN2_ROOT_WORDS] = {{0}};
    for (int i = 0; i < NVN2_ROOT_TABLES; i++) layout.roots[i] = NVN2_ROOT_WORDS;
    c0_lay = &layout; c0_push = roots;
    memset(dyn_lists, 0, sizeof dyn_lists);
    dyn_lists[NVN_STAGE_CS][0].addr = 0x567800000000ull;
    uint8_t bank[VK_C0_BYTES];
    vk_c0_fill(bank, NVN_STAGE_CS, storage, uniform, dispatch, 0);
    assert(memcmp(bank + DYN_C0, dyn_lists[NVN_STAGE_CS], sizeof dyn_lists[0]) == 0); checks++;
    layout.ubos++;
    assert(!nvn2_c0_pack(bank, DYN_C0, &layout, roots, storage, uniform)); checks++;
    printf("C0 layout PASS: %u checks\\n", checks);
}
'''
    with tempfile.TemporaryDirectory(prefix='c0-layout-') as tmp:
        path = Path(tmp) / 'fixture.c'
        path.write_text(fixture, encoding='utf-8')
        exe = Path(tmp) / 'fixture.exe'
        subprocess.run([args.cc, '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-I', str(ROOT / 'runtime'), str(path), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
