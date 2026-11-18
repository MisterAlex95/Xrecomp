/* xenos_gl.c -- see xenos_gl.h. Register layouts follow Xenia's register_table.inc / xenos.h (BSD): the vertex fetch
 * constants (two dwords each from 0x4800: type and dword address, then endian swap and size), the float constants
 * (0x4000 vertex, 0x4400 pixel, four per vec4), booleans (0x4900, eight words), loops (0x4908, 32 words), the draw
 * initiator's primitive types, VGT_DMA_SIZE's swap mode, PA_CL_VTE_CNTL and the viewport scale / offset. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <math.h>
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
#define GL_PRIMITIVE_RESTART_FIXED_INDEX 0x8D69
#define GL_TEXTURE_3D 0x806F
#define GL_TEXTURE_CUBE_MAP 0x8513
#define GL_TEXTURE_CUBE_MAP_POSITIVE_X 0x8515
#define GL_TEXTURE_WRAP_R 0x8072

#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_TEXTURE_MAX_LEVEL 0x813D
#define GL_TEXTURE_BASE_LEVEL 0x813C
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_LINEAR_MIPMAP_NEAREST 0x2701
#define GL_NEAREST_MIPMAP_LINEAR 0x2702
#define GL_SRGB8_ALPHA8 0x8C43
#define GL_RGB10_A2 0x8059
#define GL_RGBA16F 0x881A
#define GL_RG16F 0x822F
#define GL_R32F 0x822E
#define GL_RG32F 0x8230
#define GL_RGBA32F 0x8814
#define GL_RG 0x8227
#define GL_HALF_FLOAT 0x140B
#define GL_UNSIGNED_INT_2_10_10_10_REV 0x8368
#define GL_DEPTH_STENCIL 0x84F9
#define GL_UNSIGNED_INT_24_8 0x84FA
#define GL_MAX_TEXTURE_SIZE 0x0D33
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES3_BIT 0x0040
#define EGL_NONE 0x3038
#define EGL_CONTEXT_MAJOR_VERSION 0x3098
#define EGL_CONTEXT_MINOR_VERSION 0x30FB
#define EGL_OPENGL_ES_API 0x30A0

#define GLFUNCS(X)                                                                                                     \
    X(const unsigned char *, glGetString, (GLenum))                                                                   \
    X(const unsigned char *, glGetStringi, (GLenum, GLuint))                                                          \
    X(void, glTexParameterf, (GLenum, GLenum, GLfloat))                                                               \
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
    X(void, glBufferSubData, (GLenum, GLintptr, GLsizeiptr, const void *))                                             \
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
    X(GLboolean, glUnmapBuffer, (GLenum))                                                                              \
    X(void, glDrawBuffers, (GLsizei, const GLenum *))                                                                  \
    X(void, glReadBuffer, (GLenum))                                                                                    \
    X(void, glClearBufferfv, (GLenum, GLint, const GLfloat *))                                                        \
    X(void, glTexImage3D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *))   \
    X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))                                          \
    X(void, glBlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum))          \
    X(void, glGetIntegerv, (GLenum, GLint *))                                                                          \
    X(void, glGetFloatv, (GLenum, GLfloat *))                                                                          \
    X(void, glUniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat))                                              \
    X(void, glUniform3i, (GLint, GLint, GLint, GLint))

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
    unsigned outputs; /* the pixel shader's colour outputs (bit n: oCn) */
    int nvb;          /* vertex buffers fetched at a computed index (xe_vb<n>), on texture units after the samplers */
    attr vbufs[MAX_ATTRS];
    int nsamplers;
    uint32_t sampler_fetch[MAX_SAMPLERS], sampler_dim[MAX_SAMPLERS]; /* texture fetch constant, dimension */
    int memexport;    /* XBA-282: the vertex shader exports to memory (GLSL ES 3.10, the log buffer at binding 0) */
    int ntq;          /* XBA-296: texture queries: the fetch constants whose size / border modes xe_tq<k> carries */
    uint32_t tq_fetch[MAX_SAMPLERS];
    GLint tq_loc[MAX_SAMPLERS];
} program;

/* a texture as last uploaded: its fetch constant's words (address, format, size, layout) and the source's hash */
typedef struct texture {
    uint32_t key[6];
    uint64_t hash;
    GLuint tex;
    int used;
    uint32_t checked; /* the epoch its memory was last compared in */
    int copy;         /* 1 + the resolve copy it was made from (its host picture, at the internal resolution) */
} texture;

/* ---- the eDRAM model ----
 * The 10 MB of eDRAM hold the render targets at tile bases (80 x 16 samples of 32 bits per tile) that RB_COLOR_INFO,
 * RB_COLOR1..3_INFO and RB_DEPTH_INFO name, with RB_SURFACE_INFO's pitch and MSAA mode. Each (base, pitch, format,
 * MSAA) a draw or resolve meets is a host texture of guest width x height at the internal resolution (scale): its
 * height grows to what the window scissor asks for. Surfaces are kept least recently used first; two keys at the same
 * base are two surfaces (a title reinterpreting a target's eDRAM in another format starts from a clear one). MSAA
 * targets are rendered single-sampled at the pixel grid (a resolve of any sample gives the pixel). */
#define MAX_SURFACES 32
#define MAX_COPIES 16

typedef struct surface {
    uint32_t base, pitch, fmt, depth, msaa; /* the key: tile base, guest pitch, colour (0-15) or depth (0-1) format */
    int w, h;                              /* guest pixels */
    GLuint tex;
    GLenum ifmt, rfmt, rtype;              /* host internal format, format / type of glReadPixels */
    int kind;                              /* 0 unorm, 1 float, 2 depth */
    unsigned used;
} surface;

/* what a resolve wrote to guest memory, kept as a host picture too (the front buffer at the internal resolution,
 * render to texture without a trip through memory while that memory is unchanged) */
typedef struct rt_copy {
    uint32_t base, bytes;  /* physical destination, extent */
    uint64_t hash;         /* of those bytes after the resolve */
    uint32_t checked;      /* the epoch the hash was last confirmed in */
    GLuint tex;            /* the destination's pitch x height at the internal resolution, top row first */
    int w, h;
    GLenum ifmt;
    unsigned used;
} rt_copy;

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
    int attached;             /* on the caller's current context (xenos_gl_attach): no EGL of our own */
    int ready;                /* the GL objects exist (destroy deletes them) */
    GLuint fbo, rfbo, tfbo_draw, rtex, vao, vbo, ibo, ubo[2], tfbo, vbtex[MAX_ATTRS];
    GLuint dprog, dtex;       /* depth resolves: the pass that packs depth into RGBA8, its target */
    GLint dprog_at;
    int dtex_w, dtex_h, dprog_failed;
    int rtex_w, rtex_h;
    GLenum rtex_fmt;
    int width, height;        /* the default target (a surface with pitch 0 / no scissor) and the frame size */
    int scale;                /* internal resolution: every surface is scale x its guest size */
    int max_tex;
    int no_float;             /* float targets are not renderable here (no EXT_color_buffer_*): 8_8_8_8 instead */
    int readback;             /* read each swap's frame back to memory (xenos_gl_frame, PPM frames) */
    surface surf[MAX_SURFACES];
    unsigned surf_clock;
    int cur[4], cur_ds;       /* the surfaces bound (index into surf, -1 none) */
    int tw, th;               /* the guest size of the bound colour target 0 (the viewport's frame of reference) */
    rt_copy copies[MAX_COPIES];
    unsigned copy_clock;
    GLuint frame_tex;         /* the last swap's picture: a texture of frame_w x frame_h host pixels, top row first */
    int frame_w, frame_h;
    uint8_t *frame;
    char cache[512], frames[512];
    unsigned max_frames;
    program progs[MAX_PROGRAMS];
    int nprogs;
    uint32_t missing[64];
    int nmissing;
    xenos_gl_stats st;
    texture textures[MAX_TEXTURES];
    unsigned tex_clock;
    uint32_t epoch;           /* moves at each swap and each resolve: cached textures are checked again */
    uint32_t resolved[16];    /* destination bases resolved to since the last swap (the front buffer's source) */
    int nresolved;
    int title_clears;
    volatile int reset_pending; /* xenos_gl_reset asked (maybe from another thread): done at the next draw / swap */
    int tex_linear;           /* XBA-282: RECOMP_TEX_BILINEAR (the app's texture filter, shared with the Xbox) */
    float tex_aniso;          /* RECOMP_TEX_ANISO 8 / 16 when EXT_texture_filter_anisotropic is there, else 0 */
    int gl_major, gl_minor;   /* the context's GLES version (3.1 and up: memory export through SSBOs) */
    int memexport;            /* XBA-282: memory export shaders run (GLES 3.1, vertex storage blocks) */
    void (*glMemoryBarrier)(GLbitfield);
    GLuint mxbuf;             /* their log: count, capacity, then (byte address, dword) pairs */
    int mx_warned;
    FILE *log;
};

static unsigned parse_outputs(const char *src);

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

/* the GLES entry points: from libGLESv2 or eglGetProcAddress; 0 when one is missing */
static int load_gl(xenos_gl *g, char *why, size_t why_n)
{
    static const char *const egl_names[] = {"libEGL.so.1", "libEGL.so", NULL};
    static const char *const gles_names[] = {"libGLESv2.so.2", "libGLESv3.so", "libGLESv2.so", NULL};
    g->egl_lib = open_first(egl_names);
    g->gles_lib = open_first(gles_names);
    if (!g->egl_lib || !g->gles_lib) {
        fail(why, why_n, "no libEGL / libGLESv2");
        return 0;
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
        return 0;
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
        return 0;
    }
    return 1;
}

/* the objects every draw uses; the default surfaces come with the first draw */
static int has_ext(xenos_gl *g, const char *name)
{
    GLint n = 0;
    g->glGetIntegerv(0x821D /* GL_NUM_EXTENSIONS */, &n);
    for (GLint i = 0; i < n; i++) {
        const char *e = (const char *)g->glGetStringi(0x1F03 /* GL_EXTENSIONS */, (GLuint)i);
        if (e && !strcmp(e, name))
            return 1;
    }
    return 0;
}

/* XBA-282: the texture filter options the app shares with the Xbox (Settings › Graphics › Texture filter, or a game's
 * own): RECOMP_TEX_BILINEAR=1 samples every texture linearly; RECOMP_TEX_ANISO=8 / 16 too, with that much anisotropic
 * filtering when the driver has EXT_texture_filter_anisotropic (clamped to its maximum). */
static void texture_options(xenos_gl *g)
{
    const char *b = getenv("RECOMP_TEX_BILINEAR"), *a = getenv("RECOMP_TEX_ANISO");
    int aniso = a ? atoi(a) : 0;
    g->tex_linear = (b && *b && strcmp(b, "0")) || aniso > 1;
    g->tex_aniso = 0;
    if (aniso > 1 && has_ext(g, "GL_EXT_texture_filter_anisotropic")) {
        GLfloat max = 1;
        g->glGetFloatv(0x84FF /* GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT */, &max);
        g->tex_aniso = (float)aniso < max ? (float)aniso : max;
    }
    if (g->tex_linear)
        fprintf(g->log, "xenos: texture filter forced linear%s\n", g->tex_aniso > 1 ? ", anisotropic" : "");
}

/* XBA-282: memory export needs GLES 3.1 with storage blocks in vertex shaders (3.1 allows none there) and
 * glMemoryBarrier, checked on the context at run time; RECOMP_X360_MEMEXPORT=0 turns it off. Without it a shader that
 * exports to memory is not compiled and its draws are skipped (counted), as a missing shader's. */
static void memexport_caps(xenos_gl *g)
{
    const char *env = getenv("RECOMP_X360_MEMEXPORT");
    GLint blocks = 0;
    g->memexport = 0;
    if (g->gl_major > 3 || (g->gl_major == 3 && g->gl_minor >= 1)) {
        g->glGetIntegerv(0x90D6 /* GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS */, &blocks);
        *(void **)&g->glMemoryBarrier = g->eglGetProcAddress ? g->eglGetProcAddress("glMemoryBarrier") : NULL;
        if (!g->glMemoryBarrier && g->gles_lib)
            *(void **)&g->glMemoryBarrier = dlsym(g->gles_lib, "glMemoryBarrier");
        g->memexport = blocks > 0 && g->glMemoryBarrier && !(env && !strcmp(env, "0"));
    }
    fprintf(g->log, "xenos: memory export %s (GLES %d.%d, %d vertex storage block(s))\n",
            g->memexport ? "on" : env && !strcmp(env, "0") ? "off (RECOMP_X360_MEMEXPORT=0)" : "off",
            g->gl_major, g->gl_minor, (int)blocks);
}

static int setup(xenos_gl *g, char *why, size_t why_n)
{
    g->max_tex = 4096;
    g->glGetIntegerv(GL_MAX_TEXTURE_SIZE, &g->max_tex);
    g->glGetIntegerv(0x821B /* GL_MAJOR_VERSION */, &g->gl_major);
    g->glGetIntegerv(0x821C /* GL_MINOR_VERSION */, &g->gl_minor);
    texture_options(g);
    memexport_caps(g);
    while (g->scale > 1 && (g->width * g->scale > g->max_tex || g->height * g->scale > g->max_tex))
        g->scale--;
    g->glGenFramebuffers(1, &g->fbo);
    g->glGenFramebuffers(1, &g->rfbo);
    g->glGenFramebuffers(1, &g->tfbo_draw);
    g->glGenVertexArrays(1, &g->vao);
    g->glBindVertexArray(g->vao);
    g->glGenBuffers(1, &g->vbo);
    g->glGenBuffers(1, &g->ibo);
    g->glGenBuffers(2, g->ubo);
    g->glGenBuffers(1, &g->tfbo);
    g->frame = calloc((size_t)g->width * g->height, 4);
    for (int i = 0; i < 4; i++)
        g->cur[i] = -1;
    g->cur_ds = -1;
    g->tw = g->width, g->th = g->height;
    g->ready = 1;
    if (!g->frame) {
        fail(why, why_n, "out of memory");
        return 0;
    }
    return 1;
}

static xenos_gl *new_gl(const char *cache_dir, int width, int height, int scale, char *why, size_t why_n)
{
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
    g->scale = scale < 1 ? 1 : scale > 8 ? 8 : scale;
    g->readback = 1;
    snprintf(g->cache, sizeof g->cache, "%s", cache_dir ? cache_dir : ".");
    return g;
}

xenos_gl *xenos_gl_create(const char *cache_dir, int width, int height, char *why, size_t why_n)
{
    return xenos_gl_create_scaled(cache_dir, width, height, 1, why, why_n);
}

