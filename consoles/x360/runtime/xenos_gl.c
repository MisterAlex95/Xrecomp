/* xenos_gl.c -- see xenos_gl.h. Register layouts follow Xenia's register_table.inc / xenos.h (BSD): the vertex fetch
 * constants (two dwords each from 0x4800: type and dword address, then endian swap and size), the float constants
 * (0x4000 vertex, 0x4400 pixel, four per vec4), booleans (0x4900, eight words), loops (0x4908, 32 words), the draw
 * initiator's primitive types, VGT_DMA_SIZE's swap mode, PA_CL_VTE_CNTL and the viewport scale / offset. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <pthread.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    X(void, glBindBufferRange, (GLenum, GLuint, GLuint, GLintptr, GLsizeiptr))                                         \
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
    X(void, glDepthRangef, (GLfloat, GLfloat))                                                                         \
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
    X(void, glClearBufferfi, (GLenum, GLint, GLfloat, GLint))                                                         \
    X(void, glInvalidateFramebuffer, (GLenum, GLsizei, const GLenum *))                                               \
    X(void, glTexImage3D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *))   \
    X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))                                          \
    X(void, glBlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum))          \
    X(void, glGetIntegerv, (GLenum, GLint *))                                                                          \
    X(void, glGetFloatv, (GLenum, GLfloat *))                                                                          \
    X(void, glUniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat))                                              \
    X(void, glRenderbufferStorageMultisample, (GLenum, GLsizei, GLenum, GLsizei, GLsizei))                            \
    X(void, glGetInternalformativ, (GLenum, GLenum, GLenum, GLsizei, GLint *))                                        \
    X(void, glUniform3i, (GLint, GLint, GLint, GLint))                                                               \
    X(void, glGenSamplers, (GLsizei, GLuint *))                                                                        \
    X(void, glDeleteSamplers, (GLsizei, const GLuint *))                                                               \
    X(void, glBindSampler, (GLuint, GLuint))                                                                           \
    X(void, glSamplerParameteri, (GLuint, GLenum, GLint))                                                              \
    X(void, glSamplerParameterf, (GLuint, GLenum, GLfloat))

#define MAX_ATTRS 16
#define UBO_BLOCK (4096 + 32 + 128 + 48) /* upload_constants: one stage's uniform block */
#define MAX_SAMPLERS 16
#define MAX_TEXTURES 1024    /* WP-24: draw-heavy titles keep more than 128 fetch constants alive */
#define TEX_BUCKETS 512      /* texture_of's hash of the fetch constant (a power of two) */
#define MAX_REASONS 32

typedef struct attr {
    uint32_t fetch_constant, offset, format, stride, is_signed, is_integer;
    int exp_adjust;
} attr;

/* a vertex buffer fetched at computed indices, converted once into its RGBA32F texture (bind_vertex_buffers): reused
 * while the key matches and the source bytes hash the same */
typedef struct vb_cached {
    uint32_t base, c1;           /* the fetch constant's address and its size / swap mode dword */
    attr a;                      /* the layout read from it (fetch_constant cleared: not part of the key) */
    uint32_t count;              /* elements converted */
    size_t bytes;                /* the size of its texture */
    uint64_t hash;               /* span_hash of those bytes when converted */
    uint32_t checked;            /* the epoch the hash was last confirmed in */
    unsigned used;               /* LRU clock */
    GLuint tex;
} vb_cached;

#define MAX_VB_CACHE 64              /* Adventure Time's dungeon scene cycles through more than 32 */
#define VB_CACHE_BYTES (64u << 20)   /* their textures' total size, beyond which the oldest are deleted */

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
    int direct;       /* WP-18: tex is that copy's own texture (the fetch constant's size is the picture's), not ours */
    uint32_t swz;     /* WP-24: 1 + the GL swizzle last set on tex (0: unknown); a direct one's is the copy's */
    int swap_rb;      /* that copy's: red and blue are the other way round from the memory the fetch constant reads */
    int levels;       /* mip levels uploaded */
    int copy_w, copy_h; /* a copy's texture: its host size and internal format (kept for the next copy that fits) */
    GLenum copy_ifmt;
    uint32_t gen;     /* the copy's generation it was taken from (rt_copy.gen) */
    uint32_t lo, hi;  /* decoded from memory: the physical bytes its levels lie in (lazy resolves flush over them) */
    int16_t next1;    /* WP-24: 1 + the next entry in its key's bucket (0: none); linked while tex != 0 */
} texture;

static void tex_unlink(xenos_gl *g, texture *t); /* WP-24 */

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
    /* WP-17: a resolve's clear not done yet: done on the draw framebuffer at the start of the next pass that binds
     * the surface (bind_targets), or through rfbo before anything else reads or writes the surface (pclear_flush);
     * on rb when the surface has one, else on tex, as resolve_copy did it at once before */
    int pclear;
    int pc_x0, pc_y0, pc_x1, pc_y1;        /* guest pixels */
    float pc_colour[4];                    /* a colour surface's clear value */
    float pc_depth;                        /* a depth surface's */
    int pc_stencil;
} surface;

/* what a resolve wrote to guest memory, kept as a host picture too (the front buffer at the internal resolution,
 * render to texture without a trip through memory while that memory is unchanged) */
typedef struct rt_copy {
    uint32_t base, bytes;  /* physical destination, extent */
    uint64_t hash;         /* of those bytes after the resolve */
    uint64_t sparse;       /* WP-12: sparse_hash of the same bytes, what the check once per epoch compares */
    uint32_t checked;      /* the epoch the hash was last confirmed in */
    uint32_t checks;       /* sparse checks since the last full one (COPY_FULL_EVERY) */
    GLuint tex;            /* the destination's pitch x th at the internal resolution, top row first */
    int w, h;              /* pitch and height, both 32-aligned: the memory it stands for */
    uint32_t swz;          /* WP-24: 1 + the GL swizzle last set on tex (0: unknown) */
    int th;                /* WP-18: the picture's rows: RB_COPY_DEST_PITCH's height (or what was resolved past it),
                            * aligned only when that is unset, so that a texture of the destination's own size samples
                            * the picture itself */
    GLenum ifmt;
    int swap_rb;           /* the resolve exchanged red and blue in memory; the picture has them as the surface did */
    uint32_t fmt, endian;  /* the destination's texture format and endian mode: how memory was written */
    unsigned used;
    uint32_t gen;          /* moves at each resolve into it (unique across copies): a texture taken from the picture
                            * is up to date while its gen is this one */
    /* WP-2, lazy resolves: the picture holds pixels guest memory does not have yet (hash is of the memory as it is,
     * untouched), in the guest rectangle (px0, py0)-(px1, py1); frame is the swap count of the last resolve */
    int pending;
    int px0, py0, px1, py1;
    unsigned frame;
} rt_copy;

/* WP-10: rectangles per second pass of a rectangle list on the GPU: their 3 captured vertices of 16 vec4 each are one
 * uniform block of 768 vec4 (12,288 bytes, under GLES 3.0's minimum GL_MAX_UNIFORM_BLOCK_SIZE of 16,384) */
#define RECT_CHUNK 16
#define RECT_CHUNK_BYTES (RECT_CHUNK * 3 * 15 * 16)

#define GPU_TIMER_QUERIES 4 /* frames in flight a timer query may trail by */
#define GL_TIME_ELAPSED_EXT 0x88BF
#define GL_QUERY_RESULT 0x8866
#define GL_QUERY_RESULT_AVAILABLE 0x8867
#define GL_GPU_DISJOINT_EXT 0x8FBB

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
    int angle; /* loaded from X360_ANGLE_DIR: its default display, not the surfaceless one */
    /* WP-19, the perf line's GPU time: GL_TIME_ELAPSED_EXT queries (EXT_disjoint_timer_query) from one swap to the
     * next, a ring of GPU_TIMER_QUERIES read when their result is there (never waited for); gpu_timer 0: none */
    int gpu_timer, tq_active;
    unsigned tq_head, tq_n;   /* the oldest query ended and not read yet, how many */
    GLuint tq[GPU_TIMER_QUERIES];
    void (*glGenQueriesT)(GLsizei, GLuint *);
    void (*glDeleteQueriesT)(GLsizei, const GLuint *);
    void (*glBeginQueryT)(GLenum, GLuint);
    void (*glEndQueryT)(GLenum);
    void (*glGetQueryObjectuivT)(GLuint, GLenum, GLuint *);
    void (*glGetQueryObjectui64vT)(GLuint, GLenum, uint64_t *);
    EGLDisplay dpy;
    EGLContext ctx;
    int attached;             /* on the caller's current context (xenos_gl_attach): no EGL of our own */
    int ready;                /* the GL objects exist (destroy deletes them) */
    GLuint fbo, rfbo, tfbo_draw, rtex, vao, vbo, ibo, ubo[2], tfbo, vbtex[MAX_ATTRS];
    /* what each stage's uniform buffer holds (upload_constants skips an equal block), whether it is valid, and
     * whether indices 0 / 1 must be bound again (an attached context's owner draws between frames) */
    uint8_t ubo_shadow[2][UBO_BLOCK];
    int ubo_valid[2], ubo_rebind;
    GLuint dprog, dtex;       /* depth resolves: the pass that packs depth into RGBA8, its target */
    GLint dprog_at;
    int dtex_w, dtex_h, dprog_failed;
    GLuint sprog, stex;       /* the stencil of depth resolves: the pass marking one bit, its target */
    int stex_w, stex_h, sprog_failed;
    GLuint dsprog;            /* depth resolves: the pass that packs depth and stencil (WP-38) */
    GLint dsprog_at;
    int dsprog_failed;
    int rtex_w, rtex_h;
    GLenum rtex_fmt;
    void *scratch[6];         /* reused buffers (scratch_buf: SCRATCH_SLOTS): grown, never shrunk */
    size_t scratch_cap[6];
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
    int fbo_checked;          /* the draw FBO was found complete and no attachment changed since (bind_targets) */
    /* sync_surface's stale-row clears of the surfaces bind_targets binds, done on the draw FBO once they are attached
     * (WP-11): not a pass of their own through rfbo each */
    struct {
        int si, y0, y1;
    } dclear[16];
    int ndclear, defer_clears;
    int tw, th;               /* the guest size of the bound colour target 0 (the viewport's frame of reference) */
    rt_copy copies[MAX_COPIES];
    unsigned copy_clock;
    uint32_t copy_gen;        /* the last rt_copy.gen given */
    int lazy;                 /* WP-2: RECOMP_X360_RESOLVE=lazy, resolves write guest memory only when needed */
    int keep_noop;            /* WP-9: RECOMP_X360_NOOP_DRAWS=keep, draws that cannot write go through all the same */
    int rects_gpu;            /* WP-10: rectangle lists' fourth corner made on the GPU (RECOMP_X360_RECTS=cpu: not) */
    int copy_hash_full;       /* WP-12: RECOMP_X360_COPY_HASH=full, every check of a copy's memory hashes all of it */
    int tiler;                /* WP-17: TILER_OFF (RECOMP_X360_TILER=0), TILER_ON (default), TILER_MAGENTA (=magenta) */
    int depth_untouched;      /* WP-17 / WP-28: unless RECOMP_X360_UNUSED_DEPTH=stamped, see bind_targets */
    int rects_captured;       /* WP-29: vertices of 3 the start-up self-test's transform feedback captured (rects_selftest) */
    int vx, vy;               /* WP-28: the draw goes to an MSAA view's single-sampled stand-in (bind_targets): each guest
                               * pixel of the view is vx x vy pixels of the bound surface; 1 x 1 otherwise */
    int npending;             /* copies with pending set */
    int in_resolve;           /* resolve() is timing itself (resolve_ns) */
    unsigned swaps;           /* xenos_gl_swap calls (the age of a pending copy) */
    GLuint frame_tex;         /* the last swap's picture: a texture of frame_w x frame_h host pixels, top row first */
    int frame_w, frame_h;
    /* WP-18: the last swap's picture is a resolve copy's own texture (1 + its index; 0: frame_tex), show_w x show_h
     * host pixels with the frame's swizzle and filters set on it; taken into frame_tex (frame_detach) before that
     * texture changes or is sampled with other parameters */
    int frame_copy;
    int show_w, show_h;
    GLint frame_swz[4];       /* the swizzle last set on the frame's texture */
    int copy_direct;          /* WP-18: copies sampled in place (RECOMP_X360_COPY_TEXTURES=blit: not) */
    int direct_present;       /* WP-18: the front buffer's copy shown as it is (RECOMP_X360_DIRECT_PRESENT=1), not
                               * blitted into frame_tex: the next resolve into it then waits for the window's read
                               * (~18 ms/frame on the Mac), so off by default */
    /* WP-24: GLES 3 sampler objects, one per sampling state met (wraps, filters, anisotropy), bound per unit (the
     * texture keeps its swizzle and mip range). RECOMP_X360_SAMPLERS=0: texture parameters every bind, as before. */
    int samplers_on;
    struct {
        uint32_t key;
        GLuint id;
    } sampler[64];
    int nsampler;
    GLuint unit_sampler[32];  /* what each texture unit has bound (units used by bind_textures and the passes) */
    /* WP-24: what apply_state last set, by group (the registers it was made from), valid while gs_valid: every other
     * function that changes blend, masks, depth / stencil, culling, polygon offset or scissor clears it (GS_DIRTY);
     * the program, vertex array and enabled attribute arrays as last set (use_program, bind_vao, attribs_enable) */
    int gs_valid;
    uint32_t gs_blend[11], gs_depth[6], gs_cull, gs_offset[6], gs_scissor[6];
    GLuint cur_prog, cur_vao;
    uint32_t attr_on;         /* attribute arrays enabled on vao (bit n: location n) */
    GLenum frame_fmt;         /* frame_tex's internal format */
    uint8_t *frame;
    char cache[512], frames[512];
    unsigned max_frames;
    program **progs;          /* every (vs, ps, kind) met, failed ones too: each allocated once, never moved */
    int nprogs, cap_progs;
    program **prog_hash;      /* buckets of progs by (vs crc, ps crc, kind), prog_buckets of them (a power of two) */
    unsigned prog_buckets;
    /* WP-12: each stage's microcode CRC (get_program), kept while the command processor loads nothing into that stage
     * (xenos_shader.loads and words unchanged, the same xenos_cp; a state load forgets them: do_reset) */
    const xenos_cp *crc_cp;
    uint32_t crc_loads[2], crc_words[2], crc_val[2];
    int crc_ok[2];
    uint32_t missing[64];
    int nmissing;
    xenos_gl_stats st;
    texture textures[MAX_TEXTURES];
    int16_t tex_bucket[TEX_BUCKETS]; /* WP-24: 1 + the first entry of each bucket (0: none) */
    unsigned tex_clock;
    vb_cached vbc[MAX_VB_CACHE]; /* computed-index vertex buffers already converted */
    unsigned vbc_clock, vbc_draw; /* LRU clock; its value when the current draw started binding them */
    size_t vbc_bytes;             /* the cached textures' total size */
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
static void use_program(xenos_gl *g, GLuint prog);
static void bind_vao(xenos_gl *g, GLuint vao);
static void attribs_enable(xenos_gl *g, uint32_t mask);

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

/* X360_ANGLE_DIR: DIR/NAME.dylib (.so off Apple), loaded global so libEGL finds libGLESv2 */
static void *open_in(const char *dir, const char *name)
{
#ifdef __APPLE__
    static const char ext[] = ".dylib";
#else
    static const char ext[] = ".so";
#endif
    char path[1024];
    snprintf(path, sizeof path, "%s/%s%s", dir, name, ext);
    void *h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!h)
        fprintf(stderr, "xenos_gl: X360_ANGLE_DIR: %s\n", dlerror());
    return h;
}

/* the perf counters' clock (xenos_gl_stats' *_ns): a vDSO call, a few per resolve */
static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* WP-15, the perf line's passes: render passes as a tiler sees them, counted in the GL entry points rather than at the
 * ~40 places that bind framebuffers. A pass starts at the first draw, clear or blit after the draw framebuffer or one
 * of its attachments changed. The wrappers go in g's function table (load_gl); the real entry points and the count are
 * process-wide (the renderers of a process share one GLES library, and in practice there is one renderer, on one
 * thread). A renderer whose entry points are not the ones the wrappers call keeps its own, uncounted.
 *
 * WP-19, loads: the attachments a tiler reads back from memory at the start of a pass, those of the draw framebuffer
 * that the pass's first operation neither clears whole (glClear / glClearBuffer* with the scissor test off and the
 * colour / depth write mask on) nor finds invalidated (glInvalidateFramebuffer since the framebuffer was bound). A
 * depth-stencil buffer counts once, loaded unless both depth and stencil are cleared or invalidated. The attachments
 * of each framebuffer are followed in the wrappers too (names up to PC_FBS; a larger one counts as one colour
 * buffer); the colour masks of render targets 0-3 through glColorMask / glColorMaski. */
#define PC_FBS 4096
#define PC_COLOR 0x0Fu /* attachment bits: colour 0-3, depth, stencil */
#define PC_DEPTH 0x10u
#define PC_STENCIL 0x20u
static struct {
    void (*bind)(GLenum, GLuint);
    void (*tex2d)(GLenum, GLenum, GLenum, GLuint, GLint);
    void (*rb)(GLenum, GLenum, GLenum, GLuint);
    void (*draw_arrays)(GLenum, GLint, GLsizei);
    void (*draw_elements)(GLenum, GLsizei, GLenum, const void *);
    void (*clear)(GLbitfield);
    void (*clear_fv)(GLenum, GLint, const GLfloat *);
    void (*clear_fi)(GLenum, GLint, GLfloat, GLint);
    void (*blit)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
    void (*gen_fb)(GLsizei, GLuint *);
    void (*invalidate)(GLenum, GLsizei, const GLenum *);
    void (*enable)(GLenum);
    void (*disable)(GLenum);
    void (*color_mask)(GLboolean, GLboolean, GLboolean, GLboolean);
    void (*color_maski)(GLuint, GLboolean, GLboolean, GLboolean, GLboolean);
    void (*depth_mask)(GLboolean);
    GLuint draw_fb, read_fb;
    int changed;
    int scissor;          /* GL_SCISSOR_TEST on */
    unsigned masked;      /* attachment bits whose writes are masked (colour targets not RGBA, depth writes off) */
    unsigned invalidated; /* attachment bits of the draw framebuffer invalidated since it was bound */
    uint8_t att[PC_FBS];  /* each framebuffer's attachment bits */
    uint64_t passes, loads;
} pc = {.changed = 1};
static uint64_t hashed_bytes; /* WP-15: bytes of guest memory hashed (textures, copies, vertex buffers), process-wide */

#define PC_DRAW_TARGET(t) ((t) == GL_FRAMEBUFFER || (t) == GL_DRAW_FRAMEBUFFER)
#define PC_READ_TARGET(t) ((t) == GL_FRAMEBUFFER || (t) == GL_READ_FRAMEBUFFER)
static unsigned pc_att_bits(GLenum a)
{
    if (a >= 0x8CE0 && a < 0x8CE4) /* GL_COLOR_ATTACHMENT0-3 */
        return 1u << (a - 0x8CE0);
    return a == 0x8D00 ? PC_DEPTH : a == 0x8D20 ? PC_STENCIL : a == 0x821A ? PC_DEPTH | PC_STENCIL : 0;
}
/* the pass's first operation clears `cleared` (attachment bits; a masked or scissored clear clears nothing whole) */
static inline void pc_render(unsigned cleared)
{
    if (pc.changed) {
        pc.changed = 0, pc.passes++;
        unsigned a = pc.draw_fb < PC_FBS ? pc.att[pc.draw_fb] : 1u;
        if (!pc.scissor)
            a &= ~(cleared & ~pc.masked);
        a &= ~pc.invalidated;
        pc.loads += (uint64_t)__builtin_popcount(a & PC_COLOR) + ((a & (PC_DEPTH | PC_STENCIL)) != 0);
    }
}
static void pc_bind(GLenum t, GLuint fb)
{
    pc.bind(t, fb);
    if (PC_READ_TARGET(t))
        pc.read_fb = fb;
    if (PC_DRAW_TARGET(t) && fb != pc.draw_fb)
        pc.draw_fb = fb, pc.changed = 1, pc.invalidated = 0;
}
/* an attachment of the framebuffer bound to t changed: its bits */
static void pc_attach(GLenum t, GLenum a, GLuint obj)
{
    unsigned bits = pc_att_bits(a);
    if (PC_DRAW_TARGET(t)) {
        pc.changed = 1, pc.invalidated &= ~bits;
        if (pc.draw_fb < PC_FBS)
            pc.att[pc.draw_fb] = (uint8_t)(obj ? pc.att[pc.draw_fb] | bits : pc.att[pc.draw_fb] & ~bits);
    } else if (PC_READ_TARGET(t) && pc.read_fb < PC_FBS)
        pc.att[pc.read_fb] = (uint8_t)(obj ? pc.att[pc.read_fb] | bits : pc.att[pc.read_fb] & ~bits);
}
static void pc_tex2d(GLenum t, GLenum a, GLenum tt, GLuint tex, GLint l)
{
    pc.tex2d(t, a, tt, tex, l);
    pc_attach(t, a, tex);
}
static void pc_rb(GLenum t, GLenum a, GLenum rt, GLuint rb)
{
    pc.rb(t, a, rt, rb);
    pc_attach(t, a, rb);
}
static void pc_gen_fb(GLsizei n, GLuint *fbs)
{
    pc.gen_fb(n, fbs);
    for (GLsizei i = 0; i < n; i++)
        if (fbs[i] < PC_FBS)
            pc.att[fbs[i]] = 0; /* a name used before: none of its old attachments */
}
static void pc_invalidate(GLenum t, GLsizei n, const GLenum *a)
{
    pc.invalidate(t, n, a);
    if (PC_DRAW_TARGET(t))
        for (GLsizei i = 0; i < n; i++)
            pc.invalidated |= a[i] == 0x1800 /* GL_COLOR (framebuffer 0) */ ? 1u : pc_att_bits(a[i]);
}
static void pc_enable(GLenum cap)
{
    pc.enable(cap);
    if (cap == GL_SCISSOR_TEST)
        pc.scissor = 1;
}
static void pc_disable(GLenum cap)
{
    pc.disable(cap);
    if (cap == GL_SCISSOR_TEST)
        pc.scissor = 0;
}
static void pc_color_mask(GLboolean r, GLboolean gr, GLboolean b, GLboolean al)
{
    pc.color_mask(r, gr, b, al);
    pc.masked = (pc.masked & ~PC_COLOR) | (r && gr && b && al ? 0 : PC_COLOR);
}
static void pc_color_maski(GLuint i, GLboolean r, GLboolean gr, GLboolean b, GLboolean al)
{
    pc.color_maski(i, r, gr, b, al);
    if (i < 4)
        pc.masked = (pc.masked & ~(1u << i)) | (r && gr && b && al ? 0 : 1u << i);
}
static void pc_depth_mask(GLboolean on)
{
    pc.depth_mask(on);
    pc.masked = (pc.masked & ~PC_DEPTH) | (on ? 0 : PC_DEPTH);
}
static void pc_draw_arrays(GLenum m, GLint f, GLsizei n)
{
    pc_render(0);
    pc.draw_arrays(m, f, n);
}
static void pc_draw_elements(GLenum m, GLsizei n, GLenum t, const void *i)
{
    pc_render(0);
    pc.draw_elements(m, n, t, i);
}
static void pc_clear(GLbitfield b)
{
    pc_render((b & GL_COLOR_BUFFER_BIT ? PC_COLOR : 0) | (b & GL_DEPTH_BUFFER_BIT ? PC_DEPTH : 0) |
              (b & GL_STENCIL_BUFFER_BIT ? PC_STENCIL : 0));
    pc.clear(b);
}
static void pc_clear_fv(GLenum b, GLint i, const GLfloat *v)
{
    pc_render(b == 0x1800 /* GL_COLOR */ ? (i >= 0 && i < 4 ? 1u << i : 0) : b == 0x1801 /* GL_DEPTH */ ? PC_DEPTH : 0);
    pc.clear_fv(b, i, v);
}
static void pc_clear_fi(GLenum b, GLint i, GLfloat d, GLint st)
{
    pc_render(PC_DEPTH | PC_STENCIL); /* GL_DEPTH_STENCIL, the only buffer it takes */
    pc.clear_fi(b, i, d, st);
}
static void pc_blit(GLint a, GLint b, GLint c, GLint d, GLint e, GLint f, GLint h, GLint i, GLbitfield m, GLenum fl)
{
    pc_render(0);
    pc.blit(a, b, c, d, e, f, h, i, m, fl);
}

