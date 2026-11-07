// GPU breadcrumbs for a lost device (vk.h vk_crumb). VK_AMD_buffer_marker writes a 32-bit code into a buffer from
// the command stream: at TOP_OF_PIPE once the command processor reaches the marker, at BOTTOM_OF_PIPE once every
// command before it has finished. Each crumb writes both, into two words of one host-coherent buffer, so after a
// device loss word 0 names the newest point the GPU reached and word 1 the newest point it had finished: the hang lies
// between them. A code is the crumb kind in the top 4 bits and its number in the low 28. The markers cost two small
// writes per crumb, and crumbs mark submissions, upload batches, ASTC chunks and presents, not draws.
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>

#include "host.h"
#include "vk.h"

static PFN_vkCmdWriteBufferMarkerAMD write_marker;
static VkBuffer buf;
static VmaAllocation alloc;
static volatile uint32_t *words;

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
    int same = 0, said = 0;
    for (;;) {
        usleep(200000);
        uint32_t w0 = words[0], w1 = words[1];
        if (w0 == last0 && w1 == last1 && w0 != w1) same++;
        else same = 0, said = 0;
        last0 = w0, last1 = w1;
        if (same >= 3 && !said) {
            said = 1;
            char reached[96], finished[96];
            describe(w0, reached, sizeof reached);
            describe(w1, finished, sizeof finished);
            rt_log("vk: GPU PARADA há 0,6 s: alcançou %s, terminou %s (a GPU travou entre os dois)", reached, finished);
            extern void plat_log_line(const char *line, size_t n);
            char m[240];
            int k = snprintf(m, sizeof m, "FATAL: GPU travou entre %s e %s", finished, reached);
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
    write_marker(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, buf, 0, code);
    write_marker(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, buf, 4, code);
}

static void describe(uint32_t code, char *o, size_t size) {
    static const char *const kinds[] = {"nothing", "the start of a part of submission", "the end of submission",
                                        "the start of upload batch", "the end of upload batch",
                                        "ASTC decode chunk", "the start of present",
                                        "the end of present"};
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
