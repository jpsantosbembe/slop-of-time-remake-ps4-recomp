// ocarina-ps4: a "janela" do PS4 (gfx_wsys.h). Não há janela: a superfície é VK_EXT_headless_surface, que o mesa-ps4
// apresenta pela saída de vídeo (sceVideoOutSubmitFlip), e a área de desenho é a resolução da saída. O swapchain e o
// present continuam em gfx_present.c.
//
// sceGnmSubmitDone: o PS4 exige que o processo que submete trabalho à GPU chame sceGnmSubmitDone() com regularidade;
// sem isso, ao suspender ou fechar o app, o sistema derruba o processo com a exceção 0xa0d0c00a
// (CPU_FAULT_SUBMITDONE_TIMEOUT_IN_SUSPEND_ASYNC) e a tela de erro (medido na Fase 0, bench/ e probe/). Ela é chamada a
// cada present (wsys_presented, de gfx_present.c) e por uma thread ociosa a cada 100 ms quando nada foi apresentado
// nesse intervalo (filme, carregamento, modo de teste parado), sempre sob o lock da fila do Vulkan, para não cruzar com
// uma submissão do RADV.
//
// OCARINA_PS4_RES=<w>x<h> muda a resolução da saída (padrão 1920x1080; o PS4 Pro pode pedir 3840x2160).
#ifdef __ORBIS__
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "gfx_wsys.h"
#include "host.h"
#include "ui/menu.h"
#include "vk.h"
#include "platform/platform.h"

extern int32_t sceGnmSubmitDone(void);

static int g_w = 1920, g_h = 1080;
static int g_open;
static volatile uint64_t g_last_done_ns;   // plat_now_ns do último sceGnmSubmitDone

static void submit_done(void) {
    vk_queue_lock();
    sceGnmSubmitDone();
    vk_queue_unlock();
    __atomic_store_n(&g_last_done_ns, plat_now_ns(), __ATOMIC_RELEASE);
}

void wsys_presented(void) { submit_done(); }

static void idle_main(void *arg) {
    (void)arg;
    for (;;) {
        plat_sleep_ms(100);
        uint64_t last = __atomic_load_n(&g_last_done_ns, __ATOMIC_ACQUIRE);
        if (plat_now_ns() - last >= 100000000ull) submit_done();
    }
}

int gfx_window_vk_extensions(const char **out, int max) {
    if (max < 2) return 0;
    out[0] = VK_KHR_SURFACE_EXTENSION_NAME;
    out[1] = VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME;
    return 2;
}

// A superfície headless apresenta de qualquer fila.
int gfx_window_vk_presentable(VkInstance inst, VkPhysicalDevice pd, uint32_t family) {
    (void)inst, (void)pd, (void)family;
    return 1;
}

int wsys_open(void) {
    if (g_open) return 1;
    char res[32];
    int w = 0, h = 0;
    if (plat_getenv("OCARINA_PS4_RES", res, sizeof res)) {
        if (sscanf(res, "%dx%d", &w, &h) == 2 && w >= 640 && h >= 360 && w <= 3840 && h <= 2160) g_w = w, g_h = h;
        else rt_log("gfx: OCARINA_PS4_RES=%s não é <w>x<h> entre 640x360 e 3840x2160; fica %dx%d", res, g_w, g_h);
    }
    gfx_window_set_refresh(60.0);
    __atomic_store_n(&g_last_done_ns, plat_now_ns(), __ATOMIC_RELEASE);
    if (!plat_thread_spawn(idle_main, NULL, "gnm submitdone", 256 << 10)) rt_log("gfx: sem a thread do sceGnmSubmitDone");
    g_open = 1;
    rt_log("gfx: saída de vídeo do PS4 %dx%d (VK_EXT_headless_surface)", g_w, g_h);
    {
        // A splash do PKG (sce_sys/pic0/pic1) fica na frente de tudo até o app pedir para tirá-la; sem isto, o jogo desenha
        // atrás dela e a TV só mostra a splash (a sonda nunca viu isso porque o PKG dela não tinha splash).
        extern int32_t sceSystemServiceHideSplashScreen(void);
        int32_t r = sceSystemServiceHideSplashScreen();
        rt_log("gfx: splash do sistema escondida (%#x)", (unsigned)r);
    }
    return 1;
}

VkSurfaceKHR wsys_surface(VkInstance inst) {
    VkHeadlessSurfaceCreateInfoEXT ci = {.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
    PFN_vkCreateHeadlessSurfaceEXT mk =
        (PFN_vkCreateHeadlessSurfaceEXT)vkGetInstanceProcAddr(inst, "vkCreateHeadlessSurfaceEXT");
    VkSurfaceKHR s = VK_NULL_HANDLE;
    if (!mk) rt_log("gfx: o driver não tem vkCreateHeadlessSurfaceEXT");
    else if (mk(inst, &ci, NULL, &s) != VK_SUCCESS) rt_log("gfx: vkCreateHeadlessSurfaceEXT falhou");
    return s;
}

void wsys_client_size(int *w, int *h) {
    *w = g_open ? g_w : 0;
    *h = g_open ? g_h : 0;
}

int wsys_minimized(void) { return 0; }

// A saída tem o tamanho dela: um redimensionamento só refaz o swapchain no mesmo tamanho.
void wsys_resize(int w, int h) {
    (void)w, (void)h;
    gfx_window_mark_dirty();
}

// --probe-window não roda no PS4 (foco, cursor e redimensionamento são de janela); --probe-orbis é o teste daqui.
void wsys_probe_focus(int in) { gfx_window_focus_changed(in); }
void wsys_probe_escape(void) {}
int wsys_probe_click(int where) { (void)where; return 0; }

// ---- ui/menu.h
void gfx_window_apply_settings(void) {}   // tamanho e tela cheia não se aplicam à saída de vídeo
// "Sair do jogo" no menu: o mesmo fim que fechar a janela no PC (salva e encerra o processo).
void gfx_window_request_close(void) { gfx_window_closed(); }
int gfx_window_monitor_size(int *w, int *h) {
    *w = g_w, *h = g_h;
    return 1;
}
unsigned gfx_window_dpi(void) { return 96; }
#endif
