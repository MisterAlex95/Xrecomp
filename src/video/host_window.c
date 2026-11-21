/* host_window.c -- see host_window.h (XBA-145). */
#include "host_window.h"
#include "host_pad.h"
#include "host_time.h"
#include "perf_overlay.h"
#include "host_shader_progress.h"
#include "recomp_lifecycle.h"
#include "recomp_window_keys.h" /* F11 / Alt+Enter fullscreen, F12 screenshot (XBA-278) */

#include <SDL.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#if defined(XBOXRECOMP_LSFG)
/* Frame generation in the console window (XBA-186, docs/technical/framegen.md « Console window »): only in a build
 * with XBOXRECOMP_LSFG (the root CMakeLists defines it on xbr_host), active under RECOMP_LSFG=1. */
#include "framegen_console.h"
#include "framegen_policy.h"
#include "recomp_states.h"
#if defined(XBOXRECOMP_LSFG_BRIDGE)
#include "lsfg_generator.h"
#endif
#endif

/* The few GL entry points the present needs, from SDL: no GL header or GL
 * library at build time, the same code on GLES 3.0 and desktop 3.2 core. */
#ifndef APIENTRY
#define APIENTRY
#endif

#define XGL_TEXTURE_2D          0x0DE1
#define XGL_UNSIGNED_BYTE       0x1401
#define XGL_RGBA                0x1908
#define XGL_RGBA8               0x8058
#define XGL_NEAREST             0x2600
#define XGL_LINEAR              0x2601
#define XGL_PACK_ALIGNMENT      0x0D05
#define XGL_PACK_ROW_LENGTH     0x0D02
#define XGL_PACK_SKIP_ROWS      0x0D03
#define XGL_PACK_SKIP_PIXELS    0x0D04
#define XGL_PIXEL_PACK_BUFFER   0x88EB
#define XGL_PIXEL_PACK_BUFFER_BINDING 0x88ED
#define XGL_PIXEL_UNPACK_BUFFER 0x88EC
#define XGL_PIXEL_UNPACK_BUFFER_BINDING 0x88EF
#define XGL_VIEWPORT            0x0BA2
#define XGL_STREAM_READ         0x88E1
#define XGL_BUFFER_SIZE         0x8764
#define XGL_MAP_READ_BIT        0x0001
#define XGL_TEXTURE_MAG_FILTER  0x2800
#define XGL_TEXTURE_MIN_FILTER  0x2801
#define XGL_TEXTURE_WRAP_S      0x2802
#define XGL_TEXTURE_WRAP_T      0x2803
#define XGL_CLAMP_TO_EDGE       0x812F
#define XGL_COLOR_BUFFER_BIT    0x4000
#define XGL_TRIANGLES           0x0004
#define XGL_TEXTURE0            0x84C0
#define XGL_TEXTURE1            0x84C1
#define XGL_FRAGMENT_SHADER     0x8B30
#define XGL_VERTEX_SHADER       0x8B31
#define XGL_COMPILE_STATUS      0x8B81
#define XGL_LINK_STATUS         0x8B82
#define XGL_UNPACK_ROW_LENGTH   0x0CF2
#define XGL_UNPACK_ALIGNMENT    0x0CF5
#define XGL_FRAMEBUFFER         0x8D40
#define XGL_COLOR_ATTACHMENT0   0x8CE0
#define XGL_FRAMEBUFFER_COMPLETE 0x8CD5
#define XGL_SCISSOR_TEST        0x0C11
#define XGL_BLEND               0x0BE2
#define XGL_DEPTH_TEST          0x0B71
#define XGL_CULL_FACE           0x0B44
#define XGL_STENCIL_TEST        0x0B90
#define XGL_SRC_ALPHA           0x0302
#define XGL_ONE_MINUS_SRC_ALPHA 0x0303

