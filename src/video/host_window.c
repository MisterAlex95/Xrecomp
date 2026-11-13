/* host_window.c -- see host_window.h (XBA-145). */
#include "host_window.h"
#include "host_pad.h"
#include "recomp_lifecycle.h"

#include <SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The few GL entry points the present needs, from SDL: no GL header or GL
 * library at build time, the same code on GLES 3.0 and desktop 3.2 core. */
#ifndef APIENTRY
#define APIENTRY
#endif

#define XGL_TEXTURE_2D          0x0DE1
#define XGL_UNSIGNED_BYTE       0x1401
#define XGL_RGBA                0x1908
#define XGL_RGBA8               0x8058
#define XGL_LINEAR              0x2601
#define XGL_TEXTURE_MAG_FILTER  0x2800
#define XGL_TEXTURE_MIN_FILTER  0x2801
#define XGL_TEXTURE_WRAP_S      0x2802
#define XGL_TEXTURE_WRAP_T      0x2803
#define XGL_CLAMP_TO_EDGE       0x812F
#define XGL_COLOR_BUFFER_BIT    0x4000
#define XGL_TRIANGLES           0x0004
#define XGL_TEXTURE0            0x84C0
#define XGL_FRAGMENT_SHADER     0x8B30
#define XGL_VERTEX_SHADER       0x8B31
#define XGL_COMPILE_STATUS      0x8B81
#define XGL_LINK_STATUS         0x8B82
#define XGL_UNPACK_ROW_LENGTH   0x0CF2
#define XGL_UNPACK_ALIGNMENT    0x0CF5
#define XGL_FRAMEBUFFER         0x8D40
#define XGL_SCISSOR_TEST        0x0C11
#define XGL_BLEND               0x0BE2
#define XGL_DEPTH_TEST          0x0B71

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
    void (APIENTRY *Disable)(unsigned);
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
    void (APIENTRY *GenVertexArrays)(int, unsigned *);
    void (APIENTRY *DeleteVertexArrays)(int, const unsigned *);
    void (APIENTRY *BindVertexArray)(unsigned);
    void (APIENTRY *BindFramebuffer)(unsigned, unsigned);
    void (APIENTRY *ActiveTexture)(unsigned);
    void (APIENTRY *DrawArrays)(unsigned, int, int);
    unsigned (APIENTRY *GetError)(void);
} HostGl;

static SDL_Window *s_win;
static SDL_GLContext s_ctx;
static HostGl s_gl;
static unsigned s_prog, s_vao, s_tex;
static int s_tex_w, s_tex_h;
static int s_u_tex, s_u_flip;
static int s_gles, s_major, s_minor, s_vsync;
static int s_quit;
static uint64_t s_frames;
static host_window_event_fn s_hook;
static void *s_hook_user;

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
        F(PixelStorei), F(Viewport), F(ClearColor), F(Clear), F(Disable), F(CreateShader), F(ShaderSource),
        F(CompileShader), F(GetShaderiv), F(GetShaderInfoLog), F(DeleteShader), F(CreateProgram),
        F(AttachShader), F(LinkProgram), F(GetProgramiv), F(GetProgramInfoLog), F(DeleteProgram),
        F(UseProgram), F(GetUniformLocation), F(Uniform1i), F(Uniform1f), F(GenVertexArrays),
        F(DeleteVertexArrays), F(BindVertexArray), F(BindFramebuffer), F(ActiveTexture), F(DrawArrays),
        F(GetError),
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

static unsigned compile(unsigned type, const char *head, const char *body)
{
    const char *src[2] = { head, body };
    unsigned sh = s_gl.CreateShader(type);
    int ok = 0;

    s_gl.ShaderSource(sh, 2, src, NULL);
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
    fprintf(stderr, "  [HOST] window: %dx%d, GL%s %d.%d, vsync %s\n", width, height, s_gles ? " ES" : " core",
            s_major, s_minor, s_vsync ? "on" : "off");
    return 1;
}

void host_window_set_event_hook(host_window_event_fn fn, void *user)
{
    s_hook = fn;
    s_hook_user = user;
}

int host_window_pump(void)
{
    SDL_Event ev;

    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT:
            s_quit = 1;
            break;
        case SDL_WINDOWEVENT:
            if (ev.window.event == SDL_WINDOWEVENT_CLOSE)
                s_quit = 1;
            break;
        default:
            break;
        }
        host_pad_handle_event(&ev);
        if (s_hook)
            s_hook(&ev, s_hook_user);
    }
    return !s_quit;
}

