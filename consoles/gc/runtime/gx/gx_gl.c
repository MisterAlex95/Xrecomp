/* gx_gl.c -- GX's OpenGL ES 3.0 renderer (XBA-262).
 *
 * EFB: a framebuffer object (RGBA8 colour texture + 24-bit depth texture) of 640 x 528 times the internal scale,
 * GL rows bottom-up (EFB row y is GL row H-1-y). Draws are queued while the pixel state stays the same (any BP
 * register change, texture invalidation, copy, peek or poke draws the queue first) and drawn with one glDrawElements.
 *
 * Shaders: a program per TEV state (gx_shader.c), looked up by hash; until a state's program is ready the ubershader
 * draws it (the same functions, so the same pixels). Programs compile on a worker thread that owns an EGL context
 * shared with the game's, or through GL_KHR_parallel_shader_compile when that is all there is, or on first use
 * (async_shaders = 0). Every state met goes into the per-title list (written at detach) and the list read at attach is
 * compiled ahead.
 *
 * Textures: decoded on the CPU (gx_tex.c) into GL textures with their mip levels, cached by address / size / format /
 * palette, revalidated by a content hash once per epoch (each field and each TX_INVALIDATE). EFB copies to a texture
 * render into a GL texture at the internal resolution in the copy format's decoded form and enter the cache at their
 * address (with the RAM bytes' hash, to notice the CPU writing over them); optionally also encoded into RAM. Copies
 * to the XFB are filtered (the copy filter's vertical taps), gamma corrected, converted to YUV 4:2:2 on the GPU and
 * read back into RAM for the video interface; their GL picture at the internal resolution is kept for a presenter.
 * Peeks read a 1x copy of the EFB (colour, or depth encoded as colour) cached until the next draw; pokes clear one
 * pixel. */
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "gx_glapi.h"
#include "gx_internal.h"
#include "gx_shader.h"

#define BATCH_V (1u << 18)
#define BATCH_I (3u * BATCH_V)
#define PROG_BUCKETS 1024u
#define TEX_BUCKETS 4096u
#define TEX_KEEP_FIELDS 1200u

enum { P_PENDING = 0, P_READY = 1, P_FAILED = 2, P_COMPILING = 3 };

typedef struct prog_entry {
    gx_shader_key key;
    uint64_t hash;
    GLuint prog;
    GLuint vs, fs;           /* KHR_parallel_shader_compile: still attached */
    int state;               /* P_*, atomic */
    struct prog_entry *next; /* bucket chain */
    struct prog_entry *qnext;/* worker queue */
} prog_entry;

typedef struct tex_entry {
    uint32_t addr;
    unsigned w, h, fmt, levels, tlut_fmt;
    uint64_t tlut_hash, hash;
    uint32_t size;
    uint32_t epoch;
    uint64_t last_field;
    GLuint tex;
    bool from_efb;
    unsigned gw, gh;         /* GL size (EFB copies: times the scale) */
    struct tex_entry *next;
} tex_entry;

typedef struct ubo_t {
    int32_t creg[4][4], kreg[4][4], swap[4][4];
    float tcscale[8][4], texdim[8][4], indm[6][4], indscale[4][4];
    int32_t alpharef[4];
    float fog[4];
    int32_t fogcol[4], zbias[4];
    int32_t st[16][4], si[16][4];
    int32_t misc[4], misc2[4];
} ubo_t;

struct gx_gl {
    gx_glfn f;
    gx_eglfn egl;
    int scale, async, efb_copy_ram, trace;
    char *list_path;
    GLint max_tex;

    GLuint efb_fbo, efb_color, efb_depth;
    GLuint vao, vbo, ebo, ubo, empty_vao;
    GLuint samplers[8];
    GLuint white_tex;
    GLuint uber;
    GLuint copy_prog, copy_fbo;
    GLint cu_src, cu_out, cu_mode, cu_mode2, cu_filt, cu_color, cu_depth;
    GLuint peek_fbo, peek_tex;
    uint8_t *peek_color, *peek_z;
    bool peek_color_ok, peek_z_ok;

    prog_entry *progs[PROG_BUCKETS];
    tex_entry *texs[TEX_BUCKETS];
    uint8_t *decode_buf;
    size_t decode_cap;

    gx_vout *bv;
    uint32_t *bi;
    uint32_t nv, ni;
    bool batch_tri;
    unsigned batch_ntex;     /* texture coordinates the batch's draws generated (the most of them) */
    uint8_t *packed;         /* the batch's vertices with only those coordinates, for the upload */
    ubo_t ubo_data;

    struct { uint32_t addr; GLuint tex; int w, h; } xfb[4];
    unsigned xfb_next;

    /* worker */
    bool worker_ok, khr_parallel;
    pthread_t worker;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    prog_entry *queue_head, *queue_tail;
    bool quit;
    EGLDisplay dpy;
    EGLContext wctx;
    EGLSurface wsurf;

    /* states met (the per-title list) */
    gx_shader_key *keys;
    size_t nkeys, cap_keys;
    uint32_t (*vfmts)[5];
    size_t nvfmts, cap_vfmts;
    bool list_dirty;
};

#define F (g->f)

/* A desktop GL 3.3+ core context (gc_play's window on Linux / macOS) instead of GLES 3: the same GLSL under
 * #version 330 core. */
static int desktop_gl;

static void clear_depth(gx_glfn *f, float z)
{
    if (f->glClearDepthf)
        f->glClearDepthf(z);
    else if (f->glClearDepth)
        f->glClearDepth(z);
}

/* ---- programs ---- */

static void shader_source(gx_glfn *f, GLuint s, const char *src)
{
    static const char es[] = "#version 300 es\n";
    if (desktop_gl && !strncmp(src, es, sizeof es - 1)) {
        const char *parts[2] = {"#version 330 core\n", src + sizeof es - 1};
        f->glShaderSource(s, 2, parts, NULL);
    } else {
        f->glShaderSource(s, 1, &src, NULL);
    }
}

static GLuint compile_shader(gx_glfn *f, GLenum type, const char *src, char *log, size_t log_len)
{
    GLuint s = f->glCreateShader(type);
    shader_source(f, s, src);
    f->glCompileShader(s);
    GLint ok = 0;
    f->glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        if (log)
            f->glGetShaderInfoLog(s, (GLsizei)log_len, NULL, log);
        f->glDeleteShader(s);
        return 0;
    }
    return s;
}

static void program_setup(gx_glfn *f, GLuint p)
{
    f->glUseProgram(p);
    char name[16];
    for (int i = 0; i < 8; i++) {
        snprintf(name, sizeof name, "u_tex[%d]", i);
        GLint loc = f->glGetUniformLocation(p, name);
        if (loc >= 0)
            f->glUniform1i(loc, i);
    }
    GLuint bi = f->glGetUniformBlockIndex(p, "GXBlock");
    if (bi != GL_INVALID_INDEX)
        f->glUniformBlockBinding(p, bi, 0);
}

static GLuint link_program(gx_glfn *f, GLuint vs, GLuint fs, char *log, size_t log_len)
{
    GLuint p = f->glCreateProgram();
    f->glAttachShader(p, vs);
    f->glAttachShader(p, fs);
    if (f->glProgramParameteri)
        f->glProgramParameteri(p, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
    f->glLinkProgram(p);
    GLint ok = 0;
    f->glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        if (log)
            f->glGetProgramInfoLog(p, (GLsizei)log_len, NULL, log);
        f->glDeleteProgram(p);
        return 0;
    }
    return p;
}

/* Builds the program of k (NULL: the ubershader) in the current context. 0 on failure (logged). */
static GLuint build_program(gx_glfn *f, const gx_shader_key *k)
{
    static char log[4096];
    char *fsrc = gx_shader_fs_source(k);
    if (!fsrc)
        return 0;
    log[0] = 0;
    GLuint vs = compile_shader(f, GL_VERTEX_SHADER, gx_shader_vs_source(), log, sizeof log);
    GLuint fs = vs ? compile_shader(f, GL_FRAGMENT_SHADER, fsrc, log, sizeof log) : 0;
    GLuint p = fs ? link_program(f, vs, fs, log, sizeof log) : 0;
    if (!p)
        fprintf(stderr, "gx: %s shader failed: %s\n", k ? "TEV" : "uber", log);
    if (vs)
        f->glDeleteShader(vs);
    if (fs)
        f->glDeleteShader(fs);
    free(fsrc);
    if (p)
        program_setup(f, p);
    return p;
}

static void *worker_main(void *arg)
{
    gx_gl *g = arg;
    if (!g->egl.eglMakeCurrent(g->dpy, g->wsurf, g->wsurf, g->wctx)) {
        fprintf(stderr, "gx: shader worker cannot make its context current\n");
        pthread_mutex_lock(&g->mu);
        g->worker_ok = false;
        pthread_mutex_unlock(&g->mu);
        return NULL;
    }
    for (;;) {
        pthread_mutex_lock(&g->mu);
        while (!g->queue_head && !g->quit)
            pthread_cond_wait(&g->cv, &g->mu);
        if (g->quit) {
            pthread_mutex_unlock(&g->mu);
            break;
        }
        prog_entry *e = g->queue_head;
        g->queue_head = e->qnext;
        if (!g->queue_head)
            g->queue_tail = NULL;
        pthread_mutex_unlock(&g->mu);
        GLuint p = build_program(&g->f, &e->key);
        g->f.glFinish();
        e->prog = p;
        __atomic_store_n(&e->state, p ? P_READY : P_FAILED, __ATOMIC_RELEASE);
    }
    g->egl.eglMakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    return NULL;
}

