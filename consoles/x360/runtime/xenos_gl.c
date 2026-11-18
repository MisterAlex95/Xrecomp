/* xenos_gl.c -- see xenos_gl.h. Register layouts follow Xenia's register_table.inc / xenos.h (BSD): the vertex fetch
 * constants (two dwords each from 0x4800: type and dword address, then endian swap and size), the float constants
 * (0x4000 vertex, 0x4400 pixel, four per vec4), booleans (0x4900, eight words), loops (0x4908, 32 words), the draw
 * initiator's primitive types, VGT_DMA_SIZE's swap mode, PA_CL_VTE_CNTL and the viewport scale / offset. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xenos_gl.h"

/* ---- the GLES / EGL subset, declared here (no headers needed to build) ---- */
typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef unsigned char GLboolean;
typedef float GLfloat;
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr, GLintptr;
typedef void *EGLDisplay, *EGLConfig, *EGLContext, *EGLSurface;
typedef int EGLint;
typedef unsigned int EGLBoolean, EGLenum;

#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_RGBA8 0x8058
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_UNIFORM_BUFFER 0x8A11
#define GL_STREAM_DRAW 0x88E0
#define GL_FLOAT 0x1406
#define GL_UNSIGNED_INT 0x1405
#define GL_UNSIGNED_BYTE 0x1401
#define GL_RGBA 0x1908
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_RENDERER 0x1F01
#define GL_INVALID_INDEX 0xFFFFFFFFu
#define GL_DEPTH_TEST 0x0B71
#define GL_BLEND 0x0BE2
#define GL_CULL_FACE 0x0B44
#define GL_SCISSOR_TEST 0x0C11

#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES3_BIT 0x0040
#define EGL_NONE 0x3038
#define EGL_CONTEXT_MAJOR_VERSION 0x3098
#define EGL_CONTEXT_MINOR_VERSION 0x30FB
#define EGL_OPENGL_ES_API 0x30A0

#define GLFUNCS(X)                                                                                                     \
    X(const unsigned char *, glGetString, (GLenum))                                                                   \
    X(GLuint, glCreateShader, (GLenum))                                                                                \
    X(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *))                                  \
    X(void, glCompileShader, (GLuint))                                                                                 \
    X(void, glGetShaderiv, (GLuint, GLenum, GLint *))                                                                  \
    X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))                                                \
    X(GLuint, glCreateProgram, (void))                                                                                 \
    X(void, glAttachShader, (GLuint, GLuint))                                                                          \
    X(void, glLinkProgram, (GLuint))                                                                                   \
    X(void, glGetProgramiv, (GLuint, GLenum, GLint *))                                                                 \
    X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))                                               \
    X(void, glDeleteShader, (GLuint))                                                                                  \
    X(void, glDeleteProgram, (GLuint))                                                                                 \
    X(void, glUseProgram, (GLuint))                                                                                    \
    X(GLuint, glGetUniformBlockIndex, (GLuint, const GLchar *))                                                        \
    X(void, glUniformBlockBinding, (GLuint, GLuint, GLuint))                                                           \
    X(void, glGenBuffers, (GLsizei, GLuint *))                                                                         \
    X(void, glDeleteBuffers, (GLsizei, const GLuint *))                                                                \
    X(void, glBindBuffer, (GLenum, GLuint))                                                                            \
    X(void, glBufferData, (GLenum, GLsizeiptr, const void *, GLenum))                                                  \
    X(void, glBindBufferBase, (GLenum, GLuint, GLuint))                                                                \
    X(GLint, glGetAttribLocation, (GLuint, const GLchar *))                                                            \
    X(void, glEnableVertexAttribArray, (GLuint))                                                                       \
    X(void, glDisableVertexAttribArray, (GLuint))                                                                      \
    X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *))                         \
    X(void, glDrawArrays, (GLenum, GLint, GLsizei))                                                                    \
    X(void, glDrawElements, (GLenum, GLsizei, GLenum, const void *))                                                   \
    X(void, glGenFramebuffers, (GLsizei, GLuint *))                                                                    \
    X(void, glDeleteFramebuffers, (GLsizei, const GLuint *))                                                           \
    X(void, glBindFramebuffer, (GLenum, GLuint))                                                                       \
    X(void, glGenRenderbuffers, (GLsizei, GLuint *))                                                                   \
    X(void, glDeleteRenderbuffers, (GLsizei, const GLuint *))                                                          \
    X(void, glBindRenderbuffer, (GLenum, GLuint))                                                                      \
    X(void, glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei))                                                 \
    X(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint))                                               \
    X(GLenum, glCheckFramebufferStatus, (GLenum))                                                                      \
    X(void, glGenVertexArrays, (GLsizei, GLuint *))                                                                    \
    X(void, glDeleteVertexArrays, (GLsizei, const GLuint *))                                                           \
    X(void, glBindVertexArray, (GLuint))                                                                               \
    X(void, glViewport, (GLint, GLint, GLsizei, GLsizei))                                                              \
    X(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))                                                        \
    X(void, glClear, (GLbitfield))                                                                                     \
    X(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))                                   \
    X(void, glDisable, (GLenum))                                                                                       \
    X(void, glFinish, (void))

