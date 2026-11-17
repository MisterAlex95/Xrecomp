/* gx_glapi.h -- the OpenGL ES 3.0 and EGL entry points the GX renderer uses, loaded at run time (XBA-262): no GL or
 * EGL headers or libraries at build time. Values are the Khronos registry's. */
#ifndef GX_GLAPI_H
#define GX_GLAPI_H

#include <stddef.h>
#include <stdint.h>

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef float GLfloat;
typedef char GLchar;
typedef unsigned char GLubyte;
typedef intptr_t GLintptr;
typedef intptr_t GLsizeiptr;
typedef uint64_t GLuint64;
typedef struct __GLsync *GLsync;

#define GL_FALSE 0
#define GL_TRUE 1
#define GL_NO_ERROR 0
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_EXTENSIONS 0x1F03
#define GL_NUM_EXTENSIONS 0x821D
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_TRIANGLES 0x0004
#define GL_NEVER 0x0200
#define GL_FRONT 0x0404
#define GL_BACK 0x0405
#define GL_FRONT_AND_BACK 0x0408
#define GL_CULL_FACE 0x0B44
#define GL_DEPTH_TEST 0x0B71
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_DITHER 0x0BD0
#define GL_CCW 0x0901
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_PACK_ROW_LENGTH 0x0D02
#define GL_TEXTURE_2D 0x0DE1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_SHORT 0x1403
#define GL_UNSIGNED_INT 0x1405
#define GL_FLOAT 0x1406
#define GL_RGBA 0x1908
#define GL_DEPTH_COMPONENT 0x1902
#define GL_RGBA8 0x8058
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_LINEAR_MIPMAP_NEAREST 0x2701
#define GL_NEAREST_MIPMAP_LINEAR 0x2702
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_TEXTURE_MIN_LOD 0x813A
#define GL_TEXTURE_MAX_LOD 0x813B
#define GL_TEXTURE_BASE_LEVEL 0x813C
#define GL_TEXTURE_MAX_LEVEL 0x813D
#define GL_REPEAT 0x2901
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_MIRRORED_REPEAT 0x8370
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE1 0x84C1
#define GL_ZERO 0
#define GL_ONE 1
#define GL_SRC_COLOR 0x0300
#define GL_ONE_MINUS_SRC_COLOR 0x0301
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_DST_ALPHA 0x0304
#define GL_ONE_MINUS_DST_ALPHA 0x0305
#define GL_DST_COLOR 0x0306
#define GL_ONE_MINUS_DST_COLOR 0x0307
#define GL_CONSTANT_ALPHA 0x8003
#define GL_FUNC_ADD 0x8006
#define GL_FUNC_REVERSE_SUBTRACT 0x800B
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_UNIFORM_BUFFER 0x8A11
#define GL_STREAM_DRAW 0x88E0
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_MAX_TEXTURE_SIZE 0x0D33
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#define GL_SYNC_FLUSH_COMMANDS_BIT 0x00000001
#define GL_MAP_WRITE_BIT 0x0002
#define GL_MAP_INVALIDATE_BUFFER_BIT 0x0008
#define GL_PROGRAM_BINARY_RETRIEVABLE_HINT 0x8257
#define GL_COMPLETION_STATUS_KHR 0x91B1
#define GL_PROGRAM_BINARY_LENGTH 0x8741
#define GL_NUM_PROGRAM_BINARY_FORMATS 0x87FE
#define GL_INVALID_INDEX 0xFFFFFFFFu

