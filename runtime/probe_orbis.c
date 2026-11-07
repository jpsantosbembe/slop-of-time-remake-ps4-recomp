// ocarina-ps4: --probe-orbis, o teste do runtime no console sem os arquivos do jogo. Nenhum dos --probe-* do main.c
// serve sozinho: --probe-window depende de foco, cursor e redimensionamento de janela, --probe-vkexec testa o
// executor de comandos NVN sem janela, e --probe-kernel e afins não sobem o Vulkan. Este sobe, na ordem do boot:
//   1. memória: o layout do guest que o mem_init já reservou (módulos, região do kernel, bloco de pilha e heap,
//      alias); commit do heap do guest (mem_set_heap_size), escrita e leitura; mem_kernel_alloc;
//   2. threads: N threads com a pilha padrão (memória direta, platform/orbis.c), cada uma tocando o fundo da pilha;
//   3. fibra: ida e volta entre a thread e uma fibra nova, com a pilha alinhada (um movaps na fibra);
//   4. tempo: plat_now_ns contra plat_sleep_ms, e a monotonicidade;
//   5. arquivo em /data/ocarina: grava, lê e apaga;
//   6. Vulkan e a saída de vídeo: gfx_window_start (instância com VK_EXT_headless_surface, device, swapchain) e
//      OCARINA_PROBE_FRAMES quadros (padrão 600, 10 s a 60 Hz) de uma cor por vez — vermelho, verde, azul, branco, um
//      segundo cada — pelo vk_present_image do jogo; o primeiro quadro é lido de volta e conferido.
// Cada etapa escreve uma linha em /data/ocarina/probe.txt (e no log); a última linha é "probe-orbis: OK" ou
// "probe-orbis: FALHOU (<etapas>)". Uma falha numa etapa não impede as seguintes, exceto as do Vulkan entre si.
// Para rodar: /data/ocarina/args.txt com a linha --probe-orbis.
#ifdef __ORBIS__
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "host.h"
#include "vk.h"
#include "platform/platform.h"

#define REPORT "/data/ocarina/probe.txt"
#define SRC_W 640
#define SRC_H 360

static FILE *rep;
static char failed[256];

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    rt_log("probe-orbis: %s", line);
    if (rep) fprintf(rep, "%s\n", line), fflush(rep);
}

static int check(int ok, const char *step, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int check(int ok, const char *step, const char *fmt, ...) {
    char msg[384];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    say("%-8s %s  %s", step, ok ? "ok    " : "FALHOU", msg);
    if (!ok && strlen(failed) + strlen(step) + 2 < sizeof failed) {
        if (failed[0]) strcat(failed, " ");
        strcat(failed, step);
    }
    return ok;
}

// ---- 1. memória
static void step_memory(void) {
    const RtGuestLayout *l = mem_layout();
    check(l != NULL, "memoria", "layout do guest: pilha %#llx+%#llx, heap %#llx+%#llx, alias %#llx+%#llx",
          l ? (unsigned long long)l->stack.base : 0, l ? (unsigned long long)l->stack.size : 0,
          l ? (unsigned long long)l->heap.base : 0, l ? (unsigned long long)l->heap.size : 0,
          l ? (unsigned long long)l->alias.base : 0, l ? (unsigned long long)l->alias.size : 0);
    uint64_t size = 64ull << 20, addr = 0;
    uint64_t t0 = plat_now_ns();
    uint32_t r = mem_set_heap_size(size, &addr);
    if (!check(r == 0 && addr, "memoria", "heap do guest de 64 MiB: resultado %#x, em %#llx (%.1f ms)", r,
               (unsigned long long)addr, (plat_now_ns() - t0) / 1e6))
        return;
    volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)addr;
    size_t n = size / 8;
    for (size_t i = 0; i < n; i += 512) p[i] = 0x9e3779b97f4a7c15ull * (i + 1);
    size_t bad = 0;
    for (size_t i = 0; i < n; i += 512) bad += p[i] != 0x9e3779b97f4a7c15ull * (i + 1);
    check(!bad, "memoria", "escrita e leitura em todo o heap (uma palavra a cada 4 KiB): %zu erradas", bad);
    uint64_t end = 0;
    int st = plat_mem_state(addr, addr + size, &end);
    check(st == PLAT_PROT_RW, "memoria", "plat_mem_state do heap: %d (RW = %d)", st, PLAT_PROT_RW);
    mem_set_heap_size(0, &addr);
    uint8_t *k = mem_kernel_alloc(1 << 20, 0x1000);
    int kz = k != NULL;
    for (int i = 0; kz && i < (1 << 20); i += 4096) kz = k[i] == 0;
    if (k) memset(k, 0xa5, 1 << 20);
    check(kz, "memoria", "mem_kernel_alloc de 1 MiB, zerado, em %p", (void *)k);
    if (k) mem_kernel_free(k, 1 << 20);
}

