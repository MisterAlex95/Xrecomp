/* The PVIDEO overlay on POSIX hosts: films a title plays through
 * D3DDevice_UpdateOverlay (XMV, Bink's Xbox player), shown in the window.
 *
 * PVIDEO scans a YUY2 bank out over the framebuffer, optionally behind a
 * colour key. On POSIX the NV2A aperture is plain memory, so the title's D3D
 * writes the registers there and nothing traps them (nv2a_mmio_hook.c reads
 * them in place). The pieces:
 *
 *   - STOP: the NV2A ack pass answers it (xbox_Nv2aPvideoStop,
 *     xbox_memory_layout.c), not this file: D3DDevice_EnableOverlay(FALSE)
 *     spins on BUFFER and may hold the one-CPU gate the vblank waits for.
 *     The stop count it keeps is how the overlay is seen to go down here.
 *   - Once per vblank (xbox_VblankHookSet, on the kernel's timer thread) the
 *     hook only looks at NV_PVIDEO_BUFFER and the stop count, and wakes the
 *     overlay thread when either changed. That thread copies each pending
 *     bank out of guest memory (bounds-checked with VirtualQuery, as
 *     fb_present.c does), composes it over the last frame the window was
 *     given (pvideo_scanout.h: placement, scaling, colour key), publishes
 *     the result as a host frame -- so the aspect policy and the frame
 *     generation pacer see it like any other -- and only then clears the
 *     bank's BUFFER bit, as the hardware does when it latches the bank. The
 *     conversion stays off the timer thread and off the guest's core.
 *   - The overlay is down after a STOP, or once bank 0 (or the bank shown)
 *     is released with SIZE_IN = 0xFFFFFFFF: XDK D3D's EnableOverlay(FALSE)
 *     writes STOP only while a bank is still pending, which here it rarely
 *     is, and releases bank 0 either way.
 *   - A flip while the overlay is up (pvideo_frame, from the present paths)
 *     becomes the picture under the overlay and is shown with it on top;
 *     the first flip after it went down is the window's again.
 *   - RECOMP_FB_DUMP's pictures show the composition while the overlay is up
 *     (nv2a_pb_exec_set_screen_dump), headless included.
 *
 * The picture is 640x480 whatever the back end's internal resolution: a
 * film's frame is no larger. The last film frame stays on screen after STOP
 * until the title's next flip, which follows at once in practice.
 *
 * RECOMP_PVIDEO=0 turns the scan-out off (the STOP handshake stays). */
#if !defined(_WIN32)

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "platform/xbox_winnt.h"        /* VirtualQuery */
#include "platform/recomp_lifecycle.h"
#include "../nv2a/nv2a_mmio_hook.h"
#include "pvideo_scanout.h"
#include "pvideo_posix.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern uint32_t xbox_Nv2aPvideoStops(void);
extern void xbox_VblankHookSet(void (*hook)(void));
extern void nv2a_pb_exec_set_screen_dump(int (*dump)(const char *path));

#define PV_CONTIG_SIZE 0x04000000u      /* what the overlay can address */
#ifndef PAGE_GUARD
#define PAGE_GUARD 0x100u               /* not in the POSIX shim's header */
#endif
#define PV_REG(regs, reg, bank) (regs)[((reg) + (bank) * 4u) / 4u]

/* Everything below is under the present lock (fbwin_present_lock), except
 * s_up's lock-free reads and the hook's own statics. */
static uint32_t  s_base[PVIDEO_W * PVIDEO_H];  /* the frame under the overlay */
static int       s_have_base;
static uint32_t  s_out[PVIDEO_W * PVIDEO_H];   /* the composition */
static uint8_t  *s_src[2];                     /* bank copies: shown, spare */
static size_t    s_src_cap[2];
static size_t    s_src_bytes;                  /* of s_src[0] */
static uint32_t  s_regs[0x1000 / 4];           /* the registers it was shown with */
static unsigned  s_bank;
static volatile int s_up;                      /* a frame is on screen */
static uint32_t  s_stops;                      /* xbox_Nv2aPvideoStops seen */
static unsigned  s_frames, s_sessions;
static uint64_t  s_compose_ns;
static unsigned  s_compose_n;

