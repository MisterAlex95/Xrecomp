/**
 * GPU render back end for the pushbuffer executor: OpenGL 3.3 core / 4.1
 * core (macOS) / GLES 3.0 (Android), one code path. XBA-27 stage 7b.
 *
 * The executor still decodes the pushbuffer; what moves to the GPU is
 * everything per pixel -- rasterisation, texturing, the register combiners,
 * alpha test, depth, stencil and blending -- which is where the software
 * renderer spends its time, and vertex programs (7c-lite).
 *
 *   vertex programs one GLSL vertex shader per program (nv2a_gl_vsh.c) on the
 *                   attributes the executor gathers per unique vertex
 *                   (gl_draw_program); fixed-function and screen-space
 *                   batches still arrive as CPU-transformed triangles.
 *

 *   render targets  one RGBA8 texture + FBO per colour surface, created from
 *                   the guest bytes; depth/stencil textures per zeta
 *                   surface (sampled with a compare as shadow maps). A texture over a target samples the target
 *                   directly. Pixels go back to guest memory only when
 *                   someone else needs them: the presented surface at the
 *                   flip (the presenter and RECOMP_FB_DUMP read guest
 *                   memory; one frame late through a pixel pack buffer),
 *                   and a target a batch samples in a form the GPU cannot
 *                   (gl_present lists the rules).
 *   textures        uploaded from guest memory, cached by address, format and
 *                   size, re-uploaded when a hash of their bytes changes.
 *                   DXT goes up compressed where S3TC exists, everything else
 *                   is decoded once on the CPU by the executor's own sampler
 *                   (nv2a_backend_decode_texture), so formats cannot drift.
 *   pixel shading   one fragment shader per pipeline state, generated from
 *                   the combiner program (nv2a_gl_shader.c), cached by key.
 *   visibility      occlusion queries around batches drawn while the title
 *                   counts (exact counts on desktop GL, "any" on GLES).
 *
 * Threading: every call arrives on the executor thread (nv2a_backend.h).
 * The GL context is created there, headless (nv2a_gl_context.c), on the
 * first call and stays current on that thread; no other thread touches GL.
 * The SDL presenter keeps its own renderer on the main thread and is fed
 * exactly as with the CPU rasteriser -- through guest memory, after the
 * present hook below (gl_present) has read the frame back. Presenting the FBO texture
 * directly (a context shared with the presenter) is stage 7e.
 *
 * Selected with RECOMP_GPU=1 (nv2a_gl_install_from_env, called by the
 * presenter's xbox_HostWindowMain); the CPU rasteriser stays the default.
 * If no context can be created the back end unregisters itself and the
 * CPU rasteriser draws.
 */
#include "nv2a_gl.h"
#include "nv2a_gl_api.h"
#include "nv2a_gl_shader.h"
#include "nv2a_gl_vsh.h"
#include "nv2a_backend.h"
#include "d3d/d3d8_swizzle.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);

static int s_init;                 /* 0 not tried, 1 ready, -1 failed */
static int s_s3tc, s_depth_clamp, s_verbose;
static int s_border_clamp;         /* GL_CLAMP_TO_BORDER usable */
static int s_bilinear;             /* RECOMP_TEX_BILINEAR / _ANISO, as the CPU path */
static int s_recheck_frame;        /* RECOMP_GPU_TEX_RECHECK=frame */
static int s_recheck_switch;       /* RECOMP_GPU_TEX_RECHECK=switch: the old policy */
static int s_cull = 1;             /* RECOMP_GPU_CULL (default on) */
static uint32_t s_frame;
static GLuint s_vao, s_vbo, s_vs;

static struct {
    uint32_t draws, tris, clears, uploads, upload_kb, readbacks, compiles, hashes_kb;
    uint32_t vsh_draws, vsh_kb;     /* vertex-program batches, their upload */
    uint32_t rt_creates, relayouts, rt_direct, rt_copies, rt_grows, shadow_binds;
    uint32_t seeds, seeds_skipped, invalidates;
    uint32_t bump_draws;            /* batches with a bump-env stage */
    uint32_t target_switches;       /* draw/clear framebuffer changes (bind_target) */
    uint32_t frames;                /* flips since the line was last printed */
} s_st;

/* RECOMP_GPU_RT_RELAYOUT_READBACK=1: read a target back when the title
 * switches it between swizzled and linear (the old behaviour). */
static int s_readback_layout_switch;
/* RECOMP_GPU_RT_DIRECT=0: never sample a colour target directly (read it
 * back into guest memory and upload it as a texture, as before 7d). */
static int s_rt_direct_off;
/* glInvalidateSubFramebuffer usable: GLES 3.0, desktop GL 4.3 or
 * GL_ARB_invalidate_subdata (not macOS's 4.1). RECOMP_GPU_INVALIDATE=0
 * turns it off. */
static int s_invalidate;

static void nv2a_gl_invalidate_rect(GLsizei n, const GLenum *att, GLint x, GLint y,
                                    GLsizei w, GLsizei h)
{
#if NV2A_GL_ES || !defined(__APPLE__)
    glInvalidateSubFramebuffer(GL_FRAMEBUFFER, n, att, x, y, w, h);
#else
    (void)n; (void)att; (void)x; (void)y; (void)w; (void)h;
#endif
}
/* RECOMP_GPU_PRESENT=sync: read the presented surface back at its own flip
 * (the executor waits for the GPU to finish the frame); default async: one
 * frame later through a pixel pack buffer. RECOMP_GPU_READBACK=all: also
 * every other target drawn since the last flip, as before 7d (for a title
 * whose CPU reads its render targets). */
static int s_present_sync_mode, s_readback_all;

/* ---- helpers ------------------------------------------------------------ */

static uint8_t *guest(uint32_t va)
{
    return (uint8_t *)xbox_GetMemoryOffset() + va;
}

static void argb_to_rgba(uint32_t c, uint8_t *o)
{
    o[0] = (uint8_t)(c >> 16);
    o[1] = (uint8_t)(c >> 8);
    o[2] = (uint8_t)c;
    o[3] = (uint8_t)(c >> 24);
}

static void unpack4(uint32_t argb, float o[4])
{
    o[0] = (float)((argb >> 16) & 0xFF) / 255.0f;
    o[1] = (float)((argb >> 8) & 0xFF) / 255.0f;
    o[2] = (float)(argb & 0xFF) / 255.0f;
    o[3] = (float)(argb >> 24) / 255.0f;
}

/* 64-bit hash of guest bytes, eight at a time: textures are re-uploaded when
 * this changes. Not cryptographic; a collision costs a stale texture. */
static uint64_t hash_bytes(const uint8_t *p, size_t n)
{
    uint64_t h = 0x9E3779B97F4A7C15ull ^ n, w;
    size_t i;

    for (i = 0; i + 8 <= n; i += 8) {
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 29;
    }
    for (; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    s_st.hashes_kb += (uint32_t)(n >> 10);
    return h;
}

static int has_ext(const char *name)
{
    GLint n = 0, i;

    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (i = 0; i < n; i++) {
        const char *e = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
        if (e && strcmp(e, name) == 0)
            return 1;
    }
    return 0;
}

static int env_on(const char *name)
{
    const char *e = getenv(name);
    return e && *e && strcmp(e, "0") != 0;
}

static GLuint compile(GLenum type, const char *body)
{
    const char *src[2];
    GLuint sh = glCreateShader(type);
    GLint ok = 0;

    src[0] = nv2a_gl_glsl_header(NV2A_GL_ES);
    src[1] = body;
    glShaderSource(sh, 2, src, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(sh, sizeof log, NULL, log);
        fprintf(stderr, "  [GPUGL] shader compile failed:\n%s\n--- source ---\n%s\n",
                log, body);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

/* ---- set-up ------------------------------------------------------------- */

static void unregister_self(void);

static int ensure_init(void)
{
    if (s_init)
        return s_init > 0;
    s_init = -1;
    if (!nv2a_gl_context_create()) {
        fprintf(stderr, "  [GPUGL] no GL context: falling back to the CPU rasteriser\n");
        unregister_self();
        return 0;
    }
    fprintf(stderr, "  [GPUGL] %s | %s | GLSL %s\n",
            (const char *)glGetString(GL_RENDERER),
            (const char *)glGetString(GL_VERSION),
            (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION));
    s_s3tc = has_ext("GL_EXT_texture_compression_s3tc")
          && !(getenv("RECOMP_GPU_S3TC") && !env_on("RECOMP_GPU_S3TC"));
    s_depth_clamp = !NV2A_GL_ES || has_ext("GL_EXT_depth_clamp");
    {
        /* Core on desktop and in GLES 3.2; an extension before that. */
        GLint maj = 0, min = 0;
        glGetIntegerv(GL_MAJOR_VERSION, &maj);
        glGetIntegerv(GL_MINOR_VERSION, &min);
        s_border_clamp = !NV2A_GL_ES || maj > 3 || (maj == 3 && min >= 2)
                      || has_ext("GL_EXT_texture_border_clamp")
                      || has_ext("GL_OES_texture_border_clamp");
#if NV2A_GL_ES
        s_invalidate = 1;
#elif defined(__APPLE__)
        s_invalidate = 0;
#else
        s_invalidate = maj > 4 || (maj == 4 && min >= 3) || has_ext("GL_ARB_invalidate_subdata");
#endif
        if (getenv("RECOMP_GPU_INVALIDATE") && !env_on("RECOMP_GPU_INVALIDATE"))
            s_invalidate = 0;
    }
    s_verbose = env_on("RECOMP_GPU_VERBOSE");
    s_readback_layout_switch = env_on("RECOMP_GPU_RT_RELAYOUT_READBACK");
    s_rt_direct_off = getenv("RECOMP_GPU_RT_DIRECT") && !env_on("RECOMP_GPU_RT_DIRECT");
    {
        const char *a = getenv("RECOMP_GPU_PRESENT");
        s_present_sync_mode = a && strcmp(a, "sync") == 0;
        a = getenv("RECOMP_GPU_READBACK");
        s_readback_all = a && strcmp(a, "all") == 0;
    }
    {
        const char *a = getenv("RECOMP_TEX_ANISO");
        s_bilinear = env_on("RECOMP_TEX_BILINEAR")
                  || (a && (strcmp(a, "8") == 0 || strcmp(a, "16") == 0));
        a = getenv("RECOMP_GPU_TEX_RECHECK");
        s_recheck_frame = a && strcmp(a, "frame") == 0;
        s_recheck_switch = a && strcmp(a, "switch") == 0;
    }
    fprintf(stderr, "  [GPUGL] S3TC %s, depth clamp %s, border clamp %s, invalidate %s\n",
            s_s3tc ? "on" : "off (DXT decoded on the CPU)",
            s_depth_clamp ? "on" : "off (clamped in the vertex shader)",
            s_border_clamp ? "on" : "off (clamp to edge)",
            s_invalidate ? "on" : "off");

    s_vs = compile(GL_VERTEX_SHADER, nv2a_gl_vertex_body());
    if (!s_vs) {
        unregister_self();
        return 0;
    }
    glGenVertexArrays(1, &s_vao);
    glBindVertexArray(s_vao);
    glGenBuffers(1, &s_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    {
        const GLsizei st = (GLsizei)sizeof(Nv2aVertexFull);
        int t;
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, st,
                              (const void *)offsetof(Nv2aVertexFull, pos));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, st,
                              (const void *)offsetof(Nv2aVertexFull, d0));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, st,
                              (const void *)offsetof(Nv2aVertexFull, d1));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, st,
                              (const void *)offsetof(Nv2aVertexFull, fog));
        for (t = 0; t < 4; t++) {
            glEnableVertexAttribArray((GLuint)(4 + t));
            glVertexAttribPointer((GLuint)(4 + t), 4, GL_FLOAT, GL_FALSE, st,
                                  (const void *)(offsetof(Nv2aVertexFull, tex)
                                                 + (size_t)t * 4 * sizeof(float)));
        }
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    if (s_depth_clamp)
        glEnable(GL_DEPTH_CLAMP);
    s_init = 1;
    return 1;
}

/* ---- render targets ----------------------------------------------------- */

/* Sampler state last set on a GL texture object (-1: unknown). */
typedef struct {
    GLint wrap_s, wrap_t, filter, swz_a;
    uint32_t border;                      /* border colour set (with a border wrap) */
} GlSamp;

typedef struct {
    /* The guest surface as last bound: w x h pixels in the layout pitch /
     * swz (swz: 0, or 1 | log2w << 8 | log2h << 16). What read-back and
     * seeding convert. */
    uint32_t va, pitch, bpp, w, h, swz;
    uint32_t gw, gh;                      /* the GL texture's size (>= w, h) */
    uint32_t cfmt;                        /* SET_SURFACE_FORMAT colour format (bits 0-3) */
    GLuint tex, fbo, depth;               /* depth: depth texture attached now */
    int dirty;
    int seed;                             /* guest bytes not uploaded yet (rt_seed) */
    uint32_t used;                        /* frame last drawn into */
    uint32_t serial;                      /* s_rt_serial at the last colour write */
    uint32_t sampled;                     /* frame last sampled directly as a texture */
    GlSamp samp;                          /* sampler state of tex */
    GLuint copy;                          /* copy of tex for a batch that samples
                                           * the target it draws into */
    uint32_t copy_w, copy_h;
    GlSamp copy_samp;
} GlRT;

/* Depth/stencil per zeta surface: a D24S8 texture (not a renderbuffer), so
 * a shadow map drawn into it can be sampled with a depth compare. */
typedef struct {
    uint32_t va, w, h;
    GLuint tex;
    GLuint fbo;                           /* depth-only framebuffer, made on demand */
    uint32_t used, bytes;                 /* frame last bound; guest bytes it shadows */
    GlSamp samp;                          /* sampler state of tex */
    GLint cmp;                            /* GL_TEXTURE_COMPARE_FUNC set (0: none yet) */
    uint32_t sampled;                     /* frame last sampled as a shadow map */
} GlDepth;

/* Counts colour writes into any target: a texture over a target is current
 * while no target it overlaps was written after its upload. */
static uint32_t s_rt_serial;

#define NV_GL_MAX_RT    16
#define NV_GL_MAX_DEPTH 8
static GlRT    s_rt[NV_GL_MAX_RT];
static GlDepth s_depth[NV_GL_MAX_DEPTH];
static uint8_t *s_scratch;                /* readback / conversion buffer */
static size_t   s_scratch_cap;

static uint8_t *scratch(size_t n)
{
    if (n > s_scratch_cap) {
        uint8_t *p = (uint8_t *)realloc(s_scratch, n);
        if (!p)
            return NULL;
        s_scratch = p;
        s_scratch_cap = n;
    }
    return s_scratch;
}

/* Byte offset of pixel (x, y) in the guest surface. */
static size_t rt_offset(const GlRT *r, uint32_t x, uint32_t y)
{
    if (r->swz) {
        uint32_t mx, my;
        xbox_swizzle_masks(r->w, r->h, &mx, &my);
        return (size_t)(swizzle_deposit(x, mx) | swizzle_deposit(y, my)) * r->bpp;
    }
    return (size_t)y * r->pitch + (size_t)x * r->bpp;
}

/* Guest bytes -> the render target (on creation: what was there before the
 * GPU drew counts, a title may draw over a CPU-written picture). */
static void rt_upload(GlRT *r)
{
    uint8_t *buf = scratch((size_t)r->w * r->h * 4), *mem = guest(r->va);
    uint32_t x, y;

    if (!buf)
        return;
    for (y = 0; y < r->h; y++)
        for (x = 0; x < r->w; x++) {
            const uint8_t *p = mem + rt_offset(r, x, y);
            uint8_t *o = buf + ((size_t)y * r->w + x) * 4;
            if (r->bpp == 4) {
                uint32_t c;
                memcpy(&c, p, 4);
                argb_to_rgba(c, o);
            } else if (r->cfmt == 0x1 || r->cfmt == 0x2) {   /* X1R5G5B5 */
                uint32_t t = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
                o[0] = (uint8_t)((((t >> 10) & 0x1F) * 255u + 15u) / 31u);
                o[1] = (uint8_t)((((t >> 5) & 0x1F) * 255u + 15u) / 31u);
                o[2] = (uint8_t)(((t & 0x1F) * 255u + 15u) / 31u);
                o[3] = 0xFF;
            } else {
                uint32_t t = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
                o[0] = (uint8_t)(((t >> 11) & 0x1F) << 3);
                o[1] = (uint8_t)(((t >> 5) & 0x3F) << 2);
                o[2] = (uint8_t)((t & 0x1F) << 3);
                o[3] = 0xFF;
            }
        }
    glBindTexture(GL_TEXTURE_2D, r->tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)r->w, (GLsizei)r->h,
                    GL_RGBA, GL_UNSIGNED_BYTE, buf);
}