// ---- 2. threads
#define NTHREADS 8
static volatile int32_t thread_hits;

static void __attribute__((noinline)) deep(int depth, volatile char *sink) {
    volatile char buf[64 << 10];   // 64 KiB por quadro: 16 níveis passam de 1 MiB de pilha
    buf[0] = (char)depth;
    buf[sizeof buf - 1] = (char)depth;
    *sink += buf[0] + buf[sizeof buf - 1];
    if (depth > 0) deep(depth - 1, sink);
}

static void thread_fn(void *arg) {
    volatile char sink = 0;
    deep(32, &sink);   // ~2 MiB de pilha: além do padrão de 64 KiB da Sony e do piso de 2 MiB do overlay
    plat_inc32(&thread_hits);
    (void)arg;
}

static void step_threads(void) {
    PlatThread *t[NTHREADS];
    int made = 0;
    uint64_t t0 = plat_now_ns();
    for (int i = 0; i < NTHREADS; i++)
        if ((t[made] = plat_thread_new(thread_fn, NULL, "probe"))) made++;
    for (int i = 0; i < made; i++) plat_thread_join(t[i]);
    check(made == NTHREADS && thread_hits == NTHREADS, "threads",
          "%d de %d criadas com a pilha padrão, %d voltaram de ~2 MiB de recursão (%.1f ms)", made, NTHREADS,
          (int)thread_hits, (plat_now_ns() - t0) / 1e6);
}

// ---- 2b. threads em massa: quantas threads vivas ao mesmo tempo o processo aceita (o OoT caiu com EAGAIN no
// pthread_create com umas 17 pilhas vivas). Cada thread espera numa palavra até o fim do teste.
#define NMASS 256
static volatile uint32_t mass_go;
static volatile int32_t mass_alive;

static void mass_fn(void *arg) {
    (void)arg;
    plat_inc32(&mass_alive);
    while (!__atomic_load_n(&mass_go, __ATOMIC_ACQUIRE)) plat_sleep_ms(5);
}

extern unsigned long long orbis_internal_free(void);
static void step_threads_mass(void) {
    static PlatThread *t[NMASS];
    int made = 0;
    unsigned long long pool0 = orbis_internal_free();
    uint64_t t0 = plat_now_ns();
    for (int i = 0; i < NMASS; i++) {
        if (!(t[made] = plat_thread_new_ex(mass_fn, NULL, "massa", 256 << 10))) break;
        made++;
    }
    for (int i = 0; i < 200 && mass_alive < made; i++) plat_sleep_ms(5);
    int alive = mass_alive;
    unsigned long long pool1 = orbis_internal_free();
    say("threads  pool interno da libkernel: %llu KiB livres antes, %llu KiB com %d threads vivas (%.1f KiB por thread)",
        pool0 >> 10, pool1 >> 10, made, made ? (double)(pool0 - pool1) / 1024.0 / made : 0.0);
    __atomic_store_n(&mass_go, 1, __ATOMIC_RELEASE);
    for (int i = 0; i < made; i++) plat_thread_join(t[i]);
    check(made == NMASS, "threads", "em massa: %d de %d criadas ao mesmo tempo (%d chegaram a rodar), pilha de 256 KiB "
          "(%.1f ms); a falha, se houve, está no totk.log (\"não nasceu\")", made, NMASS, alive, (plat_now_ns() - t0) / 1e6);
}

