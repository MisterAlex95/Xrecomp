/* Inline fixed-function batches (RECOMP_FFP_INLINE, nv2a_pb_exec.c).
 *
 * INLINE_ARRAY and SET_VERTEX4F batches -- D3D Begin/End -- under the
 * fixed-function unit go through the composite matrix like any other batch.
 * KOTOR draws its GUI exactly so: unit quads under 640 0 0 0 / 0 -480 0 480,
 * which the old screen-space guess skipped as model units.
 *
 *   - mode 1 (default): the unit quad covers the 640x480 surface, from
 *     INLINE_ARRAY and from SET_VERTEX4F alike; a quad in pixels is
 *     transformed too (here by a half-scale matrix);
 *   - mode 0 (RECOMP_FFP_INLINE=0): the old reading -- the unit quad is
 *     skipped and the pixel quad drawn as it stands;
 *   - mode 2: only the batches the guess would skip are transformed, so the
 *     unit quad is drawn full-surface and the pixel quad as it stands.
 * Ordinary DRAW_ARRAYS batches are transformed in every mode. */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_backend.h"

#define MEM_BYTES 0x10000u
static uint8_t s_mem[MEM_BYTES];

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

/* ---- the fake back end: the last batch's screen extent ------------------ */
static struct {
    int draws;
    uint32_t count;
    float lo_x, hi_x, lo_y, hi_y;
    uint32_t diffuse;
} s_be;

static void be_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
                     uint32_t flags, uint32_t argb, uint32_t zstencil)
{ (void)s; (void)rs; (void)flags; (void)argb; (void)zstencil; }

static void be_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    uint32_t i;

    (void)s;
    s_be.draws++;
    s_be.count = b->count;
    s_be.lo_x = s_be.lo_y = 1e9f;
    s_be.hi_x = s_be.hi_y = -1e9f;
    for (i = 0; i < b->count; i++) {
        const Nv2aVertex *v = &b->vertices[i];
        if (v->x < s_be.lo_x) s_be.lo_x = v->x;
        if (v->x > s_be.hi_x) s_be.hi_x = v->x;
        if (v->y < s_be.lo_y) s_be.lo_y = v->y;
        if (v->y > s_be.hi_y) s_be.hi_y = v->y;
    }
    if (b->count)
        s_be.diffuse = b->vertices[0].diffuse;
}

static void be_flip(void) {}
static const Nv2aBackend s_backend = { be_clear, be_draw, be_flip };

/* ---- driving the executor ---------------------------------------------- */
extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
static void m(uint32_t method, uint32_t param) { nv2a_pb_exec_method(0, method, param); }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static void mf(uint32_t method, float f) { m(method, f2u(f)); }
static void putf(uint32_t at, float f) { memcpy(s_mem + at, &f, 4); }

static int s_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); s_fail = 1; } } while (0)
static int near_(float a, float b) { return fabsf(a - b) < 1e-3f; }

static void composite(const float M[16])
{
    uint32_t i;
    for (i = 0; i < 16; i++)
        mf(0x0680 + 4 * i, M[i]);
}

/* Four vertices of float3 position + D3DCOLOR diffuse as one INLINE_ARRAY
 * QUADS batch. */
static void inline_quad(const float p[4][2])
{
    int i;

    m(0x17FC, 8);                                       /* BEGIN_END: quads */
    for (i = 0; i < 4; i++) {
        mf(0x1818, p[i][0]);
        mf(0x1818, p[i][1]);
        mf(0x1818, 0.0f);
        m(0x1818, 0xFF336699u);
    }
    m(0x17FC, 0);
}

/* The same quad from SET_VERTEX4F (draw_immediate). */
static void immediate_quad(const float p[4][2])
{
    int i;

    m(0x17FC, 8);
    for (i = 0; i < 4; i++) {
        mf(0x1518, p[i][0]);
        mf(0x151C, p[i][1]);
        mf(0x1520, 0.0f);
        mf(0x1524, 1.0f);
    }
    m(0x17FC, 0);
}