xenos_gl *xenos_gl_create_scaled(const char *cache_dir, int width, int height, int scale, char *why, size_t why_n)
{
    xenos_gl *g = new_gl(cache_dir, width, height, scale, why, why_n);
    if (!g)
        return NULL;
    if (!load_gl(g, why, why_n)) {
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
    if (!setup(g, why, why_n)) {
        xenos_gl_destroy(g);
        return NULL;
    }
    return g;
}

xenos_gl *xenos_gl_attach(const char *cache_dir, int width, int height, int scale, char *why, size_t why_n)
{
    xenos_gl *g = new_gl(cache_dir, width, height, scale, why, why_n);
    if (!g)
        return NULL;
    g->attached = 1;
    g->readback = 0;
    if (!load_gl(g, why, why_n)) {
        xenos_gl_destroy(g);
        return NULL;
    }
    const char *ver = (const char *)g->glGetString(0x1F02 /* GL_VERSION */);
    if (!ver || !strstr(ver, "OpenGL ES 3")) {
        fail(why, why_n, ver ? "the current context is not OpenGL ES 3" : "no current GL context");
        xenos_gl_destroy(g);
        return NULL;
    }
    if (!setup(g, why, why_n)) {
        xenos_gl_destroy(g);
        return NULL;
    }
    return g;
}

void xenos_gl_destroy(xenos_gl *g)
{
    if (!g)
        return;
    if (g->ready) {
        for (int i = 0; i < g->nprogs; i++)
            if (g->progs[i].prog)
                g->glDeleteProgram(g->progs[i].prog);
        g->glDeleteBuffers(1, &g->vbo);
        g->glDeleteBuffers(1, &g->ibo);
        g->glDeleteBuffers(2, g->ubo);
        g->glDeleteBuffers(1, &g->tfbo);
        if (g->mxbuf)
            g->glDeleteBuffers(1, &g->mxbuf);
        for (int i = 0; i < MAX_TEXTURES; i++)
            if (g->textures[i].tex)
                g->glDeleteTextures(1, &g->textures[i].tex);
        for (int i = 0; i < MAX_SURFACES; i++)
            if (g->surf[i].tex)
                g->glDeleteTextures(1, &g->surf[i].tex);
        for (int i = 0; i < MAX_COPIES; i++)
            if (g->copies[i].tex)
                g->glDeleteTextures(1, &g->copies[i].tex);
        for (int i = 0; i < MAX_ATTRS; i++)
            if (g->vbtex[i])
                g->glDeleteTextures(1, &g->vbtex[i]);
        if (g->rtex)
            g->glDeleteTextures(1, &g->rtex);
        if (g->dtex)
            g->glDeleteTextures(1, &g->dtex);
        if (g->dprog)
            g->glDeleteProgram(g->dprog);
        if (g->frame_tex)
            g->glDeleteTextures(1, &g->frame_tex);
        g->glDeleteVertexArrays(1, &g->vao);
        g->glDeleteFramebuffers(1, &g->fbo);
        g->glDeleteFramebuffers(1, &g->rfbo);
        g->glDeleteFramebuffers(1, &g->tfbo_draw);
    }
    if (g->ctx) {
        g->eglMakeCurrent(g->dpy, NULL, NULL, NULL);
        g->eglDestroyContext(g->dpy, g->ctx);
    }
    if (g->dpy && g->eglTerminate)
        g->eglTerminate(g->dpy);
    /* the libraries stay loaded: Mesa does not like being unloaded with threads of its own alive */
    free(g->frame);
    free(g);
}

void xenos_gl_set_readback(xenos_gl *g, int on)
{
    g->readback = on;
}

int xenos_gl_scale(const xenos_gl *g)
{
    return g->scale;
}

unsigned xenos_gl_frame_texture(const xenos_gl *g, int *width, int *height)
{
    if (width)
        *width = g->frame_w;
    if (height)
        *height = g->frame_h;
    return g->frame_tex && g->frame_w > 0 ? g->frame_tex : 0;
}

void xenos_gl_reset(xenos_gl *g)
{
    g->reset_pending = 1; /* no GL here: a state may be loaded from another thread than the context's */
}

static void do_reset(xenos_gl *g)
{
    /* after a state load: the eDRAM is not in a state (as the GameCube's EFB): surfaces start clear, copies go */
    g->reset_pending = 0;
    for (int i = 0; i < MAX_SURFACES; i++)
        if (g->surf[i].tex) {
            g->glDeleteTextures(1, &g->surf[i].tex);
            memset(&g->surf[i], 0, sizeof g->surf[i]);
        }
    for (int i = 0; i < MAX_COPIES; i++)
        if (g->copies[i].tex) {
            g->glDeleteTextures(1, &g->copies[i].tex);
            memset(&g->copies[i], 0, sizeof g->copies[i]);
        }
    for (int i = 0; i < MAX_TEXTURES; i++)
        g->textures[i].copy = 0, g->textures[i].hash = 0, g->textures[i].checked = 0;
    for (int i = 0; i < 4; i++)
        g->cur[i] = -1;
    g->cur_ds = -1;
    g->nresolved = 0;
    g->epoch++;
}

void xenos_gl_set_frames_dir(xenos_gl *g, const char *dir, unsigned max_frames)
{
    snprintf(g->frames, sizeof g->frames, "%s", dir ? dir : "");
    g->max_frames = max_frames;
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

/* the translator version the cache must have been made with (x360_tools.xenos_glsl GLSL_VERSION) */
#define XENOS_GLSL_STAMP "// xenos_glsl 3\n"

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

/* "uniform highp vec4 xe_tqK;" lines of both stages: the fetch constants the texture queries read (XBA-296) */
static void parse_tqueries(const char *src, program *p)
{
    static const char pat[] = "uniform highp vec4 xe_tq";
    for (const char *s = strstr(src, pat); s && p->ntq < MAX_SAMPLERS; s = strstr(s + 1, pat)) {
        uint32_t k = (uint32_t)atoi(s + sizeof pat - 1);
        int seen = 0;
        for (int i = 0; i < p->ntq; i++)
            seen |= p->tq_fetch[i] == k;
        if (!seen)
            p->tq_fetch[p->ntq++] = k;
    }
}

static int parse_attrs_of(const char *src, attr *out, const char *decl)
{
    int n = 0;
    char fmtstr[200];
    snprintf(fmtstr, sizeof fmtstr, "%s%%u;  // fetch constant %%u, offset %%u, format %%u, stride %%u, signed %%u, "
             "integer %%u, exp %%d", decl);
    for (const char *p = strstr(src, decl); p && n < MAX_ATTRS; p = strstr(p + 1, decl)) {
        unsigned idx, fc, off, fmt, stride, sg, in = 0;
        int ex = 0;
        int got = sscanf(p, fmtstr, &idx, &fc, &off, &fmt, &stride, &sg, &in, &ex);
        if (got >= 6 && idx == (unsigned)n) {
            out[n].fetch_constant = fc, out[n].offset = off, out[n].format = fmt, out[n].stride = stride;
            out[n].is_signed = sg, out[n].is_integer = got >= 8 ? in : 0, out[n].exp_adjust = got >= 8 ? ex : 0;
            n++;
        }
    }
    return n;
}

static int parse_attrs(const char *src, attr *out)
{
    return parse_attrs_of(src, out, "in vec4 xe_v");
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
        if ((vsrc = slurp(path)) && !strstr(vsrc, XENOS_GLSL_STAMP)) { /* an older translator's: stale */
            fprintf(g->log, "xenos_gl: %s was translated by an older xenos_glsl: translate it again\n", path);
            free(vsrc);
            vsrc = NULL;
        }
        if (!vsrc)
            report_missing(g, 0, vc);
    }
    if (kind != 1) {
        snprintf(path, sizeof path, "%s/ps_%08X.frag", g->cache, pc);
        if ((psrc = slurp(path)) && !strstr(psrc, XENOS_GLSL_STAMP)) {
            fprintf(g->log, "xenos_gl: %s was translated by an older xenos_glsl: translate it again\n", path);
            free(psrc);
            psrc = NULL;
        }
        if (!psrc)
            report_missing(g, 1, pc);
    }
    /* XBA-282: a vertex shader that exports to memory is GLSL ES 3.10: it needs the capability, and its pixel shader
     * is linked as 3.10 too (GLES links one version only; 3.00 source is 3.10 source) */
    if (vsrc && !strncmp(vsrc, "#version 310 es", 15)) {
        if (!g->memexport) {
            if (!g->mx_warned++)
                fprintf(g->log, "xenos_gl: vs_%08X exports to memory: its draws are skipped (no GLES 3.1 storage "
                                "blocks in vertex shaders here)\n", vc);
            g->st.memexport_skipped++;
            free(vsrc);
            free(psrc);
            return NULL;
        }
        p->memexport = 1;
        if (psrc && !strncmp(psrc, "#version 300 es", 15))
            psrc[10] = '1';
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
                    p->nvb = parse_attrs_of(vsrc, p->vbufs, "uniform highp sampler2D xe_vb");
                    for (int i = 0; i < p->nattr; i++) {
                        char an[16];
                        snprintf(an, sizeof an, "xe_v%d", i);
                        p->loc[i] = g->glGetAttribLocation(p->prog, an);
                    }
                }
                if (vsrc) {
                    parse_samplers(vsrc, p);
                    parse_tqueries(vsrc, p);
                }
                if (psrc) {
                    parse_samplers(psrc, p);
                    parse_tqueries(psrc, p);
                    p->outputs = parse_outputs(psrc);
                }
                for (int i = 0; i < p->ntq; i++) {
                    char un[16];
                    snprintf(un, sizeof un, "xe_tq%u", p->tq_fetch[i]);
                    p->tq_loc[i] = g->glGetUniformLocation(p->prog, un);
                }
                g->glUseProgram(p->prog);
                for (int i = 0; i < p->nvb; i++) {
                    char un[16];
                    snprintf(un, sizeof un, "xe_vb%d", i);
                    GLint l = g->glGetUniformLocation(p->prog, un);
                    if (l >= 0)
                        g->glUniform1i(l, p->nsamplers + i);
                }
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
        float k = ldexpf(1.0f, a->exp_adjust); /* -32..31: no shift of 32 */
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

/* ---- eDRAM surfaces (see the model above) ----
 * Host textures are kept top row first: guest y = 0 is the texture's row 0 (GL's window y = 0), so a surface grows
 * downwards without moving its contents; the viewport transform, culling's winding and the scissor follow. */

static void host_format(const xenos_gl *g, uint32_t fmt, int depth, surface *s, GLenum *tfmt, GLenum *ttype)
{
    s->kind = 0, s->ifmt = GL_RGBA8, s->rfmt = GL_RGBA, s->rtype = GL_UNSIGNED_BYTE;
    *tfmt = GL_RGBA, *ttype = GL_UNSIGNED_BYTE;
    if (depth) {
        s->kind = 2, s->ifmt = GL_DEPTH24_STENCIL8, s->rfmt = GL_DEPTH_STENCIL, s->rtype = GL_UNSIGNED_INT_24_8;
        *tfmt = GL_DEPTH_STENCIL, *ttype = GL_UNSIGNED_INT_24_8;
        return;
    }
    switch (fmt) {
    case 1: s->ifmt = GL_SRGB8_ALPHA8; break; /* 8_8_8_8_GAMMA: stored gamma-encoded, blended linear */
    case 2:
    case 10: /* 2_10_10_10 (and as 10_10_10_10) */
        s->ifmt = GL_RGB10_A2, s->rtype = GL_UNSIGNED_INT_2_10_10_10_REV, *ttype = GL_UNSIGNED_INT_2_10_10_10_REV;
        break;
    case 3:  /* 2_10_10_10_FLOAT (7e3) */
    case 12: /* ... as 16_16_16_16 */
    case 4:  /* 16_16 (-32..32): kept as half floats */
    case 5:  /* 16_16_16_16 */
    case 7:  /* 16_16_16_16_FLOAT */
        s->kind = 1, s->ifmt = GL_RGBA16F, s->rtype = GL_FLOAT, *ttype = GL_HALF_FLOAT;
        break;
    case 6: s->kind = 1, s->ifmt = GL_RG16F, s->rtype = GL_FLOAT, *tfmt = GL_RG, *ttype = GL_HALF_FLOAT; break;
    case 14: s->kind = 1, s->ifmt = GL_R32F, s->rtype = GL_FLOAT, *tfmt = GL_RED, *ttype = GL_FLOAT; break;
    case 15: s->kind = 1, s->ifmt = GL_RG32F, s->rtype = GL_FLOAT, *tfmt = GL_RG, *ttype = GL_FLOAT; break;
    default: break;
    }
    if (s->kind == 1 && g->no_float) { /* float targets not renderable: 8_8_8_8 (values clamped to 0..1) */
        s->kind = 0, s->ifmt = GL_RGBA8, s->rtype = GL_UNSIGNED_BYTE;
        *tfmt = GL_RGBA, *ttype = GL_UNSIGNED_BYTE;
    }
}

static GLuint new_texture(xenos_gl *g, GLenum ifmt, GLenum tfmt, GLenum ttype, int w, int h)
{
    GLuint t;
    g->glGenTextures(1, &t);
    g->glBindTexture(GL_TEXTURE_2D, t);
    g->glTexImage2D(GL_TEXTURE_2D, 0, (GLint)ifmt, w, h, 0, tfmt, ttype, NULL);
    g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

/* clear a surface's texture (a new one: the eDRAM it stands for has no picture yet for us) */
static void clear_texture(xenos_gl *g, const surface *s, GLuint tex)
{
    g->glBindFramebuffer(GL_FRAMEBUFFER, g->rfbo);
    g->glFramebufferTexture2D(GL_FRAMEBUFFER, s->kind == 2 ? GL_DEPTH_STENCIL_ATTACHMENT : GL_COLOR_ATTACHMENT0,
                              GL_TEXTURE_2D, tex, 0);
    g->glDisable(GL_SCISSOR_TEST);
    if (s->kind == 2) {
        g->glDepthMask(1);
        g->glStencilMaskSeparate(GL_FRONT_AND_BACK, 0xFF);
        g->glClearDepthf(1.0f);
        g->glClearStencil(0);
        g->glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    } else {
        static const GLenum b0 = GL_COLOR_ATTACHMENT0;
        g->glDrawBuffers(1, &b0);
        g->glColorMask(1, 1, 1, 1);
        g->glClearColor(0, 0, 0, s->kind == 1 ? 0.0f : 1.0f);
        g->glClear(GL_COLOR_BUFFER_BIT);
    }
    g->glFramebufferTexture2D(GL_FRAMEBUFFER, s->kind == 2 ? GL_DEPTH_STENCIL_ATTACHMENT : GL_COLOR_ATTACHMENT0,
                              GL_TEXTURE_2D, 0, 0);
}

/* the surface for an eDRAM key, made (or grown to need_h rows) when needed; its index */
static int get_surface(xenos_gl *g, uint32_t base, uint32_t pitch, uint32_t fmt, int depth, uint32_t msaa,
                       uint32_t alias, int need_h)
{
    if (!pitch)
        pitch = (uint32_t)g->width;
    if (need_h <= 0)
        need_h = g->height;
    /* the eDRAM's 2048 tiles of 80 x 16 samples (32-bit; 40 x 16 for the 64-bit formats); 2x MSAA doubles the
     * samples vertically, 4x both ways: a title's scissor left at its maximum does not make an 8192-row surface */
    {
        int wide = !depth && (fmt == 4 || fmt == 5 || fmt == 7 || fmt == 15);
        uint32_t sx = msaa == 2 ? 2 : 1, sy = msaa ? 2 : 1, tw = wide ? 40 : 80;
        uint32_t per_row = (pitch * sx + tw - 1) / tw, left = base < 2048 ? 2048 - base : 1;
        int max_h = per_row ? (int)(left / per_row * 16 / sy) : need_h;
        if (max_h < 1)
            max_h = 1;
        if (need_h > max_h)
            need_h = max_h;
    }
    int lim = g->max_tex / g->scale;
    if ((int)pitch > lim)
        pitch = (uint32_t)lim;
    if (need_h > lim)
        need_h = lim;
    uint32_t key_fmt = fmt | alias << 8;
    int victim = -1;
    for (int i = 0; i < MAX_SURFACES; i++) {
        surface *s = &g->surf[i];
        if (s->tex && s->base == base && s->pitch == pitch && s->fmt == key_fmt && s->depth == (uint32_t)depth &&
            s->msaa == msaa) {
            s->used = ++g->surf_clock;
            if (s->h >= need_h)
                return i;
            /* taller: a new texture, the old rows copied to the top */
            GLenum tf, tt;
            surface n = *s;
            host_format(g, fmt, depth, &n, &tf, &tt);
            GLuint t = new_texture(g, s->ifmt, tf, tt, (int)pitch * g->scale, need_h * g->scale);
            clear_texture(g, s, t);
            g->glBindFramebuffer(GL_READ_FRAMEBUFFER, g->rfbo);
            g->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g->fbo);
            GLenum att = depth ? GL_DEPTH_STENCIL_ATTACHMENT : GL_COLOR_ATTACHMENT0;
            g->glFramebufferTexture2D(GL_READ_FRAMEBUFFER, att, GL_TEXTURE_2D, s->tex, 0);
            for (int k = 0; k < 4; k++) /* the draw FBO's attachments are set again by the next bind */
                g->glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + (GLenum)k, GL_TEXTURE_2D, 0, 0);
            g->glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
            g->glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, att, GL_TEXTURE_2D, t, 0);
            if (!depth) {
                static const GLenum b0 = GL_COLOR_ATTACHMENT0;
                g->glReadBuffer(GL_COLOR_ATTACHMENT0);
                g->glDrawBuffers(1, &b0);
            }
            g->glDisable(GL_SCISSOR_TEST);
            int hw = (int)pitch * g->scale, hh = s->h * g->scale;
            g->glBlitFramebuffer(0, 0, hw, hh, 0, 0, hw, hh,
                                 depth ? GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT : GL_COLOR_BUFFER_BIT, GL_NEAREST);
            g->glFramebufferTexture2D(GL_READ_FRAMEBUFFER, att, GL_TEXTURE_2D, 0, 0);
            g->glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, att, GL_TEXTURE_2D, 0, 0);
            g->glDeleteTextures(1, &s->tex);
            s->tex = t;
            s->h = need_h;
            for (int k = 0; k < 4; k++)
                if (g->cur[k] == i)
                    g->cur[k] = -2; /* attached again */
            if (g->cur_ds == i)
                g->cur_ds = -2;
            return i;
        }
        int bound = 0;
        for (int k = 0; k < 4; k++)
            bound |= g->cur[k] == i;
        bound |= g->cur_ds == i;
        if (!s->tex) {
            if (victim < 0 || g->surf[victim].tex)
                victim = i;
        } else if (!bound && (victim < 0 || (g->surf[victim].tex && s->used < g->surf[victim].used))) {
            victim = i;
        }
    }
    if (victim < 0)
        victim = 0;
    surface *s = &g->surf[victim];
    if (s->tex)
        g->glDeleteTextures(1, &s->tex);
    for (int k = 0; k < 4; k++)
        if (g->cur[k] == victim)
            g->cur[k] = -1;
    if (g->cur_ds == victim)
        g->cur_ds = -1;
    memset(s, 0, sizeof *s);
    GLenum tf, tt;
    host_format(g, fmt, depth, s, &tf, &tt);
    s->base = base, s->pitch = pitch, s->fmt = key_fmt, s->depth = (uint32_t)depth, s->msaa = msaa;
    s->w = (int)pitch, s->h = need_h;
    s->tex = new_texture(g, s->ifmt, tf, tt, s->w * g->scale, s->h * g->scale);
    s->used = ++g->surf_clock;
    clear_texture(g, s, s->tex);
    g->st.surfaces++;
    return victim;
}