typedef struct {
    void (APIENTRY *GenTextures)(int, unsigned *);
    void (APIENTRY *DeleteTextures)(int, const unsigned *);
    void (APIENTRY *BindTexture)(unsigned, unsigned);
    void (APIENTRY *TexParameteri)(unsigned, unsigned, int);
    void (APIENTRY *TexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
    void (APIENTRY *TexSubImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
    void (APIENTRY *PixelStorei)(unsigned, int);
    void (APIENTRY *Viewport)(int, int, int, int);
    void (APIENTRY *ClearColor)(float, float, float, float);
    void (APIENTRY *Clear)(unsigned);
    void (APIENTRY *ColorMask)(unsigned char, unsigned char, unsigned char, unsigned char);
    void (APIENTRY *Disable)(unsigned);
    void (APIENTRY *Enable)(unsigned);
    void (APIENTRY *BlendFunc)(unsigned, unsigned);
    unsigned (APIENTRY *CreateShader)(unsigned);
    void (APIENTRY *ShaderSource)(unsigned, int, const char *const *, const int *);
    void (APIENTRY *CompileShader)(unsigned);
    void (APIENTRY *GetShaderiv)(unsigned, unsigned, int *);
    void (APIENTRY *GetShaderInfoLog)(unsigned, int, int *, char *);
    void (APIENTRY *DeleteShader)(unsigned);
    unsigned (APIENTRY *CreateProgram)(void);
    void (APIENTRY *AttachShader)(unsigned, unsigned);
    void (APIENTRY *LinkProgram)(unsigned);
    void (APIENTRY *GetProgramiv)(unsigned, unsigned, int *);
    void (APIENTRY *GetProgramInfoLog)(unsigned, int, int *, char *);
    void (APIENTRY *DeleteProgram)(unsigned);
    void (APIENTRY *UseProgram)(unsigned);
    int (APIENTRY *GetUniformLocation)(unsigned, const char *);
    void (APIENTRY *Uniform1i)(int, int);
    void (APIENTRY *Uniform1f)(int, float);
    void (APIENTRY *Uniform2f)(int, float, float);
    void (APIENTRY *GenVertexArrays)(int, unsigned *);
    void (APIENTRY *DeleteVertexArrays)(int, const unsigned *);
    void (APIENTRY *BindVertexArray)(unsigned);
    void (APIENTRY *BindFramebuffer)(unsigned, unsigned);
    void (APIENTRY *ActiveTexture)(unsigned);
    void (APIENTRY *DrawArrays)(unsigned, int, int);
    unsigned (APIENTRY *GetError)(void);
    void (APIENTRY *ReadPixels)(int, int, int, int, unsigned, unsigned, void *);
    void (APIENTRY *GetIntegerv)(unsigned, int *);
} HostGl;

static SDL_Window *s_win;
static SDL_GLContext s_ctx;
static HostGl s_gl;
static unsigned s_prog, s_vao, s_tex;
static int s_tex_w, s_tex_h;
static unsigned s_tex_prev;               /* LCD ghosting: the previous picture (s_tex and it swap each upload) */
static int s_tex_prev_w, s_tex_prev_h, s_tex_prev_filter = -1;
static int s_u_tex, s_u_flip;
static int s_gles, s_major, s_minor, s_vsync;
static int s_quit;
static uint64_t s_frames;
static host_window_event_fn s_hook;
static void *s_hook_user;
static int s_scale = HOST_SCALE_ASPECT, s_filter = HOST_FILTER_LINEAR, s_tex_filter = -1;
static int s_aspect_num, s_aspect_den; /* host_window_set_aspect; 0: the picture's own */
static char s_capture[1024];
static int s_shot_req; /* F12 (XBA-278): a screenshot of the picture shown next (written at its present) */
static int s_drawing_generated; /* the picture being drawn is a generated frame: no capture of it */
static int s_read_lost; /* atomic: SDL made a new GL context (Android, on resume): the read-back's objects are gone */
/* The performance overlay (perf_overlay.h, RECOMP_OVERLAY=1): its text as a texture in the top left corner, drawn
 * over every picture (XBA-243: the same fields as the Xbox window's). */
static unsigned s_ov_prog, s_ov_tex, s_ov_serial;
static unsigned s_bd_tex, s_bd_serial; /* the shader badge's texture (XBA-314), drawn with the overlay's program */
static int s_ov_u_tex, s_ov_u_flip, s_ov_failed;
static uint64_t s_ov_tick_ms;
#if defined(XBOXRECOMP_LSFG)
static void fg_open(void);  /* frame generation (XBA-186), below present_rgba */
static void fg_close(void);
#endif

/* host_window_stats: written by the presenting thread, read from any. */
static int s_open;
static uint64_t s_frames_pub, s_last_present_ns, s_fps_t0, s_fps_n;
static uint32_t s_fps_centi;

/* host_window_screenshot_bmp: a copy of the last present_rgba picture (RGBA,
 * tightly packed, row 0 at the top) and the shape it was shown at. */
static pthread_mutex_t s_last_mu = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *s_last;
static size_t s_last_cap;
static int s_last_w, s_last_h, s_last_anum, s_last_aden;
static host_window_last_fn s_last_fn; /* host_window_present_rgba_shared: the caller has the picture */
static void *s_last_user;
/* What the window shows last, for host_window_screenshot_bmp. Written by the presenting thread only (under s_last_mu,
 * which lets it read it without), read by any thread under s_last_mu. LAST_GPU: a texture (present_texture), read
 * back when asked by the presenting thread (s_gpu_last, the grab below); s_last then holds the last grab. */
enum { LAST_NONE, LAST_COPY, LAST_SHARED, LAST_GPU };
static int s_last_kind;
static struct {
    unsigned tex;
    int w, h, top_down, anum, aden;
    SDL_GLContext ctx;
} s_gpu_last;   /* the presenting thread's own: no lock */
/* A screenshot of a GPU picture asked from another thread: served by the presenting thread at its next
 * present_texture or pump (a paused game only pumps). Serials and result under s_last_mu. */
static pthread_cond_t s_grab_cv = PTHREAD_COND_INITIALIZER;
static int s_grab_req;              /* atomic: a request waits */
static unsigned s_grab_asked, s_grab_served;
static int s_grab_ok;               /* the last service read the picture into s_last */
static pthread_t s_present_thread;  /* the thread that opened the window */
static void grab_service(void);

static int env_set(const char *name)
{
    const char *v = getenv(name);
    return v && v[0];
}

int host_window_headless(void)
{
    const char *drv = getenv("SDL_VIDEODRIVER");

    if (env_set("XBR_BUILD_ONLY"))
        return 1;
    if (drv && (!strcmp(drv, "dummy") || !strcmp(drv, "offscreen")))
        return 1;
#if defined(__linux__) && !defined(__ANDROID__)
    if (!env_set("DISPLAY") && !env_set("WAYLAND_DISPLAY"))
        return 1;
#endif
    return 0;
}

static int load_gl(void)
{
    struct { void **slot; const char *name; } fns[] = {
#define F(n) { (void **)&s_gl.n, "gl" #n }
        F(GenTextures), F(DeleteTextures), F(BindTexture), F(TexParameteri), F(TexImage2D), F(TexSubImage2D),
        F(PixelStorei), F(ReadPixels), F(Viewport), F(ClearColor), F(Clear), F(ColorMask), F(Disable), F(Enable), F(BlendFunc),
        F(CreateShader),
        F(ShaderSource),
        F(CompileShader), F(GetShaderiv), F(GetShaderInfoLog), F(DeleteShader), F(CreateProgram),
        F(AttachShader), F(LinkProgram), F(GetProgramiv), F(GetProgramInfoLog), F(DeleteProgram),
        F(UseProgram), F(GetUniformLocation), F(Uniform1i), F(Uniform1f), F(Uniform2f), F(GenVertexArrays),
        F(DeleteVertexArrays), F(BindVertexArray), F(BindFramebuffer), F(ActiveTexture), F(DrawArrays),
        F(GetError), F(GetIntegerv),
#undef F
    };
    for (size_t i = 0; i < sizeof fns / sizeof fns[0]; i++) {
        *fns[i].slot = SDL_GL_GetProcAddress(fns[i].name);
        if (!*fns[i].slot) {
            fprintf(stderr, "  [HOST] window: no %s in the GL context\n", fns[i].name);
            return 0;
        }
    }
    return 1;
}

static unsigned compile3(unsigned type, const char *head, const char *common, const char *body)
{
    const char *src[3] = { head, common, body };
    unsigned sh = s_gl.CreateShader(type);
    int ok = 0;

    s_gl.ShaderSource(sh, 3, src, NULL);
    s_gl.CompileShader(sh);
    s_gl.GetShaderiv(sh, XGL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        s_gl.GetShaderInfoLog(sh, (int)sizeof log, NULL, log);
        fprintf(stderr, "  [HOST] window: shader: %s\n", log);
        s_gl.DeleteShader(sh);
        return 0;
    }
    return sh;
}

static unsigned compile(unsigned type, const char *head, const char *body)
{
    return compile3(type, head, "", body);
}

/* One triangle over the viewport, no vertex buffer (gl_VertexID 0..2). */
static const char k_vs[] =
    "out vec2 v_uv;\n"
    "uniform float u_flip;\n"
    "void main() {\n"
    "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "    v_uv = vec2(p.x, mix(p.y, 1.0 - p.y, u_flip));\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";
static const char k_fs[] =
    "in vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "out vec4 o_color;\n"
    "void main() { o_color = vec4(texture(u_tex, v_uv).rgb, 1.0); }\n";

/* The overlay's pixels are 0xAARRGGBB words, bytes B, G, R, A: uploaded as RGBA, swapped back here; its alpha
 * kept (blended). */
static const char k_ov_fs[] =
    "in vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "out vec4 o_color;\n"
    "void main() { o_color = texture(u_tex, v_uv).bgra; }\n";

static int build_program(void)
{
    const char *head_vs = s_gles ? "#version 300 es\n" : "#version 150\n";
    const char *head_fs = s_gles ? "#version 300 es\nprecision mediump float;\n" : "#version 150\n";
    unsigned vs = compile(XGL_VERTEX_SHADER, head_vs, k_vs);
    unsigned fs = vs ? compile(XGL_FRAGMENT_SHADER, head_fs, k_fs) : 0;
    int ok = 0;

    if (!vs || !fs) {
        if (vs)
            s_gl.DeleteShader(vs);
        return 0;
    }
    s_prog = s_gl.CreateProgram();
    s_gl.AttachShader(s_prog, vs);
    s_gl.AttachShader(s_prog, fs);
    s_gl.LinkProgram(s_prog);
    s_gl.DeleteShader(vs);
    s_gl.DeleteShader(fs);
    s_gl.GetProgramiv(s_prog, XGL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        s_gl.GetProgramInfoLog(s_prog, (int)sizeof log, NULL, log);
        fprintf(stderr, "  [HOST] window: program: %s\n", log);
        s_gl.DeleteProgram(s_prog);
        s_prog = 0;
        return 0;
    }
    s_u_tex = s_gl.GetUniformLocation(s_prog, "u_tex");
    s_u_flip = s_gl.GetUniformLocation(s_prog, "u_flip");
    s_gl.GenVertexArrays(1, &s_vao);
    return 1;
}

/* ── Picture shaders (XBA-195) ──────────────────────────────────────────
 * The presets, their GLSL and uniforms are present_shaders.c's (shared with the Xbox presenter, XBA-203): GLSL ES
 * 3.00 / 1.50 here, over the same full-viewport triangle as the plain present. Built on first use; a failure falls
 * back to none, logged once. */
static HostShader s_shader = { HOST_SHADER_NONE, -1, 0, 0 };
static PresentShaderProg s_sprog[HOST_SHADER_COUNT + 1];   /* the presets, then PRESENT_SHADER_AREA */
/* Downscale-aware present (present_shader_taps): 1 on, 0 off, -1 from $RECOMP_PRESENT_DOWNSCALE at the next draw.
 * s_area_drawn: the last draw averaged a minified picture (logged when it changes). */
static int s_downscale = -1, s_area_drawn = -1;

/* The program of the preset to draw with, built on first use; NULL for none or a preset that failed. */
static PresentShaderProg *shader_prog(void)
{
    int p = s_shader.preset;

    if (p <= HOST_SHADER_NONE || p >= HOST_SHADER_COUNT)
        return NULL;
    if (!present_shader_build(&s_sprog[p], s_gles ? PRESENT_GLSL_ES300 : PRESENT_GLSL_150, p, "[HOST] window:"))
        return NULL;
    return &s_sprog[p];
}

/* The area program (none drawn on a minified picture), built on first use; NULL when it failed. */
static PresentShaderProg *area_prog(void)
{
    if (!present_shader_build(&s_sprog[PRESENT_SHADER_AREA], s_gles ? PRESENT_GLSL_ES300 : PRESENT_GLSL_150,
                              PRESENT_SHADER_AREA, "[HOST] window:"))
        return NULL;
    return &s_sprog[PRESENT_SHADER_AREA];
}

static void shader_free(void)
{
    for (int i = 0; i <= PRESENT_SHADER_AREA; i++)
        present_shader_release(&s_sprog[i], 0);
}

/* The presets that blend texels themselves want the texture sampled linearly whatever the filter. */
static int shader_wants_linear(void)
{
    int p = s_shader.preset;

    return (p == HOST_SHADER_SHARP || p == HOST_SHADER_FXAA || p == HOST_SHADER_CAS) && !s_sprog[p].failed;
}

/* ── Live picture changes (the in-game menu) ─────────────────────────────
 * host_window_request_picture may come from any thread (Android's UI thread); the presenting thread takes the
 * request at its next present (live_take), so scale, filter and shader only ever change between two frames. */
static pthread_mutex_t s_live_mu = PTHREAD_MUTEX_INITIALIZER;
static int s_live_pending;               /* atomic: a request waits */
static int s_live_scale = -1, s_live_filter = -1, s_live_has_shader;
static HostShader s_live_shader;

void host_window_request_picture(int scale, int filter, const HostShader *s)
{
    pthread_mutex_lock(&s_live_mu);
    s_live_scale = scale;
    s_live_filter = filter;
    s_live_has_shader = s != NULL;
    if (s)
        s_live_shader = *s;
    pthread_mutex_unlock(&s_live_mu);
    __atomic_store_n(&s_live_pending, 1, __ATOMIC_RELEASE);
}

static void live_take(void)
{
    int scale, filter, has;
    HostShader sh;

    if (!__atomic_load_n(&s_live_pending, __ATOMIC_ACQUIRE))
        return;
    pthread_mutex_lock(&s_live_mu);
    __atomic_store_n(&s_live_pending, 0, __ATOMIC_RELAXED);
    scale = s_live_scale;
    filter = s_live_filter;
    has = s_live_has_shader;
    sh = s_live_shader;
    pthread_mutex_unlock(&s_live_mu);
    host_window_set_scaling(scale < 0 ? s_scale : scale, filter < 0 ? s_filter : filter);
    if (has)
        host_window_set_shader(&sh);
    fprintf(stderr, "  [HOST] window: picture changed live: scale %d, filter %d, shader %s\n", s_scale, s_filter,
            host_window_shader_name(s_shader.preset));
}

/* LCD ghosting wants the previous picture: present_rgba then alternates between two textures. */
static int shader_wants_prev(void)
{
    return s_shader.preset == HOST_SHADER_LCD && s_shader.ghosting > 0 && !s_sprog[HOST_SHADER_LCD].failed;
}

/* The app going to the background and back: on Android these must be seen
 * as they happen, from the event watch, not at the next pump. */
static int SDLCALL lifecycle_watch(void *user, SDL_Event *ev)
{
    (void)user;
    if (ev->type == SDL_APP_WILLENTERBACKGROUND)
        recomp_lifecycle_pause();
    else if (ev->type == SDL_APP_DIDENTERFOREGROUND)
        recomp_lifecycle_resume();
    return 1;
}

static int try_context(const char *title, int w, int h, Uint32 flags, int gles, int major, int minor)
{
    SDL_GL_ResetAttributes();
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        gles ? SDL_GL_CONTEXT_PROFILE_ES : SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor);
    if (!gles)
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);

    s_win = SDL_CreateWindow(title ? title : "xrecomp", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             w, h, flags);
    if (!s_win)
        return 0;
    s_ctx = SDL_GL_CreateContext(s_win);
    if (!s_ctx) {
        SDL_DestroyWindow(s_win);
        s_win = NULL;
        return 0;
    }
    s_gles = gles;
    s_major = major;
    s_minor = minor;
    return 1;
}

int host_window_open(const char *title, int width, int height, int vsync)
{
    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;

    if (s_win)
        return 1;
    if (width <= 0 || height <= 0) {
        width = 1280;
        height = 720;
    }
#ifdef __ANDROID__
    flags |= SDL_WINDOW_FULLSCREEN;
#else
    if (recomp_window_fullscreen_env()) /* XBA-278: as the Xbox presenters */
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
#endif
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "  [HOST] window: SDL video: %s\n", SDL_GetError());
        return 0;
    }
    int ok = 0;
#if !defined(__APPLE__)
    /* RECOMP_HOST_GL=core skips GLES (a check of the macOS path on Linux). */
    const char *want = getenv("RECOMP_HOST_GL");
    if (!want || strcmp(want, "core") != 0)
        ok = try_context(title, width, height, flags, 1, 3, 0);   /* GLES 3.0: Android, Mesa */
#endif
    if (!ok)
        ok = try_context(title, width, height, flags, 0, 3, 2);   /* 3.2 core: macOS, desktop drivers */
    if (!ok) {
        fprintf(stderr, "  [HOST] window: no GL ES 3.0 or 3.2 core context: %s\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER);
        return 0;
    }
    if (!load_gl() || !build_program()) {
        host_window_close();
        return 0;
    }
    s_vsync = 0;
    if (vsync) {
        if (SDL_GL_SetSwapInterval(-1) == 0 || SDL_GL_SetSwapInterval(1) == 0)
            s_vsync = 1;
    } else {
        SDL_GL_SetSwapInterval(0);
    }
    SDL_AddEventWatch(lifecycle_watch, NULL);
    host_pad_init();
    s_quit = 0;
    s_frames = 0;
    s_fps_t0 = s_fps_n = 0;
    __atomic_store_n(&s_frames_pub, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_fps_centi, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_last_present_ns, 0, __ATOMIC_RELAXED);
    s_present_thread = pthread_self();
    __atomic_store_n(&s_open, 1, __ATOMIC_RELEASE);
    fprintf(stderr, "  [HOST] window: %dx%d, GL%s %d.%d, vsync %s\n", width, height, s_gles ? " ES" : " core",
            s_major, s_minor, s_vsync ? "on" : "off");
#if defined(XBOXRECOMP_LSFG)
    fg_open();
#endif
    return 1;
}

void host_window_set_event_hook(host_window_event_fn fn, void *user)
{
    s_hook = fn;
    s_hook_user = user;
}

/* F11 / Alt+Enter (XBA-278): borderless fullscreen on the window's display, and back. Android's window is always
 * fullscreen. */
