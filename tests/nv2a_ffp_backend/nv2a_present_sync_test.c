#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nv2a_backend.h"

#define MEM_BYTES 0x10000u
#define FB_ADDR 0x8000u
#define OLD_PIXEL 0xFF123456u
#define NEW_PIXEL 0xFFABCDEFu

static uint32_t s_memory[MEM_BYTES / sizeof(uint32_t)];
static uint8_t *const s_mem = (uint8_t *)s_memory;
static int s_fail;
static unsigned s_events[8], s_event_count, s_present_count, s_draw_count;
static uint32_t s_presented[4], s_presented_va[4];
static int s_flip_writes, s_flip_presents;

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); s_fail = 1; \
} } while (0)

enum { EVENT_SYNC = 1, EVENT_PRESENT, EVENT_FLIP, EVENT_HOST };

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

static void event(unsigned value)
{
    CHECK(s_event_count < sizeof s_events / sizeof s_events[0]);
    if (s_event_count < sizeof s_events / sizeof s_events[0])
        s_events[s_event_count++] = value;
}

void xbox_FramebufferWindowPresent(uint32_t va, uint32_t pitch)
{
    (void)pitch;
    event(EVENT_PRESENT);
    CHECK(s_present_count < sizeof s_presented / sizeof s_presented[0]);
    CHECK(va <= MEM_BYTES - sizeof(uint32_t));
    if (s_present_count < sizeof s_presented / sizeof s_presented[0] &&
        va <= MEM_BYTES - sizeof(uint32_t)) {
        s_presented_va[s_present_count] = va;
        memcpy(&s_presented[s_present_count], s_mem + va, sizeof(uint32_t));
        s_present_count++;
    }
}

/* The window's entry point for a back end's own pixels (host_px). */
static const void *s_host_px;
static uint32_t s_host_w, s_host_h, s_host_pitch, s_host_format, s_host_first;
static unsigned s_host_count;
int xbox_FramebufferWindowPresentHost(const void *px, uint32_t w, uint32_t h, uint32_t pitch,
                                      uint32_t format)
{
    event(EVENT_HOST);
    s_host_count++;
    s_host_px = px;
    s_host_w = w;
    s_host_h = h;
    s_host_pitch = pitch;
    s_host_format = format;
    memcpy(&s_host_first, px, sizeof s_host_first);
    return 1;
}

static void write_pixel(uint32_t pixel) { memcpy(s_mem + FB_ADDR, &pixel, sizeof pixel); }
static void sync_surface(void) { event(EVENT_SYNC); write_pixel(NEW_PIXEL); }
/* The present hook (nv2a_backend_register_with_present). */
static int s_hook_sync = -1, s_hook_lie;
static Nv2aPresentSurface s_hook_now;
static int present_hook(const Nv2aPresentSurface *now, int sync, Nv2aPresentSurface *ready)
{
    event(EVENT_SYNC);
    s_hook_now = *now;
    s_hook_sync = sync;
    write_pixel(NEW_PIXEL);
    *ready = *now;
    if (s_hook_lie)
        ready->va = FB_ADDR + 0x100;   /* an earlier frame, not allowed with sync */
    return 1;
}
/* A present hook handing the window a host frame (a scaled read-back) when
 * no dump needs guest memory, and guest memory otherwise. s_host_lie: hand
 * the host frame even to a synchronous flip (not allowed). */
