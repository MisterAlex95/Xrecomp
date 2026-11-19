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
 * first call and stays current on that thread. The one other thread that
 * touches GL is the shader worker (XBA-310, "programs compiled off the draw
 * path" below): a context of its own in the same share group, which only
 * compiles and links programs (and stores their binaries) and hands them
 * over through the job table; everything else stays on the executor.
 * The SDL presenter keeps its own renderer on the main thread and is fed
 * exactly as with the CPU rasteriser -- through guest memory, after the
 * present hook below (gl_present) has read the frame back -- or, with host
 * present (RECOMP_GPU_PRESENT=host, or RECOMP_GPU_SCALE above 1, windowed),
 * with the target read at its real size into a pack buffer and handed over
 * as Nv2aPresentSurface.host_px. Zero-copy present (stage 7e, XBA-308,
 * default on in a window): the context is created in the window's share
 * group (nv2a_gl_present_share), and each flip hands the window the frame
 * blitted into a texture of its back slot (src/video/fb_gpu_frame.h) instead
 * of pixels; the read-back paths above stay for what needs the pixels.
 *
 * Selected with RECOMP_GPU=1 (nv2a_gl_install_from_env, called by the
 * presenter's xbox_HostWindowMain); the default on Android (XBA-119), where
 * RECOMP_GPU=0 keeps the CPU rasteriser, which stays the default elsewhere.
 * If no context can be created (no EGL display, no GLES 3.0 config, a
 * context the driver refuses) the back end unregisters itself and the CPU
 * rasteriser draws.
 */
#include "nv2a_gl.h"
#include "nv2a_gl_api.h"
#include "nv2a_gl_shader.h"
#include "nv2a_gl_vsh.h"
#include "nv2a_gl_scale.h"
#include "nv2a_backend.h"
#include "d3d/d3d8_swizzle.h"
#include "host_shader_progress.h"   /* the window's shader badge (XBA-314) */
#include "video/gpu_ring.h"
#include "video/fb_gpu_frame.h"   /* zero-copy present (XBA-308) */
#ifdef XBOXRECOMP_LSFG
#include "framegen/framegen_policy.h"   /* frame generation forces sync present */
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>                 /* truncate */

extern ptrdiff_t xbox_GetMemoryOffset(void);

static int s_init;                 /* 0 not tried, 1 ready, -1 failed */
static int s_s3tc, s_depth_clamp, s_verbose;
static int s_border_clamp;         /* GL_CLAMP_TO_BORDER usable */
static int s_bilinear;             /* RECOMP_TEX_BILINEAR / _ANISO, as the CPU path */
static int s_recheck_frame;        /* RECOMP_GPU_TEX_RECHECK=frame */
static int s_recheck_switch;       /* RECOMP_GPU_TEX_RECHECK=switch: the old policy */
static int s_cull = 1;             /* RECOMP_GPU_CULL (default on) */
static int s_cpu_writes = 1;       /* RECOMP_GPU_CPU_WRITES (default on): rt_cpu_check */
static float s_point_max = 1.0f;   /* the point size range's top */
static float s_line_max = 1.0f;    /* GL_ALIASED_LINE_WIDTH_RANGE's top (GLES only) */
static int s_mips = 1;             /* RECOMP_GPU_MIPS (default on): every mip level */
static float s_lodb[4];            /* each stage's LOD bias this batch (u_lodb) */
static uint32_t s_frame;
static GLuint s_vao, s_vbo, s_vs;

static struct {
    uint32_t draws, tris, clears, uploads, upload_kb, readbacks, compiles, hashes_kb;
    uint32_t vsh_draws, vsh_kb;     /* vertex-program batches, their upload */
    uint32_t ffp_kb;                /* CPU-transformed vertices uploaded (gl_draw) */
    uint32_t rt_creates, relayouts, rt_direct, rt_copies, rt_grows, shadow_binds;
    uint32_t seeds, seeds_skipped, invalidates;
    uint32_t bump_draws;            /* batches with a bump-env stage */
    uint32_t target_switches;       /* draw/clear framebuffer changes (bind_target) */
    uint32_t frames;                /* flips since the line was last printed */
    uint32_t cache_loads;           /* programs from the binary cache */
    /* RECOMP_GPU_VBO: batches drawn from cached guest vertex buffers, bytes
     * uploaded into them and hashed to validate them, buffers created,
     * buffers whose bytes changed, vertex arrays created. */
    uint32_t vbo_draws, vbo_kb, vbo_hash_kb, vbo_new, vbo_changed, vao_new;
    uint32_t ib_new, ib_kb;         /* index lists cached, their bytes */
    uint32_t cpu_merges, cpu_kpx;   /* guest CPU writes merged into targets, kilopixels */
    uint32_t points, lines;         /* point and line batches (XBA-120) */
    /* Streamed vertex and index uploads (strm_put): wraps, waits for the
     * GPU at a wrap and their time, rings grown, uploads too large for any
     * ring, maps that failed (glBufferSubData instead). */
    uint32_t strm_wraps, strm_waits, strm_grows, strm_oversize, strm_mapfail;
    uint64_t strm_wait_ns, strm_wait_max_ns;
    /* glUniform* calls issued and left out because the program already
     * held the values (us_same, sync_constants); bytes hashed, exact (the
     * _kb counters drop what is under 1 KB a call): textures (tex_hash),
     * vertex ranges and index lists (vb_hash), the rest (hash_bytes). */
    uint32_t uni_calls, uni_skipped;
    uint64_t hash_tex_b, hash_vtx_b, hash_other_b;
} s_st;
static float s_line_width = 1.0f;  /* glLineWidth as last set */

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
static void nv2a_gl_invalidate(GLsizei n, const GLenum *att)
{
#if NV2A_GL_ES || !defined(__APPLE__)
    glInvalidateFramebuffer(GL_FRAMEBUFFER, n, att);
#else
    (void)n; (void)att;
#endif
}
/* RECOMP_GPU_PRESENT=sync: read the presented surface back at its own flip
 * (the executor waits for the GPU to finish the frame); default async: one
 * frame later through a pixel pack buffer. RECOMP_GPU_READBACK=all: also
 * every other target drawn since the last flip, as before 7d (for a title
 * whose CPU reads its render targets). */
static int s_present_sync_mode, s_readback_all;
/* Host present: the presented target goes to the window at its real GL size
 * (Nv2aPresentSurface.host_px), read one frame late into a pack buffer of
 * its own, never through guest memory. On with RECOMP_GPU_PRESENT=host, or
 * with an internal resolution above 1 (s_scale), and only while a window
 * shows frames (nv2a_gl_set_display_size was called). The native copy into
 * guest memory then happens only with RECOMP_GPU_READBACK=present (every
 * flip, as before) or =all. */
static int s_present_host_env, s_readback_present;
/* The window's drawable in real pixels (nv2a_gl_set_display_size), 0 x 0
 * headless. Set on the main thread before the guest (and so the executor)
 * starts. */
static uint32_t s_display_w, s_display_h;
/* Internal resolution, RECOMP_GPU_SCALE else RECOMP_UPSCALE (scale_from_env, at ensure_init):
 * 1 to 4, what the frame-buffer class of targets is drawn at (WP-S1). */
static float s_scale = 1.0f;

void nv2a_gl_set_display_size(uint32_t w, uint32_t h)
{
    s_display_w = w;
    s_display_h = h;
}
/* Internal resolution (RECOMP_GPU_SCALE, nv2a_gl_scale.h; gpu-backend.md
 * "Internal resolution"): the frame-buffer class of targets is drawn at
 * s_scale_cfg.scale times its guest size. s_scale_resolve (default on,
 * RECOMP_GPU_SCALE_RESOLVE=0 off): a native target sampling a scaled one
 * reads a native-size resolve of it rather than the scaled texture (which
 * it would under-filter). s_scale_dump (RECOMP_GPU_SCALE_DUMP=1): a dumping
 * flip also writes the presented target at its real size. */
static Nv2aGlScaleConfig s_scale_cfg = { 1.0f, 0, 0, { 0 }, { 0 }, 0 };
static int s_scale_resolve = 1, s_scale_dump;
/* The scale of the target the batch being set up draws into (known before
 * its textures are bound: surface_scale). */
static float s_draw_sc = 1.0f;
static struct {
    uint32_t resolves, rescales, depth_recreates, seed_blits;
} s_sst;

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
    s_st.hash_other_b += n;
    return h;
}

/* The same idea in four independent lanes: one multiply chain is latency
 * bound, and textures (every sampled one hashed once a frame) and vertex
 * ranges (~3 MB a frame in Oakvale) are the bulk of what is hashed. No
 * statistics: the callers count. */
static uint64_t hash4_bytes(const uint8_t *p, size_t n)
{
    uint64_t h0 = 0x9E3779B97F4A7C15ull ^ n, h1 = 0xC2B2AE3D27D4EB4Full,
             h2 = 0x165667B19E3779F9ull, h3 = 0x27D4EB2F165667C5ull, w[4];
    size_t i;

    for (i = 0; i + 32 <= n; i += 32) {
        memcpy(w, p + i, 32);
        h0 = (h0 ^ w[0]) * 0xFF51AFD7ED558CCDull; h0 ^= h0 >> 29;
        h1 = (h1 ^ w[1]) * 0xFF51AFD7ED558CCDull; h1 ^= h1 >> 29;
        h2 = (h2 ^ w[2]) * 0xFF51AFD7ED558CCDull; h2 ^= h2 >> 29;
        h3 = (h3 ^ w[3]) * 0xFF51AFD7ED558CCDull; h3 ^= h3 >> 29;
    }
    for (; i < n; i++)
        h0 = (h0 ^ p[i]) * 0x100000001B3ull;
    return h0 ^ (h1 * 31u) ^ (h2 * 961u) ^ (h3 * 29791u);
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

/* ---- GL state shadow ---------------------------------------------------- */

/* The GL state the back end sets per batch, as last set: a call that would
 * set what is already there is skipped. Each draw used to rebind the
 * framebuffer and set viewport, scissor, enables, blend/depth/stencil
 * state, program, vertex array and 16 attribute states whether or not they
 * changed, about 30 calls a batch for Fable's 1090 batches a frame; on a
 * mobile driver each costs validation on the one thread that feeds the GPU.
 * Every change of this state in this file goes through these helpers (or
 * marks the entry unknown); gl_reset_objects forgets all of it, and a
 * deleted object's bindings are forgotten with it. All bytes 0xFF means
 * unknown: no GL name or enum is 0xFFFFFFFF, and the floats are NaN, which
 * compares unequal to everything. */
enum { SH_BLEND, SH_DEPTH, SH_STENCIL, SH_CULL, SH_POLY, SH_SCISSOR, SH_NCAP };
#define SH_UNITS 4
static struct {
    GLuint fbo, prog, vao, array_buf;
    GLint vp[4], sc[4];
    GLint unit;                             /* active texture unit, 0-based */
    GLuint tex[SH_UNITS][3];                /* per unit: 2D, cube, 3D */
    GLint cap[SH_NCAP];
    GLenum blend_src, blend_dst, blend_eq;
    float blend_color[4];
    GLenum depth_func;
    GLint depth_mask;
    GLenum st_func, st_fail, st_zfail, st_zpass;
    GLint st_ref;
    GLuint st_mask, st_write;
    GLenum cull_face, front_face;
    float po_factor, po_units;
    GLint cmask;                            /* r | g << 1 | b << 2 | a << 3 */
    /* s_pvao (gl_draw_program): attribute layout (streams mask), element
     * buffer bound, generic values of the disabled attributes. */
    uint32_t pv_streams, pv_ebo;
    size_t pv_off;                          /* where the vertices start in s_pvbo */
    float pv_imm[16][4];
} s_sh;

static void sh_forget(void)
{
    memset(&s_sh, 0xFF, sizeof s_sh);
}

static void sh_fbo(GLuint f)
{
    if (s_sh.fbo != f) {
        glBindFramebuffer(GL_FRAMEBUFFER, f);
        s_sh.fbo = f;
    }
}

static void sh_viewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
    if (s_sh.vp[0] != x || s_sh.vp[1] != y || s_sh.vp[2] != w || s_sh.vp[3] != h) {
        glViewport(x, y, w, h);
        s_sh.vp[0] = x; s_sh.vp[1] = y; s_sh.vp[2] = w; s_sh.vp[3] = h;
    }
}

static void sh_scissor(GLint x, GLint y, GLsizei w, GLsizei h)
{
    if (s_sh.sc[0] != x || s_sh.sc[1] != y || s_sh.sc[2] != w || s_sh.sc[3] != h) {
        glScissor(x, y, w, h);
        s_sh.sc[0] = x; s_sh.sc[1] = y; s_sh.sc[2] = w; s_sh.sc[3] = h;
    }
}

static void sh_cap(int i, GLenum cap, int on)
{
    if (s_sh.cap[i] != on) {
        if (on)
            glEnable(cap);
        else
            glDisable(cap);
        s_sh.cap[i] = on;
    }
}

static void sh_program(GLuint p)
{
    if (s_sh.prog != p) {
        glUseProgram(p);
        s_sh.prog = p;
    }
}

static void sh_vao(GLuint v)
{
    if (s_sh.vao != v) {
        glBindVertexArray(v);
        s_sh.vao = v;
    }
}

static void sh_array_buffer(GLuint b)
{
    if (s_sh.array_buf != b) {
        glBindBuffer(GL_ARRAY_BUFFER, b);
        s_sh.array_buf = b;
    }
}

static void sh_active(int unit)
{
    if (s_sh.unit != unit) {
        glActiveTexture(GL_TEXTURE0 + (GLenum)unit);
        s_sh.unit = unit;
    }
}

/* Bind on the active unit (only units 0-3, the ones stages sample, are
 * shadowed). For sampling: the caller has made the stage's unit active. */
static void sh_tex(GLenum target, GLuint t)
{
    int k = target == GL_TEXTURE_CUBE_MAP ? 1 : target == GL_TEXTURE_3D ? 2 : 0;

    if (s_sh.unit >= 0 && s_sh.unit < SH_UNITS) {
        if (s_sh.tex[s_sh.unit][k] == t)
            return;
        s_sh.tex[s_sh.unit][k] = t;
    }
    glBindTexture(target, t);
}

/* Bind for creating, uploading to or copying into a texture, on unit
 * SH_UNITS, which no stage samples. A batch's textures are bound before its
 * target (bind_stage_textures, then bind_target), and binding there on the
 * active unit -- the stage bound last -- would make that stage sample the
 * new texture: Burnout 3's back buffer, re-created at another scale while
 * stage 1 held its bloom texture, became the bloom, a frame fed back into
 * itself every frame (the white centre at scale 2). Every path that can run
 * between the stage binds and the draw binds here: target creation and
 * growth, depth creation, seeding, resolves, texture uploads. A stage bind
 * that follows makes its own unit active again (sh_active). */
static void sh_tex_scratch(GLenum target, GLuint t)
{
    sh_active(SH_UNITS);
    sh_tex(target, t);
}

/* Deleting an object unbinds it wherever it is bound (GL reverts those
 * bindings to 0): forget them, a new object may get the same name. */
static void sh_delete_textures(GLsizei n, const GLuint *t)
{
    GLsizei i;
    int u, k;

    for (i = 0; i < n; i++)
        for (u = 0; u < SH_UNITS; u++)
            for (k = 0; k < 3; k++)
                if (s_sh.tex[u][k] == t[i])
                    s_sh.tex[u][k] = 0xFFFFFFFFu;
    glDeleteTextures(n, t);
}

static void sh_delete_fbo(GLuint f)
{
    if (s_sh.fbo == f)
        s_sh.fbo = 0xFFFFFFFFu;
    glDeleteFramebuffers(1, &f);
}

static void sh_color_mask(int r, int g, int b, int a)
{
    GLint m = (r != 0) | (g != 0) << 1 | (b != 0) << 2 | (a != 0) << 3;

    if (s_sh.cmask != m) {
        glColorMask(r != 0, g != 0, b != 0, a != 0);
        s_sh.cmask = m;
    }
}

/* Colour blit from framebuffer `src` (sw x sh at the origin) to `dst` (dw x
 * dh at the origin): scaled-target resolves and seeding. The scissor test
 * (and on some drivers the colour mask) applies to blits: both go to "all"
 * for the blit and back afterwards, and the framebuffer bound before is
 * bound again, so a blit between bind_target and the draw (seeding) leaves
 * the batch's state as it was. */
static void sh_blit_rect(GLuint src, const GLint sr[4], GLuint dst, const GLint dr[4],
                         GLenum filter)
{
    GLuint fbo = s_sh.fbo;
    GLint scissor = s_sh.cap[SH_SCISSOR], cm = s_sh.cmask;

    sh_cap(SH_SCISSOR, GL_SCISSOR_TEST, 0);
    sh_color_mask(1, 1, 1, 1);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst);
    glBlitFramebuffer(sr[0], sr[1], sr[2], sr[3], dr[0], dr[1], dr[2], dr[3],
                      GL_COLOR_BUFFER_BIT, filter);
    s_sh.fbo = 0xFFFFFFFFu;
    if (fbo != 0xFFFFFFFFu)
        sh_fbo(fbo);
    if (scissor == 1)
        sh_cap(SH_SCISSOR, GL_SCISSOR_TEST, 1);
    if (cm >= 0 && cm <= 15)
        sh_color_mask(cm & 1, cm & 2, cm & 4, cm & 8);
}

static void sh_blit(GLuint src, GLint sw, GLint sh, GLuint dst, GLint dw, GLint dh,
                    GLenum filter)
{
    const GLint sr[4] = { 0, 0, sw, sh }, dr[4] = { 0, 0, dw, dh };

    sh_blit_rect(src, sr, dst, dr, filter);
}

static void sh_depth_mask(int on)
{
    if (s_sh.depth_mask != on) {
        glDepthMask(on ? GL_TRUE : GL_FALSE);
        s_sh.depth_mask = on;
    }
}

static void sh_stencil_mask(GLuint m)
{
    if (s_sh.st_write != m) {
        glStencilMask(m);
        s_sh.st_write = m;
    }
}

/* ---- set-up ------------------------------------------------------------- */

static void unregister_self(void);
static void job_init(void);
static void pc_init(void);
static void sig_init(void);

/* RECOMP_GPU_SCALE: 1 (default), 2, 3, a decimal 1-4, or "auto" -- the
 * window's drawable height / 480 (1080 -> 2.25, 1440x1080 shown 1:1),
 * at most 3; 1 headless. Unset, the setting every console shares,
 * RECOMP_UPSCALE (Settings > Graphics > Internal resolution: 1, 2, 4; 8 draws
 * at 4; XBA-237, nv2a_gl_scale_choose). Never more than
 * GL_MAX_TEXTURE_SIZE / 640. Needs the context (called from ensure_init). */
static float scale_from_env(void)
{
    const char *key;
    GLint max_tex = 0;
    float sc = 1.0f;
    char why[96];
    int how = nv2a_gl_scale_choose(getenv("RECOMP_GPU_SCALE"), getenv("RECOMP_UPSCALE"), &sc, &key);

    if (how == NV2A_GL_SCALE_UNSET)
        return 1.0f;
    snprintf(why, sizeof why, "%s=%s", key, getenv(key));
    if (how == NV2A_GL_SCALE_AUTO) {
        if (s_display_h) {
            sc = (float)s_display_h / 480.0f;
            if (sc > 3.0f)
                sc = 3.0f;
            snprintf(why, sizeof why, "RECOMP_GPU_SCALE=auto, window drawable %ux%u",
                     s_display_w, s_display_h);
        } else {
            snprintf(why, sizeof why, "RECOMP_GPU_SCALE=auto, no window");
        }
    } else if (how == NV2A_GL_SCALE_BAD) {
        fprintf(stderr, "  [GPUGL] %s: expected %s; using 1\n", why,
                strcmp(key, "RECOMP_UPSCALE") == 0 ? "1, 2, 4 or 8" : "1-4 or auto");
    }
    if (sc < 1.0f)
        sc = 1.0f;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex);
    if (max_tex >= 640 && sc * 640.0f > (float)max_tex)
        sc = (float)max_tex / 640.0f;
    fprintf(stderr, "  [GPUGL] internal scale %.2f (%s; max texture %d)\n", sc, why, (int)max_tex);
    return sc;
}

static void zc_mode(void);
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
    sh_forget();
    zc_mode();
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
    {
        /* Points and lines (XBA-120): a vertex shader sets the point size
         * (core desktop GL needs that turned on; GLES always does it). Wide
         * lines only on GLES: a forward-compatible desktop context refuses
         * widths over 1, and macOS reports [1, 1] anyway. */
        GLfloat r[2] = { 1.0f, 1.0f };
        s_line_width = 1.0f;            /* a new context's */
        /* GLES names the range ALIASED_POINT_SIZE_RANGE; core desktop GL
         * dropped that name with the compatibility profile (Apple's gl3.h
         * does not declare it) and keeps POINT_SIZE_RANGE. */
#if NV2A_GL_ES
        glGetFloatv(GL_ALIASED_POINT_SIZE_RANGE, r);
#else
        glGetFloatv(GL_POINT_SIZE_RANGE, r);
#endif
        s_point_max = r[1] >= 1.0f ? r[1] : 1.0f;
#if NV2A_GL_ES
        r[0] = r[1] = 1.0f;
        glGetFloatv(GL_ALIASED_LINE_WIDTH_RANGE, r);
        s_line_max = r[1] >= 1.0f ? r[1] : 1.0f;
#else
        glEnable(GL_PROGRAM_POINT_SIZE);
#endif
    }
    s_mips = !(getenv("RECOMP_GPU_MIPS") && !env_on("RECOMP_GPU_MIPS"));
    s_verbose = env_on("RECOMP_GPU_VERBOSE");
    s_cpu_writes = !(getenv("RECOMP_GPU_CPU_WRITES") && !env_on("RECOMP_GPU_CPU_WRITES"));
    s_readback_layout_switch = env_on("RECOMP_GPU_RT_RELAYOUT_READBACK");
    s_rt_direct_off = getenv("RECOMP_GPU_RT_DIRECT") && !env_on("RECOMP_GPU_RT_DIRECT");
    {
        const char *a = getenv("RECOMP_GPU_PRESENT");
        s_present_sync_mode = a && strcmp(a, "sync") == 0;
#ifdef XBOXRECOMP_LSFG
        /* RECOMP_LSFG=1: the pacer times frame N by flip N; the async path
         * delivers it one flip later. */
        if (!s_present_sync_mode && framegen_policy_force_present_sync()) {
            s_present_sync_mode = 1;
            fprintf(stderr, "  [GPUGL] present read-back sync (frame generation)\n");
        }
#endif
        s_present_host_env = a && strcmp(a, "host") == 0;
        a = getenv("RECOMP_GPU_READBACK");
        s_readback_all = a && strcmp(a, "all") == 0;
        s_readback_present = a && strcmp(a, "present") == 0;
    }
    s_scale = scale_from_env();
    s_scale_cfg.scale = s_scale;
    s_scale_cfg.shadow = env_on("RECOMP_GPU_SCALE_SHADOW");
    nv2a_gl_scale_parse_targets(getenv("RECOMP_GPU_SCALE_TARGETS"), &s_scale_cfg);
    s_scale_cfg.no_sticky = getenv("RECOMP_GPU_SCALE_STICKY") && !env_on("RECOMP_GPU_SCALE_STICKY");
    s_scale_resolve = !(getenv("RECOMP_GPU_SCALE_RESOLVE") && !env_on("RECOMP_GPU_SCALE_RESOLVE"));
    s_scale_dump = env_on("RECOMP_GPU_SCALE_DUMP");
    if (s_scale > 1.0f)
        fprintf(stderr, "  [GPUGL] scaled targets at %.2f: frame %ux%u -> %ux%u, resolve %s,"
                        " shadow maps %s, %u extra target sizes\n", (double)s_scale,
                NV2A_GL_FRAME_W, NV2A_GL_FRAME_H, nv2a_gl_scale_px(NV2A_GL_FRAME_W, s_scale),
                nv2a_gl_scale_px(NV2A_GL_FRAME_H, s_scale), s_scale_resolve ? "on" : "off",
                s_scale_cfg.shadow ? "scaled" : "native", s_scale_cfg.ntargets);
    if (s_present_host_env || s_scale > 1.0f)
        fprintf(stderr, "  [GPUGL] host present %s (window %ux%u), native copy %s\n",
                s_display_w ? "on" : "off: no window", s_display_w, s_display_h,
                s_readback_present || s_readback_all || !s_display_w ? "every flip"
                                                                     : "on demand");
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
        fprintf(stderr, "  [GPUGL] the base vertex shader does not compile: falling back"
                        " to the CPU rasteriser\n");
        unregister_self();
        return 0;
    }
    glGenVertexArrays(1, &s_vao);
    sh_vao(s_vao);
    glGenBuffers(1, &s_vbo);
    sh_array_buffer(s_vbo);
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
    job_init();                     /* before pc_init: the list's warm-up depends on it */
    pc_init();
    sig_init();
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    if (s_depth_clamp)
        glEnable(GL_DEPTH_CLAMP);
    s_init = 1;
    return 1;
}

/* ---- batch signatures (RECOMP_GPU_SIGDUMP, RECOMP_GPU_SIGSKIP) ---------- */
/*      and the per-batch record (RECOMP_GPU_BATCHLOG)                       */

/* A diagnosis tool for wrong pixels: which batches draw a spot, and what is
 * left of the picture without them. Each batch gets a 32-bit signature of
 * what decides how it looks -- target address and size, combiner program
 * and words, each stage's format, kind and size, blend, alpha test, depth,
 * stencil, colour mask -- and of no texture or vertex address, so a kind of
 * batch keeps its signature from frame to frame while the title streams.
 *
 *   RECOMP_GPU_SIGDUMP=<file>  at the first flip where <file> exists, that
 *       frame's table goes to stderr and the file is deleted (touch it again
 *       for another frame): one "[GPUSIG] <sig> n <batches> tris <triangles>
 *       : <description>" line per signature (the description gives the
 *       first batch's texture addresses), and one "[GPUSEQ]" line with the
 *       frame's target runs, address(width x height)x batches (a depth-only
 *       target shows its zeta address | 1);
 *       With RECOMP_GPU_SIGDUMP_KEEP=1 the file is kept: every frame is
 *       printed for as long as it exists;
 *   RECOMP_GPU_SIGSKIP=<file>  batches whose signature is listed in <file>
 *       (hex words, re-read every 16 flips) are not drawn;
 *   RECOMP_GPU_BATCHLOG=<file>  one binary record per batch (BLG_* below:
 *       frame, signature, target, vertex buffer, stage textures, hashes of
 *       the program, indices, vertices and constants) is written to <file>,
 *       every frame, or with RECOMP_GPU_BATCHLOG_FLIPS=<flag file> only the
 *       frames that start while the flag file exists. What
 *       tools/frame_outliers reads: "same batches, other textures".
 * Off by default (one test per batch). */
#define NV_GL_MAX_SIG 512
#define NV_GL_MAX_SEQ 512
typedef struct {
    uint32_t sig, count, tris;
    char desc[400];
} GlSig;
static int s_sig_on;
static const char *s_sigdump, *s_sigskip;
static GlSig *s_sig;
static uint32_t s_nsig, s_skip[64], s_nskip;
static struct { uint32_t va, wh, n; } s_seq[NV_GL_MAX_SEQ];
static uint32_t s_nseq;
static int s_sigkeep;
/* Batch record, little-endian 32-bit words (host order; every host is
 * little-endian). Word 1 is the record size in words, so a reader can skip
 * words it does not know. Keep in step with tools/frame_outliers/records.py. */
