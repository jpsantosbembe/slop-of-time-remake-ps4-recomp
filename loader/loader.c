/* Carregador de desenvolvimento no PS4 (PKG "<jogo> (dev)", nome e ID lidos do jogo; instalado UMA vez).
 *
 * Lê SLOP_DATA_DIR/game.elf (o ELF CRU do build do runtime, não o fself), mapeia numa base fixa, aplica as relocações,
 * resolve os imports nos módulos do sistema que este eboot já carregou e pula no _start do jogo. Cada build novo é só
 * "subir o ELF por FTP + reload", sem PKG e sem reinstalar. Molde: o loader QVOX00002 do galaxy-recomp.
 *
 * TLS: o código do jogo acessa o próprio TLS por offset fixo em %fs (modelo local-exec), e quem define o bloco TLS
 * estático do processo é ESTE eboot. O PT_TLS do jogo não tem dados iniciais (só .tbss), então basta este eboot
 * reservar um bloco maior ou igual (ocl_tls_reserve) e, na thread principal, zerar a faixa do jogo antes do salto. As
 * threads novas já nascem com o bloco zerado, como num boot normal.
 *
 * Pasta de dados: SLOP_DATA_DIR (/data/slop/<title id do Switch>), passada pelo build.sh na compilação a partir do
 * game_info.py; nada do jogo fixo aqui. O jogo recebe no argv[0] o caminho do game.elf e usa a mesma pasta.
 *
 * Controle (cliente, porque o app não pode escutar): a cada 2 s conecta no IP e porta de SLOP_DATA_DIR/dev-host.txt
 * ("ip porta"), manda uma linha de estado e lê um comando:
 *   reload  -> sceSystemServiceLoadExec(/app0/eboot.bin): processo novo, que relê o ELF. Funciona com o jogo travado.
 *   ping    -> só responde o estado.
 *
 * Tudo vai para o klog com o prefixo "[ocl]". Endereço de crash no jogo: subtrair a base que o log mostra. */
#include <elf.h>
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef SLOP_DATA_DIR
#error "compile com -DSLOP_DATA_DIR=\"/data/slop/<title id>\" (loader/build.sh, de game_info.py --campo ps4_data_dir)"
#endif
#define GAME_PATH SLOP_DATA_DIR "/game.elf"
#define HOST_PATH SLOP_DATA_DIR "/dev-host.txt"
#define TLS_RESERVE 0x2000
#define TLS_MAX_ALIGN 64

/* --- libkernel e companhia (declarações próprias: alguns headers do SDK têm assinaturas erradas) --------------- */
struct ocl_modinfo {
    size_t size;
    char name[256];
    struct {
        void *address;
        uint32_t size;
        int32_t prot;
    } seg[4];
    uint32_t nseg;
    uint8_t fingerprint[20];
};
int sceKernelDebugOutText(int ch, const char *text);
int sceKernelOpen(const char *path, int flags, int mode);
int64_t sceKernelPread(int fd, void *buf, size_t n, int64_t off);
int sceKernelClose(int fd);
int sceKernelUsleep(unsigned us);
int sceKernelGetModuleList(uint32_t *handles, size_t max, size_t *n);
int sceKernelGetModuleInfo(uint32_t handle, struct ocl_modinfo *info);
int sceKernelDlsym(uint32_t handle, const char *name, void **addr);
int sceKernelLoadStartModule(const char *path, size_t argc, const void *argp, uint32_t flags, void *opt, int *res);
const char *sceKernelGetFsSandboxRandomWord(void);
int sceKernelAvailableFlexibleMemorySize(size_t *size);
size_t sceKernelGetDirectMemorySize(void);
int sceKernelAllocateDirectMemory(int64_t start, int64_t end, size_t len, size_t align, int type, int64_t *phys);
int sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags, int64_t phys, size_t align);
int sceKernelMprotect(const void *addr, size_t len, int prot);
void *mmap(void *addr, size_t len, int prot, int flags, int fd, int64_t off);
int sceGnmSubmitDone(void);
int sceSystemServiceLoadExec(const char *path, const char **argv);
/* Sockets BSD da libkernel (provados no galaxy-recomp: o app conecta, só não pode escutar). A libSceNet sem
 * sceNetInit não foi testada no console. */
