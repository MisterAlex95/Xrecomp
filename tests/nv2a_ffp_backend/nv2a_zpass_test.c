/* Deferred visibility reports (nv2a_backend.h zpass_end / zpass_resolve).
 *
 * A back end with zpass_end does not make the executor wait for the GPU at
 * every GET_REPORT: the report slot keeps the 0xFFFFFFFF D3D wrote at +0xC
 * until the count is in, and the executor writes it (stamp, count, 0) when
 * it resolves -- at FLIP_STALL, when the walker has caught up with PUT
 * (nv2a_pb_exec_idle, blocking: the guest may be waiting on the slot), or
 * when more than 512 are pending. Checked here with a fake back end whose
 * "GPU" finishes only when told to:
 *
 *   - nothing is written while pending, and a non-blocking point does not
 *     resolve what the GPU has not finished;
 *   - the idle hook and the flip resolve everything, in order, none lost;
 *   - the count is cumulative from CLEAR_REPORT_VALUE, as the synchronous
 *     path reports it;
 *   - past 512 pending the oldest are resolved;
 *   - RECOMP_GPU_ZPASS=always (second run of this test) reports 1 at once
 *     and hands the back end no counting batches. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nv2a_backend.h"

#define MEM_BYTES 0x40000u
#define FB_ADDR   0x8000u
#define POS_AT    0x2000u
#define REPORT_AT 0x20000u          /* report slots, 16 bytes each */

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
uint32_t xbox_Nv2aFenceWordVa(void) { return 0; }
int nv2a_pb_dma_walk(void) { return 1; }
void xbox_FramebufferWindowStart(void) {}
void xbox_FramebufferWindowFrameStats(uint32_t draws) { (void)draws; }
void xbox_FramebufferWindowPresent(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_Nv2aFrameCounterFlip(void) {}
int nv2a_pb_scan_in_call(void) { return 0; }
uint32_t xbox_VblankCount(void) { return 0; }
uint64_t xbox_HostNowNs(void) { return 0; }
void xbox_HostSleepNs(uint64_t ns) { (void)ns; }
uint32_t xbox_Nv2aFrameCountersOwned(void) { return 0; }
volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset) { (void)offset; return NULL; }

extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
extern void nv2a_pb_exec_idle(void);
static void m(uint32_t method, uint32_t param) { nv2a_pb_exec_method(0, method, param); }

/* ---- the fake back end: a GPU that finishes when told ----------------- */

#define MAX_GROUPS 4096
static uint32_t s_open;                 /* count of counting batches not yet closed */
static uint32_t s_group_count[MAX_GROUPS + 1];
static uint32_t s_tickets;              /* last ticket handed out */
static uint32_t s_done;                 /* the GPU has finished tickets <= this */
static uint32_t s_last_resolved;
static int s_out_of_order, s_counting_draws, s_sync_reads;

static void fake_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    (void)s;
    if (b->state->zpass_enable) {
        s_open += b->count;             /* "pixels": one per vertex */
        s_counting_draws++;
    }
}
static void fake_flip(void) {}
static uint32_t fake_read(void)
{
    uint32_t c = s_open;
    s_open = 0;
    s_sync_reads++;
    return c;
}
static uint32_t fake_end(void)
{
    if (s_tickets >= MAX_GROUPS)
        return 0;
    s_group_count[++s_tickets] = s_open;
    s_open = 0;
    return s_tickets;
}
static int fake_resolve(uint32_t ticket, int block, uint32_t *count)
{
    if (ticket != s_last_resolved + 1)
        s_out_of_order = 1;
    if (ticket > s_done && !block)
        return 0;
    if (ticket > s_done)
        s_done = ticket;                /* blocking: wait for the GPU */
    s_last_resolved = ticket;
    *count = s_group_count[ticket];
    return 1;
}
static Nv2aBackend s_backend = {
    NULL, fake_draw, fake_flip, 0, fake_read, fake_end, fake_resolve
};

/* ---- helpers ---------------------------------------------------------- */

static void slot_init(uint32_t n)
{
    memset(s_mem + REPORT_AT + 16u * n, 0, 12);
    memset(s_mem + REPORT_AT + 16u * n + 12, 0xFF, 4);
}
static uint32_t slot_done(uint32_t n)
{
    uint32_t v;
    memcpy(&v, s_mem + REPORT_AT + 16u * n + 12, 4);
    return v;
}
static uint32_t slot_count(uint32_t n)
{
    uint32_t v;
    memcpy(&v, s_mem + REPORT_AT + 16u * n + 8, 4);
    return v;
}

/* One counted batch of `tris` triangles (3 vertices each). */
static void counted_draw(uint32_t tris)
{
    m(0x17CC, 1);                       /* SET_ZPASS_PIXEL_COUNT_ENABLE */
    m(0x17FC, 5);
    m(0x1810, ((3u * tris) - 1u) << 24);
    m(0x17FC, 0);
    m(0x17CC, 0);
}