static void toggle_fullscreen(void)
{
#ifndef __ANDROID__
    if (!s_win)
        return;
    int on = !(SDL_GetWindowFlags(s_win) & SDL_WINDOW_FULLSCREEN);
    if (SDL_SetWindowFullscreen(s_win, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) == 0)
        fprintf(stderr, "  [HOST] window: fullscreen %s\n", on ? "on" : "off");
    else
        fprintf(stderr, "  [HOST] window: fullscreen: %s\n", SDL_GetError());
#endif
}

/* The F12 screenshot, once the picture it shows is kept (recomp_window_keys.h: where and how it is named). */
static void shot_write(void)
{
    char path[1200];
    __atomic_store_n(&s_shot_req, 0, __ATOMIC_RELAXED);
    if (recomp_screenshot_path(path, sizeof path, s_win ? SDL_GetWindowTitle(s_win) : NULL, time(NULL)) != 0) {
        fprintf(stderr, "  [HOST] window: screenshot: no folder to write it to (RECOMP_SCREENSHOT_DIR)\n");
        return;
    }
    if (host_window_screenshot_bmp(path) != 0)
        fprintf(stderr, "  [HOST] window: screenshot: cannot write %s\n", path);
}

void host_window_request_screenshot(void)
{
    __atomic_store_n(&s_shot_req, 1, __ATOMIC_RELAXED);
}

int host_window_pump(void)
{
    SDL_Event ev;

    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT:
            s_quit = 1;
            break;
        case SDL_KEYDOWN:
            if (!ev.key.repeat) {
                int k = recomp_window_key((int)ev.key.keysym.scancode, (ev.key.keysym.mod & KMOD_ALT) != 0);
                if (k == RECOMP_WK_FULLSCREEN)
                    toggle_fullscreen();
                else if (k == RECOMP_WK_SCREENSHOT)
                    host_window_request_screenshot();
            }
            break;
        case SDL_WINDOWEVENT:
            if (ev.window.event == SDL_WINDOWEVENT_CLOSE)
                s_quit = 1;
            break;
        case SDL_RENDER_DEVICE_RESET:
            __atomic_store_n(&s_read_lost, 1, __ATOMIC_RELEASE);
            break;
        default:
            break;
        }
        host_pad_handle_event(&ev);
        if (s_hook)
            s_hook(&ev, s_hook_user);
    }
    grab_service();   /* a paused game only pumps: a screenshot asked meanwhile is read here */
    return !s_quit;
}

/* host_window_capture: the drawable as drawn, before the swap, as a binary
 * PPM (row 0 at the top). Once per request. */
static void capture(int dw, int dh)
{
    unsigned char *px = malloc((size_t)dw * dh * 4);
    FILE *f = px ? fopen(s_capture, "wb") : NULL;

    if (f) {
        s_gl.PixelStorei(XGL_PACK_ALIGNMENT, 4);
        s_gl.ReadPixels(0, 0, dw, dh, XGL_RGBA, XGL_UNSIGNED_BYTE, px);
        fprintf(f, "P6\n%d %d\n255\n", dw, dh);
        for (int y = dh - 1; y >= 0; y--)
            for (int x = 0; x < dw; x++)
                fwrite(px + ((size_t)y * dw + x) * 4, 1, 3, f);
        fprintf(stderr, "  [HOST] window: %dx%d drawable captured to %s\n", dw, dh, s_capture);
    }
    if (!f || fclose(f) != 0)
        fprintf(stderr, "  [HOST] window: cannot write the capture %s\n", s_capture);
    free(px);
    s_capture[0] = 0;
}

/* host_window_stats: one more present; the rate is taken over a second. */
static void count_frame(void)
{
    uint64_t now = xbox_HostNowNs();

    __atomic_store_n(&s_frames_pub, s_frames, __ATOMIC_RELAXED);
    __atomic_store_n(&s_last_present_ns, now, __ATOMIC_RELAXED);
    if (!s_fps_t0 || now - s_fps_t0 > 4000000000ull) {   /* first frame, or back from a long hold */
        s_fps_t0 = now;
        s_fps_n = 0;
        return;
    }
    s_fps_n++;
    if (now - s_fps_t0 >= 1000000000ull) {
        __atomic_store_n(&s_fps_centi, (uint32_t)(s_fps_n * 100000000000ull / (now - s_fps_t0)), __ATOMIC_RELAXED);
        s_fps_t0 = now;
        s_fps_n = 0;
    }
}

/* The picture into s_last, s_last_mu held; 0 when there is no room for it. */
static int last_store(const uint8_t *px, int w, int h, int pitch, int anum, int aden)
{
    size_t need = (size_t)w * h * 4;

    if (need > s_last_cap) {
        uint8_t *n = realloc(s_last, need);
        if (!n)
            return 0;
        s_last = n;
        s_last_cap = need;
    }
    for (int y = 0; y < h; y++)
        memcpy(s_last + (size_t)y * w * 4, px + (size_t)y * pitch, (size_t)w * 4);
    s_last_w = w;
    s_last_h = h;
    s_last_anum = anum;
    s_last_aden = aden;
    s_last_fn = NULL;
    return 1;
}

/* host_window_screenshot_bmp: keeps the picture just shown. */
static void keep_last(const uint8_t *px, int w, int h, int pitch)
{
    if (w <= 0 || h <= 0)
        return;
    pthread_mutex_lock(&s_last_mu);
    /* No room: an older copy, or a texture's grab, is not this picture. */
    s_last_kind = last_store(px, w, h, pitch, s_aspect_num, s_aspect_den) ? LAST_COPY : LAST_NONE;
    pthread_mutex_unlock(&s_last_mu);
}

/* host_window_present_rgba_shared: no copy, the caller's `last` gives the picture on demand. */
static void keep_shared(host_window_last_fn fn, void *user, int w, int h)
{
    pthread_mutex_lock(&s_last_mu);
    s_last_fn = fn;
    s_last_user = user;
    s_last_w = w;
    s_last_h = h;
    s_last_anum = s_aspect_num;
    s_last_aden = s_aspect_den;
    s_last_kind = LAST_SHARED;
    pthread_mutex_unlock(&s_last_mu);
}

/* The overlay's program (text pixels over the picture, blended), built the first time the overlay or the shader
 * badge is drawn. 0 when it failed (once: logged, never tried again). */
static unsigned ov_program(void)
{
    if (s_ov_prog || s_ov_failed)
        return s_ov_prog;
    const char *head_vs = s_gles ? "#version 300 es\n" : "#version 150\n";
    const char *head_fs = s_gles ? "#version 300 es\nprecision mediump float;\n" : "#version 150\n";
    unsigned vs = compile(XGL_VERTEX_SHADER, head_vs, k_vs);
    unsigned fs = vs ? compile(XGL_FRAGMENT_SHADER, head_fs, k_ov_fs) : 0;
    int ok = 0;
    if (vs && fs) {
        s_ov_prog = s_gl.CreateProgram();
        s_gl.AttachShader(s_ov_prog, vs);
        s_gl.AttachShader(s_ov_prog, fs);
        s_gl.LinkProgram(s_ov_prog);
        s_gl.GetProgramiv(s_ov_prog, XGL_LINK_STATUS, &ok);
    }
    if (vs)
        s_gl.DeleteShader(vs);
    if (fs)
        s_gl.DeleteShader(fs);
    if (!ok) {
        fprintf(stderr, "  [HOST] window: the overlay's program failed; no overlay, no shader badge\n");
        if (s_ov_prog)
            s_gl.DeleteProgram(s_ov_prog);
        s_ov_prog = 0;
        s_ov_failed = 1;
        return 0;
    }
    s_ov_u_tex = s_gl.GetUniformLocation(s_ov_prog, "u_tex");
    s_ov_u_flip = s_gl.GetUniformLocation(s_ov_prog, "u_flip");
    return s_ov_prog;
}

/* Text pixels (0xAARRGGBB, w x h, row 0 at the top) into *tex (made the first time, uploaded when serial is not
 * *tex_serial), drawn k drawable pixels per text pixel with its top left at (x, top) counted from the drawable's top
 * left (dh high). */
static void ov_blit(unsigned *tex, unsigned *tex_serial, const uint32_t *px, int w, int h, unsigned serial, int x,
                    int top, int k, int dh)
{
    if (!ov_program())
        return;
    s_gl.ActiveTexture(XGL_TEXTURE0);
    if (!*tex) {
        s_gl.GenTextures(1, tex);
        s_gl.BindTexture(XGL_TEXTURE_2D, *tex);
        s_gl.TexParameteri(XGL_TEXTURE_2D, XGL_TEXTURE_MIN_FILTER, XGL_NEAREST);
        s_gl.TexParameteri(XGL_TEXTURE_2D, XGL_TEXTURE_MAG_FILTER, XGL_NEAREST);
        s_gl.TexParameteri(XGL_TEXTURE_2D, XGL_TEXTURE_WRAP_S, XGL_CLAMP_TO_EDGE);
        s_gl.TexParameteri(XGL_TEXTURE_2D, XGL_TEXTURE_WRAP_T, XGL_CLAMP_TO_EDGE);
        *tex_serial = serial - 1;
    }
    s_gl.BindTexture(XGL_TEXTURE_2D, *tex);
    if (serial != *tex_serial) {
        s_gl.PixelStorei(XGL_UNPACK_ALIGNMENT, 4);
        s_gl.PixelStorei(XGL_UNPACK_ROW_LENGTH, 0);
        s_gl.TexImage2D(XGL_TEXTURE_2D, 0, (int)XGL_RGBA8, w, h, 0, XGL_RGBA, XGL_UNSIGNED_BYTE, px);
        *tex_serial = serial;
    }
    s_gl.Viewport(x, dh - top - h * k, w * k, h * k);
    s_gl.Enable(XGL_BLEND);
    s_gl.BlendFunc(XGL_SRC_ALPHA, XGL_ONE_MINUS_SRC_ALPHA);
    s_gl.UseProgram(s_ov_prog);
    s_gl.Uniform1i(s_ov_u_tex, 0);
    s_gl.Uniform1f(s_ov_u_flip, 1.0f);
    s_gl.BindVertexArray(s_vao);
    s_gl.DrawArrays(XGL_TRIANGLES, 0, 3);
    s_gl.BindVertexArray(0);
    s_gl.Disable(XGL_BLEND);
}

/* The overlay over the picture just drawn: new figures every 500 ms, the text's texture uploaded when it changed,
 * about dh / 320 pixels per font pixel (as the Xbox window), in the drawable's top left corner. Returns its bottom,
 * counted from the drawable's top (0: no overlay). */
static int overlay_draw(int dw, int dh)
{
    int w = 0, h = 0, k;
    unsigned serial = 0;
    const uint32_t *px;
    uint64_t now_ms;

    if (!perf_overlay_enabled() || s_ov_failed)
        return 0;
    now_ms = xbox_HostNowNs() / 1000000ull;
    if (now_ms - s_ov_tick_ms >= 500) {
        perf_overlay_tick(now_ms, s_frames, PERF_OVERLAY_NO_DRAWS, NULL);
        s_ov_tick_ms = now_ms;
    }
    px = perf_overlay_pixels(&w, &h, &serial);
    if (!px)
        return 0;
    k = dh / 320 > 1 ? dh / 320 : 1;
    if (w * k > dw)
        k = 1;
    ov_blit(&s_ov_tex, &s_ov_serial, px, w, h, serial, k, k, k, dh);
    return s_ov_failed ? 0 : k + h * k;
}

/* The shader badge (XBA-314, perf_overlay_badge_*): "SHADERS 12/304" at the top left of the picture (vx, vy, vw x vh,
 * vy counted from the drawable's top), below the overlay when they would overlap (ov_bottom), the overlay's text size
 * against the picture's height: small and readable on a 4:3 or square screen and at any internal resolution (the
 * picture's size on screen decides, not its pixels). Always on; nothing to do while it is hidden. */
