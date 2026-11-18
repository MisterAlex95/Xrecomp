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
 * compiled ahead. Program binaries (XBA-264, gx_progcache.h): the programs of the title's cache file, when it is of
 * this driver, are linked with glProgramBinary at attach (ready before the first frame); programs compiled later are
 * added to it at a pause and at detach. gx_gl_warmup fills it ahead from the lists (the app's warm-up after Prepare).
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
#include <time.h>

#include "gx_glapi.h"
#include "gx_internal.h"
#include "gx_progcache.h"
#include "gx_ring.h"
#include "gx_shader.h"

#define BATCH_V (1u << 18)
#define BATCH_I (3u * BATCH_V)
#define PROG_BUCKETS 1024u
#define TEX_BUCKETS 4096u
#define TEX_KEEP_FIELDS 1200u
#define XFB_DIAG_LINES 128u  /* lines waiting for the log thread */
#define XFB_DIAG_LINE 400u   /* well under the Android log pump's 1024-byte line */

enum { P_PENDING = 0, P_READY = 1, P_FAILED = 2, P_COMPILING = 3 };

/* RECOMP_GC_GX_XFB_COPY: how the EFB reaches an XFB slot's texture. */
enum { XFB_COPY_BLIT = 0, XFB_COPY_FLUSH = 1, XFB_COPY_FBO = 2, XFB_COPY_DRAW = 3 };

/* RECOMP_GC_GX_STREAM: how draws reach the vertex, index and uniform buffers (ring_put). */
enum { STREAM_MAP = 0, STREAM_MAPINV = 1, STREAM_SUB = 2, STREAM_STORAGE = 3 };
static const char *const stream_names[] = {"map", "mapinv", "sub", "storage"};
#define STREAM_LOG_FIELDS 3000u /* a line of storage statistics this often */

typedef struct prog_entry {
    gx_shader_key key;
    uint64_t hash;
    GLuint prog;
    GLuint vs, fs;           /* KHR_parallel_shader_compile: still attached */
    int state;               /* P_*, atomic */
    bool in_cache;           /* its binary is in the program cache (loaded from it, or added) */
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

/* What a sampler object was last told (bind_map). The objects are the renderer's alone, so this stays true
 * whatever else happens to the context; which sampler is bound to a unit does not, and is set every time. */
typedef struct sampler_params {
    GLint wrap_s, wrap_t, mag, min;
    uint32_t lods;           /* TX_MODE1: minimum and maximum LOD */
    uint32_t set;
} sampler_params;

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
    int scale, async, efb_copy_ram, trace, xfb_ram;
    char *list_path;
    GLint max_tex;

    GLuint efb_fbo, efb_color, efb_depth;
    GLuint vao, vbo, ebo, ubo, empty_vao;
    size_t ring_size[3], ring_off[3]; /* vbo, ebo, ubo as streaming rings (ring_put) */
    GLint ubo_align;
    int stream; /* STREAM_* */
    /* STREAM_STORAGE: the three rings, mapped for their whole life */
    gx_ring sr[3];
    uint8_t *sr_map[3];
    GLuint sr_big[3];     /* an upload too large for its ring: a buffer of its own until the draw is issued */
    EGLContext sr_ctx;    /* the context the rings live in */
    uint64_t sr_waits, sr_wait_ns, sr_wait_max_ns, sr_oversize;
    bool sr_wait_warned;
    GLuint samplers[8];
    sampler_params sampler_set[8];
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

    struct {
        uint32_t addr, stride, lines;
        GLuint tex;
        int w, h;
        GLuint fbo;        /* RECOMP_GC_GX_XFB_COPY=fbo: this slot's own framebuffer, its texture attached once */
        GLuint diag_fbo;   /* RECOMP_GC_GX_XFB_DIAG=2: read through this one, so that copy_fbo is left alone */
        uint32_t copies;   /* copies made into the slot */
        uint32_t copy_sum; /* RECOMP_GC_GX_XFB_DIAG=2: the texture's checksum right after the last copy */
    } xfb[4];
    unsigned xfb_next;
    int xfb_copy;          /* XFB_COPY_* */
    GLuint copy_att;       /* the texture attached to copy_fbo now */
    GLuint xfb_draw_prog;  /* XFB_COPY_DRAW */
    GLint xd_color, xd_off;

    /* RECOMP_GC_GX_XFB_DIAG: 0 off, 1 a line per field, 2 also the slots' checksums. The lines are written by a
     * thread of their own: the render thread never waits for the log pipe. */
    int xfb_diag;
    uint8_t *diag_px;
    size_t diag_cap;
    uint64_t diag_last_ns;
    bool diag_thread_ok, diag_quit;
    pthread_t diag_thread;
    pthread_mutex_t diag_mu;
    pthread_cond_t diag_cv;
    char (*diag_lines)[XFB_DIAG_LINE];
    unsigned diag_head, diag_count, diag_dropped;

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

    /* program binaries (XBA-264) */
    char *pc_path;
    gx_progcache pc;
    bool pc_dirty;
};

#define F (g->f)

/* A desktop GL 3.3+ core context (gc_play's window on Linux / macOS) instead of GLES 3: the same GLSL under
 * #version 330 core. */
static int desktop_gl;

/* GLES 3.x, or desktop GL 3.3+ (sets desktop_gl). 0 when the context is neither. */
static int gl_flavour_ok(const char *ver)
{
    int major = 0, minor = 0;
    desktop_gl = ver && !strstr(ver, "OpenGL ES") && sscanf(ver, "%d.%d", &major, &minor) == 2 &&
                 (major > 3 || (major == 3 && minor >= 3));
    return ver && (strstr(ver, "OpenGL ES 3") || desktop_gl);
}

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

/* A new entry for k (recorded in the per-title list), its program not started. */
static prog_entry *prog_new(gx_gl *g, const gx_shader_key *k, uint64_t h)
{
    prog_entry **b = &g->progs[h & (PROG_BUCKETS - 1)];
    prog_entry *e = calloc(1, sizeof *e);
    if (!e)
        return NULL;
    e->key = *k;
    e->hash = h;
    e->next = *b;
    *b = e;
    record_key(g, k);
    return e;
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
    prog_entry *e = prog_new(g, k, h);
    if (!e)
        return NULL;
    if (g->trace)
        fprintf(stderr, "gx: new TEV state %016llX: %u stage(s), %u indirect, alpha test %06X, fog %X, ztex %X, pe %X\n",
                (unsigned long long)h, k->nst, k->nind, k->alpha_cmp, k->fog, k->ztex, k->pe);
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

/* ---- program binaries (XBA-264) ---- */

static void gl_clear_errors(gx_glfn *f)
{
    for (int i = 0; i < 16 && f->glGetError() != GL_NO_ERROR; i++) {
    }
}

static bool binaries_supported(gx_glfn *f)
{
    if (!f->glGetProgramBinary || !f->glProgramBinary || !f->glProgramParameteri)
        return false;
    GLint n = 0;
    f->glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &n);
    gl_clear_errors(f);
    return n > 0;
}

static const char *gl_str(gx_glfn *f, GLenum e)
{
    const char *s = (const char *)f->glGetString(e);
    return s ? s : "?";
}

/* The cache key and driver text of the current context. */
static uint64_t driver_key(gx_glfn *f, char *text, size_t n)
{
    snprintf(text, n, "%s | %s | %s", gl_str(f, GL_VENDOR), gl_str(f, GL_RENDERER), gl_str(f, GL_VERSION));
    return gx_progcache_key(gl_str(f, GL_VENDOR), gl_str(f, GL_RENDERER), gl_str(f, GL_VERSION), desktop_gl);
}

/* A program from a cached binary, set up; 0 when the driver refuses it. */
static GLuint program_from_binary(gx_glfn *f, const gx_progbin *b)
{
    if (b->key.nst < 1 || b->key.nst > 16 || b->key.nind > 4)
        return 0;
    GLuint p = f->glCreateProgram();
    f->glProgramBinary(p, b->format, b->data, (GLsizei)b->len);
    GLint ok = 0;
    f->glGetProgramiv(p, GL_LINK_STATUS, &ok);
    gl_clear_errors(f);
    if (!ok) {
        f->glDeleteProgram(p);
        return 0;
    }
    program_setup(f, p);
    return p;
}

/* p's binary into the cache under k. 0, or -1 (no binary). */
static int cache_program(gx_glfn *f, gx_progcache *c, const gx_shader_key *k, GLuint p)
{
    GLint len = 0;
    gl_clear_errors(f);
    f->glGetProgramiv(p, GL_PROGRAM_BINARY_LENGTH, &len);
    if (len <= 0 || (uint32_t)len > GX_PROGCACHE_MAX_BINARY)
        return -1;
    void *data = malloc((size_t)len);
    if (!data)
        return -1;
    GLsizei got = 0;
    GLenum fmt = 0;
    f->glGetProgramBinary(p, len, &got, &fmt, data);
    int rc = got > 0 && f->glGetError() == GL_NO_ERROR
                 ? gx_progcache_put(c, k, gx_progcache_src_hash(k), fmt, data, (uint32_t)got)
                 : -1;
    free(data);
    return rc;
}

/* At attach: the cache file's programs of this driver linked from their binaries, the others compiled again. */
static void progcache_attach(gx_gl *g)
{
    char drv[512], was[512];
    const char *ren = gl_str(&g->f, GL_RENDERER), *ver = gl_str(&g->f, GL_VERSION);
    if (g->async == -1 || !binaries_supported(&g->f)) {
        if (g->async != -1)
            fprintf(stderr, "gx: program binaries %s: ignored (the driver has none; driver %s %s)\n", g->pc_path, ren,
                    ver);
        free(g->pc_path);
        g->pc_path = NULL;
        return;
    }
    uint64_t key = driver_key(&g->f, drv, sizeof drv);
    const char *why = NULL;
    if (gx_progcache_load(g->pc_path, key, drv, &g->pc, &why, was, sizeof was)) {
        if (!strcmp(why, "none"))
            fprintf(stderr, "gx: program binaries %s: none yet (driver %s %s)\n", g->pc_path, ren, ver);
        else
            fprintf(stderr, "gx: program binaries %s: ignored (%s%s%s), rebuilt from scratch (driver %s %s)\n",
                    g->pc_path, why, was[0] ? "; was " : "", was, ren, ver);
        g->pc_dirty = strcmp(why, "none") != 0;
        return;
    }
    size_t loaded = 0, rebuilt = 0;
    for (size_t i = 0; i < g->pc.n;) {
        gx_progbin *b = &g->pc.e[i];
        GLuint p = gx_progcache_src_hash(&b->key) == b->src_hash ? program_from_binary(&g->f, b) : 0;
        gx_shader_key k = b->key;
        if (!p) {
            gx_progcache_drop(&g->pc, i);
            g->pc_dirty = true;
            rebuilt++;
            if (k.nst >= 1 && k.nst <= 16 && k.nind <= 4)
                prog_lookup(g, &k, true); /* compiled the usual way, added back at the next save */
            continue;
        }
        prog_entry *e = prog_lookup(g, &k, false);
        if (!e)
            e = prog_new(g, &k, gx_shader_key_hash(&k));
        if (!e || e->prog) {
            g->f.glDeleteProgram(p);
            i++;
            continue;
        }
        e->prog = p;
        e->in_cache = true;
        e->state = P_READY;
        loaded++;
        i++;
    }
    fprintf(stderr, "gx: program binaries %s: %zu loaded, %zu rebuilt (driver %s %s)\n", g->pc_path, loaded, rebuilt,
            ren, ver);
}

void gx_save_program_cache(gx_state *gx)
{
    gx_gl *g = gx ? gx->gl : NULL;
    if (!g || !g->pc_path || !F.glGetString(GL_VERSION)) /* no current context (Android while paused): later */
        return;
    size_t added = 0;
    for (unsigned i = 0; i < PROG_BUCKETS; i++)
        for (prog_entry *e = g->progs[i]; e; e = e->next) {
            if (e->in_cache || __atomic_load_n(&e->state, __ATOMIC_ACQUIRE) != P_READY || !e->prog)
                continue;
            e->in_cache = true; /* tried once: a program without a binary is not asked again */
            if (!cache_program(&g->f, &g->pc, &e->key, e->prog))
                added++;
        }
    if (!added && !g->pc_dirty)
        return;
    if (gx_progcache_save(g->pc_path, &g->pc)) {
        fprintf(stderr, "gx: cannot write the program binaries %s\n", g->pc_path);
        return;
    }
    g->pc_dirty = false;
    fprintf(stderr, "gx: program binaries %s: %zu programs written (%zu new)\n", g->pc_path, g->pc.n, added);
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
    /* one level until an upload says otherwise: Adreno crashes in a draw walking the mip chain GL's default of 1000 announces */
    F.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
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

/* gx_hash of texture bytes, counted for the statistics (per field: gx_field). */
static uint64_t tex_hash(gx_state *gx, const uint8_t *p, size_t n, uint64_t seed)
{
    gx->stats.tex_hash_bytes += n;
    return gx_hash(p, n, seed);
}

/* The GL texture for map n as the registers describe it now; *info: those registers, decoded. */
static GLuint tex_for(gx_state *gx, unsigned map, gx_texinfo *info)
{
    gx_gl *g = gx->gl;
    gx_tex_info(gx, map, info);
    const gx_texinfo ti = *info;
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
                if (tex_hash(gx, gx->ram + e->addr, e->size, 0) != e->hash)
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
        th_hash = tex_hash(gx, gx->tmem + a, tb, ti.tlut_fmt);
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
    uint64_t h = tex_hash(gx, gx->ram + ti.addr, ti.size, ti.fmt);
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
    gx_texinfo ti;
    GLuint t = tex_for(gx, map, &ti);
    F.glActiveTexture(GL_TEXTURE0 + map);
    F.glBindTexture(GL_TEXTURE_2D, t);
    uint32_t m0 = gx->bp[bp_tex_reg(BP_TX_MODE0, map)], m1 = gx->bp[bp_tex_reg(BP_TX_MODE1, map)];
    GLuint s = g->samplers[map];
    unsigned mf = BITS(m0, 5, 3);
    GLint min;
    if (ti.levels <= 1 || t == g->white_tex)
        min = (mf & 4) ? GL_LINEAR : GL_NEAREST;
    else {
        static const GLint mins[8] = {GL_NEAREST, GL_NEAREST_MIPMAP_NEAREST, GL_NEAREST_MIPMAP_LINEAR, GL_NEAREST,
                                      GL_LINEAR, GL_LINEAR_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR};
        min = mins[mf];
    }
    const sampler_params want = {gl_wrap(BITS(m0, 0, 2)), gl_wrap(BITS(m0, 2, 2)),
                                 (m0 & 0x10) ? GL_LINEAR : GL_NEAREST, min, m1 & 0xFFFFu, 1};
    if (memcmp(&g->sampler_set[map], &want, sizeof want)) {
        F.glSamplerParameteri(s, GL_TEXTURE_WRAP_S, want.wrap_s);
        F.glSamplerParameteri(s, GL_TEXTURE_WRAP_T, want.wrap_t);
        F.glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, want.mag);
        F.glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, want.min);
        F.glSamplerParameterf(s, GL_TEXTURE_MIN_LOD, (float)BITS(m1, 0, 8) / 16.0f);
        F.glSamplerParameterf(s, GL_TEXTURE_MAX_LOD, (float)BITS(m1, 8, 8) / 16.0f);
        g->sampler_set[map] = want;
    }
    F.glBindSampler(map, s);
    float *d = g->ubo_data.texdim[map];
    d[0] = (float)ti.w;
    d[1] = (float)ti.h;
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
    /* A GX front face is clockwise on screen and the EFB is upright in GL, so with GL's front = counter-clockwise a
     * GX back face is a GL front face: genmode 1 (cull back) removes GL_FRONT, 2 (cull front) GL_BACK. */
    unsigned cull = BITS(bp[BP_GENMODE], 14, 2);
    F.glFrontFace(GL_CCW);
    if (tri && cull) {
        F.glEnable(GL_CULL_FACE);
        F.glCullFace(cull == 1 ? GL_FRONT : cull == 2 ? GL_BACK : GL_FRONT_AND_BACK);
    } else {
        F.glDisable(GL_CULL_FACE);
    }
}