/* D3D's visibility test: CLEAR_REPORT_VALUE, the batch, GET_REPORT. */
static void visibility_test(uint32_t slot, uint32_t tris)
{
    slot_init(slot);
    m(0x17C8, 1);                       /* CLEAR_REPORT_VALUE */
    counted_draw(tris);
    m(0x17D0, REPORT_AT + 16u * slot);  /* GET_REPORT */
}

int main(void)
{
    const char *mode = getenv("RECOMP_GPU_ZPASS");
    int always = mode && strcmp(mode, "always") == 0;
    uint32_t i;

    m(0x0208, 0x0008);                  /* A8R8G8B8 */
    m(0x0200, 64u << 16);
    m(0x0204, 64u << 16);
    m(0x020C, 64 * 4);
    m(0x0210, FB_ADDR);
    {
        static const float I[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        for (i = 0; i < 16; i++) {
            uint32_t u;
            memcpy(&u, &I[i], 4);
            m(0x0680 + 4 * i, u);
        }
    }
    m(0x1E94, 0);                       /* fixed function */
    for (i = 0; i < 300; i++) {         /* 100 small triangles in a 64x64 surface */
        float p[3] = { (float)(i % 3 == 1 ? 8 : 1), (float)(i % 3 == 2 ? 8 : 1), 0 };
        memcpy(s_mem + POS_AT + 12u * i, p, 12);
    }
    m(0x1720, POS_AT);
    m(0x1760, 2 | (3 << 4) | (12 << 8));
    nv2a_backend_register(&s_backend);

    if (always) {
        visibility_test(0, 2);
        CHECK(slot_done(0) == 0 && slot_count(0) == 1);
        CHECK(s_counting_draws == 0);   /* no queries for the back end */
        CHECK(s_tickets == 0 && s_sync_reads == 0);
        if (!s_fail)
            puts("NV2A deferred visibility reports (RECOMP_GPU_ZPASS=always): ok");
        return s_fail;
    }

    /* Three tests in a frame: nothing written while the GPU is busy. */
    visibility_test(0, 1);
    visibility_test(1, 2);
    visibility_test(2, 3);
    CHECK(s_sync_reads == 0);
    CHECK(slot_done(0) == 0xFFFFFFFFu && slot_done(1) == 0xFFFFFFFFu
          && slot_done(2) == 0xFFFFFFFFu);

    /* The GPU has finished nothing (s_done 0): reports do not appear until
     * a resolve point; the idle hook blocks and resolves all three, in
     * order. */
    nv2a_pb_exec_idle();
    CHECK(!s_out_of_order);
    CHECK(slot_done(0) == 0 && slot_done(1) == 0 && slot_done(2) == 0);
    CHECK(slot_count(0) == 3 && slot_count(1) == 6 && slot_count(2) == 9);

    /* Cumulative between clears, as the synchronous path reports: two
     * GET_REPORTs after one CLEAR, the second carries both batches. */
    slot_init(3); slot_init(4);
    m(0x17C8, 1);
    counted_draw(1);
    m(0x17D0, REPORT_AT + 16u * 3);
    counted_draw(2);
    m(0x17D0, REPORT_AT + 16u * 4);
    CHECK(slot_done(3) == 0xFFFFFFFFu && slot_done(4) == 0xFFFFFFFFu);
    m(0x0130, 0);                       /* FLIP_STALL resolves */
    CHECK(slot_done(3) == 0 && slot_count(3) == 3);
    CHECK(slot_done(4) == 0 && slot_count(4) == 9);
    CHECK(!s_out_of_order);

    /* Stamps increase in report order. */
    {
        uint64_t a, b;
        memcpy(&a, s_mem + REPORT_AT + 16u * 3, 8);
        memcpy(&b, s_mem + REPORT_AT + 16u * 4, 8);
        CHECK(b > a);
    }

    /* A long frame: past 512 pending reports the oldest are resolved
     * without waiting for a flip or the walker; none is lost. */
    for (i = 0; i < 600; i++)
        visibility_test(8 + i, 1);
    CHECK(slot_done(8) == 0 && slot_count(8) == 3);
    CHECK(slot_done(8 + 599) == 0xFFFFFFFFu);
    nv2a_pb_exec_idle();
    for (i = 0; i < 600; i++)
        if (slot_done(8 + i) != 0 || slot_count(8 + i) != 3) {
            fprintf(stderr, "FAIL report %u: done %08X count %u\n",
                    i, slot_done(8 + i), slot_count(8 + i));
            s_fail = 1;
            break;
        }
    CHECK(!s_out_of_order);
    CHECK(s_sync_reads == 0);

    /* Nothing pending: the idle hook does nothing. */
    {
        uint32_t t = s_tickets;
        nv2a_pb_exec_idle();
        CHECK(s_tickets == t);
    }

    if (!s_fail)
        puts("NV2A deferred visibility reports: ok");
    return s_fail;
}