/* the pixel shader's colour outputs ("layout(location = n) out vec4 xe_colorN;") */
static unsigned parse_outputs(const char *src)
{
    unsigned m = 0;
    for (const char *p = strstr(src, "out vec4 xe_color"); p; p = strstr(p + 1, "out vec4 xe_color")) {
        int k = atoi(p + strlen("out vec4 xe_color"));
        if (k >= 0 && k < 4)
            m |= 1u << k;
    }
    return m;
}

/* bind the draw FBO to the surfaces RB_SURFACE_INFO / RB_COLOR*_INFO / RB_DEPTH_INFO name for a draw writing the
 * colour outputs in `outputs`; sets tw / th and the host viewport. 0 when no complete framebuffer can be had. */
static int bind_targets(xenos_gl *g, const xenos_cp *cp, unsigned outputs)
{
    static const uint32_t info_reg[4] = {0x2001, 0x2003, 0x2004, 0x2005};
    uint32_t si = cp->regs[0x2000], pitch = si & 0x3FFF, msaa = (si >> 16) & 3, mode = cp->regs[0x2208] & 7;
    uint32_t br = cp->regs[0x2082];
    int need_h = (int)((br >> 16) & 0x7FFF);
    if ((cp->regs[0x2205] >> 16) & 1) /* the window offset moves the scissor too */
        need_h += (int)((int32_t)(cp->regs[0x2080] << 1) >> 17);
    for (int attempt = 0; attempt < 2; attempt++) {
        int want[4], ds = -1;
        for (int i = 0; i < 4; i++) {
            want[i] = -1;
            if (mode == 5 || (i > 0 && !((outputs >> i) & 1)))
                continue;
            uint32_t info = cp->regs[info_reg[i]], base = info & 0xFFF, alias = 0;
            for (int k = 0; k < i; k++) /* two targets at one base (an unset RB_COLORn_INFO): kept apart */
                if (want[k] >= 0 && (g->surf[want[k]].base == base))
                    alias = (uint32_t)i;
            want[i] = get_surface(g, base, pitch, (info >> 16) & 15, 0, msaa, alias, need_h);
        }
        uint32_t dinfo = cp->regs[0x2002];
        ds = get_surface(g, dinfo & 0xFFF, pitch, (dinfo >> 16) & 1, 1, msaa, 0, need_h);
        for (int i = 0; i < 4; i++) /* a later get_surface may have reused an earlier one's slot */
            if (want[i] >= 0 && !g->surf[want[i]].tex)
                want[i] = -1;
        g->glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
        GLenum bufs[4];
        for (int i = 0; i < 4; i++) {
            if (g->cur[i] != want[i])
                g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + (GLenum)i, GL_TEXTURE_2D,
                                          want[i] >= 0 ? g->surf[want[i]].tex : 0, 0);
            g->cur[i] = want[i];
            bufs[i] = want[i] >= 0 ? GL_COLOR_ATTACHMENT0 + (GLenum)i : 0 /* GL_NONE */;
        }
        if (g->cur_ds != ds)
            g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, g->surf[ds].tex, 0);
        g->cur_ds = ds;
        g->glDrawBuffers(4, bufs);
        if (g->glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
            const surface *ref = want[0] >= 0 ? &g->surf[want[0]] : &g->surf[ds];
            g->tw = ref->w, g->th = ref->h;
            g->glViewport(0, 0, g->tw * g->scale, g->th * g->scale);
            return 1;
        }
        /* float targets not renderable here (no EXT_color_buffer_float): 8_8_8_8 from now on */
        int had_float = 0;
        for (int i = 0; i < MAX_SURFACES; i++)
            if (g->surf[i].tex && g->surf[i].kind == 1) {
                had_float = 1;
                g->glDeleteTextures(1, &g->surf[i].tex);
                memset(&g->surf[i], 0, sizeof g->surf[i]);
            }
        for (int i = 0; i < 4; i++)
            g->cur[i] = -1;
        g->cur_ds = -1;
        for (int i = 0; i < 4; i++)
            g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + (GLenum)i, GL_TEXTURE_2D, 0, 0);
        g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
        if (!had_float || g->no_float)
            break;
        g->no_float = 1;
        fprintf(g->log, "xenos_gl: float render targets are not renderable here: 8_8_8_8 in their place\n");
    }
    return 0;
}

/* The guest viewport as the vertex shader applies it (xe_k.vp; Xenia's draw_util.cc GetHostViewportInfo): window
 * position = NDC * scale + offset, scale 1 / offset 0 on an axis whose PA_CL_VTE_CNTL bits are off (screen-space
 * draws give pixels), plus PA_SC_WINDOW_OFFSET when PA_SU_SC_MODE_CNTL enables it and half a pixel for Direct3D's
 * pixel centre (PA_SU_VTX_CNTL); then to the host's clip space over the bound target (guest y = 0 at GL's y = 0). */
static void viewport_transform(const xenos_gl *g, const xenos_cp *cp, float vp[4])
{
    uint32_t vte = cp->regs[0x2206];
    float sx = vte & 1 ? as_float(cp->regs[0x210F]) : 1.0f, ox = vte & 2 ? as_float(cp->regs[0x2110]) : 0.0f;
    float sy = vte & 4 ? as_float(cp->regs[0x2111]) : 1.0f, oy = vte & 8 ? as_float(cp->regs[0x2112]) : 0.0f;
    if ((cp->regs[0x2205] >> 16) & 1) {
        uint32_t wo = cp->regs[0x2080];
        ox += (float)((int32_t)(wo << 17) >> 17), oy += (float)((int32_t)(wo << 1) >> 17);
    }
    if (!(cp->regs[0x2302] & 1)) /* kD3DZero: pixel centres at .0 */
        ox += 0.5f, oy += 0.5f;
    float w = (float)g->tw, h = (float)g->th;
    vp[0] = 2.0f * sx / w, vp[1] = 2.0f * sy / h, vp[2] = 2.0f * ox / w - 1.0f, vp[3] = 2.0f * oy / h - 1.0f;
}