static int sext11(uint32_t v)
{
    v &= 0x7FF;
    return v >= 0x400 ? (int)v - 0x800 : (int)v;
}

/* ---- RECOMP_GC_GX_STREAM=storage: persistently mapped rings (bookkeeping: gx_ring.h) ---- */

static uint64_t stream_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static void *sr_fence(void *user)
{
    gx_gl *g = user;
    GLsync s = F.glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (!s)
        F.glFinish(); /* no fence to wait for later: the region is complete when this returns */
    return s;
}

/* Returns once the GPU has passed the fence. A wait that fails, or that lasts absurdly long (a lost context need not
 * ever signal), ends in glFinish, which also means every earlier command is complete. */
static void sr_retire(void *user, void *sync)
{
    gx_gl *g = user;
    GLsync s = sync;
    if (!s)
        return;
    GLenum r = F.glClientWaitSync(s, 0, 0);
    if (r == GL_TIMEOUT_EXPIRED) {
        uint64_t t0 = stream_now_ns(), dt = 0;
        do {
            r = F.glClientWaitSync(s, GL_SYNC_FLUSH_COMMANDS_BIT, 100000000u);
            dt = stream_now_ns() - t0;
        } while (r == GL_TIMEOUT_EXPIRED && dt < 10000000000u);
        g->sr_waits++;
        g->sr_wait_ns += dt;
        if (dt > g->sr_wait_max_ns)
            g->sr_wait_max_ns = dt;
    }
    if (r != GL_ALREADY_SIGNALED && r != GL_CONDITION_SATISFIED) {
        if (!g->sr_wait_warned)
            fprintf(stderr, "gx: buffer streaming: fence wait ended with 0x%X, finishing the GPU's work instead\n", r);
        g->sr_wait_warned = true;
        F.glFinish();
    }
    F.glDeleteSync(s);
}