static const uint8_t *s_test_mem;
static size_t         s_test_size;

void xbox_PvideoTestMemory(const uint8_t *phys, size_t size)
{
    s_test_mem = phys;
    s_test_size = size;
}

const uint32_t *xbox_PvideoPicture(int *up)
{
    if (up)
        *up = s_up;
    return s_out;
}

static uint64_t pv_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* The host address of `bytes` of guest physical memory at `phys`, or NULL
 * when any of it is outside the contiguous window or not committed. */
static const uint8_t *pv_map(uint64_t phys, uint64_t bytes)
{
    uintptr_t va;
    MEMORY_BASIC_INFORMATION mapping;

    if (s_test_mem)
        return bytes && phys + bytes <= s_test_size ? s_test_mem + phys : NULL;
    if (!bytes || phys + bytes > PV_CONTIG_SIZE)
        return NULL;
    va = (uintptr_t)(0x80000000u + (uint32_t)phys) + (uintptr_t)xbox_GetMemoryOffset();
    if (!VirtualQuery((const void *)va, &mapping, sizeof(mapping)) ||
        mapping.State != MEM_COMMIT ||
        (mapping.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
        bytes > mapping.RegionSize)
        return NULL;
    return (const uint8_t *)va;
}

/* s_out = the overlay bank in `src` (bank `bank`, registers `regs`) over
 * the base frame. Without a base frame the screen is taken to be the colour
 * key (keyed overlay) or black. */
static int pv_compose(const uint32_t *regs, const uint8_t *src, size_t bytes, unsigned bank)
{
    uint64_t t0 = pv_now_ns();
    int ok;

    if (s_have_base)
        memcpy(s_out, s_base, sizeof(s_out));
    else {
        uint32_t fill = (PV_REG(regs, NV_PVIDEO_FORMAT, bank) & NV_PVIDEO_FORMAT_DISPLAY)
                      ? 0xFF000000u | regs[NV_PVIDEO_COLOR_KEY / 4] : 0xFF000000u;
        size_t i;
        for (i = 0; i < PVIDEO_W * PVIDEO_H; i++)
            s_out[i] = fill;
    }
    ok = pvideo_scanout(regs, src, bytes, s_out, PVIDEO_W, PVIDEO_H, bank);
    s_compose_ns += pv_now_ns() - t0;
    if (++s_compose_n == 300) {
        fprintf(stderr, "  [PVIDEO] compose %.3f ms/frame (last 300)\n",
                (double)s_compose_ns / 1e6 / s_compose_n);
        s_compose_ns = 0;
        s_compose_n = 0;
    }
    return ok;
}

/* D3DDevice_EnableOverlay(FALSE) writes STOP only while a bank is pending
 * (XDK 5455 at 0x00352CC0), and the banks are handed back within a vblank
 * here, so mostly it does not: it then marks bank 0 released, SIZE_IN =
 * 0xFFFFFFFF, which is what says the overlay is down. */
static int pv_released(const uint32_t *regs)
{
    return PV_REG(regs, NV_PVIDEO_SIZE_IN, 0) == UINT32_MAX
        || PV_REG(regs, NV_PVIDEO_SIZE_IN, s_bank) == UINT32_MAX;
}

static void pv_down(const char *why)
{
    if (!s_up)
        return;
    s_up = 0;
    if (s_sessions <= 8)
        fprintf(stderr, "  [PVIDEO] overlay off (%s) after %u frame(s)\n", why, s_frames);
}

static int pv_step_locked(void)
{
    uint32_t regs[0x1000 / 4], stops;
    unsigned bank;
    int shown = 0;
    static unsigned refused, unmapped;

    if (!nv2a_hook_pvideo_snapshot(regs))
        return 0;
    stops = xbox_Nv2aPvideoStops();
    if (stops != s_stops) {
        s_stops = stops;
        pv_down("STOP");
    }
    if (s_up && pv_released(regs))
        pv_down("released");
    for (bank = 0; bank < 2; bank++) {
        uint64_t physical = (uint64_t)PV_REG(regs, NV_PVIDEO_BASE, bank)
                          + PV_REG(regs, NV_PVIDEO_OFFSET, bank);
        uint32_t pitch = PV_REG(regs, NV_PVIDEO_FORMAT, bank) & NV_PVIDEO_FORMAT_PITCH;
        uint32_t height = (PV_REG(regs, NV_PVIDEO_SIZE_IN, bank) >> 16) & 0x7ff;
        uint64_t bytes = (uint64_t)pitch * height;
        const uint8_t *src;

        if (!(regs[NV_PVIDEO_BUFFER / 4] & (1u << (bank * 4u))))
            continue;
        src = pv_map(physical, bytes);
        if (!src) {
            if (unmapped++ < 4)
                fprintf(stderr, "  [PVIDEO] bank %u at 0x%08llX (+%llu bytes) is not in mapped"
                        " contiguous memory; skipped\n", bank,
                        (unsigned long long)physical, (unsigned long long)bytes);
        } else {
            /* Into the spare copy: the shown one stays valid if this one is
             * refused, for the flips composed over it. */
            if (s_src_cap[1] < bytes) {
                uint8_t *n = (uint8_t *)realloc(s_src[1], (size_t)bytes);
                if (n) {
                    s_src[1] = n;
                    s_src_cap[1] = (size_t)bytes;
                }
            }
            if (s_src_cap[1] >= bytes) {
                memcpy(s_src[1], src, (size_t)bytes);
                if (!s_up)                    /* what the screen holds now */
                    s_have_base = fbwin_last_frame(s_base);
                if (pv_compose(regs, s_src[1], (size_t)bytes, bank)) {
                    uint8_t *t = s_src[0];
                    size_t c = s_src_cap[0];
                    s_src[0] = s_src[1];
                    s_src_cap[0] = s_src_cap[1];
                    s_src[1] = t;
                    s_src_cap[1] = c;
                    s_src_bytes = (size_t)bytes;
                    memcpy(s_regs, regs, sizeof(s_regs));
                    s_bank = bank;
                    if (!s_up) {
                        s_frames = 0;
                        if (++s_sessions <= 8)
                            fprintf(stderr, "  [PVIDEO] overlay on: %ux%u YUY2 pitch %u -> %ux%u"
                                    " at %u,%u%s\n",
                                    PV_REG(regs, NV_PVIDEO_SIZE_IN, bank) & 0x7ff, height, pitch,
                                    PV_REG(regs, NV_PVIDEO_SIZE_OUT, bank) & 0xfff,
                                    (PV_REG(regs, NV_PVIDEO_SIZE_OUT, bank) >> 16) & 0xfff,
                                    PV_REG(regs, NV_PVIDEO_POINT_OUT, bank) & 0xfff,
                                    (PV_REG(regs, NV_PVIDEO_POINT_OUT, bank) >> 16) & 0xfff,
                                    (PV_REG(regs, NV_PVIDEO_FORMAT, bank) & NV_PVIDEO_FORMAT_DISPLAY)
                                        ? ", colour keyed" : "");
                    }
                    s_up = 1;
                    s_frames++;
                    shown = 1;
                } else if (refused++ < 4) {
                    fprintf(stderr, "  [PVIDEO] bank %u refused (format 0x%08X, size in 0x%08X,"
                            " out 0x%08X); only YUY2 is drawn\n", bank,
                            PV_REG(regs, NV_PVIDEO_FORMAT, bank),
                            PV_REG(regs, NV_PVIDEO_SIZE_IN, bank),
                            PV_REG(regs, NV_PVIDEO_SIZE_OUT, bank));
                }
            }
        }
        /* Handed back even when not drawn: the title waits for the bit
         * before it fills the bank again, as it would on the console. */
        nv2a_hook_pvideo_consume(bank);
    }
    if (shown)
        (void)fbwin_publish_overlay(s_out);
    return shown;
}

int xbox_PvideoStep(void)
{
    int shown;

    /* App in the background: the overlay holds like the flip does, and the
     * film's thread waits for its bank behind it. */
    recomp_lifecycle_hold();
    fbwin_present_lock();
    shown = pv_step_locked();
    fbwin_present_unlock();
    return shown;
}

int pvideo_frame(const uint8_t *px, uint32_t w, uint32_t h, uint32_t pitch, uint32_t format)
{
    uint32_t regs[0x1000 / 4];

    if (!s_up)
        return 0;
    /* Gone since the last vblank? Then this flip is the first frame
     * without it. */
    if (nv2a_hook_pvideo_snapshot(regs)) {
        uint32_t stops = xbox_Nv2aPvideoStops();
        if (stops != s_stops) {
            s_stops = stops;
            pv_down("STOP");
        } else if (pv_released(regs))
            pv_down("released");
        if (!s_up)
            return 0;
    }
    fbwin_to_xrgb(px, w, h, pitch, format, s_base);
    s_have_base = 1;
    if (!pv_compose(s_regs, s_src[0], s_src_bytes, s_bank)) {
        memcpy(s_out, s_base, sizeof(s_out));
        pv_down("registers no longer drawable");
    }
    (void)fbwin_publish_overlay(s_out);
    return 1;
}

/* RECOMP_FB_DUMP while the overlay is up: the composition. */
static int pv_dump(const char *path)
{
    int written = 0;

    fbwin_present_lock();
    if (s_up)
        written = fbwin_write_xrgb_bmp(path, s_out) == 0;
    fbwin_present_unlock();
    return written;
}

/* ── The overlay thread, woken by the vblank hook ─────────────────────── */

static pthread_mutex_t s_wake_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_wake_cv = PTHREAD_COND_INITIALIZER;
static int             s_wake;

static void *pv_thread(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&s_wake_mu);
        while (!s_wake)
            pthread_cond_wait(&s_wake_cv, &s_wake_mu);
        s_wake = 0;
        pthread_mutex_unlock(&s_wake_mu);
        (void)xbox_PvideoStep();
    }
    return NULL;
}