static void upload_constants(xenos_gl *g, const xenos_cp *cp)
{
    uint8_t block[4096 + 32 + 128 + 48];
    float vp[4];
    viewport_transform(g, cp, vp);
    uint32_t cc = cp->regs[0x2202];
    float at[4] = {(cc >> 3) & 1 ? 1.0f : 0.0f, (float)(cc & 7), as_float(cp->regs[0x210E]), 0.0f};
    /* SQ_PROGRAM_CNTL's param_gen, SQ_CONTEXT_MISC's register for it; the pixel position is gl_FragCoord.xy * zw +
     * (0, y) - 0.5: guest pixels from the top-left at any internal resolution */
    float inv = 1.0f / (float)g->scale;
    float pg[4] = {(cp->regs[0x2180] >> 18) & 1 ? (float)((cp->regs[0x2181] >> 8) & 0xFF) : -1.0f, 0.0f, inv, inv};
    for (int stage = 0; stage < 2; stage++) {
        memcpy(block, &cp->regs[0x4000 + 0x400 * stage], 4096); /* float constants: register values are the bits */
        memcpy(block + 4096, &cp->regs[0x4900], 32);
        memcpy(block + 4096 + 32, &cp->regs[0x4908], 128);
        memcpy(block + 4096 + 32 + 128, at, 16); /* RB_COLORCONTROL's alpha test, RB_ALPHA_REF */
        memcpy(block + 4096 + 32 + 128 + 16, pg, 16);
        memcpy(block + 4096 + 32 + 128 + 32, vp, 16);
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
    /* culling: Xenos's winding is as seen on the render target (Xenia: 0 = front is CCW); the host target is the
     * picture upside down (guest y = 0 at GL's y = 0), which reverses the winding GL sees */
    uint32_t sc = cp->regs[0x2205];
    if (sc & 3) {
        g->glEnable(GL_CULL_FACE);
        g->glCullFace((sc & 3) == 3 ? GL_FRONT_AND_BACK : (sc & 1) ? GL_FRONT : GL_BACK);
    } else {
        g->glDisable(GL_CULL_FACE);
    }
    g->glFrontFace((sc >> 2) & 1 ? GL_CCW : GL_CW);
    /* the window scissor (top-left / bottom-right, y downwards), when it is set and smaller than the target */
    uint32_t tl = cp->regs[0x2081], br = cp->regs[0x2082];
    int x0 = (int)(tl & 0x7FFF), y0 = (int)((tl >> 16) & 0x7FFF);
    int x1 = (int)(br & 0x7FFF), y1 = (int)((br >> 16) & 0x7FFF);
    if (br && (x0 > 0 || y0 > 0 || x1 < g->tw || y1 < g->th) && x1 > x0 && y1 > y0) {
        int s = g->scale;
        g->glEnable(GL_SCISSOR_TEST);
        g->glScissor(x0 * s, y0 * s, (x1 - x0) * s, (y1 - y0) * s);
    } else {
        g->glDisable(GL_SCISSOR_TEST);
    }
}

/* ---- textures ----
 * The texture fetch constant (six dwords from 0x4800 + 6 * index; Xenia's xenos.h): dword 0 type, signs, clamps,
 * pitch (texels >> 5), tiled; dword 1 format, endian swap, base address >> 12; dword 2 width - 1, height - 1;
 * dword 3 swizzle (3 bits per component: X, Y, Z, W, 0, 1), filters (mag, min, mip); dword 4 mip levels min / max;
 * dword 5 dimension, packed mips, mip address >> 12. Tiled textures use the Xenos 2D tiling: 32x32-block macro
 * tiles, banks and pipes (the address formula of Xenia's texture_address.h, Tiled2D). Formats: 8, 8_8, 8_8_8_8 (and
 * _A, _AS_16_16_16_16, _GAMMA), 1_5_5_5, 5_6_5, 6_5_5, 4_4_4_4, 2_10_10_10 (and _AS_16), 10_11_11, 11_11_10, 16,
 * 16_16, 16_16_16_16 (unsigned or signed normalised), their _FLOAT forms, 32_FLOAT, 32_32_FLOAT, 32_32_32_FLOAT
 * (linear), 32_32_32_32_FLOAT, 24_8 / 24_8_FLOAT (depth in X), DXT1, DXT2_3, DXT4_5 (and _AS_16), DXN, DXT3A,
 * DXT5A, CTX1; unsigned-normalised ones decoded to RGBA8, the others to floats (RGBA16F), X in red; the fetch
 * constant's swizzle goes to GL's texture swizzle. Mip levels from the mip address: each level max(1, pow2(size) >>
 * level) with its own 32-block-aligned pitch and height, levels 4 KB aligned one after the other; a packed mip tail
 * (levels whose smaller side is 16 texels or less, packed_mips set) is not decoded yet: sampling stops at the last
 * level before it. 1D, 2D, 3D and cube. */

typedef struct texfmt {
    uint32_t format, block, bytes; /* block size in texels (1 or 4), bytes per block */
    int is_float;                  /* decoded to floats (RGBA16F) rather than RGBA8 */
} texfmt;

static const texfmt *tex_format(uint32_t f)
{
    static const texfmt fmts[] = {
        {2, 1, 1, 0},   {10, 1, 2, 0},  {6, 1, 4, 0},   {14, 1, 4, 0},  {50, 1, 4, 0},  {62, 1, 4, 0},
        {3, 1, 2, 0},   {4, 1, 2, 0},   {5, 1, 2, 0},   {15, 1, 2, 0},  {7, 1, 4, 0},   {54, 1, 4, 0},
        {16, 1, 4, 0},  {17, 1, 4, 0},  {55, 1, 4, 0},  {56, 1, 4, 0},  {24, 1, 2, 1},  {25, 1, 4, 1},
        {26, 1, 8, 1},  {30, 1, 2, 1},  {31, 1, 4, 1},  {32, 1, 8, 1},  {36, 1, 4, 1},  {37, 1, 8, 1},
        {57, 1, 12, 1}, {38, 1, 16, 1}, {22, 1, 4, 1},  {23, 1, 4, 1},  {18, 4, 8, 0},  {19, 4, 16, 0},
        {20, 4, 16, 0}, {51, 4, 8, 0},  {52, 4, 16, 0}, {53, 4, 16, 0}, {49, 4, 16, 0}, {58, 4, 8, 0},
        {59, 4, 8, 0},  {60, 4, 8, 0}};
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

/* an interpolated 8-bit channel block (DXT5's alpha, DXT5A, DXN's halves): 16 values */
static void alpha_block(const uint8_t *b, uint8_t out[16])
{
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
        out[i] = (uint8_t)ap[(ab >> (3 * i)) & 7];
}

/* one 4x4 block of a compressed format at `b` (bytes after the endian swap) to 16 RGBA texels; colours are 5:6:5
 * with the 5 low bits first (X) */
static void dxt_block(const uint8_t *b, uint32_t format, uint8_t out[16][4])
{
    switch (format) {
    case 51: format = 18; break;
    case 52: format = 19; break;
    case 53: format = 20; break;
    default: break;
    }
    if (format == 49 || format == 59) { /* DXN: X then Y interpolated blocks; DXT5A: one, replicated */
        uint8_t x[16], y[16];
        alpha_block(b, x);
        if (format == 49)
            alpha_block(b + 8, y);
        for (int i = 0; i < 16; i++) {
            out[i][0] = x[i];
            out[i][1] = format == 49 ? y[i] : x[i];
            out[i][2] = format == 49 ? 0 : x[i];
            out[i][3] = format == 49 ? 255 : x[i];
        }
        return;
    }
    if (format == 58) { /* DXT3A: explicit 4-bit values, replicated */
        for (int i = 0; i < 16; i++) {
            uint8_t a = (uint8_t)(((b[i / 2] >> (4 * (i & 1))) & 15) * 17);
            out[i][0] = out[i][1] = out[i][2] = out[i][3] = a;
        }
        return;
    }
    if (format == 60) { /* CTX1: two 8:8 (X, Y) endpoints, 2-bit indices, two thirds between */
        uint8_t pal[4][2] = {{b[0], b[1]}, {b[2], b[3]}};
        for (int k = 0; k < 2; k++) {
            pal[2][k] = (uint8_t)((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = (uint8_t)((pal[0][k] + 2 * pal[1][k]) / 3);
        }
        uint32_t bits = b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24;
        for (int i = 0; i < 16; i++) {
            const uint8_t *p = pal[(bits >> (2 * i)) & 3];
            out[i][0] = p[0], out[i][1] = p[1], out[i][2] = 0, out[i][3] = 255;
        }
        return;
    }
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
        uint8_t a[16];
        alpha_block(b, a);
        for (int i = 0; i < 16; i++)
            out[i][3] = a[i];
    }
}

static uint8_t unorm8(uint32_t v, unsigned bits)
{
    uint32_t m = (1u << bits) - 1;
    return (uint8_t)(((v & m) * 255 + m / 2) / m);
}

static void texel(uint32_t format, const uint8_t *s, uint8_t out[4])
{
    uint32_t v = s[0] | (uint32_t)s[1] << 8, d = v | (uint32_t)s[2] << 16 | (uint32_t)s[3] << 24;
    switch (format) {
    case 2: out[0] = out[1] = out[2] = out[3] = s[0]; break; /* 8: replicated, the swizzle picks */
    case 10: out[0] = s[0], out[1] = s[1], out[2] = 0, out[3] = 255; break;
    case 6:
    case 14:
    case 50:
    case 62: memcpy(out, s, 4); break;
    case 3:
        out[0] = unorm8(v, 5), out[1] = unorm8(v >> 5, 5), out[2] = unorm8(v >> 10, 5), out[3] = (v >> 15) ? 255 : 0;
        break;
    case 4: unpack_565(v, out); break;
    case 5: out[0] = unorm8(v, 5), out[1] = unorm8(v >> 5, 5), out[2] = unorm8(v >> 10, 6), out[3] = 255; break;
    case 15:
        for (int k = 0; k < 4; k++)
            out[k] = (uint8_t)(((v >> (4 * k)) & 15) * 17);
        break;
    case 7:
    case 54:
        out[0] = unorm8(d, 10), out[1] = unorm8(d >> 10, 10), out[2] = unorm8(d >> 20, 10), out[3] = unorm8(d >> 30, 2);
        break;
    case 16: /* 10_11_11: 11, 11, 10 bits from X */
    case 55: out[0] = unorm8(d, 11), out[1] = unorm8(d >> 11, 11), out[2] = unorm8(d >> 22, 10), out[3] = 255; break;
    case 17: /* 11_11_10: 10, 11, 11 bits from X */
    case 56: out[0] = unorm8(d, 10), out[1] = unorm8(d >> 10, 11), out[2] = unorm8(d >> 21, 11), out[3] = 255; break;
    default: memset(out, 0, 4);
    }
}

static float half_bits(uint32_t h)
{
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v;
    if (e == 0)
        v = (float)m / 16777216.0f;
    else if (e == 31)
        v = 65504.0f; /* Xenos's 16-bit floats have no infinity / NaN: the largest value */
    else
        v = (1.0f + (float)m / 1024.0f) * (float)(1u << e) / 32768.0f;
    return s ? -v : v;
}

/* a float-decoded texel: 16-bit normalised (signed per the fetch constant's sign of X), halves, floats, depth */
static void texel_f(uint32_t format, const uint8_t *s, uint32_t sign, float out[4])
{
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    switch (format) {
    case 24:
    case 25:
    case 26: {
        int n = format == 24 ? 1 : format == 25 ? 2 : 4;
        for (int i = 0; i < n; i++) {
            uint32_t u = s[2 * i] | (uint32_t)s[2 * i + 1] << 8;
            if (sign == 1) {
                float f = (float)(int16_t)u / 32767.0f;
                out[i] = f < -1.0f ? -1.0f : f;
            } else {
                out[i] = (float)u / 65535.0f;
            }
        }
        if (n == 1)
            out[1] = out[2] = out[3] = out[0];
        break;
    }
    case 30:
    case 31:
    case 32: {
        int n = format == 30 ? 1 : format == 31 ? 2 : 4;
        for (int i = 0; i < n; i++)
            out[i] = half_bits(s[2 * i] | (uint32_t)s[2 * i + 1] << 8);
        if (n == 1)
            out[1] = out[2] = out[3] = out[0];
        break;
    }
    case 36:
    case 37:
    case 57:
    case 38: {
        int n = format == 36 ? 1 : format == 37 ? 2 : format == 57 ? 3 : 4;
        for (int i = 0; i < n; i++)
            memcpy(&out[i], s + 4 * i, 4);
        if (n == 1)
            out[1] = out[2] = out[3] = out[0];
        break;
    }
    case 22: { /* 24_8: depth in the high 24 bits */
        uint32_t d = s[0] | s[1] << 8 | s[2] << 16 | (uint32_t)s[3] << 24;
        out[0] = out[1] = out[2] = out[3] = (float)(d >> 8) / 16777215.0f;
        break;
    }
    case 23: { /* 24_8_FLOAT: a 20e4 float depth */
        uint32_t d = (s[0] | s[1] << 8 | s[2] << 16 | (uint32_t)s[3] << 24) >> 8, e = d >> 20, m = d & 0xFFFFF;
        float f = e ? ldexpf(1.0f + (float)m / 1048576.0f, (int)e - 15) : ldexpf((float)m / 1048576.0f, -14);
        out[0] = out[1] = out[2] = out[3] = f;
        break;
    }
    default: break;
    }
}

static uint64_t fnv(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

static int64_t tiled3d(int32_t x, int32_t y, int32_t z, uint32_t pitch_blocks, uint32_t height_blocks,
                       unsigned bpb_log2)
{
    int32_t outer = ((((z >> 2) * (int32_t)(height_blocks >> 4) + (y >> 4)) * (int32_t)(pitch_blocks >> 5)) + (x >> 5))
                    << 7;
    int32_t inner = ((z & 3) << 5) | (((y >> 1) & 3) << 3) | (x & 7);
    int64_t oi = (int64_t)(outer | inner) << bpb_log2;
    uint32_t bank = ((y >> 3) ^ (z >> 2)) & 1, pipe = ((x >> 3) & 3) ^ (bank << 1);
    return (int64_t)((uint32_t)(y & 1) << 4 | pipe << 6 | bank << 11) | (oi & 15) | ((oi >> 4) & 1) << 5 |
           ((oi >> 5) & 7) << 8 | (oi >> 8) << 12;
}

/* `n` bytes at a guest physical address as the GPU reads them (after its swap), a dword at a time */
static void read_bytes(const xenos_cp *cp, uint32_t addr, uint32_t n, uint32_t endian, uint8_t *out)
{
    uint32_t i = 0;
    while (i < n) {
        uint32_t a = addr + i, word = gpu_read(cp, a & ~3u, endian);
        for (uint32_t b = a & 3; b < 4 && i < n; b++, i++)
            out[i] = (uint8_t)(word >> (8 * b));
    }
}

typedef struct decoded {
    uint32_t w, h, slices, dim; /* dim as the fetch constant has it: 0 1D, 1 2D, 2 3D, 3 cube */
    int levels;                 /* mip levels decoded (1: the base only) */
    int is_float;               /* rgba holds floats (4 per texel) rather than bytes */
    void *level[14];            /* each level's slices of RGBA, top row first */
    uint32_t lw[14], lh[14], ld[14];
    uint64_t hash;              /* of the source bytes and the swizzle */
} decoded;

static void free_decoded(decoded *d)
{
    for (int i = 0; i < 14; i++)
        free(d->level[i]);
    memset(d->level, 0, sizeof d->level);
}

static uint32_t pow2_ceil(uint32_t v)
{
    uint32_t p = 1;
    while (p < v)
        p <<= 1;
    return p;
}

/* decode the texture a 6-dword fetch constant describes (the layout of Xenia's texture_util.cc
 * GetGuestTextureLayout: rows of the pitch aligned to 32 blocks, linear ones too, as Direct3D's pitches already
 * are; slices of the height aligned to 32 blocks; cube faces (+X, -X, +Y, -Y, +Z, -Z, GL's order) 4 KB apart; 3D
 * slices packed, tiled ones in the 3D tiling; mips as the header says). With `hash_only`, only the hash is made.
 * 0 when unsupported. */
static int decode_texture(const xenos_cp *cp, const uint32_t *fc, int hash_only, decoded *out)
{
    memset(out, 0, sizeof *out);
    const texfmt *tf = tex_format(fc[1] & 63);
    uint32_t dim = (fc[5] >> 9) & 3;
    if ((fc[0] & 3) != 2 || !tf)
        return 0;
    uint32_t w, h, depth = 1;
    if (dim == 0) {
        w = (fc[2] & 0xFFFFFF) + 1, h = 1;
    } else if (dim == 2) {
        w = (fc[2] & 0x7FF) + 1, h = ((fc[2] >> 11) & 0x7FF) + 1, depth = ((fc[2] >> 22) & 0x3FF) + 1;
    } else {
        w = (fc[2] & 0x1FFF) + 1, h = ((fc[2] >> 13) & 0x1FFF) + 1;
    }
    uint32_t slices = dim == 3 ? 6 : depth;
    if (w > 8192 || h > 8192 || (dim == 3 && w != h) || (uint64_t)w * h * slices > (1u << 25))
        return 0;
    uint32_t endian = (fc[1] >> 6) & 3, tiled = fc[0] >> 31;
    if (tiled && tf->bytes == 12)
        return 0; /* 32_32_32_FLOAT is linear only */
    uint32_t pitch = ((fc[0] >> 22) & 0x1FF) << 5; /* texels */
    if (!pitch)
        pitch = w;
    unsigned bpb_log2 = tf->bytes == 1 ? 0 : tf->bytes == 2 ? 1 : tf->bytes == 4 ? 2 : tf->bytes == 8 ? 3 : 4;
    /* mip levels: from the mip address, up to the fetch constant's last level, stopping before a packed tail */
    uint32_t mip_addr = fc[5] & 0xFFFFF000u, max_level = (fc[4] >> 6) & 15, packed = (fc[5] >> 11) & 1;
    uint32_t mip_filter = (fc[3] >> 23) & 3;
    int levels = 1;
    if (mip_addr && max_level && mip_filter != 2 && dim != 2) {
        uint32_t p2w = pow2_ceil(w), p2h = pow2_ceil(dim == 0 ? 1 : h);
        for (uint32_t l = 1; l <= max_level && l < 14; l++) {
            uint32_t lw = p2w >> l ? p2w >> l : 1, lh = p2h >> l ? p2h >> l : 1;
            if (packed && (lw < lh ? lw : lh) <= 16)
                break; /* the packed tail: not decoded (left) */
            levels++;
            if (lw == 1 && lh == 1)
                break;
        }
    }
    out->levels = levels;
    out->is_float = tf->is_float;
    out->w = w, out->h = h, out->slices = slices, out->dim = dim;
    uint64_t hash = (uint64_t)fc[3] << 32 ^ (uint64_t)levels;
    uint32_t level_addr = fc[1] & 0xFFFFF000u, chain = mip_addr;
    for (int l = 0; l < levels; l++) {
        uint32_t lw = l ? (pow2_ceil(w) >> l ? pow2_ceil(w) >> l : 1) : w;
        uint32_t lh = l ? (pow2_ceil(h) >> l ? pow2_ceil(h) >> l : 1) : h;
        if (dim == 0)
            lh = 1;
        uint32_t lpitch = l ? lw : pitch;
        uint32_t bw = (lw + tf->block - 1) / tf->block, bh = (lh + tf->block - 1) / tf->block;
        uint32_t pitch_blocks = (((lpitch + tf->block - 1) / tf->block) + 31) & ~31u;
        uint32_t height_blocks = (bh + 31) & ~31u;
        uint32_t row_bytes = pitch_blocks * tf->bytes;
        uint32_t z_stride = row_bytes * (dim == 0 ? 1 : height_blocks);
        uint32_t face_stride = (z_stride + 4095) & ~4095u;
        if (l) {
            level_addr = chain;
            chain += (dim == 3 ? face_stride * 6 : (z_stride * slices + 4095) & ~4095u);
        }
        size_t slice_n = (size_t)bw * bh * tf->bytes, src_n = slice_n * slices;
        uint8_t *src = malloc(src_n);
        if (!src) {
            free_decoded(out);
            return 0;
        }
        for (uint32_t z = 0; z < slices; z++)
            for (uint32_t by = 0; by < bh; by++) {
                uint8_t *row = src + z * slice_n + (size_t)by * bw * tf->bytes;
                if (!tiled) { /* a whole row at once */
                    uint32_t off = z * (dim == 3 ? face_stride : z_stride) + by * row_bytes;
                    read_bytes(cp, level_addr + off, bw * tf->bytes, endian, row);
                    continue;
                }
                for (uint32_t bx = 0; bx < bw; bx++) {
                    uint32_t off = dim == 2 ? (uint32_t)tiled3d((int32_t)bx, (int32_t)by, (int32_t)z, pitch_blocks,
                                                                height_blocks, bpb_log2)
                                            : z * face_stride + (uint32_t)tiled2d((int32_t)bx, (int32_t)by,
                                                                                  pitch_blocks, bpb_log2);
                    read_bytes(cp, level_addr + off, tf->bytes, endian, row + (size_t)bx * tf->bytes);
                }
            }
        hash = (hash * 1099511628211ull) ^ fnv(src, src_n);
        uint32_t tw = bw * tf->block, th = bh * tf->block;
        out->lw[l] = tw, out->lh[l] = th, out->ld[l] = slices;
        if (l == 0)
            out->w = tw, out->h = th;
        if (hash_only) {
            free(src);
            continue;
        }
        size_t px = (size_t)tw * th * slices;
        out->level[l] = malloc(px * (tf->is_float ? 16 : 4));
        if (!out->level[l]) {
            free(src);
            free_decoded(out);
            return 0;
        }
        uint32_t sign = (fc[0] >> 2) & 3;
        for (uint32_t z = 0; z < slices; z++) {
            for (uint32_t by = 0; by < bh; by++)
                for (uint32_t bx = 0; bx < bw; bx++) {
                    const uint8_t *s = src + z * slice_n + ((size_t)by * bw + bx) * tf->bytes;
                    size_t at = (size_t)z * tw * th;
                    if (tf->block == 4) {
                        uint8_t q[16][4];
                        dxt_block(s, tf->format, q);
                        uint8_t *o = out->level[l];
                        for (int i = 0; i < 16; i++)
                            memcpy(o + (at + ((size_t)by * 4 + i / 4) * tw + bx * 4 + i % 4) * 4, q[i], 4);
                    } else if (tf->is_float) {
                        texel_f(tf->format, s, sign, (float *)out->level[l] + (at + (size_t)by * tw + bx) * 4);
                    } else {
                        texel(tf->format, s, (uint8_t *)out->level[l] + (at + (size_t)by * tw + bx) * 4);
                    }
                }
        }
        free(src);
    }
    out->hash = hash;
    return 1;
}

/* ---- resolve copies: the host pictures of what resolves wrote ---- */

static uint64_t memory_hash(const xenos_cp *cp, uint32_t base, uint32_t bytes)
{
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < bytes; i += 4)
        h = (h ^ (cp->read32 ? cp->read32(cp->ctx, (base + i) & 0x1FFFFFFFu) : 0)) * 1099511628211ull;
    return h;
}

/* the copy whose destination is `base`, still matching guest memory (checked once per epoch), or NULL */
static rt_copy *copy_at(xenos_gl *g, const xenos_cp *cp, uint32_t base)
{
    base &= 0x1FFFF000u;
    for (int i = 0; i < MAX_COPIES; i++) {
        rt_copy *c = &g->copies[i];
        if (!c->tex || c->base != base)
            continue;
        if (c->checked != g->epoch) {
            if (memory_hash(cp, c->base, c->bytes) != c->hash) { /* the CPU (or a texture upload) wrote it */
                g->glDeleteTextures(1, &c->tex);
                memset(c, 0, sizeof *c);
                return NULL;
            }
            c->checked = g->epoch;
        }
        c->used = ++g->copy_clock;
        return c;
    }
    return NULL;
}

/* copy a w x h guest-pixel region at (sx, sy) of texture `src` into (0, 0) of texture `dst` (both at the internal
 * resolution, compatible formats) */
static void blit_tex(xenos_gl *g, GLuint src, GLuint dst, int sx, int sy, int w, int h, int dx, int dy)
{
    int s = g->scale;
    g->glBindFramebuffer(GL_READ_FRAMEBUFFER, g->rfbo);
    g->glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, src, 0);
    g->glReadBuffer(GL_COLOR_ATTACHMENT0);
    g->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g->tfbo_draw);
    g->glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst, 0);
    static const GLenum b0 = GL_COLOR_ATTACHMENT0;
    g->glDrawBuffers(1, &b0);
    g->glDisable(GL_SCISSOR_TEST);
    g->glBlitFramebuffer(sx * s, sy * s, (sx + w) * s, (sy + h) * s, dx * s, dy * s, (dx + w) * s, (dy + h) * s,
                         GL_COLOR_BUFFER_BIT, GL_NEAREST);
    g->glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    g->glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    g->glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
}