#define BLG_MAGIC 0x31474C42u           /* "BLG1" */
enum {
    BLG_MAGIC_W, BLG_SIZE_W, BLG_FRAME, BLG_INDEX, BLG_SIG, BLG_TARGET, BLG_TARGET_WH,
    BLG_ZETA, BLG_FLAGS, BLG_COUNT, BLG_VCOUNT, BLG_FIRST, BLG_VB, BLG_STRIDE, BLG_STREAMS,
    BLG_PROG, BLG_PROG_HI, BLG_IDX, BLG_IDX_HI, BLG_VTX, BLG_VTX_HI, BLG_CONST, BLG_CONST_HI,
    BLG_TEX0, BLG_TEXFMT0 = BLG_TEX0 + 4, BLG_CMASK = BLG_TEXFMT0 + 4, BLG_LOOK, BLG_WORDS
};
static FILE *s_blog;
static const char *s_blog_flag;
static int s_blog_active;
static uint32_t s_blog_n;

static void sig_init(void)
{
    const char *d = getenv("RECOMP_GPU_SIGDUMP"), *k = getenv("RECOMP_GPU_SIGSKIP");
    const char *bl = getenv("RECOMP_GPU_BATCHLOG"), *bf = getenv("RECOMP_GPU_BATCHLOG_FLIPS");
    const char *kp = getenv("RECOMP_GPU_SIGDUMP_KEEP");

    s_sigdump = d && *d ? d : NULL;
    s_sigskip = k && *k ? k : NULL;
    s_sigkeep = kp && *kp && strcmp(kp, "0") != 0;
    if (bl && *bl && !s_blog) {
        s_blog = fopen(bl, "wb");
        if (!s_blog)
            fprintf(stderr, "  [GPUGL] batch log: cannot open %s\n", bl);
        s_blog_flag = bf && *bf && strcmp(bf, "1") != 0 ? bf : NULL;
        s_blog_active = s_blog && !s_blog_flag;
        if (s_blog)
            fprintf(stderr, "  [GPUGL] batch log: %s, %s%s\n", bl,
                    s_blog_flag ? "while this file exists: " : "every frame",
                    s_blog_flag ? s_blog_flag : "");
    }
    if (!s_sig && (s_sigdump || s_sigskip || s_blog))
        s_sig = (GlSig *)calloc(NV_GL_MAX_SIG, sizeof *s_sig);
    s_sig_on = s_sig != NULL;
    if (s_sig_on)
        fprintf(stderr, "  [GPUGL] batch signatures: dump when %s exists%s, skip list %s\n",
                s_sigdump ? s_sigdump : "(none)", s_sigdump && s_sigkeep ? " (every frame)" : "",
                s_sigskip ? s_sigskip : "(none)");
}

/* One record of batch b to the batch log: sig is its signature, look the
 * same hash without the target address (swapped buffers keep it). */
static void blog_batch(const Nv2aSurface *s, const Nv2aBatch *b, uint32_t sig, uint32_t look)
{
    const Nv2aProgramBatch *pb = b->program;
    const Nv2aPixelState *ps = b->pixel;
    const Nv2aRenderState *rs = b->state;
    uint32_t w[BLG_WORDS], i;
    uint64_t h;

    memset(w, 0, sizeof w);
    w[BLG_MAGIC_W] = BLG_MAGIC;
    w[BLG_SIZE_W] = BLG_WORDS;
    w[BLG_FRAME] = s_frame;
    w[BLG_INDEX] = s_blog_n++;
    w[BLG_SIG] = sig;
    w[BLG_TARGET] = s->color_valid ? s->color_va : 0;
    w[BLG_TARGET_WH] = s->width | s->height << 16;
    w[BLG_ZETA] = rs->zeta_va;
    w[BLG_FLAGS] = b->prim | (pb ? 0x100u : 0) | (pb && pb->ffp ? 0x200u : 0)
                 | (pb && pb->raw ? 0x400u : 0);
    if (pb) {
        w[BLG_COUNT] = pb->index_count;
        w[BLG_VCOUNT] = pb->vertex_count;
        w[BLG_FIRST] = pb->raw_first;
        w[BLG_STREAMS] = pb->streams;
        w[BLG_PROG] = (uint32_t)pb->hash;
        w[BLG_PROG_HI] = (uint32_t)(pb->hash >> 32);
        if (pb->indices && pb->index_count) {
            h = hash_bytes((const uint8_t *)pb->indices, pb->index_count * 2u);
            w[BLG_IDX] = (uint32_t)h;
            w[BLG_IDX_HI] = (uint32_t)(h >> 32);
        }
        if (pb->raw) {
            /* The first stream present; its elements raw_first .. +vertex_count
             * are inside guest memory (nv2a_backend.h). */
            for (i = 0; i < 16 && !((pb->streams >> i) & 1); i++)
                ;
            if (i < 16) {
                const Nv2aRawAttr *r = &pb->raw[i];
                w[BLG_VB] = r->offset;
                w[BLG_STRIDE] = r->stride;
                if (r->stride && pb->vertex_count) {
                    h = hash_bytes(guest(r->offset + pb->raw_first * r->stride),
                                   (size_t)pb->vertex_count * r->stride);
                    w[BLG_VTX] = (uint32_t)h;
                    w[BLG_VTX_HI] = (uint32_t)(h >> 32);
                }
            }
        }
        if (pb->constants) {
            h = hash_bytes((const uint8_t *)pb->constants, 192u * 16u);
            w[BLG_CONST] = (uint32_t)h;
            w[BLG_CONST_HI] = (uint32_t)(h >> 32);
        }
    } else {
        w[BLG_COUNT] = b->count;
        w[BLG_VCOUNT] = b->count;
        if (b->vertices && b->count) {
            h = hash_bytes((const uint8_t *)b->vertices, (size_t)b->count * sizeof *b->vertices);
            w[BLG_VTX] = (uint32_t)h;
            w[BLG_VTX_HI] = (uint32_t)(h >> 32);
        }
    }
    for (i = 0; i < 4; i++) {
        const Nv2aTexStage *t = &ps->stage[i];
        if (!t->valid)
            continue;
        w[BLG_TEX0 + i] = t->tex.offset;
        w[BLG_TEXFMT0 + i] = (t->tex.color & 0xFFu) | ((t->tex.width - 1u) & 0xFFFu) << 8
                           | ((t->tex.height - 1u) & 0xFFFu) << 20;
    }
    w[BLG_CMASK] = rs->color_mask;
    w[BLG_LOOK] = look;
    fwrite(w, sizeof w, 1, s_blog);
}

/* A target switch, for the [GPUSEQ] line. */
static void sig_target(uint32_t va, uint32_t w, uint32_t h)
{
    if (s_sig_on && s_nseq < NV_GL_MAX_SEQ) {
        s_seq[s_nseq].va = va;
        s_seq[s_nseq].wh = w | h << 16;
        s_seq[s_nseq].n = 0;
        s_nseq++;
    }
}

/* Count batch b in this frame's table; 1: it is on the skip list. */
static int sig_batch(const Nv2aSurface *s, const Nv2aBatch *b, const uint32_t kind[4],
                     uint32_t tris)
{
    const Nv2aPixelState *ps = b->pixel;
    const Nv2aRenderState *rs = b->state;
    uint32_t h = 2166136261u, w[48], n = 0, i;
    GlSig *e;

    if (s_nseq)
        s_seq[s_nseq - 1].n++;
    w[n++] = ps->rc.stage_program;
    w[n++] = s->color_valid ? s->color_va : 0;
    w[n++] = s->width | s->height << 16;
    for (i = 0; i < 4; i++) {
        const Nv2aTexStage *t = &ps->stage[i];
        w[n++] = t->valid ? t->tex.color : 0xFF;
        w[n++] = kind[i];
        w[n++] = t->valid ? (t->tex.width | t->tex.height << 16) : 0;
    }
    w[n++] = rs->blend_enable;
    w[n++] = rs->blend_enable ? rs->blend_src : 0;
    w[n++] = rs->blend_enable ? rs->blend_dst : 0;
    w[n++] = rs->blend_enable ? rs->blend_eq : 0;
    w[n++] = rs->alpha_test_enable;
    w[n++] = rs->alpha_test_enable ? rs->alpha_func : 0;
    w[n++] = rs->depth_test_enable;
    w[n++] = rs->depth_write;
    w[n++] = rs->color_mask;
    w[n++] = ps->shade | (ps->rc.control & 0xFF) << 8;
    w[n++] = ps->rc.final0;
    w[n++] = ps->rc.final1;
    w[n++] = rs->stencil_enable;
    for (i = 0; i < (ps->rc.control & 0xF) && i < 8; i++) {
        w[n++] = ps->rc.color_icw[i];
        w[n++] = ps->rc.alpha_icw[i];
    }
    for (i = 0; i < n * 4; i++)
        h = (h ^ ((const uint8_t *)w)[i]) * 16777619u;
    if (s_blog_active) {
        uint32_t look = 2166136261u;
        w[1] = 0;
        for (i = 0; i < n * 4; i++)
            look = (look ^ ((const uint8_t *)w)[i]) * 16777619u;
        w[1] = s->color_valid ? s->color_va : 0;
        blog_batch(s, b, h, look);
    }

    for (i = 0; i < s_nsig && s_sig[i].sig != h; i++)
        ;
    if (i == s_nsig && s_nsig < NV_GL_MAX_SIG) {
        char *d;
        size_t left;
        int k;
        uint32_t st;
        e = &s_sig[s_nsig++];
        e->sig = h;
        e->count = e->tris = 0;
        d = e->desc;
        left = sizeof e->desc;
#define SIG_PUT(...) do { k = snprintf(d, left, __VA_ARGS__);                  \
                          if (k < 0 || (size_t)k >= left) k = (int)left - 1;   \
                          d += k; left -= (size_t)k; } while (0)
        SIG_PUT("rt %08X %ux%u%s sp %05X |", w[1], s->width, s->height,
                s->color_masked ? " masked" : "", ps->rc.stage_program);
        for (st = 0; st < 4; st++) {
            const Nv2aTexStage *t = &ps->stage[st];
            if (t->valid || ((ps->rc.stage_program >> (st * 5)) & 31))
                SIG_PUT(" t%u[m%u f%02X k%u %ux%u @%08X a%u/%u flt%X]", st,
                        (ps->rc.stage_program >> (st * 5)) & 31, t->tex.color, kind[st],
                        t->tex.width, t->tex.height, t->tex.offset, t->tex.addr_u,
                        t->tex.addr_v, t->tex.filter >> 16);
        }
        SIG_PUT(" | bl %u %X/%X eq %X at %u/%X ref %u z %u/%X w%u cm %08X st %u"
                " | rc n%u fin %08X %08X fog %08X shf %u zeta %08X",
                rs->blend_enable, rs->blend_src, rs->blend_dst, rs->blend_eq,
                rs->alpha_test_enable, rs->alpha_func, rs->alpha_ref,
                rs->depth_test_enable, rs->depth_func, rs->depth_write, rs->color_mask,
                rs->stencil_enable, ps->rc.control & 0xF, ps->rc.final0, ps->rc.final1,
                ps->fog_color, ps->shadow_func, rs->zeta_va);
#undef SIG_PUT
    }
    if (i < s_nsig) {
        s_sig[i].count++;
        s_sig[i].tris += tris;
    }
    for (i = 0; i < s_nskip; i++)
        if (s_skip[i] == h)
            return 1;
    return 0;
}

/* At the flip: dump the frame's table if asked, start the next one. */
static void sig_flip(void)
{
    FILE *f;
    uint32_t i;

    if (!s_sig_on)
        return;
    if (s_blog) {
        if (s_blog_active)
            fflush(s_blog);         /* complete frames even if the run is killed */
        if (s_blog_flag) {
            s_blog_active = (f = fopen(s_blog_flag, "rb")) != NULL;
            if (f)
                fclose(f);
        }
        s_blog_n = 0;
    }
    if (s_sigdump && (f = fopen(s_sigdump, "rb")) != NULL) {
        fclose(f);
        if (!s_sigkeep)
            remove(s_sigdump);
        fprintf(stderr, "[GPUSIG] frame %u: %u signatures, %u target runs\n",
                s_frame, s_nsig, s_nseq);
        fprintf(stderr, "[GPUSEQ]");
        for (i = 0; i < s_nseq; i++)
            fprintf(stderr, " %08X(%ux%u)x%u", s_seq[i].va, s_seq[i].wh & 0xFFFF,
                    s_seq[i].wh >> 16, s_seq[i].n);
        fprintf(stderr, "\n");
        for (i = 0; i < s_nsig; i++)
            fprintf(stderr, "[GPUSIG] %08X n %u tris %u : %s\n", s_sig[i].sig,
                    s_sig[i].count, s_sig[i].tris, s_sig[i].desc);
    }
    s_nsig = 0;
    s_nseq = 0;
    if (s_sigskip && (s_frame & 15) == 0) {
        s_nskip = 0;
        if ((f = fopen(s_sigskip, "r")) != NULL) {
            unsigned v;
            while (s_nskip < 64 && fscanf(f, "%x", &v) == 1)
                s_skip[s_nskip++] = v;
            fclose(f);
        }
    }
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
    uint32_t gw, gh;                      /* the GL texture's logical size (>= w, h) */
    /* Internal resolution: GL texels per guest pixel. tex (and copy) are
     * round(gw * sc) x round(gh * sc); w, h, gw, gh stay in guest pixels,
     * which is what positions, texture coordinates and read-back use. A
     * scaled target has a native-size resolve (rtex/rfbo, gw x gh): what
     * read-back reads, what a native target samples, and where seeding
     * uploads before the blit up. rserial: r->serial it was made from. */
    float sc;
    GLuint rtex, rfbo;
    uint32_t rw, rh, rserial;
    GlSamp rsamp;                         /* sampler state of rtex */
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
    /* Guest CPU writes (rt_cpu_check): the guest bytes of the surface
     * ([va, va + snap_len)) as they were when guest memory and the target
     * last agreed -- the seed upload, a read-back, a merge. A byte that
     * differs from it now was written by the guest CPU since. cpu_frame:
     * s_frame + 1 at the last once-a-frame check (bind, direct sampling). */
    uint8_t *snap;
    uint32_t snap_len;
    uint32_t cpu_frame;
} GlRT;

/* Depth/stencil per zeta surface: a D24S8 texture (not a renderbuffer), so
 * a shadow map drawn into it can be sampled with a depth compare. */
typedef struct {
    uint32_t va, w, h;                    /* w, h logical (guest pixels) */
    float sc;                             /* texture: round(w * sc) x round(h * sc) */
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

/* Colour targets held at once (least recently bound evicted: read back,
 * and a scaled one loses its resolution until re-seeded). Burnout 3 holds
 * 14 in a race; 32 leaves room for menus and scenes with more effect
 * targets. Only slots in use own GL objects; the walks over the table cost
 * a compare per slot. */
#define NV_GL_MAX_RT    32
#define NV_GL_MAX_DEPTH 8
static GlRT    s_rt[NV_GL_MAX_RT];
/* The colour target each stage samples directly this batch (NULL: none;
 * bind_stage_textures, break_feedback). */
static GlRT   *s_stage_rt[4];
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

/* Byte offset of pixel (x, y) in the guest surface; mx, my: the swizzle
 * masks of a swizzled target (xbox_swizzle_masks, once per surface). */
static size_t rt_px_off(const GlRT *r, uint32_t x, uint32_t y, uint32_t mx, uint32_t my)
{
    return r->swz ? (size_t)(swizzle_deposit(x, mx) | swizzle_deposit(y, my)) * r->bpp
                  : (size_t)y * r->pitch + (size_t)x * r->bpp;
}

/* Real (GL) size of a logical length on target r. */
static uint32_t rt_real_px(const GlRT *r, uint32_t v)
{
    return nv2a_gl_scale_px(v, r->sc);
}

/* The native-size resolve objects of scaled target r (gw x gh), made or
 * resized as needed; a fresh one holds nothing yet (rserial stale). */
static int rt_resolve_alloc(GlRT *r)
{
    if (r->rtex && r->rw == r->gw && r->rh == r->gh)
        return 1;
    if (!r->rtex) {
        glGenTextures(1, &r->rtex);
        glGenFramebuffers(1, &r->rfbo);
    }
    sh_tex_scratch(GL_TEXTURE_2D, r->rtex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)r->gw, (GLsizei)r->gh, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    {
        GLuint fbo = s_sh.fbo;
        sh_fbo(r->rfbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->rtex, 0);
        if (fbo != 0xFFFFFFFFu)
            sh_fbo(fbo);
    }
    r->rw = r->gw;
    r->rh = r->gh;
    r->rsamp.wrap_s = r->rsamp.wrap_t = r->rsamp.filter = r->rsamp.swz_a = -1;
    r->rserial = r->serial - 1;
    return 1;
}

/* Target r's pixels at guest size, current as of r->serial: the framebuffer
 * to read r->w x r->h guest pixels from. At scale 1 the target's own; a
 * scaled target is blitted down (linear) into its resolve when it was
 * written since the last one. */
static GLuint rt_resolve(GlRT *r)
{
    if (r->sc == 1.0f)
        return r->fbo;
    rt_resolve_alloc(r);
    if (r->rserial != r->serial) {
        sh_blit(r->fbo, (GLint)rt_real_px(r, r->gw), (GLint)rt_real_px(r, r->gh),
                r->rfbo, (GLint)r->gw, (GLint)r->gh, GL_LINEAR);
        r->rserial = r->serial;
        s_sst.resolves++;
    }
    return r->rfbo;
}

/* One guest pixel of target r (its own format) as RGBA bytes. */
static void rt_guest_rgba(const GlRT *r, const uint8_t *p, uint8_t *o)
{
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

/* Bytes of guest memory target r's surface spans in its current layout. */
static uint32_t rt_guest_len(const GlRT *r)
{
    if (r->swz)
        return r->w * r->h * r->bpp;
    return r->h ? r->pitch * (r->h - 1) + r->w * r->bpp : 0;
}

static void rt_snap_take(GlRT *r);

/* Guest bytes -> the render target (on creation: what was there before the
 * GPU drew counts, a title may draw over a CPU-written picture). A scaled
 * target gets them at guest size in its resolve, blitted up (nearest: an
 * integer scale replicates pixels, and the resolve then holds the exact
 * guest picture for read-back). */
static void rt_upload(GlRT *r)
{
    uint8_t *buf = scratch((size_t)r->w * r->h * 4), *mem = guest(r->va);
    uint32_t x, y, mx = 0, my = 0;

    if (!buf)
        return;
    if (r->swz)
        xbox_swizzle_masks(r->w, r->h, &mx, &my);
    for (y = 0; y < r->h; y++)
        for (x = 0; x < r->w; x++)
            rt_guest_rgba(r, mem + rt_px_off(r, x, y, mx, my),
                          buf + ((size_t)y * r->w + x) * 4);
    rt_snap_take(r);
    if (r->sc != 1.0f) {
        rt_resolve_alloc(r);
        sh_tex_scratch(GL_TEXTURE_2D, r->rtex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)r->w, (GLsizei)r->h,
                        GL_RGBA, GL_UNSIGNED_BYTE, buf);
        sh_blit(r->rfbo, (GLint)r->w, (GLint)r->h, r->fbo, (GLint)rt_real_px(r, r->w),
                (GLint)rt_real_px(r, r->h), GL_NEAREST);
        r->rserial = r->serial;
        s_sst.seed_blits++;
        return;
    }
    sh_tex_scratch(GL_TEXTURE_2D, r->tex);
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

/* Host present (s_present_host_env / s_scale): the presented target read
 * at its real size into one of two pack buffers at flip N, mapped at flip
 * N+1 and handed to the window as Nv2aPresentSurface.host_px, unmapped at
 * that flip's gl_flip -- meanwhile flip N+1's read-back goes into the other
 * buffer. s_rt_host_serial: the target's serial at its last host read
 * (kept apart from `dirty`, which says the guest bytes are stale; a host
 * frame leaves them so). */
static GLuint s_hpbo[2];
static size_t s_hpbo_size[2];
static int s_hnext, s_hmapped = -1;
static struct {
    int pending, buf;
    uint32_t w, h;
    Nv2aPresentSurface surf;
} s_hpend;
static uint32_t s_rt_host_serial[NV_GL_MAX_RT];
/* An anti-aliased presented surface (1280 wide and/or 960 high for the
 * 640x480 frame: AA 2x1 or 2x2) goes to the window as a host frame,
 * resolved on the GPU: a linear blit to half its size in each doubled
 * direction, an exact box filter over the 2 or 4 samples (host_issue). */
static GLuint s_aa_tex, s_aa_fbo;
static uint32_t s_aa_w, s_aa_h;
static struct {
    uint32_t frames;
    uint64_t ns;                          /* issue + map, this side */
    uint32_t w, h;
    uint32_t aa_frames;                   /* resolved from an anti-aliased surface */
} s_hst;

/* Zero-copy present (XBA-308, src/video/fb_gpu_frame.h): with the context in
 * the window's share group (s_zc_on: nv2a_gl_present_share asked for it and
 * the share worked), a flip the window can take as a texture (s_zc_wanted)
 * hands it s_zc_frame as host_px; the window then has the presented target
 * blitted into its back slot's texture (zc_render: one GPU blit, a fence)
 * and draws it itself. No read-back, no copy, no texture upload, and this
 * flip's own frame rather than the previous one. The slot textures outlive
 * gl_reset_objects (the window may still draw one) except on a lost
 * context; after that, or when the window's context is lost
 * (nv2a_gl_present_window_lost), frames are read back for the rest of the
 * run. */
static int (*s_zc_wanted)(void);
static int s_zc_on;
static char s_zc_why[160];                /* why frames are read back (the mode line) */
static int s_zc_window_lost;              /* atomic: nv2a_gl_present_window_lost */
static GLuint s_zc_tex[XBOX_FB_GPU_SLOTS], s_zc_fbo[XBOX_FB_GPU_SLOTS];
static uint32_t s_zc_w[XBOX_FB_GPU_SLOTS], s_zc_h[XBOX_FB_GPU_SLOTS];
static GLsync s_zc_prev;                  /* the last zero-copy flip's own fence (back-pressure) */
static struct {
    int pending;                          /* set by gl_present, until gl_flip */
    GlRT *r;
    Nv2aPresentSurface surf;
} s_zcp;
static XboxFbGpuFrame s_zc_frame;
static uint8_t *s_zc_px;                  /* zc_pixels: the window wanted pixels after all */
static size_t s_zc_px_cap;
static struct {
    uint32_t frames, readbacks, timeouts;
    uint64_t ns;                          /* zc_render, this side */
    uint32_t w, h;
} s_zst;

/* ---- guest CPU writes into targets (XBA-119) --------------------------- */

/* A title may write a GPU target's memory with the CPU: a movie decoded
 * straight into the back buffer, a loading bar or text drawn into the frame,
 * a surface filled with memset. The CPU rasteriser sees those bytes as it
 * draws in guest memory; the GPU target would not, and its next read-back
 * would even overwrite them. Each target keeps a copy of its guest bytes as
 * they were when the two last agreed (snap: the seed, a read-back, a merge);
 * rt_cpu_check compares guest memory with it and merges the pixels that
 * differ into the GL texture, leaving every other pixel as the GPU drew it,
 * so a bar drawn over a GPU frame keeps the frame around it. Checked when a
 * target is first bound or sampled in a frame, before any read-back (which
 * would otherwise lose the writes) and for the presented target at every
 * flip. A memcmp per check when nothing changed. RECOMP_GPU_CPU_WRITES=0
 * turns it off (the old behaviour: CPU writes into a target are not seen). */

static void rt_snap_take(GlRT *r)
{
    uint32_t len = rt_guest_len(r);

    if (!len) {
        free(r->snap);
        r->snap = NULL;
        r->snap_len = 0;
        return;
    }
    if (len != r->snap_len || !r->snap) {
        uint8_t *p = (uint8_t *)realloc(r->snap, len);
        if (!p) {
            free(r->snap);
            r->snap = NULL;
            r->snap_len = 0;
            return;
        }
        r->snap = p;
        r->snap_len = len;
    }
    memcpy(r->snap, guest(r->va), len);
}

/* The back end itself wrote guest bytes [va, va + len) (a read-back): they
 * are not the guest CPU's, for any target whose memory they overlap. */
static void snap_refresh(uint32_t va, uint32_t len)
{
    int i;

    for (i = 0; i < NV_GL_MAX_RT; i++) {
        GlRT *r = &s_rt[i];
        uint32_t lo, hi;
        if (!r->snap)
            continue;
        lo = va > r->va ? va : r->va;
        hi = va + len < r->va + r->snap_len ? va + len : r->va + r->snap_len;
        if (lo < hi)
            memcpy(r->snap + (lo - r->va), guest(lo), hi - lo);
    }
}

/* Past this many runs of changed pixels the bounding box goes up whole:
 * inside it the guest bytes win, also where the GPU drew since. */
#define NV_GL_CPU_MAX_RUNS 4096

static uint32_t s_cpu_uploads;      /* glTexSubImage2D calls of rt_cpu_merge (tests) */

/* Row m of a change mask, inside [x0, x1): 1 when it holds exactly one run
 * of changed pixels, [*a, *b). */
static int mask_one_run(const uint8_t *m, uint32_t x0, uint32_t x1, uint32_t *a, uint32_t *b)
{
    uint32_t x = x0, e;

    while (x < x1 && !m[x])
        x++;
    if (x == x1)
        return 0;
    for (e = x; e < x1 && m[e]; e++)
        ;
    *a = x;
    *b = e;
    for (; e < x1; e++)
        if (m[e])
            return 0;
    return 1;
}

/* The pixels of target r whose guest bytes differ from r->snap, into the GL
 * texture, at guest size -- a scaled target gets them in its resolve (made
 * current first), and the box around them is blitted up (nearest). A box
 * that changed whole (a movie frame) goes up in one upload; otherwise run
 * by run, with consecutive rows of one same run (a rectangle the CPU
 * drew) as one upload. */
static void rt_cpu_merge(GlRT *r)
{
    const uint8_t *mem = guest(r->va);
    uint8_t *buf = scratch((size_t)r->w * r->h * 5), *mask;
    uint32_t x, y, mx = 0, my = 0, x0 = r->w, y0 = r->h, x1 = 0, y1 = 0, runs = 0, px = 0;
    GLuint dst;

    if (!buf)
        return;
    mask = buf + (size_t)r->w * r->h * 4;   /* the scratch buffer's tail */
    if (r->swz)
        xbox_swizzle_masks(r->w, r->h, &mx, &my);
    for (y = 0; y < r->h; y++) {
        int prev = 0;
        for (x = 0; x < r->w; x++) {
            size_t off = rt_px_off(r, x, y, mx, my);
            int ch = memcmp(mem + off, r->snap + off, r->bpp) != 0;
            mask[(size_t)y * r->w + x] = (uint8_t)ch;
            if (ch) {
                rt_guest_rgba(r, mem + off, buf + ((size_t)y * r->w + x) * 4);
                if (!prev)
                    runs++;
                px++;
                if (x < x0) x0 = x;
                if (x >= x1) x1 = x + 1;
                if (y < y0) y0 = y;
                if (y >= y1) y1 = y + 1;
            }
            prev = ch;
        }
    }
    if (!px)
        return;
    if (r->sc != 1.0f) {
        rt_resolve(r);
        dst = r->rtex;
    } else {
        dst = r->tex;
    }
    sh_tex_scratch(GL_TEXTURE_2D, dst);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)r->w);
    if (runs > NV_GL_CPU_MAX_RUNS || px == (x1 - x0) * (y1 - y0)) {
        for (y = y0; y < y1; y++)
            for (x = x0; x < x1; x++)
                if (!mask[(size_t)y * r->w + x])
                    rt_guest_rgba(r, mem + rt_px_off(r, x, y, mx, my),
                                  buf + ((size_t)y * r->w + x) * 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, (GLint)x0, (GLint)y0, (GLsizei)(x1 - x0),
                        (GLsizei)(y1 - y0), GL_RGBA, GL_UNSIGNED_BYTE,
                        buf + ((size_t)y0 * r->w + x0) * 4);
        s_cpu_uploads++;
    } else {
        for (y = y0; y < y1; y++) {
            const uint8_t *m = mask + (size_t)y * r->w;
            uint32_t a, b, a2, b2, ye;
            if (mask_one_run(m, x0, x1, &a, &b)) {
                for (ye = y + 1; ye < y1
                     && mask_one_run(mask + (size_t)ye * r->w, x0, x1, &a2, &b2)
                     && a2 == a && b2 == b; ye++)
                    ;
                glTexSubImage2D(GL_TEXTURE_2D, 0, (GLint)a, (GLint)y, (GLsizei)(b - a),
                                (GLsizei)(ye - y), GL_RGBA, GL_UNSIGNED_BYTE,
                                buf + ((size_t)y * r->w + a) * 4);
                s_cpu_uploads++;
                y = ye - 1;
                continue;
            }
            for (x = x0; x < x1; ) {
                uint32_t e;
                if (!m[x]) {
                    x++;
                    continue;
                }
                for (e = x; e < x1 && m[e]; e++)
                    ;
                glTexSubImage2D(GL_TEXTURE_2D, 0, (GLint)x, (GLint)y, (GLsizei)(e - x), 1,
                                GL_RGBA, GL_UNSIGNED_BYTE, buf + ((size_t)y * r->w + x) * 4);
                s_cpu_uploads++;
                x = e;
            }
        }
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    r->serial = ++s_rt_serial;
    if (r->sc != 1.0f) {
        const GLint sr[4] = { (GLint)x0, (GLint)y0, (GLint)x1, (GLint)y1 };
        const GLint dr[4] = { (GLint)rt_real_px(r, x0), (GLint)rt_real_px(r, y0),
                              (GLint)rt_real_px(r, x1), (GLint)rt_real_px(r, y1) };
        sh_blit_rect(r->rfbo, sr, r->fbo, dr, GL_NEAREST);
        r->rserial = r->serial;
    }
    s_st.cpu_merges++;
    s_st.cpu_kpx += (px + 999) / 1000;
    if (s_verbose)
        fprintf(stderr, "  [GPUGL] guest CPU wrote target 0x%08X: %u pixels in %u runs,"
                        " box %u,%u-%u,%u%s\n", r->va, px, runs, x0, y0, x1, y1,
                r->dirty ? " (merged over GPU pixels)" : "");
}

/* once: at most once a frame for this target (binds, direct sampling). */
static void rt_cpu_check(GlRT *r, int once)
{
    uint32_t len;

    if (!s_cpu_writes || !r || !r->tex || r->seed)
        return;
    if (once) {
        if (r->cpu_frame == s_frame + 1)
            return;
        r->cpu_frame = s_frame + 1;
    }
    len = rt_guest_len(r);
    if (!len)
        return;
    if (!r->snap || r->snap_len != len) {
        /* First check, or the surface's layout or size changed: the bytes
         * from now on are compared with these. */
        rt_snap_take(r);
        return;
    }
    if (memcmp(guest(r->va), r->snap, len) == 0)
        return;
    rt_cpu_merge(r);
    rt_snap_take(r);
}

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
    /* What the guest CPU wrote there goes into the target first: the
     * read-back would overwrite it. */
    rt_cpu_check(r, 0);
    buf = scratch((size_t)r->w * r->h * 4);
    if (!buf)
        return;
    sh_fbo(rt_resolve(r));
    glReadPixels(0, 0, (GLsizei)r->w, (GLsizei)r->h, NV_GL_READ_FORMAT, NV_GL_READ_TYPE, buf);
    rt_store(r, buf);
    snap_refresh(r->va, rt_guest_len(r));
    if (s_pend.pending && s_pend.snap.va == r->va)
        s_pend.dropped = 1;           /* these pixels are newer than the PBO's */
    r->dirty = 0;
    s_st.readbacks++;
}

