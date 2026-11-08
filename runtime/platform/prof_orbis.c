// Perfilador por amostragem do PS4 (TOTK_PROFILE=<arquivo>): o equivalente do prof_win.c, no mesmo formato de texto, para
// o tools/prof_report.py ler (com o ocarina.elf NÃO stripado no lugar do totk.map).
//
// Mecanismo. O kernel do PS4 não deixa um processo parar uma thread e ler os registradores dela (sem ptrace, sem
// thr_suspend nem thr_kill exportados pela libkernel; o setitimer manda o sinal ao processo, não a uma thread). O que
// existe é o pthread_kill da libkernel. Então uma thread "prof" acorda a cada 1 ms e manda um sinal (SIGPROF, o 27 do
// FreeBSD; TOTK_PROFILE_SIG troca) a cada thread alvo; o tratador roda NA thread alvo, lê rip e rsp do ucontext (o
// layout do FreeBSD, deslocado no Orbis: calibrado no início pelo par cs = 0x43, ss = 0x3b, como o orbis-compat faz) e
// copia até STK_COPY bytes da pilha a partir do rsp, só onde ela está mapeada (pilha preguiçosa: orbis_lazy_stack_span;
// outras: sceKernelVirtualQuery). Ele não aloca, não toma trava e não escreve log. A thread prof, depois, procura na
// cópia os valores que caem no código do executável e cujo byte anterior é uma instrução de chamada (endereços de
// retorno verificados): a cadeia é rip + esses retornos, de dentro para fora. Sem frame pointer (o C gerado é -O1 e o
// runtime -O2), é uma heurística: um endereço de retorno velho que sobrou num quadro morto entra na cadeia, um quadro
// cujo retorno ficou fora da cópia se perde. A rip é exata.
//
// Alvos: a thread principal do guest, o render thread e o replay assíncrono (os papéis do prof.h, registrados por
// prof_sampler_thread NA thread, que é como se obtém o pthread_t). TOTK_PROFILE_MAIN=1 só a principal,
// TOTK_PROFILE_RENDER=1 só o render thread. Um sinal que demora mais de 2 ms para ser atendido (a thread estava fora
// da CPU ou numa chamada do kernel que não se interrompe) vira, quando chega, uma linha tstall com o tempo de espera e
// a cadeia onde a thread estava; enquanto não chega, a thread não recebe outro.
//
// Modos (os do prof_win.c):
//   TOTK_PROFILE=<arquivo>: contagem de rips e de cadeias, escrita no fim (prof_write) ou, com TOTK_PROFILE_FRAMES=lo-hi,
//     quando o present hi chega. Caminho relativo: <pasta de dados>/logs/<arquivo>.
//   TOTK_PROFILE_SLOW=<ms>: guarda as amostras do último ~1 s num anel e, depois de um present que veio <ms> ou mais depois
//     do anterior, escreve as amostras desse intervalo (mais os 20 ms antes) em <arquivo>.f<quadro>: uma linha
//     `stack 1 <rip> <retornos>...` por amostra, `tstack <tid> <ms na janela> <rip> ...` com a thread e o tempo, e
//     `tstall <tid> <ms> <ms parado> <rip> ...`. No máximo 64 arquivos.
// Cabeçalho próprio do PS4: `platform orbis`, `text_start`/`text_end` (onde o executável está; o report tira a base
// daí, pois o carregador de dev pode cair em outro endereço que 0x40000000) e `module <base> <tamanho> <nome>` dos
// módulos do sistema (libkernel e afins), para os rips fora do executável.
//
// Riscos: cada sinal interrompe a chamada de sistema em que a thread estiver; com SA_RESTART a maioria recomeça, mas
// esperas com prazo (_umtx_op, nanosleep, kevent) voltam com EINTR. As esperas do runtime (orbis.c) e do WSI do Mesa
// toleram isso (laços ou acordar espúrio), mas uma espera da Sony que não tolere aparece como erro. Por isso só as
// threads pedidas, e nenhuma por padrão sem TOTK_PROFILE.
#if defined(__ORBIS__)