// ---- 3. fibra
static PlatFiber *fib_main, *fib_new;
static volatile int fib_steps;

static void fiber_fn(void *arg) {
    (void)arg;
    // Uma conta em SSE com dados na pilha: na pilha desalinhada o compilador usaria movaps e cairia
    double v[4] __attribute__((aligned(16))) = {1, 2, 3, 4};
    volatile double s = v[0] * v[1] + v[2] * v[3];
    fib_steps += s == 14.0 ? 1 : 100;
    plat_fiber_switch(fib_main);
    fib_steps += 1;
    plat_fiber_switch(fib_main);
    for (;;) plat_fiber_switch(fib_main);   // uma fibra nunca volta
}

static void step_fiber(void) {
    fib_main = plat_fiber_self();
    fib_new = plat_fiber_new(256 << 10, fiber_fn, NULL);
    if (!check(fib_new != NULL, "fibra", "plat_fiber_new de 256 KiB")) return;
    plat_fiber_switch(fib_new);
    plat_fiber_switch(fib_new);
    check(fib_steps == 2, "fibra", "duas idas e voltas, conta SSE na pilha da fibra: passos %d (esperado 2)", fib_steps);
    plat_fiber_unself();
    plat_fiber_free(fib_new);
}

// ---- 4. tempo
static void step_time(void) {
    uint64_t a = plat_now_ns(), prev = a, back = 0;
    for (int i = 0; i < 100000; i++) {
        uint64_t t = plat_now_ns();
        back += t < prev;
        prev = t;
    }
    uint64_t b = plat_now_ns();
    plat_sleep_ms(100);
    uint64_t c = plat_now_ns();
    double ms = (c - b) / 1e6;
    check(!back && ms >= 95 && ms < 200, "tempo", "plat_sleep_ms(100) mediu %.2f ms; 100000 leituras em %.2f ms, %llu para trás",
          ms, (b - a) / 1e6, (unsigned long long)back);
}

// ---- 5. arquivo
static void step_file(void) {
    const char *path = "/data/ocarina/probe-file.tmp";
    static uint8_t data[1 << 20];
    for (size_t i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i * 131 + 7);
    int w = plat_file_write(path, data, sizeof data);
    uint64_t size = 0;
    uint8_t *back = plat_file_read(path, &size);
    int same = back && size == sizeof data && !memcmp(back, data, sizeof data);
    free(back);
    plat_file_delete(path);
    check(w && same && !plat_file_exists(path), "arquivo", "1 MiB gravado, lido igual e apagado em /data/ocarina");
}

// ---- 6. Vulkan e a saída de vídeo
typedef struct { VkImage img; VmaAllocation alloc; VkCommandPool pool; VkCommandBuffer cb; VkFence fence; } Source;

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                               .srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                               .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                               .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}

static int source_create(Source *s) {
    VkCtx *c = vk_ctx();
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {SRC_W, SRC_H, 1}, .mipLevels = 1,
                            .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ii, &ai, &s->img, &s->alloc, NULL) != VK_SUCCESS) return 0;
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = c->gfx_family};
    if (vkCreateCommandPool(c->device, &pi, NULL, &s->pool) != VK_SUCCESS) return 0;
    VkCommandBufferAllocateInfo ba = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = s->pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    if (vkAllocateCommandBuffers(c->device, &ba, &s->cb) != VK_SUCCESS) return 0;
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    return vkCreateFence(c->device, &fi, NULL, &s->fence) == VK_SUCCESS;
}

// Limpa a origem com rgba e a deixa em TRANSFER_SRC_OPTIMAL, esperando a GPU.
static int source_clear(Source *s, int first, const float rgba[4]) {
    VkCtx *c = vk_ctx();
    vkResetCommandBuffer(s->cb, 0);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(s->cb, &bi);
    barrier(s->cb, s->img, first ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue cv;
    memcpy(cv.float32, rgba, sizeof cv.float32);
    VkImageSubresourceRange rng = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(s->cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &rng);
    barrier(s->cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vkEndCommandBuffer(s->cb);
    VkCommandBufferSubmitInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->cb};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &cbi};
    vk_queue_lock();
    VkResult r = vkQueueSubmit2(c->gfx_queue, 1, &si, s->fence);
    vk_queue_unlock();
    if (r != VK_SUCCESS) return 0;
    r = vkWaitForFences(c->device, 1, &s->fence, VK_TRUE, 5000000000ull);
    vkResetFences(c->device, 1, &s->fence);
    return r == VK_SUCCESS;
}