/* Read-backs come as 0xAARRGGBB words where the API offers it (desktop GL:
 * GL_BGRA + GL_UNSIGNED_INT_8_8_8_8_REV, the guest's own byte order), else
 * as RGBA bytes (GLES guarantees only GL_RGBA / GL_UNSIGNED_BYTE). */
#if NV2A_GL_ES
#define NV_GL_READ_FORMAT GL_RGBA
#define NV_GL_READ_TYPE   GL_UNSIGNED_BYTE
#define NV_GL_READ_ARGB   0
#else
#define NV_GL_READ_FORMAT GL_BGRA
#define NV_GL_READ_TYPE   GL_UNSIGNED_INT_8_8_8_8_REV
#define NV_GL_READ_ARGB   1
#endif

static inline uint32_t read_px(const uint8_t *p)
{
#if NV_GL_READ_ARGB
    uint32_t c;
    memcpy(&c, p, 4);
    return c;
#else
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
#endif
}

/* w x h read-back pixels (read_px layout) -> guest bytes of target r, in
 * the surface's own format and layout (8888, 565 or 1555, swizzled when
 * the surface is). */
static void rt_store(const GlRT *r, const uint8_t *buf)
{
    uint8_t *mem = guest(r->va);
    uint32_t x, y, mx = 0, my = 0;
    /* 16-bit surfaces in their own format: X1R5G5B5 (colour formats 1 and
     * 2, the X bit 0 and 1: Z1R5G5B5 / O1R5G5B5) or R5G6B5. Fable's back
     * buffer is X1R5G5B5 (0x121), which it samples as LU_X1R5G5B5. */
    int x555 = r->cfmt == 0x1 || r->cfmt == 0x2;
    uint32_t x_bit = r->cfmt == 0x2 ? 0x8000u : 0;

    if (r->swz)
        xbox_swizzle_masks(r->w, r->h, &mx, &my);
    for (y = 0; y < r->h; y++) {
        const uint8_t *row = buf + (size_t)y * r->w * 4;
        if (r->bpp == 4 && !r->swz) {
            uint32_t *o = (uint32_t *)(mem + (size_t)y * r->pitch);
#if NV_GL_READ_ARGB
            memcpy(o, row, (size_t)r->w * 4);
#else
            for (x = 0; x < r->w; x++, row += 4)
                o[x] = read_px(row);
#endif
            continue;
        }
        for (x = 0; x < r->w; x++, row += 4) {
            uint32_t c = read_px(row);
            uint8_t *p = mem + (r->swz ? (size_t)(swizzle_deposit(x, mx)
                                                  | swizzle_deposit(y, my)) * r->bpp
                                       : (size_t)y * r->pitch + (size_t)x * r->bpp);
            if (r->bpp == 4) {
                memcpy(p, &c, 4);
            } else if (x555) {
                uint16_t v = (uint16_t)(x_bit | (((c >> 19) & 0x1F) << 10)
                                        | (((c >> 11) & 0x1F) << 5) | ((c >> 3) & 0x1F));
                memcpy(p, &v, 2);
            } else {
                uint16_t v = (uint16_t)((((c >> 19) & 0x1F) << 11) | (((c >> 10) & 0x3F) << 5)
                                        | ((c >> 3) & 0x1F));
                memcpy(p, &v, 2);
            }
        }
    }
}

/* A present read-back in flight (gl_present): glReadPixels into a pixel
 * pack buffer at flip N, mapped at flip N+1, when the GPU has long finished
 * frame N. snap is the target's layout at the read (the target itself may
 * be relaid out, reshaped or evicted meanwhile). dropped: a synchronous
 * read-back of the same surface has since put newer pixels there. */
static GLuint s_pbo;
static size_t s_pbo_size;
static struct {
    int pending, dropped;
    GlRT snap;
    Nv2aPresentSurface surf;
} s_pend;

/* What the title drew or put there before the GPU target existed: upload
 * the guest bytes now, if a full clear has not made them moot. */
static void rt_seed(GlRT *r)
{
    if (!r || !r->seed)
        return;
    r->seed = 0;
    rt_upload(r);
    s_st.seeds++;
}

/* The render target -> guest bytes (synchronous). */
static void rt_readback(GlRT *r)
{
    uint8_t *buf;

    if (!r->dirty || !r->fbo)
        return;
    buf = scratch((size_t)r->w * r->h * 4);
    if (!buf)
        return;
    glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
    glReadPixels(0, 0, (GLsizei)r->w, (GLsizei)r->h, NV_GL_READ_FORMAT, NV_GL_READ_TYPE, buf);
    rt_store(r, buf);
    if (s_pend.pending && s_pend.snap.va == r->va)
        s_pend.dropped = 1;           /* these pixels are newer than the PBO's */
    r->dirty = 0;
    s_st.readbacks++;
}

static void rt_free(GlRT *r)
{
    rt_readback(r);
    if (r->fbo) glDeleteFramebuffers(1, &r->fbo);
    if (r->tex) glDeleteTextures(1, &r->tex);
    if (r->copy) glDeleteTextures(1, &r->copy);
    memset(r, 0, sizeof *r);
}

/* Give target r a bigger GL texture (gw x gh), keeping its pixels (a GPU
 * copy, no read-back). */
static void rt_grow(GlRT *r, uint32_t gw, uint32_t gh)
{
    GLuint t;

    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)gw, (GLsizei)gh, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, (GLsizei)r->gw, (GLsizei)r->gh);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
    glDeleteTextures(1, &r->tex);
    r->tex = t;
    r->gw = gw;
    r->gh = gh;
    r->samp.wrap_s = r->samp.wrap_t = r->samp.filter = r->samp.swz_a = -1;
    s_st.rt_grows++;
}