#include <orbis_prefix.h>

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "prof.h"
#include "platform/platform.h"

extern int32_t sceKernelVirtualQuery(const void *addr, int32_t flags, void *info, size_t size);
extern int32_t sceKernelGetModuleList(uint32_t *array, size_t size, size_t *available);
extern int32_t sceKernelGetModuleInfo(uint32_t handle, void *info);
extern int pthread_getthreadid_np(void);
extern char __text_start[] __attribute__((weak));
extern char __eh_frame_start[] __attribute__((weak));
extern const char *orbis_data_dir(void) __attribute__((weak));
extern int orbis_lazy_stack_span(uint64_t a, uint64_t *end) __attribute__((weak));

#define ORBIS_SIGPROF 27     // FreeBSD; o SDK traz os números do Linux
#define PROF_DEPTH 24        // quadros por cadeia, a rip primeiro
#define STK_COPY (8u << 10)  // bytes de pilha copiados por amostra (como o tratador de falta: candidatos a retorno)
#define STALL_NS 2000000ull  // um sinal atendido depois disto vira tstall
#define LATE_NS 200000000ull // a primeira amostra de uma thread que não chega nisto vai para o log (uma vez)
#define RING_N 4096u         // TOTK_PROFILE_SLOW: ~1,3 s de três threads a 1 kHz
#define STALL_N 512u
#define SLOW_DUMPS 64
#define RIP_SLOTS (1u << 14) // contagem de rips (modo agregado), endereçamento aberto
#define RIP_TOP 4000
#define STK_SLOTS (1u << 12) // contagem de cadeias (modo agregado)

// O siginfo e o ucontext são os do FreeBSD; o mcontext começa em uc[mc] (8 no console, medido pelo orbis-compat e
// recalibrado aqui). Palavras do mcontext do FreeBSD amd64:
#define MC_RIP 20
#define MC_CS 21
#define MC_RFLAGS 22
#define MC_RSP 23
#define MC_SS 24

typedef struct {
    uint64_t start, end, off;
    int32_t prot, type;
    uint8_t flags;
    char name[32];
} KVQuery;   // a struct do sceKernelVirtualQuery (a mesma do orbis.c)

typedef struct {   // OrbisKernelModuleInfo do SDK (orbis/_types/kernel.h)
    size_t size;
    char name[256];
    struct { void *address; uint32_t size; int32_t prot; } seg[4];
    uint32_t nseg;
    uint8_t fingerprint[20];
} ModInfo;

enum { T_MAIN = PROF_ROLE_MAIN, T_RENDER = PROF_ROLE_RENDER, T_REPLAY = PROF_ROLE_REPLAY, T_SELF, NT };
static const char *const role_name[NT] = {"main", "render", "replay", "self"};

typedef struct {
    // registrado na própria thread (prof_sampler_thread)
    pthread_t pt;
    volatile uint32_t tid;
    volatile int have;
    // pedido e resposta: a thread prof põe gen e manda o sinal; o tratador preenche e põe done = gen
    volatile uint32_t gen, done;
    uint64_t sent_ns, hit_ns, rip, rsp;
    uint32_t ncopy;
    uint8_t stk[STK_COPY] __attribute__((aligned(16)));
    // só a thread prof
    uint32_t taken;      // último gen recolhido
    uint32_t seen_tid;   // a thread do papel na última volta (trocou: recomeça)
    int answered, dead;  // já respondeu alguma vez; desistiu dela (pthread_kill falhou)
    int warned;          // o primeiro pedido atrasou e isso foi para o log
    uint64_t n, in_exe;  // amostras recolhidas e quantas com a rip no executável
} Target;

typedef struct { uint64_t t; uint32_t tid, depth; uint64_t stall_ns; uint64_t rip[PROF_DEPTH]; } Sample;
typedef struct { uint64_t rip, n; } RipCount;
typedef struct { uint64_t hash, n; uint32_t depth; uint64_t rip[PROF_DEPTH]; } StackCount;

