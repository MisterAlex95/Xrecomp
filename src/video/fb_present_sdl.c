/**
 * Show the guest framebuffer in a window: the POSIX half of fb_present.c.
 *
 * fb_present.c is Win32 GDI and runs its window on a thread of its own. That
 * cannot work on macOS, where AppKit -- and so SDL's video and event pump --
 * only runs on the process main thread, and the guest would otherwise own
 * that thread for the whole run. So the shape is inverted here:
 *
 *   process main thread   SDL window, the one SDL event loop, the presents
 *   "guest-main" pthread  the boot sequence and the recompiled entry point
 *
 * A title opts in by handing its boot sequence to xbox_HostWindowMain()
 * from main(). With RECOMP_FB_WINDOW set (the same switch as the Win32
 * window) it opens the window, starts the guest on a pthread with at least
 * the main thread's stack and, on macOS, its QoS, and pumps events and
 * presents until the guest returns or the window is closed. Without it, or
 * with RECOMP_HEADLESS=1, no display, or SDL video failing, the guest runs
 * on the calling thread exactly as before.
 *
 * Frames come from the executor's flip (xbox_FramebufferWindowPresent, on
 * the executor's thread). The flip copies the finished surface into a free
 * slot of three and publishes it with an index swap under a mutex held for a
 * few instructions, so the executor never waits for the window and the
 * window never reads guest memory the rasteriser may be writing. A guest
 * surface is in the GDI window's format: 640x480, bpp from pitch / 640,
 * 2 = R5G6B5 (X1R5G5B5 when the executor says so,
 * xbox_FramebufferWindowSetFormat), 4 = X8R8G8B8. A GPU back end may instead
 * hand over its own pixels at their own size (xbox_FramebufferWindowPresentHost:
 * an internal resolution above 640x480, RECOMP_GPU_SCALE); each slot carries
 * its frame's width, height, pitch and pixel format, the texture follows
 * them, and the renderer's logical size stays 640x480, so letterboxing and
 * the perf overlay do not change. Before the first flip the window reads
 * the AvSetDisplayMode surface directly, as the GDI window does, for a title
 * that never flips.
 *
 * Closing the window ends the process: the window owns the main thread, and
 * lifted code cannot be stopped mid-flight. atexit handlers still run, except
 * on Android, where recomp_quit() flushes the logs and _exit()s instead
 * (recomp_quit.h). Also on Android, a guest that returns leaves the window
 * up on its last frame until the player quits (the in-game menu, or Back
 * twice where nothing above SDL takes Back): an app does not vanish on its
 * own, and the lifecycle keeps working (XBA-52).
 *
 * Knobs: RECOMP_PRESENT_VSYNC=0 presents without waiting for the display;
 * RECOMP_WINDOW_SCALE=<n> sizes the window (default 2);
 * RECOMP_FULLSCREEN=1 starts in borderless desktop fullscreen.
 */
#if !defined(_WIN32)

#include <SDL.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#include "platform/recomp_lifecycle.h"
#include "platform/recomp_quit.h"
#include "video_player.h"
#include "perf_overlay.h"
#include "keyboard_sdl.h"     /* src/input: keys into the pad stand-in */
#include "xinput_xbox.h"
#if defined(XBOXRECOMP_HAVE_GPU)
#include "nv2a_gl.h"          /* src/gpu: RECOMP_GPU=1 */
#endif

extern ptrdiff_t xbox_GetMemoryOffset(void);

#define FB_W 640u
#define FB_H 480u

/* Back twice within this many ms quits (Android, XBA-24). */
#define BACK_QUIT_MS 2000u
/* User-facing text, named by its key (spec XBA-24). */
#define TEXT_QUIT_PROMPT "Press Back again to quit"
/* The title ended on its own (a test title like the stub; a game does not):
 * the screen stays as it was, so say it there and not only in logcat. */
#define TEXT_GUEST_DONE "The title finished (code %d). Press Back for the menu (Quit)."

/* ── Handoff: three slots, one each for writer, reader and the ready frame ── */