static void sr_discard(void *user, void *sync)
{
    gx_gl *g = user;
    if (sync)
        F.glDeleteSync((GLsync)sync);
}

static const gx_ring_ops sr_ops = {sr_fence, sr_retire, sr_discard};

static bool has_extension(gx_glfn *f, const char *name)
{
    GLint n = 0;
    f->glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; i++) {
        const char *e = (const char *)f->glGetStringi(GL_EXTENSIONS, (GLuint)i);
        if (e && !strcmp(e, name))
            return true;
    }
    return false;
}

/* Unmaps and deletes the rings (their buffers stay to the caller). Nothing is waited for: GL keeps a deleted buffer
 * until the GPU is done with it. In another context than the rings' none of these names mean anything, so only the
 * pointers are dropped. */
static void stream_storage_destroy(gx_gl *g, const GLuint *bufs)
{
    bool same_ctx = g->egl.eglGetCurrentContext() == g->sr_ctx;
    for (unsigned r = 0; r < 3; r++) {
        if (same_ctx) {
            gx_ring_release(&g->sr[r], &sr_ops, g);
            if (g->sr_big[r])
                F.glDeleteBuffers(1, &g->sr_big[r]);
            if (g->sr_map[r]) {
                F.glBindBuffer(GL_COPY_WRITE_BUFFER, bufs[r]);
                F.glUnmapBuffer(GL_COPY_WRITE_BUFFER);
            }
        }
        g->sr_big[r] = 0;
        g->sr_map[r] = NULL;
        gx_ring_init(&g->sr[r], 0);
    }
    if (same_ctx)
        F.glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
}

/* Allocates and maps the three rings once. NULL, or why they cannot be used (nothing is left allocated, but the
 * buffer names must be replaced: storage may have been made immutable). */
static const char *stream_storage_create(gx_gl *g)
{
    if (!has_extension(&g->f, "GL_EXT_buffer_storage"))
        return "no GL_EXT_buffer_storage";
    if (!F.glBufferStorageEXT)
        return "no glBufferStorageEXT entry point";
    static const size_t sizes[3] = {32u << 20, 16u << 20, 8u << 20};
    int kb = 0; /* GC_GX_STREAM_RING_KB: tests, small rings wrap all the time */
    const char *kbs = getenv("GC_GX_STREAM_RING_KB");
    if (kbs && *kbs)
        kb = atoi(kbs);
    const GLuint bufs[3] = {g->vbo, g->ebo, g->ubo};
    const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT_EXT | GL_MAP_COHERENT_BIT_EXT;
    const char *why = NULL;
    g->sr_ctx = g->egl.eglGetCurrentContext();
    gl_clear_errors(&g->f);
    for (unsigned r = 0; r < 3 && !why; r++) {
        size_t size = kb > 0 ? (size_t)(kb < 64 ? 64 : kb) << 10 : sizes[r];
        F.glBindBuffer(GL_COPY_WRITE_BUFFER, bufs[r]);
        F.glBufferStorageEXT(GL_COPY_WRITE_BUFFER, (GLsizeiptr)size, NULL, flags);
        if (F.glGetError() != GL_NO_ERROR) {
            why = "glBufferStorageEXT failed";
            break;
        }
        g->sr_map[r] = F.glMapBufferRange(GL_COPY_WRITE_BUFFER, 0, (GLsizeiptr)size, flags);
        if (!g->sr_map[r]) {
            why = "the persistent map failed";
            break;
        }
        gx_ring_init(&g->sr[r], size);
    }
    if (why) {
        stream_storage_destroy(g, bufs);
        gl_clear_errors(&g->f);
    }
    F.glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
    return why;
}

