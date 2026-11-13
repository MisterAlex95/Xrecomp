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
static int (*g_refusals)(char *, int);
void nv2a_pb_set_software_method_refusals(int (*describe)(char *, int)) { g_refusals = describe; }
size_t xbox_GetMappedSize(void) { return 0x04000000u; }     /* 64 MB of RAM */
static uint32_t g_sem;
void nv2a_pb_set_semaphore_target(uint32_t va) { g_sem = va; }
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

/* ── Deferred RunPushBuffer fixups (method 0xD) ──────────────────────────
 * A node { target | flags, next, slot, fixups, base }; a recorded pushbuffer
 * [base, slot] (slot: its return JUMP); a D3DFixup list { size, offset, bytes }
 * ... 0xFFFFFFFF wherever the title keeps it (Fable: its heap, 0x0129xxxx). */
#define PB_BASE 0x80040000u
#define PB_SLOT (PB_BASE + 0xFCu)                 /* a 256-byte pushbuffer */

static void node_at(uint32_t node, uint32_t target, uint32_t next, uint32_t slot, uint32_t fix, uint32_t base)
{
    M32(node) = target;
    M32(node + 4) = next;
    M32(node + 8) = slot;
    M32(node + 12) = fix;
    M32(node + 16) = base;
}

/* One entry at `at` (size bytes of `fill` to base + off); returns the next entry's address. */
static uint32_t fixup(uint32_t at, uint32_t size, uint32_t off, uint8_t fill)
{
    M32(at) = size;
    M32(at + 4) = off;
    memset(g_mem + at + 8, fill, size);
    return at + 8 + size;
}

static int all(uint32_t va, uint32_t n, uint8_t v)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        if (g_mem[va + i] != v)
            return 0;
    return 1;
}

/* The pushbuffer and 256 bytes either side filled with 0x11. */
static void pb_fresh(void)
{
    memset(g_mem + PB_BASE - 0x100, 0x11, 0x300);
}

static uint32_t refused(void)
{
    return recomp_d3d_fixups_refused();
}

/* Method 0xD, payload 3, on `dev` (RunPushBuffer offsets +0x50/+0x264).
 * given: the addresses given, the handler writes unchecked. */
