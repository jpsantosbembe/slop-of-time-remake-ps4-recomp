/* ocarina-ps4 / Fase 0B: bateria de desempenho, lado Vulkan.
 *
 * Mede, por quadro, o custo de CPU para gravar e enviar o trabalho e o tempo de GPU de cada passe, em cenas que imitam o
 * que o Ocarina of Time (Switch 2) faz, com números medidos no Mac (build _ootup, jogar.log): ~1.030 draws por quadro,
 * ~3.600 texturas no pool bindless, deferred com 4 alvos, pós-processamento em compute.
 *
 *   S1  tempestade de draws: N draws pequenos (512 triângulos), uma textura bindless cada, constantes por draw
 *   FULL  quadro completo: G-buffer (N draws de 2.048 triângulos, 3 texturas cada) + luz deferred (32 luzes)
 *         + névoa volumétrica + bloom de 5 níveis + TAA, tudo em compute depois da luz
 *
 * Compila para Linux (validação no IdeaPad) e para o PS4 (__ORBIS__, com o RADV do mesa-ps4). Não apresenta nada na tela:
 * renderiza fora da tela, porque o que se compara é a API e o driver, não a apresentação.
 * Saída: uma linha por rodada no log e results.csv (Linux: diretório atual; PS4: /data/ocarina-bench/). */
#ifdef __ORBIS__
#include <orbis_prefix.h>
#include <orbis/libkernel.h>
#include <sys/stat.h>
#endif
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#include "geo_vert.h"
#include "forward_frag.h"
#include "gbuf_frag.h"
#include "fullscreen_vert.h"
#include "light_frag.h"
#include "fog_comp.h"
#include "bloom_comp.h"
#include "taa_comp.h"

/* ------------------------------------------------------------------------------------------------ plataforma */
static FILE *g_csv;

static void say(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    strcat(buf, "\n");
#ifdef __ORBIS__
    sceKernelDebugOutText(0, buf);
#endif
    fputs(buf, stdout);
    fflush(stdout);
}

/* O PS4 suspende o app antes de fechá-lo, e a suspensão de um app que usa a GPU espera uma chamada a
 * sceGnmSubmitDone(). Sem ela: "submitDone() is not called", exceção 0xa0d0c00a
 * (CPU_FAULT_SUBMITDONE_TIMEOUT_IN_SUSPEND_ASYNC), coredump e a tela de erro, ao fechar e ao apagar o app.
 * O mesa-ps4 não chama (o wsi_orbis.c tem um stub), então o app chama: uma vez por quadro e no ocioso. */
#ifdef __ORBIS__
extern int32_t sceGnmSubmitDone(void);
static void frame_done(void) { sceGnmSubmitDone(); }
#else
static void frame_done(void) {}
#endif

static void idle_forever(void)
{
#ifdef __ORBIS__
    say("bench: parado; feche pelo menu do botão PS");
    for (;;) { frame_done(); usleep(16000); }
#else
    exit(1);
#endif
}

static void st_fail(void);

static void must(VkResult r, const char *what)
{
    if (r != VK_SUCCESS) {
        say("bench: FALHOU em %s -> VkResult %d", what, (int)r);
        st_fail();
        idle_forever();
    }
}

static double now_ms(void)
{
#ifdef __ORBIS__
    return (double)sceKernelGetProcessTime() / 1000.0;   /* µs; o CLOCK_MONOTONIC do SDK tem numeração trocada */
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
#endif
}

/* ------------------------------------------------------------------------------------------------ parâmetros */
enum { SCENE_S1 = 1, SCENE_FULL = 2 };
typedef struct { int scene, w, h, draws; const char *name; } Run;
static const Run RUNS[] = {
    { SCENE_S1,   1280,  720, 1000, "S1-1000" },
    { SCENE_S1,   1280,  720, 2500, "S1-2500" },
    { SCENE_S1,   1280,  720, 5000, "S1-5000" },
    { SCENE_FULL, 1280,  720, 1000, "FULL-720p" },
    { SCENE_FULL, 1600,  900, 1000, "FULL-900p" },
    { SCENE_FULL, 1920, 1080, 1000, "FULL-1080p" },
};
#define NRUNS   (int)(sizeof(RUNS) / sizeof(RUNS[0]))
#define MAXDRAW 5000
#define NTEX    3600          /* texturas distintas, como o pool do OoT */
#define TEXDIM  256           /* BC1 256x256 com mips: ~43 KB cada, ~155 MB no total */
#define NMESH   32
#define SLOTS   2             /* quadros em voo */
#define NQ      8             /* timestamps por quadro */
#define BLOOM_LEVELS 5

static double g_warm_s = 5.0, g_meas_s = 20.0;
static const char *g_variant = "vulkan";

/* ------------------------------------------------------------------------------------------------ Vulkan base */
static VkInstance       inst;
static VkPhysicalDevice phys;
static VkDevice         dev;
static VkQueue          queue;
static uint32_t         qfam;
static VkPhysicalDeviceMemoryProperties memprops;
static float            ts_period;
static VkCommandPool    cpool;

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want) return i;
    say("bench: nenhum tipo de memória com flags 0x%x (bits 0x%x)", want, bits);
    idle_forever();
    return 0;
}

typedef struct { VkBuffer buf; VkDeviceMemory mem; void *map; VkDeviceSize size; } Buf;

static Buf buf_new(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags flags)
{
    Buf b = { .size = size };
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage };
    must(vkCreateBuffer(dev, &bci, NULL, &b.buf), "vkCreateBuffer");
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, b.buf, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                 .memoryTypeIndex = mem_type(mr.memoryTypeBits, flags) };
    must(vkAllocateMemory(dev, &mai, NULL, &b.mem), "vkAllocateMemory(buffer)");
    must(vkBindBufferMemory(dev, b.buf, b.mem, 0), "vkBindBufferMemory");
    if (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) must(vkMapMemory(dev, b.mem, 0, size, 0, &b.map), "vkMapMemory");
    return b;
}

static void buf_free(Buf *b)
{
    if (b->map) vkUnmapMemory(dev, b->mem);
    vkDestroyBuffer(dev, b->buf, NULL);
    vkFreeMemory(dev, b->mem, NULL);
    memset(b, 0, sizeof(*b));
}

static VkCommandBuffer one_begin(void)
{
    VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cpool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb;
    must(vkAllocateCommandBuffers(dev, &ai, &cb), "vkAllocateCommandBuffers(one)");
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}

