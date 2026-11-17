/* The OpenGL entry points d3d8_gl.c draws with.
 *
 * Desktop POSIX: OpenGL 3.3 core through libepoxy.
 * Android: OpenGL ES 3.0 from the NDK (libGLESv3). This header only makes
 * the file compile there (XBA-17). d3d8_gl.c does not draw on GLES: it
 * compiles "#version 330 core" shaders, asks SDL for a 3.3 core context and
 * passes GL_BGRA as a vertex attribute size, none of which GLES 3.0 has.
 * XBA-27 delivered the GPU rendering on Android as the NV2A GL back end
 * (src/gpu, nv2a_gl.c), which draws the pushbuffer, not this D3D8 layer. */
#ifndef XBOXRECOMP_GL_HOST_H
#define XBOXRECOMP_GL_HOST_H

#if defined(__ANDROID__)
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#define XBOXRECOMP_GLES 1
/* Desktop names d3d8_gl.c uses, mapped to their nearest GLES 3.0 form. Enough
 * to compile, not to draw (above). */
#define glClearDepth(d)           glClearDepthf((GLfloat)(d))
#define glDepthRange(n, f)        glDepthRangef((GLfloat)(n), (GLfloat)(f))
/* EXT_texture_format_BGRA8888 (Adreno, Mali). With GL_UNSIGNED_BYTE on a
 * little-endian CPU, BGRA bytes are what 8_8_8_8_REV reads on desktop GL. */
#ifndef GL_BGRA
#define GL_BGRA                   GL_BGRA_EXT
#endif
#define GL_UNSIGNED_INT_8_8_8_8_REV GL_UNSIGNED_BYTE
#else
#include <epoxy/gl.h>
#define XBOXRECOMP_GLES 0
#endif

#endif /* XBOXRECOMP_GL_HOST_H */