#define MAX_PROGRAMS 256
#define MAX_ATTRS 16

typedef struct attr {
    uint32_t fetch_constant, offset, format, stride, is_signed;
} attr;

typedef struct program {
    uint32_t vs_crc, ps_crc;
    GLuint prog;
    int ok;
    int nattr;
    attr attrs[MAX_ATTRS];
    GLint loc[MAX_ATTRS];
} program;

struct xenos_gl {
#define DECL(ret, name, args) ret(*name) args;
    GLFUNCS(DECL)
#undef DECL
    void *(*eglGetProcAddress)(const char *);
    EGLDisplay (*eglGetPlatformDisplayEXT)(EGLenum, void *, const EGLint *);
    EGLBoolean (*eglInitialize)(EGLDisplay, EGLint *, EGLint *);
    EGLBoolean (*eglBindAPI)(EGLenum);
    EGLBoolean (*eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
    EGLContext (*eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
    EGLBoolean (*eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
    EGLBoolean (*eglDestroyContext)(EGLDisplay, EGLContext);
    EGLBoolean (*eglTerminate)(EGLDisplay);
    void *egl_lib, *gles_lib;
    EGLDisplay dpy;
    EGLContext ctx;
    GLuint fbo, rb, vao, vbo, ibo, ubo[2];
    int width, height;
    uint8_t *frame;
    char cache[512], frames[512];
    program progs[MAX_PROGRAMS];
    int nprogs;
    uint32_t missing[64];
    int nmissing;
    xenos_gl_stats st;
    FILE *log;
};

static void *open_first(const char *const *names)
{
    for (int i = 0; names[i]; i++) {
        void *h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
        if (h)
            return h;
    }
    return NULL;
}

static void fail(char *why, size_t n, const char *msg)
{
    if (why && n)
        snprintf(why, n, "%s", msg);
}

xenos_gl *xenos_gl_create(const char *cache_dir, int width, int height, char *why, size_t why_n)
{
    static const char *const egl_names[] = {"libEGL.so.1", "libEGL.so", NULL};
    static const char *const gles_names[] = {"libGLESv2.so.2", "libGLESv3.so", "libGLESv2.so", NULL};
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192) {
        fail(why, why_n, "bad render size");
        return NULL;
    }
    xenos_gl *g = calloc(1, sizeof *g);
    if (!g) {
        fail(why, why_n, "out of memory");
        return NULL;
    }
    g->log = stderr;
    g->width = width, g->height = height;
    snprintf(g->cache, sizeof g->cache, "%s", cache_dir ? cache_dir : ".");
    g->egl_lib = open_first(egl_names);
    g->gles_lib = open_first(gles_names);
    if (!g->egl_lib || !g->gles_lib) {
        fail(why, why_n, "no libEGL / libGLESv2");
        xenos_gl_destroy(g);
        return NULL;
    }
#define ESYM(name) *(void **)&g->name = dlsym(g->egl_lib, #name)
    ESYM(eglGetProcAddress);
    ESYM(eglInitialize);
    ESYM(eglBindAPI);
    ESYM(eglChooseConfig);
    ESYM(eglCreateContext);
    ESYM(eglMakeCurrent);
    ESYM(eglDestroyContext);
    ESYM(eglTerminate);
#undef ESYM
    if (!g->eglGetProcAddress || !g->eglInitialize || !g->eglCreateContext || !g->eglMakeCurrent) {
        fail(why, why_n, "EGL entry points missing");
        xenos_gl_destroy(g);
        return NULL;
    }
    *(void **)&g->eglGetPlatformDisplayEXT = g->eglGetProcAddress("eglGetPlatformDisplayEXT");
    int missing = 0;
#define GSYM(ret, name, args)                                                                                          \
    if (!(*(void **)&g->name = dlsym(g->gles_lib, #name)) && !(*(void **)&g->name = g->eglGetProcAddress(#name)))      \
        missing++;
    GLFUNCS(GSYM)
#undef GSYM
    if (missing) {
        fail(why, why_n, "GLES 3.0 entry points missing");
        xenos_gl_destroy(g);
        return NULL;
    }
    g->dpy = g->eglGetPlatformDisplayEXT ? g->eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA, NULL, NULL)
                                         : NULL;
    EGLint maj, min;
    if (!g->dpy || !g->eglInitialize(g->dpy, &maj, &min)) {
        g->dpy = NULL;
        fail(why, why_n, "no surfaceless EGL display");
        xenos_gl_destroy(g);
        return NULL;
    }
    g->eglBindAPI(EGL_OPENGL_ES_API);
    static const EGLint cfg_attr[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
    EGLConfig cfg = NULL;
    EGLint ncfg = 0;
    g->eglChooseConfig(g->dpy, cfg_attr, &cfg, 1, &ncfg);
    static const EGLint ctx_attr[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE};
    g->ctx = g->eglCreateContext(g->dpy, ncfg ? cfg : NULL, NULL, ctx_attr);
    if (!g->ctx || !g->eglMakeCurrent(g->dpy, NULL, NULL, g->ctx)) {
        fail(why, why_n, "no GLES 3.0 context");
        xenos_gl_destroy(g);
        return NULL;
    }
    g->glGenFramebuffers(1, &g->fbo);
    g->glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    g->glGenRenderbuffers(1, &g->rb);
    g->glBindRenderbuffer(GL_RENDERBUFFER, g->rb);
    g->glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
    g->glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, g->rb);
    if (g->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fail(why, why_n, "incomplete render target");
        xenos_gl_destroy(g);
        return NULL;
    }
    g->glGenVertexArrays(1, &g->vao);
    g->glBindVertexArray(g->vao);
    g->glGenBuffers(1, &g->vbo);
    g->glGenBuffers(1, &g->ibo);
    g->glGenBuffers(2, g->ubo);
    g->frame = calloc((size_t)width * height, 4);
    g->glDisable(GL_DEPTH_TEST);
    g->glDisable(GL_BLEND);
    g->glDisable(GL_CULL_FACE);
    g->glDisable(GL_SCISSOR_TEST);
    g->glViewport(0, 0, width, height);
    g->glClearColor(0, 0, 0, 1);
    g->glClear(GL_COLOR_BUFFER_BIT);
    return g;
}

void xenos_gl_destroy(xenos_gl *g)
{
    if (!g)
        return;
    if (g->ctx) {
        for (int i = 0; i < g->nprogs; i++)
            if (g->progs[i].prog)
                g->glDeleteProgram(g->progs[i].prog);
        g->glDeleteBuffers(1, &g->vbo);
        g->glDeleteBuffers(1, &g->ibo);
        g->glDeleteBuffers(2, g->ubo);
        g->glDeleteVertexArrays(1, &g->vao);
        g->glDeleteRenderbuffers(1, &g->rb);
        g->glDeleteFramebuffers(1, &g->fbo);
        g->eglMakeCurrent(g->dpy, NULL, NULL, NULL);
        g->eglDestroyContext(g->dpy, g->ctx);
    }
    if (g->dpy && g->eglTerminate)
        g->eglTerminate(g->dpy);
    /* the libraries stay loaded: Mesa does not like being unloaded with threads of its own alive */
    free(g->frame);
    free(g);
}

void xenos_gl_set_frames_dir(xenos_gl *g, const char *dir)
{
    snprintf(g->frames, sizeof g->frames, "%s", dir ? dir : "");
}

void xenos_gl_set_log(xenos_gl *g, void *file)
{
    g->log = file ? file : stderr;
}

const uint8_t *xenos_gl_frame(const xenos_gl *g, int *width, int *height)
{
    if (width)
        *width = g->width;
    if (height)
        *height = g->height;
    return g->frame;
}

void xenos_gl_get_stats(const xenos_gl *g, xenos_gl_stats *s)
{
    *s = g->st;
    s->missing_shaders = (uint32_t)g->nmissing;
}

/* ---- shaders ---- */

static uint32_t crc32_words(const uint32_t *w, uint32_t n)
{
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++)
        for (int b = 3; b >= 0; b--) /* the microcode's big-endian bytes, as the dump files hold them */
            c = table[(c ^ (w[i] >> (8 * b))) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        b = NULL;
    }
    fclose(f);
    if (b)
        b[n] = 0;
    return b;
}

static void report_missing(xenos_gl *g, int pixel, uint32_t crc)
{
    uint32_t key = crc ^ (pixel ? 0x80000000u : 0); /* good enough to report each one once */
    for (int i = 0; i < g->nmissing; i++)
        if (g->missing[i] == key)
            return;
    if (g->nmissing < 64)
        g->missing[g->nmissing++] = key;
    fprintf(g->log, "xenos_gl: %s_%08X not in the shader cache\n", pixel ? "ps" : "vs", crc);
}

static GLuint compile(xenos_gl *g, GLenum type, const char *src, const char *name)
{
    GLuint s = g->glCreateShader(type);
    g->glShaderSource(s, 1, &src, NULL);
    g->glCompileShader(s);
    GLint ok = 0;
    g->glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = "";
        g->glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(g->log, "xenos_gl: %s does not compile: %s\n", name, log);
        g->glDeleteShader(s);
        return 0;
    }
    return s;
}

/* "in vec4 xe_vN;  // fetch constant F, offset O, format T, stride S, signed G" lines of the vertex shader */
static int parse_attrs(const char *src, attr *out)
{
    int n = 0;
    for (const char *p = strstr(src, "in vec4 xe_v"); p && n < MAX_ATTRS; p = strstr(p + 1, "in vec4 xe_v")) {
        unsigned idx, fc, off, fmt, stride, sg;
        if (sscanf(p, "in vec4 xe_v%u;  // fetch constant %u, offset %u, format %u, stride %u, signed %u", &idx, &fc,
                   &off, &fmt, &stride, &sg) == 6 &&
            idx == (unsigned)n) {
            out[n].fetch_constant = fc, out[n].offset = off, out[n].format = fmt, out[n].stride = stride;
            out[n].is_signed = sg;
            n++;
        }
    }
    return n;
}

static program *get_program(xenos_gl *g, const xenos_cp *cp)
{
    const xenos_shader *vs = &cp->shader[0], *ps = &cp->shader[1];
    if (!vs->words || !ps->words)
        return NULL;
    uint32_t vc = crc32_words(vs->code, vs->words), pc = crc32_words(ps->code, ps->words);
    for (int i = 0; i < g->nprogs; i++)
        if (g->progs[i].vs_crc == vc && g->progs[i].ps_crc == pc)
            return g->progs[i].ok ? &g->progs[i] : NULL;
    if (g->nprogs == MAX_PROGRAMS)
        return NULL;
    program *p = &g->progs[g->nprogs++];
    memset(p, 0, sizeof *p);
    p->vs_crc = vc, p->ps_crc = pc;
    char path[600], name[32];
    snprintf(path, sizeof path, "%s/vs_%08X.vert", g->cache, vc);
    char *vsrc = slurp(path);
    snprintf(path, sizeof path, "%s/ps_%08X.frag", g->cache, pc);
    char *psrc = slurp(path);
    if (!vsrc)
        report_missing(g, 0, vc);
    if (!psrc)
        report_missing(g, 1, pc);
    if (vsrc && psrc) {
        snprintf(name, sizeof name, "vs_%08X", vc);
        GLuint v = compile(g, GL_VERTEX_SHADER, vsrc, name);
        snprintf(name, sizeof name, "ps_%08X", pc);
        GLuint f = compile(g, GL_FRAGMENT_SHADER, psrc, name);
        if (v && f) {
            p->prog = g->glCreateProgram();
            g->glAttachShader(p->prog, v);
            g->glAttachShader(p->prog, f);
            g->glLinkProgram(p->prog);
            GLint ok = 0;
            g->glGetProgramiv(p->prog, GL_LINK_STATUS, &ok);
            if (ok) {
                p->ok = 1;
                p->nattr = parse_attrs(vsrc, p->attrs);
                for (int i = 0; i < p->nattr; i++) {
                    char an[16];
                    snprintf(an, sizeof an, "xe_v%d", i);
                    p->loc[i] = g->glGetAttribLocation(p->prog, an);
                }
                GLuint b = g->glGetUniformBlockIndex(p->prog, "XeVertexConstants");
                if (b != GL_INVALID_INDEX)
                    g->glUniformBlockBinding(p->prog, b, 0);
                b = g->glGetUniformBlockIndex(p->prog, "XePixelConstants");
                if (b != GL_INVALID_INDEX)
                    g->glUniformBlockBinding(p->prog, b, 1);
            } else {
                char log[2048] = "";
                g->glGetProgramInfoLog(p->prog, sizeof log, NULL, log);
                fprintf(g->log, "xenos_gl: vs_%08X + ps_%08X do not link: %s\n", vc, pc, log);
                g->glDeleteProgram(p->prog);
                p->prog = 0;
            }
        }
        if (v)
            g->glDeleteShader(v);
        if (f)
            g->glDeleteShader(f);
    }
    free(vsrc);
    free(psrc);
    return p->ok ? p : NULL;
}

/* ---- guest memory as the GPU reads it ---- */

static uint32_t gpu_swap(uint32_t le, uint32_t endian)
{
    switch (endian & 3) {
    case 1: return ((le >> 8) & 0x00FF00FFu) | ((le << 8) & 0xFF00FF00u); /* 8 in 16 */
    case 2: return __builtin_bswap32(le);                                 /* 8 in 32 */
    case 3: return (le >> 16) | (le << 16);                               /* 16 in 32 */
    default: return le;
    }
}

/* a dword at a physical address, as the (little-endian) GPU sees it after its swap mode: cp->read32 gives the
 * big-endian value the CPU sees */
static uint32_t gpu_read(const xenos_cp *cp, uint32_t phys, uint32_t endian)
{
    uint32_t be = cp->read32 ? cp->read32(cp->ctx, phys & 0x1FFFFFFFu) : 0;
    return gpu_swap(__builtin_bswap32(be), endian);
}

static float as_float(uint32_t v)
{
    float f;
    memcpy(&f, &v, 4);
    return f;
}

/* one attribute of one vertex as a vec4; 0 when the format is not handled */
static int fetch_vertex(const xenos_cp *cp, uint32_t base, uint32_t endian, const attr *a, uint32_t index,
                        float out[4])
{
    uint32_t at = base + 4 * (a->offset + index * a->stride);
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    switch (a->format) {
    case 36: /* 32_FLOAT */
    case 37: /* 32_32_FLOAT */
    case 57: /* 32_32_32_FLOAT */
    case 38: { /* 32_32_32_32_FLOAT */
        int n = a->format == 36 ? 1 : a->format == 37 ? 2 : a->format == 57 ? 3 : 4;
        for (int i = 0; i < n; i++)
            out[i] = as_float(gpu_read(cp, at + 4 * i, endian));
        return 1;
    }
    case 6: { /* 8_8_8_8: x in the low byte (normalised; signed when the fetch says so) */
        uint32_t v = gpu_read(cp, at, endian);
        for (int i = 0; i < 4; i++) {
            uint32_t b = (v >> (8 * i)) & 0xFF;
            out[i] = a->is_signed ? (float)(int8_t)b / 127.0f : (float)b / 255.0f;
            if (a->is_signed && out[i] < -1.0f)
                out[i] = -1.0f;
        }
        return 1;
    }
    case 25: { /* 16_16 normalised */
        uint32_t v = gpu_read(cp, at, endian);
        for (int i = 0; i < 2; i++) {
            uint32_t h = (v >> (16 * i)) & 0xFFFF;
            out[i] = a->is_signed ? (float)(int16_t)h / 32767.0f : (float)h / 65535.0f;
            if (a->is_signed && out[i] < -1.0f)
                out[i] = -1.0f;
        }
        return 1;
    }
    default:
        return 0;
    }
}

static int gl_prim(uint32_t t)
{
    switch (t) {
    case 1: return 0; /* point list */
    case 2: return 1; /* line list */
    case 3: return 3; /* line strip */
    case 4: return 4; /* triangle list */
    case 5: return 6; /* triangle fan */
    case 6: return 5; /* triangle strip */
    default: return -1; /* rectangle / quad lists and the rest: not yet */
    }
}

static void upload_constants(xenos_gl *g, const xenos_cp *cp)
{
    uint8_t block[4096 + 32 + 128];
    for (int stage = 0; stage < 2; stage++) {
        memcpy(block, &cp->regs[0x4000 + 0x400 * stage], 4096); /* float constants: register values are the bits */
        memcpy(block + 4096, &cp->regs[0x4900], 32);
        memcpy(block + 4096 + 32, &cp->regs[0x4908], 128);
        g->glBindBuffer(GL_UNIFORM_BUFFER, g->ubo[stage]);
        g->glBufferData(GL_UNIFORM_BUFFER, sizeof block, block, GL_STREAM_DRAW);
        g->glBindBufferBase(GL_UNIFORM_BUFFER, (GLuint)stage, g->ubo[stage]);
    }
}

static void set_viewport(xenos_gl *g, const xenos_cp *cp)
{
    uint32_t vte = cp->regs[0x2206];
    if ((vte & 0xF) != 0xF) { /* scale / offset not both enabled for x and y: clip space covers the target */
        g->glViewport(0, 0, g->width, g->height);
        return;
    }
    float xs = as_float(cp->regs[0x210F]), xo = as_float(cp->regs[0x2110]);
    float ys = as_float(cp->regs[0x2111]), yo = as_float(cp->regs[0x2112]);
    float ax = xs < 0 ? -xs : xs, ay = ys < 0 ? -ys : ys;
    /* Xenos window y grows downwards; the target is read back top row first, so GL's y is flipped */
    g->glViewport((GLint)(xo - ax), (GLint)(g->height - (yo + ay)), (GLsizei)(2 * ax), (GLsizei)(2 * ay));
}

void xenos_gl_draw(xenos_gl *g, const xenos_cp *cp, const xenos_draw *d)
{
    int prim = gl_prim(d->prim_type);
    if (prim < 0 || d->source_select == 1 || !d->num_indices) {
        g->st.unsupported++;
        return;
    }
    program *p = get_program(g, cp);
    if (!p) {
        g->st.no_shader++;
        return;
    }
    /* indices: from the DMA buffer (16 or 32 bits, VGT_DMA_SIZE's swap mode) or 0..n-1 */
    uint32_t n = d->num_indices, *idx = NULL, max_index = n - 1;
    if (d->source_select == 0) {
        idx = malloc(4 * (size_t)n);
        if (!idx)
            return;
        uint32_t endian = cp->regs[XENOS_REG_VGT_DMA_SIZE] >> 30;
        max_index = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t v;
            if (d->index_size) {
                v = gpu_read(cp, d->dma_base + 4 * i, endian);
            } else {
                uint32_t w = gpu_read(cp, d->dma_base + 4 * (i / 2), endian);
                v = (w >> (16 * (i & 1))) & 0xFFFF;
            }
            idx[i] = v;
            if (v > max_index)
                max_index = v;
        }
        if (max_index > 0xFFFFF) {
            free(idx);
            g->st.unsupported++;
            return;
        }
    }
    /* vertices: every attribute converted to vec4, interleaved */
    uint32_t nv = max_index + 1, na = (uint32_t)p->nattr;
    float *vb = na ? malloc((size_t)nv * na * 16) : NULL;
    if (na && !vb) {
        free(idx);
        return;
    }
    for (uint32_t a = 0; a < na; a++) {
        const attr *at = &p->attrs[a];
        uint32_t c0 = cp->regs[0x4800 + 2 * at->fetch_constant], c1 = cp->regs[0x4800 + 2 * at->fetch_constant + 1];
        for (uint32_t v = 0; v < nv; v++)
            if (!fetch_vertex(cp, c0 & ~3u, c1 & 3, at, v, &vb[(v * na + a) * 4])) {
                free(vb);
                free(idx);
                g->st.unsupported++;
                return;
            }
    }
    g->glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    g->glUseProgram(p->prog);
    upload_constants(g, cp);
    set_viewport(g, cp);
    g->glBindVertexArray(g->vao);
    g->glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
    g->glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)nv * na * 16, vb, GL_STREAM_DRAW);
    for (uint32_t a = 0; a < na; a++)
        if (p->loc[a] >= 0) {
            g->glEnableVertexAttribArray((GLuint)p->loc[a]);
            g->glVertexAttribPointer((GLuint)p->loc[a], 4, GL_FLOAT, 0, (GLsizei)(na * 16),
                                     (void *)(uintptr_t)(a * 16));
        }
    if (idx) {
        g->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g->ibo);
        g->glBufferData(GL_ELEMENT_ARRAY_BUFFER, 4 * (GLsizeiptr)n, idx, GL_STREAM_DRAW);
        g->glDrawElements((GLenum)prim, (GLsizei)n, GL_UNSIGNED_INT, NULL);
    } else {
        g->glDrawArrays((GLenum)prim, 0, (GLsizei)n);
    }
    for (uint32_t a = 0; a < na; a++)
        if (p->loc[a] >= 0)
            g->glDisableVertexAttribArray((GLuint)p->loc[a]);
    g->st.drawn++;
    free(vb);
    free(idx);
}

void xenos_gl_swap(xenos_gl *g, const xenos_cp *cp)
{
    (void)cp;
    g->glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    g->glFinish();
    size_t row = (size_t)g->width * 4;
    uint8_t *tmp = malloc(row * g->height);
    if (tmp) {
        g->glReadPixels(0, 0, g->width, g->height, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
        for (int y = 0; y < g->height; y++) /* GL's bottom row first -> top row first */
            memcpy(g->frame + row * y, tmp + row * (g->height - 1 - y), row);
        free(tmp);
    }
    if (g->frames[0]) {
        char path[600];
        snprintf(path, sizeof path, "%s/frame_%05llu.ppm", g->frames, (unsigned long long)g->st.frames);
        FILE *f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", g->width, g->height);
            for (size_t i = 0; i < (size_t)g->width * g->height; i++)
                fwrite(g->frame + 4 * i, 1, 3, f);
            fclose(f);
        }
    }
    g->st.frames++;
    g->glViewport(0, 0, g->width, g->height);
    g->glClearColor(0, 0, 0, 1);
    g->glClear(GL_COLOR_BUFFER_BIT);
}