static void one_end(VkCommandBuffer cb)
{
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    must(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit(one)");
    must(vkQueueWaitIdle(queue), "vkQueueWaitIdle(one)");
    vkFreeCommandBuffers(dev, cpool, 1, &cb);
}

static void barrier_all(VkCommandBuffer cb)
{
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                           .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                           .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

static void image_layout(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, uint32_t levels,
                         VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                               .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                               .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                               .oldLayout = from, .newLayout = to,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = img, .subresourceRange = { aspect, 0, levels, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

/* ------------------------------------------------------------------------------------------------ imagens */
typedef struct { VkImage img; VkDeviceMemory mem; VkImageView view; VkFormat fmt; uint32_t w, h; } Img;

static Img img_new(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
    Img im = { .fmt = fmt, .w = w, .h = h };
    VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
                              .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                              .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    must(vkCreateImage(dev, &ici, NULL, &im.img), "vkCreateImage(rt)");
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, im.img, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                 .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    must(vkAllocateMemory(dev, &mai, NULL, &im.mem), "vkAllocateMemory(rt)");
    must(vkBindImageMemory(dev, im.img, im.mem, 0), "vkBindImageMemory(rt)");
    VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im.img,
                                  .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt, .subresourceRange = { aspect, 0, 1, 0, 1 } };
    must(vkCreateImageView(dev, &vci, NULL, &im.view), "vkCreateImageView(rt)");
    return im;
}

static void img_free(Img *im)
{
    vkDestroyImageView(dev, im->view, NULL);
    vkDestroyImage(dev, im->img, NULL);
    vkFreeMemory(dev, im->mem, NULL);
    memset(im, 0, sizeof(*im));
}

/* As 3.600 texturas BC1 num único bloco de memória (o limite de alocações do Vulkan pode ser 4.096). */
static VkImage        tex_img[NTEX];
static VkImageView    tex_view[NTEX];
static VkDeviceMemory tex_mem;
static VkImage        noise_img;
static VkImageView    noise_view;
static VkDeviceMemory noise_mem;

static uint32_t rng_state = 12345u;
static uint32_t rng(void) { rng_state = rng_state * 1664525u + 1013904223u; return rng_state; }

static void make_textures(void)
{
    const uint32_t levels = 9;   /* 256 .. 1 */
    VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                              .format = VK_FORMAT_BC1_RGBA_UNORM_BLOCK, .extent = { TEXDIM, TEXDIM, 1 }, .mipLevels = levels,
                              .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                              .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkDeviceSize off[NTEX], total = 0;
    uint32_t bits = ~0u;
    for (int i = 0; i < NTEX; i++) {
        must(vkCreateImage(dev, &ici, NULL, &tex_img[i]), "vkCreateImage(tex)");
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(dev, tex_img[i], &mr);
        total = (total + mr.alignment - 1) & ~(mr.alignment - 1);
        off[i] = total;
        total += mr.size;
        bits &= mr.memoryTypeBits;
    }
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = total,
                                 .memoryTypeIndex = mem_type(bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    must(vkAllocateMemory(dev, &mai, NULL, &tex_mem), "vkAllocateMemory(tex)");
    for (int i = 0; i < NTEX; i++) must(vkBindImageMemory(dev, tex_img[i], tex_mem, off[i]), "vkBindImageMemory(tex)");

    /* dados: blocos BC1 aleatórios, os mesmos para todas (o que importa é o acesso, não o conteúdo) */
    VkDeviceSize chain = 0;
    for (uint32_t l = 0, d = TEXDIM; l < levels; l++, d = d > 1 ? d / 2 : 1) chain += (VkDeviceSize)((d + 3) / 4) * ((d + 3) / 4) * 8;
    Buf st = buf_new(chain, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    for (VkDeviceSize i = 0; i < chain / 4; i++) ((uint32_t *)st.map)[i] = rng();
    VkBufferImageCopy reg[9];
    VkDeviceSize o = 0;
    for (uint32_t l = 0, d = TEXDIM; l < levels; l++, d = d > 1 ? d / 2 : 1) {
        reg[l] = (VkBufferImageCopy){ .bufferOffset = o, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1 },
                                      .imageExtent = { d, d, 1 } };
        o += (VkDeviceSize)((d + 3) / 4) * ((d + 3) / 4) * 8;
    }
    for (int base = 0; base < NTEX; base += 400) {
        VkCommandBuffer cb = one_begin();
        for (int i = base; i < base + 400 && i < NTEX; i++) {
            image_layout(cb, tex_img[i], VK_IMAGE_ASPECT_COLOR_BIT, levels, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdCopyBufferToImage(cb, st.buf, tex_img[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, levels, reg);
            image_layout(cb, tex_img[i], VK_IMAGE_ASPECT_COLOR_BIT, levels, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        one_end(cb);
    }
    buf_free(&st);
    for (int i = 0; i < NTEX; i++) {
        VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = tex_img[i],
                                      .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = ici.format,
                                      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1 } };
        must(vkCreateImageView(dev, &vci, NULL, &tex_view[i]), "vkCreateImageView(tex)");
    }
    say("bench: %d texturas BC1 %dx%d com mips, %.1f MB", NTEX, TEXDIM, TEXDIM, total / 1048576.0);

    /* volume de ruído 64^3 para a névoa */
    VkImageCreateInfo nci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_3D,
                              .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { 64, 64, 64 }, .mipLevels = 1, .arrayLayers = 1,
                              .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                              .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    must(vkCreateImage(dev, &nci, NULL, &noise_img), "vkCreateImage(noise)");
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, noise_img, &mr);
    VkMemoryAllocateInfo nai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                 .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    must(vkAllocateMemory(dev, &nai, NULL, &noise_mem), "vkAllocateMemory(noise)");
    must(vkBindImageMemory(dev, noise_img, noise_mem, 0), "vkBindImageMemory(noise)");
    Buf ns = buf_new(64 * 64 * 64 * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    for (int i = 0; i < 64 * 64 * 64; i++) ((uint32_t *)ns.map)[i] = rng();
    VkCommandBuffer cb = one_begin();
    image_layout(cb, noise_img, VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy nr = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { 64, 64, 64 } };
    vkCmdCopyBufferToImage(cb, ns.buf, noise_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &nr);
    image_layout(cb, noise_img, VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    one_end(cb);
    buf_free(&ns);
    VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = noise_img,
                                  .viewType = VK_IMAGE_VIEW_TYPE_3D, .format = nci.format,
                                  .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    must(vkCreateImageView(dev, &vci, NULL, &noise_view), "vkCreateImageView(noise)");
}

/* ------------------------------------------------------------------------------------------------ geometria */
typedef struct { float p[3], n[3], uv[2]; } Vtx;
typedef struct { uint32_t first_index, index_count; int32_t vertex_offset; } Mesh;
static Mesh mesh_small[NMESH], mesh_big[NMESH];
static Buf  vbuf, ibuf;

static void sphere(Vtx *v, uint32_t *ix, int seg, float wob, uint32_t *nv, uint32_t *ni)
{
    uint32_t a = 0, b = 0;
    for (int y = 0; y <= seg; y++)
        for (int x = 0; x <= seg; x++) {
            float th = (float)y / seg * 3.14159265f, ph = (float)x / seg * 6.2831853f;
            float r = 1.0f + wob * sinf(ph * 3.0f) * sinf(th * 2.0f);
            float n[3] = { sinf(th) * cosf(ph), cosf(th), sinf(th) * sinf(ph) };
            v[a++] = (Vtx){ { n[0] * r, n[1] * r, n[2] * r }, { n[0], n[1], n[2] }, { (float)x / seg, (float)y / seg } };
        }
    for (int y = 0; y < seg; y++)
        for (int x = 0; x < seg; x++) {
            uint32_t i0 = y * (seg + 1) + x, i1 = i0 + 1, i2 = i0 + seg + 1, i3 = i2 + 1;
            ix[b++] = i0; ix[b++] = i2; ix[b++] = i1;
            ix[b++] = i1; ix[b++] = i2; ix[b++] = i3;
        }
    *nv = a; *ni = b;
}

static void make_geometry(void)
{
    const int ss = 16, sb = 32;   /* 512 e 2.048 triângulos */
    size_t vmax = (size_t)NMESH * ((ss + 1) * (ss + 1) + (sb + 1) * (sb + 1));
    size_t imax = (size_t)NMESH * (ss * ss * 6 + sb * sb * 6);
    Buf vs = buf_new(vmax * sizeof(Vtx), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    Buf is = buf_new(imax * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    Vtx *v = vs.map;
    uint32_t *ix = is.map, vo = 0, io = 0;
    for (int m = 0; m < NMESH; m++)
        for (int k = 0; k < 2; k++) {
            uint32_t nv, ni;
            sphere(v + vo, ix + io, k ? sb : ss, 0.05f + 0.01f * m, &nv, &ni);
            Mesh *me = k ? &mesh_big[m] : &mesh_small[m];
            *me = (Mesh){ io, ni, (int32_t)vo };
            vo += nv; io += ni;
        }
    vbuf = buf_new(vo * sizeof(Vtx), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    ibuf = buf_new((VkDeviceSize)io * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkCommandBuffer cb = one_begin();
    VkBufferCopy c1 = { 0, 0, vo * sizeof(Vtx) }, c2 = { 0, 0, (VkDeviceSize)io * 4 };
    vkCmdCopyBuffer(cb, vs.buf, vbuf.buf, 1, &c1);
    vkCmdCopyBuffer(cb, is.buf, ibuf.buf, 1, &c2);
    one_end(cb);
    buf_free(&vs);
    buf_free(&is);
    say("bench: geometria %u vértices, %u índices", vo, io);
}

/* ------------------------------------------------------------------------------------------------ pipelines */
static VkShaderModule shmod(const uint32_t *code, size_t size)
{
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code };
    VkShaderModule m;
    must(vkCreateShaderModule(dev, &ci, NULL, &m), "vkCreateShaderModule");
    return m;
}
#define SHMOD(name) shmod(name, sizeof(name))

static VkSampler smp_linear, smp_clamp;
static VkDescriptorSetLayout dsl_geo, dsl_light, dsl_fog, dsl_bloom, dsl_taa;
static VkPipelineLayout pl_geo, pl_light, pl_fog, pl_bloom, pl_taa;
static VkDescriptorPool dpool;
static VkDescriptorSet  ds_geo[SLOTS];
static Buf              draws_buf[SLOTS];

static VkDescriptorSetLayout make_dsl(const VkDescriptorSetLayoutBinding *b, uint32_t n, const void *next)
{
    VkDescriptorSetLayoutCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .pNext = next,
                                           .bindingCount = n, .pBindings = b };
    VkDescriptorSetLayout l;
    must(vkCreateDescriptorSetLayout(dev, &ci, NULL, &l), "vkCreateDescriptorSetLayout");
    return l;
}

static VkPipelineLayout make_pl(VkDescriptorSetLayout l, uint32_t push, VkShaderStageFlags stages)
{
    VkPushConstantRange pr = { stages, 0, push };
    VkPipelineLayoutCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                      .pSetLayouts = &l, .pushConstantRangeCount = push ? 1 : 0, .pPushConstantRanges = &pr };
    VkPipelineLayout p;
    must(vkCreatePipelineLayout(dev, &ci, NULL, &p), "vkCreatePipelineLayout");
    return p;
}

static void make_layouts(void)
{
    VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR,
                                .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
                                .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT, .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT, .maxLod = 16.0f };
    must(vkCreateSampler(dev, &sci, NULL, &smp_linear), "vkCreateSampler");
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    must(vkCreateSampler(dev, &sci, NULL, &smp_clamp), "vkCreateSampler(clamp)");

    VkDescriptorSetLayoutBinding geo[3] = {
        { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &smp_linear },
        { 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, NTEX, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
    };
    VkDescriptorBindingFlags bf[3] = { 0, 0, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT };
    VkDescriptorSetLayoutBindingFlagsCreateInfo bfi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
                                                        .bindingCount = 3, .pBindingFlags = bf };
    dsl_geo = make_dsl(geo, 3, &bfi);
    pl_geo = make_pl(dsl_geo, 16, VK_SHADER_STAGE_VERTEX_BIT);

    VkDescriptorSetLayoutBinding light[6] = { { 0, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &smp_clamp } };
    for (int i = 1; i < 6; i++) light[i] = (VkDescriptorSetLayoutBinding){ i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    dsl_light = make_dsl(light, 6, NULL);
    pl_light = make_pl(dsl_light, 0, 0);

    VkDescriptorSetLayoutBinding fog[4] = {
        { 0, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &smp_clamp },
        { 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
        { 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
        { 3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
    };
    dsl_fog = make_dsl(fog, 4, NULL);
    pl_fog = make_pl(dsl_fog, 0, 0);
    dsl_taa = make_dsl(fog, 4, NULL);   /* mesma forma: amostrador, duas texturas, uma imagem */
    pl_taa = make_pl(dsl_taa, 0, 0);
    VkDescriptorSetLayoutBinding bloom[3] = { fog[0], fog[1], { 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL } };
    dsl_bloom = make_dsl(bloom, 3, NULL);
    pl_bloom = make_pl(dsl_bloom, 16, VK_SHADER_STAGE_COMPUTE_BIT);

    VkDescriptorPoolSize ps[] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8 }, { VK_DESCRIPTOR_TYPE_SAMPLER, 64 },
                                  { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, NTEX * SLOTS + 128 }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 64 } };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, .maxSets = 64,
                                       .poolSizeCount = 4, .pPoolSizes = ps };
    must(vkCreateDescriptorPool(dev, &dpi, NULL, &dpool), "vkCreateDescriptorPool");

    VkDescriptorImageInfo *ii = calloc(NTEX, sizeof(*ii));
    for (int i = 0; i < NTEX; i++) ii[i] = (VkDescriptorImageInfo){ .imageView = tex_view[i], .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    for (int s = 0; s < SLOTS; s++) {
        draws_buf[s] = buf_new(MAXDRAW * 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dpool,
                                           .descriptorSetCount = 1, .pSetLayouts = &dsl_geo };
        must(vkAllocateDescriptorSets(dev, &ai, &ds_geo[s]), "vkAllocateDescriptorSets(geo)");
        VkDescriptorBufferInfo bi = { draws_buf[s].buf, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet w[2] = {
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds_geo[s], .dstBinding = 0, .descriptorCount = 1,
              .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi },
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds_geo[s], .dstBinding = 2, .descriptorCount = NTEX,
              .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = ii },
        };
        vkUpdateDescriptorSets(dev, 2, w, 0, NULL);
    }
    free(ii);
}

static VkPipeline make_graphics(VkShaderModule vs, VkShaderModule fs, VkRenderPass rp, VkPipelineLayout pl,
                                int ncolor, int with_vertices, int with_depth)
{
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" },
    };
    VkVertexInputBindingDescription vb = { 0, sizeof(Vtx), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[3] = { { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },
                                                { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12 },
                                                { 2, 0, VK_FORMAT_R32G32_SFLOAT, 24 } };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                                .vertexBindingDescriptionCount = with_vertices ? 1 : 0, .pVertexBindingDescriptions = &vb,
                                                .vertexAttributeDescriptionCount = with_vertices ? 3 : 0, .pVertexAttributeDescriptions = va };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                  .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                  .cullMode = with_vertices ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE,
                                                  .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineDepthStencilStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                 .depthTestEnable = with_depth, .depthWriteEnable = with_depth,
                                                 .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL };
    VkPipelineColorBlendAttachmentState cba[4];
    for (int i = 0; i < 4; i++) cba[i] = (VkPipelineColorBlendAttachmentState){ .colorWriteMask = 0xf };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                               .attachmentCount = ncolor, .pAttachments = cba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gp = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st,
                                        .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp,
                                        .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                        .pColorBlendState = &cb, .pDynamicState = &dy, .layout = pl, .renderPass = rp };
    VkPipeline p;
    must(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &p), "vkCreateGraphicsPipelines");
    return p;
}

static VkPipeline make_compute(VkShaderModule m, VkPipelineLayout pl)
{
    VkComputePipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                       .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                  .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main" },
                                       .layout = pl };
    VkPipeline p;
    must(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &p), "vkCreateComputePipelines");
    return p;
}

/* Passe de render com n alvos de cor e profundidade opcional. Os alvos de cor terminam prontos para leitura em shader,
 * a profundidade em modo só leitura (a luz a amostra). final_general: o alvo de cor fica em GENERAL (o compute o lê). */
static VkRenderPass make_rp(const VkFormat *fmt, int n, int depth, int final_general)
{
    VkAttachmentDescription at[5];
    VkAttachmentReference cr[4], dr = { (uint32_t)n, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    for (int i = 0; i < n; i++) {
        at[i] = (VkAttachmentDescription){ .format = fmt[i], .samples = VK_SAMPLE_COUNT_1_BIT,
                                           .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                           .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                           .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                           .finalLayout = final_general ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        cr[i] = (VkAttachmentReference){ (uint32_t)i, final_general ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    }
    if (depth)
        at[n] = (VkAttachmentDescription){ .format = VK_FORMAT_D32_SFLOAT, .samples = VK_SAMPLE_COUNT_1_BIT,
                                           .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                           .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                           .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                           .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
    VkSubpassDescription sp = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = n,
                                .pColorAttachments = cr, .pDepthStencilAttachment = depth ? &dr : NULL };
    VkSubpassDependency dep[2] = {
        { VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
          VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0 },
        { 0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
          VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0 },
    };
    VkRenderPassCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = n + (depth ? 1 : 0),
                                  .pAttachments = at, .subpassCount = 1, .pSubpasses = &sp, .dependencyCount = 2, .pDependencies = dep };
    VkRenderPass rp;
    must(vkCreateRenderPass(dev, &ci, NULL, &rp), "vkCreateRenderPass");
    return rp;
}

static VkFramebuffer make_fb(VkRenderPass rp, const VkImageView *v, int n, uint32_t w, uint32_t h)
{
    VkFramebufferCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = n,
                                   .pAttachments = v, .width = w, .height = h, .layers = 1 };
    VkFramebuffer f;
    must(vkCreateFramebuffer(dev, &ci, NULL, &f), "vkCreateFramebuffer");
    return f;
}

/* ------------------------------------------------------------------------------------------------ estado de uma rodada */
static VkRenderPass rp_fwd, rp_gbuf, rp_light;
static VkPipeline   pp_fwd, pp_gbuf, pp_light, pp_fog, pp_bloom, pp_taa;

typedef struct {
    Img color, depth, g[4], hdr, fog, bloom[BLOOM_LEVELS], taa[2];
    VkFramebuffer fb_fwd, fb_gbuf, fb_light;
    VkDescriptorSet ds_light, ds_fog, ds_bloom_down[BLOOM_LEVELS], ds_bloom_up[BLOOM_LEVELS - 1], ds_taa[2];
} Targets;

static VkDescriptorSet alloc_set(VkDescriptorSetLayout l)
{
    VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dpool,
                                       .descriptorSetCount = 1, .pSetLayouts = &l };
    VkDescriptorSet s;
    must(vkAllocateDescriptorSets(dev, &ai, &s), "vkAllocateDescriptorSets");
    return s;
}

static void write_img(VkDescriptorSet s, uint32_t binding, VkDescriptorType t, VkImageView v, VkImageLayout l)
{
    VkDescriptorImageInfo ii = { .imageView = v, .imageLayout = l };
    VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s, .dstBinding = binding,
                               .descriptorCount = 1, .descriptorType = t, .pImageInfo = &ii };
    vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
}