static void badge_draw(int dh, int vx, int vy, int vw, int vh, int ov_bottom)
{
    int w = 0, h = 0, k, x, top;
    unsigned serial = 0;
    const uint32_t *px;

    perf_overlay_badge_update(host_shader_progress_now_ms());
    px = perf_overlay_badge_pixels(&w, &h, &serial);
    if (!px || s_ov_failed)
        return;
    k = vh / 320 > 1 ? vh / 320 : 1;
    if (w * k > vw)
        k = 1;
    x = vx + k;
    top = vy + k;
    if (ov_bottom && top < ov_bottom + k)
        top = ov_bottom + k;
    if (top + h * k > dh)
        top = dh - h * k > 0 ? dh - h * k : 0;
    ov_blit(&s_bd_tex, &s_bd_serial, px, w, h, serial, x, top, k, dh);
}

static int draw(unsigned tex, unsigned prev, int w, int h, int top_down)
{
    int dw = 0, dh = 0, vw, vh;

    if (!s_win || !tex || w <= 0 || h <= 0)
        return 0;
    SDL_GL_GetDrawableSize(s_win, &dw, &dh);
    if (dw <= 0 || dh <= 0)
        return 0;
    /* Letterbox: the largest rectangle of the picture's aspect, or of a
     * whole multiple of its size (HOST_SCALE_INTEGER, when 1x fits). */
    int k = dw / w < dh / h ? dw / w : dh / h;
    /* The shape shown: the picture's own, or host_window_set_aspect's. */
    int64_t aw = w, ah = h;
    if (s_aspect_num > 0 && s_aspect_den > 0) {
        aw = s_aspect_num;
        ah = s_aspect_den;
        k = dh / h;
        if (k >= 1 && (int64_t)h * k * aw / ah > dw)
            k = 0; /* that height is too wide for the window */
    }
    if (s_scale == HOST_SCALE_STRETCH) {
        vw = dw;
        vh = dh;
    } else if (s_scale == HOST_SCALE_INTEGER && k >= 1) {
        vh = h * k;
        vw = s_aspect_num > 0 && s_aspect_den > 0 ? (int)((int64_t)vh * aw / ah) : w * k;
    } else if ((int64_t)dw * ah > (int64_t)dh * aw) {
        vh = dh;
        vw = (int)((int64_t)dh * aw / ah);
    } else {
        vw = dw;
        vh = (int)((int64_t)dw * ah / aw);
    }
    s_gl.BindFramebuffer(XGL_FRAMEBUFFER, 0);
    /* A GPU renderer shares this context and leaves its last draw's state: with colour writes off (the GameCube's
     * alpha-only passes) the clear and the picture below write nothing and the swap shows an old buffer again. */
    s_gl.ColorMask(1, 1, 1, 1);
    s_gl.Disable(XGL_CULL_FACE);
    s_gl.Disable(XGL_STENCIL_TEST);
    s_gl.Disable(XGL_SCISSOR_TEST);
    s_gl.Disable(XGL_BLEND);
    s_gl.Disable(XGL_DEPTH_TEST);
    s_gl.Viewport(0, 0, dw, dh);
    s_gl.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    s_gl.Clear(XGL_COLOR_BUFFER_BIT);
    s_gl.Viewport((dw - vw) / 2, (dh - vh) / 2, vw, vh);
    PresentShaderProg *sp = shader_prog();
    float taps[2];
    if (s_downscale < 0)
        s_downscale = present_downscale_env();
    int minified = present_shader_taps(s_downscale, (float)w, (float)h, (float)vw, (float)vh, taps);
    /* None on a minified picture: the area program instead of one bilinear (or nearest) tap per pixel. */
    if (!sp && minified)
        sp = area_prog();
    int averaged = minified && sp != NULL;   /* the area program or a preset (all of them average) */
    if (averaged != s_area_drawn) {
        if (averaged || s_area_drawn > 0)
            fprintf(stderr, "  [HOST] window: picture %dx%d in %dx%d: %s\n", w, h, vw, vh,
                    averaged ? "minified, texels averaged (RECOMP_PRESENT_DOWNSCALE=0: one tap)"
                             : "one tap per pixel");
        s_area_drawn = averaged;
    }
    if (sp) {
        /* Console lines: an upscaled picture (XBA-184) holds round(h / 240) output lines per console line. */
        int cells_h = present_shader_console_cells(h);
        PresentShaderParams pp = {
            .src_w = (float)w, .src_h = (float)h, .out_w = (float)vw, .out_h = (float)vh,
            .cells_w = (float)w * (float)cells_h / (float)h, .cells_h = (float)cells_h,
            .flip = top_down, .strength = present_shader_strength(&s_shader), .curvature = s_shader.curvature,
            .ghosting = prev && prev != tex ? s_shader.ghosting : 0, .downscale = s_downscale,
        };
        if (prev && prev != tex) {
            s_gl.ActiveTexture(XGL_TEXTURE1);
            s_gl.BindTexture(XGL_TEXTURE_2D, prev);
        }
        s_gl.ActiveTexture(XGL_TEXTURE0);
        s_gl.BindTexture(XGL_TEXTURE_2D, tex);
        present_shader_use(sp, &pp);
    } else {
        s_gl.UseProgram(s_prog);
        s_gl.ActiveTexture(XGL_TEXTURE0);
        s_gl.BindTexture(XGL_TEXTURE_2D, tex);
        s_gl.Uniform1i(s_u_tex, 0);
        s_gl.Uniform1f(s_u_flip, top_down ? 1.0f : 0.0f);
    }
    s_gl.BindVertexArray(s_vao);
    s_gl.DrawArrays(XGL_TRIANGLES, 0, 3);
    s_gl.BindVertexArray(0);
    badge_draw(dh, (dw - vw) / 2, (dh - vh) / 2, vw, vh, overlay_draw(dw, dh));
    unsigned err = s_gl.GetError();
    if (s_capture[0] && !s_drawing_generated)
        capture(dw, dh);
    SDL_GL_SwapWindow(s_win);
    s_frames++;
    count_frame();
    if (err) {
        fprintf(stderr, "  [HOST] window: GL error 0x%04x at present\n", err);
        return 0;
    }
    return 1;
}

#if defined(XBOXRECOMP_LSFG)
static int s_fg;   /* below: framegen_console running for this window (a tentative definition) */
static int fg_present(const void *px, int w, int h, int pitch);
static const uint8_t *rb_picture(unsigned tex, int w, int h, int top_down);
static void rb_idle(void);
static int s_rb_feeding;   /* fg_present is given the picture rb_picture returned */
static const uint8_t *texture_pixels(unsigned tex, int w, int h, int top_down);
#endif

int host_window_present_texture(unsigned tex, int w, int h, int top_down)
{
    int rc;

    live_take();
    /* What is shown, read back only when a screenshot asks for it (grab_service, after the draw; F12 now). */
    s_gpu_last.tex = tex;
    s_gpu_last.w = w;
    s_gpu_last.h = h;
    s_gpu_last.top_down = top_down;
    s_gpu_last.anum = s_aspect_num;
    s_gpu_last.aden = s_aspect_den;
    s_gpu_last.ctx = SDL_GL_GetCurrentContext();
    if (s_last_kind != LAST_GPU) {   /* the presenting thread is the only writer: read without the lock */
        pthread_mutex_lock(&s_last_mu);
        s_last_kind = LAST_GPU;
        s_last_fn = NULL;
        pthread_mutex_unlock(&s_last_mu);
    }
    if (__atomic_load_n(&s_shot_req, __ATOMIC_RELAXED))
        shot_write();   /* XBA-278: on this thread, the picture of this present grabbed at once */
#if defined(XBOXRECOMP_LSFG)
    /* XBA-274: frame generation for a picture the runtime drew on the GPU (the GameCube's GX and GPU-YUV paths):
     * read back while frame generation runs (not under fast-forward, which draws directly, nor once the pacer fell
     * back: nothing would take the copy), then as present_rgba. The pacer gets the previous present's picture
     * (rb_picture); a screenshot, above, always reads the current one. */
    if (s_fg && framegen_pacer_active() && !recomp_ff_active()) {
        const uint8_t *px = rb_picture(tex, w, h, top_down);
        s_rb_feeding = 1;
        int taken = px && fg_present(px, w, h, w * 4);
        s_rb_feeding = 0;
        if (taken) {
            grab_service();
            return 1;
        }
    } else {
        rb_idle();
    }
#endif
    rc = draw(tex, 0, w, h, top_down);
    grab_service();
    return rc;
}

/* The last upload_draw's texture pair and size, for host_window_present_again. */
static unsigned s_again_tex, s_again_prev;
static int s_again_w, s_again_h;

/* Uploads RGBA pixels into the window's texture and draws it. */
static int upload_draw(const void *px, int w, int h, int pitch)
{
    int ghost = shader_wants_prev();
    /* Sharp bilinear, FXAA and CAS blend texels themselves: they need linear sampling whatever the filter. (The
     * area filter samples texel centres: it draws the same with either.) */
    int filter = shader_wants_linear() ? HOST_FILTER_LINEAR : s_filter;

    if (ghost) {
        /* The texture of the previous picture takes this one; the last one becomes the previous. */
        unsigned t = s_tex;
        int tw = s_tex_w, th = s_tex_h, tf = s_tex_filter;
        s_tex = s_tex_prev;
        s_tex_w = s_tex_prev_w;
        s_tex_h = s_tex_prev_h;
        s_tex_filter = s_tex_prev_filter;
        s_tex_prev = t;
        s_tex_prev_w = tw;
        s_tex_prev_h = th;
        s_tex_prev_filter = tf;
    }
    if (!s_tex) {
        s_gl.GenTextures(1, &s_tex);
        s_tex_w = s_tex_h = 0;
        s_tex_filter = -1;
    }
    s_gl.ActiveTexture(XGL_TEXTURE0);
    s_gl.BindTexture(XGL_TEXTURE_2D, s_tex);
    s_gl.PixelStorei(XGL_UNPACK_ALIGNMENT, 4);
    s_gl.PixelStorei(XGL_UNPACK_ROW_LENGTH, pitch / 4);
    if (s_tex_filter != filter) {
        int f = filter == HOST_FILTER_NEAREST ? XGL_NEAREST : XGL_LINEAR;
        s_gl.TexParameteri(XGL_TEXTURE_2D, XGL_TEXTURE_MIN_FILTER, f);
        s_gl.TexParameteri(XGL_TEXTURE_2D, XGL_TEXTURE_MAG_FILTER, f);
        s_tex_filter = filter;
    }
    if (w != s_tex_w || h != s_tex_h) {
        s_gl.TexParameteri(XGL_TEXTURE_2D, XGL_TEXTURE_WRAP_S, XGL_CLAMP_TO_EDGE);
        s_gl.TexParameteri(XGL_TEXTURE_2D, XGL_TEXTURE_WRAP_T, XGL_CLAMP_TO_EDGE);
        s_gl.TexImage2D(XGL_TEXTURE_2D, 0, (int)XGL_RGBA8, w, h, 0, XGL_RGBA, XGL_UNSIGNED_BYTE, px);
        s_tex_w = w;
        s_tex_h = h;
    } else {
        s_gl.TexSubImage2D(XGL_TEXTURE_2D, 0, 0, 0, w, h, XGL_RGBA, XGL_UNSIGNED_BYTE, px);
    }
    s_gl.PixelStorei(XGL_UNPACK_ROW_LENGTH, 0);
    s_again_tex = s_tex;
    s_again_prev = ghost && s_tex_prev_w == w && s_tex_prev_h == h ? s_tex_prev : 0;
    s_again_w = w;
    s_again_h = h;
    return draw(s_again_tex, s_again_prev, w, h, 1);
}