static void worker_start(gx_gl *g)
{
    EGLDisplay dpy = g->egl.eglGetCurrentDisplay ? g->egl.eglGetCurrentDisplay() : EGL_NO_DISPLAY;
    EGLContext cur = g->egl.eglGetCurrentContext();
    if (dpy == EGL_NO_DISPLAY || cur == EGL_NO_CONTEXT)
        return;
    EGLint cfg_id = 0;
    EGLConfig cfg = NULL;
    if (g->egl.eglQueryContext && g->egl.eglQueryContext(dpy, cur, EGL_CONFIG_ID, &cfg_id) && cfg_id) {
        EGLint attr[] = {EGL_CONFIG_ID, cfg_id, EGL_NONE};
        EGLint n = 0;
        if (!g->egl.eglChooseConfig(dpy, attr, &cfg, 1, &n) || n < 1)
            cfg = NULL;
    }
    static const EGLint cattr[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext ctx = g->egl.eglCreateContext(dpy, cfg, cur, cattr);
    if (ctx == EGL_NO_CONTEXT)
        return;
    EGLSurface surf = EGL_NO_SURFACE;
    const char *ext = g->egl.eglQueryString ? g->egl.eglQueryString(dpy, EGL_EXTENSIONS) : NULL;
    if (!(ext && strstr(ext, "EGL_KHR_surfaceless_context")) && cfg) {
        static const EGLint pattr[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
        surf = g->egl.eglCreatePbufferSurface(dpy, cfg, pattr);
        if (surf == EGL_NO_SURFACE) {
            g->egl.eglDestroyContext(dpy, ctx);
            return;
        }
    }
    g->dpy = dpy;
    g->wctx = ctx;
    g->wsurf = surf;
    pthread_mutex_init(&g->mu, NULL);
    pthread_cond_init(&g->cv, NULL);
    g->worker_ok = true;
    if (pthread_create(&g->worker, NULL, worker_main, g)) {
        g->worker_ok = false;
        g->egl.eglDestroyContext(dpy, ctx);
        if (surf != EGL_NO_SURFACE)
            g->egl.eglDestroySurface(dpy, surf);
        g->wctx = EGL_NO_CONTEXT;
        return;
    }
}

static void worker_stop(gx_gl *g)
{
    if (g->wctx == EGL_NO_CONTEXT)
        return;
    pthread_mutex_lock(&g->mu);
    g->quit = true;
    pthread_cond_signal(&g->cv);
    pthread_mutex_unlock(&g->mu);
    pthread_join(g->worker, NULL);
    g->egl.eglDestroyContext(g->dpy, g->wctx);
    if (g->wsurf != EGL_NO_SURFACE)
        g->egl.eglDestroySurface(g->dpy, g->wsurf);
    g->wctx = EGL_NO_CONTEXT;
    pthread_mutex_destroy(&g->mu);
    pthread_cond_destroy(&g->cv);
}

static void record_key(gx_gl *g, const gx_shader_key *k)
{
    for (size_t i = 0; i < g->nkeys; i++)
        if (!memcmp(&g->keys[i], k, sizeof *k))
            return;
    if (g->nkeys == g->cap_keys) {
        size_t cap = g->cap_keys ? g->cap_keys * 2 : 64;
        gx_shader_key *nk = realloc(g->keys, cap * sizeof *nk);
        if (!nk)
            return;
        g->keys = nk;
        g->cap_keys = cap;
    }
    g->keys[g->nkeys++] = *k;
    g->list_dirty = true;
}

static void record_vfmt(gx_gl *g, const uint32_t v[5])
{
    for (size_t i = g->nvfmts; i-- > 0;)
        if (!memcmp(g->vfmts[i], v, 20))
            return;
    if (g->nvfmts == g->cap_vfmts) {
        size_t cap = g->cap_vfmts ? g->cap_vfmts * 2 : 32;
        uint32_t (*nv)[5] = realloc(g->vfmts, cap * sizeof *nv);
        if (!nv)
            return;
        g->vfmts = nv;
        g->cap_vfmts = cap;
    }
    memcpy(g->vfmts[g->nvfmts++], v, 20);
    g->list_dirty = true;
}

/* Starts building k's program the configured way. */
static void prog_start(gx_gl *g, prog_entry *e)
{
    if (g->async == 1 && g->worker_ok) {
        e->state = P_PENDING;
        pthread_mutex_lock(&g->mu);
        e->qnext = NULL;
        if (g->queue_tail)
            g->queue_tail->qnext = e;
        else
            g->queue_head = e;
        g->queue_tail = e;
        pthread_cond_signal(&g->cv);
        pthread_mutex_unlock(&g->mu);
        return;
    }
    if (g->async == 1 && g->khr_parallel) {
        char *fsrc = gx_shader_fs_source(&e->key);
        e->vs = F.glCreateShader(GL_VERTEX_SHADER);
        e->fs = F.glCreateShader(GL_FRAGMENT_SHADER);
        shader_source(&g->f, e->vs, gx_shader_vs_source());
        shader_source(&g->f, e->fs, fsrc ? fsrc : "");
        F.glCompileShader(e->vs);
        F.glCompileShader(e->fs);
        e->prog = F.glCreateProgram();
        F.glAttachShader(e->prog, e->vs);
        F.glAttachShader(e->prog, e->fs);
        F.glLinkProgram(e->prog);
        free(fsrc);
        e->state = P_COMPILING;
        return;
    }
    e->prog = build_program(&g->f, &e->key);
    e->state = e->prog ? P_READY : P_FAILED;
}

static void prog_poll(gx_gl *g, prog_entry *e)
{
    if (e->state != P_COMPILING)
        return;
    GLint done = 0;
    F.glGetProgramiv(e->prog, GL_COMPLETION_STATUS_KHR, &done);
    if (!done)
        return;
    GLint ok = 0;
    F.glGetProgramiv(e->prog, GL_LINK_STATUS, &ok);
    F.glDeleteShader(e->vs);
    F.glDeleteShader(e->fs);
    if (ok) {
        program_setup(&g->f, e->prog);
        e->state = P_READY;
    } else {
        char log[1024] = "";
        F.glGetProgramInfoLog(e->prog, sizeof log, NULL, log);
        fprintf(stderr, "gx: TEV shader failed: %s\n", log);
        F.glDeleteProgram(e->prog);
        e->prog = 0;
        e->state = P_FAILED;
    }
}

static prog_entry *prog_lookup(gx_gl *g, const gx_shader_key *k, bool create)
{
    uint64_t h = gx_shader_key_hash(k);
    prog_entry **b = &g->progs[h & (PROG_BUCKETS - 1)];
    for (prog_entry *e = *b; e; e = e->next)
        if (e->hash == h && !memcmp(&e->key, k, sizeof *k))
            return e;
    if (!create)
        return NULL;
    prog_entry *e = calloc(1, sizeof *e);
    if (!e)
        return NULL;
    e->key = *k;
    e->hash = h;
    e->next = *b;
    *b = e;
    if (g->trace)
        fprintf(stderr, "gx: new TEV state %016llX: %u stage(s), %u indirect, alpha test %06X, fog %X, ztex %X, pe %X\n",
                (unsigned long long)h, k->nst, k->nind, k->alpha_cmp, k->fog, k->ztex, k->pe);
    record_key(g, k);
    prog_start(g, e);
    return e;
}

/* The program to draw state k with now; *uber tells whether it is the ubershader. */
static GLuint prog_for(gx_state *gx, const gx_shader_key *k, bool *uber)
{
    gx_gl *g = gx->gl;
    *uber = true;
    if (g->async == -1) {
        record_key(g, k);
        return g->uber;
    }
    prog_entry *e = prog_lookup(g, k, true);
    if (!e)
        return g->uber;
    prog_poll(g, e);
    if (__atomic_load_n(&e->state, __ATOMIC_ACQUIRE) == P_READY) {
        *uber = false;
        return e->prog;
    }
    return g->uber;
}

/* ---- textures ---- */

static GLuint new_tex(gx_gl *g)
{
    GLuint t;
    F.glGenTextures(1, &t);
    F.glBindTexture(GL_TEXTURE_2D, t);
    F.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    F.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    F.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    F.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

static unsigned tex_family(unsigned fmt)
{
    switch (fmt) {
    case TF_C4: case TF_C8: case TF_C14X2: case TF_CMPR:
        return 0xFF; /* never an EFB copy */
    default:
        return fmt;
    }
}

static uint32_t tlut_bytes(unsigned fmt)
{
    return fmt == TF_C4 ? 32 : fmt == TF_C8 ? 512 : fmt == TF_C14X2 ? 32768 : 0;
}

static void tex_free(gx_gl *g, tex_entry *e)
{
    F.glDeleteTextures(1, &e->tex);
    free(e);
}

/* The GL texture for map n as the registers describe it now; *gw / *gh its texel size as GX sees it. */
static GLuint tex_for(gx_state *gx, unsigned map, unsigned *tw, unsigned *th)
{
    gx_gl *g = gx->gl;
    gx_texinfo ti;
    gx_tex_info(gx, map, &ti);
    *tw = ti.w;
    *th = ti.h;
    if (!ti.addr || ti.addr >= gx->ram_size || ti.size > gx->ram_size - ti.addr)
        return g->white_tex;
    tex_entry **b = &g->texs[(ti.addr >> 5) & (TEX_BUCKETS - 1)];
    /* an EFB copy at that address, read back in its own format family */
    for (tex_entry **pp = b; *pp; pp = &(*pp)->next) {
        tex_entry *e = *pp;
        if (!e->from_efb || e->addr != ti.addr)
            continue;
        if (e->w == ti.w && e->h == ti.h && e->fmt == tex_family(ti.fmt)) {
            if (e->epoch != gx->tex_epoch) {
                if (gx_hash(gx->ram + e->addr, e->size, 0) != e->hash)
                    break; /* the CPU wrote over it: decode RAM */
                e->epoch = gx->tex_epoch;
            }
            e->last_field = gx->field_count;
            gx->stats.tex_hits++;
            return e->tex;
        }
    }
    uint64_t th_hash = 0;
    uint32_t tb = tlut_bytes(ti.fmt);
    if (tb) {
        uint32_t a = ti.tlut_addr;
        if (a + tb > GX_TMEM_SIZE)
            a = GX_TMEM_SIZE - tb;
        th_hash = gx_hash(gx->tmem + a, tb, ti.tlut_fmt);
    }
    tex_entry *e = NULL;
    for (tex_entry *x = *b; x; x = x->next)
        if (!x->from_efb && x->addr == ti.addr && x->w == ti.w && x->h == ti.h && x->fmt == ti.fmt &&
            x->levels == ti.levels && x->tlut_fmt == ti.tlut_fmt && x->tlut_hash == th_hash) {
            e = x;
            break;
        }
    if (e && e->epoch == gx->tex_epoch) {
        e->last_field = gx->field_count;
        gx->stats.tex_hits++;
        return e->tex;
    }
    uint64_t h = gx_hash(gx->ram + ti.addr, ti.size, ti.fmt);
    if (e && e->hash == h) {
        e->epoch = gx->tex_epoch;
        e->last_field = gx->field_count;
        gx->stats.tex_hits++;
        return e->tex;
    }
    if (!e) {
        e = calloc(1, sizeof *e);
        if (!e)
            return g->white_tex;
        e->addr = ti.addr, e->w = ti.w, e->h = ti.h, e->fmt = ti.fmt, e->levels = ti.levels;
        e->tlut_fmt = ti.tlut_fmt, e->tlut_hash = th_hash;
        e->tex = new_tex(g);
        e->next = *b;
        *b = e;
    }
    e->hash = h;
    e->size = ti.size;
    e->epoch = gx->tex_epoch;
    e->last_field = gx->field_count;
    e->gw = ti.w, e->gh = ti.h;
    size_t need = (size_t)ti.w * ti.h * 4;
    if (need > g->decode_cap) {
        uint8_t *nb = realloc(g->decode_buf, need);
        if (!nb)
            return g->white_tex;
        g->decode_buf = nb;
        g->decode_cap = need;
    }
    F.glBindTexture(GL_TEXTURE_2D, e->tex);
    F.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    unsigned w = ti.w, hh = ti.h;
    uint32_t off = 0;
    for (unsigned l = 0; l < ti.levels; l++) {
        gx_tex_decode(gx, &ti, gx->ram + ti.addr + off, w, hh, g->decode_buf);
        F.glTexImage2D(GL_TEXTURE_2D, (GLint)l, GL_RGBA8, (GLsizei)w, (GLsizei)hh, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                       g->decode_buf);
        off += gx_tex_level_size(ti.fmt, w, hh);
        w = w > 1 ? w / 2 : 1;
        hh = hh > 1 ? hh / 2 : 1;
    }
    F.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)ti.levels - 1);
    gx->stats.tex_decodes++;
    if (g->trace)
        fprintf(stderr, "gx: texture map %u at %08X %ux%u format %u, %u level(s), tlut %05X format %u\n", map, ti.addr,
                ti.w, ti.h, ti.fmt, ti.levels, ti.tlut_addr, ti.tlut_fmt);
    return e->tex;
}

static GLint gl_wrap(unsigned w)
{
    return w == 1 ? GL_REPEAT : w == 2 ? GL_MIRRORED_REPEAT : GL_CLAMP_TO_EDGE;
}

static void bind_map(gx_state *gx, unsigned map)
{
    gx_gl *g = gx->gl;
    unsigned tw, th;
    GLuint t = tex_for(gx, map, &tw, &th);
    F.glActiveTexture(GL_TEXTURE0 + map);
    F.glBindTexture(GL_TEXTURE_2D, t);
    uint32_t m0 = gx->bp[bp_tex_reg(BP_TX_MODE0, map)], m1 = gx->bp[bp_tex_reg(BP_TX_MODE1, map)];
    gx_texinfo ti;
    gx_tex_info(gx, map, &ti);
    GLuint s = g->samplers[map];
    F.glSamplerParameteri(s, GL_TEXTURE_WRAP_S, gl_wrap(BITS(m0, 0, 2)));
    F.glSamplerParameteri(s, GL_TEXTURE_WRAP_T, gl_wrap(BITS(m0, 2, 2)));
    F.glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, (m0 & 0x10) ? GL_LINEAR : GL_NEAREST);
    unsigned mf = BITS(m0, 5, 3);
    GLint min;
    if (ti.levels <= 1 || t == g->white_tex)
        min = (mf & 4) ? GL_LINEAR : GL_NEAREST;
    else {
        static const GLint mins[8] = {GL_NEAREST, GL_NEAREST_MIPMAP_NEAREST, GL_NEAREST_MIPMAP_LINEAR, GL_NEAREST,
                                      GL_LINEAR, GL_LINEAR_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR};
        min = mins[mf];
    }
    F.glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, min);
    F.glSamplerParameterf(s, GL_TEXTURE_MIN_LOD, (float)BITS(m1, 0, 8) / 16.0f);
    F.glSamplerParameterf(s, GL_TEXTURE_MAX_LOD, (float)BITS(m1, 8, 8) / 16.0f);
    F.glBindSampler(map, s);
    float *d = g->ubo_data.texdim[map];
    d[0] = (float)tw;
    d[1] = (float)th;
    d[2] = (float)(int8_t)BITS(m0, 9, 8) / 32.0f;
    d[3] = 0;
}

