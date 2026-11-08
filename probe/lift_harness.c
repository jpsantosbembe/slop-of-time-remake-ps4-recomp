/* ocarina-ps4 / Fase 0, prova 4: funções do OoT traduzidas para C (lift_tests.c, gerado por tools/make_lift_test.py)
 * rodando com entradas artificiais. Cada função recebe o mesmo estado de CPU e a mesma arena de memória, num endereço
 * fixo (no runtime, endereço do guest = endereço do host), e o resultado é o hash do estado da CPU e da arena depois
 * dela. O IdeaPad (x86-64-v2) e o PS4 (btver2) têm de dar a mesma lista de hashes.
 * Uma função que acessa memória fora da arena termina em falta de página: conta como "falta", nos dois lados igual. */
#include <setjmp.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "cpu.h"

extern const int lift_count;
extern void (*const lift_fns[])(Cpu *);
extern const char *const lift_names[];

volatile int g_fnprof = 0;
void fnprof_hit(uint64_t a, uint64_t lr, uint64_t x0) { (void)a; (void)lr; (void)x0; }

jmp_buf lift_jb;
volatile int lift_active;
static volatile int lift_trap;

void rt_udf(Cpu *c, uint64_t pc) { (void)c; (void)pc; lift_trap = 1; longjmp(lift_jb, 2); }

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; }
    return h;
}

/* arena: `size` bytes em `base` (o mesmo endereço nas duas plataformas); devolve quantas terminaram ok */
int lift_run(uint8_t *base, size_t size, FILE *out, int *n_ok, int *n_fault, int *n_trap)
{
    /* padrão de 1 MiB repetido: palavras pares são ponteiros para dentro da arena (o código do jogo segue ponteiros
     * lidos da memória), ímpares são números pequenos; a arena fica no mesmo endereço nas duas plataformas */
    static uint64_t pattern[(1 << 20) / 8];
    uint32_t s = 0x1234567u;
    for (size_t i = 0; i < sizeof(pattern) / 8; i++) {
        s = s * 1664525u + 1013904223u;
        pattern[i] = (i & 1) ? (uint64_t)(s >> 20) : (uint64_t)(uintptr_t)base + ((uint64_t)(s >> 8) & (size - 1) & ~0xfull);
    }
    *n_ok = *n_fault = *n_trap = 0;
    for (int i = 0; i < lift_count; i++) {
        for (size_t o = 0; o < size; o += sizeof(pattern)) memcpy(base + o, pattern, sizeof(pattern));
        static Cpu c;
        memset(&c, 0, sizeof(c));
        for (int r = 0; r < 31; r++)   /* metade ponteiros para dentro da arena, metade números pequenos */
            c.x[r] = (r & 1) ? (uint64_t)(r * 37 + 5) : (uint64_t)(uintptr_t)base + 0x40000 + (uint64_t)r * 0x2000;
        c.sp = (uint64_t)(uintptr_t)base + size - 0x10000;
        for (int r = 0; r < 32; r++) c.v[r] = ((__uint128_t)(0x3ff0000000000000ull + r) << 64) | (0x40000000u + (uint32_t)r * 0x1000);
        lift_trap = 0;
        const char *res;
        int k = setjmp(lift_jb);
        if (k == 0) {
            lift_active = 1;
            lift_fns[i](&c);
            lift_active = 0;
            res = "ok";
            (*n_ok)++;
        } else {
            lift_active = 0;
            res = lift_trap ? "trap" : "falta";
            if (lift_trap) (*n_trap)++; else (*n_fault)++;
        }
        uint64_t h = 0xcbf29ce484222325ull;
        h = fnv(h, c.x, sizeof(c.x));
        h = fnv(h, &c.sp, 8);
        h = fnv(h, &c.nzcv, 8);
        h = fnv(h, c.v, sizeof(c.v));
        uint64_t hm = fnv(0xcbf29ce484222325ull, base, size);
        fprintf(out, "%s %s %016llx %016llx\n", lift_names[i], res, (unsigned long long)h, (unsigned long long)hm);
    }
    return *n_ok;
}

#ifndef __ORBIS__
/* referência no Linux: a mesma arena no mesmo endereço fixo */
#define _GNU_SOURCE
#include <sys/mman.h>
#include <unistd.h>
#define LIFT_ARENA_ADDR 0x0F00000000ull
#define LIFT_ARENA_SIZE (4u << 20)
#include <signal.h>
static void on_fault(int sig) { (void)sig; if (lift_active) longjmp(lift_jb, 1); _exit(3); }
int main(int argc, char **argv)
{
    void *a = mmap((void *)LIFT_ARENA_ADDR, LIFT_ARENA_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (a != (void *)LIFT_ARENA_ADDR) { perror("mmap arena"); return 1; }
    struct sigaction sa = { .sa_handler = on_fault, .sa_flags = SA_NODEFER };
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    if (getenv("LIFT_DAZ")) {   /* reproduz o MXCSR com DAZ|FTZ (subnormais tratados como zero) */
        unsigned csr;
        __asm__ volatile("stmxcsr %0" : "=m"(csr));
        csr |= 0x8040;
        __asm__ volatile("ldmxcsr %0" :: "m"(csr));
    }
    FILE *out = fopen(argc > 1 ? argv[1] : "lift_linux.txt", "w");
    int ok, fault, trap;
    lift_run(a, LIFT_ARENA_SIZE, out, &ok, &fault, &trap);
    fclose(out);
    printf("lift: %d funções: %d ok, %d falta de página, %d trap\n", lift_count, ok, fault, trap);
    return 0;
}
#endif