int main(void)
{
    const char *e = getenv("RECOMP_FFP_INLINE");
    const int mode = e && *e ? atoi(e) : 1;
    static const float unit[4][2] = { {0, 0}, {1, 0}, {1, 1}, {0, 1} };
    static const float pixels[4][2] = { {0, 0}, {640, 0}, {640, 480}, {0, 480} };
    /* KOTOR's GUI matrix: x 0..1 -> 0..640, y 0..1 -> 480..0. */
    static const float gui[16] = { 640, 0, 0, 0,
                                   0, -480, 0, 480,
                                   0, 0, 1, 0,
                                   0, 0, 0, 1 };
    static const float half[16] = { 0.5f, 0, 0, 0,
                                    0, 0.5f, 0, 0,
                                    0, 0, 1, 0,
                                    0, 0, 0, 1 };
    int draws;

    nv2a_backend_register(&s_backend);
    m(0x0208, 0x0008);                       /* SET_SURFACE_FORMAT: A8R8G8B8, no AA */
    m(0x0200, 640u << 16);                   /* CLIP_HORIZONTAL */
    m(0x0204, 480u << 16);                   /* CLIP_VERTICAL */
    m(0x020C, 2560);                         /* SET_SURFACE_PITCH */
    m(0x0210, 0x8000);                       /* SET_SURFACE_COLOR_OFFSET */
    m(0x1E94, 0);                            /* TRANSFORM_EXECUTION_MODE: fixed */
    m(0x1760, 2 | (3 << 4) | (16 << 8));     /* attr 0: float3 */
    m(0x176C, 0 | (4 << 4) | (16 << 8));     /* attr 3: D3DCOLOR */
    composite(gui);

    /* The unit quad under the GUI matrix. */
    draws = s_be.draws;
    inline_quad(unit);
    if (mode == 0) {
        CHECK(s_be.draws == draws);          /* skipped as not screen-space */
    } else {
        CHECK(s_be.draws == draws + 1);
        CHECK(s_be.count == 6);              /* two triangles */
        CHECK(near_(s_be.lo_x, 0) && near_(s_be.hi_x, 640));
        CHECK(near_(s_be.lo_y, 0) && near_(s_be.hi_y, 480));
        CHECK(s_be.diffuse == 0xFF336699u);  /* the payload's colour */
    }

    /* The same quad from SET_VERTEX4F. */
    draws = s_be.draws;
    immediate_quad(unit);
    if (mode == 0) {
        CHECK(s_be.draws == draws);
    } else {
        CHECK(s_be.draws == draws + 1);
        CHECK(near_(s_be.lo_x, 0) && near_(s_be.hi_x, 640));
        CHECK(near_(s_be.lo_y, 0) && near_(s_be.hi_y, 480));
    }

    /* A quad already in pixels under a half-scale matrix: transformed only
     * in mode 1; modes 0 and 2 draw it where it stands. */
    composite(half);
    draws = s_be.draws;
    inline_quad(pixels);
    CHECK(s_be.draws == draws + 1);
    if (mode == 1) {
        CHECK(near_(s_be.hi_x, 320) && near_(s_be.hi_y, 240));
    } else {
        CHECK(near_(s_be.hi_x, 640) && near_(s_be.hi_y, 480));
    }

    /* An ordinary array batch is transformed whatever the mode. */
    putf(0x2000, 0); putf(0x2004, 0); putf(0x2008, 0);
    putf(0x200C, 1); putf(0x2010, 0); putf(0x2014, 0);
    putf(0x2018, 1); putf(0x201C, 1); putf(0x2020, 0);
    m(0x1720, 0x2000); m(0x1760, 2 | (3 << 4) | (12 << 8));
    m(0x176C, 0);                            /* attr 3 off */
    composite(gui);
    draws = s_be.draws;
    m(0x17FC, 5); m(0x1810, 2u << 24); m(0x17FC, 0);
    CHECK(s_be.draws == draws + 1);
    CHECK(near_(s_be.hi_x, 640) && near_(s_be.lo_y, 0) && near_(s_be.hi_y, 480));

    if (!s_fail)
        printf("inline fixed-function batches (RECOMP_FFP_INLINE=%d): ok\n", mode);
    return s_fail;
}
