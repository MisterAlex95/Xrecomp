/* Fixed-function batches with their state resolved once per batch.
 *
 * ffp_batch_vertex (light table and attributes resolved per batch, each
 * unique index transformed once, the raster pool for big batches) must hand a
 * back end exactly what the per-vertex path (RECOMP_FFP_BATCH=0: lit_color,
 * ffp_vertex) does. A synthetic indexed mesh -- a 24 x 24 grid, every vertex
 * named up to six times, normals and colours varying -- is lit by four lights
 * (infinite, two local ones with range and attenuation, one of them out of
 * range for part of the grid, colour material on ambient and diffuse) under a
 * perspective composite matrix and a rotating model-view, and drawn three
 * ways: per vertex, per batch on the executor thread, per batch on the pool.
 * The full vertices the back end receives must be equal byte for byte. Then
 * the same with lighting off, with DRAW_ARRAYS (no repeated indices) and with
 * an index past guest memory (the whole batch dropped either way). */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_backend.h"

#define MEM_BYTES 0x100000u
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
static volatile uint32_t s_pgraph_400b10;
volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset)
{
    return offset == 0x400B10u ? &s_pgraph_400b10 : NULL;
}

/* ---- a back end that keeps every full vertex of a batch ---------------- */
#define CAP_VERTS 65536
static Nv2aVertexFull s_got[CAP_VERTS];
static Nv2aVertex s_gotv[CAP_VERTS];
static uint32_t s_ngot;

static void be_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
                     uint32_t flags, uint32_t argb, uint32_t zstencil)
{ (void)s; (void)rs; (void)flags; (void)argb; (void)zstencil; }
static void be_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    uint32_t i;

    (void)s;
    for (i = 0; i < b->count && s_ngot < CAP_VERTS; i++, s_ngot++) {
        s_gotv[s_ngot] = b->vertices[i];
        if (b->full)
            s_got[s_ngot] = b->full[i];
    }
}
static void be_flip(void) {}
static const Nv2aBackend s_backend = { be_clear, be_draw, be_flip, NV2A_BACKEND_CAP_PIXEL };

extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
extern void nv2a_ffp_batch_enable(int on);
extern void nv2a_vsh_pool_enable(int on);
extern void nv2a_ffp_pool_enable(int on);
extern int nv2a_vsh_last_workers(void);
static void m(uint32_t method, uint32_t param) { nv2a_pb_exec_method(0, method, param); }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static void mf(uint32_t method, float f) { m(method, f2u(f)); }
static void put32(uint32_t at, uint32_t v) { memcpy(s_mem + at, &v, 4); }
static void putf(uint32_t at, float f) { memcpy(s_mem + at, &f, 4); }

static int s_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); s_fail = 1; } } while (0)

#define G 24                                /* grid side */
#define POS 0x10000u
#define NRM 0x20000u
#define COL 0x30000u
#define TEX 0x40000u

static uint16_t s_idx[(G - 1) * (G - 1) * 6];
static uint32_t s_nidx;

/* One batch, drawn indexed (or DRAW_ARRAYS of `arrays` vertices). */
static void draw(uint32_t arrays)
{
    uint32_t i;

    m(0x17FC, 5);                                   /* BEGIN_END: triangles */
    if (arrays) {
        for (i = 0; i < arrays; i += 256) {
            uint32_t c = arrays - i < 256 ? arrays - i : 256;
            m(0x1810, i | (c - 1) << 24);           /* DRAW_ARRAYS */
        }
    } else {
        for (i = 0; i + 1 < s_nidx; i += 2)
            m(0x1800, s_idx[i] | (uint32_t)s_idx[i + 1] << 16);
    }
    m(0x17FC, 0);
}

/* Draws the batch per vertex, per batch serially and per batch on the pool;
 * 1 when all three hand over the same vertices. */
