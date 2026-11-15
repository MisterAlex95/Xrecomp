/* XMV movies decoded by the runtime (XBA-70). See xmv_hle.h and
 * docs/runtime/xmv.md. */
#include "xmv_hle.h"
#include "xmv/xmv_player.h"
#include "xmv/xmv_surface.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#include "platform/xbox_winnt.h"        /* VirtualQuery */
#include "audio/host_audio.h"
#endif

extern ptrdiff_t xbox_GetMemoryOffset(void);

/* ── Sessions ──────────────────────────────────────────────────────────── */

#define MAX_SESSIONS 4

typedef struct {
    uint32_t decoder;           /* 0: free */
    unsigned gen;               /* the movie open it plays */
    XmvPlayer *player;
    int ended, have_pending;
    uint32_t pending_pts;
    uint64_t start_ns;
    uint32_t frames;
    /* Audio resampling to the device's rate: position between the last two
     * input frames, in 1/65536 of a frame, and those frames. */
    uint32_t rs_frac;
    int16_t rs_prev[2], rs_cur[2];
    int rs_primed;
    /* XBA-199: the title's own decoder plays this movie (the runtime would
     * not decode it exactly, or cannot open it), and its watchdog. */
    int delegated;
    uint64_t last_poll_ns, stall_ns;
    uint32_t native_frames;
} XmvSession;

static XmvSession s_sess[MAX_SESSIONS];
static char s_path[1024];
static unsigned s_gen;

#if defined(_WIN32)
static SRWLOCK s_lock = SRWLOCK_INIT;
static void lock(void) { AcquireSRWLockExclusive(&s_lock); }
static void unlock(void) { ReleaseSRWLockExclusive(&s_lock); }
static uint64_t now_ns(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
}
#else
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static void lock(void) { pthread_mutex_lock(&s_lock); }
static void unlock(void) { pthread_mutex_unlock(&s_lock); }
static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#endif

static int mode_skip(void)
{
    const char *e = getenv("RECOMP_XMV");
    return e && (!strcmp(e, "skip") || !strcmp(e, "0"));
}

static int mode_native(void)
{
    const char *e = getenv("RECOMP_XMV");
    return e && !strcmp(e, "native");
}

/* RECOMP_XMV=runtime: the runtime's decoder for every movie, also those it
 * does not fully support (no hand-over to the title's own decoder). */
static int mode_runtime(void)
{
    const char *e = getenv("RECOMP_XMV");
    return e && !strcmp(e, "runtime");
}

/* The watchdog of a movie handed to the title's own decoder: seconds without
 * a frame while the title polls (RECOMP_XMV_WATCHDOG, default 10, 0 = off). */
static uint64_t watchdog_ns(void)
{
    const char *e = getenv("RECOMP_XMV_WATCHDOG");
    double sec = e && *e ? atof(e) : 10.0;
    return sec > 0 ? (uint64_t)(sec * 1e9) : 0;
}

int xmv_hle_bound(int what, uint32_t va, unsigned confidence_pct)
{
    static volatile int logged[2];
    static const char *const names[2] = { "XMVDecoder_GetNextFrame", "XMVDecoder_CloseDecoder" };
    int native = mode_native();
    if (what >= 0 && what < 2 && !logged[what]) {
        logged[what] = 1;
        fprintf(stderr, "[XMV] %s at %08X bound by Prepare (found with confidence %u.%02u): %s\n", names[what],
                va, confidence_pct / 100, confidence_pct % 100,
                native ? "RECOMP_XMV=native, the title's own decoder runs"
                : mode_skip() ? "RECOMP_XMV=skip, movies end at once"
                : mode_runtime() ? "the runtime decodes the title's movies (RECOMP_XMV=runtime: all of them)"
                                 : "the runtime decodes the title's movies it fully supports, the title's own "
                                   "decoder the others");
    }
    return !native;
}

void xbox_MovieFileOpened(const char *host_path)
{
    if (!host_path)
        return;
    lock();
    strncpy(s_path, host_path, sizeof s_path - 1);
    s_path[sizeof s_path - 1] = 0;
    s_gen++;
    unlock();
}

static void session_end(XmvSession *s)
{
    if (s->delegated) {
        if (s->player)
            xmv_player_close(s->player);
        s->player = NULL;
        fprintf(stderr, "[XMV] decoder %08X done: the title's own decoder gave %u frames\n", s->decoder,
                s->native_frames);
        s->delegated = 0;
    } else if (s->player) {
        XmvPlayerStats st;
        xmv_player_stats(s->player, &st);
        fprintf(stderr, "[XMV] movie ended: %u frames (%u decoded, %u X8 intra not supported, %u damaged)\n",
                st.frames, st.decoded, st.unsupported, st.errors);
        xmv_player_close(s->player);
        s->player = NULL;
    }
    s->ended = 1;
}