#define HOST_W 8u
#define HOST_H 6u
static uint32_t s_host_frame[HOST_W * HOST_H];
static int s_host_lie;
static int present_hook_host(const Nv2aPresentSurface *now, int sync, Nv2aPresentSurface *ready)
{
    event(EVENT_SYNC);
    s_hook_now = *now;
    s_hook_sync = sync;
    *ready = *now;
    if (sync && !s_host_lie) {
        write_pixel(NEW_PIXEL);
        return 1;
    }
    ready->host_px = s_host_frame;
    ready->host_w = HOST_W;
    ready->host_h = HOST_H;
    ready->host_pitch = HOST_W * 4u;
    ready->host_format = NV2A_HOST_FORMAT_BGRX8888;
    return 1;
}
static void draw_surface(const Nv2aSurface *s, const Nv2aBatch *batch)
{
    (void)batch;
    CHECK(s->color_va == FB_ADDR);
    s_draw_count++;
}
static void flip_backend(void)
{
    event(EVENT_FLIP);
    if (s_flip_writes)
        write_pixel(NEW_PIXEL);
    if (s_flip_presents)
        xbox_FramebufferWindowPresent(FB_ADDR, 4);
}
static const Nv2aBackend s_backend = {NULL, draw_surface, flip_backend};

extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
static void method(uint32_t number, uint32_t value) { nv2a_pb_exec_method(0, number, value); }
static void write_float(uint32_t address, float value) { memcpy(s_mem + address, &value, 4); }

static void reset_events(void)
{
    s_host_count = 0;
    s_host_px = NULL;
    memset(s_events, 0, sizeof s_events);
    memset(s_presented, 0, sizeof s_presented);
    memset(s_presented_va, 0, sizeof s_presented_va);
    s_event_count = s_present_count = 0;
    s_flip_writes = s_flip_presents = 0;
}

static void dump_path(char *path, size_t size, const char *prefix, unsigned sequence)
{
    int n = snprintf(path, size, "%s%05u.bmp", prefix, sequence);
    if (n < 0 || (size_t)n >= size) {
        fprintf(stderr, "framebuffer dump path is too long\n");
        exit(1);
    }
}

static void check_dump(const char *prefix, unsigned sequence, uint32_t expected)
{
    char path[512];
    uint8_t bgr[3];
    dump_path(path, sizeof path, prefix, sequence);
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    if (!f)
        return;
    CHECK(fseek(f, 54, SEEK_SET) == 0);
    if (fread(bgr, 1, sizeof bgr, f) != sizeof bgr) {
        fprintf(stderr, "cannot read pixel from %s\n", path);
        s_fail = 1;
    } else {
        CHECK(bgr[0] == (expected & 0xFFu));
        CHECK(bgr[1] == ((expected >> 8) & 0xFFu));
        CHECK(bgr[2] == ((expected >> 16) & 0xFFu));
    }
    CHECK(fclose(f) == 0);
    CHECK(remove(path) == 0);
}

static void flip_and_check_dump(const char *prefix, unsigned sequence, uint32_t expected)
{
    char path[512];
    dump_path(path, sizeof path, prefix, sequence);
    FILE *old = fopen(path, "rb");
    if (old) {
        fclose(old);
        fprintf(stderr, "refusing to overwrite an existing dump: %s\n", path);
        exit(1);
    }
    method(0x0130, 0);
    check_dump(prefix, sequence, expected);
}

static void draw_to_first_surface(void)
{
    static const float pos[3][3] = {{0, 0, 0}, {16, 0, 0}, {0, 16, 0}};
    for (unsigned i = 0; i < 3; i++)
        for (unsigned j = 0; j < 3; j++)
            write_float(0x2000 + i * 12u + j * 4u, pos[i][j]);
    method(0x1720, 0x2000);
    method(0x1760, 2 | (3 << 4) | (12 << 8));
    method(0x17FC, 5);
    method(0x1810, 2u << 24);
    method(0x17FC, 0);
    CHECK(s_draw_count == 1);
    method(0x0210, 0x9000);
}

