/* RECOMP_GPU_FPS_CAP (flip_cap_book / flip_cap_wait in nv2a_pb_exec.c) on a
 * fake host clock.
 *
 *   - frames faster than the period are held to an absolute schedule 1/cap
 *     apart (the sleep after a 2 ms frame is period - 2 ms);
 *   - a frame that took longer than a period is not held at all: no sleep
 *     before the next walk, and the schedule restarts at that flip. It used
 *     to sleep a whole period after every slow frame, which added 16.7 ms
 *     to each ~125 ms in-world Fable frame;
 *   - a fast frame after a slow one is held again, from the restarted
 *     schedule. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nv2a_backend.h"

#define MEM_BYTES 0x10000u
static uint8_t s_mem[MEM_BYTES];
static int s_fail;
static uint64_t s_now = 1000000000ull;     /* fake host clock, ns */
static uint64_t s_slept, s_sleeps;

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
void xbox_FramebufferWindowPresent(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_Nv2aFrameCounterFlip(void) {}
int nv2a_pb_scan_in_call(void) { return 0; }
uint32_t xbox_VblankCount(void) { return 0; }
uint64_t xbox_HostNowNs(void) { return s_now; }
void xbox_HostSleepNs(uint64_t ns) { s_now += ns; s_slept += ns; s_sleeps++; }
uint32_t xbox_Nv2aFrameCountersOwned(void) { return 0; }
volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset) { (void)offset; return NULL; }

extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
extern int nv2a_pb_exec_flip_poll(void);

static void fake_draw(const Nv2aSurface *s, const Nv2aBatch *b) { (void)s; (void)b; }
static void fake_flip(void) {}
static const Nv2aBackend s_backend = {NULL, fake_draw, fake_flip};

#define PERIOD (1000000000ull / 60u)
#define MS 1000000ull

/* A frame of `work` ns, its flip, then the walk that follows: returns what
 * the walk slept. */
static uint64_t frame(uint64_t work)
{
    uint64_t before;

    s_now += work;
    nv2a_pb_exec_method(0, 0x0130, 0);     /* FLIP_STALL books the slot */
    before = s_slept;
    (void)nv2a_pb_exec_flip_poll();         /* the next walk waits it out */
    return s_slept - before;
}

static int near(uint64_t a, uint64_t b) { return a + 1000 >= b && b + 1000 >= a; }

int main(void)
{
    uint64_t t;
    int i;

#ifdef _WIN32
    _putenv("RECOMP_GPU_FPS_CAP=60");
#else
    setenv("RECOMP_GPU_FPS_CAP", "60", 1);
#endif
    nv2a_pb_exec_method(0, 0x0208, 0x0008);
    nv2a_pb_exec_method(0, 0x0200, 16u << 16);
    nv2a_pb_exec_method(0, 0x0204, 16u << 16);
    nv2a_pb_exec_method(0, 0x020C, 64);
    nv2a_pb_exec_method(0, 0x0210, 0x8000);
    nv2a_backend_register(&s_backend);

    /* Fast frames: held to the schedule. */
    (void)frame(2 * MS);                    /* starts the schedule */
    for (i = 0; i < 5; i++) {
        t = frame(2 * MS);
        CHECK(near(t, PERIOD - 2 * MS));
    }
    /* A frame that runs into its slot but finishes inside the period. */
    t = frame(10 * MS);
    CHECK(near(t, PERIOD - 10 * MS));

    /* Slow frames: never held. */
    for (i = 0; i < 5; i++) {
        t = frame(125 * MS);
        if (t) {
            fprintf(stderr, "FAIL slow frame %d slept %.2f ms\n", i, t / 1e6);
            s_fail = 1;
        }
    }
    /* Fast again: held, counted from the last (late) flip. */
    t = frame(2 * MS);
    CHECK(near(t, PERIOD - 2 * MS));
    t = frame(3 * MS);
    CHECK(near(t, PERIOD - 3 * MS));

    if (!s_fail)
        puts("NV2A GPU frame cap: ok");
    return s_fail;
}
