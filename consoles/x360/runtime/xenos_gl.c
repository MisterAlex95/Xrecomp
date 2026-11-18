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
#define GL_STENCIL_TEST 0x0B90
#define GL_DEPTH24_STENCIL8 0x88F0
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define GL_DEPTH_BUFFER_BIT 0x0100
#define GL_STENCIL_BUFFER_BIT 0x0400
#define GL_NEVER 0x0200
#define GL_FRONT 0x0404
#define GL_BACK 0x0405
#define GL_FRONT_AND_BACK 0x0408
#define GL_CW 0x0900
#define GL_CCW 0x0901
#define GL_FUNC_ADD 0x8006
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_TEXTURE_SWIZZLE_R 0x8E42
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_REPEAT 0x2901
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_MIRRORED_REPEAT 0x8370
#define GL_RED 0x1903
#define GL_GREEN 0x1904
#define GL_BLUE 0x1905
#define GL_ALPHA 0x1906
#define GL_ZERO 0
#define GL_ONE 1
#define GL_TRANSFORM_FEEDBACK_BUFFER 0x8C8E
#define GL_INTERLEAVED_ATTRIBS 0x8C8C
#define GL_RASTERIZER_DISCARD 0x8C89
#define GL_MAP_READ_BIT 0x0001

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
    X(void, glFinish, (void))                                                                                          \
    X(void, glEnable, (GLenum))                                                                                        \
    X(void, glBlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum))                                                     \
    X(void, glBlendEquationSeparate, (GLenum, GLenum))                                                                 \
    X(void, glBlendColor, (GLfloat, GLfloat, GLfloat, GLfloat))                                                        \
    X(void, glColorMask, (GLboolean, GLboolean, GLboolean, GLboolean))                                                 \
    X(void, glDepthFunc, (GLenum))                                                                                     \
    X(void, glDepthMask, (GLboolean))                                                                                  \
    X(void, glStencilFuncSeparate, (GLenum, GLenum, GLint, GLuint))                                                    \
    X(void, glStencilOpSeparate, (GLenum, GLenum, GLenum, GLenum))                                                     \
    X(void, glStencilMaskSeparate, (GLenum, GLuint))                                                                   \
    X(void, glCullFace, (GLenum))                                                                                      \
    X(void, glFrontFace, (GLenum))                                                                                     \
    X(void, glScissor, (GLint, GLint, GLsizei, GLsizei))                                                               \
    X(void, glClearDepthf, (GLfloat))                                                                                  \
    X(void, glClearStencil, (GLint))                                                                                   \
    X(GLint, glGetUniformLocation, (GLuint, const GLchar *))                                                           \
    X(void, glUniform1i, (GLint, GLint))                                                                               \
    X(void, glActiveTexture, (GLenum))                                                                                 \
    X(void, glGenTextures, (GLsizei, GLuint *))                                                                        \
    X(void, glDeleteTextures, (GLsizei, const GLuint *))                                                               \
    X(void, glBindTexture, (GLenum, GLuint))                                                                           \
    X(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *))              \
    X(void, glTexParameteri, (GLenum, GLenum, GLint))                                                                  \
    X(void, glTransformFeedbackVaryings, (GLuint, GLsizei, const GLchar *const *, GLenum))                            \
    X(void, glBeginTransformFeedback, (GLenum))                                                                        \
    X(void, glEndTransformFeedback, (void))                                                                            \
    X(void *, glMapBufferRange, (GLenum, GLintptr, GLsizeiptr, GLbitfield))                                           \
    X(GLboolean, glUnmapBuffer, (GLenum))

#define MAX_PROGRAMS 256
#define MAX_ATTRS 16
#define MAX_SAMPLERS 16
#define MAX_TEXTURES 128

typedef struct attr {
    uint32_t fetch_constant, offset, format, stride, is_signed, is_integer;
    int exp_adjust;
} attr;

typedef struct program {
    uint32_t vs_crc, ps_crc;
    int kind;
    GLuint prog;
    int ok;
    int nattr;
    attr attrs[MAX_ATTRS];
    GLint loc[MAX_ATTRS];
    int nsamplers;
    uint32_t sampler_fetch[MAX_SAMPLERS], sampler_dim[MAX_SAMPLERS]; /* texture fetch constant, dimension */
} program;

/* a texture as last uploaded: its fetch constant's words (address, format, size, layout) and the source's hash */
typedef struct texture {
    uint32_t key[6];
    uint64_t hash;
    GLuint tex;
    int used;
} texture;

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
    GLuint fbo, rb, ds, vao, vbo, ibo, ubo[2], tfbo;
    int width, height;
    uint8_t *frame;
    char cache[512], frames[512];
    program progs[MAX_PROGRAMS];
    int nprogs;
    uint32_t missing[64];
    int nmissing;
    xenos_gl_stats st;
    texture textures[MAX_TEXTURES];
    unsigned tex_clock;
    int title_clears;
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
    g->glGenRenderbuffers(1, &g->ds);
    g->glBindRenderbuffer(GL_RENDERBUFFER, g->ds);
    g->glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    g->glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g->ds);
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
    g->glGenBuffers(1, &g->tfbo);
    g->frame = calloc((size_t)width * height, 4);
    g->glDisable(GL_DEPTH_TEST);
    g->glDisable(GL_BLEND);
    g->glDisable(GL_CULL_FACE);
    g->glDisable(GL_SCISSOR_TEST);
    g->glViewport(0, 0, width, height);
    g->glClearColor(0, 0, 0, 1);
    g->glClearDepthf(1.0f);
    g->glClearStencil(0);
    g->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
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
        g->glDeleteBuffers(1, &g->tfbo);
        for (int i = 0; i < MAX_TEXTURES; i++)
            if (g->textures[i].tex)
                g->glDeleteTextures(1, &g->textures[i].tex);
        g->glDeleteVertexArrays(1, &g->vao);
        g->glDeleteRenderbuffers(1, &g->rb);
        g->glDeleteRenderbuffers(1, &g->ds);
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
/* "uniform sampler2D xe_tK;" lines of both stages: the texture fetch constants the program samples */
static void parse_samplers(const char *src, program *p)
{
    static const char *const kinds[3] = {"sampler2D", "sampler3D", "samplerCube"};
    for (int kind = 0; kind < 3; kind++) {
        char pat[40];
        snprintf(pat, sizeof pat, "uniform %s xe_t", kinds[kind]);
        for (const char *s = strstr(src, pat); s && p->nsamplers < MAX_SAMPLERS; s = strstr(s + 1, pat)) {
            unsigned k = (unsigned)atoi(s + strlen(pat));
            int seen = 0;
            for (int i = 0; i < p->nsamplers; i++)
                seen |= p->sampler_fetch[i] == k;
            if (!seen) {
                p->sampler_fetch[p->nsamplers] = k;
                p->sampler_dim[p->nsamplers++] = kind == 0 ? 1 : kind == 1 ? 2 : 3;
            }
        }
    }
}

