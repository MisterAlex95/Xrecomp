/*
 * Xbox save states, part 1 (XBA-245): the guest state capture and the XBXS
 * format, through the path a title takes (PsCreateSystemThreadEx and
 * KeDelayExecutionThread by ordinal) against a real guest window (the
 * synthetic XBE).
 *
 * Game-shaped guest workers, as in tests/kernel_park (our synthetic XBE has no
 * runnable code): a "render" thread working 4 ms and sleeping 12 ms, an
 * "audio" thread working 0.3 ms every 5 ms, a "loader" sleeping 50 ms then
 * working 8 ms without a kernel call.
 *
 *   Capture       freeze, capture, thaw: CPU, MEM and RAM sections; every
 *                 thread in KeDelayExecutionThread from its own return
 *                 address, its registers consistent with guest memory (the
 *                 return address at esp in the RAM copy, or the call's
 *                 status once it returned), its KTHREAD and guest stack
 *   RAM           a page written before the capture comes back byte for
 *                 byte, a zero page is not stored; dirty pages: a second
 *                 capture in the same freeze finds none changed, one page
 *                 written finds one
 *   Slot file     written through recomp_states (console "xbox", key the
 *                 title ID, thumbnail) and read back, checked section by
 *                 section
 *   Refused       truncated, a flipped byte in a section or the header, a
 *                 bad magic (corrupt); another build ID, another title
 *                 (foreign); an unknown, missing or re-versioned section
 *   Sections      a registered section is written and checked; a failing
 *                 save fails the capture
 *   Retry         a call returning during the capture (its record changes)
 *                 makes the capture run again, and the second one records
 *                 the call as returned with its status
 *   Host waits    (XBA-251) a thread spinning in a marked poll loop and one
 *                 blocked in the PFB flush kick (xbox_GuestPollWait ->
 *                 xbox_Nv2aKickWait), with no kernel call: the capture
 *                 freezes them and writes each as a host-wait park record
 *                 (CPU v2: site kind, ordinal 0, the site's guest VA); the
 *                 reader refuses an unknown site and a host wait with an
 *                 ordinal
 *   Load          (XBA-249) a state captured, then RAM, kernel objects, the
 *                 heap and an NV2A register changed: the same-run load puts
 *                 them back and every thread returns from its call with the
 *                 state's registers; refused with nothing changed: a thread
 *                 at another call site, created or ended since, a state of
 *                 another run; ten round trips; rewind's XOR-delta ring and
 *                 steps back (measured); the recomp_states front end (slots,
 *                 a refused load, the quick save chord); a state with
 *                 threads at host waits loads
 *   Refusals      a thread that never makes a kernel call (freeze times out
 *                 and names it); a capture from a guest thread
 *   Measures      capture time and blob size (printed)
 *   Probe         RECOMP_STATE_PROBE writes a slot file that reads back
 */
#include "kernel.h"
#include "kernel_park.h"
#include "xbox_memory_layout.h"
#include "xbox_state.h"
#include "platform/recomp_states.h"
#include "platform/recomp_rewind.h"
#include "platform/recomp_hotkeys.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef void (*recomp_func_t)(void);
static void worker(void);
#define WORKER_VA 0x00010000u
recomp_func_t recomp_lookup(uint32_t va) { return va == WORKER_VA ? worker : NULL; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }

extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
extern ptrdiff_t g_xbox_mem_offset;

#define G32(va) (*(volatile uint32_t *)((uintptr_t)(va) + g_xbox_mem_offset))
#define GPTR(va) ((uint8_t *)((uintptr_t)(va) + g_xbox_mem_offset))

enum { ORD_DELAY = 99, ORD_NTCLOSE = 187, ORD_NTCREATEEVENT = 189, ORD_NTSETEVENT = 225, ORD_PSCREATE = 255,
       ORD_PSTERM = 258 };
static const uint32_t ORDS[] = { ORD_DELAY, ORD_NTCLOSE, ORD_PSCREATE, ORD_PSTERM, ORD_NTCREATEEVENT,
                                 ORD_NTSETEVENT };
#define NORDS (sizeof ORDS / sizeof ORDS[0])

enum { K_RENDER, K_AUDIO, K_LOADER, K_SPIN, K_POLL, K_KICK, K_SWITCH, K_TEMP, K_COUNT };
#define RET_VA(kind) (0x00011000u + 0x10u * (uint32_t)(kind))
#define POLL_VA 0x0001A100u     /* the host-wait sites lifted code passes (XBA-251) */
#define KICK_VA 0x0001A200u
#define PFB_FLUSH 0x00100410u
#define FLUSH_BIT 0x00010000u
/* RECOMP_BACK_EDGE (--back-edge-yield): hand the guest CPU over when a waiter asked. */
#define BACK_EDGE(va) \
    do { if (__atomic_load_n(&g_xbox_guest_cpu_preempt, __ATOMIC_RELAXED)) xbox_GuestCpuPreempt(va); } while (0)

static int failures;
static uint32_t g_mark;                    /* guest_call's esi marks */
static volatile uint32_t g_back_esi[16];   /* per call site: the older mark a load gave back */
static volatile int g_site, g_switch_stop, g_temp_stop;   /* K_SWITCH: which call site, stop; K_TEMP: stop */
static uint32_t g_thunks, g_stack, g_handles, g_pattern;
static volatile int g_stop, g_spin_stop, g_hw_stop;
static uint32_t g_pollword;
static volatile unsigned long g_progress;

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-66s %s", what, ok ? "PASS" : "FAIL");
    if (!ok && detail) printf(" -- %s", detail);
    putchar('\n');
    if (!ok) failures++;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static recomp_func_t thunk(uint32_t ordinal)
{
    unsigned i;
    for (i = 0; i < NORDS; i++)
        if (ORDS[i] == ordinal)
            return recomp_lookup_kernel(G32(g_thunks + 4 * i));
    return NULL;
}

static uint32_t guest_call(uint32_t ordinal, uint32_t ret_va, int argc, const uint32_t *argv)
{
    recomp_func_t fn = thunk(ordinal);
    uint32_t sp0 = g_esp, sp = (sp0 - 64u) & ~15u;
    int i;

    if (!fn) {
        printf("  ordinal %u did not resolve\n", ordinal);
        failures++;
        return 0xFFFFFFFFu;
    }
    G32(sp) = ret_va;
    for (i = 0; i < argc; i++)
        G32(sp + 4 + 4 * (uint32_t)i) = argv[i];
    g_esp = sp;
    g_eax = 0xDEADBEEFu;
    g_ebx = 0xB0000000u | ret_va;   /* a callee-saved register the state must carry */
    {
        /* esi: a mark unique to this call. A same-run load (XBA-249) hands the thread the registers of the
         * state's call, so the call returns with an older mark: noted per call site. */
        uint32_t mark = __atomic_add_fetch(&g_mark, 1, __ATOMIC_RELAXED), k = (ret_va - 0x00011000u) / 0x10u;
        g_esi = mark;
        fn();
        if (g_esi != mark && k < 16)
            g_back_esi[k] = g_esi;
    }
    g_esp = sp0;
    return g_eax;
}
#define GCALL(ord, ret, ...) guest_call((ord), (ret), \
    (int)(sizeof((uint32_t[]){__VA_ARGS__}) / 4), (uint32_t[]){__VA_ARGS__})

static void guest_sleep(int kind, uint32_t ms)
{
    uint32_t iv = (g_esp - 256u) & ~7u;
    int64_t rel = -(int64_t)ms * 10000;
    G32(iv) = (uint32_t)rel;
    G32(iv + 4) = (uint32_t)((uint64_t)rel >> 32);
    GCALL(ORD_DELAY, RET_VA(kind), 0, 0, iv);
}