static void test_fixups(uint32_t dev, int given)
{
    uint32_t node = dev + 0x264 + 3 * 20, at, r0;
    const uint32_t m = 3u << 5 | 0xDu, list = 0x01290000u;

    /* 1. A list on the heap, two entries inside [base, slot): both copied,
     * the return JUMP patched, one pending run fewer, next = -1, nothing refused. */
    pb_fresh();
    at = fixup(list, 4, 0x10, 0xAA);
    at = fixup(at, 8, 0x20, 0xBB);
    M32(at) = 0xFFFFFFFFu;
    node_at(node, 0x20000000u | 0x00041000u, 0, PB_SLOT, list, PB_BASE);
    M32(dev + 0x50) = 3;
    r0 = refused();
    CHECK(g_hook(m) == 1);
    CHECK(all(PB_BASE + 0x10, 4, 0xAA) && all(PB_BASE + 0x20, 8, 0xBB));
    CHECK(all(PB_BASE, 0x10, 0x11) && all(PB_BASE + 0x14, 0x0C, 0x11) && all(PB_BASE + 0x28, 0xD4, 0x11));
    CHECK(M32(PB_SLOT) == 0x00041001u);
    CHECK(M32(dev + 0x50) == 2);
    CHECK(M32(node + 4) == 0xFFFFFFFFu);
    CHECK(refused() == r0);
    CHECK(g_refusals == recomp_d3d_describe_refusals);

    if (given) {
        /* 5. The addresses given: unchecked, as D3D's handler (a destination
         * below the pushbuffer is written). */
        pb_fresh();
        at = fixup(list, 4, 0xFFFFFFF0u, 0xCC);     /* base - 16 */
        M32(at) = 0xFFFFFFFFu;
        node_at(node, 0x20000000u | 0x00041000u, 0, PB_SLOT, list, PB_BASE);
        CHECK(g_hook(m) == 1 && all(PB_BASE - 16, 4, 0xCC) && M32(PB_SLOT) == 0x00041001u);
        return;
    }

    /* 2. A destination at the slot (the return JUMP itself), below base, past
     * the slot, wrapping: not written, counted; the entry before it applied,
     * the one after it not, the return JUMP patched. */
    {
        static const uint32_t bad_off[] = { 0xFCu, 0xFFFFFFF0u, 0x200u, 0x80000010u, 0xF8u };
        static const uint32_t bad_size[] = { 4, 4, 4, 4, 8 };
        unsigned i;
        for (i = 0; i < sizeof bad_off / sizeof bad_off[0]; i++) {
            uint32_t to = PB_BASE + bad_off[i];
            pb_fresh();
            M32(0x00040010u) = 0x11111111u;          /* where the wrapped one would land */
            at = fixup(list, 4, 0x30, 0xCC);
            at = fixup(at, bad_size[i], bad_off[i], 0xEE);
            at = fixup(at, 4, 0x40, 0xDD);
            M32(at) = 0xFFFFFFFFu;
            node_at(node, 0x20000000u | 0x00041000u, 0, PB_SLOT, list, PB_BASE);
            M32(dev + 0x50) = 3;
            r0 = refused();
            CHECK(g_hook(m) == 1);
            CHECK(all(PB_BASE + 0x30, 4, 0xCC));
            CHECK(all(PB_BASE + 0x40, 4, 0x11));
            CHECK(M32(PB_SLOT) == 0x00041001u);
            CHECK(refused() == r0 + 1);
            CHECK(M32(dev + 0x50) == 2 && M32(node + 4) == 0xFFFFFFFFu);
            CHECK(M32(to) != 0xEEEEEEEEu);
            CHECK(M32(0x00040010u) == 0x11111111u && all(PB_BASE - 0x100, 0x100, 0x11)
                  && all(PB_SLOT + 4, 0x100, 0x11));
        }
    }

    /* 3. An entry larger than 64 KB; a list with no end within 1 MB (zeroed
     * memory: endless empty entries); a payload past the end of RAM; a list
     * outside guest memory: refused, counted, nothing written. */
    {
        static const uint32_t lists[] = { 0x01290000u, 0x01400000u, 0x03FFFFF0u, 0x50000000u };
        unsigned i;
        fixup(0x01290000u, 0x10001u, 0, 0xEE);
        M32(0x03FFFFF0u) = 16;
        M32(0x03FFFFF4u) = 0;
        for (i = 0; i < 4; i++) {
            pb_fresh();
            node_at(node, 0x20000000u | 0x00041000u, 0, PB_SLOT, lists[i], PB_BASE);
            r0 = refused();
            CHECK(g_hook(m) == 1);
            CHECK(refused() == r0 + 1);
            CHECK(all(PB_BASE - 0x100, 0x1FC, 0x11) && M32(PB_SLOT) == 0x00041001u && all(PB_SLOT + 4, 0x100, 0x11));
        }
        memset(g_mem + 0x01290000u, 0, 0x10010u);
        /* A base outside the contiguous window, or not below the slot. */
        pb_fresh();
        at = fixup(list, 4, 0x10, 0xAA);
        M32(at) = 0xFFFFFFFFu;
        node_at(node, 0x20000000u | 0x00041000u, 0, PB_SLOT, list, 0x00040000u);
        r0 = refused();
        CHECK(g_hook(m) == 1 && refused() == r0 + 1 && M32(0x00040010u) != 0xAAAAAAAAu);
        node_at(node, 0x20000000u | 0x00041000u, 0, PB_SLOT, list, PB_SLOT + 4);
        CHECK(g_hook(m) == 1 && refused() == r0 + 2 && all(PB_SLOT + 4, 0x100, 0x11));
    }

    /* 4. A chain of two nodes (the second in the ring, method 0xC style):
     * both serviced, both fixups applied, next = -1 in each. */
    {
        uint32_t node2 = 0x80060000u, base2 = 0x80050000u, list2 = 0x01291000u;
        pb_fresh();
        memset(g_mem + base2, 0x11, 0x100);
        at = fixup(list, 4, 0x10, 0xAA);
        M32(at) = 0xFFFFFFFFu;
        at = fixup(list2, 4, 0x08, 0xBB);
        M32(at) = 0xFFFFFFFFu;
        node_at(node, 0x20000000u | 0x00041000u, node2, PB_SLOT, list, PB_BASE);
        node_at(node2, 0x20000000u | 0x00042000u, 0, base2 + 0xFC, list2, base2);
        r0 = refused();
        CHECK(g_hook(m) == 1);
        CHECK(all(PB_BASE + 0x10, 4, 0xAA) && M32(PB_SLOT) == 0x00041001u);
        CHECK(all(base2 + 0x08, 4, 0xBB) && M32(base2 + 0xFC) == 0x00042001u);
        CHECK(M32(node + 4) == 0xFFFFFFFFu && M32(node2 + 4) == 0xFFFFFFFFu);
        CHECK(refused() == r0);
    }
}


