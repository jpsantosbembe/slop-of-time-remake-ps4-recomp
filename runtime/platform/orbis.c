// Implementação do platform.h para o PS4 (Orbis OS: kernel derivado do FreeBSD 9, musl do OpenOrbis, libkernel da
// Sony), compilada com o bundle orbis-sdk-v1 e -march=btver2. Parte do linux.c (mesma ABI SysV x86-64, mesmas fibras),
// trocando o que o PS4 não tem: memória virtual por memória direta da Sony mapeada em endereço fixo (com acesso da
// GPU), userfaultfd por mprotect + SIGSEGV, futex pelo _umtx_op da libkernel, CLOCK_MONOTONIC pelo contador de tempo
// do processo, /proc e processos filhos por stubs.
//
// Desenho: ~/ocarina-ps4/fase1-orbis.md. Referência das APIs da Sony validada no console: ~/ocarina-ps4/probe/probe_ps4.c.
// O que está marcado "A CONFIRMAR NO CONSOLE" compila mas ainda não rodou no PS4.
//
// Só existe quando o alvo é o PS4: o build_game.py compila todo platform/*.c fora do Windows, e no Linux este arquivo
// tem de virar uma unidade vazia em vez de duplicar os símbolos do linux.c.
#if defined(__ORBIS__)

#include <orbis_prefix.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "platform.h"

// ---- libkernel e afins, com as assinaturas reais (as mesmas que a sonda usou no console). Declaradas aqui, e não
// via <orbis/libkernel.h>: o header do SDK erra algumas (sceKernelAvailableDirectMemorySize, por exemplo).
extern int32_t sceKernelReserveVirtualRange(void **addr, size_t len, int32_t flags, size_t align);
extern int32_t sceKernelAllocateDirectMemory(int64_t start, int64_t end, size_t len, size_t align, int32_t type,
                                             int64_t *out);
extern int32_t sceKernelReleaseDirectMemory(int64_t start, size_t len);
extern int32_t sceKernelMapDirectMemory(void **addr, size_t len, int32_t prot, int32_t flags, int64_t off,
                                        size_t align);
extern int32_t sceKernelMunmap(void *addr, size_t len);
extern int32_t sceKernelMprotect(const void *addr, size_t len, int32_t prot);
extern int32_t sceKernelVirtualQuery(const void *addr, int32_t flags, void *info, size_t size);
extern size_t sceKernelGetDirectMemorySize(void);
extern uint64_t sceKernelGetProcessTime(void);                    // us desde o início do processo
extern uint64_t sceKernelGetProcessTimeCounter(void);             // contador monotônico do processo
extern uint64_t sceKernelGetProcessTimeCounterFrequency(void);
extern int32_t sceKernelIsNeoMode(void);
extern void rt_log(const char *fmt, ...);   // dispatch.c: o totk.log
extern int32_t sceRandomGetRandomNumber(void *buf, size_t size);  // libSceRandom: no máximo 64 bytes por chamada
// O _umtx_op exportado pela libkernel (a syscall 454 é barrada pelo sandbox; o símbolo funciona: orbis-compat
// sys/umtx.h). O <sys/umtx.h> do overlay NÃO é incluído: ele define um _umtx_op static inline que esconderia este, e
// uma variável global que colide na ligação com a cópia que o Mesa já inclui.
extern int _umtx_op(void *obj, int op, unsigned long val, void *uaddr, void *uaddr2);
// Extras do libthr do FreeBSD exportados pela libkernel; o musl não os declara (e _GNU_SOURCE quebra o sched.h do SDK).
extern int pthread_rename_np(pthread_t t, const char *name);
extern int pthread_getthreadid_np(void);

// Limites do executável, do script de ligação do bundle (toolchain/orbis-tls.ld): não há dladdr no PS4. Fracos, para
// a verificação de compilação não depender do script.
extern char __text_start[] __attribute__((weak));
extern char __eh_frame_start[] __attribute__((weak));

// Proteções e flags da libkernel (sonda: provadas no console).
#define K_PROT_CPU_R 0x01
#define K_PROT_CPU_W 0x02
#define K_PROT_GPU_R 0x10
#define K_PROT_GPU_W 0x20
#define K_PROT_GPU (K_PROT_GPU_R | K_PROT_GPU_W)
#define K_MAP_FIXED 0x10
#define K_MAP_NO_OVERWRITE 0x80
#define K_WB_ONION 0   // SCE_KERNEL_WB_ONION: CPU com cache, a GPU lê pelo barramento coerente

// Números de sinal do FreeBSD. O SDK traz os do Linux (SIGBUS 7); SIGSEGV, SIGILL e SIGFPE coincidem.
#define ORBIS_SIGSEGV 11
#define ORBIS_SIGBUS 10
#define ORBIS_SIGILL 4
#define ORBIS_SIGFPE 8

// _umtx_op (FreeBSD): esperar enquanto a palavra de 32 bits vale val, e acordar até val esperas.
#define UMTX_OP_WAIT_UINT_PRIVATE 15
#define UMTX_OP_WAKE_PRIVATE 16

// ---- espera em palavra (o futex do linux.c). Prazo relativo em uma struct _umtx_time do FreeBSD 10+ com
// uaddr = seu tamanho; um kernel FreeBSD 9 ignora uaddr e lê o timespec relativo do início da mesma struct, então as
// duas leituras dão o mesmo prazo. A CONFIRMAR NO CONSOLE: a convenção do prazo e o comportamento sob contenção (a
// sonda do orbis-compat só provou uma espera sem contenção e um prazo de 150 ms). Devolve 0, ou -1 com errno
// (ETIMEDOUT = 60 na tabela do FreeBSD, que é a do SDK).
struct orbis_umtx_time {
    struct timespec timeout;
    uint32_t flags;     // 0: relativo
    uint32_t clockid;   // 4: CLOCK_MONOTONIC na numeração do FreeBSD (o SDK chama 4 de CLOCK_MONOTONIC_RAW)
};

static int fx_wait(volatile uint32_t *w, uint32_t val, uint64_t ns) {
    if (ns == UINT64_MAX) return _umtx_op((void *)w, UMTX_OP_WAIT_UINT_PRIVATE, val, NULL, NULL);
    struct orbis_umtx_time t = {{(time_t)(ns / 1000000000u), (long)(ns % 1000000000u)}, 0, 4};
    return _umtx_op((void *)w, UMTX_OP_WAIT_UINT_PRIVATE, val, (void *)(uintptr_t)sizeof t, &t);
}

static void fx_wake(volatile uint32_t *w, int n) { _umtx_op((void *)w, UMTX_OP_WAKE_PRIVATE, (unsigned long)n, NULL, NULL); }

// ---- threads

// ---- pilhas de thread e de fibra em memória direta. A memória flexível do processo (onde o pthread_create da Sony e o
// mmap anônimo põem as pilhas) é de ~400 MiB no total e física desde a criação; a 16 MiB por thread
// (PLAT_STACK_DEFAULT), duas dúzias de threads a esgotariam. As pilhas vêm então da memória direta (~5 GiB, a mesma do
// guest), mapeada só para a CPU, com a página mais baixa (16 KiB) sem acesso: um estouro falta em vez de corromper.
// OCARINA_STACK_KIB=<KiB> troca o tamanho padrão (PLAT_STACK_DEFAULT) de threads e fibras que não escolhem.
// Sem memória direta livre, a thread volta à pilha flexível do pthread (com o tamanho pedido) e a fibra ao mmap.
#define STACK_PAGE 0x4000ull   // página do PS4

typedef struct { void *va; size_t len; int64_t off; } DStack;   // va == NULL: nenhuma (pilha do sistema ou do mmap)

static size_t stack_default(void) {
    static size_t v;
    if (!v) {
        const char *e = getenv("OCARINA_STACK_KIB");
        long k = e && *e ? strtol(e, NULL, 10) : 0;
        v = k >= 256 && k <= (1 << 20) ? (size_t)k << 10 : PLAT_STACK_DEFAULT;
    }
    return v;
}

// Teto das pilhas em memória direta (ocarina-ps4, 2026-11-07). O kernel.c pede 256 MiB de pilha de host por thread e
// por fibra do guest (HOST_STACK, "reservada, commitada sob demanda" no Linux e no Windows); aqui a memória direta é
// física desde já, e oito threads levavam 2 GiB, o que derrubou o primeiro boot do OoT ("cannot start a guest thread").
// OCARINA_STACK_CAP_MIB=<MiB> (padrão 8) limita cada pilha. TODO: pilhas preguiçosas (reserva + janelas no toque, como
// o heap) para devolver o tamanho pedido sem gastar memória.
static uint64_t stack_bytes, stack_count;   // pilhas em memória direta vivas (o monitor de memória mostra)
static size_t stack_cap(void) {
    static size_t v;
    if (!v) {
        const char *e = getenv("OCARINA_STACK_CAP_MIB");
        long m = e && *e ? strtol(e, NULL, 10) : 0;
        v = (m >= 1 && m <= 1024 ? (size_t)m : 8) << 20;
    }
    return v;
}

// len bytes usáveis acima da página de guarda (no máximo stack_cap()); 1 quando deu.
static int dstack_alloc(DStack *s, size_t len) {
    if (len > stack_cap()) len = stack_cap();
    len = (len + STACK_PAGE - 1) & ~(size_t)(STACK_PAGE - 1);
    len += STACK_PAGE;
    int64_t off = 0;
    void *va = NULL;
    if (sceKernelAllocateDirectMemory(0, (int64_t)sceKernelGetDirectMemorySize(), len, STACK_PAGE, K_WB_ONION, &off))
        return 0;
    if (sceKernelMapDirectMemory(&va, len, K_PROT_CPU_R | K_PROT_CPU_W, 0, off, STACK_PAGE)) {
        sceKernelReleaseDirectMemory(off, len);
        return 0;
    }
    sceKernelMprotect(va, STACK_PAGE, 0);   // a guarda; se o kernel recusar, a pilha só fica sem ela
    s->va = va, s->len = len, s->off = off;
    __atomic_add_fetch(&stack_bytes, len, __ATOMIC_RELAXED);
    __atomic_add_fetch(&stack_count, 1, __ATOMIC_RELAXED);
    return 1;
}

static void dstack_free(DStack *s) {
    if (!s->va) return;
    sceKernelMunmap(s->va, s->len);
    sceKernelReleaseDirectMemory(s->off, s->len);
    s->va = NULL;
    __atomic_sub_fetch(&stack_bytes, s->len, __ATOMIC_RELAXED);
    __atomic_sub_fetch(&stack_count, 1, __ATOMIC_RELAXED);
}

struct PlatThread {
    pthread_t t;
    PlatThreadFn fn;
    void *arg;
    volatile uint32_t done;   // 1 quando fn voltou (plat_thread_join_ns espera nisto)
    char name[32];
    DStack stk;               // a pilha em memória direta; liberada no join (de thread desanexada, nunca)
};