static Target tg[NT];
static volatile int mc = -1;          // índice do mcontext no ucontext (palavras); -1 até a calibração
static volatile int calibrating;
static int sig_no = ORBIS_SIGPROF;
static uintptr_t exe_lo, exe_hi;
static char out_path[512];
static int running;
static volatile int stop, written;
static PlatThread *sampler;
static int want[NT];                  // os papéis amostrados
static volatile uint64_t cur_frame;
static uint64_t frame_lo, frame_hi;   // TOTK_PROFILE_FRAMES=lo-hi
static int64_t world_lo, world_hi;    // TOTK_PROFILE_FRAMES=w<lo>-<hi> (s do relógio do mundo, em us)
static uint64_t slow_ns;              // TOTK_PROFILE_SLOW em ns, 0 desligado
static volatile int64_t dump_lo, dump_hi, dump_frame;   // o render thread pede uma janela, a thread prof escreve
static volatile int window_done;      // TOTK_PROFILE_FRAMES: o present hi passou (modo agregado escreve)

// Memória só com TOTK_PROFILE (malloc no início): a memória flexível do PS4 é física desde a criação, e um .bss grande
// pesaria sempre.
static Sample *ring, *stalls;
static uint64_t ring_n, stall_n;
static RipCount *rips;
static StackCount *stacks;
static uint64_t total, rips_dropped, stacks_dropped;

// ---- o tratador de sinal: só leitura do ucontext, uma cópia de pilha limitada ao que está mapeado e o aviso de pronto.

// Até onde dá para ler a partir de sp sem faltar, no máximo STK_COPY.
static uint32_t readable_from(uint64_t sp) {
    uint64_t want_end = sp + STK_COPY, end = sp;
    if (&orbis_lazy_stack_span && orbis_lazy_stack_span(sp, &end)) return (uint32_t)((end < want_end ? end : want_end) - sp);
    for (uint64_t a = sp; a < want_end;) {
        KVQuery q;
        memset(&q, 0, sizeof q);
        if (sceKernelVirtualQuery((void *)(uintptr_t)a, 0, &q, sizeof q) || q.start > a || q.end <= a || !(q.prot & 1)) break;
        a = end = q.end;
    }
    return (uint32_t)((end < want_end ? end : want_end) - sp);
}

static void prof_handler(int sig, siginfo_t *si, void *ucv) {
    (void)sig, (void)si;
    int saved = errno;
    const uint64_t *uc = ucv;
    if (calibrating && mc < 0) {   // a prova, na thread que chamou prof_sampler_start: acha o mcontext pelo par cs/ss
        // (com rflags plausível: IF e o bit 1 ligados), primeiro em uc[8], onde o orbis-compat o mediu
        for (int k = 0; k < 64; k++) {
            int i = k == 0 ? 8 : k == 8 ? 0 : k;
            if (uc[i + MC_CS] == 0x43 && uc[i + MC_SS] == 0x3b && (uc[i + MC_RFLAGS] & 0x202) == 0x202 &&
                uc[i + MC_RFLAGS] < 0x100000) {
                mc = i;
                break;
            }
        }
    }
    int m = mc;
    pthread_t self = pthread_self();
    for (int i = 0; i < NT && m >= 0; i++) {
        Target *t = &tg[i];
        if (!__atomic_load_n(&t->have, __ATOMIC_ACQUIRE) || !pthread_equal(t->pt, self)) continue;
        uint32_t g = __atomic_load_n(&t->gen, __ATOMIC_ACQUIRE);
        if (__atomic_load_n(&t->done, __ATOMIC_RELAXED) == g) continue;
        t->hit_ns = plat_now_ns();
        t->rip = uc[m + MC_RIP];
        t->rsp = uc[m + MC_RSP];
        uint32_t n = t->rsp ? readable_from(t->rsp) & ~7u : 0;
        memcpy(t->stk, (const void *)(uintptr_t)t->rsp, n);
        t->ncopy = n;
        __atomic_store_n(&t->done, g, __ATOMIC_RELEASE);
    }
    errno = saved;
}