/* A frame: w x h pixels, rows pitch bytes apart (top row first), in SDL
 * pixel format `format` (RGB565, RGB555, XRGB8888 or XBGR8888). General on
 * purpose: whatever produces frames for the window -- the flip, a GPU back
 * end's own read-back, a frame pacer -- fills one the same way. */
struct fb_slot {
    uint8_t *px;
    size_t   cap;
    uint32_t w, h, pitch;
    uint32_t format;            /* SDL_PIXELFORMAT_* */
};

/* What the window takes from SDL, and gives back on every way out. */
#define FBWIN_SDL_SUBSYSTEMS \
    (SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER)

static struct fb_slot  s_slot[3];
static int             s_back = 0, s_ready = 1, s_front = 2;
static uint64_t        s_ready_seq;          /* frames published */
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int    s_running;            /* window up: flips copy */
static Uint32          s_frame_event;        /* wakes the main loop */
static volatile int    s_wake_pending;
static volatile int    s_in_present;         /* executor inside Present */
static volatile uint32_t s_fb_va, s_fb_pitch; /* AvSetDisplayMode surface */
static volatile uint32_t s_fb_x555;           /* xbox_FramebufferWindowSetFormat */

/* Counted at the flip, read once a second for the title bar. */
static uint64_t s_flips;
static uint32_t s_frame_draws;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{
    s_fb_va = fb_va;
    if (pitch)
        s_fb_pitch = pitch;
}

/* ponytail: the SDL window shows the surface unscaled; 2x2 anti-aliased
 * surfaces (#162) come through at twice the height until it honours this. */
void xbox_FramebufferWindowSetAA(uint32_t sy) { (void)sy; }

/* SET_SURFACE_FORMAT colour format of what the next presents hold in guest
 * memory: 1 and 2 (X1R5G5B5) are shown as 555, anything else 16-bit as
 * R5G6B5. */
void xbox_FramebufferWindowSetFormat(uint32_t color_format)
{
    s_fb_x555 = color_format == 1 || color_format == 2;
}

/* The window is opened by xbox_HostWindowMain, on the main thread. */
void xbox_FramebufferWindowStart(void) {}

/* SDL pixel format of a guest surface of bpp bytes per pixel. */
static uint32_t guest_format(uint32_t bpp, uint32_t x555)
{
    return bpp == 4 ? SDL_PIXELFORMAT_RGB888        /* = XRGB8888 */
         : x555     ? SDL_PIXELFORMAT_RGB555
                    : SDL_PIXELFORMAT_RGB565;
}

/* The back slot with room for h rows of pitch bytes, or NULL. Only the
 * presenting thread touches s_back. */
static struct fb_slot *back_slot(uint32_t pitch, uint32_t h)
{
    struct fb_slot *s = &s_slot[s_back];
    size_t need = (size_t)pitch * h;

    if (s->cap < need) {
        uint8_t *n = (uint8_t *)realloc(s->px, need);
        if (!n)
            return NULL;
        s->px = n;
        s->cap = need;
    }
    return s;
}

/* The back slot is filled: make it the ready one. An unshown ready frame is
 * overwritten (dropped), never waited for. */
static void publish(uint32_t w, uint32_t h, uint32_t pitch, uint32_t format)
{
    s_slot[s_back].w = w;              /* only this thread touches s_back */
    s_slot[s_back].h = h;
    s_slot[s_back].pitch = pitch;
    s_slot[s_back].format = format;
    pthread_mutex_lock(&s_lock);
    {
        int t = s_ready;
        s_ready = s_back;
        s_back = t;
        s_ready_seq++;
    }
    pthread_mutex_unlock(&s_lock);

    /* Wake the main loop, once per batch of frames it has not looked at. */
    if (!__atomic_exchange_n(&s_wake_pending, 1, __ATOMIC_ACQ_REL)) {
        SDL_Event ev;
        memset(&ev, 0, sizeof ev);
        ev.type = s_frame_event;
        /* Not queued (full, or filtered): clear the latch, or no frame
         * would wake the loop again before its timeout. */
        if (SDL_PushEvent(&ev) <= 0)
            __atomic_store_n(&s_wake_pending, 0, __ATOMIC_RELEASE);
    }
}

/* Copy w x h pixels (rows src_pitch bytes apart, row_bytes of them used)
 * into the back slot and publish it. */
