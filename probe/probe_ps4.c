/* ocarina-ps4 / Fase 0: a sonda do console. Seis provas, cada uma uma barra na tela (cinza falta, amarelo rodando,
 * verde passou, vermelho falhou), resultado em /data/ocarina-probe/ (summary.txt, lift_ps4.txt, map.txt) e no klog.
 *   1 memória: quanto de memória direta e flexível um homebrew aloca de fato
 *   2 endereços: as regiões fixas do runtime do recomp (módulos em 0x7000000000, kernel, funções do host) e os blocos
 *     de 16 GiB (pilhas e heap) e 64 GiB (alias) abaixo de 2^39
 *   3 commit: memória direta com acesso da GPU mapeada em endereço fixo dentro da reserva
 *   4 write-watch: proteger uma página, escrever, o tratador de falta registra e libera (o runtime usa isso para saber
 *     o que a CPU escreveu nos pools que a GPU lê)
 *   5 Vulkan: VK_EXT_external_memory_host, a GPU escrevendo direto na memória do guest (sem cópia)
 *   6 C do lifter: funções do OoT traduzidas, comparadas com a referência do IdeaPad */
#include <orbis_prefix.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

/* libkernel, com as assinaturas reais (o header do SDK erra a de sceKernelAvailableDirectMemorySize) */
extern int32_t sceKernelDebugOutText(int32_t, const char *);
extern int32_t sceKernelIsNeoMode(void);
extern size_t  sceKernelGetDirectMemorySize(void);
extern int32_t sceKernelAvailableDirectMemorySize(int64_t start, int64_t end, size_t align, int64_t *start_out, size_t *size_out);
extern int32_t sceKernelAvailableFlexibleMemorySize(size_t *size_out);
extern int32_t sceKernelAllocateDirectMemory(int64_t start, int64_t end, size_t len, size_t align, int32_t type, int64_t *out);
extern int32_t sceKernelReleaseDirectMemory(int64_t start, size_t len);
extern int32_t sceKernelMapDirectMemory(void **addr, size_t len, int32_t prot, int32_t flags, int64_t off, size_t align);
extern int32_t sceKernelMapFlexibleMemory(void **addr, size_t len, int32_t prot, int32_t flags);
extern int32_t sceKernelReserveVirtualRange(void **addr, size_t len, int32_t flags, size_t align);
extern int32_t sceKernelMunmap(void *addr, size_t len);
extern int32_t sceKernelMprotect(const void *addr, size_t len, int32_t prot);
extern int32_t sceKernelVirtualQuery(const void *addr, int32_t flags, void *info, size_t size);
extern int32_t sceGnmSubmitDone(void);

#define PROT_CPU_R  0x01
#define PROT_CPU_W  0x02
#define PROT_GPU_R  0x10
#define PROT_GPU_W  0x20
#define K_MAP_FIXED 0x10
#define WB_ONION    0        /* SCE_KERNEL_WB_ONION: CPU com cache, a GPU lê pelo barramento coerente */
#define WC_GARLIC   3
#define MiB (1ull << 20)
#define GiB (1ull << 30)
#define KPAGE (16 * 1024ull)

/* as regiões do runtime do dkb-pc-recomp (runtime/host.h e memory.c) */
#define MOD_BASE    0x7000000000ull
#define MOD_SIZE    0x6400000ull       /* main + subsdk0 + sdk do OoT: 0x7000000000..0x7006280000 */
#define KERN_BASE   0x7010000000ull
#define KERN_SIZE   (256 * MiB)
#define HOSTFN_BASE 0x7020000000ull
#define HOSTFN_SIZE (1 * MiB)
#define LOW_SIZE    (16 * GiB)
#define ALIAS_SIZE  (64 * GiB)
#define LIFT_ARENA_ADDR 0x0F00000000ull
#define LIFT_ARENA_SIZE (4 * MiB)

static FILE *g_sum;

static void say(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    strcat(buf, "\n");
    sceKernelDebugOutText(0, buf);
    if (g_sum) { fputs(buf, g_sum); fflush(g_sum); }
}