/* On the timer thread, every vblank: cheap. Wakes the overlay thread when a
 * bank is pending or the overlay may have been stopped. */
static void pv_vblank(void)
{
    static int located, started;        /* started: 1 thread running, -1 none: inline */
    static uint32_t stops_seen;
    static uint32_t regs[0x1000 / 4];   /* this thread only */
    uint32_t stops;

    if (!located) {
        /* The register block's address: guest memory is laid out by the
         * time vblanks run. */
        nv2a_hook_init(xbox_GetMemoryOffset());
        located = 1;
    }
    stops = xbox_Nv2aPvideoStops();
    if (!nv2a_hook_pvideo_snapshot(regs))
        return;
    if (!(regs[NV_PVIDEO_BUFFER / 4] & (NV_PVIDEO_BUFFER_0_USE | NV_PVIDEO_BUFFER_1_USE))
            && stops == stops_seen && !(s_up && pv_released(regs)))
        return;
    stops_seen = stops;
    if (!started) {
        pthread_t t;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        started = pthread_create(&t, &attr, pv_thread, NULL) == 0 ? 1 : -1;
        pthread_attr_destroy(&attr);
        if (started < 0)
            fprintf(stderr, "  [PVIDEO] no overlay thread; scanning out on the timer thread\n");
    }
    if (started < 0) {
        (void)xbox_PvideoStep();
        return;
    }
    pthread_mutex_lock(&s_wake_mu);
    s_wake = 1;
    pthread_cond_signal(&s_wake_cv);
    pthread_mutex_unlock(&s_wake_mu);
}

void pvideo_install(void)
{
    static int done;
    const char *e = getenv("RECOMP_PVIDEO");

    if (done)
        return;
    done = 1;
    if (e && *e == '0') {
        fprintf(stderr, "  [PVIDEO] overlay scan-out off (RECOMP_PVIDEO=0)\n");
        return;
    }
    xbox_VblankHookSet(pv_vblank);
    nv2a_pb_exec_set_screen_dump(pv_dump);
}

#endif /* !_WIN32 */