static void present_pixels(const uint8_t *src, uint32_t w, uint32_t h,
                           uint32_t src_pitch, uint32_t row_bytes, uint32_t format)
{
    struct fb_slot *s = back_slot(row_bytes, h);
    uint32_t y;

    if (!s)
        return;
    if (src_pitch == row_bytes)
        memcpy(s->px, src, (size_t)row_bytes * h);
    else
        for (y = 0; y < h; y++)
            memcpy(s->px + (size_t)y * row_bytes, src + (size_t)y * src_pitch, row_bytes);
    publish(w, h, row_bytes, format);
}

static void present_copy(uint32_t fb_va, uint32_t pitch)
{
    uint32_t bpp = pitch / FB_W;

    if (bpp != 2 && bpp != 4)
        return;
    present_pixels((const uint8_t *)((uintptr_t)fb_va + xbox_GetMemoryOffset()),
                   FB_W, FB_H, pitch, FB_W * bpp, guest_format(bpp, bpp == 2 && s_fb_x555));
}

/* Called by the pushbuffer executor when the title flips. */
void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch)
{
    /* App in the background: the flip waits here, and the title's other
     * threads stop behind it (they wait for the GPU). Before the handshake
     * below, so a window that closes meanwhile is not kept waiting. */
    recomp_lifecycle_hold();
    /* Dekker-style handshake with the close path: each side stores its own
     * flag and then loads the other's, all seq_cst, so either the window sees
     * this present in flight or this present sees the window closing. */
    if (!__atomic_load_n(&s_running, __ATOMIC_SEQ_CST) || !fb_va || !pitch)
        return;
    __atomic_add_fetch(&s_in_present, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&s_running, __ATOMIC_SEQ_CST))
        present_copy(fb_va, pitch);
    __atomic_sub_fetch(&s_in_present, 1, __ATOMIC_SEQ_CST);
}

/* A GPU back end's own frame (FLIP_STALL with Nv2aPresentSurface.host_px):
 * copied into a slot like a guest frame, at its own size. Returns 1 when
 * the window took it, 0 when there is no window (headless, closing) or the
 * frame is not one it can show. */
int xbox_FramebufferWindowPresentHost(const void *px, uint32_t w, uint32_t h,
                                      uint32_t pitch, uint32_t format)
{
    uint32_t sdl_format;
    int taken = 0;

    recomp_lifecycle_hold();
    if (!__atomic_load_n(&s_running, __ATOMIC_SEQ_CST) || !px || !w || !h
            || w > 8192 || h > 8192 || pitch < w * 4u)
        return 0;
    if (format == XBOX_FB_HOST_BGRX8888)
        sdl_format = SDL_PIXELFORMAT_XRGB8888;   /* 0xXXRRGGBB words */
    else if (format == XBOX_FB_HOST_RGBX8888)
        sdl_format = SDL_PIXELFORMAT_XBGR8888;   /* bytes R, G, B, X */
    else
        return 0;
    __atomic_add_fetch(&s_in_present, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&s_running, __ATOMIC_SEQ_CST)) {
        /* The copy's cost on the executor thread, logged every 600 host
         * frames (only this thread presents). */
        static uint32_t frames;
        static uint64_t ns;
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        present_pixels((const uint8_t *)px, w, h, pitch, w * 4u, sdl_format);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        ns += (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ull
            + (uint64_t)t1.tv_nsec - (uint64_t)t0.tv_nsec;
        if (++frames == 600) {
            fprintf(stderr, "  [FBWIN] host frames %ux%u: copy %.3f ms/frame\n", w, h,
                    (double)ns / 1e6 / frames);
            frames = 0;
            ns = 0;
        }
        taken = 1;
    }
    __atomic_sub_fetch(&s_in_present, 1, __ATOMIC_SEQ_CST);
    return taken;
}

/* Title bar: "<XBE title> | FPS: n | draws: n", as the GDI window shows it.
 * Written by the loader and the flip, read once a second by the main
 * thread; a torn read shows one stale value for a second. */
static char s_title[128] = "Xbox Recomp";

