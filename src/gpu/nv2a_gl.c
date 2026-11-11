/**
 * GPU render back end for the pushbuffer executor: OpenGL 3.3 core / 4.1
 * core (macOS) / GLES 3.0 (Android), one code path. XBA-27 stage 7b.
 *
 * The executor still decodes the pushbuffer and runs vertex programs on the
 * CPU; what moves to the GPU is everything per pixel -- rasterisation,
 * texturing, the register combiners, alpha test, depth, stencil and
 * blending -- which is where the software renderer spends its time.
 *
 *   render targets  one RGBA8 texture + FBO per colour surface, created from
 *                   the guest bytes; depth/stencil renderbuffers per zeta
 *                   surface. Pixels go back to guest memory only when
 *                   someone else needs them: at the flip (the presenter and
 *                   RECOMP_FB_DUMP read guest memory) and when a batch
 *                   samples a surface the GPU drew into (render to texture).
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
 * present sync below has read the frame back. Presenting the FBO texture
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
static uint32_t s_frame;
static GLuint s_vao, s_vbo, s_vs;

static struct {
    uint32_t draws, tris, clears, uploads, upload_kb, readbacks, compiles, hashes_kb;
} s_st;

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
    }
    s_verbose = env_on("RECOMP_GPU_VERBOSE");
    {
        const char *a = getenv("RECOMP_TEX_ANISO");
        s_bilinear = env_on("RECOMP_TEX_BILINEAR")
                  || (a && (strcmp(a, "8") == 0 || strcmp(a, "16") == 0));
        a = getenv("RECOMP_GPU_TEX_RECHECK");
        s_recheck_frame = a && strcmp(a, "frame") == 0;
    }
    fprintf(stderr, "  [GPUGL] S3TC %s, depth clamp %s, border clamp %s\n",
            s_s3tc ? "on" : "off (DXT decoded on the CPU)",
            s_depth_clamp ? "on" : "off (clamped in the vertex shader)",
            s_border_clamp ? "on" : "off (clamp to edge)");

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

typedef struct {
    uint32_t va, pitch, bpp, w, h, swz;   /* swz: 0, or 1 | log2w << 8 | log2h << 16 */
    GLuint tex, fbo, depth;               /* depth: renderbuffer attached now */
    int dirty;
    uint32_t used;                        /* frame last drawn into */
} GlRT;

typedef struct {
    uint32_t va, w, h;
    GLuint rb;
} GlDepth;

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

/* The render target -> guest bytes, in the surface's own format and layout
 * (565 and 8888 the way put_pixel writes them, swizzled when the surface
 * is). */
static void rt_readback(GlRT *r)
{
    uint8_t *buf, *mem;
    uint32_t x, y;

    if (!r->dirty || !r->fbo)
        return;
    buf = scratch((size_t)r->w * r->h * 4);
    if (!buf)
        return;
    glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
    glReadPixels(0, 0, (GLsizei)r->w, (GLsizei)r->h, GL_RGBA, GL_UNSIGNED_BYTE, buf);
    mem = guest(r->va);
    for (y = 0; y < r->h; y++) {
        const uint8_t *row = buf + (size_t)y * r->w * 4;
        if (r->bpp == 4 && !r->swz) {
            uint32_t *o = (uint32_t *)(mem + (size_t)y * r->pitch);
            for (x = 0; x < r->w; x++, row += 4)
                o[x] = ((uint32_t)row[3] << 24) | ((uint32_t)row[0] << 16)
                     | ((uint32_t)row[1] << 8) | row[2];
            continue;
        }
        for (x = 0; x < r->w; x++, row += 4) {
            uint8_t *p = mem + rt_offset(r, x, y);
            if (r->bpp == 4) {
                uint32_t c = ((uint32_t)row[3] << 24) | ((uint32_t)row[0] << 16)
                           | ((uint32_t)row[1] << 8) | row[2];
                memcpy(p, &c, 4);
            } else {
                uint16_t c = (uint16_t)(((row[0] >> 3) << 11) | ((row[1] >> 2) << 5)
                                        | (row[2] >> 3));
                memcpy(p, &c, 2);
            }
        }
    }
    r->dirty = 0;
    s_st.readbacks++;
}

