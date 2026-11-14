/* host_window -- the host layer's window: one SDL window with a GL context
 * and a texture presented into it (XBA-145, src/platform/HOST_LAYER.md).
 *
 * Console-agnostic: a console runtime renders into a GL texture of this
 * context (or hands RGBA pixels) and presents it, letterboxed to its aspect.
 * The context is OpenGL ES 3.0 where the host has it (Android, Mesa) and a
 * 3.2 core context otherwise (macOS), current on the thread that opened the
 * window (RECOMP_HOST_GL=core skips GLES, to check the macOS path on Linux);
 * GL entry points come from SDL (host_window_gl_proc), so nothing
 * links libGL or libGLESv3 directly.
 *
 * The Xbox presenter (fb_present_sdl.c, an SDL_Renderer fed from guest
 * memory) is separate and unchanged; this is the window every other console
 * runtime uses.
 *
 * Not thread-safe: open, pump, present and close on the same thread (SDL's
 * main thread on Android and macOS). POSIX hosts. */
#ifndef XBR_HOST_WINDOW_H
#define XBR_HOST_WINDOW_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

union SDL_Event;

typedef struct HostWindowInfo {
    int drawable_w, drawable_h;  /* the back buffer, in pixels */
    int gles;                    /* 1: OpenGL ES context, 0: desktop core */
    int gl_major, gl_minor;
    int vsync;                   /* 1 when the swap waits for vsync */
    uint64_t frames;             /* presents so far */
} HostWindowInfo;

/* 1 when no window should be opened: XBR_BUILD_ONLY set (a build check),
 * SDL_VIDEODRIVER dummy or offscreen, or a Linux desktop host with neither
 * DISPLAY nor WAYLAND_DISPLAY. Tests exit 77 (skipped) on it. */
int host_window_headless(void);

/* Opens the window (width x height, resizable; fullscreen on Android) and
 * its GL context. vsync: 1 waits for vsync at the swap (adaptive when the
 * driver has it). Returns 1, or 0 with the reason on stderr. A second open
 * while one is up returns 1. */
int host_window_open(const char *title, int width, int height, int vsync);

/* Handles the pending events: quit, resize, app background / foreground
 * (recomp_lifecycle_pause / resume) and pads (host_pad_handle_event), then
 * the hook, if any. Returns 0 once quit was asked (window closed, SDL_QUIT),
 * 1 otherwise. */
int host_window_pump(void);

/* Called by host_window_pump for every event, after the host layer's own
 * handling. NULL removes it. */
typedef void (*host_window_event_fn)(const union SDL_Event *ev, void *user);
void host_window_set_event_hook(host_window_event_fn fn, void *user);

/* Uploads RGBA8 pixels (row 0 at the top, pitch in bytes) into the
 * window's own texture and presents it. Returns 1, or 0 on a GL error. */
int host_window_present_rgba(const void *px, int w, int h, int pitch);

/* Presents texture `tex` of this context (w x h, its aspect kept). top_down:
 * 1 when its row 0 is the top of the picture (uploaded pixels), 0 when it is
 * the bottom (a texture rendered by GL). Binds framebuffer 0, draws, swaps.
 * Returns 1, or 0 on a GL error. */
int host_window_present_texture(unsigned tex, int w, int h, int top_down);

/* How host_window_present_rgba / _texture fit the picture in the window.
 * HOST_SCALE_ASPECT (the default): the largest rectangle of its aspect;
 * HOST_SCALE_INTEGER: the largest whole multiple of its size that fits (a
 * pixel-art console: every source pixel the same size), aspect when not even
 * 1x fits. filter: how the window's own texture (present_rgba) is sampled,
 * HOST_FILTER_LINEAR (the default) or HOST_FILTER_NEAREST (sharp pixels).
 * May be called before open; kept across open / close. */
enum { HOST_SCALE_ASPECT = 0, HOST_SCALE_INTEGER = 1 };
enum { HOST_FILTER_LINEAR = 0, HOST_FILTER_NEAREST = 1 };
void host_window_set_scaling(int scale, int filter);

/* The shape the picture is shown at, num:den (4:3 for a PlayStation, whose
 * frame buffer is 256..640 pixels wide whatever the screen), instead of its
 * own w:h. HOST_SCALE_INTEGER then takes whole multiples of its height, the
 * width following the shape. 0 / 0: the picture's own (the default). Kept
 * across open / close. */
void host_window_set_aspect(int num, int den);

/* Writes the next presented picture, the whole drawable as drawn (letterbox
 * included), to ppm_path as a binary PPM, read back before the swap: a
 * screenshot through the real present path (headless checks under Xvfb).
 * One capture per call; NULL or "" cancels. */
void host_window_capture(const char *ppm_path);

/* A GL entry point of the window's context (SDL_GL_GetProcAddress), for a
 * console's renderer. NULL before open. */
void *host_window_gl_proc(const char *name);

void host_window_info(HostWindowInfo *out);

/* The SDL_Window, for what the host layer does not cover (NULL before open). */
void *host_window_sdl(void);

void host_window_close(void);

#ifdef __cplusplus
}
#endif

#endif /* XBR_HOST_WINDOW_H */