static void switch_sleep_other(void)   /* K_SWITCH's other call site: RET_VA(K_SWITCH) + 8 */
{
    uint32_t iv = (g_esp - 256u) & ~7u;
    int64_t rel = -40000;
    G32(iv) = (uint32_t)rel;
    G32(iv + 4) = (uint32_t)((uint64_t)rel >> 32);
    GCALL(ORD_DELAY, RET_VA(K_SWITCH) + 8u, 0, 0, iv);
}

static void guest_work(uint64_t us)
{
    uint64_t end = now_ns() + us * 1000u;
    while (now_ns() < end)
        __atomic_add_fetch(&g_progress, 1, __ATOMIC_RELAXED);
}

static void worker(void)
{
    int kind = (int)G32(g_esp + 4);

    switch (kind) {
    case K_RENDER:
        while (!g_stop) { guest_work(4000); guest_sleep(kind, 12); }
        break;
    case K_AUDIO:
        while (!g_stop) { guest_work(300); guest_sleep(kind, 5); }
        break;
    case K_LOADER:
        while (!g_stop) { guest_sleep(kind, 50); guest_work(8000); }
        break;
    case K_SWITCH:  /* sleeps from one call site or another (g_site) */
        while (!g_stop && !g_switch_stop) {
            guest_work(500);
            if (g_site)
                switch_sleep_other();
            else
                guest_sleep(kind, 4);
        }
        break;
    case K_TEMP:
        while (!g_temp_stop) { guest_work(200); guest_sleep(kind, 3); }
        break;
    case K_SPIN:
        while (!g_spin_stop)
            __atomic_add_fetch(&g_progress, 1, __ATOMIC_RELAXED);
        break;
    case K_POLL:    /* `test [w], 1; je $` with RECOMP_POLL_WAIT on the back edge */
        g_ebx = 0xB0000000u | POLL_VA;
        while (!(G32(g_pollword) & 1u) && !g_hw_stop) {
            xbox_GuestPollWait(&G32(g_pollword), 1u, 0, POLL_VA);
            BACK_EDGE(POLL_VA);
        }
        while (!g_hw_stop)
            guest_sleep(kind, 2);
        break;
    case K_KICK: {  /* D3D's KickOff */
        volatile uint32_t *kick = xbox_Nv2aRegPtr(PFB_FLUSH);
        while (!g_hw_stop && kick) {
            g_ebx = 0xB0000000u | KICK_VA;
            *kick |= FLUSH_BIT;
            while (*kick & FLUSH_BIT)
                xbox_GuestPollWait(kick, FLUSH_BIT, 1, KICK_VA);
            BACK_EDGE(KICK_VA);
        }
        break;
    }
    }
}

static void spawn(int i, int kind)
{
    uint32_t hva = g_handles + 4u * (uint32_t)i;
    uint32_t argv[10] = { hva, 0, 0, 0, 0, (uint32_t)kind, 0, 0, 0, WORKER_VA };
    recomp_func_t fn = thunk(ORD_PSCREATE);
    unsigned k;

    G32(g_stack) = 0xBEEF0001u;
    for (k = 0; k < 10; k++)
        G32(g_stack + 4 + 4 * k) = argv[k];
    g_esp = g_stack;
    fn();
}

static int wait_threads(int n, int ms)
{
    XboxParkStats st;
    int t;
    for (t = 0; t < ms; t++) {
        xbox_ParkStatsGet(&st, 0);
        if (st.threads == n)
            return 1;
        usleep(1000);
    }
    return 0;
}

static int kind_of_ret(uint32_t ret)
{
    int k;
    for (k = 0; k < K_COUNT; k++)
        if (ret == RET_VA(k))
            return k;
    return -1;
}