/* ------------------------------------------------------------------------------------- tela (como na bateria) */
#define NTEST 6
static int st_state[NTEST];
static VkInstance inst; static VkPhysicalDevice phys; static VkDevice dev; static VkQueue queue; static uint32_t qfam;
static VkSurfaceKHR st_surface; static VkSwapchainKHR st_swap; static VkImage st_img[8]; static uint32_t st_w, st_h;
static int st_bgra, st_on, st_beat; static VkBuffer st_buf; static VkDeviceMemory st_mem; static uint32_t *st_px;
static VkCommandPool cpool; static VkCommandBuffer st_cb; static VkFence st_fence; static VkSemaphore st_acq, st_done;
static VkPhysicalDeviceMemoryProperties memprops;

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static void st_rect(int x, int y, int w, int h, uint32_t rgb)
{
    uint32_t px = st_bgra ? (0xff000000u | rgb) : (0xff000000u | ((rgb & 0xff) << 16) | (rgb & 0xff00) | ((rgb >> 16) & 0xff));
    for (int j = y < 0 ? 0 : y; j < y + h && j < (int)st_h; j++)
        for (int i = x < 0 ? 0 : x; i < x + w && i < (int)st_w; i++) st_px[j * st_w + i] = px;
}

static void st_present(void)
{
    sceGnmSubmitDone();
    if (!st_on) return;
    st_beat ^= 1;
    st_rect(0, 0, st_w, st_h, 0x101820);
    st_rect(80, 80, 60, 60, st_beat ? 0x40a0ff : 0x204060);   /* azul: é a sonda, não a bateria */
    int bw = (st_w - 160 - (NTEST - 1) * 20) / NTEST;
    for (int i = 0; i < NTEST; i++) {
        uint32_t c = st_state[i] == 1 ? 0xe0c030 : st_state[i] == 2 ? 0x30c040 : st_state[i] == 3 ? 0xe03030 : 0x404850;
        st_rect(80 + i * (bw + 20), 300, bw, 160, c);
    }
    uint32_t idx;
    if (vkAcquireNextImageKHR(dev, st_swap, UINT64_MAX, st_acq, VK_NULL_HANDLE, &idx) != VK_SUCCESS) return;
    vkResetFences(dev, 1, &st_fence);
    vkResetCommandBuffer(st_cb, 0);
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(st_cb, &bi);
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                               .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = st_img[idx], .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(st_cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkBufferImageCopy r = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { st_w, st_h, 1 } };
    vkCmdCopyBufferToImage(st_cb, st_buf, st_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(st_cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
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
    sceGnmSubmitDone();
}

static int has_ext(const char *name, VkExtensionProperties *e, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

static int g_ext_host;
static VkDeviceSize g_host_align;

static int vk_init(void)
{
    const char *iext[2] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME };
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "ocarina-probe", .apiVersion = VK_API_VERSION_1_2 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                                 .enabledExtensionCount = 2, .ppEnabledExtensionNames = iext };
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) return 0;
    uint32_t n = 1;
    if (vkEnumeratePhysicalDevices(inst, &n, &phys) < 0 || !n) return 0;
    vkGetPhysicalDeviceMemoryProperties(phys, &memprops);
    uint32_t nde = 0;
    vkEnumerateDeviceExtensionProperties(phys, NULL, &nde, NULL);
    VkExtensionProperties *de = calloc(nde ? nde : 1, sizeof(*de));
    vkEnumerateDeviceExtensionProperties(phys, NULL, &nde, de);
    g_ext_host = has_ext(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME, de, nde);
    say("probe: Vulkan: %u extensões de dispositivo; VK_EXT_external_memory_host %s", nde, g_ext_host ? "SIM" : "NÃO");
    free(de);
    if (g_ext_host) {
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT };
        VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &hp };
        vkGetPhysicalDeviceProperties2(phys, &p2);
        g_host_align = hp.minImportedHostPointerAlignment;
        say("probe: minImportedHostPointerAlignment %llu", (unsigned long long)g_host_align);
    }
    uint32_t nqf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nqf, NULL);
    VkQueueFamilyProperties qfp[8];
    if (nqf > 8) nqf = 8;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nqf, qfp);
    for (qfam = 0; qfam < nqf && !(qfp[qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT); qfam++) {}
    const char *dext[2] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME };
    float prio = 1;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfam, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                               .enabledExtensionCount = g_ext_host ? 2 : 1, .ppEnabledExtensionNames = dext };
    if (vkCreateDevice(phys, &dci, NULL, &dev) != VK_SUCCESS) return 0;
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = qfam };
    vkCreateCommandPool(dev, &cpi, NULL, &cpool);
    VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cpool, .commandBufferCount = 1 };
    vkAllocateCommandBuffers(dev, &ai, &st_cb);
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    vkCreateFence(dev, &fci, NULL, &st_fence);
    VkSemaphoreCreateInfo sei = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    vkCreateSemaphore(dev, &sei, NULL, &st_acq);
    vkCreateSemaphore(dev, &sei, NULL, &st_done);

    /* tela */
    VkHeadlessSurfaceCreateInfoEXT hs = { .sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT };
    PFN_vkCreateHeadlessSurfaceEXT mk = (PFN_vkCreateHeadlessSurfaceEXT)vkGetInstanceProcAddr(inst, "vkCreateHeadlessSurfaceEXT");
    VkSurfaceCapabilitiesKHR caps;
    if (!mk || mk(inst, &hs, NULL, &st_surface) != VK_SUCCESS ||
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, st_surface, &caps) != VK_SUCCESS) return 1;
    uint32_t nf = 8;
    VkSurfaceFormatKHR fm[8];
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, st_surface, &nf, fm);
    st_bgra = fm[0].format == VK_FORMAT_B8G8R8A8_UNORM || fm[0].format == VK_FORMAT_B8G8R8A8_SRGB;
    st_w = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent.width : 1920;
    st_h = caps.currentExtent.height != UINT32_MAX ? caps.currentExtent.height : 1080;
    VkSwapchainCreateInfoKHR sci = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = st_surface,
                                     .minImageCount = caps.minImageCount < 2 ? 2 : caps.minImageCount, .imageFormat = fm[0].format,
                                     .imageColorSpace = fm[0].colorSpace, .imageExtent = { st_w, st_h }, .imageArrayLayers = 1,
                                     .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                     .preTransform = caps.currentTransform, .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                     .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE };
    if (vkCreateSwapchainKHR(dev, &sci, NULL, &st_swap) != VK_SUCCESS) return 1;
    uint32_t ni = 8;
    vkGetSwapchainImagesKHR(dev, st_swap, &ni, st_img);
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)st_w * st_h * 4, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
    vkCreateBuffer(dev, &bci, NULL, &st_buf);
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, st_buf, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                 .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    if (vkAllocateMemory(dev, &mai, NULL, &st_mem) != VK_SUCCESS) return 1;
    vkBindBufferMemory(dev, st_buf, st_mem, 0);
    vkMapMemory(dev, st_mem, 0, VK_WHOLE_SIZE, 0, (void **)&st_px);
    st_on = 1;
    return 1;
}