static int parse_attrs(const char *src, attr *out)
{
    int n = 0;
    for (const char *p = strstr(src, "in vec4 xe_v"); p && n < MAX_ATTRS; p = strstr(p + 1, "in vec4 xe_v")) {
        unsigned idx, fc, off, fmt, stride, sg, in = 0;
        int ex = 0;
        int got = sscanf(p, "in vec4 xe_v%u;  // fetch constant %u, offset %u, format %u, stride %u, signed %u, "
                            "integer %u, exp %d", &idx, &fc, &off, &fmt, &stride, &sg, &in, &ex);
        if (got >= 6 && idx == (unsigned)n) {
            out[n].fetch_constant = fc, out[n].offset = off, out[n].format = fmt, out[n].stride = stride;
            out[n].is_signed = sg, out[n].is_integer = got >= 8 ? in : 0, out[n].exp_adjust = got >= 8 ? ex : 0;
            n++;
        }
    }
    return n;
}

/* Rectangle lists: the vertex shader runs once with transform feedback (gl_Position and 15 interpolators: GLES 3.0
 * guarantees 64 interleaved components), the fourth corner of each rectangle is made on the CPU, and the result is
 * drawn as triangles through a pass-through vertex shader with the title's pixel shader. */
#define RECT_VARYINGS 16 /* gl_Position + xe_i0..14 */

static const char RECT_VS[] =
    "#version 300 es\n"
    "in vec4 xe_rp, xe_r0, xe_r1, xe_r2, xe_r3, xe_r4, xe_r5, xe_r6, xe_r7, xe_r8, xe_r9, xe_r10, xe_r11, xe_r12, "
    "xe_r13, xe_r14;\n"
    "out vec4 xe_i0, xe_i1, xe_i2, xe_i3, xe_i4, xe_i5, xe_i6, xe_i7, xe_i8, xe_i9, xe_i10, xe_i11, xe_i12, xe_i13, "
    "xe_i14, xe_i15;\n"
    "void main() { xe_i0 = xe_r0; xe_i1 = xe_r1; xe_i2 = xe_r2; xe_i3 = xe_r3; xe_i4 = xe_r4; xe_i5 = xe_r5; "
    "xe_i6 = xe_r6; xe_i7 = xe_r7; xe_i8 = xe_r8; xe_i9 = xe_r9; xe_i10 = xe_r10; xe_i11 = xe_r11; xe_i12 = xe_r12; "
    "xe_i13 = xe_r13; xe_i14 = xe_r14; xe_i15 = vec4(0.0); gl_Position = xe_rp; }\n";
static const char CAPTURE_FS[] =
    "#version 300 es\nprecision mediump float;\nout vec4 c;\nvoid main() { c = vec4(0.0); }\n";

/* kind 0: the title's two shaders; 1: its vertex shader with transform feedback; 2: the pass-through vertex shader
 * with its pixel shader (rectangle lists) */
