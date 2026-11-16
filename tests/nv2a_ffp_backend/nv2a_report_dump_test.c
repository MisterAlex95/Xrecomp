/* The [GPU] report and the flip after it (report-kotor-sound WP-B).
 *
 * The report arms a dump of the next presented frame, and an armed flip asks
 * the back end for a synchronous present (guest memory, guest size). Armed
 * without RECOMP_FB_DUMP, that was one native-size frame per report: the
 * window's texture and frame generation rebuilt every 10 s. So:
 *   off: no RECOMP_FB_DUMP -- the flip after a report presents with sync 0;
 *   on:  RECOMP_FB_DUMP=<prefix> -- sync 1 and <prefix>00000.bmp written,
 *        and only for that one flip.
 * Usage: nv2a-report-dump-test off|on [prefix]
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nv2a_backend.h"

#define MEM_BYTES 0x10000u
#define FB_ADDR 0x8000u

static uint32_t s_memory[MEM_BYTES / sizeof(uint32_t)];
static uint8_t *const s_mem = (uint8_t *)s_memory;
static int s_fail;

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
void xbox_Nv2aFrameCounterFlip(void) {}
int nv2a_pb_scan_in_call(void) { return 0; }
uint32_t xbox_VblankCount(void) { return 0; }
uint64_t xbox_HostNowNs(void) { return 0; }
void xbox_HostSleepNs(uint64_t ns) { (void)ns; }
uint32_t xbox_Nv2aFrameCountersOwned(void) { return 0; }
volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset) { (void)offset; return NULL; }
void xbox_FramebufferWindowPresent(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }

/* Host present, like the GL back end at a scale above 1: its own pixels
 * unless the flip is synchronous. */
static uint32_t s_host_frame[16];
static unsigned s_host_count;
int xbox_FramebufferWindowPresentHost(const void *px, uint32_t w, uint32_t h, uint32_t pitch,
                                      uint32_t format)
{
    (void)px; (void)w; (void)h; (void)pitch; (void)format;
    s_host_count++;
    return 1;
}

static int s_hook_sync = -1;
static unsigned s_hook_calls;
static int present_hook(const Nv2aPresentSurface *now, int sync, Nv2aPresentSurface *ready)
{
    s_hook_calls++;
    s_hook_sync = sync;
    *ready = *now;
    if (!sync) {
        ready->host_px = s_host_frame;
        ready->host_w = 4;
        ready->host_h = 4;
        ready->host_pitch = 16;
        ready->host_format = NV2A_HOST_FORMAT_BGRX8888;
    }
    return 1;
}
static void flip_backend(void) {}
static const Nv2aBackend s_backend = {NULL, NULL, flip_backend};

extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
extern void nv2a_pb_exec_report(void);
static void method(uint32_t number, uint32_t value) { nv2a_pb_exec_method(0, number, value); }

/* One flip; the sync its present was asked for. */
static int flip(void)
{
    s_hook_sync = -1;
    method(0x0210, FB_ADDR);
    method(0x012C, 0);                   /* FLIP_INCREMENT_WRITE: a flip, counted */
    method(0x0130, 0);                   /* FLIP_STALL: presented */
    return s_hook_sync;
}

static int file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

int main(int argc, char **argv)
{
    const char *prefix = getenv("RECOMP_FB_DUMP");
    int on = argc >= 2 && strcmp(argv[1], "on") == 0;
    char path[512];

    if (argc < 2 || (on && (!prefix || argc < 3 || strcmp(prefix, argv[2]) != 0))
        || (!on && prefix) || getenv("RECOMP_FB_DUMP_FLIPS")) {
        fprintf(stderr, "usage: off (no RECOMP_FB_DUMP) | on <prefix> (RECOMP_FB_DUMP=<prefix>),"
                        " without RECOMP_FB_DUMP_FLIPS\n");
        return 2;
    }
    if (on) {
        snprintf(path, sizeof path, "%s00000.bmp", prefix);
        remove(path);
    }

    method(0x0208, 0x0008);              /* A8R8G8B8 */
    method(0x0200, 4u << 16);            /* clip 4x4 */
    method(0x0204, 4u << 16);
    method(0x020C, 16);                  /* pitch */
    method(0x0210, FB_ADDR);
    nv2a_backend_register_with_present(&s_backend, present_hook);

    CHECK(flip() == 0);                  /* a plain flip: host present */
    CHECK(s_host_count == 1);

    nv2a_pb_exec_report();
    if (on) {
        CHECK(flip() == 1);              /* the report's picture: synchronous */
        CHECK(s_host_count == 1);        /* guest memory to the window */
        CHECK(file_exists(path));
        remove(path);
    } else {
        CHECK(flip() == 0);              /* nothing to dump: nothing forced */
        CHECK(s_host_count == 2);
    }
    CHECK(flip() == 0);                  /* one flip per report, at most */

    /* A second report, same answer. */
    nv2a_pb_exec_report();
    CHECK(flip() == on);
    if (on) {
        snprintf(path, sizeof path, "%s00001.bmp", prefix);
        CHECK(file_exists(path));
        remove(path);
    }
    CHECK(s_hook_calls == 4);

    if (!s_fail)
        printf("NV2A report dump (%s): ok\n", on ? "RECOMP_FB_DUMP" : "no RECOMP_FB_DUMP");
    return s_fail;
}