static int same_three_ways(uint32_t arrays, const char *what)
{
    static Nv2aVertexFull ref[CAP_VERTS];
    static Nv2aVertex refv[CAP_VERTS];
    uint32_t nref;
    int ok = 1, k;

    nv2a_ffp_batch_enable(0);
    nv2a_vsh_pool_enable(0);
    s_ngot = 0;
    draw(arrays);
    nref = s_ngot;
    memcpy(ref, s_got, nref * sizeof *ref);
    memcpy(refv, s_gotv, nref * sizeof *refv);

    for (k = 0; k < 2; k++) {
        nv2a_ffp_batch_enable(1);
        nv2a_vsh_pool_enable(k);
        nv2a_ffp_pool_enable(k);
        s_ngot = 0;
        draw(arrays);
        if (s_ngot != nref || memcmp(ref, s_got, nref * sizeof *ref)
            || memcmp(refv, s_gotv, nref * sizeof *refv)) {
            uint32_t i;
            for (i = 0; i < nref && i < s_ngot
                        && !memcmp(&ref[i], &s_got[i], sizeof *ref); i++)
                ;
            printf("FAIL %s, %s: %u vertices against %u, first difference at %u\n",
                   what, k ? "pool" : "serial", s_ngot, nref, i);
            ok = 0;
        }
    }
    printf("%s: %u vertices%s\n", what, nref, ok ? ", identical" : "");
    return ok;
}