static void stream_storage_log(const gx_gl *g)
{
    fprintf(stderr, "gx: stream storage: %llu fence waits (%.1f ms in all, longest %.1f ms), wraps vbo %llu ebo %llu "
                    "ubo %llu, %llu uploads too large for a ring\n",
            (unsigned long long)g->sr_waits, (double)g->sr_wait_ns / 1e6, (double)g->sr_wait_max_ns / 1e6,
            (unsigned long long)g->sr[0].wraps, (unsigned long long)g->sr[1].wraps,
            (unsigned long long)g->sr[2].wraps, (unsigned long long)g->sr_oversize);
}

/* The draw reading this flush's uploads is issued: fences for the rings that advanced far enough, and the buffers of
 * oversized uploads go (GL keeps their storage until the GPU has read it). */
static void stream_after_draw(gx_state *gx)
{
    gx_gl *g = gx->gl;
    gx->stats.stream_storage = g->stream == STREAM_STORAGE;
    if (g->stream != STREAM_STORAGE)
        return;
    for (unsigned r = 0; r < 3; r++) {
        gx->stats.stream_wraps[r] = g->sr[r].wraps;
        gx_ring_after_draw(&g->sr[r], &sr_ops, g);
        if (g->sr_big[r]) {
            F.glDeleteBuffers(1, &g->sr_big[r]);
            g->sr_big[r] = 0;
        }
    }
    gx->stats.stream_waits = g->sr_waits;
    gx->stats.stream_wait_us = g->sr_wait_ns / 1000u;
    gx->stats.stream_oversize = g->sr_oversize;
}

/* Streaming buffers (vbo, ebo, ubo). STREAM_STORAGE: a copy into the ring's persistent mapping, no GL call for the data
 * at all, behind fences (gx_ring.h). The other modes: each upload goes after the previous one in a buffer that keeps
 * its storage, and only a wrap orphans it (glBufferData once). A glBufferData of a new size at every draw made Adreno
 * drivers allocate GPU memory per draw (an ioctl each): Prince of Persia: The Two Thrones fell to 13 % in 3D scenes on
 * the Fold8. The ranges written after an orphan were never handed to the GPU, so they are mapped unsynchronised, without
 * GL_MAP_INVALIDATE_RANGE_BIT: with it, the Fold8's Adreno driver still allocated and copied on every map (2 % in the
 * same scene, 83 % without). RECOMP_GC_GX_STREAM=mapinv / sub keep the other ways for comparison. Returns the offset. */
static size_t ring_put(gx_gl *g, unsigned r, GLuint buf, GLenum target, const void *data, size_t n, size_t align)
{
    if (g->stream == STREAM_STORAGE && g->egl.eglGetCurrentContext() != g->sr_ctx) {
        /* The mappings died with their context: never write through them. Plain buffers from here on. */
        fprintf(stderr, "gx: buffer streaming: the GL context changed, storage rings dropped, now map\n");
        const GLuint bufs[3] = {g->vbo, g->ebo, g->ubo};
        stream_storage_destroy(g, bufs);
        g->stream = STREAM_MAP;
    }
    if (g->stream == STREAM_STORAGE) {
        gx_ring *ring = &g->sr[r];
        if (!gx_ring_fits(ring, n)) {
            F.glGenBuffers(1, &g->sr_big[r]);
            F.glBindBuffer(target, g->sr_big[r]);
            F.glBufferData(target, (GLsizeiptr)n, data, GL_STREAM_DRAW);
            g->sr_oversize++;
            return 0;
        }
        size_t off = (size_t)gx_ring_reserve(ring, &sr_ops, g, n, align);
        F.glBindBuffer(target, buf);
        memcpy(g->sr_map[r] + off, data, n);
        __atomic_thread_fence(__ATOMIC_RELEASE); /* the bytes are out before the draw that reads them is issued */
        return off;
    }
    size_t off = (g->ring_off[r] + align - 1) / align * align;
    F.glBindBuffer(target, buf);
    if (n > g->ring_size[r] || off + n > g->ring_size[r]) {
        size_t size = g->ring_size[r] ? g->ring_size[r] : (r == 2 ? 256u << 10 : 4u << 20);
        while (size < 4 * n)
            size *= 2;
        g->ring_size[r] = size;
        F.glBufferData(target, (GLsizeiptr)size, NULL, GL_STREAM_DRAW);
        off = 0;
    }
    void *p = NULL;
    if (g->stream != STREAM_SUB)
        p = F.glMapBufferRange(target, (GLintptr)off, (GLsizeiptr)n,
                               GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT |
                                   (g->stream == STREAM_MAPINV ? GL_MAP_INVALIDATE_RANGE_BIT : 0));
    if (p) {
        memcpy(p, data, n);
        F.glUnmapBuffer(target);
    } else {
        F.glBufferSubData(target, (GLintptr)off, (GLsizeiptr)n, data);
    }
    g->ring_off[r] = off + n;
    return off;
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
    size_t off = ring_put(g, 2, g->ubo, GL_UNIFORM_BUFFER, u, sizeof *u, (size_t)g->ubo_align);
    F.glBindBufferRange(GL_UNIFORM_BUFFER, 0, g->ubo, (GLintptr)off, (GLsizeiptr)sizeof *u);
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
    unsigned nt = g->batch_ntex;
    size_t stride = offsetof(gx_vout, tex) + 12 * (size_t)nt;
    for (uint32_t i = 0; i < g->nv; i++)
        memcpy(g->packed + i * stride, &g->bv[i], stride);
    size_t vo = ring_put(g, 0, g->vbo, GL_ARRAY_BUFFER, g->packed, g->nv * stride, 16);
    GLsizei st = (GLsizei)stride;
    F.glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, st, (const void *)(vo + offsetof(gx_vout, pos)));
    F.glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, st, (const void *)(vo + offsetof(gx_vout, c0)));
    F.glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, st, (const void *)(vo + offsetof(gx_vout, c1)));
    for (unsigned i = 0; i < 8; i++) {
        if (i < nt) {
            F.glEnableVertexAttribArray(3 + i);
            F.glVertexAttribPointer(3 + i, 3, GL_FLOAT, GL_FALSE, st,
                                    (const void *)(vo + offsetof(gx_vout, tex) + 12 * i));
        } else {
            F.glDisableVertexAttribArray(3 + i);
            F.glVertexAttrib4f(3 + i, 0, 0, 1, 1);
        }
    }
    size_t io = ring_put(g, 1, g->ebo, GL_ELEMENT_ARRAY_BUFFER, g->bi, (size_t)g->ni * 4, 4);
    F.glDrawElements(GL_TRIANGLES, (GLsizei)g->ni, GL_UNSIGNED_INT, (const void *)io);
    stream_after_draw(gx);
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
    /* What this draw adds: its vertices as they are for triangles; lines and points are drawn as quads, 4 vertices
     * and 6 indices per segment or point (a strip has n - 1 segments, a list n / 2). One draw (n < 65536) always
     * fits an empty batch: 4 * 65535 <= BATCH_V. */
    uint32_t need_v = tri ? n : prim == 5 ? 2 * n : 4 * n, need_i = tri ? 3 * n : prim == 5 ? 3 * n : 6 * n;
    if (g->nv + need_v > BATCH_V || g->ni + need_i > BATCH_I)
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
    g->copy_att = dst;
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

