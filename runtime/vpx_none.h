// ocarina-ps4: o build sem libvpx (RT_NO_VPX, o alvo PS4 do tools/build_game.py). Os filmes do OoT são AV1 e passam
// pelo dav1d (ov_codec.c); o VP9 de ov_codec.c e ov_movie.c (TotK, Donkey Kong Bananza) fica sem decodificador. Este
// header põe no lugar de <vpx/vp8dx.h> e <vpx/vpx_decoder.h> só o pedaço da API que os dois usam, com um decodificador
// que nunca abre: vpx_codec_dec_init falha, então ov_codec.c deixa as imagens pretas e ov_movie.c responde
// "não suportado" ao Configure, como fariam com uma libvpx quebrada. Sem libvpx nenhuma no executável.
#pragma once
#include <stddef.h>

typedef enum { VPX_CODEC_OK = 0, VPX_CODEC_ERROR = 1, VPX_CODEC_INCAPABLE = 4 } vpx_codec_err_t;
typedef enum { VPX_IMG_FMT_NONE = 0, VPX_IMG_FMT_I420 = 0x102 } vpx_img_fmt_t;
enum { VPX_PLANE_Y = 0, VPX_PLANE_U = 1, VPX_PLANE_V = 2 };
enum { VP9D_SET_ROW_MT = 1 };

typedef struct { int unused; } vpx_codec_ctx_t;
typedef struct { unsigned threads, w, h; } vpx_codec_dec_cfg_t;
typedef const void *vpx_codec_iter_t;
typedef struct vpx_image {
    vpx_img_fmt_t fmt;
    unsigned int d_w, d_h;
    unsigned char *planes[4];
    int stride[4];
} vpx_image_t;

static inline const void *vpx_codec_vp9_dx(void) { return NULL; }
static inline vpx_codec_err_t vpx_codec_dec_init(vpx_codec_ctx_t *ctx, const void *iface, const vpx_codec_dec_cfg_t *cfg,
                                                 long flags) {
    (void)ctx, (void)iface, (void)cfg, (void)flags;
    return VPX_CODEC_INCAPABLE;
}
static inline vpx_codec_err_t vpx_codec_decode(vpx_codec_ctx_t *ctx, const unsigned char *data, unsigned size,
                                               void *user, long deadline) {
    (void)ctx, (void)data, (void)size, (void)user, (void)deadline;
    return VPX_CODEC_INCAPABLE;
}
static inline vpx_image_t *vpx_codec_get_frame(vpx_codec_ctx_t *ctx, vpx_codec_iter_t *iter) {
    (void)ctx, (void)iter;
    return NULL;
}
static inline vpx_codec_err_t vpx_codec_destroy(vpx_codec_ctx_t *ctx) { (void)ctx; return VPX_CODEC_OK; }
static inline vpx_codec_err_t vpx_codec_control(vpx_codec_ctx_t *ctx, int id, int v) {
    (void)ctx, (void)id, (void)v;
    return VPX_CODEC_INCAPABLE;
}
static inline const char *vpx_codec_error_detail(const vpx_codec_ctx_t *ctx) { (void)ctx; return "build sem libvpx"; }
static inline const char *vpx_codec_err_to_string(vpx_codec_err_t e) { (void)e; return "build sem libvpx"; }
static inline const char *vpx_codec_version_str(void) { return "ausente (RT_NO_VPX)"; }