static program *get_program(xenos_gl *g, const xenos_cp *cp, int kind)
{
    const xenos_shader *vs = &cp->shader[0], *ps = &cp->shader[1];
    if (!vs->words || !ps->words)
        return NULL;
    uint32_t vc = kind == 2 ? 0 : crc32_words(vs->code, vs->words);
    uint32_t pc = kind == 1 ? 0 : crc32_words(ps->code, ps->words);
    for (int i = 0; i < g->nprogs; i++)
        if (g->progs[i].vs_crc == vc && g->progs[i].ps_crc == pc && g->progs[i].kind == kind)
            return g->progs[i].ok ? &g->progs[i] : NULL;
    if (g->nprogs == MAX_PROGRAMS)
        return NULL;
    program *p = &g->progs[g->nprogs++];
    memset(p, 0, sizeof *p);
    p->vs_crc = vc, p->ps_crc = pc, p->kind = kind;
    char path[600], name[40];
    char *vsrc = NULL, *psrc = NULL;
    if (kind != 2) {
        snprintf(path, sizeof path, "%s/vs_%08X.vert", g->cache, vc);
        if (!(vsrc = slurp(path)))
            report_missing(g, 0, vc);
    }
    if (kind != 1) {
        snprintf(path, sizeof path, "%s/ps_%08X.frag", g->cache, pc);
        if (!(psrc = slurp(path)))
            report_missing(g, 1, pc);
    }
    const char *vtext = kind == 2 ? RECT_VS : vsrc, *ftext = kind == 1 ? CAPTURE_FS : psrc;
    if (vtext && ftext) {
        snprintf(name, sizeof name, kind == 2 ? "rect pass-through" : "vs_%08X", vc);
        GLuint v = compile(g, GL_VERTEX_SHADER, vtext, name);
        snprintf(name, sizeof name, kind == 1 ? "capture" : "ps_%08X", pc);
        GLuint f = compile(g, GL_FRAGMENT_SHADER, ftext, name);
        if (v && f) {
            p->prog = g->glCreateProgram();
            g->glAttachShader(p->prog, v);
            g->glAttachShader(p->prog, f);
            if (kind == 1) {
                static const char *const vary[RECT_VARYINGS] = {
                    "gl_Position", "xe_i0", "xe_i1", "xe_i2",  "xe_i3",  "xe_i4",  "xe_i5",  "xe_i6",
                    "xe_i7",       "xe_i8", "xe_i9", "xe_i10", "xe_i11", "xe_i12", "xe_i13", "xe_i14"};
                g->glTransformFeedbackVaryings(p->prog, RECT_VARYINGS, vary, GL_INTERLEAVED_ATTRIBS);
            }
            g->glLinkProgram(p->prog);
            GLint ok = 0;
            g->glGetProgramiv(p->prog, GL_LINK_STATUS, &ok);
            if (ok) {
                p->ok = 1;
                if (kind == 2) {
                    for (int i = 0; i < RECT_VARYINGS; i++) {
                        char an[16];
                        if (i == 0)
                            snprintf(an, sizeof an, "xe_rp");
                        else
                            snprintf(an, sizeof an, "xe_r%d", i - 1);
                        p->loc[i] = g->glGetAttribLocation(p->prog, an);
                    }
                } else {
                    p->nattr = parse_attrs(vsrc, p->attrs);
                    for (int i = 0; i < p->nattr; i++) {
                        char an[16];
                        snprintf(an, sizeof an, "xe_v%d", i);
                        p->loc[i] = g->glGetAttribLocation(p->prog, an);
                    }
                }
                if (vsrc)
                    parse_samplers(vsrc, p);
                if (psrc)
                    parse_samplers(psrc, p);
                g->glUseProgram(p->prog);
                for (int i = 0; i < p->nsamplers; i++) {
                    char un[16];
                    snprintf(un, sizeof un, "xe_t%u", p->sampler_fetch[i]);
                    GLint l = g->glGetUniformLocation(p->prog, un);
                    if (l >= 0)
                        g->glUniform1i(l, i);
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
                fprintf(g->log, "xenos_gl: vs_%08X + ps_%08X (kind %d) do not link: %s\n", vc, pc, kind, log);
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

static float half_to_float(uint32_t h)
{
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v;
    if (e == 0)
        v = (float)m / 16777216.0f; /* 2^-24 */
    else if (e == 31)
        v = m ? 0.0f / 0.0f : 1.0f / 0.0f;
    else
        v = (1.0f + (float)m / 1024.0f) * (float)(1u << e) / 32768.0f;
    return s ? -v : v;
}

/* a packed component: width bits at offset of v, normalised (or the integer) by the fetch's signedness */
static float component(uint32_t v, unsigned offset, unsigned width, const attr *a)
{
    uint32_t mask = width == 32 ? 0xFFFFFFFFu : (1u << width) - 1, u = (v >> offset) & mask;
    if (a->is_signed) {
        int64_t sv = (u & (1u << (width - 1))) ? (int64_t)u - ((int64_t)1 << width) : (int64_t)u;
        if (a->is_integer)
            return (float)sv;
        float f = (float)sv / (float)(((int64_t)1 << (width - 1)) - 1);
        return f < -1.0f ? -1.0f : f; /* Microsoft's: two representations of -1 */
    }
    return a->is_integer ? (float)u : (float)u / (float)mask;
}

/* one attribute of one vertex as a vec4 (Xenia's spirv_shader_translator_fetch.cc gives the packings: X in the low
 * bits); 0 when the format is not handled */
static int fetch_vertex(const xenos_cp *cp, uint32_t base, uint32_t endian, const attr *a, uint32_t index,
                        float out[4])
{
    uint32_t at = base + 4 * (a->offset + index * a->stride);
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    uint32_t w0 = gpu_read(cp, at, endian);
    switch (a->format) {
    case 36: /* 32_FLOAT */
    case 37: /* 32_32_FLOAT */
    case 57: /* 32_32_32_FLOAT */
    case 38: { /* 32_32_32_32_FLOAT */
        int n = a->format == 36 ? 1 : a->format == 37 ? 2 : a->format == 57 ? 3 : 4;
        for (int i = 0; i < n; i++)
            out[i] = as_float(gpu_read(cp, at + 4 * i, endian));
        break;
    }
    case 6: /* 8_8_8_8 */
        for (int i = 0; i < 4; i++)
            out[i] = component(w0, 8 * i, 8, a);
        break;
    case 7: /* 2_10_10_10 */
        for (int i = 0; i < 3; i++)
            out[i] = component(w0, 10 * i, 10, a);
        out[3] = component(w0, 30, 2, a);
        break;
    case 16: /* 10_11_11: 11, 11, 10 bits from X */
        out[0] = component(w0, 0, 11, a), out[1] = component(w0, 11, 11, a), out[2] = component(w0, 22, 10, a);
        break;
    case 17: /* 11_11_10: 10, 11, 11 bits from X */
        out[0] = component(w0, 0, 10, a), out[1] = component(w0, 10, 11, a), out[2] = component(w0, 21, 11, a);
        break;
    case 25: /* 16_16 */
    case 26: { /* 16_16_16_16 */
        int n = a->format == 25 ? 2 : 4;
        for (int i = 0; i < n; i++)
            out[i] = component(i < 2 ? w0 : gpu_read(cp, at + 4, endian), 16 * (i & 1), 16, a);
        break;
    }
    case 31: /* 16_16_FLOAT */
    case 32: { /* 16_16_16_16_FLOAT */
        int n = a->format == 31 ? 2 : 4;
        for (int i = 0; i < n; i++)
            out[i] = half_to_float(((i < 2 ? w0 : gpu_read(cp, at + 4, endian)) >> (16 * (i & 1))) & 0xFFFF);
        return 1; /* exp_adjust does not apply to floats */
    }
    case 33: /* 32 */
    case 34: /* 32_32 */
    case 35: { /* 32_32_32_32 */
        int n = a->format == 33 ? 1 : a->format == 34 ? 2 : 4;
        for (int i = 0; i < n; i++)
            out[i] = component(gpu_read(cp, at + 4 * i, endian), 0, 32, a);
        break;
    }
    default:
        return 0;
    }
    if (a->exp_adjust && a->format != 36 && a->format != 37 && a->format != 57 && a->format != 38) {
        float k = a->exp_adjust > 0 ? (float)(1u << a->exp_adjust) : 1.0f / (float)(1u << -a->exp_adjust);
        for (int i = 0; i < 4; i++)
            out[i] *= k;
    }
    return 1;
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
    case 13: return 4; /* quad list: drawn as two triangles per quad */
    case 8: return 4;  /* rectangle list: the fourth corners made after the vertex shader, then triangles */
    default: return -1; /* line loops, quad strips, polygons: not yet */
    }
}

static void upload_constants(xenos_gl *g, const xenos_cp *cp)
{
    uint8_t block[4096 + 32 + 128 + 32];
    uint32_t cc = cp->regs[0x2202];
    float at[4] = {(cc >> 3) & 1 ? 1.0f : 0.0f, (float)(cc & 7), as_float(cp->regs[0x210E]), 0.0f};
    /* SQ_PROGRAM_CNTL's param_gen, SQ_CONTEXT_MISC's register for it */
    float pg[4] = {(cp->regs[0x2180] >> 18) & 1 ? (float)((cp->regs[0x2181] >> 8) & 0xFF) : -1.0f, (float)g->height,
                   0.0f, 0.0f};
    for (int stage = 0; stage < 2; stage++) {
        memcpy(block, &cp->regs[0x4000 + 0x400 * stage], 4096); /* float constants: register values are the bits */
        memcpy(block + 4096, &cp->regs[0x4900], 32);
        memcpy(block + 4096 + 32, &cp->regs[0x4908], 128);
        memcpy(block + 4096 + 32 + 128, at, 16); /* RB_COLORCONTROL's alpha test, RB_ALPHA_REF */
        memcpy(block + 4096 + 32 + 128 + 16, pg, 16);
        g->glBindBuffer(GL_UNIFORM_BUFFER, g->ubo[stage]);
        g->glBufferData(GL_UNIFORM_BUFFER, sizeof block, block, GL_STREAM_DRAW);
        g->glBindBufferBase(GL_UNIFORM_BUFFER, (GLuint)stage, g->ubo[stage]);
    }
}

/* ---- render state (Xenia's registers.h layouts, GL's enums) ---- */

static GLenum blend_factor(uint32_t f)
{
    static const GLenum map[17] = {0, 1, 1, 1, 0x0300, 0x0301, 0x0302, 0x0303, 0x0306, 0x0307, 0x0304, 0x0305,
                                   0x8001, 0x8002, 0x8003, 0x8004, 0x0308};
    return f < 17 ? map[f] : 1;
}

static GLenum blend_op(uint32_t o)
{
    static const GLenum map[5] = {0x8006, 0x800A, 0x8007, 0x8008, 0x800B};
    return o < 5 ? map[o] : 0x8006;
}

static GLenum stencil_op(uint32_t o)
{
    static const GLenum map[8] = {0x1E00, 0, 0x1E01, 0x1E02, 0x1E03, 0x150A, 0x8507, 0x8508};
    return map[o & 7];
}

static void apply_state(xenos_gl *g, const xenos_cp *cp)
{
    /* blending: off when it is src * 1 + dst * 0 for colour and alpha */
    uint32_t bc = cp->regs[0x2201];
    if ((bc & 0x1FFF1FFFu) == 0x00010001u) {
        g->glDisable(GL_BLEND);
    } else {
        g->glEnable(GL_BLEND);
        g->glBlendFuncSeparate(blend_factor(bc & 31), blend_factor((bc >> 8) & 31), blend_factor((bc >> 16) & 31),
                               blend_factor((bc >> 24) & 31));
        g->glBlendEquationSeparate(blend_op((bc >> 5) & 7), blend_op((bc >> 21) & 7));
        g->glBlendColor(as_float(cp->regs[0x2105]), as_float(cp->regs[0x2106]), as_float(cp->regs[0x2107]),
                        as_float(cp->regs[0x2108]));
    }
    uint32_t cm = cp->regs[0x2104];
    g->glColorMask(cm & 1, (cm >> 1) & 1, (cm >> 2) & 1, (cm >> 3) & 1);
    /* depth and stencil */
    uint32_t dc = cp->regs[0x2200];
    if (dc & 2) {
        g->glEnable(GL_DEPTH_TEST);
        g->glDepthFunc(GL_NEVER + ((dc >> 4) & 7));
    } else {
        g->glDisable(GL_DEPTH_TEST);
    }
    g->glDepthMask((dc >> 2) & 1);
    if (dc & 1) {
        uint32_t rm = cp->regs[0x210D], rb = (dc >> 7) & 1 ? cp->regs[0x210C] : rm;
        uint32_t fb = (dc >> 7) & 1 ? dc >> 20 : dc >> 8, fail_b = (dc >> 7) & 1 ? dc >> 23 : dc >> 11;
        uint32_t zpass_b = (dc >> 7) & 1 ? dc >> 26 : dc >> 14, zfail_b = (dc >> 7) & 1 ? dc >> 29 : dc >> 17;
        g->glEnable(GL_STENCIL_TEST);
        g->glStencilFuncSeparate(GL_FRONT, GL_NEVER + ((dc >> 8) & 7), (GLint)(rm & 0xFF), (rm >> 8) & 0xFF);
        g->glStencilOpSeparate(GL_FRONT, stencil_op(dc >> 11), stencil_op(dc >> 17), stencil_op(dc >> 14));
        g->glStencilMaskSeparate(GL_FRONT, (rm >> 16) & 0xFF);
        g->glStencilFuncSeparate(GL_BACK, GL_NEVER + (fb & 7), (GLint)(rb & 0xFF), (rb >> 8) & 0xFF);
        g->glStencilOpSeparate(GL_BACK, stencil_op(fail_b), stencil_op(zfail_b), stencil_op(zpass_b));
        g->glStencilMaskSeparate(GL_BACK, (rb >> 16) & 0xFF);
    } else {
        g->glDisable(GL_STENCIL_TEST);
    }
    /* culling: Xenos's winding is as seen on the render target (Xenia: 0 = front is CCW), and the target here is
     * the same picture GL's window holds (NDC +y up in both) */
    uint32_t sc = cp->regs[0x2205];
    if (sc & 3) {
        g->glEnable(GL_CULL_FACE);
        g->glCullFace((sc & 3) == 3 ? GL_FRONT_AND_BACK : (sc & 1) ? GL_FRONT : GL_BACK);
    } else {
        g->glDisable(GL_CULL_FACE);
    }
    g->glFrontFace((sc >> 2) & 1 ? GL_CW : GL_CCW);
    /* the window scissor (top-left / bottom-right, y downwards), when it is set and smaller than the target */
    uint32_t tl = cp->regs[0x2081], br = cp->regs[0x2082];
    int x0 = (int)(tl & 0x7FFF), y0 = (int)((tl >> 16) & 0x7FFF);
    int x1 = (int)(br & 0x7FFF), y1 = (int)((br >> 16) & 0x7FFF);
    if (br && (x0 > 0 || y0 > 0 || x1 < g->width || y1 < g->height) && x1 > x0 && y1 > y0) {
        g->glEnable(GL_SCISSOR_TEST);
        g->glScissor(x0, g->height - y1, x1 - x0, y1 - y0);
    } else {
        g->glDisable(GL_SCISSOR_TEST);
    }
}

/* ---- textures ----
 * The texture fetch constant (six dwords from 0x4800 + 6 * index; Xenia's xenos.h): dword 0 type, signs, clamps,
 * pitch (texels >> 5), tiled; dword 1 format, endian swap, base address >> 12; dword 2 width - 1, height - 1;
 * dword 3 swizzle (3 bits per component: X, Y, Z, W, 0, 1), filters. Tiled textures use the Xenos 2D tiling:
 * 32x32-block macro tiles, banks and pipes (the address formula of Xenia's texture_address.h, Tiled2D). Formats
 * here: 8, 8_8, 8_8_8_8, 1_5_5_5, 5_6_5, 4_4_4_4, DXT1, DXT2/3, DXT4/5, decoded to RGBA8 on the CPU with X in red;
 * the fetch constant's swizzle goes to GL's texture swizzle. Base level only (no mips), 2D (and 1D) only. */

typedef struct texfmt {
    uint32_t format, block, bytes; /* block size in texels (1 or 4), bytes per block */
} texfmt;

static const texfmt *tex_format(uint32_t f)
{
    static const texfmt fmts[] = {{2, 1, 1},  {10, 1, 2}, {6, 1, 4},   {3, 1, 2},  {4, 1, 2},
                                  {15, 1, 2}, {18, 4, 8}, {19, 4, 16}, {20, 4, 16}};
    for (size_t i = 0; i < sizeof fmts / sizeof fmts[0]; i++)
        if (fmts[i].format == f)
            return &fmts[i];
    return NULL;
}

static int32_t tiled2d(int32_t x, int32_t y, uint32_t pitch_blocks, unsigned bpb_log2)
{
    int32_t outer = ((y >> 5) * (int32_t)(pitch_blocks >> 5) + (x >> 5)) << 6;
    int32_t inner = (((y >> 1) & 7) << 3) | (x & 7);
    int32_t oi = (outer | inner) << bpb_log2;
    uint32_t bank = (y >> 4) & 1, pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1);
    return (int32_t)((uint32_t)(y & 1) << 4 | pipe << 6 | bank << 11) | (oi & 15) | ((oi >> 4) & 1) << 5 |
           ((oi >> 5) & 7) << 8 | (oi >> 8) << 12;
}

static void unpack_565(uint32_t v, uint8_t out[4])
{
    out[0] = (uint8_t)(((v & 31) * 255 + 15) / 31);
    out[1] = (uint8_t)((((v >> 5) & 63) * 255 + 31) / 63);
    out[2] = (uint8_t)((((v >> 11) & 31) * 255 + 15) / 31);
    out[3] = 255;
}

/* one 4x4 block of a DXT format at `b` (bytes after the endian swap) to 16 RGBA texels; colours are 5:6:5 with
 * the 5 low bits first (X) */
static void dxt_block(const uint8_t *b, uint32_t format, uint8_t out[16][4])
{
    const uint8_t *cb = format == 18 ? b : b + 8;
    uint32_t c0 = cb[0] | cb[1] << 8, c1 = cb[2] | cb[3] << 8, bits = cb[4] | cb[5] << 8 | cb[6] << 16 |
                                                                        (uint32_t)cb[7] << 24;
    uint8_t pal[4][4];
    unpack_565(c0, pal[0]);
    unpack_565(c1, pal[1]);
    int three = format == 18 && c0 <= c1;
    for (int k = 0; k < 3; k++) {
        if (three) {
            pal[2][k] = (uint8_t)((pal[0][k] + pal[1][k]) / 2);
            pal[3][k] = 0;
        } else {
            pal[2][k] = (uint8_t)((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = (uint8_t)((pal[0][k] + 2 * pal[1][k]) / 3);
        }
    }
    pal[2][3] = 255;
    pal[3][3] = three ? 0 : 255;
    for (int i = 0; i < 16; i++)
        memcpy(out[i], pal[(bits >> (2 * i)) & 3], 4);
    if (format == 19) { /* explicit 4-bit alpha */
        for (int i = 0; i < 16; i++) {
            uint32_t a = (b[i / 2] >> (4 * (i & 1))) & 15;
            out[i][3] = (uint8_t)(a * 17);
        }
    } else if (format == 20) { /* interpolated alpha */
        uint32_t a0 = b[0], a1 = b[1];
        uint64_t ab = 0;
        for (int k = 0; k < 6; k++)
            ab |= (uint64_t)b[2 + k] << (8 * k);
        uint32_t ap[8] = {a0, a1};
        for (int k = 2; k < 8; k++)
            ap[k] = a0 > a1 ? ((8 - k) * a0 + (k - 1) * a1) / 7
                    : k < 6  ? ((6 - k) * a0 + (k - 1) * a1) / 5
                    : k == 6 ? 0
                             : 255;
        for (int i = 0; i < 16; i++)
            out[i][3] = (uint8_t)ap[(ab >> (3 * i)) & 7];
    }
}

static void texel(uint32_t format, const uint8_t *s, uint8_t out[4])
{
    uint32_t v = s[0] | (uint32_t)s[1] << 8;
    switch (format) {
    case 2: out[0] = out[1] = out[2] = out[3] = s[0]; break; /* 8: replicated, the swizzle picks */
    case 10: out[0] = s[0], out[1] = s[1], out[2] = 0, out[3] = 255; break;
    case 6: memcpy(out, s, 4); break;
    case 3:
        out[0] = (uint8_t)((v & 31) * 255 / 31), out[1] = (uint8_t)(((v >> 5) & 31) * 255 / 31);
        out[2] = (uint8_t)(((v >> 10) & 31) * 255 / 31), out[3] = (v >> 15) ? 255 : 0;
        break;
    case 4: unpack_565(v, out); break;
    case 15:
        for (int k = 0; k < 4; k++)
            out[k] = (uint8_t)(((v >> (4 * k)) & 15) * 17);
        break;
    default: memset(out, 0, 4);
    }
}

static uint64_t fnv(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

/* the texture behind fetch constant k as a GL texture (decoded again when its memory changed); 0 if unsupported */
static GLuint texture_of(xenos_gl *g, const xenos_cp *cp, uint32_t k)
{
    const uint32_t *fc = &cp->regs[0x4800 + 6 * k];
    const texfmt *tf = tex_format(fc[1] & 63);
    uint32_t dim = (fc[5] >> 9) & 3; /* 0 1D, 1 2D, 2 3D, 3 cube */
    if ((fc[0] & 3) != 2 || !tf || dim > 1)
        return 0;
    uint32_t w = (dim == 0 ? fc[2] & 0xFFFFFF : fc[2] & 0x1FFF) + 1, h = dim == 0 ? 1 : ((fc[2] >> 13) & 0x1FFF) + 1;
    if (w > 8192 || h > 8192)
        return 0;
    uint32_t endian = (fc[1] >> 6) & 3, base = fc[1] & 0xFFFFF000u, tiled = fc[0] >> 31;
    uint32_t pitch = ((fc[0] >> 22) & 0x1FF) << 5; /* texels */
    if (!pitch)
        pitch = w;
    uint32_t bw = (w + tf->block - 1) / tf->block, bh = (h + tf->block - 1) / tf->block;
    uint32_t pitch_blocks = (pitch + tf->block - 1) / tf->block;
    unsigned bpb_log2 = tf->bytes == 1 ? 0 : tf->bytes == 2 ? 1 : tf->bytes == 4 ? 2 : tf->bytes == 8 ? 3 : 4;
    uint32_t row_bytes = (pitch_blocks * tf->bytes + 255) & ~255u; /* linear rows: 256-byte aligned */
    uint32_t pitch_tiled = (pitch_blocks + 31) & ~31u;
    /* the source bytes, block by block, after the endian swap */
    size_t src_n = (size_t)bw * bh * tf->bytes;
    uint8_t *src = malloc(src_n);
    uint8_t *rgba = malloc((size_t)bw * tf->block * bh * tf->block * 4);
    if (!src || !rgba) {
        free(src);
        free(rgba);
        return 0;
    }
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            uint32_t off = tiled ? (uint32_t)tiled2d((int32_t)bx, (int32_t)by, pitch_tiled, bpb_log2)
                                 : by * row_bytes + bx * tf->bytes;
            uint8_t *d = src + ((size_t)by * bw + bx) * tf->bytes;
            for (uint32_t i = 0; i < tf->bytes; i++) {
                uint32_t a = base + off + i, word = gpu_read(cp, a & ~3u, endian);
                d[i] = (uint8_t)(word >> (8 * (a & 3)));
            }
        }
    uint64_t hash = fnv(src, src_n) ^ ((uint64_t)fc[3] << 32);
    texture *t = NULL, *victim = &g->textures[0];
    for (int i = 0; i < MAX_TEXTURES; i++) {
        texture *c = &g->textures[i];
        if (c->tex && !memcmp(c->key, fc, sizeof c->key)) {
            t = c;
            break;
        }
        if (!c->tex || (victim->tex && c->used < victim->used))
            victim = c;
    }
    if (t && t->hash == hash) {
        t->used = (int)++g->tex_clock;
        free(src);
        free(rgba);
        return t->tex;
    }
    if (!t) {
        t = victim;
        if (!t->tex)
            g->glGenTextures(1, &t->tex);
        memcpy(t->key, fc, sizeof t->key);
    }
    t->hash = hash;
    t->used = (int)++g->tex_clock;
    uint32_t tw = bw * tf->block;
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            const uint8_t *s = src + ((size_t)by * bw + bx) * tf->bytes;
            if (tf->block == 4) {
                uint8_t px[16][4];
                dxt_block(s, tf->format, px);
                for (int i = 0; i < 16; i++)
                    memcpy(rgba + (((size_t)by * 4 + i / 4) * tw + bx * 4 + i % 4) * 4, px[i], 4);
            } else {
                texel(tf->format, s, rgba + ((size_t)by * tw + bx) * 4);
            }
        }
    g->glBindTexture(GL_TEXTURE_2D, t->tex);
    g->glTexImage2D(GL_TEXTURE_2D, 0, (GLint)GL_RGBA8, (GLsizei)tw, (GLsizei)(bh * tf->block), 0, GL_RGBA,
                    GL_UNSIGNED_BYTE, rgba);
    free(src);
    free(rgba);
    return t->tex;
}

static GLint wrap_mode(uint32_t c)
{
    return c == 0 ? GL_REPEAT : c == 1 ? GL_MIRRORED_REPEAT : GL_CLAMP_TO_EDGE;
}

static void bind_textures(xenos_gl *g, const xenos_cp *cp, const program *p)
{
    static const GLint swz[6] = {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA, GL_ZERO, GL_ONE};
    for (int i = 0; i < p->nsamplers; i++) {
        uint32_t k = p->sampler_fetch[i];
        GLuint tex = p->sampler_dim[i] <= 1 && k < 32 ? texture_of(g, cp, k) : 0;
        g->glActiveTexture(GL_TEXTURE0 + (GLenum)i);
        g->glBindTexture(GL_TEXTURE_2D, tex);
        if (!tex) {
            g->st.unsupported_textures++;
            continue;
        }
        const uint32_t *fc = &cp->regs[0x4800 + 6 * k];
        g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap_mode((fc[0] >> 10) & 7));
        g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap_mode((fc[0] >> 13) & 7));
        g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, ((fc[3] >> 19) & 3) == 1 ? GL_LINEAR : GL_NEAREST);
        g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, ((fc[3] >> 21) & 3) == 1 ? GL_LINEAR : GL_NEAREST);
        for (int c = 0; c < 4; c++) {
            uint32_t s = (fc[3] >> (1 + 3 * c)) & 7;
            g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R + (GLenum)c, s < 6 ? swz[s] : GL_ZERO);
        }
    }
    g->glActiveTexture(GL_TEXTURE0);
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