/* ---- state ---- */

static GLenum blend_src(unsigned v)
{
    static const GLenum t[8] = {GL_ZERO, GL_ONE, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR,
                                GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
    return t[v & 7];
}

static GLenum blend_dst(unsigned v)
{
    static const GLenum t[8] = {GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR,
                                GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
    return t[v & 7];
}

static GLenum no_dst_alpha(GLenum f)
{
    return f == GL_DST_ALPHA ? GL_ONE : f == GL_ONE_MINUS_DST_ALPHA ? GL_ZERO : f;
}

static void efb_bind(gx_gl *g)
{
    F.glBindFramebuffer(GL_FRAMEBUFFER, g->efb_fbo);
    F.glViewport(0, 0, GX_EFB_W * g->scale, GX_EFB_H * g->scale);
}

/* The EFB rectangle x0..x1, y0..y1 (top-down) as a GL scissor. */
static void scissor_rect(gx_gl *g, int x0, int y0, int x1, int y1)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > GX_EFB_W) x1 = GX_EFB_W;
    if (y1 > GX_EFB_H) y1 = GX_EFB_H;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    int s = g->scale;
    F.glEnable(GL_SCISSOR_TEST);
    F.glScissor(x0 * s, (GX_EFB_H - y1) * s, (x1 - x0) * s, (y1 - y0) * s);
}

static void apply_state(gx_state *gx, bool tri)
{
    gx_gl *g = gx->gl;
    const uint32_t *bp = gx->bp;
    efb_bind(g);
    int offx = (int)BITS(bp[BP_SCISSOR_OFFSET], 0, 10) * 2, offy = (int)BITS(bp[BP_SCISSOR_OFFSET], 10, 10) * 2;
    int top = (int)BITS(bp[BP_SCISSOR_TL], 0, 11), left = (int)BITS(bp[BP_SCISSOR_TL], 12, 11);
    int bottom = (int)BITS(bp[BP_SCISSOR_BR], 0, 11), right = (int)BITS(bp[BP_SCISSOR_BR], 12, 11);
    scissor_rect(g, left - offx, top - offy, right - offx + 1, bottom - offy + 1);

    uint32_t zm = bp[BP_ZMODE];
    if (zm & 1) {
        F.glEnable(GL_DEPTH_TEST);
        F.glDepthFunc(GL_NEVER + BITS(zm, 1, 3));
        F.glDepthMask((zm & 0x10) ? GL_TRUE : GL_FALSE);
    } else {
        F.glDisable(GL_DEPTH_TEST);
        F.glDepthMask(GL_FALSE);
    }
    uint32_t cm0 = bp[BP_CMODE0];
    unsigned fmt = BITS(bp[BP_PE_CONTROL], 0, 3);
    bool has_alpha = fmt == 1;
    bool blend = cm0 & 1, logic = (cm0 & 2) && !blend;
    GLboolean cmask = (cm0 & 8) ? GL_TRUE : GL_FALSE, amask = ((cm0 & 0x10) && has_alpha) ? GL_TRUE : GL_FALSE;
    if (logic && BITS(cm0, 12, 4) == 5) /* NOOP */
        cmask = amask = GL_FALSE;
    F.glColorMask(cmask, cmask, cmask, amask);
    if (blend) {
        F.glEnable(GL_BLEND);
        if (cm0 & 0x800) {
            F.glBlendEquationSeparate(GL_FUNC_REVERSE_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT);
            F.glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE);
        } else {
            GLenum s = blend_src(BITS(cm0, 8, 3)), d = blend_dst(BITS(cm0, 5, 3));
            if (!has_alpha)
                s = no_dst_alpha(s), d = no_dst_alpha(d);
            F.glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
            F.glBlendFuncSeparate(s, d, s, d);
        }
    } else {
        F.glDisable(GL_BLEND);
    }
    unsigned cull = BITS(bp[BP_GENMODE], 14, 2);
    F.glFrontFace(GL_CCW);
    if (tri && cull) {
        F.glEnable(GL_CULL_FACE);
        F.glCullFace(cull == 1 ? GL_BACK : cull == 2 ? GL_FRONT : GL_FRONT_AND_BACK);
    } else {
        F.glDisable(GL_CULL_FACE);
    }
}

static int sext11(uint32_t v)
{
    v &= 0x7FF;
    return v >= 0x400 ? (int)v - 0x800 : (int)v;
}