#if defined(XBOXRECOMP_LSFG)
/* ── Frame generation (XBA-186) ─────────────────────────────────────────
 * present_rgba hands each picture to framegen_console (distinct frames to the pacer, repeats dropped) and draws
 * what its timed FIFO has due; host_window_sleep_until keeps drawing FIFO entries at their due times while the
 * runtime waits for its next frame. Fast-forward and a fallen-back pacer draw directly; a state load, the app
 * pausing or resuming and the in-game menu reset the history. */
static int s_fg;                 /* framegen_console running for this window */
static int s_fg_off_logged;      /* the pacer fell back: logged once */
static unsigned s_fg_loads;      /* recomp_states_loads() at the last frame */
static uint64_t s_fg_log_at;     /* the next statistics line */
static uint64_t s_fg_drawn, s_fg_drawn_gen;
static int s_rb_discard;         /* atomic: the read-back in flight shows a moment that is over (any thread sets it) */
static uint64_t s_rb_mapped, s_rb_direct; /* pictures given to the pacer: through a pack buffer, read synchronously */

static void fg_lifecycle(int paused, void *user)
{
    (void)paused;
    (void)user;
    __atomic_store_n(&s_rb_discard, 1, __ATOMIC_RELEASE);
    fgc_reset();
}

static void fg_log(const char *when)
{
    uint32_t real = 0, shown = 0, gen_us = 0;
    FgcCounters c;
    FramegenPacerCounters p;

    framegen_pacer_stats(&real, &shown, &gen_us);
    fgc_counters(&c);
    framegen_pacer_counters(&p);
    fprintf(stderr,
            "[LSFG] console%s: real %.2f / shown %.2f fps, generation %.2f ms; pictures %llu (distinct %llu, repeats "
            "%llu, direct %llu, downscaled %llu), generated %llu, window drew %llu (%llu generated), late %llu, FIFO "
            "full %llu, skipped pairs %llu, resets %llu, GPU read-backs %llu mapped / %llu synchronous%s\n",
            when, real / 100.0, shown / 100.0, gen_us / 1000.0, (unsigned long long)c.frames,
            (unsigned long long)c.distinct, (unsigned long long)c.repeats, (unsigned long long)c.direct,
            (unsigned long long)c.downscaled, (unsigned long long)p.generated, (unsigned long long)s_fg_drawn,
            (unsigned long long)s_fg_drawn_gen, (unsigned long long)c.late, (unsigned long long)c.fifo_full,
            (unsigned long long)p.skipped, (unsigned long long)c.resets, (unsigned long long)s_rb_mapped,
            (unsigned long long)s_rb_direct, c.passthrough ? ", pass-through" : "");
}

static void fg_open(void)
{
    static int listening;
    SDL_DisplayMode mode;
    int hz = 0, idx;

    s_fg = 0;
    if (!framegen_policy_enabled())
        return;
#if defined(XBOXRECOMP_LSFG_BRIDGE)
    lsfg_bridge_register();
#endif
    idx = SDL_GetWindowDisplayIndex(s_win);
    if (idx >= 0 && SDL_GetCurrentDisplayMode(idx, &mode) == 0)
        hz = mode.refresh_rate;
    if (fgc_start_env(hz) < 0)
        return;
    s_fg = 1;
    s_fg_off_logged = 0;
    s_fg_loads = recomp_states_loads();
    s_fg_log_at = xbox_HostNowNs() + 5000000000ull;
    s_fg_drawn = s_fg_drawn_gen = 0;
    s_rb_mapped = s_rb_direct = 0;
    if (!listening)
        listening = recomp_lifecycle_listen(fg_lifecycle, NULL) == 0;
}

static void fg_close(void)
{
    if (!s_fg)
        return;
    fg_log(" at close");
    fgc_stop();
    s_fg = 0;
}

/* Draws the newest FIFO entry due at now, if any. */
static void fg_service(uint64_t now)
{
    FgcShown e;

    if (now >= s_fg_log_at) {
        s_fg_log_at = now + 5000000000ull;
        fg_log("");
    }
    if (!fgc_take(now, &e))
        return;
    s_drawing_generated = e.generated;
    upload_draw(e.px, e.w, e.h, e.w * 4);
    s_drawing_generated = 0;
    s_fg_drawn++;
    s_fg_drawn_gen += (uint64_t)e.generated;
}

#endif /* XBOXRECOMP_LSFG */

/* A GPU picture read back (frame generation's pacer; screenshots through texture_grab): RGBA, row 0 at the top. */
static void (APIENTRY *s_gen_fbs)(int, unsigned *);
static void (APIENTRY *s_del_fbs)(int, const unsigned *);
static void (APIENTRY *s_fb_tex2d)(unsigned, unsigned, unsigned, unsigned, int);
static unsigned (APIENTRY *s_fb_status)(unsigned);
static void (APIENTRY *s_bind_buffer)(unsigned, unsigned);
static unsigned s_read_fb;
static uint8_t *s_read_px;
static size_t s_read_cap;
static int s_read_failed;
static SDL_GLContext s_read_ctx;   /* the context s_read_fb, s_grab_tex and the pack buffers belong to */
static unsigned s_grab_tex;        /* texture_grab's RGBA8 target */
static int s_grab_w, s_grab_h;
static unsigned char (APIENTRY *s_is_texture)(unsigned);

/* The pack state a read needs, and what the renderer sharing this context had set (put back by pack_leave). */
typedef struct {
    int buffer, alignment, row_length, skip_rows, skip_pixels;
} PackState;

static void pack_enter(PackState *p)
{
    s_gl.GetIntegerv(XGL_PACK_ALIGNMENT, &p->alignment);
    s_gl.GetIntegerv(XGL_PACK_ROW_LENGTH, &p->row_length);
    s_gl.GetIntegerv(XGL_PACK_SKIP_ROWS, &p->skip_rows);
    s_gl.GetIntegerv(XGL_PACK_SKIP_PIXELS, &p->skip_pixels);
    s_gl.GetIntegerv(XGL_PIXEL_PACK_BUFFER_BINDING, &p->buffer);
    s_gl.PixelStorei(XGL_PACK_ALIGNMENT, 4);
    s_gl.PixelStorei(XGL_PACK_ROW_LENGTH, 0);
    s_gl.PixelStorei(XGL_PACK_SKIP_ROWS, 0);
    s_gl.PixelStorei(XGL_PACK_SKIP_PIXELS, 0);
    if (p->buffer && s_bind_buffer)
        s_bind_buffer(XGL_PIXEL_PACK_BUFFER, 0);
}

static void pack_leave(const PackState *p)
{
    s_gl.PixelStorei(XGL_PACK_ALIGNMENT, p->alignment);
    s_gl.PixelStorei(XGL_PACK_ROW_LENGTH, p->row_length);
    s_gl.PixelStorei(XGL_PACK_SKIP_ROWS, p->skip_rows);
    s_gl.PixelStorei(XGL_PACK_SKIP_PIXELS, p->skip_pixels);
    if (s_bind_buffer)
        s_bind_buffer(XGL_PIXEL_PACK_BUFFER, (unsigned)p->buffer);
}

/* s_read_px holds at least need bytes; 0 when it cannot. */
static int read_room(size_t need)
{
    if (need > s_read_cap) {
        uint8_t *n = realloc(s_read_px, need);
        if (!n)
            return 0;
        s_read_px = n;
        s_read_cap = need;
    }
    return 1;
}

#if defined(XBOXRECOMP_LSFG)
static void rb_forget(void);
#endif

/* A context SDL replaced (Android, on resume) took our objects with it: their names mean nothing any more. */
static void read_context_check(void)
{
    SDL_GLContext now = SDL_GL_GetCurrentContext();

    if (!__atomic_exchange_n(&s_read_lost, 0, __ATOMIC_ACQ_REL) && now == s_read_ctx)
        return;
    s_read_ctx = now;
    s_read_fb = 0;
    s_read_failed = 0;
    s_grab_tex = 0;
    s_grab_w = s_grab_h = 0;
#if defined(XBOXRECOMP_LSFG)
    rb_forget();
#endif
    if (s_last_kind == LAST_GPU && s_gpu_last.ctx != now) {   /* the texture shown was the old context's */
        pthread_mutex_lock(&s_last_mu);
        s_last_kind = LAST_NONE;
        pthread_mutex_unlock(&s_last_mu);
    }
}

/* The read-back's framebuffer bound with tex attached; 0 when that is incomplete (read_detach in both cases). */
static int read_attach(unsigned tex)
{
    s_gl.BindFramebuffer(XGL_FRAMEBUFFER, s_read_fb);
    s_fb_tex2d(XGL_FRAMEBUFFER, XGL_COLOR_ATTACHMENT0, XGL_TEXTURE_2D, tex, 0);
    return s_fb_status(XGL_FRAMEBUFFER) == XGL_FRAMEBUFFER_COMPLETE;
}

static void read_detach(void)
{
    s_fb_tex2d(XGL_FRAMEBUFFER, XGL_COLOR_ATTACHMENT0, XGL_TEXTURE_2D, 0, 0);
    s_gl.BindFramebuffer(XGL_FRAMEBUFFER, 0);
}

/* The read-back's framebuffer, made the first time; 0 when there are no framebuffer objects. */
static int read_fb_make(void)
{
    if (s_read_fb)
        return 1;
    s_gen_fbs = (void (APIENTRY *)(int, unsigned *))SDL_GL_GetProcAddress("glGenFramebuffers");
    s_del_fbs = (void (APIENTRY *)(int, const unsigned *))SDL_GL_GetProcAddress("glDeleteFramebuffers");
    s_bind_buffer = (void (APIENTRY *)(unsigned, unsigned))SDL_GL_GetProcAddress("glBindBuffer");
    s_fb_tex2d = (void (APIENTRY *)(unsigned, unsigned, unsigned, unsigned, int))
        SDL_GL_GetProcAddress("glFramebufferTexture2D");
    s_fb_status = (unsigned (APIENTRY *)(unsigned))SDL_GL_GetProcAddress("glCheckFramebufferStatus");
    s_is_texture = (unsigned char (APIENTRY *)(unsigned))SDL_GL_GetProcAddress("glIsTexture");
    if (s_gen_fbs && s_fb_tex2d && s_fb_status)
        s_gen_fbs(1, &s_read_fb);
    return s_read_fb != 0;
}

/* The longest side of a grab: a 4x internal resolution is read at 1920 (drawn smaller, sampled). */
#define GRAB_MAX_SIDE 1920

/* Texture tex as the window samples it (its swizzle, any format the present program reads), drawn with the plain
 * present program into s_grab_tex, an RGBA8 target of gw x gh: the picture's top in its row 0, the first row a read
 * returns. 1 with s_read_fb bound and s_grab_tex attached, ready to read; 0 when it cannot be drawn. Either way the
 * caller ends with read_detach and the viewport put back (vp, what this found). Touches only what draw() sets anyway
 * (framebuffer, viewport, program, unit 0, vertex array, the tests it turns off). */