/* The runtime starts playing the session's (open) movie now. */
static void session_start(XmvSession *s)
{
    const XmvInfo *in = xmv_player_info(s->player);
    fprintf(stderr, "[XMV] decoder %08X plays %s: %ux%u, %u ms, %d audio track(s)\n", s->decoder, s_path,
            in->width, in->height, in->duration_ms, in->ntracks);
    s->start_ns = now_ns();
#if !defined(_WIN32)
    if (xmv_player_audio_track(s->player) >= 0)
        host_audio_open(xmv_player_audio_rate(s->player), xmv_player_audio_channels(s->player) >= 2 ? 2 : 1);
#endif
}

/* The first frames the hand-over probe looks at (their picture header) and,
 * of them, decodes: X8 intra frames further in are not seen (a minute at 30
 * frames per second). */
#define PROBE_FRAMES 1800
#define PROBE_DECODE 30

/* Why the runtime's decoder would not play this movie exactly (written into
 * why), or 0 if it would. Leaves the player at the start. */
static int unsupported(XmvPlayer *pl, char *why, size_t n)
{
    const XmvInfo *in = xmv_player_info(pl);
    XmvProbe pr;
    int i;
    for (i = 0; i < in->ntracks; i++) {
        const XmvTrack *t = &in->track[i];
        if (t->compression == XMV_CODEC_WMAV2) {
            snprintf(why, n, "audio track %d is WMA (the runtime plays Xbox ADPCM and PCM)", i);
            return 1;
        }
        if (!((t->compression == XMV_CODEC_XBOX_ADPCM && t->bits_per_sample == 4) ||
              (t->compression == XMV_CODEC_PCM && t->bits_per_sample == 16))) {
            snprintf(why, n, "audio track %d is format 0x%04X, %u bits", i, t->compression, t->bits_per_sample);
            return 1;
        }
        if ((t->flags & (XMV_AUDIO_51_FRONT | XMV_AUDIO_51_CENTER | XMV_AUDIO_51_REAR)) || t->channels > 2) {
            snprintf(why, n, "%s sound (the runtime plays two channels)", t->channels > 2 ? "multichannel" : "5.1");
            return 1;
        }
    }
    if (xmv_player_probe(pl, PROBE_FRAMES, PROBE_DECODE, &pr) < 0) {
        snprintf(why, n, "the movie could not be rewound after its probe");
        return 1;
    }
    if (pr.x8) {
        snprintf(why, n, "X8 intra frames (%u in the first %u frames; XBA-198)", pr.x8, pr.frames);
        return 1;
    }
    if (pr.broken) {
        snprintf(why, n, "damaged packets after frame %u", pr.frames);
        return 1;
    }
    if (pr.damaged >= 2 && pr.damaged * 16 >= pr.decoded) {
        snprintf(why, n, "%u damaged frames in the first %u", pr.damaged, pr.decoded);
        return 1;
    }
    return 0;
}

/* The decoder object's session. A new one opens the movie the title opened
 * last. With can_delegate (the binding, which can call the title's own
 * decoder) and RECOMP_XMV=auto, a movie the runtime would not decode exactly,
 * or cannot open, is handed to the title's own decoder (delegated). */
static XmvSession *session_get(uint32_t decoder, int can_delegate)
{
    XmvSession *s = NULL, *free_slot = NULL;
    char why[sizeof s_path + 64];
    int i;
    for (i = 0; i < MAX_SESSIONS; i++) {
        if (s_sess[i].decoder == decoder)
            s = &s_sess[i];
        else if (!s_sess[i].decoder && !free_slot)
            free_slot = &s_sess[i];
    }
    /* An ended movie, and the title has opened another since: a new one on
     * the same decoder object. */
    if (s && (s->ended || s->delegated) && s->gen != s_gen) {
        session_end(s);
        memset(s, 0, sizeof *s);
        free_slot = s;
        s = NULL;
    }
    if (s)
        return s;
    if (!free_slot) {
        /* Recycle the oldest: titles play one movie at a time. */
        free_slot = &s_sess[0];
        session_end(free_slot);
    }
    s = free_slot;
    memset(s, 0, sizeof *s);
    s->decoder = decoder;
    s->gen = s_gen;
    s->start_ns = now_ns();
    if (can_delegate && !mode_skip() && !mode_runtime() &&
        (!s_path[0] || !(s->player = xmv_player_open_file(s_path)) || unsupported(s->player, why, sizeof why))) {
        if (!s_path[0])
            snprintf(why, sizeof why, "no .xmv file opened");
        else if (!s->player)
            snprintf(why, sizeof why, "%s is not an XMV file the runtime can play", s_path);
        fprintf(stderr, "[XMV] decoder %08X: %s: the title's own decoder plays this movie\n", decoder, why);
        s->delegated = 1;
        return s;
    }
    if (mode_skip() || !s_path[0]) {
        fprintf(stderr, "[XMV] decoder %08X: %s, the movie is skipped\n", decoder,
                s_path[0] ? "RECOMP_XMV=skip" : "no .xmv file opened");
        s->ended = 1;
        return s;
    }
    if (!s->player)
        s->player = xmv_player_open_file(s_path);
    if (!s->player) {
        fprintf(stderr, "[XMV] %s: not an XMV file the runtime can play, skipped\n", s_path);
        s->ended = 1;
        return s;
    }
    session_start(s);
    return s;
}