/* ---- resolves ----
 * A draw with RB_MODECONTROL's eDRAM mode "copy" is a resolve (Xenia's draw_util.cc GetResolveInfo): the rectangle
 * comes from the three window-space vertices Direct3D 9 leaves at vertex fetch constant 0; RB_COPY_CONTROL says
 * whether the colour is copied out (raw / convert) and whether colour and depth are cleared (RB_COLOR_CLEAR as a
 * D3DCOLOR, RB_DEPTH_CLEAR as depth 24 : stencil 8); the copy goes to RB_COPY_DEST_BASE as a tiled texture of
 * RB_COPY_DEST_PITCH (pitch, height) in RB_COPY_DEST_INFO's format and endian swap, at the same pixel coordinates as
 * the source (Direct3D adjusts the base for offsets). Formats copied here: 8_8_8_8 and 5_6_5. */

static void resolve(xenos_gl *g, const xenos_cp *cp)
{
    uint32_t c0 = cp->regs[0x4800], c1 = cp->regs[0x4801];
    float v[6];
    for (int i = 0; i < 6; i++)
        v[i] = as_float(gpu_read(cp, (c0 & ~3u) + 4 * (uint32_t)i, c1 & 3));
    float fx0 = v[0], fx1 = v[0], fy0 = v[1], fy1 = v[1];
    for (int i = 1; i < 3; i++) {
        fx0 = v[2 * i] < fx0 ? v[2 * i] : fx0, fx1 = v[2 * i] > fx1 ? v[2 * i] : fx1;
        fy0 = v[2 * i + 1] < fy0 ? v[2 * i + 1] : fy0, fy1 = v[2 * i + 1] > fy1 ? v[2 * i + 1] : fy1;
    }
    int x0 = (int)(fx0 + 0.5f), y0 = (int)(fy0 + 0.5f), x1 = (int)(fx1 + 0.5f), y1 = (int)(fy1 + 0.5f);
    x0 = x0 < 0 ? 0 : x0, y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > g->width ? g->width : x1, y1 = y1 > g->height ? g->height : y1;
    if (x1 <= x0 || y1 <= y0) {
        g->st.unsupported++;
        return;
    }
    uint32_t ctl = cp->regs[0x2318], cmd = (ctl >> 20) & 3, src = ctl & 7;
    g->glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    if (cmd <= 1 && src < 4) {
        uint32_t info = cp->regs[0x231B], fmt = (info >> 7) & 63, endian = info & 7, swap_rb = (info >> 24) & 1;
        uint32_t pitch = cp->regs[0x231A] & 0x3FFF, base = cp->regs[0x2319];
        uint32_t pitch_aligned = (pitch + 31) & ~31u;
        unsigned bpp_log2 = fmt == 6 ? 2 : fmt == 4 ? 1 : 0;
        uint8_t *px = malloc((size_t)(x1 - x0) * (y1 - y0) * 4);
        if ((fmt == 6 || fmt == 4) && endian < 4 && px && cp->write32 && cp->read32) {
            g->glReadPixels(x0, g->height - y1, x1 - x0, y1 - y0, GL_RGBA, GL_UNSIGNED_BYTE, px);
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) {
                    const uint8_t *p = px + ((size_t)(y1 - 1 - y) * (x1 - x0) + (x - x0)) * 4; /* GL rows bottom up */
                    uint8_t r = swap_rb ? p[2] : p[0], b = swap_rb ? p[0] : p[2];
                    uint32_t off = (uint32_t)tiled2d(x, y, pitch_aligned, bpp_log2), a = base + off;
                    uint32_t value, word_addr = a & ~3u, word;
                    if (fmt == 6) {
                        value = r | (uint32_t)p[1] << 8 | (uint32_t)b << 16 | (uint32_t)p[3] << 24;
                        word = value;
                    } else { /* 5_6_5, X in the low bits, two per dword */
                        value = (uint32_t)(r * 31 + 127) / 255 | (uint32_t)(p[1] * 63 + 127) / 255 << 5 |
                                (uint32_t)(b * 31 + 127) / 255 << 11;
                        word = gpu_read(cp, word_addr, endian);
                        uint32_t sh = 8 * (a & 2);
                        word = (word & ~(0xFFFFu << sh)) | value << sh;
                    }
                    /* store the bytes the GPU reads back as `word` through the same swap */
                    cp->write32(cp->ctx, word_addr & 0x1FFFFFFFu, __builtin_bswap32(gpu_swap(word, endian)));
                }
            g->st.resolves++;
        } else {
            g->st.unsupported++;
        }
        free(px);
    }
    uint32_t clear = 0;
    if ((ctl >> 8) & 1) {
        uint32_t cc = cp->regs[0x231E];
        g->glClearColor(((cc >> 16) & 0xFF) / 255.0f, ((cc >> 8) & 0xFF) / 255.0f, (cc & 0xFF) / 255.0f,
                        (cc >> 24) / 255.0f);
        clear |= GL_COLOR_BUFFER_BIT;
    }
    if ((ctl >> 9) & 1) {
        uint32_t dc = cp->regs[0x231D];
        g->glClearDepthf((float)(dc >> 8) / 16777215.0f);
        g->glClearStencil((GLint)(dc & 0xFF));
        clear |= GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT;
    }
    if (clear) {
        g->glEnable(GL_SCISSOR_TEST);
        g->glScissor(x0, g->height - y1, x1 - x0, y1 - y0);
        g->glColorMask(1, 1, 1, 1);
        g->glDepthMask(1);
        g->glStencilMaskSeparate(GL_FRONT_AND_BACK, 0xFF);
        g->glClear(clear);
        g->glDisable(GL_SCISSOR_TEST);
        g->title_clears = 1; /* the title clears through resolves: swaps stop clearing */
    }
}