// Pilha de sinal da thread, para os tratadores de falta rodarem mesmo com a pilha dela esgotada. O sigaltstack é o
// shim do overlay (signal.h), que corrige a ordem dos campos de stack_t; o próprio overlay marca essa correção como
// inferida, então uma falha aqui não é fatal. As threads criadas pelo pthread_create do overlay já ganham uma.
static void alt_stack(void) {
    stack_t cur;
    if (!sigaltstack(NULL, &cur) && !(cur.ss_flags & SS_DISABLE)) return;
    size_t size = 64 << 10;
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) return;
    stack_t ss;
    memset(&ss, 0, sizeof ss);
    ss.ss_sp = p, ss.ss_size = size;
    sigaltstack(&ss, NULL);
}

static void *thread_main(void *p) {
    PlatThread *t = p;
    alt_stack();
    if (t->name[0]) pthread_rename_np(pthread_self(), t->name);
    t->fn(t->arg);
    __atomic_store_n(&t->done, 1, __ATOMIC_RELEASE);
    fx_wake(&t->done, INT_MAX);
    return NULL;
}

PlatThread *plat_thread_new(PlatThreadFn fn, void *arg, const char *name) {
    return plat_thread_new_ex(fn, arg, name, 0);
}

// A pilha vem da memória direta (dstack_alloc) e entra por pthread_attr_setstack, que a libkernel exporta; com ela
// maior que o piso do overlay (2 MiB), o interpositor do orbis-compat passa o attr adiante sem mexer. A CONFIRMAR NO
// CONSOLE: que o pthread da Sony aceita uma pilha de memória direta. Se o pthread_create recusar, a thread nasce com
// a pilha flexível do sistema no mesmo tamanho (o caminho antigo) e isso fica no log uma vez.
PlatThread *plat_thread_new_ex(PlatThreadFn fn, void *arg, const char *name, size_t stack) {
    PlatThread *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->fn = fn;
    t->arg = arg;
    if (name) snprintf(t->name, sizeof t->name, "%s", name);
    size_t size = stack ? stack : stack_default();
    pthread_attr_t a;
    pthread_attr_init(&a);
    int e = -1;
    if (dstack_alloc(&t->stk, size)) {
        if (!pthread_attr_setstack(&a, (char *)t->stk.va + STACK_PAGE, t->stk.len - STACK_PAGE))
            e = pthread_create(&t->t, &a, thread_main, t);
        if (e) {
            static int said;
            if (!said) said = 1, fprintf(stderr, "plat: pilha em memória direta recusada (%d): pilhas flexíveis\n", e);
            dstack_free(&t->stk);
            pthread_attr_destroy(&a);
            pthread_attr_init(&a);
        }
    }
    if (e) {
        if (size > stack_cap()) size = stack_cap();
        pthread_attr_setstacksize(&a, size);
        e = pthread_create(&t->t, &a, thread_main, t);
    }
    pthread_attr_destroy(&a);
    if (e) { free(t); return NULL; }
    return t;
}

int plat_thread_spawn(PlatThreadFn fn, void *arg, const char *name, size_t stack) {
    PlatThread *t = plat_thread_new_ex(fn, arg, name, stack);
    if (t) plat_thread_detach(t);
    return t != NULL;
}

void plat_thread_join(PlatThread *t) {
    pthread_join(t->t, NULL);
    dstack_free(&t->stk);
    free(t);
}

// Sem pthread_timedjoin_np: o relógio do prazo dele não foi medido aqui (o orbis-compat achou condvars que medem
// CLOCK_REALTIME e CLOCK_MONOTONIC conforme o processo). Espera relativa na palavra done, depois o join, que já não
// bloqueia.
int plat_thread_join_ns(PlatThread *t, uint64_t ns) {
    uint64_t end = plat_now_ns() + ns;
    while (!__atomic_load_n(&t->done, __ATOMIC_ACQUIRE)) {
        uint64_t now = plat_now_ns();
        if (now >= end) return 0;
        fx_wait(&t->done, 0, end - now);
    }
    pthread_join(t->t, NULL);
    dstack_free(&t->stk);
    free(t);
    return 1;
}

// A thread continua usando t->fn e t->arg, então a struct fica alocada (alguns bytes por thread desanexada). A pilha em
// memória direta também fica: ninguém sabe quando a thread desanexada acaba de usá-la. As threads desanexadas do
// runtime vivem até o fim do processo, então isso não vaza na prática.
void plat_thread_detach(PlatThread *t) { pthread_detach(t->t); }

// STUB: afinidade. O caminho é scePthreadSetaffinity / sceKernelSetCpumask, ainda não medido; o runtime segue sem
// ela (perf.c ignora o retorno). 0: falha, como o platform.h pede.
uint64_t plat_thread_affinity(PlatThread *t, uint64_t mask) {
    (void)t, (void)mask;
    return 0;
}

int plat_process_affinity(uint64_t mask) {
    (void)mask;
    return 0;
}

// O sysconf do musl responde 1 (ele pergunta ao sched_getaffinity, que este kernel não tem); o overlay troca por
// orbis_sysconf (unistd.h), que só existe com o liborbis-compat.a ligado. Sem resposta plausível, 6: os núcleos que
// um jogo de PS4 usa por inteiro.
uint32_t plat_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 1 && n <= 64 ? (uint32_t)n : 6;
}

uint32_t plat_cpu_usable(void) { return plat_cpu_count(); }

// A Jaguar não tem SMT: cada processador lógico é um núcleo.
int plat_cpu_cores(uint64_t *all, uint64_t *first) {
    uint32_t n = plat_cpu_count();
    *all = *first = n >= 64 ? ~0ull : (1ull << n) - 1;
    return (int)n;
}

// STUB: tempo de CPU por thread (no Linux vinha de /proc). 0 é "desconhecido" no platform.h.
uint64_t plat_thread_cpu_ns(uint32_t tid) {
    (void)tid;
    return 0;
}

// ---- interrupção de thread (preempção assíncrona do kernel.c). STUB: o runtime roda com TOTK_ASYNC_PREEMPT=0, e
// kernel.c registra a falta do handle e segue sem preempção assíncrona. O caminho no PS4 seria o mesmo handshake do
// linux.c com pthread_kill, com rip/rsp lidos do ucontext do Orbis (ver fault_handler).
PlatIntr *plat_intr_self(void) { return NULL; }
void plat_intr_free(PlatIntr *t) { (void)t; }
int plat_intr_stop(PlatIntr *t, uint64_t *rip, uint64_t *rsp) {
    (void)t, (void)rip, (void)rsp;
    return 0;
}
int plat_intr_resume(PlatIntr *t, uint64_t to) {
    (void)t, (void)to;
    return 0;
}

// ---- fibras: a mesma troca de pilha do linux.c, com a correção de alinhamento da branch ps4 (fn entra com a pilha
// alinhada a 16). A ABI SysV x86-64 é a mesma no PS4.
struct PlatFiber {
    void *sp;
    void *stack;   // pilha do mmap, a reserva quando não há memória direta (NULL: a da própria thread ou a direta)
    size_t size;
    DStack stk;    // a pilha em memória direta (dstack_alloc)
};

void plat_fiber_swap(void **save, void *load);
void plat_fiber_entry(void);
__asm__(".text\n"
        ".globl plat_fiber_swap\n"
        ".type plat_fiber_swap, @function\n"
        ".p2align 4\n"
        "plat_fiber_swap:\n"
        "    .cfi_startproc\n"
        "    pushq %rbp\n"
        "    pushq %rbx\n"
        "    pushq %r12\n"
        "    pushq %r13\n"
        "    pushq %r14\n"
        "    pushq %r15\n"
        "    subq $16, %rsp\n"
        "    stmxcsr 8(%rsp)\n"
        "    fnstcw (%rsp)\n"
        "    movq %rsp, (%rdi)\n"
        "    movq %rsi, %rsp\n"
        "    fldcw (%rsp)\n"
        "    ldmxcsr 8(%rsp)\n"
        "    addq $16, %rsp\n"
        "    popq %r15\n"
        "    popq %r14\n"
        "    popq %r13\n"
        "    popq %r12\n"
        "    popq %rbx\n"
        "    popq %rbp\n"
        "    ret\n"
        "    .cfi_endproc\n"
        ".size plat_fiber_swap, .-plat_fiber_swap\n"
        ".globl plat_fiber_entry\n"
        ".type plat_fiber_entry, @function\n"
        ".p2align 4\n"
        "plat_fiber_entry:\n"
        "    movq %r13, %rdi\n"
        /* entra por ret (rsp = 8 mod 16): realinha antes do call, senão fn começa desalinhada e o primeiro movaps
           cai. ocarina-ps4 */
        "    andq $-16, %rsp\n"
        "    callq *%r12\n"
        "    ud2\n"
        ".size plat_fiber_entry, .-plat_fiber_entry\n");

static __thread PlatFiber *fiber_cur;
static __thread PlatFiber fiber_own;

PlatFiber *plat_fiber_self(void) {
    fiber_own = (PlatFiber){0};
    fiber_cur = &fiber_own;
    return &fiber_own;
}

void plat_fiber_unself(void) { fiber_cur = NULL; }

#define KPAGE 0x4000ull   // página do PS4