static void pump_audio(XmvSession *s)
{
#if !defined(_WIN32)
    /* Decoded audio to the host device: down to its channel count (the
     * first channel, or the first two) and, when the device runs at another
     * rate (it keeps the format of its first open), linearly resampled. */
    int16_t buf[1024 * 8], out[4096 * 2];
    int ch = xmv_player_audio_channels(s->player), dch = host_audio_channels();
    int rate = xmv_player_audio_rate(s->player), drate = host_audio_rate();
    uint32_t step;
    size_t n, i, o = 0;
    int c;
    if (ch <= 0 || dch <= 0 || drate <= 0 || rate <= 0)
        return;                         /* no device */
    step = (uint32_t)(((uint64_t)rate << 16) / (uint64_t)drate);
    while ((n = xmv_player_read_audio(s->player, buf, 1024)) > 0) {
        for (i = 0; i < n; i++) {
            const int16_t *f = buf + i * (size_t)ch;
            s->rs_prev[0] = s->rs_cur[0];
            s->rs_prev[1] = s->rs_cur[1];
            s->rs_cur[0] = f[0];
            s->rs_cur[1] = f[ch >= 2 ? 1 : 0];
            if (!s->rs_primed) {
                s->rs_primed = 1;
                continue;
            }
            /* Output frames between the previous input frame and this one. */
            while (s->rs_frac < 65536u) {
                for (c = 0; c < dch; c++) {
                    int a = s->rs_prev[c], b = s->rs_cur[c];
                    out[o * (size_t)dch + (size_t)c] = (int16_t)(a + (int)(((int64_t)(b - a) * s->rs_frac) >> 16));
                }
                s->rs_frac += step;
                if (++o == 4096) {
                    host_audio_queue(out, (uint32_t)o);
                    o = 0;
                }
            }
            s->rs_frac -= 65536u;
        }
    }
    if (o)
        host_audio_queue(out, (uint32_t)o);
#else
    int16_t buf[1024 * 8];
    while (xmv_player_read_audio(s->player, buf, 1024) > 0) {
    }
#endif
}

/* Host address of guest memory: a virtual address, or a physical one seen
 * through the contiguous mapping at 0x80000000. NULL if not committed. */
static uint8_t *guest_map(uint32_t addr, size_t bytes)
{
    uintptr_t va;
    MEMORY_BASIC_INFORMATION m;
    if (addr < 0x80000000u)
        addr |= 0x80000000u;            /* D3D keeps physical addresses in Data */
    va = (uintptr_t)addr + (uintptr_t)xbox_GetMemoryOffset();
    if (!VirtualQuery((const void *)va, &m, sizeof m) || m.State != MEM_COMMIT ||
        (m.Protect & (PAGE_NOACCESS)) ||
        bytes > m.RegionSize - ((uintptr_t)va - (uintptr_t)m.BaseAddress))
        return NULL;
    return (uint8_t *)va;
}

static int write_frame(XmvSession *s, uint32_t surface)
{
    uint8_t *res = surface ? (uint8_t *)((uintptr_t)surface + (uintptr_t)xbox_GetMemoryOffset()) : NULL;
    uint32_t data, fd, sd, f, w, h, pitch;
    size_t bytes;
    uint8_t *dst;
    const uint8_t *pl[3];
    int st[3];
    const XmvInfo *in = xmv_player_info(s->player);
    if (!res)
        return -1;
    memcpy(&data, res + 4, 4);
    memcpy(&fd, res + 0x0C, 4);
    memcpy(&sd, res + 0x10, 4);
    bytes = xmv_surface_layout(fd, sd, &f, &w, &h, &pitch);
    if (!bytes || !(dst = guest_map(data, bytes))) {
        if (s->frames == 0)
            fprintf(stderr, "[XMV] surface %08X: format %08X size %08X data %08X not written\n", surface, fd,
                    sd, data);
        return -1;
    }
    pl[0] = xmv_player_plane(s->player, 0, &st[0]);
    pl[1] = xmv_player_plane(s->player, 1, &st[1]);
    pl[2] = xmv_player_plane(s->player, 2, &st[2]);
    if (xmv_surface_write(pl, st, (int)in->width, (int)in->height, f, w, h, pitch, dst, bytes) < 0) {
        if (s->frames == 0)
            fprintf(stderr, "[XMV] surface format %02X (%ux%u) not supported\n", f, w, h);
        return -1;
    }
    return 0;
}