/* the wrappers into g's table, when its entry points are the ones they call */
static void count_passes(xenos_gl *g)
{
    if (!pc.bind) {
        pc.bind = g->glBindFramebuffer, pc.tex2d = g->glFramebufferTexture2D, pc.rb = g->glFramebufferRenderbuffer;
        pc.draw_arrays = g->glDrawArrays, pc.draw_elements = g->glDrawElements;
        pc.clear = g->glClear, pc.clear_fv = g->glClearBufferfv, pc.clear_fi = g->glClearBufferfi;
        pc.blit = g->glBlitFramebuffer, pc.gen_fb = g->glGenFramebuffers, pc.invalidate = g->glInvalidateFramebuffer;
        pc.enable = g->glEnable, pc.disable = g->glDisable, pc.color_mask = g->glColorMask;
        pc.depth_mask = g->glDepthMask;
    }
    if (pc.bind != g->glBindFramebuffer || pc.tex2d != g->glFramebufferTexture2D ||
        pc.rb != g->glFramebufferRenderbuffer || pc.draw_arrays != g->glDrawArrays ||
        pc.draw_elements != g->glDrawElements || pc.clear != g->glClear || pc.clear_fv != g->glClearBufferfv ||
        pc.clear_fi != g->glClearBufferfi || pc.blit != g->glBlitFramebuffer || pc.gen_fb != g->glGenFramebuffers ||
        pc.invalidate != g->glInvalidateFramebuffer || pc.enable != g->glEnable || pc.disable != g->glDisable ||
        pc.color_mask != g->glColorMask || pc.depth_mask != g->glDepthMask)
        return;
    g->glBindFramebuffer = pc_bind, g->glFramebufferTexture2D = pc_tex2d, g->glFramebufferRenderbuffer = pc_rb;
    g->glDrawArrays = pc_draw_arrays, g->glDrawElements = pc_draw_elements;
    g->glClear = pc_clear, g->glClearBufferfv = pc_clear_fv, g->glClearBufferfi = pc_clear_fi;
    g->glBlitFramebuffer = pc_blit, g->glGenFramebuffers = pc_gen_fb, g->glInvalidateFramebuffer = pc_invalidate;
    g->glEnable = pc_enable, g->glDisable = pc_disable, g->glColorMask = pc_color_mask;
    g->glDepthMask = pc_depth_mask;
}
/* glColorMaski comes later (draw_buffers_caps), optional: wrapped when g's table holds the other wrappers */
static void count_passes_maski(xenos_gl *g)
{
    if (!g->glColorMaski || g->glBindFramebuffer != pc_bind)
        return;
    if (!pc.color_maski)
        pc.color_maski = g->glColorMaski;
    if (pc.color_maski == g->glColorMaski)
        g->glColorMaski = pc_color_maski;
}

static void fail(char *why, size_t n, const char *msg)
{
    if (why && n)
        snprintf(why, n, "%s", msg);
}

/* WP-17, what a tiling GPU (the phones') loads into its tile memory at the start of a pass and stores at its end: the
 * contents of an attachment that is about to be overwritten whole are invalidated first (glInvalidateFramebuffer,
 * GLES 3.0), so the driver neither loads them nor keeps them. Only where the overwrite is known on the CPU: a blit
 * over all of its destination, a clear of a whole surface. (A draw's coverage would need its vertex shader run on the
 * CPU: Direct3D's clear rectangles here cover 1072 rows through a 4x MSAA view of the tiles and an 8-row strip through
 * the 1x one, never a whole attachment by themselves.) RECOMP_X360_TILER=0 turns WP-17 off; RECOMP_X360_TILER=magenta
 * clears to magenta (depth 0, stencil 0x5A) where it would invalidate: a frame that changes shows a wrong proof, the
 * only way to check one where the driver keeps the contents anyway (ANGLE on the Mac). */
enum { TILER_OFF, TILER_ON, TILER_MAGENTA };

/* WP-24: GL state apply_state shadows was changed by someone else (it sets all of it again at the next draw) */
#define GS_DIRTY(g) ((g)->gs_valid = 0)

/* the host viewport over the bound target again after a pass of another size (WP-28: tw x th are the guest pixels of
 * what is drawn, vx x vy host pixels each at an MSAA view's stand-in) */
static void viewport_restore(xenos_gl *g)
{
    g->glViewport(0, 0, g->tw * g->vx * g->scale, g->th * g->vy * g->scale);
}

/* the attachments of the bound draw framebuffer about to be overwritten whole: colour draw buffers in `colour` (bit n:
 * draw buffer n, which is GL_COLOR_ATTACHMENTn in every framebuffer here), the depth / stencil one with `ds` */
static void tiler_discard(xenos_gl *g, unsigned colour, int ds)
{
    GS_DIRTY(g); /* WP-24: GL state apply_state shadows changes here */
    if (g->tiler == TILER_OFF || (!colour && !ds))
        return;
    if (g->tiler == TILER_MAGENTA) {
        static const GLfloat magenta[4] = {1, 0, 1, 1};
        g->glDisable(GL_SCISSOR_TEST);
        g->glColorMask(1, 1, 1, 1);
        for (int i = 0; i < 4; i++)
            if ((colour >> i) & 1)
                g->glClearBufferfv(0x1800 /* GL_COLOR */, i, magenta);
        if (ds) {
            g->glDepthMask(1);
            g->glStencilMaskSeparate(GL_FRONT_AND_BACK, 0xFF);
            g->glClearBufferfi(GL_DEPTH_STENCIL, 0, 0.0f, 0x5A);
        }
    } else {
        GLenum att[5];
        int n = 0;
        for (int i = 0; i < 4; i++)
            if ((colour >> i) & 1)
                att[n++] = GL_COLOR_ATTACHMENT0 + (GLenum)i;
        if (ds)
            att[n++] = GL_DEPTH_STENCIL_ATTACHMENT;
        g->glInvalidateFramebuffer(GL_DRAW_FRAMEBUFFER, n, att);
    }
    g->st.tiler_invalidated++;
}

/* the GLES entry points: from libGLESv2 or eglGetProcAddress; 0 when one is missing */
static int load_gl(xenos_gl *g, char *why, size_t why_n)
{
    static const char *const egl_names[] = {"libEGL.so.1", "libEGL.so", NULL};
    static const char *const gles_names[] = {"libGLESv2.so.2", "libGLESv3.so", "libGLESv2.so", NULL};
    const char *angle = getenv("X360_ANGLE_DIR");
    if (angle && *angle) { /* a directory holding ANGLE's libEGL / libGLESv2 (the Mac: the Android emulator's) */
        g->gles_lib = open_in(angle, "libGLESv2");
        g->egl_lib = open_in(angle, "libEGL");
        g->angle = 1;
    } else {
        g->egl_lib = open_first(egl_names);
        g->gles_lib = open_first(gles_names);
    }
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
    count_passes(g);
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
    count_passes_maski(g);
    fprintf(g->log, "xenos: per-target blend and colour mask %s\n",
            g->glEnablei ? "on" : "off (render target 0's state for all)");
    /* RECOMP_X360_FLOAT_BLEND=0 behaves as a driver without the extension (tests) */
    const char *fb = getenv("RECOMP_X360_FLOAT_BLEND");
    g->float_blend = has_ext(g, "GL_EXT_float_blend") && !(fb && !strcmp(fb, "0"));
}

/* WP-19: GPU time per frame with EXT_disjoint_timer_query (its EXT entry points, else GLES 3.0's for all but the
 * 64-bit result); RECOMP_X360_GPU_TIMER=0 leaves it off. Most desktop drivers behind ANGLE have no such extension. */
static void gpu_timer_init(xenos_gl *g)
{
    const char *env = getenv("RECOMP_X360_GPU_TIMER");
    g->gpu_timer = 0;
    if ((env && !strcmp(env, "0")) || !has_ext(g, "GL_EXT_disjoint_timer_query"))
        return;
#define TSYM(field, name)                                                                                              \
    if (!(*(void **)&g->field = gl_proc(g, name "EXT")))                                                               \
        *(void **)&g->field = gl_proc(g, name)
    TSYM(glGenQueriesT, "glGenQueries");
    TSYM(glDeleteQueriesT, "glDeleteQueries");
    TSYM(glBeginQueryT, "glBeginQuery");
    TSYM(glEndQueryT, "glEndQuery");
    TSYM(glGetQueryObjectuivT, "glGetQueryObjectuiv");
#undef TSYM
    *(void **)&g->glGetQueryObjectui64vT = gl_proc(g, "glGetQueryObjectui64vEXT");
    if (!g->glGenQueriesT || !g->glDeleteQueriesT || !g->glBeginQueryT || !g->glEndQueryT ||
        !g->glGetQueryObjectuivT || !g->glGetQueryObjectui64vT)
        return;
    g->glGenQueriesT(GPU_TIMER_QUERIES, g->tq);
    g->tq_head = g->tq_n = 0, g->tq_active = 0;
    GLint disjoint;
    g->glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint); /* clears the flag */
    g->gpu_timer = 1;
}

/* At each swap: the running query ends, those whose result is there are read (dropped when the GPU's timing was
 * disjoint since the last look: a clock change, a context loss), the next starts while one is free. Never waits. */
static void gpu_timer_frame(xenos_gl *g)
{
    if (!g->gpu_timer)
        return;
    if (g->tq_active)
        g->glEndQueryT(GL_TIME_ELAPSED_EXT), g->tq_active = 0, g->tq_n++;
    GLint disjoint = 0;
    g->glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
    while (g->tq_n) {
        GLuint q = g->tq[g->tq_head], avail = 0;
        g->glGetQueryObjectuivT(q, GL_QUERY_RESULT_AVAILABLE, &avail);
        if (!avail)
            break;
        uint64_t ns = 0;
        g->glGetQueryObjectui64vT(q, GL_QUERY_RESULT, &ns);
        if (!disjoint)
            g->st.gpu_time_ns += ns, g->st.gpu_time_frames++;
        g->tq_head = (g->tq_head + 1) % GPU_TIMER_QUERIES, g->tq_n--;
    }
    if (g->tq_n < GPU_TIMER_QUERIES)
        g->glBeginQueryT(GL_TIME_ELAPSED_EXT, g->tq[(g->tq_head + g->tq_n) % GPU_TIMER_QUERIES]), g->tq_active = 1;
}

static void rects_selftest(xenos_gl *g);
static void rect_line(xenos_gl *g, FILE *f);

static int setup(xenos_gl *g, char *why, size_t why_n)
{
    g->max_tex = 4096;
    g->glGetIntegerv(GL_MAX_TEXTURE_SIZE, &g->max_tex);
    g->glGetIntegerv(0x821B /* GL_MAJOR_VERSION */, &g->gl_major);
    g->glGetIntegerv(0x821C /* GL_MINOR_VERSION */, &g->gl_minor);
    texture_options(g);
    memexport_caps(g);
    draw_buffers_caps(g);
    gpu_timer_init(g);
    fprintf(g->log, "xenos: GPU timer %s\n", g->gpu_timer ? "on (EXT_disjoint_timer_query)" : "off");
    const char *msaa_env = getenv("RECOMP_X360_MSAA");
    g->msaa_on = !(msaa_env && !strcmp(msaa_env, "0"));
    const char *resolve_env = getenv("RECOMP_X360_RESOLVE"); /* eager (default) | lazy (WP-2) */
    g->lazy = resolve_env && !strcmp(resolve_env, "lazy");
    fprintf(stderr, "xenos: resolves %s\n", g->lazy ? "lazy (RECOMP_X360_RESOLVE=lazy)" : "eager");
    const char *noop_env = getenv("RECOMP_X360_NOOP_DRAWS"); /* skip (default) | keep (WP-9) */
    g->keep_noop = noop_env && !strcmp(noop_env, "keep");
    /* WP-10: a rectangle list's captured vertices are read by the second pass as a uniform block of RECT_CHUNK
     * rectangles, bound at a multiple of its size in the capture buffer */
    /* cpu (default: read back, the corner on the CPU) | gpu. The GPU path lost clears at random on ANGLE/Vulkan
     * (SwiftShader) in the 2026-11-21 runs, cause unknown: opt-in until checked on devices */
    const char *rects_env = getenv("RECOMP_X360_RECTS");
    GLint ublock = 0, ualign = 0;
    g->glGetIntegerv(0x8A30 /* GL_MAX_UNIFORM_BLOCK_SIZE */, &ublock);
    g->glGetIntegerv(0x8A34 /* GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT */, &ualign);
    int rects_want_gpu = rects_env && !strcmp(rects_env, "gpu");
    g->rects_gpu = rects_want_gpu && ublock >= RECT_CHUNK_BYTES && ualign > 0 &&
                   RECT_CHUNK_BYTES % ualign == 0;
    if (!g->rects_gpu && rects_want_gpu)
        fprintf(g->log, "xenos_gl: rectangle lists read back (uniform block size %d, offset alignment %d)\n", ublock,
                ualign);
    const char *tiler_env = getenv("RECOMP_X360_TILER"); /* on (default) | 0 | magenta (WP-17) */
    g->tiler = !tiler_env || !*tiler_env ? TILER_ON : !strcmp(tiler_env, "0") ? TILER_OFF
             : !strcmp(tiler_env, "magenta")                       ? TILER_MAGENTA
                                                                   : TILER_ON;
    if (g->tiler != TILER_ON)
        fprintf(stderr, "xenos: tiler passes %s (RECOMP_X360_TILER=%s)\n",
                g->tiler == TILER_OFF ? "as before" : "magenta where invalidated", tiler_env);
    /* untouched (default since WP-28: what the GPU does) | stamped (WP-17's first default) */
    const char *ud_env = getenv("RECOMP_X360_UNUSED_DEPTH");
    g->depth_untouched = !(ud_env && !strcmp(ud_env, "stamped"));
    if (!g->depth_untouched)
        fprintf(stderr, "xenos: draws that do not use depth stamp the depth surface written "
                        "(RECOMP_X360_UNUSED_DEPTH=stamped)\n");
    const char *smp_env = getenv("RECOMP_X360_SAMPLERS"); /* on (default) | 0 (WP-24) */
    g->samplers_on = !(smp_env && !strcmp(smp_env, "0"));
    if (!g->samplers_on)
        fprintf(stderr, "xenos: texture sampling state as texture parameters (RECOMP_X360_SAMPLERS=0)\n");
    const char *ct_env = getenv("RECOMP_X360_COPY_TEXTURES"); /* direct (default) | blit (WP-18) */
    g->copy_direct = !(ct_env && !strcmp(ct_env, "blit"));
    if (!g->copy_direct)
        fprintf(stderr, "xenos: resolve pictures copied for each texture (RECOMP_X360_COPY_TEXTURES=blit)\n");
    const char *dp_env = getenv("RECOMP_X360_DIRECT_PRESENT"); /* 0 (default) | 1 (WP-18) */
    g->direct_present = g->copy_direct && dp_env && !strcmp(dp_env, "1");
    if (g->direct_present)
        fprintf(stderr, "xenos: the front buffer's picture shown in place (RECOMP_X360_DIRECT_PRESENT=1)\n");
    const char *hash_env = getenv("RECOMP_X360_COPY_HASH"); /* sparse (default) | full (WP-12) */
    g->copy_hash_full = hash_env && !strcmp(hash_env, "full");
    if (g->copy_hash_full)
        fprintf(stderr, "xenos: resolve copies hashed whole at every check (RECOMP_X360_COPY_HASH=full)\n");
    while (g->scale > 1 && (g->width * g->scale > g->max_tex || g->height * g->scale > g->max_tex))
        g->scale--;
    g->glGenFramebuffers(1, &g->fbo);
    g->glGenFramebuffers(1, &g->rfbo);
    g->glGenFramebuffers(1, &g->tfbo_draw);
    g->glGenVertexArrays(1, &g->vao);
    bind_vao(g, g->vao);
    g->glGenBuffers(1, &g->vbo);
    g->glGenBuffers(1, &g->ibo);
    g->glGenBuffers(2, g->ubo);
    for (int stage = 0; stage < 2; stage++) { /* bound once here; upload_constants fills them */
        g->glBindBuffer(GL_UNIFORM_BUFFER, g->ubo[stage]);
        g->glBufferData(GL_UNIFORM_BUFFER, UBO_BLOCK, NULL, GL_STREAM_DRAW);
        g->glBindBufferBase(GL_UNIFORM_BUFFER, (GLuint)stage, g->ubo[stage]);
        g->ubo_valid[stage] = 0;
    }
    g->glGenBuffers(1, &g->tfbo);
    g->frame = calloc((size_t)g->width * g->height, 4);
    for (int i = 0; i < 4; i++)
        g->cur[i] = -1;
    g->cur_ds = -1;
    g->tw = g->width, g->th = g->height;
    g->vx = g->vy = 1;
    g->ready = 1;
    if (!g->frame) {
        fail(why, why_n, "out of memory");
        return 0;
    }
    rects_selftest(g);
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
    if (g->angle) { /* ANGLE: its default display, used without a window (no surfaceless platform there) */
        EGLDisplay (*get_display)(void *) = (EGLDisplay (*)(void *))dlsym(g->egl_lib, "eglGetDisplay");
        g->dpy = get_display ? get_display(NULL) : NULL;
    } else
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
        if (g->gpu_timer) {
            if (g->tq_active)
                g->glEndQueryT(GL_TIME_ELAPSED_EXT);
            g->glDeleteQueriesT(GPU_TIMER_QUERIES, g->tq);
        }
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
            if (g->textures[i].tex && !g->textures[i].direct)
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
        for (int i = 0; i < MAX_VB_CACHE; i++)
            if (g->vbc[i].tex)
                g->glDeleteTextures(1, &g->vbc[i].tex);
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
        if (g->dsprog)
            g->glDeleteProgram(g->dsprog);
        if (g->aprog)
            g->glDeleteProgram(g->aprog);
        if (g->frame_tex)
            g->glDeleteTextures(1, &g->frame_tex);
        for (int i = 0; i < g->nsampler; i++)
            g->glDeleteSamplers(1, &g->sampler[i].id);
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
    for (int i = 0; i < (int)(sizeof g->scratch / sizeof *g->scratch); i++)
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
    if (g->frame_copy) {
        if (width)
            *width = g->show_w;
        if (height)
            *height = g->show_h;
        return g->copies[g->frame_copy - 1].tex;
    }
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
    GS_DIRTY(g); /* WP-24: GL state apply_state shadows changes here */
    /* after a state load: the eDRAM is not in a state (as the GameCube's EFB): surfaces start clear, copies go */
    g->reset_pending = 0;
    for (int i = 0; i < MAX_SURFACES; i++)
        if (g->surf[i].tex) {
            g->glDeleteTextures(1, &g->surf[i].tex);
            if (g->surf[i].rb)
                g->glDeleteRenderbuffers(1, &g->surf[i].rb);
            memset(&g->surf[i], 0, sizeof g->surf[i]);
        }
    /* pending resolves go unwritten: the memory is the state's now */
    for (int i = 0; i < MAX_COPIES; i++)
        if (g->copies[i].tex) {
            g->glDeleteTextures(1, &g->copies[i].tex);
            memset(&g->copies[i], 0, sizeof g->copies[i]);
        }
    g->npending = 0;
    g->frame_copy = 0; /* (frame_tex keeps what it had: the window shows black until the next swap otherwise) */
    for (int i = 0; i < MAX_TEXTURES; i++) {
        if (g->textures[i].direct) { /* the copy's texture, deleted above */
            tex_unlink(g, &g->textures[i]);
            g->textures[i].tex = 0, g->textures[i].direct = 0;
        }
        g->textures[i].copy = 0, g->textures[i].hash = 0, g->textures[i].checked = 0, g->textures[i].gen = 0;
    }
    for (int i = 0; i < 4; i++)
        g->cur[i] = -1;
    g->cur_ds = -1;
    g->fbo_checked = 0;
    g->nresolved = 0;
    g->epoch++;
    g->ubo_valid[0] = g->ubo_valid[1] = 0;
    g->crc_ok[0] = g->crc_ok[1] = 0; /* the state's microcode, with its own load counts */
}

void xenos_gl_set_frames_dir(xenos_gl *g, const char *dir, unsigned max_frames)
{
    snprintf(g->frames, sizeof g->frames, "%s", dir ? dir : "");
    g->max_frames = max_frames;
}

