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
    nv2a_pb_exec_method(0, 0x0100, (0x00123400u | interval) << 5 | 1u);   /* swap */
    nv2a_pb_exec_method(0, 0x0130, 0);                                      /* FLIP_STALL */
    before = s_slept;
    (void)nv2a_pb_exec_flip_poll();
    return s_slept - before;
}

static int near(uint64_t a, uint64_t b) { return a + 300000 >= b && b + 300000 >= a; }

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

    setenv_("RECOMP_PACING", "auto");
    setenv_("RECOMP_DISPLAY_HZ", "60");
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
    } else if (!strcmp(c, "slow") || !strcmp(c, "cpu")) {
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