/* the texture behind fetch constant k as a GL texture of the dimension the shader samples (1: 2D, 2: 3D, 3: cube).
 * A 2D texture a resolve wrote (memory unchanged since) is the resolve's host picture at the internal resolution;
 * any other cached texture is checked against its memory once per frame (and after each resolve, which may have
 * written it), decoded again when that changed. 0 if unsupported. */
static GLuint texture_of(xenos_gl *g, const xenos_cp *cp, const uint32_t *fc, uint32_t want_dim)
{
    uint32_t dim = (fc[5] >> 9) & 3, gl_dim = dim == 0 ? 1 : dim;
    if ((fc[0] & 3) != 2 || gl_dim != want_dim || !tex_format(fc[1] & 63))
        return 0;
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
    if (t && t->checked == g->epoch) { /* already checked this frame, no resolve since */
        t->used = (int)++g->tex_clock;
        return t->tex;
    }
    /* a resolve's picture */
    rt_copy *rc = dim == 1 ? copy_at(g, cp, fc[1]) : NULL;
    if (rc) {
        uint32_t w = (fc[2] & 0x1FFF) + 1, h = ((fc[2] >> 13) & 0x1FFF) + 1;
        if ((int)w <= rc->w && (int)h <= rc->h) {
            if (t && t->copy == 1 + (int)(rc - g->copies) && t->hash == rc->hash) {
                t->checked = g->epoch;
                t->used = (int)++g->tex_clock;
                return t->tex;
            }
            if (!t) {
                t = victim;
                memcpy(t->key, fc, sizeof t->key);
            }
            if (t->tex)
                g->glDeleteTextures(1, &t->tex);
            GLenum tf = GL_RGBA, tt = GL_UNSIGNED_BYTE;
            if (rc->ifmt == GL_RGB10_A2)
                tt = GL_UNSIGNED_INT_2_10_10_10_REV;
            else if (rc->ifmt == GL_RGBA16F || rc->ifmt == GL_RG16F)
                tf = rc->ifmt == GL_RG16F ? GL_RG : GL_RGBA, tt = GL_HALF_FLOAT;
            else if (rc->ifmt == GL_R32F || rc->ifmt == GL_RG32F)
                tf = rc->ifmt == GL_R32F ? GL_RED : GL_RG, tt = GL_FLOAT;
            t->tex = new_texture(g, rc->ifmt, tf, tt, (int)w * g->scale, (int)h * g->scale);
            blit_tex(g, rc->tex, t->tex, 0, 0, (int)w, (int)h, 0, 0);
            t->copy = 1 + (int)(rc - g->copies);
            t->hash = rc->hash;
            t->checked = g->epoch;
            t->used = (int)++g->tex_clock;
            g->st.copy_textures++;
            return t->tex;
        }
    }
    decoded d;
    if (t && !t->copy) {
        if (!decode_texture(cp, fc, 1, &d))
            return 0;
        if (d.hash == t->hash) {
            t->checked = g->epoch;
            t->used = (int)++g->tex_clock;
            return t->tex;
        }
    }
    if (!decode_texture(cp, fc, 0, &d))
        return 0;
    if (!t) {
        t = victim;
        memcpy(t->key, fc, sizeof t->key);
    }
    if (t->tex) /* a texture object keeps its first target and sizes: a new one */
        g->glDeleteTextures(1, &t->tex);
    g->glGenTextures(1, &t->tex);
    t->copy = 0;
    t->hash = d.hash;
    t->checked = g->epoch;
    t->used = (int)++g->tex_clock;
    GLint ifmt = d.is_float ? (GLint)GL_RGBA16F : (GLint)GL_RGBA8;
    GLenum type = d.is_float ? GL_FLOAT : GL_UNSIGNED_BYTE;
    size_t texel_n = d.is_float ? 16 : 4;
    GLenum target = gl_dim == 1 ? GL_TEXTURE_2D : gl_dim == 3 ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_3D;
    g->glBindTexture(target, t->tex);
    for (int l = 0; l < d.levels; l++) {
        const uint8_t *px = d.level[l];
        if (gl_dim == 1) {
            g->glTexImage2D(GL_TEXTURE_2D, l, ifmt, (GLsizei)d.lw[l], (GLsizei)d.lh[l], 0, GL_RGBA, type, px);
        } else if (gl_dim == 3) {
            for (uint32_t f = 0; f < 6; f++)
                g->glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, l, ifmt, (GLsizei)d.lw[l], (GLsizei)d.lh[l], 0,
                                GL_RGBA, type, px + (size_t)f * d.lw[l] * d.lh[l] * texel_n);
        } else {
            g->glTexImage3D(GL_TEXTURE_3D, l, ifmt, (GLsizei)d.lw[l], (GLsizei)d.lh[l], (GLsizei)d.ld[l], 0, GL_RGBA,
                            type, px);
        }
    }
    g->glTexParameteri(target, GL_TEXTURE_BASE_LEVEL, 0);
    g->glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, d.levels - 1);
    if (d.levels > 1)
        g->st.mipmapped++;
    free_decoded(&d);
    return t->tex;
}

static GLint wrap_mode(uint32_t c)
{
    return c == 0 ? GL_REPEAT : c == 1 ? GL_MIRRORED_REPEAT : GL_CLAMP_TO_EDGE;
}

/* a texture's sampling state from its fetch constant: wraps, filters (mips when it has them), swizzle */
static void sampler_state(xenos_gl *g, GLenum target, const uint32_t *fc, int mipped)
{
    static const GLint swz[6] = {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA, GL_ZERO, GL_ONE};
    g->glTexParameteri(target, GL_TEXTURE_WRAP_S, wrap_mode((fc[0] >> 10) & 7));
    g->glTexParameteri(target, GL_TEXTURE_WRAP_T, wrap_mode((fc[0] >> 13) & 7));
    g->glTexParameteri(target, GL_TEXTURE_WRAP_R, wrap_mode((fc[0] >> 16) & 7));
    int mag = ((fc[3] >> 19) & 3) == 1, min = ((fc[3] >> 21) & 3) == 1, mip = ((fc[3] >> 23) & 3) == 1;
    if (g->tex_linear) /* XBA-282: the app's texture filter */
        mag = min = mip = 1;
    if (g->tex_aniso > 1)
        g->glTexParameterf(target, 0x84FE /* GL_TEXTURE_MAX_ANISOTROPY_EXT */, mipped ? g->tex_aniso : 1.0f);
    g->glTexParameteri(target, GL_TEXTURE_MAG_FILTER, mag ? GL_LINEAR : GL_NEAREST);
    g->glTexParameteri(target, GL_TEXTURE_MIN_FILTER,
                       !mipped ? (min ? GL_LINEAR : GL_NEAREST)
                       : min   ? (mip ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST)
                               : (mip ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST));
    for (int c = 0; c < 4; c++) {
        uint32_t s = (fc[3] >> (1 + 3 * c)) & 7;
        g->glTexParameteri(target, GL_TEXTURE_SWIZZLE_R + (GLenum)c, s < 6 ? swz[s] : GL_ZERO);
    }
}