void xenos_gl_set_log(xenos_gl *g, void *file)
{
    g->log = file ? file : stderr;
    if (g->log != stderr && g->ready)
        rect_line(g, g->log); /* (WP-29: setup's self-test, in this log too) */
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
    s->passes = pc.passes, s->hashed_bytes = hashed_bytes;
    s->loads = pc.loads, s->gpu_timer = g->gpu_timer;
    s->rect_selftest = g->rects_captured;
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

/* Rectangle lists: the vertex shader runs once with transform feedback (gl_Position and 14 interpolators, 60
 * components: GLES 3.0 guarantees 64 interleaved components, but on Apple's GL (ANGLE on the Mac) a captured vertex of
 * exactly 64 components, 256 bytes, is not advanced and every vertex lands at offset 0; xe_i14 is 0 in rect lists),
 * the fourth corner of each rectangle is made on the CPU, and the result is
 * drawn as triangles through a pass-through vertex shader with the title's pixel shader. */
#define RECT_VARYINGS 15 /* gl_Position + xe_i0..13: 60 components (64 = 256-byte vertices are not captured on Apple GL) */

static const char RECT_VS[] =
    "#version 300 es\n"
    "in vec4 xe_rp, xe_r0, xe_r1, xe_r2, xe_r3, xe_r4, xe_r5, xe_r6, xe_r7, xe_r8, xe_r9, xe_r10, xe_r11, xe_r12, "
    "xe_r13;\n"
    "out vec4 xe_i0, xe_i1, xe_i2, xe_i3, xe_i4, xe_i5, xe_i6, xe_i7, xe_i8, xe_i9, xe_i10, xe_i11, xe_i12, xe_i13, "
    "xe_i14, xe_i15;\n"
    "void main() { xe_i0 = xe_r0; xe_i1 = xe_r1; xe_i2 = xe_r2; xe_i3 = xe_r3; xe_i4 = xe_r4; xe_i5 = xe_r5; "
    "xe_i6 = xe_r6; xe_i7 = xe_r7; xe_i8 = xe_r8; xe_i9 = xe_r9; xe_i10 = xe_r10; xe_i11 = xe_r11; xe_i12 = xe_r12; "
    "xe_i13 = xe_r13; xe_i14 = vec4(0.0); xe_i15 = vec4(0.0); gl_Position = xe_rp; }\n";
/* WP-10: the same pass-through without the read-back: the captured buffer of RECT_CHUNK rectangles is bound as the
 * uniform block XeRect (binding 2) and vertex gl_VertexID makes corner k of rectangle gl_VertexID / 6 as draw_rects'
 * CPU path does: c, a, b, a, b, a + b - c with c the vertex opposite the longest edge after the divide by w (the first
 * of equal edges: the same strictly-greater test from -1) */
static const char RECT_VS_GPU[] =
    "#version 300 es\n"
    "layout(std140) uniform XeRect { highp vec4 xe_rv[720]; };\n"
    "out vec4 xe_i0, xe_i1, xe_i2, xe_i3, xe_i4, xe_i5, xe_i6, xe_i7, xe_i8, xe_i9, xe_i10, xe_i11, xe_i12, xe_i13, "
    "xe_i14, xe_i15;\n"
    "void main() {\n"
    "    int r = gl_VertexID / 6, k = gl_VertexID - 6 * r, b0 = 45 * r;\n"
    "    vec2 s[3];\n"
    "    for (int j = 0; j < 3; j++) {\n"
    "        vec4 q = xe_rv[b0 + 15 * j];\n"
    "        float w = q.w != 0.0 ? q.w : 1.0;\n"
    "        s[j] = vec2(q.x / w, q.y / w);\n"
    "    }\n"
    "    int corner = 0;\n"
    "    float best = -1.0;\n"
    "    for (int j = 0; j < 3; j++) {\n"
    "        vec2 e = s[(j + 1) % 3] - s[(j + 2) % 3];\n"
    "        float len = e.x * e.x + e.y * e.y;\n"
    "        if (len > best) { best = len; corner = j; }\n"
    "    }\n"
    "    int c = b0 + 15 * corner, a = b0 + 15 * ((corner + 1) % 3), b = b0 + 15 * ((corner + 2) % 3);\n"
    "    int o = k == 0 ? c : (k == 1 || k == 3) ? a : b;\n"
    "    bool d = k == 5;\n"
    "#define XE_RC(i) (d ? (xe_rv[a + i] + xe_rv[b + i]) - xe_rv[c + i] : xe_rv[o + i])\n"
    "    gl_Position = XE_RC(0); xe_i0 = XE_RC(1); xe_i1 = XE_RC(2); xe_i2 = XE_RC(3); xe_i3 = XE_RC(4);\n"
    "    xe_i4 = XE_RC(5); xe_i5 = XE_RC(6); xe_i6 = XE_RC(7); xe_i7 = XE_RC(8); xe_i8 = XE_RC(9);\n"
    "    xe_i9 = XE_RC(10); xe_i10 = XE_RC(11); xe_i11 = XE_RC(12); xe_i12 = XE_RC(13); xe_i13 = XE_RC(14);\n"
    "    xe_i14 = vec4(0.0); xe_i15 = vec4(0.0);\n"
    "}\n";
_Static_assert(RECT_CHUNK_BYTES == 720 * 16 && RECT_VARYINGS == 15, "RECT_VS_GPU's block is RECT_CHUNK rectangles");
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
 * with its pixel shader (rectangle lists); 3: the same with the pass-through that reads the capture as a uniform block
 * (WP-10, RECT_VS_GPU) */
static uint32_t stage_crc(xenos_gl *g, const xenos_cp *cp, int k)
{
    const xenos_shader *sh = &cp->shader[k];
    if (g->crc_cp != cp)
        g->crc_cp = cp, g->crc_ok[0] = g->crc_ok[1] = 0;
    if (!g->crc_ok[k] || g->crc_loads[k] != sh->loads || g->crc_words[k] != sh->words) {
        g->crc_val[k] = crc32_words(sh->code, sh->words);
        g->crc_loads[k] = sh->loads, g->crc_words[k] = sh->words, g->crc_ok[k] = 1;
    }
    return g->crc_val[k];
}

static program *get_program(xenos_gl *g, const xenos_cp *cp, int kind)
{
    const xenos_shader *vs = &cp->shader[0], *ps = &cp->shader[1];
    if (!vs->words || !ps->words)
        return NULL;
    uint32_t vc = kind >= 2 ? 0 : stage_crc(g, cp, 0);
    uint32_t pc = kind == 1 ? 0 : stage_crc(g, cp, 1);
    if (g->prog_buckets)
        for (program *q = g->prog_hash[prog_bucket(vc, pc, kind, g->prog_buckets)]; q; q = q->next)
            if (q->vs_crc == vc && q->ps_crc == pc && q->kind == kind)
                return q->ok ? q : NULL;
    program *p = add_program(g, vc, pc, kind);
    if (!p)
        return NULL;
    char path[600], name[40];
    char *vsrc = NULL, *psrc = NULL;
    if (kind < 2) {
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
    char rect_vs[sizeof RECT_VS_GPU > sizeof RECT_VS ? sizeof RECT_VS_GPU : sizeof RECT_VS];
    char capture_fs[sizeof CAPTURE_FS];
    if (kind == 3)
        memcpy(rect_vs, RECT_VS_GPU, sizeof RECT_VS_GPU);
    else
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
    const char *vtext = kind >= 2 ? rect_vs : vsrc, *ftext = kind == 1 ? capture_fs : psrc;
    if (vtext && ftext) {
        snprintf(name, sizeof name, kind == 3 ? "rect pass-through (gpu)" : kind == 2 ? "rect pass-through" : "vs_%08X",
                 vc);
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
                    "xe_i7",       "xe_i8", "xe_i9", "xe_i10", "xe_i11", "xe_i12", "xe_i13"};
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
                } else if (kind == 0 || kind == 1) { /* kind 3 has no attributes: gl_VertexID */
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
                use_program(g, p->prog);
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
                b = g->glGetUniformBlockIndex(p->prog, "XeRect");
                if (b != GL_INVALID_INDEX)
                    g->glUniformBlockBinding(p->prog, b, 2); /* draw_rects binds the capture there */
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

/* A fast hash of n bytes, for "has this memory changed" checks run on every cached texture after each swap and resolve.
 * Not for anything that needs a good distribution. WP-36: 8 lanes of 8 bytes, each a chain of xor, multiply and
 * xorshift, in scalar registers (4 lanes left the multiplies' latency showing, and GCC vectorized them into SSE2
 * pmuludq triples: 5.3 GB/s; ~20 GB/s now, Warhammer hashes ~155 MB a frame). The values are only ever compared with
 * values of this same function in the same run. */
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("no-tree-vectorize")))
#endif
static uint64_t span_hash(const uint8_t *p, size_t n, uint64_t seed)
{
    hashed_bytes += n;
    const uint64_t k = 0x9E3779B97F4A7C15ull;
    uint64_t h0 = seed ^ k, h1 = seed + k, h2 = seed ^ (k >> 1), h3 = seed - k;
    uint64_t h4 = seed ^ (k >> 2), h5 = seed + (k >> 1), h6 = seed ^ (k >> 3), h7 = seed - (k >> 1);
    size_t i = 0;
#define SPAN_LANE(h, o)                                                                                               \
    do {                                                                                                              \
        uint64_t w;                                                                                                   \
        memcpy(&w, p + i + (o), 8);                                                                                   \
        h = (h ^ w) * 0xFF51AFD7ED558CCDull;                                                                          \
        h ^= h >> 29;                                                                                                 \
    } while (0)
    for (; i + 64 <= n; i += 64) {
        SPAN_LANE(h0, 0);
        SPAN_LANE(h1, 8);
        SPAN_LANE(h2, 16);
        SPAN_LANE(h3, 24);
        SPAN_LANE(h4, 32);
        SPAN_LANE(h5, 40);
        SPAN_LANE(h6, 48);
        SPAN_LANE(h7, 56);
    }
    for (; i + 8 <= n; i += 8)
        SPAN_LANE(h0, 0);
#undef SPAN_LANE
    uint64_t r = (uint64_t)n * k;
    for (; i < n; i++)
        r = (r ^ p[i]) * 0x100000001B3ull;
    const uint64_t h[8] = {h0, h1, h2, h3, h4, h5, h6, h7};
    for (int l = 0; l < 8; l++)
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

/* a 32-bit float component run with the swap mode E known at compile time: the guest's words byte-swapped straight
 * into floats (gpu_read's cp->mem path, unrolled) */
#define FETCH_F32(E)                                                                                                   \
    for (uint32_t v = 0; v < count; v++, at += step) {                                                                 \
        float *o = out + (size_t)v * ostride;                                                                          \
        o[0] = o[1] = o[2] = 0.0f;                                                                                     \
        o[3] = 1.0f;                                                                                                   \
        for (int i = 0; i < n; i++) {                                                                                  \
            uint32_t le;                                                                                               \
            memcpy(&le, cp->mem + ((at + 4 * (uint32_t)i) & 0x1FFFFFFFu), 4);                                          \
            le = gpu_swap(le, E);                                                                                      \
            memcpy(&o[i], &le, 4);                                                                                     \
        }                                                                                                              \
    }

/* WP-36: the packed formats' vertex loop with physical memory at hand, one copy per format and swap mode (both known
 * at compile time): the words byte-swapped straight from memory, the components as the generic loops below make them
 * (component, half_to_float), without gpu_read's call and switch per word. Warhammer fetches ~3.5 M attribute values
 * a frame, most of them 8_8_8_8 and 16_16. The generic loops stay for a runtime without cp->mem; xenos_vertex_fetch_test
 * checks that both give the same floats. */
/* component() of every 8-bit value for each signedness and integer flag (index 2 * signed + integer), made once by
 * component itself: 8_8_8_8 took a division per component */
static float comp8_lut[4][256];
static pthread_once_t comp8_once = PTHREAD_ONCE_INIT;
static void comp8_init(void)
{
    for (int k = 0; k < 4; k++) {
        attr t;
        memset(&t, 0, sizeof t);
        t.is_signed = (uint32_t)(k >> 1), t.is_integer = (uint32_t)(k & 1);
        for (uint32_t u = 0; u < 256; u++)
            comp8_lut[k][u] = component(u, 0, 8, &t);
    }
}

/* WP-36: the same for 16 bits (16_16, 16_16_16_16: a division per component), and half_to_float of every 16-bit
 * pattern (16_16_FLOAT, 16_16_16_16_FLOAT: up to two divisions each), made once by component / half_to_float */
static float comp16_lut[4][65536], vhalf_lut[65536];
static pthread_once_t comp16_once = PTHREAD_ONCE_INIT, vhalf_once = PTHREAD_ONCE_INIT;
static void comp16_init(void)
{
    for (int k = 0; k < 4; k++) {
        attr t;
        memset(&t, 0, sizeof t);
        t.is_signed = (uint32_t)(k >> 1), t.is_integer = (uint32_t)(k & 1);
        for (uint32_t u = 0; u < 65536; u++)
            comp16_lut[k][u] = component(u, 0, 16, &t);
    }
}
static void vhalf_init(void)
{
    for (uint32_t u = 0; u < 65536; u++)
        vhalf_lut[u] = half_to_float(u);
}

static inline __attribute__((always_inline)) void packed_vertex(uint32_t format, uint32_t w0, uint32_t w1,
                                                                const attr *a, const float *l8, float *o)
{
    switch (format) {
    case 6: /* l8: comp8_lut for the attribute's flags */
        for (int i = 0; i < 4; i++)
            o[i] = l8[(w0 >> (8 * i)) & 255];
        break;
    case 7:
        for (int i = 0; i < 3; i++)
            o[i] = component(w0, 10 * i, 10, a);
        o[3] = component(w0, 30, 2, a);
        break;
    case 16:
        o[0] = component(w0, 0, 11, a), o[1] = component(w0, 11, 11, a), o[2] = component(w0, 22, 10, a);
        o[3] = 1.0f;
        break;
    case 17:
        o[0] = component(w0, 0, 10, a), o[1] = component(w0, 10, 11, a), o[2] = component(w0, 21, 11, a);
        o[3] = 1.0f;
        break;
    case 25: /* l8: comp16_lut for the attribute's flags */
        o[0] = l8[w0 & 0xFFFF], o[1] = l8[w0 >> 16];
        o[2] = 0.0f, o[3] = 1.0f;
        break;
    case 26:
        o[0] = l8[w0 & 0xFFFF], o[1] = l8[w0 >> 16];
        o[2] = l8[w1 & 0xFFFF], o[3] = l8[w1 >> 16];
        break;
    case 31: /* l8: vhalf_lut */
        o[0] = l8[w0 & 0xFFFF], o[1] = l8[w0 >> 16];
        o[2] = 0.0f, o[3] = 1.0f;
        break;
    default: /* 32 */
        o[0] = l8[w0 & 0xFFFF], o[1] = l8[w0 >> 16];
        o[2] = l8[w1 & 0xFFFF], o[3] = l8[w1 >> 16];
        break;
    }
}

static inline __attribute__((always_inline)) void packed_loop(const uint8_t *mem, uint32_t format, uint32_t endian,
                                                              uint32_t at, uint32_t step, uint32_t count, float *out,
                                                              size_t ostride, const attr *a, const float *l8)
{
    const int two = format == 26 || format == 32;
    for (uint32_t v = 0; v < count; v++, at += step) {
        uint32_t w0, w1 = 0;
        memcpy(&w0, mem + (at & 0x1FFFFFFFu), 4);
        w0 = gpu_swap(w0, endian);
        if (two) {
            memcpy(&w1, mem + ((at + 4) & 0x1FFFFFFFu), 4);
            w1 = gpu_swap(w1, endian);
        }
        packed_vertex(format, w0, w1, a, l8, out + (size_t)v * ostride);
    }
}

/* 1 when the format is one packed_loop takes (and it ran), else 0 */
static int fetch_packed_mem(const uint8_t *mem, uint32_t format, uint32_t endian, uint32_t at, uint32_t step,
                            uint32_t count, float *out, size_t ostride, const attr *a)
{
#define PACKED_E(F)                                                                                                    \
    switch (endian & 3) {                                                                                              \
    case 0: packed_loop(mem, F, 0, at, step, count, out, ostride, a, l8); break;                                       \
    case 1: packed_loop(mem, F, 1, at, step, count, out, ostride, a, l8); break;                                       \
    case 2: packed_loop(mem, F, 2, at, step, count, out, ostride, a, l8); break;                                       \
    default: packed_loop(mem, F, 3, at, step, count, out, ostride, a, l8); break;                                      \
    }
    const float *l8 = NULL;
    if (format == 6) {
        pthread_once(&comp8_once, comp8_init);
        l8 = comp8_lut[2 * (a->is_signed != 0) + (a->is_integer != 0)];
    } else if (format == 25 || format == 26) {
        pthread_once(&comp16_once, comp16_init);
        l8 = comp16_lut[2 * (a->is_signed != 0) + (a->is_integer != 0)];
    } else if (format == 31 || format == 32) {
        pthread_once(&vhalf_once, vhalf_init);
        l8 = vhalf_lut;
    }
    switch (format) {
    case 6: PACKED_E(6) return 1;
    case 7: PACKED_E(7) return 1;
    case 16: PACKED_E(16) return 1;
    case 17: PACKED_E(17) return 1;
    case 25: PACKED_E(25) return 1;
    case 26: PACKED_E(26) return 1;
    case 31: PACKED_E(31) return 1;
    case 32: PACKED_E(32) return 1;
    default: return 0;
    }
#undef PACKED_E
}

/* one attribute of `count` vertices from index `first`, each as a vec4 `ostride` floats after the previous one
 * (Xenia's spirv_shader_translator_fetch.cc gives the packings: X in the low bits); 0 when the format is not
 * handled. The format is chosen once, then one loop runs over the vertices. */
static int fetch_vertices(const xenos_cp *cp, uint32_t base, uint32_t endian, const attr *a, uint32_t first,
                          uint32_t count, float *out, size_t ostride)
{
    uint32_t step = 4 * a->stride, at = base + 4 * (a->offset + first * a->stride);
    float *o;
    if (cp->mem && fetch_packed_mem(cp->mem, a->format, endian, at, step, count, out, ostride, a)) {
        if (a->format == 31 || a->format == 32)
            return 1; /* exp_adjust does not apply to floats */
        goto adjust;
    }
    switch (a->format) {
    case 36: /* 32_FLOAT */
    case 37: /* 32_32_FLOAT */
    case 57: /* 32_32_32_FLOAT */
    case 38: { /* 32_32_32_32_FLOAT */
        int n = a->format == 36 ? 1 : a->format == 37 ? 2 : a->format == 57 ? 3 : 4;
        if (cp->mem) {
            switch (endian & 3) {
            case 0: FETCH_F32(0) break;
            case 1: FETCH_F32(1) break;
            case 2: FETCH_F32(2) break;
            default: FETCH_F32(3) break;
            }
        } else {
            for (uint32_t v = 0; v < count; v++, at += step) {
                o = out + (size_t)v * ostride;
                o[0] = o[1] = o[2] = 0.0f;
                o[3] = 1.0f;
                for (int i = 0; i < n; i++)
                    o[i] = as_float(gpu_read(cp, at + 4 * (uint32_t)i, endian));
            }
        }
        return 1; /* exp_adjust does not apply to floats */
    }
    case 6: /* 8_8_8_8 */
        for (uint32_t v = 0; v < count; v++, at += step) {
            uint32_t w0 = gpu_read(cp, at, endian);
            o = out + (size_t)v * ostride;
            for (int i = 0; i < 4; i++)
                o[i] = component(w0, 8 * i, 8, a);
        }
        break;
    case 7: /* 2_10_10_10 */
        for (uint32_t v = 0; v < count; v++, at += step) {
            uint32_t w0 = gpu_read(cp, at, endian);
            o = out + (size_t)v * ostride;
            for (int i = 0; i < 3; i++)
                o[i] = component(w0, 10 * i, 10, a);
            o[3] = component(w0, 30, 2, a);
        }
        break;
    case 16: /* 10_11_11: 11, 11, 10 bits from X */
        for (uint32_t v = 0; v < count; v++, at += step) {
            uint32_t w0 = gpu_read(cp, at, endian);
            o = out + (size_t)v * ostride;
            o[0] = component(w0, 0, 11, a), o[1] = component(w0, 11, 11, a), o[2] = component(w0, 22, 10, a);
            o[3] = 1.0f;
        }
        break;
    case 17: /* 11_11_10: 10, 11, 11 bits from X */
        for (uint32_t v = 0; v < count; v++, at += step) {
            uint32_t w0 = gpu_read(cp, at, endian);
            o = out + (size_t)v * ostride;
            o[0] = component(w0, 0, 10, a), o[1] = component(w0, 10, 11, a), o[2] = component(w0, 21, 11, a);
            o[3] = 1.0f;
        }
        break;
    case 25: /* 16_16 */
        for (uint32_t v = 0; v < count; v++, at += step) {
            uint32_t w0 = gpu_read(cp, at, endian);
            o = out + (size_t)v * ostride;
            o[0] = component(w0, 0, 16, a), o[1] = component(w0, 16, 16, a);
            o[2] = 0.0f, o[3] = 1.0f;
        }
        break;
    case 26: /* 16_16_16_16 */
        for (uint32_t v = 0; v < count; v++, at += step) {
            uint32_t w0 = gpu_read(cp, at, endian), w1 = gpu_read(cp, at + 4, endian);
            o = out + (size_t)v * ostride;
            o[0] = component(w0, 0, 16, a), o[1] = component(w0, 16, 16, a);
            o[2] = component(w1, 0, 16, a), o[3] = component(w1, 16, 16, a);
        }
        break;
    case 31: /* 16_16_FLOAT */
        for (uint32_t v = 0; v < count; v++, at += step) {
            uint32_t w0 = gpu_read(cp, at, endian);
            o = out + (size_t)v * ostride;
            o[0] = half_to_float(w0 & 0xFFFF), o[1] = half_to_float(w0 >> 16);
            o[2] = 0.0f, o[3] = 1.0f;
        }
        return 1; /* exp_adjust does not apply to floats */
    case 32: /* 16_16_16_16_FLOAT */
        for (uint32_t v = 0; v < count; v++, at += step) {
            uint32_t w0 = gpu_read(cp, at, endian), w1 = gpu_read(cp, at + 4, endian);
            o = out + (size_t)v * ostride;
            o[0] = half_to_float(w0 & 0xFFFF), o[1] = half_to_float(w0 >> 16);
            o[2] = half_to_float(w1 & 0xFFFF), o[3] = half_to_float(w1 >> 16);
        }
        return 1;
    case 33: /* 32 */
    case 34: /* 32_32 */
    case 35: { /* 32_32_32_32 */
        int n = a->format == 33 ? 1 : a->format == 34 ? 2 : 4;
        for (uint32_t v = 0; v < count; v++, at += step) {
            o = out + (size_t)v * ostride;
            o[0] = o[1] = o[2] = 0.0f;
            o[3] = 1.0f;
            for (int i = 0; i < n; i++)
                o[i] = component(gpu_read(cp, at + 4 * (uint32_t)i, endian), 0, 32, a);
        }
        break;
    }
    default:
        return 0;
    }
adjust:
    if (a->exp_adjust) {
        float k = ldexpf(1.0f, a->exp_adjust); /* -32..31: no shift of 32 */
        for (uint32_t v = 0; v < count; v++)
            for (int i = 0; i < 4; i++)
                out[(size_t)v * ostride + i] *= k;
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
    GS_DIRTY(g); /* WP-24: GL state apply_state shadows changes here */
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
static void pclear_flush(xenos_gl *g, const surface *cs);
static void pclear_bound(xenos_gl *g, int si, int slot);
static void alias_clear_to(xenos_gl *g, const surface *d, int y0, int y1, GLuint rb);
static void deferred_clears_flush(xenos_gl *g, int si, int use_ms);

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
    GS_DIRTY(g); /* WP-24: GL state apply_state shadows changes here */
    if (!s->rb || !s->ms_dirty)
        return;
    pclear_flush(g, s); /* on rb: before rb is read */
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
            GS_DIRTY(g);
            pclear_flush(g, s);
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
            g->fbo_checked = 0;
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
            g->cur[k] = -1, g->fbo_checked = 0;
    if (g->cur_ds == victim)
        g->cur_ds = -1, g->fbo_checked = 0;
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
 * colour outputs in `outputs` (a rectangle list: `rects`); sets tw / th, vx / vy and the host viewport. 0 when no
 * complete framebuffer can be had. */
static int bind_targets(xenos_gl *g, const xenos_cp *cp, unsigned outputs, int rects)
{
    static const uint32_t info_reg[4] = {0x2001, 0x2003, 0x2004, 0x2005};
    uint32_t si = cp->regs[0x2000], pitch = si & 0x3FFF, msaa = (si >> 16) & 3, mode = cp->regs[0x2208] & 7;
    uint32_t br = cp->regs[0x2082];
    int need_h = (int)((br >> 16) & 0x7FFF);
    if ((cp->regs[0x2205] >> 16) & 1) /* the window offset moves the scissor too */
        need_h += (int)((int32_t)(cp->regs[0x2080] << 1) >> 17);
    /* WP-28: a 2x / 4x view (pitch P) of tiles that single-sampled surfaces of the same footprint (pitch 2P / P, the
     * same base, kind and format) already stand for: Direct3D clears a 1x picture through a 4x view of its tiles, all
     * samples of a pixel written at once. Kept apart, the view is a second picture the clear never leaves (Adventure
     * Time: the depth clear lost, stale rows cleared to 1.0 instead). Drawn into the 1x surfaces, each pixel of the
     * view vx x vy of theirs: the same eDRAM samples. Only rectangle lists (the clears), only colour target 0 and
     * depth, and only when every one of them has its stand-in: real multisampled drawing keeps its own surfaces. */
    int fx = 1, fy = 1;
    if (msaa && rects && !(outputs & ~1u)) {
        uint32_t c0 = cp->regs[0x2001], dinfo0 = cp->regs[0x2002], vpitch = pitch * (msaa == 2 ? 2u : 1u);
        int have_c = mode == 5, have_d = 0;
        for (int i = 0; i < MAX_SURFACES; i++) {
            const surface *o = &g->surf[i];
            if (!o->tex || o->msaa || o->pitch != vpitch)
                continue;
            if (!o->depth && o->base == (c0 & 0xFFF) && o->fmt == ((c0 >> 16) & 15))
                have_c = 1;
            if (o->depth && o->base == (dinfo0 & 0xFFF) && o->fmt == ((dinfo0 >> 16) & 1))
                have_d = 1;
        }
        if (have_c && have_d) {
            fx = msaa == 2 ? 2 : 1, fy = 2;
            pitch = vpitch, msaa = 0;
            need_h = need_h > 0 && need_h < 4096 ? need_h * fy : need_h;
            g->st.msaa_views++;
        }
    }
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
        /* WP-17: nor, on a tiler, any depth attachment it would start a pass with (dsa, what is attached): the depth
         * buffer would be loaded and stored for nothing. Kept when the framebuffer already has these colour targets
         * and this depth buffer: no new pass for one draw. The surface is still synced and stamped written as if
         * attached with RECOMP_X360_UNUSED_DEPTH=stamped; by default (WP-28) it is left alone as the GPU would, which
         * changes what overlapping depth surfaces carry over (Adventure Time: the top of the left tile is drawn). */
        int dsa = ds;
        if (ds >= 0 && (g->tiler != TILER_OFF || g->depth_untouched) && !(cp->regs[0x2200] & 7)) {
            int same = g->cur_ds == ds;
            for (int i = 0; i < 4; i++)
                same &= want[i] == g->cur[i];
            if (want[0] >= 0 && !same) /* (colour target 0: a framebuffer with an attachment, the viewport's) */
                dsa = -1, ds = g->depth_untouched ? -1 : ds;
        }
        /* what another format or position left in these tiles since (not what is bound together with them) */
        int together[5] = {want[0], want[1], want[2], want[3], ds};
        g->ndclear = 0;
        g->defer_clears = 1;
        for (int i = 0; i < 5; i++)
            if (together[i] >= 0)
                sync_surface(g, together[i], together, 5);
        g->defer_clears = 0;
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
        int changed = reattach || g->cur_ds != dsa;
        GLenum bufs[4];
        for (int i = 0; i < 4; i++) {
            if (g->cur[i] != want[i] || reattach) {
                changed = 1;
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
        if (g->cur_ds != dsa || reattach) {
            if (dsa < 0)
                g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
            else if (use_ms)
                g->glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g->surf[dsa].rb);
            else
                g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, g->surf[dsa].tex, 0);
        }
        g->cur_ds = dsa;
        g->glDrawBuffers(4, bufs);
        /* completeness depends on the attachments only (GLES 3.0: not on the draw buffers): asked again only when one
         * changed here or elsewhere (a surface deleted or grown, a state loaded) since it was last found complete */
        if (changed || !g->fbo_checked)
            g->fbo_checked = g->glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        if (g->fbo_checked && g->tiler != TILER_OFF) { /* WP-17: pending clears first (they came before) */
            for (int i = 0; i < 4; i++)
                if (want[i] >= 0)
                    pclear_bound(g, want[i], i);
            if (dsa >= 0)
                pclear_bound(g, dsa, -1);
        }
        if (g->fbo_checked && g->ndclear) { /* the stale rows, on the attachment each surface is bound at */
            GS_DIRTY(g);
            int k = g->scale;
            g->glEnable(GL_SCISSOR_TEST);
            for (int c = 0; c < g->ndclear; c++) {
                const surface *d = &g->surf[g->dclear[c].si];
                int slot = -1;
                for (int i = 0; i < 4; i++)
                    slot = want[i] == g->dclear[c].si ? i : slot;
                if (d->kind == 2 ? dsa != g->dclear[c].si : slot < 0) { /* not attached: through rfbo */
                    alias_clear_to(g, d, g->dclear[c].y0, g->dclear[c].y1, use_ms ? d->rb : 0);
                    g->glEnable(GL_SCISSOR_TEST);
                    continue;
                }
                g->glScissor(0, g->dclear[c].y0 * k, d->w * k, (g->dclear[c].y1 - g->dclear[c].y0) * k);
                if (g->tiler != TILER_OFF && !g->dclear[c].y0 && g->dclear[c].y1 >= d->h) {
                    /* WP-17: all of it: invalidated and cleared whole (a tiler loads nothing) */
                    tiler_discard(g, d->kind == 2 ? 0 : 1u << slot, d->kind == 2);
                    g->glDisable(GL_SCISSOR_TEST);
                }
                if (d->kind == 2) {
                    g->glDepthMask(1);
                    g->glStencilMaskSeparate(GL_FRONT_AND_BACK, 0xFF);
                    g->glClearBufferfi(GL_DEPTH_STENCIL, 0, 1.0f, 0);
                } else {
                    static const GLfloat zero[4] = {0, 0, 0, 0};
                    g->glColorMask(1, 1, 1, 1);
                    g->glClearBufferfv(0x1800 /* GL_COLOR */, slot, zero);
                }
                g->glEnable(GL_SCISSOR_TEST);
            }
            g->glDisable(GL_SCISSOR_TEST);
            g->ndclear = 0;
        }
        deferred_clears_flush(g, -1, use_ms); /* no complete framebuffer: each through rfbo, as before */
        if (g->fbo_checked) {
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
                    int rows = reach < w->h / fy ? reach * fy : w->h;
                    w->written = stamp;
                    w->unbounded = !bounded;
                    w->dirty_h = rows > w->dirty_h ? rows : w->dirty_h;
                }
            g->vx = fx, g->vy = fy;
            g->tw = ref->w / fx, g->th = ref->h / fy;
            viewport_restore(g);
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
        g->fbo_checked = 0;
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

/* each stage's block: 256 float constants, the 8 boolean and 32 loop constants, alpha test, param_gen, viewport. Sent
 * only when it differs from what the buffer holds (about 95 % of the stage blocks are equal to the last draw's);
 * indices 0 / 1 stay bound to the two buffers from set-up (nothing else here binds uniform buffers), again after an
 * attached context's owner has drawn. The shadow goes with a state load (do_reset) and a new context (setup). */
static void upload_constants(xenos_gl *g, const xenos_cp *cp)
{
    uint8_t block[UBO_BLOCK];
    float vp[4];
    viewport_transform(g, cp, vp);
    uint32_t cc = cp->regs[0x2202];
    float at[4] = {(cc >> 3) & 1 ? 1.0f : 0.0f, (float)(cc & 7), as_float(cp->regs[0x210E]), 0.0f};
    /* SQ_PROGRAM_CNTL's param_gen, SQ_CONTEXT_MISC's register for it; the pixel position is gl_FragCoord.xy * zw +
     * (0, y) - 0.5: guest pixels from the top-left at any internal resolution */
    float inv = 1.0f / (float)g->scale;
    float pg[4] = {(cp->regs[0x2180] >> 18) & 1 ? (float)((cp->regs[0x2181] >> 8) & 0xFF) : -1.0f, 0.0f,
                   inv / (float)g->vx, inv / (float)g->vy}; /* (WP-28: guest pixels of an MSAA view) */
    if (g->ubo_rebind) {
        g->ubo_rebind = 0;
        for (int stage = 0; stage < 2; stage++)
            g->glBindBufferBase(GL_UNIFORM_BUFFER, (GLuint)stage, g->ubo[stage]);
    }
    for (int stage = 0; stage < 2; stage++) {
        memcpy(block, &cp->regs[0x4000 + 0x400 * stage], 4096); /* float constants: register values are the bits */
        memcpy(block + 4096, &cp->regs[0x4900], 32);
        memcpy(block + 4096 + 32, &cp->regs[0x4908], 128);
        memcpy(block + 4096 + 32 + 128, at, 16); /* RB_COLORCONTROL's alpha test, RB_ALPHA_REF */
        memcpy(block + 4096 + 32 + 128 + 16, pg, 16);
        memcpy(block + 4096 + 32 + 128 + 32, vp, 16);
        if (g->ubo_valid[stage] && !memcmp(block, g->ubo_shadow[stage], sizeof block))
            continue;
        g->glBindBuffer(GL_UNIFORM_BUFFER, g->ubo[stage]);
        /* a new store, not glBufferSubData: the last draw may still read the old one (Sub stalls on it: ANGLE's Metal
         * went from 2.1 to 6-11 ms of uploads per frame); the binding points keep the buffer name */
        g->glBufferData(GL_UNIFORM_BUFFER, sizeof block, block, GL_STREAM_DRAW);
        memcpy(g->ubo_shadow[stage], block, sizeof block);
        g->ubo_valid[stage] = 1;
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

static void apply_blend(xenos_gl *g, const xenos_cp *cp);
static void apply_depth(xenos_gl *g, const xenos_cp *cp);
static void apply_cull(xenos_gl *g, const xenos_cp *cp);
static void apply_scissor(xenos_gl *g, const xenos_cp *cp);

static void use_program(xenos_gl *g, GLuint prog)
{
    if (g->cur_prog != prog) {
        g->glUseProgram(prog);
        g->cur_prog = prog;
    }
}

static void bind_vao(xenos_gl *g, GLuint vao)
{
    if (g->cur_vao != vao) {
        g->glBindVertexArray(vao);
        g->cur_vao = vao;
    }
}

/* the vertex array's enabled attribute arrays made `mask` (bit n: location n) */
static void attribs_enable(xenos_gl *g, uint32_t mask)
{
    for (uint32_t d = g->attr_on ^ mask; d; d &= d - 1) {
        GLuint loc = (GLuint)__builtin_ctz(d);
        if ((mask >> loc) & 1)
            g->glEnableVertexAttribArray(loc);
        else
            g->glDisableVertexAttribArray(loc);
    }
    g->attr_on = mask;
}

static void apply_state(xenos_gl *g, const xenos_cp *cp, uint32_t prim_type)
{
    /* WP-24: each group set only when its registers changed since apply_state last set it (and nothing else touched
     * that state since: gs_valid) */
    int valid = g->gs_valid;
    g->gs_valid = 1;
    uint32_t f32 = 0;
    if (!g->float_blend)
        for (int i = 0; i < 4; i++)
            f32 |= (uint32_t)float32_target(g, i) << i;
    uint32_t in_blend[11] = {cp->regs[0x2104], cp->regs[0x2105], cp->regs[0x2106], cp->regs[0x2107], cp->regs[0x2108],
                             cp->regs[0x2201], cp->regs[0x2209], cp->regs[0x220A], cp->regs[0x220B], f32, 0};
    uint32_t in_depth[6] = {cp->regs[0x2200], cp->regs[0x210C], cp->regs[0x210D], cp->regs[0x2206] & 0x30u,
                            cp->regs[0x2113], cp->regs[0x2114]};
    int polygonal = (prim_type >= 4 && prim_type <= 7) || (prim_type >= 13 && prim_type <= 15);
    uint32_t in_offset[6] = {cp->regs[0x2205] & 0x3803u, cp->regs[0x2380], cp->regs[0x2381], cp->regs[0x2382],
                             cp->regs[0x2383], (uint32_t)polygonal};
    uint32_t in_scissor[6] = {cp->regs[0x2081], cp->regs[0x2082], (uint32_t)g->tw, (uint32_t)g->th, cp->regs[0x2080],
                              (uint32_t)(g->vx | g->vy << 8)};
    int do_blend = !valid || memcmp(in_blend, g->gs_blend, sizeof in_blend);
    int do_depth = !valid || memcmp(in_depth, g->gs_depth, sizeof in_depth);
    int do_cull = !valid || (cp->regs[0x2205] & 7) != g->gs_cull;
    int do_offset = !valid || memcmp(in_offset, g->gs_offset, sizeof in_offset);
    int do_scissor = !valid || memcmp(in_scissor, g->gs_scissor, sizeof in_scissor);
    memcpy(g->gs_blend, in_blend, sizeof in_blend);
    memcpy(g->gs_depth, in_depth, sizeof in_depth);
    g->gs_cull = cp->regs[0x2205] & 7;
    memcpy(g->gs_offset, in_offset, sizeof in_offset);
    memcpy(g->gs_scissor, in_scissor, sizeof in_scissor);
    if (do_blend)
        apply_blend(g, cp);
    if (do_depth)
        apply_depth(g, cp);
    if (do_cull)
        apply_cull(g, cp);
    if (do_offset)
        apply_polygon_offset(g, cp, prim_type);
    if (do_scissor)
        apply_scissor(g, cp);
}

static void apply_blend(xenos_gl *g, const xenos_cp *cp)
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
}

static void apply_depth(xenos_gl *g, const xenos_cp *cp)
{
    /* depth and stencil */
    uint32_t dc = cp->regs[0x2200];
    /* WP-27: the viewport's z (PA_CL_VPORT_ZSCALE / ZOFFSET, PA_CL_VTE_CNTL bits 4 / 5; else 1 / 0): window z = z/w *
     * scale + offset, which the shaders leave to the depth range (they hand GL z/w over 0..1). A title reversing
     * depth through the viewport (scale -1, offset 1, test "greater" against a buffer cleared to 0: Adventure Time)
     * otherwise has its depth test inverted. GL clamps the range to 0..1, as Xenos's depth is. */
    uint32_t vte = cp->regs[0x2206];
    float zs = vte & 0x10 ? as_float(cp->regs[0x2113]) : 1.0f, zo = vte & 0x20 ? as_float(cp->regs[0x2114]) : 0.0f;
    float zn = zo, zf = zo + zs;
    zn = zn > 0.0f ? (zn < 1.0f ? zn : 1.0f) : 0.0f; /* (NaN: 0) */
    zf = zf > 0.0f ? (zf < 1.0f ? zf : 1.0f) : 0.0f;
    g->glDepthRangef(zn, zf);
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
}

static void apply_cull(xenos_gl *g, const xenos_cp *cp)
{
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
}

static void apply_scissor(xenos_gl *g, const xenos_cp *cp)
{
    /* the window scissor (top-left / bottom-right, y downwards), when it is set and smaller than the target */
    uint32_t tl = cp->regs[0x2081], br = cp->regs[0x2082];
    int x0 = (int)(tl & 0x7FFF), y0 = (int)((tl >> 16) & 0x7FFF);
    int x1 = (int)(br & 0x7FFF), y1 = (int)((br >> 16) & 0x7FFF);
    if (br && !(tl >> 31) && cp->regs[0x2080]) {
        /* WP-26: PA_SC_WINDOW_OFFSET moves the window scissor unless PA_SC_WINDOW_SCISSOR_TL's window_offset_disable
         * (bit 31) is set (Xenia's GetScissor): a title drawing a 1920-wide picture as two 960-wide tiles gives the
         * right tile's scissor as 960..1920 and the offset -960 */
        int empty = !(x1 > x0 && y1 > y0);
        uint32_t wo = cp->regs[0x2080];
        int wx = (int32_t)(wo << 17) >> 17, wy = (int32_t)(wo << 1) >> 17;
        x0 += wx, x1 += wx, y0 += wy, y1 += wy;
        x0 = x0 < 0 ? 0 : x0, y0 = y0 < 0 ? 0 : y0;
        if (!empty && !(x1 > x0 && y1 > y0)) { /* moved off the target: nothing is drawn */
            g->glEnable(GL_SCISSOR_TEST);
            g->glScissor(0, 0, 0, 0);
            return;
        }
    }
    if (br && (x0 > 0 || y0 > 0 || x1 < g->tw || y1 < g->th) && x1 > x0 && y1 > y0) {
        int s = g->scale * g->vx, t = g->scale * g->vy;
        g->glEnable(GL_SCISSOR_TEST);
        g->glScissor(x0 * s, y0 * t, (x1 - x0) * s, (y1 - y0) * t);
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
static inline __attribute__((always_inline)) void texel(uint32_t format, const uint8_t *s, uint8_t out[4])
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

/* WP-36: half_bits of every 16-bit pattern, made once by half_bits itself (a half-float texel took up to 8 divisions);
 * texel_f reads it, decode_texture makes it before a float format's first texel */
static float half_lut[65536];
static pthread_once_t half_once = PTHREAD_ONCE_INIT;
static void half_lut_init(void)
{
    for (uint32_t i = 0; i < 65536; i++)
        half_lut[i] = half_bits(i);
}

/* a float-decoded texel: 16-bit normalised (signed per the fetch constant's sign of X), halves, floats, depth */
static inline __attribute__((always_inline)) void texel_f(uint32_t format, const uint8_t *s, uint32_t sign,
                                                          float out[4])
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
            out[i] = half_lut[s[2 * i] | (uint32_t)s[2 * i + 1] << 8];
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

/* WP-36: a row of n texels of a 1-texel-block format, `bytes` apart: the format chosen once, then a loop with texel /
 * texel_f folded for it (the usual formats; any other through the same functions with the format at run time) */
#define TEXEL_ROW(F)                                                                                                   \
    case F:                                                                                                            \
        for (uint32_t i = 0; i < n; i++)                                                                               \
            texel(F, s + (size_t)i * bytes, o + 4 * (size_t)i);                                                        \
        return;
static void texel_row(uint32_t format, const uint8_t *s, unsigned bytes, uint8_t *o, uint32_t n)
{
    switch (format) {
    TEXEL_ROW(2) TEXEL_ROW(3) TEXEL_ROW(4) TEXEL_ROW(5) TEXEL_ROW(6) TEXEL_ROW(7) TEXEL_ROW(10) TEXEL_ROW(14)
    TEXEL_ROW(15)
    default:
        for (uint32_t i = 0; i < n; i++)
            texel(format, s + (size_t)i * bytes, o + 4 * (size_t)i);
    }
}
#undef TEXEL_ROW

#define TEXEL_ROW_F(F)                                                                                                 \
    case F:                                                                                                            \
        for (uint32_t i = 0; i < n; i++)                                                                               \
            texel_f(F, s + (size_t)i * bytes, sign, o + 4 * (size_t)i);                                                \
        return;
static void texel_row_f(uint32_t format, const uint8_t *s, unsigned bytes, uint32_t sign, float *o, uint32_t n)
{
    switch (format) {
    TEXEL_ROW_F(22) TEXEL_ROW_F(24) TEXEL_ROW_F(25) TEXEL_ROW_F(26) TEXEL_ROW_F(30) TEXEL_ROW_F(31) TEXEL_ROW_F(32)
    TEXEL_ROW_F(36)
    default:
        for (uint32_t i = 0; i < n; i++)
            texel_f(format, s + (size_t)i * bytes, sign, o + 4 * (size_t)i);
    }
}
#undef TEXEL_ROW_F

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

/* WP-36: `words` aligned dwords from physical memory with the swap mode known at compile time: each one's bytes as
 * the little-endian host stores the word gpu_read gives */
static inline __attribute__((always_inline)) void read_words(const uint8_t *src, uint32_t words, uint32_t endian,
                                                             uint8_t *out)
{
    for (uint32_t k = 0; k < words; k++) {
        uint32_t w;
        memcpy(&w, src + 4 * (size_t)k, 4);
        w = gpu_swap(w, endian);
        memcpy(out + 4 * (size_t)k, &w, 4);
    }
}

/* `n` bytes at a guest physical address as the GPU reads them (after its swap), a dword at a time */
static void read_bytes(const xenos_cp *cp, uint32_t addr, uint32_t n, uint32_t endian, uint8_t *out)
{
    uint32_t i = 0;
    while (i < n) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        uint32_t at = (addr + i) & 0x1FFFFFFFu;
        if (cp->mem && !(at & 3) && n - i >= 4) { /* the whole dwords, up to the end of physical memory */
            uint32_t words = (n - i) / 4, room = (0x20000000u - at) / 4;
            words = words < room ? words : room;
            switch (endian & 3) {
            case 0: read_words(cp->mem + at, words, 0, out + i); break;
            case 1: read_words(cp->mem + at, words, 1, out + i); break;
            case 2: read_words(cp->mem + at, words, 2, out + i); break;
            default: read_words(cp->mem + at, words, 3, out + i); break;
            }
            i += 4 * words;
            continue;
        }
#endif
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
    uint32_t lo, hi;            /* the physical bytes the levels' storage covers */
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
 * slices packed, tiled ones in the 3D tiling; mips as the header says). With `hash_only`, only the hash is made;
 * with 2, only lo / hi (memory is not read). 0 when unsupported. */
static int decode_texture(const xenos_cp *cp, const uint32_t *fc, int hash_only, decoded *out)
{
    memset(out, 0, sizeof *out);
    out->lo = 0xFFFFFFFFu;
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
        {
            uint32_t lat = level_addr & 0x1FFFFFFFu, lspan = st.stride * (dim == 3 ? 6 : 1);
            out->lo = lat < out->lo ? lat : out->lo;
            out->hi = lat + lspan > out->hi ? lat + lspan : out->hi;
            if (hash_only == 2) {
                out->lw[l] = lw, out->lh[l] = lh, out->ld[l] = ld;
                continue;
            }
        }
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
                if (dim == 2) {
                    for (uint32_t bx = 0; bx < bw; bx++) {
                        uint32_t off = (uint32_t)tiled3d((int32_t)(bx + ox), (int32_t)(by + oy), (int32_t)(z + oz),
                                                         st.pitch_blocks, st.height_blocks, bpb_log2);
                        read_bytes(cp, level_addr + off, tf->bytes, endian, row + (size_t)bx * tf->bytes);
                    }
                    continue;
                }
                /* WP-36: in a 2D-tiled row, 16 bytes of blocks from an aligned x lie together (8 one-byte blocks):
                 * one address and one read per run of them, the bytes read_bytes gave block by block */
                const uint32_t per = bpb_log2 ? 16u >> bpb_log2 : 8u;
                for (uint32_t bx = 0; bx < bw;) {
                    uint32_t gx = bx + ox, run = per - (gx & (per - 1));
                    run = run < bw - bx ? run : bw - bx;
                    uint32_t off = z * st.stride + (uint32_t)tiled2d((int32_t)gx, (int32_t)(by + oy), st.pitch_blocks,
                                                                     bpb_log2);
                    read_bytes(cp, level_addr + off, run * tf->bytes, endian, row + (size_t)bx * tf->bytes);
                    bx += run;
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
        if (tf->is_float)
            pthread_once(&half_once, half_lut_init);
        for (uint32_t z = 0; z < ld; z++) {
            size_t at = (size_t)z * lw * lh;
            for (uint32_t by = 0; by < bh; by++) {
                if (tf->block != 4) { /* one texel a block: bw is lw */
                    const uint8_t *s = src + z * slice_n + (size_t)by * bw * tf->bytes;
                    if (tf->is_float)
                        texel_row_f(tf->format, s, tf->bytes, sign, (float *)out->level[l] + (at + (size_t)by * lw) * 4,
                                    bw);
                    else
                        texel_row(tf->format, s, tf->bytes, (uint8_t *)out->level[l] + (at + (size_t)by * lw) * 4, bw);
                    continue;
                }
                for (uint32_t bx = 0; bx < bw; bx++) {
                    const uint8_t *s = src + z * slice_n + ((size_t)by * bw + bx) * tf->bytes;
                    uint8_t q[16][4];
                    dxt_block(s, tf->format, q);
                    uint8_t *o = out->level[l];
                    for (uint32_t i = 0; i < 16; i++) {
                        uint32_t tx = bx * 4 + i % 4, ty = by * 4 + i / 4;
                        if (tx < lw && ty < lh)
                            memcpy(o + (at + (size_t)ty * lw + tx) * 4, q[i], 4);
                    }
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
    hashed_bytes += bytes;
    for (uint32_t i = 0; i < bytes; i += 4)
        h = (h ^ (cp->read32 ? cp->read32(cp->ctx, (base + i) & 0x1FFFFFFFu) : 0)) * 1099511628211ull;
    return h;
}

/* WP-12: what the check of a copy's memory once per epoch hashes when the copy is large (COPY_SPARSE_MIN and up: the
 * full-screen buffers, 8 MB at 1080p, ~26 MB of hashing per frame before): one 64-byte line of each 4 KB page, at an
 * offset that changes from page to page (1/64 of the bytes). A write the CPU makes over a resolve's destination is nearly
 * always a whole buffer (a texture loaded or decoded there, the memory handed to something else): it changes a line of
 * every page it covers. One that misses every sampled line (less than a page, or the same bytes there) is caught by the
 * full hash every COPY_FULL_EVERY checks instead of the next one: until then the picture is sampled for that memory,
 * and a lazy resolve pending there that is written in between (WP-2) goes over the CPU's bytes. Smaller copies are
 * hashed whole every time, as before (cheap); RECOMP_X360_COPY_HASH=full hashes every copy whole every time. Nothing in
 * the runtime tracks guest writes (the recompiled code stores straight into the mapped physical memory): a hash is
 * the only way to know. */
#define COPY_FULL_EVERY 16
#define COPY_SPARSE_MIN (256u << 10)
static uint64_t sparse_hash(const xenos_cp *cp, uint32_t base, uint32_t bytes)
{
    base &= 0x1FFFFFFFu;
    if (bytes > 0x20000000u - base)
        bytes = 0x20000000u - base;
    uint32_t end = base + bytes;
    uint64_t h = 0x243F6A8885A308D3ull ^ bytes;
    for (uint32_t page = base & ~0xFFFu; page < end; page += 0x1000) {
        uint32_t lo = page < base ? base : page, hi = end - page > 0x1000 ? page + 0x1000 : end;
        uint32_t at = page + ((((page >> 12) * 0x9E3779B1u) >> 26) << 6);
        if (at + 64 > hi)
            at = hi - lo >= 64 ? hi - 64 : lo;
        if (at < lo)
            at = lo;
        uint32_t n = hi - at < 64 ? hi - at : 64;
        if (cp->mem) {
            h = span_hash(cp->mem + at, n, h);
        } else {
            hashed_bytes += n;
            for (uint32_t i = 0; i + 4 <= n; i += 4)
                h = (h ^ (cp->read32 ? cp->read32(cp->ctx, at + i) : 0)) * 1099511628211ull;
            h ^= h >> 29;
        }
    }
    return h;
}

/* a copy's memory as it is now becomes what its checks compare with (after a resolve or a flush wrote it, or when it is
 * made): hashed whole, and sparsely */
static void copy_rehash(xenos_gl *g, const xenos_cp *cp, rt_copy *rc)
{
    rc->hash = memory_hash(cp, rc->base, rc->bytes);
    rc->sparse = sparse_hash(cp, rc->base, rc->bytes);
    rc->checks = 0;
    rc->checked = g->epoch;
}

/* WP-18: the swap's picture taken out of copy rc's texture into frame_tex (rc about to change, go, or be sampled):
 * what the window shows stays that swap's */
static void frame_texture(xenos_gl *g, int fw, int fh, GLenum ifmt);
static void blit_tex(xenos_gl *g, GLuint src, GLuint dst, int sx, int sy, int w, int h, int dx, int dy, int whole);
static void frame_detach(xenos_gl *g, const rt_copy *rc)
{
    if (!g->frame_copy || (rc && &g->copies[g->frame_copy - 1] != rc))
        return;
    const rt_copy *c = &g->copies[g->frame_copy - 1];
    int fw = g->show_w / g->scale, fh = g->show_h / g->scale;
    frame_texture(g, fw, fh, c->ifmt);
    blit_tex(g, c->tex, g->frame_tex, 0, 0, fw, fh, 0, 0, 1);
    g->glBindTexture(GL_TEXTURE_2D, g->frame_tex);
    for (int k = 0; k < 4; k++)
        g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R + (GLenum)k, g->frame_swz[k]);
    g->frame_copy = 0;
}

/* WP-18: copy rc's texture is deleted or replaced: the textures that were it go too, the frame is taken out of it */
static void copy_tex_gone(xenos_gl *g, const rt_copy *rc)
{
    frame_detach(g, rc);
    int idx = 1 + (int)(rc - g->copies);
    for (int i = 0; i < MAX_TEXTURES; i++) {
        texture *t = &g->textures[i];
        if (t->direct && t->copy == idx) {
            tex_unlink(g, t);
            memset(t, 0, sizeof *t);
        }
    }
}

/* a copy goes: its picture, its pending write if any (the CPU wrote that memory since: its bytes win, as they would
 * have over the resolve), and with `retake` the textures taken from it are checked again at their next use */
static void drop_copy(xenos_gl *g, rt_copy *c, int retake)
{
    if (c->pending) {
        g->npending--;
        g->st.resolves_dropped++;
    }
    if (retake)
        for (int i = 0; i < MAX_TEXTURES; i++)
            if (g->textures[i].copy == 1 + (int)(c - g->copies))
                g->textures[i].checked = g->epoch - 1u;
    copy_tex_gone(g, c);
    g->glDeleteTextures(1, &c->tex);
    memset(c, 0, sizeof *c);
}

/* WP-2: a copy's pending resolve written to guest memory; those overlapping physical [lo, hi) (after resolve_write) */
static void flush_copy(xenos_gl *g, const xenos_cp *cp, rt_copy *rc);
static void flush_range(xenos_gl *g, const xenos_cp *cp, uint32_t lo, uint32_t hi);

/* the copy whose destination is `base`, still matching guest memory (checked once per epoch), or NULL */
static rt_copy *copy_at(xenos_gl *g, const xenos_cp *cp, uint32_t base)
{
    base &= 0x1FFFF000u;
    for (int i = 0; i < MAX_COPIES; i++) {
        rt_copy *c = &g->copies[i];
        if (!c->tex || c->base != base)
            continue;
        if (c->checked != g->epoch) {
            /* a large copy sparsely, and whole every COPY_FULL_EVERY checks (WP-12) */
            int full = g->copy_hash_full || c->bytes < COPY_SPARSE_MIN || ++c->checks >= COPY_FULL_EVERY;
            if (full)
                c->checks = 0;
            if (full ? memory_hash(cp, c->base, c->bytes) != c->hash
                     : sparse_hash(cp, c->base, c->bytes) != c->sparse) { /* the CPU (or a texture upload) wrote it */
                drop_copy(g, c, 0);
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

/* copy a w x h guest-pixel region at (sx, sy) of texture `src` into (dx, dy) of texture `dst` (both at the internal
 * resolution, compatible formats); `whole`: the region is all of dst (its old contents invalidated, WP-17) */
static void blit_tex(xenos_gl *g, GLuint src, GLuint dst, int sx, int sy, int w, int h, int dx, int dy, int whole)
{
    GS_DIRTY(g); /* WP-24: GL state apply_state shadows changes here */
    int s = g->scale;
    g->glBindFramebuffer(GL_READ_FRAMEBUFFER, g->rfbo);
    g->glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, src, 0);
    g->glReadBuffer(GL_COLOR_ATTACHMENT0);
    g->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g->tfbo_draw);
    g->glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst, 0);
    static const GLenum b0 = GL_COLOR_ATTACHMENT0;
    g->glDrawBuffers(1, &b0);
    g->glDisable(GL_SCISSOR_TEST);
    if (whole)
        tiler_discard(g, 1, 0);
    g->glBlitFramebuffer(sx * s, sy * s, (sx + w) * s, (sy + h) * s, dx * s, dy * s, (dx + w) * s, (dy + h) * s,
                         GL_COLOR_BUFFER_BIT, GL_NEAREST);
    g->glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    g->glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    g->glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
}

/* WP-24: texture entries by their key (the fetch constant's six words) */
static unsigned tex_bucket_of(const uint32_t *key)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < 6; i++)
        h = (h ^ key[i]) * 16777619u;
    return (h ^ h >> 15) & (TEX_BUCKETS - 1);
}

static void tex_unlink(xenos_gl *g, texture *t)
{
    int idx = (int)(t - g->textures);
    int16_t *link = &g->tex_bucket[tex_bucket_of(t->key)];
    for (; *link; link = &g->textures[*link - 1].next1)
        if (*link - 1 == idx) {
            *link = t->next1;
            break;
        }
    t->next1 = 0;
}

static void tex_link(xenos_gl *g, texture *t)
{
    int16_t *head = &g->tex_bucket[tex_bucket_of(t->key)];
    t->next1 = *head;
    *head = (int16_t)(1 + (t - g->textures));
}

/* an entry for key fc: the least recently used (or a free one) taken, moved to fc's bucket */
static texture *tex_take(xenos_gl *g, const uint32_t *fc)
{
    texture *victim = &g->textures[0];
    for (int i = 0; i < MAX_TEXTURES; i++) {
        texture *c = &g->textures[i];
        if (!c->tex) {
            victim = c;
            break;
        }
        if (c->used < victim->used)
            victim = c;
    }
    if (victim->tex)
        tex_unlink(g, victim);
    memcpy(victim->key, fc, sizeof victim->key);
    tex_link(g, victim);
    return victim;
}

/* the texture behind fetch constant k as a GL texture of the dimension the shader samples (1: 2D, 2: 3D, 3: cube).
 * A 2D texture a resolve wrote (memory unchanged since) is the resolve's host picture at the internal resolution;
 * any other cached texture is checked against its memory once per frame (and after each resolve, which may have
 * written it), decoded again when that changed. 0 if unsupported. */
static GLuint texture_of_entry(xenos_gl *g, const xenos_cp *cp, const uint32_t *fc, uint32_t want_dim, int no_direct,
                               texture **entry);
/* WP-24: and its entry in *entry (NULL with 0) */
static GLuint texture_of(xenos_gl *g, const xenos_cp *cp, const uint32_t *fc, uint32_t want_dim, int no_direct,
                         texture **entry)
{
    *entry = NULL;
    return texture_of_entry(g, cp, fc, want_dim, no_direct, entry);
}

static GLuint texture_of_entry(xenos_gl *g, const xenos_cp *cp, const uint32_t *fc, uint32_t want_dim, int no_direct,
                               texture **entry)
{
    uint32_t dim = (fc[5] >> 9) & 3, gl_dim = dim == 0 ? 1 : dim;
    if ((fc[0] & 3) != 2 || gl_dim != want_dim || !tex_format(fc[1] & 63))
        return 0;
    texture *t = NULL;
    for (int16_t e = g->tex_bucket[tex_bucket_of(fc)]; e; e = g->textures[e - 1].next1)
        if (!memcmp(g->textures[e - 1].key, fc, sizeof t->key)) {
            t = &g->textures[e - 1];
            break;
        }
    if (t && t->checked == g->epoch) { /* already checked this frame, no resolve since */
        t->used = (int)++g->tex_clock;
        return *entry = t, t->tex;
    }
    /* a resolve's picture */
    rt_copy *rc = dim == 1 ? copy_at(g, cp, fc[1]) : NULL;
    if (rc && !copy_fits(rc, fc))
        rc = NULL; /* read another way than it was written: decoded from memory */
    if (rc) {
        uint32_t w = (fc[2] & 0x1FFF) + 1, h = ((fc[2] >> 13) & 0x1FFF) + 1;
        int idx = 1 + (int)(rc - g->copies);
        if (g->copy_direct && !no_direct && (int)w == rc->w && (int)h == rc->th) {
            /* WP-18: the picture's own size: its texture sampled in place, no copy (it changes with the copy, as the
             * memory would: a resolve moves the copy's generation and its texture keeps its name; one deleted or
             * replaced takes these entries with it, copy_tex_gone) */
            if (!t)
                t = tex_take(g, fc);
            if (t->tex && !t->direct)
                g->glDeleteTextures(1, &t->tex);
            if (!(t->direct && t->copy == idx && t->gen == rc->gen))
                g->st.copy_textures++; /* (taken again: what a copy was counted for before) */
            t->tex = rc->tex;
            t->direct = 1;
            t->copy = idx;
            t->copy_w = t->copy_h = 0;
            t->levels = 1;
            t->swap_rb = rc->swap_rb;
            t->hash = rc->hash;
            t->gen = rc->gen;
            t->checked = g->epoch;
            t->used = (int)++g->tex_clock;
            return *entry = t, t->tex;
        }
        if ((int)w <= rc->w && (int)h <= rc->th) {
            if (t && !t->direct && t->copy == idx && t->gen == rc->gen) {
                t->swap_rb = rc->swap_rb;
                t->checked = g->epoch;
                t->used = (int)++g->tex_clock;
                return *entry = t, t->tex;
            }
            if (!t)
                t = tex_take(g, fc);
            if (t->direct) /* not ours to keep or delete */
                t->tex = 0, t->direct = 0;
            /* another copy's picture in a texture of this size and format: the blit below overwrites all of it, the
             * texture object is kept (a new 1080p texture per copy change is a reallocation and a render pass more) */
            int hw = (int)w * g->scale, hh = (int)h * g->scale;
            int keep = t->tex && t->copy && t->copy_w == hw && t->copy_h == hh && t->copy_ifmt == rc->ifmt;
            if (t->tex && !keep)
                g->glDeleteTextures(1, &t->tex);
            if (!keep) {
                GLenum tf = GL_RGBA, tt = GL_UNSIGNED_BYTE;
                if (rc->ifmt == GL_RGB10_A2)
                    tt = GL_UNSIGNED_INT_2_10_10_10_REV;
                else if (rc->ifmt == GL_RGBA16F || rc->ifmt == GL_RG16F)
                    tf = rc->ifmt == GL_RG16F ? GL_RG : GL_RGBA, tt = GL_HALF_FLOAT;
                else if (rc->ifmt == GL_R32F || rc->ifmt == GL_RG32F)
                    tf = rc->ifmt == GL_R32F ? GL_RED : GL_RG, tt = GL_FLOAT;
                t->tex = new_texture(g, rc->ifmt, tf, tt, hw, hh);
                t->copy_w = hw, t->copy_h = hh, t->copy_ifmt = rc->ifmt;
                t->swz = 0;
            }
            blit_tex(g, rc->tex, t->tex, 0, 0, (int)w, (int)h, 0, 0, 1);
            t->copy = 1 + (int)(rc - g->copies);
            t->swap_rb = rc->swap_rb;
            t->hash = rc->hash;
            t->gen = rc->gen;
            t->checked = g->epoch;
            t->used = (int)++g->tex_clock;
            g->st.copy_textures++;
            g->st.copy_blits++;
            return *entry = t, t->tex;
        }
    }
    decoded d;
    if (g->npending) { /* lazy resolves over that memory: written first, so it is decoded as it would be */
        uint32_t lo = 0, hi = 0;
        if (t && !t->copy && t->hi > t->lo)
            lo = t->lo, hi = t->hi;
        else if (decode_texture(cp, fc, 2, &d))
            lo = d.lo, hi = d.hi;
        flush_range(g, cp, lo, hi);
    }
    if (t && !t->copy) {
        if (!decode_texture(cp, fc, 1, &d))
            return 0;
        if (d.hash == t->hash) {
            t->checked = g->epoch;
            t->used = (int)++g->tex_clock;
            return *entry = t, t->tex;
        }
    }
    if (!decode_texture(cp, fc, 0, &d))
        return 0;
    if (!t)
        t = tex_take(g, fc);
    if (t->tex && !t->direct) /* a texture object keeps its first target and sizes: a new one */
        g->glDeleteTextures(1, &t->tex);
    t->direct = 0;
    t->swz = 0;
    g->glGenTextures(1, &t->tex);
    t->copy = 0;
    t->swap_rb = 0;
    t->hash = d.hash;
    t->lo = d.lo, t->hi = d.hi;
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
    return *entry = t, t->tex;
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
/* WP-24: unit `unit` without a sampler object (the passes' texelFetch: a depth or float texture would be incomplete
 * under a filtering sampler) */
static void unit_plain(xenos_gl *g, int unit)
{
    if (unit < 32 && g->unit_sampler[unit]) {
        g->glBindSampler((GLuint)unit, 0);
        g->unit_sampler[unit] = 0;
    }
}

/* WP-24: the sampler object of a sampling state (made at first use; the table full: 0, texture parameters instead) */
static GLuint sampler_of(xenos_gl *g, uint32_t key, GLint ws, GLint wt, GLint wr, GLint mag, GLint min, float aniso)
{
    for (int i = 0; i < g->nsampler; i++)
        if (g->sampler[i].key == key)
            return g->sampler[i].id;
    if (g->nsampler == (int)(sizeof g->sampler / sizeof g->sampler[0]))
        return 0;
    GLuint id = 0;
    g->glGenSamplers(1, &id);
    if (!id)
        return 0;
    g->glSamplerParameteri(id, GL_TEXTURE_WRAP_S, ws);
    g->glSamplerParameteri(id, GL_TEXTURE_WRAP_T, wt);
    g->glSamplerParameteri(id, GL_TEXTURE_WRAP_R, wr);
    g->glSamplerParameteri(id, GL_TEXTURE_MAG_FILTER, mag);
    g->glSamplerParameteri(id, GL_TEXTURE_MIN_FILTER, min);
    if (aniso > 0.0f)
        g->glSamplerParameterf(id, 0x84FE /* GL_TEXTURE_MAX_ANISOTROPY_EXT */, aniso);
    g->sampler[g->nsampler].key = key, g->sampler[g->nsampler].id = id;
    g->nsampler++;
    return id;
}

/* a texture's sampling state from its fetch constant: wraps, filters (mips when it has them), swizzle. copy: 0 the
 * texture was decoded from memory, 1 it is a resolve's host picture, 3 one kept with red and blue unswapped. The
 * fetch constant's mip range (dword 4: mip_min_level bits 2-5, mip_max_level 6-9) is the texture's own
 * GL_TEXTURE_BASE_LEVEL / MAX_LEVEL (texture_of), GL's LOD counting from the base level; the mip filter "base map"
 * (2) samples the base level only, as a filter without mipmaps does (Xenia's mip_base_map: MaxLOD = MinLOD). Xenia's
 * base level there is mip_min_level; decode_texture uploads level 0 alone for a base map fetch constant, so one
 * with mip_min_level >= 1 still reads level 0 from the base address. WP-24: the wraps and filters are a sampler
 * object bound to `unit`, the swizzle is set on the texture when it is not the one last set there (*swz_cache:
 * 1 + that swizzle, 0 unknown). */
static void sampler_state(xenos_gl *g, int unit, GLenum target, const uint32_t *fc, int mipped, int copy,
                          uint32_t *swz_cache)
{
    static const GLint swz[6] = {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA, GL_ZERO, GL_ONE};
    if (((fc[3] >> 23) & 3) == 2)
        mipped = 0;
    uint32_t cs = (fc[0] >> 10) & 7, ct = (fc[0] >> 13) & 7, cr = (fc[0] >> 16) & 7;
    int mag = ((fc[3] >> 19) & 3) == 1, min = ((fc[3] >> 21) & 3) == 1, mip = ((fc[3] >> 23) & 3) == 1;
    if (g->tex_linear) /* XBA-282: the app's texture filter */
        mag = min = mip = 1;
    GLint gmag = mag ? GL_LINEAR : GL_NEAREST;
    GLint gmin = !mipped ? (min ? GL_LINEAR : GL_NEAREST)
                 : min   ? (mip ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST)
                         : (mip ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST);
    float aniso = g->tex_aniso > 1 ? (mipped ? g->tex_aniso : 1.0f) : 0.0f;
    GLuint smp = 0;
    if (g->samplers_on && unit < 32) {
        uint32_t key = wrap_mode(cs) == GL_REPEAT ? 0 : wrap_mode(cs) == GL_MIRRORED_REPEAT ? 1 : 2;
        key |= (wrap_mode(ct) == GL_REPEAT ? 0u : wrap_mode(ct) == GL_MIRRORED_REPEAT ? 1u : 2u) << 2;
        key |= (wrap_mode(cr) == GL_REPEAT ? 0u : wrap_mode(cr) == GL_MIRRORED_REPEAT ? 1u : 2u) << 4;
        key |= (uint32_t)mag << 6 | (uint32_t)(gmin - GL_NEAREST) << 7 | (uint32_t)(aniso > 1.0f) << 16 |
               (uint32_t)(aniso > 0.0f) << 17;
        smp = sampler_of(g, key, wrap_mode(cs), wrap_mode(ct), wrap_mode(cr), gmag, gmin, aniso);
    }
    if (unit < 32 && g->unit_sampler[unit] != smp) {
        g->glBindSampler((GLuint)unit, smp);
        g->unit_sampler[unit] = smp;
    }
    if (!smp) {
        g->glTexParameteri(target, GL_TEXTURE_WRAP_S, wrap_mode(cs));
        g->glTexParameteri(target, GL_TEXTURE_WRAP_T, wrap_mode(ct));
        g->glTexParameteri(target, GL_TEXTURE_WRAP_R, wrap_mode(cr));
        if (aniso > 0.0f)
            g->glTexParameterf(target, 0x84FE /* GL_TEXTURE_MAX_ANISOTROPY_EXT */, aniso);
        g->glTexParameteri(target, GL_TEXTURE_MAG_FILTER, gmag);
        g->glTexParameteri(target, GL_TEXTURE_MIN_FILTER, gmin);
    }
    uint32_t swizzle = (fc[3] >> 1) & 0xFFF;
    if (copy)
        swizzle = swizzle_as_decoded(swizzle, fc[1] & 63);
    if (copy & 2)
        swizzle = swizzle_swap_rb(swizzle);
    if (swz_cache && *swz_cache == 1 + swizzle && g->samplers_on)
        return;
    for (int c = 0; c < 4; c++) {
        uint32_t s = (swizzle >> (3 * c)) & 7;
        g->glTexParameteri(target, GL_TEXTURE_SWIZZLE_R + (GLenum)c, s < 6 ? swz[s] : GL_ZERO);
    }
    if (swz_cache)
        *swz_cache = 1 + swizzle;
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
    texture *ents[MAX_SAMPLERS];
    for (int i = 0; i < p->nsamplers; i++) {
        uint32_t k = p->sampler_fetch[i];
        ents[i] = NULL;
        texs[i] = k < 32 ? texture_of(g, cp, &cp->regs[0x4800 + 6 * (k & 31)], p->sampler_dim[i], 0, &ents[i]) : 0;
    }
    /* WP-18: a copy's texture sampled in place carries the sampling state on itself: two units sampling it another
     * way in one draw get a copy of their own for the second; the swap's picture shown from it is taken out first */
    for (int i = 0; i < p->nsamplers; i++) {
        if (!texs[i])
            continue;
        if (g->frame_copy && texs[i] == g->copies[g->frame_copy - 1].tex)
            frame_detach(g, NULL);
        const uint32_t *fi = &cp->regs[0x4800 + 6 * (p->sampler_fetch[i] & 31)];
        for (int j = 0; j < i; j++) {
            const uint32_t *fj = &cp->regs[0x4800 + 6 * (p->sampler_fetch[j] & 31)];
            uint32_t m0 = g->samplers_on ? 0 : 0x7FC00u, m3 = g->samplers_on ? 0x1FFEu : 0x01FFFFFEu;
            if (texs[j] == texs[i] && (((fi[0] ^ fj[0]) & m0) || ((fi[3] ^ fj[3]) & m3) || ((fi[1] ^ fj[1]) & 63))) {
                texs[i] = texture_of(g, cp, fi, p->sampler_dim[i], 1, &ents[i]);
                break;
            }
        }
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
        const texture *e = ents[i]; /* WP-24: texture_of's entry, no search for it */
        int mipped = !e->copy && e->levels > 1, copy = e->copy ? (e->swap_rb ? 3 : 1) : 0;
        uint32_t *cache = e->direct ? &g->copies[e->copy - 1].swz : &ents[i]->swz;
        sampler_state(g, i, target, fc, mipped, copy, cache);
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

/* Reused buffers (WP-1, WP-5): resolves, their depth and stencil, the frame read back at the swap, and each draw's
 * indices and converted vertices take them rather than a malloc / free pair each time (at 1080p a fresh 33 MB mapping: page faults, then an munmap, per
 * resolve on Android). Grown, never shrunk; freed in xenos_gl_destroy. NULL when out of memory. */
enum { SCRATCH_PIXELS, SCRATCH_STENCIL, SCRATCH_ROWS, SCRATCH_INDICES, SCRATCH_VERTICES, SCRATCH_VBTEX, SCRATCH_SLOTS };
_Static_assert(SCRATCH_SLOTS == sizeof ((xenos_gl *)0)->scratch / sizeof(void *), "xenos_gl.scratch: one per slot");
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
    GS_DIRTY(g); /* WP-24: GL state apply_state shadows changes here */
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

/* WP-36: tiled2d(x, y) is tiled2d(x & ~31, y) ^ tiled2d(x & 31, 0) (each address bit comes from one bit of x or of y,
 * the pipe bits from an exclusive or of both, and a 32-texel tile row's x never reaches the tile number's bits), so a
 * row's addresses take one tiled2d per 32 texels and a table lookup (xenos_resolve_write_test checks the identity).
 * (A 1920 x 1080 8_8_8_8 write in xenos_resolve_write_test: 3.9 ms with a tiled2d per 16 bytes, 2.4 ms so.) */
static inline uint32_t tiled2d_split(int32_t x, int32_t y, uint32_t pitch_blocks, unsigned bpb_log2)
{
    return (uint32_t)tiled2d(x & ~31, y, pitch_blocks, bpb_log2) ^ (uint32_t)tiled2d(x & 31, 0, 32, bpb_log2);
}

static inline __attribute__((always_inline)) void direct_rows(const xenos_cp *cp, const resolve_dest *d,
                                                             const uint8_t *px, int x0, int y0, int w, int h,
                                                             int kind, int swap_rb, uint32_t endian)
{
    const unsigned bpp_log2 = kind == DIRECT_HALF4 ? 3 : 2, bpp = 1u << bpp_log2, per = 16u >> bpp_log2;
    const size_t stride = kind == DIRECT_8888 || kind == DIRECT_2101010 ? 4 : 16;
    const uint32_t base = d->base, pitch = d->pitch_aligned; /* locals: the stores through wmem could alias *d, *cp */
    uint8_t *const wmem = cp->wmem;
    uint32_t in_tile[32]; /* tiled2d of x in a 32-texel tile row at y = 0: see tiled2d_split */
    for (int i = 0; i < 32; i++)
        in_tile[i] = tiled2d_split(i, 0, 32, bpp_log2);
    for (int y = 0; y < h; y++) {
        const uint8_t *sp = px + (size_t)y * w * stride;
        for (int x = 0; x < w;) {
            int gx = x0 + x, xe = x + 32 - (gx & 31); /* this tile row: [x, xe) */
            xe = xe < w ? xe : w;
            const uint32_t tile = (uint32_t)tiled2d(gx & ~31, y0 + y, pitch, bpp_log2);
            /* a whole tile row, its 32 texels inside the tile's 1024 << bpp_log2 bytes, below the end of memory */
            const uint32_t tile_bytes = 1024u << bpp_log2;
            if (xe - x == 32 && wmem && (uint64_t)base + (tile & ~(tile_bytes - 1)) + tile_bytes <= 0x20000000u) {
                for (unsigned g = 0; g < 32; g += per, sp += per * stride) {
                    uint8_t *dp = wmem + ((base + (tile ^ in_tile[g])) & 0x1FFFFFFFu);
                    for (unsigned i = 0; i < per; i++)
                        direct_texel(kind, sp + i * stride, swap_rb, endian, dp + i * bpp);
                }
                x = xe;
                continue;
            }
            while (x < xe) {
                gx = x0 + x;
                uint32_t a = (base + (tile ^ in_tile[gx & 31])) & 0x1FFFFFFFu;
                int run = (int)(per - ((unsigned)gx & (per - 1)));
                run = run < xe - x ? run : xe - x;
                if (wmem && a + (uint32_t)run * bpp <= 0x20000000u) {
                    uint8_t *dp = wmem + a;
                    if (run == (int)per) { /* a whole 16 bytes: a loop of known length, unrolled */
                        for (unsigned i = 0; i < per; i++)
                            direct_texel(kind, sp + i * stride, swap_rb, endian, dp + i * bpp);
                        sp += per * stride;
                    } else {
                        for (int i = 0; i < run; i++, sp += stride, dp += bpp)
                            direct_texel(kind, sp, swap_rb, endian, dp);
                    }
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
}

/* WP-36: RGBA bytes into a 16-bit format whose fields each hold one channel's unorm (3 1_5_5_5, 4 5_6_5, 5 6_5_5,
 * 10 8_8, 15 4_4_4_4, 24 16): encode_texel of one channel alone (the others 0, which encode to 0 bits) is that
 * channel's field, so a table per channel built with it and ORed gives the generic way's bits without its floats; the
 * bytes of up to 8 texels (16 bytes, contiguous in a tiled row) go out in one put_bytes, whose whole dwords are what
 * the generic way's per-texel read-modify-writes end with. (Warhammer resolves its 1280 x 720 picture to 5_6_5 twice
 * a frame: ~19 ms per frame in encode_texel and the per-texel read-modify-write before.) */
static int direct16_format(uint32_t f)
{
    return f == 3 || f == 4 || f == 5 || f == 10 || f == 15 || f == 24;
}

static void direct16_rows(const xenos_cp *cp, const resolve_dest *d, const uint8_t *px, int x0, int y0, int w, int h)
{
    uint16_t lut[4][256];
    for (int c = 0; c < 4; c++)
        for (int b = 0; b < 256; b++) {
            float p[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            uint8_t o[16] = {0};
            p[c] = (float)b / 255.0f; /* pix_float's value of that byte (the generic way's factor 1 leaves it so) */
            encode_texel(d->fmt, p, o);
            lut[c][b] = (uint16_t)(o[0] | o[1] << 8);
        }
    const int sr = d->swap_rb ? 2 : 0, sb = d->swap_rb ? 0 : 2; /* the source byte of the format's red / blue */
    const uint32_t base = d->base, endian = d->endian;
    uint8_t *const wmem = cp->wmem;
    uint32_t in_tile[32]; /* the addresses by tile row, as direct_rows (tiled2d_split) */
    for (int i = 0; i < 32; i++)
        in_tile[i] = tiled2d_split(i, 0, 32, 1);
    for (int y = 0; y < h; y++) {
        const uint8_t *row = px + (size_t)y * w * 4;
        uint32_t tile = 0;
        for (int x = 0; x < w;) {
            int gx = x0 + x, run = 8 - (gx & 7);
            run = run < w - x ? run : w - x;
            if (x == 0 || (gx & 31) == 0)
                tile = (uint32_t)tiled2d(gx & ~31, y0 + y, d->pitch_aligned, 1);
            uint32_t a = base + (tile ^ in_tile[gx & 31]), pa = a & 0x1FFFFFFFu;
            if (run == 8 && wmem && !(a & 3) && pa <= 0x20000000u - 16) { /* 4 whole dwords: put_bytes' stores */
                const uint8_t *sp = row + (size_t)x * 4;
                for (int k = 0; k < 4; k++, sp += 8) {
                    uint32_t lo = lut[0][sp[sr]] | lut[1][sp[1]] | lut[2][sp[sb]] | lut[3][sp[3]];
                    uint32_t hi = lut[0][sp[4 + sr]] | lut[1][sp[5]] | lut[2][sp[4 + sb]] | lut[3][sp[7]];
                    uint32_t word = gpu_swap(lo | hi << 16, endian);
                    memcpy(wmem + pa + 4 * (uint32_t)k, &word, 4);
                }
                x += run;
                continue;
            }
            uint8_t t[16];
            for (int i = 0; i < run; i++) {
                const uint8_t *sp = row + (size_t)(x + i) * 4;
                uint32_t v = lut[0][sp[sr]] | lut[1][sp[1]] | lut[2][sp[sb]] | lut[3][sp[3]];
                t[2 * i] = (uint8_t)v, t[2 * i + 1] = (uint8_t)(v >> 8);
            }
            put_bytes(cp, a, t, 2 * run, endian);
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
    else if (lay == PIX_U8X4 && direct16_format(f))
        return direct16_rows(cp, d, px, x0, y0, w, h), 1;
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
 * in the low 8; packed: dv holds depth | stencil << 24, read_depth_stencil's), tiled 32-bit texels, w x h values top
 * row first for the guest pixels at (x0, y0) */
static inline __attribute__((always_inline)) void depth_rows(uint8_t *wmem, uint32_t base, uint32_t pitch_aligned,
                                                            uint32_t endian, const uint32_t *dv, const uint8_t *sv,
                                                            int packed, int x0, int y0, int w, int h)
{
    uint32_t in_tile[32]; /* the addresses by tile row, as direct_rows (tiled2d_split) */
    for (int i = 0; i < 32; i++)
        in_tile[i] = tiled2d_split(i, 0, 32, 2);
    for (int y = 0; y < h; y++) {
        uint32_t tile = 0;
        for (int x = 0; x < w; x++) {
            int gx = x0 + x;
            if (x == 0 || (gx & 31) == 0)
                tile = (uint32_t)tiled2d(gx & ~31, y0 + y, pitch_aligned, 2);
            size_t at = (size_t)y * w + x;
            uint32_t v = gpu_swap(packed ? dv[at] << 8 | dv[at] >> 24 : dv[at] << 8 | (sv ? sv[at] : 0), endian);
            memcpy(wmem + ((base + (tile ^ in_tile[gx & 31])) & 0x1FFFFFFFu), &v, 4); /* gpu_store's store */
        }
    }
}

static void depth_write(const xenos_cp *cp, uint32_t base, uint32_t pitch_aligned, uint32_t endian, const uint32_t *dv,
                        const uint8_t *sv, int packed, int x0, int y0, int w, int h)
{
    if (cp->wmem && !(base & 3)) { /* WP-36: dword stores straight into memory, one loop per swap mode */
        switch (endian & 3) {
        case 0: depth_rows(cp->wmem, base, pitch_aligned, 0, dv, sv, packed, x0, y0, w, h); break;
        case 1: depth_rows(cp->wmem, base, pitch_aligned, 1, dv, sv, packed, x0, y0, w, h); break;
        case 2: depth_rows(cp->wmem, base, pitch_aligned, 2, dv, sv, packed, x0, y0, w, h); break;
        default: depth_rows(cp->wmem, base, pitch_aligned, 3, dv, sv, packed, x0, y0, w, h); break;
        }
        return;
    }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            size_t at = (size_t)y * w + x;
            uint32_t v = packed ? dv[at] << 8 | dv[at] >> 24 : dv[at] << 8 | (sv ? sv[at] : 0);
            uint32_t a = base + (uint32_t)tiled2d(x0 + x, y0 + y, pitch_aligned, 2);
            if (a & 3) {
                uint8_t b4[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
                put_bytes(cp, a, b4, 4, endian);
            } else {
                gpu_store(cp, a, gpu_swap(v, endian));
            }
        }
}

/* ---- lazy resolves (WP-2) ----
 * With RECOMP_X360_RESOLVE=lazy, a resolve that keeps a host picture (rt_copy: a colour target, a page-aligned base, no
 * exponent bias, the rectangle inside the destination) only blits into the picture and marks it pending; guest memory
 * stays as it was, and the copy's hash is of that untouched memory, so a CPU write there still drops the copy (and its
 * pending pixels: the CPU's bytes win, as they would have over an eager resolve). The pixels are written, from the
 * picture through resolve_write (the same bytes as the eager resolve: the picture is a nearest copy of the surface,
 * read_raw filters it down the same way), when the GPU is about to read that memory another way (a texture that does
 * not fit the picture, vertices, indices, the front buffer decoded at a swap) or write over it (another resolve, a
 * depth resolve, memory export), when the copy is evicted or resized, 2 swaps after its last resolve (a picture
 * resolved once and kept: save states get it) and at xenos_gl_flush_resolves. A pending resolve does not move the
 * epoch (memory did not change): the cached textures over its range and those taken from the copy are checked again
 * one by one instead. */
static void flush_copy(xenos_gl *g, const xenos_cp *cp, rt_copy *rc)
{
    if (!rc->pending)
        return;
    uint64_t t0 = mono_ns();
    int w = rc->px1 - rc->px0, h = rc->py1 - rc->py0;
    const uint8_t *px = read_raw(g, rc->tex, rc->ifmt, rc->px0, rc->py0, w, h);
    uint64_t t1 = mono_ns();
    if (px) {
        const resolve_dest d = {rc->base, (uint32_t)rc->w, rc->fmt, rc->endian, rc->swap_rb, 0};
        resolve_write(cp, &d, px, pix_layout(rc->ifmt), rc->px0, rc->py0, w, h);
        g->st.resolved_pixels += (uint64_t)w * (uint64_t)h;
    }
    rc->pending = 0;
    g->npending--;
    g->epoch++; /* memory changed under the cached textures: compare them again */
    copy_rehash(g, cp, rc);
    uint64_t t2 = mono_ns();
    g->st.resolve_flushes++;
    g->st.resolve_read_ns += t1 - t0;
    g->st.resolve_write_ns += t2 - t1;
    if (!g->in_resolve) /* else resolve() counts it */
        g->st.resolve_ns += t2 - t0;
}

static void flush_range(xenos_gl *g, const xenos_cp *cp, uint32_t lo, uint32_t hi)
{
    lo &= 0x1FFFFFFFu;
    for (int i = 0; g->npending && i < MAX_COPIES && hi > lo; i++) {
        rt_copy *c = &g->copies[i];
        if (c->pending && c->base < hi && lo < c->base + c->bytes)
            flush_copy(g, cp, c);
    }
}

void xenos_gl_flush_resolves(xenos_gl *g, const xenos_cp *cp)
{
    if (!g || !g->ready)
        return;
    if (g->reset_pending)
        do_reset(g); /* a state was loaded: what was pending belongs to the memory before it */
    if (!g->npending)
        return;
    for (int i = 0; g->npending && i < MAX_COPIES; i++)
        flush_copy(g, cp, &g->copies[i]);
}

int xenos_gl_lazy_resolves(const xenos_gl *g)
{
    return g && g->lazy;
}

/* WP-18: the rows of a copy's picture: RB_COPY_DEST_PITCH's height (`given`) or the rows resolved past it, within the
 * aligned height; the aligned height when the register leaves it unset (as before) */
static int copy_rows(uint32_t given, int y1, uint32_t height_aligned)
{
    uint32_t th = given > (uint32_t)y1 ? given : (uint32_t)y1;
    return (int)(given && th < height_aligned ? th : height_aligned);
}

/* WP-26: a resolve destination whose base lies inside the memory of a live copy of the same pitch, format, endian
 * mode and red / blue order, whole 32x32 blocks in (Direct3D resolving the right half of a two-tile picture moves the
 * base by the blocks of the left half): that copy, the rectangle (*dx, *dy) further in. Tiled 2D addresses are linear
 * in whole blocks, so it is the same memory. NULL when there is none or the rectangle would not fit its picture. */
static rt_copy *copy_holding(xenos_gl *g, const xenos_cp *cp, const surface *s, uint32_t base, uint32_t pitch_aligned,
                             uint32_t fmt, uint32_t endian, int swap_rb, int bpp, int x1, int y1, int *dx, int *dy)
{
    uint32_t blk = 32u * 32u * (uint32_t)bpp, per = pitch_aligned / 32;
    if (!bpp || !per)
        return NULL;
    for (int i = 0; i < MAX_COPIES; i++) {
        rt_copy *c = &g->copies[i];
        if (!c->tex || c->base >= base || base >= c->base + c->bytes || c->w != (int)pitch_aligned ||
            c->ifmt != s->ifmt || c->fmt != fmt || c->endian != endian || c->swap_rb != swap_rb ||
            (base - c->base) % blk)
            continue;
        uint32_t nb = (base - c->base) / blk;
        int ox = (int)(nb % per) * 32, oy = (int)(nb / per) * 32;
        if (x1 + ox > c->w || y1 + oy > c->th)
            return NULL;
        if (copy_at(g, cp, c->base) != c) /* the CPU wrote its memory since: dropped */
            return NULL;
        *dx = ox, *dy = oy;
        return c;
    }
    return NULL;
}

/* A colour resolve the lazy way, into its copy only; 0 when it cannot be (no host picture for it, or the rectangle
 * goes past the destination): the caller resolves it eagerly. into (WP-26): the copy holding the destination
 * (copy_holding), the guest rectangle (dx, dy) further in there. */
static int resolve_lazy(xenos_gl *g, const xenos_cp *cp, const surface *s, uint32_t base, uint32_t pitch_aligned,
                        uint32_t height, uint32_t fmt, uint32_t endian, int swap_rb, int bias, int bpp, int x0, int y0,
                        int x1, int y1, rt_copy *into, int dx, int dy)
{
    if (s->kind == 2 || (base & 0xFFF) || bias)
        return 0;
    int sx0 = x0, sy0 = y0; /* the source rectangle's corner; from here x0..y1 are the destination's */
    if (into) {
        base = into->base;
        x0 += dx, x1 += dx, y0 += dy, y1 += dy;
    }
    uint32_t given = height;
    if (!height)
        height = (uint32_t)y1;
    uint32_t height_aligned = (height + 31) & ~31u, bytes = pitch_aligned * height_aligned * (uint32_t)bpp;
    int th = copy_rows(given, y1, height_aligned);
    if (into) /* its picture as it is (the registers give the moved destination's height) */
        height_aligned = (uint32_t)into->h, bytes = into->bytes, th = into->th;
    if ((uint32_t)x1 > pitch_aligned || (uint32_t)y1 > height_aligned)
        return 0;
    /* other copies over this memory: their pending pixels go under ours first, then the copies go (an eager resolve
     * would change their memory) */
    for (int i = 0; i < MAX_COPIES; i++) {
        rt_copy *c = &g->copies[i];
        if (c->tex && c->base != base && c->base < base + bytes && base < c->base + c->bytes) {
            flush_copy(g, cp, c);
            drop_copy(g, c, 1);
        }
    }
    rt_copy *rc = copy_at(g, cp, base); /* still what memory holds (or held, plus what is pending) */
    if (rc && (rc->ifmt != s->ifmt || rc->w != (int)pitch_aligned || rc->h != (int)height_aligned || rc->th != th ||
               rc->swap_rb != swap_rb || rc->fmt != fmt || rc->endian != endian)) {
        flush_copy(g, cp, rc); /* resized or another format: what it holds is written the old way first */
        drop_copy(g, rc, 1);
    } else if (!rc) {
        rt_copy *victim = &g->copies[0];
        for (int i = 0; i < MAX_COPIES; i++) {
            rt_copy *c = &g->copies[i];
            if (!c->tex || (victim->tex && c->used < victim->used))
                victim = c;
        }
        if (victim->tex) { /* evicted: its pending pixels written first */
            flush_copy(g, cp, victim);
            drop_copy(g, victim, 1);
        }
        rc = victim;
    }
    int fresh = !rc->tex;
    if (fresh) {
        GLenum tf, tt;
        upload_fmt(s->ifmt, &tf, &tt);
        rc->tex = new_texture(g, s->ifmt, tf, tt, (int)pitch_aligned * g->scale, th * g->scale);
        rc->ifmt = s->ifmt, rc->w = (int)pitch_aligned, rc->h = (int)height_aligned, rc->th = th;
        rc->swz = 0;
        rc->swap_rb = swap_rb, rc->fmt = fmt, rc->endian = endian;
        rc->base = base, rc->bytes = bytes;
    }
    if (rc->pending) {
        /* one pending rectangle per copy: the new one when it holds the old one, the old one when it holds the new
         * one (the picture there is what memory would have), else the old one is written now */
        int holds_old = x0 <= rc->px0 && y0 <= rc->py0 && x1 >= rc->px1 && y1 >= rc->py1;
        int in_old = rc->px0 <= x0 && rc->py0 <= y0 && rc->px1 >= x1 && rc->py1 >= y1;
        /* WP-26: or two that share an edge whole (the two tiles of one picture): the one rectangle they make */
        int beside = rc->py0 == y0 && rc->py1 == y1 && (rc->px1 == x0 || rc->px0 == x1);
        int below = rc->px0 == x0 && rc->px1 == x1 && (rc->py1 == y0 || rc->py0 == y1);
        if (holds_old)
            rc->px0 = x0, rc->py0 = y0, rc->px1 = x1, rc->py1 = y1;
        else if (beside)
            rc->px0 = x0 < rc->px0 ? x0 : rc->px0, rc->px1 = x1 > rc->px1 ? x1 : rc->px1;
        else if (below)
            rc->py0 = y0 < rc->py0 ? y0 : rc->py0, rc->py1 = y1 > rc->py1 ? y1 : rc->py1;
        else if (!in_old)
            flush_copy(g, cp, rc);
    }
    frame_detach(g, rc); /* the swap's picture stays the swap's */
    blit_tex(g, s->tex, rc->tex, sx0, sy0, x1 - x0, y1 - y0, x0, y0, !x0 && !y0 && x1 == rc->w && y1 == rc->th);
    if (fresh)
        copy_rehash(g, cp, rc); /* of the memory as it is: a CPU write still shows */
    if (!rc->pending) {
        rc->pending = 1;
        g->npending++;
        rc->px0 = x0, rc->py0 = y0, rc->px1 = x1, rc->py1 = y1;
    }
    rc->used = ++g->copy_clock;
    rc->gen = ++g->copy_gen;
    rc->frame = g->swaps;
    g->st.resolves++;
    g->st.resolves_deferred++;
    if (g->nresolved < 16)
        g->resolved[g->nresolved++] = base;
    /* no epoch move: the textures taken from this copy, and those decoded from memory under it, are checked again */
    int idx = 1 + (int)(rc - g->copies);
    for (int i = 0; i < MAX_TEXTURES; i++) {
        texture *t = &g->textures[i];
        if (t->tex && t->checked == g->epoch && (t->copy ? t->copy == idx : t->lo < base + bytes && base < t->hi))
            t->checked = g->epoch - 1u;
    }
    return 1;
}

/* the source surface of a resolve: render target src (0-3) or depth (4), as last bound, else as the registers say */
static int resolve_source(xenos_gl *g, const xenos_cp *cp, uint32_t src)
{
    static const uint32_t info_reg[4] = {0x2001, 0x2003, 0x2004, 0x2005};
    uint32_t si = cp->regs[0x2000], pitch = si & 0x3FFF, msaa = (si >> 16) & 3;
    if (src < 4) {
        uint32_t info = cp->regs[info_reg[src]];
        int c = g->vx == 1 && g->vy == 1 ? g->cur[src] : -1; /* (WP-28: not an MSAA view's stand-in) */
        if (c >= 0 && g->surf[c].tex && g->surf[c].base == (info & 0xFFF) &&
            (g->surf[c].fmt & 255) == ((info >> 16) & 15) && (!pitch || g->surf[c].pitch == pitch))
            return c;
        c = get_surface(g, info & 0xFFF, pitch, (info >> 16) & 15, 0, msaa, 0, 0);
        sync_surface(g, c, NULL, 0);
        return c;
    }
    uint32_t dinfo = cp->regs[0x2002];
    int c = g->vx == 1 && g->vy == 1 ? g->cur_ds : -1;
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
    GS_DIRTY(g); /* WP-24: GL state apply_state shadows changes here */
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
    use_program(g, g->dprog);
    g->glActiveTexture(GL_TEXTURE0);
    unit_plain(g, 0); /* WP-24 */
    g->glBindTexture(GL_TEXTURE_2D, s->tex);
    g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    g->glUniform1i(g->glGetUniformLocation(g->dprog, "d"), 0);
    g->glUniform3i(g->dprog_at, x0, y0, g->scale);
    bind_vao(g, g->vao);
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
    viewport_restore(g);
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
    GS_DIRTY(g); /* WP-24: GL state apply_state shadows changes here */
    int dc = alias_class(g, d), sc = alias_class(g, s);
    if (dc < 0 || sc < 0 || fmt_wide(d->fmt & 255) != fmt_wide(s->fmt & 255) || !alias_program(g))
        return 0;
    pclear_flush(g, s);
    pclear_flush(g, d);
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
    use_program(g, g->aprog);
    g->glActiveTexture(GL_TEXTURE0);
    unit_plain(g, 0); /* WP-24 */
    g->glBindTexture(GL_TEXTURE_2D, s->tex);
    g->glUniform1i(g->glGetUniformLocation(g->aprog, "t"), 0);
    g->glUniform3i(g->aprog_off, 0, -dy * k, 0);
    g->glUniform1i(g->aprog_sc, sc);
    g->glUniform1i(g->aprog_dc, dc);
    bind_vao(g, g->vao);
    g->glDrawArrays(4 /* GL_TRIANGLES */, 0, 3);
    g->glDisable(GL_SCISSOR_TEST);
    g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    g->glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    return 1;
}

/* rows y0..y1 of depth surface d from depth surface s */
static void alias_depth(xenos_gl *g, const surface *d, const surface *s, int dy, int y0, int y1)
{
    GS_DIRTY(g); /* WP-24: GL state apply_state shadows changes here */
    pclear_flush(g, s);
    pclear_flush(g, d);
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

/* rows y0..y1 of a surface whose eDRAM was overwritten in a way that cannot be carried over: its texture, or its
 * multisampled buffer rb when that is what is drawn into from now on */
static void alias_clear_to(xenos_gl *g, const surface *d, int y0, int y1, GLuint rb)
{
    GS_DIRTY(g); /* WP-24: GL state apply_state shadows changes here */
    pclear_flush(g, d); /* in their order */
    int k = g->scale;
    GLenum att = d->kind == 2 ? GL_DEPTH_STENCIL_ATTACHMENT : GL_COLOR_ATTACHMENT0;
    g->glBindFramebuffer(GL_FRAMEBUFFER, g->rfbo);
    if (rb)
        g->glFramebufferRenderbuffer(GL_FRAMEBUFFER, att, GL_RENDERBUFFER, rb);
    else
        g->glFramebufferTexture2D(GL_FRAMEBUFFER, att, GL_TEXTURE_2D, d->tex, 0);
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
    if (rb)
        g->glFramebufferRenderbuffer(GL_FRAMEBUFFER, att, GL_RENDERBUFFER, 0);
    else
        g->glFramebufferTexture2D(GL_FRAMEBUFFER, att, GL_TEXTURE_2D, 0, 0);
    g->glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
}

/* WP-17: a surface's pending clear done now, through rfbo (its buffer: rb when it has one, else tex) */
static void pclear_flush(xenos_gl *g, const surface *cs)
{
    GS_DIRTY(g); /* WP-24: GL state apply_state shadows changes here */
    surface *s = &g->surf[cs - g->surf];
    if (!s->pclear)
        return;
    s->pclear = 0;
    int k = g->scale, depth = s->kind == 2;
    GLenum att = depth ? GL_DEPTH_STENCIL_ATTACHMENT : GL_COLOR_ATTACHMENT0;
    g->glBindFramebuffer(GL_FRAMEBUFFER, g->rfbo);
    if (s->rb)
        g->glFramebufferRenderbuffer(GL_FRAMEBUFFER, att, GL_RENDERBUFFER, s->rb);
    else
        g->glFramebufferTexture2D(GL_FRAMEBUFFER, att, GL_TEXTURE_2D, s->tex, 0);
    g->glEnable(GL_SCISSOR_TEST);
    g->glScissor(s->pc_x0 * k, s->pc_y0 * k, (s->pc_x1 - s->pc_x0) * k, (s->pc_y1 - s->pc_y0) * k);
    if (depth) {
        g->glDepthMask(1);
        g->glStencilMaskSeparate(GL_FRONT_AND_BACK, 0xFF);
        g->glClearBufferfi(GL_DEPTH_STENCIL, 0, s->pc_depth, s->pc_stencil);
    } else {
        static const GLenum b0 = GL_COLOR_ATTACHMENT0;
        g->glDrawBuffers(1, &b0);
        g->glColorMask(1, 1, 1, 1);
        g->glClearBufferfv(0x1800 /* GL_COLOR */, 0, s->pc_colour);
    }
    g->glDisable(GL_SCISSOR_TEST);
    if (s->rb)
        g->glFramebufferRenderbuffer(GL_FRAMEBUFFER, att, GL_RENDERBUFFER, 0);
    else
        g->glFramebufferTexture2D(GL_FRAMEBUFFER, att, GL_TEXTURE_2D, 0, 0);
    g->glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
}

/* WP-17: a resolve's clear of rectangle (x0, y0)-(x1, y1) of surface si left pending (bookkeeping as if done). A
 * pending one it does not hold is done first. */
static void pclear_set(xenos_gl *g, int si, int x0, int y0, int x1, int y1, const float *colour, float depth,
                       int stencil)
{
    surface *s = &g->surf[si];
    if (s->pclear && !(x0 <= s->pc_x0 && y0 <= s->pc_y0 && x1 >= s->pc_x1 && y1 >= s->pc_y1))
        pclear_flush(g, s);
    s->pclear = 1;
    s->pc_x0 = x0, s->pc_y0 = y0, s->pc_x1 = x1, s->pc_y1 = y1;
    if (colour)
        memcpy(s->pc_colour, colour, sizeof s->pc_colour);
    s->pc_depth = depth, s->pc_stencil = stencil;
    g->st.pending_clears++;
}

/* WP-17: in bind_targets, the pending clear of surface si where the draw framebuffer has it (colour draw buffer
 * `slot`, or the depth / stencil attachment for -1): scissored, or the whole attachment invalidated and cleared */
static void pclear_bound(xenos_gl *g, int si, int slot)
{
    GS_DIRTY(g); /* WP-24: GL state apply_state shadows changes here */
    surface *s = &g->surf[si];
    if (!s->pclear)
        return;
    s->pclear = 0;
    int k = g->scale;
    if (!s->pc_x0 && !s->pc_y0 && s->pc_x1 >= s->w && s->pc_y1 >= s->h) {
        tiler_discard(g, slot >= 0 ? 1u << slot : 0, slot < 0);
        g->glDisable(GL_SCISSOR_TEST);
    } else {
        g->glEnable(GL_SCISSOR_TEST);
        g->glScissor(s->pc_x0 * k, s->pc_y0 * k, (s->pc_x1 - s->pc_x0) * k, (s->pc_y1 - s->pc_y0) * k);
    }
    if (slot < 0) {
        g->glDepthMask(1);
        g->glStencilMaskSeparate(GL_FRONT_AND_BACK, 0xFF);
        g->glClearBufferfi(GL_DEPTH_STENCIL, 0, s->pc_depth, s->pc_stencil);
    } else {
        g->glColorMask(1, 1, 1, 1);
        g->glClearBufferfv(0x1800 /* GL_COLOR */, slot, s->pc_colour);
    }
    g->glDisable(GL_SCISSOR_TEST);
}

static void alias_clear(xenos_gl *g, const surface *d, int y0, int y1)
{
    alias_clear_to(g, d, y0, y1, 0);
}

/* the clears deferred for surface si (all for -1) done now through rfbo, in their order: before another write to
 * the surface (an alias copy) or when bind_targets finds no complete framebuffer (use_ms: on the multisampled buffer
 * the draw would have used) */
static void deferred_clears_flush(xenos_gl *g, int si, int use_ms)
{
    int n = 0;
    for (int c = 0; c < g->ndclear; c++) {
        if (si >= 0 && g->dclear[c].si != si) {
            g->dclear[n++] = g->dclear[c];
            continue;
        }
        const surface *d = &g->surf[g->dclear[c].si];
        alias_clear_to(g, d, g->dclear[c].y0, g->dclear[c].y1, use_ms ? d->rb : 0);
    }
    g->ndclear = n;
}

/* the multisampled buffer filled from the texture, every sample of a pixel alike: a pass, as a blit cannot write a
 * multisampled buffer. GLES 3.0 cannot sample stencil: a depth buffer's stencil starts at 0 again (counted once it
 * had contents). */
static void surface_reload(xenos_gl *g, surface *s)
{
    GS_DIRTY(g); /* WP-24: GL state apply_state shadows changes here */
    pclear_flush(g, s); /* rb takes tex: the clear first, on rb, as it came first */
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
    unit_plain(g, 0); /* WP-24 */
    g->glBindTexture(GL_TEXTURE_2D, s->tex);
    bind_vao(g, g->vao);
    if (depth) {
        static const GLenum none = 0;
        g->glDrawBuffers(1, &none);
        g->glEnable(GL_DEPTH_TEST);
        g->glDepthFunc(GL_NEVER + 7 /* GL_ALWAYS */);
        g->glDepthRangef(0.0f, 1.0f); /* WP-27: the depth as it is (some drivers clamp gl_FragDepth to the range) */
        g->glDepthMask(1);
        g->glStencilMaskSeparate(GL_FRONT_AND_BACK, 0xFF);
        g->glClearStencil(0);
        g->glClear(GL_STENCIL_BUFFER_BIT);
        use_program(g, g->zprog);
        g->glUniform1i(g->glGetUniformLocation(g->zprog, "d"), 0);
        if (s->written)
            g->st.edram_stale++;
    } else {
        static const GLenum b0 = GL_COLOR_ATTACHMENT0;
        g->glDrawBuffers(1, &b0);
        g->glDisable(GL_DEPTH_TEST);
        g->glColorMask(1, 1, 1, 1);
        use_program(g, g->aprog);
        g->glUniform1i(g->glGetUniformLocation(g->aprog, "t"), 0);
        g->glUniform3i(g->aprog_off, 0, 0, 0);
        g->glUniform1i(g->aprog_sc, -1);
        g->glUniform1i(g->aprog_dc, -1);
    }
    g->glDrawArrays(4 /* GL_TRIANGLES */, 0, 3);
    g->glDisable(GL_DEPTH_TEST);
    g->glFramebufferRenderbuffer(GL_FRAMEBUFFER, att, GL_RENDERBUFFER, 0);
    g->glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    viewport_restore(g);
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
            deferred_clears_flush(g, si, 0); /* rows cleared before them stay before them */
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
        if (r1 > r0 && g->defer_clears && g->ndclear < (int)(sizeof g->dclear / sizeof g->dclear[0])) {
            g->dclear[g->ndclear].si = si, g->dclear[g->ndclear].y0 = r0, g->dclear[g->ndclear].y1 = r1;
            g->ndclear++;
        } else if (r1 > r0) {
            alias_clear(g, d, r0, r1);
        }
        g->st.edram_stale++;
    }
    if (touched) {
        d->written = newest;
        viewport_restore(g);
    }
}

/* The stencil of a depth resolve (XBA-296). GLES 3.0 cannot sample stencil, so it is read a bit at a time: for each of
 * the 8 bits a full-target pass with the stencil test on that bit marks the pixels that have it in one colour channel
 * (four bits a read-back, WP-36), read back at the host texel of each guest pixel (the one read_depth takes). Returns
 * w * h values, top row first, or NULL. */
static const char STENCIL_FS[] =
    "#version 300 es\nprecision mediump float;\nout vec4 c;\nvoid main() { c = vec4(1.0); }\n";

/* the stencil passes' program (made at first use; 0 when it cannot be) */
static GLuint stencil_prog(xenos_gl *g)
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
    return g->sprog;
}

static const uint8_t *read_stencil(xenos_gl *g, const surface *s, int x0, int y0, int w, int h)
{
    GS_DIRTY(g); /* WP-24: GL state apply_state shadows changes here */
    if (!stencil_prog(g))
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
    use_program(g, g->sprog);
    bind_vao(g, g->vao);
    /* WP-36: four bits a read-back, one per colour channel (the pass of bit 4 * half + c writes channel c alone): 2
     * read-backs and CPU passes instead of 8 */
    for (int half = 0; half < 2; half++) {
        g->glColorMask(1, 1, 1, 1);
        g->glClearColor(0, 0, 0, 0);
        g->glClear(GL_COLOR_BUFFER_BIT);
        for (int c = 0; c < 4; c++) {
            g->glColorMask(c == 0, c == 1, c == 2, c == 3);
            g->glStencilFuncSeparate(GL_FRONT_AND_BACK, 0x0202 /* GL_EQUAL */, 0xFF, 1u << (4 * half + c));
            g->glDrawArrays(4 /* GL_TRIANGLES */, 0, 3);
        }
        g->glReadPixels(0, y0 * sc, hw, h * sc, GL_RGBA, GL_UNSIGNED_BYTE, rows);
        for (int y = 0; y < h; y++) {
            const uint8_t *r = rows + ((size_t)y * sc * hw + (size_t)x0 * sc) * 4;
            uint8_t *o = out + (size_t)y * w;
            for (int x = 0; x < w; x++, r += (size_t)sc * 4)
                o[x] |= (uint8_t)(((r[0] != 0) | (r[1] != 0) << 1 | (r[2] != 0) << 2 | (r[3] != 0) << 3) << (4 * half));
        }
    }
    g->glColorMask(1, 1, 1, 1);
    g->glDisable(GL_STENCIL_TEST);
    g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
    g->glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    viewport_restore(g);
    return out;
}

/* A depth resolve's depth and stencil in one read-back (WP-38): the stencil's 8 bits go into the alpha channel of the
 * stencil target (stex) by 8 passes on the GPU, each with the stencil test on one bit adding 2^bit / 255 (blended,
 * alpha alone, inside the resolved rectangle's host texels), then one pass packs, for each guest pixel, its depth as
 * read_depth does (the same expression, the same host texel) in red / green / blue and that alpha in alpha. The w * h
 * values read back are depth | stencil << 24 (the RGBA bytes as a little-endian word); NULL when a pass is missing
 * (read_depth and read_stencil then, as before). */
static const char DEPTH_STENCIL_FS[] =
    "#version 300 es\nprecision highp float;\nprecision highp int;\nuniform highp sampler2D d;\n"
    "uniform highp sampler2D s;\nuniform ivec3 at;\nout vec4 c;\n"
    "void main() { ivec2 q = (ivec2(gl_FragCoord.xy) + at.xy) * at.z;\n"
    " uint v = uint(clamp(texelFetch(d, q, 0).r, 0.0, 1.0) * 16777215.0 + 0.5);\n"
    " c = vec4(float(v & 255u), float((v >> 8) & 255u), float(v >> 16), 0.0) / 255.0;\n"
    " c.a = texelFetch(s, q, 0).a; }\n";

static const uint32_t *read_depth_stencil(xenos_gl *g, const surface *s, int x0, int y0, int w, int h)
{
    GS_DIRTY(g); /* WP-24: GL state apply_state shadows changes here */
    if (!g->dsprog && !g->dsprog_failed) {
        GLuint v = compile(g, GL_VERTEX_SHADER, DEPTH_VS, "depth stencil resolve vs");
        GLuint f = compile(g, GL_FRAGMENT_SHADER, DEPTH_STENCIL_FS, "depth stencil resolve fs");
        GLint ok = 0;
        if (v && f) {
            g->dsprog = g->glCreateProgram();
            g->glAttachShader(g->dsprog, v);
            g->glAttachShader(g->dsprog, f);
            g->glLinkProgram(g->dsprog);
            g->glGetProgramiv(g->dsprog, GL_LINK_STATUS, &ok);
        }
        if (v)
            g->glDeleteShader(v);
        if (f)
            g->glDeleteShader(f);
        if (!ok) {
            if (g->dsprog)
                g->glDeleteProgram(g->dsprog);
            g->dsprog = 0;
            g->dsprog_failed = 1;
            return NULL;
        }
        g->dsprog_at = g->glGetUniformLocation(g->dsprog, "at");
        use_program(g, g->dsprog);
        g->glUniform1i(g->glGetUniformLocation(g->dsprog, "d"), 0);
        g->glUniform1i(g->glGetUniformLocation(g->dsprog, "s"), 1);
    }
    if (!g->dsprog)
        return NULL;
    if (!stencil_prog(g))
        return NULL;
    int sc = g->scale, hw = s->w * sc, hh = s->h * sc;
    if (!g->stex || g->stex_w != hw || g->stex_h != hh) {
        if (g->stex)
            g->glDeleteTextures(1, &g->stex);
        g->stex = new_texture(g, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, hw, hh);
        g->stex_w = hw, g->stex_h = hh;
    }
    if (!g->dtex || g->dtex_w < w || g->dtex_h < h) {
        if (g->dtex)
            g->glDeleteTextures(1, &g->dtex);
        g->dtex_w = w > g->dtex_w ? w : g->dtex_w, g->dtex_h = h > g->dtex_h ? h : g->dtex_h;
        g->dtex = new_texture(g, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, g->dtex_w, g->dtex_h);
    }
    size_t n = (size_t)w * h;
    uint32_t *out = scratch_buf(g, SCRATCH_PIXELS, n * 4);
    if (!out)
        return NULL;
    static const GLenum b0 = GL_COLOR_ATTACHMENT0;
    /* the stencil, bit by bit, into stex's alpha */
    g->glBindFramebuffer(GL_FRAMEBUFFER, g->rfbo);
    g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g->stex, 0);
    g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, s->tex, 0);
    g->glDrawBuffers(1, &b0);
    g->glDisable(GL_DEPTH_TEST);
    g->glDisable(GL_CULL_FACE);
    g->glDepthMask(0);
    g->glViewport(0, 0, hw, hh);
    g->glEnable(GL_SCISSOR_TEST);
    g->glScissor(x0 * sc, y0 * sc, w * sc, h * sc);
    g->glColorMask(0, 0, 0, 1);
    g->glClearColor(0, 0, 0, 0);
    g->glClear(GL_COLOR_BUFFER_BIT);
    g->glEnable(GL_STENCIL_TEST);
    g->glStencilMaskSeparate(GL_FRONT_AND_BACK, 0);
    g->glStencilOpSeparate(GL_FRONT_AND_BACK, 0x1E00, 0x1E00, 0x1E00); /* GL_KEEP */
    g->glEnable(GL_BLEND);
    g->glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    g->glBlendFuncSeparate(0 /* GL_ZERO */, GL_ONE, 0x8003 /* GL_CONSTANT_ALPHA */, GL_ONE);
    use_program(g, g->sprog);
    bind_vao(g, g->vao);
    for (int b = 0; b < 8; b++) { /* alpha + 2^b / 255 where the bit is set: exact in 8 bits (the sum of the bits) */
        g->glBlendColor(0, 0, 0, (float)(1u << b) / 255.0f);
        g->glStencilFuncSeparate(GL_FRONT_AND_BACK, 0x0202 /* GL_EQUAL */, 0xFF, 1u << b);
        g->glDrawArrays(4 /* GL_TRIANGLES */, 0, 3);
    }
    g->glDisable(GL_BLEND);
    g->glDisable(GL_STENCIL_TEST);
    g->glDisable(GL_SCISSOR_TEST);
    g->glColorMask(1, 1, 1, 1);
    g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
    /* depth and that alpha, packed for the guest pixels */
    g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g->dtex, 0);
    g->glViewport(0, 0, w, h);
    use_program(g, g->dsprog);
    g->glActiveTexture(GL_TEXTURE0 + 1);
    unit_plain(g, 1);
    g->glBindTexture(GL_TEXTURE_2D, g->stex);
    g->glActiveTexture(GL_TEXTURE0);
    unit_plain(g, 0);
    g->glBindTexture(GL_TEXTURE_2D, s->tex);
    g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    g->glUniform3i(g->dsprog_at, x0, y0, sc);
    g->glDrawArrays(4 /* GL_TRIANGLES */, 0, 3);
    uint8_t *bytes = (uint8_t *)out;
    g->glReadBuffer(GL_COLOR_ATTACHMENT0);
    g->glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, bytes);
#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
    for (size_t i = 0; i < n; i++)
        out[i] = bytes[4 * i] | (uint32_t)bytes[4 * i + 1] << 8 | (uint32_t)bytes[4 * i + 2] << 16 |
                 (uint32_t)bytes[4 * i + 3] << 24;
#endif
    g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    g->glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    viewport_restore(g);
    return out;
}

static void resolve_copy(xenos_gl *g, const xenos_cp *cp);

/* a resolve, timed whole (resolve_ns); resolve_copy times its read back and its write */
static void resolve(xenos_gl *g, const xenos_cp *cp)
{
    uint64_t t0 = mono_ns();
    g->in_resolve = 1;
    resolve_copy(g, cp);
    g->in_resolve = 0;
    g->st.resolve_ns += mono_ns() - t0;
}

static void resolve_copy(xenos_gl *g, const xenos_cp *cp)
{
    GS_DIRTY(g); /* WP-24: GL state apply_state shadows changes here */
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
    pclear_flush(g, s); /* WP-17: an earlier resolve's clear, before the copy reads the picture */
    surface_flush(g, s); /* multisampled: the samples averaged into the picture the copy reads */
    if ((cp->regs[0x2205] >> 16) & 1) {
        /* WP-26: the rectangle is in window coordinates: PA_SC_WINDOW_OFFSET moves it onto the surface, as it moves
         * the vertices (Xenia's GetResolveInfo). The destination stays where the registers put it: the right tile of
         * a two-tile picture is resolved from 960..1920 with the offset -960, into a base already moved by 960 px */
        uint32_t wo = cp->regs[0x2080];
        int wx = (int32_t)(wo << 17) >> 17, wy = (int32_t)(wo << 1) >> 17;
        x0 += wx, x1 += wx, y0 += wy, y1 += wy;
    }
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
        int writable = bpp && endian < 4 && cp->write32 && cp->read32;
        int dx = 0, dy = 0; /* WP-26: the copy holding the destination, the rectangle (dx, dy) further in there */
        rt_copy *into = writable && s->kind != 2 && !(base & 0xFFF) && !bias
                            ? copy_holding(g, cp, s, base, pitch_aligned, fmt, endian, (int)swap_rb, bpp, x1, y1, &dx, &dy)
                            : NULL;
        if (writable && g->lazy && resolve_lazy(g, cp, s, base, pitch_aligned, height, fmt, endian, (int)swap_rb, bias,
                                                bpp, x0, y0, x1, y1, into, dx, dy))
            goto cleared;
        if (writable && g->npending) { /* written over: what is pending under it first */
            uint32_t h = height ? height : (uint32_t)y1;
            flush_range(g, cp, base, base + pitch_aligned * ((h + 31) & ~31u) * (uint32_t)bpp);
        }
        uint64_t tr = mono_ns();
        const uint8_t *px = writable
                                ? read_raw(g, s->tex, s->ifmt, x0, y0, x1 - x0, y1 - y0)
                                : NULL;
        if (px) {
            uint64_t tw = mono_ns();
            g->st.resolve_read_ns += tw - tr;
            const resolve_dest d = {base, pitch_aligned, fmt, endian, (int)swap_rb, bias};
            resolve_write(cp, &d, px, pix_layout(s->ifmt), x0, y0, x1 - x0, y1 - y0);
            g->st.resolve_write_ns += mono_ns() - tw;
            g->st.resolved_pixels += (uint64_t)(x1 - x0) * (uint64_t)(y1 - y0);
            g->st.resolves++;
            g->epoch++;
            if (g->nresolved < 16)
                g->resolved[g->nresolved++] = base & 0x1FFFF000u;
            /* the host picture at the internal resolution. A resolve that swaps red and blue (what Direct3D asks for
             * every A8R8G8B8 destination) keeps the picture unswapped and says so: whoever samples it exchanges the
             * two in its swizzle (sampler_state, the frame at the swap). */
            uint32_t given = height;
            if (!height)
                height = (uint32_t)y1;
            uint32_t height_aligned = (height + 31) & ~31u;
            int th = copy_rows(given, y1, height_aligned);
            if (into && into->tex) {
                /* WP-26: the picture of the copy holding the destination takes the rectangle where it lies in it
                 * (memory was written above at the destination's own base: the same bytes) */
                frame_detach(g, into);
                blit_tex(g, s->tex, into->tex, x0, y0, x1 - x0, y1 - y0, x0 + dx, y0 + dy, 0);
                copy_rehash(g, cp, into);
                into->used = ++g->copy_clock;
                into->gen = ++g->copy_gen;
                into->frame = g->swaps;
                goto cleared;
            }
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
                    copy_tex_gone(g, rc);
                    g->glDeleteTextures(1, &rc->tex);
                    memset(rc, 0, sizeof *rc);
                }
            } else {
                if (!rc)
                    rc = victim;
                if (rc->tex && (rc->ifmt != s->ifmt || rc->w != (int)pitch_aligned || rc->h != (int)height_aligned ||
                                rc->th != th || rc->swap_rb != (int)swap_rb)) {
                    copy_tex_gone(g, rc);
                    g->glDeleteTextures(1, &rc->tex);
                    rc->tex = 0;
                }
                rc->swap_rb = (int)swap_rb;
                rc->fmt = fmt, rc->endian = endian;
                if (!rc->tex) {
                    GLenum tf, tt;
                    upload_fmt(s->ifmt, &tf, &tt);
                    rc->tex = new_texture(g, s->ifmt, tf, tt, (int)pitch_aligned * sc, th * sc);
                    rc->ifmt = s->ifmt, rc->w = (int)pitch_aligned, rc->h = (int)height_aligned, rc->th = th;
                    rc->swz = 0;
                }
                rc->base = base & 0x1FFFF000u;
                rc->bytes = pitch_aligned * height_aligned * (uint32_t)bpp;
                int cw = x1 < rc->w ? x1 - x0 : rc->w - x0, ch = y1 < rc->th ? y1 - y0 : rc->th - y0;
                frame_detach(g, rc); /* the swap's picture stays the swap's */
                if (cw > 0 && ch > 0)
                    blit_tex(g, s->tex, rc->tex, x0, y0, cw, ch, x0, y0, !x0 && !y0 && cw == rc->w && ch == rc->th);
                copy_rehash(g, cp, rc);
                rc->used = ++g->copy_clock;
                rc->gen = ++g->copy_gen;
                rc->frame = g->swaps;
            }
        } else {
            unsupported(g, "resolve destination format");
        }
    } else if (cmd <= 1) { /* depth out: 24_8 (depth in the high 24 bits, stencil in the low 8), tiled 32-bit */
        uint32_t info = cp->regs[0x231B], fmt = (info >> 7) & 63, endian = info & 7;
        uint32_t pitch = cp->regs[0x231A] & 0x3FFF, base = cp->regs[0x2319] & 0x1FFFFFFFu;
        uint64_t tr = mono_ns();
        int ok = (fmt == 22 || fmt == 6 || fmt == 33) && endian < 4 && cp->write32 && cp->read32;
        /* WP-38: depth and stencil packed on the GPU, one read-back; else depth, then the stencil (read_stencil) */
        const uint32_t *dsv = ok ? read_depth_stencil(g, s, x0, y0, x1 - x0, y1 - y0) : NULL;
        const uint32_t *dv = ok && !dsv ? read_depth(g, s, x0, y0, x1 - x0, y1 - y0) : NULL;
        if (dsv || dv) {
            uint32_t pitch_aligned = (pitch + 31) & ~31u;
            if (g->npending) {
                uint32_t h = (cp->regs[0x231A] >> 16) & 0x3FFF;
                h = h > (uint32_t)y1 ? h : (uint32_t)y1;
                flush_range(g, cp, base, base + pitch_aligned * ((h + 31) & ~31u) * 4);
            }
            const uint8_t *sv = dsv ? NULL : read_stencil(g, s, x0, y0, x1 - x0, y1 - y0);
            uint64_t tw = mono_ns();
            g->st.resolve_read_ns += tw - tr;
            if (!dsv && !sv)
                unsupported(g, "stencil of a depth resolve (written as 0)");
            depth_write(cp, base, pitch_aligned, endian, dsv ? dsv : dv, sv, dsv != NULL, x0, y0, x1 - x0, y1 - y0);
            g->st.resolve_write_ns += mono_ns() - tw;
            g->st.resolved_pixels += (uint64_t)(x1 - x0) * (uint64_t)(y1 - y0);
            g->st.resolves++;
            g->st.depth_resolves++;
            g->epoch++;
        } else {
            unsupported(g, "depth resolve destination format");
        }
    }
cleared:;
    int clear_c = (ctl >> 8) & 1, clear_d = (ctl >> 9) & 1;
    if (clear_c || clear_d) {
        /* WP-17: left pending, done by the next pass that draws into the surface (pclear_set). RECOMP_X360_TILER=0:
         * now, on the draw FBO when it has the surface attached (complete, its buffer the one cleared here: the
         * multisampled one when the surface has one), through its draw buffer: no pass through rfbo (WP-11) */
        if (clear_c && src < 4) { /* the resolved render target (none when the source is depth) */
            float colour[4];
            clear_colour(s->fmt, cp->regs[0x231E], cp->regs[0x231F], colour);
            if (s->rb) {
                if (s->ms_reload)
                    surface_reload(g, s);
                s->ms_dirty = 1;
            }
            if (g->tiler != TILER_OFF) {
                pclear_set(g, si, x0, y0, x1, y1, colour, 0.0f, 0);
            } else {
                int slot = -1;
                for (int i = 0; i < 4; i++)
                    slot = g->cur[i] == si ? i : slot;
                if (slot < 0 || !g->fbo_checked || (s->rb && !g->cur_ms))
                    slot = -1;
                if (slot >= 0) {
                    g->glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
                } else {
                    g->glBindFramebuffer(GL_FRAMEBUFFER, g->rfbo);
                    if (s->rb)
                        g->glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, s->rb);
                    else
                        g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->tex, 0);
                    static const GLenum b0 = GL_COLOR_ATTACHMENT0;
                    g->glDrawBuffers(1, &b0);
                }
                g->glEnable(GL_SCISSOR_TEST);
                g->glScissor(x0 * sc, y0 * sc, (x1 - x0) * sc, (y1 - y0) * sc);
                g->glColorMask(1, 1, 1, 1);
                g->glClearBufferfv(0x1800 /* GL_COLOR */, slot >= 0 ? slot : 0, colour);
                if (slot < 0)
                    g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
            }
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
            float depth = (float)(dc >> 8) / 16777215.0f;
            if (g->tiler != TILER_OFF) {
                pclear_set(g, di, x0, y0, x1, y1, NULL, depth, (GLint)(dc & 0xFF));
            } else {
                int attached = g->cur_ds == di && g->fbo_checked && (!ds->rb || g->cur_ms);
                if (attached) {
                    g->glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
                } else {
                    g->glBindFramebuffer(GL_FRAMEBUFFER, g->rfbo);
                    if (ds->rb)
                        g->glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                                                     ds->rb);
                    else
                        g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, ds->tex,
                                                  0);
                }
                g->glEnable(GL_SCISSOR_TEST);
                g->glScissor(x0 * sc, y0 * sc, (x1 - x0) * sc, (y1 - y0) * sc);
                g->glDepthMask(1);
                g->glStencilMaskSeparate(GL_FRONT_AND_BACK, 0xFF);
                if (attached) {
                    g->glClearBufferfi(GL_DEPTH_STENCIL, 0, depth, (GLint)(dc & 0xFF));
                } else {
                    g->glClearDepthf(depth);
                    g->glClearStencil((GLint)(dc & 0xFF));
                    g->glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
                    g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
                }
            }
            g->surf[di].written = ++g->write_clock;
            g->surf[di].dirty_h = y1 > g->surf[di].dirty_h ? y1 : g->surf[di].dirty_h;
        }
        g->glDisable(GL_SCISSOR_TEST);
        g->glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
        g->title_clears = 1; /* the title clears through resolves: swaps stop clearing */
    }
}

/* one vertex buffer of a computed-index fetch converted to vec4s into the texture bound on the active unit;
 * 0 when its format is not handled */
static int upload_vertex_buffer(xenos_gl *g, const xenos_cp *cp, const attr *at, uint32_t base, uint32_t endian,
                                uint32_t count)
{
    uint32_t rows = (count + 1023) / 1024, wid = count < 1024 ? count : 1024;
    size_t total = (size_t)wid * rows * 4;
    float *buf = scratch_buf(g, SCRATCH_VBTEX, total * sizeof(float));
    if (!buf || !fetch_vertices(cp, base, endian, at, 0, count, buf, 4))
        return 0;
    g->st.vertices_fetched += count;
    memset(buf + (size_t)count * 4, 0, (total - (size_t)count * 4) * sizeof(float)); /* the last row's tail */
    g->glTexImage2D(GL_TEXTURE_2D, 0, (GLint)GL_RGBA32F, (GLsizei)wid, (GLsizei)rows, 0, GL_RGBA, GL_FLOAT, buf);
    g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    return 1;
}

/* the cached texture of a computed-index vertex buffer, converted again only when its bytes changed. As for textures,
 * the source is hashed again once per epoch (each swap, resolve and memory export), not at every draw: hashing at
 * every use cost 6 ms a frame on the Adventure Time dungeon scene (130 uses of buffers of about 1 MB). A title that
 * rewrites such a buffer between two draws with no swap or resolve between them would draw the old contents.
 * *fresh: the texture must be (re)filled. */
static vb_cached *vertex_buffer_cached(xenos_gl *g, const xenos_cp *cp, const attr *at, uint32_t base, uint32_t c1,
                                       uint32_t count, uint32_t span, int *fresh)
{
    attr key = *at;
    key.fetch_constant = 0;
    vb_cached *hit = NULL, *victim = &g->vbc[0];
    for (int i = 0; i < MAX_VB_CACHE; i++) {
        vb_cached *c = &g->vbc[i];
        if (c->tex && c->base == base && c->c1 == c1 && c->count == count && !memcmp(&c->a, &key, sizeof key)) {
            hit = c;
            break;
        }
        if (!c->tex || (victim->tex && c->used < victim->used))
            victim = c;
    }
    *fresh = 0;
    if (hit) {
        if (hit->checked != g->epoch) {
            uint64_t h = span_hash(cp->mem + base, span, 0);
            if (h != hit->hash)
                hit->hash = h, *fresh = 1;
            hit->checked = g->epoch;
        }
        hit->used = ++g->vbc_clock;
        return hit;
    }
    /* room under the size budget: the oldest go, but none bound for this draw already */
    size_t bytes = (size_t)(count < 1024 ? count : 1024) * ((count + 1023) / 1024) * 16;
    g->vbc_bytes -= victim->bytes, victim->bytes = 0;
    while (g->vbc_bytes + bytes > VB_CACHE_BYTES) {
        vb_cached *old = NULL;
        for (int i = 0; i < MAX_VB_CACHE; i++) {
            vb_cached *c = &g->vbc[i];
            if (c->tex && c != victim && c->used <= g->vbc_draw && (!old || c->used < old->used))
                old = c;
        }
        if (!old)
            break;
        g->vbc_bytes -= old->bytes;
        g->glDeleteTextures(1, &old->tex);
        memset(old, 0, sizeof *old);
    }
    if (!victim->tex)
        g->glGenTextures(1, &victim->tex);
    victim->bytes = bytes, g->vbc_bytes += bytes;
    victim->base = base, victim->c1 = c1, victim->a = key, victim->count = count;
    victim->hash = span_hash(cp->mem + base, span, 0);
    victim->checked = g->epoch;
    victim->used = ++g->vbc_clock;
    *fresh = 1;
    return victim;
}

/* the vertex buffers a program fetches at computed indices: each whole buffer (the fetch constant's size in dwords
 * over the stride) converted to vec4s in an RGBA32F texture of 1024 elements a row, on the units after the
 * samplers. With the guest memory mapped (cp->mem) the textures are cached by address and layout and reused while
 * the source bytes do not change. 0 when a format is not handled. */
static int bind_vertex_buffers(xenos_gl *g, const xenos_cp *cp, const program *p)
{
    g->vbc_draw = g->vbc_clock;
    for (int i = 0; i < p->nvb; i++) {
        const attr *at = &p->vbufs[i];
        uint32_t c0 = cp->regs[0x4800 + 2 * at->fetch_constant], c1 = cp->regs[0x4800 + 2 * at->fetch_constant + 1];
        uint32_t size = (c1 >> 2) & 0xFFFFFF, stride = at->stride ? at->stride : 1;
        uint32_t count = size / stride;
        if (!count)
            count = 1;
        if (count > (1u << 20))
            count = 1u << 20;
        uint32_t base = (c0 & ~3u) & 0x1FFFFFFFu;
        /* the bytes the conversion reads: up to 4 dwords from the last element's start */
        uint64_t span = 4 * ((uint64_t)at->offset + (uint64_t)(count - 1) * at->stride) + 16;
        if (g->npending) /* lazy resolves under the buffer: written first */
            flush_range(g, cp, base, (uint32_t)(base + span));
        g->glActiveTexture(GL_TEXTURE0 + (GLenum)(p->nsamplers + i));
        unit_plain(g, p->nsamplers + i); /* WP-24: texelFetch of a float texture: no filtering sampler there */
        if (cp->mem && (c0 & ~3u) < 0x20000000u && base + span <= 0x20000000u) {
            int fresh;
            vb_cached *c = vertex_buffer_cached(g, cp, at, base, c1, count, (uint32_t)span, &fresh);
            g->glBindTexture(GL_TEXTURE_2D, c->tex);
            if (fresh && !upload_vertex_buffer(g, cp, at, c0 & ~3u, c1 & 3, count)) {
                g->vbc_bytes -= c->bytes;
                g->glDeleteTextures(1, &c->tex);
                memset(c, 0, sizeof *c);
                g->glActiveTexture(GL_TEXTURE0);
                return 0;
            }
        } else {
            if (!g->vbtex[i])
                g->glGenTextures(1, &g->vbtex[i]);
            g->glBindTexture(GL_TEXTURE_2D, g->vbtex[i]);
            if (!upload_vertex_buffer(g, cp, at, c0 & ~3u, c1 & 3, count)) {
                g->glActiveTexture(GL_TEXTURE0);
                return 0;
            }
        }
        g->st.computed_fetches++;
    }
    g->glActiveTexture(GL_TEXTURE0);
    return 1;
}

/* rp kind 3 (WP-10): the capture stays on the GPU and the second pass reads it as uniform blocks of RECT_CHUNK
 * rectangles; kind 2: it is read back and the triangles are made here. The capture's vertex attributes are turned
 * off before a kind 3 pass: it has none. */
/* WP-29: rectangle lists (every Direct3D clear) go through a transform feedback capture of the vertex shader's
 * outputs, as points (draw_rects). On the Mac harness (ANGLE over Apple's GL) the capture holds the first vertex and
 * zeros, so every clear draws nothing. Checked once at start-up, the same way draw_rects captures (the shared vertex
 * array, gl_Position and 14 vectors interleaved, points, rasterizer discard): three known vertices drawn,
 * the capture read back and compared. Only logged ("rect lists OK" / "BROKEN (n of 3 vertices captured)"); never
 * fails the renderer. The GL state it touches is put back (or the shadows told). */
static void rect_line(xenos_gl *g, FILE *f)
{
    if (g->rects_captured == 3)
        fprintf(f, "xenos_gl: rect lists OK\n");
    else
        fprintf(f, "xenos_gl: rect lists BROKEN (%d of 3 vertices captured)\n", g->rects_captured);
}

static void rects_selftest(xenos_gl *g)
{
    enum { V = RECT_VARYINGS, N = 3 };
    static const char vs[] =
        "#version 300 es\nin vec4 xe_v0;\nout vec4 xe_i0, xe_i1, xe_i2, xe_i3, xe_i4, xe_i5, xe_i6, xe_i7, xe_i8, xe_i9, "
        "xe_i10, xe_i11, xe_i12, xe_i13;\nvoid main() {\n gl_Position = xe_v0;\n xe_i0 = xe_v0 + 1.0; "
        "xe_i1 = xe_v0 + 2.0; xe_i2 = xe_v0 + 3.0; xe_i3 = xe_v0 + 4.0; xe_i4 = xe_v0 + 5.0; xe_i5 = xe_v0 + 6.0; "
        "xe_i6 = xe_v0 + 7.0; xe_i7 = xe_v0 + 8.0; xe_i8 = xe_v0 + 9.0; xe_i9 = xe_v0 + 10.0; xe_i10 = xe_v0 + 11.0; "
        "xe_i11 = xe_v0 + 12.0; xe_i12 = xe_v0 + 13.0; xe_i13 = xe_v0 + 14.0;\n}\n";
    static const char *const vary[V] = {"gl_Position", "xe_i0", "xe_i1", "xe_i2",  "xe_i3",  "xe_i4",  "xe_i5",  "xe_i6",
                                        "xe_i7",       "xe_i8", "xe_i9", "xe_i10", "xe_i11", "xe_i12", "xe_i13"};
    static const float in[N][4] = {{0.25f, 0.5f, 0.75f, 1.0f}, {-0.5f, 0.125f, 0.5f, 1.0f}, {0.625f, -0.75f, 0.25f, 1.0f}};
    g->rects_captured = 0;
    GLuint v = compile(g, GL_VERTEX_SHADER, vs, "rect self-test");
    GLuint f = compile(g, GL_FRAGMENT_SHADER, CAPTURE_FS, "rect self-test capture");
    GLuint prog = v && f ? g->glCreateProgram() : 0;
    GLint ok = 0;
    if (prog) {
        g->glAttachShader(prog, v);
        g->glAttachShader(prog, f);
        g->glTransformFeedbackVaryings(prog, V, vary, GL_INTERLEAVED_ATTRIBS);
        g->glLinkProgram(prog);
        g->glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    }
    GLint loc = ok ? g->glGetAttribLocation(prog, "xe_v0") : -1;
    if (ok && loc >= 0 && loc < 32) {
        /* a complete framebuffer of its own, as draw_rects always has one (a draw without one does nothing, even
         * with the rasterizer off: a surfaceless context's default framebuffer); the draw FBO is bound again by the
         * next bind_targets */
        GLuint fb = 0, tex = new_texture(g, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 1, 1), tb = 0;
        /* and a capture buffer of its own: where the capture is broken, draw_rects's buffer holds what was in it
         * before (a driver may hand back the same memory), and these vertices would be drawn as a guest clear */
        g->glGenBuffers(1, &tb);
        g->glGenFramebuffers(1, &fb);
        g->glBindFramebuffer(GL_FRAMEBUFFER, fb);
        g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        use_program(g, prog);
        bind_vao(g, g->vao);
        g->glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
        g->glBufferData(GL_ARRAY_BUFFER, sizeof in, in, GL_STREAM_DRAW);
        attribs_enable(g, 1u << loc);
        g->glVertexAttribPointer((GLuint)loc, 4, GL_FLOAT, 0, 16, NULL);
        g->glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, tb);
        g->glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, (GLsizeiptr)N * V * 16, NULL, GL_STREAM_DRAW);
        g->glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, tb);
        g->glEnable(GL_RASTERIZER_DISCARD);
        g->glBeginTransformFeedback(0 /* GL_POINTS */);
        g->glDrawArrays(0, 0, N);
        g->glEndTransformFeedback();
        g->glDisable(GL_RASTERIZER_DISCARD);
        const float *cap = g->glMapBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0, (GLsizeiptr)N * V * 16, GL_MAP_READ_BIT);
        if (cap) {
            for (int k = 0; k < N; k++) {
                int same = 1;
                for (int i = 0; i < V; i++)
                    for (int c = 0; c < 4; c++)
                        same &= cap[(k * V + i) * 4 + c] == in[k][c] + (float)i;
                g->rects_captured += same;
            }
            g->glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER);
        }
        g->glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
        g->glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, 0);
        attribs_enable(g, 0);
        g->glUseProgram(0);
        g->cur_prog = 0;
        g->glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
        g->glDeleteFramebuffers(1, &fb);
        g->glDeleteTextures(1, &tex);
        g->glDeleteBuffers(1, &tb);
        g->glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
        static const float zero[N][4];
        g->glBufferData(GL_ARRAY_BUFFER, sizeof zero, zero, GL_STREAM_DRAW); /* (nor these vertices in the draws') */
    }
    if (prog)
        g->glDeleteProgram(prog);
    if (v)
        g->glDeleteShader(v);
    if (f)
        g->glDeleteShader(f);
    rect_line(g, stderr);
}

#define RECT_POINT_DRAWS 3072 /* indices of a rectangle list drawn one point at a time below GLES 3.2 */

static void draw_rects(xenos_gl *g, const xenos_cp *cp, const program *rp, const uint32_t *idx, uint32_t n)
{
    enum { F = RECT_VARYINGS * 4 };
    uint32_t nr = n / 3;
    if (!nr)
        return;
    int gpu = rp->kind == 3;
    uint32_t chunks = (nr + RECT_CHUNK - 1) / RECT_CHUNK;
    g->glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, g->tfbo);
    /* gpu: whole blocks, so that the last chunk's uniform block lies inside the buffer */
    g->glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER,
                    gpu ? (GLsizeiptr)chunks * RECT_CHUNK_BYTES : (GLsizeiptr)nr * 3 * F * 4, NULL, GL_STREAM_DRAW);
    g->glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, g->tfbo);
    g->glEnable(GL_RASTERIZER_DISCARD);
    g->glBeginTransformFeedback(0 /* GL_POINTS */);
    /* WP-35 step 2: GLES 3.0 and 3.1 have no indexed draw while transform feedback is active (GL_INVALID_OPERATION,
     * nothing captured; 3.2 lifts it): there each index is a draw of one point of its own inside the one capture
     * (vertex idx[i], gl_VertexID = idx[i]; the captures follow each other in order). A long indexed list (no title
     * seen draws one) keeps glDrawElements rather than thousands of draws. */
    int gles32 = g->gl_major > 3 || (g->gl_major == 3 && g->gl_minor >= 2);
    if (idx && !gles32 && nr * 3 <= RECT_POINT_DRAWS) {
        for (uint32_t i = 0; i < nr * 3; i++)
            g->glDrawArrays(0, (GLint)idx[i], 1);
    } else if (idx) {
        g->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g->ibo);
        g->glBufferData(GL_ELEMENT_ARRAY_BUFFER, 4 * (GLsizeiptr)nr * 3, idx, GL_STREAM_DRAW);
        g->glDrawElements(0, (GLsizei)(nr * 3), GL_UNSIGNED_INT, NULL);
    } else {
        g->glDrawArrays(0, 0, (GLsizei)(nr * 3));
    }
    g->glEndTransformFeedback();
    g->glDisable(GL_RASTERIZER_DISCARD);
    if (gpu) {
        /* the capture ended before it is read (GL orders the two in the command stream: no read-back, no barrier);
         * off the feedback bindings so that it is never bound for feedback and as a uniform buffer at once */
        g->glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
        g->glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, 0);
        attribs_enable(g, 0); /* the pass reads none */
        use_program(g, rp->prog);
        bind_textures(g, cp, rp);
        for (uint32_t k = 0; k < chunks; k++) {
            uint32_t m = nr - k * RECT_CHUNK < RECT_CHUNK ? nr - k * RECT_CHUNK : RECT_CHUNK;
            g->glBindBufferRange(GL_UNIFORM_BUFFER, 2, g->tfbo, (GLintptr)k * RECT_CHUNK_BYTES, RECT_CHUNK_BYTES);
            g->glDrawArrays(4 /* GL_TRIANGLES */, 0, (GLsizei)(m * 6));
        }
        g->glBindBufferBase(GL_UNIFORM_BUFFER, 2, 0);
        return;
    }
    g->st.rect_lists_read_back++;
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
    use_program(g, rp->prog);
    bind_textures(g, cp, rp);
    g->glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
    g->glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)nr * 6 * F * 4, tri, GL_STREAM_DRAW);
    uint32_t need = 0;
    for (int i = 0; i < RECT_VARYINGS; i++)
        if (rp->loc[i] >= 0)
            need |= 1u << rp->loc[i];
    attribs_enable(g, need);
    for (int i = 0; i < RECT_VARYINGS; i++)
        if (rp->loc[i] >= 0)
            g->glVertexAttribPointer((GLuint)rp->loc[i], 4, GL_FLOAT, 0, F * 4, (void *)(uintptr_t)(i * 16));
    g->glDrawArrays(4 /* GL_TRIANGLES */, 0, (GLsizei)(nr * 6));
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
        if (log && g->npending) { /* lazy resolves under what is exported: written first, the export over them */
            uint32_t lo = 0xFFFFFFFFu, hi = 0;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t pa = log[2 * i] & 0x1FFFFFFCu;
                lo = pa < lo ? pa : lo, hi = pa + 4 > hi ? pa + 4 : hi;
            }
            flush_range(g, cp, lo, hi);
        }
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

