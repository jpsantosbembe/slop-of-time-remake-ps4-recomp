// Mesh shader emulation, SPIR-V part: turns the MeshEXT modules the translator writes (tools/xlate patch 0019,
// GenerateMeshEntry) into GLCompute modules that write their vertices and triangles into storage buffers, for devices
// without VK_EXT_mesh_shader (the PS4's GCN). Design: docs/frente-M.md. Pure C, no Vulkan: also built by the test
// (MESH_EMU_TEST) that converts the extracted modules for spirv-val.
//
// The program body already runs as a compute workgroup (the stage buffer is a Workgroup array); only the generated
// entry function touches mesh state. The rewrite, in that function only:
// - every OpAccessChain into an Output vertex array (Position, Location n), [vertex][component], addresses
//   vtx[base + vertex * stride + attribute * 4 + component] in set 3 binding 0 (float); attribute 0 is Position, then
//   the Locations in increasing order;
// - the stores of PrimitiveTriangleIndicesEXT become three stores of global vertex indices (slot * maxV + local) into
//   idx[slot * P3 + primitive * 3 + k], set 3 binding 1 (uint), P3 = 3 * roundup(maxP, threads);
// - OpSetMeshOutputsEXT becomes degenerate {0,0,0} stores for every primitive of the slab, by the invocation that would
//   write it (so no race with the real stores that follow);
// - TaskPayloadWorkgroupEXT loads read ctl[pbase + i], set 3 binding 2 (uint): the task's payload, or a zeroed region;
// - a prologue reads the control block, picks the slot and leaves early (uniform in the workgroup) when the slot is
//   past the capacity or, with a task, when WorkgroupId.x >= count[WorkgroupId.y]. Without a task, workgroup 0 writes
//   the indexed draw's index count.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "vk_mesh_emu.h"

enum {
    OP_NAME = 5, OP_MEMBER_NAME = 6, OP_EXTENSION = 10, OP_EXT_INST_IMPORT = 11, OP_EXT_INST = 12, OP_MEMORY_MODEL = 14,
    OP_ENTRY_POINT = 15, OP_EXECUTION_MODE = 16, OP_CAPABILITY = 17, OP_TYPE_VOID = 19, OP_TYPE_BOOL = 20,
    OP_TYPE_INT = 21, OP_TYPE_FLOAT = 22, OP_TYPE_VECTOR = 23, OP_TYPE_ARRAY = 28, OP_TYPE_RUNTIME_ARRAY = 29,
    OP_TYPE_STRUCT = 30, OP_TYPE_POINTER = 32, OP_CONSTANT = 43, OP_FUNCTION = 54, OP_FUNCTION_END = 56,
    OP_VARIABLE = 59, OP_LOAD = 61, OP_STORE = 62, OP_ACCESS_CHAIN = 65, OP_IN_BOUNDS_ACCESS_CHAIN = 66,
    OP_DECORATE = 71, OP_MEMBER_DECORATE = 72, OP_COMPOSITE_EXTRACT = 81, OP_IADD = 128, OP_IMUL = 132,
    OP_LOGICAL_OR = 166, OP_LOGICAL_AND = 167, OP_LOGICAL_NOT = 168, OP_SELECT = 169, OP_IEQUAL = 170,
    OP_INOT_EQUAL = 171, OP_UGREATER_THAN_EQUAL = 174, OP_PHI = 245, OP_SELECTION_MERGE = 247, OP_LABEL = 248,
    OP_BRANCH = 249, OP_BRANCH_CONDITIONAL = 250, OP_RETURN = 253, OP_ATOMIC_UMAX = 239,
    OP_EMIT_MESH_TASKS = 5294, OP_SET_MESH_OUTPUTS = 5295,
};
enum { SC_INPUT = 1, SC_OUTPUT = 3, SC_STORAGE_BUFFER = 12, SC_PAYLOAD = 5402 };
enum { DEC_BLOCK = 2, DEC_ARRAY_STRIDE = 6, DEC_BUILTIN = 11, DEC_LOCATION = 30, DEC_BINDING = 33,
       DEC_DESCRIPTOR_SET = 34, DEC_OFFSET = 35 };