static void targets_new(Targets *t, const Run *r)
{
    memset(t, 0, sizeof(*t));
    const VkImageUsageFlags rt = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    const VkImageUsageFlags st = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    t->depth = img_new(r->w, r->h, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                       VK_IMAGE_ASPECT_DEPTH_BIT);
    if (r->scene == SCENE_S1) {
        t->color = img_new(r->w, r->h, VK_FORMAT_R8G8B8A8_UNORM, rt, VK_IMAGE_ASPECT_COLOR_BIT);
        VkImageView v[2] = { t->color.view, t->depth.view };
        t->fb_fwd = make_fb(rp_fwd, v, 2, r->w, r->h);
        return;
    }
    const VkFormat gf[4] = { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16_SFLOAT };
    for (int i = 0; i < 4; i++) t->g[i] = img_new(r->w, r->h, gf[i], rt, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageView gv[5] = { t->g[0].view, t->g[1].view, t->g[2].view, t->g[3].view, t->depth.view };
    t->fb_gbuf = make_fb(rp_gbuf, gv, 5, r->w, r->h);
    t->hdr = img_new(r->w, r->h, VK_FORMAT_R16G16B16A16_SFLOAT, rt | VK_IMAGE_USAGE_STORAGE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    t->fb_light = make_fb(rp_light, &t->hdr.view, 1, r->w, r->h);
    t->fog = img_new(r->w, r->h, VK_FORMAT_R16G16B16A16_SFLOAT, st, VK_IMAGE_ASPECT_COLOR_BIT);
    for (int i = 0; i < BLOOM_LEVELS; i++)
        t->bloom[i] = img_new(r->w >> (i + 1), r->h >> (i + 1), VK_FORMAT_R16G16B16A16_SFLOAT, st, VK_IMAGE_ASPECT_COLOR_BIT);
    for (int i = 0; i < 2; i++) t->taa[i] = img_new(r->w, r->h, VK_FORMAT_R16G16B16A16_SFLOAT, st, VK_IMAGE_ASPECT_COLOR_BIT);

    /* tudo o que o compute toca vive em GENERAL */
    VkCommandBuffer cb = one_begin();
    image_layout(cb, t->fog.img, VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    for (int i = 0; i < BLOOM_LEVELS; i++)
        image_layout(cb, t->bloom[i].img, VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    for (int i = 0; i < 2; i++)
        image_layout(cb, t->taa[i].img, VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    one_end(cb);

    const VkImageLayout RO = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, GE = VK_IMAGE_LAYOUT_GENERAL;
    t->ds_light = alloc_set(dsl_light);
    for (int i = 0; i < 4; i++) write_img(t->ds_light, 1 + i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, t->g[i].view, RO);
    write_img(t->ds_light, 5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, t->depth.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    t->ds_fog = alloc_set(dsl_fog);
    write_img(t->ds_fog, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, t->hdr.view, GE);
    write_img(t->ds_fog, 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, noise_view, RO);
    write_img(t->ds_fog, 3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, t->fog.view, GE);
    for (int i = 0; i < BLOOM_LEVELS; i++) {
        t->ds_bloom_down[i] = alloc_set(dsl_bloom);
        write_img(t->ds_bloom_down[i], 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, i ? t->bloom[i - 1].view : t->fog.view, GE);
        write_img(t->ds_bloom_down[i], 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, t->bloom[i].view, GE);
    }
    for (int i = 0; i < BLOOM_LEVELS - 1; i++) {   /* sobe do nível i+1 para o i */
        t->ds_bloom_up[i] = alloc_set(dsl_bloom);
        write_img(t->ds_bloom_up[i], 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, t->bloom[i + 1].view, GE);
        write_img(t->ds_bloom_up[i], 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, t->bloom[i].view, GE);
    }
    for (int i = 0; i < 2; i++) {   /* ping-pong: lê fog + taa[1-i], escreve taa[i] */
        t->ds_taa[i] = alloc_set(dsl_taa);
        write_img(t->ds_taa[i], 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, t->fog.view, GE);
        write_img(t->ds_taa[i], 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, t->taa[1 - i].view, GE);
        write_img(t->ds_taa[i], 3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, t->taa[i].view, GE);
    }
}

static void targets_free(Targets *t)
{
    vkDeviceWaitIdle(dev);
    VkDescriptorSet sets[2 + BLOOM_LEVELS * 2 + 1];
    int n = 0;
    if (t->ds_light) {
        sets[n++] = t->ds_light; sets[n++] = t->ds_fog;
        for (int i = 0; i < BLOOM_LEVELS; i++) sets[n++] = t->ds_bloom_down[i];
        for (int i = 0; i < BLOOM_LEVELS - 1; i++) sets[n++] = t->ds_bloom_up[i];
        sets[n++] = t->ds_taa[0]; sets[n++] = t->ds_taa[1];
        vkFreeDescriptorSets(dev, dpool, n, sets);
    }
    if (t->fb_fwd) vkDestroyFramebuffer(dev, t->fb_fwd, NULL);
    if (t->fb_gbuf) vkDestroyFramebuffer(dev, t->fb_gbuf, NULL);
    if (t->fb_light) vkDestroyFramebuffer(dev, t->fb_light, NULL);
    Img *all[] = { &t->color, &t->depth, &t->g[0], &t->g[1], &t->g[2], &t->g[3], &t->hdr, &t->fog,
                   &t->bloom[0], &t->bloom[1], &t->bloom[2], &t->bloom[3], &t->bloom[4], &t->taa[0], &t->taa[1] };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++)
        if (all[i]->img) img_free(all[i]);
}

/* ------------------------------------------------------------------------------------------------ o quadro */
static void fill_draws(float *m, int n, uint32_t frame)
{
    /* matriz MVP por draw: uma grade de objetos girando diante de uma câmera em perspectiva */
    const float f = 1.0f / tanf(0.6f), asp = 16.0f / 9.0f, zn = 0.1f, zf = 200.0f;
    int side = (int)ceilf(sqrtf((float)n));
    for (int i = 0; i < n; i++) {
        float a = frame * 0.01f + i * 0.37f, c = cosf(a), s = sinf(a), sc = 0.35f;
        float tx = ((i % side) - side * 0.5f) * 0.9f, ty = ((i / side) - side * 0.5f) * 0.5f, tz = -(8.0f + (i % 7) * 3.0f);
        /* modelo em colunas: rotação em Y com escala, depois translação */
        const float M[4][4] = { { c * sc, 0, -s * sc, 0 }, { 0, sc, 0, 0 }, { s * sc, 0, c * sc, 0 }, { tx, ty, tz, 1 } };
        /* projeção em perspectiva com z de 0 a 1 (Vulkan), multiplicada coluna a coluna */
        const float p0 = f / asp, p5 = f, p10 = zf / (zn - zf), p14 = zn * zf / (zn - zf);
        float *o = m + i * 16;
        for (int j = 0; j < 4; j++) {
            o[j * 4 + 0] = p0 * M[j][0];
            o[j * 4 + 1] = p5 * M[j][1];
            o[j * 4 + 2] = p10 * M[j][2] + p14 * M[j][3];
            o[j * 4 + 3] = -M[j][2];
        }
    }
}

typedef struct { VkCommandBuffer cb; VkFence fence; VkQueryPool qp; int used; } Slot;

static void record(Slot *sl, int slot, const Run *r, Targets *t, uint32_t frame)
{
    VkCommandBuffer cb = sl->cb;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(cb, &bi);
    vkCmdResetQueryPool(cb, sl->qp, 0, NQ);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, sl->qp, 0);

    VkViewport vp = { 0, 0, (float)r->w, (float)r->h, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { (uint32_t)r->w, (uint32_t)r->h } };
    VkClearValue cv[5] = { { .color = { { 0.1f, 0.1f, 0.15f, 1 } } }, { .color = { { 0 } } }, { .color = { { 0 } } },
                           { .color = { { 0 } } }, { .depthStencil = { 1.0f, 0 } } };
    VkDeviceSize zero = 0;
    const int s1 = r->scene == SCENE_S1;
    const Mesh *meshes = s1 ? mesh_small : mesh_big;

    VkClearValue cv_fwd[2] = { cv[0], cv[4] };
    VkRenderPassBeginInfo rb = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = s1 ? rp_fwd : rp_gbuf,
                                 .framebuffer = s1 ? t->fb_fwd : t->fb_gbuf, .renderArea = sc,
                                 .clearValueCount = s1 ? 2 : 5, .pClearValues = s1 ? cv_fwd : cv };
    vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s1 ? pp_fwd : pp_gbuf);
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl_geo, 0, 1, &ds_geo[slot], 0, NULL);
    vkCmdBindVertexBuffers(cb, 0, 1, &vbuf.buf, &zero);
    vkCmdBindIndexBuffer(cb, ibuf.buf, 0, VK_INDEX_TYPE_UINT32);
    for (int i = 0; i < r->draws; i++) {
        uint32_t pc[4] = { (uint32_t)i, (uint32_t)((i * 7) % (NTEX - 3)), 0, 0 };
        vkCmdPushConstants(cb, pl_geo, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, pc);
        const Mesh *m = &meshes[i % NMESH];
        vkCmdDrawIndexed(cb, m->index_count, 1, m->first_index, m->vertex_offset, 0);
    }
    vkCmdEndRenderPass(cb);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, sl->qp, 1);

    if (!s1) {
        /* luz deferred */
        VkClearValue hc = cv[1];
        VkRenderPassBeginInfo lb = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp_light,
                                     .framebuffer = t->fb_light, .renderArea = sc, .clearValueCount = 1, .pClearValues = &hc };
        vkCmdBeginRenderPass(cb, &lb, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pp_light);
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &sc);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl_light, 0, 1, &t->ds_light, 0, NULL);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRenderPass(cb);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, sl->qp, 2);

        /* névoa */
        barrier_all(cb);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pp_fog);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl_fog, 0, 1, &t->ds_fog, 0, NULL);
        vkCmdDispatch(cb, (r->w + 7) / 8, (r->h + 7) / 8, 1);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, sl->qp, 3);

        /* bloom: desce 5 níveis, sobe 4 */
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pp_bloom);
        for (int i = 0; i < BLOOM_LEVELS; i++) {
            barrier_all(cb);
            Img *src = i ? &t->bloom[i - 1] : &t->fog;
            struct { uint32_t mode; float tx, ty; uint32_t pad; } pc = { 0, 1.0f / src->w, 1.0f / src->h, 0 };
            vkCmdPushConstants(cb, pl_bloom, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, &pc);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl_bloom, 0, 1, &t->ds_bloom_down[i], 0, NULL);
            vkCmdDispatch(cb, (t->bloom[i].w + 7) / 8, (t->bloom[i].h + 7) / 8, 1);
        }
        for (int i = BLOOM_LEVELS - 2; i >= 0; i--) {
            barrier_all(cb);
            struct { uint32_t mode; float tx, ty; uint32_t pad; } pc = { 1, 1.0f / t->bloom[i + 1].w, 1.0f / t->bloom[i + 1].h, 0 };
            vkCmdPushConstants(cb, pl_bloom, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, &pc);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl_bloom, 0, 1, &t->ds_bloom_up[i], 0, NULL);
            vkCmdDispatch(cb, (t->bloom[i].w + 7) / 8, (t->bloom[i].h + 7) / 8, 1);
        }
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, sl->qp, 4);

        /* TAA */
        barrier_all(cb);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pp_taa);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl_taa, 0, 1, &t->ds_taa[frame & 1], 0, NULL);
        vkCmdDispatch(cb, (r->w + 7) / 8, (r->h + 7) / 8, 1);
        barrier_all(cb);
    } else {
        /* S1 não tem esses passes, mas a leitura pede as 6 consultas disponíveis */
        for (uint32_t q = 2; q <= 4; q++) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, sl->qp, q);
    }
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, sl->qp, 5);
    vkEndCommandBuffer(cb);
}

