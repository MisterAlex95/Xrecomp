/* CLEAR_SURFACE through a render back end: the rectangle it clears.
 *
 * The CPU path clears SET_CLEAR_RECT_HORIZONTAL / _VERTICAL on a swizzled
 * surface (SSX 3 clears its 128x128 shadow maps whole and draws them through
 * a 124x124 clip; sampled clamped, the ring the clip leaves must hold the
 * clear colour) and the clip rectangle on a linear one. A back end clears the
 * rectangle it is handed in Nv2aSurface (clip_x/clip_y/width/height), so the
 * executor must hand it the clear rectangle for a swizzled surface (XBA-119:
 * the GL back end cleared the 124x124 clip, leaving the ring dark):
 *
 *   - swizzled, no clear rectangle sent yet: the clip;
 *   - swizzled with one: the clear rectangle, clamped to the surface;
 *   - linear: the clip, whatever the clear rectangle says. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_backend.h"

#define MEM_BYTES 0x40000u
#define FB_ADDR   0x10000u
static uint8_t s_mem[MEM_BYTES];
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
void xbox_FramebufferWindowPresent(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
int xbox_FramebufferWindowPresentHost(const void *px, uint32_t w, uint32_t h, uint32_t pitch,
                                      uint32_t format)
{ (void)px; (void)w; (void)h; (void)pitch; (void)format; return 0; }
void xbox_Nv2aFrameCounterFlip(void) {}
int nv2a_pb_scan_in_call(void) { return 0; }
uint32_t xbox_VblankCount(void) { return 0; }
uint64_t xbox_HostNowNs(void) { return 0; }
void xbox_HostSleepNs(uint64_t ns) { (void)ns; }
uint32_t xbox_Nv2aFrameCountersOwned(void) { return 0; }
volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset) { (void)offset; return NULL; }

extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
static void m(uint32_t method, uint32_t param) { nv2a_pb_exec_method(0, method, param); }

static Nv2aSurface s_last;
static uint32_t s_clears, s_flags;
static void fake_clear(const Nv2aSurface *s, const Nv2aRenderState *rs, uint32_t flags,
                       uint32_t argb, uint32_t zs)
{
    (void)rs; (void)argb; (void)zs;
    s_last = *s;
    s_flags = flags;
    s_clears++;
}
static void fake_draw(const Nv2aSurface *s, const Nv2aBatch *b) { (void)s; (void)b; }
static void fake_flip(void) {}
static Nv2aBackend s_backend = { fake_clear, fake_draw, fake_flip, 0, NULL, NULL, NULL, NULL, NULL };

/* 128x128 swizzled A8R8G8B8 (or linear, pitch 512), clip 2,2 124x124. */
static void surface(int swizzled)
{
    m(0x0208, swizzled ? 0x0008u | 2u << 8 | 7u << 16 | 7u << 24 : 0x0108u);
    m(0x0200, 2u | 124u << 16);
    m(0x0204, 2u | 124u << 16);
    m(0x020C, 128 * 4);
    m(0x0210, FB_ADDR);
}

static void clear(void)
{
    m(0x1D90, 0xFFFFFFFFu);             /* SET_COLOR_CLEAR_VALUE */
    m(0x1D94, 0xF0);                    /* CLEAR_SURFACE, colour */
}

int main(void)
{
    nv2a_backend_register(&s_backend);

    surface(1);
    clear();
    CHECK(s_clears == 1 && s_flags == 0xF0);
    CHECK(s_last.clip_x == 2 && s_last.clip_y == 2 && s_last.width == 124
          && s_last.height == 124);

    m(0x1D98, 0u | 127u << 16);         /* SET_CLEAR_RECT_HORIZONTAL: 0..127 */
    m(0x1D9C, 0u | 127u << 16);         /* _VERTICAL */
    clear();
    CHECK(s_clears == 2);
    CHECK(s_last.clip_x == 0 && s_last.clip_y == 0 && s_last.width == 128
          && s_last.height == 128);

    m(0x1D98, 16u | 300u << 16);        /* past the surface: clamped */
    m(0x1D9C, 8u | 23u << 16);
    clear();
    CHECK(s_last.clip_x == 16 && s_last.width == 112 && s_last.clip_y == 8
          && s_last.height == 16);

    surface(0);                         /* linear: the clip */
    clear();
    CHECK(s_clears == 4);
    CHECK(s_last.clip_x == 2 && s_last.clip_y == 2 && s_last.width == 124
          && s_last.height == 124);

    if (!s_fail)
        puts("NV2A CLEAR_SURFACE rectangle through a back end: ok");
    return s_fail;
}