/* The GL state the XFB_COPY_DRAW pass changes, put back after it. */
typedef struct gl_saved {
    GLint draw_fb, read_fb, prog, vao, viewport[4], scissor[4], active, tex0, sampler0;
    GLboolean scissor_on, depth_on, blend_on, cull_on, dither_on, mask[4];
} gl_saved;

static void gl_save(gx_gl *g, gl_saved *s)
{
    F.glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &s->draw_fb);
    F.glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &s->read_fb);
    F.glGetIntegerv(GL_CURRENT_PROGRAM, &s->prog);
    F.glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s->vao);
    F.glGetIntegerv(GL_VIEWPORT, s->viewport);
    F.glGetIntegerv(GL_SCISSOR_BOX, s->scissor);
    F.glGetIntegerv(GL_ACTIVE_TEXTURE, &s->active);
    F.glActiveTexture(GL_TEXTURE0);
    F.glGetIntegerv(GL_TEXTURE_BINDING_2D, &s->tex0);
    F.glGetIntegerv(GL_SAMPLER_BINDING, &s->sampler0);
    F.glGetBooleanv(GL_COLOR_WRITEMASK, s->mask);
    s->scissor_on = F.glIsEnabled(GL_SCISSOR_TEST);
    s->depth_on = F.glIsEnabled(GL_DEPTH_TEST);
    s->blend_on = F.glIsEnabled(GL_BLEND);
    s->cull_on = F.glIsEnabled(GL_CULL_FACE);
    s->dither_on = F.glIsEnabled(GL_DITHER);
}

static void gl_set(gx_gl *g, GLenum cap, GLboolean on)
{
    if (on)
        F.glEnable(cap);
    else
        F.glDisable(cap);
}

static void gl_restore(gx_gl *g, const gl_saved *s)
{
    F.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)s->draw_fb);
    F.glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)s->read_fb);
    F.glUseProgram((GLuint)s->prog);
    F.glBindVertexArray((GLuint)s->vao);
    F.glViewport(s->viewport[0], s->viewport[1], s->viewport[2], s->viewport[3]);
    F.glScissor(s->scissor[0], s->scissor[1], s->scissor[2], s->scissor[3]);
    gl_set(g, GL_SCISSOR_TEST, s->scissor_on);
    gl_set(g, GL_DEPTH_TEST, s->depth_on);
    gl_set(g, GL_BLEND, s->blend_on);
    gl_set(g, GL_CULL_FACE, s->cull_on);
    gl_set(g, GL_DITHER, s->dither_on);
    F.glColorMask(s->mask[0], s->mask[1], s->mask[2], s->mask[3]);
    F.glActiveTexture(GL_TEXTURE0);
    F.glBindTexture(GL_TEXTURE_2D, (GLuint)s->tex0);
    F.glBindSampler(0, (GLuint)s->sampler0);
    F.glActiveTexture((GLenum)s->active);
}

/* RECOMP_GC_GX_XFB_COPY=fbo: the slot's own framebuffer, made once; 0 when the driver refuses it. */
static GLuint xfb_slot_fbo(gx_gl *g, unsigned slot)
{
    if (!g->xfb[slot].fbo) {
        F.glGenFramebuffers(1, &g->xfb[slot].fbo);
        F.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g->xfb[slot].fbo);
        F.glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g->xfb[slot].tex, 0);
        if (F.glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "gx: XFB slot %u: its framebuffer is incomplete, copying through the shared one\n", slot);
            F.glDeleteFramebuffers(1, &g->xfb[slot].fbo);
            g->xfb[slot].fbo = 0;
        }
    }
    return g->xfb[slot].fbo;
}

/* RECOMP_GC_GX_XFB_COPY=draw: what the blit does, as a draw through copy_fbo: texel for texel, the picture top-down,
 * and the slot's texels whose source lies outside the EFB left as they are. */
static void xfb_draw(gx_gl *g, unsigned slot, int x, int y, int w, int h)
{
    int s = g->scale, ox = x * s, oy = (GX_EFB_H - y) * s - 1;
    int x0 = ox < 0 ? -ox : 0, x1 = w * s < GX_EFB_W * s - ox ? w * s : GX_EFB_W * s - ox;
    int y0 = oy >= GX_EFB_H * s ? oy - GX_EFB_H * s + 1 : 0, y1 = h * s < oy + 1 ? h * s : oy + 1;
    gl_saved sv;
    gl_save(g, &sv);
    F.glBindFramebuffer(GL_FRAMEBUFFER, g->copy_fbo);
    F.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g->xfb[slot].tex, 0);
    g->copy_att = g->xfb[slot].tex;
    if (x1 > x0 && y1 > y0) {
        F.glViewport(0, 0, w * s, h * s);
        F.glEnable(GL_SCISSOR_TEST);
        F.glScissor(x0, y0, x1 - x0, y1 - y0);
        F.glDisable(GL_DEPTH_TEST);
        F.glDisable(GL_BLEND);
        F.glDisable(GL_CULL_FACE);
        F.glDisable(GL_DITHER);
        F.glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        F.glUseProgram(g->xfb_draw_prog);
        F.glActiveTexture(GL_TEXTURE0);
        F.glBindTexture(GL_TEXTURE_2D, g->efb_color);
        F.glBindSampler(0, 0);
        GLint off[4] = {ox, oy, 0, 0};
        F.glUniform1i(g->xd_color, 0);
        F.glUniform4iv(g->xd_off, 1, off);
        F.glBindVertexArray(g->empty_vao);
        F.glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    gl_restore(g, &sv);
}