/* the capture program is bound with its attributes: run it on the n vertices into the feedback buffer, make each
 * rectangle's fourth corner (opposite the right angle: the vertex off the longest edge is the corner, the fourth is
 * the sum of the other two minus it, for every captured component), draw two triangles per rectangle */
static void draw_rects(xenos_gl *g, const xenos_cp *cp, const program *rp, const uint32_t *idx, uint32_t n)
{
    enum { F = RECT_VARYINGS * 4 };
    uint32_t nr = n / 3;
    if (!nr)
        return;
    g->glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, g->tfbo);
    g->glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, (GLsizeiptr)nr * 3 * F * 4, NULL, GL_STREAM_DRAW);
    g->glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, g->tfbo);
    g->glEnable(GL_RASTERIZER_DISCARD);
    g->glBeginTransformFeedback(0 /* GL_POINTS */);
    if (idx) {
        g->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g->ibo);
        g->glBufferData(GL_ELEMENT_ARRAY_BUFFER, 4 * (GLsizeiptr)nr * 3, idx, GL_STREAM_DRAW);
        g->glDrawElements(0, (GLsizei)(nr * 3), GL_UNSIGNED_INT, NULL);
    } else {
        g->glDrawArrays(0, 0, (GLsizei)(nr * 3));
    }
    g->glEndTransformFeedback();
    g->glDisable(GL_RASTERIZER_DISCARD);
    const float *cap =
        g->glMapBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0, (GLsizeiptr)nr * 3 * F * 4, GL_MAP_READ_BIT);
    float *tri = cap ? malloc((size_t)nr * 6 * F * 4) : NULL;
    if (!tri) {
        if (cap)
            g->glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER);
        g->st.unsupported++;
        return;
    }
    for (uint32_t r = 0; r < nr; r++) {
        const float *v[3] = {cap + (3 * r) * F, cap + (3 * r + 1) * F, cap + (3 * r + 2) * F};
        float sx[3], sy[3];
        for (int k = 0; k < 3; k++) {
            float w = v[k][3] != 0.0f ? v[k][3] : 1.0f;
            sx[k] = v[k][0] / w, sy[k] = v[k][1] / w;
        }
        int corner = 0;
        float best = -1.0f;
        for (int k = 0; k < 3; k++) { /* the edge opposite vertex k */
            int a = (k + 1) % 3, b = (k + 2) % 3;
            float dx = sx[a] - sx[b], dy = sy[a] - sy[b], len = dx * dx + dy * dy;
            if (len > best)
                best = len, corner = k;
        }
        const float *c = v[corner], *a = v[(corner + 1) % 3], *b = v[(corner + 2) % 3];
        float *o = tri + (size_t)r * 6 * F;
        memcpy(o, c, F * 4);
        memcpy(o + F, a, F * 4);
        memcpy(o + 2 * F, b, F * 4);
        memcpy(o + 3 * F, a, F * 4);
        memcpy(o + 4 * F, b, F * 4);
        for (int i = 0; i < F; i++)
            o[5 * F + i] = a[i] + b[i] - c[i];
    }
    g->glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER);
    g->glUseProgram(rp->prog);
    bind_textures(g, cp, rp);
    g->glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
    g->glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)nr * 6 * F * 4, tri, GL_STREAM_DRAW);
    for (int i = 0; i < RECT_VARYINGS; i++)
        if (rp->loc[i] >= 0) {
            g->glEnableVertexAttribArray((GLuint)rp->loc[i]);
            g->glVertexAttribPointer((GLuint)rp->loc[i], 4, GL_FLOAT, 0, F * 4, (void *)(uintptr_t)(i * 16));
        }
    g->glDrawArrays(4 /* GL_TRIANGLES */, 0, (GLsizei)(nr * 6));
    for (int i = 0; i < RECT_VARYINGS; i++)
        if (rp->loc[i] >= 0)
            g->glDisableVertexAttribArray((GLuint)rp->loc[i]);
    free(tri);
}