/* ------------------------------------------------------------------------------------------------ tela de progresso
 * Só no console (VK_EXT_headless_surface, que o mesa-ps4 transforma em sceVideoOutSubmitFlip). Uma vez por segundo a CPU
 * desenha a tela num buffer e ela vai para a TV; o quadro da tela fica fora da medição. Sem texto: um quadrado que
 * pisca (vivo), uma barra por cena (cinza falta, amarelo rodando, verde feita, vermelho falhou) e números de 7 segmentos
 * (cena atual e segundos restantes). */
static int            st_on;
static VkSurfaceKHR   st_surface;
static VkSwapchainKHR st_swap;
static VkImage        st_img[8];
static uint32_t       st_nimg, st_w, st_h;
static int            st_bgra;
static Buf            st_buf;
static VkCommandBuffer st_cb;
static VkFence        st_fence;
static VkSemaphore    st_acq, st_done;
static int            st_state[16];      /* 0 falta, 1 rodando, 2 feita, 3 falhou */
static double         st_frac;           /* progresso da cena atual, 0..1 */
static int            st_cur = -1, st_left, st_beat;

static void st_rect(int x, int y, int w, int h, uint32_t rgb)
{
    uint32_t px = st_bgra ? (0xff000000u | rgb) : (0xff000000u | ((rgb & 0xff) << 16) | (rgb & 0xff00) | ((rgb >> 16) & 0xff));
    uint32_t *p = st_buf.map;
    for (int j = y < 0 ? 0 : y; j < y + h && j < (int)st_h; j++)
        for (int i = x < 0 ? 0 : x; i < x + w && i < (int)st_w; i++) p[j * st_w + i] = px;
}