// ---- a thread prof

// v é um endereço de retorno se a instrução que termina em v é uma chamada: E8 rel32, ou FF /2 (call r/m64) com o
// tamanho que o modrm dá. Lê até 8 bytes antes de v, dentro do executável (mapeado R|X pelo carregador).
static int is_return(uint64_t v) {
    if (v < exe_lo + 8 || v >= exe_hi) return 0;
    const uint8_t *p = (const uint8_t *)(uintptr_t)v;
    if (p[-5] == 0xE8) return 1;
    for (int k = 2; k <= 7; k++) {
        if (p[-k] != 0xFF) continue;
        uint8_t modrm = p[-k + 1], mod = modrm >> 6, reg = (modrm >> 3) & 7, rm = modrm & 7;
        if (reg != 2) continue;
        int len = 2;
        if (mod != 3 && rm == 4) {   // SIB
            len++;
            if (mod == 0 && (p[-k + 2] & 7) == 5) len += 4;
        }
        if (mod == 1) len += 1;
        else if (mod == 2 || (mod == 0 && rm == 5)) len += 4;
        if (len == k) return 1;
    }
    return 0;
}

static uint32_t chain_of(const Target *t, uint64_t *out) {
    uint32_t d = 0;
    out[d++] = t->rip;
    const uint64_t *w = (const uint64_t *)t->stk;
    for (uint32_t i = 0; i < t->ncopy / 8 && d < PROF_DEPTH; i++)
        if (w[i] >= exe_lo && w[i] < exe_hi && is_return(w[i])) out[d++] = w[i];
    return d;
}

static void count_rip(uint64_t rip) {
    uint32_t i = (uint32_t)((rip * 0x9E3779B97F4A7C15ull) >> 50);
    for (uint32_t k = 0; k < 64; k++, i = (i + 1) & (RIP_SLOTS - 1)) {
        if (rips[i].rip == rip || !rips[i].rip) {
            rips[i].rip = rip;
            rips[i].n++;
            return;
        }
    }
    rips_dropped++;
}

static void count_stack(const uint64_t *rip, uint32_t depth) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t d = 0; d < depth; d++) h = (h ^ rip[d]) * 0x100000001b3ull;
    h |= 1;
    uint32_t i = (uint32_t)(h >> 52);
    for (uint32_t k = 0; k < 64; k++, i = (i + 1) & (STK_SLOTS - 1)) {
        StackCount *s = &stacks[i];
        if (!s->hash) {
            s->hash = h, s->depth = depth;
            memcpy(s->rip, rip, depth * sizeof *rip);
        } else if (s->hash != h || s->depth != depth || memcmp(s->rip, rip, depth * sizeof *rip)) {
            continue;
        }
        s->n++;
        return;
    }
    stacks_dropped++;
}

// Recolhe a resposta do pedido atual de t, se chegou e ainda não foi recolhida.
static void take(Target *t) {
    uint32_t g = __atomic_load_n(&t->gen, __ATOMIC_RELAXED);
    if (t->taken == g || __atomic_load_n(&t->done, __ATOMIC_ACQUIRE) != g) return;
    t->taken = g;
    t->answered = 1;
    uint64_t chain[PROF_DEPTH];
    uint32_t d = chain_of(t, chain);
    uint64_t wait = t->hit_ns > t->sent_ns ? t->hit_ns - t->sent_ns : 0;
    total++;
    t->n++;
    if (t->rip >= exe_lo && t->rip < exe_hi) t->in_exe++;
    if (slow_ns) {
        Sample *s = &ring[ring_n++ & (RING_N - 1)];
        s->t = t->hit_ns, s->tid = t->tid, s->depth = d, s->stall_ns = wait;
        memcpy(s->rip, chain, d * sizeof *chain);
        if (wait > STALL_NS) {
            Sample *w = &stalls[stall_n++ & (STALL_N - 1)];
            *w = *s;
            w->t = t->sent_ns;
        }
    } else {
        count_rip(t->rip);
        count_stack(chain, d);
    }
}