int main(void)
{
    uint32_t i, x, y;

#ifdef _WIN32
    _putenv("RECOMP_RASTER_THREADS=4");
#else
    setenv("RECOMP_RASTER_THREADS", "4", 1);
#endif
    nv2a_backend_register(&s_backend);

    m(0x0208, 0x2008);                       /* A8R8G8B8, AA 2x2 */
    m(0x0200, 640u << 16);
    m(0x0204, 480u << 16);
    m(0x020C, 5120);
    m(0x0210, 0x8000);
    mf(0x0A20, 0.5f); mf(0x0A24, 0.5f); mf(0x0A28, 0.0f);   /* c[59]: viewport offset */

    /* Perspective composite: x, y scaled to the viewport, w = z + 3, so w
     * varies across the (tilted) grid. Model-view: a rotation about y and z
     * plus a translation, so eye space is not object space. */
    {
        const float M[16] = { 300, 0, 320, 960,
                              0, -220, 240, 720,
                              0, 0, 0.5f, 1.5f,
                              0, 0, 1, 3 };
        const float c = 0.8f, s = 0.6f;
        const float MV[16] = { c, -s * c, s * s, 0.25f,
                               s, c * c, -c * s, -0.5f,
                               0, s, c, 1.0f,
                               0, 0, 0, 1 };
        for (i = 0; i < 16; i++) {
            mf(0x0680 + 4 * i, M[i]);
            mf(0x0480 + 4 * i, MV[i]);
        }
    }
    m(0x1E94, 0);                            /* fixed function */

    for (y = 0; y < G; y++)
        for (x = 0; x < G; x++) {
            uint32_t v = y * G + x;
            float fx = (float)x / (G - 1) * 2 - 1, fy = (float)y / (G - 1) * 2 - 1;
            float nx = 0.3f * fx, ny = -0.4f * fy, nz = 1.0f;
            putf(POS + 12 * v, fx); putf(POS + 12 * v + 4, fy);
            putf(POS + 12 * v + 8, 0.3f * fx * fy);
            putf(NRM + 12 * v, nx); putf(NRM + 12 * v + 4, ny); putf(NRM + 12 * v + 8, nz);
            put32(COL + 4 * v, 0x80000000u | (x * 10) << 16 | (y * 10) << 8 | (x ^ y) * 7);
            putf(TEX + 8 * v, (float)x / (G - 1)); putf(TEX + 8 * v + 4, (float)y / (G - 1));
        }
    for (y = 0; y + 1 < G; y++)
        for (x = 0; x + 1 < G; x++) {
            uint16_t a = (uint16_t)(y * G + x), b = (uint16_t)(a + 1);
            uint16_t c = (uint16_t)(a + G), d = (uint16_t)(c + 1);
            s_idx[s_nidx++] = a; s_idx[s_nidx++] = b; s_idx[s_nidx++] = c;
            s_idx[s_nidx++] = b; s_idx[s_nidx++] = d; s_idx[s_nidx++] = c;
        }
    m(0x1720, POS); m(0x1760, 2 | (3 << 4) | (12 << 8));   /* attr 0: float3 */
    m(0x1728, NRM); m(0x1768, 2 | (3 << 4) | (12 << 8));   /* attr 2: float3 */
    m(0x172C, COL); m(0x176C, 0 | (4 << 4) | (4 << 8));    /* attr 3: D3DCOLOR */
    m(0x1744, TEX); m(0x1784, 2 | (2 << 4) | (8 << 8));    /* attr 9: float2 */

    /* Four lights: 0 infinite, 2 and 3 local (3 with a range that cuts the
     * grid), 5 infinite; 1 and 4 off. */
    m(0x0314, 1);
    m(0x03BC, 1 | 2u << 4 | 2u << 6 | 1u << 10);
    m(0x0298, 1u << 2 | 1u << 4);            /* colour material: ambient and diffuse */
    mf(0x0A10, 0.05f); mf(0x0A14, 0.04f); mf(0x0A18, 0.03f);
    mf(0x03B4, 0.75f);
    for (i = 0; i < 6; i++) {
        uint32_t b = 0x1000 + i * 0x80, k;
        for (k = 0; k < 3; k++) {
            mf(b + 4 * k, 0.02f * (float)(i + k + 1));          /* ambient */
            mf(b + 0x0C + 4 * k, 0.11f * (float)(i + 1) / (float)(k + 1));
        }
        mf(b + 0x34, 0.3f * (float)i - 0.5f); mf(b + 0x38, 0.6f); mf(b + 0x3C, 0.74f);
        mf(b + 0x5C, (float)i - 2.0f); mf(b + 0x60, 1.0f); mf(b + 0x64, 2.0f + (float)i);
        mf(b + 0x24, i == 3 ? 4.6f : 0.0f);                    /* range */
        mf(b + 0x68, 1.0f); mf(b + 0x6C, 0.1f * (float)i); mf(b + 0x70, 0.05f);
    }

    CHECK(same_three_ways(0, "lit, indexed"));
    CHECK(s_ngot > 1000);
    CHECK(nv2a_vsh_last_workers() > 1);      /* the last run was on the pool */
    {
        /* The lighting is not degenerate: colours vary and are not all clamped. */
        int mid = 0;
        for (i = 0; i < s_ngot; i++)
            mid += s_got[i].d0[0] > 0.0f && s_got[i].d0[0] < 1.0f
                && s_got[i].d0[1] > 0.0f && s_got[i].d0[1] < 1.0f
                && s_got[i].d0[0] != s_got[0].d0[0];
        CHECK(mid > (int)s_ngot / 2);
        CHECK(s_got[0].d0[3] != 0.75f);      /* alpha from the vertex (colour material) */
    }
    CHECK(same_three_ways(G * G - G, "lit, draw arrays"));
    m(0x0298, 0);                            /* colour material off */
    CHECK(same_three_ways(0, "lit, no colour material"));
    m(0x0314, 0);
    CHECK(same_three_ways(0, "unlit, indexed"));

    /* One vertex at w = 0 (z = -3) has no image: the batch is dropped every
     * way, though the vertex is named six times and transformed once. */
    putf(POS + 12 * (5 * G + 5) + 8, -3.0f);
    CHECK(same_three_ways(0, "a vertex at w = 0"));
    CHECK(s_ngot == 0);

    if (s_fail)
        return 1;
    printf("nv2a ffp batch: OK\n");
    return 0;
}