static void fill_ubo(gx_state *gx, const gx_shader_key *k)
{
    gx_gl *g = gx->gl;
    ubo_t *u = &g->ubo_data;
    const uint32_t *bp = gx->bp;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            u->creg[r][c] = gx->creg[r][c];
            u->kreg[r][c] = gx->kreg[r][c];
        }
    for (int t = 0; t < 4; t++) {
        uint32_t a = bp[BP_TEV_KSEL + 2 * t], b = bp[BP_TEV_KSEL + 2 * t + 1];
        u->swap[t][0] = (int32_t)BITS(a, 0, 2);
        u->swap[t][1] = (int32_t)BITS(a, 2, 2);
        u->swap[t][2] = (int32_t)BITS(b, 0, 2);
        u->swap[t][3] = (int32_t)BITS(b, 2, 2);
    }
    for (int c = 0; c < 8; c++) {
        u->tcscale[c][0] = (float)(BITS(bp[BP_SU_SSIZE + 2 * c], 0, 16) + 1);
        u->tcscale[c][1] = (float)(BITS(bp[BP_SU_SSIZE + 2 * c + 1], 0, 16) + 1);
        u->tcscale[c][2] = u->tcscale[c][3] = 0;
    }
    for (int m = 0; m < 3; m++) {
        uint32_t ra = bp[BP_IND_MTXA + 3 * m], rb = bp[BP_IND_MTXA + 3 * m + 1], rc = bp[BP_IND_MTXA + 3 * m + 2];
        int sc = (int)(BITS(ra, 22, 2) | BITS(rb, 22, 2) << 2 | BITS(rc, 22, 2) << 4);
        float scale = ldexpf(1.0f, sc - 17);
        float e = scale / 1024.0f;
        u->indm[2 * m][0] = (float)sext11(ra) * e;
        u->indm[2 * m][1] = (float)sext11(rb) * e;
        u->indm[2 * m][2] = (float)sext11(rc) * e;
        u->indm[2 * m][3] = scale;
        u->indm[2 * m + 1][0] = (float)sext11(ra >> 11) * e;
        u->indm[2 * m + 1][1] = (float)sext11(rb >> 11) * e;
        u->indm[2 * m + 1][2] = (float)sext11(rc >> 11) * e;
        u->indm[2 * m + 1][3] = scale;
    }
    for (int j = 0; j < 4; j++) {
        uint32_t ss = bp[j < 2 ? BP_RAS_SS0 : BP_RAS_SS1] >> (8 * (j & 1));
        u->indscale[j][0] = ldexpf(1.0f, -(int)BITS(ss, 0, 4));
        u->indscale[j][1] = ldexpf(1.0f, -(int)BITS(ss, 4, 4));
        u->indscale[j][2] = u->indscale[j][3] = 0;
    }
    uint32_t ac = bp[BP_ALPHA_COMPARE];
    u->alpharef[0] = (int32_t)BITS(ac, 0, 8);
    u->alpharef[1] = (int32_t)BITS(ac, 8, 8);
    u->alpharef[2] = (int32_t)BITS(bp[BP_CMODE1], 0, 8);
    u->alpharef[3] = 0;
    float a = gx_f32((bp[BP_FOG_A] & 0xFFFFFu) << 12), c = gx_f32((bp[BP_FOG_C] & 0xFFFFFu) << 12);
    u->fog[0] = a;
    u->fog[1] = (float)BITS(bp[BP_FOG_B_MAG], 0, 24) / 16777216.0f; /* ze = a / (b_mag / 2^24 - z / 2^shift) */
    u->fog[2] = ldexpf(1.0f, -(int)BITS(bp[BP_FOG_B_SHIFT], 0, 5));
    u->fog[3] = c;
    u->fogcol[0] = (int32_t)BITS(bp[BP_FOG_COLOR], 16, 8);
    u->fogcol[1] = (int32_t)BITS(bp[BP_FOG_COLOR], 8, 8);
    u->fogcol[2] = (int32_t)BITS(bp[BP_FOG_COLOR], 0, 8);
    u->fogcol[3] = 0;
    u->zbias[0] = (int32_t)BITS(bp[BP_ZTEX0], 0, 24);
    if (g->trace && k->fog)
        fprintf(stderr, "gx: fog %X a %g b %g shift %g c %g colour %d %d %d\n", k->fog, u->fog[0], u->fog[1], u->fog[2],
                u->fog[3], u->fogcol[0], u->fogcol[1], u->fogcol[2]);
    for (int s = 0; s < 16; s++) {
        u->st[s][0] = (int32_t)k->st_c[s];
        u->st[s][1] = (int32_t)k->st_a[s];
        u->st[s][2] = (int32_t)k->st_t[s];
        u->st[s][3] = (int32_t)k->st_k[s];
        u->si[s][0] = (int32_t)k->st_i[s];
    }
    u->misc[0] = (int32_t)k->nst;
    u->misc[1] = (int32_t)k->nind;
    u->misc[2] = (int32_t)k->iref;
    u->misc[3] = (int32_t)k->alpha_cmp;
    u->misc2[0] = (int32_t)k->fog;
    u->misc2[1] = (int32_t)k->ztex;
    u->misc2[2] = (int32_t)k->pe;
    F.glBindBuffer(GL_UNIFORM_BUFFER, g->ubo);
    F.glBufferData(GL_UNIFORM_BUFFER, sizeof *u, u, GL_STREAM_DRAW);
    F.glBindBufferBase(GL_UNIFORM_BUFFER, 0, g->ubo);
}

void gx_gl_flush(gx_state *gx)
{
    gx_gl *g = gx->gl;
    if (!g || !g->ni)
        return;
    gx_shader_key k;
    gx_shader_key_make(gx, &k);
    bool uber;
    GLuint prog = prog_for(gx, &k, &uber);
    if (!prog) {
        g->nv = g->ni = 0;
        return;
    }
    apply_state(gx, g->batch_tri);
    bool used[8] = {false};
    for (unsigned s = 0; s < k.nst; s++)
        if (k.st_t[s] & 0x40)
            used[k.st_t[s] & 7] = true;
    for (unsigned j = 0; j < k.nind; j++)
        used[BITS(k.iref, 6 * j, 3)] = true;
    for (unsigned m = 0; m < 8; m++) {
        if (used[m]) {
            bind_map(gx, m);
        } else {
            g->ubo_data.texdim[m][0] = g->ubo_data.texdim[m][1] = 1.0f;
        }
    }
    fill_ubo(gx, &k);
    F.glUseProgram(prog);
    /* upload position, colours and only the texture coordinates generated: 24 + 12 x ntex bytes a vertex */
    F.glBindVertexArray(g->vao);
    F.glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
    unsigned nt = g->batch_ntex;
    size_t stride = offsetof(gx_vout, tex) + 12 * (size_t)nt;
    for (uint32_t i = 0; i < g->nv; i++)
        memcpy(g->packed + i * stride, &g->bv[i], stride);
    F.glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(g->nv * stride), g->packed, GL_STREAM_DRAW);
    GLsizei st = (GLsizei)stride;
    F.glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, st, (const void *)offsetof(gx_vout, pos));
    F.glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, st, (const void *)offsetof(gx_vout, c0));
    F.glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, st, (const void *)offsetof(gx_vout, c1));
    for (unsigned i = 0; i < 8; i++) {
        if (i < nt) {
            F.glEnableVertexAttribArray(3 + i);
            F.glVertexAttribPointer(3 + i, 3, GL_FLOAT, GL_FALSE, st, (const void *)(offsetof(gx_vout, tex) + 12 * i));
        } else {
            F.glDisableVertexAttribArray(3 + i);
            F.glVertexAttrib4f(3 + i, 0, 0, 1, 1);
        }
    }
    F.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g->ebo);
    F.glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(g->ni * 4), g->bi, GL_STREAM_DRAW);
    F.glDrawElements(GL_TRIANGLES, (GLsizei)g->ni, GL_UNSIGNED_INT, NULL);
    gx->stats.batches++;
    if (uber)
        gx->stats.uber_draws++;
    else
        gx->stats.special_draws++;
    g->nv = g->ni = 0;
    g->peek_color_ok = g->peek_z_ok = false;
}

/* ---- primitives ---- */

static void push_tri(gx_gl *g, uint32_t a, uint32_t b, uint32_t c)
{
    g->bi[g->ni++] = a;
    g->bi[g->ni++] = b;
    g->bi[g->ni++] = c;
}

/* A line or point as two triangles: offsets in EFB pixels, along x and y. */
static void push_quad(gx_gl *g, const gx_vout *a, const gx_vout *b, float ax, float ay, float bx, float by)
{
    uint32_t base = g->nv;
    const gx_vout *src[4] = {a, a, b, b};
    float dx[4] = {-ax, ax, -bx, bx}, dy[4] = {-ay, ay, -by, by};
    for (int i = 0; i < 4; i++) {
        gx_vout *o = &g->bv[g->nv++];
        *o = *src[i];
        o->pos[0] += dx[i] * 2.0f / GX_EFB_W * o->pos[3];
        o->pos[1] -= dy[i] * 2.0f / GX_EFB_H * o->pos[3];
    }
    push_tri(g, base, base + 1, base + 2);
    push_tri(g, base + 1, base + 3, base + 2);
}