static GlRT *rt_get(const Nv2aSurface *s)
{
    uint32_t bpp = s->bytes_per_pixel, w, h, swz = 0, i;
    GlRT *r, *free_slot = NULL, *lru = NULL;

    if (bpp != 2 && bpp != 4)
        return NULL;
    if (((s->format >> 8) & 0xF) == 2) {
        uint32_t lw = (s->format >> 16) & 0xF, lh = (s->format >> 24) & 0xF;
        w = 1u << lw;
        h = 1u << lh;
        swz = 1u | lw << 8 | lh << 16;
    } else {
        if (!s->pitch)
            return NULL;
        w = s->pitch / bpp;
        h = s->clip_y + s->height;
        if (w < s->clip_x + s->width)
            w = s->clip_x + s->width;
    }
    if (!w || !h || w > 4096 || h > 4096)
        return NULL;
    for (i = 0; i < NV_GL_MAX_RT; i++) {
        r = &s_rt[i];
        if (!r->tex) {
            if (!free_slot)
                free_slot = r;
            continue;
        }
        if (r->va == s->color_va) {
            /* The same memory in another layout or size (bytes per pixel
             * unchanged): Fable's bloom chain draws its 256x256 target at
             * 0x80D38080 swizzled in one pass and linear (pitch 1024) in the
             * next, and 0x80CF9080 alternately as a 256x256 and a 128x128
             * swizzled surface. The GL texture holds pixels by (x, y)
             * whatever the guest layout, and each pass draws over what it
             * then reads, so the target is kept: only the layout read-back
             * and seeding use changes, and the texture grows (by a GPU copy)
             * when the new shape does not fit. Recreating it cost a
             * read-back and an upload per switch, 16 a frame. */
            if (r->bpp == bpp) {
                if (w > r->gw || h > r->gh)
                    rt_grow(r, w > r->gw ? w : r->gw, h > r->gh ? h : r->gh);
                if (r->swz != swz || r->pitch != s->pitch || (swz && (r->w != w || r->h != h))) {
                    if (r->dirty && s_readback_layout_switch)
                        rt_readback(r);
                    r->swz = swz;
                    r->pitch = s->pitch;
                    r->w = w;
                    r->h = h;
                    s_st.relayouts++;
                } else {
                    /* A linear surface of the same pitch: rows drawn by an
                     * earlier, taller pass still count. */
                    if (w > r->w) r->w = w;
                    if (h > r->h) r->h = h;
                }
                r->cfmt = s->format & 0xF;
                return r;
            }
            /* Same memory, another pixel size: the bytes mean something
             * else now. Hand the old one back first. */
            rt_free(r);
            if (!free_slot)
                free_slot = r;
            continue;
        }
        if (!lru || r->used < lru->used)
            lru = r;
    }
    if (!free_slot) {
        rt_free(lru);
        free_slot = lru;
    }
    r = free_slot;
    r->va = s->color_va;
    r->pitch = s->pitch;
    r->bpp = bpp;
    r->w = r->gw = w;
    r->h = r->gh = h;
    r->swz = swz;
    r->cfmt = s->format & 0xF;
    r->samp.wrap_s = r->samp.wrap_t = r->samp.filter = r->samp.swz_a = -1;
    glGenTextures(1, &r->tex);
    glBindTexture(GL_TEXTURE_2D, r->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    /* Seeded from the guest bytes when first drawn or sampled, unless the
     * first thing the title does is clear it whole (rt_seed, gl_clear). */
    r->seed = 1;
    s_st.rt_creates++;
    glGenFramebuffers(1, &r->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->tex, 0);
    r->depth = 0;
    if (s_verbose)
        fprintf(stderr, "  [GPUGL] render target 0x%08X %ux%u bpp %u%s\n",
                r->va, w, h, bpp, swz ? " swizzled" : "");
    return r;
}

/* Bytes per zeta pixel: Z16 (SET_SURFACE_FORMAT bits 4-7 = 1) or Z24S8. */
static uint32_t zeta_bpp(const Nv2aSurface *s)
{
    return ((s->format >> 4) & 0xF) == 1 ? 2u : 4u;
}

static void depth_free(GlDepth *d)
{
    int i;

    /* Colour targets that have it attached drop it (re-attached at bind). */
    for (i = 0; i < NV_GL_MAX_RT; i++)
        if (s_rt[i].depth == d->tex && d->tex) {
            glBindFramebuffer(GL_FRAMEBUFFER, s_rt[i].fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                   GL_TEXTURE_2D, 0, 0);
            s_rt[i].depth = 0;
        }
    if (d->fbo) glDeleteFramebuffers(1, &d->fbo);
    if (d->tex) glDeleteTextures(1, &d->tex);
    memset(d, 0, sizeof *d);
}

/* Depth/stencil for zeta surface `va`, at least w x h. The CPU rasteriser
 * keeps one float buffer per zeta address; so does this, as a D24S8
 * texture. */
static GlDepth *depth_get(uint32_t va, uint32_t w, uint32_t h, uint32_t zbpp)
{
    int i, slot = -1;
    GlDepth *d;

    for (i = 0; i < NV_GL_MAX_DEPTH; i++) {
        d = &s_depth[i];
        if (d->tex && d->va == va) {
            if (d->w >= w && d->h >= h) {
                d->used = s_frame;
                return d;
            }
            depth_free(d);
            slot = i;
            break;
        }
        if (!d->tex && slot < 0)
            slot = i;
    }
    if (slot < 0) {
        static int next;
        slot = next++ % NV_GL_MAX_DEPTH;
        depth_free(&s_depth[slot]);
    }
    d = &s_depth[slot];
    d->va = va;
    d->used = s_frame;
    d->bytes = w * h * zbpp;
    d->w = w < 640 ? 640 : w;
    d->h = h < 480 ? 480 : h;
    d->samp.wrap_s = d->samp.wrap_t = d->samp.filter = d->samp.swz_a = -1;
    d->cmp = 0;
    glGenTextures(1, &d->tex);
    glBindTexture(GL_TEXTURE_2D, d->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, (GLsizei)d->w, (GLsizei)d->h, 0,
                 GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    return d;
}

/* What a draw or clear goes into: a colour target (rt) with its depth, or,
 * for a surface without colour (Nv2aSurface.color_valid == 0), the zeta
 * surface's depth-only framebuffer (rt NULL). w x h is the framebuffer's
 * size, which the vertex shader maps surface pixels against. */
typedef struct {
    GlRT *rt;
    uint32_t w, h, bpp;
} GlTarget;

/* The framebuffer the last draw or clear went into: a change is a target
 * switch, which on a tiler ends a render pass (stored, later reloaded). */
static GLuint s_target_fbo;

static void note_target(GLuint fbo)
{
    if (fbo != s_target_fbo) {
        s_target_fbo = fbo;
        s_st.target_switches++;
    }
}

/* The depth-only framebuffer for zeta surface `va` (glDrawBuffers GL_NONE):
 * a shadow-map pass writes depth and nothing else, so no colour target is
 * created in guest memory and nothing is read back. */
static int bind_depth_only(const Nv2aSurface *s, uint32_t zeta_va, GlTarget *t)
{
    uint32_t w = s->clip_x + s->width, h = s->clip_y + s->height;
    GlDepth *d;

    if (!zeta_va || !w || !h || w > 4096 || h > 4096)
        return 0;
    d = depth_get(zeta_va, w, h, zeta_bpp(s));
    if (!d->fbo) {
        const GLenum none = GL_NONE;
        glGenFramebuffers(1, &d->fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, d->fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                               GL_TEXTURE_2D, d->tex, 0);
        glDrawBuffers(1, &none);
        glReadBuffer(GL_NONE);
        if (s_verbose)
            fprintf(stderr, "  [GPUGL] depth-only target for zeta 0x%08X %ux%u\n",
                    zeta_va, d->w, d->h);
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, d->fbo);
    }
    note_target(d->fbo);
    t->rt = NULL;
    t->w = d->w;
    t->h = d->h;
    t->bpp = 4;
    glViewport(0, 0, (GLsizei)t->w, (GLsizei)t->h);
    glEnable(GL_SCISSOR_TEST);
    glScissor((GLint)s->clip_x, (GLint)s->clip_y, (GLsizei)s->width, (GLsizei)s->height);
    return 1;
}

/* A colour target already made for surface s's memory, with its pixel size. */
static int rt_exists(const Nv2aSurface *s)
{
    int i;

    for (i = 0; i < NV_GL_MAX_RT; i++)
        if (s_rt[i].tex && s_rt[i].va == s->color_va
            && s_rt[i].bpp == s->bytes_per_pixel)
            return 1;
    return 0;
}

/* Bind the render target for drawing into, with its depth surface. A batch
 * with every colour channel masked (Nv2aSurface.color_masked: depth
 * pre-passes and visibility proxies) stays on the colour target when there
 * is one -- apply_state masks every channel and such a batch does not make
 * the target dirty -- instead of going to the depth-only framebuffer of the
 * same zeta surface and back: Fable did that 65-90 times a frame in
 * Oakvale. Without a target for that memory yet, it does not make one. */
static int bind_target(const Nv2aSurface *s, uint32_t zeta_va, GlTarget *t)
{
    GlRT *r;
    GLuint d;

    if (!s->color_valid || (s->color_masked && !rt_exists(s)))
        return bind_depth_only(s, zeta_va, t);
    if (!(r = rt_get(s)))
        return 0;
    glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
    note_target(r->fbo);
    d = depth_get(zeta_va, r->gw, r->gh, zeta_bpp(s))->tex;
    if (d != r->depth) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                               GL_TEXTURE_2D, d, 0);
        r->depth = d;
    }
    glViewport(0, 0, (GLsizei)r->gw, (GLsizei)r->gh);
    glEnable(GL_SCISSOR_TEST);
    glScissor((GLint)s->clip_x, (GLint)s->clip_y, (GLsizei)s->width, (GLsizei)s->height);
    r->used = s_frame;
    t->rt = r;
    t->w = r->gw;
    t->h = r->gh;
    t->bpp = r->bpp;
    return 1;
}

/* Before a batch samples guest memory the GPU has drawn into, put the GPU's
 * pixels there (render to texture). */
static void readback_overlapping(uint32_t va, uint32_t bytes)
{
    int i;

    for (i = 0; i < NV_GL_MAX_RT; i++) {
        GlRT *r = &s_rt[i];
        uint32_t span;
        if (!r->tex || !r->dirty)
            continue;
        span = r->swz ? r->w * r->h * r->bpp : r->pitch * r->h;
        if (va < r->va + span && r->va < va + bytes)
            rt_readback(r);
    }
}

/* A texture format that reads a colour target's pixels as they are: same
 * bytes per pixel and component layout. *alpha: what alpha reads (GL_RED..
 * for "the target's", GL_ONE / GL_ZERO for a format without one). 0: the
 * format reinterprets the bytes (a 32-bit target read as 16-bit texels, BGRA
 * orders, palettes, YUV...): read back and decode instead. */
static int rt_format_compatible(const GlRT *r, uint32_t color, GLint *alpha)
{
    *alpha = GL_ALPHA;
    if (r->bpp == 4) {
        switch (color) {
        case 0x06: case 0x12:                       /* SZ/LU A8R8G8B8 */
            /* X8R8G8B8 surfaces (formats 4, 5) keep their own alpha byte in
             * memory as the CPU path does; read it as stored. */
            return 1;
        case 0x07: case 0x1E:                       /* SZ/LU X8R8G8B8 */
            *alpha = GL_ONE;
            return 1;
        default:
            return 0;
        }
    }
    if (r->cfmt == 0x3)                             /* R5G6B5 */
        return color == 0x05 || color == 0x11;
    if (r->cfmt == 0x1 || r->cfmt == 0x2) {         /* X1R5G5B5, Z or O */
        switch (color) {
        case 0x03: case 0x1C:                       /* SZ/LU X1R5G5B5 */
            *alpha = GL_ONE;
            return 1;
        case 0x02: case 0x10:                       /* SZ/LU A1R5G5B5: the X bit */
            *alpha = r->cfmt == 0x2 ? GL_ONE : GL_ZERO;
            return 1;
        default:
            return 0;
        }
    }
    return 0;
}

/* The colour target a texture reads, when the GPU can sample it as it is
 * (render to texture without a read-back): same address, a compatible
 * format (rt_format_compatible), and the texture inside the target --
 * swizzled textures cover the whole target (the target is stored linear, so
 * a normalised coordinate means the same texel), linear ones read rows
 * of the same pitch. A cube map never matches. */
static GlRT *rt_for_texture(const Nv2aTexture *t, uint32_t cube, GLint *alpha)
{
    int i;

    if (cube || s_rt_direct_off)
        return NULL;
    for (i = 0; i < NV_GL_MAX_RT; i++) {
        GlRT *r = &s_rt[i];
        if (!r->tex || r->va != t->offset)
            continue;
        if (!rt_format_compatible(r, t->color, alpha))
            return NULL;
        if (t->width > r->w || t->height > r->h)
            return NULL;
        if (d3d8_format_is_swizzled(t->color)) {
            if (t->width != r->w || t->height != r->h)
                return NULL;
        } else if (!r->swz && t->pitch != r->pitch) {
            return NULL;
        }
        return r;
    }
    return NULL;
}

/* ---- textures ----------------------------------------------------------- */

typedef struct {
    uint32_t offset, color, width, height, pitch, palette, cube;   /* key */
    uint32_t bytes;
    uint64_t hash;
    GLuint tex;
    uint32_t checked;               /* frame of the last hash check */
    GlSamp samp;                    /* sampler state last set */
    /* The last upload failed (a format the decoder does not know, such as
     * Fable's Z16 shadow map bound as 0x30): drawn as "no texture" until the
     * bytes change. Returned as usable, the incomplete GL texture sampled
     * (0, 0, 0, 1) and darkened every batch that used it. */
    int failed;
    /* Hash policy (tex_get): the frame the bytes last changed, whether they
     * ever changed after the upload that created the entry, and
     * s_rt_serial at the last upload. */
    uint32_t changed;
    int changes;
    uint32_t serial;
} GlTex;