static void rt_free(GlRT *r)
{
    rt_readback(r);
    if (r->fbo) glDeleteFramebuffers(1, &r->fbo);
    if (r->tex) glDeleteTextures(1, &r->tex);
    memset(r, 0, sizeof *r);
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
            if (r->bpp == bpp && r->swz == swz && r->pitch == s->pitch
                && r->w >= w && r->h >= h)
                return r;
            /* Same memory, another shape: hand the old one back first. */
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
    r->w = w;
    r->h = h;
    r->swz = swz;
    glGenTextures(1, &r->tex);
    glBindTexture(GL_TEXTURE_2D, r->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    rt_upload(r);
    glGenFramebuffers(1, &r->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->tex, 0);
    r->depth = 0;
    if (s_verbose)
        fprintf(stderr, "  [GPUGL] render target 0x%08X %ux%u bpp %u%s\n",
                r->va, w, h, bpp, swz ? " swizzled" : "");
    return r;
}

/* Depth/stencil for zeta surface `va`, at least w x h. The CPU rasteriser
 * keeps one float buffer per zeta address; so does this, as D24S8. */
static GLuint depth_get(uint32_t va, uint32_t w, uint32_t h)
{
    int i, slot = -1;

    for (i = 0; i < NV_GL_MAX_DEPTH; i++) {
        GlDepth *d = &s_depth[i];
        if (d->rb && d->va == va) {
            if (d->w >= w && d->h >= h)
                return d->rb;
            glDeleteRenderbuffers(1, &d->rb);
            memset(d, 0, sizeof *d);
            slot = i;
            break;
        }
        if (!d->rb && slot < 0)
            slot = i;
    }
    if (slot < 0) {
        static int next;
        slot = next++ % NV_GL_MAX_DEPTH;
        glDeleteRenderbuffers(1, &s_depth[slot].rb);
    }
    s_depth[slot].va = va;
    s_depth[slot].w = w < 640 ? 640 : w;
    s_depth[slot].h = h < 480 ? 480 : h;
    glGenRenderbuffers(1, &s_depth[slot].rb);
    glBindRenderbuffer(GL_RENDERBUFFER, s_depth[slot].rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8,
                          (GLsizei)s_depth[slot].w, (GLsizei)s_depth[slot].h);
    return s_depth[slot].rb;
}

/* Bind the render target for drawing into, with its depth surface. */
static GlRT *bind_target(const Nv2aSurface *s, uint32_t zeta_va)
{
    GlRT *r = rt_get(s);
    GLuint d;

    if (!r)
        return NULL;
    glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
    d = depth_get(zeta_va, r->w, r->h);
    if (d != r->depth) {
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                  GL_RENDERBUFFER, d);
        r->depth = d;
    }
    glViewport(0, 0, (GLsizei)r->w, (GLsizei)r->h);
    glEnable(GL_SCISSOR_TEST);
    glScissor((GLint)s->clip_x, (GLint)s->clip_y, (GLsizei)s->width, (GLsizei)s->height);
    r->used = s_frame;
    return r;
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

/* ---- textures ----------------------------------------------------------- */

typedef struct {
    uint32_t offset, color, width, height, pitch, palette, cube;   /* key */
    uint32_t bytes;
    uint64_t hash;
    GLuint tex;
    uint32_t checked;               /* frame of the last hash check */
    GLint wrap_s, wrap_t, filter;   /* sampler state last set */
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
    e->wrap_s = e->wrap_t = e->filter = -1;
    s_st.uploads++;
    return ok;
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
        /* Re-check the bytes once per frame, and whenever this unit sampled
         * something else in between (a glyph cache written between two text
         * batches). */
        if (e->checked == s_frame && (s_recheck_frame || s_bound[unit] == e))
            return e;
        readback_overlapping(t->offset, e->bytes);
        e->checked = s_frame;
        {
            uint64_t h = tex_hash(t, e->bytes);
            if (h == e->hash)
                return e;
            e->hash = h;
        }
        return tex_upload(e, t) ? e : NULL;
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
    e->hash = tex_hash(t, bytes);
    e->checked = s_frame;
    s_ntex++;
    if (!tex_upload(e, t)) {
        /* Keep the entry (bytes != 0, tex set) so a format the decoder does
         * not know is not retried every batch; drawn as "no texture". */
        return NULL;
    }
    return e;
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

/* Filtering the way the CPU rasteriser chooses it: the combiner path
 * honours the title's TENT filters (rc_texel), the two simpler paths take
 * the nearest texel unless RECOMP_TEX_BILINEAR. Level 0 only on both. */
static void tex_bind(GlTex *e, const Nv2aTexture *t, int unit, uint32_t shade)
{
    GLenum target = e->cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
    uint32_t mag = (t->filter >> 24) & 0xF, min = (t->filter >> 16) & 0xFF;
    int lin = s_bilinear
           || (shade == NV2A_SHADE_COMBINERS
               && (mag == 2 || min == 2 || min == 4 || min == 6));
    GLint f = lin ? GL_LINEAR : GL_NEAREST;
    GLint ws = wrap_mode(t->addr_u, (int)t->border_color);
    GLint wt = wrap_mode(t->addr_v, (int)t->border_color);

    glActiveTexture(GL_TEXTURE0 + (GLenum)unit);
    glBindTexture(target, e->tex);
    if (e->filter != f) {
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, f);
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, f);
        e->filter = f;
    }
    if (e->wrap_s != ws || e->wrap_t != wt) {
        glTexParameteri(target, GL_TEXTURE_WRAP_S, ws);
        glTexParameteri(target, GL_TEXTURE_WRAP_T, wt);
        if (target == GL_TEXTURE_CUBE_MAP)
            glTexParameteri(target, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        e->wrap_s = ws;
        e->wrap_t = wt;
        if (ws == GL_CLAMP_TO_BORDER || wt == GL_CLAMP_TO_BORDER) {
            float bc[4];
            unpack4(t->border, bc);
            glTexParameterfv(target, GL_TEXTURE_BORDER_COLOR, bc);
        }
    }
    s_bound[unit] = e;
}

/* ---- shader programs ---------------------------------------------------- */

typedef struct {
    Nv2aGlShaderKey key;
    uint64_t hash;
    GLuint prog;                    /* 0 with `failed`: do not retry */
    int failed;
    GLint u_vp, u_tscale, u_c0, u_c1, u_fc0, u_fc1, u_fogc, u_aref;
} GlProg;

#define NV_GL_MAX_PROG 512          /* power of two */
static GlProg s_prog[NV_GL_MAX_PROG];

static GlProg *prog_get(const Nv2aGlShaderKey *key)
{
    static char body[32768];
    uint64_t h = nv2a_gl_shader_key_hash(key);
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
        if (c->hash == h && memcmp(&c->key, key, sizeof *key) == 0)
            return c->failed ? NULL : c;
    }
    if (!p)
        return NULL;                /* ponytail: 512 pipelines, then no more */
    p->key = *key;
    p->hash = h;
    if (!nv2a_gl_fragment_body(key, body, sizeof body)
        || !(fs = compile(GL_FRAGMENT_SHADER, body))) {
        p->failed = 1;
        return NULL;
    }
    p->prog = glCreateProgram();
    glAttachShader(p->prog, s_vs);
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

static void apply_state(const Nv2aRenderState *rs, uint32_t shade, uint32_t bpp)
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

#define NV_GL_MAX_QUERY 256
static GLuint s_query[NV_GL_MAX_QUERY];
static int s_nquery;

static uint32_t gl_zpass_read(void)
{
    uint32_t sum = 0;
    int i;

    if (s_init <= 0)
        return 0;
    for (i = 0; i < s_nquery; i++) {
        GLuint r = 0;
        glGetQueryObjectuiv(s_query[i], GL_QUERY_RESULT, &r);
        sum += r;
    }
    if (s_nquery)
        glDeleteQueries(s_nquery, s_query);
    s_nquery = 0;
    return sum;
}

/* ---- the back end ------------------------------------------------------- */

static void gl_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
                     uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    GlRT *r;
    GLbitfield mask = 0;

    if (!ensure_init() || !(r = bind_target(s, rs->zeta_va)))
        return;
    if (flags & 0xF0) {
        float c[4];
        unpack4(argb, c);
        if (r->bpp == 2)
            c[3] = 1.0f;
        glColorMask((flags & 0x10) != 0, (flags & 0x20) != 0, (flags & 0x40) != 0,
                    r->bpp == 2 || (flags & 0x80) != 0);
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
    if (mask & GL_COLOR_BUFFER_BIT)
        r->dirty = 1;
    s_st.clears++;
}

static void gl_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    const Nv2aPixelState *ps = b->pixel;
    const Nv2aRenderState *rs = b->state;
    uint32_t kind[4] = {0, 0, 0, 0}, st;
    float tscale[8], c0[32], c1[32], v[4];
    Nv2aGlShaderKey key;
    GlProg *p;
    GlRT *r;
    int query = 0;

    if (!b->full || !ps || b->count < 3)
        return;
    if (!ensure_init())
        return;
    /* Textures first: sampling a surface the GPU drew may read it back,
     * which rebinds the framebuffer. */
    for (st = 0; st < 4; st++) {
        const Nv2aTexStage *ts = &ps->stage[st];
        uint32_t mode = ts->mode, cube = 0;
        GlTex *e;
        tscale[st * 2] = tscale[st * 2 + 1] = 1.0f;
        if (ps->shade == NV2A_SHADE_COMBINERS ? !(mode >= 1 && mode <= 3) : (st || !mode))
            continue;
        if (!ts->valid)
            continue;
        if (ps->shade == NV2A_SHADE_COMBINERS && mode == 3 && ts->tex.cube)
            cube = 1;
        e = tex_get(&ts->tex, cube, (int)st);
        if (!e)
            continue;
        tex_bind(e, &ts->tex, (int)st, ps->shade);
        kind[st] = cube ? NV2A_GL_TEX_CUBE : NV2A_GL_TEX_2D;
        /* Texel coordinates (linear formats, and every format on the
         * screen-space path) to normalised. */
        if (ps->shade == NV2A_SHADE_REPLACE
            || !(d3d8_format_is_swizzled(ts->tex.color)
                 || d3d8_format_dxt_block_bytes(ts->tex.color))) {
            tscale[st * 2] = 1.0f / (float)ts->tex.width;
            tscale[st * 2 + 1] = 1.0f / (float)ts->tex.height;
        }
    }
    nv2a_gl_shader_key(ps, rs, kind, &key);
    p = prog_get(&key);
    if (!p)
        return;
    if (!(r = bind_target(s, rs->zeta_va)))
        return;
    apply_state(rs, ps->shade, r->bpp);
    glUseProgram(p->prog);
    v[0] = 2.0f / (float)r->w;
    v[1] = 2.0f / (float)r->h;
    v[2] = 2.0f / zmax_of(s->format);
    v[3] = s_depth_clamp ? 0.0f : 1.0f;    /* vertex-shader clamp instead */
    glUniform4fv(p->u_vp, 1, v);
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
    }
    glUniform1f(p->u_aref, (float)(rs->alpha_ref & 0xFF));

    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)b->count * (GLsizeiptr)sizeof(Nv2aVertexFull),
                 b->full, GL_STREAM_DRAW);
    if (rs->zpass_enable && s_nquery < NV_GL_MAX_QUERY) {
        glGenQueries(1, &s_query[s_nquery]);
        glBeginQuery(NV2A_GL_ES ? GL_ANY_SAMPLES_PASSED : GL_SAMPLES_PASSED,
                     s_query[s_nquery]);
        query = 1;
    }
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)b->count);
    if (query) {
        glEndQuery(NV2A_GL_ES ? GL_ANY_SAMPLES_PASSED : GL_SAMPLES_PASSED);
        s_nquery++;
    }
    r->dirty = 1;
    s_st.draws++;
    s_st.tris += b->count / 3;
}