// A pilha vem da memória direta, como a das threads (dstack_alloc, com a página de guarda); sem memória direta livre,
// do mmap (memória flexível, física desde já: o PS4 não faz overcommit), também com a página mais baixa inacessível.
PlatFiber *plat_fiber_new(size_t stack, void (*fn)(void *arg), void *arg) {
    PlatFiber *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    if (!stack) stack = stack_default();
    stack = (stack + KPAGE - 1) & ~(size_t)(KPAGE - 1);
    if (stack < 2 * KPAGE) stack = 2 * KPAGE;
    if (stack > stack_cap()) stack = stack_cap();   // também no mmap de reserva (memória flexível, ~400 MiB no total)
    void *m;
    if (dstack_alloc(&f->stk, stack)) {
        m = f->stk.va;
        stack = f->stk.len;
    } else {
        m = mmap(NULL, stack, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (m == MAP_FAILED) {
            free(f);
            return NULL;
        }
        mprotect(m, KPAGE, PROT_NONE);
        f->stack = m, f->size = stack;
    }
    uint64_t *top = (uint64_t *)(((uintptr_t)m + stack) & ~(uintptr_t)15);
    uint64_t *sp = top - 10;   // estado da FPU (2 palavras), r15, r14, r13, r12, rbx, rbp, retorno, enchimento
    sp[0] = 0x037f;            // palavra de controle x87: a padrão
    sp[1] = 0x1f80;            // MXCSR: o padrão
    sp[2] = sp[3] = 0;         // r15, r14
    sp[4] = (uint64_t)(uintptr_t)arg;   // r13
    sp[5] = (uint64_t)(uintptr_t)fn;    // r12
    sp[6] = sp[7] = 0;         // rbx, rbp
    sp[8] = (uint64_t)(uintptr_t)plat_fiber_entry;
    sp[9] = 0;
    f->sp = sp;
    return f;
}

void plat_fiber_switch(PlatFiber *to) {
    PlatFiber *from = fiber_cur;
    fiber_cur = to;
    plat_fiber_swap(&from->sp, to->sp);
}

void plat_fiber_free(PlatFiber *f) {
    if (!f) return;
    if (f->stack) munmap(f->stack, f->size);
    dstack_free(&f->stk);
    free(f);
}

// STUB: prioridade. O caminho é scePthreadSetprio (prioridades da Sony, 256 a 767, menor é mais urgente), ainda não
// medido; todas as threads ficam na normal, como no Linux sem CAP_SYS_NICE.
void plat_thread_priority(int prio) { (void)prio; }
void plat_thread_set_priority(PlatThread *t, int prio) { (void)t, (void)prio; }
int plat_thread_priority_get(void) { return PLAT_PRIO_NORMAL; }

void plat_yield(void) { sched_yield(); }

// ---- tempo. Nada de clock_gettime(CLOCK_MONOTONIC): o SDK manda o id 1, que neste kernel é CLOCK_VIRTUAL (tempo de
// CPU do usuário; orbis-compat orbis_clock.h). O contador de tempo do processo é monotônico e tem frequência declarada.

static PlatScale ns_scale;
static volatile uint64_t counter_freq;   // 0 até a primeira leitura

static uint64_t counter_hz_get(void) {
    uint64_t hz = __atomic_load_n(&counter_freq, __ATOMIC_ACQUIRE);
    if (!hz) {
        hz = sceKernelGetProcessTimeCounterFrequency();
        if (!hz) hz = 1000000u;
        ns_scale = plat_scale(1000000000u, hz);   // idempotente: outra thread escreve o mesmo valor
        __atomic_store_n(&counter_freq, hz, __ATOMIC_RELEASE);
    }
    return hz;
}

uint64_t plat_now_ns(void) {
    counter_hz_get();
    return plat_scaled(ns_scale, sceKernelGetProcessTimeCounter());
}

uint64_t plat_counter(void) { return sceKernelGetProcessTimeCounter(); }
uint64_t plat_counter_hz(void) { return counter_hz_get(); }

// Sem timer slack para ajustar no PS4.
void plat_timer_1ms(void) {}

#define PLAT_SPIN_NS 100000u

static void sleep_rel(uint64_t ns) {
    struct timespec ts = {(time_t)(ns / 1000000000u), (long)(ns % 1000000000u)};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
}

// nanosleep relativo até PLAT_SPIN_NS antes do prazo, depois cede a vez até ele.
void plat_sleep_ns(uint64_t ns) {
    uint64_t end = plat_now_ns() + ns;
    if (ns > PLAT_SPIN_NS) {
        uint64_t now = plat_now_ns();
        if (end - PLAT_SPIN_NS > now) sleep_rel(end - PLAT_SPIN_NS - now);
    }
    while (plat_now_ns() < end) sched_yield();
}

void plat_wait_ns(uint64_t ns) { sleep_rel(ns); }

void plat_sleep_ms(uint32_t ms) { sleep_rel((uint64_t)ms * 1000000u); }

// ---- travas: a trava leitor-escritor de uma palavra do linux.c, com o _umtx_op no lugar do futex.
// bit 31: um escritor a detém; bit 30: alguém dorme na palavra; bits 0-29: leitores.

#define LK_W 0x80000000u
#define LK_S 0x40000000u
#define LK_R 0x3fffffffu
#define LK_SPINS 64

static inline volatile uint32_t *lk(PlatMutex *m) { return (volatile uint32_t *)&m->p; }

static void lk_sleep(volatile uint32_t *w, uint32_t s) {
    if (!(s & LK_S) && !__atomic_compare_exchange_n(w, &s, s | LK_S, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return;
    fx_wait(w, s | LK_S, UINT64_MAX);
}

static void lk_wake(volatile uint32_t *w) {
    __atomic_fetch_and(w, ~LK_S, __ATOMIC_RELAXED);
    fx_wake(w, INT_MAX);
}

void plat_mutex_init(PlatMutex *m) { m->p = 0; }

int plat_mutex_trylock(PlatMutex *m) {
    volatile uint32_t *w = lk(m);
    uint32_t s = __atomic_load_n(w, __ATOMIC_RELAXED);
    while (!(s & (LK_W | LK_R)))
        if (__atomic_compare_exchange_n(w, &s, s | LK_W, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 1;
    return 0;
}

void plat_mutex_lock(PlatMutex *m) {
    volatile uint32_t *w = lk(m);
    for (int spin = 0;; spin++) {
        uint32_t s = __atomic_load_n(w, __ATOMIC_RELAXED);
        if (!(s & (LK_W | LK_R))) {
            if (__atomic_compare_exchange_n(w, &s, s | LK_W, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
            continue;
        }
        if (spin < LK_SPINS) plat_pause();
        else lk_sleep(w, s);
    }
}

void plat_mutex_unlock(PlatMutex *m) {
    volatile uint32_t *w = lk(m);
    uint32_t s = __atomic_fetch_and(w, ~LK_W, __ATOMIC_RELEASE);
    if (s & LK_S) lk_wake(w);
}

int plat_mutex_trylock_shared(PlatMutex *m) {
    volatile uint32_t *w = lk(m);
    uint32_t s = __atomic_load_n(w, __ATOMIC_RELAXED);
    while (!(s & (LK_W | LK_S)))
        if (__atomic_compare_exchange_n(w, &s, s + 1, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 1;
    return 0;
}

void plat_mutex_lock_shared(PlatMutex *m) {
    volatile uint32_t *w = lk(m);
    for (int spin = 0;; spin++) {
        uint32_t s = __atomic_load_n(w, __ATOMIC_RELAXED);
        if (!(s & (LK_W | LK_S))) {
            if (__atomic_compare_exchange_n(w, &s, s + 1, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
            continue;
        }
        if (spin < LK_SPINS) plat_pause();
        else lk_sleep(w, s);
    }
}

void plat_mutex_unlock_shared(PlatMutex *m) {
    volatile uint32_t *w = lk(m);
    uint32_t s = __atomic_sub_fetch(w, 1, __ATOMIC_RELEASE);
    if (!(s & LK_R) && (s & LK_S)) lk_wake(w);
}

// ---- variáveis de condição: número de sequência nos primeiros 32 bits, como no linux.c.

static inline volatile uint32_t *cv(PlatCond *c) { return (volatile uint32_t *)&c->p; }

void plat_cond_init(PlatCond *c) { c->p = 0; }

void plat_cond_wait(PlatCond *c, PlatMutex *m) {
    uint32_t seq = __atomic_load_n(cv(c), __ATOMIC_RELAXED);
    plat_mutex_unlock(m);
    fx_wait(cv(c), seq, UINT64_MAX);
    plat_mutex_lock(m);
}

int plat_cond_wait_ns(PlatCond *c, PlatMutex *m, uint64_t ns) {
    uint32_t seq = __atomic_load_n(cv(c), __ATOMIC_RELAXED);
    plat_mutex_unlock(m);
    int r = fx_wait(cv(c), seq, ns);
    int timed_out = r == -1 && errno == ETIMEDOUT;
    plat_mutex_lock(m);
    return !timed_out;
}

void plat_cond_signal(PlatCond *c) {
    __atomic_add_fetch(cv(c), 1, __ATOMIC_RELEASE);
    fx_wake(cv(c), 1);
}

void plat_cond_broadcast(PlatCond *c) {
    __atomic_add_fetch(cv(c), 1, __ATOMIC_RELEASE);
    fx_wake(cv(c), INT_MAX);
}

// ---- caminhos. O processo não tem diretório de trabalho (getcwd dá ENOSYS e um open relativo dá EINVAL:
// orbis-compat orbis_paths.h). Os dados do app estão em /app0 (somente leitura); o que o runtime grava vai para
// /data/ocarina, que também ancora os caminhos relativos.
#define ORBIS_DATA_DIR "/data/ocarina"

// ---- argumentos: o PS4 não passa linha de comando a um app. orbis_boot_args lê ORBIS_DATA_DIR/args.txt (uma entrada
// por linha; vazias e as começadas por # são ignoradas): NOME=valor vira variável de ambiente (setenv, valendo para o
// cfg_get, o TOTK_* e os knobs do Mesa, todos neste executável), e o resto vira argumentos, separados por espaço
// ("--probe-orbis", "--seconds 30"). main.c chama isto antes de tudo, antes do mem_init. Sem o arquivo, nada muda.
int orbis_boot_args(int *argc, char ***argv) {
    FILE *f = fopen(ORBIS_DATA_DIR "/args.txt", "r");
    if (!f) return 0;
    enum { MAX_ARGS = 64 };
    static char *av[MAX_ARGS + 1];
    int ac = 0, envs = 0;
    av[ac++] = *argc > 0 && (*argv)[0] ? (*argv)[0] : (char *)"/app0/eboot.bin";
    for (int i = 1; i < *argc && ac < MAX_ARGS; i++) av[ac++] = (*argv)[i];
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *p = line, *end = line + strlen(line);
        while (end > p && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') continue;
        char *eq = strchr(p, '=');
        if (*p != '-' && eq && eq > p) {
            *eq = 0;
            if (!setenv(p, eq + 1, 1)) envs++;
            continue;
        }
        for (char *tok = strtok(p, " \t"); tok && ac < MAX_ARGS; tok = strtok(NULL, " \t")) av[ac++] = strdup(tok);
    }
    fclose(f);
    av[ac] = NULL;
    *argc = ac, *argv = av;
    return envs + ac - 1;
}

int plat_full_path(const char *path, char *out, size_t cap) {
    if (path[0] == '/') return snprintf(out, cap, "%s", path) < (int)cap;
    return snprintf(out, cap, "%s/%s", ORBIS_DATA_DIR, path) < (int)cap;
}

// ---- arquivos. POSIX: open, pread e afins vão direto à libkernel; as constantes O_* do SDK já são as do FreeBSD.
// A família stat só tem o layout certo com o liborbis-compat.a ligado (--whole-archive): o struct stat do SDK não é o
// que o kernel preenche (orbis-compat orbis_stat.h).

int plat_file_exists(const char *path) {
    struct stat st;
    return !stat(path, &st) && !S_ISDIR(st.st_mode);
}

int plat_dir_exists(const char *path) {
    struct stat st;
    return !stat(path, &st) && S_ISDIR(st.st_mode);
}

void plat_file_mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof tmp, "%s", path) >= (int)sizeof tmp) return;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char c = *p;
            *p = 0;
            mkdir(tmp, 0755);
            *p = c;
        }
    }
    mkdir(tmp, 0755);
}

static int write_all(int fd, const void *data, size_t n) {
    const uint8_t *p = data;
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return 0;
        p += k, n -= (size_t)k;
    }
    return 1;
}

uint8_t *plat_file_read(const char *path, uint64_t *size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    uint8_t *buf = NULL;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size < (1ll << 30)) {
        buf = malloc((size_t)st.st_size + 1);
        size_t got = 0;
        while (buf && got < (size_t)st.st_size) {
            ssize_t k = read(fd, buf + got, (size_t)st.st_size - got);
            if (k < 0 && errno == EINTR) continue;
            if (k <= 0) break;
            got += (size_t)k;
        }
        if (buf && got != (size_t)st.st_size) {
            free(buf);
            buf = NULL;
        }
        if (buf) {
            buf[st.st_size] = 0;
            *size = (uint64_t)st.st_size;
        }
    }
    close(fd);
    return buf;
}

int plat_file_write(const char *path, const void *data, size_t n) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return 0;
    int ok = write_all(fd, data, n);
    return close(fd) == 0 && ok;
}

PlatAppend *plat_append_open(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
    return fd < 0 ? NULL : (PlatAppend *)(intptr_t)(fd + 1);
}

int plat_append_write(PlatAppend *f, const void *data, size_t n) {
    int fd = (int)(intptr_t)f - 1;
    ssize_t k;
    do k = write(fd, data, n);
    while (k < 0 && errno == EINTR);
    return k == (ssize_t)n;
}

static void sync_dir_of(const char *path) {
    char dir[PATH_MAX];
    if (snprintf(dir, sizeof dir, "%s", path) >= (int)sizeof dir) return;
    char *slash = strrchr(dir, '/');
    if (slash == dir) slash[1] = 0;
    else if (slash) *slash = 0;
    else snprintf(dir, sizeof dir, "%s", ORBIS_DATA_DIR);
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
}

int plat_file_replace(const char *from, const char *to) {
    if (rename(from, to)) return 0;
    sync_dir_of(to);
    return 1;
}

// Gravação atômica: arquivo temporário, fsync e rename. No PS4 cada passo que falha vai para o log com o errno (o
// primeiro boot do OoT caiu aqui ao gravar o account.uid sem dizer onde). O fsync que falha não derruba a gravação, e
// se o rename falhar, grava direto no destino: perder a atomicidade é melhor que um fatal no boot.
int plat_file_write_atomic(const char *path, const void *data, size_t n) {
    if (access(path, W_OK) && errno != ENOENT)
        rt_log("orbis: write_atomic %s: access deu errno %d (seguindo mesmo assim)", path, errno);
    char tmp[PATH_MAX + 48];
    int len = snprintf(tmp, sizeof tmp, "%s.%u.%u.tmp", path, plat_process_id(), plat_thread_id());
    if (len < 0 || len >= (int)sizeof tmp) return 0;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        rt_log("orbis: write_atomic %s: open do temporário deu errno %d", path, errno);
        return plat_file_write(path, data, n);
    }
    int ok = write_all(fd, data, n);
    if (!ok) rt_log("orbis: write_atomic %s: write deu errno %d", path, errno);
    if (fsync(fd)) rt_log("orbis: write_atomic %s: fsync deu errno %d (ignorado)", path, errno);
    if (close(fd)) rt_log("orbis: write_atomic %s: close deu errno %d (ignorado)", path, errno);
    if (ok && !rename(tmp, path)) {
        sync_dir_of(path);
        return 1;
    }
    if (ok) rt_log("orbis: write_atomic %s: rename deu errno %d; gravando direto", path, errno);
    unlink(tmp);
    if (!ok) return 0;
    if (plat_file_write(path, data, n)) return 1;
    rt_log("orbis: write_atomic %s: gravação direta deu errno %d", path, errno);
    return 0;
}

int plat_file_flush(const char *path) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    int ok = fsync(fd) == 0;
    close(fd);
    return ok;
}

int plat_file_sync(FILE *f) { return !fflush(f) && !fsync(fileno(f)); }

void plat_file_delete(const char *path) { unlink(path); }

int plat_file_truncate(const char *path, uint64_t size) { return truncate(path, (off_t)size) == 0; }

int64_t plat_file_size(const char *path) {
    struct stat st;
    return stat(path, &st) ? -1 : (int64_t)st.st_size;
}

int plat_file_set_readonly(const char *path, int on) {
    struct stat st;
    if (stat(path, &st)) return 0;
    mode_t m = on ? st.st_mode & ~(mode_t)0222 : st.st_mode | S_IWUSR;
    return chmod(path, m & 07777) == 0;
}

// A CONFIRMAR NO CONSOLE: opendir/readdir do musl sobre o getdents da libkernel (layout do dirent do FreeBSD).
int plat_file_list(const char *dir, const char *ext, int (*cb)(const char *name, void *ctx), void *ctx) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    size_t el = strlen(ext);
    for (struct dirent *e; (e = readdir(d));) {
        size_t n = strlen(e->d_name);
        if (n < el || strcmp(e->d_name + n - el, ext) || e->d_name[0] == '.') continue;
        if (!cb(e->d_name, ctx)) break;
    }
    closedir(d);
    return 1;
}

