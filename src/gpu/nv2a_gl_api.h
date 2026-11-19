/* The GL entry points the GPU back end draws with, and its context.
 *
 *   macOS    OpenGL 4.1 core from the system framework, headless CGL context
 *   Android  OpenGL ES 3.0 (libGLESv3), EGL pbuffer context
 *   Linux    desktop GL through libepoxy, EGL (pbuffer or surfaceless);
 *            with NV2A_GL_FORCE_ES=1 (test builds) the GLES 3.0 path of
 *            Android, through libepoxy on an EGL ES context, so the tests
 *            run what the device runs on a host without one
 *
 * One code path for all three: nv2a_gl.c uses only what GL 3.3 core and
 * GLES 3.0 have in common, and asks at run time for the rest (S3TC, depth
 * clamp, exact occlusion counts). */
#ifndef XBOXRECOMP_NV2A_GL_API_H
#define XBOXRECOMP_NV2A_GL_API_H

#include <stddef.h>

#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/gl3.h>
#define NV2A_GL_ES 0
#elif defined(__ANDROID__)
#include <GLES3/gl3.h>
#define NV2A_GL_ES 1
#elif defined(NV2A_GL_FORCE_ES) && NV2A_GL_FORCE_ES
#include <epoxy/gl.h>
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
#ifndef GL_CONTEXT_LOST
#define GL_CONTEXT_LOST 0x0507      /* GLES 3.2, KHR_robustness */
#endif
#ifndef GL_TEXTURE_BORDER_COLOR
#define GL_TEXTURE_BORDER_COLOR 0x1004
#endif

/* Create a context with no window and make it current on the calling
 * thread. Returns 0 on failure (and says why on stderr); 1 at once if the
 * context already exists. The context stays bound to that thread for the
 * rest of the run, or until nv2a_gl_context_destroy. */
int nv2a_gl_context_create(void);

/* 1 if the context is gone: the driver reported a reset (EGL, with
 * EGL_EXT_create_context_robustness) or it is no longer current on this
 * thread. Every GL object it held is then invalid. Cheap: call per frame. */
int nv2a_gl_context_lost(void);

/* Drop the context after a loss; the next nv2a_gl_context_create makes a
 * new one (EGL keeps the display, config and pbuffer). */
void nv2a_gl_context_destroy(void);

/* A second context in the same share group, for the shader worker thread
 * (XBA-310: programs compiled off the draw path; nv2a_gl.c). create: on the
 * thread whose context is current (the executor), after
 * nv2a_gl_context_create; 0 when it cannot be had (macOS: none; EGL: the
 * driver refuses a shared context, no surface to be current on), and the
 * back end then compiles another way. bind / unbind: on the worker thread,
 * which keeps it current for its life. destroy: once the worker has ended
 * (it unbound), before the main context goes. Programs and shaders made in
 * it are the main context's too. */
int  nv2a_gl_shared_context_create(void);
int  nv2a_gl_shared_context_bind(void);
void nv2a_gl_shared_context_unbind(void);
void nv2a_gl_shared_context_destroy(void);

/* Zero-copy present (XBA-308): the window's GL context joins the back end's
 * share group, so the window draws the frame's texture itself.
 * window: on the window's thread with its renderer's context current,
 * before nv2a_gl_context_create; the next context is created in that
 * context's share group (on its EGL display, without reset notification,
 * which a share group must agree on). 0, with why[] filled: no EGL context
 * current, another client API (GL against GLES), or macOS (CGL; SDL draws
 * with Metal there). shared: 1 when the current context is in that share
 * group (the share may still fail at creation; logged). unshare: the next
 * context is created alone again (the window's context was lost). */
int  nv2a_gl_context_share_window(char *why, size_t n);
int  nv2a_gl_context_shared(void);
void nv2a_gl_context_unshare(void);

#endif