static void st_digit(int x, int y, int s, int d, uint32_t rgb)
{
    static const unsigned char seg[10] = { 0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f };   /* gfedcba */
    unsigned m = seg[d % 10];
    int t = s, L = s * 5;
    if (m & 0x01) st_rect(x + t, y, L, t, rgb);                 /* a */
    if (m & 0x02) st_rect(x + t + L, y + t, t, L, rgb);         /* b */
    if (m & 0x04) st_rect(x + t + L, y + 2 * t + L, t, L, rgb); /* c */
    if (m & 0x08) st_rect(x + t, y + 2 * t + 2 * L, L, t, rgb); /* d */
    if (m & 0x10) st_rect(x, y + 2 * t + L, t, L, rgb);         /* e */
    if (m & 0x20) st_rect(x, y + t, t, L, rgb);                 /* f */
    if (m & 0x40) st_rect(x + t, y + t + L, L, t, rgb);         /* g */
}

static void st_number(int x, int y, int s, int v, uint32_t rgb)
{
    char b[12];
    int n = snprintf(b, sizeof(b), "%d", v < 0 ? 0 : v);
    for (int i = 0; i < n; i++) st_digit(x + i * s * 9, y, s, b[i] - '0', rgb);
}

static void st_present(void)
{
    if (!st_on) return;
    st_beat ^= 1;
    st_rect(0, 0, st_w, st_h, 0x101820);
    st_rect(80, 80, 60, 60, st_beat ? 0x40ff40 : 0x206020);                 /* vivo */
    int bw = (st_w - 160 - (NRUNS - 1) * 20) / NRUNS;
    for (int i = 0; i < NRUNS; i++) {
        int x = 80 + i * (bw + 20), y = 300;
        uint32_t c = st_state[i] == 2 ? 0x30c040 : st_state[i] == 3 ? 0xe03030 : 0x404850;
        st_rect(x, y, bw, 120, c);
        if (st_state[i] == 1) st_rect(x, y, (int)(bw * st_frac), 120, 0xe0c030);
        st_number(x + 10, y + 140, 4, i + 1, 0xa0a8b0);
    }
    if (st_cur >= 0) {
        st_number(80, 560, 16, st_cur + 1, 0xffffff);                         /* cena */
        st_number(600, 560, 16, st_left, 0xe0c030);                           /* segundos restantes */
    }
    uint32_t idx;
    if (vkAcquireNextImageKHR(dev, st_swap, UINT64_MAX, st_acq, VK_NULL_HANDLE, &idx) != VK_SUCCESS) return;
    vkResetFences(dev, 1, &st_fence);
    vkResetCommandBuffer(st_cb, 0);
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(st_cb, &bi);
    image_layout(st_cb, st_img[idx], VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy r = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { st_w, st_h, 1 } };
    vkCmdCopyBufferToImage(st_cb, st_buf.buf, st_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    image_layout(st_cb, st_img[idx], VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    vkEndCommandBuffer(st_cb);
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &st_acq,
                        .pWaitDstStageMask = &ws, .commandBufferCount = 1, .pCommandBuffers = &st_cb,
                        .signalSemaphoreCount = 1, .pSignalSemaphores = &st_done };
    if (vkQueueSubmit(queue, 1, &si, st_fence) != VK_SUCCESS) return;
    VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &st_done,
                            .swapchainCount = 1, .pSwapchains = &st_swap, .pImageIndices = &idx };
    vkQueuePresentKHR(queue, &pi);
    vkWaitForFences(dev, 1, &st_fence, VK_TRUE, UINT64_MAX);
    frame_done();
}