// ---- memória virtual.
//
// Reserva: sceKernelReserveVirtualRange fixo e sem sobrescrever (provado). Cada reserva guarda uma tabela própria com
// um byte por subpágina de 4 KiB (a página do Switch, a unidade do runtime): o estado pedido (reservada, ou commitada
// com PLAT_PROT_*). O PS4 não faz overcommit, então nada de tabela global sobre 2^39 como no linux.c.
//
// Commit: memória DIRETA WB_ONION com CPU_RW+GPU_RW, mapeada em endereço fixo dentro da reserva (provado), porque a
// GPU importa os pools do jogo pelo próprio endereço (VK_EXT_external_memory_host). A memória direta é alocada e
// mapeada em janelas de 2 MiB alinhadas (recortadas nas bordas da reserva) no primeiro commit que as toca, zerada, e
// fica mapeada até o fim: decommit zera as subpáginas pedidas e tira o acesso da CPU às páginas de 16 KiB que ficaram
// sem subpágina commitada. Toda subpágina não commitada de uma janela mapeada vale zero, então commit não precisa
// zerar. TODO: devolver uma janela inteira sem commit (munmap + nova reserva fixa no lugar, não provado).
//
// Página de 16 KiB do PS4 contra 4 KiB do runtime: o commit vale para a página inteira (a janela já está mapeada); a
// proteção efetiva de uma página de 16 KiB é a mais permissiva entre as suas subpáginas commitadas, ou seja, uma
// proteção arredonda para dentro e nunca protege mais do que o pedido; plat_mem_state responde por subpágina, pela
// tabela.
//
// Write-watch (reservas com watch=1): sem userfaultfd. Uma página de 16 KiB "armada" fica CPU somente leitura (a GPU
// continua RW); a primeira escrita falta, o tratador a marca como escrita e devolve a escrita (CPU RW). plat_mem_written
// lista as subpáginas commitadas das páginas escritas e, com reset, arma-as de novo. A granularidade é 16 KiB: escrever
// uma subpágina reporta as quatro (nunca menos do que foi escrito).

#define SUB 0x1000ull           // subpágina: a página do runtime
#define WIN (2ull << 20)        // janela de memória direta
#define MAX_RESV 64

enum { SS_RESERVED = 1, SS_NONE, SS_READ, SS_RW };   // por subpágina; commitada: SS_NONE + PLAT_PROT_*
enum { WW_CLEAN, WW_DIRTY, WW_RESET };              // por página de 16 KiB das reservas vigiadas

typedef struct {
    uint64_t base, end;
    uint64_t win_base;      // base alinhada para baixo a 2 MiB: a janela i cobre win_base + i * WIN
    uint8_t *sub;           // estado de cada subpágina
    uint8_t *ww;            // estado de escrita de cada página de 16 KiB; NULL sem watch
    uint8_t *win;           // 1 por janela já mapeada
} Resv;

static void fault_install(void);   // o tratador do write-watch (abaixo)
static int ww_fault(uint64_t a);

static Resv resv[MAX_RESV];
static volatile int32_t nresv;   // entradas só são acrescentadas; o tratador de sinal lê sem trava
static PlatMutex vm_lock = PLAT_MUTEX_INIT;
static uint64_t mapped_bytes;   // memória direta mapeada nas reservas (plat_commit_info)

static uint64_t down(uint64_t v, uint64_t a) { return v & ~(a - 1); }
static uint64_t up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

static Resv *resv_of(uint64_t addr) {
    int n = __atomic_load_n(&nresv, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++)
        if (addr >= resv[i].base && addr < resv[i].end) return &resv[i];
    return NULL;
}

// A reserva que contém [addr, addr + n) inteira, ou NULL.
static Resv *resv_of_range(uint64_t addr, uint64_t n) {
    Resv *r = resv_of(addr);
    return r && n <= r->end - addr ? r : NULL;
}

static int win_mapped(const Resv *r, uint64_t a) {
    return __atomic_load_n(&r->win[(a - r->win_base) / WIN], __ATOMIC_ACQUIRE);
}

// A proteção pedida mais permissiva entre as subpáginas commitadas da página de 16 KiB em pg; -1 sem nenhuma.
static int page_req(const Resv *r, uint64_t pg) {
    const uint8_t *s = r->sub + (pg - r->base) / SUB;
    int m = -1;
    for (int k = 0; k < (int)(KPAGE / SUB); k++) {
        int v = __atomic_load_n(&s[k], __ATOMIC_RELAXED);
        if (v >= SS_NONE && v - SS_NONE > m) m = v - SS_NONE;
    }
    return m;
}

// A proteção da libkernel para a página: a pedida, reduzida a leitura enquanto vigiada e não escrita. A GPU fica com
// RW em toda a memória mapeada: o importador do Vulkan precisa dela, e uma falta do lado da GPU trava o console.
static int32_t page_kprot(const Resv *r, uint64_t pg) {
    int m = page_req(r, pg);
    int32_t cpu = m == PLAT_PROT_RW ? K_PROT_CPU_R | K_PROT_CPU_W : m == PLAT_PROT_READ ? K_PROT_CPU_R : 0;
    if (cpu & K_PROT_CPU_W && r->ww && __atomic_load_n(&r->ww[(pg - r->base) / KPAGE], __ATOMIC_ACQUIRE) != WW_DIRTY)
        cpu = K_PROT_CPU_R;
    return cpu | K_PROT_GPU;
}

// Aplica a proteção efetiva às páginas de 16 KiB de [a, end) (alinhados a 16 KiB) nas janelas mapeadas, juntando as
// páginas vizinhas de mesma proteção numa chamada. vm_lock seguro.
static int apply_range(const Resv *r, uint64_t a, uint64_t end) {
    int ok = 1;
    uint64_t run = 0, run_end = 0;
    int32_t run_prot = -1;
    for (uint64_t pg = a; pg <= end; pg += KPAGE) {
        int mapped = pg < end && win_mapped(r, pg);
        int32_t p = mapped ? page_kprot(r, pg) : -1;
        if (run_prot >= 0 && (p != run_prot || pg != run_end)) {
            if (sceKernelMprotect((void *)(uintptr_t)run, run_end - run, run_prot)) ok = 0;
            run_prot = -1;
        }
        if (p >= 0) {
            if (run_prot < 0) run = pg, run_prot = p;
            run_end = pg + KPAGE;
        }
    }
    return ok;
}

