/* The GL entry points the GPU back end draws with, and its context.
 *
 *   macOS    OpenGL 4.1 core from the system framework, headless CGL context
 *   Android  OpenGL ES 3.0 (libGLESv3), EGL pbuffer context
 *   Linux    desktop GL through libepoxy, EGL (pbuffer or surfaceless)
 *
 * One code path for all three: nv2a_gl.c uses only what GL 3.3 core and
 * GLES 3.0 have in common, and asks at run time for the rest (S3TC, depth
 * clamp, exact occlusion counts). */
#ifndef XBOXRECOMP_NV2A_GL_API_H
#define XBOXRECOMP_NV2A_GL_API_H

#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/gl3.h>
#define NV2A_GL_ES 0
#elif defined(__ANDROID__)
#include <GLES3/gl3.h>
#define NV2A_GL_ES 1
#else
#include <epoxy/gl.h>
#define NV2A_GL_ES 0
#endif

#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif
#ifndef GL_DEPTH_CLAMP
#define GL_DEPTH_CLAMP 0x864F
#endif
#ifndef GL_SAMPLES_PASSED
#define GL_SAMPLES_PASSED 0x8914
#endif
#ifndef GL_CLAMP_TO_BORDER
#define GL_CLAMP_TO_BORDER 0x812D
#endif
#ifndef GL_TEXTURE_BORDER_COLOR
#define GL_TEXTURE_BORDER_COLOR 0x1004
#endif

/* Create a context with no window and make it current on the calling
 * thread. Returns 0 on failure (and says why on stderr). The context stays
 * bound to that thread for the rest of the run. */
int nv2a_gl_context_create(void);

#endif