/* ------------------------------------------------------------------------------------- tratador de falta */
extern jmp_buf lift_jb;
extern volatile int lift_active;
static volatile uintptr_t ww_lo, ww_hi;
static volatile int ww_hits;

static void on_fault(int sig, siginfo_t *si, void *uc)
{
    (void)uc;
    uintptr_t addr = *(uintptr_t *)((char *)si + 24);   /* si_addr no siginfo do FreeBSD (offset 24, não o 16 do musl) */
    if (addr >= ww_lo && addr < ww_hi) {
        sceKernelMprotect((void *)(addr & ~(KPAGE - 1)), KPAGE, PROT_CPU_R | PROT_CPU_W | PROT_GPU_R | PROT_GPU_W);
        ww_hits++;
        return;
    }
    if (lift_active) longjmp(lift_jb, 1);
    say("probe: falta fora de qualquer teste: sinal %d endereço %#lx", sig, (unsigned long)addr);
    for (;;) { sceGnmSubmitDone(); usleep(100000); }
}

/* ------------------------------------------------------------------------------------- provas */
static int reserve_exact(uint64_t at, uint64_t size)
{
    /* fixo e sem sobrescrever (MAP_FIXED | SCE_KERNEL_MAP_NO_OVERWRITE): falha limpo se já houver algo lá */
    void *a = (void *)at;
    if (sceKernelReserveVirtualRange(&a, size, K_MAP_FIXED | 0x80, 0) == 0 && (uint64_t)a == at) return 1;
    /* sem suporte à combinação: dica de endereço, aceita só se vier exatamente nele */
    a = (void *)at;
    if (sceKernelReserveVirtualRange(&a, size, 0, 0) != 0) return 0;
    if ((uint64_t)a == at) return 1;
    sceKernelMunmap(a, size);
    return 0;
}

