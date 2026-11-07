// Mesh shader emulation, Vulkan part (docs/frente-M.md): the set 3 layout, the pull vertex shader, the compute
// pipeline that goes with each emulated graphics pipeline, and the device-local slabs the computes write.
//
// OCARINA_MESH_EMULATE=1 takes this path on a device that has VK_EXT_mesh_shader too (testing on NVIDIA); without the
// extension it is always taken. OCARINA_MESH_EMU_MB (default 8): size of a slab chunk; a draw that needs more gets a
// chunk of its own. OCARINA_MESH_EMU_CAP (default 256): slabs reserved for an indirect draw whose group count the
// CPU does not know; workgroups past it draw nothing. OCARINA_MESH_EMU_TASKS (default 256): task workgroups an indirect
// draw with a task stage can have; the ones past it emit nothing.
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "mesh_pull_spv.h"
#include "mesh_scan_spv.h"
#include "vk.h"
#include "vk_mesh_emu.h"
#include "platform/platform.h"

static PlatMutex lock = PLAT_MUTEX_INIT;
static VkDescriptorSetLayout set3;

int mesh_emu_on(void) { return vk_ctx()->mesh_emulate; }

VkDescriptorSetLayout mesh_emu_set_layout(void) {
    plat_mutex_lock(&lock);
    if (!set3) {
        VkDescriptorSetLayoutBinding b[3];
        for (uint32_t i = 0; i < 3; i++)
            b[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                                  VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3,
                                              .pBindings = b};
        if (vkCreateDescriptorSetLayout(vk_ctx()->device, &ci, NULL, &set3) != VK_SUCCESS) set3 = VK_NULL_HANDLE;
    }
    plat_mutex_unlock(&lock);
    return set3;
}

const uint32_t *mesh_emu_pull_vs(uint64_t *words) {
    *words = sizeof mesh_pull_spv / 4;
    return mesh_pull_spv;
}

// ---- graphics pipeline -> its compute pipeline (entered by vk_pipeline.c, possibly on a worker thread)

#define REG_BUCKETS 1024
static MeshEmuPipe *reg[REG_BUCKETS];

static uint32_t reg_hash(VkPipeline p) { return (uint32_t)(((uint64_t)(uintptr_t)p * 0x9e3779b97f4a7c15ull) >> 40) % REG_BUCKETS; }

const uint32_t *mesh_emu_scan_cs(uint64_t *words) {
    *words = sizeof mesh_scan_spv / 4;
    return mesh_scan_spv;
}

uint32_t mesh_emu_default_tasks(void) {
    static uint32_t n;
    if (!n) {
        const char *e = getenv("OCARINA_MESH_EMU_TASKS");
        n = e && atoi(e) > 0 ? (uint32_t)atoi(e) : 256;
    }
    return n;
}

void mesh_emu_register(VkPipeline gfx, VkPipeline cs, VkPipeline task, VkPipeline scan, const MeshEmuInfo *info) {
    MeshEmuPipe *e = calloc(1, sizeof *e);
    if (!e) return;
    e->gfx = gfx, e->cs = cs, e->task = task, e->scan = scan, e->info = *info;
    plat_mutex_lock(&lock);
    uint32_t h = reg_hash(gfx);
    e->next = reg[h];
    reg[h] = e;
    plat_mutex_unlock(&lock);
}

MeshEmuPipe *mesh_emu_find(VkPipeline gfx) {
    plat_mutex_lock(&lock);
    MeshEmuPipe *e = reg[reg_hash(gfx)];
    while (e && e->gfx != gfx) e = e->next;
    plat_mutex_unlock(&lock);
    return e;
}

// ---- slabs: device-local chunks, given back when the command buffer that used them completes

typedef struct { VkBuffer buf; VmaAllocation alloc; VkDeviceSize size, used; } Chunk;
typedef struct { Chunk *c; int n, cap; } Held;
enum { SPARE = 6 };
static Chunk spare[SPARE];
static int nspare;
static Held *held;                 // the chunks of the command buffer being recorded
static uint64_t held_fence;
static VkDeviceSize chunk_bytes;

static void held_done(void *arg) {   // completion thread
    Held *h = arg;
    for (int i = 0; i < h->n; i++) {
        plat_mutex_lock(&lock);
        int keep = h->c[i].size == chunk_bytes && nspare < SPARE;
        if (keep) spare[nspare++] = h->c[i];
        plat_mutex_unlock(&lock);
        if (!keep) vmaDestroyBuffer(vk_ctx()->vma, h->c[i].buf, h->c[i].alloc);
    }
    free(h->c), free(h);
}

uint32_t mesh_emu_default_cap(void) {
    static uint32_t cap;
    if (!cap) {
        const char *e = getenv("OCARINA_MESH_EMU_CAP");
        cap = e && atoi(e) > 0 ? (uint32_t)atoi(e) : 256;
    }
    return cap;
}

int mesh_emu_slab(uint64_t fence, VkDeviceSize bytes, VkBuffer *buf, VkDeviceSize *off) {
    if (!chunk_bytes) {
        const char *e = getenv("OCARINA_MESH_EMU_MB");
        chunk_bytes = (VkDeviceSize)(e && atoi(e) > 0 ? atoi(e) : 8) << 20;
    }
    if (!held || held_fence != fence) {
        held = calloc(1, sizeof *held);
        if (!held) return 0;
        held_fence = fence;
        vk_completion_add(fence, held_done, held);
    }
    bytes = (bytes + 255) & ~(VkDeviceSize)255;
    Chunk *c = held->n ? &held->c[held->n - 1] : NULL;
    if (!c || c->used + bytes > c->size) {
        if (held->n == held->cap) {
            int nc = held->cap ? held->cap * 2 : 4;
            Chunk *p = realloc(held->c, (size_t)nc * sizeof *p);
            if (!p) return 0;
            held->c = p, held->cap = nc;
        }
        c = &held->c[held->n];
        VkDeviceSize n = bytes > chunk_bytes ? bytes : chunk_bytes;
        int reused = 0;
        if (n == chunk_bytes) {
            plat_mutex_lock(&lock);
            if (nspare) *c = spare[--nspare], reused = 1;
            plat_mutex_unlock(&lock);
        }
        if (!reused) {
            VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n,
                                     .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT};
            VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
            if (vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &c->buf, &c->alloc, NULL) != VK_SUCCESS) {
                static int bad;
                if (!bad++) rt_log("vk: mesh emulation: a slab chunk of %llu bytes could not be allocated", (unsigned long long)n);
                return 0;
            }
            c->size = n;
            static int told;
            if (!told++) rt_log("vk: mesh emulation: slab chunks of %llu MiB", (unsigned long long)(chunk_bytes >> 20));
        }
        c->used = 0;
        held->n++;
    }
    *buf = c->buf, *off = c->used;
    c->used += bytes;
    return 1;
}
