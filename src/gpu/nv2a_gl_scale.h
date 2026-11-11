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
 * 1-4, or auto from the window's drawable). */
typedef struct {
    float scale;                        /* 1: off */
    int shadow;                         /* depth-only targets scale too */
    uint32_t ntargets;                  /* extra real sizes of the frame class */
    uint32_t target_w[NV2A_GL_SCALE_MAX_TARGETS], target_h[NV2A_GL_SCALE_MAX_TARGETS];
} Nv2aGlScaleConfig;

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

#endif