// Aloca e mapeia a janela que contém a, recortada na reserva, e a zera. vm_lock seguro. A CONFIRMAR NO CONSOLE: a sonda
// só provou mapas de 2 MiB alinhados; uma janela recortada (janela do host de 1 MiB, fim da janela dos módulos) usa
// alinhamento de 16 KiB.
static int map_window(Resv *r, uint64_t a) {
    uint64_t w = down(a, WIN);
    uint64_t start = w > r->base ? w : r->base, end = w + WIN < r->end ? w + WIN : r->end;
    size_t len = (size_t)(end - start), align = start % WIN == 0 && len == WIN ? WIN : KPAGE;
    int64_t off = 0;
    int32_t e = sceKernelAllocateDirectMemory(0, (int64_t)sceKernelGetDirectMemorySize(), len, align, K_WB_ONION, &off);
    if (e) {
        fprintf(stderr, "platform: memória direta esgotada (%#x) ao commitar %#llx+%#zx\n", (unsigned)e,
                (unsigned long long)start, len);
        return 0;
    }
    void *va = (void *)(uintptr_t)start;
    e = sceKernelMapDirectMemory(&va, len, K_PROT_CPU_R | K_PROT_CPU_W | K_PROT_GPU, K_MAP_FIXED, off, align);
    if (e || va != (void *)(uintptr_t)start) {
        fprintf(stderr, "platform: mapear memória direta em %#llx+%#zx falhou (%#x, veio %p)\n",
                (unsigned long long)start, len, (unsigned)e, va);
        if (!e) sceKernelMunmap(va, len);
        sceKernelReleaseDirectMemory(off, len);
        return 0;
    }
    memset(va, 0, len);   // nenhuma fonte diz que a memória direta nova vem zerada
    __atomic_store_n(&r->win[(w - r->win_base) / WIN], 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(&mapped_bytes, len, __ATOMIC_RELAXED);
    return 1;
}

// ---- commit preguiçoso (ocarina-ps4, 2026-11-07). O OoT pede um heap de 6,2 GiB de uma vez (nninitStartup:
// SetHeapSize 0x18ac00000), mais do que a memória direta inteira do PS4. Com o commit preguiçoso, plat_mem_commit só
// marca as subpáginas; a janela de 2 MiB é mapeada no primeiro toque da CPU (lazy_fault, no tratador de SIGSEGV) ou
// antes de o kernel ou a GPU escreverem nela (orbis_mem_prefault: pread e importação de pool). O console só gasta o que
// o jogo usa. OCARINA_EAGER_COMMIT=1 volta ao commit imediato.
// Monitor de memória (ocarina-ps4): a cada 2 s, se a memória direta mapeada mudou 64 MiB ou mais, uma linha no log
// com o mapeado, o livre e o total da memória direta e a flexível livre. É o dado para a dieta de memória do OoT.
extern int32_t sceKernelAvailableDirectMemorySize(int64_t start, int64_t end, size_t align, int64_t *out_start,
                                                  size_t *out_size);
extern int32_t sceKernelAvailableFlexibleMemorySize(size_t *size);
static void mem_report(const char *why) {
    int64_t st = 0;
    size_t dfree = 0, ffree = 0;
    // o maior trecho livre contíguo, não a soma: um limite inferior do livre
    sceKernelAvailableDirectMemorySize(0, (int64_t)sceKernelGetDirectMemorySize(), 0x200000, &st, &dfree);
    sceKernelAvailableFlexibleMemorySize(&ffree);
    rt_log("orbis mem (%s): jogo mapeou %llu MiB; pilhas %llu (%llu MiB); memória direta: maior trecho livre %llu MiB "
           "de %llu MiB; flexível livre %llu MiB", why,
           (unsigned long long)(__atomic_load_n(&mapped_bytes, __ATOMIC_RELAXED) >> 20),
           (unsigned long long)__atomic_load_n(&stack_count, __ATOMIC_RELAXED),
           (unsigned long long)(__atomic_load_n(&stack_bytes, __ATOMIC_RELAXED) >> 20),
           (unsigned long long)(dfree >> 20), (unsigned long long)(sceKernelGetDirectMemorySize() >> 20),
           (unsigned long long)(ffree >> 20));
}
static void *mem_monitor(void *arg) {
    (void)arg;
    uint64_t last = 0;
    for (;;) {
        usleep(2000000);
        uint64_t m = __atomic_load_n(&mapped_bytes, __ATOMIC_RELAXED);
        if (m > last + (64ull << 20) || m + (64ull << 20) < last) {
            mem_report("mudou");
            last = m;
        }
    }
    return NULL;
}

static int lazy_commit(void) {
    static int on = -1;
    if (on < 0) {
        char v[8];
        on = !(plat_getenv("OCARINA_EAGER_COMMIT", v, sizeof v) && v[0] == '1');
        mem_report(on ? "início, commit preguiçoso" : "início, commit imediato");
        pthread_t t;
        if (!pthread_create(&t, NULL, mem_monitor, NULL)) pthread_detach(t);
    }
    return on;
}

// Mapeia (se ainda não) a janela que contém a, numa reserva; aplica as proteções dela. vm_lock tomado aqui.
static int map_window_locked(Resv *r, uint64_t a) {
    plat_mutex_lock(&vm_lock);
    int ok = win_mapped(r, a) || map_window(r, a);
    if (ok) {
        uint64_t w = down(a, WIN);
        apply_range(r, w > r->base ? w : r->base, w + WIN < r->end ? w + WIN : r->end);
    }
    plat_mutex_unlock(&vm_lock);
    return ok;
}

// Falta numa janela ainda não mapeada de subpágina commitada: mapeia e devolve a instrução. Subpágina não commitada é
// uma falta de verdade (0). Nenhum código que segura vm_lock toca memória do jogo de janela não mapeada.
static int lazy_fault(uint64_t a) {
    Resv *r = resv_of(a);
    if (!r || win_mapped(r, a) || __atomic_load_n(&r->sub[(a - r->base) / SUB], __ATOMIC_RELAXED) < SS_NONE) return 0;
    return map_window_locked(r, a);
}

// Garante as janelas de [addr, addr + n) mapeadas: o kernel (pread) e a GPU (importação) não geram falta, só erro.
void orbis_mem_prefault(uint64_t addr, uint64_t n) {
    if (!n) return;
    for (uint64_t w = down(addr, WIN); w < addr + n; w += WIN) {
        uint64_t at = w < addr ? addr : w;
        Resv *r = resv_of(at);
        if (r && !win_mapped(r, at)) map_window_locked(r, at);
    }
}

// A reserva exige endereço e tamanho múltiplos de 16 KiB (a página do PS4); as do runtime são (módulos alinhados a
// 64 KiB, as outras a 1 MiB ou mais).
int plat_mem_reserve(uint64_t addr, uint64_t n, int watch) {
    if (!n || ((addr | n) & (KPAGE - 1))) {
        fprintf(stderr, "platform: reserva %#llx+%#llx não alinhada a 16 KiB (a página do PS4)\n",
                (unsigned long long)addr, (unsigned long long)n);
        return 0;
    }
    if (watch) fault_install();   // antes da primeira página armada
    plat_mutex_lock(&vm_lock);
    int ok = 0;
    if (nresv < MAX_RESV) {
        void *a = (void *)(uintptr_t)addr;
        int32_t e = sceKernelReserveVirtualRange(&a, n, K_MAP_FIXED | K_MAP_NO_OVERWRITE, 0);
        if (!e && a == (void *)(uintptr_t)addr) {
            Resv r = {addr, addr + n, down(addr, WIN), NULL, NULL, NULL};
            r.sub = malloc(n / SUB);
            r.ww = watch ? calloc(n / KPAGE, 1) : NULL;
            r.win = calloc((up(addr + n, WIN) - r.win_base) / WIN, 1);
            if (r.sub && r.win && (!watch || r.ww)) {
                memset(r.sub, SS_RESERVED, n / SUB);
                resv[nresv] = r;
                __atomic_store_n(&nresv, nresv + 1, __ATOMIC_RELEASE);
                ok = 1;
            } else {
                free(r.sub), free(r.ww), free(r.win);
                sceKernelMunmap(a, n);
            }
        } else if (!e) {
            sceKernelMunmap(a, n);
        }
    }
    plat_mutex_unlock(&vm_lock);
    return ok;
}

// Memória direta com CPU_RW+GPU_RW em qualquer endereço, registrada como uma reserva toda commitada, para
// plat_mem_protect (dispatch.c lacra as tabelas) e plat_mem_state funcionarem nela. Diferente do Linux, é física desde
// já, não página a página.
void *plat_mem_alloc(size_t n) {
    size_t len = (size_t)up(n ? n : 1, KPAGE);
    int64_t off = 0;
    if (sceKernelAllocateDirectMemory(0, (int64_t)sceKernelGetDirectMemorySize(), len, KPAGE, K_WB_ONION, &off))
        return NULL;
    void *va = NULL;
    if (sceKernelMapDirectMemory(&va, len, K_PROT_CPU_R | K_PROT_CPU_W | K_PROT_GPU, 0, off, KPAGE)) {
        sceKernelReleaseDirectMemory(off, len);
        return NULL;
    }
    memset(va, 0, len);
    uint64_t base = (uint64_t)(uintptr_t)va;
    plat_mutex_lock(&vm_lock);
    if (nresv < MAX_RESV) {
        Resv r = {base, base + len, down(base, WIN), malloc(len / SUB), NULL, NULL};
        r.win = calloc((up(base + len, WIN) - r.win_base) / WIN, 1);
        if (r.sub && r.win) {
            memset(r.sub, SS_RW, len / SUB);
            memset(r.win, 1, (up(base + len, WIN) - r.win_base) / WIN);
            resv[nresv] = r;
            __atomic_store_n(&nresv, nresv + 1, __ATOMIC_RELEASE);
        } else {
            free(r.sub), free(r.win);
        }
    }
    plat_mutex_unlock(&vm_lock);
    return va;
}

int plat_mem_commit(uint64_t addr, size_t n) {
    if (!n) return 1;
    plat_mutex_lock(&vm_lock);
    Resv *r = resv_of_range(addr, n);
    int ok = r != NULL;
    uint64_t a = down(addr, KPAGE), end = up(addr + n, KPAGE);
    uint64_t lo = a, hi = end;   // o que precisa de proteção aplicada: o pedido e as janelas novas
    for (uint64_t w = down(a, WIN); ok && !lazy_commit() && w < end; w += WIN) {
        uint64_t at = w > r->base ? w : r->base;
        if (win_mapped(r, at)) continue;
        ok = map_window(r, at);
        uint64_t wend = w + WIN < r->end ? w + WIN : r->end;
        if (at < lo) lo = at;
        if (wend > hi) hi = wend;
    }
    if (ok) {
        uint64_t s0 = (down(addr, SUB) - r->base) / SUB, s1 = (up(addr + n, SUB) - r->base) / SUB;
        memset(r->sub + s0, SS_RW, (size_t)(s1 - s0));
        ok = apply_range(r, lo, hi);
    } else if (r) {
        apply_range(r, lo, hi);   // as janelas que chegaram a mapear voltam sem acesso da CPU
    }
    plat_mutex_unlock(&vm_lock);
    return ok;
}

int plat_mem_decommit(uint64_t addr, uint64_t n) {
    if (!n) return 1;
    plat_mutex_lock(&vm_lock);
    Resv *r = resv_of_range(addr, n);
    if (!r) {
        plat_mutex_unlock(&vm_lock);
        return 0;
    }
    uint64_t a = down(addr, KPAGE), end = up(addr + n, KPAGE);
    uint64_t s = down(addr, SUB), se = up(addr + n, SUB);
    // Zera as subpáginas commitadas do pedido com a CPU liberada nelas (podem estar só para leitura, vigiadas ou sem
    // acesso). Uma subpágina não commitada de janela mapeada já vale zero.
    for (uint64_t pg = a; pg < end; pg += KPAGE) {
        if (!win_mapped(r, pg)) continue;
        uint64_t x0 = pg > s ? pg : s, x1 = pg + KPAGE < se ? pg + KPAGE : se;
        int dirty = 0;
        for (uint64_t x = x0; x < x1; x += SUB) dirty |= r->sub[(x - r->base) / SUB] >= SS_NONE;
        if (!dirty) continue;
        sceKernelMprotect((void *)(uintptr_t)pg, KPAGE, K_PROT_CPU_R | K_PROT_CPU_W | K_PROT_GPU);
        for (uint64_t x = x0; x < x1; x += SUB)
            if (r->sub[(x - r->base) / SUB] >= SS_NONE) memset((void *)(uintptr_t)x, 0, SUB);
    }
    memset(r->sub + (s - r->base) / SUB, SS_RESERVED, (size_t)((se - s) / SUB));
    if (r->ww)
        for (uint64_t pg = a; pg < end; pg += KPAGE) {
            // Página sem subpágina commitada: volta a limpa (no Linux, uma página descartada não conta como escrita).
            // Com subpáginas restantes: escrita, por segurança, porque a CPU ficou liberada enquanto zerávamos.
            int left = page_req(r, pg) >= 0;
            __atomic_store_n(&r->ww[(pg - r->base) / KPAGE], left ? WW_DIRTY : WW_CLEAN, __ATOMIC_RELEASE);
        }
    int ok = apply_range(r, a, end);
    plat_mutex_unlock(&vm_lock);
    return ok;
}

// Só subpáginas commitadas, como no Windows: com alguma não commitada no pedido, nada muda e devolve 0.
int plat_mem_protect(uint64_t addr, uint64_t n, int prot) {
    if (!n) return 1;
    plat_mutex_lock(&vm_lock);
    Resv *r = resv_of_range(addr, n);
    int ok = r != NULL;
    uint64_t s = down(addr, SUB), se = up(addr + n, SUB);
    for (uint64_t x = s; ok && x < se; x += SUB) ok = r->sub[(x - r->base) / SUB] >= SS_NONE;
    if (ok) {
        memset(r->sub + (s - r->base) / SUB, SS_NONE + prot, (size_t)((se - s) / SUB));
        ok = apply_range(r, down(addr, KPAGE), up(addr + n, KPAGE));
    }
    plat_mutex_unlock(&vm_lock);
    return ok;
}

int plat_mem_state(uint64_t addr, uint64_t limit, uint64_t *end) {
    Resv *r = resv_of(addr);
    if (!r) {
        // Livre até a próxima reserva (ou limit).
        uint64_t e = limit;
        int nr = __atomic_load_n(&nresv, __ATOMIC_ACQUIRE);
        for (int i = 0; i < nr; i++)
            if (resv[i].base > addr && resv[i].base < e) e = resv[i].base;
        *end = e;
        return PLAT_MEM_FREE;
    }
    uint64_t lim = limit < r->end ? limit : r->end, i = (addr - r->base) / SUB;
    uint8_t v = r->sub[i];
    while (r->base + (i + 1) * SUB < lim && r->sub[i + 1] == v) i++;
    *end = r->base + (i + 1) * SUB < lim ? r->base + (i + 1) * SUB : lim;
    return v == SS_RESERVED ? PLAT_MEM_RESERVED : v - SS_NONE;
}

// As subpáginas commitadas das páginas de 16 KiB escritas, em ordem de endereço. Uma página só é rearmada (reset)
// quando cabe inteira no intervalo pedido e todas as suas subpáginas couberam em pages: rearmar uma página que avança
// para fora do intervalo perderia escritas que a consulta vizinha ainda não viu. Reportar a mais é seguro; a menos não.
//
// Rearmar sem perder escrita concorrente: DIRTY -> RESET, mprotect para leitura, depois RESET -> CLEAN só se ninguém
// escreveu no meio (o tratador grava DIRTY depois de liberar a página, e aí a página fica escrita e liberada).
uint64_t plat_mem_written(uint64_t addr, uint64_t n, uint64_t *pages, uint64_t max, int reset) {
    if (!max || !n) return 0;
    Resv *r = resv_of_range(addr, n);
    if (!r || !r->ww) return UINT64_MAX;
    plat_mutex_lock(&vm_lock);
    uint64_t got = 0, end = addr + n, rearm_lo = 0, rearm_hi = 0;
    for (uint64_t pg = down(addr, KPAGE); pg < end && got < max; pg += KPAGE) {
        uint8_t *w = &r->ww[(pg - r->base) / KPAGE];
        if (__atomic_load_n(w, __ATOMIC_ACQUIRE) != WW_DIRTY) continue;
        uint64_t x0 = pg > addr ? pg : down(addr, SUB), x1 = pg + KPAGE < end ? pg + KPAGE : up(end, SUB);
        uint64_t cnt = 0;
        for (uint64_t x = x0; x < x1; x += SUB) cnt += r->sub[(x - r->base) / SUB] >= SS_NONE;
        if (got && got + cnt > max) break;   // a página inteira fica para a próxima chamada
        uint64_t put = 0;
        for (uint64_t x = x0; x < x1 && got < max; x += SUB)
            if (r->sub[(x - r->base) / SUB] >= SS_NONE) pages[got++] = x, put++;
        if (reset && put == cnt && pg >= addr && pg + KPAGE <= end) {
            uint8_t d = WW_DIRTY;
            if (__atomic_compare_exchange_n(w, &d, WW_RESET, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                if (!rearm_hi) rearm_lo = pg;
                rearm_hi = pg + KPAGE;
            }
        }
    }
    if (rearm_hi) {
        apply_range(r, rearm_lo, rearm_hi);
        for (uint64_t pg = rearm_lo; pg < rearm_hi; pg += KPAGE) {
            uint8_t s = WW_RESET;
            __atomic_compare_exchange_n(&r->ww[(pg - r->base) / KPAGE], &s, WW_CLEAN, 0, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE);
        }
        apply_range(r, rearm_lo, rearm_hi);   // as que voltaram a CLEAN depois do primeiro passe ficam armadas
    }
    plat_mutex_unlock(&vm_lock);
    return got;
}

// ---- faltas. Um tratador para os sinais síncronos, instalado como a sonda instalou (SA_SIGINFO|SA_NODEFER com os
// valores do FreeBSD, do signal.h do overlay). Ordem:
//   1. write-watch: falta numa página vigiada, armada, cuja proteção pedida é RW. É uma escrita por construção (a
//      página só está somente leitura porque a armamos), então não depende do código de erro do ucontext. Vem antes
//      do gancho do runtime porque o gancho (memory.c thaw_fault) devolve 1 para toda página pedida RW, e a escrita
//      repetiria contra a mesma página armada para sempre. No linux.c esta etapa é o próprio kernel (userfaultfd
//      assíncrono), que também vem antes do gancho.
//   2. o gancho de escrita do runtime (memory.c descongela uma região). Se ele torna a página RW e ela ainda está
//      armada, a escrita repetida falta mais uma vez e cai em 1: a escrita depois do descongelamento é reportada.
//   3. o gancho de crash, se a instrução está no executável; depois a ação padrão.
//
// O siginfo e o ucontext são os do FreeBSD, não os do musl: si_addr no deslocamento 24 (provado na sonda); no
// ucontext do Orbis, em palavras de 8 bytes desde o início, rip em [28] e rsp em [31] (anotado no galaxy-recomp), e
// por consequência o código de erro da falta em [27]. A CONFIRMAR NO CONSOLE: rip, rsp e o código de erro.
#define UC_ERR 27
#define UC_RIP 28
#define UC_RSP 31

static int (*fault_hook)(uint64_t addr);
static void (*crash_fn)(const PlatCrash *c);

static int ww_fault(uint64_t a) {
    Resv *r = resv_of(a);
    if (!r || !r->ww || !win_mapped(r, a)) return 0;
    uint64_t pg = down(a, KPAGE);
    uint8_t *w = &r->ww[(pg - r->base) / KPAGE];
    // Pedida RW e faltou: a proteção do hardware está atrasada em qualquer estado, inclusive DIRTY (um apply_range
    // concorrente pode ter lido CLEAN entre o mprotect abaixo e a marca DIRTY e rearmado a página). Corrigir sempre
    // evita o laço eterno com o gancho do runtime, que devolveria 1 para esta página RW.
    if (page_req(r, pg) != PLAT_PROT_RW) return 0;
    if (sceKernelMprotect((void *)(uintptr_t)pg, KPAGE, K_PROT_CPU_R | K_PROT_CPU_W | K_PROT_GPU)) return 0;
    __atomic_store_n(w, WW_DIRTY, __ATOMIC_RELEASE);   // depois de liberar: ver plat_mem_written
    return 1;
}

static void fault_handler(int sig, siginfo_t *si, void *ucv) {
    uint64_t a = *(uint64_t *)((char *)si + 24);
    const uint64_t *uc = ucv;
    if (sig == ORBIS_SIGSEGV || sig == ORBIS_SIGBUS) {
        if (lazy_fault(a)) return;
        if (ww_fault(a)) return;
        if (fault_hook && fault_hook(a)) return;
    }
    uintptr_t rip = (uintptr_t)uc[UC_RIP];
    uintptr_t lo = (uintptr_t)__text_start, hi = (uintptr_t)__eh_frame_start;
    if (crash_fn && (!lo || (rip >= lo && (!hi || rip < hi)))) {
        PlatCrash c;
        memset(&c, 0, sizeof c);
        c.what = sig == ORBIS_SIGSEGV ? "access violation" : sig == ORBIS_SIGBUS ? "bus error"
               : sig == ORBIS_SIGILL ? "illegal instruction" : "arithmetic exception";
        c.code = (uint32_t)sig, c.rip = rip, c.offset = rip - lo, c.access = -1;
        if (sig == ORBIS_SIGSEGV || sig == ORBIS_SIGBUS) {
            uint64_t err = uc[UC_ERR];
            c.access = (err & 16) ? 8 : (err & 2) ? 1 : 0, c.addr = a;
        }
        crash_fn(&c);
    }
    // Ação padrão e volta: a instrução falta de novo e o processo termina. Sem signal(): o wrapper do musl foi
    // compilado com o SA_RESTART do Linux.
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_DFL;
    sigaction(sig, &sa, NULL);
}

static void fault_install(void) {
    static int done;
    if (done) return;
    done = 1;
    alt_stack();
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigaction(ORBIS_SIGSEGV, &sa, NULL);
    sigaction(ORBIS_SIGBUS, &sa, NULL);
    sigaction(ORBIS_SIGILL, &sa, NULL);
    sigaction(ORBIS_SIGFPE, &sa, NULL);
}

void plat_mem_fault_hook(int (*fn)(uint64_t addr)) {
    fault_hook = fn;
    fault_install();
}

void plat_crash_hook(void (*fn)(const PlatCrash *c)) {
    crash_fn = fn;
    fault_install();
}

// STUB fatal: no PS4 o processo não reexecuta a si mesmo. As regiões fixas do runtime estão livres no console (Fase 0),
// então memory.c não deveria chegar aqui.
void plat_reexec(void) {
    fprintf(stderr, "FATAL: plat_reexec não existe no PS4 (uma região fixa da memória estava ocupada)\n");
    fflush(stderr);
    plat_exit(70);
}

// ---- leitura de memória sem falta: sceKernelVirtualQuery em cada trecho (flags 0: o mapa que contém o endereço; a
// struct é a que a sonda usou). A CONFIRMAR NO CONSOLE: flags 0 e o significado de prot para trechos só reservados.
typedef struct {
    uint64_t start, end, off;
    int32_t prot, type;
    uint8_t flags;
    char name[32];
} KVQuery;

static int range_readable(uint64_t addr, uint64_t n) {
    uint64_t end = addr + (n ? n : 1);
    for (uint64_t a = addr; a < end;) {
        KVQuery q;
        memset(&q, 0, sizeof q);
        if (sceKernelVirtualQuery((void *)(uintptr_t)a, 0, &q, sizeof q) || q.start > a || q.end <= a ||
            !(q.prot & K_PROT_CPU_R))
            return 0;
        a = q.end;
    }
    return 1;
}

int plat_mem_read(uint64_t addr, void *out, size_t n) {
    if (!range_readable(addr, n)) return 0;
    memcpy(out, (const void *)(uintptr_t)addr, n);
    return 1;
}

int plat_mem_readable(uint64_t addr, uint64_t n) { return range_readable(addr, n); }

void *plat_aligned_alloc(size_t size, size_t align) {
    void *p = NULL;
    return posix_memalign(&p, align < sizeof(void *) ? sizeof(void *) : align, size) ? NULL : p;
}

void plat_aligned_free(void *p) { free(p); }

// ---- arquivos abertos para leitura posicional. Um PlatFile é o descritor + 1.
PlatFile *plat_file_open(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    return fd < 0 ? NULL : (PlatFile *)(intptr_t)(fd + 1);
}

PlatFile *plat_file_open_unbuffered(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_DIRECT);
    return fd < 0 ? NULL : (PlatFile *)(intptr_t)(fd + 1);
}

static int fd_of(PlatFile *f) { return (int)(intptr_t)f - 1; }

void plat_file_close(PlatFile *f) {
    if (f) close(fd_of(f));
}

int64_t plat_file_pread(PlatFile *f, void *buf, uint32_t n, uint64_t off) {
    // O kernel escreve direto no buffer: janela não mapeada ou página vigiada (CPU só leitura) dariam EFAULT, não falta.
    orbis_mem_prefault((uint64_t)(uintptr_t)buf, n);
    for (uint64_t pg = down((uint64_t)(uintptr_t)buf, KPAGE); n && pg < (uint64_t)(uintptr_t)buf + n; pg += KPAGE)
        ww_fault(pg);
    ssize_t k;
    do k = pread(fd_of(f), buf, n, (off_t)off);
    while (k < 0 && errno == EINTR);
    return k >= 0 ? k : errno == EFAULT ? -2 : -1;
}

int plat_file_id(PlatFile *f, PlatFileId *out) {
    struct stat st;
    if (fstat(fd_of(f), &st)) return 0;
    *out = (PlatFileId){(uint64_t)st.st_dev, (uint64_t)st.st_ino, (uint64_t)st.st_size,
                        (uint64_t)st.st_mtim.tv_sec * 1000000000u + (uint64_t)st.st_mtim.tv_nsec,
                        (uint64_t)st.st_ctim.tv_sec * 1000000000u + (uint64_t)st.st_ctim.tv_nsec};
    return 1;
}

// mmap de arquivo: o orbis-compat mediu no console um mapa de arquivo igual ao read() byte a byte.
const void *plat_file_map(PlatFile *f) {
    struct stat st;
    if (fstat(fd_of(f), &st) || !st.st_size) return NULL;
    void *v = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd_of(f), 0);
    return v == MAP_FAILED ? NULL : v;
}