static void rt_free(GlRT *r)
{
    int st;

    /* A target freed between the stage binds and the draw (re-created at
     * another scale, evicted for the batch's own target): its slot may be
     * reused at once, and break_feedback must not take the new occupant for
     * the texture a stage samples. */
    for (st = 0; st < 4; st++)
        if (s_stage_rt[st] == r)
            s_stage_rt[st] = NULL;
    rt_readback(r);
    if (r->fbo) sh_delete_fbo(r->fbo);
    if (r->tex) sh_delete_textures(1, &r->tex);
    if (r->copy) sh_delete_textures(1, &r->copy);
    if (r->rfbo) sh_delete_fbo(r->rfbo);
    if (r->rtex) sh_delete_textures(1, &r->rtex);
    free(r->snap);
    memset(r, 0, sizeof *r);
}

/* Give target r a bigger GL texture (gw x gh logical), keeping its pixels
 * (a GPU copy, no read-back). */
static void rt_grow(GlRT *r, uint32_t gw, uint32_t gh)
{
    GLuint t;

    glGenTextures(1, &t);
    sh_tex_scratch(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)rt_real_px(r, gw),
                 (GLsizei)rt_real_px(r, gh), 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    sh_fbo(r->fbo);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, (GLsizei)rt_real_px(r, r->gw),
                        (GLsizei)rt_real_px(r, r->gh));
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
    sh_delete_textures(1, &r->tex);
    r->tex = t;
    r->gw = gw;
    r->gh = gh;
    r->samp.wrap_s = r->samp.wrap_t = r->samp.filter = r->samp.swz_a = -1;
    s_st.rt_grows++;
}

/* The colour target already made for surface s's memory, with its pixel
 * size (NULL: none yet). */
static GlRT *rt_find(const Nv2aSurface *s)
{
    int i;

    for (i = 0; i < NV_GL_MAX_RT; i++)
        if (s_rt[i].tex && s_rt[i].va == s->color_va
            && s_rt[i].bpp == s->bytes_per_pixel)
            return &s_rt[i];
    return NULL;
}

/* The scale surface s is drawn at (nv2a_gl_target_scale: the policy, and a
 * scaled target's scale sticks to its memory). */
static float surface_scale(const Nv2aSurface *s)
{
    const GlRT *r;
    Nv2aGlHeldTarget held;

    if (s_scale_cfg.scale == 1.0f || !s->color_valid || !(r = rt_find(s)))
        return nv2a_gl_target_scale(&s_scale_cfg, s, NULL);
    held.sc = r->sc;
    held.pitch = r->pitch;
    held.swizzled = r->swz != 0;
    return nv2a_gl_target_scale(&s_scale_cfg, s, &held);
}

static GlRT *rt_get(const Nv2aSurface *s)
{
    float sc = surface_scale(s);
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
            if (r->bpp == bpp && r->sc != sc) {
                /* Another scale class now: a native target promoted to
                 * the frame class, or the memory reused in another layout
                 * (surface_scale keeps a scaled target's scale for any clip
                 * on the same pitch, and keys the policy on real pixels, so
                 * neither Fable's 0x121 / 0x2121 back buffer nor Burnout
                 * 3's HUD clip comes here): hand it back through guest
                 * memory. */
                s_sst.rescales++;
                if (s_verbose)
                    fprintf(stderr, "  [GPUGL] target 0x%08X rescaled %.2f -> %.2f"
                                    " (format 0x%X, clip %u,%u %ux%u)\n", r->va,
                            (double)r->sc, (double)sc, s->format, s->clip_x, s->clip_y,
                            s->width, s->height);
                rt_free(r);
                if (!free_slot)
                    free_slot = r;
                continue;
            }
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
    r->sc = sc;
    r->cfmt = s->format & 0xF;
    r->samp.wrap_s = r->samp.wrap_t = r->samp.filter = r->samp.swz_a = -1;
    glGenTextures(1, &r->tex);
    sh_tex_scratch(GL_TEXTURE_2D, r->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)rt_real_px(r, w),
                 (GLsizei)rt_real_px(r, h), 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    /* Seeded from the guest bytes when first drawn or sampled, unless the
     * first thing the title does is clear it whole (rt_seed, gl_clear). */
    r->seed = 1;
    s_st.rt_creates++;
    glGenFramebuffers(1, &r->fbo);
    sh_fbo(r->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->tex, 0);
    r->depth = 0;
    if (s_verbose) {
        if (sc != 1.0f)
            fprintf(stderr, "  [GPUGL] render target 0x%08X %ux%u bpp %u%s at %.2fx (%ux%u)\n",
                    r->va, w, h, bpp, swz ? " swizzled" : "", (double)sc,
                    rt_real_px(r, w), rt_real_px(r, h));
        else
            fprintf(stderr, "  [GPUGL] render target 0x%08X %ux%u bpp %u%s\n",
                    r->va, w, h, bpp, swz ? " swizzled" : "");
    }
    return r;
}

/* Bytes per zeta pixel: Z16 (SET_SURFACE_FORMAT bits 4-7 = 1) or Z24S8. */
static uint32_t zeta_bpp(const Nv2aSurface *s)
{
    return ((s->format >> 4) & 0xF) == 1 ? 2u : 4u;
}

static void depth_free(GlDepth *d)
{
    GLuint fbo = s_sh.fbo;
    int i;

    /* Colour targets that have it attached drop it (re-attached at bind).
     * The framebuffer bound before is bound again: bind_target binds its
     * target and then asks depth_get, which comes here when the depth must
     * grow -- left on another target's framebuffer, the new depth was
     * attached there and the batch or clear went into that target (an
     * anti-aliased 1280x960 target cleared after 640x480 ones: XBA-119). */
    for (i = 0; i < NV_GL_MAX_RT; i++)
        if (s_rt[i].depth == d->tex && d->tex) {
            sh_fbo(s_rt[i].fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                   GL_TEXTURE_2D, 0, 0);
            s_rt[i].depth = 0;
        }
    if (fbo != 0xFFFFFFFFu && fbo != d->fbo)
        sh_fbo(fbo);
    if (d->fbo) sh_delete_fbo(d->fbo);
    if (d->tex) sh_delete_textures(1, &d->tex);
    memset(d, 0, sizeof *d);
}

/* Depth/stencil for zeta surface `va` at scale sc, at least w x h logical.
 * The CPU rasteriser keeps one float buffer per zeta address; so does this,
 * as a D24S8 texture, one per (address, scale): depth follows the colour
 * target's scale, and a zeta address shared by a scaled and a native target
 * (zeta 0, the scratch depth) gets one of each rather than a re-creation
 * per switch. */
static GlDepth *depth_get(uint32_t va, uint32_t w, uint32_t h, uint32_t zbpp, float sc)
{
    int i, slot = -1, twin = 0;
    GlDepth *d;

    for (i = 0; i < NV_GL_MAX_DEPTH; i++) {
        d = &s_depth[i];
        if (d->tex && d->va == va && d->sc == sc) {
            if (d->w >= w && d->h >= h) {
                d->used = s_frame;
                return d;
            }
            if (sc != 1.0f)
                s_sst.depth_recreates++;
            depth_free(d);
            slot = i;
            break;
        }
        if (d->tex && d->va == va)
            twin = 1;
        if (!d->tex && slot < 0)
            slot = i;
    }
    if (twin && s_scale_cfg.scale != 1.0f)
        s_sst.depth_recreates++;      /* the same zeta at another scale */
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
    d->sc = sc;
    d->samp.wrap_s = d->samp.wrap_t = d->samp.filter = d->samp.swz_a = -1;
    d->cmp = 0;
    glGenTextures(1, &d->tex);
    sh_tex_scratch(GL_TEXTURE_2D, d->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, (GLsizei)nv2a_gl_scale_px(d->w, sc),
                 (GLsizei)nv2a_gl_scale_px(d->h, sc), 0,
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
    float sc;                             /* real pixels per guest pixel */
} GlTarget;

/* The framebuffer the last draw or clear went into: a change is a target
 * switch, which on a tiler ends a render pass (stored, later reloaded). */
static GLuint s_target_fbo;
/* That target was drawn without a zeta surface (rs->zeta_va 0: Fable's
 * bloom and noise chains, 16x16 to 256x256): the depth attached to it is
 * the scratch one of zeta address 0, whose contents mean nothing once the
 * target is left. */
static int s_target_scratch_depth;

static void note_target(GLuint fbo, uint32_t va, uint32_t w, uint32_t h)
{
    if (fbo != s_target_fbo) {
        s_target_fbo = fbo;
        s_st.target_switches++;
        sig_target(va, w, h);
    }
}

/* Before switching to framebuffer `next` (0: one not made yet): a tiler
 * stores every attachment at the end of a render pass unless told its
 * contents are dead, so the scratch depth of a target drawn without a zeta
 * surface is invalidated as the pass leaves it (where the API has it,
 * s_invalidate). Skipped when something else (a read-back) has bound
 * another framebuffer in between. */
static void leave_target(GLuint next)
{
    static const GLenum att = GL_DEPTH_STENCIL_ATTACHMENT;

    if (next == s_target_fbo || !s_target_scratch_depth)
        return;
    if (s_invalidate && s_sh.fbo == s_target_fbo) {
        nv2a_gl_invalidate(1, &att);
        s_st.invalidates++;
    }
    s_target_scratch_depth = 0;
}

/* Viewport (the whole framebuffer, w x h logical) and scissor (the clip
 * rectangle) of a target drawn at scale sc: real pixels. The vertex
 * mapping (u_vp) stays logical, so positions scale with the viewport. */
static void set_view(const Nv2aSurface *s, uint32_t w, uint32_t h, float sc)
{
    int32_t c[4];

    sh_viewport(0, 0, (GLsizei)nv2a_gl_scale_px(w, sc), (GLsizei)nv2a_gl_scale_px(h, sc));
    sh_cap(SH_SCISSOR, GL_SCISSOR_TEST, 1);
    nv2a_gl_scale_rect(s->clip_x, s->clip_y, s->width, s->height, sc, c);
    sh_scissor(c[0], c[1], c[2], c[3]);
}

/* The depth-only framebuffer for zeta surface `va` (glDrawBuffers GL_NONE):
 * a shadow-map pass writes depth and nothing else, so no colour target is
 * created in guest memory and nothing is read back. */
static int bind_depth_only(const Nv2aSurface *s, uint32_t zeta_va, GlTarget *t)
{
    uint32_t w = s->clip_x + s->width, h = s->clip_y + s->height;
    float sc = surface_scale(s);   /* masked: the colour surface's */
    GlDepth *d;

    if (!zeta_va || !w || !h || w > 4096 || h > 4096)
        return 0;
    d = depth_get(zeta_va, w, h, zeta_bpp(s), sc);
    leave_target(d->fbo);
    if (!d->fbo) {
        const GLenum none = GL_NONE;
        glGenFramebuffers(1, &d->fbo);
        sh_fbo(d->fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                               GL_TEXTURE_2D, d->tex, 0);
        glDrawBuffers(1, &none);
        glReadBuffer(GL_NONE);
        if (s_verbose)
            fprintf(stderr, "  [GPUGL] depth-only target for zeta 0x%08X %ux%u\n",
                    zeta_va, d->w, d->h);
    } else {
        sh_fbo(d->fbo);
    }
    note_target(d->fbo, zeta_va | 1, d->w, d->h);
    s_target_scratch_depth = 0;
    t->rt = NULL;
    t->w = d->w;
    t->h = d->h;
    t->bpp = 4;
    t->sc = sc;
    set_view(s, t->w, t->h, sc);
    return 1;
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

    if (!s->color_valid)
        return bind_depth_only(s, zeta_va, t);
    r = rt_find(s);
    if (s->color_masked && !r)
        return bind_depth_only(s, zeta_va, t);
    leave_target(r ? r->fbo : 0);
    if (!(r = rt_get(s)))
        return 0;
    rt_cpu_check(r, 1);
    sh_fbo(r->fbo);
    note_target(r->fbo, r->va, s->width, s->height);
    s_target_scratch_depth = zeta_va == 0;
    d = depth_get(zeta_va, r->gw, r->gh, zeta_bpp(s), r->sc)->tex;
    if (d != r->depth) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                               GL_TEXTURE_2D, d, 0);
        r->depth = d;
    }
    set_view(s, r->gw, r->gh, r->sc);
    r->used = s_frame;
    t->rt = r;
    t->w = r->gw;
    t->h = r->gh;
    t->bpp = r->bpp;
    t->sc = r->sc;
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
        rt_cpu_check(r, 1);
        return r;
    }
    return NULL;
}

/* ---- textures ----------------------------------------------------------- */

typedef struct {
    uint32_t offset, color, width, height, pitch, palette, cube;   /* key */
    uint32_t levels, depth;                                        /* key */
    uint32_t bytes;
    uint64_t hash;
    GLuint tex;
    uint32_t checked;               /* frame of the last hash check */
    uint32_t used;                  /* frame of the last tex_get hit (eviction) */
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
    /* Mip chain (XBA-120): levels uploaded; GL_TEXTURE_MIN_FILTER and the
     * LOD clamps as last set (samp_set resets MIN with MAG: min_seen is the
     * samp.filter it last saw). */
    uint32_t nlev;
    GLint min_cur, min_seen;
    float lod_lo, lod_hi;
    GLint wrap_r;                   /* a volume's GL_TEXTURE_WRAP_R as last set */
} GlTex;

#define NV_GL_MAX_TEX 2048          /* power of two */
static GlTex s_tex[NV_GL_MAX_TEX];
static int s_ntex;
static const GlTex *s_bound[4];     /* what each unit sampled last batch */
static uint32_t s_tex_evictions, s_tex_drops;   /* for the tests */

static uint32_t tex_slot_key(uint32_t offset, uint32_t color, uint32_t width,
                             uint32_t height, uint32_t cube)
{
    uint32_t h = (offset >> 7) ^ (offset >> 19) ^ color * 0x9E37u
               ^ width * 31u ^ height * 131u ^ cube;
    return h & (NV_GL_MAX_TEX - 1);
}

static uint32_t tex_slot(const Nv2aTexture *t, uint32_t cube)
{
    return tex_slot_key(t->offset, t->color, t->width, t->height, cube);
}

static void tex_drop_all(void)
{
    int i;
    for (i = 0; i < NV_GL_MAX_TEX; i++)
        if (s_tex[i].tex)
            sh_delete_textures(1, &s_tex[i].tex);
    memset(s_tex, 0, sizeof s_tex);
    memset(s_bound, 0, sizeof s_bound);
    s_ntex = 0;
    s_tex_drops++;
}

/* The table is 3/4 full: delete the entries not sampled in the last `age`
 * frames and put the others back in their probe order (linear probing has
 * no deletion of its own). Returns the entries left. Entries sampled this
 * frame always stay, so a texture already bound for this batch keeps its
 * GL name; s_bound is cleared because the entries move. */
static int tex_evict(uint32_t age)
{
    static GlTex keep[NV_GL_MAX_TEX];
    int i, n = 0;

    for (i = 0; i < NV_GL_MAX_TEX; i++) {
        GlTex *c = &s_tex[i];
        if (!c->tex && !c->bytes)
            continue;
        if (s_frame - c->used < age)
            keep[n++] = *c;
        else if (c->tex)
            sh_delete_textures(1, &c->tex);
    }
    memset(s_tex, 0, sizeof s_tex);
    memset(s_bound, 0, sizeof s_bound);
    for (i = 0; i < n; i++) {
        uint32_t slot = tex_slot_key(keep[i].offset, keep[i].color, keep[i].width,
                                     keep[i].height, keep[i].cube);
        while (s_tex[slot].tex || s_tex[slot].bytes)
            slot = (slot + 1) & (NV_GL_MAX_TEX - 1);
        s_tex[slot] = keep[i];
    }
    if (s_verbose)
        fprintf(stderr, "  [GPUGL] texture cache: %d of %d entries not sampled in %u frames"
                        " deleted at frame %u\n", s_ntex - n, s_ntex, age, s_frame);
    s_ntex = n;
    s_tex_evictions++;
    return n;
}

/* Room for more entries (the whole table used to be dropped here, and every live texture decoded and uploaded again in the
 * same frame): the textures unused for a second go first, then those not
 * sampled this frame or the last; everything only when the live set of two
 * frames alone nearly fills the table. */
static void tex_make_room(void)
{
    if (tex_evict(60) < NV_GL_MAX_TEX / 2)
        return;
    if (tex_evict(2) < NV_GL_MAX_TEX * 5 / 8)
        return;
    tex_drop_all();
}

static uint64_t tex_hash(const Nv2aTexture *t, uint32_t bytes)
{
    uint64_t h = hash4_bytes(guest(t->offset), bytes);
    s_st.hashes_kb += bytes >> 10;
    s_st.hash_tex_b += bytes;
    if (t->color == 0x0B && t->palette) {        /* the palette is content too */
        h ^= hash4_bytes(guest(t->palette), 1024) * 31u;
        s_st.hashes_kb += 1;
        s_st.hash_tex_b += 1024;
    }
    return h;
}

/* Upload one 2D image (a texture, or one cube face at `target`) as mip
 * level `level`. */
