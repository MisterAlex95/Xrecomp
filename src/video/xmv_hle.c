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
    if (s->player) {
        XmvPlayerStats st;
        xmv_player_stats(s->player, &st);
        fprintf(stderr, "[XMV] movie ended: %u frames (%u decoded, %u X8 intra not supported, %u damaged)\n",
                st.frames, st.decoded, st.unsupported, st.errors);
        xmv_player_close(s->player);
        s->player = NULL;
    }
    s->ended = 1;
}

static XmvSession *session_get(uint32_t decoder)
{
    XmvSession *s = NULL, *free_slot = NULL;
    int i;
    for (i = 0; i < MAX_SESSIONS; i++) {
        if (s_sess[i].decoder == decoder)
            s = &s_sess[i];
        else if (!s_sess[i].decoder && !free_slot)
            free_slot = &s_sess[i];
    }
    /* An ended movie, and the title has opened another since: a new one on
     * the same decoder object. */
    if (s && s->ended && s->gen != s_gen) {
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
    if (mode_skip() || !s_path[0]) {
        fprintf(stderr, "[XMV] decoder %08X: %s, the movie is skipped\n", decoder,
                s_path[0] ? "RECOMP_XMV=skip" : "no .xmv file opened");
        s->ended = 1;
        return s;
    }
    s->player = xmv_player_open_file(s_path);
    if (!s->player) {
        fprintf(stderr, "[XMV] %s: not an XMV file the runtime can play, skipped\n", s_path);
        s->ended = 1;
        return s;
    }
    {
        const XmvInfo *in = xmv_player_info(s->player);
        fprintf(stderr, "[XMV] decoder %08X plays %s: %ux%u, %u ms, %d audio track(s)\n", decoder, s_path,
                in->width, in->height, in->duration_ms, in->ntracks);
    }
#if !defined(_WIN32)
    if (xmv_player_audio_track(s->player) >= 0)
        host_audio_open(xmv_player_audio_rate(s->player), xmv_player_audio_channels(s->player) >= 2 ? 2 : 1);
#endif
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
    s = session_get(decoder);
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
