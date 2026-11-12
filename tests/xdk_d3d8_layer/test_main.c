/* The XDK D3D profile layer (profiles/xdk/d3d8/d3d8.c, XBA-80): finding D3D's
 * device in the title's image, and the software methods it services. The
 * guest's 4 GB are a reserved mapping, touched only where the test writes. */
#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
int main(void) { puts("skipped: POSIX mapping"); return 77; }
#else
#include <sys/mman.h>

static uint8_t *g_mem;
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)g_mem; }
static int (*g_hook)(uint32_t);
void nv2a_pb_set_software_method_hook(int (*hook)(uint32_t)) { g_hook = hook; }
void nv2a_pb_set_semaphore_target(uint32_t va) { (void)va; }
static uint32_t g_event;
int xbox_KeSetGuestEvent(uint32_t va) { g_event = va; return 1; }
static uint32_t g_adopted;
int xbox_KeAdoptGuestEvent(uint32_t va) { g_adopted = va; return 1; }
int nv2a_pb_dma_walk(void) { const char *e = getenv("RECOMP_PB_WALK"); return e && !strcmp(e, "dma"); }
int xbox_Nv2aMirrorFence(uint32_t a, uint32_t b, uint32_t c) { (void)a; (void)b; (void)c; return 0; }
static uint32_t g_disc_dev, g_disc_slot;
void recomp_discovery_d3d(uint32_t d, uint32_t s) { g_disc_dev = d; g_disc_slot = s; }
static uint32_t g_cb_routine, g_cb_ctx;
void nv2a_pb_exec_insert_callback_args(uint32_t *r, uint32_t *c) { *r = 0x00123450u; *c = 0xC0FFEEu; }
int xbox_DpcQueueGuestCall(uint32_t r, uint32_t c) { g_cb_routine = r; g_cb_ctx = c; return 1; }

void xba_d3d8_configure(void);
int xba_d3d8_is_device(uint32_t d);
uint32_t xba_d3d8_scan(uint32_t lo, uint32_t hi);
int xba_d3d8_locate(void);

#define M32(a) (*(uint32_t *)(g_mem + (uint32_t)(a)))
static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void plant(uint32_t d, uint32_t ring_lo, uint32_t ring_hi, uint32_t put)
{
    M32(d) = put;
    M32(d + 0x24) = ring_lo;
    M32(d + 0x28) = ring_hi;
    M32(d + 0x24A8) = d;
}

int main(void)
{
    g_mem = mmap(NULL, 0x100000000ull, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (g_mem == MAP_FAILED) {
        puts("skipped: cannot reserve 4 GB");
        return 77;
    }
    M32(0x10000 + 0x10C) = 0x00880000u;           /* SizeOfImage: image 0x10000-0x890000 */
    unsetenv("RECOMP_PB_WALK");
    xba_d3d8_configure();
#ifdef TEST_LTCG
    CHECK(nv2a_pb_dma_walk());                    /* XBA_D3D_DMA_WALK: the walk by default */
#else
    CHECK(!nv2a_pb_dma_walk());
#endif
    CHECK(g_hook != NULL);                        /* both variants service some methods */

    /* 8 and 9 (PGRAPH RDI writes) are acknowledged in both variants, device or not (XBA-94). */
    CHECK(g_hook(0x123u << 5 | 8u) == 1 && g_hook(9u) == 1);
    /* 6 and 7 (InsertCallback): the routine and context from the clear values, queued (XBA-94). */
    CHECK(g_hook(6u) == 1 && g_cb_routine == 0x00123450u && g_cb_ctx == 0xC0FFEEu);
    g_cb_routine = 0;
    CHECK(g_hook(7u) == 1 && g_cb_routine == 0x00123450u);
#ifndef TEST_GIVEN
    /* Before CreateDevice: nothing, and the hook does nothing. */
    CHECK(xba_d3d8_locate() == 0);
    CHECK(g_hook(0x7u << 5 | 0xDu) == 0);
    /* Decoys: a slot pointing at itself with no ring; a ring outside the
     * contiguous window; a put outside the ring. */
    M32(0x00300000 + 0x24A8) = 0x00300000;
    plant(0x00310000, 0x00400000, 0x00500000, 0x00400010);
    plant(0x00320000, 0x80010000, 0x80090000, 0x800A0000);
    CHECK(xba_d3d8_scan(0x10000, 0x890000) == 0);
    /* D3D's device. */
    plant(0x0085FDE0, 0x80010000, 0x80090000, 0x80012340);
    CHECK(xba_d3d8_is_device(0x0085FDE0));
    CHECK(xba_d3d8_scan(0x10000, 0x890000) == 0x0085FDE0);
    /* Two that pass: neither is trusted. */
    plant(0x00700000, 0x80100000, 0x80200000, 0x80100000);
    CHECK(xba_d3d8_scan(0x10000, 0x890000) == 0);
    M32(0x00700000 + 0x24A8) = 0;
    CHECK(xba_d3d8_locate() == 1);
    CHECK(g_disc_dev == 0x0085FDE0 && g_disc_slot == 0x00862288);
#ifdef TEST_LTCG
    /* Method 5 is delivered under the DMA walk: the fence event becomes a guest event. */
    CHECK(g_adopted == 0x0085FDE0 + 0x1DCC);
#else
    CHECK(g_adopted == 0);                        /* segment scan: a blocking wait could hang */
#endif
    /* Method 5: the fence event, device + 0x1DCC. */
    CHECK(g_hook(5u) == 1 && g_event == 0x0085FDE0 + 0x1DCC);
#else
    CHECK(g_hook(5u) == 0);                       /* Fable's params: 0xC-0xE only */
#endif
    /* Method 0xD, payload 2: node = device + 0x264 + 2*20; its slot gets the
     * JUMP, one pending run fewer, next = -1, bit 30 cleared. */
    {
        uint32_t dev = 0x0085FDE0, node = dev + 0x264 + 2 * 20;
        plant(dev, 0x80010000, 0x80090000, 0x80012340);
        M32(dev + 0x50) = 3;
        M32(node) = 0xC0000000u | 0x00020000u;    /* target 0x20000, bits 31 and 30 */
        M32(node + 4) = 0;                        /* end of chain */
        M32(node + 8) = 0x80020000u;              /* the slot: in the ring */
        CHECK(g_hook(2u << 5 | 0xDu) == 1);
        CHECK(M32(0x80020000u) == 0x00020001u);
        CHECK(M32(dev + 0x50) == 2);
        CHECK(M32(node + 4) == 0xFFFFFFFFu && M32(node) == (0x80000000u | 0x00020000u));
        /* A slot outside the contiguous window: not written for a device
         * found at run time (its layout is assumed); written as D3D's handler
         * does with the profile's addresses (Fable's code, unchanged). */
        M32(node + 8) = 0x00400000u;
        M32(0x00400000u) = 0x12345678u;
        CHECK(g_hook(2u << 5 | 0xDu) == 1);
#ifdef TEST_GIVEN
        CHECK(M32(0x00400000u) == 0x00020001u);
#else
        CHECK(M32(0x00400000u) == 0x12345678u);
#endif
    }
    if (failures)
        return 1;
    puts("xdk_d3d8_layer: ok");
    return 0;
}
#endif