static int tex_upload_image(GLenum target, GLint level, const Nv2aTexture *t)
{
    uint32_t bb = d3d8_format_dxt_block_bytes(t->color), n, i;
    uint32_t *argb;
    uint8_t *buf;

    if (bb && s_s3tc) {
        GLenum f = t->color == 0x0C ? GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
                 : t->color == 0x0E ? GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
                 : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
        uint32_t size = ((t->width + 3) / 4) * ((t->height + 3) / 4) * bb;
        glCompressedTexImage2D(target, level, f, (GLsizei)t->width, (GLsizei)t->height,
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
    glTexImage2D(target, level, GL_RGBA8, (GLsizei)t->width, (GLsizei)t->height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, buf);
    s_st.upload_kb += n >> 8;
    free(buf);
    return 1;
}

/* Every level of one face (or of a 2D texture) at `target`, as many as
 * RECOMP_GPU_MIPS allows. Returns the levels uploaded, 0 on failure. */
static uint32_t tex_upload_levels(GLenum target, const Nv2aTexture *face)
{
    uint32_t n = s_mips && face->levels > 1 ? face->levels : 1, lv;
    Nv2aTexture one;

    for (lv = 0; lv < n; lv++) {
        if (!nv2a_backend_texture_level(face, lv, &one)) {
            if (!lv)
                one = *face;            /* a back end's own struct: level 0 as is */
            else
                break;
        }
        one.cube = 0;
        if (!tex_upload_image(target, (GLint)lv, &one))
            return lv ? lv : 0;
        if (one.width == 1 && one.height == 1)
            return lv + 1;
    }
    return lv;
}

/* A volume's levels (XBA-120): each decoded whole (w*h*d texels) and
 * uploaded with glTexImage3D. Returns the levels uploaded, 0 on failure. */
static uint32_t tex_upload_volume(const Nv2aTexture *t)
{
    uint32_t n = s_mips && t->levels > 1 ? t->levels : 1, lv, i;
    Nv2aTexture one;

    for (lv = 0; lv < n; lv++) {
        uint32_t texels;
        uint32_t *argb;
        uint8_t *buf;
        if (!nv2a_backend_texture_level(t, lv, &one))
            break;
        texels = one.width * one.height * one.depth;
        buf = (uint8_t *)malloc((size_t)texels * 8);
        if (!buf)
            break;
        argb = (uint32_t *)(buf + (size_t)texels * 4);
        if (!nv2a_backend_decode_texture(&one, argb)) {
            free(buf);
            break;
        }
        for (i = 0; i < texels; i++)
            argb_to_rgba(argb[i], buf + (size_t)i * 4);
        glTexImage3D(GL_TEXTURE_3D, (GLint)lv, GL_RGBA8, (GLsizei)one.width,
                     (GLsizei)one.height, (GLsizei)one.depth, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
        s_st.upload_kb += texels >> 8;
        free(buf);
        if (one.width == 1 && one.height == 1 && one.depth == 1)
            return lv + 1;
    }
    return lv;
}

static int tex_upload(GlTex *e, const Nv2aTexture *t)
{
    GLenum target = e->depth ? GL_TEXTURE_3D : e->cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
    Nv2aTexture one = *t;
    uint32_t nlev = 0;
    int ok = 1, f;

    if (!e->tex)
        glGenTextures(1, &e->tex);
    sh_tex_scratch(target, e->tex);
    one.cube = 0;
    e->wrap_r = -1;
    if (e->depth) {
        nlev = tex_upload_volume(t);
        ok = nlev != 0;
    } else if (e->cube) {
        uint32_t stride = e->bytes / 6;
        for (f = 0; f < 6 && ok; f++) {
            uint32_t n;
            one.offset = t->offset + (uint32_t)f * stride;
            n = tex_upload_levels(GL_TEXTURE_CUBE_MAP_POSITIVE_X + (GLenum)f, &one);
            ok = n != 0;
            nlev = f ? (n < nlev ? n : nlev) : n;
        }
    } else {
        nlev = tex_upload_levels(GL_TEXTURE_2D, &one);   /* a cube seen as 2D: face 0 */
        ok = nlev != 0;
    }
    e->nlev = nlev ? nlev : 1;
    glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, (GLint)e->nlev - 1);
    e->min_cur = e->min_seen = -1;
    e->lod_lo = e->lod_hi = -1.0f;
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
            && c->palette == t->palette && c->cube == cube && c->levels == t->levels
            && c->depth == (t->depth > 1 ? t->depth : 0)) {
            e = c;
            break;
        }
    }
    if (!e || (!e->tex && s_ntex >= NV_GL_MAX_TEX * 3 / 4)) {
        /* Entries are only made under 3/4, so an empty slot is always
         * found again after this. */
        tex_make_room();
        return tex_get(t, cube, unit);
    }
    e->used = s_frame;
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
    e->levels = t->levels;
    e->depth = t->depth > 1 ? t->depth : 0;
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

/* The minifying filter with a mip chain (XBA-120): SET_TEXTURE_FILTER's
 * MIN (xemu: 3 BOX_NEARESTLOD, 4 TENT_NEARESTLOD, 5 BOX_TENT_LOD, 6
 * TENT_TENT_LOD) picks the level filter; the texel filter stays the one
 * tex_filter chose (the CPU path's rule). Without levels, or for the LOD0
 * modes, the base filter. */
static GLint tex_min_filter(const Nv2aTexture *t, uint32_t nlev, GLint base)
{
    uint32_t min = (t->filter >> 16) & 0xFF;
    int lin = base == GL_LINEAR;

    if (nlev < 2 || min < 3 || min > 6)
        return base;
    if (min == 3 || min == 4)
        return lin ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
    return lin ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
}

/* SET_TEXTURE_FILTER's MIPMAP_LOD_BIAS: signed 5.8 fixed point. */
static float tex_lod_bias(const Nv2aTexture *t)
{
    int32_t b = (int32_t)(t->filter << 19) >> 19;
    return (float)b / 256.0f;
}

static void tex_bind(GlTex *e, const Nv2aTexture *t, int unit, uint32_t shade)
{
    GLenum target = e->depth ? GL_TEXTURE_3D : e->cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
    GLint f = tex_filter(t, shade), minf = tex_min_filter(t, e->nlev, f);

    sh_active((int)unit);
    sh_tex(target, e->tex);
    samp_set(target, &e->samp, f,
             wrap_mode(t->addr_u, (int)t->border_color),
             wrap_mode(t->addr_v, (int)t->border_color), t->border, GL_ALPHA);
    if (e->min_seen != e->samp.filter) {    /* samp_set set MIN to f */
        e->min_seen = e->samp.filter;
        e->min_cur = e->samp.filter;
    }
    if (e->min_cur != minf) {
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, minf);
        e->min_cur = minf;
    }
    if (e->depth) {
        GLint wr = wrap_mode(t->addr_p, 0);
        if (wr == GL_CLAMP_TO_BORDER)
            wr = GL_CLAMP_TO_EDGE;
        if (e->wrap_r != wr) {
            glTexParameteri(target, GL_TEXTURE_WRAP_R, wr);
            e->wrap_r = wr;
        }
    }
    if (e->nlev > 1) {
        /* SET_TEXTURE_CONTROL0's LOD clamps (4.8 fixed point, xemu). */
        float lo = (float)((t->control0 >> 18) & 0xFFF) / 256.0f;
        float hi = (float)((t->control0 >> 6) & 0xFFF) / 256.0f;
        if (lo != e->lod_lo || hi != e->lod_hi) {
            glTexParameterf(target, GL_TEXTURE_MIN_LOD, lo);
            glTexParameterf(target, GL_TEXTURE_MAX_LOD, hi);
            e->lod_lo = lo;
            e->lod_hi = hi;
        }
        s_lodb[unit] = tex_lod_bias(t);
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
    GLint u_bm, u_bso;              /* -1 without a bump-env stage */
    GLint u_psize;                  /* the vertex shader's point size */
    GLint u_lodb;                   /* each stage's LOD bias (vec4) */
    GLint u_eye;                    /* SET_EYE_VECTOR (DOT_RFLCT_SPEC_CONST) */
    /* With a translated vertex program (vsh_hash != 0): its uniforms, the
     * location of each c[i] (-1 where the compiler dropped it), and the
     * constant serial this program's copy of c[] is synced to. */
    uint64_t vsh_hash;
    GLint u_xf, u_fog;
    GLint *cloc;
    uint64_t const_synced;
    uint64_t src_hash;              /* the program binary cache's key */
    int pending;                    /* compiling off the draw path (XBA-310): prog 0 until done */
    int job;                        /* then its job, index + 1 */
    /* The uniform values this program was last sent (us_same): one not
     * changed since is not sent again. Valid for GL program us_prog only
     * (0: nothing yet; a program linked again starts over): us_ok has a bit
     * per NV_GL_U_* held in us[], cvalid one per c[i] held in cshadow
     * (allocated at the first sync_constants). */
    GLuint us_prog;
    uint32_t us_ok;
    float us[129];
    float (*cshadow)[4];
    uint32_t cvalid[6];
} GlProg;

/* A translated vertex program (nv2a_gl_vsh.c): its GLSL, kept so that the
 * program binary cache can key on it, compiled the first time a program
 * that uses it has to be linked (with every program it is in found in the
 * cache, never). */
typedef struct {
    uint64_t hash;                  /* 0: empty slot */
    uint32_t count;
    char *src;
    uint64_t src_hash;
    uint64_t fmt;                   /* input formats (nv2a_gl_vsh_body_fmt) */
    GLuint sh;                      /* 0 until compiled */
    int failed;                     /* translation or compile failed: do not retry */
} GlVsh;

#define NV_GL_MAX_PROG 2048         /* power of two; pipelines x programs */
#define NV_GL_MAX_VSH  512          /* power of two */
static GlProg s_prog[NV_GL_MAX_PROG];
static uint32_t s_nprog;            /* slots taken, failed ones included */

static GlVsh s_vsh[NV_GL_MAX_VSH];
static uint32_t s_nvsh;
static uint32_t s_prog_drops, s_vsh_drops;   /* for the tests */

/* The tables are dropped whole when 3/4 full, as the VB/IB/VAO caches
 * are: entries were never retired, so a long session across many areas
 * ended with every new pipeline refused (batches skipped, or drawn on the
 * CPU) and every miss probing the whole table. A dropped program comes
 * back from the program binary cache (glProgramBinary, no compile) when it
 * is on; a dropped vertex shader is translated again. Neither is called
 * between a lookup and its use: gl_draw and gl_draw_program take the
 * vertex shader first, then the program, and use both at once. */
static void job_disown_all(void);

static void prog_drop_all(void)
{
    uint32_t i;

    if (s_verbose)
        fprintf(stderr, "  [GPUGL] program table full (%u entries): dropping it at frame %u\n",
                s_nprog, s_frame);
    for (i = 0; i < NV_GL_MAX_PROG; i++) {
        if (s_prog[i].prog) {
            if (s_sh.prog == s_prog[i].prog)
                sh_program(0);
            glDeleteProgram(s_prog[i].prog);
        }
        free(s_prog[i].cloc);
        free(s_prog[i].cshadow);
    }
    memset(s_prog, 0, sizeof s_prog);
    s_nprog = 0;
    s_prog_drops++;
    job_disown_all();               /* compiles in flight go to the warm table when done */
}

/* The programs keep their vertex shader's hash (it is computed from the
 * batch, not stored), so they stay valid; a linked program keeps a deleted
 * shader's code. */
static void vsh_drop_all(void)
{
    uint32_t i;

    if (s_verbose)
        fprintf(stderr, "  [GPUGL] vertex shader table full (%u entries): dropping it at"
                        " frame %u\n", s_nvsh, s_frame);
    for (i = 0; i < NV_GL_MAX_VSH; i++) {
        if (s_vsh[i].sh)
            glDeleteShader(s_vsh[i].sh);
        free(s_vsh[i].src);
    }
    memset(s_vsh, 0, sizeof s_vsh);
    s_nvsh = 0;
    s_vsh_drops++;
}

/* 64-bit FNV-1a, continuing from h (shader sources; not hash_bytes, whose
 * byte count is the texture statistics'). */
static uint64_t hash_str(uint64_t h, const char *t)
{
    for (; *t; t++)
        h = (h ^ (uint8_t)*t) * 0x100000001B3ull;
    return h;
}

/* The shader for program pb reading its inputs in formats `fmt` (0: all
 * gathered vec4s). The input formats are part of the key (and of hash,
 * which prog_get keys its programs on): the same program drawn from
 * gathered and raw vertices is two shaders. */
static GlVsh *vsh_get(const Nv2aProgramBatch *pb, uint64_t fmt)
{
    static char body[131072];
    uint64_t h = (pb->hash ^ (fmt * 0xC2B2AE3D27D4EB4Full)) | 1;
    uint32_t i, slot = (uint32_t)h & (NV_GL_MAX_VSH - 1);
    GlVsh *v = NULL;

    for (i = 0; i < NV_GL_MAX_VSH; i++) {
        GlVsh *c = &s_vsh[(slot + i) & (NV_GL_MAX_VSH - 1)];
        if (!c->hash) {
            v = c;
            break;
        }
        if (c->hash == h && c->count == pb->ins_count && c->fmt == fmt)
            return c->failed ? NULL : c;
    }
    if (!v || s_nvsh >= NV_GL_MAX_VSH * 3 / 4) {
        vsh_drop_all();
        return vsh_get(pb, fmt);
    }
    s_nvsh++;
    v->hash = h;
    v->count = pb->ins_count;
    v->fmt = fmt;
    if (!(pb->ffp
          ? nv2a_gl_vsh_ffp_body(pb->ffp, NV2A_GL_ES ? NV2A_GL_VSH_RENDER
                                                     : NV2A_GL_VSH_RENDER | NV2A_GL_VSH_CLIP_DISTANCE,
                                 fmt, body, sizeof body)
          : nv2a_gl_vsh_body_fmt(pb->ins, pb->ins_count,
                                 NV2A_GL_ES ? NV2A_GL_VSH_RENDER
                                            : NV2A_GL_VSH_RENDER | NV2A_GL_VSH_CLIP_DISTANCE,
                                 fmt, body, sizeof body, NULL))
        || !(v->src = strdup(body))) {
        v->failed = 1;
        return NULL;
    }
    v->src_hash = hash_str(0xCBF29CE484222325ull, body);
    return v;
}

/* ---- program binary cache ----------------------------------------------- */

/* Linked programs are kept on disk (glGetProgramBinary) and loaded at the
 * next start (glProgramBinary) instead of compiled: a mobile driver takes
 * tens of milliseconds a program, and Fable links ~70 by Oakvale, each the
 * first time a batch needs it, on the executor thread, mid-frame.
 *
 *   where   <save dir>/shadercache/<title id>/<source hash>.bin -- the save
 *           dir is $XDG_DATA_HOME/xboxrecomp on desktop POSIX and
 *           <internal files dir>/save on Android (xbox_path_init).
 *   key     a 64-bit hash of the program's whole GLSL (header, vertex and
 *           fragment shader), so a change of either generator, of the
 *           pipeline key or of the vertex program gives another file.
 *   header  generator version (NV_GL_PCACHE_VERSION), GL_RENDERER and
 *           GL_VERSION as strings, binary format and length: a file from
 *           another driver or GPU is deleted and the program compiled.
 *   warm-up the files found at context creation are loaded a few per flip
 *           (NV_GL_PCACHE_WARM) during the first frames -- the intro
 *           movies, the title screen -- so the first in-world frames find
 *           them linked. A program needed before its turn is loaded from
 *           its file there and then.
 * A driver that offers no binary format, or RECOMP_GPU_SHADER_CACHE=0,
 * compiles every time, as before. */
#define NV_GL_PCACHE_VERSION 1
#define NV_GL_PCACHE_WARM    4      /* binaries loaded per flip in the warm-up */
#define NV_GL_MAX_WARM       1024

typedef struct {
    char magic[8];                  /* "NV2AGLPB" */
    uint32_t gen_version, format, length, reserved;
    uint64_t src_hash;
    char renderer[128], version[128];
} GlPcHeader;

static int s_pc_on;
static char s_pc_dir[900];
static char s_pc_renderer[128], s_pc_version[128];
static uint64_t *s_pc_list;         /* files found at start-up, warm-up order */
static int s_pc_nlist, s_pc_next;
static struct { uint64_t hash; GLuint prog; } s_warm[NV_GL_MAX_WARM];
static int s_nwarm;
static uint32_t s_pc_total_compiled, s_pc_total_loaded;

extern const char *xbox_path_save_dir_utf8(void);
static void pl_load(void);
static void pl_warm(void);
static void pl_flush(void);
static int s_async;                 /* how programs compile (XBA-310): NV_ASYNC_* below */
static uint32_t s_wait_max_us, s_wait_left_us;

static void mkdir_p(char *path)
{
    char *q;

    for (q = path + 1; *q; q++)
        if (*q == '/') {
            *q = 0;
            mkdir(path, 0755);
            *q = '/';
        }
    mkdir(path, 0755);
}

/* The title ID from the XBE certificate (header mapped at 0x10000, the
 * certificate's address at +0x118, the ID at +8 in it). */
static uint32_t title_id(void)
{
    uint32_t cert = 0, id = 0;

    memcpy(&cert, guest(0x00010118u), 4);
    if (cert < 0x00010000u || cert >= 0x04000000u)
        return 0;
    memcpy(&id, guest(cert + 8), 4);
    return id;
}

static void pc_path(uint64_t h, char *out, size_t n, const char *ext)
{
    snprintf(out, n, "%s/%016llX.%s", s_pc_dir, (unsigned long long)h, ext);
}

static void pc_init(void)
{
    const char *e = getenv("RECOMP_GPU_SHADER_CACHE"), *save;
    GLint nfmt = 0;
    DIR *d;
    struct dirent *de;
    int cap = 0;

    s_pc_on = 0;
    s_pc_nlist = s_pc_next = 0;
    host_shader_progress_reset();
    if (e && strcmp(e, "0") == 0)
        return;
    glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &nfmt);
    save = xbox_path_save_dir_utf8();
    if (nfmt <= 0 || !save || !save[0]) {
        fprintf(stderr, "  [GPUGL] program binary cache off (%s)\n",
                nfmt <= 0 ? "the driver offers no binary format" : "no save dir");
        return;
    }
    snprintf(s_pc_renderer, sizeof s_pc_renderer, "%s", (const char *)glGetString(GL_RENDERER));
    snprintf(s_pc_version, sizeof s_pc_version, "%s", (const char *)glGetString(GL_VERSION));
    snprintf(s_pc_dir, sizeof s_pc_dir, "%s/shadercache/%08X", save, title_id());
    mkdir_p(s_pc_dir);
    if ((d = opendir(s_pc_dir)) != NULL) {
        while ((de = readdir(d)) != NULL) {
            char *end;
            unsigned long long h;
            if (strlen(de->d_name) != 20 || strcmp(de->d_name + 16, ".bin") != 0)
                continue;
            h = strtoull(de->d_name, &end, 16);
            if (end != de->d_name + 16 || !h)
                continue;
            if (s_pc_nlist == cap) {
                uint64_t *l = (uint64_t *)realloc(s_pc_list, (size_t)(cap ? cap * 2 : 64)
                                                             * sizeof *l);
                if (!l)
                    break;
                s_pc_list = l;
                cap = cap ? cap * 2 : 64;
            }
            s_pc_list[s_pc_nlist++] = h;
        }
        closedir(d);
    }
    s_pc_on = 1;
    /* The badge: "SHADERS 1/<binaries>" until the warm-up has been through them. */
    host_shader_progress_expect((uint32_t)s_pc_nlist);
    fprintf(stderr, "  [GPUGL] program binary cache %s: %d binaries, warm-up %d a frame\n",
            s_pc_dir, s_pc_nlist, NV_GL_PCACHE_WARM);
    pl_load();                      /* the program list: what has no binary, compiled ahead */
}

/* The program in file `h`, linked from its binary; 0 when there is no file
 * or it does not load (then deleted: another driver, a damaged file). */
static GLuint pc_load(uint64_t h)
{
    char path[1024];
    GlPcHeader hd;
    FILE *f;
    void *bin = NULL;
    GLuint prog = 0;
    GLint ok = 0;

    pc_path(h, path, sizeof path, "bin");
    if (!(f = fopen(path, "rb")))
        return 0;
    if (fread(&hd, sizeof hd, 1, f) != 1 || memcmp(hd.magic, "NV2AGLPB", 8) != 0
        || hd.gen_version != NV_GL_PCACHE_VERSION || hd.src_hash != h
        || !hd.length || hd.length > (64u << 20)
        || strncmp(hd.renderer, s_pc_renderer, sizeof hd.renderer) != 0
        || strncmp(hd.version, s_pc_version, sizeof hd.version) != 0
        || !(bin = malloc(hd.length)) || fread(bin, hd.length, 1, f) != 1) {
        fclose(f);
        free(bin);
        remove(path);
        return 0;
    }
    fclose(f);
    prog = glCreateProgram();
    glProgramBinary(prog, (GLenum)hd.format, bin, (GLsizei)hd.length);
    free(bin);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        glDeleteProgram(prog);
        remove(path);
        return 0;
    }
    return prog;
}

static void pc_store(GLuint prog, uint64_t h)
{
    char path[1024], tmp[1024];
    GlPcHeader hd;
    GLint len = 0;
    GLsizei got = 0;
    GLenum fmt = 0;
    void *bin;
    FILE *f;
    int ok;

    glGetProgramiv(prog, GL_PROGRAM_BINARY_LENGTH, &len);
    if (len <= 0 || !(bin = malloc((size_t)len)))
        return;
    glGetProgramBinary(prog, len, &got, &fmt, bin);
    if (got <= 0) {
        free(bin);
        return;
    }
    memset(&hd, 0, sizeof hd);
    memcpy(hd.magic, "NV2AGLPB", 8);
    hd.gen_version = NV_GL_PCACHE_VERSION;
    hd.format = (uint32_t)fmt;
    hd.length = (uint32_t)got;
    hd.src_hash = h;
    snprintf(hd.renderer, sizeof hd.renderer, "%s", s_pc_renderer);
    snprintf(hd.version, sizeof hd.version, "%s", s_pc_version);
    pc_path(h, path, sizeof path, "bin");
    pc_path(h, tmp, sizeof tmp, "tmp");
    if (!(f = fopen(tmp, "wb"))) {
        free(bin);
        return;
    }
    ok = fwrite(&hd, sizeof hd, 1, f) == 1 && fwrite(bin, (size_t)got, 1, f) == 1;
    ok = fclose(f) == 0 && ok;
    free(bin);
    if (!ok || rename(tmp, path) != 0)
        remove(tmp);
}

/* Is the program for source hash h linked already (in use or warmed)? */
static int pc_have(uint64_t h)
{
    int i;

    for (i = 0; i < s_nwarm; i++)
        if (s_warm[i].hash == h)
            return 1;
    for (i = 0; i < NV_GL_MAX_PROG; i++)
        if (s_prog[i].prog && s_prog[i].src_hash == h)
            return 1;
    return 0;
}

/* The program for source hash h from the cache, warmed or from its file. */
static GLuint pc_take(uint64_t h)
{
    int i;

    for (i = 0; i < s_nwarm; i++)
        if (s_warm[i].hash == h) {
            GLuint prog = s_warm[i].prog;
            s_warm[i] = s_warm[--s_nwarm];
            return prog;
        }
    return s_pc_on ? pc_load(h) : 0;
}

/* Warm-up, at the flip: the next few files found at start-up. */
static void pc_warm(void)
{
    int n = 0;

    while (s_pc_on && n < NV_GL_PCACHE_WARM && s_pc_next < s_pc_nlist) {
        uint64_t h = s_pc_list[s_pc_next++];
        GLuint prog;
        host_shader_progress_done(1);       /* loaded, already there or refused: the badge counts it */
        if (s_nwarm >= NV_GL_MAX_WARM) {
            /* No room left for warmed programs: the rest load when a draw needs them. */
            host_shader_progress_done((uint32_t)(s_pc_nlist - s_pc_next));
            s_pc_next = s_pc_nlist;
            break;
        }
        if (pc_have(h))
            continue;
        n++;
        if ((prog = pc_load(h)) != 0) {
            s_warm[s_nwarm].hash = h;
            s_warm[s_nwarm].prog = prog;
            s_nwarm++;
        }
    }
    if (s_pc_on && s_pc_list && s_pc_next >= s_pc_nlist) {
        if (s_verbose)
            fprintf(stderr, "  [GPUGL] program binary cache: warm-up done, %d programs ready\n",
                    s_nwarm);
        free(s_pc_list);
        s_pc_list = NULL;
        s_pc_nlist = s_pc_next = 0;
    }
    /* XBA-310: a new frame's wait budget, the program list's new records
     * written, compiles nobody waits for collected, the list's next queued. */
    s_wait_left_us = s_wait_max_us * 2u;
    pl_flush();
    if (s_async)
        pl_warm();
}

/* Uniform locations and sampler units of a linked program (a program
 * loaded from a binary starts with every uniform at its default too). */
