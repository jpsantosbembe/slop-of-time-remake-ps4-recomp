// malloc_census.so: LD_PRELOAD que conta o malloc vivo por pilha, por amostragem proporcional ao tamanho (como o
// tcmalloc): cada thread tem um contador de bytes com média MC_SAMPLE_KIB (padrão 64); a alocação que o cruza é
// amostrada (backtrace) e pesa s / (1 - exp(-s/R)). O total vivo é exato (malloc_usable_size).
// Saída: a cada MC_PERIOD segundos (padrão 10), MC_OUT.<n> com o total, o pico e as pilhas (módulo+offset) por bytes
// vivos estimados e pelo pico de cada pilha. Símbolos offline (tools/memprof/symbolize.py).
// Build: gcc -O2 -fPIC -shared -o malloc_census.so malloc_census.c -ldl -lm -lpthread
#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <malloc.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEPTH 24
#define NSTACK (1 << 16)
#define NPTR (1 << 21)

typedef struct {
    uint64_t hash;
    int n;
    void *pc[DEPTH];
    _Atomic int64_t live, peak, allocs;
} Stack;
typedef struct {
    void *p;
    uint32_t stack;
    int64_t w;
} PtrEnt;

static void *(*r_malloc)(size_t), *(*r_calloc)(size_t, size_t), *(*r_realloc)(void *, size_t);
static void (*r_free)(void *);
static int (*r_posix_memalign)(void **, size_t, size_t);
static void *(*r_aligned_alloc)(size_t, size_t), *(*r_memalign)(size_t, size_t);

static Stack *stacks;
static PtrEnt *ptrs;
static pthread_mutex_t stack_mu = PTHREAD_MUTEX_INITIALIZER;
#define NSTRIPE 64
static pthread_mutex_t ptr_mu[NSTRIPE];
static _Atomic int64_t total_live, total_peak, n_sampled_live;
static double R = 65536.0;
static int ready;
static __thread int in_hook;
static __thread int64_t countdown;
static __thread uint64_t rng;
static char boot_buf[1 << 16];
static size_t boot_used;

static uint64_t xs(void) {
    if (!rng) rng = (uint64_t)(uintptr_t)&rng ^ 0x9e3779b97f4a7c15ull ^ (uint64_t)time(NULL);
    rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17;
    return rng;
}
static int64_t next_gap(void) {
    double u = ((xs() >> 11) + 1) * (1.0 / 9007199254740993.0);
    return (int64_t)(-log(u) * R) + 1;
}

static void init(void) {
    static int once;
    if (once) return;
    once = 1;
    r_malloc = dlsym(RTLD_NEXT, "malloc");
    r_calloc = dlsym(RTLD_NEXT, "calloc");
    r_realloc = dlsym(RTLD_NEXT, "realloc");
    r_free = dlsym(RTLD_NEXT, "free");
    r_posix_memalign = dlsym(RTLD_NEXT, "posix_memalign");
    r_aligned_alloc = dlsym(RTLD_NEXT, "aligned_alloc");
    r_memalign = dlsym(RTLD_NEXT, "memalign");
}

static uint32_t stack_id(void **pc, int n) {
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < n; i++) h = (h ^ (uint64_t)(uintptr_t)pc[i]) * 1099511628211ull;
    if (!h) h = 1;
    pthread_mutex_lock(&stack_mu);
    uint32_t i = (uint32_t)(h >> 20) & (NSTACK - 1);
    for (int probe = 0; probe < NSTACK; probe++, i = (i + 1) & (NSTACK - 1)) {
        if (stacks[i].hash == h) break;
        if (!stacks[i].hash) {
            stacks[i].hash = h;
            stacks[i].n = n;
            memcpy(stacks[i].pc, pc, n * sizeof *pc);
            break;
        }
    }
    pthread_mutex_unlock(&stack_mu);
    return i;
}

static void ptr_put(void *p, uint32_t s, int64_t w) {
    uint64_t h = ((uint64_t)(uintptr_t)p >> 4) * 0x9e3779b97f4a7c15ull;
    pthread_mutex_t *m = &ptr_mu[(h >> 58) & (NSTRIPE - 1)];
    pthread_mutex_lock(m);
    uint32_t i = (uint32_t)(h >> 30) & (NPTR - 1);
    for (int probe = 0; probe < NPTR; probe++, i = (i + 1) & (NPTR - 1)) {
        if (!ptrs[i].p || ptrs[i].p == (void *)1) {
            ptrs[i].p = p, ptrs[i].stack = s, ptrs[i].w = w;
            break;
        }
    }
    pthread_mutex_unlock(m);
}
// The stripes share one table, so a stripe lock does not cover a probe sequence that crosses slots of another
// stripe: one global lock for the table instead (sampled events are rare; the lookup on free is the cost).
static pthread_mutex_t tab_mu = PTHREAD_MUTEX_INITIALIZER;