static int fs_err(int e) {
    switch (e) {
    case ENOENT: case ENOTDIR: return PLAT_FS_NOT_FOUND;
    case EEXIST: return PLAT_FS_EXISTS;
    case ENOTEMPTY: return PLAT_FS_NOT_EMPTY;
    case EBUSY: case ETXTBSY: return PLAT_FS_BUSY;
    default: return PLAT_FS_ERROR;
    }
}

int plat_fs_kind(const char *path) {
    struct stat st;
    if (lstat(path, &st)) return PLAT_FS_MISSING;
    return S_ISLNK(st.st_mode) ? PLAT_FS_LINK : S_ISDIR(st.st_mode) ? PLAT_FS_DIR : PLAT_FS_FILE;
}

int plat_fs_mkdir(const char *path) { return mkdir(path, 0755) ? fs_err(errno) : PLAT_FS_OK; }

int plat_fs_rmdir(const char *path) {
    struct stat st;
    if (!lstat(path, &st) && S_ISLNK(st.st_mode)) return unlink(path) ? fs_err(errno) : PLAT_FS_OK;
    return rmdir(path) ? fs_err(errno) : PLAT_FS_OK;
}

int plat_fs_unlink(const char *path) { return unlink(path) ? fs_err(errno) : PLAT_FS_OK; }

