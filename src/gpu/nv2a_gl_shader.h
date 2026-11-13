/**
 * GLSL for the GL back end (nv2a_gl.c): the NV2A's register combiners and
 * texture stages as a fragment shader, generated per pipeline state.
 *
 * The generator mirrors the CPU rasteriser rule for rule -- nv2a_rc_eval()
 * in src/kernel/nv2a_combiner.c for the combiners, rc_stage_fetch() and the
 * shading branches of xf_rows() in nv2a_pb_exec.c for texture stages,
 * alpha test and the two non-combiner colour rules -- so that both paths
 * draw the same picture. When one changes, change the other.
 *
 * What varies per pipeline (the key) is everything that changes the code:
 * combiner words, stage count and flags, stage modes, which stages have a
 * texture and of which kind, the alpha-test function. Constants (combiner
 * factors, fog colour, alpha reference, texture coordinate scales) are
 * uniforms, so a title that animates a factor does not compile a shader per
 * frame.
 *
 * The text is GLSL that compiles both as "#version 330 core" (desktop GL
 * 3.3+, macOS 4.1 core) and "#version 300 es" (GLES 3.0, Android): the
 * caller prepends nv2a_gl_glsl_header(). No GL calls here, so the generator
 * is testable without a context (tests/nv2a_gl_shader).
 */
#ifndef XBOXRECOMP_NV2A_GL_SHADER_H
#define XBOXRECOMP_NV2A_GL_SHADER_H

#include <stddef.h>
#include <stdint.h>
#include "nv2a_backend.h"

/* Texture kinds a stage can sample as. */
enum {
    NV2A_GL_TEX_NONE  = 0,   /* no usable texture: a fetch reads white */
    NV2A_GL_TEX_2D    = 1,
    NV2A_GL_TEX_CUBE  = 2,
    /* A depth texture (shadow map) sampled with a depth compare
     * (sampler2DShadow): the stage reads the comparison's result. 16 and 24
     * give the depth format's range (Y16: 0xFFFF, X8_Y24: 0xFFFFFF), the
     * units the stage's r/q is in. */
    NV2A_GL_TEX_SHADOW16 = 3,
    NV2A_GL_TEX_SHADOW24 = 4,
    /* A volume texture (sampler3D, XBA-120): PROJECT2D/3D stages sample at
     * (s/q, t/q, r/q). */
    NV2A_GL_TEX_3D       = 5
};

typedef struct {
    uint32_t shade;                       /* NV2A_SHADE_* */
    uint32_t stages;                      /* combiner stage count, 0..8 */
    uint32_t flags;                       /* COMBINER_CONTROL >> 8 */
    uint32_t color_icw[8], alpha_icw[8], color_ocw[8], alpha_ocw[8];
    uint32_t final0, final1;
    uint32_t stage_program;               /* SHADER_STAGE_PROGRAM */
    uint32_t clip_plane_mode;             /* only when a stage clips */
    uint32_t tex_kind[4];                 /* NV2A_GL_TEX_* */
    uint32_t alpha_func;                  /* 0: alpha test off */
    /* Bump-env stages (modes 6, 7; stages 1-3): bump_src[st] is 0x10 | the
     * input stage. bump_fetch[src] says how that input stage's dU/dV/L are
     * read: NV2A_GL_DSDT_TEXEL set, texel by texel with texelFetch, each
     * texel's bytes made signed before filtering (what the hardware does;
     * filtering unsigned bytes first turns 0x01 next to 0xFF into -1), with
     * _LINEAR a bilinear blend of four, _WRAP_U/_V wrapping rather than
     * clamping at the edge; 0, from the filtered texel (xemu's way). */
    uint32_t bump_src[4];
    uint32_t bump_fetch[4];
    /* A point batch with point sprites on (SET_POINT_SMOOTH_ENABLE, as xemu
     * reads it): every stage samples at the position inside the point. Set
     * by the caller after nv2a_gl_shader_key (the batch, not the pixel
     * state, knows it). */
    uint32_t point_sprite;
    /* SET_TEXTURE_FILTER's signed channels per combiner stage (bits 0-3:
     * A, R, G, B), read as two's complement over 127 after the fetch
     * (nv_s8); not for a stage a bump-env stage reads (it signs dU/dV
     * itself). */
    uint32_t tex_signed[4];
} Nv2aGlShaderKey;

enum {
    NV2A_GL_DSDT_TEXEL  = 0x1,
    NV2A_GL_DSDT_LINEAR = 0x2,
    NV2A_GL_DSDT_WRAP_U = 0x4,
    NV2A_GL_DSDT_WRAP_V = 0x8
};

/* Fill the key for one batch. Unused words are zeroed so equal states give
 * equal keys byte for byte (memcmp, hashing). */
void nv2a_gl_shader_key(const Nv2aPixelState *ps, const Nv2aRenderState *rs,
                        const uint32_t tex_kind[4], Nv2aGlShaderKey *key);

uint64_t nv2a_gl_shader_key_hash(const Nv2aGlShaderKey *key);

/* "#version 330 core\n" or "#version 300 es\n" plus precision lines. */
const char *nv2a_gl_glsl_header(int gles);

/* The vertex shader (one for every pipeline). Attributes: a_pos (location
 * 0), a_d0 1, a_d1 2, a_fog 3, a_t0..a_t3 4..7, laid out as Nv2aVertexFull.
 * Uniform u_vp = (2/width, 2/height, 2/zmax, 0) maps surface pixels and
 * window z to clip space. */
const char *nv2a_gl_vertex_body(void);

/* Write the fragment shader body for `key` into buf (without the header).
 * Returns the length, or 0 if it did not fit. Uniforms it declares:
 *   sampler2D/samplerCube/sampler2DShadow u_t0..u_t3, vec2 u_tscale[4],
 *   vec4 u_c0[8], u_c1[8], u_fc0, u_fc1, u_fogc, float u_aref,
 *   vec4 u_bm[4] (bump-env matrix per stage: m00, m01, m10, m11) and
 *   vec2 u_bso[4] (bump-env luminance scale, offset per stage). */
size_t nv2a_gl_fragment_body(const Nv2aGlShaderKey *key, char *buf, size_t cap);

#endif
