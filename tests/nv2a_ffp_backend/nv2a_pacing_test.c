/* RECOMP_PACING=auto in the executor (XBA-107, XBA-135): a synthetic title
 * that swaps (software method 1 NOP with its presentation interval) and
 * flips (FLIP_STALL) with a programmable frame cost, on a fake host clock
 * with the guest vblank count running from it. What the walk after each
 * flip sleeps (nv2a_pb_exec_flip_poll) gives the rate the executor paces at.
 *
 *   nv2a-pacing-test <case>
 *     fast      10 ms frames, interval ONE: 60 fps (sleeps ~6.7 ms)
 *     slow      20 ms frames, IMMEDIATE: 30 fps within ~1.5 s (sleeps ~13.3 ms)
 *     slower    40 ms frames, IMMEDIATE: 20 fps
 *     locked    9 ms frames, ONE, for 30 s (judged locked), then 18 ms: never
 *               divided, no sleep (the title would run in slow motion at 30)
 *     capwins   RECOMP_GPU_FPS_CAP=60 set (a profile, the launcher): 20 ms
 *               IMMEDIATE frames are not divided, as before auto
 *     pal       10 ms frames, ONE, 50 Hz vblanks: 50 fps
 *     cpu       no back end (the CPU rasteriser): 20 ms IMMEDIATE: 30 fps
 *     align     20 ms IMMEDIATE (30 fps) with a display grid published at a
 *               phase far from the flips' (XBA-136): within a second the
 *               flips + the presenter's 1 ms land at half a refresh, and stay
 *     alignoff  the same with RECOMP_PACING_ALIGN=0: the flips keep the
 *               schedule's phase
 *     default   RECOMP_PACING unset (WP4: auto is the default): 20 ms
 *               IMMEDIATE frames paced at 30 fps, as under auto
 *     off       RECOMP_PACING=off: the fixed cap, 20 ms IMMEDIATE frames not
 *               divided (60 or no cap, by host: both leave them at 20 ms)
 *
 *   nv2a-pacing-fg-test <case>  (the same source built with XBOXRECOMP_LSFG,
 *   RECOMP_LSFG=1: frame generation's gate, WP4)
 *     fglocked  9 ms frames, ONE: never capped at 30, before the verdict
 *               (20 s + 5 s) or after; the verdict holds frame generation
 *               off; then the title asks TWO (30): frame generation on
 *     fgfree    20 ms frames, ONE: at its ask (20 ms, not divided) until the
 *               verdict says free (20 s + six windows of 5 s that do not
 *               hold), then frame generation on and 30 fps
 *     fgask30   10 ms frames, interval TWO: 30 fps throughout, locked to 30,
 *               which frame generation's cap does not divide: on
 *     fgoff     RECOMP_PACING=off: the fixed cap lowered to 30 from the first
 *               frame, then the XBA-96 watch turns frame generation off
 *     fgnolock  RECOMP_FRAME_LOCK=0: 30 from the first frame, never stopped
 *
 * One case per process: the executor's state is static. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nv2a_backend.h"

#define MEM_BYTES 0x10000u
static uint8_t s_mem[MEM_BYTES];
static int s_fail;
#define T0 1000000000ull
static uint64_t s_now = T0;                 /* fake host clock, ns */
static uint64_t s_slept;
static uint32_t s_vblank_hz = 60;

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); s_fail = 1; \
} } while (0)

ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)s_mem; }
uint32_t g_xbox_image_lo, g_xbox_image_hi;
uint32_t g_xbox_stack_base, g_xbox_stack_size;
uint32_t xbox_ContiguousAllocatedBytes(void) { return 0; }
void xbox_FramebufferWindowSet(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_FramebufferWindowSetAA(uint32_t sy) { (void)sy; }
void xbox_FramebufferWindowSetFormat(uint32_t f) { (void)f; }
uint32_t xbox_Nv2aFenceWordVa(void) { return 0; }
int nv2a_pb_dma_walk(void) { return 1; }
void xbox_FramebufferWindowStart(void) {}
void xbox_FramebufferWindowFrameStats(uint32_t draws) { (void)draws; }
void xbox_FramebufferWindowFrameTris(uint32_t tris) { (void)tris; }
void xbox_FramebufferWindowPresent(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
int xbox_FramebufferWindowPresentHost(const void *px, uint32_t w, uint32_t h, uint32_t pitch,
                                      uint32_t format)
{ (void)px; (void)w; (void)h; (void)pitch; (void)format; return 0; }
void xbox_Nv2aFrameCounterFlip(void) {}
int nv2a_pb_scan_in_call(void) { return 0; }
uint32_t xbox_VblankCount(void) { return (uint32_t)((s_now - T0) * s_vblank_hz / 1000000000ull); }
uint64_t xbox_HostNowNs(void) { return s_now; }
void xbox_HostSleepNs(uint64_t ns) { s_now += ns; s_slept += ns; }
uint32_t xbox_Nv2aFrameCountersOwned(void) { return 0; }
volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset) { (void)offset; return NULL; }

extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
extern int nv2a_pb_exec_flip_poll(void);
extern void nv2a_pb_exec_display_clock(uint64_t vsync_ns, uint64_t period_ns, uint32_t gen);
extern void nv2a_pb_exec_present_latency(uint64_t lat_ns);

/* XBA-136: a fake display grid (period 0: none), republished at every
 * frame as the presenter's vsync source does. */
static uint64_t s_disp_period, s_disp_phase;
static uint64_t s_last_flip;

static void fake_draw(const Nv2aSurface *s, const Nv2aBatch *b) { (void)s; (void)b; }
static void fake_flip(void) {}
static const Nv2aBackend s_backend = {NULL, fake_draw, fake_flip};

#define MS 1000000ull
#define SEC 1000000000ull
#define IV_ONE 1u
#define IV_IMMEDIATE 8u

/* One frame: `work` ns of walk, the swap NOP, FLIP_STALL, then the next
 * walk's wait. Returns what that wait slept. */
static uint64_t frame(uint64_t work, uint32_t interval)
{
    uint64_t before;

    s_now += work;
    if (s_disp_period) {
        uint64_t k = (s_now - T0 - s_disp_phase) / s_disp_period;
        nv2a_pb_exec_display_clock(T0 + s_disp_phase + k * s_disp_period, s_disp_period, 1);
        nv2a_pb_exec_present_latency(1000000ull);
    }
    s_last_flip = s_now;
    nv2a_pb_exec_method(0, 0x0100, (0x00123400u | interval) << 5 | 1u);   /* swap */
    nv2a_pb_exec_method(0, 0x0130, 0);                                      /* FLIP_STALL */
    before = s_slept;
    (void)nv2a_pb_exec_flip_poll();
    return s_slept - before;
}

static int near(uint64_t a, uint64_t b) { return a + 300000 >= b && b + 300000 >= a; }

#ifdef XBOXRECOMP_LSFG
/* The presenter's side of frame generation's gate. */
static int s_fg_stops, s_fg_releases;
void xbox_FramegenRequestStop(const char *why) { (void)why; s_fg_stops++; }
void xbox_FramegenRelease(const char *why) { (void)why; s_fg_releases++; }
#endif

/* Frames of `work` for `secs`; returns the mean frame period (flip to flip)
 * over the last second and checks that every sleep in it was `want_sleep`
 * (UINT64_MAX: no check). */
static double run(uint64_t work, uint32_t interval, double secs, uint64_t want_sleep)
{
    uint64_t end = s_now + (uint64_t)(secs * SEC), last_start = end - SEC, t0 = 0;
    uint32_t n = 0;
    while (s_now < end) {
        uint64_t slept = frame(work, interval);
        if (s_now >= last_start) {
            if (!t0)
                t0 = s_now;
            else
                n++;
            if (want_sleep != UINT64_MAX && !near(slept, want_sleep)) {
                fprintf(stderr, "FAIL slept %.2f ms, want %.2f\n", slept / 1e6, want_sleep / 1e6);
                s_fail = 1;
                want_sleep = UINT64_MAX;    /* one line is enough */
            }
        }
    }
    return n ? (double)(s_now - t0) / n / 1e6 : 0;
}

static void setenv_(const char *k, const char *v)
{
#ifdef _WIN32
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
}

int main(int argc, char **argv)
{
    const char *c = argc > 1 ? argv[1] : "fast";
    double period;

    if (!strcmp(c, "off") || !strcmp(c, "fgoff"))
        setenv_("RECOMP_PACING", "off");
    else if (strcmp(c, "default") != 0 && strncmp(c, "fg", 2) != 0)
        setenv_("RECOMP_PACING", "auto");
    setenv_("RECOMP_DISPLAY_HZ", "60");
#ifdef XBOXRECOMP_LSFG
    setenv_("RECOMP_LSFG", "1");
    if (!strcmp(c, "fgnolock"))
        setenv_("RECOMP_FRAME_LOCK", "0");
#endif
    if (!strcmp(c, "capwins"))
        setenv_("RECOMP_GPU_FPS_CAP", "60");
    if (!strcmp(c, "pal"))
        s_vblank_hz = 50;
    nv2a_pb_exec_method(0, 0x0208, 0x0008);
    nv2a_pb_exec_method(0, 0x0200, 16u << 16);
    nv2a_pb_exec_method(0, 0x0204, 16u << 16);
    nv2a_pb_exec_method(0, 0x020C, 64);
    nv2a_pb_exec_method(0, 0x0210, 0x8000);
    if (strcmp(c, "cpu") != 0)
        nv2a_backend_register(&s_backend);

    if (!strcmp(c, "fast")) {
        period = run(10 * MS, IV_ONE, 10, SEC / 60 - 10 * MS);
        CHECK(period > 16.4 && period < 16.9);
    } else if (!strcmp(c, "slow") || !strcmp(c, "cpu") || !strcmp(c, "default")) {
        (void)run(20 * MS, IV_IMMEDIATE, 1.6, UINT64_MAX);      /* down within ~1.5 s */
        period = run(20 * MS, IV_IMMEDIATE, 5, SEC / 30 - 20 * MS);
        CHECK(period > 33.0 && period < 33.7);
    } else if (!strcmp(c, "slower")) {
        period = run(40 * MS, IV_IMMEDIATE, 10, SEC / 20 - 40 * MS);
        CHECK(period > 49.7 && period < 50.3);
    } else if (!strcmp(c, "locked")) {
        period = run(9 * MS, IV_ONE, 30, SEC / 60 - 9 * MS);
        CHECK(period > 16.4 && period < 16.9);
        period = run(18 * MS, IV_ONE, 10, 0);                   /* at its ask, never held */
        CHECK(period > 17.9 && period < 18.1);
    } else if (!strcmp(c, "capwins")) {
        period = run(20 * MS, IV_IMMEDIATE, 10, 0);
        CHECK(period > 19.9 && period < 20.1);
    } else if (!strcmp(c, "align") || !strcmp(c, "alignoff")) {
        /* 60 Hz grid; the schedule starts at the first flip (20 ms in), the
         * grid's edge 7.3 ms later: the presents would sit at 0.37 of a
         * refresh from it, never at the target 0.5. */
        uint64_t R = SEC / 60, worst = 0, ph;
        int i;
        if (!strcmp(c, "alignoff"))
            setenv_("RECOMP_PACING_ALIGN", "0");
        s_disp_period = R;
        s_disp_phase = 20 * MS + 7300000ull;
        (void)run(20 * MS, IV_IMMEDIATE, 3, UINT64_MAX);
        for (i = 0; i < 60; i++) {
            int64_t e;
            (void)frame(20 * MS, IV_IMMEDIATE);
            ph = (s_last_flip + 1 * MS - T0 - s_disp_phase) % R;
            e = (int64_t)ph - (int64_t)(R / 2);
            if ((uint64_t)(e < 0 ? -e : e) > worst)
                worst = (uint64_t)(e < 0 ? -e : e);
        }
        period = run(20 * MS, IV_IMMEDIATE, 2, UINT64_MAX);
        printf("NV2A pacing %s: presents' worst distance to half a refresh %.2f ms\n", c, worst / 1e6);
        CHECK(period > 33.0 && period < 33.7);
        if (!strcmp(c, "align"))
            CHECK(worst < 300000);
        else
            CHECK(worst > 1 * MS);
    } else if (!strcmp(c, "off")) {
        period = run(20 * MS, IV_IMMEDIATE, 5, 0);
        CHECK(period > 19.9 && period < 20.1);
#ifdef XBOXRECOMP_LSFG
    } else if (!strcmp(c, "fglocked")) {
        int i;
        for (i = 0; i < 24; i++) {                              /* before the verdict: never lowered */
            period = run(9 * MS, IV_ONE, 1, UINT64_MAX);
            CHECK(period > 16.4 && period < 16.9);
        }
        period = run(9 * MS, IV_ONE, 10, SEC / 60 - 9 * MS);    /* after it: still 60 */
        CHECK(period > 16.4 && period < 16.9);
        CHECK(s_fg_stops == 0 && s_fg_releases == 0);
        period = run(9 * MS, 2u, 5, SEC / 30 - 9 * MS);         /* asks 30: the cap divides nothing */
        CHECK(period > 33.0 && period < 33.7);
        CHECK(s_fg_stops == 0 && s_fg_releases == 1);
    } else if (!strcmp(c, "fgfree")) {
        int i;
        for (i = 0; i < 48; i++) {                              /* at its ask, no generation, no cap */
            period = run(20 * MS, IV_ONE, 1, i ? 0 : UINT64_MAX); /* the first slot: a sleep */
            CHECK(period > 19.9 && period < 20.1);
        }
        CHECK(s_fg_releases == 0);
        period = run(20 * MS, IV_ONE, 10, SEC / 30 - 20 * MS);  /* judged free: 30, generation on */
        CHECK(period > 33.0 && period < 33.7);
        CHECK(s_fg_stops == 0 && s_fg_releases == 1);
    } else if (!strcmp(c, "fgask30")) {
        period = run(10 * MS, 2u, 20, SEC / 30 - 10 * MS);
        CHECK(period > 33.0 && period < 33.7);
        CHECK(s_fg_releases == 0);
        period = run(10 * MS, 2u, 10, SEC / 30 - 10 * MS);
        CHECK(period > 33.0 && period < 33.7);
        CHECK(s_fg_stops == 0 && s_fg_releases == 1);
    } else if (!strcmp(c, "fgoff") || !strcmp(c, "fgnolock")) {
        period = run(10 * MS, IV_ONE, 5, SEC / 30 - 10 * MS);   /* lowered from the first frame */
        CHECK(period > 33.0 && period < 33.7);
        period = run(10 * MS, IV_ONE, 35, UINT64_MAX);
        if (!strcmp(c, "fgoff")) {                              /* XBA-96: 600 + 180 flips at 30 */
            CHECK(period > 16.4 && period < 16.9);
            CHECK(s_fg_stops == 1);
        } else {
            CHECK(period > 33.0 && period < 33.7);
            CHECK(s_fg_stops == 0);
        }
        CHECK(s_fg_releases == 0);
#endif
    } else if (!strcmp(c, "pal")) {
        period = run(10 * MS, IV_ONE, 10, SEC / 50 - 10 * MS);
        CHECK(period > 19.7 && period < 20.3);
    } else {
        fprintf(stderr, "unknown case %s\n", c);
        return 2;
    }
    printf("NV2A pacing %s: %.2f ms per frame%s\n", c, period, s_fail ? " FAILED" : "");
    return s_fail;
}