// Sem renameat2(RENAME_NOREPLACE) no FreeBSD: confere antes. Outro processo poderia correr entre a conferência e o
// rename, mas no console só este processo mexe em /data/ocarina.
int plat_fs_rename(const char *from, const char *to, int durable) {
    if (plat_fs_kind(to) != PLAT_FS_MISSING) return PLAT_FS_EXISTS;
    if (rename(from, to)) return fs_err(errno);
    if (durable) {
        sync_dir_of(from);
        sync_dir_of(to);
    }
    return PLAT_FS_OK;
}

// A CONFIRMAR NO CONSOLE: o link() do libc.a do SDK (o musl o implementa sobre linkat).
int plat_fs_link(const char *existing, const char *path) { return link(existing, path) ? fs_err(errno) : PLAT_FS_OK; }

int plat_fs_links(const char *path) {
    struct stat st;
    return stat(path, &st) ? 0 : (int)st.st_nlink;
}

void plat_fs_sync_dir(const char *path) {
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
}

void plat_fs_busy_reason(const char *dir, char *out, size_t cap) {
    (void)dir;
    if (cap) *out = 0;
}

// lstat no caminho completo em vez de fstatat(dirfd(d), ...): a família stat corrigida pelo overlay é a que foi medida.
int plat_fs_list(const char *dir, int (*cb)(const char *name, int kind, uint64_t size, void *ctx), void *ctx) {
    DIR *d = opendir(dir);
    if (!d) return fs_err(errno);
    for (struct dirent *e; (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char path[PATH_MAX];
        if (snprintf(path, sizeof path, "%s/%s", dir, e->d_name) >= (int)sizeof path) continue;
        struct stat st;
        if (lstat(path, &st)) continue;
        int kind = S_ISLNK(st.st_mode) ? PLAT_FS_LINK : S_ISDIR(st.st_mode) ? PLAT_FS_DIR : PLAT_FS_FILE;
        if (!cb(e->d_name, kind, kind == PLAT_FS_FILE ? (uint64_t)st.st_size : 0, ctx)) break;
    }
    closedir(d);
    return PLAT_FS_OK;
}

PlatFile *plat_fs_open(const char *path, int write, int *err) {
    struct stat st;
    int fd = open(path, (write ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (fd >= 0 && (fstat(fd, &st) || !S_ISREG(st.st_mode))) {
        close(fd);
        fd = -1, errno = ENOENT;
    }
    if (fd < 0 && err) *err = fs_err(errno);
    return fd < 0 ? NULL : (PlatFile *)(intptr_t)(fd + 1);
}

PlatFile *plat_fs_create(const char *path, int *err) {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0 && err) *err = fs_err(errno);
    return fd < 0 ? NULL : (PlatFile *)(intptr_t)(fd + 1);
}

int64_t plat_file_pwrite(PlatFile *f, const void *buf, uint32_t n, uint64_t off) {
    orbis_mem_prefault((uint64_t)(uintptr_t)buf, n);   // o kernel lê direto do buffer (commit preguiçoso)
    ssize_t k;
    do k = pwrite(fd_of(f), buf, n, (off_t)off);
    while (k < 0 && errno == EINTR);
    return k;
}

int plat_file_set_size(PlatFile *f, uint64_t size) { return ftruncate(fd_of(f), (off_t)size) ? fs_err(errno) : 0; }

int plat_file_fsync(PlatFile *f) { return fsync(fd_of(f)) == 0; }

// Temporários em /data/ocarina/tmp (criado na primeira chamada): /tmp não é gravável para um app do PS4.
int plat_temp_dir(char *out, size_t cap) {
    static int made;
    if (!made) {
        plat_file_mkdir_p(ORBIS_DATA_DIR "/tmp");
        made = 1;
    }
    return snprintf(out, cap, "%s", ORBIS_DATA_DIR "/tmp") < (int)cap;
}

// O executável é /app0/eboot.bin, mas /app0 é somente leitura e o PKG só leva o eboot. O runtime usa plat_exe_path
// apenas para achar a pasta "ao lado do executável" (totk.ini, shader-cache, pipeline-cache.bin, romfs-index.bin,
// romfs-patches, exefs/romfs/profile portáteis, mesa-cache, logs): no PS4 essa pasta é /data/ocarina, então o caminho
// devolvido é o de um eboot.bin que estaria lá. Ninguém lê o arquivo em si (plat_exe_base e plat_module_of não usam
// isto). A alternativa seria um TOTK_* por caminho no args.txt.
int plat_exe_path(char *out, size_t cap) { return snprintf(out, cap, ORBIS_DATA_DIR "/eboot.bin") < (int)cap; }

// Sem dladdr: o início do .text pelo símbolo do script de ligação.
uintptr_t plat_exe_base(void) { return (uintptr_t)__text_start; }

int plat_module_of(uintptr_t addr, char *name, size_t cap, uintptr_t *base) {
    uintptr_t lo = (uintptr_t)__text_start, hi = (uintptr_t)__eh_frame_start;
    if (!lo || addr < lo || (hi && addr >= hi)) return 0;
    *base = lo;
    return snprintf(name, cap, "eboot.bin") < (int)cap;
}

size_t plat_getenv(const char *name, char *out, size_t cap) {
    const char *v = getenv(name);
    size_t n = v ? strlen(v) : 0;
    if (!n || n >= cap) return 0;
    memcpy(out, v, n + 1);
    return n;
}

// Um id por thread, em cache como no linux.c (plat_rmutex_lock pergunta a cada aquisição).
uint32_t plat_thread_id(void) {
    static __thread uint32_t tid;
    if (!tid) tid = (uint32_t)pthread_getthreadid_np();
    return tid;
}

// STUB: idioma e região fixos em inglês dos EUA. TODO: sceSystemServiceParamGetInt(SCE_SYSTEM_SERVICE_PARAM_ID_LANG)
// mapeado para BCP-47 (a libSceSystemService exporta a função; falta a tabela de idiomas do PS4).
const char *plat_ui_language(void) { return "en-US"; }
const char *plat_user_locale(void) { return "en-US"; }

// STUB: o sistema não oferece arquivos de fonte ao app; o menu cai na fonte embutida.
int plat_font_path(const char *lang, char *out, size_t cap) {
    (void)lang, (void)out, (void)cap;
    return 0;
}

int plat_os_version(char *out, size_t cap) {
    return snprintf(out, cap, "%s", sceKernelIsNeoMode() ? "PlayStation 4 Pro" : "PlayStation 4") < (int)cap;
}

// STUB: o stderr do app no PS4 não é um arquivo com caminho conhecido.
int plat_stderr_path(char *out, size_t cap) {
    (void)out, (void)cap;
    return 0;
}

// ---- processo

// Um app do PS4 não sai sozinho: o _exit/exit do musl faz a chamada de sistema do Linux (exit_group), que o kernel do
// PS4 não tem, e o processo morre com SIGSYS e o relatório de erro (medido em 2026-11-07, no fim do --probe-orbis; o
// galaxy-recomp já tinha anotado "retornar do main = SIGSYS"). Então registra o código e fica ocioso até o usuário
// fechar pelo botão PS, chamando sceGnmSubmitDone para a suspensão do fechamento não estourar (0xa0d0c00a).
extern int32_t sceGnmSubmitDone(void);
_Noreturn void orbis_idle_exit(int code) {
    fprintf(stderr, "[host] orbis: fim (código %d); o app fica parado até ser fechado pelo botão PS\n", code);
    fflush(stdout);
    fflush(stderr);
    for (;;) {
        sceGnmSubmitDone();
        usleep(100000);
    }
}

void plat_exit(int code) { orbis_idle_exit(code); }

// O main do runtime (main.c, renomeado para rt_main no PS4) volta por vários caminhos; aqui ele nunca retorna.
int rt_main(int argc, char **argv);
int main(int argc, char **argv) { orbis_idle_exit(rt_main(argc, argv)); }

int plat_setenv(const char *name, const char *value) {
    return (value ? setenv(name, value, 1) : unsetenv(name)) == 0;
}

uint32_t plat_process_id(void) { return (uint32_t)getpid(); }

// getrusage da libkernel: struct rusage começa com ru_utime e ru_stime nos dois lados (musl e FreeBSD). A CONFIRMAR
// NO CONSOLE.
uint64_t plat_process_cpu_ns(void) {
    struct rusage u;
    if (getrusage(RUSAGE_SELF, &u)) return 0;
    return ((uint64_t)u.ru_utime.tv_sec + (uint64_t)u.ru_stime.tv_sec) * 1000000000u +
           ((uint64_t)u.ru_utime.tv_usec + (uint64_t)u.ru_stime.tv_usec) * 1000u;
}

uint64_t plat_process_uptime_ns(void) { return sceKernelGetProcessTime() * 1000u; }

// CLOCK_REALTIME é o id 0 nas duas numerações.
uint64_t plat_unix_time_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

// O fuso: o musl lê TZ e /etc/localtime, que o app não tem, e cai em UTC. TODO: o fuso do sistema (sceRtc ou
// sceSystemServiceParamGetInt(TIME_ZONE)).
void plat_local_time(struct tm *out) {
    time_t t = time(NULL);
    localtime_r(&t, out);
}

#define PLAT_TIME_MAX 32535215999ll

int plat_localtime(int64_t unix_s, struct tm *out) {
    time_t t = (time_t)unix_s;
    return unix_s >= 0 && unix_s <= PLAT_TIME_MAX && localtime_r(&t, out) != NULL;
}

int plat_gmtime(int64_t unix_s, struct tm *out) {
    time_t t = (time_t)unix_s;
    return unix_s >= 0 && unix_s <= PLAT_TIME_MAX && gmtime_r(&t, out) != NULL;
}

long plat_tz_west(void) {
    tzset();
    return timezone;
}

// STUB: sem processos filhos no PS4. Quem usa (sondas que esperam um fatal no filho) trata -1 como "não deu para
// rodar".
int plat_run_self(const char *env_name, const char *env_value, char *out, size_t cap, uint32_t timeout_ms) {
    (void)env_name, (void)env_value, (void)timeout_ms;
    if (cap) out[0] = 0;
    return -1;
}

// sceRandomGetRandomNumber (libSceRandom; ligar -lSceRandom), no máximo 64 bytes por chamada. O getentropy do musl
// usa a syscall getrandom, que este kernel não tem.
int plat_random(void *out, size_t n) {
    uint8_t *p = out;
    while (n) {
        size_t k = n < 64 ? n : 64;
        if (sceRandomGetRandomNumber(p, k)) return 0;
        p += k, n -= k;
    }
    return 1;
}

// ---- área de trabalho: STUB, o PS4 não tem área de transferência nem gerenciador de arquivos para o app.
int plat_clipboard_set(const char *utf8) {
    (void)utf8;
    return 0;
}

int plat_open_folder(const char *path) {
    (void)path;
    return 0;
}

// ---- bibliotecas dinâmicas: STUB. O eboot é estático e não carrega .so de fornecedor de GPU (o NVML e afins do PC).
void *plat_lib_open(const char *name) {
    (void)name;
    return NULL;
}

void *plat_lib_sym(void *lib, const char *name) {
    (void)lib, (void)name;
    return NULL;
}

// ---- carga da máquina para o overlay de desempenho. STUB quase todo: sem /proc nem sysfs. A memória total é a direta
// do console; o resto fica -1 (desconhecido). TODO: sceKernelGetCpuTemperature para a temperatura.
struct PlatLoad {
    int unused;
};

static PlatLoad load_static;   // sem memória: plat_load_open nunca devolve NULL

PlatLoad *plat_load_open(const uint8_t *gpu_luid, int pci_bus) {
    (void)gpu_luid, (void)pci_bus;
    PlatLoad *l = calloc(1, sizeof *l);
    return l ? l : &load_static;
}

void plat_load_close(PlatLoad *l) {
    if (l != &load_static) free(l);
}

void plat_cpu_load(PlatLoad *l, float *total_pct, float *proc_pct, float *temp_c) {
    (void)l;
    *total_pct = *proc_pct = *temp_c = -1;
}

void plat_gpu_load(PlatLoad *l, float *load_pct, float *vram_used_mb) {
    (void)l;
    *load_pct = *vram_used_mb = -1;
}

void plat_mem_info(float *used_mb, float *total_mb, float *proc_mb) {
    *used_mb = *proc_mb = -1;
    *total_mb = (float)(sceKernelGetDirectMemorySize() >> 20);
}

// proc: memória direta mapeada nas reservas do runtime; limit: a memória direta do processo.
void plat_commit_info(double *proc_mb, double *used_mb, double *limit_mb) {
    mem_report("fatal");
    *proc_mb = (double)__atomic_load_n(&mapped_bytes, __ATOMIC_RELAXED) / (1 << 20);
    *used_mb = -1;
    *limit_mb = (double)sceKernelGetDirectMemorySize() / (1 << 20);
}

void plat_process_memory(uint64_t *resident, uint64_t *private_bytes) { *resident = *private_bytes = 0; }

// ---- processos filhos: o PS4 não cria processos (plat_child_start falha com ENOSYS). O tradutor de shaders roda no PC,
// e o canal com ele vira uma conexão TCP (plat_child_connect, OCARINA_XLATE_REMOTE=ip:porta; do outro lado,
// tools/oot/xlate_bridge.py). O app pode conectar, só não pode escutar (bind dá EACCES, medido no galaxy-recomp).
// Rede pela libSceNet (ligar com -lSceNet). A CONFIRMAR NO CONSOLE: se a libSceNet precisa de sceNetInit ou de
// sceSysmoduleLoadModule antes da primeira chamada.
extern int sceNetSocket(const char *name, int family, int type, int protocol);
extern int sceNetConnect(int s, const void *addr, unsigned int addrlen);
extern int sceNetSend(int s, const void *msg, size_t len, int flags);
extern int sceNetRecv(int s, void *buf, size_t len, int flags);
extern int sceNetSocketClose(int s);
extern int sceNetSetsockopt(int s, int level, int optname, const void *optval, unsigned int optlen);
extern int sceNetInetPton(int af, const char *src, void *dst);

struct PlatChild { int s; };

PlatChild *plat_child_start(const char *exe, const char *arg, uint32_t *err) {
    (void)exe, (void)arg;
    if (err) *err = ENOSYS;
    return NULL;
}

PlatChild *plat_child_connect(const char *host_port, uint32_t *err) {
    if (err) *err = 0;
    char host[64];
    const char *colon = strrchr(host_port, ':');
    if (!colon || (size_t)(colon - host_port) >= sizeof host) return NULL;
    memcpy(host, host_port, (size_t)(colon - host_port));
    host[colon - host_port] = 0;
    int port = atoi(colon + 1);
    // sockaddr_in do FreeBSD: sin_len, sin_family (AF_INET = 2), porta big-endian, endereço, 8 zeros
    struct { uint8_t len, family; uint16_t port; uint32_t addr; uint8_t zero[8]; } sa = {16, 2, 0, 0, {0}};
    sa.port = (uint16_t)(((port & 0xff) << 8) | ((port >> 8) & 0xff));
    if (sceNetInetPton(2, host, &sa.addr) <= 0) {
        if (err) *err = EINVAL;
        return NULL;
    }
    int sk = sceNetSocket("ocarina-xlate", 2, 1 /* SOCK_STREAM */, 0);
    if (sk < 0) {
        if (err) *err = (uint32_t)sk;
        return NULL;
    }
    int r = sceNetConnect(sk, &sa, sizeof sa);
    if (r < 0) {
        if (err) *err = (uint32_t)r;
        sceNetSocketClose(sk);
        return NULL;
    }
    int one = 1;
    sceNetSetsockopt(sk, 6 /* IPPROTO_TCP */, 1 /* TCP_NODELAY */, &one, sizeof one);
    PlatChild *c = malloc(sizeof *c);
    if (!c) {
        sceNetSocketClose(sk);
        return NULL;
    }
    c->s = sk;
    return c;
}

void plat_child_stop(PlatChild *c) {
    if (!c) return;
    sceNetSocketClose(c->s);
    free(c);
}

int plat_child_write(PlatChild *c, const void *data, size_t n) {
    const uint8_t *p = data;
    while (n) {
        int k = sceNetSend(c->s, p, n, 0);
        if (k <= 0) return 0;
        p += k, n -= (size_t)k;
    }
    return 1;
}

long plat_child_read(PlatChild *c, void *buf, size_t cap) {
    int k = sceNetRecv(c->s, buf, cap, 0);
    return k > 0 ? (long)k : -1;
}

#endif /* __ORBIS__ */
