/* The XDK D3D service (src/kernel/recomp_d3d_service.c, XBA-80; it was the
 * profile layer profiles/xdk/d3d8/d3d8.c): finding D3D's device in the title's
 * image, the software methods it services, the DMA walk it makes the default.
 * The guest's 4 GB are a reserved mapping, touched only where the test writes. */
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
void *xbox_GetMemoryBase(void) { return g_mem; }
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
/* XBA-105: the rules that fell back, "rule=value;" each, in order. */
static char g_fallbacks[512];
void recomp_discovery_d3d_layout(const char *lib, uint32_t build, const char *rule, uint32_t used)
{
    char e[96];
    snprintf(e, sizeof e, "%s %u %s=0x%X;", lib, build, rule, used);
    if (strlen(g_fallbacks) + strlen(e) < sizeof g_fallbacks)
        strcat(g_fallbacks, e);
}
static uint32_t g_cb_routine, g_cb_ctx;
void nv2a_pb_exec_insert_callback_args(uint32_t *r, uint32_t *c) { *r = 0x00123450u; *c = 0xC0FFEEu; }
int xbox_DpcQueueGuestCall(uint32_t r, uint32_t c) { g_cb_routine = r; g_cb_ctx = c; return 1; }

#include "recomp_d3d_service.h"

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

/* A KEVENT header as D3D writes it: type, Size 4, empty wait list. */
static void kevent(uint32_t va, uint32_t type)
{
    M32(va) = type | (4u << 16);
    M32(va + 4) = 0;
    M32(va + 8) = va + 8;
    M32(va + 12) = va + 8;
}

/* An XDK 5455 device (KOTOR): no slot at +0x24A8, the pointer at D - 8, the
 * GPU semaphore pointer at +0x30, vblank and fence events at +0x199C/+0x19AC. */
static void plant_5455(uint32_t d, uint32_t ring_lo, uint32_t ring_hi, uint32_t put)
{
    M32(d - 8) = d;
    M32(d) = put;
    M32(d + 0x24) = ring_lo;
    M32(d + 0x28) = ring_hi;
    M32(d + 0x30) = 0x8006B000u;
    kevent(d + 0x199C, 1);
    kevent(d + 0x19AC, 1);
}

/* The XBE header the kernel maps at 0x10000, with one library entry: the
 * title's D3D (XBA-105: the layer keys its layout table on it). */
static void plant_header(const char *lib, uint32_t build)
{
    memcpy(g_mem + 0x10000, "XBEH", 4);
    M32(0x10000 + 0x10C) = 0x00880000u;           /* SizeOfImage: image 0x10000-0x890000 */
    M32(0x10000 + 0x160) = 1;
    M32(0x10000 + 0x164) = 0x10900;
    memset(g_mem + 0x10900, 0, 16);
    memcpy(g_mem + 0x10900, lib, strlen(lib) < 8 ? strlen(lib) : 8);
    *(uint16_t *)(g_mem + 0x10900 + 12) = (uint16_t)build;
}