/* what the texture queries read of a fetch constant (XBA-296): the guest size, and two bits an axis for its border
 * mode: 1 clamp to border (clamp mode 6), 2 mirror then clamp to border (7), 0 for the others */
static void set_tquery(xenos_gl *g, GLint loc, const uint32_t *fc)
{
    uint32_t dim = (fc[5] >> 9) & 3, w, h = 1, d = 1, modes = 0;
    if (dim == 0)
        w = (fc[2] & 0xFFFFFF) + 1;
    else if (dim == 2)
        w = (fc[2] & 0x7FF) + 1, h = ((fc[2] >> 11) & 0x7FF) + 1, d = ((fc[2] >> 22) & 0x3FF) + 1;
    else
        w = (fc[2] & 0x1FFF) + 1, h = ((fc[2] >> 13) & 0x1FFF) + 1;
    for (int axis = 0; axis < 3; axis++) {
        uint32_t c = (fc[0] >> (10 + 3 * axis)) & 7;
        modes |= (c == 6 ? 1u : c == 7 ? 2u : 0u) << (2 * axis);
    }
    g->glUniform4f(loc, (GLfloat)w, (GLfloat)h, (GLfloat)d, (GLfloat)modes);
}

static void bind_textures(xenos_gl *g, const xenos_cp *cp, const program *p)
{
    for (int i = 0; i < p->ntq; i++)
        if (p->tq_loc[i] >= 0)
            set_tquery(g, p->tq_loc[i], &cp->regs[0x4800 + 6 * (p->tq_fetch[i] & 31)]);
    for (int i = 0; i < p->nsamplers; i++) {
        uint32_t k = p->sampler_fetch[i], dim = p->sampler_dim[i];
        GLenum target = dim == 2 ? GL_TEXTURE_3D : dim == 3 ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
        const uint32_t *fc = &cp->regs[0x4800 + 6 * (k & 31)];
        GLuint tex = k < 32 ? texture_of(g, cp, fc, dim) : 0;
        g->glActiveTexture(GL_TEXTURE0 + (GLenum)i);
        g->glBindTexture(target, tex);
        if (!tex) {
            g->st.unsupported_textures++;
            continue;
        }
        int mipped = 0;
        for (int t = 0; t < MAX_TEXTURES; t++)
            if (g->textures[t].tex == tex) {
                mipped = !g->textures[t].copy && ((fc[4] >> 6) & 15) && (fc[5] & 0xFFFFF000u) &&
                         ((fc[3] >> 23) & 3) != 2 && dim != 2;
                break;
            }
        sampler_state(g, target, fc, mipped);
    }
    g->glActiveTexture(GL_TEXTURE0);
}

/* ---- resolves ----
 * A draw with RB_MODECONTROL's eDRAM mode "copy" is a resolve (Xenia's draw_util.cc GetResolveInfo): the rectangle
 * comes from the three window-space vertices Direct3D 9 leaves at vertex fetch constant 0; RB_COPY_CONTROL says
 * which surface is the source (render target 0-3 as last bound, else the one RB_COLORn_INFO names), whether the
 * colour is copied out (raw / convert) and whether colour and depth are cleared (RB_COLOR_CLEAR / _LO as the raw
 * eDRAM value of the source's format, X in the low bits; RB_DEPTH_CLEAR as depth 24 : stencil 8); the copy goes to
 * RB_COPY_DEST_BASE as a tiled texture of RB_COPY_DEST_PITCH (pitch, height) in RB_COPY_DEST_INFO's format, endian
 * swap, red / blue swap and exponent bias, at the same pixel coordinates as the source (Direct3D adjusts the base
 * for offsets). Destination formats: 8, 8_8, 8_8_8_8 (and _A, _GAMMA), 1_5_5_5, 5_6_5, 6_5_5, 4_4_4_4, 2_10_10_10,
 * 10_11_11, 11_11_10, 16, 16_16, 16_16_16_16, their _FLOAT forms, 32_FLOAT, 32_32_FLOAT, 32_32_32_32_FLOAT. The
 * source is read at the guest resolution (scaled surfaces filtered down); the destination also keeps the host
 * picture at the internal resolution (rt_copy) while its memory is unchanged. */

static void upload_fmt(GLenum ifmt, GLenum *tf, GLenum *tt)
{
    *tf = GL_RGBA, *tt = GL_UNSIGNED_BYTE;
    if (ifmt == GL_RGB10_A2)
        *tt = GL_UNSIGNED_INT_2_10_10_10_REV;
    else if (ifmt == GL_RGBA16F)
        *tt = GL_HALF_FLOAT;
    else if (ifmt == GL_RG16F)
        *tf = GL_RG, *tt = GL_HALF_FLOAT;
    else if (ifmt == GL_R32F)
        *tf = GL_RED, *tt = GL_FLOAT;
    else if (ifmt == GL_RG32F)
        *tf = GL_RG, *tt = GL_FLOAT;
    else if (ifmt == GL_RGBA32F)
        *tt = GL_FLOAT;
}

static int is_float_fmt(GLenum ifmt)
{
    return ifmt == GL_RGBA16F || ifmt == GL_RG16F || ifmt == GL_R32F || ifmt == GL_RG32F || ifmt == GL_RGBA32F;
}

/* a w x h guest-pixel region at (x0, y0) of a colour texture as RGBA floats, top row first (scaled textures
 * filtered down to the guest resolution first); NULL when out of memory */
static float *read_pixels(xenos_gl *g, GLuint tex, GLenum ifmt, int x0, int y0, int w, int h)
{
    int s = g->scale;
    GLuint from = tex;
    int fx = x0, fy = y0;
    if (s > 1) {
        if (!g->rtex || g->rtex_fmt != ifmt || g->rtex_w < w || g->rtex_h < h) {
            if (g->rtex)
                g->glDeleteTextures(1, &g->rtex);
            GLenum tf, tt;
            upload_fmt(ifmt, &tf, &tt);
            g->rtex_w = w > g->rtex_w ? w : g->rtex_w, g->rtex_h = h > g->rtex_h ? h : g->rtex_h;
            g->rtex = new_texture(g, ifmt, tf, tt, g->rtex_w, g->rtex_h);
            g->rtex_fmt = ifmt;
        }
        g->glBindFramebuffer(GL_READ_FRAMEBUFFER, g->rfbo);
        g->glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        g->glReadBuffer(GL_COLOR_ATTACHMENT0);
        g->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g->tfbo_draw);
        g->glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g->rtex, 0);
        static const GLenum b0 = GL_COLOR_ATTACHMENT0;
        g->glDrawBuffers(1, &b0);
        g->glDisable(GL_SCISSOR_TEST);
        g->glBlitFramebuffer(x0 * s, y0 * s, (x0 + w) * s, (y0 + h) * s, 0, 0, w, h, GL_COLOR_BUFFER_BIT,
                             ifmt == GL_R32F || ifmt == GL_RG32F || ifmt == GL_RGBA32F ? GL_NEAREST : GL_LINEAR);
        g->glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        from = g->rtex, fx = 0, fy = 0;
    }
    g->glBindFramebuffer(GL_FRAMEBUFFER, g->rfbo);
    g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, from, 0);
    g->glReadBuffer(GL_COLOR_ATTACHMENT0);
    size_t n = (size_t)w * h;
    float *out = malloc(n * 16);
    void *raw = malloc(n * 16);
    if (out && raw) {
        if (is_float_fmt(ifmt)) {
            g->glReadPixels(fx, fy, w, h, GL_RGBA, GL_FLOAT, out);
        } else if (ifmt == GL_RGB10_A2) {
            g->glReadPixels(fx, fy, w, h, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, raw);
            const uint32_t *v = raw;
            for (size_t i = 0; i < n; i++) {
                out[4 * i] = (float)(v[i] & 1023) / 1023.0f, out[4 * i + 1] = (float)((v[i] >> 10) & 1023) / 1023.0f;
                out[4 * i + 2] = (float)((v[i] >> 20) & 1023) / 1023.0f, out[4 * i + 3] = (float)(v[i] >> 30) / 3.0f;
            }
        } else {
            g->glReadPixels(fx, fy, w, h, GL_RGBA, GL_UNSIGNED_BYTE, raw);
            const uint8_t *b = raw;
            for (size_t i = 0; i < 4 * n; i++)
                out[i] = (float)b[i] / 255.0f;
        }
    } else {
        free(out);
        out = NULL;
    }
    free(raw);
    g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    g->glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    return out;
}

static uint32_t unorm(float f, unsigned bits)
{
    float m = (float)((1u << bits) - 1);
    f = f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
    return (uint32_t)(f * m + 0.5f);
}

static uint32_t snorm16(float f)
{
    f = f < -1.0f ? -1.0f : f > 1.0f ? 1.0f : f;
    return (uint32_t)(uint16_t)(int16_t)lrintf(f * 32767.0f);
}

static uint32_t float_to_half(float f)
{
    uint32_t x;
    memcpy(&x, &f, 4);
    uint32_t s = (x >> 16) & 0x8000u;
    int e = (int)((x >> 23) & 0xFF) - 127 + 15;
    uint32_t m = x & 0x7FFFFFu;
    if (((x >> 23) & 0xFF) == 0xFF)
        return s | 0x7BFF; /* no infinity on Xenos: the largest value */
    if (e >= 31)
        return s | 0x7BFF;
    if (e <= 0) {
        if (e < -10)
            return s;
        m |= 0x800000u;
        uint32_t sh = (uint32_t)(14 - e);
        return s | ((m + (1u << (sh - 1))) >> sh);
    }
    uint32_t h = s | (uint32_t)e << 10 | m >> 13;
    if (m & 0x1000u)
        h++; /* round half up (carries into the exponent correctly) */
    return h;
}

/* one pixel (RGBA floats) in a destination texture format, X in the low bits: the byte count, 0 if unsupported */
static int encode_texel(uint32_t fmt, const float c[4], uint8_t out[16])
{
    uint32_t v = 0, v2 = 0;
    int n = 4;
    switch (fmt) {
    case 2: out[0] = (uint8_t)unorm(c[0], 8); return 1;
    case 10: v = unorm(c[0], 8) | unorm(c[1], 8) << 8, n = 2; break;
    case 6:
    case 14:
    case 50:
    case 62: v = unorm(c[0], 8) | unorm(c[1], 8) << 8 | unorm(c[2], 8) << 16 | unorm(c[3], 8) << 24; break;
    case 3: v = unorm(c[0], 5) | unorm(c[1], 5) << 5 | unorm(c[2], 5) << 10 | (c[3] >= 0.5f) << 15, n = 2; break;
    case 4: v = unorm(c[0], 5) | unorm(c[1], 6) << 5 | unorm(c[2], 5) << 11, n = 2; break;
    case 5: v = unorm(c[0], 5) | unorm(c[1], 5) << 5 | unorm(c[2], 6) << 10, n = 2; break;
    case 15: v = unorm(c[0], 4) | unorm(c[1], 4) << 4 | unorm(c[2], 4) << 8 | unorm(c[3], 4) << 12, n = 2; break;
    case 7:
    case 54: v = unorm(c[0], 10) | unorm(c[1], 10) << 10 | unorm(c[2], 10) << 20 | unorm(c[3], 2) << 30; break;
    case 16: v = unorm(c[0], 11) | unorm(c[1], 11) << 11 | unorm(c[2], 10) << 22; break;
    case 17: v = unorm(c[0], 10) | unorm(c[1], 11) << 10 | unorm(c[2], 11) << 21; break;
    case 24: v = unorm(c[0], 16), n = 2; break;
    case 25: v = unorm(c[0], 16) | unorm(c[1], 16) << 16; break;
    case 26: v = unorm(c[0], 16) | unorm(c[1], 16) << 16, v2 = unorm(c[2], 16) | unorm(c[3], 16) << 16, n = 8; break;
    case 30: v = float_to_half(c[0]), n = 2; break;
    case 31: v = float_to_half(c[0]) | float_to_half(c[1]) << 16; break;
    case 32:
        v = float_to_half(c[0]) | float_to_half(c[1]) << 16, v2 = float_to_half(c[2]) | float_to_half(c[3]) << 16, n = 8;
        break;
    case 36: memcpy(out, &c[0], 4); return 4;
    case 37: memcpy(out, c, 8); return 8;
    case 38: memcpy(out, c, 16); return 16;
    default: return 0;
    }
    (void)snorm16;
    for (int i = 0; i < n && i < 4; i++)
        out[i] = (uint8_t)(v >> (8 * i));
    for (int i = 4; i < n; i++)
        out[i] = (uint8_t)(v2 >> (8 * (i - 4)));
    return n;
}

/* n bytes the GPU writes at a guest physical address through its swap mode (read-modify-write of partial dwords) */
static void put_bytes(const xenos_cp *cp, uint32_t addr, const uint8_t *b, int n, uint32_t endian)
{
    int i = 0;
    while (i < n) {
        uint32_t a = addr + (uint32_t)i, wa = a & ~3u, word;
        if ((a & 3) == 0 && n - i >= 4) {
            word = b[i] | (uint32_t)b[i + 1] << 8 | (uint32_t)b[i + 2] << 16 | (uint32_t)b[i + 3] << 24;
            i += 4;
        } else {
            word = gpu_read(cp, wa, endian);
            for (uint32_t k = a & 3; k < 4 && i < n; k++, i++)
                word = (word & ~(0xFFu << (8 * k))) | (uint32_t)b[i] << (8 * k);
        }
        cp->write32(cp->ctx, wa & 0x1FFFFFFFu, __builtin_bswap32(gpu_swap(word, endian)));
    }
}

/* the source surface of a resolve: render target src (0-3) or depth (4), as last bound, else as the registers say */
static int resolve_source(xenos_gl *g, const xenos_cp *cp, uint32_t src)
{
    static const uint32_t info_reg[4] = {0x2001, 0x2003, 0x2004, 0x2005};
    uint32_t si = cp->regs[0x2000], pitch = si & 0x3FFF, msaa = (si >> 16) & 3;
    if (src < 4) {
        uint32_t info = cp->regs[info_reg[src]];
        int c = g->cur[src];
        if (c >= 0 && g->surf[c].tex && g->surf[c].base == (info & 0xFFF))
            return c;
        return get_surface(g, info & 0xFFF, pitch, (info >> 16) & 15, 0, msaa, 0, 0);
    }
    uint32_t dinfo = cp->regs[0x2002];
    int c = g->cur_ds;
    if (c >= 0 && g->surf[c].tex && g->surf[c].base == (dinfo & 0xFFF))
        return c;
    return get_surface(g, dinfo & 0xFFF, pitch, (dinfo >> 16) & 1, 1, msaa, 0, 0);
}

