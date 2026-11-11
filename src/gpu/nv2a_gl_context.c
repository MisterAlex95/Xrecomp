/* A GL context with no window, for the GPU back end. See nv2a_gl_api.h.
 *
 * The back end renders into framebuffer objects only and hands finished
 * frames to the presenter through guest memory (nv2a_gl.c), so it never
 * needs a drawable: a headless CGL context on macOS, an EGL context with a
 * 1x1 pbuffer (or none, where EGL_KHR_surfaceless_context exists) elsewhere.
 * Created on the executor thread, which keeps it current for the whole run:
 * the SDL presenter on the main thread never touches it.
 *
 * EGL (Android): the display is EGL_DEFAULT_DISPLAY, the one SDL uses;
 * Android's eglInitialize is reference-counted, and this file never calls
 * eglTerminate, which would end SDL's display too. Where the driver offers
 * them the context asks for
 *   EGL_IMG_context_priority   high priority (RECOMP_GPU_PRIORITY=0 keeps the
 *                              default), so the frame the executor waits for
 *                              is not queued behind other apps' work;
 *   EGL_EXT_create_context_robustness  lose-context-on-reset notification,
 *                              so a GPU reset is seen at the flip
 *                              (nv2a_gl_context_lost) instead of every later
 *                              call failing silently.
 * Each is dropped again if the driver refuses the context with it.
 * RECOMP_GPU_VERBOSE=1 logs the EGL version, vendor and extensions. */
#include "nv2a_gl_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <OpenGL/OpenGL.h>

static CGLContextObj s_cgl;

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

    if (s_cgl)
        return 1;
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
    s_cgl = ctx;
    return 1;
}

/* macOS reports no resets to a core context without robustness; a context
 * that stopped being current is the one sign there is. */
int nv2a_gl_context_lost(void)
{
    return s_cgl && CGLGetCurrentContext() != s_cgl;
}

void nv2a_gl_context_destroy(void)
{
    if (!s_cgl)
        return;
    if (CGLGetCurrentContext() == s_cgl)
        CGLSetCurrentContext(NULL);
    CGLDestroyContext(s_cgl);
    s_cgl = NULL;
}

#else  /* EGL: Android, Linux */

#if defined(__ANDROID__)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#else
#include <epoxy/egl.h>
#endif

#ifndef EGL_CONTEXT_PRIORITY_LEVEL_IMG
#define EGL_CONTEXT_PRIORITY_LEVEL_IMG  0x3100
#define EGL_CONTEXT_PRIORITY_HIGH_IMG   0x3101
#define EGL_CONTEXT_PRIORITY_MEDIUM_IMG 0x3102
#define EGL_CONTEXT_PRIORITY_LOW_IMG    0x3103
#endif
#ifndef EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_EXT
#define EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_EXT 0x3138
#define EGL_LOSE_CONTEXT_ON_RESET_EXT 0x31BF
#endif

static int env_set_off(const char *name)
{
    const char *e = getenv(name);
    return e && strcmp(e, "0") == 0;
}

static int env_on(const char *name)
{
    const char *e = getenv(name);
    return e && *e && strcmp(e, "0") != 0;
}

static EGLDisplay s_dpy = EGL_NO_DISPLAY;
static EGLContext s_ctx = EGL_NO_CONTEXT;
static EGLSurface s_surf = EGL_NO_SURFACE;
static EGLConfig  s_cfg;
static int s_have_cfg;
static int s_robust;               /* reset notification asked for and given */
/* glGetGraphicsResetStatus(EXT/KHR), looked up once a robust context exists. */
static GLenum (*s_reset_status)(void);

/* `name` in a space-separated extension list, whole words only. */
static int ext_in(const char *list, const char *name)
{
    size_t n = strlen(name);
    const char *p = list;
    while (p && (p = strstr(p, name)) != NULL) {
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == 0))
            return 1;
        p += n;
    }
    return 0;
}