void xenos_gl_draw(xenos_gl *g, const xenos_cp *cp, const xenos_draw *d)
{
    if ((cp->regs[0x2208] & 7) == 6) { /* eDRAM mode "copy": a resolve */
        resolve(g, cp);
        return;
    }
    int prim = gl_prim(d->prim_type);
    if (prim < 0 || d->source_select == 1 || !d->num_indices) {
        g->st.unsupported++;
        return;
    }
    program *p = get_program(g, cp, d->prim_type == 8 ? 1 : 0);
    program *rp = d->prim_type == 8 ? get_program(g, cp, 2) : NULL;
    if (!p || (d->prim_type == 8 && !rp)) {
        g->st.no_shader++;
        return;
    }
    /* indices: from the DMA buffer (16 or 32 bits, VGT_DMA_SIZE's swap mode) or 0..n-1 */
    uint32_t n = d->num_indices, *idx = NULL, max_index = n - 1;
    if (d->prim_type == 13 && (n /= 4) * 4 == 0) {
        g->st.unsupported++;
        return;
    } else if (d->prim_type == 13) {
        n *= 4; /* whole quads */
    }
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
    if (d->prim_type == 13) { /* quads (a, b, c, d) -> triangles (a, b, c), (a, c, d) */
        uint32_t *t = malloc(6 * (size_t)(n / 4) * 4);
        if (!t) {
            free(idx);
            return;
        }
        for (uint32_t q = 0; q < n / 4; q++) {
            uint32_t v[4];
            for (int k = 0; k < 4; k++)
                v[k] = idx ? idx[4 * q + k] : 4 * q + k;
            uint32_t *o = &t[6 * q];
            o[0] = v[0], o[1] = v[1], o[2] = v[2], o[3] = v[0], o[4] = v[2], o[5] = v[3];
        }
        free(idx);
        idx = t;
        n = n / 4 * 6;
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
    apply_state(g, cp);
    bind_textures(g, cp, p);
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
    if (d->prim_type == 8) {
        draw_rects(g, cp, rp, idx, n);
    } else if (idx) {
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
    if (g->title_clears)
        return; /* the title clears through its resolves */
    /* until the title clears through a resolve, each frame starts clear */
    g->glViewport(0, 0, g->width, g->height);
    g->glDisable(GL_SCISSOR_TEST);
    g->glColorMask(1, 1, 1, 1);
    g->glDepthMask(1);
    g->glStencilMaskSeparate(GL_FRONT_AND_BACK, 0xFF);
    g->glClearColor(0, 0, 0, 1);
    g->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}