int socket(int family, int type, int proto);
int connect(int s, const void *addr, unsigned len);
int64_t read(int fd, void *buf, size_t n);
int64_t write(int fd, const void *buf, size_t n);
int close(int fd);
/* Bibliotecas que o jogo importa e que este eboot não usa: um endereço de cada faz o create-fself manter o import,
 * e o sistema as carrega para este processo antes do main. Se o jogo passar a pedir outra, o carregador ainda tenta
 * carregá-la pelo DT_NEEDED (ensure_needed). */
int scePadInit(void);
int sceAudioOutInit(void);
int sceUserServiceInitialize(void *);
int sceRandomGetRandomNumber(void *, size_t);
int sceSysmoduleLoadModule(uint16_t);
int sceVideoOutOpen(int, int, int, const void *);
__attribute__((used)) void *const ocl_keep_imports[] = {
    (void *)scePadInit, (void *)sceAudioOutInit, (void *)sceUserServiceInitialize, (void *)sceRandomGetRandomNumber,
    (void *)sceSysmoduleLoadModule, (void *)sceVideoOutOpen,
};

#define PROT_R 1
#define PROT_W 2
#define PROT_X 4
#define MAP_PRIV 0x2
#define MAP_FIX 0x10
#define MAP_NO_OVERWRITE 0x80
#define MAP_ANONYMOUS 0x1000

/* O bloco TLS do processo inteiro: maior que o do jogo, sem dados iniciais. Pequeno de propósito: no PS4 o teto de
 * threads vivas cai com o tamanho do bloco TLS (64 KiB davam umas 30 threads; 2026-11-07). O jogo tinha 0xa1b8 até o
 * rhit do vk_memory.c sair do TLS; depois, uns 5 KiB. */
__thread unsigned char ocl_tls_reserve[TLS_RESERVE] __attribute__((aligned(TLS_MAX_ALIGN)));

static volatile const char *g_state = "iniciando";
static volatile uintptr_t g_base = 0;
static volatile uint64_t g_elf_size = 0;

static void llog(const char *fmt, ...) {
    char b[320];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b + 6, sizeof b - 8, fmt, ap);
    va_end(ap);
    memcpy(b, "[ocl] ", 6);
    if (n < 0) n = 0;
    if (n > (int)sizeof b - 8) n = (int)sizeof b - 8;
    b[6 + n] = '\n';
    b[7 + n] = 0;
    sceKernelDebugOutText(0, b);
}

/* --- Canal de controle --------------------------------------------------------------------------------------- */
struct ocl_sockaddr_in {
    uint8_t len, family;
    uint16_t port;
    uint32_t addr;
    uint8_t zero[8];
};

