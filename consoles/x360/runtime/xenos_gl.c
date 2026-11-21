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
#define GL_POLYGON_OFFSET_FILL 0x8037

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
    X(void, glPolygonOffset, (GLfloat, GLfloat))                                                                       \
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
    X(void, glRenderbufferStorageMultisample, (GLenum, GLsizei, GLenum, GLsizei, GLsizei))                            \
    X(void, glGetInternalformativ, (GLenum, GLenum, GLenum, GLsizei, GLint *))                                        \
    X(void, glUniform3i, (GLint, GLint, GLint, GLint))

#define MAX_ATTRS 16
#define MAX_SAMPLERS 16
#define MAX_TEXTURES 128
#define MAX_REASONS 32

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
    struct program *next; /* the next program in its hash bucket */
} program;

/* a texture as last uploaded: its fetch constant's words (address, format, size, layout) and the source's hash */
typedef struct texture {
    uint32_t key[6];
    uint64_t hash;
    GLuint tex;
    int used;
    uint32_t checked; /* the epoch its memory was last compared in */
    int copy;         /* 1 + the resolve copy it was made from (its host picture, at the internal resolution) */
    int swap_rb;      /* that copy's: red and blue are the other way round from the memory the fetch constant reads */
    int levels;       /* mip levels uploaded */
} texture;

/* ---- the eDRAM model ----
 * The 10 MB of eDRAM hold the render targets at tile bases (80 x 16 samples of 32 bits per tile) that RB_COLOR_INFO,
 * RB_COLOR1..3_INFO and RB_DEPTH_INFO name, with RB_SURFACE_INFO's pitch and MSAA mode. Each (base, pitch, format,
 * MSAA) a draw or resolve meets is a host texture of guest width x height at the internal resolution (scale): its
 * height grows to what the window scissor asks for. Surfaces are kept least recently used first; two keys over the
 * same tiles are two surfaces, and the one bound or resolved takes what the other wrote since (sync_surface): the raw
 * bits at the same pitch for formats of one bit width, else the rows are cleared and counted. An MSAA target (2x or 4x
 * in RB_SURFACE_INFO) is drawn into a multisampled renderbuffer at the internal resolution when every attachment of the
 * draw has one of the same sample count (the driver's list for each format, RECOMP_X360_MSAA=0 turns it off); `tex`
 * stays the resolved picture every read uses, brought up to date by surface_flush. Otherwise it is drawn single-sampled
 * at the pixel grid, as a resolve of any sample gives the pixel. */
#define MAX_SURFACES 32
#define MAX_COPIES 16

typedef struct surface {
    uint32_t base, pitch, fmt, depth, msaa; /* the key: tile base, guest pitch, colour (0-15) or depth (0-1) format */
    int w, h;                              /* guest pixels */
    GLuint tex;
    GLenum ifmt, rfmt, rtype;              /* host internal format, format / type of glReadPixels */
    int kind;                              /* 0 unorm, 1 float, 2 depth */
    unsigned used;
    unsigned written;                      /* the write clock of its last contents (XBA-296) */
    int dirty_h;                           /* guest rows written so far */
    int unbounded;                         /* the last draw's extent is unknown (no scissor, no viewport): dirty_h is
                                            * only the surface's height, not grounds for overwriting a neighbour */
    GLuint rb;                            /* the multisampled colour or depth / stencil buffer, 0: none */
    int samples;
    int ms_dirty;                          /* rb has what tex does not yet */
    int ms_reload;                         /* tex has what rb does not yet */
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
    int swap_rb;           /* the resolve exchanged red and blue in memory; the picture has them as the surface did */
    uint32_t fmt, endian;  /* the destination's texture format and endian mode: how memory was written */
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
    GLuint sprog, stex;       /* the stencil of depth resolves: the pass marking one bit, its target */
    int stex_w, stex_h, sprog_failed;
    int rtex_w, rtex_h;
    GLenum rtex_fmt;
    void *scratch[3];         /* reused read-back buffers (scratch_buf): grown, never shrunk */
    size_t scratch_cap[3];
    int width, height;        /* the default target (a surface with pitch 0 / no scissor) and the frame size */
    int scale;                /* internal resolution: every surface is scale x its guest size */
    int max_tex;
    int no_float;             /* float targets are not renderable here (no EXT_color_buffer_*): 8_8_8_8 instead */
    int readback;             /* read each swap's frame back to memory (xenos_gl_frame, PPM frames) */
    surface surf[MAX_SURFACES];
    unsigned surf_clock, write_clock;
    GLuint aprog;             /* eDRAM aliasing: the pass that reinterprets a surface's bits in another format */
    GLint aprog_off, aprog_sc, aprog_dc;
    int aprog_failed;
    GLuint zprog;             /* MSAA: the pass that writes a resolved depth back into a multisampled buffer */
    int zprog_failed;
    int msaa_on;              /* multisampled render targets are allowed (RECOMP_X360_MSAA=0: not) */
    int cur_ms;               /* the draw FBO has the renderbuffers attached rather than the textures */
    int cur[4], cur_ds;       /* the surfaces bound (index into surf, -1 none) */
    int tw, th;               /* the guest size of the bound colour target 0 (the viewport's frame of reference) */
    rt_copy copies[MAX_COPIES];
    unsigned copy_clock;
    GLuint frame_tex;         /* the last swap's picture: a texture of frame_w x frame_h host pixels, top row first */
    int frame_w, frame_h;
    GLenum frame_fmt;         /* frame_tex's internal format */
    uint8_t *frame;
    char cache[512], frames[512];
    unsigned max_frames;
    program **progs;          /* every (vs, ps, kind) met, failed ones too: each allocated once, never moved */
    int nprogs, cap_progs;
    program **prog_hash;      /* buckets of progs by (vs crc, ps crc, kind), prog_buckets of them (a power of two) */
    unsigned prog_buckets;
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
    int memexport_ps;         /* XBA-296: in pixel shaders too (fragment storage blocks) */
    void (*glMemoryBarrier)(GLbitfield);
    /* per-target blend and colour mask (GLES 3.2 or OES / EXT_draw_buffers_indexed); NULL: render target 0's state for
     * every target, as GLES 3.0 has it */
    void (*glEnablei)(GLenum, GLuint);
    void (*glDisablei)(GLenum, GLuint);
    void (*glBlendFuncSeparatei)(GLuint, GLenum, GLenum, GLenum, GLenum);
    void (*glBlendEquationSeparatei)(GLuint, GLenum, GLenum);
    void (*glColorMaski)(GLuint, GLboolean, GLboolean, GLboolean, GLboolean);
    int float_blend;          /* EXT_float_blend: 32-bit float targets blend (else a draw blending one is an error) */
    uint64_t float_blend_off; /* draws whose blending was turned off on a 32-bit float target for want of it */
    GLuint mxbuf;             /* their log: count, capacity, then (byte address, dword) pairs */
    int mx_warned;
    FILE *log;
    const char *why[MAX_REASONS]; /* the distinct reasons behind st.unsupported, each logged once */
    uint64_t why_n[MAX_REASONS];
    int nwhy;
};

static unsigned parse_outputs(const char *src);

/* a draw or resolve that cannot be handled: counted, with its reason (a string literal) */
static void unsupported(xenos_gl *g, const char *reason)
{
    g->st.unsupported++;
    for (int i = 0; i < g->nwhy; i++)
        if (!strcmp(g->why[i], reason)) {
            g->why_n[i]++;
            return;
        }
    fprintf(g->log, "xenos_gl: unsupported: %s\n", reason);
    if (g->nwhy < MAX_REASONS) {
        g->why[g->nwhy] = reason;
        g->why_n[g->nwhy++] = 1;
    }
}

const char *xenos_gl_unsupported_reason(const xenos_gl *g, int i, uint64_t *count)
{
    if (i < 0 || i >= g->nwhy)
        return NULL;
    if (count)
        *count = g->why_n[i];
    return g->why[i];
}

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
    GLint blocks = 0, fblocks = 0;
    g->memexport = g->memexport_ps = 0;
    if (g->gl_major > 3 || (g->gl_major == 3 && g->gl_minor >= 1)) {
        g->glGetIntegerv(0x90D6 /* GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS */, &blocks);
        g->glGetIntegerv(0x90DA /* GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS */, &fblocks);
        *(void **)&g->glMemoryBarrier = g->eglGetProcAddress ? g->eglGetProcAddress("glMemoryBarrier") : NULL;
        if (!g->glMemoryBarrier && g->gles_lib)
            *(void **)&g->glMemoryBarrier = dlsym(g->gles_lib, "glMemoryBarrier");
        int allowed = g->glMemoryBarrier && !(env && !strcmp(env, "0"));
        g->memexport = blocks > 0 && allowed;
        g->memexport_ps = fblocks > 0 && allowed;
    }
    fprintf(g->log, "xenos: memory export %s (GLES %d.%d, %d vertex, %d fragment storage block(s))\n",
            g->memexport || g->memexport_ps ? "on" : env && !strcmp(env, "0") ? "off (RECOMP_X360_MEMEXPORT=0)" : "off",
            g->gl_major, g->gl_minor, (int)blocks, (int)fblocks);
}

/* an optional entry point: eglGetProcAddress, then libGLESv2 (NULL when neither has it) */
static void *gl_proc(xenos_gl *g, const char *name)
{
    void *f = g->eglGetProcAddress ? g->eglGetProcAddress(name) : NULL;
    if (!f && g->gles_lib)
        f = dlsym(g->gles_lib, name);
    return f;
}