static void prog_setup(GlProg *p, int program)
{
    uint32_t i;

    sh_program(p->prog);
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
    p->u_psize = glGetUniformLocation(p->prog, "u_psize");
    p->u_lodb = glGetUniformLocation(p->prog, "u_lodb");
    p->u_eye = glGetUniformLocation(p->prog, "u_eye");
    if (program) {
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
}

/* Compile and link program p from fragment body `fs` and the fixed vertex
 * shader or vsh (compiled here the first time). 0 on failure. */
static int prog_link(GlProg *p, const char *fs_body, GlVsh *vsh)
{
    GLuint fs;
    GLint ok = 0;

    if (vsh && !vsh->sh) {
        if (!(vsh->sh = compile(GL_VERTEX_SHADER, vsh->src))) {
            vsh->failed = 1;
            return 0;
        }
        s_st.compiles++;
        s_pc_total_compiled++;
    }
    if (!(fs = compile(GL_FRAGMENT_SHADER, fs_body)))
        return 0;
    p->prog = glCreateProgram();
    if (s_pc_on)
        glProgramParameteri(p->prog, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
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
        return 0;
    }
    s_st.compiles++;
    s_pc_total_compiled++;
    host_shader_progress_live();            /* the badge: "COMPILING... (n)" */
    return 1;
}

/* ---- programs compiled off the draw path (XBA-310) --------------------- */

/* A program a draw needs for the first time used to be compiled and linked
 * there and then (prog_link), on the executor thread, mid-frame: tens of
 * milliseconds a program on a mobile driver, every first run and after
 * every driver update (the binary cache above only helps from the second).
 * Now the draw starts a compile job and waits a little for it:
 *
 *   worker   a thread with a context of its own in the back end's share
 *            group (nv2a_gl_shared_context_*) compiles and links the job's
 *            GLSL, stores the binary (pc_store) and hands the linked
 *            program back; the default where the context can be had
 *   khr      GL_KHR_parallel_shader_compile (or ARB): the job's compile and
 *            link are issued on the executor and polled for completion
 *            (GL_COMPLETION_STATUS_KHR); where there is no shared context
 *   sync     neither (macOS 4.1 core), or RECOMP_GPU_ASYNC_SHADERS=0: at
 *            the draw, as before
 * RECOMP_GPU_ASYNC_SHADERS=khr forces khr (tests).
 *
 * While a program is not ready its batches wait a short bounded time, then
 * are skipped: a draw that finds its program compiling waits up to
 * RECOMP_GPU_SHADER_WAIT_MS (default 4 ms) for it, at most twice that over
 * a frame; past that the batch is drawn with a stand-in program that
 * produces no fragment (s_standin: every vertex at one point outside the clip
 * volume, the fragment shader discards), so the caller's work and its GL
 * state stay as they are and nothing reaches the target. The cost: an
 * object can be missing from the first frame or two it appears in, rather
 * than the frame stalling for the whole compile; a target the title draws
 * once and keeps (a cached render target, a loading screen) keeps the hole
 * until it is drawn again. Occlusion queries around a skipped batch count
 * no sample. A large RECOMP_GPU_SHADER_WAIT_MS (the GL tests) waits for
 * every program.
 *
 * The program list (programs.list next to the binaries): the GLSL of every
 * program linked, written as they appear (the vertex shaders once each),
 * kept whatever the driver: a run whose binaries are gone or of another
 * driver (a driver update, the first run after the app's data was cleared
 * but not the save dir) compiles them ahead from it during the first
 * frames, a few in flight at a time, with a known total for the window's
 * badge (host_shader_progress.h). Format: "NV2AGLPL", version, GLES flag,
 * generator version (NV_GL_PCACHE_VERSION), then records 'V' <vertex hash>
 * <length> <body> and 'P' <vertex hash, 0 for the fixed one> <length>
 * <fragment body>, native byte order (the file stays on its device). A file
 * of another version, GL flavour or generator is replaced; a broken record
 * at the end (a run killed while appending) is cut off. At most
 * NV_GL_PLIST_MAX programs (what the warm table holds). */
#ifndef GL_COMPLETION_STATUS_KHR
#define GL_COMPLETION_STATUS_KHR 0x91B1
#endif
#define NV_GL_MAX_JOBS     256      /* compiles in flight or done and not taken */
#define NV_GL_AHEAD_WORKER 4        /* list programs queued ahead at a time: worker */
#define NV_GL_AHEAD_KHR    2        /* ... and with KHR_parallel_shader_compile */
#define NV_GL_PLIST_VERSION 1
#define NV_GL_PLIST_MAX    NV_GL_MAX_WARM
#define NV_GL_PLIST_BODY   (256u << 10)  /* a longer record is not read */

enum { NV_ASYNC_SYNC, NV_ASYNC_WORKER, NV_ASYNC_KHR };
enum { JOB_FREE, JOB_QUEUED, JOB_COMPILING, JOB_READY, JOB_FAILED };

typedef struct {
    int state;                      /* atomic: the worker publishes READY / FAILED */
    int ahead;                      /* compiled ahead from the program list (the badge's batch) */
    GlProg *owner;                  /* the pending program table entry waiting for it */
    int next;                       /* the worker's queue: next job index, -1 none */
    uint64_t src_hash;
    char *vs, *fs;                  /* bodies without the GLSL header; vs NULL: s_vs's */
    GLuint prog, sh_v, sh_f;        /* khr: the shaders stay attached until done */
} GlJob;

static GlJob s_job[NV_GL_MAX_JOBS];
static int s_ahead_inflight;
static uint32_t s_skips, s_waited_us, s_async_done;   /* since the last log */
static GlProg s_standin;            /* the stand-in that draws nothing */

static pthread_t s_worker;
static pthread_mutex_t s_job_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_job_cv = PTHREAD_COND_INITIALIZER;
static int s_worker_on, s_worker_quit;
static int s_worker_state;          /* the worker's start: 0 waiting, 1 its context is current, -1 not */
static int s_qlive_head = -1, s_qlive_tail = -1, s_qahead_head = -1, s_qahead_tail = -1;

/* The program list in memory: the source hashes known (read or recorded),
 * the vertex shaders written, the warm-up's to-do (list order, the
 * programs without a binary first) and the records not written yet. */
typedef struct { uint64_t src_hash; int vsi; char *fs; int bin; } GlPlTodo;
static uint64_t *s_pl_known;
static int s_npl_known;
static uint64_t *s_pl_vknown;
static int s_npl_vknown;
static struct { uint64_t hash; char *src; } *s_plv;   /* the list's vertex bodies (warm-up) */
static int s_nplv;
static GlPlTodo *s_pl_todo;
static int s_npl_todo, s_pl_next;
static char *s_pl_buf;
static size_t s_pl_buf_n, s_pl_buf_cap;

static int job_is_done(GlJob *j);

/* The whole program's source hash, as prog_get computes it. */
static uint64_t prog_src_hash(const char *vs_body, const char *fs_body)
{
    return hash_str(hash_str(hash_str(0xCBF29CE484222325ull, nv2a_gl_glsl_header(NV2A_GL_ES)),
                             vs_body ? vs_body : nv2a_gl_vertex_body()),
                    fs_body);
}

/* Link vs + fs into a program in the current context (either thread);
 * 0 on failure (logged). */
static GLuint link_bodies(const char *vs_body, const char *fs_body, uint64_t h)
{
    GLuint vs = compile(GL_VERTEX_SHADER, vs_body ? vs_body : nv2a_gl_vertex_body());
    GLuint fs = vs ? compile(GL_FRAGMENT_SHADER, fs_body) : 0;
    GLuint prog = 0;
    GLint ok = 0;

    if (vs && fs) {
        prog = glCreateProgram();
        if (s_pc_on)
            glProgramParameteri(prog, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
        glAttachShader(prog, vs);
        glAttachShader(prog, fs);
        glLinkProgram(prog);
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[2048];
            glGetProgramInfoLog(prog, sizeof log, NULL, log);
            fprintf(stderr, "  [GPUGL] link failed: %s\n", log);
            glDeleteProgram(prog);
            prog = 0;
        } else if (s_pc_on) {
            pc_store(prog, h);
        }
    }
    if (vs)
        glDeleteShader(vs);
    if (fs)
        glDeleteShader(fs);
    return prog;
}

extern void xbox_HostThreadFloat(const char *role);   /* kernel_thread.c (XBA-305) */
extern void recomp_host_cpus_untrack(void);           /* host_cpus.c */

static void *job_worker(void *arg)
{
    int ok;

    xbox_HostThreadFloat("shader worker");   /* off the guest's core, as the executor */
    ok = nv2a_gl_shared_context_bind();

    (void)arg;
    pthread_mutex_lock(&s_job_mu);
    s_worker_state = ok ? 1 : -1;   /* job_init waits for this */
    pthread_cond_broadcast(&s_job_cv);
    pthread_mutex_unlock(&s_job_mu);
    if (!ok)
        return NULL;
    for (;;) {
        GlJob *j;
        int i;
        pthread_mutex_lock(&s_job_mu);
        while (s_qlive_head < 0 && s_qahead_head < 0 && !s_worker_quit)
            pthread_cond_wait(&s_job_cv, &s_job_mu);
        if (s_worker_quit) {
            pthread_mutex_unlock(&s_job_mu);
            break;
        }
        /* A program a draw is waiting for goes before the list's. */
        if (s_qlive_head >= 0) {
            i = s_qlive_head;
            s_qlive_head = s_job[i].next;
            if (s_qlive_head < 0)
                s_qlive_tail = -1;
        } else {
            i = s_qahead_head;
            s_qahead_head = s_job[i].next;
            if (s_qahead_head < 0)
                s_qahead_tail = -1;
        }
        j = &s_job[i];
        pthread_mutex_unlock(&s_job_mu);
        j->prog = link_bodies(j->vs, j->fs, j->src_hash);
        glFinish();                 /* complete before the executor's context uses it */
        __atomic_store_n(&j->state, j->prog ? JOB_READY : JOB_FAILED, __ATOMIC_RELEASE);
    }
    nv2a_gl_shared_context_unbind();
    recomp_host_cpus_untrack();     /* its tid may go to another thread */
    return NULL;
}

/* The stand-in program skipped batches are drawn with (see above). */
static void skip_init(void)
{
    static const char vs[] =
        "void main() { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); gl_PointSize = 1.0; }\n";
    static const char fs[] = "out vec4 o;\nvoid main() { o = vec4(0.0); discard; }\n";
    GLuint v = compile(GL_VERTEX_SHADER, vs), f = v ? compile(GL_FRAGMENT_SHADER, fs) : 0;
    GLint ok = 0;
    int i;

    memset(&s_standin, 0, sizeof s_standin);
    if (v && f) {
        s_standin.prog = glCreateProgram();
        glAttachShader(s_standin.prog, v);
        glAttachShader(s_standin.prog, f);
        glLinkProgram(s_standin.prog);
        glGetProgramiv(s_standin.prog, GL_LINK_STATUS, &ok);
    }
    if (v)
        glDeleteShader(v);
    if (f)
        glDeleteShader(f);
    s_standin.cloc = (GLint *)malloc(192 * sizeof(GLint));
    if (!ok || !s_standin.cloc) {
        if (s_standin.prog)
            glDeleteProgram(s_standin.prog);
        free(s_standin.cloc);
        memset(&s_standin, 0, sizeof s_standin);
        return;
    }
    for (i = 0; i < 192; i++)
        s_standin.cloc[i] = -1;
    s_standin.u_vp = s_standin.u_tscale = s_standin.u_c0 = s_standin.u_c1 = -1;
    s_standin.u_fc0 = s_standin.u_fc1 = s_standin.u_fogc = s_standin.u_aref = -1;
    s_standin.u_bm = s_standin.u_bso = s_standin.u_psize = s_standin.u_lodb = -1;
    s_standin.u_eye = -1;
    s_standin.u_xf = s_standin.u_fog = -1;
    s_standin.const_synced = ~0ull;    /* never uploads constants */
}

/* At context creation (ensure_init, before pc_init): how programs compile
 * from now on, its worker started. */
static void job_init(void)
{
    const char *e = getenv("RECOMP_GPU_ASYNC_SHADERS");
    const char *w = getenv("RECOMP_GPU_SHADER_WAIT_MS");
    int i, khr = has_ext("GL_KHR_parallel_shader_compile") || has_ext("GL_ARB_parallel_shader_compile");

    for (i = 0; i < NV_GL_MAX_JOBS; i++)
        s_job[i].state = JOB_FREE;
    s_qlive_head = s_qlive_tail = s_qahead_head = s_qahead_tail = -1;
    s_ahead_inflight = 0;
    s_async = NV_ASYNC_SYNC;
    if (w && *w) {
        double ms = atof(w);
        s_wait_max_us = ms <= 0 ? 0 : ms >= 600000.0 ? 600000000u : (uint32_t)(ms * 1000.0);
    } else {
        s_wait_max_us = 4000;
    }
    s_wait_left_us = s_wait_max_us * 2u;
    skip_init();
    if (!s_standin.prog || (e && strcmp(e, "0") == 0)) {
        fprintf(stderr, "  [GPUGL] shaders: compiled at the draw%s\n",
                s_standin.prog ? " (RECOMP_GPU_ASYNC_SHADERS=0)" : " (no stand-in program)");
        return;
    }
    if (!(e && strcmp(e, "khr") == 0) && nv2a_gl_shared_context_create()) {
        s_worker_quit = 0;
        s_worker_state = 0;
        if (pthread_create(&s_worker, NULL, job_worker, NULL) == 0) {
            /* Its context current before any job is queued for it. */
            pthread_mutex_lock(&s_job_mu);
            while (!s_worker_state)
                pthread_cond_wait(&s_job_cv, &s_job_mu);
            pthread_mutex_unlock(&s_job_mu);
            if (s_worker_state > 0) {
                s_worker_on = 1;
                s_async = NV_ASYNC_WORKER;
            } else {
                pthread_join(s_worker, NULL);
            }
        }
        if (s_async != NV_ASYNC_WORKER)
            nv2a_gl_shared_context_destroy();
    }
    if (s_async == NV_ASYNC_SYNC && khr)
        s_async = NV_ASYNC_KHR;
    fprintf(stderr, "  [GPUGL] shaders: %s, a draw waits up to %.1f ms for its program, then is skipped\n",
            s_async == NV_ASYNC_WORKER ? "compiled on a worker thread (shared context)"
            : s_async == NV_ASYNC_KHR ? "compiled in parallel (KHR_parallel_shader_compile)"
                                      : "compiled at the draw (no shared context, no parallel compile)",
            (double)s_wait_max_us / 1000.0);
}

/* The jobs, their worker and the stand-in, gone (gl_reset_objects): the
 * worker joined first; lost: the context is gone, names only forgotten. */
static void job_shutdown(int lost)
{
    int i;

    if (s_async == NV_ASYNC_WORKER) {
        pthread_mutex_lock(&s_job_mu);
        s_worker_quit = 1;
        pthread_cond_broadcast(&s_job_cv);
        pthread_mutex_unlock(&s_job_mu);
        pthread_join(s_worker, NULL);
        s_worker_on = 0;
        nv2a_gl_shared_context_destroy();
    }
    for (i = 0; i < NV_GL_MAX_JOBS; i++) {
        GlJob *j = &s_job[i];
        if (j->state == JOB_FREE)
            continue;
        if (!lost) {
            if (j->prog)
                glDeleteProgram(j->prog);
            if (j->sh_v)
                glDeleteShader(j->sh_v);
            if (j->sh_f)
                glDeleteShader(j->sh_f);
        }
        free(j->vs);
        free(j->fs);
        memset(j, 0, sizeof *j);
    }
    if (!lost && s_standin.prog)
        glDeleteProgram(s_standin.prog);
    free(s_standin.cloc);
    memset(&s_standin, 0, sizeof s_standin);
    s_qlive_head = s_qlive_tail = s_qahead_head = s_qahead_tail = -1;
    s_ahead_inflight = 0;
    s_async = NV_ASYNC_SYNC;
}

static GlJob *job_find(uint64_t h)
{
    int i;

    for (i = 0; i < NV_GL_MAX_JOBS; i++)
        if (s_job[i].state != JOB_FREE && s_job[i].src_hash == h)
            return &s_job[i];
    return NULL;
}

/* A compile of (vs, fs) started the configured way; NULL when it cannot be
 * (sync mode, no free slot, the worker gone): the caller compiles at once. */
static GlJob *job_start(uint64_t h, const char *vs, const char *fs, int ahead)
{
    GlJob *j = NULL;
    int i;

    if (s_async == NV_ASYNC_SYNC)
        return NULL;
    for (i = 0; i < NV_GL_MAX_JOBS; i++)
        if (s_job[i].state == JOB_FREE) {
            j = &s_job[i];
            break;
        }
    if (!j)
        return NULL;
    memset(j, 0, sizeof *j);
    j->src_hash = h;
    j->ahead = ahead;
    j->next = -1;
    if ((vs && !(j->vs = strdup(vs))) || !(j->fs = strdup(fs))) {
        free(j->vs);
        free(j->fs);
        memset(j, 0, sizeof *j);
        return NULL;
    }
    if (ahead)
        s_ahead_inflight++;
    if (s_async == NV_ASYNC_WORKER) {
        j->state = JOB_QUEUED;
        pthread_mutex_lock(&s_job_mu);
        if (ahead) {
            if (s_qahead_tail >= 0)
                s_job[s_qahead_tail].next = i;
            else
                s_qahead_head = i;
            s_qahead_tail = i;
        } else {
            if (s_qlive_tail >= 0)
                s_job[s_qlive_tail].next = i;
            else
                s_qlive_head = i;
            s_qlive_tail = i;
        }
        pthread_cond_signal(&s_job_cv);
        pthread_mutex_unlock(&s_job_mu);
    } else {
        /* KHR: issued here, the driver's threads do the work. */
        const char *src[2];
        src[0] = nv2a_gl_glsl_header(NV2A_GL_ES);
        j->sh_v = glCreateShader(GL_VERTEX_SHADER);
        src[1] = j->vs ? j->vs : nv2a_gl_vertex_body();
        glShaderSource(j->sh_v, 2, src, NULL);
        glCompileShader(j->sh_v);
        j->sh_f = glCreateShader(GL_FRAGMENT_SHADER);
        src[1] = j->fs;
        glShaderSource(j->sh_f, 2, src, NULL);
        glCompileShader(j->sh_f);
        j->prog = glCreateProgram();
        if (s_pc_on)
            glProgramParameteri(j->prog, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
        glAttachShader(j->prog, j->sh_v);
        glAttachShader(j->prog, j->sh_f);
        glLinkProgram(j->prog);
        j->state = JOB_COMPILING;
    }
    if (!ahead)
        host_shader_progress_live();    /* the badge: "COMPILING... (n)" */
    return j;
}

/* The job that will give program table entry p its program: one compiling
 * it already that nobody waits for (the list's warm-up), else a new one;
 * NULL: compile it at once. */
static GlJob *job_claim(GlProg *p, const char *vs, const char *fs)
{
    GlJob *j = job_find(p->src_hash);

    if (!j || j->owner)
        j = job_start(p->src_hash, vs, fs, 0);
    if (j) {
        j->owner = p;
        p->job = (int)(j - s_job) + 1;
        p->pending = 1;
    }
    return j;
}

/* prog_drop_all: the table's entries are gone, their jobs nobody's (the
 * warm table takes them when done). */
static void job_disown_all(void)
{
    int i;

    for (i = 0; i < NV_GL_MAX_JOBS; i++)
        s_job[i].owner = NULL;
}

/* Done (ready or failed)? KHR: polled, finished here when complete. */
static int job_is_done(GlJob *j)
{
    int st = __atomic_load_n(&j->state, __ATOMIC_ACQUIRE);

    if (st == JOB_COMPILING) {
        GLint done = 0, ok = 0;
        glGetProgramiv(j->prog, GL_COMPLETION_STATUS_KHR, &done);
        if (!done)
            return 0;
        glGetProgramiv(j->prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[2048];
            log[0] = 0;
            glGetShaderInfoLog(j->sh_f, sizeof log, NULL, log);
            if (!log[0])
                glGetShaderInfoLog(j->sh_v, sizeof log, NULL, log);
            if (!log[0])
                glGetProgramInfoLog(j->prog, sizeof log, NULL, log);
            fprintf(stderr, "  [GPUGL] shader compile or link failed: %s\n", log);
            glDeleteProgram(j->prog);
            j->prog = 0;
        } else if (s_pc_on) {
            pc_store(j->prog, j->src_hash);
        }
        glDeleteShader(j->sh_v);
        glDeleteShader(j->sh_f);
        j->sh_v = j->sh_f = 0;
        j->state = st = j->prog ? JOB_READY : JOB_FAILED;
    }
    return st == JOB_READY || st == JOB_FAILED;
}

/* A done job's program (0: it failed), its slot freed. */
static GLuint job_take(GlJob *j)
{
    GLuint prog = j->state == JOB_READY ? j->prog : 0;

    if (j->ahead) {
        s_ahead_inflight--;
        host_shader_progress_done(1);
    }
    s_st.compiles++;
    s_pc_total_compiled++;
    s_async_done++;
    free(j->vs);
    free(j->fs);
    memset(j, 0, sizeof *j);
    return prog;
}

/* Wait for j within the draw's and the frame's budget. */
static void job_wait(GlJob *j)
{
    uint32_t budget = s_wait_max_us < s_wait_left_us ? s_wait_max_us : s_wait_left_us;
    struct timespec t0, t1;
    uint32_t spent = 0;

    if (!budget || job_is_done(j))
        return;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (spent < budget && !job_is_done(j)) {
        struct timespec nap = { 0, 100000 };   /* 0.1 ms */
        nanosleep(&nap, NULL);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        spent = (uint32_t)((t1.tv_sec - t0.tv_sec) * 1000000 + (t1.tv_nsec - t0.tv_nsec) / 1000);
    }
    s_wait_left_us = spent < s_wait_left_us ? s_wait_left_us - spent : 0;
    s_waited_us += spent;
}

/* ---- the program list ---- */

static int u64_in(const uint64_t *a, int n, uint64_t v)
{
    int i;

    for (i = 0; i < n; i++)
        if (a[i] == v)
            return 1;
    return 0;
}

static void pl_path(char *out, size_t n)
{
    snprintf(out, n, "%s/programs.list", s_pc_dir);
}

static void pl_put(const void *p, size_t n)
{
    if (s_pl_buf_n + n > s_pl_buf_cap) {
        size_t cap = s_pl_buf_cap ? s_pl_buf_cap * 2 : 65536;
        char *b;
        while (cap < s_pl_buf_n + n)
            cap *= 2;
        if (!(b = (char *)realloc(s_pl_buf, cap)))
            return;
        s_pl_buf = b;
        s_pl_buf_cap = cap;
    }
    memcpy(s_pl_buf + s_pl_buf_n, p, n);
    s_pl_buf_n += n;
}

static void pl_record_body(char type, uint64_t hash, const char *body)
{
    uint32_t len = (uint32_t)strlen(body);

    pl_put(&type, 1);
    pl_put(&hash, 8);
    pl_put(&len, 4);
    pl_put(body, len);
}

/* A program linked for the first time: into the list (written at the next
 * flip) unless it is there already. */
static void pl_record(uint64_t src_hash, const GlVsh *vsh, const char *fs)
{
    if (!s_pc_on || !s_pl_known || s_npl_known >= NV_GL_PLIST_MAX
        || u64_in(s_pl_known, s_npl_known, src_hash))
        return;
    s_pl_known[s_npl_known++] = src_hash;
    if (vsh && !u64_in(s_pl_vknown, s_npl_vknown, vsh->src_hash) && s_npl_vknown < NV_GL_PLIST_MAX) {
        s_pl_vknown[s_npl_vknown++] = vsh->src_hash;
        pl_record_body('V', vsh->src_hash, vsh->src);
    }
    pl_record_body('P', vsh ? vsh->src_hash : 0, fs);
}

/* At the flip: the records of the programs linked since, appended. */
static void pl_flush(void)
{
    char path[1024];
    FILE *f;

    if (!s_pl_buf_n || !s_pc_on)
        return;
    pl_path(path, sizeof path);
    if ((f = fopen(path, "ab")) != NULL) {
        fseek(f, 0, SEEK_END);
        if (ftell(f) == 0) {
            uint32_t hd[3] = { NV_GL_PLIST_VERSION, NV2A_GL_ES, NV_GL_PCACHE_VERSION };
            fwrite("NV2AGLPL", 8, 1, f);
            fwrite(hd, sizeof hd, 1, f);
        }
        fwrite(s_pl_buf, s_pl_buf_n, 1, f);
        fclose(f);
    }
    s_pl_buf_n = 0;
}

static void pl_free_warm(void)
{
    int i;

    for (i = 0; i < s_npl_todo; i++)
        free(s_pl_todo[i].fs);
    free(s_pl_todo);
    s_pl_todo = NULL;
    s_npl_todo = s_pl_next = 0;
    for (i = 0; i < s_nplv; i++)
        free(s_plv[i].src);
    free(s_plv);
    s_plv = NULL;
    s_nplv = 0;
}

static void pl_free(void)
{
    pl_free_warm();
    free(s_pl_known);
    free(s_pl_vknown);
    s_pl_known = s_pl_vknown = NULL;
    s_npl_known = s_npl_vknown = 0;
    free(s_pl_buf);
    s_pl_buf = NULL;
    s_pl_buf_n = s_pl_buf_cap = 0;
}

/* One body of a record; NULL at the end of the file or on a bad length. */
static char *pl_read_body(FILE *f)
{
    uint32_t len;
    char *b;

    if (fread(&len, 4, 1, f) != 1 || len > NV_GL_PLIST_BODY || !(b = (char *)malloc(len + 1u)))
        return NULL;
    if (len && fread(b, len, 1, f) != 1) {
        free(b);
        return NULL;
    }
    b[len] = 0;
    return b;
}

/* At pc_init: the list read, the programs it has that have no binary
 * queued to be compiled ahead (with async compiles), then those with one
 * (compiled only if their binary does not load). */
static void pl_load(void)
{
    char path[1024], magic[8];
    uint32_t hd[3];
    FILE *f;
    int i, nbin = 0;
    long good;

    pl_free();
    s_pl_known = (uint64_t *)malloc(NV_GL_PLIST_MAX * sizeof *s_pl_known);
    s_pl_vknown = (uint64_t *)malloc(NV_GL_PLIST_MAX * sizeof *s_pl_vknown);
    if (!s_pl_known || !s_pl_vknown) {
        pl_free();
        return;
    }
    pl_path(path, sizeof path);
    if (!(f = fopen(path, "rb")))
        return;
    if (fread(magic, 8, 1, f) != 1 || memcmp(magic, "NV2AGLPL", 8) != 0
        || fread(hd, sizeof hd, 1, f) != 1 || hd[0] != NV_GL_PLIST_VERSION || hd[1] != NV2A_GL_ES
        || hd[2] != NV_GL_PCACHE_VERSION) {
        fclose(f);
        remove(path);               /* another version, GL flavour or generator: written again */
        return;
    }
    good = ftell(f);
    s_pl_todo = (GlPlTodo *)calloc(NV_GL_PLIST_MAX, sizeof *s_pl_todo);
    s_plv = calloc(NV_GL_PLIST_MAX, sizeof *s_plv);
    while (s_pl_todo && s_plv && s_npl_known < NV_GL_PLIST_MAX) {
        char type;
        uint64_t hash;
        char *body;
        if (fread(&type, 1, 1, f) != 1 || fread(&hash, 8, 1, f) != 1 || !(body = pl_read_body(f)))
            break;
        if (type == 'V' || type == 'P')
            good = ftell(f);        /* the end of the last whole record */
        if (type == 'V' && s_npl_vknown < NV_GL_PLIST_MAX) {
            s_pl_vknown[s_npl_vknown++] = hash;
            s_plv[s_nplv].hash = hash;
            s_plv[s_nplv++].src = body;
        } else if (type == 'P') {
            int vsi = -1;
            uint64_t h;
            if (hash) {
                for (i = 0; i < s_nplv; i++)
                    if (s_plv[i].hash == hash)
                        vsi = i;
                if (vsi < 0) {      /* its vertex shader is not in the file */
                    free(body);
                    continue;
                }
            }
            h = prog_src_hash(vsi >= 0 ? s_plv[vsi].src : NULL, body);
            if (u64_in(s_pl_known, s_npl_known, h)) {
                free(body);
                continue;
            }
            s_pl_known[s_npl_known++] = h;
            s_pl_todo[s_npl_todo].src_hash = h;
            s_pl_todo[s_npl_todo].vsi = vsi;
            s_pl_todo[s_npl_todo].fs = body;
            s_pl_todo[s_npl_todo].bin = u64_in(s_pc_list, s_pc_nlist, h);
            nbin += s_pl_todo[s_npl_todo].bin;
            s_npl_todo++;
        } else {
            free(body);
            break;
        }
    }
    fclose(f);
    {
        /* A run killed while appending leaves a broken record: cut it off,
         * or every record appended after it would never be read. */
        struct stat st;
        if (s_pl_todo && s_plv && stat(path, &st) == 0 && st.st_size > good
            && s_npl_known < NV_GL_PLIST_MAX && truncate(path, good) != 0)
            remove(path);
    }
    if (s_async == NV_ASYNC_SYNC || !s_npl_todo) {
        pl_free_warm();             /* nothing compiled ahead: only the known hashes kept */
    } else {
        /* The ones without a binary first (stable: list order kept). */
        GlPlTodo *t = (GlPlTodo *)malloc((size_t)s_npl_todo * sizeof *t);
        int n = 0;
        if (t) {
            for (i = 0; i < s_npl_todo; i++)
                if (!s_pl_todo[i].bin)
                    t[n++] = s_pl_todo[i];
            for (i = 0; i < s_npl_todo; i++)
                if (s_pl_todo[i].bin)
                    t[n++] = s_pl_todo[i];
            memcpy(s_pl_todo, t, (size_t)n * sizeof *t);
            free(t);
        }
        host_shader_progress_expect((uint32_t)(s_npl_todo - nbin));
    }
    fprintf(stderr, "  [GPUGL] program list %s: %d programs, %d without a binary%s\n", path, s_npl_known,
            s_npl_known - nbin, s_pl_todo ? ", compiled ahead" : "");
}

static void prog_install(GlProg *p, GlJob *j);

/* At the flip (pc_warm): finished jobs into the program table entry
 * waiting for them (a pipeline drawn only while it compiled is never asked
 * for again: its job would stay, its slot and the badge's count with it),
 * the others into the warm table; then the list's next programs queued, a
 * few in flight at a time. Those with a binary wait for the binary warm-up,
 * and are compiled only if it did not link them. */
static void pl_warm(void)
{
    int i, cap = s_async == NV_ASYNC_WORKER ? NV_GL_AHEAD_WORKER : NV_GL_AHEAD_KHR;

    for (i = 0; i < NV_GL_MAX_JOBS; i++) {
        GlJob *j = &s_job[i];
        uint64_t h;
        GLuint prog;
        if (j->state == JOB_FREE || !job_is_done(j))
            continue;
        if (j->owner) {
            prog_install(j->owner, j);
            continue;
        }
        h = j->src_hash;
        prog = job_take(j);
        if (!prog)
            continue;
        if (s_nwarm < NV_GL_MAX_WARM && !pc_have(h)) {
            s_warm[s_nwarm].hash = h;
            s_warm[s_nwarm].prog = prog;
            s_nwarm++;
        } else {
            glDeleteProgram(prog);
        }
    }
    while (s_pl_todo && s_pl_next < s_npl_todo && s_ahead_inflight < cap) {
        GlPlTodo *t = &s_pl_todo[s_pl_next];
        if (t->bin && s_pc_list)
            break;                  /* the binary warm-up is not through yet */
        if (pc_have(t->src_hash) || job_find(t->src_hash)) {
            if (!t->bin)
                host_shader_progress_done(1);
            s_pl_next++;
            continue;
        }
        if (s_nwarm + s_ahead_inflight >= NV_GL_MAX_WARM
            || !job_start(t->src_hash, t->vsi >= 0 ? s_plv[t->vsi].src : NULL, t->fs, 1)) {
            if (s_nwarm + s_ahead_inflight >= NV_GL_MAX_WARM || s_async == NV_ASYNC_SYNC) {
                /* No room to keep more: the rest compile when a draw needs them. */
                for (; s_pl_next < s_npl_todo; s_pl_next++)
                    if (!s_pl_todo[s_pl_next].bin)
                        host_shader_progress_done(1);
            }
            break;
        }
        if (t->bin)
            host_shader_progress_expect(1);   /* its binary did not load: counted again */
        s_pl_next++;
    }
    if (s_pl_todo && s_pl_next >= s_npl_todo && !s_ahead_inflight) {
        if (s_verbose)
            fprintf(stderr, "  [GPUGL] program list: warm-up done, %d programs ready\n", s_nwarm);
        pl_free_warm();
    }
    if (s_frame % 600 == 0 && (s_skips || s_async_done)) {
        fprintf(stderr, "  [GPUGL] shaders: %u compiled off the draw path, %u batches skipped while"
                        " compiling, %.1f ms waited (last 600 frames)\n",
                s_async_done, s_skips, (double)s_waited_us / 1000.0);
        s_skips = s_waited_us = s_async_done = 0;
    }
}

/* The draw met program p still compiling: wait within the budget; ready,
 * set up and returned; else the stand-in (the batch drawn as nothing). */
static GlProg *prog_pending(GlProg *p)
{
    GlJob *j = p->job > 0 && p->job <= NV_GL_MAX_JOBS ? &s_job[p->job - 1] : NULL;

    if (!j || j->owner != p || j->state == JOB_FREE) {   /* cannot happen: a pending entry owns its job */
        p->pending = 0;
        p->failed = 1;
        return NULL;
    }
    job_wait(j);
    if (!job_is_done(j)) {
        s_skips++;
        return &s_standin;
    }
    prog_install(p, j);
    return p->failed ? NULL : p;
}

/* Done job j's program into its entry p, set up (failed: p refused). */
static void prog_install(GlProg *p, GlJob *j)
{
    p->prog = job_take(j);
    p->pending = 0;
    p->job = 0;
    if (!p->prog)
        p->failed = 1;
    else
        prog_setup(p, p->vsh_hash != 0);
}

/* The program for pipeline `key` with the fixed vertex shader (vsh NULL:
 * CPU-transformed vertices) or a translated vertex program. */
static GlProg *prog_get(const Nv2aGlShaderKey *key, GlVsh *vsh)
{
    static char body[32768];
    uint64_t vh = vsh ? vsh->hash : 0;
    uint64_t h = nv2a_gl_shader_key_hash(key) ^ (vh * 0x9E3779B97F4A7C15ull);
    uint32_t i, slot = (uint32_t)h & (NV_GL_MAX_PROG - 1);
    GlProg *p = NULL;

    for (i = 0; i < NV_GL_MAX_PROG; i++) {
        GlProg *c = &s_prog[(slot + i) & (NV_GL_MAX_PROG - 1)];
        if (!c->prog && !c->failed && !c->pending) {
            p = c;
            break;
        }
        if (c->hash == h && c->vsh_hash == vh && memcmp(&c->key, key, sizeof *key) == 0)
            return c->failed ? NULL : c->pending ? prog_pending(c) : c;
    }
    if (!p || s_nprog >= NV_GL_MAX_PROG * 3 / 4) {
        prog_drop_all();
        return prog_get(key, vsh);
    }
    s_nprog++;
    p->key = *key;
    p->hash = h;
    p->vsh_hash = vh;
    if (!nv2a_gl_fragment_body(key, body, sizeof body)) {
        p->failed = 1;
        return NULL;
    }
    /* The whole program's source, as the cache keys it. */
    p->src_hash = prog_src_hash(vsh ? vsh->src : NULL, body);
    if ((p->prog = pc_take(p->src_hash)) != 0) {
        s_st.cache_loads++;
        s_pc_total_loaded++;
    } else if (job_claim(p, vsh ? vsh->src : NULL, body)) {
        /* Compiling off the draw path (XBA-310): a short wait, else skipped. */
        pl_record(p->src_hash, vsh, body);
        return prog_pending(p);
    } else if (prog_link(p, body, vsh)) {
        if (s_pc_on)
            pc_store(p->prog, p->src_hash);
        pl_record(p->src_hash, vsh, body);
    } else {
        p->failed = 1;
        return NULL;
    }
    prog_setup(p, vsh != NULL);
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

/* po_scale: the target's internal scale (1 for a depth-only target), which
 * the polygon offset factor follows: it multiplies the depth slope per
 * pixel, and a pixel of a target drawn at scale S spans 1/S of the guest's.
 * Internal resolution and what is not scaled yet: anything the executor
 * starts sending in surface pixels must go through real = round(logical *
 * t.sc) as the viewport and scissor do (set_view) -- points and lines
 * (glPointSize / glLineWidth), SET_WINDOW_CLIP_*, SET_CLEAR_RECT. */
static void apply_state(const Nv2aRenderState *rs, uint32_t shade, uint32_t bpp,
                        int zeta16, float po_scale)
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
        GLenum src = blend_factor(rs->blend_src, 1), dst = blend_factor(rs->blend_dst, 0);
        sh_cap(SH_BLEND, GL_BLEND, 1);
        if (s_sh.blend_src != src || s_sh.blend_dst != dst) {
            glBlendFunc(src, dst);
            s_sh.blend_src = src;
            s_sh.blend_dst = dst;
        }
        if (s_sh.blend_eq != eq) {
            glBlendEquation(eq);
            s_sh.blend_eq = eq;
        }
        unpack4(rs->blend_color, bc);
        if (memcmp(bc, s_sh.blend_color, sizeof bc) != 0) {
            glBlendColor(bc[0], bc[1], bc[2], bc[3]);
            memcpy(s_sh.blend_color, bc, sizeof bc);
        }
    } else {
        sh_cap(SH_BLEND, GL_BLEND, 0);
    }
    if (shade == NV2A_SHADE_REPLACE
        || !(rs->depth_test_enable || rs->depth_write)) {
        sh_cap(SH_DEPTH, GL_DEPTH_TEST, 0);
    } else {
        /* GL writes depth only with the test on: ALWAYS stands in for "off". */
        GLenum f = rs->depth_test_enable ? compare_func(rs->depth_func) : GL_ALWAYS;
        sh_cap(SH_DEPTH, GL_DEPTH_TEST, 1);
        if (s_sh.depth_func != f) {
            glDepthFunc(f);
            s_sh.depth_func = f;
        }
        sh_depth_mask(rs->depth_write != 0);
    }
    if (rs->stencil_enable && shade != NV2A_SHADE_REPLACE) {
        GLenum f = compare_func(rs->stencil_func);
        GLenum o1 = stencil_op(rs->stencil_fail), o2 = stencil_op(rs->stencil_zfail);
        GLenum o3 = stencil_op(rs->stencil_zpass);
        GLint ref = (GLint)(rs->stencil_ref & 0xFF);
        GLuint fm = rs->stencil_func_mask & 0xFF;
        sh_cap(SH_STENCIL, GL_STENCIL_TEST, 1);
        if (s_sh.st_func != f || s_sh.st_ref != ref || s_sh.st_mask != fm) {
            glStencilFunc(f, ref, fm);
            s_sh.st_func = f;
            s_sh.st_ref = ref;
            s_sh.st_mask = fm;
        }
        if (s_sh.st_fail != o1 || s_sh.st_zfail != o2 || s_sh.st_zpass != o3) {
            glStencilOp(o1, o2, o3);
            s_sh.st_fail = o1;
            s_sh.st_zfail = o2;
            s_sh.st_zpass = o3;
        }
        sh_stencil_mask(rs->stencil_write_mask & 0xFF);
    } else {
        sh_cap(SH_STENCIL, GL_STENCIL_TEST, 0);
    }
    /* Culling as the title set it. Rows map one to one (guest row y = GL
     * row y), which mirrors the picture vertically in GL's window space, so
     * a triangle the title sees clockwise winds counter-clockwise here: the
     * front face is the opposite of SET_FRONT_FACE. Strips arrive with one
     * winding (NV2A_BACKEND_CAP_CULL). */
    if (s_cull && rs->cull_enable
        && (rs->cull_face == GL_FRONT || rs->cull_face == GL_BACK
            || rs->cull_face == GL_FRONT_AND_BACK)) {
        GLenum ff = rs->front_face == GL_CCW ? GL_CW : GL_CCW;
        sh_cap(SH_CULL, GL_CULL_FACE, 1);
        if (s_sh.cull_face != (GLenum)rs->cull_face) {
            glCullFace((GLenum)rs->cull_face);
            s_sh.cull_face = (GLenum)rs->cull_face;
        }
        if (s_sh.front_face != ff) {
            glFrontFace(ff);
            s_sh.front_face = ff;
        }
    } else {
        sh_cap(SH_CULL, GL_CULL_FACE, 0);
    }
    /* Polygon offset (shadow-map passes push their depth back). The units
     * count the zeta format's steps; the depth buffer is D24S8 whatever the
     * format, so a Z16 surface's step is 256 of ours. */
    if (rs->poly_offset_fill && shade != NV2A_SHADE_REPLACE) {
        float units = rs->poly_offset_units * (zeta16 ? 256.0f : 1.0f);
        float factor = rs->poly_offset_factor * po_scale;
        sh_cap(SH_POLY, GL_POLYGON_OFFSET_FILL, 1);
        if (s_sh.po_factor != factor || s_sh.po_units != units) {
            glPolygonOffset(factor, units);
            s_sh.po_factor = factor;
            s_sh.po_units = units;
        }
    } else {
        sh_cap(SH_POLY, GL_POLYGON_OFFSET_FILL, 0);
    }
    /* A 16-bit surface has no alpha: keep the target's at 1, as the CPU
     * path reads it back. */
    sh_color_mask((cm & 0x00010000u) != 0, (cm & 0x00000100u) != 0,
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
    float sc;                       /* the target's scale: counts / sc^2 */
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
        sum += nv2a_gl_scale_count(r, s_qring[s_qhead].sc);
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
        sum += nv2a_gl_scale_count(r, s_qring[s_qhead].sc);
        query_pop();
    }
    *count = sum;
    return 1;
}

/* A report the title may be spinning on: hand the driver what is queued,
 * so its query result comes in (nv2a_backend.h zpass_flush). */
static void gl_zpass_flush(void)
{
    if (s_init > 0)
        glFlush();
}

/* ---- the back end ------------------------------------------------------- */

/* The clear covers the scissor (the clip rectangle, in real pixels at the
 * target's scale: bind_target). A SET_CLEAR_RECT on this path would need
 * the same rounding (nv2a_gl_scale_rect). */
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
            int32_t c[4];
            nv2a_gl_scale_rect(s->clip_x, s->clip_y, s->width, s->height, t.sc, c);
            nv2a_gl_invalidate_rect(n, att, c[0], c[1], c[2], c[3]);
            s_st.invalidates++;
        }
    }
    if ((flags & 0xF0) && t.rt) {
        float c[4];
        unpack4(argb, c);
        if (t.bpp == 2)
            c[3] = 1.0f;
        sh_color_mask((flags & 0x10) != 0, (flags & 0x20) != 0, (flags & 0x40) != 0,
                    t.bpp == 2 || (flags & 0x80) != 0);
        glClearColor(c[0], c[1], c[2], c[3]);
        mask |= GL_COLOR_BUFFER_BIT;
    }
    if (flags & 0x01) {
        /* zclear_value(): Z16 in the low 16 bits, Z24 in the top 24. */
        float z = ((s->format >> 4) & 0xF) == 1 ? (float)(zstencil & 0xFFFF) / 65535.0f
                                                : (float)(zstencil >> 8) / 16777215.0f;
        sh_depth_mask(1);
#if NV2A_GL_ES
        glClearDepthf(z);
#else
        glClearDepth(z);
#endif
        mask |= GL_DEPTH_BUFFER_BIT;
    }
    if (flags & 0x02) {
        sh_stencil_mask(0xFF);
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
/* Texel coordinates (linear formats, and every format on the screen-space
 * path) rather than normalised ones. */
static int tex_coords_in_texels(const Nv2aPixelState *ps, const Nv2aTexture *t)
{
    return ps->shade == NV2A_SHADE_REPLACE
        || !(d3d8_format_is_swizzled(t->color) || d3d8_format_dxt_block_bytes(t->color));
}

/* Sample colour target r on `unit` as texture t (rt_for_texture). The GL
 * texture can be larger than the guest texture (a linear target is as wide
 * as its pitch): coordinates scale to the target's size. That size is the
 * logical one at any internal scale (coordinates are normalised); a scaled
 * target sampled by a batch drawing at guest size (s_draw_sc 1: an effect
 * target) reads its native resolve, as many texels as the guest has. */
static void rt_bind_as_texture(GlRT *r, const Nv2aPixelState *ps, const Nv2aTexture *t,
                               int unit, GLint alpha, float *tscale)
{
    GLuint tex = r->tex;
    GlSamp *sp = &r->samp;

    rt_seed(r);
    if (r->sc != 1.0f && s_draw_sc == 1.0f && s_scale_resolve) {
        rt_resolve(r);
        tex = r->rtex;
        sp = &r->rsamp;
    }
    sh_active((int)unit);
    sh_tex(GL_TEXTURE_2D, tex);
    samp_set(GL_TEXTURE_2D, sp, tex_filter(t, ps->shade),
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

    /* At an internal scale one zeta address can have a depth texture per
     * scale (depth_get): the one bound last. */
    for (i = 0; i < NV_GL_MAX_DEPTH; i++)
        if (s_depth[i].tex && s_depth[i].va == t->offset
            && (!d || s_depth[i].used > d->used))
            d = &s_depth[i];
    if (!d || t->width > d->w || t->height > d->h)
        return 0;
    sh_active((int)unit);
    sh_tex(GL_TEXTURE_2D, d->tex);
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

    memset(s_lodb, 0, sizeof s_lodb);
    for (st = 0; st < 4; st++) {
        const Nv2aTexStage *ts = &ps->stage[st];
        uint32_t mode = ts->mode, cube = 0;
        GlTex *e;
        GlRT *r;
        GLint alpha;
        kind[st] = NV2A_GL_TEX_NONE;
        tscale[st * 2] = tscale[st * 2 + 1] = 1.0f;
        s_stage_rt[st] = NULL;
        /* Combiners: 2D, 3D, cube, and from stage 1 on bump-env (6, 7)
         * and the dot-product and dependent-read modes that sample
         * (XBA-311; DOTPRODUCT and DOT_ZW do not, BRDF is not modelled). */
        if (ps->shade == NV2A_SHADE_COMBINERS
            ? !((mode >= 1 && mode <= 3)
                || (st && nv2a_stage_mode_reads_stage(mode) && mode != 0x0A && mode != 0x11))
            : (st || !mode))
            continue;
        if (!ts->valid)
            continue;
        /* Cube lookups: CUBEMAP, the reflections, DOT_STR_CUBE. */
        if (ps->shade == NV2A_SHADE_COMBINERS && ts->tex.cube
            && (mode == 3 || mode == 0x0B || mode == 0x0C || mode == 0x0E || mode == 0x12))
            cube = 1;
        if (ps->shade == NV2A_SHADE_COMBINERS && (mode == 1 || mode == 2)
            && shadow_kind(ts->tex.color)) {
            /* A shadow map. Not drawn by the GPU: white, as before (the
             * decoder knows no depth format). */
            if (bind_shadow(ps, &ts->tex, (int)st, &tscale[st * 2]))
                kind[st] = shadow_kind(ts->tex.color);
            continue;
        }
        if (ts->tex.depth > 1) {
            /* A volume (XBA-120): PROJECT2D/3D on the combiners, stage 0
             * otherwise; never a render target. */
            if (ps->shade == NV2A_SHADE_COMBINERS && mode != 1 && mode != 2 && mode != 0x0D)
                continue;
            if (!(e = tex_get(&ts->tex, 0, (int)st)))
                continue;
            tex_bind(e, &ts->tex, (int)st, ps->shade);
            kind[st] = NV2A_GL_TEX_3D;
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
        sh_active((int)st);
        if (!r->copy || r->copy_w != r->gw || r->copy_h != r->gh) {
            if (!r->copy)
                glGenTextures(1, &r->copy);
            sh_tex(GL_TEXTURE_2D, r->copy);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)rt_real_px(r, r->gw),
                         (GLsizei)rt_real_px(r, r->gh), 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
            r->copy_w = r->gw;
            r->copy_h = r->gh;
            r->copy_samp.wrap_s = r->copy_samp.wrap_t = -1;
            r->copy_samp.filter = r->copy_samp.swz_a = -1;
        } else {
            sh_tex(GL_TEXTURE_2D, r->copy);
        }
        /* The target's framebuffer is bound for drawing (and reading). */
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, (GLsizei)rt_real_px(r, r->gw),
                            (GLsizei)rt_real_px(r, r->gh));
        samp_set(GL_TEXTURE_2D, &r->copy_samp, r->samp.filter, r->samp.wrap_s,
                 r->samp.wrap_t, r->samp.border, r->samp.swz_a);
        s_st.rt_copies++;
    }
}

/* ---- uniform shadow (XBA-312) ----------------------------------------
 * A batch used to send 10-13 uniforms whatever they held (~12k glUniform
 * calls a frame in Fable); each program now keeps the values it was last
 * sent and only a changed one goes out, compared byte for byte (memcmp:
 * -0 and NaN payloads count as changes, so the program ends up holding
 * exactly what it did before). RECOMP_GPU_UNIFORM_SHADOW=0 sends every
 * value again, for comparison. */
enum {
    NV_GL_U_VP, NV_GL_U_XF, NV_GL_U_FOG, NV_GL_U_TSCALE, NV_GL_U_LODB, NV_GL_U_C0, NV_GL_U_C1,
    NV_GL_U_FC0, NV_GL_U_FC1, NV_GL_U_FOGC, NV_GL_U_BM, NV_GL_U_BSO, NV_GL_U_AREF, NV_GL_U_PSIZE,
    NV_GL_U_EYE,
    NV_GL_NU
};
/* Where each one's floats are in GlProg.us (sizes 4 4 4 8 4 32 32 4 4 4 16 8 1 1 3). */
static const uint8_t s_us_off[NV_GL_NU + 1] = {
    0, 4, 8, 12, 20, 24, 56, 88, 92, 96, 100, 116, 124, 125, 126, 129
};
static int s_ushadow = -1;          /* RECOMP_GPU_UNIFORM_SHADOW (default on), -1 not read */

/* Program p is about to get uniforms; its shadow starts over if it is no
 * longer the GL program it was made for. */
static void us_check(GlProg *p)
{
    if (s_ushadow < 0) {
        const char *e = getenv("RECOMP_GPU_UNIFORM_SHADOW");
        s_ushadow = !(e && strcmp(e, "0") == 0);
    }
    if (p->us_prog != p->prog) {
        p->us_prog = p->prog;
        p->us_ok = 0;
        memset(p->cvalid, 0, sizeof p->cvalid);
    }
}

/* 1 when program p already holds v for uniform u (nothing to send);
 * otherwise records v as sent, counts the call, 0: the caller sends it. */
static int us_same(GlProg *p, int u, const float *v)
{
    size_t n = (size_t)(s_us_off[u + 1] - s_us_off[u]) * sizeof(float);
    float *sh = p->us + s_us_off[u];

    us_check(p);
    if (s_ushadow && (p->us_ok >> u & 1u) && memcmp(sh, v, n) == 0) {
        s_st.uni_skipped++;
        return 1;
    }
    memcpy(sh, v, n);
    p->us_ok |= 1u << u;
    s_st.uni_calls++;
    return 0;
}

/* The fragment shader's uniforms (combiner constants, fog colour, alpha
 * reference, texture scales), those that changed. */
static void set_pixel_uniforms(GlProg *p, const Nv2aPixelState *ps,
                               const Nv2aRenderState *rs, const float tscale[8])
{
    float c0[32], c1[32], v[4];

    if (!us_same(p, NV_GL_U_TSCALE, tscale))
        glUniform2fv(p->u_tscale, 4, tscale);
    if (p->u_lodb >= 0 && !us_same(p, NV_GL_U_LODB, s_lodb))
        glUniform4fv(p->u_lodb, 1, s_lodb);
    if (ps->shade == NV2A_SHADE_COMBINERS) {
        int i;
        for (i = 0; i < 8; i++) {
            unpack4(ps->rc.factor0[i], &c0[i * 4]);
            unpack4(ps->rc.factor1[i], &c1[i * 4]);
        }
        if (!us_same(p, NV_GL_U_C0, c0))
            glUniform4fv(p->u_c0, 8, c0);
        if (!us_same(p, NV_GL_U_C1, c1))
            glUniform4fv(p->u_c1, 8, c1);
        unpack4(ps->rc.final_c0, v);
        if (!us_same(p, NV_GL_U_FC0, v))
            glUniform4fv(p->u_fc0, 1, v);
        unpack4(ps->rc.final_c1, v);
        if (!us_same(p, NV_GL_U_FC1, v))
            glUniform4fv(p->u_fc1, 1, v);
        unpack4(ps->fog_color, v);
        if (!us_same(p, NV_GL_U_FOGC, v))
            glUniform4fv(p->u_fogc, 1, v);
        if (p->u_bm >= 0 || p->u_bso >= 0) {
            float bm[16], bso[8];
            for (i = 0; i < 4; i++) {
                memcpy(&bm[i * 4], ps->stage[i].bump_mat, sizeof ps->stage[i].bump_mat);
                bso[i * 2] = ps->stage[i].bump_scale;
                bso[i * 2 + 1] = ps->stage[i].bump_offset;
            }
            if (p->u_bm >= 0 && !us_same(p, NV_GL_U_BM, bm))
                glUniform4fv(p->u_bm, 4, bm);
            if (p->u_bso >= 0 && !us_same(p, NV_GL_U_BSO, bso))
                glUniform2fv(p->u_bso, 4, bso);
        }
        if (p->u_eye >= 0 && !us_same(p, NV_GL_U_EYE, ps->eye))
            glUniform3fv(p->u_eye, 1, ps->eye);
    }
    v[0] = (float)(rs->alpha_ref & 0xFF);
    if (!us_same(p, NV_GL_U_AREF, v))
        glUniform1f(p->u_aref, v[0]);
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

/* The GL primitive for a batch (Nv2aBatch.prim), with the point size and
 * line width it draws at: surface pixels times the internal scale, clamped
 * to what the context offers (desktop lines stay 1, see ensure_init). */
static GLenum prim_setup(const Nv2aBatch *b, GlProg *p, float sc)
{
    float one = 1.0f;

    if (b->prim == NV2A_PRIM_POINTS) {
        float size = b->point_size * sc;
        if (size < 1.0f) size = 1.0f;
        if (size > s_point_max) size = s_point_max;
        if (p->u_psize >= 0 && !us_same(p, NV_GL_U_PSIZE, &size))
            glUniform1f(p->u_psize, size);
        s_st.points++;
        return GL_POINTS;
    }
    if (p->u_psize >= 0 && !us_same(p, NV_GL_U_PSIZE, &one))
        glUniform1f(p->u_psize, one);
    if (b->prim == NV2A_PRIM_LINES) {
        float w = sc < 1.0f ? 1.0f : (sc > s_line_max ? s_line_max : sc);
        if (w != s_line_width) {
            glLineWidth(w);
            s_line_width = w;
        }
        s_st.lines++;
        return GL_LINES;
    }
    return GL_TRIANGLES;
}

/* ---- streamed vertex and index uploads ---------------------------------
 *
 * What is drawn from bytes that change at every batch -- gl_draw's
 * CPU-transformed vertices, gl_draw_program's gathered attributes and its
 * index list -- goes into a ring in a buffer that keeps its storage
 * (src/video/gpu_ring.h, shared with the GameCube's renderer): each upload
 * is mapped write-only and unsynchronised right after the previous one, and
 * the GPU is only waited for when the ring comes back round to bytes a draw
 * may still read (fences, every 1/16 of the ring). A glBufferData of a new
 * size at every draw made Adreno drivers allocate GPU memory per draw (an
 * ioctl each: 58 % of the GameCube game thread, 867b8603), and a map with
 * GL_MAP_INVALIDATE_RANGE_BIT still allocated and copied (2f5dc00e).
 *
 * RECOMP_GPU_STREAM=sub writes the same ranges with glBufferSubData, =off
 * goes back to a glBufferData per draw (for comparison on other drivers);
 * RECOMP_GPU_STREAM_RING_KB sizes all rings (tests: small rings wrap all
 * the time). A map that fails falls back to glBufferSubData on the range. */
enum { NV_GL_STRM_FFP, NV_GL_STRM_PROG, NV_GL_STRM_IDX, NV_GL_NSTRM };
enum { NV_GL_STREAM_RING, NV_GL_STREAM_SUB, NV_GL_STREAM_OFF };
#define NV_GL_STRM_MAX (32u << 20)  /* a ring grows up to this, then a batch goes alone */

typedef struct {
    size_t size;                    /* allocated: 0 not yet (or after a reset) */
    gpu_ring r;
} GlStream;
static GlStream s_strm[NV_GL_NSTRM];
static int s_strm_mode = -1;        /* NV_GL_STREAM_*, -1 not read yet */
static size_t s_strm_size[NV_GL_NSTRM];

static void *strm_fence(void *user)
{
    GLsync f = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    (void)user;
    if (!f)
        glFinish();                 /* nothing to wait for later: complete now */
    return f;
}

/* Returns once the GPU has passed the fence. A wait that fails, or lasts
 * absurdly long (a lost context need not ever signal), ends in glFinish. */
static void strm_retire(void *user, void *sync)
{
    GLsync f = (GLsync)sync;
    GLenum r;
    (void)user;
    if (!f)
        return;
    r = glClientWaitSync(f, 0, 0);
    if (r == GL_TIMEOUT_EXPIRED) {
        struct timespec ts;
        uint64_t t0, dt;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        t0 = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
        do {
            r = glClientWaitSync(f, GL_SYNC_FLUSH_COMMANDS_BIT, 100000000u);
            clock_gettime(CLOCK_MONOTONIC, &ts);
            dt = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec - t0;
        } while (r == GL_TIMEOUT_EXPIRED && dt < 10000000000ull);
        s_st.strm_waits++;
        s_st.strm_wait_ns += dt;
        if (dt > s_st.strm_wait_max_ns)
            s_st.strm_wait_max_ns = dt;
    }
    if (r != GL_ALREADY_SIGNALED && r != GL_CONDITION_SATISFIED)
        glFinish();
    glDeleteSync(f);
}

static void strm_discard(void *user, void *sync)
{
    (void)user;
    if (sync)
        glDeleteSync((GLsync)sync);
}

static const gpu_ring_ops s_strm_ops = {strm_fence, strm_retire, strm_discard};

static void strm_config(void)
{
    static const size_t def[NV_GL_NSTRM] = {8u << 20, 8u << 20, 2u << 20};
    const char *m = getenv("RECOMP_GPU_STREAM");
    const char *kb = getenv("RECOMP_GPU_STREAM_RING_KB");
    int i, k = kb && *kb ? atoi(kb) : 0;

    s_strm_mode = m && strcmp(m, "sub") == 0 ? NV_GL_STREAM_SUB
                : m && strcmp(m, "off") == 0 ? NV_GL_STREAM_OFF
                                             : NV_GL_STREAM_RING;
    for (i = 0; i < NV_GL_NSTRM; i++)
        s_strm_size[i] = k > 0 ? (size_t)(k < 4 ? 4 : k) << 10 : def[i];
    fprintf(stderr, "  [GPUGL] vertex streams: %s (rings %zu/%zu/%zu KB)\n",
            s_strm_mode == NV_GL_STREAM_RING ? "ring, unsynchronised maps"
            : s_strm_mode == NV_GL_STREAM_SUB ? "ring, glBufferSubData"
                                              : "off, glBufferData per draw",
            s_strm_size[0] >> 10, s_strm_size[1] >> 10, s_strm_size[2] >> 10);
}

/* Forget the rings (the buffers are the caller's: s_vbo, s_pvbo, s_pebo).
 * lost: the context is gone, so its fences are only forgotten. */
static void strm_reset(int lost)
{
    int i;
    for (i = 0; i < NV_GL_NSTRM; i++) {
        if (!lost)
            gpu_ring_release(&s_strm[i].r, &s_strm_ops, NULL);
        memset(&s_strm[i], 0, sizeof s_strm[i]);
    }
}

/* Upload n bytes for the next draw to stream k, whose buffer the caller has
 * bound to `target` (the element target: with its vertex array bound).
 * Returns the byte offset to draw from, a multiple of align. At most once
 * per draw in each stream, and strm_after_draw once the draw is issued. */
static size_t strm_put(int k, GLenum target, const void *data, size_t n, size_t align)
{
    GlStream *st = &s_strm[k];
    uint64_t wraps;
    size_t off;
    void *p = NULL;

    if (s_strm_mode < 0)
        strm_config();
    if (s_strm_mode == NV_GL_STREAM_OFF || n == 0) {
        glBufferData(target, (GLsizeiptr)n, data, GL_STREAM_DRAW);
        gpu_ring_release(&st->r, &s_strm_ops, NULL);
        st->size = 0;               /* the storage is that batch's now */
        return 0;
    }
    if (st->size && !gpu_ring_fits(&st->r, n)) {
        /* Too large for the ring: a larger one; the old storage stays the
         * GPU's until its draws are done (orphaned, no wait). */
        gpu_ring_release(&st->r, &s_strm_ops, NULL);
        st->size = 0;
        s_st.strm_grows++;
    }
    if (!st->size) {
        size_t size = s_strm_size[k];
        while (size < 4 * n && size < NV_GL_STRM_MAX)
            size *= 2;
        if (size < 4 * n) {
            /* Larger than any ring: on its own, as before; a ring starts
             * again at the next upload. */
            glBufferData(target, (GLsizeiptr)n, data, GL_STREAM_DRAW);
            s_st.strm_oversize++;
            return 0;
        }
        glBufferData(target, (GLsizeiptr)size, NULL, GL_STREAM_DRAW);
        gpu_ring_init(&st->r, size);
        st->size = size;
    }
    wraps = st->r.wraps;
    off = (size_t)gpu_ring_reserve(&st->r, &s_strm_ops, NULL, n, align);
    if (st->r.wraps != wraps)
        s_st.strm_wraps++;
    if (s_strm_mode == NV_GL_STREAM_RING)
        p = glMapBufferRange(target, (GLintptr)off, (GLsizeiptr)n,
                             GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
    if (p) {
        memcpy(p, data, n);
        if (glUnmapBuffer(target))
            return off;
        s_st.strm_mapfail++;        /* the contents are undefined: again */
    } else if (s_strm_mode == NV_GL_STREAM_RING) {
        s_st.strm_mapfail++;
    }
    glBufferSubData(target, (GLintptr)off, (GLsizeiptr)n, data);
    return off;
}

/* The draw reading stream k's latest upload is issued. */
static void strm_after_draw(int k)
{
    if (s_strm[k].size)
        gpu_ring_after_draw(&s_strm[k].r, &s_strm_ops, NULL);
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
    size_t first;

    if (!b->full || !ps
        || b->count < (b->prim == NV2A_PRIM_POINTS ? 1u : b->prim == NV2A_PRIM_LINES ? 2u : 3u))
        return;
    if (!ensure_init())
        return;
    s_draw_sc = surface_scale(s);
    bind_stage_textures(ps, kind, tscale);
    if (s_sig_on && sig_batch(s, b, kind, b->count / 3))
        return;
    nv2a_gl_shader_key(ps, rs, kind, &key);
    key.point_sprite = b->prim == NV2A_PRIM_POINTS && b->point_sprite;
    if (key.bump_src[1] | key.bump_src[2] | key.bump_src[3])
        s_st.bump_draws++;
    p = prog_get(&key, NULL);
    if (!p)
        return;
    if (!bind_target(s, rs->zeta_va, &t))
        return;
    rt_seed(t.rt);
    break_feedback(t.rt);
    apply_state(rs, ps->shade, t.bpp, ((s->format >> 4) & 0xF) == 1, t.rt ? t.sc : 1.0f);
    set_near_mode(0, 0.0f);
    sh_program(p->prog);
    v[0] = 2.0f / (float)t.w;
    v[1] = 2.0f / (float)t.h;
    v[2] = 2.0f / zmax_of(s->format);
    v[3] = s_depth_clamp ? 0.0f : 1.0f;    /* vertex-shader clamp instead */
    if (!us_same(p, NV_GL_U_VP, v))
        glUniform4fv(p->u_vp, 1, v);
    set_pixel_uniforms(p, ps, rs, tscale);

    sh_vao(s_vao);
    sh_array_buffer(s_vbo);
    first = strm_put(NV_GL_STRM_FFP, GL_ARRAY_BUFFER, b->full,
                     (size_t)b->count * sizeof(Nv2aVertexFull), sizeof(Nv2aVertexFull))
          / sizeof(Nv2aVertexFull);
    s_st.ffp_kb += (uint32_t)(((size_t)b->count * sizeof(Nv2aVertexFull) + 1023) / 1024);
    if (rs->zpass_enable && s_qcount < NV_GL_MAX_QUERY) {
        GlQuery *q = &s_qring[(s_qhead + s_qcount) & (NV_GL_MAX_QUERY - 1)];
        q->q = query_new();
        q->ticket = 0;
        q->sc = t.sc;
        glBeginQuery(NV2A_GL_ES ? GL_ANY_SAMPLES_PASSED : GL_SAMPLES_PASSED, q->q);
        query = 1;
    }
    glDrawArrays(prim_setup(b, p, t.rt ? t.sc : 1.0f), (GLint)first, (GLsizei)b->count);
    strm_after_draw(NV_GL_STRM_FFP);
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
    if (b->prim == NV2A_PRIM_TRIANGLES)
        s_st.tris += b->count / 3;
}

/* Upload what changed in c[] since this program last drew (nv2a_backend.h:
 * stamps), as runs of consecutive uniform locations. With the uniform
 * shadow, a run is trimmed at both ends to the constants whose values the
 * program does not hold already (a title that writes the same constants
 * again at every draw sends nothing), one call for what is left. */
static int c_held(const GlProg *p, uint32_t i, const float *v)
{
    return (p->cvalid[i >> 5] >> (i & 31) & 1u) && memcmp(p->cshadow[i], v, 16) == 0;
}

static void sync_constants(GlProg *p, const Nv2aProgramBatch *pb)
{
    uint32_t i, k, lo = 192, hi = 0;
    int sh;

    if (p->const_synced >= pb->const_serial)
        return;
    us_check(p);
    if (!p->cshadow)
        p->cshadow = (float (*)[4])malloc(192 * sizeof *p->cshadow);
    sh = p->cshadow != NULL;            /* recorded even with the shadow off */
    for (i = 0; i < 192; i++)
        if (pb->const_stamp[i] > p->const_synced) {
            if (lo == 192)
                lo = i;
            hi = i;
        }
    for (i = lo; i <= hi && i < 192;) {
        uint32_t j = i + 1, a = i, b;
        if (p->cloc[i] < 0) {
            i++;
            continue;
        }
        while (j <= hi && p->cloc[j] == p->cloc[j - 1] + 1)
            j++;
        b = j;
        if (sh && s_ushadow) {
            while (a < b && c_held(p, a, pb->constants[a]))
                a++;
            while (b > a && c_held(p, b - 1, pb->constants[b - 1]))
                b--;
        }
        if (a < b) {
            glUniform4fv(p->cloc[a], (GLsizei)(b - a), pb->constants[a]);
            s_st.uni_calls++;
            if (sh) {
                memcpy(p->cshadow[a], pb->constants[a], (size_t)(b - a) * sizeof *p->cshadow);
                for (k = a; k < b; k++)
                    p->cvalid[k >> 5] |= 1u << (k & 31);
            }
        } else {
            s_st.uni_skipped++;
        }
        i = j;
    }
    p->const_synced = pb->const_serial;
}

static GLuint s_pvao, s_pvbo, s_pebo;

/* ---- guest vertex buffers (RECOMP_GPU_VBO) -------------------------------
 *
 * A batch that arrives raw (Nv2aProgramBatch.raw) is drawn from GL buffers
 * holding the title's own vertex bytes, in their own formats (the shader
 * converts them, nv2a_gl_vsh_attr), instead of from floats the executor
 * gathered vertex by vertex. Fable's 1000 indexed batches a frame read the
 * same 1050 vertex ranges every frame, unchanged.
 *
 *   buffer   one per (first byte, length, stride): the bytes of the batch's
 *            index range [raw_first, raw_first + vertex_count) of the
 *            streams that interleave in it (same stride, starting less than
 *            a stride apart); the indices count from that range's start.
 *   validity the bytes are hashed again the first time a buffer is used in
 *            a frame, and re-uploaded when they changed. A buffer created or
 *            changed this frame is hashed at every use: a ring of dynamic
 *            vertices rewritten within the frame is caught. A static buffer
 *            rewritten mid-frame is caught at the next frame.
 *   bounds   NV_GL_MAX_VB buffers or NV_GL_VB_BYTES bytes, then every
 *            buffer (and every vertex array, which name them) is dropped.
 *   arrays   one vertex array object per layout (buffers, offsets, formats,
 *            strides), with the shared element buffer s_pebo. */
#define NV_GL_MAX_VB   4096         /* power of two */
#define NV_GL_VB_BYTES (64u << 20)
#define NV_GL_MAX_VAO  1024         /* power of two */

typedef struct {
    uint32_t va, len, stride;       /* len 0: empty slot */
    GLuint buf;
    uint64_t hash;
    uint32_t checked;               /* s_frame + 1 of the last hash, 0 never */
    uint32_t uploaded;              /* s_frame + 1 of the last upload */
} GlVb;

typedef struct {
    uint32_t streams;
    GLuint buf[16];
    uint32_t off[16];
    uint8_t kind[16], comps[16], stride[16];
} GlVaoKey;

typedef struct {
    GlVaoKey key;
    uint64_t hash;
    GLuint vao;                     /* 0: empty slot */
    GLuint ebo;                     /* element buffer bound in it */
} GlVao;

/* Index lists of raw batches, by content: one GL element buffer per
 * distinct triangle list (Fable sends the same ~1000 lists every frame).
 * Dropped whole when full, with the vertex arrays (they bind them). */
#define NV_GL_MAX_IB   8192         /* power of two */
#define NV_GL_IB_BYTES (32u << 20)
typedef struct {
    uint64_t hash;
    uint32_t len;                   /* 0: empty slot */
    GLuint buf;
} GlIb;
static GlIb *s_ib;
static uint32_t s_nib;
static size_t s_ib_bytes;

static GlVb s_vb[NV_GL_MAX_VB];
static uint32_t s_nvb;
static size_t s_vb_bytes;
static uint32_t s_vb_gen;           /* vb_drop_all calls */
static GlVao *s_vao_cache;          /* NV_GL_MAX_VAO, allocated at first use */
static uint32_t s_nvao;
static int s_vbo_on;                /* RECOMP_GPU_VBO, default on */

/* Vertex ranges and index lists: hash4_bytes, counted. */
static uint64_t vb_hash(const uint8_t *p, size_t n)
{
    s_st.vbo_hash_kb += (uint32_t)(n >> 10);
    s_st.hash_vtx_b += n;
    return hash4_bytes(p, n);
}

static void vao_drop_all(void)
{
    uint32_t i;

    if (!s_vao_cache)
        return;
    for (i = 0; i < NV_GL_MAX_VAO; i++)
        if (s_vao_cache[i].vao) {
            if (s_sh.vao == s_vao_cache[i].vao)
                sh_vao(0);
            glDeleteVertexArrays(1, &s_vao_cache[i].vao);
        }
    memset(s_vao_cache, 0, NV_GL_MAX_VAO * sizeof *s_vao_cache);
    s_nvao = 0;
}

static void ib_drop_all(void)
{
    uint32_t i;

    if (!s_ib)
        return;
    vao_drop_all();
    for (i = 0; i < NV_GL_MAX_IB; i++)
        if (s_ib[i].buf)
            glDeleteBuffers(1, &s_ib[i].buf);
    memset(s_ib, 0, NV_GL_MAX_IB * sizeof *s_ib);
    s_nib = 0;
    s_ib_bytes = 0;
}

/* The element buffer holding `n` indices idx (uploaded the first time this
 * list is seen). Binds nothing; 0 without memory. */
static GLuint ib_get(const uint16_t *idx, uint32_t n)
{
    uint32_t len = n * 2u, i, slot;
    uint64_t h = vb_hash((const uint8_t *)idx, len);
    GlIb *e = NULL;

    if (!s_ib && !(s_ib = (GlIb *)calloc(NV_GL_MAX_IB, sizeof *s_ib)))
        return 0;
    slot = (uint32_t)(h ^ (h >> 32)) & (NV_GL_MAX_IB - 1);
    for (i = 0; i < NV_GL_MAX_IB; i++) {
        GlIb *c = &s_ib[(slot + i) & (NV_GL_MAX_IB - 1)];
        if (!c->len) {
            e = c;
            break;
        }
        if (c->hash == h && c->len == len)
            return c->buf;
    }
    if (!e || s_nib >= NV_GL_MAX_IB * 3 / 4 || s_ib_bytes + len > NV_GL_IB_BYTES) {
        if (s_verbose)
            fprintf(stderr, "  [GPUGL] index buffer cache full (%u lists, %zu KB):"
                            " dropping it at frame %u\n", s_nib, s_ib_bytes >> 10, s_frame);
        ib_drop_all();
        return ib_get(idx, n);
    }
    e->hash = h;
    e->len = len;
    glGenBuffers(1, &e->buf);
    /* Through the copy target: the element binding is vertex-array state. */
    glBindBuffer(GL_COPY_WRITE_BUFFER, e->buf);
    glBufferData(GL_COPY_WRITE_BUFFER, (GLsizeiptr)len, idx, GL_STATIC_DRAW);
    s_nib++;
    s_ib_bytes += len;
    s_st.ib_new++;
    s_st.ib_kb += len >> 10;
    return e->buf;
}

static void vb_drop_all(void)
{
    uint32_t i;

    vao_drop_all();                 /* they name the buffers */
    for (i = 0; i < NV_GL_MAX_VB; i++)
        if (s_vb[i].buf) {
            if (s_sh.array_buf == s_vb[i].buf)
                sh_array_buffer(0);
            glDeleteBuffers(1, &s_vb[i].buf);
        }
    memset(s_vb, 0, sizeof s_vb);
    s_vb_gen++;
    s_nvb = 0;
    s_vb_bytes = 0;
}

/* The GL buffer holding guest bytes [va, va + len), up to date. */
static GLuint vb_get(uint32_t va, uint32_t len, uint32_t stride)
{
    uint32_t h = (va * 2654435761u) ^ (len * 0x85EBCA6Bu) ^ (stride * 0xC2B2AE35u);
    uint32_t i, slot = (h ^ (h >> 15)) & (NV_GL_MAX_VB - 1);
    const uint8_t *src = guest(va);
    GlVb *e = NULL;

    for (i = 0; i < NV_GL_MAX_VB; i++) {
        GlVb *c = &s_vb[(slot + i) & (NV_GL_MAX_VB - 1)];
        if (!c->len) {
            e = c;
            break;
        }
        if (c->va == va && c->len == len && c->stride == stride) {
            /* Stable (hashed equal in a frame after its upload) and already
             * checked this frame: trust it. */
            if (c->checked != s_frame + 1 || c->uploaded == s_frame + 1) {
                uint64_t hh = vb_hash(src, len);
                if (hh != c->hash) {
                    sh_array_buffer(c->buf);
                    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)len, src, GL_DYNAMIC_DRAW);
                    c->hash = hh;
                    c->uploaded = s_frame + 1;
                    s_st.vbo_changed++;
                    s_st.vbo_kb += len >> 10;
                }
                c->checked = s_frame + 1;
            }
            return c->buf;
        }
    }
    if (!e || s_nvb >= NV_GL_MAX_VB * 3 / 4 || s_vb_bytes + len > NV_GL_VB_BYTES) {
        if (s_verbose)
            fprintf(stderr, "  [GPUGL] vertex buffer cache full (%u buffers, %zu KB):"
                            " dropping it at frame %u\n", s_nvb, s_vb_bytes >> 10, s_frame);
        vb_drop_all();
        ib_drop_all();
        return vb_get(va, len, stride);
    }
    e->va = va;
    e->len = len;
    e->stride = stride;
    e->hash = vb_hash(src, len);
    e->checked = e->uploaded = s_frame + 1;
    glGenBuffers(1, &e->buf);
    sh_array_buffer(e->buf);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)len, src, GL_STATIC_DRAW);
    s_nvb++;
    s_vb_bytes += len;
    s_st.vbo_new++;
    s_st.vbo_kb += len >> 10;
    return e->buf;
}