static const char *priority_name(EGLint v)
{
    switch (v) {
    case EGL_CONTEXT_PRIORITY_HIGH_IMG:   return "high";
    case EGL_CONTEXT_PRIORITY_MEDIUM_IMG: return "medium";
    case EGL_CONTEXT_PRIORITY_LOW_IMG:    return "low";
    default:                              return "?";
    }
}

/* Try a context with the version attributes plus, if asked, high priority
 * and reset notification. */
static EGLContext try_context(int priority, int robust)
{
    EGLint a[16];
    int n = 0;
#if NV2A_GL_ES
    a[n++] = EGL_CONTEXT_CLIENT_VERSION; a[n++] = 3;
#else
    a[n++] = EGL_CONTEXT_MAJOR_VERSION_KHR; a[n++] = 3;
    a[n++] = EGL_CONTEXT_MINOR_VERSION_KHR; a[n++] = 3;
    a[n++] = EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR;
    a[n++] = EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR;
#endif
    if (priority) {
        a[n++] = EGL_CONTEXT_PRIORITY_LEVEL_IMG;
        a[n++] = EGL_CONTEXT_PRIORITY_HIGH_IMG;
    }
    if (robust) {
        a[n++] = EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_EXT;
        a[n++] = EGL_LOSE_CONTEXT_ON_RESET_EXT;
    }
    a[n] = EGL_NONE;
    return eglCreateContext(s_dpy, s_cfg, EGL_NO_CONTEXT, a);
}

int nv2a_gl_context_create(void)
{
    static int said;
    EGLint ncfg = 0, major = 0, minor = 0;
    const char *exts;
    int surfaceless, want_prio, want_robust, prio, robust;
#if NV2A_GL_ES
    const EGLint renderable = EGL_OPENGL_ES3_BIT_KHR;
    const EGLenum api = EGL_OPENGL_ES_API;
#else
    const EGLint renderable = EGL_OPENGL_BIT;
    const EGLenum api = EGL_OPENGL_API;
#endif
    EGLint cfg_attrs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, renderable,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    const EGLint pb_attrs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };

    if (s_ctx != EGL_NO_CONTEXT)
        return 1;
    if (s_dpy == EGL_NO_DISPLAY) {
        EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor)) {
            fprintf(stderr, "  [GPUGL] no EGL display (0x%X)\n", eglGetError());
            return 0;
        }
        s_dpy = dpy;
    }
    exts = eglQueryString(s_dpy, EGL_EXTENSIONS);
    if (!exts)
        exts = "";
    if (!said++ && env_on("RECOMP_GPU_VERBOSE")) {
        const char *v = eglQueryString(s_dpy, EGL_VERSION);
        const char *vendor = eglQueryString(s_dpy, EGL_VENDOR);
        fprintf(stderr, "  [GPUGL] EGL %s (%s)\n  [GPUGL] EGL extensions: %s\n",
                v ? v : "?", vendor ? vendor : "?", exts);
    }
    surfaceless = ext_in(exts, "EGL_KHR_surfaceless_context");
    want_prio = ext_in(exts, "EGL_IMG_context_priority")
             && !env_set_off("RECOMP_GPU_PRIORITY");
#if NV2A_GL_ES
    want_robust = ext_in(exts, "EGL_EXT_create_context_robustness");
#else
    want_robust = 0;   /* desktop GL spells it through EGL_KHR_create_context */
