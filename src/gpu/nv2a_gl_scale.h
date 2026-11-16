/* Internal resolution scaling for the GL back end (nv2a_gl.c): which render
 * targets are drawn at a multiple of their guest size, and the rectangle
 * arithmetic that goes with it. No GL here, so tests/nv2a_gl_scale checks
 * it without a context.
 *
 * A scaled target keeps its logical (guest) size for everything the title
 * sees -- positions, texture coordinates, read-back -- and only its GL
 * texture, viewport and scissor are real = round(logical * scale). */
#ifndef XBOXRECOMP_NV2A_GL_SCALE_H
#define XBOXRECOMP_NV2A_GL_SCALE_H

#include <stdint.h>
#include "nv2a_backend.h"

/* The frame the presenter shows, in guest pixels (fb_present_sdl.c's
 * FB_W x FB_H; hard-wired there too). */
#define NV2A_GL_FRAME_W 640u
#define NV2A_GL_FRAME_H 480u

#define NV2A_GL_SCALE_MAX_TARGETS 8

/* The scale itself comes from RECOMP_GPU_SCALE (nv2a_gl.c scale_from_env:
 * 1-4, or auto from the window's drawable), else from the setting every
 * console shares, RECOMP_UPSCALE (nv2a_gl_scale_choose, XBA-237). */
typedef struct {
    float scale;                        /* 1: off */
    int shadow;                         /* depth-only targets scale too */
    uint32_t ntargets;                  /* extra real sizes of the frame class */
    uint32_t target_w[NV2A_GL_SCALE_MAX_TARGETS], target_h[NV2A_GL_SCALE_MAX_TARGETS];
    int no_sticky;                      /* RECOMP_GPU_SCALE_STICKY=0: policy only */
} Nv2aGlScaleConfig;

/* The render target the back end already holds for a colour surface's
 * memory (same address, same bytes per pixel): what nv2a_gl_target_scale
 * needs of it. */
typedef struct {
    float sc;                           /* its scale */
    uint32_t pitch;                     /* bytes per row of its linear layout */
    int swizzled;                       /* last bound swizzled */
} Nv2aGlHeldTarget;

/* RECOMP_GPU_SCALE_TARGETS: "WxH[,WxH...]" real sizes added to the frame
 * class. Returns the number parsed (stored in cfg, at most
 * NV2A_GL_SCALE_MAX_TARGETS); malformed entries are skipped. */
uint32_t nv2a_gl_scale_parse_targets(const char *s, Nv2aGlScaleConfig *cfg);

/* A colour surface of the frame-buffer class: linear (not swizzled), and
 * its real size (clip origin + size, real pixels) is the frame's, or the
 * frame's times its anti-aliasing factor. Fable binds its back buffer both
 * as 0x121 (640x480) and 0x2121 (AA 2x2: 320x240 logical, the same 640x480
 * real pixels): both are frame buffers. */
int nv2a_gl_frame_surface(const Nv2aSurface *s);

/* The scale of the target surface s draws into, decided from its colour
 * surface: frame class (or a RECOMP_GPU_SCALE_TARGETS size) -> cfg->scale,
 * everything else (swizzled and power-of-two effect targets) -> 1; no colour
 * surface (a depth-only pass, a shadow map) -> cfg->scale with cfg->shadow,
 * else 1. A batch with every colour channel masked keeps its colour
 * surface's scale. */
float nv2a_gl_surface_scale(const Nv2aGlScaleConfig *cfg, const Nv2aSurface *s);

/* The scale a batch on surface s draws at, given the target already held
 * for s's memory (NULL: none). The policy above, except that the scale of
 * a scaled target sticks to its memory: a linear surface on it with the
 * same pitch (and bytes per pixel, as `held` implies) keeps it whatever its
 * clip. Burnout 3 binds its 640x480 back buffer with clip 0,0 640x480 for
 * its post-process quads and 0,16 640x448 for its HUD (menus: other
 * scissor rectangles, 160,16 480x447); keyed on the clip alone the target
 * went 1 -> 2 -> 1 twice a frame, each time back through guest memory.
 * Never down: a native target bound as frame class is promoted (re-created
 * once) and then stays. Swizzled surfaces, another pitch and depth-only
 * passes follow the policy. Every caller (draw, depth-only, target
 * creation) asks this, so draw, depth and target agree. */
float nv2a_gl_target_scale(const Nv2aGlScaleConfig *cfg, const Nv2aSurface *s,
                           const Nv2aGlHeldTarget *held);

/* Which internal resolution the Xbox draws at (XBA-237), from the values
 * of RECOMP_GPU_SCALE (gpu_scale: the Xbox's own, a decimal 1-4 or "auto")
 * and RECOMP_UPSCALE (upscale: Settings > Graphics > Internal resolution,
 * shared with the PS1 and the N64: 1, 2, 4 or 8), either NULL or empty when
 * unset. The Xbox's own wins when set, as RECOMP_N64_UPSCALE does over the
 * shared one; the shared one is held to 1-4 (8, the N64 Vulkan renderer's,
 * draws at 4). *key is the variable that decided (NULL: neither set).
 * Returns: NV2A_GL_SCALE_FIXED (*scale set), _AUTO (fit the drawable;
 * RECOMP_GPU_SCALE only), _UNSET (*scale 1) or _BAD (a value of neither
 * form; *scale 1). No GL: GL_MAX_TEXTURE_SIZE is the caller's. */
enum { NV2A_GL_SCALE_UNSET, NV2A_GL_SCALE_FIXED, NV2A_GL_SCALE_AUTO, NV2A_GL_SCALE_BAD };
int nv2a_gl_scale_choose(const char *gpu_scale, const char *upscale, float *scale,
                         const char **key);

/* round(v * sc). */
uint32_t nv2a_gl_scale_px(uint32_t v, float sc);

/* A logical rectangle in real pixels: both edges rounded, so adjacent
 * rectangles stay adjacent (out: x, y, w, h). */
void nv2a_gl_scale_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, float sc,
                        int32_t out[4]);

/* A visibility count measured at scale sc in the units of the guest's
 * pixels: samples / sc^2, rounded, and at least 1 when any sample passed
 * (a visible object stays visible). */
uint32_t nv2a_gl_scale_count(uint32_t samples, float sc);

/* The SDL window reads a guest frame as 640 pixels a row of pitch / 640
 * bytes each (fb_present_sdl.c): a presented surface with another pitch --
 * an anti-aliased back buffer, 1280 samples a row -- shows as garbage or
 * not at all through guest memory, on either rasteriser. 1: the GL back
 * end hands such a surface over as a host frame while a window shows
 * frames. */
int nv2a_gl_present_needs_host(const Nv2aPresentSurface *s);

/* The anti-aliasing factors of a presented surface whose format has an
 * anti-aliasing mode (bits 12-15): 2 along x when it is 1280 real pixels
 * wide (clip origin + size: the 640 frame's samples, AA
 * 2x1 and 2x2), 2 along y when 960 high, else 1. Its host frame is
 * resolved by them (a box filter over the samples). */
void nv2a_gl_aa_factors(const Nv2aPresentSurface *s, uint32_t *fx, uint32_t *fy);

#endif