int main(int argc, char **argv)
{
    /* RECOMP_FB_DUMP_FLIPS names a file: frames are dumped while it exists,
     * so the host-frame case below can flip without a dump. */
    const char *dumping = getenv("RECOMP_FB_DUMP_FLIPS");
    if (argc != 2 || !dumping || strcmp(dumping, "1") == 0) {
        fprintf(stderr, "expected the framebuffer dump prefix and RECOMP_FB_DUMP_FLIPS=<file>\n");
        return 2;
    }
    {
        FILE *f = fopen(dumping, "wb");
        CHECK(f != NULL);
        if (f)
            fclose(f);
    }
    method(0x0208, 0x0008);
    method(0x0200, 1u << 16);
    method(0x0204, 1u << 16);
    method(0x020C, 4);
    method(0x0210, FB_ADDR);

    reset_events();
    write_pixel(OLD_PIXEL);
    flip_and_check_dump(argv[1], 0, OLD_PIXEL);
    CHECK(s_event_count == 1 && s_events[0] == EVENT_PRESENT);
    CHECK(s_present_count == 1 && s_presented[0] == OLD_PIXEL);

    nv2a_backend_register(&s_backend);
    reset_events();
    write_pixel(OLD_PIXEL);
    s_flip_writes = s_flip_presents = 1;
    flip_and_check_dump(argv[1], 1, OLD_PIXEL);
    CHECK(s_event_count == 3 && s_events[0] == EVENT_PRESENT &&
          s_events[1] == EVENT_FLIP && s_events[2] == EVENT_PRESENT);
    CHECK(s_present_count == 2 && s_presented[0] == OLD_PIXEL && s_presented[1] == NEW_PIXEL);

    nv2a_backend_register_with_present_sync(&s_backend, sync_surface);
    reset_events();
    write_pixel(OLD_PIXEL);
    draw_to_first_surface();
    flip_and_check_dump(argv[1], 2, NEW_PIXEL);
    CHECK(s_event_count == 3 && s_events[0] == EVENT_SYNC &&
          s_events[1] == EVENT_PRESENT && s_events[2] == EVENT_FLIP);
    CHECK(s_present_count == 1 && s_presented_va[0] == FB_ADDR && s_presented[0] == NEW_PIXEL);

    nv2a_backend_register_with_present_sync(&s_backend, NULL);
    reset_events();
    write_pixel(OLD_PIXEL);
    flip_and_check_dump(argv[1], 3, OLD_PIXEL);
    CHECK(s_event_count == 2 && s_events[0] == EVENT_PRESENT && s_events[1] == EVENT_FLIP);
    CHECK(s_present_count == 1 && s_presented[0] == OLD_PIXEL);

    nv2a_backend_register_with_present_sync(&s_backend, sync_surface);
    nv2a_backend_register(&s_backend);
    reset_events();
    write_pixel(OLD_PIXEL);
    flip_and_check_dump(argv[1], 4, OLD_PIXEL);
    CHECK(s_event_count == 2 && s_events[0] == EVENT_PRESENT && s_events[1] == EVENT_FLIP);
    CHECK(s_present_count == 1 && s_presented[0] == OLD_PIXEL);

    nv2a_backend_register_with_present_sync(NULL, sync_surface);
    reset_events();
    write_pixel(OLD_PIXEL);
    flip_and_check_dump(argv[1], 5, OLD_PIXEL);
    CHECK(s_event_count == 1 && s_events[0] == EVENT_PRESENT);
    CHECK(s_present_count == 1 && s_presented[0] == OLD_PIXEL);

    nv2a_backend_register_with_present_sync(&s_backend, sync_surface);
    nv2a_backend_register(NULL);
    reset_events();
    write_pixel(OLD_PIXEL);
    flip_and_check_dump(argv[1], 6, OLD_PIXEL);
    CHECK(s_event_count == 1 && s_events[0] == EVENT_PRESENT);
    CHECK(s_present_count == 1 && s_presented[0] == OLD_PIXEL);

    /* The present hook: called with the presented surface, synchronous
     * while frames are dumped (RECOMP_FB_DUMP_FLIPS=1 here), and the window
     * shows the surface it hands back -- `now` whenever sync is set. */
    nv2a_backend_register_with_present(&s_backend, present_hook);
    reset_events();
    write_pixel(OLD_PIXEL);
    method(0x0210, FB_ADDR);
    s_draw_count = 0;
    draw_to_first_surface();
    flip_and_check_dump(argv[1], 7, NEW_PIXEL);
    CHECK(s_event_count == 3 && s_events[0] == EVENT_SYNC &&
          s_events[1] == EVENT_PRESENT && s_events[2] == EVENT_FLIP);
    CHECK(s_hook_sync == 1);
    CHECK(s_hook_now.va == FB_ADDR && s_hook_now.pitch == 4 && s_hook_now.bpp == 4);
    CHECK(s_present_count == 1 && s_presented_va[0] == FB_ADDR && s_presented[0] == NEW_PIXEL);

    reset_events();
    write_pixel(OLD_PIXEL);
    method(0x0210, FB_ADDR);
    s_draw_count = 0;
    draw_to_first_surface();
    s_hook_lie = 1;
    flip_and_check_dump(argv[1], 8, NEW_PIXEL);
    s_hook_lie = 0;
    CHECK(s_present_count == 1 && s_presented_va[0] == FB_ADDR);

    /* A later plain registration drops the hook. */
    nv2a_backend_register(&s_backend);
    reset_events();
    write_pixel(OLD_PIXEL);
    flip_and_check_dump(argv[1], 9, OLD_PIXEL);
    CHECK(s_event_count == 2 && s_events[0] == EVENT_PRESENT && s_events[1] == EVENT_FLIP);

    /* A host frame: without a dump the window gets the back end's pixels
     * through xbox_FramebufferWindowPresentHost, at their own size, and
     * never reads guest memory. */
    for (unsigned i = 0; i < HOST_W * HOST_H; i++)
        s_host_frame[i] = 0xFF000000u | i;
    nv2a_backend_register_with_present(&s_backend, present_hook_host);
    CHECK(remove(dumping) == 0);
    reset_events();
    write_pixel(OLD_PIXEL);
    method(0x0210, FB_ADDR);       /* no batch: one would be dumped after its draw */
    method(0x0130, 0);
    CHECK(s_hook_sync == 0);
    CHECK(s_event_count == 3 && s_events[0] == EVENT_SYNC &&
          s_events[1] == EVENT_HOST && s_events[2] == EVENT_FLIP);
    CHECK(s_present_count == 0 && s_host_count == 1);
    CHECK(s_host_px == s_host_frame && s_host_w == HOST_W && s_host_h == HOST_H &&
          s_host_pitch == HOST_W * 4u && s_host_format == NV2A_HOST_FORMAT_BGRX8888);
    CHECK(s_host_first == 0xFF000000u);
    {
        uint32_t px;
        memcpy(&px, s_mem + FB_ADDR, sizeof px);
        CHECK(px == OLD_PIXEL);                /* guest memory untouched */
    }

    /* A dumping flip reads guest memory: the hook answers with it, and a
     * host frame handed to a synchronous flip anyway is ignored. (No
     * batches: the flip that removed the file also re-armed the dumps
     * taken after a draw.) */
    {
        FILE *f = fopen(dumping, "wb");
        CHECK(f != NULL);
        if (f)
            fclose(f);
    }
    reset_events();
    write_pixel(OLD_PIXEL);
    method(0x0210, FB_ADDR);
    flip_and_check_dump(argv[1], 10, NEW_PIXEL);
    CHECK(s_hook_sync == 1);
    CHECK(s_host_count == 0 && s_present_count == 1 && s_presented[0] == NEW_PIXEL);

    reset_events();
    write_pixel(NEW_PIXEL);
    method(0x0210, FB_ADDR);
    s_host_lie = 1;
    flip_and_check_dump(argv[1], 11, NEW_PIXEL);
    s_host_lie = 0;
    CHECK(s_host_count == 0 && s_present_count == 1 && s_presented_va[0] == FB_ADDR);
    remove(dumping);

    if (!s_fail)
        puts("NV2A present synchronization: ok");
    return s_fail;
}