void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars)
{
    char buf[sizeof s_title];
    size_t o = 0;
    int i;

    /* UTF-16 (the certificate's title) to UTF-8 for SDL. BMP only: a lone
     * surrogate becomes '?'. */
    for (i = 0; i < max_chars && name[i]; i++) {
        uint32_t c = name[i];
        if (c >= 0xD800 && c <= 0xDFFF)
            c = '?';
        if (c < 0x80) {
            if (o + 1 >= sizeof buf) break;
            buf[o++] = (char)c;
        } else if (c < 0x800) {
            if (o + 2 >= sizeof buf) break;
            buf[o++] = (char)(0xC0 | (c >> 6));
            buf[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            if (o + 3 >= sizeof buf) break;
            buf[o++] = (char)(0xE0 | (c >> 12));
            buf[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            buf[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    if (o) {
        buf[o] = 0;
        memcpy(s_title, buf, o + 1);
    }
}

void xbox_FramebufferWindowFrameStats(uint32_t draws)
{
    __atomic_add_fetch(&s_flips, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&s_frame_draws, draws, __ATOMIC_RELAXED);
}

/* The title bar's rate, kept for other readers (the Android companion
 * panel): frames per second x 100, set once a second by the main loop. */
static uint32_t s_fps_centi;

void xbox_FramebufferWindowGetStats(uint32_t *fps_centi, uint32_t *draws)
{
    if (fps_centi)
        *fps_centi = __atomic_load_n(&s_fps_centi, __ATOMIC_RELAXED);
    if (draws)
        *draws = __atomic_load_n(&s_frame_draws, __ATOMIC_RELAXED);
}

/* One pixel of a slot as 0xRRGGBB. */
static uint32_t slot_rgb(const uint8_t *p, uint32_t format)
{
    uint32_t v;

    switch (format) {
    case SDL_PIXELFORMAT_XRGB8888:      /* bytes B, G, R, X */
        return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16;
    case SDL_PIXELFORMAT_XBGR8888:      /* bytes R, G, B, X */
        return (uint32_t)p[2] | (uint32_t)p[1] << 8 | (uint32_t)p[0] << 16;
    case SDL_PIXELFORMAT_RGB555:
        v = (uint32_t)p[0] | (uint32_t)p[1] << 8;
        return ((v >> 10 & 0x1F) * 255u / 31u) << 16 | ((v >> 5 & 0x1F) * 255u / 31u) << 8
             | (v & 0x1F) * 255u / 31u;
    default:                            /* RGB565 */
        v = (uint32_t)p[0] | (uint32_t)p[1] << 8;
        return ((v >> 11 & 0x1F) * 255u / 31u) << 16 | ((v >> 5 & 0x3F) * 255u / 63u) << 8
             | (v & 0x1F) * 255u / 31u;
    }
}

/* w x h pixels of a slot as a 24-bit BMP. -1: the file could not be written. */
static int write_bmp(const char *path, const uint8_t *px, uint32_t w, uint32_t h,
                     uint32_t pitch, uint32_t format)
{
    uint32_t bypp = SDL_BYTESPERPIXEL(format), row = (w * 3u + 3u) & ~3u;
    uint32_t img = row * h, total = 54u + img, x, y;
    uint8_t hdr[54], *line;
    FILE *f;

    line = (uint8_t *)calloc(1, row);
    f = line ? fopen(path, "wb") : NULL;
    if (!f) {
        free(line);
        return -1;
    }
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &total, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, sizeof hdr, f);
    for (y = 0; y < h; y++) {
        const uint8_t *src = px + (size_t)(h - 1 - y) * pitch;
        for (x = 0; x < w; x++) {
            uint32_t v = slot_rgb(src + (size_t)x * bypp, format);
            line[x * 3 + 0] = (uint8_t)v;
            line[x * 3 + 1] = (uint8_t)(v >> 8);
            line[x * 3 + 2] = (uint8_t)(v >> 16);
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    if (fclose(f) != 0)
        return -1;
    fprintf(stderr, "  [FBWIN] wrote %s (%ux%u)\n", path, w, h);
    return 0;
}

/* The last published frame as a 24-bit BMP at its own size (640x480 for a
 * guest frame, the back end's size for a host frame), from any thread. The
 * ready slot is only swapped under s_lock and nobody writes it, so it is
 * copied under the lock and written out after. -1: no frame yet, or the
 * file could not be written. */
int xbox_FramebufferDumpBmp(const char *path)
{
    uint32_t w = 0, h = 0, pitch = 0, format = 0;
    uint8_t *px = NULL;
    int rc;

    pthread_mutex_lock(&s_lock);
    if (s_ready_seq && s_slot[s_ready].px) {
        w = s_slot[s_ready].w;
        h = s_slot[s_ready].h;
        pitch = s_slot[s_ready].pitch;
        format = s_slot[s_ready].format;
        if ((px = (uint8_t *)malloc((size_t)pitch * h)))
            memcpy(px, s_slot[s_ready].px, (size_t)pitch * h);
    }
    pthread_mutex_unlock(&s_lock);
    if (!px)
        return -1;
    rc = write_bmp(path, px, w, h, pitch, format);
    free(px);
    return rc;
}

/* ── The guest thread ─────────────────────────────────────────────────── */

static int (*s_guest_main)(void);
static volatile int s_guest_rc;
static Uint32 s_guest_done_event;

static void *guest_thread(void *arg)
{
    SDL_Event ev;

    (void)arg;
    s_guest_rc = s_guest_main();
    memset(&ev, 0, sizeof ev);
    ev.type = s_guest_done_event;
    SDL_PushEvent(&ev);
    return NULL;
}

static int start_guest(void)
{
    pthread_attr_t attr;
    pthread_t th;
    struct rlimit rl;
    size_t stack = 64u << 20;
    int rc;

    /* At least the main thread's stack (8 MB by default), and generously
     * more: lifted code recurses on the native stack. */
    if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY
            && rl.rlim_cur > stack)
        stack = (size_t)rl.rlim_cur;

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack);
#if defined(__APPLE__)
    /* Same scheduling class as the thread it replaces, so moving the guest
     * off the main thread does not move it to the efficiency cores. */
    {
        qos_class_t qos;
        int rel;
        if (pthread_get_qos_class_np(pthread_self(), &qos, &rel) == 0
                && qos != QOS_CLASS_UNSPECIFIED)
            pthread_attr_set_qos_class_np(&attr, qos, rel);
    }
#endif
    rc = pthread_create(&th, &attr, guest_thread, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0)
        return 0;
    pthread_detach(th);
    return 1;
}

/* ── The window, on the process main thread ───────────────────────────── */

/* SDL's background / foreground events, on the thread that sends them (the
 * Android UI thread), before the main loop sees them. */
static int SDLCALL lifecycle_watch(void *user, SDL_Event *ev)
{
    (void)user;
    if (ev->type == SDL_APP_WILLENTERBACKGROUND)
        recomp_lifecycle_pause();
    else if (ev->type == SDL_APP_DIDENTERFOREGROUND)
        recomp_lifecycle_resume();
    return 1;
}

static int env_on(const char *name)
{
    const char *e = getenv(name);
    return e && *e && *e != '0';
}

int xbox_HostWindowMain(int (*guest_main)(void))
{
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex = NULL;
    uint32_t tex_w = 0, tex_h = 0, tex_format = 0;
    int tex_linear = 0;
    uint64_t shown_seq = 0, t_title = 0, flips_title = 0, t_overlay = 0;
    const char *e;
    int vsync, scale, quit = 0, guest_done = 0, dirty = 1, reupload = 0;
    int stay_after_guest = 0;   /* Android: the window outlives the guest */
    uint64_t back_at = 0;

    /* Ctrl-C and SIGTERM keep their default meaning; SDL would turn them
     * into an SDL_QUIT that a headless run never reads. Set before the
     * headless return: audio and input start SDL subsystems there too. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
#if defined(XBOXRECOMP_HAVE_GPU)
    /* RECOMP_GPU=1: the GL back end draws instead of the CPU rasteriser.
     * Before the guest starts, windowed or not; its context lives on the
     * executor thread, frames still reach this window through guest
     * memory (src/gpu/nv2a_gl.c). */
    nv2a_gl_install_from_env();
#endif
    if (!env_on("RECOMP_FB_WINDOW") || env_on("RECOMP_HEADLESS"))
        return guest_main();

    e = getenv("RECOMP_PRESENT_VSYNC");
    vsync = !e || env_on("RECOMP_PRESENT_VSYNC");
    e = getenv("RECOMP_WINDOW_SCALE");
    scale = e ? atoi(e) : 2;
    if (scale < 1 || scale > 8)
        scale = 2;

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
#ifdef __ANDROID__
    /* Back is trapped. In the app ShellActivity takes it first (it opens the
     * in-game menu, docs/android/app-shell.md « In-game menu »), so the
     * Back-twice path below only runs where nothing above SDL takes it. */
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
#endif
    if (SDL_Init(FBWIN_SDL_SUBSYSTEMS) != 0) {
        fprintf(stderr, "  [FBWIN] SDL video unavailable (%s); running headless\n",
                SDL_GetError());
        return guest_main();
    }
    /* Honor the same fullscreen environment setting as the Windows presenter. */
    Uint32 window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (env_on("RECOMP_FULLSCREEN"))
        window_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

    win = SDL_CreateWindow(s_title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           (int)FB_W * scale, (int)FB_H * scale,
                           window_flags);
    ren = win ? SDL_CreateRenderer(win, -1,
                                   vsync ? SDL_RENDERER_PRESENTVSYNC : 0)
              : NULL;
    if (!ren) {
        fprintf(stderr, "  [FBWIN] SDL window/renderer failed (%s); running headless\n",
                SDL_GetError());
        if (win)
            SDL_DestroyWindow(win);
        SDL_QuitSubSystem(FBWIN_SDL_SUBSYSTEMS);
        return guest_main();
    }
    SDL_RenderSetLogicalSize(ren, (int)FB_W, (int)FB_H);
    {
        SDL_RendererInfo ri;
        if (SDL_GetRendererInfo(ren, &ri) == 0)
            fprintf(stderr, "  [FBWIN] framebuffer window open (%ux%u, SDL %s renderer,"
                    " vsync %s); guest runs on a pthread\n",
                    FB_W, FB_H, ri.name, vsync ? "on" : "off");
    }
#if defined(XBOXRECOMP_HAVE_GPU)
    {
        /* The drawable's real size (HiDPI pixels), for the GL back end's
         * RECOMP_GPU_SCALE=auto; read on the executor thread after the
         * guest starts. Headless runs never set it: scale 1, no host
         * frames. */
        int ow = 0, oh = 0;
        if (SDL_GetRendererOutputSize(ren, &ow, &oh) == 0 && ow > 0 && oh > 0)
            nv2a_gl_set_display_size((uint32_t)ow, (uint32_t)oh);
    }
#endif

    SDL_AddEventWatch(lifecycle_watch, NULL);

    s_guest_done_event = SDL_RegisterEvents(2);
    if (s_guest_done_event == (Uint32)-1) {
        fprintf(stderr, "  [FBWIN] SDL_RegisterEvents failed; running headless\n");
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        SDL_QuitSubSystem(FBWIN_SDL_SUBSYSTEMS);
        return guest_main();
    }
    s_frame_event = s_guest_done_event + 1;
#ifdef __ANDROID__
    stay_after_guest = 1;
#endif
    s_guest_main = guest_main;
    /* This thread pumps SDL events from here on, which also updates the
     * game controllers; the input layer then only reads them. */
    xbox_InputSetMainPump(1);
    xbox_InputSdlInit();
    s_running = 1;
    if (!start_guest()) {
        fprintf(stderr, "  [FBWIN] cannot start the guest thread; running headless\n");
        s_running = 0;
        xbox_InputSetMainPump(0);
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        SDL_QuitSubSystem(FBWIN_SDL_SUBSYSTEMS);
        return guest_main();
    }

    while (!quit && !guest_done) {
        SDL_Event ev;
        uint64_t seq, now;
        const uint8_t *px = NULL;
        uint32_t fw = 0, fh = 0, pitch = 0, format = 0;

        /* Before the first flip, poll at the display rate: the window then
         * reads the display surface itself. After, the flip's event wakes
         * the loop. */
        if (SDL_WaitEventTimeout(&ev, shown_seq ? 100 : 16)) {
            do {
                /* Keys for the pad stand-in, and the window events that
                 * release them; it only reads the event. */
                xbox_InputSdlEvent(&ev);
                if (ev.type == SDL_QUIT)
                    quit = 1;
                else if (ev.type == s_guest_done_event) {
                    if (stay_after_guest) {
                        fprintf(stderr, "  [FBWIN] guest returned (%d); the window stays\n",
                                s_guest_rc);
#ifdef __ANDROID__
                        char done_msg[96];
                        SDL_snprintf(done_msg, sizeof done_msg, TEXT_GUEST_DONE, s_guest_rc);
                        SDL_AndroidShowToast(done_msg, 1, -1, 0, 0);
#endif
                    } else
                        guest_done = 1;
                }
                else if (ev.type == s_frame_event)
                    __atomic_store_n(&s_wake_pending, 0, __ATOMIC_RELEASE);
                else if (ev.type == SDL_WINDOWEVENT
                         && (ev.window.event == SDL_WINDOWEVENT_EXPOSED
                             || ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED))
                    dirty = 1;
                else if (ev.type == SDL_RENDER_TARGETS_RESET)
                    dirty = 1;
                else if (ev.type == SDL_RENDER_DEVICE_RESET) {
                    /* GL context lost (some Android devices, on resume): the
                     * texture is gone; upload the frame shown again. */
                    if (tex)
                        SDL_DestroyTexture(tex);
                    tex = NULL;
                    perf_overlay_reset();
                    reupload = 1;
                    dirty = 1;
                } else if (ev.type == SDL_KEYDOWN && !ev.key.repeat
                           && ev.key.keysym.scancode == SDL_SCANCODE_AC_BACK) {
                    uint64_t t = now_ms();
                    if (back_at && t - back_at <= BACK_QUIT_MS) {
                        fprintf(stderr, "  [FBWIN] Back pressed twice, quitting\n");
                        quit = 1;
                    } else {
                        back_at = t;
                        fprintf(stderr, "  [FBWIN] QUIT_PROMPT shown\n");
#ifdef __ANDROID__
                        SDL_AndroidShowToast(TEXT_QUIT_PROMPT, 0, -1, 0, 0);
#endif
                    }
                }
            } while (SDL_PollEvent(&ev));
        }

        pthread_mutex_lock(&s_lock);
        seq = s_ready_seq;
        if (seq != shown_seq) {
            int t = s_front;
            s_front = s_ready;
            s_ready = t;
        }
        pthread_mutex_unlock(&s_lock);

        if (seq != shown_seq || (reupload && shown_seq)) {
            shown_seq = seq;
            px = s_slot[s_front].px;
            fw = s_slot[s_front].w;
            fh = s_slot[s_front].h;
            pitch = s_slot[s_front].pitch;
            format = s_slot[s_front].format;
        } else if (!shown_seq && s_fb_va && s_fb_pitch) {
            /* No flip yet: the display surface, as the GDI window reads it. */
            uint32_t bpp = s_fb_pitch / FB_W;
            fw = FB_W;
            fh = FB_H;
            format = guest_format(bpp, bpp == 2 && s_fb_x555);
            pitch = s_fb_pitch;
            if (bpp == 2 || bpp == 4)
                px = (const uint8_t *)((uintptr_t)s_fb_va + xbox_GetMemoryOffset());
        }
        if (px && fw && fh) {
            if (!tex || fw != tex_w || fh != tex_h || format != tex_format) {
                if (tex)
                    SDL_DestroyTexture(tex);
                tex = SDL_CreateTexture(ren, format, SDL_TEXTUREACCESS_STREAMING,
                                        (int)fw, (int)fh);
                if (tex && (fw != tex_w || fh != tex_h))
                    fprintf(stderr, "  [FBWIN] texture %ux%u (%s)\n", fw, fh,
                            SDL_GetPixelFormatName(format));
                tex_w = tex ? fw : 0;
                tex_h = tex ? fh : 0;
                tex_format = format;
                tex_linear = -1;            /* chosen below */
            }
            if (tex)
                SDL_UpdateTexture(tex, NULL, px, (int)pitch);
            dirty = 1;
            reupload = 0;
        }
        if (dirty && tex) {
            /* A frame at the guest's 640x480 keeps the window's nearest
             * filter; one at another size (a GPU back end's internal
             * resolution) is filtered linearly unless it maps 1:1 onto the
             * letterboxed rectangle it is shown in. */
            int linear = 0;
            if (tex_w != FB_W || tex_h != FB_H) {
                int ow = 0, oh = 0;
                uint32_t rw, rh;
                SDL_GetRendererOutputSize(ren, &ow, &oh);
                if ((uint64_t)ow * FB_H <= (uint64_t)oh * FB_W) {
                    rw = (uint32_t)ow;
                    rh = (uint32_t)((uint64_t)ow * FB_H / FB_W);
                } else {
                    rh = (uint32_t)oh;
                    rw = (uint32_t)((uint64_t)oh * FB_W / FB_H);
                }
                linear = tex_w != rw || tex_h != rh;
            }
            if (linear != tex_linear) {
                SDL_SetTextureScaleMode(tex, linear ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
                tex_linear = linear;
            }
        }
        if (dirty) {
            dirty = 0;
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
            SDL_RenderClear(ren);
            if (tex)
                SDL_RenderCopy(ren, tex, NULL, NULL);
            perf_overlay_draw(ren);
            SDL_RenderPresent(ren);
        }

        now = now_ms();
        if (perf_overlay_enabled() && now - t_overlay >= 500) {
            if (perf_overlay_tick(now, __atomic_load_n(&s_flips, __ATOMIC_RELAXED),
                                  __atomic_load_n(&s_frame_draws, __ATOMIC_RELAXED),
                                  strcmp(s_title, "Xbox Recomp") ? s_title : NULL))
                dirty = 1;
            t_overlay = now;
        }
        if (now - t_title >= 1000) {
            uint64_t f = __atomic_load_n(&s_flips, __ATOMIC_RELAXED);
            char tb[192];
            snprintf(tb, sizeof tb, "%s | FPS: %.1f | draws: %u", s_title,
                     t_title ? (double)(f - flips_title) * 1000.0 / (double)(now - t_title)
                             : 0.0,
                     (unsigned)__atomic_load_n(&s_frame_draws, __ATOMIC_RELAXED));
            SDL_SetWindowTitle(win, tb);
            __atomic_store_n(&s_fps_centi, t_title ? (uint32_t)((f - flips_title) * 100000u
                                                                / (now - t_title)) : 0u,
                             __ATOMIC_RELAXED);
            t_title = now;
            flips_title = f;
        }
    }

    /* No present may be mid-publish (it pushes an SDL event) once SDL goes.
     * A flip held by the lifecycle is let go first: it then sees s_running
     * clear and returns. */
    SDL_DelEventWatch(lifecycle_watch, NULL);
    __atomic_store_n(&s_running, 0, __ATOMIC_SEQ_CST);
    recomp_lifecycle_resume_for(RECOMP_HOLD_ALL);   /* background and menu */
    while (__atomic_load_n(&s_in_present, __ATOMIC_SEQ_CST))
        sched_yield();
    fprintf(stderr, "  [FBWIN] %s\n", quit ? "window closed" : "guest returned");
    fflush(stdout);
    fflush(stderr);

    /* Window and renderer go first, on this thread as AppKit wants. SDL_Quit
     * is only safe once nothing else uses SDL: the USB and guest threads read
     * the game controllers, so with one open SDL stays up and exit()
     * reclaims it. The main-pump flag stays set, so those threads do not
     * start calling SDL_GameControllerUpdate while SDL goes down. */
    if (tex)
        SDL_DestroyTexture(tex);
    perf_overlay_reset();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    if (!xbox_InputAnyOpen())
        SDL_Quit();
    else
        SDL_QuitSubSystem(FBWIN_SDL_SUBSYSTEMS);
    if (quit)
        recomp_quit(0);     /* exit(0); on Android flush, drain, _exit(0) */
    return s_guest_rc;
}

#endif /* !_WIN32 */