int main(void)
{
    g_mem = mmap(NULL, 0x100000000ull, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (g_mem == MAP_FAILED) {
        puts("skipped: cannot reserve 4 GB");
        return 77;
    }
#if defined(TEST_5455)
    plant_header("D3D8", 5455);                   /* KOTOR's: in the table */
#elif defined(TEST_GIVEN)
    plant_header("D3D8", 5849);                   /* a build the table leaves out */
#elif defined(TEST_UNKNOWN)
    plant_header("D3D8LTCG", 5933);               /* not in the table */
#elif defined(TEST_4627)
    plant_header("D3D8", 4627);                   /* not in the table either */
#else
    plant_header("D3D8LTCG", 5849);               /* Burnout 3's, Black's, Fable's */
#endif
    unsetenv("RECOMP_PB_WALK");
    unsetenv("RECOMP_D3D_SERVICE");
#if defined(TEST_GIVEN)
    /* Fable's old title params, as the run-time overrides. */
    setenv("RECOMP_D3D_DEVICE", "0x0085FDE0", 1);
    setenv("RECOMP_D3D_DEVICE_PTR", "0x00862288", 1);
    setenv("RECOMP_D3D_SW_METHODS", "0x7000", 1);
#elif defined(TEST_SCAN)
    setenv("RECOMP_PB_WALK", "scan", 1);          /* the A/B value: the segment scan */
#elif defined(TEST_OFF)
    setenv("RECOMP_D3D_SERVICE", "0", 1);
#endif
#ifdef TEST_OFF
    /* Left out: no hook, the walk not chosen, no watch, no device searched for. */
    CHECK(recomp_d3d_configure() == 0);
    CHECK(g_hook == NULL && !nv2a_pb_dma_walk() && getenv("RECOMP_PB_WALK") == NULL);
    recomp_d3d_start("g", NULL);
    if (failures)
        return 1;
    puts("recomp_d3d_service: ok");
    return 0;
#endif
    CHECK(recomp_d3d_configure() == 1);
#ifdef TEST_SCAN
    CHECK(!nv2a_pb_dma_walk());                   /* the environment wins over the default */
#else
    CHECK(nv2a_pb_dma_walk());                    /* the DMA walk: the default */
#endif
    CHECK(g_hook != NULL);                        /* every variant services some methods */

    /* 8 and 9 (PGRAPH RDI writes) are acknowledged in both variants, device or not (XBA-94). */
    CHECK(g_hook(0x123u << 5 | 8u) == 1 && g_hook(9u) == 1);
    /* 6 and 7 (InsertCallback): the routine and context from the clear values, queued (XBA-94). */
    CHECK(g_hook(6u) == 1 && g_cb_routine == 0x00123450u && g_cb_ctx == 0xC0FFEEu);
    g_cb_routine = 0;
    CHECK(g_hook(7u) == 1 && g_cb_routine == 0x00123450u);
#ifdef TEST_5455
    /* An XDK 5455 title (KOTOR's layout), no params: found through D - 8, the
     * fence event read from the device; the walk on, so it is adopted. */
    setenv("RECOMP_PB_WALK", "dma", 1);
    CHECK(recomp_d3d_locate() == 0);
    plant_5455(0x003550F0, 0x8006C000, 0x800EC000, 0x8006C100);
    CHECK(recomp_d3d_scan(0x10000, 0x890000) == 0);
    CHECK(recomp_d3d_locate() == 1);
    CHECK(g_disc_dev == 0x003550F0 && g_disc_slot == 0x003550E8);
    CHECK(g_adopted == 0x003550F0 + 0x19AC);
    CHECK(g_hook(5u) == 1 && g_event == 0x003550F0 + 0x19AC);
    CHECK(g_fallbacks[0] == 0);                   /* a known build: nothing fell back */
    if (failures)
        return 1;
    puts("recomp_d3d_service: ok");
    return 0;
#endif
#ifdef TEST_UNKNOWN
    /* XBA-105: a D3D8LTCG build the table does not know (5933), 5849's slot
     * rule, no KEVENT pair: found, and every rule that fell back is said and
     * recorded; the RunPushBuffer methods guard what they were not told. */
    {
        uint32_t dev = 0x00600000, node = dev + 0x264 + 20;
        setenv("RECOMP_PB_WALK", "dma", 1);
        plant(dev, 0x80010000, 0x80090000, 0x80012340);
        M32(dev + 0x30) = 0x8006B000u;
        CHECK(recomp_d3d_locate() == 1 && g_disc_dev == dev && g_disc_slot == dev + 0x24A8);
        CHECK(strstr(g_fallbacks, "D3D8LTCG 5933 slot=0x24A8;") != NULL);
        CHECK(strstr(g_fallbacks, "D3D8LTCG 5933 device_core=0x24;") != NULL);
        CHECK(strstr(g_fallbacks, "D3D8LTCG 5933 fence_event=0x1DCC;") != NULL);
        CHECK(g_hook(5u) == 1 && g_event == dev + 0x1DCC);
        /* Method 0xD on an unconfirmed layout: the pending count is left
         * alone while it does not read as a count, decremented when it does;
         * the node offsets used are said once. */
        M32(dev + 0x50) = 0;
        M32(node) = 0x80000000u | 0x00030000u;
        M32(node + 4) = 0;
        M32(node + 8) = 0x80030000u;
        CHECK(g_hook(1u << 5 | 0xDu) == 1 && M32(0x80030000u) == 0x00030001u && M32(dev + 0x50) == 0);
        M32(dev + 0x50) = 0x12345678u;
        CHECK(g_hook(1u << 5 | 0xDu) == 1 && M32(dev + 0x50) == 0x12345678u);
        M32(dev + 0x50) = 3;
        CHECK(g_hook(1u << 5 | 0xDu) == 1 && M32(dev + 0x50) == 2);
        CHECK(strstr(g_fallbacks, "D3D8LTCG 5933 rpb_pending=0x50;") != NULL);
        CHECK(strstr(g_fallbacks, "D3D8LTCG 5933 rpb_nodes=0x264;") != NULL);
        /* Each rule once. */
        CHECK(strstr(strstr(g_fallbacks, "fence_event") + 1, "fence_event") == NULL);
    }
    if (failures)
        return 1;
    puts("recomp_d3d_service: ok");
    return 0;
#endif
#ifdef TEST_4627
    /* XBA-105: another plausible layout on a build the table does not know:
     * the pointer slot well after the device (+0x1E00, not 5849's +0x24A8),
     * the event pair at +0x1500/+0x1510. Found by structure (any pointer, the
     * ring, the semaphore, the pair): no slot or fence-event fallback, only
     * the ring offsets the search itself assumes are said. */
    {
        uint32_t dev = 0x00480000;
        M32(dev) = 0x80100100u;
        M32(dev + 0x24) = 0x80100000u;
        M32(dev + 0x28) = 0x80180000u;
        M32(dev + 0x30) = 0x8006B000u;
        M32(dev + 0x1E00) = dev;
        kevent(dev + 0x1500, 1);
        kevent(dev + 0x1510, 1);
        CHECK(recomp_d3d_scan(0x10000, 0x890000) == 0);
        CHECK(recomp_d3d_locate() == 1 && g_disc_dev == dev && g_disc_slot == dev + 0x1E00);
        CHECK(strstr(g_fallbacks, "slot=") == NULL && strstr(g_fallbacks, "fence_event=") == NULL);
        CHECK(strstr(g_fallbacks, "D3D8 4627 device_core=0x24;") != NULL);
        CHECK(g_hook(5u) == 1 && g_event == dev + 0x1510);
    }
    if (failures)
        return 1;
    puts("recomp_d3d_service: ok");
    return 0;
#endif
#ifndef TEST_GIVEN
    /* The fence event's offset: the one pair of KEVENTs 16 bytes apart. */
    {
        uint32_t d = 0x00600000, slot = 0;
        CHECK(recomp_d3d_fence_event_offset(d) == 0);         /* no events */
        plant_5455(d, 0x80100000, 0x80200000, 0x80100000);
        CHECK(recomp_d3d_fence_event_offset(d) == 0x19AC);
        CHECK(recomp_d3d_scan(0x10000, 0x890000) == 0);       /* not 5849's slot rule */
        CHECK(recomp_d3d_scan_slots(0x10000, 0x890000, &slot) == d && slot == d - 8);
        /* A second pair: the offset is not trusted. */
        kevent(d + 0x800, 0);
        kevent(d + 0x810, 0);
        CHECK(recomp_d3d_fence_event_offset(d) == 0);
        M32(d + 0x800) = M32(d + 0x810) = 0;
        /* A wait list that is not empty, a Size that is not 4: not an event. */
        M32(d + 0x19AC + 8) = 0x80001234u;
        CHECK(recomp_d3d_fence_event_offset(d) == 0);
        kevent(d + 0x19AC, 1);
        M32(d + 0x199C) = 1u | (5u << 16);
        CHECK(recomp_d3d_fence_event_offset(d) == 0);
        kevent(d + 0x199C, 1);
        /* No semaphore pointer yet: not the device. */
        M32(d + 0x30) = 0;
        CHECK(recomp_d3d_scan_slots(0x10000, 0x890000, &slot) == 0);
        M32(d + 0x30) = 0x8006B000u;
        /* A second pointer to the same device: still one device, the first slot. */
        M32(0x00200000) = d;
        slot = 0;
        CHECK(recomp_d3d_scan_slots(0x10000, 0x890000, &slot) == d && slot == 0x00200000);
        M32(0x00200000) = 0;
        /* Two devices that pass: neither is trusted. */
        plant_5455(0x00680000, 0x80300000, 0x80400000, 0x80300000);
        CHECK(recomp_d3d_scan_slots(0x10000, 0x890000, &slot) == 0);
        M32(0x00680000 - 8) = 0;
        /* Pointers near the top of the 4 GB: skipped without wrapping (d + a
         * window would overflow), and no read past 0xFFFFFFFF. */
        M32(0x00210000) = 0xFFFFFFF0u;
        M32(0x00210004) = 0xFFFFC000u;
        slot = 0;
        CHECK(recomp_d3d_scan_slots(0x10000, 0x890000, &slot) == d && slot == d - 8);
        CHECK(recomp_d3d_fence_event_offset(0xFFFFFFF0u) == 0);
        CHECK(recomp_d3d_scan_slots(0xFFFF0000u, 0xFFFFFFFCu, &slot) == 0);
        CHECK(recomp_d3d_scan(0xFFFF0000u, 0xFFFFFFFCu) == 0);
        /* A device too close to the end of the range for its event window. */
        CHECK(recomp_d3d_scan_slots(0x10000, d + 0x1000, &slot) == 0);
        M32(0x00210000) = M32(0x00210004) = 0;
        M32(d - 8) = 0;                                     /* gone for what follows */
    }
    /* Before CreateDevice: nothing, and the hook does nothing. */
    CHECK(recomp_d3d_locate() == 0);
    CHECK(g_hook(0x7u << 5 | 0xDu) == 0);
    /* Decoys: a slot pointing at itself with no ring; a ring outside the
     * contiguous window; a put outside the ring. */
    M32(0x00300000 + 0x24A8) = 0x00300000;
    plant(0x00310000, 0x00400000, 0x00500000, 0x00400010);
    plant(0x00320000, 0x80010000, 0x80090000, 0x800A0000);
    CHECK(recomp_d3d_scan(0x10000, 0x890000) == 0);
    /* D3D's device, with its vblank and fence events (5849: +0x1DBC/+0x1DCC). */
    plant(0x0085FDE0, 0x80010000, 0x80090000, 0x80012340);
    kevent(0x0085FDE0 + 0x1DBC, 1);
    kevent(0x0085FDE0 + 0x1DCC, 1);
    CHECK(recomp_d3d_fence_event_offset(0x0085FDE0) == 0x1DCC);
    CHECK(recomp_d3d_is_device(0x0085FDE0));
    CHECK(recomp_d3d_scan(0x10000, 0x890000) == 0x0085FDE0);
    /* Two that pass: neither is trusted. */
    plant(0x00700000, 0x80100000, 0x80200000, 0x80100000);
    CHECK(recomp_d3d_scan(0x10000, 0x890000) == 0);
    M32(0x00700000 + 0x24A8) = 0;
    CHECK(recomp_d3d_locate() == 1);
    CHECK(g_disc_dev == 0x0085FDE0 && g_disc_slot == 0x00862288);
    CHECK(g_fallbacks[0] == 0);                   /* D3D8LTCG 5849 is in the table */
#ifndef TEST_SCAN
    /* Method 5 is delivered under the DMA walk: the fence event becomes a guest event. */
    CHECK(g_adopted == 0x0085FDE0 + 0x1DCC);
#else
    CHECK(g_adopted == 0);                        /* segment scan: a blocking wait could hang */
#endif
    /* Method 5: the fence event, device + 0x1DCC. */
    CHECK(g_hook(5u) == 1 && g_event == 0x0085FDE0 + 0x1DCC);
#else
    /* Fable's old params (0x7000): method 5 is serviced anyway, on the fence
     * event read from the given device at the first call. */
    kevent(0x0085FDE0 + 0x199C, 1);
    kevent(0x0085FDE0 + 0x19AC, 1);
    CHECK(g_hook(5u) == 1 && g_event == 0x0085FDE0 + 0x19AC);
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
         * does with the addresses given (RECOMP_D3D_DEVICE[_PTR]). */
        M32(node + 8) = 0x00400000u;
        M32(0x00400000u) = 0x12345678u;
        CHECK(g_hook(2u << 5 | 0xDu) == 1);
#ifdef TEST_GIVEN
        CHECK(M32(0x00400000u) == 0x00020001u);
        /* Fable's offsets, on a D3D8 build the table leaves out: used, and said. */
        CHECK(strstr(g_fallbacks, "D3D8 5849 rpb_nodes=0x264;") != NULL);
#else
        CHECK(M32(0x00400000u) == 0x12345678u);
        /* D3D8LTCG 5849's RunPushBuffer offsets are in the table: nothing fell back. */
        CHECK(g_fallbacks[0] == 0);
#endif
    }
    if (failures)
        return 1;
    puts("recomp_d3d_service: ok");
    return 0;
}
#endif