static int grab_draw(unsigned tex, int top_down, int gw, int gh, int vp[4])
{
    s_gl.GetIntegerv(XGL_VIEWPORT, vp);
    s_gl.ActiveTexture(XGL_TEXTURE0);
    if (!s_grab_tex || s_grab_w != gw || s_grab_h != gh) {
        int unpack = 0;
        if (!s_grab_tex)
            s_gl.GenTextures(1, &s_grab_tex);
        if (!s_grab_tex) {
            s_gl.BindFramebuffer(XGL_FRAMEBUFFER, s_read_fb);
            return 0;
        }
        s_gl.BindTexture(XGL_TEXTURE_2D, s_grab_tex);
        s_gl.TexParameteri(XGL_TEXTURE_2D, XGL_TEXTURE_MIN_FILTER, XGL_NEAREST);
        s_gl.TexParameteri(XGL_TEXTURE_2D, XGL_TEXTURE_MAG_FILTER, XGL_NEAREST);
        s_gl.TexParameteri(XGL_TEXTURE_2D, XGL_TEXTURE_WRAP_S, XGL_CLAMP_TO_EDGE);
        s_gl.TexParameteri(XGL_TEXTURE_2D, XGL_TEXTURE_WRAP_T, XGL_CLAMP_TO_EDGE);
        /* A renderer's unpack buffer would be read as the texture's (absent) pixels. */
        s_gl.GetIntegerv(XGL_PIXEL_UNPACK_BUFFER_BINDING, &unpack);
        if (unpack && s_bind_buffer)
            s_bind_buffer(XGL_PIXEL_UNPACK_BUFFER, 0);
        s_gl.TexImage2D(XGL_TEXTURE_2D, 0, (int)XGL_RGBA8, gw, gh, 0, XGL_RGBA, XGL_UNSIGNED_BYTE, NULL);
        if (unpack && s_bind_buffer)
            s_bind_buffer(XGL_PIXEL_UNPACK_BUFFER, (unsigned)unpack);
        s_grab_w = gw;
        s_grab_h = gh;
    }
    if (!read_attach(s_grab_tex))
        return 0;
    s_gl.ColorMask(1, 1, 1, 1);
    s_gl.Disable(XGL_CULL_FACE);
    s_gl.Disable(XGL_STENCIL_TEST);
    s_gl.Disable(XGL_SCISSOR_TEST);
    s_gl.Disable(XGL_BLEND);
    s_gl.Disable(XGL_DEPTH_TEST);
    s_gl.Viewport(0, 0, gw, gh);
    s_gl.UseProgram(s_prog);
    s_gl.BindTexture(XGL_TEXTURE_2D, tex);
    s_gl.Uniform1i(s_u_tex, 0);
    /* Read-back row 0 is the target's bottom: the picture's top goes there (draw() flips the other way). */
    s_gl.Uniform1f(s_u_flip, top_down ? 0.0f : 1.0f);
    s_gl.BindVertexArray(s_vao);
    s_gl.DrawArrays(XGL_TRIANGLES, 0, 3);
    s_gl.BindVertexArray(0);
    return 1;
}

/* grab_draw, then read into s_read_px: RGBA, packed, row 0 the picture's top. The pack state is put back; ends with
 * framebuffer 0 and the viewport it found. NULL when it cannot; nothing is latched (the next request tries again). */
static const uint8_t *texture_grab(unsigned tex, int top_down, int gw, int gh)
{
    int vp[4] = {0, 0, 0, 0};

    read_context_check();
    if (!tex || gw <= 0 || gh <= 0 || !s_prog || !s_vao || !read_fb_make())
        return NULL;
    if (!read_room((size_t)gw * (size_t)gh * 4))
        return NULL;
    int ok = grab_draw(tex, top_down, gw, gh, vp);
    if (ok) {
        PackState pack;
        pack_enter(&pack);
        s_gl.ReadPixels(0, 0, gw, gh, XGL_RGBA, XGL_UNSIGNED_BYTE, s_read_px);
        pack_leave(&pack);
    }
    read_detach();
    s_gl.Viewport(vp[0], vp[1], vp[2], vp[3]);
    return ok ? s_read_px : NULL;
}

#if defined(XBOXRECOMP_LSFG)
/* The pixels of a texture of the window's context as it shows them now (RGBA, packed, row 0 at the top), drawn into
 * texture_grab's target and read (its sampler swizzle applied, a float texture too, as the window shows it): frame
 * generation when no earlier read-back can serve. NULL when it cannot be read (no framebuffer objects, an incomplete
 * attachment: latched, frame generation then skips GPU pictures). The buffer stays valid until the next read. */
static const uint8_t *texture_pixels(unsigned tex, int w, int h, int top_down)
{
    read_context_check();
    if (s_read_failed || !tex || w <= 0 || h <= 0)
        return NULL;
    if (!s_read_fb) {
        if (!read_fb_make()) {
            s_read_failed = 1;
            fprintf(stderr, "  [HOST] window: no framebuffer objects: frame generation skips GPU pictures\n");
            return NULL;
        }
    }
    if (!read_room((size_t)w * (size_t)h * 4))
        return NULL;
    int vp[4] = {0, 0, 0, 0};
    int ok = grab_draw(tex, top_down, w, h, vp);
    if (ok) {
        PackState pack;
        pack_enter(&pack);
        s_gl.ReadPixels(0, 0, w, h, XGL_RGBA, XGL_UNSIGNED_BYTE, s_read_px);
        pack_leave(&pack);
    }
    read_detach();
    s_gl.Viewport(vp[0], vp[1], vp[2], vp[3]);
    if (!ok) {
        s_read_failed = 1;
        fprintf(stderr, "  [HOST] window: a GPU picture cannot be read back: frame generation skips them\n");
        return NULL;
    }
    return s_read_px;   /* drawn the right way up: row 0 is the top whatever top_down */
}
#endif

/* The picture present_texture showed last, grabbed into s_last (presenting thread). 0, with the reason on stderr,
 * when it cannot be had now: no window, another context current (the app in the background), the texture gone. */
static int grab_now(void)
{
    const char *why = NULL;
    int w = s_gpu_last.w, h = s_gpu_last.h, gw = w, gh = h;

    if (!s_win || !s_gpu_last.tex || w <= 0 || h <= 0)
        why = "nothing shown";
    else if (!s_gpu_last.ctx || SDL_GL_GetCurrentContext() != s_gpu_last.ctx)
        why = "not the context it was shown in";
    else if (!read_fb_make())
        why = "no framebuffer objects";
    else if (s_is_texture && !s_is_texture(s_gpu_last.tex))
        why = "its texture is gone";
    if (!why) {
        if (w > GRAB_MAX_SIDE || h > GRAB_MAX_SIDE) {   /* the shape kept */
            gw = w >= h ? GRAB_MAX_SIDE : (int)((int64_t)w * GRAB_MAX_SIDE / h);
            gh = w >= h ? (int)((int64_t)h * GRAB_MAX_SIDE / w) : GRAB_MAX_SIDE;
            gw = gw > 0 ? gw : 1;
            gh = gh > 0 ? gh : 1;
        }
        unsigned before = s_gl.GetError();   /* the renderer's, not the grab's */
        if (before)
            fprintf(stderr, "  [HOST] window: GL error 0x%04x before a screenshot\n", before);
        const uint8_t *px = texture_grab(s_gpu_last.tex, s_gpu_last.top_down, gw, gh);
        unsigned err = s_gl.GetError();
        if (!px || err) {
            why = "it cannot be drawn into a picture of our own";
        } else {
            pthread_mutex_lock(&s_last_mu);
            int stored = s_last_kind == LAST_GPU &&
                         last_store(px, gw, gh, gw * 4, s_gpu_last.anum, s_gpu_last.aden);
            pthread_mutex_unlock(&s_last_mu);
            if (stored)
                return 1;
            why = "no memory";
        }
    }
    fprintf(stderr, "  [HOST] window: screenshot: the picture shown cannot be read (%s)\n", why);
    return 0;
}

/* A screenshot asked from another thread, read here on the presenting thread; one atomic load when none is. */
static void grab_service(void)
{
    unsigned asked;

    if (!__atomic_load_n(&s_grab_req, __ATOMIC_ACQUIRE))
        return;
    pthread_mutex_lock(&s_last_mu);
    __atomic_store_n(&s_grab_req, 0, __ATOMIC_RELAXED);
    asked = s_grab_asked;
    int gpu = s_last_kind == LAST_GPU;
    pthread_mutex_unlock(&s_last_mu);
    int ok = gpu && grab_now();   /* not a GPU picture any more: the waiter takes the copy (it sees the kind) */
    pthread_mutex_lock(&s_last_mu);
    s_grab_served = asked;
    s_grab_ok = ok;
    pthread_cond_broadcast(&s_grab_cv);
    pthread_mutex_unlock(&s_last_mu);
}

/* The read-back's framebuffer and memory, while the window's context is current. */
static void read_release(void)
{
    int current = SDL_GL_GetCurrentContext() == s_read_ctx;
    if (s_read_fb && s_del_fbs && current)
        s_del_fbs(1, &s_read_fb);
    if (s_grab_tex && current && s_gl.DeleteTextures)
        s_gl.DeleteTextures(1, &s_grab_tex);
    s_grab_tex = 0;
    s_grab_w = s_grab_h = 0;
    s_read_fb = 0;
    s_read_failed = 0;
    s_read_ctx = NULL;
    free(s_read_px);
    s_read_px = NULL;
    s_read_cap = 0;
}

#if defined(XBOXRECOMP_LSFG)
/* ── Frame generation's read-back (GC-003) ──────────────────────────────
 * glReadPixels into client memory makes the driver finish the frame first, on every present. Here each present
 * starts reading its picture into one of two pack buffers and takes the picture the previous present started, which
 * the GPU has usually finished by then: the pacer sees every picture one present late.
 *
 * Only the picture of the present just before can be taken, of the same size and orientation, with nothing in
 * between that makes it old (a state load, the app paused or resumed, fast-forward, a picture from another path).
 * Otherwise this present primes: it reads its own picture synchronously, so the pacer never gets an empty buffer, and
 * starts the next read; the next present then gives the same picture again, which fgc_frame drops as a repeat.
 * Every read is of the picture drawn into texture_grab's target first (grab_draw: one more full-picture draw per
 * present), not of the texture itself: the pacer gets what the window shows (an X360 frame's sampler swizzle, an
 * RGBA16F frame), row 0 at the top. Screenshots never come here (texture_grab). */
static void (APIENTRY *s_gen_buffers)(int, unsigned *);
static void (APIENTRY *s_del_buffers)(int, const unsigned *);
static void (APIENTRY *s_buffer_data)(unsigned, intptr_t, const void *, unsigned);
static void (APIENTRY *s_buffer_param)(unsigned, unsigned, int *);
static void *(APIENTRY *s_map_range)(unsigned, intptr_t, intptr_t, unsigned);
static unsigned char (APIENTRY *s_unmap)(unsigned);
static void (APIENTRY *s_flush)(void);

static struct {
    int loaded;            /* 1: the entry points are there, -1: they are not (always synchronous) */
    unsigned pbo[2];
    size_t size[2];        /* bytes allocated in each */
    int slot;              /* the buffer of the read in flight */
    int pending;           /* a read is in flight in pbo[slot]: */
    int w, h, top_down;    /* its picture */
    unsigned loads;        /* recomp_states_loads() when it started */
} s_rb;