static uint64_t g_low, g_alias;

static int t_memory(void)
{
    size_t dm = sceKernelGetDirectMemorySize(), flex = 0;
    int64_t st = 0;
    size_t avail = 0;
    sceKernelAvailableDirectMemorySize(0, (int64_t)dm, 0, &st, &avail);
    sceKernelAvailableFlexibleMemorySize(&flex);
    say("probe: memória direta total %.2f GiB, maior bloco livre %.2f GiB; flexível livre %.0f MiB",
        dm / (double)GiB, avail / (double)GiB, flex / (double)MiB);
    /* aloca em blocos de 256 MiB até falhar, mapeia com acesso da GPU e toca uma vez por página */
    enum { MAXB = 64 };
    int64_t off[MAXB];
    void *va[MAXB];
    int n = 0;
    for (; n < MAXB; n++) {
        if (sceKernelAllocateDirectMemory(0, (int64_t)dm, 256 * MiB, 2 * MiB, WB_ONION, &off[n]) != 0) break;
        va[n] = NULL;
        if (sceKernelMapDirectMemory(&va[n], 256 * MiB, PROT_CPU_R | PROT_CPU_W | PROT_GPU_R | PROT_GPU_W, 0, off[n], 2 * MiB) != 0) {
            sceKernelReleaseDirectMemory(off[n], 256 * MiB);
            break;
        }
        for (uint64_t p = 0; p < 256 * MiB; p += KPAGE) ((volatile uint8_t *)va[n])[p] = (uint8_t)p;
        sceGnmSubmitDone();   /* sem tela aqui: a memória está sendo esgotada de propósito */
    }
    double direct = n * 256.0 / 1024.0;
    for (int i = 0; i < n; i++) { sceKernelMunmap(va[i], 256 * MiB); sceKernelReleaseDirectMemory(off[i], 256 * MiB); }
    int m = 0;
    void *fv[64];
    for (; m < 64; m++) {
        fv[m] = NULL;
        if (sceKernelMapFlexibleMemory(&fv[m], 16 * MiB, PROT_CPU_R | PROT_CPU_W, 0) != 0) break;
        memset(fv[m], 1, 16 * MiB);
    }
    for (int i = 0; i < m; i++) sceKernelMunmap(fv[i], 16 * MiB);
    say("probe: RESULTADO memória: %.2f GiB de memória direta alocados e tocados (blocos de 256 MiB, CPU+GPU); "
        "%d MiB de memória flexível", direct, m * 16);
    if (g_sum) fprintf(g_sum, "memoria_direta_gib=%.2f\nmemoria_flexivel_mib=%d\n", direct, m * 16);
    return direct >= 4.0;
}