/* per-target blend state and colour masks: GLES 3.2's glEnablei & co., else OES / EXT_draw_buffers_indexed's */
static void draw_buffers_caps(xenos_gl *g)
{
    const char *sfx = g->gl_major > 3 || (g->gl_major == 3 && g->gl_minor >= 2) ? ""
                      : has_ext(g, "GL_OES_draw_buffers_indexed")                 ? "OES"
                      : has_ext(g, "GL_EXT_draw_buffers_indexed")                 ? "EXT"
                                                                                  : NULL;
    if (sfx) {
        char n[64];
#define ISYM(name)                                                                                                     \
    snprintf(n, sizeof n, "%s%s", #name, sfx);                                                                         \
    *(void **)&g->name = gl_proc(g, n)
        ISYM(glEnablei);
        ISYM(glDisablei);
        ISYM(glBlendFuncSeparatei);
        ISYM(glBlendEquationSeparatei);
        ISYM(glColorMaski);
#undef ISYM
    }
    if (!g->glEnablei || !g->glDisablei || !g->glBlendFuncSeparatei || !g->glBlendEquationSeparatei ||
        !g->glColorMaski)
        g->glEnablei = NULL, g->glDisablei = NULL, g->glBlendFuncSeparatei = NULL, g->glBlendEquationSeparatei = NULL,
        g->glColorMaski = NULL;
    fprintf(g->log, "xenos: per-target blend and colour mask %s\n",
            g->glEnablei ? "on" : "off (render target 0's state for all)");
    /* RECOMP_X360_FLOAT_BLEND=0 behaves as a driver without the extension (tests) */
    const char *fb = getenv("RECOMP_X360_FLOAT_BLEND");
    g->float_blend = has_ext(g, "GL_EXT_float_blend") && !(fb && !strcmp(fb, "0"));
}

static int setup(xenos_gl *g, char *why, size_t why_n)
{
    g->max_tex = 4096;
    g->glGetIntegerv(GL_MAX_TEXTURE_SIZE, &g->max_tex);
    g->glGetIntegerv(0x821B /* GL_MAJOR_VERSION */, &g->gl_major);
    g->glGetIntegerv(0x821C /* GL_MINOR_VERSION */, &g->gl_minor);
    texture_options(g);
    memexport_caps(g);
    draw_buffers_caps(g);
    const char *msaa_env = getenv("RECOMP_X360_MSAA");
    g->msaa_on = !(msaa_env && !strcmp(msaa_env, "0"));
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
            if (g->progs[i]->prog)
                g->glDeleteProgram(g->progs[i]->prog);
        g->glDeleteBuffers(1, &g->vbo);
        g->glDeleteBuffers(1, &g->ibo);
        g->glDeleteBuffers(2, g->ubo);
        g->glDeleteBuffers(1, &g->tfbo);
        if (g->mxbuf)
            g->glDeleteBuffers(1, &g->mxbuf);
        for (int i = 0; i < MAX_TEXTURES; i++)
            if (g->textures[i].tex)
                g->glDeleteTextures(1, &g->textures[i].tex);
        for (int i = 0; i < MAX_SURFACES; i++) {
            if (g->surf[i].tex)
                g->glDeleteTextures(1, &g->surf[i].tex);
            if (g->surf[i].rb)
                g->glDeleteRenderbuffers(1, &g->surf[i].rb);
        }
        if (g->zprog)
            g->glDeleteProgram(g->zprog);
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
        if (g->stex)
            g->glDeleteTextures(1, &g->stex);
        if (g->sprog)
            g->glDeleteProgram(g->sprog);
        if (g->aprog)
            g->glDeleteProgram(g->aprog);
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
    for (int i = 0; i < g->nprogs; i++)
        free(g->progs[i]);
    free(g->progs);
    free(g->prog_hash);
    free(g->frame);
    for (int i = 0; i < 3; i++)
        free(g->scratch[i]);
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
            if (g->surf[i].rb)
                g->glDeleteRenderbuffers(1, &g->surf[i].rb);
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
#define XENOS_GLSL_STAMP "// xenos_glsl 4\n"

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

static unsigned prog_bucket(uint32_t vc, uint32_t pc, int kind, unsigned buckets)
{
    uint32_t h = vc * 0x9E3779B1u ^ (pc + 0x7F4A7C15u) * 0x85EBCA77u ^ (uint32_t)kind * 0xC2B2AE3Du;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 13;
    return h & (buckets - 1);
}

/* a new, zeroed program for (vc, pc, kind) in progs and the hash (no limit: the table and the buckets grow, a program
 * itself never moves, as a draw holds two at once); NULL only when memory runs out */
static program *add_program(xenos_gl *g, uint32_t vc, uint32_t pc, int kind)
{
    if (g->nprogs == g->cap_progs) {
        int cap = g->cap_progs ? g->cap_progs * 2 : 256;
        program **a = realloc(g->progs, (size_t)cap * sizeof *a);
        if (!a)
            return NULL;
        g->progs = a, g->cap_progs = cap;
    }
    if ((unsigned)g->nprogs + 1 > g->prog_buckets / 2) { /* load factor 1/2 */
        unsigned nb = g->prog_buckets ? g->prog_buckets * 2 : 512;
        program **h = calloc(nb, sizeof *h);
        if (!h)
            return NULL;
        for (int i = 0; i < g->nprogs; i++) {
            program *q = g->progs[i];
            unsigned b = prog_bucket(q->vs_crc, q->ps_crc, q->kind, nb);
            q->next = h[b], h[b] = q;
        }
        free(g->prog_hash);
        g->prog_hash = h, g->prog_buckets = nb;
    }
    program *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    p->vs_crc = vc, p->ps_crc = pc, p->kind = kind;
    unsigned b = prog_bucket(vc, pc, kind, g->prog_buckets);
    p->next = g->prog_hash[b], g->prog_hash[b] = p;
    g->progs[g->nprogs++] = p;
    return p;
}

/* kind 0: the title's two shaders; 1: its vertex shader with transform feedback; 2: the pass-through vertex shader
 * with its pixel shader (rectangle lists) */
static program *get_program(xenos_gl *g, const xenos_cp *cp, int kind)
{
    const xenos_shader *vs = &cp->shader[0], *ps = &cp->shader[1];
    if (!vs->words || !ps->words)
        return NULL;
    uint32_t vc = kind == 2 ? 0 : crc32_words(vs->code, vs->words);
    uint32_t pc = kind == 1 ? 0 : crc32_words(ps->code, ps->words);
    if (g->prog_buckets)
        for (program *q = g->prog_hash[prog_bucket(vc, pc, kind, g->prog_buckets)]; q; q = q->next)
            if (q->vs_crc == vc && q->ps_crc == pc && q->kind == kind)
                return q->ok ? q : NULL;
    program *p = add_program(g, vc, pc, kind);
    if (!p)
        return NULL;
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
    /* XBA-282, XBA-296: a shader that exports to memory is GLSL ES 3.10: it needs the capability of its stage, and the
     * other stage is linked as 3.10 too (GLES links one version only; 3.00 source is 3.10 source) */
    int vs310 = vsrc && !strncmp(vsrc, "#version 310 es", 15), ps310 = psrc && !strncmp(psrc, "#version 310 es", 15);
    char rect_vs[sizeof RECT_VS], capture_fs[sizeof CAPTURE_FS];
    memcpy(rect_vs, RECT_VS, sizeof RECT_VS);
    memcpy(capture_fs, CAPTURE_FS, sizeof CAPTURE_FS);
    if (vs310 || ps310) {
        if ((vs310 && !g->memexport) || (ps310 && !g->memexport_ps)) {
            if (!g->mx_warned++)
                fprintf(g->log, "xenos_gl: %s_%08X exports to memory: its draws are skipped (no GLES 3.1 storage "
                                "blocks in that stage here)\n", vs310 ? "vs" : "ps", vs310 ? vc : pc);
            g->st.memexport_skipped++;
            free(vsrc);
            free(psrc);
            return NULL;
        }
        p->memexport = 1;
        if (vsrc)
            vsrc[10] = '1';
        if (psrc)
            psrc[10] = '1';
        rect_vs[10] = capture_fs[10] = '1';
    }
    const char *vtext = kind == 2 ? rect_vs : vsrc, *ftext = kind == 1 ? capture_fs : psrc;
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
                g->st.programs++;
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
    if (cp->mem) { /* the guest's big-endian word, byte-swapped: its bytes as a little-endian load reads them */
        uint32_t le;
        memcpy(&le, cp->mem + (phys & 0x1FFFFFFFu), 4);
        return gpu_swap(le, endian);
    }
    uint32_t be = cp->read32 ? cp->read32(cp->ctx, phys & 0x1FFFFFFFu) : 0;
    return gpu_swap(__builtin_bswap32(be), endian);
}

/* A fast hash of n bytes (4 lanes of 8 bytes: ~10x FNV's byte at a time), for "has this memory changed" checks run
 * on every cached texture after each swap and resolve. Not for anything that needs a good distribution. */
static uint64_t span_hash(const uint8_t *p, size_t n, uint64_t seed)
{
    const uint64_t k = 0x9E3779B97F4A7C15ull;
    uint64_t h[4] = {seed ^ k, seed + k, seed ^ (k >> 1), seed - k};
    size_t i = 0;
    for (; i + 32 <= n; i += 32)
        for (int l = 0; l < 4; l++) {
            uint64_t w;
            memcpy(&w, p + i + 8 * l, 8);
            h[l] = (h[l] ^ w) * 0xFF51AFD7ED558CCDull;
            h[l] ^= h[l] >> 29;
        }
    uint64_t r = (uint64_t)n * k;
    for (; i < n; i++)
        r = (r ^ p[i]) * 0x100000001B3ull;
    for (int l = 0; l < 4; l++)
        r = (r ^ h[l]) * 0xC4CEB9FE1A85EC53ull, r ^= r >> 31;
    return r;
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
    case 12: return 1; /* line loop: drawn as a line list (convert_indices) */
    case 14: return 4; /* quad strip: two triangles per quad */
    case 15: return 4; /* polygon: a fan from its first vertex, as triangles */
    default: return -1;
    }
}

/* the primitive types VGT_MULTI_PRIM_IB_RESET_INDX cuts (Xenia's primitive_processor.cc): not the lists */
static int prim_resets(uint32_t t)
{
    return t == 3 || t == 5 || t == 6 || t == 12 || t == 14 || t == 15;
}

#define INDEX_RESTART 0xFFFFFFFFu

/* Line loops, quad lists, quad strips and polygons as what GLES draws, each run between reset indices on its own. The
 * first vertex of each output primitive is the first of the guest's (Direct3D's provoking vertex); the interpolators
 * are not flat, so it only shows in the order. Returns the list and its length (0 entries: nothing to draw), NULL when
 * out of memory. */
static uint32_t *convert_indices(uint32_t type, const uint32_t *in, uint32_t n, uint32_t *out_n)
{
    uint32_t *o = malloc(((size_t)n + 1) * 6 * sizeof *o), m = 0;
    if (!o)
        return NULL;
    for (uint32_t s = 0; s < n;) {
        uint32_t e = s;
        while (e < n && in[e] != INDEX_RESTART)
            e++;
        const uint32_t *r = in + s;
        uint32_t len = e - s;
        if (type == 12 && len >= 2) {
            for (uint32_t k = 0; k < len; k++)
                o[m++] = r[k], o[m++] = r[(k + 1) % len];
        } else if (type == 13) { /* (a, b, c, d) -> (a, b, c), (a, c, d) */
            for (uint32_t k = 0; k + 4 <= len; k += 4) {
                o[m++] = r[k], o[m++] = r[k + 1], o[m++] = r[k + 2];
                o[m++] = r[k], o[m++] = r[k + 2], o[m++] = r[k + 3];
            }
        } else if (type == 14) { /* (a, b, c, d) goes round as a, b, d, c -> (a, b, d), (a, d, c) */
            for (uint32_t k = 0; k + 4 <= len; k += 2) {
                o[m++] = r[k], o[m++] = r[k + 1], o[m++] = r[k + 3];
                o[m++] = r[k], o[m++] = r[k + 3], o[m++] = r[k + 2];
            }
        } else if (type == 15) {
            for (uint32_t k = 1; k + 1 < len; k++)
                o[m++] = r[0], o[m++] = r[k], o[m++] = r[k + 1];
        }
        s = e + 1;
    }
    *out_n = m;
    return o;
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

/* the 64-bit colour formats: 16_16_16_16, its _FLOAT form, 32_32_FLOAT */
static int fmt_wide(uint32_t fmt)
{
    return fmt == 5 || fmt == 7 || fmt == 15;
}

static void sync_surface(xenos_gl *g, int si, const int *bound, int nbound);
static void surface_reload(xenos_gl *g, surface *s);

/* the multisampled buffer of an MSAA surface: the smallest sample count the driver lists for its format that is at
 * least the guest's; none (single-sampled) when it lists no such count */
static void surface_ms_alloc(xenos_gl *g, surface *s)
{
    s->rb = 0, s->samples = 0, s->ms_dirty = 0, s->ms_reload = 0;
    if (!s->msaa || !g->msaa_on)
        return;
    GLint n = 0, list[16];
    int want = s->msaa == 1 ? 2 : 4, pick = 0;
    g->glGetInternalformativ(GL_RENDERBUFFER, s->ifmt, 0x9380 /* GL_NUM_SAMPLE_COUNTS */, 1, &n);
    n = n > 16 ? 16 : n;
    if (n > 0)
        g->glGetInternalformativ(GL_RENDERBUFFER, s->ifmt, 0x80A9 /* GL_SAMPLES */, n, list);
    for (GLint i = 0; i < n; i++)
        if (list[i] >= want && (!pick || list[i] < pick))
            pick = list[i];
    if (!pick)
        return;
    g->glGenRenderbuffers(1, &s->rb);
    g->glBindRenderbuffer(GL_RENDERBUFFER, s->rb);
    g->glRenderbufferStorageMultisample(GL_RENDERBUFFER, pick, s->ifmt, s->w * g->scale, s->h * g->scale);
    s->samples = pick;
    s->ms_reload = 1; /* it starts as the texture does */
    g->st.msaa_surfaces++;
}

/* the texture brought up to date with the multisampled buffer: the samples resolved (the whole surface: GLES 3.0
 * resolves identical rectangles only) */
static void surface_flush(xenos_gl *g, surface *s)
{
    if (!s->rb || !s->ms_dirty)
        return;
    GLenum att = s->kind == 2 ? GL_DEPTH_STENCIL_ATTACHMENT : GL_COLOR_ATTACHMENT0;
    int hw = s->w * g->scale, hh = s->h * g->scale;
    g->glBindFramebuffer(GL_READ_FRAMEBUFFER, g->rfbo);
    g->glFramebufferRenderbuffer(GL_READ_FRAMEBUFFER, att, GL_RENDERBUFFER, s->rb);
    g->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g->tfbo_draw);
    g->glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, att, GL_TEXTURE_2D, s->tex, 0);
    if (s->kind != 2) {
        static const GLenum b0 = GL_COLOR_ATTACHMENT0;
        g->glReadBuffer(GL_COLOR_ATTACHMENT0);
        g->glDrawBuffers(1, &b0);
    }
    g->glDisable(GL_SCISSOR_TEST);
    g->glBlitFramebuffer(0, 0, hw, hh, 0, 0, hw, hh,
                         s->kind == 2 ? GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT : GL_COLOR_BUFFER_BIT, GL_NEAREST);
    g->glFramebufferRenderbuffer(GL_READ_FRAMEBUFFER, att, GL_RENDERBUFFER, 0);
    g->glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, att, GL_TEXTURE_2D, 0, 0);
    g->glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    s->ms_dirty = 0;
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
        int wide = !depth && fmt_wide(fmt);
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
            surface_flush(g, s);
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
            if (s->rb) { /* a blit cannot write a multisampled buffer: a new one, filled from the texture */
                g->glDeleteRenderbuffers(1, &s->rb);
                g->st.msaa_surfaces--;
                surface_ms_alloc(g, s);
            }
            /* every attachment of the draw FBO was taken off above, not only this surface's: all are attached again
             * by the next bind (a depth buffer left believed attached would be missing from then on) */
            for (int k = 0; k < 4; k++)
                g->cur[k] = -2;
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
    if (s->rb)
        g->glDeleteRenderbuffers(1, &s->rb);
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
    surface_ms_alloc(g, s);
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
        /* a draw that neither tests nor writes depth or stencil (RB_DEPTHCONTROL's stencil, z and z write enables off)
         * takes no depth attachment when its depth surface is shorter than colour target 0: GL draws only where every
         * attachment reaches, and a 1080p title can leave RB_DEPTH_INFO where the eDRAM holds fewer rows than the
         * colour target (Adventure Time: depth base tile 0x3FC at pitch 1920, 672 of 1080 rows) */
        if (ds >= 0 && want[0] >= 0 && !(cp->regs[0x2200] & 7) && g->surf[ds].h < g->surf[want[0]].h)
            ds = -1;
        /* what another format or position left in these tiles since (not what is bound together with them) */
        int together[5] = {want[0], want[1], want[2], want[3], ds};
        for (int i = 0; i < 5; i++)
            if (together[i] >= 0)
                sync_surface(g, together[i], together, 5);
        /* multisampled only when every attachment is, with one sample count */
        int use_ms = 1, samples = 0;
        for (int i = 0; i < 5; i++)
            if (together[i] >= 0) {
                const surface *w = &g->surf[together[i]];
                use_ms &= w->rb != 0 && (!samples || samples == w->samples);
                samples = w->samples;
            }
        for (int i = 0; i < 5; i++)
            if (together[i] >= 0 && g->surf[together[i]].rb) {
                surface *w = &g->surf[together[i]];
                if (use_ms) {
                    if (w->ms_reload)
                        surface_reload(g, w);
                    w->ms_dirty = 1;
                } else { /* this draw goes to the texture: the buffer takes it up again when next used */
                    surface_flush(g, w);
                    w->ms_reload = 1;
                }
            }
        g->glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
        int reattach = use_ms != g->cur_ms;
        g->cur_ms = use_ms;
        GLenum bufs[4];
        for (int i = 0; i < 4; i++) {
            if (g->cur[i] != want[i] || reattach) {
                GLenum att = GL_COLOR_ATTACHMENT0 + (GLenum)i;
                if (want[i] >= 0 && use_ms)
                    g->glFramebufferRenderbuffer(GL_FRAMEBUFFER, att, GL_RENDERBUFFER, g->surf[want[i]].rb);
                else
                    g->glFramebufferTexture2D(GL_FRAMEBUFFER, att, GL_TEXTURE_2D, want[i] >= 0 ? g->surf[want[i]].tex : 0,
                                              0);
            }
            g->cur[i] = want[i];
            bufs[i] = want[i] >= 0 ? GL_COLOR_ATTACHMENT0 + (GLenum)i : 0 /* GL_NONE */;
        }
        if (g->cur_ds != ds || reattach) {
            if (ds < 0)
                g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
            else if (use_ms)
                g->glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g->surf[ds].rb);
            else
                g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, g->surf[ds].tex, 0);
        }
        g->cur_ds = ds;
        g->glDrawBuffers(4, bufs);
        if (g->glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
            const surface *ref = want[0] >= 0 ? &g->surf[want[0]] : &g->surf[ds];
            unsigned stamp = ++g->write_clock;
            /* how far down this draw can reach: the scissor unless it is left at its maximum, and the viewport */
            int reach = need_h > 0 && need_h < 8191 ? need_h : 0x7FFFFFFF, bounded = reach != 0x7FFFFFFF;
            if ((cp->regs[0x2206] & 12) == 12) { /* VTE: y scale and offset on */
                float vy = fabsf(as_float(cp->regs[0x2111])) + as_float(cp->regs[0x2112]);
                if (vy > 0.0f && vy < 16384.0f) {
                    int rows = (int)ceilf(vy);
                    reach = rows < reach ? rows : reach;
                    bounded = 1;
                }
            }
            for (int i = 0; i < 5; i++)
                if (together[i] >= 0) {
                    surface *w = &g->surf[together[i]];
                    int rows = reach < w->h ? reach : w->h;
                    w->written = stamp;
                    w->unbounded = !bounded;
                    w->dirty_h = rows > w->dirty_h ? rows : w->dirty_h;
                }
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
                if (g->surf[i].rb)
                    g->glDeleteRenderbuffers(1, &g->surf[i].rb);
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

/* the guest's depth bias (Xenia's draw_util.cc GetPreferredFacePolygonOffset): GL has one pair for both faces, so
 * the front pair when its offset is on and front faces are not culled, else the back pair; non-polygonal primitives
 * (rectangle lists here: points and lines take no offset in GLES) use the front pair when poly_offset_para is on. The
 * guest's constant is absolute depth, GL's is in units of the depth buffer's resolution (unorm24: x (2^24 - 1)); the
 * guest's slope scale is per subpixel (x 1/16, Xenia's kPolygonOffsetScaleSubpixelUnit), x the internal resolution
 * as a host pixel is that much smaller. */
static void apply_polygon_offset(xenos_gl *g, const xenos_cp *cp, uint32_t prim_type)
{
    uint32_t sc = cp->regs[0x2205];
    float scale = 0.0f, offset = 0.0f;
    int polygonal = (prim_type >= 4 && prim_type <= 7) || (prim_type >= 13 && prim_type <= 15);
    if (polygonal) {
        if ((sc >> 11) & 1 && !(sc & 1))
            scale = as_float(cp->regs[0x2380]), offset = as_float(cp->regs[0x2381]);
        if ((sc >> 12) & 1 && !(sc & 2) && scale == 0.0f && offset == 0.0f)
            scale = as_float(cp->regs[0x2382]), offset = as_float(cp->regs[0x2383]);
    } else if ((sc >> 13) & 1) {
        scale = as_float(cp->regs[0x2380]), offset = as_float(cp->regs[0x2381]);
    }
    if (!(fabsf(scale) < 1e30f)) /* NaN / infinite: none */
        scale = 0.0f;
    if (!(fabsf(offset) < 1e30f))
        offset = 0.0f;
    if (scale != 0.0f || offset != 0.0f) {
        g->glEnable(GL_POLYGON_OFFSET_FILL);
        g->glPolygonOffset(scale * (1.0f / 16.0f) * (float)g->scale, offset * 16777215.0f);
    } else {
        g->glDisable(GL_POLYGON_OFFSET_FILL);
    }
}

/* render target i (-1: any bound) is 32-bit float (32_FLOAT, 32_32_FLOAT as host_format makes them) */
static int float32_target(const xenos_gl *g, int i)
{
    for (int k = i < 0 ? 0 : i; k < (i < 0 ? 4 : i + 1); k++) {
        int s = g->cur[k];
        if (s >= 0 && (g->surf[s].ifmt == GL_R32F || g->surf[s].ifmt == GL_RG32F))
            return 1;
    }
    return 0;
}

static void apply_state(xenos_gl *g, const xenos_cp *cp, uint32_t prim_type)
{
    /* blending, render target n from RB_BLENDCONTROLn (off when it is src * 1 + dst * 0 for colour and alpha), and
     * RB_COLOR_MASK's nibble n; without per-target state (GLES 3.0) target 0's for all */
    static const uint32_t blend_reg[4] = {0x2201, 0x2209, 0x220A, 0x220B};
    int indexed = g->glEnablei != NULL;
    uint32_t cm = cp->regs[0x2104];
    g->glBlendColor(as_float(cp->regs[0x2105]), as_float(cp->regs[0x2106]), as_float(cp->regs[0x2107]),
                    as_float(cp->regs[0x2108]));
    for (GLuint i = 0; i < (indexed ? 4u : 1u); i++) {
        uint32_t bc = cp->regs[blend_reg[i]], m = cm >> (4 * i);
        GLenum sc = blend_factor(bc & 31), dc = blend_factor((bc >> 8) & 31);
        GLenum sa = blend_factor((bc >> 16) & 31), da = blend_factor((bc >> 24) & 31);
        GLenum oc = blend_op((bc >> 5) & 7), oa = blend_op((bc >> 21) & 7);
        int on = (bc & 0x1FFF1FFFu) != 0x00010001u;
        /* blending a 32-bit float target needs EXT_float_blend (GL_INVALID_OPERATION, the draw lost, without it):
         * that target unblended; without per-target state, every target */
        if (on && !g->float_blend && float32_target(g, indexed ? (int)i : -1)) {
            on = 0;
            if (!g->float_blend_off++)
                fprintf(g->log, "xenos_gl: no EXT_float_blend: blending off on 32-bit float render targets\n");
        }
        if (indexed) {
            if (on) {
                g->glEnablei(GL_BLEND, i);
                g->glBlendFuncSeparatei(i, sc, dc, sa, da);
                g->glBlendEquationSeparatei(i, oc, oa);
            } else {
                g->glDisablei(GL_BLEND, i);
            }
            g->glColorMaski(i, m & 1, (m >> 1) & 1, (m >> 2) & 1, (m >> 3) & 1);
        } else {
            if (on) {
                g->glEnable(GL_BLEND);
                g->glBlendFuncSeparate(sc, dc, sa, da);
                g->glBlendEquationSeparate(oc, oa);
            } else {
                g->glDisable(GL_BLEND);
            }
            g->glColorMask(m & 1, (m >> 1) & 1, (m >> 2) & 1, (m >> 3) & 1);
        }
    }
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
    apply_polygon_offset(g, cp, prim_type);
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
 * constant's swizzle goes to GL's texture swizzle. Mip levels from the mip address: each stored in max(1, pow2(size) >>
 * level) with its own 32-block-aligned pitch and height, levels 4 KB aligned one after the other; with packed_mips set
 * the levels whose smaller side is 16 texels or less share one tile (packed_offset). 1D, 2D, 3D and cube. */

typedef struct texfmt {
    uint32_t format, block, bytes; /* block size in texels (1 or 4), bytes per block */
    int is_float;                  /* decoded to floats (RGBA16F) rather than RGBA8 */
} texfmt;

static const texfmt *tex_format(uint32_t f)
{
    static const texfmt fmts[] = {
        {2, 1, 1, 0},   {8, 1, 1, 0},   {9, 1, 1, 0},   {10, 1, 2, 0},  {6, 1, 4, 0},   {14, 1, 4, 0},  {50, 1, 4, 0},  {62, 1, 4, 0},
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

/* a DXT colour endpoint: S3TC's 5:6:5 with red in the 5 high bits (the uncompressed 5_6_5 format is the other way
 * round: X in the low bits). The GPU decodes the standard block: Xenia hands it to the host's BC decoder as it is. */
static void unpack_dxt_colour(uint32_t v, uint8_t out[4])
{
    out[0] = (uint8_t)((((v >> 11) & 31) * 255 + 15) / 31);
    out[1] = (uint8_t)((((v >> 5) & 63) * 255 + 31) / 63);
    out[2] = (uint8_t)(((v & 31) * 255 + 15) / 31);
    out[3] = 255;
}

/* one 4x4 block of a compressed format at `b` (bytes after the endian swap) to 16 RGBA texels */
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
    unpack_dxt_colour(c0, pal[0]);
    unpack_dxt_colour(c1, pal[1]);
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

/* `s` holds one texel of tex_format(format)->bytes bytes and no more: the last texel of an 8 or 16 bpp level ends its
 * buffer, which can end its page (a 1024 x 768 k_8 one in Warhammer - Battle March, XBA-318) */
static void texel(uint32_t format, const uint8_t *s, uint8_t out[4])
{
    unsigned bytes = format == 2 || format == 8 || format == 9 ? 1 : (format == 10 || format == 3 || format == 4 || format == 5 || format == 15) ? 2 : 4;
    uint32_t v = s[0] | (bytes > 1 ? (uint32_t)s[1] << 8 : 0),
             d = v | (bytes > 2 ? (uint32_t)s[2] << 16 | (uint32_t)s[3] << 24 : 0);
    switch (format) {
    case 2: /* 8: replicated, the swizzle picks */
    case 8: /* 8_A, 8_B: one byte as well (Xenia's host format for both is R8_UNORM) */
    case 9: out[0] = out[1] = out[2] = out[3] = s[0]; break;
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
    int levels;                 /* mip levels of the texture, up to the fetch constant's last (1: the base only) */
    int base_level;             /* the first one decoded: the fetch constant's first level */
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

static uint32_t log2_ceil(uint32_t v)
{
    uint32_t n = 0;
    while ((1u << n) < v)
        n++;
    return n;
}

/* Packed mip tails (the layout Xenia's texture_util describes): once a level's smaller side is 16 texels or less, it and
 * the levels after it share one 32 x 32 block tile, each at its own offset. The level whose storage holds the tail: */
static uint32_t packed_level(uint32_t w, uint32_t h)
{
    uint32_t a = log2_ceil(w), b = log2_ceil(h), s = a < b ? a : b;
    return s > 4 ? s - 4 : 0;
}

/* where level `mip` lies inside that tile, in blocks */
static void packed_offset(uint32_t w, uint32_t h, uint32_t depth, uint32_t dim, uint32_t block, uint32_t mip,
                          uint32_t *x, uint32_t *y, uint32_t *z)
{
    uint32_t lw = log2_ceil(w), lh = log2_ceil(h), base = packed_level(w, h), k = mip - base;
    *x = *y = *z = 0;
    if (k < 3) { /* 16, 8, 4 blocks along the short side's axis */
        if (lw > lh)
            *y = 16 >> k;
        else
            *x = 16 >> k;
    } else { /* the smallest ones, along the long side */
        uint32_t off = (1u << ((lw > lh ? lw : lh) - base)) >> (k - 2);
        if (lw > lh)
            *x = off;
        else
            *y = off;
        if (off < 4 && dim == 2) { /* 3D: the last ones go along Z */
            uint32_t ld = log2_ceil(depth);
            *z = ld > 1 + k ? (ld - k) * 4 : 4;
        }
    }
    *x /= block, *y /= block;
}

/* a level's storage: level 0 at the base address with the fetch constant's pitch; levels from 1 one after the other
 * at the mip address, sized from the base's next powers of two */
typedef struct tex_storage {
    uint32_t pitch_blocks, height_blocks, row_bytes, z_stride; /* a row; a 3D slice */
    uint32_t stride;                                           /* a face, or the whole of a 3D level */
} tex_storage;

static tex_storage storage_of(const texfmt *tf, uint32_t dim, uint32_t w, uint32_t h, uint32_t depth, uint32_t pitch,
                              uint32_t tiled, uint32_t level, int is_base)
{
    tex_storage s;
    uint32_t pw = is_base ? pitch : pow2_ceil(w) >> level, ph = is_base ? h : pow2_ceil(h) >> level;
    pw = pw ? pw : 1, ph = ph ? ph : 1;
    s.pitch_blocks = (((pw + tf->block - 1) / tf->block) + 31) & ~31u;
    s.height_blocks = (((ph + tf->block - 1) / tf->block) + 31) & ~31u;
    s.row_bytes = s.pitch_blocks * tf->bytes;
    if (!is_base && !tiled)
        s.row_bytes = (s.row_bytes + 255) & ~255u; /* linear mip rows are 256-byte aligned */
    s.z_stride = s.row_bytes * (dim == 0 ? 1 : s.height_blocks);
    s.stride = (s.z_stride * (dim == 2 ? (depth + 3) & ~3u : 1) + 4095) & ~4095u;
    return s;
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
    /* mip levels: from the mip address, up to the fetch constant's last level */
    uint32_t mip_addr = fc[5] & 0xFFFFF000u, max_level = (fc[4] >> 6) & 15, packed = (fc[5] >> 11) & 1;
    uint32_t mip_filter = (fc[3] >> 23) & 3, h1 = dim == 0 ? 1 : h, d3 = dim == 2 ? depth : 1;
    int levels = 1;
    /* mip filter "base map" (2) samples one level, the fetch constant's first (Xenia: mip_min_level), so the levels
     * go up to that one only: a streamed texture's level 0 may not be in memory at all */
    uint32_t last_level = mip_filter == 2 ? (fc[4] >> 2) & 15 : max_level;
    if (mip_addr && last_level) {
        for (uint32_t l = 1; l <= last_level && l < 14; l++) {
            if (!(w >> (l - 1) > 1 || h1 >> (l - 1) > 1 || d3 >> (l - 1) > 1))
                break; /* the level before was 1 x 1 x 1 */
            levels++;
        }
    }
    uint32_t tail = packed ? packed_level(w, h1) : 0xFFFFFFFFu; /* the level whose storage holds the tail */
    /* the fetch constant's first level: a title that streams textures loads the mips, points the sampler at level 1
     * or further and leaves the base address on memory that does not hold level 0 (often the mip address again).
     * Nothing below that level is read, here or by the sampler (GL_TEXTURE_BASE_LEVEL). */
    uint32_t min_level = (fc[4] >> 2) & 15;
    int base_level = (int)min_level < levels ? (int)min_level : levels - 1;
    out->levels = levels;
    out->base_level = base_level;
    out->is_float = tf->is_float;
    out->w = w, out->h = h1, out->slices = slices, out->dim = dim;
    uint64_t hash = (uint64_t)fc[3] << 32 ^ (uint64_t)levels;
    uint32_t hashed_at = 0xFFFFFFFFu; /* the storage hashed last (the levels of a packed tail share one) */
    for (int l = base_level; l < levels; l++) {
        /* what is drawn: the guest's size at this level (a compressed level is read in whole blocks, then cropped) */
        uint32_t lw = w >> l ? w >> l : 1, lh = h1 >> l ? h1 >> l : 1, ld = dim == 2 ? (depth >> l ? depth >> l : 1) : slices;
        /* where it is stored: the base, its own mip storage, or inside the tail's (the base of a texture that is
         * all tail keeps its level 0 in the base storage, the mips in the first mip storage) */
        uint32_t stored = (uint32_t)l;
        if (packed && stored > (tail ? tail : 1))
            stored = tail ? tail : 1;
        /* a texture that is all tail sizes its mips' storage from level 0, not 1 (Xenia's GetGuestTextureLayout) */
        tex_storage st = storage_of(tf, dim, w, h1, depth, pitch, tiled, packed && !tail ? 0 : stored, !stored);
        uint32_t level_addr = fc[1] & 0xFFFFF000u;
        if (stored) {
            level_addr = mip_addr;
            for (uint32_t k = 1; k < stored; k++) {
                tex_storage before = storage_of(tf, dim, w, h1, depth, pitch, tiled, k, 0);
                level_addr += before.stride * (dim == 3 ? 6 : 1);
            }
        }
        uint32_t ox = 0, oy = 0, oz = 0;
        if (packed && (uint32_t)l >= tail)
            packed_offset(w, h1, depth, dim, tf->block, (uint32_t)l, &ox, &oy, &oz);
        uint32_t bw = (lw + tf->block - 1) / tf->block, bh = (lh + tf->block - 1) / tf->block;
        size_t slice_n = (size_t)bw * bh * tf->bytes, src_n = slice_n * ld;
        /* With physical memory at hand the hash is of the level's whole storage as it lies in memory (padding
         * included): no decode to know a texture has not changed, which every cached texture is asked after each
         * swap and resolve (likely much of the 47 ms a Warhammer frame took on the Thor, XBA-318). */
        if (cp->mem) {
            uint32_t at = level_addr & 0x1FFFFFFFu, span = st.stride * (dim == 3 ? 6 : 1);
            if (at != hashed_at) {
                hash = span_hash(cp->mem + at, span < 0x20000000u - at ? span : 0x20000000u - at, hash);
                hashed_at = at;
            }
            if (hash_only) {
                out->lw[l] = lw, out->lh[l] = lh, out->ld[l] = ld;
                continue;
            }
        }
        uint8_t *src = malloc(src_n);
        if (!src) {
            free_decoded(out);
            return 0;
        }
        for (uint32_t z = 0; z < ld; z++)
            for (uint32_t by = 0; by < bh; by++) {
                uint8_t *row = src + z * slice_n + (size_t)by * bw * tf->bytes;
                if (!tiled) { /* a whole row at once */
                    uint32_t off = (dim == 2 ? (z + oz) * st.z_stride : z * st.stride) + (by + oy) * st.row_bytes +
                                   ox * tf->bytes;
                    read_bytes(cp, level_addr + off, bw * tf->bytes, endian, row);
                    continue;
                }
                for (uint32_t bx = 0; bx < bw; bx++) {
                    uint32_t off = dim == 2 ? (uint32_t)tiled3d((int32_t)(bx + ox), (int32_t)(by + oy), (int32_t)(z + oz),
                                                                st.pitch_blocks, st.height_blocks, bpb_log2)
                                            : z * st.stride + (uint32_t)tiled2d((int32_t)(bx + ox), (int32_t)(by + oy),
                                                                                st.pitch_blocks, bpb_log2);
                    read_bytes(cp, level_addr + off, tf->bytes, endian, row + (size_t)bx * tf->bytes);
                }
            }
        if (!cp->mem)
            hash = (hash * 1099511628211ull) ^ fnv(src, src_n);
        out->lw[l] = lw, out->lh[l] = lh, out->ld[l] = ld;
        if (hash_only) {
            free(src);
            continue;
        }
        size_t px = (size_t)lw * lh * ld;
        out->level[l] = malloc(px * (tf->is_float ? 16 : 4));
        if (!out->level[l]) {
            free(src);
            free_decoded(out);
            return 0;
        }
        uint32_t sign = (fc[0] >> 2) & 3;
        for (uint32_t z = 0; z < ld; z++) {
            size_t at = (size_t)z * lw * lh;
            for (uint32_t by = 0; by < bh; by++)
                for (uint32_t bx = 0; bx < bw; bx++) {
                    const uint8_t *s = src + z * slice_n + ((size_t)by * bw + bx) * tf->bytes;
                    if (tf->block == 4) {
                        uint8_t q[16][4];
                        dxt_block(s, tf->format, q);
                        uint8_t *o = out->level[l];
                        for (uint32_t i = 0; i < 16; i++) {
                            uint32_t tx = bx * 4 + i % 4, ty = by * 4 + i / 4;
                            if (tx < lw && ty < lh)
                                memcpy(o + (at + (size_t)ty * lw + tx) * 4, q[i], 4);
                        }
                    } else if (tf->is_float) {
                        texel_f(tf->format, s, sign, (float *)out->level[l] + (at + (size_t)by * lw + bx) * 4);
                    } else {
                        texel(tf->format, s, (uint8_t *)out->level[l] + (at + (size_t)by * lw + bx) * 4);
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
    base &= 0x1FFFFFFFu;
    if (cp->mem)
        return span_hash(cp->mem + base, bytes < 0x20000000u - base ? bytes : 0x20000000u - base, 0);
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

/* A resolve's host picture stands for a texture only when the texture is read as the resolve wrote it: the same
 * format and, for texels wider than a byte, the same endian mode (read with another one, the bytes land in other
 * channels: a front buffer resolved for the display and sampled back as a big-endian texture). */
static int copy_fits(const rt_copy *rc, const uint32_t *fc)
{
    uint32_t fmt = fc[1] & 63, endian = (fc[1] >> 6) & 3;
    return fmt == rc->fmt && (endian == rc->endian || fmt == 2);
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
    if (rc && !copy_fits(rc, fc))
        rc = NULL; /* read another way than it was written: decoded from memory */
    if (rc) {
        uint32_t w = (fc[2] & 0x1FFF) + 1, h = ((fc[2] >> 13) & 0x1FFF) + 1;
        if ((int)w <= rc->w && (int)h <= rc->h) {
            if (t && t->copy == 1 + (int)(rc - g->copies) && t->hash == rc->hash) {
                t->swap_rb = rc->swap_rb;
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
            t->swap_rb = rc->swap_rb;
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
    t->swap_rb = 0;
    t->hash = d.hash;
    t->checked = g->epoch;
    t->used = (int)++g->tex_clock;
    /* a fetch constant whose X sign is gamma (Xenia's TextureSign::kGamma, 3) samples the bytes as gamma-encoded and
     * returns them linear: an sRGB texture does that (the sRGB curve for the console's piecewise-linear one) */
    int gamma = !d.is_float && ((fc[0] >> 2) & 3) == 3;
    GLint ifmt = d.is_float ? (GLint)GL_RGBA16F : gamma ? (GLint)GL_SRGB8_ALPHA8 : (GLint)GL_RGBA8;
    GLenum type = d.is_float ? GL_FLOAT : GL_UNSIGNED_BYTE;
    size_t texel_n = d.is_float ? 16 : 4;
    GLenum target = gl_dim == 1 ? GL_TEXTURE_2D : gl_dim == 3 ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_3D;
    g->glBindTexture(target, t->tex);
    for (int l = d.base_level; l < d.levels; l++) {
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
    g->glTexParameteri(target, GL_TEXTURE_BASE_LEVEL, d.base_level);
    g->glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, d.levels - 1);
    t->levels = d.levels - d.base_level;
    if (t->levels > 1)
        g->st.mipmapped++;
    free_decoded(&d);
    return t->tex;
}

static GLint wrap_mode(uint32_t c)
{
    return c == 0 ? GL_REPEAT : c == 1 ? GL_MIRRORED_REPEAT : GL_CLAMP_TO_EDGE;
}

/* a swizzle (3 bits a component) over a host picture whose red and blue are the other way round from memory's: a
 * resolve that swaps them writes the surface's red where the texture format has blue, the picture kept is unswapped */
static uint32_t swizzle_swap_rb(uint32_t swizzle)
{
    uint32_t out = 0;
    for (int c = 0; c < 4; c++) {
        uint32_t s = (swizzle >> (3 * c)) & 7;
        out |= (s == 0 ? 2u : s == 2 ? 0u : s) << (3 * c);
    }
    return out;
}

/* a swizzle over a host picture (the surface's four channels) that reads as the texture decoded from memory would:
 * a one-channel format gives X in every component, a two-channel one (X, Y, 0, 1), a three-channel one alpha 1 */
static uint32_t swizzle_as_decoded(uint32_t swizzle, uint32_t format)
{
    int channels = 4;
    switch (format) {
    case 2: case 22: case 23: case 24: case 30: case 36: channels = 1; break;
    case 10: case 25: case 31: case 37: channels = 2; break;
    case 4: case 5: case 16: case 17: case 55: case 56: case 57: channels = 3; break;
    default: break;
    }
    uint32_t out = 0;
    for (int c = 0; c < 4; c++) {
        uint32_t s = (swizzle >> (3 * c)) & 7;
        if (s < 4 && channels == 1)
            s = 0;
        else if (s == 2 && channels == 2)
            s = 4;
        else if (s == 3 && channels < 4)
            s = 5;
        out |= s << (3 * c);
    }
    return out;
}

/* a texture's sampling state from its fetch constant: wraps, filters (mips when it has them), swizzle. copy: 0 the
 * texture was decoded from memory, 1 it is a resolve's host picture, 3 one kept with red and blue unswapped. The
 * fetch constant's mip range (dword 4: mip_min_level bits 2-5, mip_max_level 6-9) is the texture's own
 * GL_TEXTURE_BASE_LEVEL / MAX_LEVEL (texture_of), GL's LOD counting from the base level; the mip filter "base map"
 * (2) samples the base level only, as a filter without mipmaps does (Xenia's mip_base_map: MaxLOD = MinLOD). Xenia's
 * base level there is mip_min_level; decode_texture uploads level 0 alone for a base map fetch constant, so one
 * with mip_min_level >= 1 still reads level 0 from the base address. */
static void sampler_state(xenos_gl *g, GLenum target, const uint32_t *fc, int mipped, int copy)
{
    static const GLint swz[6] = {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA, GL_ZERO, GL_ONE};
    if (((fc[3] >> 23) & 3) == 2)
        mipped = 0;
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
    uint32_t swizzle = (fc[3] >> 1) & 0xFFF;
    if (copy)
        swizzle = swizzle_as_decoded(swizzle, fc[1] & 63);
    if (copy & 2)
        swizzle = swizzle_swap_rb(swizzle);
    for (int c = 0; c < 4; c++) {
        uint32_t s = (swizzle >> (3 * c)) & 7;
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
    /* every texture first, then the units: texture_of uploads (and copies resolves) through whatever unit is active,
     * so binding as it goes would leave a sampler's unit on the texture uploaded for the next one */
    GLuint texs[MAX_SAMPLERS];
    for (int i = 0; i < p->nsamplers; i++) {
        uint32_t k = p->sampler_fetch[i];
        texs[i] = k < 32 ? texture_of(g, cp, &cp->regs[0x4800 + 6 * (k & 31)], p->sampler_dim[i]) : 0;
    }
    for (int i = 0; i < p->nsamplers; i++) {
        uint32_t k = p->sampler_fetch[i], dim = p->sampler_dim[i];
        GLenum target = dim == 2 ? GL_TEXTURE_3D : dim == 3 ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
        const uint32_t *fc = &cp->regs[0x4800 + 6 * (k & 31)];
        GLuint tex = texs[i];
        g->glActiveTexture(GL_TEXTURE0 + (GLenum)i);
        g->glBindTexture(target, tex);
        if (!tex) {
            g->st.unsupported_textures++;
            continue;
        }
        int mipped = 0, copy = 0;
        for (int t = 0; t < MAX_TEXTURES; t++)
            if (g->textures[t].tex == tex) {
                mipped = !g->textures[t].copy && g->textures[t].levels > 1;
                copy = g->textures[t].copy ? (g->textures[t].swap_rb ? 3 : 1) : 0;
                break;
            }
        sampler_state(g, target, fc, mipped, copy);
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

/* Reused read-back buffers (WP-1): resolves, their depth and stencil, and the frame read back at the swap take them
 * rather than a malloc / free pair each time (at 1080p a fresh 33 MB mapping: page faults, then an munmap, per
 * resolve on Android). Grown, never shrunk; freed in xenos_gl_destroy. NULL when out of memory. */
enum { SCRATCH_PIXELS, SCRATCH_STENCIL, SCRATCH_ROWS };
static void *scratch_buf(xenos_gl *g, int slot, size_t bytes)
{
    if (g->scratch_cap[slot] < bytes) {
        free(g->scratch[slot]);
        g->scratch[slot] = malloc(bytes);
        g->scratch_cap[slot] = g->scratch[slot] ? bytes : 0;
    }
    return g->scratch[slot];
}

/* How read_raw leaves the texels of a colour texture: GL_RGBA bytes (every fixed-point format), packed
 * GL_UNSIGNED_INT_2_10_10_10_REV words (GL_RGB10_A2), RGBA floats (the float formats) */
enum { PIX_U8X4, PIX_RGB10A2, PIX_F32X4 };

static int pix_layout(GLenum ifmt)
{
    return is_float_fmt(ifmt) ? PIX_F32X4 : ifmt == GL_RGB10_A2 ? PIX_RGB10A2 : PIX_U8X4;
}

static size_t pix_bytes(int lay)
{
    return lay == PIX_F32X4 ? 16 : 4;
}

/* one texel of read_raw's output as RGBA floats */
static void pix_float(int lay, const uint8_t *p, float out[4])
{
    if (lay == PIX_F32X4) {
        memcpy(out, p, 16);
    } else if (lay == PIX_RGB10A2) {
        uint32_t v;
        memcpy(&v, p, 4);
        out[0] = (float)(v & 1023) / 1023.0f, out[1] = (float)((v >> 10) & 1023) / 1023.0f;
        out[2] = (float)((v >> 20) & 1023) / 1023.0f, out[3] = (float)(v >> 30) / 3.0f;
    } else {
        for (int c = 0; c < 4; c++)
            out[c] = (float)p[c] / 255.0f;
    }
}

/* a w x h guest-pixel region at (x0, y0) of a colour texture in its own format (pix_layout(ifmt)), top row first, rows
 * of w texels (scaled textures filtered down to the guest resolution first). The pointer is a scratch buffer, valid
 * until the next read; NULL when out of memory. */
static const uint8_t *read_raw(xenos_gl *g, GLuint tex, GLenum ifmt, int x0, int y0, int w, int h)
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
    int lay = pix_layout(ifmt);
    uint8_t *out = scratch_buf(g, SCRATCH_PIXELS, (size_t)w * h * pix_bytes(lay));
    if (out)
        g->glReadPixels(fx, fy, w, h, GL_RGBA,
                        lay == PIX_F32X4     ? GL_FLOAT
                        : lay == PIX_RGB10A2 ? GL_UNSIGNED_INT_2_10_10_10_REV
                                             : GL_UNSIGNED_BYTE,
                        out);
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

/* One dword the GPU writes at a guest physical address: x is the dword after the swap mode (gpu_swap), its
 * little-endian bytes are what lands in memory (the CPU then reads them big-endian). Straight into cp->wmem when the
 * runtime gives it (the host is little-endian, as gpu_read assumes too), else through write32. */
static inline void gpu_store(const xenos_cp *cp, uint32_t phys, uint32_t x)
{
    phys &= 0x1FFFFFFFu;
    if (cp->wmem)
        memcpy(cp->wmem + phys, &x, 4);
    else
        cp->write32(cp->ctx, phys, __builtin_bswap32(x));
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
        gpu_store(cp, wa, gpu_swap(word, endian));
    }
}

/* ---- writing a resolved rectangle into guest memory (WP-1) ----
 * resolve_write takes a host picture as read_raw leaves it and writes it the way a resolve's destination registers
 * say. The usual pairs are byte (or bit) shuffles and go a direct way: no floats, the tiled address computed once per
 * 16 bytes (4 consecutive 32-bit texels in x, or 2 of 64 bits, are contiguous in a tiled row), stores straight into
 * memory. Everything else goes the generic way (RGBA floats, encode_texel, put_bytes) and gives the same bytes. */
typedef struct {
    uint32_t base;          /* physical address of the texture */
    uint32_t pitch_aligned; /* its pitch in texels, a multiple of 32 */
    uint32_t fmt, endian;   /* destination texture format (encode_texel) and swap mode (0-3) */
    int swap_rb, bias;      /* red / blue exchanged; exponent bias (a factor of 2^bias) */
} resolve_dest;

/* bytes per texel of a destination format, 0 when resolves cannot write it */
static int dest_bytes(uint32_t fmt)
{
    static const float zero[4] = {0, 0, 0, 0};
    uint8_t probe[16];
    return encode_texel(fmt, zero, probe);
}

/* the generic way: any pair encode_texel knows. Returns 0 (nothing written) for a format it does not. */
static int resolve_write_float(const xenos_cp *cp, const resolve_dest *d, const uint8_t *px, int lay, int x0, int y0,
                               int w, int h)
{
    int bpp = dest_bytes(d->fmt);
    if (!bpp)
        return 0;
    unsigned bpp_log2 = bpp == 1 ? 0 : bpp == 2 ? 1 : bpp == 4 ? 2 : bpp == 8 ? 3 : 4;
    float k = d->bias ? ldexpf(1.0f, d->bias) : 1.0f;
    size_t stride = pix_bytes(lay);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float p[4];
            pix_float(lay, px + ((size_t)y * w + x) * stride, p);
            float c[4] = {(d->swap_rb ? p[2] : p[0]) * k, p[1] * k, (d->swap_rb ? p[0] : p[2]) * k, p[3] * k};
            uint8_t b[16];
            int n = encode_texel(d->fmt, c, b);
            uint32_t off = (uint32_t)tiled2d(x0 + x, y0 + y, d->pitch_aligned, bpp_log2);
            put_bytes(cp, d->base + off, b, n, d->endian);
        }
    return 1;
}

enum { DIRECT_8888, DIRECT_2101010, DIRECT_HALF4, DIRECT_F32 };

/* one texel of a direct pair: the bytes it puts in memory (4, or 8 for DIRECT_HALF4) */
static inline __attribute__((always_inline)) void direct_texel(int kind, const uint8_t *sp, int swap_rb,
                                                              uint32_t endian, uint8_t *out)
{
    switch (kind) {
    case DIRECT_8888: { /* RGBA bytes: unorm(b / 255, 8) is b again, so a byte shuffle */
        uint32_t v;
        memcpy(&v, sp, 4);
        if (swap_rb)
            v = (v & 0xFF00FF00u) | (v & 0xFFu) << 16 | ((v >> 16) & 0xFFu);
        v = gpu_swap(v, endian);
        memcpy(out, &v, 4);
        break;
    }
    case DIRECT_2101010: { /* 2_10_10_10_REV words: the fields as they are (unorm(v / 1023, 10) is v again) */
        uint32_t v;
        memcpy(&v, sp, 4);
        if (swap_rb)
            v = (v & 0xC00FFC00u) | (v & 1023u) << 20 | ((v >> 20) & 1023u);
        v = gpu_swap(v, endian);
        memcpy(out, &v, 4);
        break;
    }
    case DIRECT_HALF4: { /* RGBA floats to 16_16_16_16_FLOAT: what encode_texel does, without its switch */
        float f[4];
        memcpy(f, sp, 16);
        uint32_t r = float_to_half(swap_rb ? f[2] : f[0]), b = float_to_half(swap_rb ? f[0] : f[2]);
        uint32_t lo = gpu_swap(r | float_to_half(f[1]) << 16, endian);
        uint32_t hi = gpu_swap(b | float_to_half(f[3]) << 16, endian);
        memcpy(out, &lo, 4);
        memcpy(out + 4, &hi, 4);
        break;
    }
    default: { /* DIRECT_F32: the red float's bits (blue's when swapped) */
        uint32_t v;
        memcpy(&v, sp + (swap_rb ? 8 : 0), 4);
        v = gpu_swap(v, endian);
        memcpy(out, &v, 4);
        break;
    }
    }
}

static inline __attribute__((always_inline)) void direct_rows(const xenos_cp *cp, const resolve_dest *d,
                                                             const uint8_t *px, int x0, int y0, int w, int h,
                                                             int kind, int swap_rb, uint32_t endian)
{
    const unsigned bpp_log2 = kind == DIRECT_HALF4 ? 3 : 2, bpp = 1u << bpp_log2, per = 16u >> bpp_log2;
    const size_t stride = kind == DIRECT_8888 || kind == DIRECT_2101010 ? 4 : 16;
    for (int y = 0; y < h; y++) {
        const uint8_t *sp = px + (size_t)y * w * stride;
        for (int x = 0; x < w;) {
            int gx = x0 + x;
            uint32_t a = (d->base + (uint32_t)tiled2d(gx, y0 + y, d->pitch_aligned, bpp_log2)) & 0x1FFFFFFFu;
            int run = (int)(per - ((unsigned)gx & (per - 1)));
            run = run < w - x ? run : w - x;
            if (cp->wmem && a + (uint32_t)run * bpp <= 0x20000000u) {
                uint8_t *dp = cp->wmem + a;
                for (int i = 0; i < run; i++, sp += stride, dp += bpp)
                    direct_texel(kind, sp, swap_rb, endian, dp);
            } else { /* through write32, or at the very end of physical memory */
                for (int i = 0; i < run; i++, sp += stride) {
                    uint8_t t[8];
                    uint32_t v;
                    direct_texel(kind, sp, swap_rb, endian, t);
                    for (unsigned k = 0; k < bpp; k += 4) {
                        memcpy(&v, t + k, 4);
                        gpu_store(cp, a + (uint32_t)i * bpp + k, v);
                    }
                }
            }
            x += run;
        }
    }
}

/* the direct way, for the pairs it knows (with no exponent bias and a dword-aligned destination); 0 (nothing
 * written) for any other */
static int resolve_write_direct(const xenos_cp *cp, const resolve_dest *d, const uint8_t *px, int lay, int x0, int y0,
                                int w, int h)
{
    if (d->bias || (d->base & 3) || d->endian > 3 || (!cp->wmem && !cp->write32))
        return 0;
    int kind;
    uint32_t f = d->fmt;
    if (lay == PIX_U8X4 && (f == 6 || f == 14 || f == 50 || f == 62))
        kind = DIRECT_8888;
    else if (lay == PIX_RGB10A2 && (f == 7 || f == 54))
        kind = DIRECT_2101010;
    else if (lay == PIX_F32X4 && f == 32)
        kind = DIRECT_HALF4;
    else if (lay == PIX_F32X4 && f == 36)
        kind = DIRECT_F32;
    else
        return 0;
    /* one copy of the loop per pair, swap mode and red / blue exchange: the per-texel work folds into a few
     * instructions (the 8_8_8_8 one runs at about a nanosecond per texel) */
#define DIRECT_ROWS(k, e)                                                                                             \
    (d->swap_rb ? direct_rows(cp, d, px, x0, y0, w, h, k, 1, e) : direct_rows(cp, d, px, x0, y0, w, h, k, 0, e))
#define DIRECT_ENDIAN(k)                                                                                              \
    (d->endian == 0   ? DIRECT_ROWS(k, 0)                                                                             \
     : d->endian == 1 ? DIRECT_ROWS(k, 1)                                                                             \
     : d->endian == 2 ? DIRECT_ROWS(k, 2)                                                                             \
                      : DIRECT_ROWS(k, 3))
    switch (kind) {
    case DIRECT_8888: DIRECT_ENDIAN(DIRECT_8888); break;
    case DIRECT_2101010: DIRECT_ENDIAN(DIRECT_2101010); break;
    case DIRECT_HALF4: direct_rows(cp, d, px, x0, y0, w, h, DIRECT_HALF4, d->swap_rb, d->endian); break;
    default: direct_rows(cp, d, px, x0, y0, w, h, DIRECT_F32, d->swap_rb, d->endian); break;
    }
#undef DIRECT_ENDIAN
#undef DIRECT_ROWS
    return 1;
}

/* A resolved rectangle into guest memory: px is a host picture of w x h texels as read_raw leaves it (layout lay, top
 * row first, rows of w texels) and holds the guest pixels (x0..x0+w-1, y0..y0+h-1) of the resolve, written to the
 * tiled texture d describes. Needs cp->wmem or cp->write32 (and read32 or mem for partial dwords). Returns 0, with
 * nothing written, when the destination format is not one a resolve can write. Callable from anywhere that has the
 * picture (a resolve, or later a deferred one written from its host copy). */
static int resolve_write(const xenos_cp *cp, const resolve_dest *d, const uint8_t *px, int lay, int x0, int y0, int w,
                         int h)
{
    return resolve_write_direct(cp, d, px, lay, x0, y0, w, h) || resolve_write_float(cp, d, px, lay, x0, y0, w, h);
}

/* A depth resolve's rectangle into guest memory: 24_8 (depth dv in the high 24 bits, stencil sv, or 0 when sv is NULL,
 * in the low 8), tiled 32-bit texels, w x h values top row first for the guest pixels at (x0, y0) */
static void depth_write(const xenos_cp *cp, uint32_t base, uint32_t pitch_aligned, uint32_t endian, const uint32_t *dv,
                        const uint8_t *sv, int x0, int y0, int w, int h)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            size_t at = (size_t)y * w + x;
            uint32_t v = dv[at] << 8 | (sv ? sv[at] : 0);
            uint32_t a = base + (uint32_t)tiled2d(x0 + x, y0 + y, pitch_aligned, 2);
            if (a & 3) {
                uint8_t b4[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
                put_bytes(cp, a, b4, 4, endian);
            } else {
                gpu_store(cp, a, gpu_swap(v, endian));
            }
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
        if (c >= 0 && g->surf[c].tex && g->surf[c].base == (info & 0xFFF) &&
            (g->surf[c].fmt & 255) == ((info >> 16) & 15) && (!pitch || g->surf[c].pitch == pitch))
            return c;
        c = get_surface(g, info & 0xFFF, pitch, (info >> 16) & 15, 0, msaa, 0, 0);
        sync_surface(g, c, NULL, 0);
        return c;
    }
    uint32_t dinfo = cp->regs[0x2002];
    int c = g->cur_ds;
    if (c >= 0 && g->surf[c].tex && g->surf[c].base == (dinfo & 0xFFF))
        return c;
    c = get_surface(g, dinfo & 0xFFF, pitch, (dinfo >> 16) & 1, 1, msaa, 0, 0);
    sync_surface(g, c, NULL, 0);
    return c;
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
 * target read back as bytes (the stencil: read_stencil). Returns w * h depth values (0..2^24-1), top row first, or
 * NULL. */
static const char DEPTH_VS[] =
    "#version 300 es\nvoid main() { vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));"
    " gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";
static const char DEPTH_FS[] =
    "#version 300 es\nprecision highp float;\nprecision highp int;\nuniform highp sampler2D d;\nuniform ivec3 at;\n"
    "out vec4 c;\nvoid main() { ivec2 q = (ivec2(gl_FragCoord.xy) + at.xy) * at.z;\n"
    " uint v = uint(clamp(texelFetch(d, q, 0).r, 0.0, 1.0) * 16777215.0 + 0.5);\n"
    " c = vec4(float(v & 255u), float((v >> 8) & 255u), float(v >> 16), 0.0) / 255.0; }\n";

static const uint32_t *read_depth(xenos_gl *g, const surface *s, int x0, int y0, int w, int h)
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
    uint32_t *out = scratch_buf(g, SCRATCH_PIXELS, n * 4);
    if (out) { /* in place: value i is made of bytes 4i..4i+2, read before they are overwritten */
        uint8_t *b = (uint8_t *)out;
        g->glReadBuffer(GL_COLOR_ATTACHMENT0);
        g->glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, b);
        for (size_t i = 0; i < n; i++)
            out[i] = b[4 * i] | (uint32_t)b[4 * i + 1] << 8 | (uint32_t)b[4 * i + 2] << 16;
    }
    g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    g->glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    g->glViewport(0, 0, g->tw * g->scale, g->th * g->scale);
    return out;
}

/* ---- eDRAM aliasing (XBA-296) ----
 * Each surface knows when it was last written. A surface about to be drawn to or resolved takes, oldest first, what
 * overlapping surfaces wrote after it: at the same pitch and MSAA mode, whole tile rows apart, the raw bits go through
 * a pass that encodes them from the source's format and decodes them in its own (32-bit formats among themselves, 64-bit
 * ones too), depth by a blit. Anything else (another pitch, another bit width, 7e3 formats, float targets
 * stood in for by 8_8_8_8) cannot be carried over: the overlapping rows are cleared rather than left stale, and counted.
 * Colour over depth or depth over colour is counted and left as it is. */
static const char ALIAS_FS[] =
    "#version 300 es\nprecision highp float;\nprecision highp int;\nuniform highp sampler2D t;\nuniform ivec3 off;\n"
    "uniform int sc, dc;\nout vec4 c;\n"
    "float gam(float v) { return v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1.0 / 2.4) - 0.055; }\n"
    "float lin(float v) { return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4); }\n"
    "uint fx(float v) { return uint(int(round(clamp(v, -32.0, 32.0) * 32767.0 / 32.0))) & 0xFFFFu; }\n"
    "float xf(uint v) { return float(int(v << 16) >> 16) * 32.0 / 32767.0; }\n"
    "uvec2 enc(vec4 v, int k) {\n"
    " if (k == 1) { v.r = gam(v.r); v.g = gam(v.g); v.b = gam(v.b); k = 0; }\n"
    " if (k == 0) { uvec4 b = uvec4(round(clamp(v, 0.0, 1.0) * 255.0));\n"
    "  return uvec2(b.r | b.g << 8 | b.b << 16 | b.a << 24, 0u); }\n"
    " if (k == 2) { uvec4 b = uvec4(round(clamp(v, 0.0, 1.0) * vec4(1023.0, 1023.0, 1023.0, 3.0)));\n"
    "  return uvec2(b.r | b.g << 10 | b.b << 20 | b.a << 30, 0u); }\n"
    " if (k == 3) return uvec2(fx(v.r) | fx(v.g) << 16, 0u);\n"
    " if (k == 4) return uvec2(packHalf2x16(v.rg), 0u);\n"
    " if (k == 5) return uvec2(floatBitsToUint(v.r), 0u);\n"
    " if (k == 6) return uvec2(fx(v.r) | fx(v.g) << 16, fx(v.b) | fx(v.a) << 16);\n"
    " if (k == 7) return uvec2(packHalf2x16(v.rg), packHalf2x16(v.ba));\n"
    " return uvec2(floatBitsToUint(v.r), floatBitsToUint(v.g)); }\n"
    "vec4 dec(uvec2 r, int k) {\n"
    " uint a = r.x, b = r.y;\n"
    " if (k <= 1) { vec4 v = vec4(a & 255u, (a >> 8) & 255u, (a >> 16) & 255u, a >> 24) / 255.0;\n"
    "  return k == 1 ? vec4(lin(v.r), lin(v.g), lin(v.b), v.a) : v; }\n"
    " if (k == 2) return vec4(a & 1023u, (a >> 10) & 1023u, (a >> 20) & 1023u, a >> 30) / vec4(1023.0, 1023.0, 1023.0, 3.0);\n"
    " if (k == 3) return vec4(xf(a & 0xFFFFu), xf(a >> 16), 0.0, 1.0);\n"
    " if (k == 4) return vec4(unpackHalf2x16(a), 0.0, 1.0);\n"
    " if (k == 5) return vec4(uintBitsToFloat(a), 0.0, 0.0, 1.0);\n"
    " if (k == 6) return vec4(xf(a & 0xFFFFu), xf(a >> 16), xf(b & 0xFFFFu), xf(b >> 16));\n"
    " if (k == 7) return vec4(unpackHalf2x16(a), unpackHalf2x16(b));\n"
    " return vec4(uintBitsToFloat(a), uintBitsToFloat(b), 0.0, 1.0); }\n"
    "void main() { vec4 v = texelFetch(t, ivec2(gl_FragCoord.xy) + off.xy, 0); c = sc < 0 ? v : dec(enc(v, sc), dc); }\n";
static const char RELOAD_DEPTH_FS[] =
    "#version 300 es\nprecision highp float;\nuniform highp sampler2D d;\n"
    "void main() { gl_FragDepth = texelFetch(d, ivec2(gl_FragCoord.xy), 0).r; }\n";

/* the pass's name for a surface's format, -1 when its bits cannot be recovered */
static int alias_class(const xenos_gl *g, const surface *s)
{
    static const signed char map[16] = {0, 1, 2, -1, 3, 6, 4, 7, 0, 0, 2, 0, -1, 0, 5, 8};
    int k = map[s->fmt & 15];
    return k >= 3 && g->no_float ? -1 : k;
}

static uint32_t surface_row_tiles(const surface *s)
{
    uint32_t sx = s->msaa == 2 ? 2 : 1, tw = !s->depth && fmt_wide(s->fmt & 255) ? 40 : 80;
    return (s->pitch * sx + tw - 1) / tw;
}

static int alias_program(xenos_gl *g)
{
    if (g->aprog || g->aprog_failed)
        return g->aprog != 0;
    GLuint v = compile(g, GL_VERTEX_SHADER, DEPTH_VS, "eDRAM alias vs");
    GLuint f = compile(g, GL_FRAGMENT_SHADER, ALIAS_FS, "eDRAM alias fs");
    GLint ok = 0;
    if (v && f) {
        g->aprog = g->glCreateProgram();
        g->glAttachShader(g->aprog, v);
        g->glAttachShader(g->aprog, f);
        g->glLinkProgram(g->aprog);
        g->glGetProgramiv(g->aprog, GL_LINK_STATUS, &ok);
    }
    if (v)
        g->glDeleteShader(v);
    if (f)
        g->glDeleteShader(f);
    if (!ok) {
        if (g->aprog)
            g->glDeleteProgram(g->aprog);
        g->aprog = 0;
        g->aprog_failed = 1;
        return 0;
    }
    g->aprog_off = g->glGetUniformLocation(g->aprog, "off");
    g->aprog_sc = g->glGetUniformLocation(g->aprog, "sc");
    g->aprog_dc = g->glGetUniformLocation(g->aprog, "dc");
    return 1;
}

/* rows y0..y1 of colour surface d from surface s, whose row y - dy they are */
static int alias_copy(xenos_gl *g, const surface *d, const surface *s, int dy, int y0, int y1)
{
    int dc = alias_class(g, d), sc = alias_class(g, s);
    if (dc < 0 || sc < 0 || fmt_wide(d->fmt & 255) != fmt_wide(s->fmt & 255) || !alias_program(g))
        return 0;
    static const GLenum b0 = GL_COLOR_ATTACHMENT0;
    int k = g->scale;
    g->glBindFramebuffer(GL_FRAMEBUFFER, g->rfbo);
    g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, d->tex, 0);
    g->glDrawBuffers(1, &b0);
    g->glDisable(GL_BLEND);
    g->glDisable(GL_DEPTH_TEST);
    g->glDisable(GL_STENCIL_TEST);
    g->glDisable(GL_CULL_FACE);
    g->glColorMask(1, 1, 1, 1);
    g->glEnable(GL_SCISSOR_TEST);
    g->glScissor(0, y0 * k, d->w * k, (y1 - y0) * k);
    g->glViewport(0, 0, d->w * k, d->h * k);
    g->glUseProgram(g->aprog);
    g->glActiveTexture(GL_TEXTURE0);
    g->glBindTexture(GL_TEXTURE_2D, s->tex);
    g->glUniform1i(g->glGetUniformLocation(g->aprog, "t"), 0);
    g->glUniform3i(g->aprog_off, 0, -dy * k, 0);
    g->glUniform1i(g->aprog_sc, sc);
    g->glUniform1i(g->aprog_dc, dc);
    g->glBindVertexArray(g->vao);
    g->glDrawArrays(4 /* GL_TRIANGLES */, 0, 3);
    g->glDisable(GL_SCISSOR_TEST);
    g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    g->glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    return 1;
}

/* rows y0..y1 of depth surface d from depth surface s */
static void alias_depth(xenos_gl *g, const surface *d, const surface *s, int dy, int y0, int y1)
{
    int k = g->scale, hw = d->w * k;
    g->glBindFramebuffer(GL_READ_FRAMEBUFFER, g->rfbo);
    g->glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, s->tex, 0);
    g->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g->tfbo_draw);
    g->glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, d->tex, 0);
    g->glDisable(GL_SCISSOR_TEST);
    g->glBlitFramebuffer(0, (y0 - dy) * k, hw, (y1 - dy) * k, 0, y0 * k, hw, y1 * k,
                         GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST);
    g->glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
    g->glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
    g->glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
}

/* rows y0..y1 of a surface whose eDRAM was overwritten in a way that cannot be carried over */
static void alias_clear(xenos_gl *g, const surface *d, int y0, int y1)
{
    int k = g->scale;
    g->glBindFramebuffer(GL_FRAMEBUFFER, g->rfbo);
    g->glFramebufferTexture2D(GL_FRAMEBUFFER, d->kind == 2 ? GL_DEPTH_STENCIL_ATTACHMENT : GL_COLOR_ATTACHMENT0,
                              GL_TEXTURE_2D, d->tex, 0);
    g->glEnable(GL_SCISSOR_TEST);
    g->glScissor(0, y0 * k, d->w * k, (y1 - y0) * k);
    if (d->kind == 2) {
        g->glDepthMask(1);
        g->glStencilMaskSeparate(GL_FRONT_AND_BACK, 0xFF);
        g->glClearDepthf(1.0f);
        g->glClearStencil(0);
        g->glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    } else {
        static const GLenum b0 = GL_COLOR_ATTACHMENT0;
        g->glDrawBuffers(1, &b0);
        g->glColorMask(1, 1, 1, 1);
        g->glClearColor(0, 0, 0, 0);
        g->glClear(GL_COLOR_BUFFER_BIT);
    }
    g->glDisable(GL_SCISSOR_TEST);
    g->glFramebufferTexture2D(GL_FRAMEBUFFER, d->kind == 2 ? GL_DEPTH_STENCIL_ATTACHMENT : GL_COLOR_ATTACHMENT0,
                              GL_TEXTURE_2D, 0, 0);
    g->glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
}

/* the multisampled buffer filled from the texture, every sample of a pixel alike: a pass, as a blit cannot write a
 * multisampled buffer. GLES 3.0 cannot sample stencil: a depth buffer's stencil starts at 0 again (counted once it
 * had contents). */
static void surface_reload(xenos_gl *g, surface *s)
{
    int hw = s->w * g->scale, hh = s->h * g->scale, depth = s->kind == 2;
    s->ms_reload = 0;
    s->ms_dirty = 0;
    if (depth && !g->zprog && !g->zprog_failed) {
        GLuint v = compile(g, GL_VERTEX_SHADER, DEPTH_VS, "MSAA reload vs");
        GLuint f = compile(g, GL_FRAGMENT_SHADER, RELOAD_DEPTH_FS, "MSAA reload fs");
        GLint ok = 0;
        if (v && f) {
            g->zprog = g->glCreateProgram();
            g->glAttachShader(g->zprog, v);
            g->glAttachShader(g->zprog, f);
            g->glLinkProgram(g->zprog);
            g->glGetProgramiv(g->zprog, GL_LINK_STATUS, &ok);
        }
        if (v)
            g->glDeleteShader(v);
        if (f)
            g->glDeleteShader(f);
        if (!ok) {
            if (g->zprog)
                g->glDeleteProgram(g->zprog);
            g->zprog = 0;
            g->zprog_failed = 1;
        }
    }
    if (depth ? !g->zprog : !alias_program(g)) {
        g->st.edram_stale++;
        return;
    }
    GLenum att = depth ? GL_DEPTH_STENCIL_ATTACHMENT : GL_COLOR_ATTACHMENT0;
    g->glBindFramebuffer(GL_FRAMEBUFFER, g->rfbo);
    g->glFramebufferRenderbuffer(GL_FRAMEBUFFER, att, GL_RENDERBUFFER, s->rb);
    g->glDisable(GL_SCISSOR_TEST);
    g->glDisable(GL_BLEND);
    g->glDisable(GL_STENCIL_TEST);
    g->glDisable(GL_CULL_FACE);
    g->glViewport(0, 0, hw, hh);
    g->glActiveTexture(GL_TEXTURE0);
    g->glBindTexture(GL_TEXTURE_2D, s->tex);
    g->glBindVertexArray(g->vao);
    if (depth) {
        static const GLenum none = 0;
        g->glDrawBuffers(1, &none);
        g->glEnable(GL_DEPTH_TEST);
        g->glDepthFunc(GL_NEVER + 7 /* GL_ALWAYS */);
        g->glDepthMask(1);
        g->glStencilMaskSeparate(GL_FRONT_AND_BACK, 0xFF);
        g->glClearStencil(0);
        g->glClear(GL_STENCIL_BUFFER_BIT);
        g->glUseProgram(g->zprog);
        g->glUniform1i(g->glGetUniformLocation(g->zprog, "d"), 0);
        if (s->written)
            g->st.edram_stale++;
    } else {
        static const GLenum b0 = GL_COLOR_ATTACHMENT0;
        g->glDrawBuffers(1, &b0);
        g->glDisable(GL_DEPTH_TEST);
        g->glColorMask(1, 1, 1, 1);
        g->glUseProgram(g->aprog);
        g->glUniform1i(g->glGetUniformLocation(g->aprog, "t"), 0);
        g->glUniform3i(g->aprog_off, 0, 0, 0);
        g->glUniform1i(g->aprog_sc, -1);
        g->glUniform1i(g->aprog_dc, -1);
    }
    g->glDrawArrays(4 /* GL_TRIANGLES */, 0, 3);
    g->glDisable(GL_DEPTH_TEST);
    g->glFramebufferRenderbuffer(GL_FRAMEBUFFER, att, GL_RENDERBUFFER, 0);
    g->glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    g->glViewport(0, 0, g->tw * g->scale, g->th * g->scale);
}

static void sync_surface(xenos_gl *g, int si, const int *bound, int nbound)
{
    surface *d = &g->surf[si];
    uint32_t dsy = d->msaa ? 2 : 1, dper = surface_row_tiles(d);
    uint32_t dend = d->base + dper * (((uint32_t)d->h * dsy + 15) / 16);
    unsigned since = d->written, newest = d->written;
    uint32_t done = 0; /* surfaces bound together share one stamp: each is taken in its turn */
    int touched = 0;
    for (;;) {
        const surface *s = NULL;
        for (int i = 0; i < MAX_SURFACES; i++) {
            const surface *o = &g->surf[i];
            int together = i == si || ((done >> i) & 1);
            for (int k = 0; k < nbound; k++)
                together |= bound[k] == i;
            if (together || !o->tex || o->written <= since || !o->dirty_h || (o->fmt >> 8) || (d->fmt >> 8))
                continue;
            uint32_t osy = o->msaa ? 2 : 1;
            uint32_t oend = o->base + surface_row_tiles(o) * (((uint32_t)o->dirty_h * osy + 15) / 16);
            if (o->base < dend && d->base < oend && (!s || o->written < s->written))
                s = o;
        }
        if (!s)
            break;
        done |= 1u << (s - g->surf);
        newest = s->written > newest ? s->written : newest;
        touched = 1;
        /* colour and depth over one another are not reinterpreted; a draw of unknown extent proves no overlap */
        if ((s->kind == 2) != (d->kind == 2) || s->unbounded) {
            g->st.edram_stale++;
            continue;
        }
        surface_flush(g, &g->surf[s - g->surf]); /* the source's samples resolved, this surface's own rows kept */
        surface_flush(g, d);
        if (d->rb)
            d->ms_reload = 1;
        uint32_t sper = surface_row_tiles(s);
        int64_t tiles = (int64_t)s->base - (int64_t)d->base;
        int same_grid = s->pitch == d->pitch && s->msaa == d->msaa && sper == dper && tiles % (int64_t)dper == 0;
        int dy = same_grid ? (int)(tiles / (int64_t)dper) * 16 / (int)dsy : 0;
        int y0 = dy > 0 ? dy : 0, y1 = dy + s->dirty_h < d->h ? dy + s->dirty_h : d->h;
        if (same_grid && y1 > y0) {
            if (d->kind == 2) {
                alias_depth(g, d, s, dy, y0, y1);
                g->st.edram_reinterpreted++;
                continue;
            }
            if (alias_copy(g, d, s, dy, y0, y1)) {
                g->st.edram_reinterpreted++;
                continue;
            }
        }
        /* the rows of this surface the other one's tiles reach */
        uint32_t from = s->base > d->base ? s->base - d->base : 0;
        uint32_t send = s->base + sper * (((uint32_t)s->dirty_h * (s->msaa ? 2 : 1) + 15) / 16);
        uint32_t to = (send < dend ? send : dend) - d->base;
        int r0 = (int)(from / dper * 16 / dsy), r1 = (int)((to + dper - 1) / dper * 16 / dsy);
        r1 = r1 < d->h ? r1 : d->h;
        if (r1 > r0)
            alias_clear(g, d, r0, r1);
        g->st.edram_stale++;
    }
    if (touched) {
        d->written = newest;
        g->glViewport(0, 0, g->tw * g->scale, g->th * g->scale);
    }
}

/* The stencil of a depth resolve (XBA-296). GLES 3.0 cannot sample stencil, so it is read a bit at a time: for each of
 * the 8 bits a full-target pass with the stencil test on that bit marks the pixels that have it, read back at the host
 * texel of each guest pixel (the one read_depth takes). Returns w * h values, top row first, or NULL. */
static const char STENCIL_FS[] =
    "#version 300 es\nprecision mediump float;\nout vec4 c;\nvoid main() { c = vec4(1.0); }\n";

static const uint8_t *read_stencil(xenos_gl *g, const surface *s, int x0, int y0, int w, int h)
{
    if (!g->sprog && !g->sprog_failed) {
        GLuint v = compile(g, GL_VERTEX_SHADER, DEPTH_VS, "stencil resolve vs");
        GLuint f = compile(g, GL_FRAGMENT_SHADER, STENCIL_FS, "stencil resolve fs");
        GLint ok = 0;
        if (v && f) {
            g->sprog = g->glCreateProgram();
            g->glAttachShader(g->sprog, v);
            g->glAttachShader(g->sprog, f);
            g->glLinkProgram(g->sprog);
            g->glGetProgramiv(g->sprog, GL_LINK_STATUS, &ok);
        }
        if (v)
            g->glDeleteShader(v);
        if (f)
            g->glDeleteShader(f);
        if (!ok) {
            if (g->sprog)
                g->glDeleteProgram(g->sprog);
            g->sprog = 0;
            g->sprog_failed = 1;
        }
    }
    if (!g->sprog)
        return NULL;
    int sc = g->scale, hw = s->w * sc, hh = s->h * sc;
    if (!g->stex || g->stex_w != hw || g->stex_h != hh) {
        if (g->stex)
            g->glDeleteTextures(1, &g->stex);
        g->stex = new_texture(g, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, hw, hh);
        g->stex_w = hw, g->stex_h = hh;
    }
    size_t n = (size_t)w * h;
    uint8_t *out = scratch_buf(g, SCRATCH_STENCIL, n), *rows = scratch_buf(g, SCRATCH_ROWS, (size_t)hw * h * sc * 4);
    if (!out || !rows)
        return NULL;
    memset(out, 0, n);
    static const GLenum b0 = GL_COLOR_ATTACHMENT0;
    g->glBindFramebuffer(GL_FRAMEBUFFER, g->rfbo);
    g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g->stex, 0);
    g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, s->tex, 0);
    g->glDrawBuffers(1, &b0);
    g->glReadBuffer(GL_COLOR_ATTACHMENT0);
    g->glDisable(GL_SCISSOR_TEST);
    g->glDisable(GL_BLEND);
    g->glDisable(GL_DEPTH_TEST);
    g->glDisable(GL_CULL_FACE);
    g->glDepthMask(0);
    g->glColorMask(1, 1, 1, 1);
    g->glEnable(GL_STENCIL_TEST);
    g->glStencilMaskSeparate(GL_FRONT_AND_BACK, 0);
    g->glStencilOpSeparate(GL_FRONT_AND_BACK, 0x1E00, 0x1E00, 0x1E00); /* GL_KEEP */
    g->glViewport(0, 0, hw, hh);
    g->glUseProgram(g->sprog);
    g->glBindVertexArray(g->vao);
    for (int bit = 0; bit < 8; bit++) {
        g->glClearColor(0, 0, 0, 0);
        g->glClear(GL_COLOR_BUFFER_BIT);
        g->glStencilFuncSeparate(GL_FRONT_AND_BACK, 0x0202 /* GL_EQUAL */, 0xFF, 1u << bit);
        g->glDrawArrays(4 /* GL_TRIANGLES */, 0, 3);
        g->glReadPixels(0, y0 * sc, hw, h * sc, GL_RGBA, GL_UNSIGNED_BYTE, rows);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                if (rows[((size_t)y * sc * hw + (size_t)(x0 + x) * sc) * 4])
                    out[(size_t)y * w + x] |= (uint8_t)(1u << bit);
    }
    g->glDisable(GL_STENCIL_TEST);
    g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
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
        unsupported(g, "resolve source");
        return;
    }
    if (cmd == 2)
        unsupported(g, "resolve of constant one"); /* the clears below still happen; 3 is no copy at all */
    int si = resolve_source(g, cp, src);
    surface *s = &g->surf[si];
    surface_flush(g, s); /* multisampled: the samples averaged into the picture the copy reads */
    x0 = x0 < 0 ? 0 : x0, y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > s->w ? s->w : x1, y1 = y1 > s->h ? s->h : y1;
    if (x1 <= x0 || y1 <= y0) {
        unsupported(g, "empty resolve rectangle");
        return;
    }
    int sc = g->scale;
    if (cmd <= 1 && src < 4) {
        uint32_t info = cp->regs[0x231B], fmt = (info >> 7) & 63, endian = info & 7, swap_rb = (info >> 24) & 1;
        int bias = (int)((int32_t)(info << 10) >> 26); /* copy_dest_exp_bias, bits 16-21 */
        uint32_t pitch = cp->regs[0x231A] & 0x3FFF, height = (cp->regs[0x231A] >> 16) & 0x3FFF;
        uint32_t base = cp->regs[0x2319] & 0x1FFFFFFFu;
        uint32_t pitch_aligned = (pitch + 31) & ~31u;
        int bpp = dest_bytes(fmt);
        const uint8_t *px = bpp && endian < 4 && cp->write32 && cp->read32
                                ? read_raw(g, s->tex, s->ifmt, x0, y0, x1 - x0, y1 - y0)
                                : NULL;
        if (px) {
            const resolve_dest d = {base, pitch_aligned, fmt, endian, (int)swap_rb, bias};
            resolve_write(cp, &d, px, pix_layout(s->ifmt), x0, y0, x1 - x0, y1 - y0);
            g->st.resolves++;
            g->epoch++;
            if (g->nresolved < 16)
                g->resolved[g->nresolved++] = base & 0x1FFFF000u;
            /* the host picture at the internal resolution. A resolve that swaps red and blue (what Direct3D asks for
             * every A8R8G8B8 destination) keeps the picture unswapped and says so: whoever samples it exchanges the
             * two in its swizzle (sampler_state, the frame at the swap). */
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
            /* no picture where it would not read as the memory does: values scaled by an exponent bias */
            if (s->kind == 2 || (base & 0xFFF) || bias) {
                if (rc) {
                    g->glDeleteTextures(1, &rc->tex);
                    memset(rc, 0, sizeof *rc);
                }
            } else {
                if (!rc)
                    rc = victim;
                if (rc->tex && (rc->ifmt != s->ifmt || rc->w != (int)pitch_aligned || rc->h != (int)height_aligned ||
                                rc->swap_rb != (int)swap_rb)) {
                    g->glDeleteTextures(1, &rc->tex);
                    rc->tex = 0;
                }
                rc->swap_rb = (int)swap_rb;
                rc->fmt = fmt, rc->endian = endian;
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
            unsupported(g, "resolve destination format");
        }
    } else if (cmd <= 1) { /* depth out: 24_8 (depth in the high 24 bits, stencil in the low 8), tiled 32-bit */
        uint32_t info = cp->regs[0x231B], fmt = (info >> 7) & 63, endian = info & 7;
        uint32_t pitch = cp->regs[0x231A] & 0x3FFF, base = cp->regs[0x2319] & 0x1FFFFFFFu;
        const uint32_t *dv = (fmt == 22 || fmt == 6 || fmt == 33) && endian < 4 && cp->write32 && cp->read32
                           ? read_depth(g, s, x0, y0, x1 - x0, y1 - y0)
                           : NULL;
        if (dv) {
            uint32_t pitch_aligned = (pitch + 31) & ~31u;
            const uint8_t *sv = read_stencil(g, s, x0, y0, x1 - x0, y1 - y0);
            if (!sv)
                unsupported(g, "stencil of a depth resolve (written as 0)");
            depth_write(cp, base, pitch_aligned, endian, dv, sv, x0, y0, x1 - x0, y1 - y0);
            g->st.resolves++;
            g->st.depth_resolves++;
            g->epoch++;
        } else {
            unsupported(g, "depth resolve destination format");
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
            if (s->rb) {
                if (s->ms_reload)
                    surface_reload(g, s);
                g->glBindFramebuffer(GL_FRAMEBUFFER, g->rfbo);
                g->glEnable(GL_SCISSOR_TEST);
                g->glScissor(x0 * sc, y0 * sc, (x1 - x0) * sc, (y1 - y0) * sc);
                g->glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, s->rb);
                s->ms_dirty = 1;
            } else {
                g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->tex, 0);
            }
            static const GLenum b0 = GL_COLOR_ATTACHMENT0;
            g->glDrawBuffers(1, &b0);
            g->glColorMask(1, 1, 1, 1);
            g->glClearBufferfv(0x1800 /* GL_COLOR */, 0, colour);
            g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
            s->written = ++g->write_clock;
            s->dirty_h = y1 > s->dirty_h ? y1 : s->dirty_h;
        }
        if (clear_d) {
            int di = resolve_source(g, cp, 4);
            uint32_t dc = cp->regs[0x231D];
            surface *ds = &g->surf[di];
            if (ds->rb) {
                if (ds->ms_reload)
                    surface_reload(g, ds);
                ds->ms_dirty = 1;
            }
            g->glBindFramebuffer(GL_FRAMEBUFFER, g->rfbo);
            g->glEnable(GL_SCISSOR_TEST);
            g->glScissor(x0 * sc, y0 * sc, (x1 - x0) * sc, (y1 - y0) * sc);
            if (ds->rb)
                g->glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, ds->rb);
            else
                g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, ds->tex, 0);
            g->glDepthMask(1);
            g->glStencilMaskSeparate(GL_FRONT_AND_BACK, 0xFF);
            g->glClearDepthf((float)(dc >> 8) / 16777215.0f);
            g->glClearStencil((GLint)(dc & 0xFF));
            g->glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
            g->surf[di].written = ++g->write_clock;
            g->surf[di].dirty_h = y1 > g->surf[di].dirty_h ? y1 : g->surf[di].dirty_h;
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
        unsupported(g, "rectangle list capture");
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
            for (uint32_t i = 0; i < n; i++) { /* the dword's bytes in memory order: big-endian word = its bswap */
                uint32_t a = log[2 * i], pa = a & 0x1FFFFFFCu, v = __builtin_bswap32(log[2 * i + 1]);
                uint32_t keep = (a & 3) | (a >> 30) << 2; /* bytes of the dword left as they are (XBA-296) */
                if (keep && cp->read32) {
                    uint32_t mask = 0;
                    for (int k = 0; k < 4; k++)
                        if ((keep >> k) & 1)
                            mask |= 0xFF000000u >> (8 * k);
                    v = (cp->read32(cp->ctx, pa) & mask) | (v & ~mask);
                }
                cp->write32(cp->ctx, pa, v);
            }
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
    if (prim < 0) {
        unsupported(g, "primitive type");
        return;
    }
    if (!d->num_indices) {
        unsupported(g, "draw of no indices");
        return;
    }
    if (d->source_select > 2) {
        unsupported(g, "index source");
        return;
    }
    if (d->source_select == 1 &&
        (!d->imm || d->imm_words < (d->index_size ? d->num_indices : (d->num_indices + 1) / 2))) {
        unsupported(g, "immediate indices shorter than the draw");
        return;
    }
    program *p = get_program(g, cp, d->prim_type == 8 ? 1 : 0);
    program *rp = d->prim_type == 8 ? get_program(g, cp, 2) : NULL;
    if (!p || (d->prim_type == 8 && !rp)) {
        g->st.no_shader++;
        return;
    }
    /* indices: from the DMA buffer or the packet (16 or 32 bits, VGT_DMA_SIZE's swap mode) or 0..n-1 */
    uint32_t n = d->num_indices, *idx = NULL, max_index = n - 1;
    int convert = d->prim_type >= 12 && d->prim_type <= 15;
    /* VGT_INDX_OFFSET is added to every index (Direct3D's base vertex); with PA_SU_SC_MODE_CNTL's
     * multi_prim_ib_ena, VGT_MULTI_PRIM_IB_RESET_INDX cuts the strips, fans, loops and polygons */
    uint32_t index_offset = cp->regs[0x2102];
    int restart = d->source_select != 2 && (cp->regs[0x2205] >> 21) & 1 && prim_resets(d->prim_type);
    uint32_t reset_index = cp->regs[0x2103] & (d->index_size ? 0xFFFFFFFFu : 0xFFFFu);
    int restarted = 0;
    if (d->source_select != 2 || index_offset || convert) {
        idx = malloc(4 * (size_t)n);
        if (!idx) {
            unsupported(g, "out of memory");
            return;
        }
        uint32_t endian = cp->regs[XENOS_REG_VGT_DMA_SIZE] >> 30;
        max_index = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t v, at = d->index_size ? i : i / 2, w = 0;
            if (d->source_select == 0)
                w = gpu_read(cp, d->dma_base + 4 * at, endian);
            else if (d->source_select == 1) /* the packet's dword, as the same dword in an index buffer would read */
                w = gpu_swap(__builtin_bswap32(d->imm[at]), endian);
            if (d->source_select == 2)
                v = i;
            else
                v = d->index_size ? w : (w >> (16 * (i & 1))) & 0xFFFF;
            if (restart && v == reset_index) {
                idx[i] = INDEX_RESTART;
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
            unsupported(g, "vertex index past 2^20");
            return;
        }
    }
    if (convert) {
        uint32_t m = 0, *t = convert_indices(d->prim_type, idx, n, &m);
        free(idx);
        idx = t;
        if (!t || !m) {
            free(idx);
            unsupported(g, t ? "primitive with too few vertices" : "out of memory");
            return;
        }
        n = m;
        restarted = 0; /* every run is its own primitives now */
    }
    /* vertices: every attribute converted to vec4, interleaved */
    uint32_t nv = max_index + 1, na = (uint32_t)p->nattr;
    float *vb = na ? malloc((size_t)nv * na * 16) : NULL;
    if (na && !vb) {
        free(idx);
        unsupported(g, "out of memory");
        return;
    }
    for (uint32_t a = 0; a < na; a++) {
        const attr *at = &p->attrs[a];
        uint32_t c0 = cp->regs[0x4800 + 2 * at->fetch_constant], c1 = cp->regs[0x4800 + 2 * at->fetch_constant + 1];
        for (uint32_t v = 0; v < nv; v++)
            if (!fetch_vertex(cp, c0 & ~3u, c1 & 3, at, v, &vb[(v * na + a) * 4])) {
                free(vb);
                free(idx);
                unsupported(g, "vertex format");
                return;
            }
    }
    if (!bind_targets(g, cp, (rp ? rp : p)->outputs)) { /* the eDRAM surfaces; the host viewport over them */
        free(vb);
        free(idx);
        unsupported(g, "no complete framebuffer for the render targets");
        return;
    }
    g->glUseProgram(p->prog);
    upload_constants(g, cp); /* the guest viewport is applied in the vertex shader (xe_k.vp) */
    apply_state(g, cp, d->prim_type);
    bind_textures(g, cp, p);
    if (p->nvb && !bind_vertex_buffers(g, cp, p)) {
        free(vb);
        free(idx);
        unsupported(g, "vertex format of a computed-index fetch");
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
    int exports = p->memexport || (rp && rp->memexport);
    if (exports)
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
    if (exports)
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
    if (!g->frame_tex || g->frame_w != fw * g->scale || g->frame_h != fh * g->scale || g->frame_fmt != want) {
        if (g->frame_tex)
            g->glDeleteTextures(1, &g->frame_tex);
        GLenum tf, tt;
        upload_fmt(want, &tf, &tt);
        g->frame_tex = new_texture(g, want, tf, tt, fw * g->scale, fh * g->scale);
        g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        g->frame_w = fw * g->scale, g->frame_h = fh * g->scale;
        g->frame_fmt = want;
    }
}

/* an 8_8_8_8_GAMMA picture (GL_SRGB8_ALPHA8) into the RGBA8 frame texture with its gamma-encoded values: the guest
 * scans out the bytes its gamma target holds (through the display's gamma ramp, linear by default), where a blit
 * from sRGB into a linear texture would decode them to linear (a darker picture). The eDRAM alias pass does it:
 * texelFetch decodes, the gamma class encodes again, written as 8_8_8_8. 0 when that pass is not available. */
static int frame_encode(xenos_gl *g, GLuint src, int fw, int fh)
{
    if (!alias_program(g))
        return 0;
    static const GLenum b0 = GL_COLOR_ATTACHMENT0;
    int k = g->scale;
    g->glBindFramebuffer(GL_FRAMEBUFFER, g->rfbo);
    g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g->frame_tex, 0);
    g->glDrawBuffers(1, &b0);
    g->glDisable(GL_BLEND);
    g->glDisable(GL_DEPTH_TEST);
    g->glDisable(GL_STENCIL_TEST);
    g->glDisable(GL_CULL_FACE);
    g->glDisable(GL_SCISSOR_TEST);
    g->glColorMask(1, 1, 1, 1);
    g->glViewport(0, 0, fw * k, fh * k);
    g->glUseProgram(g->aprog);
    g->glActiveTexture(GL_TEXTURE0);
    g->glBindTexture(GL_TEXTURE_2D, src);
    g->glUniform1i(g->glGetUniformLocation(g->aprog, "t"), 0);
    g->glUniform3i(g->aprog_off, 0, 0, 0);
    g->glUniform1i(g->aprog_sc, 1); /* gamma */
    g->glUniform1i(g->aprog_dc, 0); /* 8_8_8_8 */
    g->glBindVertexArray(g->vao);
    g->glDrawArrays(4 /* GL_TRIANGLES */, 0, 3);
    g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    g->glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    return 1;
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
    if (rc && !copy_fits(rc, fc))
        rc = NULL;
    if (front) {
        fw = (int)(fc[2] & 0x1FFF) + 1, fh = (int)((fc[2] >> 13) & 0x1FFF) + 1;
        swizzle = (fc[3] >> 1) & 0xFFF;
    }
    if (rc && fw <= rc->w && fh <= rc->h) {
        src = rc->tex, src_fmt = rc->ifmt;
        if (rc->swap_rb)
            swizzle = swizzle_swap_rb(swizzle);
        g->st.front_buffer_frames++;
    } else if (front) {
        decoded d;
        if (decode_texture(cp, fc, 0, &d)) { /* from memory (no host picture of it) */
            g->st.front_buffer_frames++;
            uint8_t *rgba = d.level[0] ? calloc((size_t)d.w * d.h, 4) : NULL; /* a front buffer has its level 0 */
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
        surface *s = &g->surf[g->cur[0]];
        surface_flush(g, s);
        src = s->tex, src_fmt = s->ifmt;
        fw = fw < s->w ? fw : s->w, fh = fh < s->h ? fh : s->h;
        swizzle = ident;
    }
    if (src) {
        frame_texture(g, fw, fh, src_fmt);
        if (src_fmt != GL_SRGB8_ALPHA8 || !frame_encode(g, src, fw, fh))
            blit_tex(g, src, g->frame_tex, 0, 0, fw, fh, 0, 0);
        g->glBindTexture(GL_TEXTURE_2D, g->frame_tex);
        for (int c = 0; c < 4; c++) {
            uint32_t sel = (swizzle >> (3 * c)) & 7;
            g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R + (GLenum)c, sel < 6 ? swz[sel] : GL_ZERO);
        }
        if (g->readback) {
            GLenum rf = is_float_fmt(src_fmt) ? GL_RGBA16F : GL_RGBA8;
            const uint8_t *px = read_raw(g, g->frame_tex, rf, 0, 0, fw, fh);
            int lay = pix_layout(rf);
            memset(g->frame, 0, (size_t)g->width * g->height * 4);
            for (int y = 0; px && y < fh && y < g->height; y++)
                for (int x = 0; x < fw && x < g->width; x++) {
                    float p[4];
                    pix_float(lay, px + ((size_t)y * fw + x) * pix_bytes(lay), p);
                    uint8_t *o = g->frame + ((size_t)y * g->width + x) * 4;
                    for (int c = 0; c < 4; c++) {
                        uint32_t sel = (swizzle >> (3 * c)) & 7;
                        o[c] = sel < 4 ? (uint8_t)unorm(p[sel], 8) : sel == 5 ? 255 : 0;
                    }
                }
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
            g->surf[g->cur[i]].ms_dirty = g->cur_ms;
        }
    if (g->cur_ds >= 0) {
        g->surf[g->cur_ds].ms_dirty = g->cur_ms;
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
    g->glDisable(GL_POLYGON_OFFSET_FILL);
    g->glColorMask(1, 1, 1, 1);
    g->glFrontFace(GL_CCW);
    g->glActiveTexture(GL_TEXTURE0);
    g->glBindVertexArray(0);
    g->glUseProgram(0);
}