static GlVao *vao_get(const GlVaoKey *k)
{
    uint64_t h = hash_str(0xCBF29CE484222325ull, "");
    const uint8_t *kb = (const uint8_t *)k;
    uint32_t i, a, slot;
    GlVao *e = NULL;
    static const GLenum gl_type[4] = { GL_FLOAT, GL_UNSIGNED_BYTE, GL_SHORT, GL_INT };

    for (i = 0; i < sizeof *k; i++)
        h = (h ^ kb[i]) * 0x100000001B3ull;
    if (!s_vao_cache
        && !(s_vao_cache = (GlVao *)calloc(NV_GL_MAX_VAO, sizeof *s_vao_cache)))
        return NULL;
    slot = (uint32_t)h & (NV_GL_MAX_VAO - 1);
    for (i = 0; i < NV_GL_MAX_VAO; i++) {
        GlVao *c = &s_vao_cache[(slot + i) & (NV_GL_MAX_VAO - 1)];
        if (!c->vao) {
            e = c;
            break;
        }
        if (c->hash == h && memcmp(&c->key, k, sizeof *k) == 0)
            return c;
    }
    if (!e || s_nvao >= NV_GL_MAX_VAO * 3 / 4) {
        vao_drop_all();
        return vao_get(k);
    }
    e->key = *k;
    e->hash = h;
    glGenVertexArrays(1, &e->vao);
    sh_vao(e->vao);
    for (a = 0; a < 16; a++) {
        if (!(k->streams & (1u << a)))
            continue;
        sh_array_buffer(k->buf[a]);
        glEnableVertexAttribArray(a);
        if (k->kind[a] == NV2A_GL_ATTR_INT)
            glVertexAttribIPointer(a, k->comps[a], GL_INT, k->stride[a],
                                   (const void *)(size_t)k->off[a]);
        else
            glVertexAttribPointer(a, k->comps[a], gl_type[k->kind[a]],
                                  k->kind[a] == NV2A_GL_ATTR_UBYTE, k->stride[a],
                                  (const void *)(size_t)k->off[a]);
    }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_pebo);
    e->ebo = s_pebo;
    s_nvao++;
    s_st.vao_new++;
    return e;
}