static void sample(void *p, size_t s) {
    void *pc[DEPTH + 2];
    int n = backtrace(pc, DEPTH + 2);
    n = n > 2 ? n - 2 : 0;   // drop sample() and the hook
    uint32_t id = stack_id(pc + 2, n > DEPTH ? DEPTH : n);
    double ps = 1.0 - exp(-(double)s / R);
    int64_t w = (int64_t)((double)s / (ps > 0 ? ps : 1e-9));
    pthread_mutex_lock(&tab_mu);
    ptr_put(p, id, w);
    pthread_mutex_unlock(&tab_mu);
    int64_t l = atomic_fetch_add(&stacks[id].live, w) + w;
    atomic_fetch_add(&stacks[id].allocs, 1);
    int64_t pk = atomic_load(&stacks[id].peak);
    while (l > pk && !atomic_compare_exchange_weak(&stacks[id].peak, &pk, l)) {}
    atomic_fetch_add(&n_sampled_live, 1);
}

static void on_alloc(void *p) {
    if (!p || !ready) return;
    size_t s = malloc_usable_size(p);
    int64_t t = atomic_fetch_add(&total_live, (int64_t)s) + (int64_t)s;
    int64_t pk = atomic_load(&total_peak);
    while (t > pk && !atomic_compare_exchange_weak(&total_peak, &pk, t)) {}
    if (in_hook) return;
    if (!countdown) countdown = next_gap();
    countdown -= (int64_t)s;
    if (countdown > 0) return;
    while (countdown <= 0) countdown += next_gap();
    in_hook = 1;
    sample(p, s);
    in_hook = 0;
}

static void on_free(void *p) {
    if (!p || !ready) return;
    atomic_fetch_sub(&total_live, (int64_t)malloc_usable_size(p));
    if (!atomic_load_explicit(&n_sampled_live, memory_order_relaxed)) return;
    uint64_t h = ((uint64_t)(uintptr_t)p >> 4) * 0x9e3779b97f4a7c15ull;
    pthread_mutex_lock(&tab_mu);
    uint32_t i = (uint32_t)(h >> 30) & (NPTR - 1);
    for (int probe = 0; probe < NPTR; probe++, i = (i + 1) & (NPTR - 1)) {
        if (!ptrs[i].p) break;
        if (ptrs[i].p == p) {
            atomic_fetch_sub(&stacks[ptrs[i].stack].live, ptrs[i].w);
            atomic_fetch_sub(&n_sampled_live, 1);
            ptrs[i].p = (void *)1;   // tombstone
            break;
        }
    }
    pthread_mutex_unlock(&tab_mu);
}

static int in_boot(void *p) { return (char *)p >= boot_buf && (char *)p < boot_buf + sizeof boot_buf; }

void *malloc(size_t n) {
    if (!r_malloc) {
        init();
        if (!r_malloc) {
            void *p = boot_buf + boot_used;
            boot_used += (n + 15) & ~15ul;
            return p;
        }
    }
    void *p = r_malloc(n);
    on_alloc(p);
    return p;
}
void *calloc(size_t a, size_t b) {
    if (!r_calloc) {
        size_t n = a * b;
        void *p = boot_buf + boot_used;   // dlsym's own calloc, before the real one is known
        boot_used += (n + 15) & ~15ul;
        memset(p, 0, n);
        if (!r_malloc && !in_hook) {
            in_hook = 1;
            init();
            in_hook = 0;
        }
        return p;
    }
    void *p = r_calloc(a, b);
    on_alloc(p);
    return p;
}
void free(void *p) {
    if (!p || in_boot(p)) return;
    if (!r_free) init();
    on_free(p);
    r_free(p);
}
void *realloc(void *p, size_t n) {
    if (in_boot(p)) {
        void *q = malloc(n);
        memcpy(q, p, n);
        return q;
    }
    if (!r_realloc) init();
    if (p) on_free(p);
    void *q = r_realloc(p, n);
    if (q) on_alloc(q);
    else if (p && n) on_alloc(p);   // failed: the old block stays
    return q;
}
int posix_memalign(void **out, size_t a, size_t n) {
    if (!r_posix_memalign) init();
    int r = r_posix_memalign(out, a, n);
    if (!r) on_alloc(*out);
    return r;
}
void *aligned_alloc(size_t a, size_t n) {
    if (!r_aligned_alloc) init();
    void *p = r_aligned_alloc(a, n);
    on_alloc(p);
    return p;
}
void *memalign(size_t a, size_t n) {
    if (!r_memalign) init();
    void *p = r_memalign(a, n);
    on_alloc(p);
    return p;
}

