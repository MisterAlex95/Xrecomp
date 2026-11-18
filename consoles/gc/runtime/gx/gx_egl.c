/* gx_egl.c -- EGL and GLES entry points loaded at run time, and a headless GLES 3 context for gc_run and the tests
 * (XBA-262). Linux: libEGL.so.1 / libGLESv2.so.2 (Mesa: EGL_MESA_platform_surfaceless, llvmpipe with
 * LIBGL_ALWAYS_SOFTWARE=1); Android: libEGL.so / libGLESv3.so. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gx.h"
#include "gx_glapi.h"

static void *egl_lib, *gles_lib;

static void *open_first(const char *const *names)
{
    for (int i = 0; names[i]; i++) {
        void *h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
        if (h)
            return h;
    }
    return NULL;
}

int gx_egl_load(gx_eglfn *egl)
{
    static const char *const egl_names[] = {"libEGL.so.1", "libEGL.so", NULL};
    static const char *const gles_names[] = {"libGLESv3.so", "libGLESv2.so.2", "libGLESv2.so", NULL};
    if (!egl_lib)
        egl_lib = open_first(egl_names);
    if (!gles_lib)
        gles_lib = open_first(gles_names);
    if (!egl_lib)
        return -1;
    memset(egl, 0, sizeof *egl);
    egl->eglGetProcAddress = (void *(*)(const char *))dlsym(egl_lib, "eglGetProcAddress");
    if (!egl->eglGetProcAddress)
        return -1;
#define E(name) *(void **)&egl->name = dlsym(egl_lib, #name)
    E(eglGetCurrentDisplay);
    E(eglGetCurrentContext);
    E(eglQueryContext);
    E(eglChooseConfig);
    E(eglCreateContext);
    E(eglDestroyContext);
    E(eglMakeCurrent);
    E(eglCreatePbufferSurface);
    E(eglDestroySurface);
    E(eglQueryString);
    E(eglInitialize);
    E(eglTerminate);
    E(eglBindAPI);
    E(eglGetDisplay);
    E(eglGetError);
#undef E
    *(void **)&egl->eglGetPlatformDisplayEXT = egl->eglGetProcAddress("eglGetPlatformDisplayEXT");
    return egl->eglGetCurrentContext && egl->eglMakeCurrent && egl->eglCreateContext ? 0 : -1;
}

static void *gl_sym(const gx_eglfn *egl, const char *name)
{
    void *p = gles_lib ? dlsym(gles_lib, name) : NULL;
    if (!p)
        p = egl->eglGetProcAddress(name);
    return p;
}

int gx_gl_load(gx_glfn *gl, const gx_eglfn *egl)
{
    int missing = 0;
    /* glClearDepthf and glProgramParameteri are GL 4.1 on a desktop: optional (gx_gl.c copes) */
#define GX_GL_LOAD(ret, name, args) \
    if (!(*(void **)&gl->name = gl_sym(egl, #name)) && strcmp(#name, "glClearDepthf") && \
        strcmp(#name, "glProgramParameteri")) { \
        fprintf(stderr, "gx: GL entry point %s missing\n", #name); \
        missing++; \
    }
    GX_GL_FUNCS(GX_GL_LOAD)
#undef GX_GL_LOAD
    *(void **)&gl->glMaxShaderCompilerThreadsKHR = egl->eglGetProcAddress("glMaxShaderCompilerThreadsKHR");
    *(void **)&gl->glClearDepth = gl_sym(egl, "glClearDepth");
    *(void **)&gl->glGetProgramBinary = gl_sym(egl, "glGetProgramBinary");
    *(void **)&gl->glProgramBinary = gl_sym(egl, "glProgramBinary");
    *(void **)&gl->glBufferStorageEXT = gl_sym(egl, "glBufferStorageEXT");
    return missing ? -1 : 0;
}

/* ---- headless context ---- */

static gx_eglfn hl;
static EGLDisplay hl_dpy;
static EGLContext hl_ctx;
static EGLSurface hl_surf;

int gx_egl_headless_create(bool software, char *why, size_t why_len)
{
    if (software)
        setenv("LIBGL_ALWAYS_SOFTWARE", "1", 0);
    if (gx_egl_load(&hl)) {
        snprintf(why, why_len, "no EGL library");
        return -1;
    }
    hl_dpy = EGL_NO_DISPLAY;
    hl_surf = EGL_NO_SURFACE;
    const char *client = hl.eglQueryString ? hl.eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS) : NULL;
    if (hl.eglGetPlatformDisplayEXT && client && strstr(client, "EGL_MESA_platform_surfaceless"))
        hl_dpy = hl.eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (hl_dpy == EGL_NO_DISPLAY && hl.eglGetDisplay)
        hl_dpy = hl.eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint ma = 0, mi = 0;
    if (hl_dpy == EGL_NO_DISPLAY || !hl.eglInitialize(hl_dpy, &ma, &mi)) {
        snprintf(why, why_len, "no EGL display");
        return -1;
    }
    hl.eglBindAPI(EGL_OPENGL_ES_API);
    EGLConfig cfg = NULL;
    EGLint n = 0;
    static const EGLint cattr[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
    if (!hl.eglChooseConfig(hl_dpy, cattr, &cfg, 1, &n) || n < 1)
        cfg = NULL; /* EGL_KHR_no_config_context */
    static const EGLint attr[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    hl_ctx = hl.eglCreateContext(hl_dpy, cfg, EGL_NO_CONTEXT, attr);
    if (hl_ctx == EGL_NO_CONTEXT) {
        snprintf(why, why_len, "eglCreateContext failed (0x%X)", hl.eglGetError ? hl.eglGetError() : 0);
        return -1;
    }
    if (!hl.eglMakeCurrent(hl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, hl_ctx)) {
        /* no EGL_KHR_surfaceless_context: a 1 x 1 pbuffer (the GX renderer draws into its own FBOs anyway) */
        EGLint err = hl.eglGetError ? hl.eglGetError() : 0;
        hl.eglDestroyContext(hl_dpy, hl_ctx);
        hl_ctx = EGL_NO_CONTEXT;
        static const EGLint pcfg[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                      EGL_NONE};
        static const EGLint pattr[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
        n = 0;
        if (hl.eglChooseConfig(hl_dpy, pcfg, &cfg, 1, &n) && n >= 1 && hl.eglCreatePbufferSurface)
            hl_surf = hl.eglCreatePbufferSurface(hl_dpy, cfg, pattr);
        if (hl_surf != EGL_NO_SURFACE)
            hl_ctx = hl.eglCreateContext(hl_dpy, cfg, EGL_NO_CONTEXT, attr);
        if (hl_ctx == EGL_NO_CONTEXT || !hl.eglMakeCurrent(hl_dpy, hl_surf, hl_surf, hl_ctx)) {
            snprintf(why, why_len, "surfaceless eglMakeCurrent failed (0x%X), no pbuffer either", err);
            gx_egl_headless_destroy();
            return -1;
        }
    }
    return 0;
}

void gx_egl_headless_destroy(void)
{
    if (hl_ctx != EGL_NO_CONTEXT) {
        hl.eglMakeCurrent(hl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        hl.eglDestroyContext(hl_dpy, hl_ctx);
        hl_ctx = EGL_NO_CONTEXT;
    }
    if (hl_surf != EGL_NO_SURFACE) {
        hl.eglDestroySurface(hl_dpy, hl_surf);
        hl_surf = EGL_NO_SURFACE;
    }
    if (hl_dpy != EGL_NO_DISPLAY) {
        hl.eglTerminate(hl_dpy);
        hl_dpy = EGL_NO_DISPLAY;
    }
}