/* ── The other run-time guards of RunPushBuffer's methods (WP-C) ─────────
 * A found device: the node in guest memory, the slot in the contiguous
 * window, each refusal counted by kind and described for the report line.
 * Both: the chain cut at 4096 nodes. Method 0xC: node = payload | 0x80000000. */
#define M0xC(node) ((((node) & 0x07FFFFFFu) << 5) | 0xCu)

static void test_guards(uint32_t dev, int given)
{
    uint32_t r, i;
    const uint32_t n1 = 0x80070000u, n2 = 0x80070020u, w2 = 0x80071000u;
    char buf[192];

    M32(dev + 0x50) = 0x100;
    /* 1. Slot: the window's last word accepted, the chain followed. */
    node_at(n1, 0x00031000u, n2, 0x83FFFFFCu, 0, 0);
    node_at(n2, 0x00032000u, 0, w2, 0, 0);
    M32(w2) = 0;
    r = recomp_d3d_refused(RECOMP_D3D_REFUSED_SLOT);
    CHECK(g_hook(M0xC(n1)) == 1);
    CHECK(M32(0x83FFFFFCu) == 0x00031001u && M32(w2) == 0x00032001u);
    CHECK(M32(n2 + 4) == 0xFFFFFFFFu && recomp_d3d_refused(RECOMP_D3D_REFUSED_SLOT) == r);
    CHECK(M32(dev + 0x50) == 0xFF);
    /* A slot straddling the window's end: refused and counted, the chain not
     * followed (n2 untouched); with the device given, written. */
    node_at(n1, 0x00031000u, n2, 0x83FFFFFEu, 0, 0);
    node_at(n2, 0x00032000u, 0, w2, 0, 0);
    M32(w2) = 0;
    M32(0x83FFFFF8u) = M32(0x83FFFFFCu) = 0;
    CHECK(g_hook(M0xC(n1)) == 1);
    if (given) {
        CHECK(M32(w2) == 0x00032001u && recomp_d3d_refused(RECOMP_D3D_REFUSED_SLOT) == 0);
    } else {
        CHECK(M32(0x83FFFFFCu) == 0 && M32(w2) == 0 && M32(n2 + 4) == 0 && M32(n1 + 4) == n2);
        CHECK(recomp_d3d_refused(RECOMP_D3D_REFUSED_SLOT) == r + 1);
    }
    CHECK(M32(dev + 0x50) == 0xFE);         /* the pending count is the method's, not the chain's */

    if (!given) {
        /* 2. Node: the first one past the contiguous window (payload 0x4000000:
         * 0x84000000), refused and counted, nothing written. */
        r = recomp_d3d_refused(RECOMP_D3D_REFUSED_NODE);
        M32(0) = 0x5A5A5A5Au;
        M32(0x84000000u) = 0x00033000u;
        M32(0x84000008u) = 0x80072000u;
        M32(0x80072000u) = 0;
        CHECK(g_hook(0x04000000u << 5 | 0xCu) == 1);
        CHECK(M32(0x80072000u) == 0 && M32(0x84000004u) == 0 && M32(0) == 0x5A5A5A5Au);
        CHECK(recomp_d3d_refused(RECOMP_D3D_REFUSED_NODE) == r + 1);
        M32(0x84000000u) = M32(0x84000008u) = 0;
        /* A next naming no guest memory: the first node serviced, the second refused. */
        node_at(n1, 0x00031000u, 0x50000000u, 0x80073000u, 0, 0);
        CHECK(g_hook(M0xC(n1)) == 1 && M32(0x80073000u) == 0x00031001u && M32(n1 + 4) == 0xFFFFFFFFu);
        CHECK(recomp_d3d_refused(RECOMP_D3D_REFUSED_NODE) == r + 2);
        /* A next naming a node of D3D's table (low RAM, in the image): serviced. */
        node_at(n1, 0x00031000u, dev + 0x264 + 5 * 20, 0x80073000u, 0, 0);
        node_at(dev + 0x264 + 5 * 20, 0x00034000u, 0, 0x80074000u, 0, 0);
        CHECK(g_hook(M0xC(n1)) == 1 && M32(0x80074000u) == 0x00034001u);
        CHECK(recomp_d3d_refused(RECOMP_D3D_REFUSED_NODE) == r + 2);
    }

    /* 3. A chain of 4097 nodes: 4096 serviced, cut and counted, the last one
     * left linked (with the device given too). */
    {
        const uint32_t nodes = 0x80200000u, slots = 0x80300000u;
        r = recomp_d3d_refused(RECOMP_D3D_REFUSED_CHAIN);
        for (i = 0; i < 4097; i++) {
            node_at(nodes + i * 20, 0x00040000u + i * 4, i < 4096 ? nodes + (i + 1) * 20 : 0, slots + i * 4, 0, 0);
            M32(slots + i * 4) = 0;
        }
        CHECK(g_hook(M0xC(nodes)) == 1);
        CHECK(M32(slots) == 0x00040001u && M32(slots + 4095 * 4) == 0x00040000u + 4095 * 4 + 1);
        CHECK(M32(slots + 4096 * 4) == 0 && M32(nodes + 4096 * 20 + 4) == 0);
        CHECK(M32(nodes + 4095 * 20 + 4) == 0xFFFFFFFFu);
        CHECK(recomp_d3d_refused(RECOMP_D3D_REFUSED_CHAIN) == r + 1);
        /* The same chain once more: every next is -1 now, one node serviced. */
        M32(slots + 4 * 1) = 0;
        CHECK(g_hook(M0xC(nodes)) == 1 && M32(slots + 4) == 0);
        CHECK(recomp_d3d_refused(RECOMP_D3D_REFUSED_CHAIN) == r + 1);
    }

    /* 4. The report line's suffix: every kind with a count, in order, by name. */
    CHECK(g_refusals == recomp_d3d_describe_refusals);
    CHECK(recomp_d3d_describe_refusals(buf, sizeof buf) == (int)strlen(buf));
    {
        char want[192] = "", e[64];
        static const char *const names[] = { "fixup lists", "slots", "nodes", "pending counts", "chains" };
        for (i = 0; i < RECOMP_D3D_REFUSAL_KINDS; i++)
            if (recomp_d3d_refused((RecompD3dRefusal)i)) {
                snprintf(e, sizeof e, "; %s refused %u", names[i], recomp_d3d_refused((RecompD3dRefusal)i));
                strcat(want, e);
            }
        CHECK(!strcmp(buf, want));
        CHECK(strstr(buf, "; chains refused 1") != NULL);
        if (!given)
            CHECK(strstr(buf, "; fixup lists refused ") == buf && strstr(buf, "; nodes refused 2") != NULL);
    }
    CHECK(recomp_d3d_describe_refusals(buf, 10) <= 9 && strlen(buf) <= 9);   /* truncated, still a string */
    CHECK(recomp_d3d_describe_refusals(buf, 0) == 0);

    /* 5. The semaphore watch on the device (found or given): a heap pointer
     * not handed over, a contiguous one handed over once. */
    g_sem = 0;
    M32(dev + 0x30) = 0x01234560u;
    CHECK(recomp_d3d_watch_once(0) == 0 && g_sem == 0);
    M32(dev + 0x30) = 0x8006B000u;
    CHECK(recomp_d3d_watch_once(0) == 0x8006B000u && g_sem == 0x8006B000u);
    g_sem = 0;
    CHECK(recomp_d3d_watch_once(0x8006B000u) == 0x8006B000u && g_sem == 0);
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
#elif defined(TEST_DEVONLY)
    /* The device given alone, no RECOMP_D3D_SW_METHODS (WP-C). */
    setenv("RECOMP_D3D_DEVICE", "0x0085FDE0", 1);
#elif defined(TEST_SWMASK)
    setenv("RECOMP_D3D_SW_METHODS", "0x1000", 1); /* RunPushBuffer's 0xC only */
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
#ifdef TEST_DEVONLY
    /* RECOMP_D3D_DEVICE alone: every method serviced, as for a found device
     * (it used to leave 0xC-0xE off: Fable stopped flipping after its load,
     * "not serviced D:4620"); unchecked, as with the addresses given; the
     * semaphore watched on the given device, not searched for. */
    {
        uint32_t dev = 0x0085FDE0, node = dev + 0x264 + 2 * 20;
        CHECK(g_hook(5u) == 1);
        M32(dev + 0x50) = 3;
        node_at(node, 0x00020000u, 0, 0x00400000u, 0, 0);   /* a slot the guards would refuse */
        CHECK(g_hook(2u << 5 | 0xDu) == 1 && M32(0x00400000u) == 0x00020001u && M32(dev + 0x50) == 2);
        node_at(0x80070000u, 0x00021000u, 0, 0x80071000u, 0, 0);
        CHECK(g_hook(M0xC(0x80070000u)) == 1 && M32(0x80071000u) == 0x00021001u);
        node_at(0x80070000u, 0x00022000u, 0, 0x80071000u, 0, 0);
        CHECK(g_hook(M0xC(0x80070000u) | 0x2u) == 1 && M32(0x80071000u) == 0x00022001u);   /* 0xE */
        CHECK(recomp_d3d_refused(RECOMP_D3D_REFUSED_SLOT) == 0);
        M32(dev + 0x30) = 0x8006B000u;
        CHECK(recomp_d3d_watch_once(0) == 0x8006B000u && g_sem == 0x8006B000u);
        CHECK(g_disc_dev == 0);                   /* nothing searched for */
    }
    if (failures)
        return 1;
    puts("recomp_d3d_service: ok");
    return 0;
#endif
#ifdef TEST_SWMASK
    /* RECOMP_D3D_SW_METHODS=0x1000 on a found device: 0xC serviced, 0xD and 0xE
     * not (counted unserviced by the executor; configure said so at start). */
    {
        uint32_t dev = 0x0085FDE0;
        plant(dev, 0x80010000, 0x80090000, 0x80012340);
        CHECK(recomp_d3d_locate() == 1);
        M32(dev + 0x50) = 3;
        node_at(0x80070000u, 0x00021000u, 0, 0x80071000u, 0, 0);
        CHECK(g_hook(M0xC(0x80070000u)) == 1 && M32(0x80071000u) == 0x00021001u && M32(dev + 0x50) == 2);
        M32(0x80071000u) = 0;
        node_at(dev + 0x264, 0x00021000u, 0, 0x80071000u, 0, 0);
        CHECK(g_hook(0xDu) == 0 && g_hook(M0xC(0x80070000u) | 0x2u) == 0 && M32(0x80071000u) == 0);
        CHECK(M32(dev + 0x50) == 2 && g_hook(5u) == 1);
    }
    if (failures)
        return 1;
    puts("recomp_d3d_service: ok");
    return 0;
#endif
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
        CHECK(recomp_d3d_refused(RECOMP_D3D_REFUSED_PENDING) == 1);
        M32(dev + 0x50) = 0x12345678u;
        CHECK(g_hook(1u << 5 | 0xDu) == 1 && M32(dev + 0x50) == 0x12345678u);
        CHECK(recomp_d3d_refused(RECOMP_D3D_REFUSED_PENDING) == 2);
        M32(dev + 0x50) = 3;
        CHECK(g_hook(1u << 5 | 0xDu) == 1 && M32(dev + 0x50) == 2);
        CHECK(recomp_d3d_refused(RECOMP_D3D_REFUSED_PENDING) == 2);
        /* 0xE leaves the count alone: nothing to refuse. */
        M32(dev + 0x50) = 0;
        CHECK(g_hook(0x00030000u << 5 | 0xEu) == 1 && recomp_d3d_refused(RECOMP_D3D_REFUSED_PENDING) == 2);
        CHECK(strstr(g_fallbacks, "D3D8LTCG 5933 rpb_pending=0x50;") != NULL);
        CHECK(strstr(g_fallbacks, "D3D8LTCG 5933 rpb_nodes=0x264;") != NULL);
        test_fixups(dev, 0);
        test_guards(dev, 0);
        CHECK(recomp_d3d_refused(RECOMP_D3D_REFUSED_PENDING) >= 2);   /* test_fixups runs the count down to 0 */
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
        CHECK(recomp_d3d_refused(RECOMP_D3D_REFUSED_SLOT) == 1);
        /* D3D8LTCG 5849's RunPushBuffer offsets are in the table: nothing fell back. */
        CHECK(g_fallbacks[0] == 0);
        /* A confirmed layout: the pending count decremented as D3D's handler
         * does, whatever it reads (nothing to refuse). */
        M32(dev + 0x50) = 0;
        M32(node + 8) = 0x80020000u;
        CHECK(g_hook(2u << 5 | 0xDu) == 1 && M32(dev + 0x50) == 0xFFFFFFFFu);
        CHECK(recomp_d3d_refused(RECOMP_D3D_REFUSED_PENDING) == 0);
#endif
    }
#ifdef TEST_GIVEN
    test_fixups(0x0085FDE0, 1);
    test_guards(0x0085FDE0, 1);
#else
    test_fixups(0x0085FDE0, 0);
    test_guards(0x0085FDE0, 0);
#endif
    if (failures)
        return 1;
    puts("recomp_d3d_service: ok");
    return 0;
}
#endif