#define NV_GL_MAX_TEX 512           /* power of two */
static GlTex s_tex[NV_GL_MAX_TEX];
static int s_ntex;
static const GlTex *s_bound[4];     /* what each unit sampled last batch */

static uint32_t tex_slot(const Nv2aTexture *t, uint32_t cube)
{
    uint32_t h = (t->offset >> 7) ^ (t->offset >> 19) ^ t->color * 0x9E37u
               ^ t->width * 31u ^ t->height * 131u ^ cube;
    return h & (NV_GL_MAX_TEX - 1);
}

static void tex_drop_all(void)
{
    int i;
    for (i = 0; i < NV_GL_MAX_TEX; i++)
        if (s_tex[i].tex)
            glDeleteTextures(1, &s_tex[i].tex);
    memset(s_tex, 0, sizeof s_tex);
    memset(s_bound, 0, sizeof s_bound);
    s_ntex = 0;
}

static uint64_t tex_hash(const Nv2aTexture *t, uint32_t bytes)
{
    uint64_t h = hash_bytes(guest(t->offset), bytes);
    if (t->color == 0x0B && t->palette)          /* the palette is content too */
        h ^= hash_bytes(guest(t->palette), 1024) * 31u;
    return h;
}

/* Upload one 2D image (a texture, or one cube face at `target`). */
static int tex_upload_image(GLenum target, const Nv2aTexture *t)
{
    uint32_t bb = d3d8_format_dxt_block_bytes(t->color), n, i;
    uint32_t *argb;
    uint8_t *buf;

    if (bb && s_s3tc) {
        GLenum f = t->color == 0x0C ? GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
                 : t->color == 0x0E ? GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
                 : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
        uint32_t size = ((t->width + 3) / 4) * ((t->height + 3) / 4) * bb;
        glCompressedTexImage2D(target, 0, f, (GLsizei)t->width, (GLsizei)t->height,
                               0, (GLsizei)size, guest(t->offset));
        s_st.upload_kb += size >> 10;
        return 1;
    }
    n = t->width * t->height;
    buf = (uint8_t *)malloc((size_t)n * 8);
    if (!buf)
        return 0;
    argb = (uint32_t *)(buf + (size_t)n * 4);
    if (!nv2a_backend_decode_texture(t, argb)) {
        free(buf);
        return 0;
    }
    for (i = 0; i < n; i++)
        argb_to_rgba(argb[i], buf + (size_t)i * 4);
    glTexImage2D(target, 0, GL_RGBA8, (GLsizei)t->width, (GLsizei)t->height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, buf);
    s_st.upload_kb += n >> 8;
    free(buf);
    return 1;
}

static int tex_upload(GlTex *e, const Nv2aTexture *t)
{
    GLenum target = e->cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
    Nv2aTexture one = *t;
    int ok = 1, f;

    if (!e->tex)
        glGenTextures(1, &e->tex);
    glBindTexture(target, e->tex);
    one.cube = 0;
    if (e->cube) {
        uint32_t stride = e->bytes / 6;
        for (f = 0; f < 6 && ok; f++) {
            one.offset = t->offset + (uint32_t)f * stride;
            ok = tex_upload_image(GL_TEXTURE_CUBE_MAP_POSITIVE_X + (GLenum)f, &one);
        }
    } else {
        ok = tex_upload_image(GL_TEXTURE_2D, &one);   /* a cube seen as 2D: face 0 */
    }
    glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, 0);
    e->samp.wrap_s = e->samp.wrap_t = e->samp.filter = e->samp.swz_a = -1;
    s_st.uploads++;
    return ok;
}

/* Does [va, va + bytes) overlap memory the GPU owns now? 2: a zeta surface
 * bound in the last two frames (its bytes in guest memory are not what the
 * GPU holds, and hashing Fable's 1.2 MB shadow map every frame bought
 * nothing); 1: a colour target drawn in the last two frames, with the
 * highest serial among those it overlaps in *serial; 0: neither -- hash. */
static int tex_gpu_overlap(uint32_t va, uint32_t bytes, uint32_t *serial)
{
    int i, ov = 0;

    *serial = 0;
    for (i = 0; i < NV_GL_MAX_DEPTH; i++) {
        const GlDepth *d = &s_depth[i];
        if (d->tex && s_frame - d->used <= 2
            && va < d->va + d->bytes && d->va < va + bytes)
            return 2;
    }
    for (i = 0; i < NV_GL_MAX_RT; i++) {
        const GlRT *r = &s_rt[i];
        uint32_t span;
        if (!r->tex || s_frame - r->used > 2)
            continue;
        span = r->swz ? r->w * r->h * r->bpp : r->pitch * r->h;
        if (va < r->va + span && r->va < va + bytes) {
            ov = 1;
            if (r->serial > *serial)
                *serial = r->serial;
        }
    }
    return ov;
}

/* The GL texture for `t`, current with guest memory. NULL: not samplable. */
static GlTex *tex_get(const Nv2aTexture *t, uint32_t cube, int unit)
{
    uint32_t slot = tex_slot(t, cube), i, bytes;
    GlTex *e = NULL;

    if (!t->width || !t->height || t->width > 4096 || t->height > 4096)
        return NULL;
    for (i = 0; i < NV_GL_MAX_TEX; i++) {
        GlTex *c = &s_tex[(slot + i) & (NV_GL_MAX_TEX - 1)];
        if (!c->tex && !c->bytes) {
            e = c;                         /* empty: not cached */
            break;
        }
        if (c->offset == t->offset && c->color == t->color && c->width == t->width
            && c->height == t->height && c->pitch == t->pitch
            && c->palette == t->palette && c->cube == cube) {
            e = c;
            break;
        }
    }
    if (!e || (!e->tex && s_ntex >= NV_GL_MAX_TEX * 3 / 4)) {
        tex_drop_all();
        e = &s_tex[slot];
    }
    if (e->tex) {
        uint32_t serial = 0;
        int ov = s_recheck_switch ? 0 : tex_gpu_overlap(t->offset, e->bytes, &serial);

        if (ov == 2)                        /* depth the GPU keeps: never hashed */
            return e->failed ? NULL : e;
        if (ov == 1) {
            /* Over a target the GPU draws: current unless a target it
             * overlaps was written since the upload -- then read back and
             * upload again, without hashing (render to texture). */
            if (serial <= e->serial)
                return e->failed ? NULL : e;
            readback_overlapping(t->offset, e->bytes);
            e->serial = s_rt_serial;
            e->checked = s_frame;
            e->failed = !tex_upload(e, t);
            return e->failed ? NULL : e;
        }
        /* Hash the bytes once per frame; again within the frame only when
         * this unit sampled something else in between AND the texture is
         * volatile (changed in the last 60 frames: a glyph cache written
         * between two text batches) or new this frame (its first bytes may
         * be a buffer the title is still filling: Fable rewrote a 512x512
         * texture at 0x80DD6080 between two batches of the frame that
         * created it, and the entry kept the first bytes). A texture that
         * has never changed is not hashed twice in a later frame. */
        if (e->checked == s_frame
            && (s_recheck_frame || s_bound[unit] == e
                || (!s_recheck_switch && !(e->changes && s_frame - e->changed < 60)
                    && e->changed != s_frame)))
            return e->failed ? NULL : e;
        readback_overlapping(t->offset, e->bytes);
        e->checked = s_frame;
        {
            uint64_t h = tex_hash(t, e->bytes);
            if (h == e->hash)
                return e->failed ? NULL : e;
            e->hash = h;
        }
        e->changed = s_frame;
        e->changes = 1;
        e->serial = s_rt_serial;
        e->failed = !tex_upload(e, t);
        return e->failed ? NULL : e;
    }
    {
        Nv2aTexture q = *t;
        q.cube = cube;
        bytes = nv2a_backend_texture_bytes(&q);
    }
    if (!bytes)
        return NULL;
    e->offset = t->offset; e->color = t->color; e->width = t->width;
    e->height = t->height; e->pitch = t->pitch; e->palette = t->palette;
    e->cube = cube;
    e->bytes = bytes;
    readback_overlapping(t->offset, bytes);
    {
        uint32_t serial;
        e->hash = !s_recheck_switch && tex_gpu_overlap(t->offset, bytes, &serial)
                ? 0 : tex_hash(t, bytes);
    }
    e->checked = s_frame;
    e->changed = s_frame;
    e->changes = 0;
    e->serial = s_rt_serial;
    s_ntex++;
    /* A failed upload keeps the entry (bytes != 0, tex set, failed) so a
     * format the decoder does not know is not retried every batch. */
    e->failed = !tex_upload(e, t);
    return e->failed ? NULL : e;
}

static GLint wrap_mode(uint32_t a, int border)
{
    switch (a) {
    case 1:  return GL_REPEAT;
    case 2:  return GL_MIRRORED_REPEAT;
    case 4:  return (border && s_border_clamp) ? GL_CLAMP_TO_BORDER : GL_CLAMP_TO_EDGE;
    default: return GL_CLAMP_TO_EDGE;
    }
}

/* Sampler state on the texture bound to `target`, set only where it changed. */
static void samp_set(GLenum target, GlSamp *sp, GLint f, GLint ws, GLint wt,
                     uint32_t border, GLint swz_a)
{
    if (sp->filter != f) {
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, f);
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, f);
        sp->filter = f;
    }
    if (sp->wrap_s != ws || sp->wrap_t != wt
        || ((ws == GL_CLAMP_TO_BORDER || wt == GL_CLAMP_TO_BORDER) && sp->border != border)) {
        glTexParameteri(target, GL_TEXTURE_WRAP_S, ws);
        glTexParameteri(target, GL_TEXTURE_WRAP_T, wt);
        if (target == GL_TEXTURE_CUBE_MAP)
            glTexParameteri(target, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        sp->wrap_s = ws;
        sp->wrap_t = wt;
        if (ws == GL_CLAMP_TO_BORDER || wt == GL_CLAMP_TO_BORDER) {
            float bc[4];
            unpack4(border, bc);
            glTexParameterfv(target, GL_TEXTURE_BORDER_COLOR, bc);
            sp->border = border;
        }
    }
    if (sp->swz_a != swz_a) {
        glTexParameteri(target, GL_TEXTURE_SWIZZLE_A, swz_a);
        sp->swz_a = swz_a;
    }
}

/* Filtering the way the CPU rasteriser chooses it: the combiner path
 * honours the title's TENT filters (rc_texel), the two simpler paths take
 * the nearest texel unless RECOMP_TEX_BILINEAR. Level 0 only on both. */
static GLint tex_filter(const Nv2aTexture *t, uint32_t shade)
{
    uint32_t mag = (t->filter >> 24) & 0xF, min = (t->filter >> 16) & 0xFF;
    int lin = s_bilinear
           || (shade == NV2A_SHADE_COMBINERS
               && (mag == 2 || min == 2 || min == 4 || min == 6));
    return lin ? GL_LINEAR : GL_NEAREST;
}

static void tex_bind(GlTex *e, const Nv2aTexture *t, int unit, uint32_t shade)
{
    GLenum target = e->cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;

    glActiveTexture(GL_TEXTURE0 + (GLenum)unit);
    glBindTexture(target, e->tex);
    samp_set(target, &e->samp, tex_filter(t, shade),
             wrap_mode(t->addr_u, (int)t->border_color),
             wrap_mode(t->addr_v, (int)t->border_color), t->border, GL_ALPHA);
    s_bound[unit] = e;
}

/* ---- shader programs ---------------------------------------------------- */

typedef struct {
    Nv2aGlShaderKey key;
    uint64_t hash;
    GLuint prog;                    /* 0 with `failed`: do not retry */
    int failed;
    GLint u_vp, u_tscale, u_c0, u_c1, u_fc0, u_fc1, u_fogc, u_aref;
    GLint u_bm, u_bso;              /* -1 without a bump-env stage */
    /* With a translated vertex program (vsh_hash != 0): its uniforms, the
     * location of each c[i] (-1 where the compiler dropped it), and the
     * constant serial this program's copy of c[] is synced to. */
    uint64_t vsh_hash;
    GLint u_xf, u_fog;
    GLint *cloc;
    uint64_t const_synced;
} GlProg;