static int in_window(void) {
    if (frame_hi && (cur_frame < frame_lo || cur_frame >= frame_hi)) return 0;
    if (world_hi) {
        int64_t w = frame_step_world_us();
        if (w < world_lo || w >= world_hi) return 0;
    }
    return 1;
}

static void write_header(FILE *f) {
    fprintf(f, "totk-prof 1\nplatform orbis\ntext_start %llx\ntext_end %llx\nperiod_us 1000\n", (unsigned long long)exe_lo,
            (unsigned long long)exe_hi);
    fprintf(f, "main %u render %u replay %u\n", prof_main_tid, prof_render_tid, prof_replay_tid);
    uint32_t h[256];
    size_t n = 0;
    if (sceKernelGetModuleList(h, sizeof h / sizeof *h, &n)) n = 0;
    for (size_t i = 0; i < n && i < 256; i++) {
        ModInfo mi;
        memset(&mi, 0, sizeof mi);
        mi.size = sizeof mi;
        if (sceKernelGetModuleInfo(h[i], &mi)) continue;
        uint64_t lo = ~0ull, hi = 0;
        for (uint32_t s = 0; s < mi.nseg && s < 4; s++) {
            uint64_t a = (uint64_t)(uintptr_t)mi.seg[s].address;
            if (!a) continue;
            if (a < lo) lo = a;
            if (a + mi.seg[s].size > hi) hi = a + mi.seg[s].size;
        }
        mi.name[255] = 0;
        if (hi > lo) fprintf(f, "module %llx %llx %s\n", (unsigned long long)lo, (unsigned long long)(hi - lo), mi.name);
    }
    for (int i = 0; i < NT; i++)
        if (want[i] && tg[i].have)
            fprintf(f, "thread %u %llu %llu %s\n", tg[i].tid, (unsigned long long)tg[i].n, (unsigned long long)tg[i].in_exe,
                    role_name[i]);
}

static void put_chain(FILE *f, const Sample *s) {
    for (uint32_t d = 0; d < s->depth; d++) fprintf(f, " %llx", (unsigned long long)s->rip[d]);
    fputc('\n', f);
}

static void slow_dump(void) {
    uint64_t lo = (uint64_t)dump_lo, hi = (uint64_t)dump_hi, frame = (uint64_t)dump_frame;
    static int dumps;
    char path[600];
    snprintf(path, sizeof path, "%s.f%llu", out_path, (unsigned long long)frame);
    FILE *f = dumps < SLOW_DUMPS ? fopen(path, "w") : NULL;
    if (f) {
        dumps++;
        write_header(f);
        fprintf(f, "frame %llu interval_ms %.1f\n", (unsigned long long)frame, (hi - lo) / 1e6 - 20);
        uint64_t first = ring_n > RING_N ? ring_n - RING_N : 0, kept = 0, oldest = 0;
        if (ring_n) oldest = ring[first & (RING_N - 1)].t;
        for (int pass = 0; pass < 2; pass++)
            for (uint64_t k = first; k < ring_n; k++) {
                const Sample *s = &ring[k & (RING_N - 1)];
                if (s->t < lo || s->t > hi) continue;
                if (pass) fprintf(f, "tstack %u %.1f", s->tid, ((double)s->t - (double)lo) / 1e6);
                else fprintf(f, "stack 1"), kept++;
                put_chain(f, s);
            }
        for (uint64_t k = stall_n > STALL_N ? stall_n - STALL_N : 0; k < stall_n; k++) {
            const Sample *s = &stalls[k & (STALL_N - 1)];
            if (s->t + s->stall_ns < lo || s->t > hi) continue;
            fprintf(f, "tstall %u %.1f %.1f", s->tid, ((double)s->t - (double)lo) / 1e6, s->stall_ns / 1e6);
            put_chain(f, s);
        }
        // o anel não cobre a janela inteira: o report avisa
        if (oldest > lo) fprintf(f, "ring_from_ms %.1f\n", ((double)oldest - (double)lo) / 1e6);
        fprintf(f, "samples %llu\n", (unsigned long long)kept);
        fclose(f);
        rt_log("prof: quadro lento %llu (%.1f ms), %llu amostras -> %s", (unsigned long long)frame, (hi - lo) / 1e6 - 20,
               (unsigned long long)kept, path);
    }
    __atomic_store_n(&dump_hi, 0, __ATOMIC_RELEASE);
}