static int cmp_live(const void *a, const void *b) {
    int64_t x = atomic_load(&(*(Stack **)a)->live), y = atomic_load(&(*(Stack **)b)->live);
    return x < y ? 1 : x > y ? -1 : 0;
}
static int cmp_peak(const void *a, const void *b) {
    int64_t x = atomic_load(&(*(Stack **)a)->peak), y = atomic_load(&(*(Stack **)b)->peak);
    return x < y ? 1 : x > y ? -1 : 0;
}

static void write_pc(FILE *f, void *pc) {
    Dl_info di;
    if (dladdr(pc, &di) && di.dli_fname)
        fprintf(f, " %s+%#lx", di.dli_fname, (unsigned long)((char *)pc - (char *)di.dli_fbase));
    else fprintf(f, " ?%p", pc);
}

static void dump(int n) {
    const char *out = getenv("MC_OUT");
    if (!out) out = "/tmp/malloc_census";
    char path[512];
    snprintf(path, sizeof path, "%s.%d.%03d", out, (int)getpid(), n);
    in_hook = 1;
    FILE *f = fopen(path, "w");
    if (!f) {
        in_hook = 0;
        return;
    }
    static Stack *v[NSTACK];
    int k = 0;
    pthread_mutex_lock(&stack_mu);
    for (int i = 0; i < NSTACK; i++)
        if (stacks[i].hash) v[k++] = &stacks[i];
    pthread_mutex_unlock(&stack_mu);
    int64_t est = 0;
    for (int i = 0; i < k; i++) est += atomic_load(&v[i]->live);
    struct mallinfo2 mi = mallinfo2();
    fprintf(f, "total_live %lld total_peak %lld sampled_est %lld stacks %d sample_bytes %.0f arena %zu hblkhd %zu "
               "uordblks %zu fordblks %zu\n",
            (long long)atomic_load(&total_live), (long long)atomic_load(&total_peak), (long long)est, k, R, mi.arena,
            mi.hblkhd, mi.uordblks, mi.fordblks);
    qsort(v, k, sizeof *v, cmp_live);
    for (int i = 0; i < k && i < 400; i++) {
        fprintf(f, "L %lld P %lld A %lld |", (long long)atomic_load(&v[i]->live), (long long)atomic_load(&v[i]->peak),
                (long long)atomic_load(&v[i]->allocs));
        for (int j = 0; j < v[i]->n; j++) write_pc(f, v[i]->pc[j]);
        fputc('\n', f);
    }
    qsort(v, k, sizeof *v, cmp_peak);
    for (int i = 0; i < k && i < 400; i++) {
        fprintf(f, "P %lld L %lld A %lld |", (long long)atomic_load(&v[i]->peak), (long long)atomic_load(&v[i]->live),
                (long long)atomic_load(&v[i]->allocs));
        for (int j = 0; j < v[i]->n; j++) write_pc(f, v[i]->pc[j]);
        fputc('\n', f);
    }
    fclose(f);
    in_hook = 0;
}

static void *dumper(void *arg) {
    (void)arg;
    in_hook = 1;
    const char *e = getenv("MC_PERIOD");
    int period = e ? atoi(e) : 10;
    for (int n = 0;; n++) {
        sleep(period > 0 ? period : 10);
        dump(n);
    }
    return NULL;
}

__attribute__((constructor)) static void mc_start(void) {
    init();
    in_hook = 1;
    const char *e = getenv("MC_SAMPLE_KIB");
    if (e && atoi(e) > 0) R = atoi(e) * 1024.0;
    for (int i = 0; i < NSTRIPE; i++) pthread_mutex_init(&ptr_mu[i], NULL);
    stacks = r_calloc(NSTACK, sizeof *stacks);
    ptrs = r_calloc(NPTR, sizeof *ptrs);
    void *pc[4];
    backtrace(pc, 4);   // loads the unwinder now, outside a hook
    pthread_t t;
    pthread_create(&t, NULL, dumper, NULL);
    pthread_detach(t);
    in_hook = 0;
    ready = 1;
}