/* A translated vertex program (nv2a_gl_vsh.c), compiled once per program. */
typedef struct {
    uint64_t hash;
    uint32_t count;
    GLuint sh;                      /* 0 with `failed`: do not retry */
    int failed;
} GlVsh;

#define NV_GL_MAX_PROG 2048         /* power of two; pipelines x programs */
#define NV_GL_MAX_VSH  512          /* power of two */
static GlProg s_prog[NV_GL_MAX_PROG];

static GlVsh s_vsh[NV_GL_MAX_VSH];

static GlVsh *vsh_get(const Nv2aProgramBatch *pb)
{
    static char body[131072];
    uint64_t h = pb->hash ? pb->hash : 1;
    uint32_t i, slot = (uint32_t)h & (NV_GL_MAX_VSH - 1);
    GlVsh *v = NULL;

    for (i = 0; i < NV_GL_MAX_VSH; i++) {
        GlVsh *c = &s_vsh[(slot + i) & (NV_GL_MAX_VSH - 1)];
        if (!c->sh && !c->failed) {
            v = c;
            break;
        }
        if (c->hash == h && c->count == pb->ins_count)
            return c->failed ? NULL : c;
    }
    if (!v)
        return NULL;                /* ponytail: 512 programs, then the CPU */
    v->hash = h;
    v->count = pb->ins_count;
    if (!nv2a_gl_vsh_body(pb->ins, pb->ins_count,
                          NV2A_GL_ES ? NV2A_GL_VSH_RENDER
                                     : NV2A_GL_VSH_RENDER | NV2A_GL_VSH_CLIP_DISTANCE,
                          body, sizeof body, NULL)
        || !(v->sh = compile(GL_VERTEX_SHADER, body))) {
        v->failed = 1;
        return NULL;
    }
    s_st.compiles++;
    return v;
}

/* The program for pipeline `key` with the fixed vertex shader (vsh NULL:
 * CPU-transformed vertices) or a translated vertex program. */
static GlProg *prog_get(const Nv2aGlShaderKey *key, const GlVsh *vsh)
{
    static char body[32768];
    uint64_t vh = vsh ? vsh->hash : 0;
    uint64_t h = nv2a_gl_shader_key_hash(key) ^ (vh * 0x9E3779B97F4A7C15ull);
    uint32_t i, slot = (uint32_t)h & (NV_GL_MAX_PROG - 1);
    GlProg *p = NULL;
    GLuint fs;
    GLint ok = 0;

    for (i = 0; i < NV_GL_MAX_PROG; i++) {
        GlProg *c = &s_prog[(slot + i) & (NV_GL_MAX_PROG - 1)];
        if (!c->prog && !c->failed) {
            p = c;
            break;
        }
        if (c->hash == h && c->vsh_hash == vh && memcmp(&c->key, key, sizeof *key) == 0)
            return c->failed ? NULL : c;
    }
    if (!p)
        return NULL;                /* ponytail: 2048 pipelines, then no more */
    p->key = *key;
    p->hash = h;
    p->vsh_hash = vh;
    if (!nv2a_gl_fragment_body(key, body, sizeof body)
        || !(fs = compile(GL_FRAGMENT_SHADER, body))) {
        p->failed = 1;
        return NULL;
    }
    p->prog = glCreateProgram();
    glAttachShader(p->prog, vsh ? vsh->sh : s_vs);
    glAttachShader(p->prog, fs);
    glLinkProgram(p->prog);
    glDeleteShader(fs);
    glGetProgramiv(p->prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p->prog, sizeof log, NULL, log);
        fprintf(stderr, "  [GPUGL] link failed: %s\n", log);
        glDeleteProgram(p->prog);
        p->prog = 0;
        p->failed = 1;
        return NULL;
    }
    glUseProgram(p->prog);
    for (i = 0; i < 4; i++) {
        char name[8];
        snprintf(name, sizeof name, "u_t%u", i);
        glUniform1i(glGetUniformLocation(p->prog, name), (GLint)i);
    }
    p->u_vp = glGetUniformLocation(p->prog, "u_vp");
    p->u_tscale = glGetUniformLocation(p->prog, "u_tscale");
    p->u_c0 = glGetUniformLocation(p->prog, "u_c0");
    p->u_c1 = glGetUniformLocation(p->prog, "u_c1");
    p->u_fc0 = glGetUniformLocation(p->prog, "u_fc0");
    p->u_fc1 = glGetUniformLocation(p->prog, "u_fc1");
    p->u_fogc = glGetUniformLocation(p->prog, "u_fogc");
    p->u_aref = glGetUniformLocation(p->prog, "u_aref");
    p->u_bm = glGetUniformLocation(p->prog, "u_bm");
    p->u_bso = glGetUniformLocation(p->prog, "u_bso");
    if (vsh) {
        p->u_xf = glGetUniformLocation(p->prog, "u_xf");
        p->u_fog = glGetUniformLocation(p->prog, "u_fog");
        p->cloc = (GLint *)malloc(192 * sizeof(GLint));
        if (p->cloc) {
            for (i = 0; i < 192; i++) {
                char name[12];
                snprintf(name, sizeof name, "c[%u]", i);
                p->cloc[i] = glGetUniformLocation(p->prog, name);
            }
        }
        p->const_synced = 0;
    }
    s_st.compiles++;
    return p;
}

/* ---- fixed-function state ----------------------------------------------- */

static GLenum blend_factor(uint32_t f, int src)
{
    switch (f) {
    case 0x0000: case 0x0001:
    case 0x0300: case 0x0301: case 0x0302: case 0x0303:
    case 0x0304: case 0x0305: case 0x0306: case 0x0307:
    case 0x8001: case 0x8002: case 0x8003: case 0x8004:
        return (GLenum)f;
    case 0x0308:
        return src ? GL_SRC_ALPHA_SATURATE : GL_ONE;
    default:
        return f ? GL_ONE : GL_ZERO;
    }
}

static GLenum compare_func(uint32_t f)
{
    return f >= 0x200 && f <= 0x207 ? (GLenum)f : GL_ALWAYS;
}

static GLenum stencil_op(uint32_t op)
{
    switch (op) {
    case 0: case 0x1E00: case 0x1E01: case 0x1E02: case 0x1E03:
    case 0x150A: case 0x8507: case 0x8508:
        return (GLenum)op;
    default:
        return GL_KEEP;
    }
}

static void apply_state(const Nv2aRenderState *rs, uint32_t shade, uint32_t bpp,
                        int zeta16)
{
    uint32_t cm = rs->color_mask;

    if (rs->blend_enable && !(rs->blend_src == 1 && rs->blend_dst == 0)) {
        GLenum eq;
        float bc[4];
        switch (rs->blend_eq) {
        case 0x800A: case 0x800B: case 0x8007: case 0x8008:
            eq = (GLenum)rs->blend_eq;
            break;
        default:
            eq = GL_FUNC_ADD;
        }
        glEnable(GL_BLEND);
        glBlendFunc(blend_factor(rs->blend_src, 1), blend_factor(rs->blend_dst, 0));
        glBlendEquation(eq);
        unpack4(rs->blend_color, bc);
        glBlendColor(bc[0], bc[1], bc[2], bc[3]);
    } else {
        glDisable(GL_BLEND);
    }
    if (shade == NV2A_SHADE_REPLACE
        || !(rs->depth_test_enable || rs->depth_write)) {
        glDisable(GL_DEPTH_TEST);
    } else {
        /* GL writes depth only with the test on: ALWAYS stands in for "off". */
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(rs->depth_test_enable ? compare_func(rs->depth_func) : GL_ALWAYS);
        glDepthMask(rs->depth_write ? GL_TRUE : GL_FALSE);
    }
    if (rs->stencil_enable && shade != NV2A_SHADE_REPLACE) {
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(compare_func(rs->stencil_func), (GLint)(rs->stencil_ref & 0xFF),
                      rs->stencil_func_mask & 0xFF);
        glStencilOp(stencil_op(rs->stencil_fail), stencil_op(rs->stencil_zfail),
                    stencil_op(rs->stencil_zpass));
        glStencilMask(rs->stencil_write_mask & 0xFF);
    } else {
        glDisable(GL_STENCIL_TEST);
    }
    /* Culling as the title set it. Rows map one to one (guest row y = GL
     * row y), which mirrors the picture vertically in GL's window space, so
     * a triangle the title sees clockwise winds counter-clockwise here: the
     * front face is the opposite of SET_FRONT_FACE. Strips arrive with one
     * winding (NV2A_BACKEND_CAP_CULL). */
    if (s_cull && rs->cull_enable
        && (rs->cull_face == GL_FRONT || rs->cull_face == GL_BACK
            || rs->cull_face == GL_FRONT_AND_BACK)) {
        glEnable(GL_CULL_FACE);
        glCullFace((GLenum)rs->cull_face);
        glFrontFace(rs->front_face == GL_CCW ? GL_CW : GL_CCW);
    } else {
        glDisable(GL_CULL_FACE);
    }
    /* Polygon offset (shadow-map passes push their depth back). The units
     * count the zeta format's steps; the depth buffer is D24S8 whatever the
     * format, so a Z16 surface's step is 256 of ours. */
    if (rs->poly_offset_fill && shade != NV2A_SHADE_REPLACE) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(rs->poly_offset_factor,
                        rs->poly_offset_units * (zeta16 ? 256.0f : 1.0f));
    } else {
        glDisable(GL_POLYGON_OFFSET_FILL);
    }
    /* A 16-bit surface has no alpha: keep the target's at 1, as the CPU
     * path reads it back. */
    glColorMask((cm & 0x00010000u) != 0, (cm & 0x00000100u) != 0,
                (cm & 0x00000001u) != 0, bpp == 4 && (cm & 0x01000000u) != 0);
}

static float zmax_of(uint32_t format)
{
    return ((format >> 4) & 0xF) == 1 ? 65535.0f : 16777215.0f;
}

/* ---- visibility queries ------------------------------------------------- */

/* One occlusion query per counting batch, in a ring in submission order.
 * ticket 0: the open count (since the last zpass_end); zpass_end stamps
 * them with its ticket and zpass_resolve takes a ticket's queries off the
 * head once GL has their results. Query objects are recycled. Past the
 * ring's size a batch is not counted (the report is then a lower bound). */
#define NV_GL_MAX_QUERY 8192        /* power of two */
typedef struct {
    GLuint q;
    uint32_t ticket;
} GlQuery;
static GlQuery  s_qring[NV_GL_MAX_QUERY];
static uint32_t s_qhead, s_qcount, s_qopen;   /* s_qopen: ticket-0 entries at the tail */
static GLuint   s_qfree[NV_GL_MAX_QUERY];
static uint32_t s_nqfree;
static uint32_t s_ticket;

static GLuint query_new(void)
{
    GLuint q = 0;

    if (s_nqfree)
        return s_qfree[--s_nqfree];
    glGenQueries(1, &q);
    return q;
}

static void query_pop(void)
{
    s_qfree[s_nqfree++] = s_qring[s_qhead].q;
    s_qhead = (s_qhead + 1) & (NV_GL_MAX_QUERY - 1);
    s_qcount--;
}

/* Synchronous: every query queued so far, waited for (RECOMP_GPU_ZPASS=sync). */
static uint32_t gl_zpass_read(void)
{
    uint32_t sum = 0;

    if (s_init <= 0)
        return 0;
    while (s_qcount) {
        GLuint r = 0;
        glGetQueryObjectuiv(s_qring[s_qhead].q, GL_QUERY_RESULT, &r);
        sum += r;
        query_pop();
    }
    s_qopen = 0;
    return sum;
}

static uint32_t gl_zpass_end(void)
{
    uint32_t i;

    if (++s_ticket == 0)
        s_ticket = 1;
    for (i = s_qcount - s_qopen; i < s_qcount; i++)
        s_qring[(s_qhead + i) & (NV_GL_MAX_QUERY - 1)].ticket = s_ticket;
    s_qopen = 0;
    return s_ticket;
}