/* WP-9: a draw that cannot change the eDRAM or memory: eDRAM mode colour + depth, RB_COLOR_MASK's nibble 0 for every
 * colour target the pixel shader writes (and target 0, always bound), RB_DEPTHCONTROL with stencil off and not both z
 * enable and z write, no memory export in either stage. Direct3D issues such dummy draws by the hundred per frame
 * (Adventure Time: 1 point, runs of 24, ~792 of ~868 draws). Nothing else the GPU does here depends on a draw:
 * occlusion and visibility queries report fixed results (xenos_cp.c), screen extents are faked. Skipped, the draw does
 * not bind its targets, which would mark them written and make overlapping surfaces stale (rows cleared) for nothing,
 * and it does not fetch indices or vertices (pending lazy resolves under them are written when something else reads
 * that memory). Counted in noop_draws (the perf line's "no-op") either way; RECOMP_X360_NOOP_DRAWS=keep draws them. */
static int draw_writes_nothing(const xenos_cp *cp, const program *p, const program *rp)
{
    if ((cp->regs[0x2208] & 7) != 4 || p->memexport || (rp && rp->memexport))
        return 0;
    uint32_t cm = cp->regs[0x2104], depth = cp->regs[0x2200]; /* RB_DEPTHCONTROL: stencil enable 0, z 1, z write 2 */
    unsigned targets = ((rp ? rp : p)->outputs | 1u) & 15u;
    for (int i = 0; i < 4; i++)
        if ((targets >> i) & 1 && (cm >> (4 * i)) & 15)
            return 0;
    return !(depth & 1) && (depth & 6) != 6;
}