/* the raw eDRAM clear value of a colour format as floats (RB_COLOR_CLEAR, RB_COLOR_CLEAR_LO for 64-bit formats) */
static void clear_colour(uint32_t fmt, uint32_t hi, uint32_t lo, float c[4])
{
    switch (fmt & 15) {
    case 2:
    case 10:
        c[0] = (float)(hi & 1023) / 1023.0f, c[1] = (float)((hi >> 10) & 1023) / 1023.0f;
        c[2] = (float)((hi >> 20) & 1023) / 1023.0f, c[3] = (float)(hi >> 30) / 3.0f;
        break;
    case 5:
    case 7:
    case 3:
    case 12:
    case 4:
        c[0] = half_bits(lo & 0xFFFF), c[1] = half_bits(lo >> 16), c[2] = half_bits(hi & 0xFFFF);
        c[3] = half_bits(hi >> 16);
        break;
    case 6: c[0] = half_bits(hi & 0xFFFF), c[1] = half_bits(hi >> 16), c[2] = 0, c[3] = 1; break;
    case 14: memcpy(&c[0], &hi, 4), c[1] = c[2] = 0, c[3] = 1; break;
    case 15: memcpy(&c[0], &lo, 4), memcpy(&c[1], &hi, 4), c[2] = 0, c[3] = 1; break;
    default: /* 8_8_8_8: X (red) in the low byte */
        c[0] = (float)(hi & 0xFF) / 255.0f, c[1] = (float)((hi >> 8) & 0xFF) / 255.0f;
        c[2] = (float)((hi >> 16) & 0xFF) / 255.0f, c[3] = (float)(hi >> 24) / 255.0f;
    }
}

/* Depth out of the eDRAM (a resolve whose source is the depth buffer): GLES 3.0 reads no depth with glReadPixels, so
 * a pass samples the depth texture (texelFetch at the guest pixel's host texel) and packs its 24 bits into an RGBA8
 * target read back as bytes; the stencil is not readable in GLES 3.0 (0). Returns w * h depth values (0..2^24-1),
 * top row first, or NULL. */
static const char DEPTH_VS[] =
    "#version 300 es\nvoid main() { vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));"
    " gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";
static const char DEPTH_FS[] =
    "#version 300 es\nprecision highp float;\nprecision highp int;\nuniform highp sampler2D d;\nuniform ivec3 at;\n"
    "out vec4 c;\nvoid main() { ivec2 q = (ivec2(gl_FragCoord.xy) + at.xy) * at.z;\n"
    " uint v = uint(clamp(texelFetch(d, q, 0).r, 0.0, 1.0) * 16777215.0 + 0.5);\n"
    " c = vec4(float(v & 255u), float((v >> 8) & 255u), float(v >> 16), 0.0) / 255.0; }\n";

static uint32_t *read_depth(xenos_gl *g, const surface *s, int x0, int y0, int w, int h)
{
    if (!g->dprog && !g->dprog_failed) {
        GLuint v = compile(g, GL_VERTEX_SHADER, DEPTH_VS, "depth resolve vs");
        GLuint f = compile(g, GL_FRAGMENT_SHADER, DEPTH_FS, "depth resolve fs");
        GLint ok = 0;
        if (v && f) {
            g->dprog = g->glCreateProgram();
            g->glAttachShader(g->dprog, v);
            g->glAttachShader(g->dprog, f);
            g->glLinkProgram(g->dprog);
            g->glGetProgramiv(g->dprog, GL_LINK_STATUS, &ok);
        }
        if (v)
            g->glDeleteShader(v);
        if (f)
            g->glDeleteShader(f);
        if (!ok) {
            if (g->dprog)
                g->glDeleteProgram(g->dprog);
            g->dprog = 0;
            g->dprog_failed = 1;
            return NULL;
        }
        g->dprog_at = g->glGetUniformLocation(g->dprog, "at");
    }
    if (!g->dprog)
        return NULL;
    if (!g->dtex || g->dtex_w < w || g->dtex_h < h) {
        if (g->dtex)
            g->glDeleteTextures(1, &g->dtex);
        g->dtex_w = w > g->dtex_w ? w : g->dtex_w, g->dtex_h = h > g->dtex_h ? h : g->dtex_h;
        g->dtex = new_texture(g, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, g->dtex_w, g->dtex_h);
    }
    g->glBindFramebuffer(GL_FRAMEBUFFER, g->rfbo);
    g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g->dtex, 0);
    static const GLenum b0 = GL_COLOR_ATTACHMENT0;
    g->glDrawBuffers(1, &b0);
    g->glDisable(GL_SCISSOR_TEST);
    g->glDisable(GL_BLEND);
    g->glDisable(GL_DEPTH_TEST);
    g->glDisable(GL_STENCIL_TEST);
    g->glDisable(GL_CULL_FACE);
    g->glColorMask(1, 1, 1, 1);
    g->glViewport(0, 0, w, h);
    g->glUseProgram(g->dprog);
    g->glActiveTexture(GL_TEXTURE0);
    g->glBindTexture(GL_TEXTURE_2D, s->tex);
    g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    g->glUniform1i(g->glGetUniformLocation(g->dprog, "d"), 0);
    g->glUniform3i(g->dprog_at, x0, y0, g->scale);
    g->glBindVertexArray(g->vao);
    g->glDrawArrays(4 /* GL_TRIANGLES */, 0, 3);
    size_t n = (size_t)w * h;
    uint8_t *b = malloc(n * 4);
    uint32_t *out = b ? malloc(n * 4) : NULL;
    if (out) {
        g->glReadBuffer(GL_COLOR_ATTACHMENT0);
        g->glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, b);
        for (size_t i = 0; i < n; i++)
            out[i] = b[4 * i] | (uint32_t)b[4 * i + 1] << 8 | (uint32_t)b[4 * i + 2] << 16;
    }
    free(b);
    g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    g->glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    g->glViewport(0, 0, g->tw * g->scale, g->th * g->scale);
    return out;
}

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
    uint32_t ctl = cp->regs[0x2318], cmd = (ctl >> 20) & 3, src = ctl & 7;
    if (src > 4) {
        g->st.unsupported++;
        return;
    }
    int si = resolve_source(g, cp, src);
    surface *s = &g->surf[si];
    x0 = x0 < 0 ? 0 : x0, y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > s->w ? s->w : x1, y1 = y1 > s->h ? s->h : y1;
    if (x1 <= x0 || y1 <= y0) {
        g->st.unsupported++;
        return;
    }
    int sc = g->scale;
    if (cmd <= 1 && src < 4) {
        uint32_t info = cp->regs[0x231B], fmt = (info >> 7) & 63, endian = info & 7, swap_rb = (info >> 24) & 1;
        int bias = (int)((int32_t)(info << 10) >> 26); /* copy_dest_exp_bias, bits 16-21 */
        uint32_t pitch = cp->regs[0x231A] & 0x3FFF, height = (cp->regs[0x231A] >> 16) & 0x3FFF;
        uint32_t base = cp->regs[0x2319] & 0x1FFFFFFFu;
        uint32_t pitch_aligned = (pitch + 31) & ~31u;
        uint8_t probe[16];
        static const float zero[4] = {0, 0, 0, 0};
        int bpp = encode_texel(fmt, zero, probe);
        unsigned bpp_log2 = bpp == 1 ? 0 : bpp == 2 ? 1 : bpp == 4 ? 2 : bpp == 8 ? 3 : 4;
        float *px = bpp && endian < 4 && cp->write32 && cp->read32
                        ? read_pixels(g, s->tex, s->ifmt, x0, y0, x1 - x0, y1 - y0)
                        : NULL;
        if (px) {
            float k = bias ? ldexpf(1.0f, bias) : 1.0f;
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) {
                    const float *p = px + ((size_t)(y - y0) * (x1 - x0) + (x - x0)) * 4;
                    float c[4] = {(swap_rb ? p[2] : p[0]) * k, p[1] * k, (swap_rb ? p[0] : p[2]) * k, p[3] * k};
                    uint8_t b[16];
                    int n = encode_texel(fmt, c, b);
                    uint32_t off = (uint32_t)tiled2d(x, y, pitch_aligned, bpp_log2);
                    put_bytes(cp, base + off, b, n, endian);
                }
            free(px);
            g->st.resolves++;
            g->epoch++;
            if (g->nresolved < 16)
                g->resolved[g->nresolved++] = base & 0x1FFFF000u;
            /* the host picture at the internal resolution (not with a red / blue swap: memory's X would be blue) */
            if (!height)
                height = (uint32_t)y1;
            uint32_t height_aligned = (height + 31) & ~31u;
            rt_copy *rc = NULL, *victim = &g->copies[0];
            for (int i = 0; i < MAX_COPIES; i++) {
                rt_copy *c = &g->copies[i];
                if (c->tex && c->base == (base & 0x1FFFF000u)) {
                    rc = c;
                    break;
                }
                if (!c->tex || (victim->tex && c->used < victim->used))
                    victim = c;
            }
            if (swap_rb || s->kind == 2 || (base & 0xFFF)) {
                if (rc) {
                    g->glDeleteTextures(1, &rc->tex);
                    memset(rc, 0, sizeof *rc);
                }
            } else {
                if (!rc)
                    rc = victim;
                if (rc->tex && (rc->ifmt != s->ifmt || rc->w != (int)pitch_aligned || rc->h != (int)height_aligned)) {
                    g->glDeleteTextures(1, &rc->tex);
                    rc->tex = 0;
                }
                if (!rc->tex) {
                    GLenum tf, tt;
                    upload_fmt(s->ifmt, &tf, &tt);
                    rc->tex = new_texture(g, s->ifmt, tf, tt, (int)pitch_aligned * sc, (int)height_aligned * sc);
                    rc->ifmt = s->ifmt, rc->w = (int)pitch_aligned, rc->h = (int)height_aligned;
                }
                rc->base = base & 0x1FFFF000u;
                rc->bytes = pitch_aligned * height_aligned * (uint32_t)bpp;
                int cw = x1 < rc->w ? x1 - x0 : rc->w - x0, ch = y1 < rc->h ? y1 - y0 : rc->h - y0;
                if (cw > 0 && ch > 0)
                    blit_tex(g, s->tex, rc->tex, x0, y0, cw, ch, x0, y0);
                rc->hash = memory_hash(cp, rc->base, rc->bytes);
                rc->checked = g->epoch;
                rc->used = ++g->copy_clock;
            }
        } else {
            g->st.unsupported++;
        }
    } else if (cmd <= 1) { /* depth out: 24_8 (depth in the high 24 bits, stencil 0), tiled 32-bit */
        uint32_t info = cp->regs[0x231B], fmt = (info >> 7) & 63, endian = info & 7;
        uint32_t pitch = cp->regs[0x231A] & 0x3FFF, base = cp->regs[0x2319] & 0x1FFFFFFFu;
        uint32_t *dv = (fmt == 22 || fmt == 6 || fmt == 33) && endian < 4 && cp->write32 && cp->read32
                           ? read_depth(g, s, x0, y0, x1 - x0, y1 - y0)
                           : NULL;
        if (dv) {
            uint32_t pitch_aligned = (pitch + 31) & ~31u;
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) {
                    uint32_t d = dv[(size_t)(y - y0) * (x1 - x0) + (x - x0)] << 8;
                    uint8_t b4[4] = {(uint8_t)d, (uint8_t)(d >> 8), (uint8_t)(d >> 16), (uint8_t)(d >> 24)};
                    put_bytes(cp, base + (uint32_t)tiled2d(x, y, pitch_aligned, 2), b4, 4, endian);
                }
            free(dv);
            g->st.resolves++;
            g->st.depth_resolves++;
            g->epoch++;
        } else {
            g->st.unsupported++;
        }
    }
    int clear_c = (ctl >> 8) & 1, clear_d = (ctl >> 9) & 1;
    if (clear_c || clear_d) {
        g->glBindFramebuffer(GL_FRAMEBUFFER, g->rfbo);
        g->glEnable(GL_SCISSOR_TEST);
        g->glScissor(x0 * sc, y0 * sc, (x1 - x0) * sc, (y1 - y0) * sc);
        if (clear_c && src < 4) { /* the resolved render target (none when the source is depth) */
            float colour[4];
            clear_colour(s->fmt, cp->regs[0x231E], cp->regs[0x231F], colour);
            g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->tex, 0);
            static const GLenum b0 = GL_COLOR_ATTACHMENT0;
            g->glDrawBuffers(1, &b0);
            g->glColorMask(1, 1, 1, 1);
            g->glClearBufferfv(0x1800 /* GL_COLOR */, 0, colour);
            g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        }
        if (clear_d) {
            int di = resolve_source(g, cp, 4);
            uint32_t dc = cp->regs[0x231D];
            g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, g->surf[di].tex, 0);
            g->glDepthMask(1);
            g->glStencilMaskSeparate(GL_FRONT_AND_BACK, 0xFF);
            g->glClearDepthf((float)(dc >> 8) / 16777215.0f);
            g->glClearStencil((GLint)(dc & 0xFF));
            g->glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
        }
        g->glDisable(GL_SCISSOR_TEST);
        g->glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
        g->title_clears = 1; /* the title clears through resolves: swaps stop clearing */
    }
}

/* the vertex buffers a program fetches at computed indices: each whole buffer (the fetch constant's size in dwords
 * over the stride) converted to vec4s in an RGBA32F texture of 1024 elements a row, on the units after the
 * samplers. 0 when a format is not handled. */
static int bind_vertex_buffers(xenos_gl *g, const xenos_cp *cp, const program *p)
{
    for (int i = 0; i < p->nvb; i++) {
        const attr *at = &p->vbufs[i];
        uint32_t c0 = cp->regs[0x4800 + 2 * at->fetch_constant], c1 = cp->regs[0x4800 + 2 * at->fetch_constant + 1];
        uint32_t size = (c1 >> 2) & 0xFFFFFF, stride = at->stride ? at->stride : 1;
        uint32_t count = size / stride;
        if (!count)
            count = 1;
        if (count > (1u << 20))
            count = 1u << 20;
        uint32_t rows = (count + 1023) / 1024, wid = count < 1024 ? count : 1024;
        float *buf = calloc((size_t)wid * rows, 16);
        if (!buf)
            return 0;
        for (uint32_t v = 0; v < count; v++)
            if (!fetch_vertex(cp, c0 & ~3u, c1 & 3, at, v, &buf[4 * v])) {
                free(buf);
                return 0;
            }
        if (!g->vbtex[i])
            g->glGenTextures(1, &g->vbtex[i]);
        g->glActiveTexture(GL_TEXTURE0 + (GLenum)(p->nsamplers + i));
        g->glBindTexture(GL_TEXTURE_2D, g->vbtex[i]);
        g->glTexImage2D(GL_TEXTURE_2D, 0, (GLint)GL_RGBA32F, (GLsizei)wid, (GLsizei)rows, 0, GL_RGBA, GL_FLOAT, buf);
        g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        free(buf);
        g->st.computed_fetches++;
    }
    g->glActiveTexture(GL_TEXTURE0);
    return 1;
}

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