static int gl_zpass_resolve(uint32_t ticket, int block, uint32_t *count)
{
    uint32_t sum = 0, n = 0, i;

    *count = 0;
    if (s_init <= 0)
        return 1;
    /* The ticket's queries: the head of the ring (tickets resolve in
     * order). Not blocking: all of them must have their result. */
    while (n < s_qcount - s_qopen
           && s_qring[(s_qhead + n) & (NV_GL_MAX_QUERY - 1)].ticket == ticket)
        n++;
    if (!block)
        for (i = n; i-- > 0; ) {
            GLuint avail = 0;
            glGetQueryObjectuiv(s_qring[(s_qhead + i) & (NV_GL_MAX_QUERY - 1)].q,
                                GL_QUERY_RESULT_AVAILABLE, &avail);
            if (!avail)
                return 0;
        }
    for (i = 0; i < n; i++) {
        GLuint r = 0;
        glGetQueryObjectuiv(s_qring[s_qhead].q, GL_QUERY_RESULT, &r);
        sum += r;
        query_pop();
    }
    *count = sum;
    return 1;
}

/* ---- the back end ------------------------------------------------------- */

static void gl_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
                     uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    GlTarget t;
    GLbitfield mask = 0;

    if (!ensure_init() || !bind_target(s, rs->zeta_va, &t))
        return;
    if (t.rt) {
        /* A clear of every channel over the whole guest surface makes the
         * old bytes moot: no seed upload, and where the API has it the
         * driver is told the attachment's old contents are dead (a tiler
         * then neither loads nor keeps them). */
        int whole = s->clip_x == 0 && s->clip_y == 0
                 && s->width >= t.rt->w && s->height >= t.rt->h;
        int all = (flags & 0x70) == 0x70 && (t.bpp == 2 || (flags & 0x80));
        if (whole && all && t.rt->seed) {
            t.rt->seed = 0;
            s_st.seeds_skipped++;
        }
        rt_seed(t.rt);
    }
    if (s_invalidate) {
        GLenum att[2];
        GLsizei n = 0;
        if (t.rt && (flags & 0x70) == 0x70 && (t.bpp == 2 || (flags & 0x80)))
            att[n++] = GL_COLOR_ATTACHMENT0;
        if ((flags & 0x03) == 0x03)
            att[n++] = GL_DEPTH_STENCIL_ATTACHMENT;
        if (n) {
            nv2a_gl_invalidate_rect(n, att, (GLint)s->clip_x, (GLint)s->clip_y,
                                    (GLsizei)s->width, (GLsizei)s->height);
            s_st.invalidates++;
        }
    }
    if ((flags & 0xF0) && t.rt) {
        float c[4];
        unpack4(argb, c);
        if (t.bpp == 2)
            c[3] = 1.0f;
        glColorMask((flags & 0x10) != 0, (flags & 0x20) != 0, (flags & 0x40) != 0,
                    t.bpp == 2 || (flags & 0x80) != 0);
        glClearColor(c[0], c[1], c[2], c[3]);
        mask |= GL_COLOR_BUFFER_BIT;
    }
    if (flags & 0x01) {
        /* zclear_value(): Z16 in the low 16 bits, Z24 in the top 24. */
        float z = ((s->format >> 4) & 0xF) == 1 ? (float)(zstencil & 0xFFFF) / 65535.0f
                                                : (float)(zstencil >> 8) / 16777215.0f;
        glDepthMask(GL_TRUE);
#if NV2A_GL_ES
        glClearDepthf(z);
#else
        glClearDepth(z);
#endif
        mask |= GL_DEPTH_BUFFER_BIT;
    }
    if (flags & 0x02) {
        glStencilMask(0xFF);
        glClearStencil((GLint)(zstencil & 0xFF));
        mask |= GL_STENCIL_BUFFER_BIT;
    }
    if (mask)
        glClear(mask);
    if ((mask & GL_COLOR_BUFFER_BIT) && t.rt) {
        t.rt->dirty = 1;
        t.rt->serial = ++s_rt_serial;
    }
    s_st.clears++;
}

/* The batch's textures, stage by stage: kind[] for the shader key, tscale
 * the texel-to-normalised scale per stage. Before the target is bound:
 * sampling a surface the GPU drew may read it back, which rebinds the
 * framebuffer. */
/* The colour target each stage samples directly this batch (NULL: none). */
static GlRT *s_stage_rt[4];

/* Texel coordinates (linear formats, and every format on the screen-space
 * path) rather than normalised ones. */
static int tex_coords_in_texels(const Nv2aPixelState *ps, const Nv2aTexture *t)
{
    return ps->shade == NV2A_SHADE_REPLACE
        || !(d3d8_format_is_swizzled(t->color) || d3d8_format_dxt_block_bytes(t->color));
}

/* Sample colour target r on `unit` as texture t (rt_for_texture). The GL
 * texture can be larger than the guest texture (a linear target is as wide
 * as its pitch): coordinates scale to the target's size. */
static void rt_bind_as_texture(GlRT *r, const Nv2aPixelState *ps, const Nv2aTexture *t,
                               int unit, GLint alpha, float *tscale)
{
    rt_seed(r);
    glActiveTexture(GL_TEXTURE0 + (GLenum)unit);
    glBindTexture(GL_TEXTURE_2D, r->tex);
    samp_set(GL_TEXTURE_2D, &r->samp, tex_filter(t, ps->shade),
             wrap_mode(t->addr_u, (int)t->border_color),
             wrap_mode(t->addr_v, (int)t->border_color), t->border, alpha);
    if (tex_coords_in_texels(ps, t)) {
        tscale[0] = 1.0f / (float)r->gw;
        tscale[1] = 1.0f / (float)r->gh;
    } else {
        tscale[0] = (float)t->width / (float)r->gw;
        tscale[1] = (float)t->height / (float)r->gh;
    }
    r->sampled = s_frame;
    s_bound[unit] = NULL;
    s_stage_rt[unit] = r;
    s_st.rt_direct++;
}

/* A depth texture format (a shadow map): its depth range's kind, else 0. */
static uint32_t shadow_kind(uint32_t color)
{
    switch (color) {
    case 0x2C: case 0x2D: case 0x30: case 0x31:   /* SZ/LU DEPTH_Y16 fixed/float */
        return NV2A_GL_TEX_SHADOW16;
    case 0x2A: case 0x2B: case 0x2E: case 0x2F:   /* SZ/LU DEPTH_X8_Y24 fixed/float */
        return NV2A_GL_TEX_SHADOW24;
    default:
        return 0;
    }
}

/* A stage reading a depth format in PROJECT2D/3D mode whose address is a
 * zeta surface the GPU holds: bind that depth texture with a depth compare
 * (SET_SHADOW_DEPTH_FUNC). Coordinates scale to the GL texture's size. */
static int bind_shadow(const Nv2aPixelState *ps, const Nv2aTexture *t, int unit,
                       float *tscale)
{
    GlDepth *d = NULL;
    /* The NV2A compares "stored depth <func> r/q" (xemu psh.c); GL compares
     * "r <func> stored depth": the operands swap, so LESS reads as GREATER,
     * LEQUAL as GEQUAL and back (Fable's GREATER: lit where r/q < stored). */
    static const GLint swapped[8] = { GL_NEVER, GL_GREATER, GL_EQUAL, GL_GEQUAL,
                                      GL_LESS, GL_NOTEQUAL, GL_LEQUAL, GL_ALWAYS };
    GLint cmp = swapped[ps->shadow_func & 7];
    int i;

    for (i = 0; i < NV_GL_MAX_DEPTH; i++)
        if (s_depth[i].tex && s_depth[i].va == t->offset)
            d = &s_depth[i];
    if (!d || t->width > d->w || t->height > d->h)
        return 0;
    glActiveTexture(GL_TEXTURE0 + (GLenum)unit);
    glBindTexture(GL_TEXTURE_2D, d->tex);
    samp_set(GL_TEXTURE_2D, &d->samp, tex_filter(t, ps->shade),
             wrap_mode(t->addr_u, (int)t->border_color),
             wrap_mode(t->addr_v, (int)t->border_color), t->border, GL_ALPHA);
    if (d->cmp != cmp) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, cmp);
        d->cmp = cmp;
    }
    if (d3d8_format_is_swizzled(t->color)) {
        tscale[0] = (float)t->width / (float)d->w;
        tscale[1] = (float)t->height / (float)d->h;
    } else {
        tscale[0] = 1.0f / (float)d->w;
        tscale[1] = 1.0f / (float)d->h;
    }
    d->sampled = s_frame;
    s_bound[unit] = NULL;
    s_st.shadow_binds++;
    if (s_verbose) {
        static int said;
        if (said++ < 2)
            fprintf(stderr, "  [GPUGL] shadow map 0x%08X stage %d: %ux%u pitch %u fmt 0x%X func %u"
                    " filter 0x%X addr %u/%u border %u/0x%08X, depth %ux%u\n",
                    t->offset, unit, t->width, t->height, t->pitch, t->color, ps->shadow_func,
                    t->filter, t->addr_u, t->addr_v, t->border_color, t->border, d->w, d->h);
    }
    return 1;
}

static void bind_stage_textures(const Nv2aPixelState *ps, uint32_t kind[4], float tscale[8])
{
    uint32_t st;

    for (st = 0; st < 4; st++) {
        const Nv2aTexStage *ts = &ps->stage[st];
        uint32_t mode = ts->mode, cube = 0;
        GlTex *e;
        GlRT *r;
        GLint alpha;
        kind[st] = NV2A_GL_TEX_NONE;
        tscale[st * 2] = tscale[st * 2 + 1] = 1.0f;
        s_stage_rt[st] = NULL;
        /* Combiners: 2D, 3D, cube, and bump-env (6, 7) from stage 1 on. */
        if (ps->shade == NV2A_SHADE_COMBINERS
            ? !((mode >= 1 && mode <= 3) || (st && (mode == 6 || mode == 7)))
            : (st || !mode))
            continue;
        if (!ts->valid)
            continue;
        if (ps->shade == NV2A_SHADE_COMBINERS && mode == 3 && ts->tex.cube)
            cube = 1;
        if (ps->shade == NV2A_SHADE_COMBINERS && (mode == 1 || mode == 2)
            && shadow_kind(ts->tex.color)) {
            /* A shadow map. Not drawn by the GPU: white, as before (the
             * decoder knows no depth format). */
            if (bind_shadow(ps, &ts->tex, (int)st, &tscale[st * 2]))
                kind[st] = shadow_kind(ts->tex.color);
            continue;
        }
        if ((r = rt_for_texture(&ts->tex, cube, &alpha)) != NULL) {
            rt_bind_as_texture(r, ps, &ts->tex, (int)st, alpha, &tscale[st * 2]);
            kind[st] = NV2A_GL_TEX_2D;
            continue;
        }
        e = tex_get(&ts->tex, cube, (int)st);
        if (!e)
            continue;
        tex_bind(e, &ts->tex, (int)st, ps->shade);
        kind[st] = cube ? NV2A_GL_TEX_CUBE : NV2A_GL_TEX_2D;
        /* Texel coordinates to normalised. */
        if (tex_coords_in_texels(ps, &ts->tex)) {
            tscale[st * 2] = 1.0f / (float)ts->tex.width;
            tscale[st * 2 + 1] = 1.0f / (float)ts->tex.height;
        }
    }
}

/* A batch that samples the target it draws into (a feedback loop GL leaves
 * undefined): those stages read a copy of the target taken before the
 * batch, which is what the hardware's texture cache sees in practice. */
static void break_feedback(GlRT *target)
{
    int st;

    if (!target)
        return;
    for (st = 0; st < 4; st++) {
        GlRT *r = s_stage_rt[st];
        if (r != target)
            continue;
        glActiveTexture(GL_TEXTURE0 + (GLenum)st);
        if (!r->copy || r->copy_w != r->gw || r->copy_h != r->gh) {
            if (!r->copy)
                glGenTextures(1, &r->copy);
            glBindTexture(GL_TEXTURE_2D, r->copy);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)r->gw, (GLsizei)r->gh, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
            r->copy_w = r->gw;
            r->copy_h = r->gh;
            r->copy_samp.wrap_s = r->copy_samp.wrap_t = -1;
            r->copy_samp.filter = r->copy_samp.swz_a = -1;
        } else {
            glBindTexture(GL_TEXTURE_2D, r->copy);
        }
        /* The target's framebuffer is bound for drawing (and reading). */
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, (GLsizei)r->gw, (GLsizei)r->gh);
        samp_set(GL_TEXTURE_2D, &r->copy_samp, r->samp.filter, r->samp.wrap_s,
                 r->samp.wrap_t, r->samp.border, r->samp.swz_a);
        s_st.rt_copies++;
    }
}