enum { BI_POSITION = 0, BI_NUM_WORKGROUPS = 24, BI_WORKGROUP_ID = 26, BI_LOCAL_INVOCATION_ID = 27,
       BI_PRIMITIVE_TRIANGLE_INDICES = 5296 };
enum { EM_MESH = 5365, EM_TASK = 5364, EM_GLCOMPUTE = 5 };
enum { MODE_LOCAL_SIZE = 17, MODE_OUTPUT_VERTICES = 26, MODE_OUTPUT_PRIMITIVES = 5270, MODE_OUTPUT_TRIANGLES = 5298 };
enum { CAP_MESH = 5283 };

typedef struct { uint32_t *w; size_t n, cap; int oom; } Buf;

static void put(Buf *b, uint32_t v) {
    if (b->n == b->cap) {
        size_t c = b->cap ? b->cap * 2 : 1024;
        uint32_t *p = realloc(b->w, c * 4);
        if (!p) { b->oom = 1; return; }
        b->w = p, b->cap = c;
    }
    b->w[b->n++] = v;
}

static void ins(Buf *b, uint32_t op, int n, const uint32_t *a) {   // an instruction with n operand words
    put(b, (uint32_t)(n + 1) << 16 | op);
    for (int i = 0; i < n; i++) put(b, a[i]);
}
#define INS(b, op, ...) do { const uint32_t a_[] = {__VA_ARGS__}; ins(b, op, (int)(sizeof a_ / 4), a_); } while (0)

typedef struct {
    const uint32_t *w;
    uint64_t n;
    uint32_t bound;
    // types and ids found in the module
    uint32_t t_uint, t_float, t_bool, t_v3uint, glsl;
    uint32_t entry, kind;                  // entry function id, EM_MESH or EM_TASK
    uint32_t v_wgid, v_lid, v_nwg;         // builtin variables (0: none yet)
    uint32_t v_pos, v_idx, v_payload;      // removed variables
    uint32_t v_gen[32];                    // Output variable of Location i
    int attr_of_loc[32];
    uint32_t threads, max_v, max_p, payload_words;
    uint32_t pt_out_payload_arr;           // pointer type of the payload variable
    // new globals
    Buf g, dec;
    uint32_t consts[64][2];
    int nconsts;
    uint32_t p_in_v3uint, p_sb_f, p_sb_u, v_vtx, v_ix, v_ctl;
} Rw;

static uint32_t new_id(Rw *r) { return r->bound++; }

static uint32_t cst(Rw *r, uint32_t v) {
    for (int i = 0; i < r->nconsts; i++) if (r->consts[i][0] == v) return r->consts[i][1];
    uint32_t id = new_id(r);
    INS(&r->g, OP_CONSTANT, r->t_uint, id, v);
    if (r->nconsts < 64) r->consts[r->nconsts][0] = v, r->consts[r->nconsts++][1] = id;
    return id;
}

static uint32_t builtin_var(Rw *r, uint32_t builtin) {
    if (!r->p_in_v3uint) { r->p_in_v3uint = new_id(r); INS(&r->g, OP_TYPE_POINTER, r->p_in_v3uint, SC_INPUT, r->t_v3uint); }
    uint32_t id = new_id(r);
    INS(&r->g, OP_VARIABLE, r->p_in_v3uint, id, SC_INPUT);
    INS(&r->dec, OP_DECORATE, id, DEC_BUILTIN, builtin);
    return id;
}

