// Mesh shader emulation for devices without VK_EXT_mesh_shader (docs/frente-M.md).
#pragma once
#include <stdint.h>

#define MESH_EMU_MAX_V 256
#define MESH_EMU_MAX_P 256
#define MESH_EMU_MAX_PAYLOAD 4096   // words
#define MESH_EMU_LOCS 28            // Locations 0..27: with Position and the 7 built-in components, 119 of 128
#define MESH_EMU_NO_LOC 0xffffffffu

// Set 3 of the emulation's pipelines: three dynamic storage buffers.
enum { MESH_EMU_BIND_VTX = 0, MESH_EMU_BIND_IDX = 1, MESH_EMU_BIND_CTL = 2 };

// Control block (binding 2), in words.
enum {
    MESH_EMU_CTL_DRAW = 0,       // VkDrawIndexedIndirectCommand
    MESH_EMU_CTL_DISPATCH = 5,   // VkDispatchIndirectCommand
    MESH_EMU_CTL_CAP = 8,        // slabs the vertex and index buffers hold
    MESH_EMU_CTL_HAS_TASK = 9,
    MESH_EMU_CTL_TASKS = 10,     // task workgroups
    MESH_EMU_CTL_MAX_MESHES = 11,
    MESH_EMU_CTL_PAYLOAD = 12,   // payload words per task workgroup
    MESH_EMU_CTL_TMAX = 13,      // capacity of count[] and offset[]
    MESH_EMU_CTL_GROUPS = 14,    // written by workgroup 0: the groups dispatched (more than CAP draw nothing)
    MESH_EMU_CTL_SLAB_INDICES = 15,   // indices per slab (3 * prims), for the scan
    MESH_EMU_CTL_STRIDE = 16,    // floats per vertex (the pull vertex shader)
    MESH_EMU_CTL_LOC = 17,       // per Location: float offset in the vertex, or MESH_EMU_NO_LOC
    MESH_EMU_CTL_ARRAYS = 64,    // count[tmax], offset[tmax], payload[tmax * payload]; without a task, a zeroed
                                 // payload region
};

enum { MESH_EMU_MESH = 1, MESH_EMU_TASK = 2 };

typedef struct {
    int kind;
    uint32_t threads, max_v, max_p;
    uint32_t prims;                     // primitives per slab: max_p rounded up to threads
    uint32_t stride;                    // floats per vertex: Position, then the Locations in order
    uint32_t payload_words;
    uint32_t loc_off[MESH_EMU_LOCS];    // float offset of Location i in a vertex, or MESH_EMU_NO_LOC
} MeshEmuInfo;

// A GLCompute module from a MeshEXT one the translator wrote; malloc'd, NULL (with *why) for a shape it does not know.
uint32_t *mesh_emu_spirv(const uint32_t *w, uint64_t n, uint64_t *out_n, MeshEmuInfo *info, const char **why);

#ifdef VK_VERSION_1_0
// Vulkan side (vk_mesh_emu.c). VkPipeInfo comes from vk.h.
typedef struct MeshEmuPipe {
    struct MeshEmuPipe *next;
    VkPipeline gfx, cs;                 // the pull-vertex graphics pipeline and the mesh compute
    VkPipeline task, scan;              // with a task stage: its compute and the scan (shaders/mesh_scan.comp)
    MeshEmuInfo info;
    VkPipeInfo cinfo;                   // the graphics info with the compute bind point (vk_exec_draw.c fills it)
    int cinfo_ok;
    uint64_t told_prog;                 // the last program logged as overflowing
} MeshEmuPipe;

int mesh_emu_on(void);                                   // the device takes the emulated path
VkDescriptorSetLayout mesh_emu_set_layout(void);         // set 3: three storage buffers, compute and vertex
const uint32_t *mesh_emu_pull_vs(uint64_t *words);
void mesh_emu_register(VkPipeline gfx, VkPipeline cs, VkPipeline task, VkPipeline scan, const MeshEmuInfo *info);
const uint32_t *mesh_emu_scan_cs(uint64_t *words);
uint32_t mesh_emu_default_tasks(void);                   // task workgroups of an indirect draw (OCARINA_MESH_EMU_TASKS)
MeshEmuPipe *mesh_emu_find(VkPipeline gfx);
uint32_t mesh_emu_default_cap(void);                     // slabs of an indirect draw (OCARINA_MESH_EMU_CAP)
// Device-local bytes for the command buffer of fence, freed when it completes.
int mesh_emu_slab(uint64_t fence, VkDeviceSize bytes, VkBuffer *buf, VkDeviceSize *off);
#endif
