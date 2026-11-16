/* Save states of the pushbuffer executor (XBA-248, xbox_dev_state.h).
 *
 * The executor against a fake guest memory and a spy back end with
 * NV2A_BACKEND_CAP_PIXEL, which hashes everything it is handed (surfaces,
 * render state, transformed vertices, texture and pixel state):
 *
 *   1. state A sent (surface, clip, vertex formats and arrays, composite
 *      matrix, viewport offset in c[59], blend, colour mask, depth, fog,
 *      texture stage 0, constants), a frame drawn; then part 2 -- a clear
 *      and two array batches that use all of that state -- hashed: the
 *      reference;
 *   2. state A again, captured (nv2a_pb_exec_state_save); state B sent over
 *      it (every one of those methods with other values); the capture
 *      restored; part 2 hashes as the reference;
 *   3. the control: part 2 under state B hashes otherwise, so the restore
 *      is what step 2 carried;
 *   4. prepare / after_load reach the back end's state hook (FLUSH, DROP);
 *      a truncated or foreign capture is refused and leaves the state whole. */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_backend.h"
#include "nv2a_vsh_interp.h"
#include "xbox_dev_state.h"

#define MEM_BYTES 0x80000u
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

static int s_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); s_fail = 1; } } while (0)

/* ---- the spy back end ---------------------------------------------------- */
static uint64_t s_hash;
static uint32_t s_calls, s_flushes, s_drops;

static void mix(const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    size_t i;
    for (i = 0; i < n; i++)
        s_hash = (s_hash ^ b[i]) * 1099511628211ull;
}
static void mix32(uint32_t v) { mix(&v, 4); }

static void be_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
                     uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    s_calls++;
    mix32(0xC1EA);
    mix(s, sizeof *s);
    mix(rs, sizeof *rs);
    mix32(flags);
    mix32(argb);
    mix32(zstencil);
}

static void be_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    uint32_t st;

    s_calls++;
    mix32(0xD7A3);
    mix(s, sizeof *s);
    if (b->state)
        mix(b->state, sizeof *b->state);
    mix32(b->count);
    mix(b->vertices, sizeof *b->vertices * b->count);
    if (b->full)
        mix(b->full, sizeof *b->full * b->count);
    if (b->texture)
        mix(b->texture, sizeof *b->texture);
    if (b->pixel)
        for (st = 0; st < 4; st++) {
            mix32(b->pixel->stage[st].mode);
            mix32(b->pixel->stage[st].valid);
            if (b->pixel->stage[st].valid)
                mix(&b->pixel->stage[st].tex, sizeof b->pixel->stage[st].tex);
        }
}

static void be_flip(void) {}
static void be_state(int op)
{
    if (op == NV2A_BACKEND_STATE_FLUSH) s_flushes++;
    if (op == NV2A_BACKEND_STATE_DROP) s_drops++;
}
static Nv2aBackend s_backend = { be_clear, be_draw, be_flip, NV2A_BACKEND_CAP_PIXEL };

/* ---- driving the executor ---------------------------------------------- */
extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
static void m(uint32_t method, uint32_t param) { nv2a_pb_exec_method(0, method, param); }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static void mf(uint32_t method, float f) { m(method, f2u(f)); }
static void putf(uint32_t at, float f) { memcpy(s_mem + at, &f, 4); }
static void put32(uint32_t at, uint32_t v) { memcpy(s_mem + at, &v, 4); }

#define FB_A   0x10000u
#define FB_B   0x40000u
#define VB_A   0x02000u          /* float3 positions */
#define VB_B   0x03000u
#define CB_A   0x04000u          /* D3DCOLOR */
#define CB_B   0x05000u
#define TEX_A  0x06000u
#define TEX_B  0x07000u

/* Everything part 2 reads, set one way (b = 0) or another (b = 1). */
static void send_state(int b)
{
    uint32_t i;
    const float sc = b ? 0.5f : 1.0f;
    const float M[16] = { 2 * sc, 0, 0, 0,  0, -2 * sc, 0, 0,  0, 0, 1, 0,
                          b ? 3.0f : 5.0f, b ? 7.0f : 90.0f, 0, 1 };

    m(0x0208, b ? 0x0105u : 0x0108u);                 /* SET_SURFACE_FORMAT */
    m(0x0200, (b ? 4u : 0u) | (b ? 100u : 128u) << 16);   /* CLIP_HORIZONTAL */
    m(0x0204, (b ? 2u : 0u) | (b ? 60u : 64u) << 16);     /* CLIP_VERTICAL */
    m(0x020C, b ? 256u : 512u);                       /* SET_SURFACE_PITCH */
    m(0x0210, b ? FB_B : FB_A);                       /* SET_SURFACE_COLOR_OFFSET */
    m(0x1E94, 0);                                     /* TRANSFORM_EXECUTION_MODE: fixed */
    m(0x1760, 2 | (3 << 4) | (12 << 8));              /* attr 0: float3, stride 12 */
    m(0x176C, 0 | (4 << 4) | (4 << 8));               /* attr 3: D3DCOLOR */
    m(0x1720, b ? VB_B : VB_A);                       /* attr 0 offset */
    m(0x172C, b ? CB_B : CB_A);                       /* attr 3 offset */
    for (i = 0; i < 16; i++)
        mf(0x0680 + 4 * i, M[i]);                     /* composite matrix */
    mf(0x0A20, b ? 1.0f : 0.5f);                      /* SET_VIEWPORT_OFFSET: c[59] */
    mf(0x0A24, b ? 2.0f : 0.5f);
    m(0x0304, b ? 0u : 1u);                           /* SET_BLEND_ENABLE */
    m(0x0344, b ? 0x0001u : 0x0302u);                 /* SFACTOR */
    m(0x0348, b ? 0x0000u : 0x0303u);                 /* DFACTOR */
    m(0x0358, b ? 0x01010100u : 0x01010101u);         /* SET_COLOR_MASK */
    m(0x030C, b ? 1u : 0u);                           /* DEPTH_TEST_ENABLE */
    m(0x0354, b ? 0x0203u : 0x0201u);                 /* DEPTH_FUNC */
    m(0x02A8, b ? 0x00112233u : 0x00445566u);         /* fog colour */
    m(0x1D90, b ? 0xFF00FF00u : 0xFF102030u);         /* SET_COLOR_CLEAR_VALUE */
    m(0x1B00, b ? TEX_B : TEX_A);                     /* texture 0 offset */
    m(0x1B04, (b ? 0x05u : 0x06u) << 8 | 2u << 4 | 6u << 20 | 6u << 24);   /* format */
    m(0x1EA4, 96 + 4);                                /* SET_TRANSFORM_CONSTANT_LOAD */
    for (i = 0; i < 8; i++)
        mf(0x0B80 + 4 * (i & 3), (float)(b ? 100 + i : 3 * i + 1));
}