#endif

    if (!eglBindAPI(api)) {
        fprintf(stderr, "  [GPUGL] eglBindAPI failed (0x%X)\n", eglGetError());
        return 0;
    }
    if (!s_have_cfg) {
        if (!eglChooseConfig(s_dpy, cfg_attrs, &s_cfg, 1, &ncfg) || ncfg < 1) {
            /* No pbuffer config: any config of the API will do for a
             * context that is made current with no surface. */
            cfg_attrs[1] = EGL_DONT_CARE;
            if (!surfaceless
                || !eglChooseConfig(s_dpy, cfg_attrs, &s_cfg, 1, &ncfg) || ncfg < 1) {
                fprintf(stderr, "  [GPUGL] no EGL config for a headless GL context%s\n",
                        surfaceless ? "" : " (no pbuffer config, no"
                                           " EGL_KHR_surfaceless_context)");
                return 0;
            }
        }
        s_have_cfg = 1;
    }

    /* Ask for everything, then drop what the driver refuses. */
    prio = want_prio;
    robust = want_robust;
    s_ctx = try_context(prio, robust);
    if (s_ctx == EGL_NO_CONTEXT && robust)
        s_ctx = try_context(prio, robust = 0);
    if (s_ctx == EGL_NO_CONTEXT && prio)
        s_ctx = try_context(prio = 0, robust = want_robust);
    if (s_ctx == EGL_NO_CONTEXT && robust)
        s_ctx = try_context(prio, robust = 0);
    if (s_ctx == EGL_NO_CONTEXT) {
        fprintf(stderr, "  [GPUGL] eglCreateContext failed (0x%X)\n", eglGetError());
        return 0;
    }

    /* A 1x1 pbuffer to be current on; without one, no surface at all. */
    if (s_surf == EGL_NO_SURFACE) {
        EGLint st = 0;
        eglGetConfigAttrib(s_dpy, s_cfg, EGL_SURFACE_TYPE, &st);
        if (st & EGL_PBUFFER_BIT)
            s_surf = eglCreatePbufferSurface(s_dpy, s_cfg, pb_attrs);
        if (s_surf == EGL_NO_SURFACE && !surfaceless) {
            fprintf(stderr, "  [GPUGL] eglCreatePbufferSurface failed (0x%X) and no"
                            " EGL_KHR_surfaceless_context\n", eglGetError());
            eglDestroyContext(s_dpy, s_ctx);
            s_ctx = EGL_NO_CONTEXT;
            return 0;
        }
    }
    if (!eglMakeCurrent(s_dpy, s_surf, s_surf, s_ctx)) {
        fprintf(stderr, "  [GPUGL] eglMakeCurrent (%s) failed (0x%X)\n",
                s_surf == EGL_NO_SURFACE ? "surfaceless" : "pbuffer", eglGetError());
        eglDestroyContext(s_dpy, s_ctx);
        s_ctx = EGL_NO_CONTEXT;
        if (s_surf != EGL_NO_SURFACE) {
            eglDestroySurface(s_dpy, s_surf);
            s_surf = EGL_NO_SURFACE;
        }
        return 0;
    }

    s_robust = robust;
    s_reset_status = NULL;
    if (robust) {
        s_reset_status = (GLenum (*)(void))eglGetProcAddress("glGetGraphicsResetStatusEXT");
        if (!s_reset_status)
            s_reset_status = (GLenum (*)(void))eglGetProcAddress("glGetGraphicsResetStatusKHR");
    }
    {
        EGLint got = 0;
        if (prio)
            eglQueryContext(s_dpy, s_ctx, EGL_CONTEXT_PRIORITY_LEVEL_IMG, &got);
        fprintf(stderr, "  [GPUGL] EGL context: %s, priority %s, reset notification %s\n",
                s_surf == EGL_NO_SURFACE ? "surfaceless" : "1x1 pbuffer",
                prio ? priority_name(got) : "default",
                s_reset_status ? "on" : "off");
    }
    return 1;
}

int nv2a_gl_context_lost(void)
{
    if (s_ctx == EGL_NO_CONTEXT)
        return 0;
    if (eglGetCurrentContext() != s_ctx)
        return 1;
    return s_robust && s_reset_status && s_reset_status() != GL_NO_ERROR;
}

/* The display, the config and the pbuffer stay for the next context. */
void nv2a_gl_context_destroy(void)
{
    if (s_ctx == EGL_NO_CONTEXT)
        return;
    eglMakeCurrent(s_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(s_dpy, s_ctx);
    s_ctx = EGL_NO_CONTEXT;
}
#endif