static int rb_load(void)
{
    if (s_rb.loaded)
        return s_rb.loaded > 0;
    s_gen_buffers = (void (APIENTRY *)(int, unsigned *))SDL_GL_GetProcAddress("glGenBuffers");
    s_del_buffers = (void (APIENTRY *)(int, const unsigned *))SDL_GL_GetProcAddress("glDeleteBuffers");
    s_buffer_data = (void (APIENTRY *)(unsigned, intptr_t, const void *, unsigned))
        SDL_GL_GetProcAddress("glBufferData");
    s_buffer_param = (void (APIENTRY *)(unsigned, unsigned, int *))SDL_GL_GetProcAddress("glGetBufferParameteriv");
    s_map_range = (void *(APIENTRY *)(unsigned, intptr_t, intptr_t, unsigned))
        SDL_GL_GetProcAddress("glMapBufferRange");
    s_unmap = (unsigned char (APIENTRY *)(unsigned))SDL_GL_GetProcAddress("glUnmapBuffer");
    s_flush = (void (APIENTRY *)(void))SDL_GL_GetProcAddress("glFlush");
    s_rb.loaded = s_gen_buffers && s_del_buffers && s_buffer_data && s_buffer_param && s_map_range && s_unmap &&
                  s_flush && s_bind_buffer ? 1 : -1;
    fprintf(stderr, "  [HOST] window: frame generation reads GPU pictures %s\n",
            s_rb.loaded > 0 ? "through pack buffers" : "synchronously (no pack buffers)");
    return s_rb.loaded > 0;
}

/* The context is gone and its buffers with it: only the names are dropped. */
static void rb_forget(void)
{
    s_rb.pbo[0] = s_rb.pbo[1] = 0;
    s_rb.size[0] = s_rb.size[1] = 0;
    s_rb.pending = 0;
}

/* The buffers go, in their living context. */
static void rb_release(void)
{
    if (s_rb.pbo[0] && s_del_buffers)
        s_del_buffers(2, s_rb.pbo);
    rb_forget();
}

/* A present that gives the pacer nothing: what is in flight is older than the next picture. Frame generation off for
 * good (the pacer fell back): the buffers go too. */
static void rb_idle(void)
{
    s_rb.pending = 0;
    if (!s_rb.pbo[0] || (s_fg && framegen_pacer_active()))
        return;
    read_context_check();   /* names of a context SDL replaced are not deleted in the new one */
    rb_release();
}

/* Starts reading tex into the buffer not in flight; s_rb then describes it. 0: nothing was started. */
static int rb_issue(unsigned tex, int w, int h, int top_down)
{
    int slot = !s_rb.slot;
    size_t need = (size_t)w * (size_t)h * 4;
    PackState pack;

    s_rb.pending = 0;
    if (!s_read_fb)
        return 0;
    if (!s_rb.pbo[0]) {
        s_gen_buffers(2, s_rb.pbo);
        s_rb.size[0] = s_rb.size[1] = 0;
        if (!s_rb.pbo[0] || !s_rb.pbo[1])
            return 0;
    }
    int vp[4] = {0, 0, 0, 0};
    if (!grab_draw(tex, top_down, w, h, vp)) {   /* as the window shows it: swizzle, float (texture_pixels) */
        read_detach();
        s_gl.Viewport(vp[0], vp[1], vp[2], vp[3]);
        return 0;
    }
    pack_enter(&pack);
    s_bind_buffer(XGL_PIXEL_PACK_BUFFER, s_rb.pbo[slot]);
    if (need > s_rb.size[slot]) {
        int got = 0;
        s_buffer_data(XGL_PIXEL_PACK_BUFFER, (intptr_t)need, NULL, XGL_STREAM_READ);
        s_buffer_param(XGL_PIXEL_PACK_BUFFER, XGL_BUFFER_SIZE, &got);
        s_rb.size[slot] = got > 0 ? (size_t)got : 0;
    }
    int ok = need <= s_rb.size[slot];
    if (ok)
        s_gl.ReadPixels(0, 0, w, h, XGL_RGBA, XGL_UNSIGNED_BYTE, NULL);
    pack_leave(&pack);
    read_detach();
    s_gl.Viewport(vp[0], vp[1], vp[2], vp[3]);
    if (!ok)
        return 0;
    s_flush();   /* a tiler starts on it now, not at the next swap */
    s_rb.slot = slot;
    s_rb.pending = 1;
    s_rb.w = w;
    s_rb.h = h;
    s_rb.top_down = top_down;
    s_rb.loads = recomp_states_loads();
    return 1;
}

/* The finished read in pbo[slot] into s_read_px, row 0 at the top (grab_draw drew it so); unmapped and unbound when
 * it returns, as the caller may draw next. NULL when it cannot be had. */
static const uint8_t *rb_take(int slot, int w, int h)
{
    size_t need = (size_t)w * 4 * (size_t)h;
    PackState pack;
    int ok = 0;

    if (need > s_rb.size[slot] || !read_room(need))
        return NULL;
    pack_enter(&pack);
    s_bind_buffer(XGL_PIXEL_PACK_BUFFER, s_rb.pbo[slot]);
    const uint8_t *p = s_map_range(XGL_PIXEL_PACK_BUFFER, 0, (intptr_t)need, XGL_MAP_READ_BIT);
    if (p) {
        memcpy(s_read_px, p, need);
        ok = s_unmap(XGL_PIXEL_PACK_BUFFER) != 0;   /* 0: the store was corrupted while mapped */
    }
    pack_leave(&pack);
    return ok ? s_read_px : NULL;
}

/* The picture to give the pacer at this present (see above), NULL when it cannot be read. */
static const uint8_t *rb_picture(unsigned tex, int w, int h, int top_down)
{
    read_context_check();
    int discard = __atomic_exchange_n(&s_rb_discard, 0, __ATOMIC_ACQ_REL);
    if (discard)
        rb_release();   /* the app paused or resumed: made again below, whatever the driver did meanwhile */
    int have = s_rb.pending && !discard && s_rb.w == w && s_rb.h == h && s_rb.top_down == top_down &&
               s_rb.loads == recomp_states_loads();
    int slot = s_rb.slot;
    const uint8_t *px = NULL;

    s_rb.pending = 0;
    if (!have) {   /* priming: this picture now, and the next present takes it from the buffer */
        px = texture_pixels(tex, w, h, top_down);
        s_rb_direct += px != NULL;
    }
    if ((have || px) && rb_load())
        rb_issue(tex, w, h, top_down);
    if (!have)
        return px;
    px = rb_take(slot, w, h);   /* have: the read in flight is of this very shape */
    if (px) {
        s_rb_mapped++;
        return px;
    }
    px = texture_pixels(tex, w, h, top_down);
    s_rb_direct += px != NULL;
    return px;
}

/* 1 when the picture was taken by frame generation (nothing to draw now), 0 to draw it as usual. */
static int fg_present(const void *px, int w, int h, int pitch)
{
    unsigned flags = 0, loads;
    uint64_t now = xbox_HostNowNs();

    if (!s_fg)
        return 0;
    if (!s_rb_feeding)
        s_rb.pending = 0;   /* a picture from another path: the read in flight is older than it */
    if (recomp_ff_active())
        flags |= FGC_BYPASS;
    loads = recomp_states_loads();
    if (loads != s_fg_loads) {
        s_fg_loads = loads;
        flags |= FGC_RESET;
    }
    int r = fgc_frame(px, w, h, pitch, now, flags);
    if (!framegen_pacer_active() && !s_fg_off_logged) {
        s_fg_off_logged = 1;
        fprintf(stderr, "  [HOST] window: frame generation off (%s); normal present\n", framegen_pacer_off_reason());
    }
    if (r == FGC_DIRECT)
        return 0;
    fg_service(now);
    return 1;
}
#endif /* XBOXRECOMP_LSFG */

int host_window_present_rgba(const void *px, int w, int h, int pitch)
{
    if (!s_win || !px || w <= 0 || h <= 0 || pitch < w * 4)
        return 0;
    keep_last(px, w, h, pitch);   /* the real picture, for host_window_screenshot_bmp */
    if (__atomic_load_n(&s_shot_req, __ATOMIC_RELAXED))
        shot_write();
    live_take();
#if defined(XBOXRECOMP_LSFG)
    if (fg_present(px, w, h, pitch))
        return 1;
#endif
    return upload_draw(px, w, h, pitch);
}

int host_window_present_rgba_shared(const void *px, int w, int h, int pitch, host_window_last_fn last, void *user)
{
    if (!s_win || !px || w <= 0 || h <= 0 || pitch < w * 4)
        return 0;
    if (last)
        keep_shared(last, user, w, h);
    else
        keep_last(px, w, h, pitch);
    if (__atomic_load_n(&s_shot_req, __ATOMIC_RELAXED))
        shot_write();
    live_take();
#if defined(XBOXRECOMP_LSFG)
    if (fg_present(px, w, h, pitch))
        return 1;
#endif
    return upload_draw(px, w, h, pitch);
}

/* The shared picture as a kept copy (the caller's pixels are about to go). */
void host_window_keep_shared(void)
{
    pthread_mutex_lock(&s_last_mu);
    host_window_last_fn fn = s_last_fn;
    void *user = s_last_user;
    int anum = s_last_anum, aden = s_last_aden;
    s_last_fn = NULL;
    pthread_mutex_unlock(&s_last_mu);
    uint8_t *px = NULL;
    int w = 0, h = 0;
    if (!fn || fn(user, &px, &w, &h) != 0 || !px) {
        free(px);
        return;
    }
    int keep_num = s_aspect_num, keep_den = s_aspect_den;
    s_aspect_num = anum; /* the shape it was shown at */
    s_aspect_den = aden;
    keep_last(px, w, h, w * 4);
    s_aspect_num = keep_num;
    s_aspect_den = keep_den;
    free(px);
}

int host_window_present_again(void)
{
    if (!s_win || !s_again_tex)
        return 0;
    if (__atomic_load_n(&s_shot_req, __ATOMIC_RELAXED))
        shot_write(); /* the picture kept at its upload */
    live_take();
#if defined(XBOXRECOMP_LSFG)
    if (s_fg && framegen_pacer_active() && !recomp_ff_active())
        return 1; /* the pacer draws its FIFO at its due times (host_window_sleep_until): nothing new to give it */
#endif
    return draw(s_again_tex, s_again_prev, s_again_w, s_again_h, 1);
}

void host_window_sleep_until(uint64_t deadline_ns)
{
    uint64_t now;

#if defined(XBOXRECOMP_LSFG)
    if (s_fg && s_win) {
        for (;;) {
            now = xbox_HostNowNs();
            fg_service(now);
            now = xbox_HostNowNs();
            if (now >= deadline_ns)
                return;
            uint64_t due = fgc_next_due(), until = deadline_ns;
            if (due && due < until)
                until = due > now + 1000000ull ? due - 1000000ull : now; /* within a millisecond counts as due */
            if (until > now)
                fgc_wait(until);
        }
    }
#endif
    while ((now = xbox_HostNowNs()) < deadline_ns) {
        uint64_t d = deadline_ns - now;
        struct timespec ts = {(time_t)(d / 1000000000ull), (long)(d % 1000000000ull)};
        nanosleep(&ts, NULL);
    }
}

int host_window_framegen_active(void)
{
#if defined(XBOXRECOMP_LSFG)
    return s_fg && framegen_pacer_active();
#else
    return 0;
#endif
}

void host_window_set_scaling(int scale, int filter)
{
    s_scale = scale == HOST_SCALE_INTEGER || scale == HOST_SCALE_STRETCH ? scale : HOST_SCALE_ASPECT;
    s_filter = filter == HOST_FILTER_NEAREST ? HOST_FILTER_NEAREST : HOST_FILTER_LINEAR;
}

void host_window_set_aspect(int num, int den)
{
    s_aspect_num = num > 0 && den > 0 ? num : 0;
    s_aspect_den = num > 0 && den > 0 ? den : 0;
}