/* ---- memory export (XBA-282) ----
 * A draw whose vertex shader exports to memory logs (byte address, dword in memory order) pairs into a storage
 * buffer (xenos_glsl.py MEMEXPORT); after the draw the log is read back and written to guest memory through the
 * command processor's write32 (big-endian words: the pair's bytes as they lie in memory). Textures and vertex buffers
 * see the writes at their next check. A log that overflows keeps its first MX_CAPACITY dwords (counted). */
#define GL_SHADER_STORAGE_BUFFER 0x90D2
#define GL_DYNAMIC_READ 0x88E9
#define MX_CAPACITY (1u << 18)

static void memexport_begin(xenos_gl *g)
{
    uint32_t head[2] = {0, MX_CAPACITY};
    if (!g->mxbuf) {
        g->glGenBuffers(1, &g->mxbuf);
        g->glBindBuffer(GL_SHADER_STORAGE_BUFFER, g->mxbuf);
        g->glBufferData(GL_SHADER_STORAGE_BUFFER, 8 + 8 * (GLsizeiptr)MX_CAPACITY, NULL, GL_DYNAMIC_READ);
    }
    g->glBindBuffer(GL_SHADER_STORAGE_BUFFER, g->mxbuf);
    g->glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof head, head);
    g->glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, g->mxbuf);
}

static void memexport_end(xenos_gl *g, const xenos_cp *cp)
{
    g->glMemoryBarrier(0xFFFFFFFFu); /* GL_ALL_BARRIER_BITS: the shader's writes before the map */
    g->glBindBuffer(GL_SHADER_STORAGE_BUFFER, g->mxbuf);
    const uint32_t *head = g->glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, 8, GL_MAP_READ_BIT);
    uint32_t n = head ? head[0] : 0;
    if (head)
        g->glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
    if (n > MX_CAPACITY) {
        g->st.memexport_dropped += n - MX_CAPACITY;
        n = MX_CAPACITY;
    }
    if (n && cp->write32) {
        const uint32_t *log = g->glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 8, 8 * (GLsizeiptr)n, GL_MAP_READ_BIT);
        if (log) {
            for (uint32_t i = 0; i < n; i++) /* the dword's bytes in memory order: big-endian word = its bswap */
                cp->write32(cp->ctx, log[2 * i] & 0x1FFFFFFCu, __builtin_bswap32(log[2 * i + 1]));
            g->glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
        }
        g->st.memexport_dwords += n;
    }
    g->st.memexport_draws++;
    g->epoch++; /* memory changed under the cached textures: compare them again */
}

void xenos_gl_draw(xenos_gl *g, const xenos_cp *cp, const xenos_draw *d)
{
    if (g->reset_pending)
        do_reset(g);
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
    /* VGT_INDX_OFFSET is added to every index (Direct3D's base vertex); with PA_SU_SC_MODE_CNTL's
     * multi_prim_ib_ena, VGT_MULTI_PRIM_IB_RESET_INDX restarts a strip / fan (GL's fixed restart index) */
    uint32_t index_offset = cp->regs[0x2102];
    int restart = d->source_select == 0 && (cp->regs[0x2205] >> 21) & 1;
    uint32_t reset_index = cp->regs[0x2103] & (d->index_size ? 0xFFFFFFFFu : 0xFFFFu);
    int restarted = 0;
    if (d->source_select == 0 || index_offset) {
        idx = malloc(4 * (size_t)n);
        if (!idx)
            return;
        uint32_t endian = cp->regs[XENOS_REG_VGT_DMA_SIZE] >> 30;
        max_index = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t v;
            if (d->source_select != 0) {
                v = i;
            } else if (d->index_size) {
                v = gpu_read(cp, d->dma_base + 4 * i, endian);
            } else {
                uint32_t w = gpu_read(cp, d->dma_base + 4 * (i / 2), endian);
                v = (w >> (16 * (i & 1))) & 0xFFFF;
            }
            if (restart && v == reset_index) {
                idx[i] = 0xFFFFFFFFu;
                restarted = 1;
                continue;
            }
            v = (v + index_offset) & 0xFFFFFF; /* 24-bit vertex indices */
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
    if (!bind_targets(g, cp, (rp ? rp : p)->outputs)) { /* the eDRAM surfaces; the host viewport over them */
        free(vb);
        free(idx);
        g->st.unsupported++;
        return;
    }
    g->glUseProgram(p->prog);
    upload_constants(g, cp); /* the guest viewport is applied in the vertex shader (xe_k.vp) */
    apply_state(g, cp);
    bind_textures(g, cp, p);
    if (p->nvb && !bind_vertex_buffers(g, cp, p)) {
        free(vb);
        free(idx);
        g->st.unsupported++;
        return;
    }
    g->glBindVertexArray(g->vao);
    g->glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
    g->glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)nv * na * 16, vb, GL_STREAM_DRAW);
    for (uint32_t a = 0; a < na; a++)
        if (p->loc[a] >= 0) {
            g->glEnableVertexAttribArray((GLuint)p->loc[a]);
            g->glVertexAttribPointer((GLuint)p->loc[a], 4, GL_FLOAT, 0, (GLsizei)(na * 16),
                                     (void *)(uintptr_t)(a * 16));
        }
    if (p->memexport)
        memexport_begin(g);
    if (d->prim_type == 8) {
        draw_rects(g, cp, rp, idx, n);
    } else if (idx) {
        g->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g->ibo);
        g->glBufferData(GL_ELEMENT_ARRAY_BUFFER, 4 * (GLsizeiptr)n, idx, GL_STREAM_DRAW);
        if (restarted)
            g->glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
        g->glDrawElements((GLenum)prim, (GLsizei)n, GL_UNSIGNED_INT, NULL);
        if (restarted)
            g->glDisable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
    } else {
        g->glDrawArrays((GLenum)prim, 0, (GLsizei)n);
    }
    if (p->memexport)
        memexport_end(g, cp);
    for (uint32_t a = 0; a < na; a++)
        if (p->loc[a] >= 0)
            g->glDisableVertexAttribArray((GLuint)p->loc[a]);
    g->st.drawn++;
    free(vb);
    free(idx);
}

/* the frame texture: fw x fh guest pixels at the internal resolution, in a format a blit from ifmt can write */
static void frame_texture(xenos_gl *g, int fw, int fh, GLenum ifmt)
{
    GLenum want = is_float_fmt(ifmt) ? GL_RGBA16F : GL_RGBA8;
    static GLenum have;
    if (!g->frame_tex || g->frame_w != fw * g->scale || g->frame_h != fh * g->scale || have != want) {
        if (g->frame_tex)
            g->glDeleteTextures(1, &g->frame_tex);
        GLenum tf, tt;
        upload_fmt(want, &tf, &tt);
        g->frame_tex = new_texture(g, want, tf, tt, fw * g->scale, fh * g->scale);
        g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        g->frame_w = fw * g->scale, g->frame_h = fh * g->scale;
        have = want;
    }
}

/* At a swap, the picture: the front buffer VdSwap names (its fetch constant, in fetch constant 0) when a resolve
 * wrote it this frame -- the resolve's host picture at the internal resolution, or decoded from guest memory --,
 * else render target 0 as it stands. It lands in frame_tex (with the fetch constant's swizzle as the texture's
 * swizzle: xenos_gl_frame_texture for a window) and, with readback on, in `frame` at the guest resolution. */
void xenos_gl_swap(xenos_gl *g, const xenos_cp *cp)
{
    if (g->reset_pending)
        do_reset(g);
    static const GLint swz[6] = {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA, GL_ZERO, GL_ONE};
    const uint32_t *fc = &cp->regs[0x4800];
    uint32_t base = fc[1] & 0x1FFFF000u, ident = 0 | 1 << 3 | 2 << 6 | 3 << 9;
    int hit = 0;
    for (int i = 0; i < g->nresolved; i++)
        hit |= g->resolved[i] == base;
    int front = hit && (fc[0] & 3) == 2 && ((fc[5] >> 9) & 3) == 1;
    uint32_t swizzle = ident;
    GLuint src = 0;
    GLenum src_fmt = GL_RGBA8;
    int fw = g->width, fh = g->height, done = 0;
    rt_copy *rc = front ? copy_at(g, cp, base) : NULL;
    if (front) {
        fw = (int)(fc[2] & 0x1FFF) + 1, fh = (int)((fc[2] >> 13) & 0x1FFF) + 1;
        swizzle = (fc[3] >> 1) & 0xFFF;
    }
    if (rc && fw <= rc->w && fh <= rc->h) {
        src = rc->tex, src_fmt = rc->ifmt;
        g->st.front_buffer_frames++;
    } else if (front) {
        decoded d;
        if (decode_texture(cp, fc, 0, &d)) { /* from memory (no host picture of it) */
            g->st.front_buffer_frames++;
            uint8_t *rgba = calloc((size_t)d.w * d.h, 4);
            if (rgba) {
                for (uint32_t i = 0; i < d.w * d.h; i++)
                    for (int c = 0; c < 4; c++) {
                        uint32_t sel = (swizzle >> (3 * c)) & 7;
                        float f = d.is_float ? ((float *)d.level[0])[4 * i + (sel & 3)] : 0.0f;
                        uint8_t b = d.is_float ? (uint8_t)unorm(f, 8) : ((uint8_t *)d.level[0])[4 * i + (sel & 3)];
                        rgba[4 * i + c] = sel < 4 ? b : sel == 5 ? 255 : 0;
                    }
                if (g->readback) {
                    memset(g->frame, 0, (size_t)g->width * g->height * 4);
                    for (uint32_t y = 0; y < d.h && y < (uint32_t)g->height; y++)
                        memcpy(g->frame + (size_t)y * g->width * 4, rgba + (size_t)y * d.w * 4,
                               4 * (d.w < (uint32_t)g->width ? d.w : (uint32_t)g->width));
                }
                if (g->frame_tex)
                    g->glDeleteTextures(1, &g->frame_tex);
                g->frame_tex = new_texture(g, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, (int)d.w, (int)d.h);
                g->glTexImage2D(GL_TEXTURE_2D, 0, (GLint)GL_RGBA8, (GLsizei)d.w, (GLsizei)d.h, 0, GL_RGBA,
                                GL_UNSIGNED_BYTE, rgba);
                g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                g->frame_w = (int)d.w, g->frame_h = (int)d.h; /* frame_texture remakes it when the size differs */
                free(rgba);
                done = 1;
            }
            free_decoded(&d);
        }
        if (!done)
            fw = g->width, fh = g->height, swizzle = ident;
    }
    if (!done && !src && g->cur[0] >= 0 && g->surf[g->cur[0]].tex) { /* render target 0 as it stands */
        const surface *s = &g->surf[g->cur[0]];
        src = s->tex, src_fmt = s->ifmt;
        fw = fw < s->w ? fw : s->w, fh = fh < s->h ? fh : s->h;
        swizzle = ident;
    }
    if (src) {
        frame_texture(g, fw, fh, src_fmt);
        blit_tex(g, src, g->frame_tex, 0, 0, fw, fh, 0, 0);
        g->glBindTexture(GL_TEXTURE_2D, g->frame_tex);
        for (int c = 0; c < 4; c++) {
            uint32_t sel = (swizzle >> (3 * c)) & 7;
            g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R + (GLenum)c, sel < 6 ? swz[sel] : GL_ZERO);
        }
        if (g->readback) {
            float *px = read_pixels(g, g->frame_tex, is_float_fmt(src_fmt) ? GL_RGBA16F : GL_RGBA8, 0, 0, fw, fh);
            memset(g->frame, 0, (size_t)g->width * g->height * 4);
            for (int y = 0; px && y < fh && y < g->height; y++)
                for (int x = 0; x < fw && x < g->width; x++) {
                    const float *p = px + ((size_t)y * fw + x) * 4;
                    uint8_t *o = g->frame + ((size_t)y * g->width + x) * 4;
                    for (int c = 0; c < 4; c++) {
                        uint32_t sel = (swizzle >> (3 * c)) & 7;
                        o[c] = sel < 4 ? (uint8_t)unorm(p[sel], 8) : sel == 5 ? 255 : 0;
                    }
                }
            free(px);
        }
    } else if (!done && g->readback) {
        memset(g->frame, 0, (size_t)g->width * g->height * 4);
    }
    g->nresolved = 0;
    g->epoch++;
    if (g->readback && g->frames[0] && g->st.frames < g->max_frames) {
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
    g->glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    if (g->title_clears)
        return; /* the title clears through its resolves */
    /* until the title clears through a resolve, each frame starts clear (the targets bound) */
    g->glDisable(GL_SCISSOR_TEST);
    g->glColorMask(1, 1, 1, 1);
    g->glDepthMask(1);
    g->glStencilMaskSeparate(GL_FRONT_AND_BACK, 0xFF);
    for (int i = 0; i < 4; i++)
        if (g->cur[i] >= 0) {
            static const GLfloat black[4] = {0, 0, 0, 1};
            g->glClearBufferfv(0x1800 /* GL_COLOR */, i, black);
        }
    if (g->cur_ds >= 0) {
        g->glClearDepthf(1.0f);
        g->glClearStencil(0);
        g->glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }
}

void xenos_gl_end_frame(xenos_gl *g)
{
    /* what a window's own drawing expects back (host_window_present_texture): framebuffer 0, no test, no blend */
    g->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    g->glDisable(GL_SCISSOR_TEST);
    g->glDisable(GL_BLEND);
    g->glDisable(GL_DEPTH_TEST);
    g->glDisable(GL_STENCIL_TEST);
    g->glDisable(GL_CULL_FACE);
    g->glColorMask(1, 1, 1, 1);
    g->glFrontFace(GL_CCW);
    g->glActiveTexture(GL_TEXTURE0);
    g->glBindVertexArray(0);
    g->glUseProgram(0);
}