static void step_vulkan(void) {
    uint64_t t0 = plat_now_ns();
    gfx_window_start();   // rt_fatal se não houver device ou superfície: o log diz por quê
    VkCtx *c = vk_ctx();
    if (!check(c != NULL, "vulkan", "device e swapchain prontos em %.0f ms", (plat_now_ns() - t0) / 1e6)) return;
    VkPhysicalDeviceProperties pp;
    vkGetPhysicalDeviceProperties(c->phys, &pp);
    say("vulkan   GPU %s, Vulkan %u.%u.%u", pp.deviceName, VK_API_VERSION_MAJOR(pp.apiVersion),
        VK_API_VERSION_MINOR(pp.apiVersion), VK_API_VERSION_PATCH(pp.apiVersion));
    Source s = {0};
    if (!check(source_create(&s), "vulkan", "imagem de origem %dx%d", SRC_W, SRC_H)) return;

    char e[16];
    int frames = plat_getenv("OCARINA_PROBE_FRAMES", e, sizeof e) ? atoi(e) : 600;
    if (frames < 1) frames = 1;
    static const float colors[4][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 1, 1}};
    vk_present_capture(1);
    int bad_clear = 0, readback = -1;
    uint64_t f0 = plat_now_ns(), worst = 0, prev = f0;
    for (int i = 0; i < frames; i++) {
        const float *rgba = colors[(i / 60) % 4];
        if (!source_clear(&s, i == 0, rgba)) bad_clear++;
        vk_present_image(s.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_FORMAT_R8G8B8A8_UNORM, SRC_W, SRC_H, NULL);
        if (i == 0) {   // o centro do primeiro quadro, lido de volta do swapchain: vermelho puro
            const uint8_t *px;
            int w, h;
            VkFormat fmt;
            if (vk_present_captured(&px, &w, &h, &fmt)) {
                const uint8_t *m = px + ((size_t)(h / 2) * w + w / 2) * 4;
                int red = fmt == VK_FORMAT_B8G8R8A8_UNORM ? m[2] : m[0], blue = fmt == VK_FORMAT_B8G8R8A8_UNORM ? m[0] : m[2];
                readback = red == 255 && m[1] == 0 && blue == 0;
                say("vulkan   swapchain %dx%d formato %d; centro do 1o quadro %02x %02x %02x", w, h, (int)fmt, m[0], m[1], m[2]);
            }
            vk_present_capture(0);
        }
        uint64_t now = plat_now_ns();
        if (now - prev > worst) worst = now - prev;
        prev = now;
    }
    double secs = (plat_now_ns() - f0) / 1e9;
    check(!bad_clear, "vulkan", "%d quadros em %.2f s (%.1f por segundo, o pior %.1f ms), %d limpezas falharam", frames,
          secs, frames / secs, worst / 1e6, bad_clear);
    check(readback == 1, "vulkan", "leitura de volta do 1o quadro: %s",
          readback < 0 ? "não capturado" : readback ? "vermelho" : "cor errada");
    say("vulkan   na tela: vermelho, verde, azul e branco, um segundo cada (confira a olho)");
    vkDeviceWaitIdle(c->device);
}

int probe_orbis(void) {
    plat_file_mkdir_p("/data/ocarina");
    rep = fopen(REPORT, "w");
    say("probe-orbis: o runtime no PS4 sem o jogo (%s)", rt_port_version);
    step_memory();
    step_threads();
    step_threads_mass();
    step_fiber();
    step_time();
    step_file();
    step_vulkan();
    if (failed[0]) say("probe-orbis: FALHOU (%s)", failed);
    else say("probe-orbis: OK");
    if (rep) fclose(rep);
    return failed[0] ? 1 : 0;
}
#endif