static int cmp_rip(const void *a, const void *b) {
    uint64_t x = ((const RipCount *)a)->n, y = ((const RipCount *)b)->n;
    return (x < y) - (x > y);
}

static int cmp_stack(const void *a, const void *b) {
    uint64_t x = ((const StackCount *)a)->n, y = ((const StackCount *)b)->n;
    return (x < y) - (x > y);
}

// O perfil agregado, pela thread prof (já parada ou no fim da janela de quadros).
static void write_profile(void) {
    if (slow_ns || __atomic_exchange_n(&written, 1, __ATOMIC_ACQ_REL)) return;
    FILE *f = fopen(out_path, "w");
    if (!f) {
        rt_log("prof: não consegui escrever %s", out_path);
        return;
    }
    write_header(f);
    fprintf(f, "samples %llu\ndropped %llu\n", (unsigned long long)total, (unsigned long long)rips_dropped);
    int cnt = 0;
    for (uint32_t i = 0; i < RIP_SLOTS; i++)
        if (rips[i].rip) rips[cnt++] = rips[i];
    qsort(rips, cnt, sizeof *rips, cmp_rip);
    for (int i = 0; i < cnt && i < RIP_TOP; i++)
        fprintf(f, "rip %llx %llu\n", (unsigned long long)rips[i].rip, (unsigned long long)rips[i].n);
    int ns = 0;
    for (uint32_t i = 0; i < STK_SLOTS; i++)
        if (stacks[i].hash) stacks[ns++] = stacks[i];
    qsort(stacks, ns, sizeof *stacks, cmp_stack);
    for (int i = 0; i < ns; i++) {
        fprintf(f, "stack %llu", (unsigned long long)stacks[i].n);
        for (uint32_t d = 0; d < stacks[i].depth; d++) fprintf(f, " %llx", (unsigned long long)stacks[i].rip[d]);
        fputc('\n', f);
    }
    if (stacks_dropped) fprintf(f, "stacks_dropped %llu\n", (unsigned long long)stacks_dropped);
    fclose(f);
    rt_log("prof: %llu amostras, %d rips -> %s", (unsigned long long)total, cnt, out_path);
}