static unsigned char *load(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    unsigned char *b;
    long len;

    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    rewind(f);
    b = malloc((size_t)len);
    if (b && fread(b, 1, (size_t)len, f) != (size_t)len) { free(b); b = NULL; }
    fclose(f);
    *n = (size_t)len;
    return b;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* A blob with the byte at `at` flipped. */
static int check_flipped(const uint8_t *b, size_t n, size_t at)
{
    uint8_t *c = malloc(n);
    int rc;
    memcpy(c, b, n);
    c[at] ^= 0x40;
    rc = xbox_StateCheck(c, n, NULL);
    free(c);
    return rc;
}

/* ---- A test section ------------------------------------------------------------------------------------- */

static int s_test_saves, s_test_fail;

static int test_save(XboxStateWriter *w, const XboxStateContext *ctx, void *user)
{
    (void)user;
    s_test_saves++;
    if (s_test_fail)
        return XBOX_STATE_ECORRUPT;
    xbox_StatePut32(w, 0x54455354u);
    xbox_StatePut32(w, (uint32_t)ctx->threads);
    xbox_StatePut64(w, 0x0123456789ABCDEFull);
    return XBOX_STATE_OK;
}

static int test_check(const uint8_t *p, size_t n, uint32_t version, void *user)
{
    (void)user; (void)version;
    return n == 16 && rd32(p) == 0x54455354u ? XBOX_STATE_OK : XBOX_STATE_ECORRUPT;
}

#define TAG_TEST XBOX_STATE_TAG('T', 'E', 'S', 'T')

/* Pages that differ between two blobs' RAM sections, walked by their documented format (ranges in the
 * same order with the same bases); *first the first such page, *mine how many of the two pattern pages. */
static const uint8_t *ram_page_at(const uint8_t *bits, const uint8_t *data, uint32_t pg)
{
    uint32_t i, before = 0;
    if (!((bits[pg >> 3] >> (pg & 7)) & 1u))
        return NULL;
    for (i = 0; i < pg; i++)
        before += (bits[i >> 3] >> (i & 7)) & 1u;
    return data + (size_t)before * 4096;
}

static uint32_t ram_diff(const uint8_t *b1, size_t n1, const uint8_t *b2, size_t n2, uint32_t *first, int *mine)
{
    static const uint8_t zero[4096];
    const uint8_t *p1, *p2;
    size_t l1, l2;
    uint32_t count, r, diff = 0;

    *first = 0;
    *mine = 0;
    xbox_StateSection(b1, n1, XBOX_STATE_TAG_RAM, &p1, &l1, NULL);
    xbox_StateSection(b2, n2, XBOX_STATE_TAG_RAM, &p2, &l2, NULL);
    count = rd32(p1);
    if (count != rd32(p2))
        return 0xFFFFFFFFu;
    p1 += 4;
    p2 += 4;
    for (r = 0; r < count; r++) {
        uint32_t va = rd32(p1 + 4), pages = rd32(p1 + 8), pg, s1 = 0, s2 = 0;
        const uint8_t *bits1 = p1 + 12, *bits2 = p2 + 12, *d1, *d2;
        if (va != rd32(p2 + 4) || pages != rd32(p2 + 8))
            return 0xFFFFFFFFu;
        d1 = bits1 + (pages + 7) / 8;
        d2 = bits2 + (pages + 7) / 8;
        for (pg = 0; pg < pages; pg++) {
            s1 += (bits1[pg >> 3] >> (pg & 7)) & 1u;
            s2 += (bits2[pg >> 3] >> (pg & 7)) & 1u;
        }
        for (pg = 0; pg < pages; pg++) {
            const uint8_t *a = ram_page_at(bits1, d1, pg), *b = ram_page_at(bits2, d2, pg);
            if (memcmp(a ? a : zero, b ? b : zero, 4096) != 0) {
                uint32_t at = va + pg * 4096u;
                if (!diff)
                    *first = at;
                diff++;
                if (at == g_pattern || at == g_pattern + 4096)
                    (*mine)++;
            }
        }
        p1 = d1 + (size_t)s1 * 4096;
        p2 = d2 + (size_t)s2 * 4096;
    }
    return diff;
}

/* ---- Phases --------------------------------------------------------------------------------------------- */

static uint8_t *g_blob;
static size_t g_blob_n;

static void phase_capture(void)
{
    XboxStateStats st;
    XboxStateInfo info;
    XboxStateThread th[XBOX_PARK_MAX_THREADS];
    const uint8_t *p;
    size_t len;
    uint32_t ver;
    int rc, n, k, sites = 1, regs = 1, kt = 1, stacks = 1;
    char d[256];

    printf("Capture (three game-shaped threads running)\n");
    rc = xbox_StateCapture(200, &g_blob, &g_blob_n, &st);
    snprintf(d, sizeof d, "%s: %s", xbox_StateError(rc), st.reason);
    check(rc == XBOX_STATE_OK && g_blob && g_blob_n > XBOX_STATE_HEADER_BYTES, "a capture is taken", d);
    if (rc != XBOX_STATE_OK)
        return;
    printf("  capture %.2f ms (freeze %.2f, settle %.2f, sections %.2f, %d attempt(s)); blob %.2f MB:"
           " CPU %zu B, MEM %zu B, RAM %zu B (%u of %u pages stored)\n",
           st.freeze_ms + st.settle_ms + st.capture_ms, st.freeze_ms, st.settle_ms, st.capture_ms, st.attempts,
           g_blob_n / 1048576.0, st.cpu_bytes, st.mem_bytes, st.ram_bytes, st.ram_stored, st.ram_pages);
    rc = xbox_StateCheck(g_blob, g_blob_n, &info);
    snprintf(d, sizeof d, "%s: %s", xbox_StateError(rc), info.reason);
    check(rc == XBOX_STATE_OK, "it reads back: identity and every section check", d);
    snprintf(d, sizeof d, "%d sections", info.sections);
    check(info.sections == 7 && info.section[0].tag == XBOX_STATE_TAG_CPU
          && info.section[1].tag == XBOX_STATE_TAG_MEM && info.section[2].tag == XBOX_STATE_TAG_RAM
          && info.section[3].tag == XBOX_STATE_TAG_KOBJ && info.section[4].tag == XBOX_STATE_TAG_NV2A
          && info.section[5].tag == XBOX_STATE_TAG_APU && info.section[6].tag == XBOX_STATE_TAG_USB,
          "sections CPU, MEM, RAM, KOBJ, NV2A, APU, USB in that order", d);
    snprintf(d, sizeof d, "title %08X, toolkit %s, build id %u bytes", info.id.title_id, info.id.toolkit,
             info.id.build_id_len);
    printf("  identity: %s\n", d);
    check(info.id.title_id == xbox_path_title_id() && info.id.cpu_bytes == sizeof(RecompCpuRegs)
          && info.id.toolkit[0] && strcmp(info.id.toolkit, "unknown") != 0,
          "identity: the title ID, the toolkit version, sizeof(RecompCpuRegs)", d);

    if (xbox_StateSection(g_blob, g_blob_n, XBOX_STATE_TAG_CPU, &p, &len, &ver) != XBOX_STATE_OK) {
        check(0, "CPU section found", NULL);
        return;
    }
    n = xbox_StateCpuThreads(p, len, th, XBOX_PARK_MAX_THREADS);
    snprintf(d, sizeof d, "%d threads", n);
    check(n == 3 && st.threads == 3, "CPU: three guest threads", d);
    for (k = 0; k < n; k++) {
        const uint8_t *page;
        int kind = kind_of_ret(th[k].ret_va);
        uint32_t esp = th[k].cpu.r_esp;
        if (th[k].ordinal != ORD_DELAY || kind < 0 || kind > K_LOADER)
            sites = 0;
        if (!th[k].returned) {
            /* In the call: esp at the return address, in the captured RAM. */
            {
                const uint8_t *rp;
                size_t rl;
                xbox_StateSection(g_blob, g_blob_n, XBOX_STATE_TAG_RAM, &rp, &rl, NULL);
                if (xbox_StateRamPage(rp, rl, esp & ~0xFFFu, &page) != 1 || !page
                    || rd32(page + (esp & 0xFFFu)) != th[k].ret_va)
                    regs = 0;
            }
        } else if (th[k].status != 0) {
            regs = 0;               /* KeDelayExecutionThread: STATUS_SUCCESS */
        }
        if (th[k].cpu.r_ebx != (0xB0000000u | th[k].ret_va))
            regs = 0;
        if (!th[k].tib || !th[k].kthread || !th[k].guest_tid)
            kt = 0;
        if (!(th[k].stack_limit < esp && esp <= th[k].stack_base))
            stacks = 0;
        printf("  thread %u (guest id %u): %s ordinal %u from 0x%08X, esp 0x%08X in stack [0x%08X, 0x%08X),"
               " ebx 0x%08X, KTHREAD 0x%08X\n", th[k].tid, th[k].guest_tid,
               th[k].returned ? "returned from" : "in", th[k].ordinal, th[k].ret_va, esp, th[k].stack_limit,
               th[k].stack_base, th[k].cpu.r_ebx, th[k].kthread);
    }
    check(sites, "each in KeDelayExecutionThread from its own return address", NULL);
    check(regs, "registers match guest memory (return address at esp) or the status", NULL);
    check(kt, "TIB, KTHREAD and guest thread id recorded", NULL);
    check(stacks, "esp inside the guest stack bounds from the KTHREAD", NULL);

    {
        XboxMemStateInfo mi;
        xbox_StateSection(g_blob, g_blob_n, XBOX_STATE_TAG_MEM, &p, &len, NULL);
        rc = xbox_MemStateCheck(p, len, &mi);
        snprintf(d, sizeof d, "heap next 0x%08X, %u blocks (%u live), contig %u pages in use", mi.heap_next,
                 mi.heap_blocks, mi.heap_live, mi.contig_in_use);
        printf("  MEM: %s\n", d);
        check(rc == 0 && mi.heap_next > g_pattern && mi.heap_live >= 4, "MEM: the heap's cursor and blocks", d);
    }
}

static void phase_ram(void)
{
    const uint8_t *p, *page;
    size_t len;
    XboxStateStats st;
    uint8_t *b2 = NULL, *b3 = NULL;
    size_t n2 = 0, n3 = 0;
    char d[200];
    int ok;

    printf("RAM\n");
    if (!g_blob)
        return;
    xbox_StateSection(g_blob, g_blob_n, XBOX_STATE_TAG_RAM, &p, &len, NULL);
    ok = xbox_StateRamPage(p, len, g_pattern, &page) == 1 && page && memcmp(page, GPTR(g_pattern), 4096) == 0;
    check(ok, "a page written before the capture comes back byte for byte", NULL);
    ok = xbox_StateRamPage(p, len, g_pattern + 4096, &page) == 1 && page == NULL;
    check(ok, "an all-zero page is not stored", NULL);
    ok = xbox_StateRamPage(p, len, XBOX_CONTIG_BASE - 0x10000000u, &page) == 0;
    check(ok, "an address no range covers is not found", NULL);

    /* Dirty pages, within one freeze. Host threads (the timer thread's clock, the device models) still
     * write guest memory, so the count is checked against the pages that really differ between the two
     * blobs, and the two pages written here must be among them. */
    if (!xbox_ParkFreeze(200, NULL)) {
        check(0, "freeze for the dirty-page captures", NULL);
        return;
    }
    memset(&st, 0, sizeof st);
    xbox_StateCaptureFrozen(&b2, &n2, &st);
    GPTR(g_pattern)[100] ^= 0xFF;
    GPTR(g_pattern + 4096)[7] = 1;
    xbox_StateCaptureFrozen(&b3, &n3, &st);
    xbox_ParkThaw();
    GPTR(g_pattern + 4096)[7] = 0;
    GPTR(g_pattern)[100] ^= 0xFF;
    if (b2 && b3) {
        uint32_t diff, va;
        int mine = 0;
        diff = ram_diff(b2, n2, b3, n3, &va, &mine);
        snprintf(d, sizeof d, "%u counted, %u differ (first at 0x%08X), %d of 2 written", st.ram_changed, diff,
                 va, mine);
        printf("  dirty pages: %s\n", d);
        check(st.ram_changed == diff && mine == 2,
              "dirty pages: the count matches the pages that differ, the two written among them", d);
    } else {
        check(0, "two captures in one freeze", NULL);
    }
    free(b2);
    free(b3);
}

static void phase_slot(const char *dir)
{
    char path[512], d[256];
    uint16_t thumb[RECOMP_STATE_THUMB_W * RECOMP_STATE_THUMB_H];
    uint16_t back[RECOMP_STATE_THUMB_W * RECOMP_STATE_THUMB_H];
    recomp_state_meta meta;
    XboxStateInfo info;
    XboxStateStats st;
    uint8_t *b = NULL;
    size_t n = 0;
    int rc, i;
    char key[16];

    printf("Slot file\n");
    for (i = 0; i < 64 * 48; i++)
        thumb[i] = (uint16_t)(i * 37);
    snprintf(path, sizeof path, "%s/state1.st", dir);
    rc = xbox_StateSaveSlot(path, 200, thumb, 64, 48, &st);
    snprintf(d, sizeof d, "%s: %s", xbox_StateError(rc), st.reason);
    check(rc == XBOX_STATE_OK, "capture and write a slot file", d);
    rc = recomp_states_read(path, &meta, back, NULL, NULL);
    snprintf(key, sizeof key, "%08X", xbox_path_title_id());
    snprintf(d, sizeof d, "rc %d, console %s, key %s", rc, meta.console, meta.key);
    check(rc == RECOMP_STATE_OK && !strcmp(meta.console, "xbox") && !strcmp(meta.key, key)
          && meta.thumb_w == 64 && meta.thumb_h == 48 && !memcmp(back, thumb, 64 * 48 * 2),
          "its XRSLOT header: console xbox, key the title ID, thumbnail", d);
    rc = xbox_StateReadSlot(path, &b, &n, &info);
    snprintf(d, sizeof d, "%s: %s", xbox_StateError(rc), info.reason);
    check(rc == XBOX_STATE_OK && b && n == st.blob_bytes, "read back and checked section by section", d);
    free(b);
    snprintf(path, sizeof path, "%s/state2.st", dir);
    rc = xbox_StateReadSlot(path, NULL, NULL, &info);
    check(rc == XBOX_STATE_EEMPTY, "an empty slot says so", NULL);
}

static void phase_refused(void)
{
    XboxStateInfo info;
    uint8_t *c;
    int rc;
    const uint8_t *p;
    size_t len;
    char d[256];

    printf("Refused\n");
    if (!g_blob)
        return;
    rc = xbox_StateCheck(g_blob, g_blob_n - 1, NULL);
    check(rc == XBOX_STATE_ECORRUPT, "truncated by one byte: corrupt", xbox_StateError(rc));
    rc = xbox_StateCheck(g_blob, 64, NULL);
    check(rc == XBOX_STATE_ECORRUPT, "truncated to 64 bytes: corrupt", xbox_StateError(rc));
    xbox_StateSection(g_blob, g_blob_n, XBOX_STATE_TAG_RAM, &p, &len, NULL);
    rc = check_flipped(g_blob, g_blob_n, (size_t)(p - g_blob) + len / 2);
    check(rc == XBOX_STATE_ECORRUPT, "a byte flipped in RAM: corrupt (section hash)", xbox_StateError(rc));
    xbox_StateSection(g_blob, g_blob_n, XBOX_STATE_TAG_CPU, &p, &len, NULL);
    rc = check_flipped(g_blob, g_blob_n, (size_t)(p - g_blob) + 20);
    check(rc == XBOX_STATE_ECORRUPT, "a byte flipped in CPU: corrupt", xbox_StateError(rc));
    rc = check_flipped(g_blob, g_blob_n, 30);
    check(rc == XBOX_STATE_ECORRUPT, "a byte flipped in the header: corrupt (header hash)", xbox_StateError(rc));
    rc = check_flipped(g_blob, g_blob_n, 0);
    check(rc == XBOX_STATE_ECORRUPT, "not XBXS: corrupt", xbox_StateError(rc));

    /* Another title: the header rewritten with a valid hash. */
    c = malloc(g_blob_n);
    memcpy(c, g_blob, g_blob_n);
    c[16] ^= 1;
    {
        uint64_t h = xbox_StateHash(c, 120);
        int i;
        for (i = 0; i < 8; i++)
            c[120 + i] = (uint8_t)(h >> (8 * i));
    }
    rc = xbox_StateCheck(c, g_blob_n, &info);
    snprintf(d, sizeof d, "%s: %s", xbox_StateError(rc), info.reason);
    check(rc == XBOX_STATE_EGAME, "another title ID: another game", d);
    /* Another format version (same trick). */
    memcpy(c, g_blob, g_blob_n);
    c[4] = 2;
    rc = xbox_StateCheck(c, g_blob_n, &info);
    check(rc == XBOX_STATE_EVERSION, "another format version: another version", xbox_StateError(rc));
    free(c);

    /* Another build. */
    xbox_StateSetBuildId((const uint8_t *)"another build of the code", 25);
    rc = xbox_StateCheck(g_blob, g_blob_n, &info);
    snprintf(d, sizeof d, "%s: %s", xbox_StateError(rc), info.reason);
    check(rc == XBOX_STATE_EVERSION, "another build ID: another version", d);
    xbox_StateSetBuildId(NULL, 0);
    rc = xbox_StateCheck(g_blob, g_blob_n, NULL);
    check(rc == XBOX_STATE_OK, "the binary's own build ID again: accepted", xbox_StateError(rc));
}

static void phase_sections(void)
{
    XboxStateSection s = { TAG_TEST, 1, "test", test_save, test_check, NULL, NULL };
    XboxStateInfo info;
    XboxStateStats st;
    uint8_t *b = NULL;
    size_t n = 0;
    int rc;
    const uint8_t *p;
    size_t len;
    char d[256];

    printf("Sections\n");
    check(xbox_StateRegister(&s) == 0, "a component registers a section", NULL);
    rc = xbox_StateCapture(200, &b, &n, &st);
    check(rc == XBOX_STATE_OK && s_test_saves >= 1, "it is saved during the capture", st.reason);
    rc = xbox_StateCheck(b, n, &info);
    check(rc == XBOX_STATE_OK && info.sections == 8 && info.section[7].tag == TAG_TEST
          && xbox_StateSection(b, n, TAG_TEST, &p, &len, NULL) == XBOX_STATE_OK && len == 16,
          "written after the built-ins, checked, found by tag", info.reason);
    rc = xbox_StateCheck(g_blob, g_blob_n, &info);
    snprintf(d, sizeof d, "%s: %s", xbox_StateError(rc), info.reason);
    check(rc == XBOX_STATE_EVERSION, "an older blob without it: another version (missing)", d);
    s.version = 2;
    xbox_StateRegister(&s);
    rc = xbox_StateCheck(b, n, &info);
    snprintf(d, sizeof d, "%s: %s", xbox_StateError(rc), info.reason);
    check(rc == XBOX_STATE_EVERSION, "its version bumped: another version", d);
    xbox_StateUnregister(TAG_TEST);
    rc = xbox_StateCheck(b, n, &info);
    snprintf(d, sizeof d, "%s: %s", xbox_StateError(rc), info.reason);
    check(rc == XBOX_STATE_EVERSION, "unregistered: the blob's section is unknown", d);
    free(b);
    b = NULL;
    s.version = 1;
    s_test_fail = 1;
    xbox_StateRegister(&s);
    rc = xbox_StateCapture(200, &b, &n, &st);
    snprintf(d, sizeof d, "%s: %s", xbox_StateError(rc), st.reason);
    check(rc == XBOX_STATE_ESECTION && !b && strstr(st.reason, "TEST"), "a failing save fails the capture,"
          " naming it", d);
    xbox_StateUnregister(TAG_TEST);
    s_test_fail = 0;
    {
        XboxParkStats ps;
        xbox_ParkStatsGet(&ps, 0);
        check(!ps.frozen, "and the threads run again", NULL);
    }
}

/* ---- Retry: a call returns during the capture ------------------------------------------------------------ */

static volatile int s_fake_go, s_fake_done, s_fake_slot = -1;
static int s_retry_saves;

static void *fake_thread(void *arg)
{
    XboxParkRecord r[XBOX_PARK_MAX_THREADS];
    int n, i;
    (void)arg;

    g_esp = g_stack - 0x400u;
    g_eax = 0x11111111u;
    g_ebx = 0xFEEDF00Du;
    xbox_ParkThreadStart();
    xbox_ParkKernelEnter(ORD_DELAY, 0x00012340u);
    n = xbox_ParkRecords(r, XBOX_PARK_MAX_THREADS);
    for (i = 0; i < n; i++)
        if (r[i].ret_va == 0x00012340u)
            s_fake_slot = r[i].slot;
    while (!s_fake_go)
        usleep(100);
    g_eax = 0x00000102u;          /* the "wait" returns STATUS_TIMEOUT */
    g_esp += 16;
    xbox_ParkKernelLeave();       /* rewrites the record, then the freeze holds it */
    xbox_ParkThreadEnd();
    s_fake_done = 1;
    return NULL;
}

static int retry_save(XboxStateWriter *w, const XboxStateContext *ctx, void *user)
{
    (void)ctx; (void)user;
    if (s_retry_saves++ == 0) {
        XboxParkRecord r[XBOX_PARK_MAX_THREADS];
        uint64_t end = now_ns() + 2000000000ull;
        int n, i, seen = 0;
        s_fake_go = 1;
        while (!seen && now_ns() < end) {       /* until its Leave has rewritten its record */
            n = xbox_ParkRecords(r, XBOX_PARK_MAX_THREADS);
            for (i = 0; i < n; i++)
                if (r[i].slot == s_fake_slot && r[i].returned)
                    seen = 1;
            usleep(200);
        }
    }
    xbox_StatePut32(w, 1);
    return XBOX_STATE_OK;
}

static void phase_retry(void)
{
    XboxStateSection s = { XBOX_STATE_TAG('R', 'T', 'R', 'Y'), 1, "retry", retry_save, NULL, NULL, NULL };
    XboxStateThread th[XBOX_PARK_MAX_THREADS];
    XboxStateStats st;
    pthread_t t;
    uint8_t *b = NULL;
    size_t n = 0, len;
    const uint8_t *p;
    int rc, k, cnt, found = 0;
    char d[256];

    printf("A call returns during the capture\n");
    pthread_create(&t, NULL, fake_thread, NULL);
    check(wait_threads(4, 2000) && s_fake_slot >= 0, "a fourth thread parks in a call", NULL);
    xbox_StateRegister(&s);
    rc = xbox_StateCapture(200, &b, &n, &st);
    snprintf(d, sizeof d, "%s, %d attempts: %s", xbox_StateError(rc), st.attempts, st.reason);
    /* At least twice: the other threads' sleeps can end during an attempt too. */
    check(rc == XBOX_STATE_OK && st.attempts >= 2, "the capture runs again", d);
    if (b && xbox_StateSection(b, n, XBOX_STATE_TAG_CPU, &p, &len, NULL) == XBOX_STATE_OK) {
        cnt = xbox_StateCpuThreads(p, len, th, XBOX_PARK_MAX_THREADS);
        for (k = 0; k < cnt; k++)
            if (th[k].slot == s_fake_slot) {
                found = th[k].returned && th[k].status == 0x102u && th[k].ret_va == 0x00012340u
                     && th[k].cpu.r_ebx == 0xFEEDF00Du && th[k].state == XBOX_PARK_HELD;
                snprintf(d, sizeof d, "returned %d, status 0x%X, state %d", th[k].returned, th[k].status,
                         th[k].state);
            }
    }
    check(found, "and records that call as returned, with its status, held", d);
    xbox_StateUnregister(s.tag);
    free(b);
    pthread_join(t, NULL);
    check(s_fake_done && wait_threads(3, 2000), "the thread runs on after the thaw", NULL);
}

static void phase_host_waits(void)
{
    XboxStateThread th[XBOX_PARK_MAX_THREADS];
    XboxStateInfo info;
    XboxStateStats st;
    uint8_t *b = NULL, *copy;
    size_t n = 0, len;
    const uint8_t *p;
    uint32_t ver = 0;
    int rc, k, cnt = 0, poll = 0, kick = 0, regs = 1, kernel = 1;
    char d[256];

    printf("Host waits inside lifted code (a poll loop, an NV2A kick wait)\n");
    if (!xbox_Nv2aRegPtr(PFB_FLUSH)) {
        check(0, "NV2A register file mapped", NULL);
        return;
    }
    G32(g_pollword) = 0;
    spawn(4, K_POLL);
    spawn(5, K_KICK);
    check(wait_threads(5, 2000), "two threads that make no kernel call start", NULL);
    usleep(20000);
    rc = xbox_StateCapture(200, &b, &n, &st);
    snprintf(d, sizeof d, "%s: %s", xbox_StateError(rc), st.reason);
    check(rc == XBOX_STATE_OK && b, "the capture freezes them and is taken", d);
    if (rc != XBOX_STATE_OK)
        goto out;
    rc = xbox_StateCheck(b, n, &info);
    check(rc == XBOX_STATE_OK, "it reads back", info.reason);
    if (xbox_StateSection(b, n, XBOX_STATE_TAG_CPU, &p, &len, &ver) != XBOX_STATE_OK) {
        check(0, "CPU section found", NULL);
        goto out;
    }
    check(ver == 4 && ver == XBOX_STATE_CPU_VERSION, "CPU section version 4 (the park site, the run, esp at"
          " the park point, the chain)", NULL);
    cnt = xbox_StateCpuThreads(p, len, th, XBOX_PARK_MAX_THREADS);
    for (k = 0; k < cnt; k++) {
        printf("  thread %u: %s %s, ordinal %u, 0x%08X, ebx 0x%08X\n", th[k].tid,
               th[k].returned ? "returned from" : "in", xbox_ParkSiteName(th[k].site), th[k].ordinal,
               th[k].ret_va, th[k].cpu.r_ebx);
        if (th[k].site == XBOX_PARK_SITE_KERNEL) {
            if (th[k].ordinal != ORD_DELAY)
                kernel = 0;
            continue;
        }
        if (th[k].ordinal != 0)
            regs = 0;
        if (th[k].ret_va == POLL_VA && (th[k].site == XBOX_PARK_SITE_POLL || th[k].site == XBOX_PARK_SITE_PREEMPT))
            poll++;
        else if (th[k].ret_va == KICK_VA
                 && (th[k].site == XBOX_PARK_SITE_KICK || th[k].site == XBOX_PARK_SITE_PREEMPT))
            kick++;
        else
            regs = 0;
        if (th[k].cpu.r_ebx != (0xB0000000u | th[k].ret_va) || !th[k].kthread
            || !(th[k].stack_limit < th[k].cpu.r_esp && th[k].cpu.r_esp <= th[k].stack_base))
            regs = 0;
    }
    snprintf(d, sizeof d, "%d threads, poll %d, kick %d", cnt, poll, kick);
    check(cnt == 5 && poll == 1 && kick == 1, "five threads: the poll and kick threads at their host waits", d);
    check(kernel, "the others in their kernel calls", NULL);
    check(regs, "host-wait records: ordinal 0, the site's VA, registers, KTHREAD, stack", NULL);
    {   /* XBA-249: a same-run load takes them back at their host waits (waiting again, or resuming) */
        XboxStateLoadStats ls;
        usleep(20000);
        rc = xbox_StateLoad(b, n, 500, &ls);
        snprintf(d, sizeof d, "%s: %s (%d made again)", xbox_StateError(rc), ls.reason, ls.reissued);
        check(rc == XBOX_STATE_OK, "the state loads with two threads at host waits", d);
    }

    /* The reader stays strict about the new field. */
    copy = malloc(len);
    if (copy && cnt > 0) {
        /* host routines, no lifted frame: every chain is empty (24 bytes), tests/park_chain has real ones */
        size_t head = 68 + sizeof(RecompCpuRegs) + 24, site_at = 16 + 64, ord_at = 16 + 8;
        int hw = -1, kn = -1;
        for (k = 0; k < cnt; k++) {
            if (th[k].site != XBOX_PARK_SITE_KERNEL && hw < 0) hw = k;
            if (th[k].site == XBOX_PARK_SITE_KERNEL && kn < 0) kn = k;
        }
        memcpy(copy, p, len);
        copy[site_at + (size_t)(hw >= 0 ? hw : 0) * head] = 9;
        check(xbox_StateCpuThreads(copy, len, NULL, 0) < 0, "a record with an unknown site is refused", NULL);
        if (hw >= 0) {
            memcpy(copy, p, len);
            copy[ord_at + (size_t)hw * head] = 99;
            check(xbox_StateCpuThreads(copy, len, NULL, 0) < 0, "a host-wait record with an ordinal is refused",
                  NULL);
        }
        if (kn >= 0) {
            memcpy(copy, p, len);
            copy[site_at + (size_t)kn * head] = XBOX_PARK_SITE_POLL;   /* a kernel call's ordinal with a host site */
            check(xbox_StateCpuThreads(copy, len, NULL, 0) < 0, "a kernel-call ordinal at a host-wait site is refused",
                  NULL);
        }
    }
    free(copy);
out:
    free(b);
    G32(g_pollword) = 1;
    g_hw_stop = 1;
    check(wait_threads(3, 3000), "they end", NULL);
}

/* ---- Same-run load (XBA-249) ------------------------------------------------------------------------------ */

static uint32_t create_event(void)
{
    uint32_t hva = g_handles + 0x80u, sp0 = g_esp, st;
    g_esp = g_stack - 0x800u;
    G32(hva) = 0;
    st = GCALL(ORD_NTCREATEEVENT, 0x00012000u, hva, 0, 0 /* NotificationEvent */, 0);
    g_esp = sp0;
    return st == 0 ? G32(hva) : 0;
}

/* NtSetEvent: 0 on a live event, an error status on a handle that names nothing (NtClose always succeeds). */
static uint32_t set_event(uint32_t h)
{
    uint32_t sp0 = g_esp, st;
    g_esp = g_stack - 0x800u;
    st = GCALL(ORD_NTSETEVENT, 0x00012020u, h, 0);
    g_esp = sp0;
    return st;
}

static uint32_t close_handle(uint32_t h)
{
    uint32_t sp0 = g_esp, st;
    g_esp = g_stack - 0x800u;
    st = GCALL(ORD_NTCLOSE, 0x00012010u, h);
    g_esp = sp0;
    return st;
}

/* The CPU section's run id changed, its hash recomputed: a state from another run. */
static uint8_t *other_run(const uint8_t *b, size_t n)
{
    uint8_t *c = malloc(n);
    const uint8_t *p;
    size_t len;
    uint64_t h;
    int i;
    if (!c) return NULL;
    memcpy(c, b, n);
    xbox_StateSection(b, n, XBOX_STATE_TAG_CPU, &p, &len, NULL);
    c[(p - b) + 8] ^= 0x5A;
    h = xbox_StateHash(c + (p - b), len);
    for (i = 0; i < 8; i++)
        c[(p - b) - 8 + i] = (uint8_t)(h >> (8 * i));
    return c;
}

static int wait_site(int site, int ms)
{
    /* Until K_SWITCH has made a call from that site (its record names it). */
    XboxParkRecord r[XBOX_PARK_MAX_THREADS];
    int t, n, i;
    for (t = 0; t < ms; t++) {
        n = xbox_ParkRecords(r, XBOX_PARK_MAX_THREADS);
        for (i = 0; i < n; i++)
            if (r[i].ret_va == RET_VA(K_SWITCH) + (site ? 8u : 0u))
                return 1;
        usleep(1000);
    }
    return 0;
}

static void phase_load(const char *dir)
{
    XboxStateStats st;
    XboxStateLoadStats ls;
    XboxStateThread th[XBOX_PARK_MAX_THREADS];
    uint8_t *a = NULL, *c;
    size_t an = 0, len;
    const uint8_t *p;
    volatile uint32_t *nvreg = xbox_Nv2aRegPtr(0x00400750u);   /* a PGRAPH word the walker leaves alone */
    uint32_t e1, e2, nv_before = 0, alloc_after, alloc_again, back_ok = 1, nth;
    int rc, k;
    char d[300];

    printf("Same-run load (XBA-249)\n");
    g_site = 0;
    spawn(6, K_SWITCH);
    check(wait_threads(4, 2000), "a fourth thread that sleeps from one call site or another starts", NULL);
    e1 = create_event();
    check(e1 != 0, "an event is created before the capture", NULL);
    if (nvreg) { *nvreg = 0x13572468u; nv_before = *nvreg; }
    usleep(20000);
    rc = xbox_StateCapture(200, &a, &an, &st);
    check(rc == XBOX_STATE_OK, "state A is captured (KOBJ, NV2A, APU, USB included)", st.reason);
    if (rc != XBOX_STATE_OK)
        return;

    /* Change memory, objects, the memory manager and a device. */
    GPTR(g_pattern)[10] ^= 0xFF;
    GPTR(g_pattern + 4096)[20] = 0x77;
    e2 = create_event();
    check(e2 != 0 && e2 != e1 && set_event(e2) == 0 && close_handle(e1) == 0 && set_event(e1) != 0,
          "an event created and the first closed after the capture", NULL);
    alloc_after = xbox_HeapAlloc(0x3000, 4096);
    if (nvreg) *nvreg = 0xDEADBEEFu;
    memset((void *)g_back_esi, 0, sizeof g_back_esi);
    usleep(80000);    /* every thread is in another call than the state's (the loader sleeps 50 ms) */

    rc = xbox_StateLoad(a, an, 500, &ls);
    snprintf(d, sizeof d, "%s: %s; %d threads, %d calls made again, settle %.1f ms, backup %.1f ms, sections"
             " %.1f ms", xbox_StateError(rc), ls.reason, ls.threads, ls.reissued, ls.settle_ms, ls.backup_ms,
             ls.load_ms);
    printf("  %s\n", d);
    check(rc == XBOX_STATE_OK && ls.threads == 4, "state A loads: every thread at the same call", d);
    check(GPTR(g_pattern)[10] == (uint8_t)(10 * 7 + 3) && GPTR(g_pattern + 4096)[20] == 0,
          "guest RAM is back (a changed page, a page zero in the state)", NULL);
    check(set_event(e2) != 0, "the event created after the state is gone", NULL);
    check(set_event(e1) == 0, "the event closed after the state is there again (same handle)", NULL);
    alloc_again = xbox_HeapAlloc(0x3000, 4096);
    snprintf(d, sizeof d, "0x%08X then 0x%08X", alloc_after, alloc_again);
    check(alloc_again == alloc_after, "the heap is back: the same allocation gets the same address", d);
    if (nvreg) {
        snprintf(d, sizeof d, "0x%08X", *nvreg);
        check(*nvreg == nv_before, "the NV2A register file is back", d);
    }
    usleep(150000);   /* every thread returns from its call at least once */
    xbox_StateSection(a, an, XBOX_STATE_TAG_CPU, &p, &len, NULL);
    nth = (uint32_t)xbox_StateCpuThreads(p, len, th, XBOX_PARK_MAX_THREADS);
    for (k = 0; k < (int)nth; k++) {
        uint32_t site = (th[k].ret_va - 0x00011000u) / 0x10u;
        if (site >= 16 || g_back_esi[site] != th[k].cpu.r_esi) {
            back_ok = 0;
            printf("  thread %u at 0x%08X: esi 0x%08X in the state, 0x%08X seen after the load\n", th[k].tid,
                   th[k].ret_va, th[k].cpu.r_esi, site < 16 ? g_back_esi[site] : 0);
        }
    }
    check(back_ok, "each thread returned from its call with the state's registers (esi)", NULL);

    /* Refused: a thread parked at another call site. */
    g_site = 1;
    check(wait_site(1, 2000), "the switching thread now sleeps from its other call site", NULL);
    rc = xbox_StateLoad(a, an, 500, &ls);
    snprintf(d, sizeof d, "%s: %s", xbox_StateError(rc), ls.reason);
    check(rc == XBOX_STATE_ECHAIN && strstr(ls.reason, "kernel call 99 from 0x00011068"),
          "a thread parked at another call: refused, naming it", d);
    GPTR(g_pattern)[11] ^= 0xFF;
    check(GPTR(g_pattern)[11] != (uint8_t)(11 * 7 + 3), "and nothing changed", NULL);
    GPTR(g_pattern)[11] ^= 0xFF;
    g_site = 0;
    usleep(30000);
    rc = xbox_StateLoad(a, an, 500, &ls);
    check(rc == XBOX_STATE_OK, "back at the first site: it loads again", ls.reason);

    /* Refused: a thread created since, one ended since, a state of another run. */
    g_temp_stop = 0;
    spawn(7, K_TEMP);
    check(wait_threads(5, 2000), "a fifth thread starts", NULL);
    rc = xbox_StateLoad(a, an, 500, &ls);
    snprintf(d, sizeof d, "%s: %s", xbox_StateError(rc), ls.reason);
    check(rc == XBOX_STATE_ECHAIN && strstr(ls.reason, "created since"), "a thread created since: refused", d);
    {
        uint8_t *b5 = NULL;
        size_t b5n = 0;
        rc = xbox_StateCapture(200, &b5, &b5n, &st);
        g_temp_stop = 1;
        check(wait_threads(4, 2000), "it ends", NULL);
        rc = rc == XBOX_STATE_OK ? xbox_StateLoad(b5, b5n, 500, &ls) : rc;
        snprintf(d, sizeof d, "%s: %s", xbox_StateError(rc), ls.reason);
        check(rc == XBOX_STATE_ECHAIN && strstr(ls.reason, "ended since"), "a thread that ended since: refused", d);
        free(b5);
    }
    c = other_run(a, an);
    rc = c ? xbox_StateLoad(c, an, 500, &ls) : -1;
    snprintf(d, sizeof d, "%s: %s", xbox_StateError(rc), ls.reason);
    check(rc == XBOX_STATE_ECHAIN && strstr(ls.reason, "another run"), "a state of another run: refused", d);
    free(c);

    /* Ten loads in a row, each capture taken fresh (the game goes on between them). */
    {
        double total = 0, worst = 0;
        int i, ok = 1;
        for (i = 0; i < 10; i++) {
            uint8_t *b = NULL;
            size_t bn = 0;
            double ms;
            usleep(3000 + 1700 * (unsigned)i);
            if (xbox_StateCapture(200, &b, &bn, &st) != XBOX_STATE_OK) { ok = 0; continue; }
            usleep(8000);
            rc = xbox_StateLoad(b, bn, 500, &ls);
            free(b);
            if (rc != XBOX_STATE_OK) { ok = 0; printf("  load %d: %s\n", i, ls.reason); continue; }
            ms = ls.freeze_ms + ls.settle_ms + ls.backup_ms + ls.load_ms;
            total += ms;
            if (ms > worst) worst = ms;
        }
        printf("  load: %.2f ms on average, %.2f ms at worst (freeze, settle, backup capture and sections)\n",
               total / 10.0, worst);
        check(ok, "ten capture / load round trips in a row", NULL);
    }

    /* Rewind's cost on this title: XOR deltas of the blobs (recomp_rewind), then steps back. */
    {
        recomp_rewind *ring = recomp_rewind_create((size_t)64 << 20);
        double cap = 0, push = 0, step = 0;
        int i, ok = ring != NULL, steps = 0;
        for (i = 0; ring && i < 12; i++) {
            uint8_t *b = NULL;
            size_t bn = 0;
            uint64_t t0;
            usleep(20000);
            t0 = now_ns();
            if (xbox_StateCapture(200, &b, &bn, &st) != XBOX_STATE_OK) { ok = 0; continue; }
            cap += (double)(now_ns() - t0) / 1e6;
            t0 = now_ns();
            if (recomp_rewind_push(ring, b, bn) != 0) ok = 0;
            push += (double)(now_ns() - t0) / 1e6;
            free(b);
        }
        for (i = 0; ring && i < 4; i++) {
            const uint8_t *top;
            size_t tn = 0;
            uint64_t t0 = now_ns();
            top = recomp_rewind_top(ring, &tn);
            uint8_t *copy = top ? malloc(tn) : NULL;
            if (copy) memcpy(copy, top, tn);
            rc = copy ? xbox_StateLoad(copy, tn, 500, &ls) : -1;
            free(copy);
            if (rc == XBOX_STATE_OK) steps++;
            else printf("  rewind step %d: %s\n", i, ls.reason);
            recomp_rewind_pop(ring);
            step += (double)(now_ns() - t0) / 1e6;
        }
        if (ring)
            printf("  rewind: 12 captures, %.2f ms each + %.2f ms to code the delta; ring %.2f MB for %zu captures"
                   " (blob %.2f MB); a step back %.2f ms\n", cap / 12, push / 12,
                   recomp_rewind_bytes(ring) / 1048576.0, recomp_rewind_count(ring), an / 1048576.0, step / 4);
        check(ok && steps == 4, "rewind: captures coded as XOR deltas, four steps back load", NULL);
        recomp_rewind_destroy(ring);
    }

    /* The front end: the title attached to recomp_states once its paths are set. */
    {
        char why[200];
        extern void xbox_path_init(const char *game_dir, const char *save_dir);
        xbox_path_init(dir, dir);
        xbox_StateServiceStart();
        rc = xbox_StateAvailable(why, sizeof why);
        check(rc == 1 && recomp_states_available() && !strcmp(recomp_states_console(), "xbox"),
              "save states available, attached as console xbox", why);
        rc = recomp_states_save(2);
        check(rc == RECOMP_STATE_OK, "the menu's Save state (slot 2) captures", recomp_states_error(rc));
        GPTR(g_pattern)[12] ^= 0xFF;
        rc = recomp_states_load(2);
        check(rc == RECOMP_STATE_OK && GPTR(g_pattern)[12] == (uint8_t)(12 * 7 + 3),
              "the menu's Load state (slot 2) puts it back", recomp_states_error(rc));
        g_site = 1;
        wait_site(1, 2000);
        rc = recomp_states_load(2);
        check(rc == RECOMP_STATE_EREFUSED, "a refused load says so to the menu (RECOMP_STATE_EREFUSED)",
              recomp_states_error(rc));
        g_site = 0;
        usleep(30000);
        xbox_StateServiceHotkeys(RECOMP_HK_BIT(RECOMP_HK_QSAVE), 0);
        {
            recomp_state_event ev;
            int t;
            for (t = 0; t < 200; t++) {
                recomp_states_last_event(&ev);
                if (ev.seq && ev.kind == RECOMP_STATE_EV_SAVED)
                    break;
                usleep(10000);
            }
            check(ev.kind == RECOMP_STATE_EV_SAVED && ev.rc == RECOMP_STATE_OK,
                  "the quick save chord runs on the service's thread", recomp_states_error(ev.rc));
        }
    }
    g_switch_stop = 1;
    check(wait_threads(3, 2000), "the switching thread ends", NULL);
    free(a);
}

static void phase_refusals(void)
{
    XboxStateStats st;
    uint8_t *b = NULL;
    size_t n = 0;
    int rc;
    char d[256];

    printf("Refusals\n");
    spawn(3, K_SPIN);
    check(wait_threads(4, 2000), "a thread that never makes a kernel call starts", NULL);
    rc = xbox_StateCapture(100, &b, &n, &st);
    snprintf(d, sizeof d, "%s: %s", xbox_StateError(rc), st.reason);
    check(rc == XBOX_STATE_EFREEZE && !b && strstr(st.reason, "did not reach a kernel call"),
          "the capture is refused and names it", d);
    g_spin_stop = 1;
    check(wait_threads(3, 2000), "it ends", NULL);

    xbox_ParkThreadStart();
    rc = xbox_StateCapture(100, &b, &n, &st);
    check(rc == XBOX_STATE_EBUSY && !b, "a capture from a guest thread is refused", st.reason);
    xbox_ParkThreadEnd();
}

static void phase_measure(void)
{
    double total = 0, worst = 0;
    size_t bytes = 0;
    int i, ok = 1;

    printf("Measures, 10 captures at odd moments\n");
    for (i = 0; i < 10; i++) {
        XboxStateStats st;
        uint8_t *b = NULL;
        size_t n = 0;
        double ms;
        usleep(5000 + 2300 * (unsigned)i);
        if (xbox_StateCapture(200, &b, &n, &st) != XBOX_STATE_OK) {
            ok = 0;
            continue;
        }
        ms = st.freeze_ms + st.settle_ms + st.capture_ms;
        total += ms;
        if (ms > worst) worst = ms;
        bytes = n;
        free(b);
    }
    printf("  %.2f ms per capture on average, %.2f ms at worst (freeze included); blob %.2f MB\n",
           total / 10.0, worst, bytes / 1048576.0);
    check(ok, "every capture succeeds", NULL);
}

/* RECOMP_STATE_PROBE: started late here (xbox_kernel_bridge_init found no variable), so it does not race
 * the phases above for the one capture at a time. */
static void phase_probe(const char *dir)
{
    char path[512];
    XboxStateInfo info;
    int rc = -1, t;

    printf("The measurement probe (RECOMP_STATE_PROBE=1)\n");
    setenv("RECOMP_STATE_PROBE", "1", 1);
    setenv("RECOMP_STATE_PROBE_DIR", dir, 1);
    snprintf(path, sizeof path, "%s/probe.st", dir);
    xbox_StateProbeStart();
    for (t = 0; t < 40 && rc != XBOX_STATE_OK; t++) {
        usleep(100000);
        rc = xbox_StateReadSlot(path, NULL, NULL, &info);
    }
    check(rc == XBOX_STATE_OK, "it writes a slot file that reads back", info.reason);
}

int main(int argc, char **argv)
{
    char tmpl[] = "/tmp/xstateXXXXXX", cmd[300];
    size_t len = 0;
    unsigned char *xbe;
    unsigned i;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (!mkdtemp(tmpl)) return 2;
    setenv("XDG_DATA_HOME", tmpl, 1);
    xbe = load(argc > 1 ? argv[1] : "tools/conformance/test.xbe", &len);
    if (!xbe || !xbox_MemoryLayoutInit(xbe, len)) {
        printf("cannot map guest memory (run tools/conformance/mkxbe.py)\n");
        return 2;
    }
    g_thunks  = xbox_HeapAlloc(0x1000, 4096);
    g_stack   = xbox_HeapAlloc(0x1000, 4096);
    g_handles = xbox_HeapAlloc(0x100, 4096);
    g_pattern = xbox_HeapAlloc(0x2000, 4096);
    g_pollword = xbox_HeapAlloc(0x10, 16);
    for (i = 0; i < 4096; i++)
        GPTR(g_pattern)[i] = (uint8_t)(i * 7 + 3);
    for (i = 0; i < NORDS; i++)
        G32(g_thunks + 4 * i) = 0x80000000u | ORDS[i];
    G32(g_thunks + 4 * NORDS) = 0;
    xbox_kernel_set_thunk_address(g_thunks, NORDS + 1);
    setenv("RECOMP_REWIND", "0", 1);   /* the service's periodic captures would race the phases (measured below) */
    xbox_kernel_bridge_init();
    xbox_SetThreadMode(XBOX_THREAD_MODE_SPAWN);
    xbox_GuestCpuPart();
    printf("guest CPU lock %s, dispatch gate %s\n", xbox_GuestCpuOn() ? "on" : "off",
           xbox_DispatchGateOn() ? "on" : "off");

    spawn(0, K_RENDER);
    spawn(1, K_AUDIO);
    spawn(2, K_LOADER);
    check(wait_threads(3, 2000), "three workers start", NULL);
    usleep(30000);

    phase_capture();
    phase_ram();
    phase_slot(tmpl);
    phase_refused();
    phase_sections();
    phase_retry();
    phase_host_waits();
    phase_load(tmpl);
    phase_refusals();
    phase_measure();
    phase_probe(tmpl);

    g_stop = 1;
    check(wait_threads(0, 3000), "the workers end", NULL);
    free(g_blob);
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmpl);
    (void)system(cmd);
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