static int read_small(const char *path, char *buf, size_t cap) {
    int fd = sceKernelOpen(path, 0, 0);
    if (fd < 0) return -1;
    int64_t n = sceKernelPread(fd, buf, cap - 1, 0);
    sceKernelClose(fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    return (int)n;
}

static void *ctrl_thread(void *arg) {
    (void)arg;
    int warned = 0;
    for (;; sceKernelUsleep(2000000)) {
        char host[96], ip[64];
        unsigned port = 0;
        if (read_small(HOST_PATH, host, sizeof host) < 0 || sscanf(host, "%63s %u", ip, &port) != 2 || !port) {
            if (!warned++) llog("controle: sem %s (\"ip porta\"); o reload remoto fica parado", HOST_PATH);
            continue;
        }
        struct ocl_sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.len = sizeof a;
        a.family = 2;
        a.port = (uint16_t)((port >> 8) | ((port & 0xff) << 8));
        unsigned q[4];
        if (sscanf(ip, "%u.%u.%u.%u", &q[0], &q[1], &q[2], &q[3]) != 4) continue;
        a.addr = q[0] | q[1] << 8 | q[2] << 16 | q[3] << 24;
        int s = socket(2, 1, 0);
        if (s < 0) continue;
        if (connect(s, &a, sizeof a) < 0) {
            close(s);
            continue;
        }
        char line[200];
        int n = snprintf(line, sizeof line, "ocl estado=%s base=0x%lx elf=%lu\n", (const char *)g_state,
                         (unsigned long)g_base, (unsigned long)g_elf_size);
        write(s, line, (size_t)n);
        char cmd[64];
        int got = 0;
        while (got < (int)sizeof cmd - 1) {
            int64_t r = read(s, cmd + got, 1);
            if (r <= 0 || cmd[got] == '\n') break;
            got++;
        }
        cmd[got] = 0;
        if (got && cmd[got - 1] == '\r') cmd[got - 1] = 0;
        if (strcmp(cmd, "reload") == 0) {
            write(s, "ok reload\n", 10);
            close(s);
            llog("controle: reload pedido por %s", ip);
            sceSystemServiceLoadExec("/app0/eboot.bin", NULL);
            llog("controle: LoadExec RETORNOU (falhou)");
            continue;
        }
        if (cmd[0]) {
            const char *r = strcmp(cmd, "ping") == 0 ? "ok\n" : "erro: comando desconhecido\n";
            write(s, r, strlen(r));
        }
        close(s);
    }
    return NULL;
}

/* --- Imports -------------------------------------------------------------------------------------------------- */
#define MAX_MODS 96
static uint32_t g_mods[MAX_MODS];
static char g_modnames[MAX_MODS][64];
static int g_nmods = 0;

/* Ordem de busca: libkernel primeiro, as libcs do sistema por último (o jogo traz a própria musl estática e não deve
 * ligar símbolo nenhum a elas por acidente). */
static void collect_modules(void) {
    uint32_t all[MAX_MODS];
    size_t n = 0;
    if (sceKernelGetModuleList(all, MAX_MODS, &n) != 0) n = 0;
    g_nmods = 0;
    for (int pass = 0; pass < 3; pass++) {
        for (size_t i = 0; i < n && g_nmods < MAX_MODS; i++) {
            struct ocl_modinfo info;
            memset(&info, 0, sizeof info);
            info.size = sizeof info;
            if (sceKernelGetModuleInfo(all[i], &info) != 0) continue;
            if (strcmp(info.name, "eboot.bin") == 0) continue;
            int cls = strstr(info.name, "libkernel") ? 0 : (strncmp(info.name, "libc", 4) == 0 ||
                                                           strstr(info.name, "LibcInternal")) ? 2 : 1;
            if (cls != pass) continue;
            g_mods[g_nmods] = all[i];
            snprintf(g_modnames[g_nmods], sizeof g_modnames[0], "%s", info.name);
            g_nmods++;
        }
    }
}

static int module_loaded(const char *base) {
    size_t bl = strlen(base);
    for (int i = 0; i < g_nmods; i++)
        if (strncmp(g_modnames[i], base, bl) == 0 && g_modnames[i][bl] == '.') return 1;
    return 0;
}

/* DT_NEEDED "libSceFoo.so": se o sistema não carregou libSceFoo.sprx para este eboot, carrega do sandbox. */
static void ensure_needed(const char *needed) {
    char base[64];
    snprintf(base, sizeof base, "%s", needed);
    char *dot = strstr(base, ".so");
    if (dot) *dot = 0;
    if (module_loaded(base)) return;
    char path[160];
    const char *word = sceKernelGetFsSandboxRandomWord();
    const char *pfx[2] = {word, "system"};
    for (int i = 0; i < 2; i++) {
        if (!pfx[i]) continue;
        snprintf(path, sizeof path, "/%s/common/lib/%s.sprx", pfx[i], base);
        int res = 0;
        int h = sceKernelLoadStartModule(path, 0, NULL, 0, NULL, &res);
        if (h >= 0) {
            llog("carregou %s (handle %d)", path, h);
            collect_modules();
            return;
        }
    }
    llog("AVISO: %s não está carregado e não foi possível carregá-lo", base);
}

static void *resolve(const char *name) {
    for (int i = 0; i < g_nmods; i++) {
        void *p = NULL;
        if (sceKernelDlsym(g_mods[i], name, &p) == 0 && p) return p;
    }
    return NULL;
}

/* --- Carga do ELF --------------------------------------------------------------------------------------------- */
static int read_at(int fd, void *dst, size_t n, uint64_t off) {
    uint8_t *d = dst;
    while (n) {
        size_t chunk = n > (16u << 20) ? (16u << 20) : n;
        int64_t r = sceKernelPread(fd, d, chunk, (int64_t)off);
        if (r <= 0) return -1;
        d += r;
        off += (uint64_t)r;
        n -= (size_t)r;
    }
    return 0;
}

static uint8_t *map_image(uint64_t span, const char **how) {
    static const uintptr_t bases[] = {0x40000000UL, 0x80000000UL, 0x100000000UL, 0x180000000UL};
    size_t flex = 0;
    sceKernelAvailableFlexibleMemorySize(&flex);
    llog("memória flexível livre: %lu MiB; a imagem precisa de %lu MiB", (unsigned long)(flex >> 20),
         (unsigned long)(span >> 20));
    for (unsigned i = 0; i < sizeof bases / sizeof bases[0]; i++) {
        /* 0x80 é o "não sobrescrever" da Sony; 0x4000 é o MAP_EXCL do FreeBSD, que o loader do galaxy usou */
        static const int excl[2] = {MAP_NO_OVERWRITE, 0x4000};
        for (int k = 0; k < 2; k++) {
            void *p = mmap((void *)bases[i], span, PROT_R | PROT_W, MAP_PRIV | MAP_FIX | excl[k] | MAP_ANONYMOUS, -1, 0);
            if (p == (void *)bases[i]) {
                *how = k ? "flexível, MAP_EXCL" : "flexível";
                return p;
            }
            llog("mmap flexível em 0x%lx (flag 0x%x) falhou (%p)", (unsigned long)bases[i], excl[k], p);
        }
    }
    /* Plano B: memória direta (o orçamento do jogo), mapeada RW e depois RX. */
    uint64_t len = (span + 0x1fffff) & ~0x1fffffUL;
    int64_t phys = 0;
    int r = sceKernelAllocateDirectMemory(0, (int64_t)sceKernelGetDirectMemorySize(), len, 0x200000, 0, &phys);
    if (r != 0) {
        llog("memória direta de %lu MiB falhou: 0x%x", (unsigned long)(len >> 20), r);
        return NULL;
    }
    for (unsigned i = 0; i < sizeof bases / sizeof bases[0]; i++) {
        void *a = (void *)bases[i];
        r = sceKernelMapDirectMemory(&a, len, PROT_R | PROT_W, MAP_FIX | MAP_NO_OVERWRITE, phys, 0x200000);
        if (r == 0 && a == (void *)bases[i]) {
            *how = "direta";
            return a;
        }
        llog("mapa direto em 0x%lx falhou: 0x%x", (unsigned long)bases[i], r);
    }
    return NULL;
}

static uint64_t g_tls_span = 0;

static int load_game(void (**entry)(void *)) {
    int fd = sceKernelOpen(GAME_PATH, 0, 0);
    if (fd < 0) {
        llog("sem %s (0x%x): suba o game.elf por FTP e mande reload", GAME_PATH, fd);
        return -1;
    }
    Elf64_Ehdr eh;
    Elf64_Phdr ph[32];
    if (read_at(fd, &eh, sizeof eh, 0) || memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 || eh.e_machine != EM_X86_64 ||
        eh.e_type != ET_DYN || eh.e_phnum > 32 || read_at(fd, ph, eh.e_phnum * sizeof ph[0], eh.e_phoff)) {
        llog("%s não é um ELF x86-64 PIE (é o ELF cru do build, não o eboot.bin?)", GAME_PATH);
        sceKernelClose(fd);
        return -1;
    }
    uint64_t span = 0, dyn_va = 0;
    for (int i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr + ph[i].p_memsz > span) span = ph[i].p_vaddr + ph[i].p_memsz;
        if (ph[i].p_type == PT_DYNAMIC) dyn_va = ph[i].p_vaddr;
        if (ph[i].p_type == PT_TLS) {
            uint64_t al = ph[i].p_align ? ph[i].p_align : 1;
            g_tls_span = (ph[i].p_memsz + al - 1) & ~(al - 1);
            llog("TLS do jogo: 0x%lx bytes (dados iniciais 0x%lx), alinhamento %lu; reserva do carregador 0x%x",
                 (unsigned long)ph[i].p_memsz, (unsigned long)ph[i].p_filesz, (unsigned long)al, TLS_RESERVE);
            if (ph[i].p_filesz != 0 || g_tls_span > TLS_RESERVE || al > TLS_MAX_ALIGN) {
                llog("ERRO: o TLS do jogo não cabe na reserva deste carregador; recompile e reinstale o carregador");
                sceKernelClose(fd);
                return -1;
            }
        }
    }
    span = (span + 0x3fff) & ~0x3fffUL;
    const char *how = "?";
    uint8_t *base = map_image(span, &how);
    if (!base) {
        sceKernelClose(fd);
        return -1;
    }
    llog("imagem de %lu MiB em %p (memória %s)", (unsigned long)(span >> 20), (void *)base, how);
    for (int i = 0; i < eh.e_phnum; i++)
        if (ph[i].p_type == PT_LOAD && ph[i].p_filesz &&
            read_at(fd, base + ph[i].p_vaddr, ph[i].p_filesz, ph[i].p_offset)) {
            llog("leitura do segmento %d falhou", i);
            sceKernelClose(fd);
            return -1;
        }
    sceKernelClose(fd);

    Elf64_Rela *rela = NULL, *jmprel = NULL;
    size_t relasz = 0, pltsz = 0;
    Elf64_Sym *symtab = NULL;
    const char *strtab = NULL;
    uint64_t needed[32];
    int nneeded = 0;
    for (Elf64_Dyn *d = (Elf64_Dyn *)(base + dyn_va); d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_RELA: rela = (Elf64_Rela *)(base + d->d_un.d_ptr); break;
        case DT_RELASZ: relasz = d->d_un.d_val; break;
        case DT_JMPREL: jmprel = (Elf64_Rela *)(base + d->d_un.d_ptr); break;
        case DT_PLTRELSZ: pltsz = d->d_un.d_val; break;
        case DT_SYMTAB: symtab = (Elf64_Sym *)(base + d->d_un.d_ptr); break;
        case DT_STRTAB: strtab = (const char *)(base + d->d_un.d_ptr); break;
        case DT_NEEDED: if (nneeded < 32) needed[nneeded++] = d->d_un.d_val; break;
        }
    }
    collect_modules();
    for (int i = 0; i < nneeded; i++) ensure_needed(strtab + needed[i]);
    llog("%d módulos para os imports", g_nmods);

    int nrel = 0, nimp = 0, missing = 0;
    for (int pass = 0; pass < 2; pass++) {
        Elf64_Rela *r = pass ? jmprel : rela;
        size_t cnt = (pass ? pltsz : relasz) / sizeof(Elf64_Rela);
        for (size_t i = 0; r && i < cnt; i++) {
            uint64_t *where = (uint64_t *)(base + r[i].r_offset);
            uint32_t type = ELF64_R_TYPE(r[i].r_info);
            if (type == R_X86_64_RELATIVE) {
                *where = (uint64_t)(uintptr_t)base + r[i].r_addend;
                nrel++;
                continue;
            }
            if (type == R_X86_64_NONE) continue;
            if (type != R_X86_64_GLOB_DAT && type != R_X86_64_JUMP_SLOT && type != R_X86_64_64) {
                llog("relocação do tipo %u não suportada (offset 0x%lx)", type, (unsigned long)r[i].r_offset);
                return -1;
            }
            Elf64_Sym *sym = &symtab[ELF64_R_SYM(r[i].r_info)];
            uint64_t s;
            if (sym->st_shndx != SHN_UNDEF) {
                s = (uint64_t)(uintptr_t)base + sym->st_value;
            } else {
                const char *nm = strtab + sym->st_name;
                void *p = resolve(nm);
                if (!p && ELF64_ST_BIND(sym->st_info) != STB_WEAK) {
                    llog("import NÃO resolvido: %s", nm);
                    missing++;
                }
                s = (uint64_t)(uintptr_t)p;
                nimp++;
            }
            *where = s + (type == R_X86_64_JUMP_SLOT ? 0 : (uint64_t)r[i].r_addend);
        }
    }
    llog("%d relocações relativas, %d imports, %d faltando", nrel, nimp, missing);
    if (missing) return -1;

    for (int i = 0; i < eh.e_phnum; i++)
        if (ph[i].p_type == PT_LOAD && (ph[i].p_flags & PF_X)) {
            uintptr_t lo = (uintptr_t)base + (ph[i].p_vaddr & ~0x3fffUL);
            uintptr_t hi = ((uintptr_t)base + ph[i].p_vaddr + ph[i].p_memsz + 0x3fff) & ~0x3fffUL;
            int r = sceKernelMprotect((void *)lo, hi - lo, PROT_R | PROT_X);
            if (r != 0) {
                llog("mprotect RX do código falhou: 0x%x", r);
                return -1;
            }
        }
    *entry = (void (*)(void *))(base + eh.e_entry);
    g_base = (uintptr_t)base;
    return 0;
}