void gx_gl_draw(gx_state *gx, unsigned prim, unsigned n)
{
    gx_gl *g = gx->gl;
    if (!g)
        return;
    uint32_t vf[5] = {gx->cpr[0x50], gx->cpr[0x60], gx->cpr[0x70 + gx->last_vat], gx->cpr[0x80 + gx->last_vat],
                      gx->cpr[0x90 + gx->last_vat]};
    record_vfmt(g, vf);
    bool tri = prim < 5;
    if (g->ni && tri != g->batch_tri)
        gx_gl_flush(gx);
    if (g->nv + 2 * n + 4 > BATCH_V || g->ni + 6 * n + 6 > BATCH_I)
        gx_gl_flush(gx);
    if (!g->ni)
        g->batch_ntex = 0;
    if (gx->vout_ntex > g->batch_ntex)
        g->batch_ntex = gx->vout_ntex;
    g->batch_tri = tri;
    const gx_vout *v = gx->vout;
    if (tri) {
        uint32_t base = g->nv;
        memcpy(g->bv + g->nv, v, n * sizeof *v);
        g->nv += n;
        switch (prim) {
        case 0: case 1: /* quads */
            for (unsigned i = 0; i + 3 < n; i += 4) {
                push_tri(g, base + i, base + i + 1, base + i + 2);
                push_tri(g, base + i, base + i + 2, base + i + 3);
            }
            break;
        case 2:
            for (unsigned i = 0; i + 2 < n; i += 3)
                push_tri(g, base + i, base + i + 1, base + i + 2);
            break;
        case 3: /* strip: keep the winding */
            for (unsigned i = 0; i + 2 < n; i++) {
                if (i & 1)
                    push_tri(g, base + i + 1, base + i, base + i + 2);
                else
                    push_tri(g, base + i, base + i + 1, base + i + 2);
            }
            break;
        default: /* fan */
            for (unsigned i = 1; i + 1 < n; i++)
                push_tri(g, base, base + i, base + i + 1);
            break;
        }
        return;
    }
    uint32_t lp = gx->bp[BP_LPSIZE];
    if (prim == 7) {
        float hs = (float)BITS(lp, 8, 8) / 12.0f;
        for (unsigned i = 0; i < n; i++) {
            gx_vout a = v[i], b = v[i];
            /* a point: a square, as a "line" from the top edge to the bottom edge */
            float w = a.pos[3];
            a.pos[1] += hs * 2.0f / GX_EFB_H * w;
            b.pos[1] -= hs * 2.0f / GX_EFB_H * w;
            push_quad(g, &a, &b, hs, 0, hs, 0);
        }
        return;
    }
    float hw = (float)BITS(lp, 0, 8) / 12.0f;
    unsigned step = prim == 5 ? 2 : 1;
    for (unsigned i = 0; i + 1 < n; i += step) {
        const gx_vout *a = &v[i], *b = &v[i + 1];
        float ax = a->pos[0] / a->pos[3], ay = a->pos[1] / a->pos[3];
        float bx = b->pos[0] / b->pos[3], by = b->pos[1] / b->pos[3];
        float dx = (bx - ax) * GX_EFB_W / 2, dy = (by - ay) * GX_EFB_H / 2;
        if (fabsf(dx) >= fabsf(dy))
            push_quad(g, a, b, 0, hw, 0, hw);
        else
            push_quad(g, a, b, hw, 0, hw, 0);
    }
}

void gx_gl_tex_invalidate(gx_state *gx)
{
    gx_gl_flush(gx);
}

/* ---- copies ---- */

static const char copy_vs[] =
    "#version 300 es\n"
    "void main() {\n"
    "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

/* u_mode.x: 0 texture copy, 1 XFB (YUYV), 2 depth as colour, 3 colour as is.
 * u_mode.yzw: family, selector 0, selector 1 (sources: r g b a Y zhi zmid zlo 255).
 * u_mode2: depth source, EFB has alpha, gamma (0 1.0, 1 1.7, 2 2.2), filter on.
 * u_src: EFB rectangle (x, y, w, h, top-down); u_out: output size, scale, EFB height. */
static const char copy_fs[] =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "uniform sampler2D u_color;\n"
    "uniform highp sampler2D u_depth;\n"
    "uniform vec4 u_src;\n"
    "uniform vec4 u_out;\n"
    "uniform ivec4 u_mode;\n"
    "uniform ivec4 u_mode2;\n"
    "uniform ivec4 u_filt;\n"
    "out vec4 o_color;\n"
    "ivec4 col_at(vec2 p) {\n"
    "  p.y = clamp(p.y, u_src.y + 0.5, u_src.y + u_src.w - 0.5);\n"
    "  vec2 uv = vec2(p.x / 640.0, (u_out.w - p.y) / u_out.w);\n"
    "  ivec4 c = ivec4(round(texture(u_color, uv) * 255.0));\n"
    "  if (u_mode2.y == 0) c.a = 255;\n"
    "  return c;\n"
    "}\n"
    "ivec4 col_f(vec2 p) {\n"
    "  if (u_mode2.w == 0) return col_at(p);\n"
    "  ivec4 c = col_at(p - vec2(0.0, 1.0)) * u_filt.x + col_at(p) * u_filt.y + col_at(p + vec2(0.0, 1.0)) * u_filt.z;\n"
    "  return clamp(c >> 6, 0, 255);\n"
    "}\n"
    "int z_at(vec2 p) {\n"
    "  ivec2 t = ivec2(floor(vec2(p.x, u_out.w - p.y) * u_out.z));\n"
    "  t = clamp(t, ivec2(0), ivec2(int(640.0 * u_out.z) - 1, int(u_out.w * u_out.z) - 1));\n"
    "  return int(texelFetch(u_depth, t, 0).r * 16777215.0 + 0.5);\n"
    "}\n"
    "ivec4 gam(ivec4 c) {\n"
    "  if (u_mode2.z == 0) return c;\n"
    "  float g = u_mode2.z == 1 ? 1.0 / 1.7 : 1.0 / 2.2;\n"
    "  return ivec4(round(pow(vec4(c) / 255.0, vec4(g, g, g, 1.0)) * 255.0));\n"
    "}\n"
    "ivec3 yuv(ivec4 c) {\n"
    "  vec3 f = vec3(c.rgb);\n"
    "  float y = 0.257 * f.r + 0.504 * f.g + 0.098 * f.b + 16.0;\n"
    "  float u = -0.148 * f.r - 0.291 * f.g + 0.439 * f.b + 128.0;\n"
    "  float v = 0.439 * f.r - 0.368 * f.g - 0.071 * f.b + 128.0;\n"
    "  return clamp(ivec3(round(vec3(y, u, v))), 0, 255);\n"
    "}\n"
    "int q4(int v) { return (v >> 4) * 17; }\n"
    "void main() {\n"
    "  vec2 o = floor(gl_FragCoord.xy);\n"
    "  if (u_mode.x == 1) {\n"
    "    float sy = u_src.y + (o.y + 0.5) * u_src.w / u_out.y;\n"
    "    float sx = u_src.x + (2.0 * o.x + 0.5) * u_src.z / (2.0 * u_out.x);\n"
    "    float dx = u_src.z / (2.0 * u_out.x);\n"
    "    ivec3 a = yuv(gam(col_f(vec2(sx, sy)))), b = yuv(gam(col_f(vec2(sx + dx, sy))));\n"
    "    o_color = vec4(float(a.x), float((a.y + b.y + 1) >> 1), float(b.x), float((a.z + b.z + 1) >> 1)) / 255.0;\n"
    "    return;\n"
    "  }\n"
    "  vec2 p = u_src.xy + (o + 0.5) * u_src.zw / u_out.xy;\n"
    "  if (u_mode.x == 2) {\n"
    "    int z = z_at(p);\n"
    "    o_color = vec4(float(z >> 16), float((z >> 8) & 255), float(z & 255), 255.0) / 255.0;\n"
    "    return;\n"
    "  }\n"
    "  if (u_mode.x == 3) { o_color = vec4(col_at(p)) / 255.0; return; }\n"
    "  ivec4 c = u_mode2.x == 1 ? ivec4(0, 0, 0, 255) : col_f(p);\n"
    "  int z = u_mode2.x == 1 ? z_at(p) : 0;\n"
    "  int src[9];\n"
    "  src[0] = c.r; src[1] = c.g; src[2] = c.b; src[3] = c.a;\n"
    "  src[4] = clamp(int(round(0.257 * float(c.r) + 0.504 * float(c.g) + 0.098 * float(c.b) + 16.0)), 0, 255);\n"
    "  src[5] = z >> 16; src[6] = (z >> 8) & 255; src[7] = z & 255; src[8] = 255;\n"
    "  int fam = u_mode.y, s0 = src[u_mode.z], s1 = src[u_mode.w];\n"
    "  ivec4 r;\n"
    "  if (fam == 0) r = ivec4(q4(s0));\n"
    "  else if (fam == 1) r = ivec4(s0);\n"
    "  else if (fam == 2) r = ivec4(q4(s0), q4(s0), q4(s0), q4(s1));\n"
    "  else if (fam == 3) r = ivec4(s0, s0, s0, s1);\n"
    "  else {\n"
    "    ivec4 x = u_mode.z == 5 ? ivec4(src[5], src[6], src[7], 255) : c;\n"
    "    if (fam == 4) { ivec3 q = x.rgb >> ivec3(3, 2, 3); r = ivec4((q << ivec3(3, 2, 3)) | (q >> ivec3(2, 4, 2)), 255); }\n"
    "    else if (fam == 5) {\n"
    "      if (x.a >= 224) { ivec3 q = x.rgb >> 3; r = ivec4((q << 3) | (q >> 2), 255); }\n"
    "      else { ivec3 q = x.rgb >> 4; int a3 = x.a >> 5; r = ivec4(q * 17, (a3 << 5) | (a3 << 2) | (a3 >> 1)); }\n"
    "    } else r = x;\n"
    "  }\n"
    "  o_color = vec4(r) / 255.0;\n"
    "}\n";

/* EFB copy format -> (texture family, selector 0, selector 1). Sources: 0 r 1 g 2 b 3 a 4 Y 5 zhi 6 zmid 7 zlo. */
static void copy_format(unsigned real, bool intensity, bool depth, unsigned *fam, unsigned *s0, unsigned *s1)
{
    *s1 = 3;
    if (depth) {
        switch (real) {
        case 0: *fam = TF_I4, *s0 = 5; return;
        case 1: case 8: *fam = TF_I8, *s0 = 5; return;
        case 9: *fam = TF_I8, *s0 = 6; return;
        case 10: *fam = TF_I8, *s0 = 7; return;
        case 3: case 11: *fam = TF_IA8, *s0 = 6, *s1 = 5; return;
        case 12: *fam = TF_IA8, *s0 = 7, *s1 = 6; return;
        default: *fam = TF_RGBA8, *s0 = 5; return;
        }
    }
    unsigned i = intensity ? 4 : 0;
    switch (real) {
    case 0: *fam = TF_I4, *s0 = i; return;
    case 1: *fam = TF_I8, *s0 = i; return;
    case 2: *fam = TF_IA4, *s0 = i; return;
    case 3: *fam = TF_IA8, *s0 = i; return;
    case 4: *fam = TF_RGB565, *s0 = 0; return;
    case 5: *fam = TF_RGB5A3, *s0 = 0; return;
    case 7: *fam = TF_I8, *s0 = 3; return;
    case 8: *fam = TF_I8, *s0 = 0; return;
    case 9: *fam = TF_I8, *s0 = 1; return;
    case 10: *fam = TF_I8, *s0 = 2; return;
    case 11: *fam = TF_IA8, *s0 = 1, *s1 = 0; return;
    case 12: *fam = TF_IA8, *s0 = 2, *s1 = 1; return;
    default: *fam = TF_RGBA8, *s0 = 0; return;
    }
}