// A runtime array of elem in a Block struct, a StorageBuffer variable at set 3, binding.
static uint32_t ssbo(Rw *r, uint32_t elem, uint32_t binding) {
    uint32_t arr = new_id(r), st = new_id(r), ptr = new_id(r), var = new_id(r);
    INS(&r->g, OP_TYPE_RUNTIME_ARRAY, arr, elem);
    INS(&r->g, OP_TYPE_STRUCT, st, arr);
    INS(&r->g, OP_TYPE_POINTER, ptr, SC_STORAGE_BUFFER, st);
    INS(&r->g, OP_VARIABLE, ptr, var, SC_STORAGE_BUFFER);
    INS(&r->dec, OP_DECORATE, arr, DEC_ARRAY_STRIDE, 4);
    INS(&r->dec, OP_DECORATE, st, DEC_BLOCK);
    INS(&r->dec, OP_MEMBER_DECORATE, st, 0, DEC_OFFSET, 0);
    INS(&r->dec, OP_DECORATE, var, DEC_DESCRIPTOR_SET, 3);
    INS(&r->dec, OP_DECORATE, var, DEC_BINDING, binding);
    return var;
}

// Instructions before the types: capabilities, extensions, imports, the memory model, entry points, execution modes,
// debug instructions and annotations.
static int preamble(uint32_t op) {
    return (op >= 1 && op <= 17 && op != 1) || op == 71 || op == 72 || op == 73 || op == 74 || op == 330 || op == 331 ||
           op == 332 || op == 5632 || op == 5633;
}

static int removed(const Rw *r, uint32_t id) {
    if (!id) return 0;
    if (id == r->v_pos || id == r->v_idx || id == r->v_payload) return 1;
    for (int i = 0; i < 32; i++) if (r->v_gen[i] == id) return 1;
    return 0;
}

// Arithmetic helpers emitting into the function body f.
static uint32_t bin(Rw *r, Buf *f, uint32_t op, uint32_t type, uint32_t a, uint32_t b) {
    uint32_t id = new_id(r);
    INS(f, op, type, id, a, b);
    return id;
}
static uint32_t ctl_ptr(Rw *r, Buf *f, uint32_t index) {
    uint32_t id = new_id(r);
    INS(f, OP_ACCESS_CHAIN, r->p_sb_u, id, r->v_ctl, cst(r, 0), index);
    return id;
}
static uint32_t ctl_load(Rw *r, Buf *f, uint32_t index) {
    uint32_t p = ctl_ptr(r, f, index), id = new_id(r);
    INS(f, OP_LOAD, r->t_uint, id, p);
    return id;
}
static uint32_t extract(Rw *r, Buf *f, uint32_t type, uint32_t v, uint32_t k) {
    uint32_t id = new_id(r);
    INS(f, OP_COMPOSITE_EXTRACT, type, id, v, k);
    return id;
}
static uint32_t load(Rw *r, Buf *f, uint32_t type, uint32_t ptr) {
    uint32_t id = new_id(r);
    INS(f, OP_LOAD, type, id, ptr);
    return id;
}
// if (cond) { body } as a structured selection; returns the label of the then block's start via *then_label.
static void if_begin(Rw *r, Buf *f, uint32_t cond, uint32_t *merge) {
    uint32_t then = new_id(r);
    *merge = new_id(r);
    INS(f, OP_SELECTION_MERGE, *merge, 0);
    INS(f, OP_BRANCH_CONDITIONAL, cond, then, *merge);
    INS(f, OP_LABEL, then);
}
static void if_end(Buf *f, uint32_t merge) {
    INS(f, OP_BRANCH, merge);
    INS(f, OP_LABEL, merge);
}

typedef struct { uint32_t ac, prim; } IdxChain;

