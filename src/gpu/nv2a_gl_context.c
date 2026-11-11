/* A GL context with no window, for the GPU back end. See nv2a_gl_api.h.
 *
 * The back end renders into framebuffer objects only and hands finished
 * frames to the presenter through guest memory (nv2a_gl.c), so it never
 * needs a drawable: a headless CGL context on macOS, an EGL context with a
 * 1x1 pbuffer (or none, where EGL_KHR_surfaceless_context exists) elsewhere.
 * Created on the executor thread, which keeps it current for the whole run:
 * the SDL presenter on the main thread never touches it. */
#include "nv2a_gl_api.h"

#include <stdio.h>

#if defined(__APPLE__)
#include <OpenGL/OpenGL.h>

int nv2a_gl_context_create(void)
{
    CGLPixelFormatAttribute attrs[] = {
        kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,
        kCGLPFAAccelerated,
        kCGLPFAAllowOfflineRenderers,
        (CGLPixelFormatAttribute)0
    };
    CGLPixelFormatObj pf = NULL;
    CGLContextObj ctx = NULL;
    GLint npf = 0;
    CGLError e;

    e = CGLChoosePixelFormat(attrs, &pf, &npf);
    if (e != kCGLNoError || !pf) {
        fprintf(stderr, "  [GPUGL] CGLChoosePixelFormat failed: %s\n", CGLErrorString(e));
        return 0;
    }
    e = CGLCreateContext(pf, NULL, &ctx);
    CGLDestroyPixelFormat(pf);
    if (e != kCGLNoError || !ctx) {
        fprintf(stderr, "  [GPUGL] CGLCreateContext failed: %s\n", CGLErrorString(e));
        return 0;
    }
    e = CGLSetCurrentContext(ctx);
    if (e != kCGLNoError) {
        fprintf(stderr, "  [GPUGL] CGLSetCurrentContext failed: %s\n", CGLErrorString(e));
        CGLDestroyContext(ctx);
        return 0;
    }
    return 1;
}

#else  /* EGL: Android, Linux */

#if defined(__ANDROID__)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#else
#include <epoxy/egl.h>
#endif

int nv2a_gl_context_create(void)
{
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLConfig cfg;
    EGLint ncfg = 0, major, minor;
    EGLContext ctx;
    EGLSurface surf;
#if NV2A_GL_ES
    const EGLint renderable = EGL_OPENGL_ES3_BIT_KHR;
    const EGLenum api = EGL_OPENGL_ES_API;
    const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
#else
    const EGLint renderable = EGL_OPENGL_BIT;
    const EGLenum api = EGL_OPENGL_API;
    const EGLint ctx_attrs[] = { EGL_CONTEXT_MAJOR_VERSION_KHR, 3,
                                 EGL_CONTEXT_MINOR_VERSION_KHR, 3,
                                 EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
                                 EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
                                 EGL_NONE };
#endif
    const EGLint cfg_attrs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, renderable,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    const EGLint pb_attrs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };

    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor)) {
        fprintf(stderr, "  [GPUGL] no EGL display\n");
        return 0;
    }
    if (!eglBindAPI(api)
        || !eglChooseConfig(dpy, cfg_attrs, &cfg, 1, &ncfg) || ncfg < 1) {
        fprintf(stderr, "  [GPUGL] no EGL config for a pbuffer GL context\n");
        return 0;
    }
    ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (ctx == EGL_NO_CONTEXT) {
        fprintf(stderr, "  [GPUGL] eglCreateContext failed (0x%X)\n", eglGetError());
        return 0;
    }
    surf = eglCreatePbufferSurface(dpy, cfg, pb_attrs);
    if (!eglMakeCurrent(dpy, surf, surf, ctx)) {
        fprintf(stderr, "  [GPUGL] eglMakeCurrent failed (0x%X)\n", eglGetError());
        eglDestroyContext(dpy, ctx);
        return 0;
    }
    return 1;
}
#endif