/* X(return type, name, parameters) */
#define GX_GL_FUNCS(X) \
    X(const GLubyte *, glGetString, (GLenum)) \
    X(const GLubyte *, glGetStringi, (GLenum, GLuint)) \
    X(void, glGetIntegerv, (GLenum, GLint *)) \
    X(GLenum, glGetError, (void)) \
    X(void, glEnable, (GLenum)) \
    X(void, glDisable, (GLenum)) \
    X(void, glViewport, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, glScissor, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, glClearDepthf, (GLfloat)) \
    X(void, glClear, (GLbitfield)) \
    X(void, glColorMask, (GLboolean, GLboolean, GLboolean, GLboolean)) \
    X(void, glDepthMask, (GLboolean)) \
    X(void, glDepthFunc, (GLenum)) \
    X(void, glBlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum)) \
    X(void, glBlendEquationSeparate, (GLenum, GLenum)) \
    X(void, glBlendColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, glCullFace, (GLenum)) \
    X(void, glFrontFace, (GLenum)) \
    X(void, glGenTextures, (GLsizei, GLuint *)) \
    X(void, glDeleteTextures, (GLsizei, const GLuint *)) \
    X(void, glBindTexture, (GLenum, GLuint)) \
    X(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    X(void, glTexParameteri, (GLenum, GLenum, GLint)) \
    X(void, glActiveTexture, (GLenum)) \
    X(void, glPixelStorei, (GLenum, GLint)) \
    X(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) \
    X(void, glGenSamplers, (GLsizei, GLuint *)) \
    X(void, glDeleteSamplers, (GLsizei, const GLuint *)) \
    X(void, glBindSampler, (GLuint, GLuint)) \
    X(void, glSamplerParameteri, (GLuint, GLenum, GLint)) \
    X(void, glSamplerParameterf, (GLuint, GLenum, GLfloat)) \
    X(void, glGenFramebuffers, (GLsizei, GLuint *)) \
    X(void, glDeleteFramebuffers, (GLsizei, const GLuint *)) \
    X(void, glBindFramebuffer, (GLenum, GLuint)) \
    X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(GLenum, glCheckFramebufferStatus, (GLenum)) \
    X(void, glBlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
    X(GLuint, glCreateShader, (GLenum)) \
    X(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    X(void, glCompileShader, (GLuint)) \
    X(void, glGetShaderiv, (GLuint, GLenum, GLint *)) \
    X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(void, glDeleteShader, (GLuint)) \
    X(GLuint, glCreateProgram, (void)) \
    X(void, glAttachShader, (GLuint, GLuint)) \
    X(void, glLinkProgram, (GLuint)) \
    X(void, glGetProgramiv, (GLuint, GLenum, GLint *)) \
    X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(void, glDeleteProgram, (GLuint)) \
    X(void, glUseProgram, (GLuint)) \
    X(GLint, glGetUniformLocation, (GLuint, const GLchar *)) \
    X(void, glUniform1i, (GLint, GLint)) \
    X(void, glUniform4fv, (GLint, GLsizei, const GLfloat *)) \
    X(void, glUniform4iv, (GLint, GLsizei, const GLint *)) \
    X(GLuint, glGetUniformBlockIndex, (GLuint, const GLchar *)) \
    X(void, glUniformBlockBinding, (GLuint, GLuint, GLuint)) \
    X(void, glBindBufferBase, (GLenum, GLuint, GLuint)) \
    X(void, glBindAttribLocation, (GLuint, GLuint, const GLchar *)) \
    X(void, glProgramParameteri, (GLuint, GLenum, GLint)) \
    X(void, glGenBuffers, (GLsizei, GLuint *)) \
    X(void, glDeleteBuffers, (GLsizei, const GLuint *)) \
    X(void, glBindBuffer, (GLenum, GLuint)) \
    X(void, glBufferData, (GLenum, GLsizeiptr, const void *, GLenum)) \
    X(void, glBufferSubData, (GLenum, GLintptr, GLsizeiptr, const void *)) \
    X(void, glGenVertexArrays, (GLsizei, GLuint *)) \
    X(void, glBindVertexArray, (GLuint)) \
    X(void, glDeleteVertexArrays, (GLsizei, const GLuint *)) \
    X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    X(void, glEnableVertexAttribArray, (GLuint)) \
    X(void, glDisableVertexAttribArray, (GLuint)) \
    X(void, glVertexAttrib4f, (GLuint, GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, glDrawElements, (GLenum, GLsizei, GLenum, const void *)) \
    X(void, glDrawArrays, (GLenum, GLint, GLsizei)) \
    X(GLsync, glFenceSync, (GLenum, GLbitfield)) \
    X(GLenum, glClientWaitSync, (GLsync, GLbitfield, GLuint64)) \
    X(void, glDeleteSync, (GLsync)) \
    X(void, glFinish, (void)) \
    X(void, glFlush, (void))

typedef struct gx_glfn {
#define GX_GL_PTR(ret, name, args) ret(*name) args;
    GX_GL_FUNCS(GX_GL_PTR)
#undef GX_GL_PTR
    void (*glMaxShaderCompilerThreadsKHR)(GLuint);
    void (*glClearDepth)(double); /* desktop GL before 4.1 has no glClearDepthf */
    /* program binaries (XBA-264; GLES 3.0, desktop GL 4.1 / ARB_get_program_binary): NULL when missing */
    void (*glGetProgramBinary)(GLuint, GLsizei, GLsizei *, GLenum *, void *);
    void (*glProgramBinary)(GLuint, GLenum, const void *, GLsizei);
} gx_glfn;

/* EGL, the part the renderer and the headless context use. */
typedef void *EGLDisplay;
typedef void *EGLContext;
typedef void *EGLSurface;
typedef void *EGLConfig;
typedef int32_t EGLint;
typedef unsigned int EGLBoolean;
typedef unsigned int EGLenum;
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_NO_SURFACE ((EGLSurface)0)
#define EGL_NO_DISPLAY ((EGLDisplay)0)
#define EGL_DEFAULT_DISPLAY ((void *)0)
#define EGL_NONE 0x3038
#define EGL_EXTENSIONS 0x3055
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_CONFIG_ID 0x3028
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_SURFACE_TYPE 0x3033
#define EGL_PBUFFER_BIT 0x0001
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES3_BIT 0x00000040
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD

typedef struct gx_eglfn {
    void *(*eglGetProcAddress)(const char *);
    EGLDisplay (*eglGetCurrentDisplay)(void);
    EGLContext (*eglGetCurrentContext)(void);
    EGLBoolean (*eglQueryContext)(EGLDisplay, EGLContext, EGLint, EGLint *);
    EGLBoolean (*eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
    EGLContext (*eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
    EGLBoolean (*eglDestroyContext)(EGLDisplay, EGLContext);
    EGLBoolean (*eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
    EGLSurface (*eglCreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint *);
    EGLBoolean (*eglDestroySurface)(EGLDisplay, EGLSurface);
    const char *(*eglQueryString)(EGLDisplay, EGLint);
    EGLBoolean (*eglInitialize)(EGLDisplay, EGLint *, EGLint *);
    EGLBoolean (*eglTerminate)(EGLDisplay);
    EGLBoolean (*eglBindAPI)(EGLenum);
    EGLDisplay (*eglGetDisplay)(void *);
    EGLDisplay (*eglGetPlatformDisplayEXT)(EGLenum, void *, const EGLint *);
    EGLint (*eglGetError)(void);
} gx_eglfn;

/* gx_egl.c: loads EGL (and GLES for the core entry points eglGetProcAddress may not give). 0, or -1. */
int gx_egl_load(gx_eglfn *egl);
int gx_gl_load(gx_glfn *gl, const gx_eglfn *egl);

#endif