static void sampler_main(void *arg) {
    (void)arg;
    while (!__atomic_load_n(&stop, __ATOMIC_ACQUIRE)) {
        plat_wait_ns(1000000);
        if (__atomic_load_n(&dump_hi, __ATOMIC_ACQUIRE)) slow_dump();
        if (window_done) {
            write_profile();
            break;
        }
        for (int i = 0; i < NT; i++) take(&tg[i]);   // respostas atrasadas
        if (!in_window()) continue;
        uint64_t now = plat_now_ns();
        int sent[NT] = {0}, nsent = 0;
        for (int i = 0; i < NT; i++) {
            Target *t = &tg[i];
            if (!want[i] || !__atomic_load_n(&t->have, __ATOMIC_ACQUIRE)) continue;
            if (t->tid != t->seen_tid) {   // outra thread no papel: o pedido pendente era da anterior e não volta
                t->seen_tid = t->tid;
                t->taken = t->gen;
                t->answered = t->dead = t->warned = 0;
            }
            if (t->dead) continue;
            int dup = 0;   // dois papéis na mesma thread: um sinal só
            for (int j = 0; j < i; j++)
                if (want[j] && tg[j].have && !tg[j].dead && tg[j].tid == t->tid) dup = 1;
            if (dup) continue;
            if (t->taken != t->gen) {   // o pedido anterior ainda não foi atendido: continua esperando, sem desistir
                // (uma espera longa no kernel é justamente o que se procura; quando o sinal chegar vira tstall)
                if (!t->answered && !t->warned && now - t->sent_ns > LATE_NS) {
                    t->warned = 1;
                    rt_log("prof: a thread %s (tid %u) ainda não atendeu o primeiro sinal %d (%.0f ms); se nunca atender, "
                           "o sinal não chega a ela", role_name[i], t->tid, sig_no, (now - t->sent_ns) / 1e6);
                }
                continue;
            }
            t->sent_ns = now;
            __atomic_store_n(&t->gen, t->gen + 1, __ATOMIC_RELEASE);
            int e = pthread_kill(t->pt, sig_no);
            if (e) {
                t->dead = 1;
                rt_log("prof: pthread_kill na thread %s (tid %u) falhou (%d); deixo de amostrá-la", role_name[i], t->tid, e);
                continue;
            }
            sent[i] = 1, nsent++;
        }
        // espera curta pelas respostas; o que não chegar é recolhido num tick seguinte (e vira tstall)
        uint64_t until = now + STALL_NS;
        while (nsent) {
            int left = 0;
            for (int i = 0; i < NT; i++)
                if (sent[i]) {
                    take(&tg[i]);
                    if (tg[i].taken != tg[i].gen) left++;
                    else sent[i] = 0;
                }
            if (!left || plat_now_ns() >= until) break;
            sched_yield();
        }
    }
}

// ---- interface do prof.h

void prof_sampler_thread(int role) {
    if (role < 0 || role >= T_SELF) return;
    Target *t = &tg[role];
    __atomic_store_n(&t->have, 0, __ATOMIC_RELEASE);   // troca de thread: o tratador ignora a vaga enquanto isso
    t->pt = pthread_self();
    t->tid = plat_thread_id();
    __atomic_store_n(&t->have, 1, __ATOMIC_RELEASE);
}