static void put_vertices(void)
{
    static const float a[4][3] = { {1, 2, 0}, {30, 2, 0}, {30, 20, 0}, {1, 20, 0} };
    static const float bq[4][3] = { {5, 5, 0}, {50, 6, 0}, {48, 25, 0}, {4, 24, 0} };
    int i, k;
    for (i = 0; i < 4; i++)
        for (k = 0; k < 3; k++) {
            putf(VB_A + (uint32_t)(i * 12 + k * 4), a[i][k]);
            putf(VB_B + (uint32_t)(i * 12 + k * 4), bq[i][k]);
            putf(VB_A + 48 + (uint32_t)(i * 12 + k * 4), a[i][k] + 7.0f);
        }
    for (i = 0; i < 8; i++) {
        put32(CB_A + (uint32_t)i * 4, 0x80FF4020u + (uint32_t)i);
        put32(CB_B + (uint32_t)i * 4, 0xFF00FF00u - (uint32_t)i);
    }
}

/* Part 2: a clear and two array batches under whatever state was sent. */
static uint64_t part2(void)
{
    s_hash = 1469598103934665603ull;
    s_calls = 0;
    m(0x1D94, 0xF0);                                  /* CLEAR_SURFACE, colour */
    m(0x17FC, 8);                                     /* BEGIN_END: quads */
    m(0x1810, 0u | 3u << 24);                         /* DRAW_ARRAYS 0..3 */
    m(0x17FC, 0);
    m(0x17FC, 8);
    m(0x1810, 4u | 3u << 24);                         /* DRAW_ARRAYS 4..7 */
    m(0x17FC, 0);
    return s_hash;
}

int main(void)
{
    XdsWriter w = { 0 };
    XdsReader r;
    uint64_t ref, got, control;
    uint32_t calls_ref;
    float c100_a[4];

    s_backend.state = be_state;
    nv2a_backend_register(&s_backend);
    put_vertices();

    /* 1. Reference. */
    send_state(0);
    part2();                                          /* a first frame */
    ref = part2();
    calls_ref = s_calls;
    memcpy(c100_a, nv2a_vsh_constant(100), sizeof c100_a);
    printf("reference: %u back-end calls, hash %016llx\n", calls_ref, (unsigned long long)ref);
    CHECK(calls_ref == 3);

    /* 2. Capture, overwrite, restore. */
    send_state(0);
    nv2a_pb_exec_state_prepare();
    CHECK(s_flushes == 1);
    nv2a_pb_exec_state_save(&w);
    CHECK(!w.err);
    printf("executor section: %zu bytes\n", w.len);
    send_state(1);
    r = xds_reader(w.p, w.len);
    CHECK(nv2a_pb_exec_state_load(&r) && !r.err && r.pos == r.len);
    nv2a_pb_exec_state_after_load();
    CHECK(s_drops == 1);
    CHECK(!memcmp(nv2a_vsh_constant(100), c100_a, sizeof c100_a));
    got = part2();
    printf("restored:  %u back-end calls, hash %016llx\n", s_calls, (unsigned long long)got);
    CHECK(got == ref && s_calls == calls_ref);

    /* 3. Control. */
    send_state(1);
    control = part2();
    printf("state B:   %u back-end calls, hash %016llx\n", s_calls, (unsigned long long)control);
    CHECK(control != ref);

    /* 4. Refusals leave the state whole. */
    send_state(0);
    r = xds_reader(w.p, w.len / 3);
    CHECK(!nv2a_pb_exec_state_load(&r));
    {
        uint8_t *bad = (uint8_t *)malloc(w.len);
        memcpy(bad, w.p, w.len);
        bad[0] ^= 0x55;
        r = xds_reader(bad, w.len);
        CHECK(!nv2a_pb_exec_state_load(&r));
        free(bad);
    }
    got = part2();
    CHECK(got == ref);

    xds_writer_free(&w);
    if (!s_fail)
        puts("NV2A executor save state: restored state draws as the uninterrupted run: ok");
    return s_fail;
}