static void put32(uint32_t va, uint32_t v)
{
    if (va)
        memcpy((uint8_t *)((uintptr_t)va + (uintptr_t)xbox_GetMemoryOffset()), &v, 4);
}

uint32_t xmv_hle_get_next_frame(uint32_t decoder, uint32_t surface, uint32_t result_va, uint32_t time_va)
{
    XmvSession *s;
    uint32_t result = XMV_NOFRAME, t_ms = 0;
    lock();
    s = session_get(decoder, 0);
    if (s->delegated) {
        /* Handed to the title's own decoder, yet the hook is called: a
         * hand-written override with no own decoder to call. */
        s->delegated = 0;
        if (s->player)
            session_start(s);
        else
            s->ended = 1;
    }
    if (!s->ended && !s->have_pending) {
        if (xmv_player_next_frame(s->player, &s->pending_pts) == 1)
            s->have_pending = 1;
        else
            session_end(s);
    }
    if (s->ended) {
        result = XMV_ENDOFFILE;
    } else {
        uint64_t elapsed_ms = (now_ns() - s->start_ns) / 1000000u;
        pump_audio(s);
        if (elapsed_ms >= s->pending_pts) {
            write_frame(s, surface);
            s->frames++;
            s->have_pending = 0;
            t_ms = s->pending_pts;
            result = XMV_NEWFRAME;
        }
    }
    unlock();
    put32(result_va, result);
    if (time_va) {
        uint64_t t = (uint64_t)t_ms * 10000u;
        put32(time_va, (uint32_t)t);
        put32(time_va + 4, (uint32_t)(t >> 32));
    }
    return 0;                           /* S_OK */
}

void xmv_hle_close(uint32_t decoder)
{
    int i;
    lock();
    for (i = 0; i < MAX_SESSIONS; i++)
        if (s_sess[i].decoder == decoder) {
            session_end(&s_sess[i]);
            memset(&s_sess[i], 0, sizeof s_sess[i]);
        }
    unlock();
}

/* ── The binding's routing (XBA-199) ───────────────────────────────────── */

int xmv_hle_route(uint32_t va, unsigned confidence_pct, uint32_t decoder)
{
    XmvSession *s;
    int ours;
    if (!xmv_hle_bound(0, va, confidence_pct))
        return 0;                       /* RECOMP_XMV=native */
    lock();
    s = session_get(decoder, 1);
    ours = !s->delegated;
    unlock();
    return ours;
}

void xmv_hle_native_result(uint32_t decoder, uint32_t result_va)
{
    XmvSession *s = NULL;
    uint64_t now, wd;
    uint32_t r;
    int i;
    if (!result_va || mode_native())
        return;
    memcpy(&r, (const uint8_t *)((uintptr_t)result_va + (uintptr_t)xbox_GetMemoryOffset()), 4);
    now = now_ns();
    lock();
    for (i = 0; i < MAX_SESSIONS; i++)
        if (s_sess[i].decoder == decoder && s_sess[i].delegated)
            s = &s_sess[i];
    if (!s) {
        unlock();
        return;
    }
    if (r == XMV_NEWFRAME || r == XMV_ENDOFFILE) {
        s->native_frames += r == XMV_NEWFRAME;
        s->stall_ns = 0;
    } else if (s->last_poll_ns && now - s->last_poll_ns < 1000000000ull) {
        /* A second or more between two polls is not counted: the title paused
         * the movie, or was busy elsewhere. */
        s->stall_ns += now - s->last_poll_ns;
    }
    s->last_poll_ns = now;
    wd = watchdog_ns();
    if (wd && s->stall_ns >= wd) {
        /* Hung: nothing of the movie shown yet, the runtime plays it if it
         * can open it (not exactly, but better than a hang); else, or once
         * the title's own decoder has shown frames, the movie ends. */
        int play = s->player && !s->native_frames;
        fprintf(stderr, "[XMV] decoder %08X: the title's own decoder gave no frame for %.1f s while the title "
                "polled: %s\n", decoder, (double)s->stall_ns / 1e9,
                play ? "the runtime plays the movie instead" : "the movie ends");
        s->delegated = 0;
        if (play) {
            session_start(s);
        } else {
            if (s->player)
                xmv_player_close(s->player);
            s->player = NULL;
            s->ended = 1;
        }
    }
    unlock();
}