/* Runs the copy shader into texture dst (ow x oh texels) from EFB rectangle src. */
static void copy_pass(gx_state *gx, GLuint dst, int ow, int oh, const float src[4], int mode, unsigned fam,
                      unsigned s0, unsigned s1, bool depth, int gamma)
{
    gx_gl *g = gx->gl;
    const uint32_t *bp = gx->bp;
    F.glBindFramebuffer(GL_FRAMEBUFFER, g->copy_fbo);
    F.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst, 0);
    F.glViewport(0, 0, ow, oh);
    F.glDisable(GL_SCISSOR_TEST);
    F.glDisable(GL_DEPTH_TEST);
    F.glDisable(GL_BLEND);
    F.glDisable(GL_CULL_FACE);
    F.glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    F.glUseProgram(g->copy_prog);
    F.glActiveTexture(GL_TEXTURE0);
    F.glBindTexture(GL_TEXTURE_2D, g->efb_color);
    F.glBindSampler(0, 0);
    F.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    F.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    F.glActiveTexture(GL_TEXTURE1);
    F.glBindTexture(GL_TEXTURE_2D, g->efb_depth);
    F.glBindSampler(1, 0);
    F.glUniform1i(g->cu_color, 0);
    F.glUniform1i(g->cu_depth, 1);
    float out[4] = {(float)ow, (float)oh, (float)g->scale, (float)GX_EFB_H};
    F.glUniform4fv(g->cu_src, 1, src);
    F.glUniform4fv(g->cu_out, 1, out);
    GLint m[4] = {mode, (GLint)fam, (GLint)s0, (GLint)s1};
    F.glUniform4iv(g->cu_mode, 1, m);
    unsigned c[7] = {BITS(bp[BP_COPY_FILTER0], 0, 6), BITS(bp[BP_COPY_FILTER0], 6, 6), BITS(bp[BP_COPY_FILTER0], 12, 6),
                     BITS(bp[BP_COPY_FILTER0], 18, 6), BITS(bp[BP_COPY_FILTER1], 0, 6), BITS(bp[BP_COPY_FILTER1], 6, 6),
                     BITS(bp[BP_COPY_FILTER1], 12, 6)};
    GLint filt[4] = {(GLint)(c[0] + c[1]), (GLint)(c[2] + c[3] + c[4]), (GLint)(c[5] + c[6]), 0};
    bool filter_on = !(filt[0] == 0 && filt[2] == 0 && filt[1] == 64);
    unsigned fmt = BITS(bp[BP_PE_CONTROL], 0, 3);
    GLint m2[4] = {depth ? 1 : 0, fmt == 1 ? 1 : 0, gamma, (mode <= 1 && filter_on) ? 1 : 0};
    F.glUniform4iv(g->cu_mode2, 1, m2);
    F.glUniform4iv(g->cu_filt, 1, filt);
    F.glBindVertexArray(g->empty_vao);
    F.glDrawArrays(GL_TRIANGLES, 0, 3);
    F.glActiveTexture(GL_TEXTURE0);
}

static void efb_clear_rect(gx_state *gx, int x, int y, int w, int h)
{
    gx_gl *g = gx->gl;
    const uint32_t *bp = gx->bp;
    efb_bind(g);
    scissor_rect(g, x, y, x + w, y + h);
    uint32_t cm0 = bp[BP_CMODE0];
    unsigned fmt = BITS(bp[BP_PE_CONTROL], 0, 3);
    int r = (int)BITS(bp[BP_CLEAR_AR], 0, 8), a = (int)BITS(bp[BP_CLEAR_AR], 8, 8);
    int gg = (int)BITS(bp[BP_CLEAR_GB], 8, 8), b = (int)BITS(bp[BP_CLEAR_GB], 0, 8);
    if (fmt == 1) { /* RGBA6 */
        r = (r >> 2) << 2 | r >> 6, gg = (gg >> 2) << 2 | gg >> 6, b = (b >> 2) << 2 | b >> 6, a = (a >> 2) << 2 | a >> 6;
    } else if (fmt == 2) {
        r = (r >> 3) << 3 | r >> 5, gg = (gg >> 2) << 2 | gg >> 6, b = (b >> 3) << 3 | b >> 5;
    }
    GLbitfield bits = 0;
    if (cm0 & 0x18) {
        F.glColorMask((cm0 & 8) ? GL_TRUE : GL_FALSE, (cm0 & 8) ? GL_TRUE : GL_FALSE, (cm0 & 8) ? GL_TRUE : GL_FALSE,
                      (cm0 & 0x10) ? GL_TRUE : GL_FALSE);
        F.glClearColor((float)r / 255.0f, (float)gg / 255.0f, (float)b / 255.0f, (float)a / 255.0f);
        bits |= GL_COLOR_BUFFER_BIT;
    }
    if (bp[BP_ZMODE] & 0x10) {
        F.glDepthMask(GL_TRUE);
        clear_depth(&g->f, (float)BITS(bp[BP_CLEAR_Z], 0, 24) / 16777215.0f);
        bits |= GL_DEPTH_BUFFER_BIT;
    }
    if (bits)
        F.glClear(bits);
    g->peek_color_ok = g->peek_z_ok = false;
}

static void xfb_keep(gx_state *gx, uint32_t addr, int x, int y, int w, int h)
{
    gx_gl *g = gx->gl;
    unsigned slot = 4;
    for (unsigned i = 0; i < 4; i++)
        if (g->xfb[i].tex && g->xfb[i].addr == addr)
            slot = i;
    if (slot == 4) {
        slot = g->xfb_next;
        g->xfb_next = (g->xfb_next + 1) & 3;
    }
    int s = g->scale;
    if (!g->xfb[slot].tex || g->xfb[slot].w != w * s || g->xfb[slot].h != h * s) {
        if (!g->xfb[slot].tex)
            g->xfb[slot].tex = new_tex(g);
        F.glBindTexture(GL_TEXTURE_2D, g->xfb[slot].tex);
        F.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w * s, h * s, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        F.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        F.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    g->xfb[slot].addr = addr;
    g->xfb[slot].w = w * s;
    g->xfb[slot].h = h * s;
    /* the picture top-down in the texture (row 0 = top), as the RAM XFB is */
    F.glBindFramebuffer(GL_READ_FRAMEBUFFER, g->efb_fbo);
    F.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g->copy_fbo);
    F.glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g->xfb[slot].tex, 0);
    F.glDisable(GL_SCISSOR_TEST);
    int gy0 = (GX_EFB_H - y - h) * s, gy1 = (GX_EFB_H - y) * s;
    F.glBlitFramebuffer(x * s, gy0, (x + w) * s, gy1, 0, h * s, w * s, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    F.glBindFramebuffer(GL_FRAMEBUFFER, g->efb_fbo);
}

static GLuint scratch_tex(gx_gl *g, int w, int h)
{
    static GLuint t;
    static int tw, th;
    if (!t)
        t = new_tex(g);
    F.glBindTexture(GL_TEXTURE_2D, t);
    if (tw != w || th != h) {
        F.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        tw = w, th = h;
    }
    return t;
}