/* The fragment shader's uniforms (combiner constants, fog colour, alpha
 * reference, texture scales). */
static void set_pixel_uniforms(const GlProg *p, const Nv2aPixelState *ps,
                               const Nv2aRenderState *rs, const float tscale[8])
{
    float c0[32], c1[32], v[4];

    glUniform2fv(p->u_tscale, 4, tscale);
    if (ps->shade == NV2A_SHADE_COMBINERS) {
        int i;
        for (i = 0; i < 8; i++) {
            unpack4(ps->rc.factor0[i], &c0[i * 4]);
            unpack4(ps->rc.factor1[i], &c1[i * 4]);
        }
        glUniform4fv(p->u_c0, 8, c0);
        glUniform4fv(p->u_c1, 8, c1);
        unpack4(ps->rc.final_c0, v);
        glUniform4fv(p->u_fc0, 1, v);
        unpack4(ps->rc.final_c1, v);
        glUniform4fv(p->u_fc1, 1, v);
        unpack4(ps->fog_color, v);
        glUniform4fv(p->u_fogc, 1, v);
        if (p->u_bm >= 0 || p->u_bso >= 0) {
            float bm[16], bso[8];
            for (i = 0; i < 4; i++) {
                memcpy(&bm[i * 4], ps->stage[i].bump_mat, sizeof ps->stage[i].bump_mat);
                bso[i * 2] = ps->stage[i].bump_scale;
                bso[i * 2 + 1] = ps->stage[i].bump_offset;
            }
            if (p->u_bm >= 0)
                glUniform4fv(p->u_bm, 4, bm);
            if (p->u_bso >= 0)
                glUniform2fv(p->u_bso, 4, bso);
        }
    }
    glUniform1f(p->u_aref, (float)(rs->alpha_ref & 0xFF));
}

/* Near plane and depth mapping. CPU-transformed triangles arrive clipped:
 * depth clamp on (where it exists), depth range 0..1. A translated vertex
 * program clips at z >= SET_CLIP_MIN itself: on desktop GL with
 * gl_ClipDistance[0] (depth clamp stays on, nothing clips at the far
 * plane, as on the CPU path); on GLES depth clamp goes off so GL's own near
 * plane is there. Either way glDepthRange(zmin/zmax, 1) keeps window depth
 * at z/zmax (nv2a_gl_vsh.h). */
static int s_near_program;          /* set_near_mode's current state */
static float s_near01;

static void set_near_mode(int program, float near01)
{
    int cur_program = s_near_program;
    float cur_near = s_near01;

    if (program == cur_program && near01 == cur_near)
        return;
    if (program != cur_program) {
#if NV2A_GL_ES
        if (s_depth_clamp) {
            if (program)
                glDisable(GL_DEPTH_CLAMP);
            else
                glEnable(GL_DEPTH_CLAMP);
        }
#else
        if (program)
            glEnable(GL_CLIP_DISTANCE0);
        else
            glDisable(GL_CLIP_DISTANCE0);
#endif
    }
#if NV2A_GL_ES
    glDepthRangef(near01, 1.0f);
#else
    glDepthRange((double)near01, 1.0);
#endif
    s_near_program = program;
    s_near01 = near01;
}

static void gl_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    const Nv2aPixelState *ps = b->pixel;
    const Nv2aRenderState *rs = b->state;
    uint32_t kind[4];
    float tscale[8], v[4];
    Nv2aGlShaderKey key;
    GlProg *p;
    GlTarget t;
    int query = 0;

    if (!b->full || !ps || b->count < 3)
        return;
    if (!ensure_init())
        return;
    bind_stage_textures(ps, kind, tscale);
    nv2a_gl_shader_key(ps, rs, kind, &key);
    if (key.bump_src[1] | key.bump_src[2] | key.bump_src[3])
        s_st.bump_draws++;
    p = prog_get(&key, NULL);
    if (!p)
        return;
    if (!bind_target(s, rs->zeta_va, &t))
        return;
    rt_seed(t.rt);
    break_feedback(t.rt);
    apply_state(rs, ps->shade, t.bpp, ((s->format >> 4) & 0xF) == 1);
    set_near_mode(0, 0.0f);
    glUseProgram(p->prog);
    v[0] = 2.0f / (float)t.w;
    v[1] = 2.0f / (float)t.h;
    v[2] = 2.0f / zmax_of(s->format);
    v[3] = s_depth_clamp ? 0.0f : 1.0f;    /* vertex-shader clamp instead */
    glUniform4fv(p->u_vp, 1, v);
    set_pixel_uniforms(p, ps, rs, tscale);

    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)b->count * (GLsizeiptr)sizeof(Nv2aVertexFull),
                 b->full, GL_STREAM_DRAW);
    if (rs->zpass_enable && s_qcount < NV_GL_MAX_QUERY) {
        GlQuery *q = &s_qring[(s_qhead + s_qcount) & (NV_GL_MAX_QUERY - 1)];
        q->q = query_new();
        q->ticket = 0;
        glBeginQuery(NV2A_GL_ES ? GL_ANY_SAMPLES_PASSED : GL_SAMPLES_PASSED, q->q);
        query = 1;
    }
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)b->count);
    if (query) {
        glEndQuery(NV2A_GL_ES ? GL_ANY_SAMPLES_PASSED : GL_SAMPLES_PASSED);
        s_qcount++;
        s_qopen++;
    }
    /* Only a batch that writes colour makes the target worth reading back. */
    if (t.rt && (rs->color_mask & 0x01010101u)) {
        t.rt->dirty = 1;
        t.rt->serial = ++s_rt_serial;
    }
    s_st.draws++;
    s_st.tris += b->count / 3;
}

/* Upload what changed in c[] since this program last drew (nv2a_backend.h:
 * stamps), as runs of consecutive uniform locations. */
static void sync_constants(GlProg *p, const Nv2aProgramBatch *pb)
{
    uint32_t i, lo = 192, hi = 0;

    if (p->const_synced >= pb->const_serial)
        return;
    for (i = 0; i < 192; i++)
        if (pb->const_stamp[i] > p->const_synced) {
            if (lo == 192)
                lo = i;
            hi = i;
        }
    for (i = lo; i <= hi && i < 192;) {
        uint32_t j = i + 1;
        if (p->cloc[i] < 0) {
            i++;
            continue;
        }
        while (j <= hi && p->cloc[j] == p->cloc[j - 1] + 1)
            j++;
        glUniform4fv(p->cloc[i], (GLsizei)(j - i), pb->constants[i]);
        i = j;
    }
    p->const_synced = pb->const_serial;
}

static GLuint s_pvao, s_pvbo, s_pebo;

/* A vertex-program batch (NV2A_BACKEND_CAP_PROGRAM): the program runs as a
 * GLSL vertex shader (nv2a_gl_vsh.c) on the gathered attributes; the pixel
 * side is gl_draw's. Declines (0) only when the program cannot be compiled
 * or a cache is full: the executor then transforms on the CPU. */
static int gl_draw_program(const Nv2aSurface *s, const Nv2aBatch *b)
{
    const Nv2aProgramBatch *pb = b->program;
    const Nv2aPixelState *ps = b->pixel;
    const Nv2aRenderState *rs = b->state;
    uint32_t kind[4], a, k, nstream = 0;
    float tscale[8], v[4], zmax, zmin;
    Nv2aGlShaderKey key;
    GlVsh *vsh;
    GlProg *p;
    GlTarget t;
    int query = 0;

    if (!pb || !ps || !ensure_init())
        return 0;
    if (!(vsh = vsh_get(pb)))
        return 0;
    bind_stage_textures(ps, kind, tscale);
    nv2a_gl_shader_key(ps, rs, kind, &key);
    if (key.bump_src[1] | key.bump_src[2] | key.bump_src[3])
        s_st.bump_draws++;
    p = prog_get(&key, vsh);
    if (!p || !p->cloc)
        return 0;
    if (!bind_target(s, rs->zeta_va, &t))
        return 1;                   /* gl_draw drops these too */
    rt_seed(t.rt);
    break_feedback(t.rt);
    apply_state(rs, ps->shade, t.bpp, ((s->format >> 4) & 0xF) == 1);
    zmax = zmax_of(s->format);
    zmin = rs->depth_min;
    if (!(zmin >= 0.0f && zmin < zmax))
        zmin = 0.0f;
    set_near_mode(1, zmin / zmax);
    glUseProgram(p->prog);
    v[0] = 2.0f / (float)t.w;
    v[1] = 2.0f / (float)t.h;
    v[2] = 2.0f / (zmax - zmin);
    v[3] = 0.0f;                    /* GL clips near and far itself (set_near_mode) */
    glUniform4fv(p->u_vp, 1, v);
    v[0] = pb->aa_sx;
    v[1] = pb->aa_sy;
    v[2] = zmin;
    glUniform4fv(p->u_xf, 1, v);
    nv2a_gl_vsh_fog_uniform(pb->fog_enable, pb->fog_mode, pb->fog_param[0],
                            pb->fog_param[1], v);
    glUniform4fv(p->u_fog, 1, v);
    set_pixel_uniforms(p, ps, rs, tscale);
    sync_constants(p, pb);

    if (!s_pvao) {
        glGenVertexArrays(1, &s_pvao);
        glGenBuffers(1, &s_pvbo);
        glGenBuffers(1, &s_pebo);
    }
    glBindVertexArray(s_pvao);
    for (a = 0; a < 16; a++)
        nstream += (pb->streams >> a) & 1;
    glBindBuffer(GL_ARRAY_BUFFER, s_pvbo);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)((size_t)pb->vertex_count * nstream * 4 * sizeof(float)),
                 pb->attrs, GL_STREAM_DRAW);
    for (a = 0, k = 0; a < 16; a++) {
        if (pb->streams & (1u << a)) {
            glEnableVertexAttribArray(a);
            glVertexAttribPointer(a, 4, GL_FLOAT, GL_FALSE,
                                  (GLsizei)(nstream * 4 * sizeof(float)),
                                  (const void *)((size_t)k * 4 * sizeof(float)));
            k++;
        } else {
            glDisableVertexAttribArray(a);
            glVertexAttrib4fv(a, pb->imm[a]);
        }
    }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_pebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(pb->index_count * sizeof(uint16_t)),
                 pb->indices, GL_STREAM_DRAW);
    if (rs->zpass_enable && s_qcount < NV_GL_MAX_QUERY) {
        GlQuery *q = &s_qring[(s_qhead + s_qcount) & (NV_GL_MAX_QUERY - 1)];
        q->q = query_new();
        q->ticket = 0;
        glBeginQuery(NV2A_GL_ES ? GL_ANY_SAMPLES_PASSED : GL_SAMPLES_PASSED, q->q);
        query = 1;
    }
    glDrawElements(GL_TRIANGLES, (GLsizei)pb->index_count, GL_UNSIGNED_SHORT, NULL);
    if (query) {
        glEndQuery(NV2A_GL_ES ? GL_ANY_SAMPLES_PASSED : GL_SAMPLES_PASSED);
        s_qcount++;
        s_qopen++;
    }
    /* Only a batch that writes colour makes the target worth reading back. */
    if (t.rt && (rs->color_mask & 0x01010101u)) {
        t.rt->dirty = 1;
        t.rt->serial = ++s_rt_serial;
    }
    s_st.draws++;
    s_st.vsh_draws++;
    s_st.vsh_kb += (uint32_t)(((size_t)pb->vertex_count * nstream * 16
                               + pb->index_count * 2) >> 10);
    s_st.tris += pb->index_count / 3;
    return 1;
}

/* ---- context loss and GL_OUT_OF_MEMORY ---------------------------------- */