static int draw(unsigned tex, int w, int h, int top_down)
{
    int dw = 0, dh = 0, vw, vh;

    if (!s_win || !tex || w <= 0 || h <= 0)
        return 0;
    SDL_GL_GetDrawableSize(s_win, &dw, &dh);
    if (dw <= 0 || dh <= 0)
        return 0;
    /* Letterbox: the largest rectangle of the picture's aspect. */
    if ((int64_t)dw * h > (int64_t)dh * w) {
        vh = dh;
        vw = (int)((int64_t)dh * w / h);
    } else {
        vw = dw;
        vh = (int)((int64_t)dw * h / w);
    }
    s_gl.BindFramebuffer(XGL_FRAMEBUFFER, 0);
    s_gl.Disable(XGL_SCISSOR_TEST);
    s_gl.Disable(XGL_BLEND);
    s_gl.Disable(XGL_DEPTH_TEST);
    s_gl.Viewport(0, 0, dw, dh);
    s_gl.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    s_gl.Clear(XGL_COLOR_BUFFER_BIT);
    s_gl.Viewport((dw - vw) / 2, (dh - vh) / 2, vw, vh);
    s_gl.UseProgram(s_prog);
    s_gl.ActiveTexture(XGL_TEXTURE0);
    s_gl.BindTexture(XGL_TEXTURE_2D, tex);
    s_gl.Uniform1i(s_u_tex, 0);
    s_gl.Uniform1f(s_u_flip, top_down ? 1.0f : 0.0f);
    s_gl.BindVertexArray(s_vao);
    s_gl.DrawArrays(XGL_TRIANGLES, 0, 3);
    s_gl.BindVertexArray(0);
    unsigned err = s_gl.GetError();
    SDL_GL_SwapWindow(s_win);
    s_frames++;
    if (err) {
        fprintf(stderr, "  [HOST] window: GL error 0x%04x at present\n", err);
        return 0;
    }
    return 1;
}

int host_window_present_texture(unsigned tex, int w, int h, int top_down)
{
    return draw(tex, w, h, top_down);
}

int host_window_present_rgba(const void *px, int w, int h, int pitch)
{
    if (!s_win || !px || w <= 0 || h <= 0 || pitch < w * 4)
        return 0;
    if (!s_tex) {
        s_gl.GenTextures(1, &s_tex);
        s_tex_w = s_tex_h = 0;
    }
    s_gl.BindTexture(XGL_TEXTURE_2D, s_tex);
    s_gl.PixelStorei(XGL_UNPACK_ALIGNMENT, 4);
    s_gl.PixelStorei(XGL_UNPACK_ROW_LENGTH, pitch / 4);
    if (w != s_tex_w || h != s_tex_h) {
        s_gl.TexParameteri(XGL_TEXTURE_2D, XGL_TEXTURE_MIN_FILTER, XGL_LINEAR);
        s_gl.TexParameteri(XGL_TEXTURE_2D, XGL_TEXTURE_MAG_FILTER, XGL_LINEAR);
        s_gl.TexParameteri(XGL_TEXTURE_2D, XGL_TEXTURE_WRAP_S, XGL_CLAMP_TO_EDGE);
        s_gl.TexParameteri(XGL_TEXTURE_2D, XGL_TEXTURE_WRAP_T, XGL_CLAMP_TO_EDGE);
        s_gl.TexImage2D(XGL_TEXTURE_2D, 0, (int)XGL_RGBA8, w, h, 0, XGL_RGBA, XGL_UNSIGNED_BYTE, px);
        s_tex_w = w;
        s_tex_h = h;
    } else {
        s_gl.TexSubImage2D(XGL_TEXTURE_2D, 0, 0, 0, w, h, XGL_RGBA, XGL_UNSIGNED_BYTE, px);
    }
    s_gl.PixelStorei(XGL_UNPACK_ROW_LENGTH, 0);
    return draw(s_tex, w, h, 1);
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
    SDL_DelEventWatch(lifecycle_watch, NULL);
    host_pad_shutdown();
    if (s_ctx) {
        if (s_gl.DeleteTextures && s_tex)
            s_gl.DeleteTextures(1, &s_tex);
        if (s_gl.DeleteVertexArrays && s_vao)
            s_gl.DeleteVertexArrays(1, &s_vao);
        if (s_gl.DeleteProgram && s_prog)
            s_gl.DeleteProgram(s_prog);
        SDL_GL_DeleteContext(s_ctx);
    }
    SDL_DestroyWindow(s_win);
    s_win = NULL;
    s_ctx = NULL;
    s_tex = s_vao = s_prog = 0;
    s_tex_w = s_tex_h = 0;
    memset(&s_gl, 0, sizeof s_gl);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER);
}