void xenos_gl_draw(xenos_gl *g, const xenos_cp *cp, const xenos_draw *d)
{
    g->st.draw_calls++;
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
    program *rp = NULL;
    if (d->prim_type == 8) { /* WP-10: the GPU pass-through; the read-back one if that does not compile here */
        if (g->rects_gpu)
            rp = get_program(g, cp, 3);
        if (!rp)
            rp = get_program(g, cp, 2);
    }
    if (!p || (d->prim_type == 8 && !rp)) {
        g->st.no_shader++;
        return;
    }
    if (draw_writes_nothing(cp, p, rp)) {
        g->st.noop_draws++;
        if (!g->keep_noop)
            return;
    }
    /* indices: from the DMA buffer or the packet (16 or 32 bits, VGT_DMA_SIZE's swap mode) or 0..n-1 */
    uint32_t n = d->num_indices, *idx = NULL, max_index = n - 1, min_index = 0;
    uint32_t *conv = NULL; /* convert_indices' array (idx then points at it), freed at the end */
    int convert = d->prim_type >= 12 && d->prim_type <= 15;
    /* VGT_INDX_OFFSET is added to every index (Direct3D's base vertex); with PA_SU_SC_MODE_CNTL's
     * multi_prim_ib_ena, VGT_MULTI_PRIM_IB_RESET_INDX cuts the strips, fans, loops and polygons */
    uint32_t index_offset = cp->regs[0x2102];
    int restart = d->source_select != 2 && (cp->regs[0x2205] >> 21) & 1 && prim_resets(d->prim_type);
    uint32_t reset_index = cp->regs[0x2103] & (d->index_size ? 0xFFFFFFFFu : 0xFFFFu);
    int restarted = 0;
    if (d->source_select != 2 || index_offset || convert) {
        idx = scratch_buf(g, SCRATCH_INDICES, 4 * (size_t)n);
        if (!idx) {
            unsupported(g, "out of memory");
            return;
        }
        uint32_t endian = cp->regs[XENOS_REG_VGT_DMA_SIZE] >> 30;
        max_index = 0, min_index = 0xFFFFFFFFu;
        if (d->source_select == 0 && g->npending) /* lazy resolves under the indices: written first */
            flush_range(g, cp, d->dma_base, d->dma_base + (d->index_size ? 4 : 2) * n + 4);
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
            if (v < min_index)
                min_index = v;
        }
        if (min_index > max_index) /* only reset indices */
            min_index = max_index;
        if (max_index > 0xFFFFF) {
            unsupported(g, "vertex index past 2^20");
            return;
        }
    }
    if (convert) {
        uint32_t m = 0;
        conv = convert_indices(d->prim_type, idx, n, &m);
        idx = conv;
        if (!conv || !m) {
            free(conv);
            unsupported(g, conv ? "primitive with too few vertices" : "out of memory");
            return;
        }
        n = m;
        restarted = 0; /* every run is its own primitives now */
    }
    /* vertices: every attribute converted to vec4, interleaved; only those from min_index to max_index, placed at
     * their own index in the vertex buffer (the indices and gl_VertexID, which shaders read as r0.x, stay as they
     * are: the buffer's start is left unwritten) */
    uint32_t nv = max_index + 1 - min_index, na = (uint32_t)p->nattr;
    g->st.vertices_fetched += na ? nv : 0;
    float *vb = na ? scratch_buf(g, SCRATCH_VERTICES, (size_t)nv * na * 16) : NULL;
    if (na && !vb) {
        free(conv);
        unsupported(g, "out of memory");
        return;
    }
    for (uint32_t a = 0; a < na; a++) {
        const attr *at = &p->attrs[a];
        uint32_t c0 = cp->regs[0x4800 + 2 * at->fetch_constant], c1 = cp->regs[0x4800 + 2 * at->fetch_constant + 1];
        if (g->npending) /* lazy resolves under the vertices: written first */
            flush_range(g, cp, c0 & ~3u,
                        (c0 & ~3u) + 4 * (at->offset + (max_index + 1) * at->stride + 4));
        if (!fetch_vertices(cp, c0 & ~3u, c1 & 3, at, min_index, nv, vb + 4 * a, 4 * (size_t)na)) {
            free(conv);
            unsupported(g, "vertex format");
            return;
        }
    }
    if (!bind_targets(g, cp, (rp ? rp : p)->outputs, d->prim_type == 8)) { /* the eDRAM surfaces; the host viewport over them */
        free(conv);
        unsupported(g, "no complete framebuffer for the render targets");
        return;
    }
    use_program(g, p->prog);
    upload_constants(g, cp); /* the guest viewport is applied in the vertex shader (xe_k.vp) */
    apply_state(g, cp, d->prim_type);
    bind_textures(g, cp, p);
    if (p->nvb && !bind_vertex_buffers(g, cp, p)) {
        free(conv);
        unsupported(g, "vertex format of a computed-index fetch");
        return;
    }
    bind_vao(g, g->vao);
    g->glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
    if (!min_index) {
        g->glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)nv * na * 16, vb, GL_STREAM_DRAW);
    } else {
        g->glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(max_index + 1) * na * 16, NULL, GL_STREAM_DRAW);
        if (na)
            g->glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)min_index * na * 16, (GLsizeiptr)nv * na * 16, vb);
    }
    uint32_t need = 0;
    for (uint32_t a = 0; a < na; a++)
        if (p->loc[a] >= 0)
            need |= 1u << p->loc[a];
    attribs_enable(g, need); /* WP-24: left enabled between draws, only the difference changed */
    for (uint32_t a = 0; a < na; a++)
        if (p->loc[a] >= 0)
            g->glVertexAttribPointer((GLuint)p->loc[a], 4, GL_FLOAT, 0, (GLsizei)(na * 16),
                                     (void *)(uintptr_t)(a * 16));
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
    g->st.drawn++;
    g->st.rect_draws += d->prim_type == 8;
    free(conv);
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
    GS_DIRTY(g); /* WP-24: GL state apply_state shadows changes here */
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
    tiler_discard(g, 1, 0); /* the triangle covers all of frame_tex (frame_texture: fw x fh) */
    g->glViewport(0, 0, fw * k, fh * k);
    use_program(g, g->aprog);
    g->glActiveTexture(GL_TEXTURE0);
    unit_plain(g, 0); /* WP-24 */
    g->glBindTexture(GL_TEXTURE_2D, src);
    g->glUniform1i(g->glGetUniformLocation(g->aprog, "t"), 0);
    g->glUniform3i(g->aprog_off, 0, 0, 0);
    g->glUniform1i(g->aprog_sc, 1); /* gamma */
    g->glUniform1i(g->aprog_dc, 0); /* 8_8_8_8 */
    bind_vao(g, g->vao);
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
    GS_DIRTY(g); /* WP-24: GL state apply_state shadows changes here */
    gpu_timer_frame(g);
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
    if (rc && fw <= rc->w && fh <= rc->th) {
        src = rc->tex, src_fmt = rc->ifmt;
        if (rc->swap_rb)
            swizzle = swizzle_swap_rb(swizzle);
        g->st.front_buffer_frames++;
    } else if (front) {
        decoded d;
        if (g->npending && decode_texture(cp, fc, 2, &d)) /* lazy resolves there: written first */
            flush_range(g, cp, d.lo, d.hi);
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
                g->frame_copy = 0;
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
        pclear_flush(g, s);
        surface_flush(g, s);
        src = s->tex, src_fmt = s->ifmt;
        fw = fw < s->w ? fw : s->w, fh = fh < s->h ? fh : s->h;
        swizzle = ident;
    }
    if (src) {
        GLenum rf = is_float_fmt(src_fmt) ? GL_RGBA16F : GL_RGBA8; /* frame_tex's format */
        /* WP-18, RECOMP_X360_DIRECT_PRESENT=1: a copy of the frame's own size in frame_tex's format is shown as it is
         * (frame_detach takes it out when it changes before the window shows it); else into frame_tex (by default,
         * a smaller frame, gamma, render target 0) */
        GLuint shown = g->frame_tex;
        if (rc && src == rc->tex && g->direct_present && fw == rc->w && fh == rc->th && src_fmt == rf) {
            g->frame_copy = 1 + (int)(rc - g->copies);
            g->show_w = fw * g->scale, g->show_h = fh * g->scale;
            shown = src;
            g->glBindTexture(GL_TEXTURE_2D, src); /* as frame_texture sets frame_tex's */
            g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        } else {
            g->frame_copy = 0;
            frame_texture(g, fw, fh, src_fmt);
            if (src_fmt != GL_SRGB8_ALPHA8 || !frame_encode(g, src, fw, fh))
                blit_tex(g, src, g->frame_tex, 0, 0, fw, fh, 0, 0, 1);
            shown = g->frame_tex;
            g->glBindTexture(GL_TEXTURE_2D, g->frame_tex);
        }
        for (int c = 0; c < 4; c++) {
            uint32_t sel = (swizzle >> (3 * c)) & 7;
            g->frame_swz[c] = sel < 6 ? swz[sel] : GL_ZERO;
            g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R + (GLenum)c, g->frame_swz[c]);
        }
        if (g->frame_copy)
            g->copies[g->frame_copy - 1].swz = 0; /* its swizzle is the frame's now: set again when sampled */
        if (g->readback) {
            const uint8_t *px = read_raw(g, shown, rf, 0, 0, fw, fh);
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
    /* lazy resolves not resolved again for 2 swaps: written (a picture resolved once and kept, that states hold) */
    g->swaps++;
    for (int i = 0; g->npending && i < MAX_COPIES; i++)
        if (g->copies[i].pending && g->swaps - g->copies[i].frame >= 2)
            flush_copy(g, cp, &g->copies[i]);
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
    GS_DIRTY(g); /* WP-24: GL state apply_state shadows changes here */
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
    for (int i = 0; i < 32; i++) /* WP-24: the owner samples its own textures with their own parameters */
        unit_plain(g, i);
    g->glActiveTexture(GL_TEXTURE0);
    g->glBindVertexArray(0);
    g->glUseProgram(0);
    g->cur_vao = g->cur_prog = ~0u; /* the owner's own drawing follows */
    g->ubo_rebind = 1; /* the owner's drawing may bind its own uniform buffers; the buffers' contents stay ours */
}