/* Forget every GL object the back end holds and start again at the next
 * call: ensure_init re-creates the shared objects, the caches fill again
 * from guest memory (targets are seeded from the guest bytes, which the
 * caller has just read back when the context still lives). lost: the context is gone, so
 * its objects are only forgotten, not deleted. Every cache of GL names
 * belongs here. */
static void gl_reset_objects(int lost)
{
    int i;

    if (!lost) {
        for (i = 0; i < NV_GL_MAX_RT; i++) {
            if (s_rt[i].fbo) glDeleteFramebuffers(1, &s_rt[i].fbo);
            if (s_rt[i].tex) glDeleteTextures(1, &s_rt[i].tex);
            if (s_rt[i].copy) glDeleteTextures(1, &s_rt[i].copy);
        }
        for (i = 0; i < NV_GL_MAX_DEPTH; i++) {
            if (s_depth[i].fbo) glDeleteFramebuffers(1, &s_depth[i].fbo);
            if (s_depth[i].tex) glDeleteTextures(1, &s_depth[i].tex);
        }
        for (i = 0; i < NV_GL_MAX_TEX; i++)
            if (s_tex[i].tex) glDeleteTextures(1, &s_tex[i].tex);
        for (i = 0; i < NV_GL_MAX_PROG; i++)
            if (s_prog[i].prog) glDeleteProgram(s_prog[i].prog);
        for (i = 0; i < NV_GL_MAX_VSH; i++)
            if (s_vsh[i].sh) glDeleteShader(s_vsh[i].sh);
        if (s_pbo) glDeleteBuffers(1, &s_pbo);
        if (s_pvbo) glDeleteBuffers(1, &s_pvbo);
        if (s_pebo) glDeleteBuffers(1, &s_pebo);
        if (s_pvao) glDeleteVertexArrays(1, &s_pvao);
        for (i = 0; (uint32_t)i < s_qcount; i++)
            glDeleteQueries(1, &s_qring[(s_qhead + (uint32_t)i) & (NV_GL_MAX_QUERY - 1)].q);
        if (s_nqfree) glDeleteQueries((GLsizei)s_nqfree, s_qfree);
        if (s_vbo) glDeleteBuffers(1, &s_vbo);
        if (s_vao) glDeleteVertexArrays(1, &s_vao);
        if (s_vs) glDeleteShader(s_vs);
        glFinish();
        while (glGetError() != GL_NO_ERROR)
            ;
    }
    memset(s_rt, 0, sizeof s_rt);
    memset(s_depth, 0, sizeof s_depth);
    memset(s_tex, 0, sizeof s_tex);
    memset(s_bound, 0, sizeof s_bound);
    memset(s_stage_rt, 0, sizeof s_stage_rt);
    s_ntex = 0;
    for (i = 0; i < NV_GL_MAX_PROG; i++)
        free(s_prog[i].cloc);
    memset(s_prog, 0, sizeof s_prog);
    memset(s_vsh, 0, sizeof s_vsh);
    s_pvao = s_pvbo = s_pebo = 0;
    s_pbo = 0;
    s_pbo_size = 0;
    memset(&s_pend, 0, sizeof s_pend);
    s_near_program = 0;             /* ensure_init: depth clamp on, range 0..1 */
    s_near01 = 0.0f;
    s_target_fbo = 0;
    /* Visibility queries: dropped; reports still pending in the executor
     * resolve with what was counted (gl_zpass_resolve answers 0 for a
     * ticket whose queries are gone). */
    s_qhead = s_qcount = s_qopen = s_nqfree = 0;
    s_vbo = s_vao = s_vs = 0;
    s_init = 0;
}

/* At the flip: drain glGetError, and on a lost context (GPU reset, the
 * Android driver dropping it) or GL_OUT_OF_MEMORY forget all objects and
 * re-create them lazily. Out of memory leaves GL state undefined, and a
 * texture or target that failed to allocate would otherwise stay in its
 * cache as incomplete. After NV_GL_MAX_RESETS the CPU rasteriser takes
 * over for the rest of the run. */
#define NV_GL_MAX_RESETS 4
static int pbo_complete(Nv2aPresentSurface *done);
static void gl_check_frame(void)
{
    static int said, resets;
    int lost, oom = 0, n = 0;
    GLenum e;

    if (s_init <= 0)
        return;
    lost = nv2a_gl_context_lost();
    {
        /* RECOMP_GPU_FAKE_RESET=<frame>[,oom]: act as if the context were
         * lost (or out of memory) at that frame, to exercise this path on
         * a host whose driver never resets. */
        static long at = -2;
        static int fake_oom;
        if (at == -2) {
            const char *f = getenv("RECOMP_GPU_FAKE_RESET");
            at = f && *f ? strtol(f, NULL, 10) : -1;
            fake_oom = f && strstr(f, "oom") != NULL;
        }
        if (at >= 0 && s_frame == (uint32_t)at) {
            if (fake_oom)
                oom = 1;
            else
                lost = 1;
        }
    }
    while (!lost && n++ < 16 && (e = glGetError()) != GL_NO_ERROR) {
        if (e == GL_OUT_OF_MEMORY)
            oom = 1;
        else if (e == GL_CONTEXT_LOST)
            lost = 1;
        else if (said++ < 8)
            fprintf(stderr, "  [GPUGL] GL error 0x%X during frame %u\n", e, s_frame);
    }
    if (!lost && !oom)
        return;
    fprintf(stderr, "  [GPUGL] %s at frame %u: dropping every GL object%s\n",
            lost ? "context lost" : "GL_OUT_OF_MEMORY", s_frame,
            resets + 1 < NV_GL_MAX_RESETS ? ", re-created as needed"
                                          : "; the CPU rasteriser takes over");
    if (!lost) {
        /* Out of memory, the context lives: what only the GPU holds goes to
         * guest memory first, so the targets reseed from it. */
        Nv2aPresentSurface done;
        int i;
        (void)pbo_complete(&done);
        for (i = 0; i < NV_GL_MAX_RT; i++)
            if (s_rt[i].tex)
                rt_readback(&s_rt[i]);
    }
    gl_reset_objects(lost);
    if (lost)
        nv2a_gl_context_destroy();
    if (++resets >= NV_GL_MAX_RESETS) {
        s_init = -1;
        unregister_self();
    }
}

/* ---- present ------------------------------------------------------------ */

/* The present read-back in flight, into guest memory now (its surface in
 * *done). 0: none was pending. */
static int pbo_complete(Nv2aPresentSurface *done)
{
    const uint8_t *p;
    size_t n;

    if (!s_pend.pending)
        return 0;
    s_pend.pending = 0;
    *done = s_pend.surf;
    if (s_pend.dropped || !s_pbo)
        return 1;                     /* guest memory already holds newer pixels */
    n = (size_t)s_pend.snap.w * s_pend.snap.h * 4;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, s_pbo);
    p = (const uint8_t *)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)n,
                                          GL_MAP_READ_BIT);
    if (p) {
        rt_store(&s_pend.snap, p);
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    return 1;
}

/* Start reading target r back into the pack buffer; pbo_complete finishes. */
static void pbo_issue(GlRT *r, const Nv2aPresentSurface *surf)
{
    size_t n = (size_t)r->w * r->h * 4;

    if (!s_pbo)
        glGenBuffers(1, &s_pbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, s_pbo);
    if (n > s_pbo_size) {
        glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)n, NULL, GL_STREAM_READ);
        s_pbo_size = n;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
    glReadPixels(0, 0, (GLsizei)r->w, (GLsizei)r->h, NV_GL_READ_FORMAT, NV_GL_READ_TYPE, NULL);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    s_pend.pending = 1;
    s_pend.dropped = 0;
    s_pend.snap = *r;
    s_pend.surf = *surf;
    r->dirty = 0;
    s_st.readbacks++;
}

/* The flip's present (nv2a_backend_register_with_present): only the
 * presented surface comes back to guest memory. Other targets stay on the
 * GPU until something needs their bytes: a texture the GPU cannot sample
 * them as (rt_for_texture fails: another format or shape), their eviction
 * or a change of their bytes per pixel, a reset of the GL objects (out of
 * memory), or RECOMP_GPU_READBACK=all. */
static int gl_present(const Nv2aPresentSurface *now, int sync, Nv2aPresentSurface *ready)
{
    Nv2aPresentSurface prev;
    GlRT *r = NULL;
    int i, have_prev;

    *ready = *now;
    if (s_init <= 0)
        return 1;                     /* the CPU's bytes are all there is */
    for (i = 0; i < NV_GL_MAX_RT; i++) {
        if (s_rt[i].tex && s_rt[i].va == now->va)
            r = &s_rt[i];
        else if (s_readback_all && s_rt[i].tex && s_rt[i].dirty)
            rt_readback(&s_rt[i]);
    }
    have_prev = pbo_complete(&prev);
    if (!r)
        return 1;                     /* not a GPU target: guest memory is current */
    if (sync || s_present_sync_mode) {
        rt_readback(r);
        return 1;
    }
    if (!r->dirty) {
        /* Not drawn since its last read-back: guest memory holds it. */
        return 1;
    }
    pbo_issue(r, now);
    if (!have_prev)
        return 0;                     /* the pipeline starts: show nothing new yet */
    *ready = prev;
    return 1;
}

static void gl_flip(void)
{
    s_frame++;
    s_st.frames++;
    if (s_init > 0 && (s_verbose ? s_frame % 60 == 0 : s_frame % 600 == 0)) {
        int i, nrt = 0, nprog = 0;
        for (i = 0; i < NV_GL_MAX_RT; i++)
            nrt += s_rt[i].tex != 0;
        for (i = 0; i < NV_GL_MAX_PROG; i++)
            nprog += s_prog[i].prog != 0;
        fprintf(stderr, "  [GPUGL] frame %u: %u draws (%u vertex programs, %u KB),"
                        " %u tris, %u clears, %u readbacks,"
                        " targets: %u created, %u relayouts, %u grown, %u sampled directly"
                        " (%u copies), %u shadow maps, %u bump-env, %u seeded, %u seeds skipped, %u invalidates,"
                        " %u uploads (%u KB), %u KB hashed, %d textures,"
                        " %d targets, %d programs (%u compiled),"
                        " %.1f target switches/frame\n",
                s_frame, s_st.draws, s_st.vsh_draws, s_st.vsh_kb, s_st.tris,
                s_st.clears, s_st.readbacks, s_st.rt_creates, s_st.relayouts, s_st.rt_grows,
                s_st.rt_direct, s_st.rt_copies, s_st.shadow_binds, s_st.bump_draws, s_st.seeds,
                s_st.seeds_skipped, s_st.invalidates,
                s_st.uploads, s_st.upload_kb, s_st.hashes_kb, s_ntex, nrt, nprog,
                s_st.compiles,
                s_st.frames ? (double)s_st.target_switches / s_st.frames : 0.0);
        memset(&s_st, 0, sizeof s_st);
    }
    gl_check_frame();
}

static Nv2aBackend s_backend = {
    gl_clear, gl_draw, gl_flip,
    NV2A_BACKEND_CAP_PIXEL | NV2A_BACKEND_CAP_CULL | NV2A_BACKEND_CAP_PROGRAM,
    gl_zpass_read, gl_zpass_end, gl_zpass_resolve, gl_draw_program
};

static void unregister_self(void)
{
    nv2a_backend_register(NULL);
}

int nv2a_gl_install_from_env(void)
{
    const char *e = getenv("RECOMP_GPU");

    if (!e || !*e || strcmp(e, "0") == 0)
        return 0;
    /* RECOMP_GPU_CULL=0: draw both faces (and strips as the CPU path
     * assembles them). Read here: the caps are part of the registration. */
    e = getenv("RECOMP_GPU_CULL");
    s_cull = !(e && strcmp(e, "0") == 0);
    if (!s_cull)
        s_backend.caps &= ~NV2A_BACKEND_CAP_CULL;
    e = getenv("RECOMP_GPU");
    nv2a_backend_register_with_present(&s_backend, gl_present);
    fprintf(stderr, "  [GPUGL] GPU back end selected (RECOMP_GPU=%s); the context is"
                    " created on the executor thread at the first draw\n", e);
    return 1;
}