/* Bind the vertex array for raw batch pb (its buffers up to date) with
 * its index list's element buffer. The streams group into buffers by
 * interleaving. 0: cannot (no memory). */
static int raw_bind(const Nv2aProgramBatch *pb)
{
    GlVaoKey k;
    struct { uint32_t lo, hi, base, stride; } g[16];
    uint32_t gof[16], start[16], a, j, ng = 0, try;
    Nv2aGlVshAttr at[16];
    GlVao *vao;
    GLuint ebo;

    memset(&k, 0, sizeof k);
    k.streams = pb->streams;
    for (a = 0; a < 16; a++) {
        const Nv2aRawAttr *r = &pb->raw[a];
        uint32_t end;
        if (!(pb->streams & (1u << a)))
            continue;
        if (!nv2a_gl_vsh_attr(r->type, r->size, &at[a]))
            return 0;
        start[a] = r->offset + pb->raw_first * r->stride;
        end = start[a] + (pb->vertex_count - 1) * r->stride + at[a].bytes;
        for (j = 0; j < ng; j++)
            if (g[j].stride == r->stride
                && (start[a] >= g[j].base ? start[a] - g[j].base : g[j].base - start[a])
                       < r->stride)
                break;
        if (j == ng) {
            g[ng].lo = start[a];
            g[ng].hi = end;
            g[ng].base = start[a];
            g[ng].stride = r->stride;
            ng++;
        } else {
            if (start[a] < g[j].lo) g[j].lo = start[a];
            if (end > g[j].hi) g[j].hi = end;
        }
        gof[a] = j;
    }
    /* vb_get may drop the cache (and earlier groups' buffers) when it
     * fills: then once more, into the emptied cache. */
    for (try = 0; try < 2; try++) {
        uint32_t gen = s_vb_gen;
        for (j = 0; j < ng; j++) {
            GLuint b = vb_get(g[j].lo, g[j].hi - g[j].lo, g[j].stride);
            for (a = 0; a < 16; a++)
                if ((pb->streams & (1u << a)) && gof[a] == j)
                    k.buf[a] = b;
        }
        if (gen == s_vb_gen)
            break;
    }
    for (a = 0; a < 16; a++) {
        if (!(pb->streams & (1u << a)))
            continue;
        k.off[a] = start[a] - g[gof[a]].lo;
        k.kind[a] = (uint8_t)at[a].kind;
        k.comps[a] = (uint8_t)at[a].comps;
        k.stride[a] = (uint8_t)pb->raw[a].stride;
    }
    /* The index list first: dropping a full index cache drops the vertex
     * arrays too. */
    if (!(ebo = ib_get(pb->indices, pb->index_count)) || !(vao = vao_get(&k)))
        return 0;
    sh_vao(vao->vao);
    if (vao->ebo != ebo) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        vao->ebo = ebo;
    }
    return 1;
}

/* The input format word of raw batch pb (nv2a_gl_vsh_body_fmt); 0 for a
 * gathered one. *ok 0: a stream in a format nv2a_gl_vsh_attr cannot read. */
static uint64_t raw_formats(const Nv2aProgramBatch *pb, int *ok)
{
    uint64_t f = 0;
    uint32_t a;
    Nv2aGlVshAttr at;

    *ok = 1;
    if (!pb->raw)
        return 0;
    for (a = 0; a < 16; a++)
        if (pb->streams & (1u << a)) {
            if (!nv2a_gl_vsh_attr(pb->raw[a].type, pb->raw[a].size, &at)) {
                *ok = 0;
                return 0;
            }
            f |= (uint64_t)at.fmt << (4 * a);
        }
    return f;
}

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
    int query = 0, ok;
    uint64_t fmt;
    size_t voff = 0, ioff = 0;      /* this batch's bytes in the stream rings */

    if (!pb || !ps || !ensure_init())
        return 0;
    fmt = raw_formats(pb, &ok);
    if (!ok || (pb->raw && !s_vbo_on))
        return 0;                   /* the executor sends it gathered */
    if (!(vsh = vsh_get(pb, fmt)))
        return 0;
    s_draw_sc = surface_scale(s);
    bind_stage_textures(ps, kind, tscale);
    if (s_sig_on && sig_batch(s, b, kind, pb->index_count / 3))
        return 1;                   /* skipped: drawn by nobody */
    nv2a_gl_shader_key(ps, rs, kind, &key);
    key.point_sprite = b->prim == NV2A_PRIM_POINTS && b->point_sprite;
    if (key.bump_src[1] | key.bump_src[2] | key.bump_src[3])
        s_st.bump_draws++;
    p = prog_get(&key, vsh);
    if (!p || !p->cloc)
        return 0;
    if (!bind_target(s, rs->zeta_va, &t))
        return 1;                   /* gl_draw drops these too */
    rt_seed(t.rt);
    break_feedback(t.rt);
    apply_state(rs, ps->shade, t.bpp, ((s->format >> 4) & 0xF) == 1, t.rt ? t.sc : 1.0f);
    zmax = zmax_of(s->format);
    zmin = rs->depth_min;
    if (!(zmin >= 0.0f && zmin < zmax))
        zmin = 0.0f;
    set_near_mode(1, zmin / zmax);
    sh_program(p->prog);
    v[0] = 2.0f / (float)t.w;
    v[1] = 2.0f / (float)t.h;
    v[2] = 2.0f / (zmax - zmin);
    v[3] = 0.0f;                    /* GL clips near and far itself (set_near_mode) */
    if (!us_same(p, NV_GL_U_VP, v))
        glUniform4fv(p->u_vp, 1, v);
    v[0] = pb->aa_sx;
    v[1] = pb->aa_sy;
    v[2] = zmin;
    if (!us_same(p, NV_GL_U_XF, v))
        glUniform4fv(p->u_xf, 1, v);
    nv2a_gl_vsh_fog_uniform(pb->fog_enable, pb->fog_mode, pb->fog_param[0],
                            pb->fog_param[1], v);
    if (!us_same(p, NV_GL_U_FOG, v))
        glUniform4fv(p->u_fog, 1, v);
    set_pixel_uniforms(p, ps, rs, tscale);
    sync_constants(p, pb);

    if (!s_pvao) {
        glGenVertexArrays(1, &s_pvao);
        glGenBuffers(1, &s_pebo);
        glGenBuffers(1, &s_pvbo);
    }
    for (a = 0; a < 16; a++)
        nstream += (pb->streams >> a) & 1;
    if (pb->raw) {
        if (!raw_bind(pb))
            return 0;
        s_st.vbo_draws++;
        goto bound;
    }
    sh_vao(s_pvao);
    sh_array_buffer(s_pvbo);
    voff = strm_put(NV_GL_STRM_PROG, GL_ARRAY_BUFFER, pb->attrs,
                    (size_t)pb->vertex_count * nstream * 4 * sizeof(float), 16);
    /* The layout follows from the streams mask and where this batch's
     * vertices start in s_pvbo's ring (one interleaved buffer; GLES 3.0 has
     * no base vertex for the 16-bit indices): specified again when either
     * changes, the attributes enabled again only when the mask does. The
     * generic values of attributes without a stream are context state, set
     * when they change. The element buffer is s_pvao's for good. */
    if (s_sh.pv_streams != pb->streams || s_sh.pv_off != voff) {
        int mask_changed = s_sh.pv_streams != pb->streams;
        for (a = 0, k = 0; a < 16; a++) {
            if (pb->streams & (1u << a)) {
                if (mask_changed)
                    glEnableVertexAttribArray(a);
                glVertexAttribPointer(a, 4, GL_FLOAT, GL_FALSE,
                                      (GLsizei)(nstream * 4 * sizeof(float)),
                                      (const void *)(voff + (size_t)k * 4 * sizeof(float)));
                k++;
            } else if (mask_changed) {
                glDisableVertexAttribArray(a);
            }
        }
        s_sh.pv_streams = pb->streams;
        s_sh.pv_off = voff;
    }
    if (s_sh.pv_ebo != s_pebo) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_pebo);
        s_sh.pv_ebo = s_pebo;
    }
bound:
    for (a = 0; a < 16; a++)
        if (!(pb->streams & (1u << a))
            && memcmp(s_sh.pv_imm[a], pb->imm[a], sizeof s_sh.pv_imm[a]) != 0) {
            glVertexAttrib4fv(a, pb->imm[a]);
            memcpy(s_sh.pv_imm[a], pb->imm[a], sizeof s_sh.pv_imm[a]);
        }
    if (!pb->raw)
        ioff = strm_put(NV_GL_STRM_IDX, GL_ELEMENT_ARRAY_BUFFER, pb->indices,
                        (size_t)pb->index_count * sizeof(uint16_t), sizeof(uint16_t));
    if (rs->zpass_enable && s_qcount < NV_GL_MAX_QUERY) {
        GlQuery *q = &s_qring[(s_qhead + s_qcount) & (NV_GL_MAX_QUERY - 1)];
        q->q = query_new();
        q->ticket = 0;
        q->sc = t.sc;
        glBeginQuery(NV2A_GL_ES ? GL_ANY_SAMPLES_PASSED : GL_SAMPLES_PASSED, q->q);
        query = 1;
    }
    glDrawElements(prim_setup(b, p, t.rt ? t.sc : 1.0f), (GLsizei)pb->index_count,
                   GL_UNSIGNED_SHORT, (const void *)ioff);
    if (!pb->raw) {
        strm_after_draw(NV_GL_STRM_PROG);
        strm_after_draw(NV_GL_STRM_IDX);
    }
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
    if (!pb->raw)
        s_st.vsh_kb += (uint32_t)(((size_t)pb->vertex_count * nstream * 16
                                   + pb->index_count * 2) >> 10);
    if (b->prim == NV2A_PRIM_TRIANGLES)
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

    job_shutdown(lost);             /* the shader worker joined before its context goes */
    strm_reset(lost);               /* their fences, before the buffers go */
    if (!lost) {
        for (i = 0; i < NV_GL_MAX_RT; i++) {
            if (s_rt[i].fbo) glDeleteFramebuffers(1, &s_rt[i].fbo);
            if (s_rt[i].tex) glDeleteTextures(1, &s_rt[i].tex);
            if (s_rt[i].copy) glDeleteTextures(1, &s_rt[i].copy);
            if (s_rt[i].rfbo) glDeleteFramebuffers(1, &s_rt[i].rfbo);
            if (s_rt[i].rtex) glDeleteTextures(1, &s_rt[i].rtex);
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
        for (i = 0; i < s_nwarm; i++)
            glDeleteProgram(s_warm[i].prog);
        if (s_pbo) glDeleteBuffers(1, &s_pbo);
        for (i = 0; i < 2; i++)
            if (s_hpbo[i]) glDeleteBuffers(1, &s_hpbo[i]);
        if (s_aa_fbo) glDeleteFramebuffers(1, &s_aa_fbo);
        if (s_aa_tex) glDeleteTextures(1, &s_aa_tex);
        if (s_pvbo) glDeleteBuffers(1, &s_pvbo);
        if (s_pebo) glDeleteBuffers(1, &s_pebo);
        if (s_pvao) glDeleteVertexArrays(1, &s_pvao);
        vb_drop_all();
        ib_drop_all();              /* element buffers: up to NV_GL_IB_BYTES */
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
    for (i = 0; i < NV_GL_MAX_RT; i++)
        free(s_rt[i].snap);
    memset(s_rt, 0, sizeof s_rt);
    memset(s_depth, 0, sizeof s_depth);
    memset(s_tex, 0, sizeof s_tex);
    memset(s_bound, 0, sizeof s_bound);
    memset(s_stage_rt, 0, sizeof s_stage_rt);
    s_ntex = 0;
    for (i = 0; i < NV_GL_MAX_PROG; i++) {
        free(s_prog[i].cloc);
        free(s_prog[i].cshadow);
    }
    memset(s_prog, 0, sizeof s_prog);
    s_nprog = 0;
    for (i = 0; i < NV_GL_MAX_VSH; i++)
        free(s_vsh[i].src);
    memset(s_vsh, 0, sizeof s_vsh);
    s_nvsh = 0;
    s_nwarm = 0;
    free(s_pc_list);                /* ensure_init scans the cache again */
    pl_free();
    s_pc_list = NULL;
    s_pc_nlist = s_pc_next = 0;
    s_pc_on = 0;
    host_shader_progress_reset();   /* no warm-up in progress any more */
    s_pvao = s_pvbo = s_pebo = 0;
    /* The cache tables, emptied: with a live context their GL objects were
     * deleted above; with a lost one the names are only forgotten. */
    memset(s_vb, 0, sizeof s_vb);
    s_nvb = 0;
    s_vb_bytes = 0;
    s_vb_gen++;
    if (s_vao_cache)
        memset(s_vao_cache, 0, NV_GL_MAX_VAO * sizeof *s_vao_cache);
    s_nvao = 0;
    if (s_ib)
        memset(s_ib, 0, NV_GL_MAX_IB * sizeof *s_ib);
    s_nib = 0;
    s_ib_bytes = 0;
    s_pbo = 0;
    s_pbo_size = 0;
    memset(&s_pend, 0, sizeof s_pend);
    memset(s_hpbo, 0, sizeof s_hpbo);
    memset(s_hpbo_size, 0, sizeof s_hpbo_size);
    memset(&s_hpend, 0, sizeof s_hpend);
    s_hmapped = -1;
    s_aa_tex = s_aa_fbo = 0;
    s_aa_w = s_aa_h = 0;
    s_near_program = 0;             /* ensure_init: depth clamp on, range 0..1 */
    s_near01 = 0.0f;
    s_target_fbo = 0;
    s_target_scratch_depth = 0;
    sh_forget();
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
    if (__atomic_exchange_n(&s_zc_window_lost, 0, __ATOMIC_ACQ_REL)) {
        /* The window's context went (and with it, most likely, the GPU
         * state of the whole share group): start over, alone. */
        fprintf(stderr, "  [GPUGL] the window's GL context was lost\n");
        lost = 1;
    }
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
    if (lost) {
        if (s_zc_on) {
            /* Read back from now on: the slot textures are gone with the
             * context (the window keeps their names until new frames
             * come), and the next context is created alone. */
            nv2a_gl_context_unshare();
            s_zc_wanted = NULL;
            snprintf(s_zc_why, sizeof s_zc_why, "the GL context was lost");
        }
        s_zc_on = 0;
        memset(s_zc_tex, 0, sizeof s_zc_tex);
        memset(s_zc_fbo, 0, sizeof s_zc_fbo);
        memset(s_zc_w, 0, sizeof s_zc_w);
        memset(s_zc_h, 0, sizeof s_zc_h);
        s_zc_prev = NULL;
        nv2a_gl_context_destroy();
    }
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
        snap_refresh(s_pend.snap.va, rt_guest_len(&s_pend.snap));
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
    sh_fbo(rt_resolve(r));
    glReadPixels(0, 0, (GLsizei)r->w, (GLsizei)r->h, NV_GL_READ_FORMAT, NV_GL_READ_TYPE, NULL);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    s_pend.pending = 1;
    s_pend.dropped = 0;
    s_pend.snap = *r;
    s_pend.surf = *surf;
    r->dirty = 0;
    s_st.readbacks++;
}

/* The pixels a host present (and the scaled dump) reads for target r: the
 * framebuffer that holds them and their real size in GL pixels, origin
 * (0, 0), top row first (guest row y is GL row y), for the area of r's guest
 * surface (r->w x r->h): round(w * sc) x round(h * sc) of the scaled
 * texture at an internal resolution, the guest size at 1. */
static GLuint rt_present_source(const GlRT *r, uint32_t *w, uint32_t *h)
{
    *w = rt_real_px(r, r->w);
    *h = rt_real_px(r, r->h);
    return r->fbo;
}

static int host_present_on(void)
{
    return s_display_w && (s_present_host_env || s_scale > 1.0f);
}

static uint64_t host_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* The size of the picture target r presents for surf: its real size, halved
 * along each direction an anti-aliased surface doubles. */
static void present_size(const GlRT *r, const Nv2aPresentSurface *surf, uint32_t *w, uint32_t *h)
{
    uint32_t fx, fy;

    *w = rt_real_px(r, r->w);
    *h = rt_real_px(r, r->h);
    nv2a_gl_aa_factors(surf, &fx, &fy);
    *w /= fx;
    *h /= fy;
}

/* The framebuffer holding the picture target r presents for surf, at (0, 0),
 * present_size big, top row first: what a host frame reads and what a
 * zero-copy frame copies, so both show the same pixels. An anti-aliased
 * surface is resolved here (host_issue's comment). 0: nothing to show. */
static GLuint present_source(GlRT *r, const Nv2aPresentSurface *surf, uint32_t *pw, uint32_t *ph)
{
    uint32_t w, h, fx, fy;
    GLuint fbo = rt_present_source(r, &w, &h);

    *pw = *ph = 0;
    if (!fbo || !w || !h)
        return 0;
    nv2a_gl_aa_factors(surf, &fx, &fy);
    if (fx > 1 || fy > 1) {
        const GLint sr[4] = { 0, 0, (GLint)w, (GLint)h };
        GLint dr[4] = { 0, 0, 0, 0 };
        w /= fx;
        h /= fy;
        if (!s_aa_tex || s_aa_w != w || s_aa_h != h) {
            if (!s_aa_tex) {
                glGenTextures(1, &s_aa_tex);
                glGenFramebuffers(1, &s_aa_fbo);
            }
            sh_tex_scratch(GL_TEXTURE_2D, s_aa_tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, NULL);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
            sh_fbo(s_aa_fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                   s_aa_tex, 0);
            s_aa_w = w;
            s_aa_h = h;
        }
        dr[2] = (GLint)w;
        dr[3] = (GLint)h;
        sh_blit_rect(fbo, sr, s_aa_fbo, dr, GL_LINEAR);
        fbo = s_aa_fbo;
        s_hst.aa_frames++;
    }
    *pw = w;
    *ph = h;
    return fbo;
}

/* Start reading target r at its real size into the next host pack buffer. */
static void host_issue(GlRT *r, const Nv2aPresentSurface *surf)
{
    uint32_t w, h;
    GLuint fbo = present_source(r, surf, &w, &h);
    int b = s_hnext;
    size_t n;

    if (!fbo)
        return;
    n = (size_t)w * h * 4;
    if (b == s_hmapped)
        b ^= 1;                           /* never into the buffer still mapped */
    s_hnext = b ^ 1;
    if (!s_hpbo[b])
        glGenBuffers(1, &s_hpbo[b]);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, s_hpbo[b]);
    if (n > s_hpbo_size[b]) {
        glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)n, NULL, GL_STREAM_READ);
        s_hpbo_size[b] = n;
    }
    sh_fbo(fbo);
    glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, NV_GL_READ_FORMAT, NV_GL_READ_TYPE, NULL);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    s_hpend.pending = 1;
    s_hpend.buf = b;
    s_hpend.w = w;
    s_hpend.h = h;
    s_hpend.surf = *surf;
    s_rt_host_serial[r - s_rt] = r->serial;
}