/* ---- context loss and GL_OUT_OF_MEMORY ---------------------------------- */

/* Forget every GL object the back end holds and start again at the next
 * call: ensure_init re-creates the shared objects, the caches fill again
 * from guest memory (targets are seeded from the guest bytes, which the
 * present sync has just brought up to date). lost: the context is gone, so
 * its objects are only forgotten, not deleted. Every cache of GL names
 * belongs here. */
static void gl_reset_objects(int lost)
{
    int i;

    if (!lost) {
        for (i = 0; i < NV_GL_MAX_RT; i++) {
            if (s_rt[i].fbo) glDeleteFramebuffers(1, &s_rt[i].fbo);
            if (s_rt[i].tex) glDeleteTextures(1, &s_rt[i].tex);
        }
        for (i = 0; i < NV_GL_MAX_DEPTH; i++)
            if (s_depth[i].rb) glDeleteRenderbuffers(1, &s_depth[i].rb);
        for (i = 0; i < NV_GL_MAX_TEX; i++)
            if (s_tex[i].tex) glDeleteTextures(1, &s_tex[i].tex);
        for (i = 0; i < NV_GL_MAX_PROG; i++)
            if (s_prog[i].prog) glDeleteProgram(s_prog[i].prog);
        if (s_nquery) glDeleteQueries(s_nquery, s_query);
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
    s_ntex = 0;
    memset(s_prog, 0, sizeof s_prog);
    s_nquery = 0;
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
    gl_reset_objects(lost);
    if (lost)
        nv2a_gl_context_destroy();
    if (++resets >= NV_GL_MAX_RESETS) {
        s_init = -1;
        unregister_self();
    }
}

/* Before the flip's window snapshot and frame dump: every surface the GPU
 * drew into this frame goes back to guest memory. */
static void gl_present_sync(void)
{
    int i;

    if (s_init <= 0)
        return;
    for (i = 0; i < NV_GL_MAX_RT; i++)
        if (s_rt[i].tex && s_rt[i].dirty)
            rt_readback(&s_rt[i]);
}

static void gl_flip(void)
{
    s_frame++;
    if (s_init > 0 && (s_verbose ? s_frame % 60 == 0 : s_frame % 600 == 0)) {
        int i, nrt = 0, nprog = 0;
        for (i = 0; i < NV_GL_MAX_RT; i++)
            nrt += s_rt[i].tex != 0;
        for (i = 0; i < NV_GL_MAX_PROG; i++)
            nprog += s_prog[i].prog != 0;
        fprintf(stderr, "  [GPUGL] frame %u: %u draws, %u tris, %u clears, %u readbacks,"
                        " %u uploads (%u KB), %u KB hashed, %d textures, %d targets,"
                        " %d programs (%u compiled)\n",
                s_frame, s_st.draws, s_st.tris, s_st.clears, s_st.readbacks,
                s_st.uploads, s_st.upload_kb, s_st.hashes_kb, s_ntex, nrt, nprog,
                s_st.compiles);
        memset(&s_st, 0, sizeof s_st);
    }
    gl_check_frame();
}

static const Nv2aBackend s_backend = {
    gl_clear, gl_draw, gl_flip, NV2A_BACKEND_CAP_PIXEL, gl_zpass_read
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
    nv2a_backend_register_with_present_sync(&s_backend, gl_present_sync);
    fprintf(stderr, "  [GPUGL] GPU back end selected (RECOMP_GPU=%s); the context is"
                    " created on the executor thread at the first draw\n", e);
    return 1;
}