int prof_sampler_start(void) {
    const char *p = cfg_get("TOTK_PROFILE");
    if (!p || !*p) return 0;
    if (p[0] == '/' || !&orbis_data_dir) snprintf(out_path, sizeof out_path, "%s", p);
    else snprintf(out_path, sizeof out_path, "%s/logs/%s", orbis_data_dir(), p);
    exe_lo = (uintptr_t)__text_start, exe_hi = (uintptr_t)__eh_frame_start;
    if (!exe_lo || exe_hi <= exe_lo) {
        rt_log("prof: sem os limites do executável (__text_start/__eh_frame_start); perfilador desligado");
        return 0;
    }
    const char *e = cfg_get("TOTK_PROFILE_SIG");
    if (e && atoi(e) > 0 && atoi(e) < 128) sig_no = atoi(e);
    int only_main = cfg_get("TOTK_PROFILE_MAIN") != NULL, only_render = !only_main && cfg_get("TOTK_PROFILE_RENDER") != NULL;
    want[T_MAIN] = !only_render;
    want[T_RENDER] = !only_main;
    want[T_REPLAY] = !only_main && !only_render;
    const char *sl = cfg_get("TOTK_PROFILE_SLOW");
    slow_ns = sl && atoi(sl) > 0 ? (uint64_t)atoi(sl) * 1000000ull : 0;
    const char *fr = cfg_get("TOTK_PROFILE_FRAMES");
    double wl, wh;
    if (fr && fr[0] == 'w' && sscanf(fr + 1, "%lf-%lf", &wl, &wh) == 2 && wl >= 0 && wl < wh)
        world_lo = (int64_t)(wl * 1e6), world_hi = (int64_t)(wh * 1e6);
    else if (fr && sscanf(fr, "%llu-%llu", (unsigned long long *)&frame_lo, (unsigned long long *)&frame_hi) != 2) frame_hi = 0;
    if (slow_ns) {
        ring = calloc(RING_N, sizeof *ring);
        stalls = calloc(STALL_N, sizeof *stalls);
    } else {
        rips = calloc(RIP_SLOTS, sizeof *rips);
        stacks = calloc(STK_SLOTS, sizeof *stacks);
    }
    if (slow_ns ? !ring || !stalls : !rips || !stacks) {
        rt_log("prof: sem memória para o perfilador");
        return 0;
    }

    struct sigaction sa;   // as constantes SA_* são as do FreeBSD (overlay signal.h), como no fault_install do orbis.c
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = prof_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
    if (sigaction(sig_no, &sa, NULL)) {
        rt_log("prof: sigaction(%d) falhou (errno %d); perfilador desligado", sig_no, errno);
        return 0;
    }
    // Prova na própria thread: o sinal chega, o mcontext é achado pelo par cs/ss e a rip cai no executável.
    Target *s = &tg[T_SELF];
    s->pt = pthread_self();
    s->tid = plat_thread_id();
    __atomic_store_n(&s->have, 1, __ATOMIC_RELEASE);
    s->sent_ns = plat_now_ns();
    __atomic_store_n(&s->gen, 1, __ATOMIC_RELEASE);
    calibrating = 1;
    int k = pthread_kill(s->pt, sig_no);
    for (int i = 0; i < 1000 && __atomic_load_n(&s->done, __ATOMIC_ACQUIRE) != 1; i++) plat_wait_ns(100000);
    calibrating = 0;
    __atomic_store_n(&s->have, 0, __ATOMIC_RELEASE);
    s->taken = 1;   // a prova não entra nas amostras
    if (k || s->done != 1 || mc < 0) {
        rt_log("prof: o sinal %d não voltou à própria thread (pthread_kill %d, resposta %u, mcontext %d); perfilador desligado",
               sig_no, k, s->done, mc);
        sa.sa_handler = SIG_IGN;   // não SIG_DFL: um sinal ainda pendente terminaria o processo
        sa.sa_flags = 0;
        sigaction(sig_no, &sa, NULL);
        return 0;
    }
    uint64_t chain[PROF_DEPTH];
    uint32_t d = chain_of(s, chain);
    rt_log("prof: sinal %d atendido em %.0f us, mcontext em uc[%d], rip %#llx (%s), %u bytes de pilha, %u quadros; "
           "amostrando%s%s%s a cada 1 ms%s -> %s", sig_no, (s->hit_ns - s->sent_ns) / 1e3, mc, (unsigned long long)s->rip,
           s->rip >= exe_lo && s->rip < exe_hi ? "no executável" : "fora do executável", s->ncopy, d,
           want[T_MAIN] ? " main" : "", want[T_RENDER] ? " render" : "", want[T_REPLAY] ? " replay" : "",
           slow_ns ? ", modo quadro lento" : "", out_path);
    sampler = plat_thread_new(sampler_main, NULL, "prof");
    if (!sampler) {
        rt_log("prof: a thread do perfilador não nasceu");
        return 0;
    }
    running = 1;
    return 1;
}

void prof_sampler_write(void) {
    if (!running || slow_ns || written) return;
    __atomic_store_n(&stop, 1, __ATOMIC_RELEASE);
    if (!plat_thread_join_ns(sampler, 2000000000ull)) return;   // presa: nada de contagem pela metade
    sampler = NULL;
    write_profile();
}

// perf_present, a cada present (render thread): o quadro da janela TOTK_PROFILE_FRAMES e o pedido de um arquivo de quadro
// lento (d: o intervalo desde o present anterior, 0 no primeiro).
void prof_sampler_frame(uint64_t frame, uint64_t now, uint64_t d) {
    cur_frame = frame;
    if (!running) return;
    if (!slow_ns && frame_hi && frame >= frame_hi) window_done = 1;
    if (slow_ns && d >= slow_ns && !__atomic_load_n(&dump_hi, __ATOMIC_ACQUIRE) && in_window()) {
        dump_lo = (int64_t)(now - d - 20000000ull);
        dump_frame = (int64_t)frame;
        __atomic_store_n(&dump_hi, (int64_t)now, __ATOMIC_RELEASE);
    }
}

#endif