void gx_gl_copy(gx_state *gx, uint32_t exec)
{
    gx_gl *g = gx->gl;
    const uint32_t *bp = gx->bp;
    int x = (int)BITS(bp[BP_COPY_SRC], 0, 10), y = (int)BITS(bp[BP_COPY_SRC], 10, 10);
    int w = (int)BITS(bp[BP_COPY_SIZE], 0, 10) + 1, h = (int)BITS(bp[BP_COPY_SIZE], 10, 10) + 1;
    uint32_t dst = (bp[BP_COPY_DST] & 0xFFFFFFu) << 5;
    uint32_t stride = BITS(bp[BP_COPY_STRIDE], 0, 10) * 32u;
    if (!g) {
        if (exec & 0x4000)
            gx->stats.xfb_copies++;
        else
            gx->stats.efb_copies++;
        return;
    }
    float src[4] = {(float)x, (float)y, (float)w, (float)h};
    if (exec & 0x4000) { /* to the XFB */
        int lines = h;
        if (exec & 0x400) {
            unsigned ys = BITS(bp[BP_COPY_YSCALE], 0, 9);
            if (ys)
                lines = (int)((((unsigned)h - 1) << 8) / ys) + 1;
        }
        if (lines > 1024)
            lines = 1024;
        int ow = (w + 1) / 2;
        GLuint t = scratch_tex(g, ow, lines);
        copy_pass(gx, t, ow, lines, src, 1, 0, 0, 0, false, (int)BITS(exec, 7, 2));
        size_t row = (size_t)ow * 4;
        uint8_t *buf = malloc(row * (size_t)lines);
        if (buf) {
            F.glPixelStorei(GL_PACK_ALIGNMENT, 1);
            F.glReadPixels(0, 0, ow, lines, GL_RGBA, GL_UNSIGNED_BYTE, buf);
            for (int r = 0; r < lines; r++) {
                uint32_t a = dst + (uint32_t)r * stride;
                if (a >= gx->ram_size)
                    break;
                size_t n = row;
                if (a + n > gx->ram_size)
                    n = gx->ram_size - a;
                memcpy(gx->ram + a, buf + (size_t)r * row, n);
            }
            free(buf);
        }
        xfb_keep(gx, dst, x, y, w, h);
        gx->stats.xfb_copies++;
    } else { /* to a texture */
        unsigned real = BITS(exec, 4, 3) | BITS(exec, 3, 1) << 3;
        bool depth = BITS(bp[BP_PE_CONTROL], 0, 3) == 3;
        bool half = exec & 0x200;
        unsigned fam, s0, s1;
        copy_format(real, (exec & 0x8000) != 0, depth, &fam, &s0, &s1);
        int ow = half ? w / 2 : w, oh = half ? h / 2 : h;
        if (ow < 1) ow = 1;
        if (oh < 1) oh = 1;
        uint32_t size = gx_tex_level_size(fam, (unsigned)ow, (unsigned)oh);
        if (dst < gx->ram_size && size <= gx->ram_size - dst) {
            if (g->efb_copy_ram) {
                GLuint t = scratch_tex(g, ow, oh);
                copy_pass(gx, t, ow, oh, src, 0, fam, s0, s1, depth, 0);
                uint8_t *buf = malloc((size_t)ow * oh * 4);
                if (buf) {
                    F.glPixelStorei(GL_PACK_ALIGNMENT, 1);
                    F.glReadPixels(0, 0, ow, oh, GL_RGBA, GL_UNSIGNED_BYTE, buf);
                    uint32_t bw = (fam == TF_I4 || fam == TF_I8 || fam == TF_IA4) ? 8 : 4;
                    uint32_t blk = fam == TF_RGBA8 ? 64 : 32;
                    uint32_t st = stride ? stride : (((uint32_t)ow + bw - 1) / bw) * blk;
                    uint32_t bh = fam == TF_I4 ? 8 : 4;
                    uint32_t need = st * (((uint32_t)oh + bh - 1) / bh);
                    if (need <= gx->ram_size - dst)
                        gx_tex_encode(fam, buf, (unsigned)ow, (unsigned)oh, gx->ram + dst, st);
                    free(buf);
                }
            }
            /* the cache entry at the internal resolution */
            tex_entry **b = &g->texs[(dst >> 5) & (TEX_BUCKETS - 1)], *e = NULL;
            for (tex_entry *q = *b; q; q = q->next)
                if (q->from_efb && q->addr == dst)
                    e = q;
            if (!e) {
                e = calloc(1, sizeof *e);
                if (e) {
                    e->from_efb = true;
                    e->addr = dst;
                    e->tex = new_tex(g);
                    e->next = *b;
                    *b = e;
                }
            }
            if (e) {
                int s = g->scale;
                F.glBindTexture(GL_TEXTURE_2D, e->tex);
                if ((int)e->gw != ow * s || (int)e->gh != oh * s)
                    F.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ow * s, oh * s, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
                F.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
                e->gw = (unsigned)(ow * s), e->gh = (unsigned)(oh * s);
                e->w = (unsigned)ow, e->h = (unsigned)oh, e->fmt = fam, e->levels = 1;
                copy_pass(gx, e->tex, ow * s, oh * s, src, 0, fam, s0, s1, depth, 0);
                e->size = size;
                e->hash = gx_hash(gx->ram + dst, size, 0);
                gx->tex_epoch++;
                e->epoch = gx->tex_epoch;
                e->last_field = gx->field_count;
            }
        }
        gx->stats.efb_copies++;
    }
    if (exec & 0x800)
        efb_clear_rect(gx, x, y, w, h);
    efb_bind(g);
}

/* ---- peeks and pokes ---- */

static void peek_fill(gx_state *gx, bool z)
{
    gx_gl *g = gx->gl;
    float src[4] = {0, 0, GX_EFB_W, GX_EFB_H};
    copy_pass(gx, g->peek_tex, GX_EFB_W, GX_EFB_H, src, z ? 2 : 3, 0, 0, 0, false, 0);
    F.glPixelStorei(GL_PACK_ALIGNMENT, 1);
    F.glReadPixels(0, 0, GX_EFB_W, GX_EFB_H, GL_RGBA, GL_UNSIGNED_BYTE, z ? g->peek_z : g->peek_color);
    if (z)
        g->peek_z_ok = true;
    else
        g->peek_color_ok = true;
    efb_bind(g);
}

uint32_t gx_efb_peek(gx_state *gx, uint32_t pa)
{
    gx_gl *g = gx->gl;
    gx->stats.peeks++;
    if (!g)
        return 0;
    gx_gl_flush(gx);
    unsigned x = BITS(pa, 2, 10), y = BITS(pa, 12, 10);
    bool z = pa & 0x400000;
    if (x >= GX_EFB_W || y >= GX_EFB_H)
        return 0;
    if (z ? !g->peek_z_ok : !g->peek_color_ok)
        peek_fill(gx, z);
    const uint8_t *p = (z ? g->peek_z : g->peek_color) + ((size_t)y * GX_EFB_W + x) * 4;
    if (g->trace)
        gx_log(gx, "peek %s (%u, %u) = %02X %02X %02X %02X", z ? "Z" : "colour", x, y, p[0], p[1], p[2], p[3]);
    if (z)
        return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
    unsigned fmt = BITS(gx->bp[BP_PE_CONTROL], 0, 3);
    uint32_t a = p[3];
    if (fmt != 1) {
        unsigned mode = gx->pe_reg[4] & 3;
        a = mode == 0 ? 0x00 : mode == 1 ? 0xFF : 0xFF;
    }
    return a << 24 | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
}

void gx_efb_poke(gx_state *gx, uint32_t pa, uint32_t v, unsigned size)
{
    gx_gl *g = gx->gl;
    gx->stats.pokes++;
    if (!g || size != 4)
        return;
    gx_gl_flush(gx);
    int x = (int)BITS(pa, 2, 10), y = (int)BITS(pa, 12, 10);
    efb_bind(g);
    scissor_rect(g, x, y, x + 1, y + 1);
    if (pa & 0x400000) {
        F.glDepthMask(GL_TRUE);
        clear_depth(&g->f, (float)(v & 0xFFFFFF) / 16777215.0f);
        F.glClear(GL_DEPTH_BUFFER_BIT);
        g->peek_z_ok = false;
    } else {
        F.glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        F.glClearColor((float)((v >> 16) & 255) / 255.0f, (float)((v >> 8) & 255) / 255.0f, (float)(v & 255) / 255.0f,
                       (float)(v >> 24) / 255.0f);
        F.glClear(GL_COLOR_BUFFER_BIT);
        g->peek_color_ok = false;
    }
}

/* ---- per field ---- */

void gx_gl_frame(gx_state *gx)
{
    gx_gl *g = gx->gl;
    if (!g)
        return;
    gx_gl_flush(gx);
    if (gx->field_count % 60 == 0) { /* drop textures unused for a while */
        for (unsigned i = 0; i < TEX_BUCKETS; i++) {
            tex_entry **pp = &g->texs[i];
            while (*pp) {
                tex_entry *e = *pp;
                if (gx->field_count - e->last_field > TEX_KEEP_FIELDS) {
                    *pp = e->next;
                    tex_free(g, e);
                } else {
                    pp = &e->next;
                }
            }
        }
    }
    uint64_t ready = 0, pending = 0;
    for (unsigned i = 0; i < PROG_BUCKETS; i++)
        for (prog_entry *e = g->progs[i]; e; e = e->next) {
            prog_poll(g, e);
            int st = __atomic_load_n(&e->state, __ATOMIC_ACQUIRE);
            if (st == P_READY)
                ready++;
            else if (st == P_PENDING || st == P_COMPILING)
                pending++;
        }
    gx->stats.shaders_ready = ready;
    gx->stats.shaders_pending = pending;
}

unsigned gx_xfb_texture(gx_state *gx, uint32_t addr, int *w, int *h)
{
    gx_gl *g = gx->gl;
    if (!g)
        return 0;
    addr &= 0x3FFFFFFFu;
    for (unsigned i = 0; i < 4; i++)
        if (g->xfb[i].tex && g->xfb[i].addr == addr) {
            *w = g->xfb[i].w;
            *h = g->xfb[i].h;
            return g->xfb[i].tex;
        }
    return 0;
}

/* ---- attach / detach ---- */

static int env_int(const char *name, int def)
{
    const char *v = getenv(name);
    return v && *v ? atoi(v) : def;
}

bool gx_gl_attached(const gx_state *gx)
{
    return gx->gl != NULL;
}