static void st_fail(void)
{
    static int once;
    if (!st_on || once) return;
    once = 1;
    if (st_cur >= 0) st_state[st_cur] = 3;
    st_present();
}

static void st_init(void)
{
    if (!st_surface) return;
    VkBool32 ok = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(phys, qfam, st_surface, &ok);
    VkSurfaceCapabilitiesKHR caps;
    if (!ok || vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, st_surface, &caps) != VK_SUCCESS ||
        !(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        say("bench: tela de progresso indisponível (superfície sem cópia para a imagem)");
        return;
    }
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, st_surface, &nf, NULL);
    VkSurfaceFormatKHR fm[16];
    if (nf > 16) nf = 16;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, st_surface, &nf, fm);
    VkSurfaceFormatKHR f = fm[0];
    for (uint32_t i = 0; i < nf; i++)
        if (fm[i].format == VK_FORMAT_B8G8R8A8_UNORM || fm[i].format == VK_FORMAT_R8G8B8A8_UNORM) { f = fm[i]; break; }
    st_bgra = f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_SRGB;
    st_w = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent.width : 1920;
    st_h = caps.currentExtent.height != UINT32_MAX ? caps.currentExtent.height : 1080;
    VkSwapchainCreateInfoKHR sci = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = st_surface,
                                     .minImageCount = caps.minImageCount < 2 ? 2 : caps.minImageCount, .imageFormat = f.format,
                                     .imageColorSpace = f.colorSpace, .imageExtent = { st_w, st_h }, .imageArrayLayers = 1,
                                     .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                     .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = caps.currentTransform,
                                     .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = VK_PRESENT_MODE_FIFO_KHR,
                                     .clipped = VK_TRUE };
    if (vkCreateSwapchainKHR(dev, &sci, NULL, &st_swap) != VK_SUCCESS) { say("bench: tela de progresso: swapchain falhou"); return; }
    st_nimg = 8;
    vkGetSwapchainImagesKHR(dev, st_swap, &st_nimg, st_img);
    st_buf = buf_new((VkDeviceSize)st_w * st_h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cpool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    must(vkAllocateCommandBuffers(dev, &ai, &st_cb), "vkAllocateCommandBuffers(tela)");
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    must(vkCreateFence(dev, &fci, NULL, &st_fence), "vkCreateFence(tela)");
    VkSemaphoreCreateInfo sei = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    must(vkCreateSemaphore(dev, &sei, NULL, &st_acq), "vkCreateSemaphore(tela)");
    must(vkCreateSemaphore(dev, &sei, NULL, &st_done), "vkCreateSemaphore(tela)");
    st_on = 1;
    say("bench: tela de progresso %ux%u, %u imagens, formato %d", st_w, st_h, st_nimg, f.format);
    st_present();
}

/* ------------------------------------------------------------------------------------------------ estatística */
static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }
static double pct(double *v, int n, double p) { if (!n) return 0; qsort(v, n, sizeof(double), cmp_d); return v[(int)((n - 1) * p)]; }