// Rewrites the entry function [i0, i1) of a mesh module into f. Returns 0 on a shape it does not know.
static int rewrite_mesh_entry(Rw *r, uint64_t i0, uint64_t i1, Buf *f, const char **why) {
    const uint32_t *w = r->w;
    uint32_t stride = 4 * (1 + (uint32_t)r->attr_of_loc[31]);   // attr_of_loc[31] holds the Location count
    uint32_t p3 = 3 * ((r->max_p + r->threads - 1) / r->threads * r->threads);
    uint32_t c0 = cst(r, 0), first_label = 0, last_label = 0;
    uint32_t inv = 0, slot = 0, vslot = 0, vbase = 0, ibase = 0, pbase = 0;
    IdxChain chains[16];
    int nchains = 0, prologue_done = 0;
    for (uint64_t i = i0; i < i1;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        const uint32_t *a = &w[i + 1];
        if (op == OP_LABEL && !first_label) {
            first_label = a[0];
            ins(f, op, (int)len - 1, a);
            i += len;
            continue;
        }
        if (first_label && !prologue_done && op != OP_VARIABLE) {
            prologue_done = 1;
            uint32_t wg = load(r, f, r->t_v3uint, r->v_wgid);
            uint32_t x = extract(r, f, r->t_uint, wg, 0), y = extract(r, f, r->t_uint, wg, 1);
            inv = extract(r, f, r->t_uint, load(r, f, r->t_v3uint, r->v_lid), 0);
            uint32_t cap = ctl_load(r, f, cst(r, MESH_EMU_CTL_CAP));
            uint32_t has_task = bin(r, f, OP_INOT_EQUAL, r->t_bool, ctl_load(r, f, cst(r, MESH_EMU_CTL_HAS_TASK)), c0);
            uint32_t tmax = ctl_load(r, f, cst(r, MESH_EMU_CTL_TMAX));
            uint32_t arrays = cst(r, MESH_EMU_CTL_ARRAYS);
            uint32_t count = ctl_load(r, f, bin(r, f, OP_IADD, r->t_uint, arrays, y));
            uint32_t off = ctl_load(r, f, bin(r, f, OP_IADD, r->t_uint, bin(r, f, OP_IADD, r->t_uint, arrays, tmax), y));
            uint32_t slot_task = bin(r, f, OP_IADD, r->t_uint, off, x);
            slot = new_id(r);
            INS(f, OP_SELECT, r->t_uint, slot, has_task, slot_task, x);
            uint32_t over = bin(r, f, OP_UGREATER_THAN_EQUAL, r->t_bool, slot, cap);
            uint32_t past = bin(r, f, OP_LOGICAL_AND, r->t_bool, has_task, bin(r, f, OP_UGREATER_THAN_EQUAL, r->t_bool, x, count));
            uint32_t dead = bin(r, f, OP_LOGICAL_OR, r->t_bool, over, past);
            uint32_t ret = new_id(r), cont = new_id(r);
            INS(f, OP_SELECTION_MERGE, cont, 0);
            INS(f, OP_BRANCH_CONDITIONAL, dead, ret, cont);
            INS(f, OP_LABEL, ret);
            INS(f, OP_RETURN);
            INS(f, OP_LABEL, cont);
            vslot = bin(r, f, OP_IMUL, r->t_uint, slot, cst(r, r->max_v));
            vbase = bin(r, f, OP_IMUL, r->t_uint, vslot, cst(r, stride));
            ibase = bin(r, f, OP_IMUL, r->t_uint, slot, cst(r, p3));
            uint32_t pw = cst(r, r->payload_words ? r->payload_words : 1);
            uint32_t ptask = bin(r, f, OP_IADD, r->t_uint, bin(r, f, OP_IADD, r->t_uint, arrays, bin(r, f, OP_IMUL, r->t_uint, cst(r, 2), tmax)),
                                 bin(r, f, OP_IMUL, r->t_uint, y, pw));
            pbase = new_id(r);
            INS(f, OP_SELECT, r->t_uint, pbase, has_task, ptask, arrays);
            // Without a task, workgroup 0 writes the draw's index count: min(groups, cap) slabs.
            uint32_t first = bin(r, f, OP_LOGICAL_AND, r->t_bool, bin(r, f, OP_IEQUAL, r->t_bool, x, c0), bin(r, f, OP_IEQUAL, r->t_bool, inv, c0));
            uint32_t not_task = new_id(r);
            INS(f, OP_LOGICAL_NOT, r->t_bool, not_task, has_task);
            uint32_t merge;
            if_begin(r, f, bin(r, f, OP_LOGICAL_AND, r->t_bool, first, not_task), &merge);
            uint32_t groups = extract(r, f, r->t_uint, load(r, f, r->t_v3uint, r->v_nwg), 0);
            uint32_t used = new_id(r);
            INS(f, OP_EXT_INST, r->t_uint, used, r->glsl, 38, groups, cap);   // UMin
            INS(f, OP_STORE, ctl_ptr(r, f, cst(r, MESH_EMU_CTL_DRAW)), bin(r, f, OP_IMUL, r->t_uint, used, cst(r, p3)));
            INS(f, OP_STORE, ctl_ptr(r, f, cst(r, MESH_EMU_CTL_GROUPS)), groups);   // for dumps: more than cap drew nothing
            if_end(f, merge);
            last_label = merge;
        }
        if ((op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN) && removed(r, a[2])) {
            uint32_t base = a[2], res = a[1];
            if (base == r->v_idx) {
                if (len != 5 || nchains == 16) return *why = "index access chain shape", 0;
                chains[nchains++] = (IdxChain){res, a[3]};
            } else if (base == r->v_payload) {
                if (len != 5) return *why = "payload access chain shape", 0;
                INS(f, OP_ACCESS_CHAIN, r->p_sb_u, res, r->v_ctl, c0, bin(r, f, OP_IADD, r->t_uint, a[3], pbase));
            } else {
                if (len != 6) return *why = "vertex output access chain shape", 0;
                int attr = 0;
                if (base != r->v_pos) for (int l = 0; l < 31; l++) if (r->v_gen[l] == base) attr = r->attr_of_loc[l];
                uint32_t e = bin(r, f, OP_IMUL, r->t_uint, a[3], cst(r, stride));
                e = bin(r, f, OP_IADD, r->t_uint, e, cst(r, (uint32_t)attr * 4));
                e = bin(r, f, OP_IADD, r->t_uint, e, a[4]);
                e = bin(r, f, OP_IADD, r->t_uint, e, vbase);
                INS(f, OP_ACCESS_CHAIN, r->p_sb_f, res, r->v_vtx, c0, e);
            }
            i += len;
            continue;
        }
        if (op == OP_STORE) {
            int k = -1;
            for (int c = 0; c < nchains; c++) if (chains[c].ac == a[0]) k = c;
            if (k >= 0) {
                uint32_t at = bin(r, f, OP_IADD, r->t_uint, bin(r, f, OP_IMUL, r->t_uint, chains[k].prim, cst(r, 3)), ibase);
                for (uint32_t c = 0; c < 3; c++) {
                    uint32_t v = bin(r, f, OP_IADD, r->t_uint, extract(r, f, r->t_uint, a[1], c), vslot);
                    uint32_t p = new_id(r);
                    INS(f, OP_ACCESS_CHAIN, r->p_sb_u, p, r->v_ix, c0, bin(r, f, OP_IADD, r->t_uint, at, cst(r, c)));
                    INS(f, OP_STORE, p, v);
                }
                i += len;
                continue;
            }
        }
        if (op == OP_SET_MESH_OUTPUTS) {   // degenerate triangles over the whole slab, by the invocation that owns each
            for (uint32_t p = 0; p < p3 / 3; p += r->threads) {
                uint32_t prim = bin(r, f, OP_IADD, r->t_uint, inv, cst(r, p));
                uint32_t at = bin(r, f, OP_IADD, r->t_uint, bin(r, f, OP_IMUL, r->t_uint, prim, cst(r, 3)), ibase);
                for (uint32_t c = 0; c < 3; c++) {
                    uint32_t q = new_id(r);
                    INS(f, OP_ACCESS_CHAIN, r->p_sb_u, q, r->v_ix, c0, bin(r, f, OP_IADD, r->t_uint, at, cst(r, c)));
                    INS(f, OP_STORE, q, c0);
                }
            }
            i += len;
            continue;
        }
        if (op == OP_PHI) {   // a predecessor that was the first block is now the prologue's last
            put(f, w[i]);
            for (uint32_t k = 1; k < len; k++) put(f, (k >= 4 && !(k & 1) && w[i + k] == first_label && last_label) ? last_label : w[i + k]);
            i += len;
            continue;
        }
        if (op == OP_EMIT_MESH_TASKS) return *why = "task instruction in a mesh module", 0;
        for (uint32_t k = 1; k < len; k++)
            if (removed(r, w[i + k]) && op != OP_FUNCTION) return *why = "other use of a mesh variable", 0;
        ins(f, op, (int)len - 1, a);
        i += len;
    }
    return prologue_done ? 1 : (*why = "no prologue", 0);
}