/* RECOMP_GC_GX_XFB_DIAG=2: a checksum of all of a slot's texels, read through a framebuffer that only this uses. */
static uint32_t xfb_slot_sum(gx_gl *g, unsigned slot)
{
    size_t n = (size_t)g->xfb[slot].w * (size_t)g->xfb[slot].h;
    if (!g->xfb[slot].tex || !n)
        return 0;
    if (n * 4 > g->diag_cap) {
        uint8_t *p = realloc(g->diag_px, n * 4);
        if (!p)
            return 0;
        g->diag_px = p;
        g->diag_cap = n * 4;
    }
    GLint read_fb = 0, pack = 4;
    F.glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
    F.glGetIntegerv(GL_PACK_ALIGNMENT, &pack);
    if (!g->xfb[slot].diag_fbo) {
        F.glGenFramebuffers(1, &g->xfb[slot].diag_fbo);
        F.glBindFramebuffer(GL_READ_FRAMEBUFFER, g->xfb[slot].diag_fbo);
        F.glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g->xfb[slot].tex, 0);
    } else {
        F.glBindFramebuffer(GL_READ_FRAMEBUFFER, g->xfb[slot].diag_fbo);
    }
    F.glPixelStorei(GL_PACK_ALIGNMENT, 1);
    F.glReadPixels(0, 0, g->xfb[slot].w, g->xfb[slot].h, GL_RGBA, GL_UNSIGNED_BYTE, g->diag_px);
    F.glPixelStorei(GL_PACK_ALIGNMENT, pack);
    F.glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read_fb);
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n * 4; i += 4) {
        uint32_t v;
        memcpy(&v, g->diag_px + i, 4);
        h = (h ^ v) * 16777619u;
    }
    return h;
}

static void xfb_keep(gx_state *gx, uint32_t addr, uint32_t stride, int lines, int x, int y, int w, int h)
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
    g->xfb[slot].stride = stride;
    g->xfb[slot].lines = (uint32_t)lines;
    g->xfb[slot].w = w * s;
    g->xfb[slot].h = h * s;
    g->xfb[slot].copies++;
    /* the picture top-down in the texture (row 0 = top), as the RAM XFB is */
    if (g->xfb_copy == XFB_COPY_DRAW) {
        xfb_draw(g, slot, x, y, w, h);
        F.glDisable(GL_SCISSOR_TEST); /* as the blit leaves it */
    } else {
        GLuint own = g->xfb_copy == XFB_COPY_FBO ? xfb_slot_fbo(g, slot) : 0;
        F.glBindFramebuffer(GL_READ_FRAMEBUFFER, g->efb_fbo);
        if (own) {
            F.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, own);
        } else {
            F.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g->copy_fbo);
            F.glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g->xfb[slot].tex, 0);
            g->copy_att = g->xfb[slot].tex;
        }
        F.glDisable(GL_SCISSOR_TEST);
        int gy0 = (GX_EFB_H - y - h) * s, gy1 = (GX_EFB_H - y) * s;
        F.glBlitFramebuffer(x * s, gy0, (x + w) * s, gy1, 0, h * s, w * s, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        if (g->xfb_copy == XFB_COPY_FLUSH)
            F.glFlush();
    }
    F.glBindFramebuffer(GL_FRAMEBUFFER, g->efb_fbo);
    if (g->xfb_diag >= 2)
        g->xfb[slot].copy_sum = xfb_slot_sum(g, slot);
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
        if (g->xfb_ram) {
            GLuint t = scratch_tex(g, ow, lines);
            copy_pass(gx, t, ow, lines, src, 1, 0, 0, 0, false, (int)BITS(exec, 7, 2));
        }
        size_t row = (size_t)ow * 4;
        uint8_t *buf = g->xfb_ram ? malloc(row * (size_t)lines) : NULL; /* GC_GX_XFB_RAM=0: texture only */
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
        xfb_keep(gx, dst, stride, lines, x, y, w, h);
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
                e->hash = tex_hash(gx, gx->ram + dst, size, 0);
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
    if (g->stream == STREAM_STORAGE && gx->field_count % STREAM_LOG_FIELDS == 0)
        stream_storage_log(g);
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

/* A save state was loaded (XBA-264): RAM holds another moment. EFB copies kept as textures no longer match RAM (their
 * hash is spoilt, so the next use decodes RAM, or the next copy replaces them); the XFB pictures belong to the
 * fields before the load (the presenter shows the XFB in RAM until GX copies again); peeks read the EFB again. */
void gx_gl_state_loaded(gx_state *gx)
{
    gx_gl *g = gx->gl;
    if (!g)
        return;
    for (unsigned i = 0; i < TEX_BUCKETS; i++)
        for (tex_entry *e = g->texs[i]; e; e = e->next)
            if (e->from_efb)
                e->hash = ~e->hash;
    for (unsigned i = 0; i < 4; i++)
        g->xfb[i].addr = 0;
    g->peek_color_ok = g->peek_z_ok = false;
}

/* The slot holding the picture the VI scans at addr, or -1. *offset: found by the VI-offset rule, not at its address. */
static int xfb_lookup(const gx_gl *g, uint32_t addr, bool *offset)
{
    addr &= 0x3FFFFFFFu;
    *offset = false;
    for (unsigned i = 0; i < 4; i++)
        if (g->xfb[i].tex && g->xfb[i].addr == addr)
            return (int)i;
    /* the VI may start a few lines into the copy (Prince of Persia: The Two Thrones scans from the copy's second
     * line): that copy's picture, a line or two off, rather than the XFB in RAM */
    for (unsigned i = 0; i < 4; i++) {
        uint32_t off = addr - g->xfb[i].addr, st = g->xfb[i].stride;
        if (g->xfb[i].tex && st && addr > g->xfb[i].addr && off % st == 0 && off / st <= g->xfb[i].lines / 32) {
            *offset = true;
            return (int)i;
        }
    }
    return -1;
}

unsigned gx_xfb_texture(gx_state *gx, uint32_t addr, int *w, int *h)
{
    gx_gl *g = gx->gl;
    if (!g)
        return 0;
    /* the window presents next: it must not sample through the last draw's samplers (bind_map sets them per draw) */
    F.glBindSampler(0, 0);
    F.glBindSampler(1, 0);
    bool offset;
    int i = xfb_lookup(g, addr, &offset);
    if (i < 0)
        return 0;
    *w = g->xfb[i].w;
    *h = g->xfb[i].h;
    return g->xfb[i].tex;
}

/* ---- XFB diagnostics (RECOMP_GC_GX_XFB_DIAG) ---- */

static void *diag_thread_main(void *arg)
{
    gx_gl *g = arg;
    char line[XFB_DIAG_LINE];
    pthread_mutex_lock(&g->diag_mu);
    for (;;) {
        while (!g->diag_count && !g->diag_quit)
            pthread_cond_wait(&g->diag_cv, &g->diag_mu);
        if (!g->diag_count)
            break;
        memcpy(line, g->diag_lines[g->diag_head], sizeof line);
        g->diag_head = (g->diag_head + 1) % XFB_DIAG_LINES;
        g->diag_count--;
        pthread_mutex_unlock(&g->diag_mu);
        fwrite(line, 1, strlen(line), stderr); /* one write: the line stays whole in the log pipe */
        pthread_mutex_lock(&g->diag_mu);
    }
    pthread_mutex_unlock(&g->diag_mu);
    return NULL;
}

static void diag_start(gx_gl *g)
{
    g->diag_lines = malloc(XFB_DIAG_LINES * XFB_DIAG_LINE);
    pthread_mutex_init(&g->diag_mu, NULL);
    pthread_cond_init(&g->diag_cv, NULL);
    g->diag_thread_ok = g->diag_lines && pthread_create(&g->diag_thread, NULL, diag_thread_main, g) == 0;
    if (!g->diag_thread_ok) {
        fprintf(stderr, "gx: XFB diagnostics: no log thread, off\n");
        g->xfb_diag = 0;
    }
}