static int t_address(void)
{
    /* o mapa do processo abaixo de 2^39, para saber o que já está ocupado */
    FILE *mf = fopen("/data/ocarina-probe/map.txt", "w");
    struct { uint64_t start, end, off; int32_t prot, type; uint8_t flags; char name[32]; } q;
    uint64_t at = 0;
    for (int i = 0; i < 400; i++) {
        memset(&q, 0, sizeof(q));
        if (sceKernelVirtualQuery((void *)at, 1, &q, sizeof(q)) != 0) break;
        if (mf) fprintf(mf, "%016llx-%016llx prot %#x tipo %d %.32s\n", (unsigned long long)q.start, (unsigned long long)q.end, q.prot, q.type, q.name);
        at = q.end;
        if (at >= (1ull << 40)) break;
    }
    if (mf) fclose(mf);
    int ok_mod = reserve_exact(MOD_BASE, MOD_SIZE), ok_k = reserve_exact(KERN_BASE, KERN_SIZE), ok_h = reserve_exact(HOSTFN_BASE, HOSTFN_SIZE);
    int ok_arena = reserve_exact(LIFT_ARENA_ADDR, LIFT_ARENA_SIZE);
    for (uint64_t a = 1 * GiB * 4; a + LOW_SIZE <= MOD_BASE && !g_low; a += GiB)
        if (a + LOW_SIZE <= LIFT_ARENA_ADDR || a >= LIFT_ARENA_ADDR + LIFT_ARENA_SIZE)
            if (reserve_exact(a, LOW_SIZE)) g_low = a;
    for (uint64_t a = (g_low + LOW_SIZE + 4 * GiB - 1) & ~(4 * GiB - 1); a + ALIAS_SIZE <= MOD_BASE && !g_alias; a += 4 * GiB)
        if (a >= LIFT_ARENA_ADDR + LIFT_ARENA_SIZE || a + ALIAS_SIZE <= LIFT_ARENA_ADDR)
            if (reserve_exact(a, ALIAS_SIZE)) g_alias = a;
    say("probe: RESULTADO endereços: módulos 0x7000000000 %s, kernel %s, funções do host %s, arena %s; "
        "16 GiB em %#llx, 64 GiB de alias em %#llx", ok_mod ? "ok" : "OCUPADO", ok_k ? "ok" : "OCUPADO", ok_h ? "ok" : "OCUPADO",
        ok_arena ? "ok" : "OCUPADO", (unsigned long long)g_low, (unsigned long long)g_alias);
    if (g_sum) fprintf(g_sum, "enderecos_modulos=%d\nenderecos_kernel=%d\nenderecos_hostfn=%d\nlow=%#llx\nalias=%#llx\n",
                       ok_mod, ok_k, ok_h, (unsigned long long)g_low, (unsigned long long)g_alias);
    return ok_mod && ok_k && ok_h && ok_arena && g_low && g_alias;
}

static void *g_commit;   /* 64 MiB de memória direta com GPU, dentro do bloco de 16 GiB */
static int64_t g_commit_off;

static int t_commit(void)
{
    if (!g_low) return 0;
    if (sceKernelAllocateDirectMemory(0, (int64_t)sceKernelGetDirectMemorySize(), 64 * MiB, 2 * MiB, WB_ONION, &g_commit_off) != 0) return 0;
    void *want = (void *)(g_low + 1 * GiB);
    void *a = want;
    int r = sceKernelMapDirectMemory(&a, 64 * MiB, PROT_CPU_R | PROT_CPU_W | PROT_GPU_R | PROT_GPU_W, K_MAP_FIXED, g_commit_off, 2 * MiB);
    if (r != 0 || a != want) { say("probe: commit fixo falhou: %#x em %p", r, a); return 0; }
    uint64_t *p = a;
    for (size_t i = 0; i < 64 * MiB / 8; i += 512) p[i] = i ^ 0xa5a5a5a5a5a5a5a5ull;
    int bad = 0;
    for (size_t i = 0; i < 64 * MiB / 8; i += 512) bad += p[i] != (i ^ 0xa5a5a5a5a5a5a5a5ull);
    /* memória flexível fixa também (heap do guest que a GPU não precisa ver) */
    void *f = (void *)(g_low + 2 * GiB);
    int rf = sceKernelMapFlexibleMemory(&f, 16 * MiB, PROT_CPU_R | PROT_CPU_W, K_MAP_FIXED);
    if (rf == 0) memset(f, 7, 16 * MiB);
    say("probe: RESULTADO commit: direta+GPU fixa em %p %s; flexível fixa em %p %s", a, bad ? "LEITURA ERRADA" : "ok", f,
        rf == 0 && f == (void *)(g_low + 2 * GiB) ? "ok" : "FALHOU");
    g_commit = a;
    return !bad && rf == 0;
}

