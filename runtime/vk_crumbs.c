// GPU breadcrumbs for a lost device (vk.h vk_crumb). VK_AMD_buffer_marker writes a 32-bit code into a buffer from
// the command stream: at TOP_OF_PIPE once the command processor reaches the marker, at BOTTOM_OF_PIPE once every
// command before it has finished. Each crumb writes both, into two words of one host-coherent buffer, so after a
// device loss word 0 names the newest point the GPU reached and word 1 the newest point it had finished: the hang lies
// between them. A code is the crumb kind in the top 4 bits and its number in the low 28. The markers cost two small
// writes per crumb, and crumbs mark submissions, upload batches, ASTC bands and presents; TOTK_CRUMBS=draw also puts one
// before every draw and dispatch (VK_CRUMB_DRAW, numbered across the run; TOTK_CRUMBS_LOG=1 logs each as recorded).
//
// A hang can leave reached == finished: when the command processor itself stops right after crumb N (a packet it
// cannot get past, or a wait for a shader that never ends), crumb N's two writes both land and nothing after it does.
// The watcher therefore also counts the crumbs recorded on the CPU: words that stay still while the CPU records new
// crumbs past them is a stalled GPU too, and the hang lies after the finished crumb.
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>

#include <string.h>

#include "cfg.h"
#include "host.h"
#include "vk.h"

static PFN_vkCmdWriteBufferMarkerAMD write_marker;
static VkBuffer buf;
static VmaAllocation alloc;
static volatile uint32_t *words;
static volatile uint64_t n_recorded;   // crumbs recorded into command buffers (the watcher's sign of new work)
static volatile uint32_t last_recorded;

int vk_crumbs_draws(void) {
    static volatile int v = -1;
    if (v < 0) {
        const char *e = cfg_get("TOTK_CRUMBS");
        v = e && !strcmp(e, "draw");
        if (v) rt_log("vk: GPU breadcrumbs before every draw and dispatch (TOTK_CRUMBS=draw)");
    }
    return v;
}

void vk_crumbs_init(VkCtx *c) {
    if (!c->has_buffer_marker) return;
    write_marker = (PFN_vkCmdWriteBufferMarkerAMD)vkGetDeviceProcAddr(c->device, "vkCmdWriteBufferMarkerAMD");
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 16,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                  .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    VmaAllocationInfo info;
    if (!write_marker || vmaCreateBuffer(c->vma, &bi, &ai, &buf, &alloc, &info) != VK_SUCCESS) {
        rt_log("vk: GPU breadcrumbs unavailable");
        write_marker = NULL;
        return;
    }
    words = info.pMappedData;
    words[0] = words[1] = 0;
    rt_log("vk: GPU breadcrumbs on (VK_AMD_buffer_marker)");
#ifdef __ORBIS__
    {
        // ocarina-ps4: o PS4 mata o app 1 a 2 s depois de a GPU travar (0xa0d0c007), sem device lost. Um vigia lê os
        // breadcrumbs a cada 200 ms: a GPU parada entre o início e o fim de um lote (alcançado != terminado) por 0,6 s
        // é uma GPU travada, e o relatório sai no log e na tela antes de o sistema matar o processo.
        extern void *vk_crumbs_watch(void *);
        pthread_t t;
        if (!pthread_create(&t, NULL, vk_crumbs_watch, NULL)) pthread_detach(t);
    }
#endif
}

#ifdef __ORBIS__
static void describe(uint32_t code, char *o, size_t size);
void *vk_crumbs_watch(void *arg) {
    (void)arg;
    uint32_t last0 = 0, last1 = 0;
    uint64_t rec0 = 0;   // n_recorded when the words last changed
    int same = 0, said = 0;
    for (;;) {
        usleep(200000);
        uint32_t w0 = words[0], w1 = words[1];
        uint64_t rec = n_recorded;
        if (w0 == last0 && w1 == last1) same++;
        else same = 0, said = 0, rec0 = rec;
        last0 = w0, last1 = w1;
        // still words: between two crumbs (w0 != w1), or after the last one while the CPU recorded more past it
        // An end crumb (submission, upload batch, present) both reached and finished is an idle GPU: the next submission
        // starts with its own crumb, so a hang there would have moved the words (a false alarm after end of present 1504)
        unsigned kind = w1 >> 28;
        int idle = w0 == w1 && (kind == VK_CRUMB_SUBMIT_END || kind == VK_CRUMB_UPLOAD_END || kind == VK_CRUMB_PRESENT_END);
        int ahead = rec >= rec0 + 2 && !idle;
        if (same >= 3 && !said && w0 && (w0 != w1 || ahead)) {
            said = 1;
            char reached[96], finished[96], next[96];
            describe(w0, reached, sizeof reached);
            describe(w1, finished, sizeof finished);
            describe(last_recorded, next, sizeof next);
            extern void plat_log_line(const char *line, size_t n);
            char m[320];
            int k;
            if (w0 != w1) {
                rt_log("vk: GPU PARADA há 0,6 s: alcançou %s, terminou %s (a GPU travou entre os dois)", reached, finished);
                k = snprintf(m, sizeof m, "FATAL: GPU travou entre %s e %s", finished, reached);
            } else {
                rt_log("vk: GPU PARADA há 0,6 s depois de %s (alcançado = terminado), com %llu crumbs gravados pela CPU "
                       "depois (o último: %s): o processador de comandos parou logo depois desse crumb",
                       finished, (unsigned long long)(rec - rec0), next);
                k = snprintf(m, sizeof m, "FATAL: GPU travou depois de %s (nada depois dele começou)", finished);
            }
            plat_log_line(m, k > 0 ? (size_t)k : 0);
        }
    }
    return NULL;
}
#endif

void vk_crumbs_release(void) {
    VkCtx *c = vk_ctx();
    if (buf && c) vmaDestroyBuffer(c->vma, buf, alloc);
    buf = VK_NULL_HANDLE, alloc = NULL, words = NULL, write_marker = NULL;
}

void vk_crumb(VkCommandBuffer cb, int kind, uint64_t n) {
    if (!write_marker || !cb) return;
    uint32_t code = (uint32_t)kind << 28 | (uint32_t)(n & 0x0fffffff);
    __atomic_add_fetch(&n_recorded, 1, __ATOMIC_RELAXED);
    last_recorded = code;
    write_marker(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, buf, 0, code);
    write_marker(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, buf, 4, code);
}

static void describe(uint32_t code, char *o, size_t size) {
    static const char *const kinds[] = {"nothing", "the start of a part of submission", "the end of submission",
                                        "the start of upload batch", "the end of upload batch",
                                        "ASTC decode band", "the start of present",
                                        "the end of present", "draw or dispatch"};
    unsigned k = code >> 28;
    if (!code) snprintf(o, size, "nothing yet");
    else if (k < sizeof kinds / sizeof *kinds) snprintf(o, size, "%s %u", kinds[k], code & 0x0fffffff);
    else snprintf(o, size, "unknown code %#x", code);
}

void vk_crumbs_report(void) {
    if (!words) return;
    char reached[96], finished[96];
    describe(words[0], reached, sizeof reached);
    describe(words[1], finished, sizeof finished);
    rt_log("vk: GPU breadcrumbs: the GPU last reached %s; everything before %s had finished", reached, finished);
}