uint32_t *mesh_emu_spirv(const uint32_t *w, uint64_t n, uint64_t *out_n, MeshEmuInfo *info, const char **why) {
    static const char *dummy;
    if (!why) why = &dummy;
    *why = NULL;
    memset(info, 0, sizeof *info);
    if (n < 5 || w[0] != 0x07230203) return *why = "not SPIR-V", NULL;
    Rw r = {.w = w, .n = n, .bound = w[3]};
    for (int l = 0; l < 32; l++) r.attr_of_loc[l] = 0;
    uint64_t first_fn = 0, entry_at = 0;
    // Pass 1: what the module has.
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) return *why = "malformed", NULL;
        const uint32_t *a = &w[i + 1];
        switch (op) {
        case OP_ENTRY_POINT: if (!r.kind) r.kind = a[0], r.entry = a[1], entry_at = i; break;
        case OP_EXECUTION_MODE:
            if (a[0] == r.entry && a[1] == MODE_LOCAL_SIZE) r.threads = a[2];
            if (a[0] == r.entry && a[1] == MODE_OUTPUT_VERTICES) r.max_v = a[2];
            if (a[0] == r.entry && a[1] == MODE_OUTPUT_PRIMITIVES) r.max_p = a[2];
            break;
        case OP_EXT_INST_IMPORT: if (len >= 3 && !memcmp(&a[1], "GLSL.std.450", 12)) r.glsl = a[0]; break;
        case OP_TYPE_INT: if (a[1] == 32 && !a[2] && !r.t_uint) r.t_uint = a[0]; break;
        case OP_TYPE_FLOAT: if (a[1] == 32 && !r.t_float) r.t_float = a[0]; break;
        case OP_TYPE_BOOL: if (!r.t_bool) r.t_bool = a[0]; break;
        case OP_DECORATE:
            if (a[1] == DEC_BUILTIN && a[2] == BI_POSITION) r.v_pos = a[0];
            if (a[1] == DEC_BUILTIN && a[2] == BI_PRIMITIVE_TRIANGLE_INDICES) r.v_idx = a[0];
            if (a[1] == DEC_BUILTIN && a[2] == BI_WORKGROUP_ID) r.v_wgid = a[0];
            if (a[1] == DEC_BUILTIN && a[2] == BI_LOCAL_INVOCATION_ID) r.v_lid = a[0];
            if (a[1] == DEC_BUILTIN && a[2] == BI_NUM_WORKGROUPS) r.v_nwg = a[0];
            break;
        case OP_FUNCTION: if (!first_fn) first_fn = i; break;
        default: break;
        }
        i += len;
    }
    if (r.kind != EM_MESH) return *why = r.kind == EM_TASK ? "task modules are not emulated yet" : "not a mesh module", NULL;
    if (!r.t_uint || !r.t_float || !r.t_bool || !r.glsl || !first_fn) return *why = "missing base types", NULL;
    if (!r.threads || r.threads > 128 || !r.max_v || r.max_v > MESH_EMU_MAX_V || !r.max_p || r.max_p > MESH_EMU_MAX_P)
        return *why = "limits", NULL;
    // Types needed by id (vectors after their component type), Locations and the payload.
    for (uint64_t i = 5; i < first_fn; i += w[i] >> 16) {
        uint32_t op = w[i] & 0xffff;
        const uint32_t *a = &w[i + 1];
        if (op == OP_TYPE_VECTOR && a[1] == r.t_uint && a[2] == 3 && !r.t_v3uint) r.t_v3uint = a[0];
        if (op == OP_DECORATE && a[1] == DEC_LOCATION && a[2] < 31) r.v_gen[a[2]] = a[0];
        if (op == OP_VARIABLE && a[2] == SC_PAYLOAD) r.v_payload = a[1], r.pt_out_payload_arr = a[0];
    }
    if (!r.t_v3uint || !r.v_lid || !r.v_pos || !r.v_idx) return *why = "missing builtins", NULL;
    // Location decorations of variables that are not Output vertex arrays (none expected) are not ours.
    for (uint64_t i = 5; i < first_fn; i += w[i] >> 16) {
        uint32_t op = w[i] & 0xffff;
        const uint32_t *a = &w[i + 1];
        if (op == OP_VARIABLE && a[2] != SC_OUTPUT)
            for (int l = 0; l < 31; l++) if (r.v_gen[l] == a[1]) r.v_gen[l] = 0;
    }
    int nloc = 0;
    for (int l = 0; l < 31; l++) if (r.v_gen[l]) r.attr_of_loc[l] = ++nloc;
    r.attr_of_loc[31] = nloc;
    if (r.v_payload) {   // payload words: the array length constant of its pointee
        uint32_t arr = 0, len_id = 0;
        for (uint64_t i = 5; i < first_fn; i += w[i] >> 16)
            if ((w[i] & 0xffff) == OP_TYPE_POINTER && w[i + 1] == r.pt_out_payload_arr) arr = w[i + 3];
        for (uint64_t i = 5; i < first_fn; i += w[i] >> 16)
            if ((w[i] & 0xffff) == OP_TYPE_ARRAY && w[i + 1] == arr) len_id = w[i + 3];
        for (uint64_t i = 5; i < first_fn; i += w[i] >> 16)
            if ((w[i] & 0xffff) == OP_CONSTANT && w[i + 2] == len_id) r.payload_words = w[i + 3];
        if (!r.payload_words || r.payload_words > MESH_EMU_MAX_PAYLOAD) return *why = "payload size", NULL;
    }
    // New globals.
    r.p_sb_f = new_id(&r), r.p_sb_u = new_id(&r);
    INS(&r.g, OP_TYPE_POINTER, r.p_sb_f, SC_STORAGE_BUFFER, r.t_float);
    INS(&r.g, OP_TYPE_POINTER, r.p_sb_u, SC_STORAGE_BUFFER, r.t_uint);
    r.v_vtx = ssbo(&r, r.t_float, MESH_EMU_BIND_VTX);
    r.v_ix = ssbo(&r, r.t_uint, MESH_EMU_BIND_IDX);
    r.v_ctl = ssbo(&r, r.t_uint, MESH_EMU_BIND_CTL);
    if (!r.v_wgid) r.v_wgid = builtin_var(&r, BI_WORKGROUP_ID);
    if (!r.v_nwg) r.v_nwg = builtin_var(&r, BI_NUM_WORKGROUPS);
    // The entry function, rewritten (it may add constants to r.g, so before the module is put together).
    Buf fn = {0};
    uint64_t e0 = 0, e1 = 0;
    for (uint64_t i = first_fn; i < n; i += w[i] >> 16) {
        uint32_t op = w[i] & 0xffff;
        if (op == OP_FUNCTION && w[i + 2] == r.entry) e0 = i;
        if (op == OP_FUNCTION_END && e0 && !e1) e1 = i + 1;
    }
    if (!e0 || !e1) return *why = "no entry function", NULL;
    if (!rewrite_mesh_entry(&r, e0, e1, &fn, why)) goto fail;

    Buf o = {0};
    int dec_done = 0;
    for (int k = 0; k < 5; k++) put(&o, w[k]);
    for (uint64_t i = 5; i < first_fn;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        const uint32_t *a = &w[i + 1];
        int keep = 1;
        if (op == OP_CAPABILITY && a[0] == CAP_MESH) keep = 0;
        if (op == OP_EXTENSION && !memcmp(a, "SPV_EXT_mesh_shader", 19)) keep = 0;
        if (op == OP_EXECUTION_MODE && a[0] == r.entry && a[1] != MODE_LOCAL_SIZE) keep = 0;
        if ((op == OP_NAME || op == OP_MEMBER_NAME || op == OP_DECORATE || op == OP_MEMBER_DECORATE) && removed(&r, a[0])) keep = 0;
        if (op == OP_VARIABLE && removed(&r, a[1])) keep = 0;
        if (op == OP_TYPE_POINTER && a[1] == SC_PAYLOAD) keep = 0;   // needs the mesh capability; only removed chains used it
        if (op == OP_DECORATE && !keep) { i += len; continue; }
        if (i == entry_at) {   // GLCompute, without the removed variables, with the new ones
            uint32_t nm = 2;   // name words: the string with its NUL, padded
            while (!(((const char *)&a[nm])[0] == 0 || ((const char *)&a[nm])[1] == 0 || ((const char *)&a[nm])[2] == 0 ||
                     ((const char *)&a[nm])[3] == 0)) nm++;
            nm++;
            size_t at = o.n;
            put(&o, 0);
            put(&o, EM_GLCOMPUTE);
            for (uint32_t k = 1; k < nm; k++) put(&o, a[k]);
            for (uint32_t k = nm; k < len - 1; k++) if (!removed(&r, a[k]) && a[k] != r.v_wgid && a[k] != r.v_nwg) put(&o, a[k]);
            put(&o, r.v_vtx), put(&o, r.v_ix), put(&o, r.v_ctl), put(&o, r.v_wgid), put(&o, r.v_nwg);
            if (!o.oom) o.w[at] = (uint32_t)(o.n - at) << 16 | OP_ENTRY_POINT;
            i += len;
            continue;
        }
        if (!dec_done && !preamble(op)) {   // new decorations close the annotations, before the first type
            for (size_t k = 0; k < r.dec.n; k++) put(&o, r.dec.w[k]);
            dec_done = 1;
        }
        if (keep) ins(&o, op, (int)len - 1, a);
        i += len;
    }
    for (size_t k = 0; k < r.g.n; k++) put(&o, r.g.w[k]);
    for (uint64_t i = first_fn; i < e0; i++) put(&o, w[i]);
    for (size_t k = 0; k < fn.n; k++) put(&o, fn.w[k]);
    for (uint64_t i = e1; i < n; i++) put(&o, w[i]);
    if (o.oom || r.g.oom || r.dec.oom || fn.oom) { free(o.w); *why = "out of memory"; goto fail; }
    o.w[3] = r.bound;
    free(fn.w), free(r.g.w), free(r.dec.w);
    info->kind = MESH_EMU_MESH;
    info->threads = r.threads, info->max_v = r.max_v, info->max_p = r.max_p;
    info->prims = (r.max_p + r.threads - 1) / r.threads * r.threads;
    info->stride = 4 * (1 + (uint32_t)nloc);
    info->payload_words = r.payload_words;
    for (int l = 0; l < MESH_EMU_LOCS; l++) info->loc_off[l] = r.v_gen[l] ? 4 * (uint32_t)r.attr_of_loc[l] : MESH_EMU_NO_LOC;
    *out_n = o.n;
    return o.w;
fail:
    free(fn.w), free(r.g.w), free(r.dec.w);
    return NULL;
}

#ifdef MESH_EMU_TEST
#include <stdio.h>
int main(int argc, char **argv) {   // mesh_emu_test in.spv out.spv
    FILE *f = fopen(argv[1], "rb");
    if (!f || argc < 3) return 2;
    static uint32_t buf[1 << 22];
    size_t n = fread(buf, 4, sizeof buf / 4, f);
    fclose(f);
    MeshEmuInfo info;
    uint64_t on = 0;
    const char *why = NULL;
    uint32_t *o = mesh_emu_spirv(buf, n, &on, &info, &why);
    if (!o) { printf("%s: %s\n", argv[1], why ? why : "?"); return 1; }
    f = fopen(argv[2], "wb");
    fwrite(o, 4, on, f);
    fclose(f);
    printf("%s: v %u p %u slab %u stride %u payload %u\n", argv[1], info.max_v, info.max_p, info.prims, info.stride, info.payload_words);
    free(o);
    return 0;
}
#endif