static int t_writewatch(void)
{
    if (!g_commit) return 0;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(11, &sa, NULL);   /* SIGSEGV */
    sigaction(10, &sa, NULL);   /* SIGBUS do FreeBSD */
    uint8_t *page = (uint8_t *)g_commit + 5 * KPAGE;
    ww_lo = (uintptr_t)g_commit; ww_hi = (uintptr_t)g_commit + 64 * MiB;
    ww_hits = 0;
    int r = sceKernelMprotect(page, KPAGE, PROT_CPU_R | PROT_GPU_R | PROT_GPU_W);   /* CPU só lê */
    page[100] = 42;      /* falta -> tratador libera -> a escrita repete */
    page[200] = 43;      /* já liberada: sem falta */
    int ok = r == 0 && ww_hits == 1 && page[100] == 42 && page[200] == 43;
    ww_lo = ww_hi = 0;
    say("probe: RESULTADO write-watch: mprotect %#x, %d falta(s) (esperado 1), dados %s", r, ww_hits,
        page[100] == 42 && page[200] == 43 ? "ok" : "ERRADOS");
    if (g_sum) fprintf(g_sum, "writewatch=%d\n", ok);
    return ok;
}

static int t_hostmem(void)
{
    if (!g_ext_host || !g_commit) { say("probe: RESULTADO Vulkan host: extensão ausente"); return 0; }
    PFN_vkGetMemoryHostPointerPropertiesEXT getp = (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(dev, "vkGetMemoryHostPointerPropertiesEXT");
    void *ptr = (uint8_t *)g_commit + 32 * MiB;
    VkMemoryHostPointerPropertiesEXT hp = { .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
    VkResult r1 = getp(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, ptr, &hp);
    say("probe: vkGetMemoryHostPointerPropertiesEXT(direta+GPU) = %d, tipos 0x%x", r1, hp.memoryTypeBits);
    if (r1 != VK_SUCCESS || !hp.memoryTypeBits) return 0;
    VkExternalMemoryBufferCreateInfo eb = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT };
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &eb, .size = 4 * MiB, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    VkBuffer b;
    if (vkCreateBuffer(dev, &bci, NULL, &b) != VK_SUCCESS) return 0;
    VkImportMemoryHostPointerInfoEXT imp = { .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                             .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, .pHostPointer = ptr };
    uint32_t t = 0;
    while (!(hp.memoryTypeBits & (1u << t))) t++;
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = 4 * MiB, .memoryTypeIndex = t };
    VkDeviceMemory m;
    VkResult r2 = vkAllocateMemory(dev, &mai, NULL, &m);
    if (r2 != VK_SUCCESS) { say("probe: importação do ponteiro falhou: %d", r2); return 0; }
    vkBindBufferMemory(dev, b, m, 0);
    memset(ptr, 0, 4 * MiB);
    /* 4 MiB em quartos: a CPU escreve um padrão no 1º; a GPU preenche o 2º e o 3º com uma constante (escrita) e
     * copia o 1º para o 4º (leitura do que a CPU escreveu) */
    uint32_t *w0 = ptr;
    for (size_t i = 0; i < MiB / 4; i++) w0[i] = (uint32_t)(i * 2654435761u);
    vkResetCommandBuffer(st_cb, 0);
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(st_cb, &bi);
    vkCmdFillBuffer(st_cb, b, MiB, 2 * MiB, 0xC0FFEE11u);
    VkBufferCopy cp = { 0, 3 * MiB, MiB };
    vkCmdCopyBuffer(st_cb, b, b, 1, &cp);
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(st_cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    vkEndCommandBuffer(st_cb);
    vkResetFences(dev, 1, &st_fence);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &st_cb };
    vkQueueSubmit(queue, 1, &si, st_fence);
    vkWaitForFences(dev, 1, &st_fence, VK_TRUE, UINT64_MAX);
    uint32_t *w = ptr;
    int bad_fill = 0, bad_copy = 0;
    for (size_t i = MiB / 4; i < 3 * MiB / 4; i += 256) bad_fill += w[i] != 0xC0FFEE11u;
    for (size_t i = 0; i < MiB / 4; i += 256) bad_copy += w[3 * MiB / 4 + i] != (uint32_t)(i * 2654435761u);
    int bad = bad_fill + bad_copy;
    /* memória flexível (sem GPU) tem de ser recusada */
    void *flex = NULL;
    VkResult r3 = VK_ERROR_UNKNOWN;
    if (sceKernelMapFlexibleMemory(&flex, 2 * MiB, PROT_CPU_R | PROT_CPU_W, 0) == 0) {
        VkMemoryHostPointerPropertiesEXT fp = { .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
        VkImportMemoryHostPointerInfoEXT fi = imp;
        fi.pHostPointer = flex;
        VkMemoryAllocateInfo fa = mai;
        fa.pNext = &fi;
        fa.allocationSize = 2 * MiB;
        VkDeviceMemory fm;
        getp(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, flex, &fp);
        r3 = vkAllocateMemory(dev, &fa, NULL, &fm);
        if (r3 == VK_SUCCESS) vkFreeMemory(dev, fm, NULL);
        sceKernelMunmap(flex, 2 * MiB);
    }
    say("probe: RESULTADO Vulkan host: GPU escreveu na memória do guest %s, GPU leu o que a CPU escreveu %s; "
        "memória sem GPU recusada %s (%d)", bad_fill ? "ERRADO" : "ok", bad_copy ? "ERRADO" : "ok",
        r3 != VK_SUCCESS ? "ok" : "NÃO (aceitou)", r3);
    if (g_sum) fprintf(g_sum, "vulkan_hostmem=%d\n", !bad);
    vkFreeMemory(dev, m, NULL);
    vkDestroyBuffer(dev, b, NULL);
    return !bad;
}

int lift_run(uint8_t *base, size_t size, FILE *out, int *n_ok, int *n_fault, int *n_trap);
extern const int lift_count;

static int t_lift(void)
{
    void *a = (void *)LIFT_ARENA_ADDR;
    if (sceKernelMapFlexibleMemory(&a, LIFT_ARENA_SIZE, PROT_CPU_R | PROT_CPU_W, K_MAP_FIXED) != 0 || a != (void *)LIFT_ARENA_ADDR) {
        say("probe: arena das funções não mapeou em %#llx", (unsigned long long)LIFT_ARENA_ADDR);
        return 0;
    }
    FILE *out = fopen("/data/ocarina-probe/lift_ps4.txt", "w");
    if (!out) return 0;
    int ok, fault, trap;
    lift_run(a, LIFT_ARENA_SIZE, out, &ok, &fault, &trap);
    fclose(out);
    say("probe: RESULTADO C do lifter: %d funções do OoT rodaram na Jaguar: %d ok, %d falta de página, %d trap "
        "(comparar lift_ps4.txt com a referência do IdeaPad)", lift_count, ok, fault, trap);
    if (g_sum) fprintf(g_sum, "lift_ok=%d\nlift_falta=%d\nlift_trap=%d\n", ok, fault, trap);
    return ok > 0;
}

int main(void)
{
    mkdir("/data/ocarina-probe", 0777);
    g_sum = fopen("/data/ocarina-probe/summary.txt", "w");
    say("probe: ocarina-ps4 Fase 0, modo %s", sceKernelIsNeoMode() ? "PRO" : "base");
    if (g_sum) fprintf(g_sum, "modo=%s\n", sceKernelIsNeoMode() ? "pro" : "base");
    if (!vk_init()) say("probe: Vulkan não subiu (as provas de memória seguem)");
    st_present();
    int (*const tests[NTEST])(void) = { t_memory, t_address, t_commit, t_writewatch, t_hostmem, t_lift };
    for (int i = 0; i < NTEST; i++) {
        st_state[i] = 1;
        st_present();
        st_state[i] = tests[i]() ? 2 : 3;
        st_present();
    }
    say("probe: FIM");
    if (g_sum) { fprintf(g_sum, "fim=1\n"); fclose(g_sum); g_sum = NULL; }
    for (;;) { st_present(); usleep(500000); }
}