int host_window_scale_parse(const char *v)
{
    if (!v)
        return -1;
    if (!strcasecmp(v, "aspect"))
        return HOST_SCALE_ASPECT;
    if (!strcasecmp(v, "integer"))
        return HOST_SCALE_INTEGER;
    if (!strcasecmp(v, "stretch"))
        return HOST_SCALE_STRETCH;
    return -1;
}

int host_window_filter_parse(const char *v)
{
    if (!v)
        return -1;
    if (!strcasecmp(v, "nearest"))
        return HOST_FILTER_NEAREST;
    if (!strcasecmp(v, "linear"))
        return HOST_FILTER_LINEAR;
    return -1;
}

int host_window_env_scale(const char *console_var, int def)
{
    int v = console_var ? host_window_scale_parse(getenv(console_var)) : -1;

    if (v < 0)
        v = host_window_scale_parse(getenv("RECOMP_SCALE"));
    return v < 0 ? def : v;
}

int host_window_env_filter(const char *console_var, int def)
{
    int v = console_var ? host_window_filter_parse(getenv(console_var)) : -1;

    if (v < 0)
        v = host_window_filter_parse(getenv("RECOMP_FILTER"));
    return v < 0 ? def : v;
}

int host_window_env_vsync(void)
{
    const char *v = getenv("RECOMP_PRESENT_VSYNC");

    return !(v && v[0] == '0');
}

void host_window_set_shader(const HostShader *s)
{
    s_shader = present_shader_clamp(s);
}

void host_window_set_downscale(int on)
{
    s_downscale = on < 0 ? -1 : on != 0;
}

int host_window_downscale_drawn(void)
{
    return s_area_drawn > 0;
}

int host_window_shader_active(void)
{
    int p = s_shader.preset;

    return p > HOST_SHADER_NONE && p < HOST_SHADER_COUNT && !s_sprog[p].failed ? p : HOST_SHADER_NONE;
}

int host_window_shader_parse(const char *v)
{
    return present_shader_parse(v);
}

const char *host_window_shader_name(int preset)
{
    return present_shader_name(preset);
}

void host_window_env_shader(const char *console_var, int def, HostShader *out)
{
    present_shader_env(console_var, def, out);
}

void host_window_shader_env(const char *console_var)
{
    HostShader s;

    host_window_env_shader(console_var, HOST_SHADER_NONE, &s);
    host_window_set_shader(&s);
    if (s.preset != HOST_SHADER_NONE)
        fprintf(stderr, "  [HOST] window: picture shader %s requested (strength %d, curvature %d, ghosting %d)\n",
                host_window_shader_name(s.preset), s.strength, s.curvature, s.ghosting);
}

int host_window_stats(uint32_t *fps_centi, uint64_t *frames)
{
    uint32_t fps = 0;
    uint64_t n = 0;
    int open = __atomic_load_n(&s_open, __ATOMIC_ACQUIRE);

    if (open) {
        uint64_t last = __atomic_load_n(&s_last_present_ns, __ATOMIC_RELAXED);
        n = __atomic_load_n(&s_frames_pub, __ATOMIC_RELAXED);
        if (last && xbox_HostNowNs() - last < 2000000000ull)
            fps = __atomic_load_n(&s_fps_centi, __ATOMIC_RELAXED);
    }
    if (fps_centi)
        *fps_centi = fps;
    if (frames)
        *frames = n;
    return open;
}

/* How long a screenshot from another thread waits for the presenting thread to read a GPU picture. */
#define GRAB_WAIT_MS 500

/* s_last_mu held, the picture shown a texture: has it read into s_last (released while the presenting thread works).
 * 1 when s_last holds it now, or when what is shown changed meanwhile (the caller looks at s_last_kind again); 0 when
 * it cannot be had (the texture gone, nothing presenting or pumping for GRAB_WAIT_MS, the window closed). */
static int grab_wait(void)
{
    if (pthread_equal(pthread_self(), s_present_thread)) {
        pthread_mutex_unlock(&s_last_mu);
        int ok = grab_now();
        pthread_mutex_lock(&s_last_mu);
        return ok || s_last_kind != LAST_GPU;
    }
    unsigned mine = ++s_grab_asked;
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);   /* pthread_cond_timedwait's clock everywhere (macOS has no other) */
    until.tv_sec += GRAB_WAIT_MS / 1000;
    until.tv_nsec += (long)(GRAB_WAIT_MS % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    __atomic_store_n(&s_grab_req, 1, __ATOMIC_RELEASE);
    while (s_last_kind == LAST_GPU && (int)(s_grab_served - mine) < 0) {
        if (pthread_cond_timedwait(&s_grab_cv, &s_last_mu, &until) != 0 && (int)(s_grab_served - mine) < 0) {
            if (s_grab_asked == mine)   /* nobody else waits: the request is withdrawn */
                __atomic_store_n(&s_grab_req, 0, __ATOMIC_RELAXED);
            fprintf(stderr, "  [HOST] window: screenshot: nothing presented or pumped in %d ms\n", GRAB_WAIT_MS);
            return 0;
        }
    }
    return s_last_kind != LAST_GPU || s_grab_ok;
}

int host_window_screenshot_bmp(const char *path)
{
    uint8_t *px = NULL, hdr[54], *line;
    int w, h, anum, aden;

    if (!path || !path[0])
        return -1;
    pthread_mutex_lock(&s_last_mu);
    if ((s_last_kind == LAST_GPU && !grab_wait()) || s_last_kind == LAST_NONE) {
        pthread_mutex_unlock(&s_last_mu);
        return -1;
    }
    host_window_last_fn fn = s_last_fn;
    void *user = s_last_user;
    w = s_last_w;
    h = s_last_h;
    anum = s_last_anum;
    aden = s_last_aden;
    if (!fn && s_last && w > 0 && h > 0 && (px = malloc((size_t)w * h * 4)))
        memcpy(px, s_last, (size_t)w * h * 4);
    pthread_mutex_unlock(&s_last_mu);
    if (fn && fn(user, &px, &w, &h) != 0) { /* the caller's picture (host_window_present_rgba_shared) */
        free(px);
        px = NULL;
    }
    if (!px || w <= 0 || h <= 0) {
        free(px);
        return -1;
    }

    /* The shape it was shown at: the height kept and the width following, or the width kept when that would
     * make it narrower than it is. */
    int ow = w, oh = h;
    if (anum > 0 && aden > 0) {
        ow = (int)(((int64_t)h * anum + aden / 2) / aden);
        if (ow < w) {
            ow = w;
            oh = (int)(((int64_t)w * aden + anum / 2) / anum);
        }
    }
    uint32_t row = ((uint32_t)ow * 3u + 3u) & ~3u, img = row * (uint32_t)oh, total = 54u + img;
    uint32_t uw = (uint32_t)ow, uh = (uint32_t)oh;
    FILE *f = fopen(path, "wb");
    line = calloc(1, row);
    if (!f || !line) {
        if (f)
            fclose(f);
        free(line);
        free(px);
        return -1;
    }
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B';
    hdr[1] = 'M';
    memcpy(hdr + 2, &total, 4);
    hdr[10] = 54;
    hdr[14] = 40;
    memcpy(hdr + 18, &uw, 4);
    memcpy(hdr + 22, &uh, 4);
    hdr[26] = 1;
    hdr[28] = 24;
    memcpy(hdr + 34, &img, 4);
    int ok = fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr;
    for (int y = oh - 1; ok && y >= 0; y--) {   /* bottom-up */
        const uint8_t *src = px + (size_t)((int64_t)y * h / oh) * w * 4;
        for (int x = 0; x < ow; x++) {
            const uint8_t *p = src + (size_t)((int64_t)x * w / ow) * 4;
            line[x * 3 + 0] = p[2];
            line[x * 3 + 1] = p[1];
            line[x * 3 + 2] = p[0];
        }
        ok = fwrite(line, 1, row, f) == row;
    }
    free(line);
    free(px);
    if (fclose(f) != 0 || !ok)
        return -1;
    fprintf(stderr, "  [HOST] window: screenshot %dx%d (from %dx%d) to %s\n", ow, oh, w, h, path);
    return 0;
}

void host_window_capture(const char *ppm_path)
{
    snprintf(s_capture, sizeof s_capture, "%s", ppm_path ? ppm_path : "");
}

void *host_window_gl_proc(const char *name)
{
    return s_ctx ? SDL_GL_GetProcAddress(name) : NULL;
}

void host_window_info(HostWindowInfo *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof *out);
    if (s_win)
        SDL_GL_GetDrawableSize(s_win, &out->drawable_w, &out->drawable_h);
    out->gles = s_gles;
    out->gl_major = s_major;
    out->gl_minor = s_minor;
    out->vsync = s_vsync;
    out->frames = s_frames;
}

void *host_window_sdl(void)
{
    return s_win;
}

void host_window_close(void)
{
    if (!s_win)
        return;
#if defined(XBOXRECOMP_LSFG)
    fg_close(); /* the pacer's thread stops (and the bridge closes) before the GL context goes */
#endif
    __atomic_store_n(&s_open, 0, __ATOMIC_RELEASE);
    pthread_mutex_lock(&s_last_mu);
    free(s_last);
    s_last = NULL;
    s_last_cap = 0;
    s_last_w = s_last_h = 0;
    s_last_fn = NULL;
    s_last_kind = LAST_NONE;
    s_grab_served = s_grab_asked;   /* a screenshot waiting gets -1 now */
    s_grab_ok = 0;
    __atomic_store_n(&s_grab_req, 0, __ATOMIC_RELAXED);
    pthread_cond_broadcast(&s_grab_cv);
    pthread_mutex_unlock(&s_last_mu);
    memset(&s_gpu_last, 0, sizeof s_gpu_last);
    SDL_DelEventWatch(lifecycle_watch, NULL);
    host_pad_shutdown();
    if (s_ctx) {
        if (s_gl.DeleteTextures && s_tex)
            s_gl.DeleteTextures(1, &s_tex);
        if (s_gl.DeleteTextures && s_tex_prev)
            s_gl.DeleteTextures(1, &s_tex_prev);
        shader_free();
        read_context_check();   /* names of a context SDL replaced are not deleted in this one */
#if defined(XBOXRECOMP_LSFG)
        rb_release();
        memset(&s_rb, 0, sizeof s_rb);   /* the entry points are looked up again with the next context */
#endif
        read_release();
        if (s_gl.DeleteVertexArrays && s_vao)
            s_gl.DeleteVertexArrays(1, &s_vao);
        if (s_gl.DeleteProgram && s_prog)
            s_gl.DeleteProgram(s_prog);
        if (s_gl.DeleteProgram && s_ov_prog)
            s_gl.DeleteProgram(s_ov_prog);
        if (s_gl.DeleteTextures && s_ov_tex)
            s_gl.DeleteTextures(1, &s_ov_tex);
        if (s_gl.DeleteTextures && s_bd_tex)
            s_gl.DeleteTextures(1, &s_bd_tex);
        SDL_GL_DeleteContext(s_ctx);
    }
    SDL_DestroyWindow(s_win);
    s_win = NULL;
    s_ctx = NULL;
    s_tex = s_vao = s_prog = 0;
    s_ov_prog = s_ov_tex = s_bd_tex = 0;
    s_ov_failed = 0;
    s_tex_w = s_tex_h = 0;
    s_tex_filter = -1;
    s_tex_prev = 0;
    s_tex_prev_w = s_tex_prev_h = 0;
    s_again_tex = s_again_prev = 0;
    s_tex_prev_filter = -1;
    memset(s_sprog, 0, sizeof s_sprog);   /* a context that could not be made current: built again next time */
    memset(&s_gl, 0, sizeof s_gl);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER);
}