/* Map the host read-back in flight and describe it in *ready (host_px);
 * it stays mapped until gl_flip. 0: none pending, or the map failed. */
static int host_map(Nv2aPresentSurface *ready)
{
    const void *p;
    int b = s_hpend.buf;

    if (!s_hpend.pending)
        return 0;
    s_hpend.pending = 0;
    if (!s_hpbo[b] || s_hmapped >= 0)
        return 0;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, s_hpbo[b]);
    p = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)((size_t)s_hpend.w * s_hpend.h * 4),
                         GL_MAP_READ_BIT);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    if (!p)
        return 0;
    s_hmapped = b;
    *ready = s_hpend.surf;
    ready->host_px = p;
    ready->host_w = s_hpend.w;
    ready->host_h = s_hpend.h;
    ready->host_pitch = s_hpend.w * 4;
    ready->host_format = NV_GL_READ_ARGB ? NV2A_HOST_FORMAT_BGRX8888 : NV2A_HOST_FORMAT_RGBX8888;
    s_hst.frames++;
    s_hst.w = s_hpend.w;
    s_hst.h = s_hpend.h;
    return 1;
}

/* At gl_flip: the window has copied the mapped host frame. */
static void host_unmap(void)
{
    if (s_hmapped < 0)
        return;
    if (s_hpbo[s_hmapped]) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, s_hpbo[s_hmapped]);
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }
    s_hmapped = -1;
}

/* The flip's present, host frames (host_present_on): target r goes to the
 * window at its real size. The guest bytes are refreshed only with
 * RECOMP_GPU_READBACK=present or =all; otherwise r stays dirty and is read
 * back when something needs its bytes. Returns gl_present's answer. */
static int host_present(GlRT *r, const Nv2aPresentSurface *now, Nv2aPresentSurface *ready)
{
    uint64_t t0 = host_now_ns();
    int fresh = r->serial != s_rt_host_serial[r - s_rt], got;

    if ((s_readback_present || s_readback_all) && r->dirty) {
        if (s_present_sync_mode)
            rt_readback(r);
        else
            pbo_issue(r, now);
    }
    if (s_present_sync_mode) {
        /* This flip's own frame (frame generation times frame N by flip N). */
        s_hpend.pending = 0;
        if (fresh)
            host_issue(r, now);
        got = host_map(ready);
    } else {
        got = host_map(ready);            /* the previous flip's frame */
        if (fresh)
            host_issue(r, now);
    }
    s_hst.ns += host_now_ns() - t0;
    if (got)
        return 1;
    *ready = *now;
    /* Not drawn since its last host read and the guest bytes not stale:
     * the guest CPU may have written the frame there (a movie over a
     * former target), so show guest memory, as without host present. */
    if (!fresh && !r->dirty)
        return 1;
    /* Nothing new on the host side (the pipeline starts, or the map
     * failed): keep showing what is shown. */
    return 0;
}

/* ---- zero-copy present (XBA-308) ----------------------------------------- */

/* The mode, logged when a context is created in a window (and again only
 * when it changed: after a lost context). */
static void zc_mode(void)
{
    static int said = -1;
    int on = s_zc_wanted && nv2a_gl_context_shared();

    s_zc_on = on;
    if (!s_display_w || said == on)
        return;
    said = on;
    if (on)
        fprintf(stderr, "  [GPUGL] present: zero-copy (the window draws the frame's texture in"
                        " a shared GL context; RECOMP_GPU_ZERO_COPY=0 reads frames back)\n");
    else
        fprintf(stderr, "  [GPUGL] present: read-back (%s)\n",
                s_zc_why[0] ? s_zc_why : "the window's GL context is not shared");
}

/* A fence of the other side, waited for (bounded: a fence of a context that
 * went away must not hang this one) and deleted. */
static void zc_fence_wait(void *fence)
{
    GLsync f = (GLsync)fence;
    GLenum r = glClientWaitSync(f, 0, 0);

    if (r == GL_TIMEOUT_EXPIRED)
        r = glClientWaitSync(f, 0, 100000000ull);   /* 100 ms */
    if (r != GL_ALREADY_SIGNALED && r != GL_CONDITION_SATISFIED) {
        static int said;
        s_zst.timeouts++;
        if (!said++)
            fprintf(stderr, "  [GPUGL] zero-copy: a fence not signalled in 100 ms"
                            " (0x%X); the slot is written anyway\n", r);
    }
    glDeleteSync(f);
}

static void zc_drop(void *fence)
{
    if (fence)
        glDeleteSync((GLsync)fence);
}

/* XboxFbGpuFrame.render: the flip's picture into slot's texture. */
static unsigned zc_render(const XboxFbGpuFrame *f, int slot, void *release, void **acquire)
{
    uint64_t t0 = host_now_ns();
    uint32_t w, h;
    GLuint src;

    *acquire = NULL;
    if (release)
        zc_fence_wait(release);
    if (!s_zcp.pending || !s_zcp.r || !s_zcp.r->tex || slot < 0 || slot >= XBOX_FB_GPU_SLOTS)
        return 0;
    src = present_source(s_zcp.r, &s_zcp.surf, &w, &h);
    if (!src || w != f->w || h != f->h)
        return 0;
    if (!s_zc_tex[slot]) {
        glGenTextures(1, &s_zc_tex[slot]);
        glGenFramebuffers(1, &s_zc_fbo[slot]);
    }
    if (s_zc_w[slot] != w || s_zc_h[slot] != h) {
        /* A new size (the first frame, an internal resolution or surface
         * change): the window last drew this texture before the release
         * fence above, so its storage can go. */
        sh_tex_scratch(GL_TEXTURE_2D, s_zc_tex[slot]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        sh_fbo(s_zc_fbo[slot]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               s_zc_tex[slot], 0);
        s_zc_w[slot] = w;
        s_zc_h[slot] = h;
    }
    {
        const GLint r4[4] = { 0, 0, (GLint)w, (GLint)h };
        sh_blit_rect(src, r4, s_zc_fbo[slot], r4, GL_NEAREST);
    }
    *acquire = (void *)glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();                            /* the window's context waits for it */
    /* Back-pressure, as the read-back paths had it (they mapped frame N-1's
     * pack buffer at flip N): the executor stays within a frame of the GPU,
     * and visibility reports resolved at the flip have their counts. */
    if (s_zc_prev)
        zc_fence_wait((void *)s_zc_prev);
    s_zc_prev = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    s_zst.frames++;
    s_zst.w = w;
    s_zst.h = h;
    s_zst.ns += host_now_ns() - t0;
    return s_zc_tex[slot];
}

/* XboxFbGpuFrame.pixels: the window needs the pixels after all (frame
 * generation, the video overlay); a synchronous read-back. */
static const void *zc_pixels(const XboxFbGpuFrame *f, uint32_t *pitch, uint32_t *format)
{
    uint32_t w, h;
    GLuint src;
    size_t n;

    if (!s_zcp.pending || !s_zcp.r || !s_zcp.r->tex)
        return NULL;
    src = present_source(s_zcp.r, &s_zcp.surf, &w, &h);
    if (!src || w != f->w || h != f->h)
        return NULL;
    n = (size_t)w * h * 4;
    if (n > s_zc_px_cap) {
        uint8_t *p = (uint8_t *)realloc(s_zc_px, n);
        if (!p)
            return NULL;
        s_zc_px = p;
        s_zc_px_cap = n;
    }
    sh_fbo(src);
    glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, NV_GL_READ_FORMAT, NV_GL_READ_TYPE, s_zc_px);
    *pitch = w * 4;
    *format = NV_GL_READ_ARGB ? NV2A_HOST_FORMAT_BGRX8888 : NV2A_HOST_FORMAT_RGBX8888;
    s_zst.readbacks++;
    return s_zc_px;
}

/* The flip's present as a zero-copy frame: target r's picture goes to the
 * window as a texture (the window asks for it in xbox_FramebufferWindowPresentHost,
 * still on this thread). Returns 0 when the window cannot take one now. */
static int zc_present(GlRT *r, const Nv2aPresentSurface *now, Nv2aPresentSurface *ready)
{
    uint32_t w, h;

    if (!s_zc_on || !s_zc_wanted || !s_zc_wanted())
        return 0;
    present_size(r, now, &w, &h);
    if (!w || !h)
        return 0;
    /* Guest memory as with host frames: refreshed only when asked. */
    if ((s_readback_present || s_readback_all) && r->dirty) {
        if (s_present_sync_mode)
            rt_readback(r);
        else
            pbo_issue(r, now);
    }
    s_hpend.pending = 0;                  /* a host read-back in flight is not shown */
    s_zcp.pending = 1;
    s_zcp.r = r;
    s_zcp.surf = *now;
    s_zc_frame.w = w;
    s_zc_frame.h = h;
    s_zc_frame.render = zc_render;
    s_zc_frame.pixels = zc_pixels;
    s_zc_frame.drop = zc_drop;
    *ready = *now;
    ready->host_px = &s_zc_frame;
    ready->host_w = w;
    ready->host_h = h;
    ready->host_pitch = 0;
    ready->host_format = NV2A_HOST_FORMAT_GPU;
    s_rt_host_serial[r - s_rt] = r->serial;
    return 1;
}

int nv2a_gl_present_share(int (*wanted)(void), const char *refuse)
{
    const char *e = getenv("RECOMP_GPU_ZERO_COPY");
    char why[sizeof s_zc_why];

    if (e && strcmp(e, "0") == 0) {
        snprintf(s_zc_why, sizeof s_zc_why, "RECOMP_GPU_ZERO_COPY=0");
        return 0;
    }
    if (refuse || !wanted) {
        snprintf(s_zc_why, sizeof s_zc_why, "%s", refuse ? refuse : "no window");
        return 0;
    }
    if (!nv2a_gl_context_share_window(why, sizeof why)) {
        snprintf(s_zc_why, sizeof s_zc_why, "%s", why);
        return 0;
    }
    s_zc_wanted = wanted;
    return 1;
}

void nv2a_gl_present_window_lost(void)
{
    __atomic_store_n(&s_zc_window_lost, 1, __ATOMIC_RELEASE);
}

/* RECOMP_GPU_SCALE_DUMP=1: at a dumping flip, target r at its real size
 * (rt_present_source) into <RECOMP_FB_DUMP>S<n>.bmp, 24-bit, next to the guest-size
 * dumps the executor writes. */
static void scaled_dump(const GlRT *r)
{
    static int seq;
    const char *prefix = getenv("RECOMP_FB_DUMP");
    GLuint fbo;
    uint32_t w, h, y, x, row, img, total;
    uint8_t hdr[54], *px, *line;
    char path[1024];
    FILE *f;

    if (!prefix || !*prefix)
        return;
    fbo = rt_present_source(r, &w, &h);
    row = (w * 3 + 3) & ~3u;
    img = row * h;
    total = 54 + img;
    px = (uint8_t *)malloc((size_t)w * h * 4);
    line = (uint8_t *)calloc(1, row);
    snprintf(path, sizeof path, "%sS%05d.bmp", prefix, seq++);
    f = px && line ? fopen(path, "wb") : NULL;
    if (!f) {
        free(px);
        free(line);
        return;
    }
    sh_fbo(fbo);
    glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B';
    hdr[1] = 'M';
    memcpy(hdr + 2, &total, 4);
    hdr[10] = 54;
    hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1;
    hdr[28] = 24;
    memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, sizeof hdr, f);
    for (y = 0; y < h; y++) {
        /* BMP rows go bottom up; GL row y is guest row y (top down). */
        const uint8_t *src = px + (size_t)(h - 1 - y) * w * 4;
        for (x = 0; x < w; x++) {
            line[x * 3] = src[x * 4 + 2];
            line[x * 3 + 1] = src[x * 4 + 1];
            line[x * 3 + 2] = src[x * 4];
        }
        fwrite(line, 1, row, f);
    }
    fclose(f);
    free(px);
    free(line);
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
    if (!r || sync)
        s_hpend.pending = 0;          /* a guest frame comes between: drop it */
    if (!r)
        return 1;                     /* not a GPU target: guest memory is current */
    /* A frame the guest CPU wrote into (a movie, a loading bar) reaches the
     * target before it is read back or shown from the GPU. */
    rt_cpu_check(r, 0);
    if (sync && s_scale_dump)
        scaled_dump(r);
    if (!sync && zc_present(r, now, ready))
        return 1;
    if (!sync && (host_present_on() || (s_display_w && nv2a_gl_present_needs_host(now))))
        return host_present(r, now, ready);
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
    if (s_init > 0)
        host_unmap();                 /* the window has its copy */
    s_zcp.pending = 0;                /* ... or its texture */
    s_zcp.r = NULL;
    sig_flip();
    s_frame++;
    s_st.frames++;
    if (s_init > 0)
        pc_warm();
    if (s_init > 0 && (s_verbose ? s_frame % 60 == 0 : s_frame % 600 == 0)) {
        int i, nrt = 0, nprog = 0;
        for (i = 0; i < NV_GL_MAX_RT; i++)
            nrt += s_rt[i].tex != 0;
        for (i = 0; i < NV_GL_MAX_PROG; i++)
            nprog += s_prog[i].prog != 0;
        fprintf(stderr, "  [GPUGL] frame %u: %u draws (%u vertex programs, %u KB;"
                        " %u KB transformed on the CPU), %u tris, %u clears, %u readbacks,"
                        " targets: %u created, %u relayouts, %u grown, %u sampled directly"
                        " (%u copies), %u shadow maps, %u bump-env, %u seeded, %u seeds skipped, %u invalidates,"
                        " %u uploads (%u KB), %u KB hashed, %d textures,"
                        " %d targets, %d programs (%u compiled, %u from cache;"
                        " run %u/%u),"
                        " %.1f target switches/frame, %.0f uniform calls/frame (%.0f left out),"
                        " %.0f KB hashed/frame (textures %.0f, vertices and indices %.0f,"
                        " other %.0f)\n",
                s_frame, s_st.draws, s_st.vsh_draws, s_st.vsh_kb, s_st.ffp_kb, s_st.tris,
                s_st.clears, s_st.readbacks, s_st.rt_creates, s_st.relayouts, s_st.rt_grows,
                s_st.rt_direct, s_st.rt_copies, s_st.shadow_binds, s_st.bump_draws, s_st.seeds,
                s_st.seeds_skipped, s_st.invalidates,
                s_st.uploads, s_st.upload_kb, s_st.hashes_kb, s_ntex, nrt, nprog,
                s_st.compiles, s_st.cache_loads, s_pc_total_compiled, s_pc_total_loaded,
                s_st.frames ? (double)s_st.target_switches / s_st.frames : 0.0,
                s_st.frames ? (double)s_st.uni_calls / s_st.frames : 0.0,
                s_st.frames ? (double)s_st.uni_skipped / s_st.frames : 0.0,
                s_st.frames ? (double)(s_st.hash_tex_b + s_st.hash_vtx_b + s_st.hash_other_b)
                                  / 1024.0 / s_st.frames : 0.0,
                s_st.frames ? (double)s_st.hash_tex_b / 1024.0 / s_st.frames : 0.0,
                s_st.frames ? (double)s_st.hash_vtx_b / 1024.0 / s_st.frames : 0.0,
                s_st.frames ? (double)s_st.hash_other_b / 1024.0 / s_st.frames : 0.0);
        if (s_vbo_on && s_st.frames)
            fprintf(stderr, "  [GPUGL] vertex buffers: %.0f raw batches/frame,"
                            " %.1f KB/frame uploaded (%.1f new, %.1f changed buffers/frame),"
                            " %.0f KB/frame hashed, %u buffers (%zu KB), %u vertex arrays"
                            " (%u new); index lists: %.1f new/frame (%.1f KB/frame),"
                            " %u cached (%zu KB)\n",
                    (double)s_st.vbo_draws / s_st.frames, (double)s_st.vbo_kb / s_st.frames,
                    (double)s_st.vbo_new / s_st.frames, (double)s_st.vbo_changed / s_st.frames,
                    (double)s_st.vbo_hash_kb / s_st.frames, s_nvb, s_vb_bytes >> 10,
                    s_nvao, s_st.vao_new, (double)s_st.ib_new / s_st.frames,
                    (double)s_st.ib_kb / s_st.frames, s_nib, s_ib_bytes >> 10);
        if (s_strm_mode >= 0 && s_strm_mode != NV_GL_STREAM_OFF && s_st.frames)
            fprintf(stderr, "  [GPUGL] vertex streams: rings %zu/%zu/%zu KB, %u wraps,"
                            " %u fence waits (%.1f ms/frame, longest %.1f ms), %u grown,"
                            " %u too large, %u map failures\n",
                    s_strm[0].size >> 10, s_strm[1].size >> 10, s_strm[2].size >> 10,
                    s_st.strm_wraps, s_st.strm_waits,
                    (double)s_st.strm_wait_ns / 1e6 / s_st.frames,
                    (double)s_st.strm_wait_max_ns / 1e6, s_st.strm_grows, s_st.strm_oversize,
                    s_st.strm_mapfail);
        if (s_st.cpu_merges)
            fprintf(stderr, "  [GPUGL] guest CPU writes into targets: %u merged (%u kpx)\n",
                    s_st.cpu_merges, s_st.cpu_kpx);
        if (s_st.points || s_st.lines)
            fprintf(stderr, "  [GPUGL] %u point batches, %u line batches (points up to %.0f px,"
                            " lines up to %.0f px)\n",
                    s_st.points, s_st.lines, (double)s_point_max, (double)s_line_max);
        if (s_hst.frames)
            fprintf(stderr, "  [GPUGL] host present: %u frames at %ux%u, %.3f ms/frame"
                            " (read-back issue + map), %u resolved from an anti-aliased"
                            " surface\n",
                    s_hst.frames, s_hst.w, s_hst.h,
                    (double)s_hst.ns / 1e6 / (double)s_hst.frames, s_hst.aa_frames);
        if (s_zst.frames || s_zst.readbacks)
            fprintf(stderr, "  [GPUGL] zero-copy present: %u frames at %ux%u, %.3f ms/frame"
                            " (blit + fence), %u read back for the window (frame generation,"
                            " video overlay), %u fence timeouts\n",
                    s_zst.frames, s_zst.w, s_zst.h,
                    s_zst.frames ? (double)s_zst.ns / 1e6 / (double)s_zst.frames : 0.0,
                    s_zst.readbacks, s_zst.timeouts);
        memset(&s_st, 0, sizeof s_st);
        memset(&s_hst, 0, sizeof s_hst);
        memset(&s_zst, 0, sizeof s_zst);
        if (s_scale_cfg.scale != 1.0f)
            fprintf(stderr, "  [GPUGL] internal scale %.2f: %u resolves, %u seed blits,"
                            " %u target rescales, %u depth re-creations\n",
                    (double)s_scale_cfg.scale, s_sst.resolves, s_sst.seed_blits, s_sst.rescales,
                    s_sst.depth_recreates);
        memset(&s_sst, 0, sizeof s_sst);
    }
    gl_check_frame();
}

/* Save states (XBA-248, nv2a_backend.h's state hook), on the executor
 * thread with the title frozen. FLUSH: what only the GPU holds goes to guest
 * memory, as before an out-of-memory reset -- the present read-back in
 * flight, then every dirty target -- so the state's RAM has the picture.
 * DROP, after a load: every GL object is forgotten as after a reset, and the
 * targets, textures, programs and vertex buffers rebuild from the restored
 * guest memory (programs come back from the disk cache). Depth lives only on
 * the GPU and is not saved: the first frame after a load starts with it
 * cleared. */
static uint32_t s_state_flushes, s_state_drops;   /* for the tests */
static void gl_state(int op)
{
    Nv2aPresentSurface done;
    int i;

    if (s_init <= 0)
        return;
    if (op == NV2A_BACKEND_STATE_FLUSH) {
        (void)pbo_complete(&done);
        for (i = 0; i < NV_GL_MAX_RT; i++)
            if (s_rt[i].tex)
                rt_readback(&s_rt[i]);
        s_state_flushes++;
    } else if (op == NV2A_BACKEND_STATE_DROP) {
        host_unmap();
        gl_reset_objects(0);
        s_state_drops++;
    }
}

static Nv2aBackend s_backend = {
    gl_clear, gl_draw, gl_flip,
    NV2A_BACKEND_CAP_PIXEL | NV2A_BACKEND_CAP_CULL | NV2A_BACKEND_CAP_PROGRAM
        | NV2A_BACKEND_CAP_POINTS_LINES | NV2A_BACKEND_CAP_VOLUME,
    gl_zpass_read, gl_zpass_end, gl_zpass_resolve, gl_draw_program, gl_zpass_flush,
    gl_state
};

static void unregister_self(void)
{
    host_shader_progress_reset();
    nv2a_backend_register(NULL);
}

/* RECOMP_GPU as set (NULL: not set) -> whether the GL back end draws. Not
 * set: the platform's default, on for Android (the launcher's Renderer
 * "Auto"; "Software" writes RECOMP_GPU=0, a title profile forces it with
 * env_force), off elsewhere. */
static int gpu_selected(const char *e, int platform_default)
{
    if (!e || !*e)
        return platform_default;
    return strcmp(e, "0") != 0;
}

#if defined(__ANDROID__)
#define NV2A_GL_DEFAULT_ON 1
#else
#define NV2A_GL_DEFAULT_ON 0
#endif

int nv2a_gl_install_from_env(void)
{
    const char *e = getenv("RECOMP_GPU");
    int by_default = !e || !*e;

    if (!gpu_selected(e, NV2A_GL_DEFAULT_ON)) {
        /* Said, so a log (the library check's renderer field, XBA-130)
         * tells a software run from one that never drew. */
        fprintf(stderr, "  [GPUGL] CPU rasteriser selected (RECOMP_GPU=%s)\n",
                e && *e ? e : "unset");
        return 0;
    }
    if (by_default) {
        /* Code that asks getenv("RECOMP_GPU") later (a title layer, the
         * frame pacing choice) sees what was chosen. */
        setenv("RECOMP_GPU", "1", 0);
        fprintf(stderr, "  [GPUGL] GPU back end by default on this platform"
                        " (RECOMP_GPU=0 for the software renderer)\n");
    }
    /* RECOMP_GPU_CULL=0: draw both faces (and strips as the CPU path
     * assembles them). Read here: the caps are part of the registration. */
    e = getenv("RECOMP_GPU_CULL");
    s_cull = !(e && strcmp(e, "0") == 0);
    if (!s_cull)
        s_backend.caps &= ~NV2A_BACKEND_CAP_CULL;
    /* RECOMP_GPU_VBO (default on): vertex-program batches from cached
     * guest vertex buffers in their own formats (raw_bind) instead of
     * gathered floats; 0 gathers every batch, as before. */
    e = getenv("RECOMP_GPU_VBO");
    s_vbo_on = !(e && strcmp(e, "0") == 0);
    if (s_vbo_on)
        s_backend.caps |= NV2A_BACKEND_CAP_RAW_VERTEX;
    e = getenv("RECOMP_GPU");
    nv2a_backend_register_with_present(&s_backend, gl_present);
    fprintf(stderr, "  [GPUGL] GPU back end selected (RECOMP_GPU=%s); the context is"
                    " created on the executor thread at the first draw\n", e);
    return 1;
}