int main(void) {
    ocl_tls_reserve[0] = 0; /* mantém a reserva no PT_TLS */
    llog("início: %s", GAME_PATH);
    pthread_t ct;
    pthread_create(&ct, NULL, ctrl_thread, NULL);

    void (*entry)(void *) = NULL;
    if (load_game(&entry) != 0) {
        g_state = "erro-carga";
        llog("carga falhou; esperando reload");
        for (;;) {
            sceGnmSubmitDone();
            sceKernelUsleep(100000);
        }
    }
    g_state = "rodando";
    llog("pulando no _start do jogo em %p (base 0x%lx: endereço no ELF = endereço - base)", (void *)entry,
         (unsigned long)g_base);

    /* A faixa do TLS do jogo nesta thread volta a zero, como numa thread nova (o carregador usou o bloco). */
    uintptr_t tp;
    __asm__ volatile("movq %%fs:0, %0" : "=r"(tp));
    memset((void *)(tp - g_tls_span), 0, g_tls_span);

    /* Bloco que o _start do SDK espera: {argc, argv..., NULL, envp NULL, auxv AT_NULL}. O argv[0] é o caminho do ELF:
     * o runtime tira dele a pasta de dados (platform/orbis.c orbis_data_dir). */
    static uint64_t args[8];
    args[0] = 1;
    args[1] = (uint64_t)(uintptr_t)GAME_PATH;
    entry(args);
    for (;;) {
        sceGnmSubmitDone();
        sceKernelUsleep(100000);
    }
}