#define MAXF 20000
static double s_fill[MAXF], s_rec[MAXF], s_sub[MAXF], s_frame[MAXF], s_gpu[MAXF], s_geo[MAXF], s_light[MAXF], s_post[MAXF];

static void run_one(int ri, const Run *r, Slot *slots)
{
    Targets t;
    targets_new(&t, r);
    say("bench: rodada %s (%dx%d, %d draws) - aquecendo %.0f s, medindo %.0f s", r->name, r->w, r->h, r->draws, g_warm_s, g_meas_s);
    st_cur = ri; st_state[ri] = 1; st_frac = 0; st_left = (int)(g_warm_s + g_meas_s);
    st_present();
    int n = 0, nall = 0;
    uint32_t frame = 0;
    double t0 = now_ms(), last = t0, next_st = t0 + 1000.0, next_log = t0 + 10000.0;
    for (;;) {
        int s = frame % SLOTS;
        Slot *sl = &slots[s];
        must(vkWaitForFences(dev, 1, &sl->fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
        double now = now_ms(), el = (now - t0) / 1000.0;
        int measuring = el >= g_warm_s;
        if (now >= next_log) {
            say("bench: %s ... %.0f s de %.0f, %d quadros medidos", r->name, el, g_warm_s + g_meas_s, nall);
            next_log += 10000.0;
        }
        if (st_on && now >= next_st) {
            /* a tela entra entre dois quadros: espera a GPU, apresenta e recomeça o relógio do intervalo */
            vkDeviceWaitIdle(dev);
            st_frac = el / (g_warm_s + g_meas_s);
            st_left = (int)(g_warm_s + g_meas_s - el + 0.5);
            st_present();
            now = now_ms();
            next_st = now + 1000.0;
            last = now;
            if (sl->used) { sl->used = 0; }   /* as consultas desse slot já foram lidas ou ficam de fora */
        }
        if (measuring && sl->used) nall++;
        if (sl->used && measuring && n < MAXF) {
            uint64_t q[NQ];
            if (vkGetQueryPoolResults(dev, sl->qp, 0, 6, sizeof(q), q, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
                double k = ts_period / 1e6;
                s_gpu[n] = (q[5] - q[0]) * k;
                s_geo[n] = (q[1] - q[0]) * k;
                s_light[n] = r->scene == SCENE_FULL ? (q[2] - q[1]) * k : 0;
                s_post[n] = r->scene == SCENE_FULL ? (q[5] - q[2]) * k : 0;
            }
            s_frame[n] = now - last;
        }
        if (el >= g_warm_s + g_meas_s) break;
        last = now;
        must(vkResetFences(dev, 1, &sl->fence), "vkResetFences");
        double a = now_ms();
        fill_draws(draws_buf[s].map, r->draws, frame);
        double b = now_ms();
        must(vkResetCommandBuffer(sl->cb, 0), "vkResetCommandBuffer");
        record(sl, s, r, &t, frame);
        double c = now_ms();
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &sl->cb };
        must(vkQueueSubmit(queue, 1, &si, sl->fence), "vkQueueSubmit");
        double d = now_ms();
        frame_done();   /* fora da medida de envio */
        if (sl->used && measuring && n < MAXF) { s_fill[n] = b - a; s_rec[n] = c - b; s_sub[n] = d - c; n++; }
        sl->used = 1;
        frame++;
    }
    vkDeviceWaitIdle(dev);
    for (int i = 0; i < SLOTS; i++) slots[i].used = 0;
    double fps = nall / g_meas_s;   /* todos os quadros; as amostras guardadas param em MAXF */
    st_state[ri] = 2;
    st_present();
    double rec50 = pct(s_rec, n, 0.5), rec95 = pct(s_rec, n, 0.95), sub50 = pct(s_sub, n, 0.5), fill50 = pct(s_fill, n, 0.5);
    double gpu50 = pct(s_gpu, n, 0.5), gpu95 = pct(s_gpu, n, 0.95), geo50 = pct(s_geo, n, 0.5);
    double light50 = pct(s_light, n, 0.5), post50 = pct(s_post, n, 0.5), fr50 = pct(s_frame, n, 0.5);
    double us_per_draw = (rec50 + sub50) * 1000.0 / r->draws;
    say("bench: %-10s quadros %5d  fps %6.1f  quadro p50 %6.2f ms | CPU gravar p50 %6.2f p95 %6.2f, enviar %5.2f, matrizes %5.2f ms"
        " (%.2f us/draw) | GPU p50 %6.2f p95 %6.2f ms (geo %6.2f luz %5.2f pós %5.2f)",
        r->name, n, fps, fr50, rec50, rec95, sub50, fill50, us_per_draw, gpu50, gpu95, geo50, light50, post50);
    if (g_csv) {
        fprintf(g_csv, "%s,%s,%d,%d,%d,%d,%.2f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n", g_variant, r->name, r->w, r->h,
                r->draws, n, fps, fr50, rec50, rec95, sub50, fill50, us_per_draw, gpu50, gpu95, geo50, light50, post50);
        fflush(g_csv);
    }
    targets_free(&t);
}

/* ------------------------------------------------------------------------------------------------ main */
int main(int argc, char **argv)
{
    if (argc > 1) g_meas_s = atof(argv[1]);
    if (argc > 2) g_warm_s = atof(argv[2]);
#ifdef __ORBIS__
    g_meas_s = 60.0; g_warm_s = 10.0;
    mkdir("/data/ocarina-bench", 0777);
    g_csv = fopen("/data/ocarina-bench/results.csv", "a");
#else
    g_csv = fopen("results.csv", "a");
#endif
    if (g_csv && ftell(g_csv) == 0)   /* em modo append o cabeçalho só vai uma vez */
        fprintf(g_csv, "api,run,w,h,draws,frames,fps,frame_p50,rec_p50,rec_p95,submit_p50,fill_p50,us_per_draw,gpu_p50,gpu_p95,geo_p50,light_p50,post_p50\n");
    say("bench: ocarina-ps4 Fase 0B, lado Vulkan");
#ifdef __ORBIS__
    {
        extern int sceKernelIsNeoMode(void);
        int neo = sceKernelIsNeoMode();
        g_variant = neo ? "vulkan-pro" : "vulkan-base";
#ifdef BENCH_NUM_SE
        /* O mesa-ps4 fixa a topologia do PS4 comum (2 motores de shader, 18 CUs) e não detecta o Pro. A chave de
         * experimento dele, ORBIS_NUM_SE, muda o número de motores; o Pro tem 4 (36 CUs). */
        char se[8];
        snprintf(se, sizeof(se), "%d", BENCH_NUM_SE);
        setenv("ORBIS_NUM_SE", se, 1);
        g_variant = neo ? "vulkan-pro-se" BENCH_NUM_SE_STR : "vulkan-base-se" BENCH_NUM_SE_STR;
#endif
        say("bench: modo %s (sceKernelIsNeoMode=%d), variante %s", neo ? "PRO" : "base", neo, g_variant);
    }
#endif

    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "ocarina-bench", .apiVersion = VK_API_VERSION_1_2 };
    /* a tela de progresso precisa de VK_EXT_headless_surface (o mesa-ps4 tem; no Linux sem janela fica desligada) */
    uint32_t nie = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &nie, NULL);
    VkExtensionProperties *ie = calloc(nie ? nie : 1, sizeof(*ie));
    vkEnumerateInstanceExtensionProperties(NULL, &nie, ie);
    int has_headless = 0, has_surface = 0;
    for (uint32_t i = 0; i < nie; i++) {
        if (!strcmp(ie[i].extensionName, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME)) has_headless = 1;
        if (!strcmp(ie[i].extensionName, VK_KHR_SURFACE_EXTENSION_NAME)) has_surface = 1;
    }
    free(ie);
    const char *iext[2] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME };
    int want_screen = has_headless && has_surface;