static void diag_stop(gx_gl *g)
{
    if (g->diag_thread_ok) {
        pthread_mutex_lock(&g->diag_mu);
        g->diag_quit = true;
        pthread_cond_signal(&g->diag_cv);
        pthread_mutex_unlock(&g->diag_mu);
        pthread_join(g->diag_thread, NULL);
        g->diag_thread_ok = false;
    }
    free(g->diag_lines);
    g->diag_lines = NULL;
    free(g->diag_px);
    g->diag_px = NULL;
}

bool gx_xfb_diag_enabled(const gx_state *gx)
{
    return gx->gl && gx->gl->xfb_diag > 0;
}

void gx_xfb_diag_field(gx_state *gx, uint32_t vi_addr, const char *path, unsigned presents, uint64_t busy_ns)
{
    gx_gl *g = gx->gl;
    if (!g || !g->xfb_diag)
        return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
    uint64_t dur = g->diag_last_ns ? now - g->diag_last_ns : 0;
    g->diag_last_ns = now;
    bool offset;
    int slot = xfb_lookup(g, vi_addr, &offset), att = -1;
    for (int i = 0; i < 4; i++)
        if (g->xfb[i].tex && g->xfb[i].tex == g->copy_att)
            att = i;
    char line[XFB_DIAG_LINE], sl[12] = "-", at[12] = "-";
    if (slot >= 0)
        snprintf(sl, sizeof sl, "%d", slot);
    if (att >= 0)
        snprintf(at, sizeof at, "%d", att);
    int n = snprintf(line, sizeof line,
                     "gx: xfb-diag field=%llu vi=%08X slot=%s match=%s path=%s presents=%u dur_us=%llu busy_us=%llu "
                     "copies=%u att=%s",
                     (unsigned long long)gx->field_count, vi_addr, sl, slot < 0 ? "miss" : offset ? "offset" : "exact",
                     path, presents, (unsigned long long)(dur / 1000), (unsigned long long)(busy_ns / 1000),
                     slot >= 0 ? g->xfb[slot].copies : 0, at);
    /* now / right after the slot's last copy */
    for (unsigned i = 0; g->xfb_diag >= 2 && i < 4 && n > 0 && (size_t)n < sizeof line; i++)
        if (g->xfb[i].tex)
            n += snprintf(line + n, sizeof line - (size_t)n, " sum%u=%08X/%08X", i, xfb_slot_sum(g, i),
                          g->xfb[i].copy_sum);
    pthread_mutex_lock(&g->diag_mu);
    if (g->diag_count == XFB_DIAG_LINES) {
        g->diag_dropped++;
    } else {
        if (g->diag_dropped && n > 0 && (size_t)n < sizeof line) {
            n += snprintf(line + n, sizeof line - (size_t)n, " dropped=%u", g->diag_dropped);
            g->diag_dropped = 0;
        }
        if (n < 0 || (size_t)n > sizeof line - 2)
            n = (int)sizeof line - 2;
        line[n] = '\n';
        line[n + 1] = 0;
        memcpy(g->diag_lines[(g->diag_head + g->diag_count) % XFB_DIAG_LINES], line, (size_t)n + 2);
        g->diag_count++;
        pthread_cond_signal(&g->diag_cv);
    }
    pthread_mutex_unlock(&g->diag_mu);
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
    if (!gl_flavour_ok(ver)) {
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
    g->xfb_ram = env_int("GC_GX_XFB_RAM", 1);
    const char *list = opt && opt->shader_list ? opt->shader_list : getenv("GC_GX_SHADER_LIST");
    if (list && *list)
        g->list_path = strdup(list);
    const char *pcp = opt && opt->program_cache ? opt->program_cache : getenv("GC_GX_PROGRAM_CACHE");
    if (pcp && *pcp)
        g->pc_path = strdup(pcp);
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
    {
        /* Unset: persistent rings where the driver has them, else unsynchronised maps. */
        const char *sm = getenv("RECOMP_GC_GX_STREAM");
        int want = -1;
        if (sm && *sm) {
            for (int m = 0; m < 4; m++)
                if (!strcmp(sm, stream_names[m]))
                    want = m;
            if (want < 0)
                fprintf(stderr, "gx: RECOMP_GC_GX_STREAM=%s is not a mode (storage, map, mapinv, sub)\n", sm);
        }
        g->stream = want < 0 ? STREAM_MAP : want;
        if (want < 0 || want == STREAM_STORAGE) {
            const char *why = stream_storage_create(g);
            if (!why) {
                g->stream = STREAM_STORAGE;
            } else {
                g->stream = STREAM_MAP;
                if (want == STREAM_STORAGE)
                    fprintf(stderr, "gx: buffer streaming: storage requested but unavailable (%s), falling back to map\n",
                            why);
                /* a failed attempt may have made storage immutable: the other modes need fresh buffers */
                F.glDeleteBuffers(1, &g->vbo);
                F.glDeleteBuffers(1, &g->ebo);
                F.glDeleteBuffers(1, &g->ubo);
                F.glGenBuffers(1, &g->vbo);
                F.glGenBuffers(1, &g->ebo);
                F.glGenBuffers(1, &g->ubo);
            }
        }
        fprintf(stderr, "gx: buffer streaming: %s\n", stream_names[g->stream]);
    }
    g->ubo_align = 256;
    F.glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &g->ubo_align);
    if (g->ubo_align < 16)
        g->ubo_align = 256;
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
    /* how copies reach the XFB slots, and the per-field diagnostics (the flicker on Adreno 840) */
    {
        const char *cm = getenv("RECOMP_GC_GX_XFB_COPY");
        g->xfb_copy = cm && !strcmp(cm, "blit") ? XFB_COPY_BLIT : cm && !strcmp(cm, "fbo") ? XFB_COPY_FBO
                      : cm && !strcmp(cm, "draw") ? XFB_COPY_DRAW : XFB_COPY_FLUSH;
        if (cm && *cm && g->xfb_copy == XFB_COPY_FLUSH && strcmp(cm, "flush"))
            fprintf(stderr, "gx: RECOMP_GC_GX_XFB_COPY=%s is not a mode (blit, flush, fbo, draw)\n", cm);
        if (g->xfb_copy == XFB_COPY_DRAW) {
            static const char xd_fs[] =
                "#version 300 es\n"
                "precision highp float;\n"
                "precision highp int;\n"
                "uniform highp sampler2D u_color;\n"
                "uniform ivec4 u_off;\n"
                "out vec4 o_color;\n"
                "void main() {\n"
                "  ivec2 o = ivec2(floor(gl_FragCoord.xy));\n"
                "  o_color = texelFetch(u_color, ivec2(u_off.x + o.x, u_off.y - o.y), 0);\n"
                "}\n";
            GLuint xv = compile_shader(&g->f, GL_VERTEX_SHADER, copy_vs, log, sizeof log);
            GLuint xf = xv ? compile_shader(&g->f, GL_FRAGMENT_SHADER, xd_fs, log, sizeof log) : 0;
            g->xfb_draw_prog = xf ? link_program(&g->f, xv, xf, log, sizeof log) : 0;
            if (xv)
                F.glDeleteShader(xv);
            if (xf)
                F.glDeleteShader(xf);
            if (g->xfb_draw_prog) {
                g->xd_color = F.glGetUniformLocation(g->xfb_draw_prog, "u_color");
                g->xd_off = F.glGetUniformLocation(g->xfb_draw_prog, "u_off");
            } else {
                fprintf(stderr, "gx: XFB copy shader failed, blitting: %s\n", log);
                g->xfb_copy = XFB_COPY_BLIT;
            }
        }
        static const char *const copy_names[] = {"blit", "flush", "fbo", "draw"};
        fprintf(stderr, "gx: XFB copy: %s\n", copy_names[g->xfb_copy]);
        int diag = env_int("RECOMP_GC_GX_XFB_DIAG", 0);
        if (diag > 0) {
            g->xfb_diag = diag;
            diag_start(g);
            if (g->xfb_diag)
                fprintf(stderr, "gx: XFB diagnostics: level %d\n", g->xfb_diag);
        }
    }
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
    /* program binaries of this driver: those states ready before the first frame */
    if (g->pc_path)
        progcache_attach(g);
    /* the per-title list: compile ahead what earlier runs met */
    if (g->list_path) {
        gx_shader_key *keys;
        size_t n, ready = 0;
        if (!gx_shader_list_read(g->list_path, &keys, &n)) {
            for (size_t i = 0; i < n; i++)
                if (g->async != -1) {
                    prog_entry *e = prog_lookup(g, &keys[i], false);
                    if (e && e->in_cache)
                        ready++;
                    else if (!e)
                        prog_lookup(g, &keys[i], true);
                } else {
                    record_key(g, &keys[i]);
                }
            free(keys);
            /* written again only with states it lacks (the program cache's included) */
            g->list_dirty = g->nkeys > n;
            fprintf(stderr, "gx: shader list %s: %zu states queued (%zu ready from program binaries)\n", g->list_path,
                    n, ready);
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
    diag_stop(g);
    worker_stop(g);
    gx_save_shader_list(gx);
    gx_save_program_cache(gx);
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
    for (unsigned i = 0; i < 4; i++) {
        if (g->xfb[i].fbo)
            F.glDeleteFramebuffers(1, &g->xfb[i].fbo);
        if (g->xfb[i].diag_fbo)
            F.glDeleteFramebuffers(1, &g->xfb[i].diag_fbo);
        if (g->xfb[i].tex)
            F.glDeleteTextures(1, &g->xfb[i].tex);
    }
    if (g->xfb_draw_prog)
        F.glDeleteProgram(g->xfb_draw_prog);
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
    if (g->stream == STREAM_STORAGE) {
        const GLuint bufs[3] = {g->vbo, g->ebo, g->ubo};
        stream_storage_log(g);
        stream_storage_destroy(g, bufs);
    }
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
    gx_progcache_free(&g->pc);
    free(g->pc_path);
    free(g);
    gx->gl = NULL;
}

/* ---- shader warm-up (XBA-264) ---- */

static double warm_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

int gx_gl_warmup(const char *const *lists, size_t nlists, const char *cache, gx_warmup_result *r)
{
    gx_warmup_result res = {0};
    double t0 = warm_now();
    if (r)
        *r = res;
    gx_eglfn egl;
    gx_glfn fn;
    if (!cache || !*cache || gx_egl_load(&egl) || gx_gl_load(&fn, &egl)) {
        fprintf(stderr, "gx: warm-up: no cache path or no GLES 3 entry points\n");
        return 2;
    }
    gx_glfn *f = &fn;
    const char *ver = (const char *)f->glGetString(GL_VERSION);
    if (!gl_flavour_ok(ver)) {
        fprintf(stderr, "gx: warm-up: needs OpenGL ES 3 or GL 3.3, the context is %s\n", ver ? ver : "none");
        return 2;
    }
    const char *ren = gl_str(f, GL_RENDERER);
    if (!binaries_supported(f)) {
        fprintf(stderr, "gx: warm-up %s: the driver has no program binaries (driver %s %s): nothing to do\n", cache,
                ren, ver);
        return 1;
    }
    char drv[512];
    uint64_t key = driver_key(f, drv, sizeof drv);
    gx_progcache pc;
    const char *why = NULL;
    bool dirty = gx_progcache_load(cache, key, drv, &pc, &why, NULL, 0) != 0 && strcmp(why, "none") != 0;
    if (why && strcmp(why, "none"))
        fprintf(stderr, "gx: warm-up %s: the cache was ignored (%s), rebuilt\n", cache, why);
    /* the union of the lists */
    gx_shader_key *all = NULL;
    size_t nall = 0, cap = 0;
    for (size_t l = 0; l < nlists; l++) {
        gx_shader_key *keys;
        size_t n;
        if (!lists[l] || !*lists[l] || gx_shader_list_read(lists[l], &keys, &n))
            continue;
        for (size_t i = 0; i < n; i++) {
            size_t j = 0;
            while (j < nall && memcmp(&all[j], &keys[i], sizeof keys[i]))
                j++;
            if (j < nall)
                continue;
            if (nall == cap) {
                cap = cap ? cap * 2 : 64;
                gx_shader_key *na = realloc(all, cap * sizeof *na);
                if (!na)
                    break;
                all = na;
            }
            all[nall++] = keys[i];
        }
        free(keys);
    }
    res.states = (int)nall;
    for (size_t i = 0; i < nall; i++) {
        gx_progbin *b = gx_progcache_find(&pc, &all[i]);
        if (b && b->src_hash == gx_progcache_src_hash(&all[i])) {
            res.cached++;
            continue;
        }
        GLuint p = build_program(f, &all[i]);
        if (p && !cache_program(f, &pc, &all[i], p)) {
            res.compiled++;
            dirty = true;
        } else {
            res.failed++;
        }
        if (p)
            f->glDeleteProgram(p);
    }
    free(all);
    int rc = 0;
    if (dirty && gx_progcache_save(cache, &pc)) {
        fprintf(stderr, "gx: warm-up: cannot write %s\n", cache);
        rc = 3;
    }
    res.seconds = warm_now() - t0;
    fprintf(stderr, "gx: warm-up %s: %d states, %d already cached, %d compiled, %d failed, %zu programs in the file, "
                    "%.2f s (driver %s %s)\n",
            cache, res.states, res.cached, res.compiled, res.failed, pc.n, res.seconds, ren, ver);
    gx_progcache_free(&pc);
    if (r)
        *r = res;
    return rc;
}

int gx_warmup_headless(const char *const *lists, size_t nlists, const char *cache, bool software,
                       gx_warmup_result *r)
{
    char why[160] = "";
    if (r)
        memset(r, 0, sizeof *r);
    if (gx_egl_headless_create(software, why, sizeof why)) {
        fprintf(stderr, "gx: warm-up: no GLES 3 context (%s)\n", why);
        return 2;
    }
    int rc = gx_gl_warmup(lists, nlists, cache, r);
    gx_egl_headless_destroy();
    return rc;
}