int gx_gl_attach(gx_state *gx, const gx_gl_options *opt)
{
    if (gx->gl)
        return 0;
    gx_gl *g = calloc(1, sizeof *g);
    if (!g)
        return -1;
    if (gx_egl_load(&g->egl) || gx_gl_load(&g->f, &g->egl)) {
        fprintf(stderr, "gx: no GLES 3 entry points: GX runs without drawing\n");
        free(g);
        return -1;
    }
    if (!g->egl.eglGetCurrentContext() && !g->f.glGetString(GL_VERSION)) {
        fprintf(stderr, "gx: no current GL context: GX runs without drawing\n");
        free(g);
        return -1;
    }
    const char *ver = (const char *)g->f.glGetString(GL_VERSION);
    int major = 0, minor = 0;
    desktop_gl = ver && !strstr(ver, "OpenGL ES") && sscanf(ver, "%d.%d", &major, &minor) == 2 &&
                 (major > 3 || (major == 3 && minor >= 3));
    if (!ver || (!strstr(ver, "OpenGL ES 3") && !desktop_gl)) {
        fprintf(stderr, "gx: needs OpenGL ES 3 or GL 3.3, the context is %s: GX runs without drawing\n",
                ver ? ver : "?");
        free(g);
        return -1;
    }
    int scale = opt && opt->scale ? opt->scale : env_int("RECOMP_GC_UPSCALE", env_int("RECOMP_UPSCALE", 1));
    if (scale < 1) scale = 1;
    if (scale > 8) scale = 8;
    g->f.glGetIntegerv(GL_MAX_TEXTURE_SIZE, &g->max_tex);
    while (scale > 1 && GX_EFB_W * scale > g->max_tex)
        scale--;
    g->scale = scale;
    g->async = opt ? opt->async_shaders : 1;
    if (env_int("GC_GX_ASYNC", 99) != 99)
        g->async = env_int("GC_GX_ASYNC", 1);
    g->efb_copy_ram = opt && opt->efb_copy_ram ? 1 : env_int("GC_GX_EFB_COPY_RAM", 0);
    g->trace = env_int("GC_GX_TRACE", 0);
    const char *list = opt && opt->shader_list ? opt->shader_list : getenv("GC_GX_SHADER_LIST");
    if (list && *list)
        g->list_path = strdup(list);
    g->bv = malloc(BATCH_V * sizeof *g->bv);
    g->packed = malloc(BATCH_V * sizeof *g->bv);
    g->bi = malloc(BATCH_I * sizeof *g->bi);
    g->peek_color = malloc(GX_EFB_W * GX_EFB_H * 4);
    g->peek_z = malloc(GX_EFB_W * GX_EFB_H * 4);
    if (!g->bv || !g->packed || !g->bi || !g->peek_color || !g->peek_z) {
        free(g->bv), free(g->packed), free(g->bi), free(g->peek_color), free(g->peek_z), free(g);
        return -1;
    }
    gx->gl = g;
    /* EFB */
    int W = GX_EFB_W * scale, H = GX_EFB_H * scale;
    g->efb_color = new_tex(g);
    F.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    F.glGenTextures(1, &g->efb_depth);
    F.glBindTexture(GL_TEXTURE_2D, g->efb_depth);
    F.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    F.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    F.glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, W, H, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
    F.glGenFramebuffers(1, &g->efb_fbo);
    F.glBindFramebuffer(GL_FRAMEBUFFER, g->efb_fbo);
    F.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g->efb_color, 0);
    F.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, g->efb_depth, 0);
    if (F.glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "gx: EFB framebuffer incomplete: GX runs without drawing\n");
        gx_gl_detach(gx);
        return -1;
    }
    F.glDisable(GL_DITHER);
    F.glDisable(GL_SCISSOR_TEST);
    F.glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    F.glDepthMask(GL_TRUE);
    F.glClearColor(0, 0, 0, 0);
    clear_depth(&g->f, 1.0f);
    F.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    F.glFrontFace(GL_CCW);
    F.glGenFramebuffers(1, &g->copy_fbo);
    g->peek_tex = new_tex(g);
    F.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GX_EFB_W, GX_EFB_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    static const uint8_t white[4] = {255, 255, 255, 255};
    g->white_tex = new_tex(g);
    F.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    F.glGenSamplers(8, g->samplers);
    /* vertex layout */
    F.glGenVertexArrays(1, &g->vao);
    F.glGenVertexArrays(1, &g->empty_vao);
    F.glGenBuffers(1, &g->vbo);
    F.glGenBuffers(1, &g->ebo);
    F.glGenBuffers(1, &g->ubo);
    F.glBindVertexArray(g->vao);
    F.glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
    GLsizei st = (GLsizei)sizeof(gx_vout);
    F.glEnableVertexAttribArray(0);
    F.glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, st, (const void *)offsetof(gx_vout, pos));
    F.glEnableVertexAttribArray(1);
    F.glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, st, (const void *)offsetof(gx_vout, c0));
    F.glEnableVertexAttribArray(2);
    F.glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, st, (const void *)offsetof(gx_vout, c1));
    for (int i = 0; i < 8; i++) {
        F.glEnableVertexAttribArray((GLuint)(3 + i));
        F.glVertexAttribPointer((GLuint)(3 + i), 3, GL_FLOAT, GL_FALSE, st,
                                (const void *)(offsetof(gx_vout, tex) + 12 * (size_t)i));
    }
    F.glBindVertexArray(0);
    /* programs */
    g->uber = build_program(&g->f, NULL);
    char log[2048] = "";
    GLuint cvs = compile_shader(&g->f, GL_VERTEX_SHADER, copy_vs, log, sizeof log);
    GLuint cfs = cvs ? compile_shader(&g->f, GL_FRAGMENT_SHADER, copy_fs, log, sizeof log) : 0;
    g->copy_prog = cfs ? link_program(&g->f, cvs, cfs, log, sizeof log) : 0;
    if (cvs)
        F.glDeleteShader(cvs);
    if (cfs)
        F.glDeleteShader(cfs);
    if (!g->uber || !g->copy_prog) {
        if (!g->copy_prog)
            fprintf(stderr, "gx: copy shader failed: %s\n", log);
        fprintf(stderr, "gx: renderer shaders failed: GX runs without drawing\n");
        gx_gl_detach(gx);
        return -1;
    }
    g->cu_src = F.glGetUniformLocation(g->copy_prog, "u_src");
    g->cu_out = F.glGetUniformLocation(g->copy_prog, "u_out");
    g->cu_mode = F.glGetUniformLocation(g->copy_prog, "u_mode");
    g->cu_mode2 = F.glGetUniformLocation(g->copy_prog, "u_mode2");
    g->cu_filt = F.glGetUniformLocation(g->copy_prog, "u_filt");
    g->cu_color = F.glGetUniformLocation(g->copy_prog, "u_color");
    g->cu_depth = F.glGetUniformLocation(g->copy_prog, "u_depth");
    /* async compilation */
    if (g->async == 1) {
        worker_start(g);
        if (!g->worker_ok) {
            bool khr = false;
            GLint n = 0;
            F.glGetIntegerv(GL_NUM_EXTENSIONS, &n);
            for (GLint i = 0; i < n; i++) {
                const char *e = (const char *)F.glGetStringi(GL_EXTENSIONS, (GLuint)i);
                if (e && !strcmp(e, "GL_KHR_parallel_shader_compile"))
                    khr = true;
            }
            g->khr_parallel = khr && g->f.glMaxShaderCompilerThreadsKHR;
            if (g->khr_parallel)
                g->f.glMaxShaderCompilerThreadsKHR(0xFFFFFFFFu);
            else
                g->async = 0;
        }
    }
    fprintf(stderr, "gx: GLES renderer on %s (%s), %dx internal resolution, shaders %s, EFB copies %s\n",
            (const char *)F.glGetString(GL_RENDERER), ver, g->scale,
            g->async == 1 ? (g->worker_ok ? "async (worker thread)" : "async (KHR_parallel_shader_compile)")
            : g->async == -1 ? "ubershader only" : "on first use",
            g->efb_copy_ram ? "to texture and RAM" : "to texture");
    /* the per-title list: compile ahead what earlier runs met */
    if (g->list_path) {
        gx_shader_key *keys;
        size_t n;
        if (!gx_shader_list_read(g->list_path, &keys, &n)) {
            for (size_t i = 0; i < n; i++)
                if (g->async != -1)
                    prog_lookup(g, &keys[i], true);
                else
                    record_key(g, &keys[i]);
            free(keys);
            g->list_dirty = false;
            fprintf(stderr, "gx: shader list %s: %zu states queued\n", g->list_path, n);
        }
    }
    efb_bind(g);
    return 0;
}

void gx_save_shader_list(gx_state *gx)
{
    gx_gl *g = gx ? gx->gl : NULL;
    if (!g || !g->list_path || !g->list_dirty)
        return;
    if (gx_shader_list_write(g->list_path, g->keys, g->nkeys, (const uint32_t(*)[5])g->vfmts, g->nvfmts)) {
        fprintf(stderr, "gx: cannot write the shader list %s\n", g->list_path);
        return;
    }
    g->list_dirty = false;
    fprintf(stderr, "gx: shader list %s: %zu TEV states, %zu vertex formats\n", g->list_path, g->nkeys, g->nvfmts);
}

void gx_gl_detach(gx_state *gx)
{
    gx_gl *g = gx->gl;
    if (!g)
        return;
    gx_gl_flush(gx);
    worker_stop(g);
    gx_save_shader_list(gx);
    for (unsigned i = 0; i < PROG_BUCKETS; i++)
        while (g->progs[i]) {
            prog_entry *e = g->progs[i];
            g->progs[i] = e->next;
            if (e->prog)
                F.glDeleteProgram(e->prog);
            free(e);
        }
    for (unsigned i = 0; i < TEX_BUCKETS; i++)
        while (g->texs[i]) {
            tex_entry *e = g->texs[i];
            g->texs[i] = e->next;
            tex_free(g, e);
        }
    for (unsigned i = 0; i < 4; i++)
        if (g->xfb[i].tex)
            F.glDeleteTextures(1, &g->xfb[i].tex);
    if (g->uber)
        F.glDeleteProgram(g->uber);
    if (g->copy_prog)
        F.glDeleteProgram(g->copy_prog);
    F.glDeleteFramebuffers(1, &g->efb_fbo);
    F.glDeleteFramebuffers(1, &g->copy_fbo);
    F.glDeleteTextures(1, &g->efb_color);
    F.glDeleteTextures(1, &g->efb_depth);
    F.glDeleteTextures(1, &g->peek_tex);
    F.glDeleteTextures(1, &g->white_tex);
    F.glDeleteSamplers(8, g->samplers);
    F.glDeleteBuffers(1, &g->vbo);
    F.glDeleteBuffers(1, &g->ebo);
    F.glDeleteBuffers(1, &g->ubo);
    F.glDeleteVertexArrays(1, &g->vao);
    F.glDeleteVertexArrays(1, &g->empty_vao);
    free(g->bv);
    free(g->packed);
    free(g->bi);
    free(g->peek_color);
    free(g->peek_z);
    free(g->decode_buf);
    free(g->keys);
    free(g->vfmts);
    free(g->list_path);
    free(g);
    gx->gl = NULL;
}