#ifndef __ORBIS__
    want_screen = want_screen && getenv("BENCH_SCREEN") != NULL;
#endif
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                                 .enabledExtensionCount = want_screen ? 2 : 0, .ppEnabledExtensionNames = iext };
    must(vkCreateInstance(&ici, NULL, &inst), "vkCreateInstance");
    if (want_screen) {
        VkHeadlessSurfaceCreateInfoEXT hs = { .sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT };
        PFN_vkCreateHeadlessSurfaceEXT mk = (PFN_vkCreateHeadlessSurfaceEXT)vkGetInstanceProcAddr(inst, "vkCreateHeadlessSurfaceEXT");
        if (!mk || mk(inst, &hs, NULL, &st_surface) != VK_SUCCESS) { st_surface = VK_NULL_HANDLE; say("bench: sem superfície para a tela"); }
    }
    uint32_t np = 0;
    vkEnumeratePhysicalDevices(inst, &np, NULL);
    VkPhysicalDevice pd[8];
    if (np > 8) np = 8;
    vkEnumeratePhysicalDevices(inst, &np, pd);
    if (!np) { say("bench: nenhum dispositivo Vulkan"); idle_forever(); }
    phys = pd[0];
    for (uint32_t i = 0; i < np; i++) {   /* no IdeaPad: a NVIDIA, não a Intel */
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { phys = pd[i]; break; }
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    ts_period = props.limits.timestampPeriod;
    vkGetPhysicalDeviceMemoryProperties(phys, &memprops);
    say("bench: dispositivo %s, Vulkan %u.%u.%u, driver 0x%x, timestamp %.2f ns", props.deviceName,
        VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion),
        props.driverVersion, ts_period);

    {
        /* topologia que o driver declara: no PS4 Pro com o patch Gladius do mesa-ps4, 4 motores e 36 CUs */
        uint32_t nde = 0;
        vkEnumerateDeviceExtensionProperties(phys, NULL, &nde, NULL);
        VkExtensionProperties *de = calloc(nde ? nde : 1, sizeof(*de));
        vkEnumerateDeviceExtensionProperties(phys, NULL, &nde, de);
        int has_core = 0;
        for (uint32_t i = 0; i < nde; i++)
            if (!strcmp(de[i].extensionName, VK_AMD_SHADER_CORE_PROPERTIES_EXTENSION_NAME)) has_core = 1;
        free(de);
        if (has_core) {
            VkPhysicalDeviceShaderCorePropertiesAMD sc = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CORE_PROPERTIES_AMD };
            VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &sc };
            vkGetPhysicalDeviceProperties2(phys, &p2);
            say("bench: topologia: %u motores de shader, %u arrays por motor, %u CUs por array (%u CUs), %u SIMDs por CU",
                sc.shaderEngineCount, sc.shaderArraysPerEngineCount, sc.computeUnitsPerShaderArray,
                sc.shaderEngineCount * sc.shaderArraysPerEngineCount * sc.computeUnitsPerShaderArray, sc.simdPerComputeUnit);
        }
    }

    VkPhysicalDeviceVulkan12Features f12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12 };
    vkGetPhysicalDeviceFeatures2(phys, &f2);
    say("bench: recursos: BC %d, descriptorIndexing %d, runtimeArray %d, nonUniformSampled %d, partiallyBound %d",
        f2.features.textureCompressionBC, f12.descriptorIndexing, f12.runtimeDescriptorArray,
        f12.shaderSampledImageArrayNonUniformIndexing, f12.descriptorBindingPartiallyBound);
    if (!f2.features.textureCompressionBC || !f12.runtimeDescriptorArray || !f12.shaderSampledImageArrayNonUniformIndexing ||
        !f12.descriptorBindingPartiallyBound) {
        say("bench: falta um recurso que a cena bindless precisa");
        idle_forever();
    }

    uint32_t nqf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nqf, NULL);
    VkQueueFamilyProperties qfp[16];
    if (nqf > 16) nqf = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nqf, qfp);
    qfam = UINT32_MAX;
    for (uint32_t i = 0; i < nqf; i++)
        if ((qfp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qfp[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) { qfam = i; break; }
    if (qfam == UINT32_MAX) { say("bench: sem fila gráfica+compute"); idle_forever(); }
    say("bench: fila %u, timestampValidBits %u", qfam, qfp[qfam].timestampValidBits);

    VkPhysicalDeviceVulkan12Features e12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                             .descriptorIndexing = VK_TRUE, .runtimeDescriptorArray = VK_TRUE,
                                             .shaderSampledImageArrayNonUniformIndexing = VK_TRUE,
                                             .descriptorBindingPartiallyBound = VK_TRUE };
    VkPhysicalDeviceFeatures2 e2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &e12,
                                     .features = { .textureCompressionBC = VK_TRUE } };
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfam,
                                    .queueCount = 1, .pQueuePriorities = &prio };
    const char *dext[1] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &e2, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                               .enabledExtensionCount = st_surface ? 1 : 0, .ppEnabledExtensionNames = dext };
    must(vkCreateDevice(phys, &dci, NULL, &dev), "vkCreateDevice");
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = qfam };
    must(vkCreateCommandPool(dev, &cpi, NULL, &cpool), "vkCreateCommandPool");

    double ta = now_ms();
    make_textures();
    make_geometry();
    make_layouts();
    const VkFormat fwd_f[1] = { VK_FORMAT_R8G8B8A8_UNORM };
    const VkFormat gb_f[4] = { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16_SFLOAT };
    const VkFormat hdr_f[1] = { VK_FORMAT_R16G16B16A16_SFLOAT };
    rp_fwd = make_rp(fwd_f, 1, 1, 0);
    rp_gbuf = make_rp(gb_f, 4, 1, 0);
    rp_light = make_rp(hdr_f, 1, 0, 1);
    double tb = now_ms();
    VkShaderModule geo = SHMOD(geo_vert), fwd = SHMOD(forward_frag), gbuf = SHMOD(gbuf_frag), fs = SHMOD(fullscreen_vert),
                   light = SHMOD(light_frag), fog = SHMOD(fog_comp), bloom = SHMOD(bloom_comp), taa = SHMOD(taa_comp);
    pp_fwd = make_graphics(geo, fwd, rp_fwd, pl_geo, 1, 1, 1);
    pp_gbuf = make_graphics(geo, gbuf, rp_gbuf, pl_geo, 4, 1, 1);
    pp_light = make_graphics(fs, light, rp_light, pl_light, 1, 0, 0);
    pp_fog = make_compute(fog, pl_fog);
    pp_bloom = make_compute(bloom, pl_bloom);
    pp_taa = make_compute(taa, pl_taa);
    double tc = now_ms();
    say("bench: recursos prontos em %.0f ms; 6 pipelines compilados em %.0f ms", tb - ta, tc - tb);

    Slot slots[SLOTS];
    for (int i = 0; i < SLOTS; i++) {
        VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cpool,
                                           .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        must(vkAllocateCommandBuffers(dev, &ai, &slots[i].cb), "vkAllocateCommandBuffers");
        VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
        must(vkCreateFence(dev, &fci, NULL, &slots[i].fence), "vkCreateFence");
        VkQueryPoolCreateInfo qpi = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = NQ };
        must(vkCreateQueryPool(dev, &qpi, NULL, &slots[i].qp), "vkCreateQueryPool");
        slots[i].used = 0;
    }
    st_init();
    for (int i = 0; i < NRUNS; i++) run_one(i, &RUNS[i], slots);
    say("bench: FIM - %d rodadas", NRUNS);
    st_cur = -1;
    st_present();
    if (g_csv) fclose(g_csv);
#ifdef __ORBIS__
    idle_forever();
#endif
    return 0;
}
