/* x360_hle_test -- the kernel HLE called directly (XBA-270): registers set as a title's call would set them, the
 * export run through x360_hle_call, its results and guest memory checked. Covers what the test titles do not reach:
 * the formatters, reserve-then-commit memory, physical memory, mutants, semaphore limits, pulses, timers, waits on
 * several objects, duplicated handles, suspend / resume, absolute timeouts, files written in the data directory and
 * symbolic links. Argument orders and structure layouts follow Xenia's kernel (src/xenia/kernel, BSD). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "x360_internal.h"
#include "xenos_cp.h"

static int failures;

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            failures++;                                                                                                \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

static x360_machine *X;
static uint32_t scratch; /* guest memory for the tests' arguments */
static uint32_t stack_top;

static xe_cpu *cpu(void) { return &x360_cur(X)->cpu; }

/* call an export with up to 10 arguments (the ninth and tenth in the caller's stack slots) */
static uint32_t callv(int lib, unsigned ord, int n, va_list ap)
{
    xe_cpu *c = cpu();
    c->gpr[1] = stack_top;
    for (int k = 0; k < n; k++) {
        uint32_t v = va_arg(ap, uint32_t);
        if (k < 8)
            c->gpr[3 + k] = v;
        else
            x360_w64(X, stack_top + 0x50 + 8 * (uint32_t)(k - 8), v);
    }
    x360_thunk th = {0, (uint16_t)ord, (uint8_t)lib};
    X->m.slice = 1000;
    x360_hle_call(X, &th);
    return (uint32_t)c->gpr[3];
}

static uint32_t K(unsigned ord, int n, ...)
{
    va_list ap;
    va_start(ap, n);
    uint32_t r = callv(X360_LIB_XBOXKRNL, ord, n, ap);
    va_end(ap);
    return r;
}

static uint32_t gstr(const char *s)
{
    uint32_t a = x360_kalloc(X, (uint32_t)strlen(s) + 1);
    memcpy(x360_p(X, a), s, strlen(s) + 1);
    return a;
}

/* OBJECT_ATTRIBUTES {root, ANSI_STRING*, attributes} for a path */
static uint32_t attrs(const char *path)
{
    uint32_t s = gstr(path), ansi = x360_kalloc(X, 8), oa = x360_kalloc(X, 12);
    x360_w16(X, ansi, (uint32_t)strlen(path));
    x360_w16(X, ansi + 2, (uint32_t)strlen(path) + 1);
    x360_w32(X, ansi + 4, s);
    x360_w32(X, oa + 4, ansi);
    return oa;
}

/* finish the waits that can finish now (what the scheduler does) */
static void settle(void)
{
    for (int k = 0; k < X->nthreads; k++)
        if (X->threads[k].state == TH_WAIT)
            x360_wait_check(X, &X->threads[k]);
}

static uint32_t wait_now(uint32_t handle) /* NtWaitForSingleObjectEx with a zero timeout, from the current thread */
{
    uint32_t zero = x360_kalloc(X, 8);
    K(X360_xboxkrnl_NtWaitForSingleObjectEx, 4, handle, 1, 0, zero);
    settle();
    return (uint32_t)cpu()->gpr[3];
}

static void test_formatters(void)
{
    uint32_t buf = x360_kalloc(X, 256);
    uint32_t r = K(X360_xboxkrnl_sprintf, 10, buf, gstr("%s|%5d|%-4x|%c|%%|%08X|%.3s|%u"), gstr("ab"),
                   (uint32_t)-42, 0xAB, 'z', 0xBEEF, gstr("abcdef"), 4000000000u, 0);
    const char *want = "ab|  -42|ab  |z|%|0000BEEF|abc|4000000000";
    CHECK(!strcmp((char *)x360_p(X, buf), want), "sprintf gave \"%s\"", (char *)x360_p(X, buf));
    CHECK(r == strlen(want), "sprintf returned %u", r);
    memset(x360_p(X, buf), 'q', 16);
    r = K(X360_xboxkrnl__snprintf, 4, buf, 5, gstr("%d"), 123456);
    CHECK(r == 0xFFFFFFFFu && !memcmp(x360_p(X, buf), "12345q", 6), "_snprintf truncation: %d \"%.6s\"", (int)r,
          (char *)x360_p(X, buf));
    uint32_t va = x360_kalloc(X, 32);
    x360_w64(X, va, 7);
    x360_w64(X, va + 8, gstr("seven"));
    r = K(X360_xboxkrnl_vsprintf, 3, buf, gstr("%d=%s"), va);
    CHECK(!strcmp((char *)x360_p(X, buf), "7=seven"), "vsprintf gave \"%s\"", (char *)x360_p(X, buf));
}

static void test_memory(void)
{
    uint32_t base_p = x360_kalloc(X, 8), size_p = base_p + 4;
    x360_w32(X, base_p, 0);
    x360_w32(X, size_p, 0x10000);
    CHECK(K(X360_xboxkrnl_NtAllocateVirtualMemory, 5, base_p, size_p, 0x2000, 4, 0) == 0, "reserve");
    uint32_t base = x360_r32(X, base_p);
    CHECK(base >= 0x10000 && base < 0x40000000, "reserve at %08X", base);
    x360_w32(X, base_p, base + 0x4321);
    x360_w32(X, size_p, 0x2000);
    uint32_t st = K(X360_xboxkrnl_NtAllocateVirtualMemory, 5, base_p, size_p, 0x1000, 4, 0);
    CHECK(st == 0 && x360_r32(X, base_p) == base + 0x4000, "commit inside the reservation: %08X at %08X", st,
          x360_r32(X, base_p));
    x360_p(X, base + 0x5000)[0] = 1; /* committed: writable */
    x360_w32(X, base_p, base);
    CHECK(K(X360_xboxkrnl_NtFreeVirtualMemory, 4, base_p, size_p, 0x8000, 0) == 0 &&
              x360_r32(X, size_p) == 0x10000, "release");
    uint32_t p = K(X360_xboxkrnl_MmAllocatePhysicalMemoryEx, 6, 0, 0x2000, 4, 0, 0xFFFFFFFFu, 0);
    CHECK(p >= 0xE0000000u, "physical 4 KB view: %08X", p);
    /* the 4 KB view starts one page into physical memory: virtual 0xE0000000 is physical 0x1000 */
    uint32_t pp = K(X360_xboxkrnl_MmGetPhysicalAddress, 1, p);
    CHECK(pp == p - 0xE0000000u + 0x1000u, "physical address of %08X: %08X", p, pp);
    CHECK(K(X360_xboxkrnl_MmGetPhysicalAddress, 1, p + 0x1234) == pp + 0x1234, "inside the allocation");
    CHECK(K(X360_xboxkrnl_MmQueryAllocationSize, 1, p) == 0x2000, "allocation size");
    uint32_t info = x360_kalloc(X, 32);
    CHECK(K(X360_xboxkrnl_NtQueryVirtualMemory, 2, p + 4, info) == 0 && x360_r32(X, info) == p &&
              x360_r32(X, info + 16) == 0x1000 /* MEM_COMMIT */,
          "a query inside it: base %08X state %X", x360_r32(X, info), x360_r32(X, info + 16));
    CHECK(K(X360_xboxkrnl_MmIsAddressValid, 1, p + 0x1FFF) == 1 && K(X360_xboxkrnl_MmIsAddressValid, 1, p + 0x2000) == 0,
          "valid to its last byte, not past it");
    /* one memory through the three views */
    x360_p(X, p + 5)[0] = 0x77;
    CHECK(x360_p(X, 0xA0000000u + pp + 5)[0] == 0x77 && x360_p(X, 0xC0000000u + pp + 5)[0] == 0x77, "mirrors");
    CHECK(K(X360_xboxkrnl_MmGetPhysicalAddress, 1, 0xA0000000u + pp) == pp &&
              K(X360_xboxkrnl_MmGetPhysicalAddress, 1, 0xC0000000u + pp) == pp, "the other views start at 0");
    uint32_t q = K(X360_xboxkrnl_MmAllocatePhysicalMemoryEx, 6, 0, 0x10000, 0x20000004u, 0, 0xFFFFFFFFu, 0);
    CHECK(q >= 0xA0000000u && q < 0xC0000000u && !(q & 0xFFFF), "physical 64 KB view: %08X", q);
    CHECK(K(X360_xboxkrnl_MmGetPhysicalAddress, 1, q) == q - 0xA0000000u, "its physical address");
    /* an alignment is the physical address's */
    uint32_t r = K(X360_xboxkrnl_MmAllocatePhysicalMemoryEx, 6, 0, 0x1000, 4, 0, 0xFFFFFFFFu, 0x10000);
    CHECK(r >= 0xE0000000u && !(K(X360_xboxkrnl_MmGetPhysicalAddress, 1, r) & 0xFFFF), "aligned: %08X", r);
    /* freed by its virtual address: the next allocation of that size gets the pages back, zeroed */
    K(X360_xboxkrnl_MmFreePhysicalMemory, 2, 0, p);
    CHECK(K(X360_xboxkrnl_MmQueryAllocationSize, 1, p) == 0 && K(X360_xboxkrnl_MmIsAddressValid, 1, p) == 0, "freed");
    uint32_t again = K(X360_xboxkrnl_MmAllocatePhysicalMemoryEx, 6, 0, 0x2000, 4, 0, 0xFFFFFFFFu, 0);
    CHECK(again == p && x360_p(X, p + 5)[0] == 0, "the same pages again, cleared: %08X", again);
}

/* The host's page size (x360_mem.c). The guest's pages are 4 KB; where the host's are larger the 0xE0000000 view,
 * which starts 0x1000 into physical memory, cannot be an mmap of the file at that offset: its guest addresses sit
 * 0x1000 higher in the host instead. X360_HOST_PAGE makes a machine behave as on such a host, and refuse every
 * mapping call that host would refuse. Checked on a machine of each size: one memory through the three views, in
 * both directions and at the view's two ends, through the accessors lifted code uses and through the runtime's
 * pointers; commits smaller than a host page; an allocation through the kernel; a pack whose accesses do not follow
 * the view (XE_VIEW4K_DYNAMIC, xenon_rt.h) refused where it moved. */
static void nothing_lifted(xe_machine *m) { (void)m; }

static void check_host_page(uint32_t page)
{
    char why[256] = "", env[16];
    long real = sysconf(_SC_PAGESIZE);
    if (page < (uint32_t)real)
        return; /* a host cannot act as one of smaller pages */
    snprintf(env, sizeof env, "%u", (unsigned)page);
    const char *before = getenv("X360_HOST_PAGE");
    char *saved = before ? strdup(before) : NULL;
    setenv("X360_HOST_PAGE", env, 1);
    x360_machine *y = x360_create_bare(why, sizeof why);
    if (saved)
        setenv("X360_HOST_PAGE", saved, 1);
    else
        unsetenv("X360_HOST_PAGE");
    free(saved);
    CHECK(y != NULL, "a machine on host pages of %u bytes: %s", (unsigned)page, why);
    if (!y)
        return;
    uint32_t skew = page > 0x1000 ? 0x1000 : 0;
    CHECK(y->host_page == page && y->m.view4k_skew == skew, "page %u: host page %u, skew %X", (unsigned)page,
          (unsigned)y->host_page, (unsigned)y->m.view4k_skew);
    uint8_t *r = y->reservation;
    CHECK(x360_p(y, 0xA0000000u) == r + 0xA0000000u && x360_p(y, 0xC0000000u) == r + 0xC0000000u &&
              x360_p(y, 0xDFFFFFFFu) == r + 0xDFFFFFFFu && x360_p(y, 0x82000000u) == r + 0x82000000u &&
              x360_p(y, 0xE0000000u) == r + 0xE0000000u + skew,
          "page %u: only the 4 KB view moves", (unsigned)page);
    /* virtual 0xE0000000 + v is physical v + 0x1000: the first and last bytes of the view, and a few inside, across
     * the host's page boundaries */
    static const uint32_t at[] = {0, 4, 0xFFC, 0x1000, 0x2FFC, 0x3000, 0x3FFC, 0x4000, 0xFFFC, 0x10000,
                                  0x12344, 0x1FFFDFFC, 0x1FFFEFFC};
    for (unsigned k = 0; k < sizeof at / sizeof at[0]; k++) {
        uint32_t v = at[k], phys = v + 0x1000, tag = 0xC0DE0000u + k;
        x360_w32(y, 0xE0000000u + v, tag); /* written through the 4 KB view ... */
        CHECK(x360_r32(y, 0xA0000000u + phys) == tag && x360_r32(y, 0xC0000000u + phys) == tag &&
                  x360_r32(y, x360_p2v(X360_VIEW_64K, x360_v2p(0xE0000000u + v))) == tag,
              "page %u: E%07X written, seen at A%07X %08X and C%07X %08X", (unsigned)page, (unsigned)v, (unsigned)phys,
              x360_r32(y, 0xA0000000u + phys), (unsigned)phys, x360_r32(y, 0xC0000000u + phys));
        x360_w32(y, 0xA0000000u + phys, ~tag); /* ... and the other way round */
        CHECK(x360_r32(y, 0xE0000000u + v) == ~tag && x360_r32(y, 0xC0000000u + phys) == ~tag,
              "page %u: A%07X written, seen at E%07X %08X", (unsigned)page, (unsigned)phys, (unsigned)v,
              x360_r32(y, 0xE0000000u + v));
        x360_p(y, 0xC0000000u + phys)[1] = 0x5A; /* the runtime's pointer, a byte in the middle of the word */
        CHECK(x360_p(y, 0xE0000000u + v)[1] == 0x5A && x360_r8(y, 0xE0000001u + v) == 0x5A &&
                  x360_p(y, 0xA0000000u + phys)[1] == 0x5A,
              "page %u: a byte at C%07X seen through the others", (unsigned)page, (unsigned)phys + 1);
    }
    /* the page below the view's start (physical 0) is not in it: what is written there stays out of the view */
    x360_w32(y, 0xA0000FFCu, 0x11223344u);
    x360_w32(y, 0xE0000000u, 0x55667788u);
    CHECK(x360_r32(y, 0xA0000FFCu) == 0x11223344u && x360_r32(y, 0xA0001000u) == 0x55667788u,
          "page %u: physical FFC %08X, 1000 %08X", (unsigned)page, x360_r32(y, 0xA0000FFCu), x360_r32(y, 0xA0001000u));
    /* a commit of one guest page, anywhere in a host page */
    for (uint32_t a = 0x30001000u; a < 0x30021000u; a += 0x5000) {
        CHECK(x360_commit(y, a, 0x1000) == 0, "page %u: commit of %08X", (unsigned)page, a);
        x360_w32(y, a + 0xFFC, a);
        CHECK(x360_r32(y, a + 0xFFC) == a, "page %u: %08X committed", (unsigned)page, a);
    }
    /* the heaps hand out 4 KB pages all the same, and an allocation in the 4 KB view is the memory at its physical
     * address */
    uint32_t v1 = x360_heap_alloc(y, &y->heap_v4k, 0x1000, 0), v2 = x360_heap_alloc(y, &y->heap_v4k, 0x1000, 0);
    CHECK(v1 && v2 == v1 + 0x1000, "page %u: 4 KB virtual pages %08X %08X", (unsigned)page, v1, v2);
    x360_w32(y, v1 + 0xFFC, 1);
    x360_w32(y, v2, 2);
    CHECK(x360_r32(y, v1 + 0xFFC) == 1 && x360_r32(y, v2) == 2, "page %u: both usable", (unsigned)page);
    uint32_t ph = x360_heap_alloc(y, &y->heap_phys, 0x3000, 0);
    uint32_t pv = x360_p2v(X360_VIEW_4K, ph);
    CHECK(ph && pv >= 0xE0000000u && x360_v2p(pv) == ph, "page %u: physical %08X is %08X", (unsigned)page, ph, pv);
    memset(x360_p(y, pv), 0xA5, 0x3000);
    CHECK(x360_r32(y, 0xA0000000u + ph) == 0xA5A5A5A5u && x360_r32(y, 0xA0000000u + ph + 0x2FFC) == 0xA5A5A5A5u &&
              x360_r32(y, 0xA0000000u + ph - 4) != 0xA5A5A5A5u && x360_r32(y, 0xA0000000u + ph + 0x3000) != 0xA5A5A5A5u,
          "page %u: the allocation filled through its 4 KB view pointer, and only it", (unsigned)page);
    /* a pack whose accesses do not follow the view cannot run where it moved */
    static const xe_rc_entry table[] = {{0x82000000u, nothing_lifted}};
    x360_pack old = {table, 1, y->image_crc, NULL, 0, 1u, NULL, 0};
    x360_pack now = {table, 1, y->image_crc, NULL, 0, 1u | XE_RC_FEAT_VIEW4K, NULL, 0};
    int r_old = x360_bind(y, &old);
    CHECK(r_old == (skew ? -1 : 0), "page %u: a pack without the view's feature: %d (%s)", (unsigned)page, r_old,
          x360_error(y));
    CHECK(x360_bind(y, &now) == 0, "page %u: a pack with it: %s", (unsigned)page, x360_error(y));
    x360_destroy(y);
}

static void test_host_page(void)
{
    /* the machine of this run follows X360_HOST_PAGE (the *_16k tests) or the host */
    const char *e = getenv("X360_HOST_PAGE");
    long real = sysconf(_SC_PAGESIZE);
    uint32_t want = e && strtoul(e, NULL, 0) >= (unsigned long)real ? (uint32_t)strtoul(e, NULL, 0) : (uint32_t)real;
    CHECK(X->host_page == want && X->m.view4k_skew == (want > 0x1000 ? 0x1000u : 0u),
          "this machine: host page %u, skew %X", (unsigned)X->host_page, (unsigned)X->m.view4k_skew);
    check_host_page(0x1000);
    check_host_page(0x4000);
    check_host_page(0x10000);
    /* a size that is no page size is ignored */
    setenv("X360_HOST_PAGE", "12288", 1);
    char why[64] = "";
    x360_machine *y = x360_create_bare(why, sizeof why);
    CHECK(y && y->host_page == (uint32_t)real, "a size that is no power of two is ignored");
    if (y)
        x360_destroy(y);
    if (e)
        setenv("X360_HOST_PAGE", e, 1);
    else
        unsetenv("X360_HOST_PAGE");
}

static void test_objects(void)
{
    uint32_t hp = x360_kalloc(X, 16);
    /* mutant: owned at creation, taken again by its owner, released twice, then not owned */
    CHECK(K(X360_xboxkrnl_NtCreateMutant, 3, hp, 0, 1) == 0, "NtCreateMutant");
    uint32_t mh = x360_r32(X, hp);
    CHECK(wait_now(mh) == 0, "owner waits on its mutant");
    CHECK(K(X360_xboxkrnl_NtReleaseMutant, 2, mh, 0) == 0, "release 1");
    CHECK(K(X360_xboxkrnl_NtReleaseMutant, 2, mh, 0) == 0, "release 2");
    CHECK(K(X360_xboxkrnl_NtReleaseMutant, 2, mh, 0) == X_STATUS_MUTANT_NOT_OWNED, "release 3: not owned");
    /* semaphore: count 1, limit 2 */
    CHECK(K(X360_xboxkrnl_NtCreateSemaphore, 4, hp, 0, 1, 2) == 0, "NtCreateSemaphore");
    uint32_t sh = x360_r32(X, hp), prev = x360_kalloc(X, 4);
    CHECK(K(X360_xboxkrnl_NtReleaseSemaphore, 3, sh, 1, prev) == 0 && x360_r32(X, prev) == 1, "release to 2");
    CHECK(K(X360_xboxkrnl_NtReleaseSemaphore, 3, sh, 1, prev) == X_STATUS_SEMAPHORE_LIMIT_EXCEEDED, "limit");
    CHECK(wait_now(sh) == 0 && wait_now(sh) == 0 && wait_now(sh) == X_STATUS_TIMEOUT, "two takes, then empty");
    /* duplicated handle */
    CHECK(K(X360_xboxkrnl_NtCreateEvent, 4, hp, 0, 0, 0) == 0, "NtCreateEvent");
    uint32_t eh = x360_r32(X, hp);
    CHECK(K(X360_xboxkrnl_NtDuplicateObject, 3, eh, hp + 4, 1) == 0, "NtDuplicateObject");
    uint32_t dh = x360_r32(X, hp + 4);
    CHECK(dh != eh && x360_handle_obj(X, dh) && x360_handle_obj(X, eh) == 0, "duplicate kept, source closed");
    K(X360_xboxkrnl_NtSetEvent, 2, dh, 0);
    CHECK(wait_now(dh) == 0 && wait_now(dh) == 0, "notification event stays set");
    /* wait on several objects (Ke*, objects by address) */
    uint32_t e1 = x360_kalloc(X, 16), e2 = x360_kalloc(X, 16), objs = x360_kalloc(X, 8), zero = x360_kalloc(X, 8);
    K(X360_xboxkrnl_KeInitializeEvent, 3, e1, 1, 0);
    K(X360_xboxkrnl_KeInitializeEvent, 3, e2, 1, 1);
    x360_w32(X, objs, e1);
    x360_w32(X, objs + 4, e2);
    K(X360_xboxkrnl_KeWaitForMultipleObjects, 8, 2, objs, 1, 0, 1, 0, zero, 0);
    settle();
    CHECK(cpu()->gpr[3] == 1 && x360_r32(X, e2 + 4) == 0, "wait any: the second, auto-reset");
    K(X360_xboxkrnl_KeSetEvent, 3, e2, 0, 0);
    K(X360_xboxkrnl_KeWaitForMultipleObjects, 8, 2, objs, 0, 0, 1, 0, zero, 0);
    settle();
    CHECK(cpu()->gpr[3] == X_STATUS_TIMEOUT && x360_r32(X, e2 + 4) == 1, "wait all times out, takes nothing");
}

static void test_threads_and_time(void)
{
    uint32_t hp = x360_kalloc(X, 16);
    CHECK(K(X360_xboxkrnl_ExCreateThread, 7, hp, 0, hp + 4, 0, 0x82001000u, 0x1234, 0) == 0, "ExCreateThread");
    x360_thread *t1 = x360_thread_by_kthread(X, x360_handle_obj(X, x360_r32(X, hp)));
    CHECK(t1 && t1->cpu.gpr[3] == 0x1234 && t1->cpu.pc == 0x82001000u && t1->state == TH_READY, "the new thread");
    if (!t1)
        return;
    CHECK(t1->cpu.gpr[13] == t1->pcr && x360_r32(X, t1->pcr + 0x100) == t1->kthread, "r13 is its PCR");
    uint32_t th = x360_r32(X, hp), prev = x360_kalloc(X, 4);
    K(X360_xboxkrnl_KeSetAffinityThread, 3, t1->kthread, 0x10, prev);
    CHECK(x360_r8(X, t1->pcr + 0x10C) == 4 && x360_r8(X, t1->kthread + 0xBF) == 4, "affinity: hardware thread 4");
    /* the creation flags' high byte is an affinity mask too: a thread made (suspended) for hardware thread 5, another
     * for 4, read their processor numbers without any later call */
    for (uint32_t want = 4; want <= 5; want++) {
        CHECK(K(X360_xboxkrnl_ExCreateThread, 7, hp + 12, 0, 0, 0, 0x82001000u, 0, (1u << want) << 24 | 1) == 0,
              "ExCreateThread with an affinity");
        x360_thread *ta = x360_thread_by_kthread(X, x360_handle_obj(X, x360_r32(X, hp + 12)));
        CHECK(ta && ta->state == TH_SUSPENDED && x360_r8(X, ta->pcr + 0x10C) == want &&
                  x360_r8(X, ta->kthread + 0xBF) == want,
              "created on hardware thread %u: %u", want, ta ? x360_r8(X, ta->pcr + 0x10C) : 99);
    }
    CHECK(K(X360_xboxkrnl_NtSuspendThread, 2, th, prev) == 0 && t1->state == TH_SUSPENDED, "suspend");
    CHECK(K(X360_xboxkrnl_NtResumeThread, 2, th, prev) == 0 && x360_r32(X, prev) == 1 && t1->state == TH_READY,
          "resume");
    /* a pulse wakes the thread waiting now and leaves the event clear */
    K(X360_xboxkrnl_NtCreateEvent, 4, hp + 8, 0, 0, 0);
    uint32_t eh = x360_r32(X, hp + 8);
    int me = X->cur;
    X->cur = (int)(t1 - X->threads);
    K(X360_xboxkrnl_NtWaitForSingleObjectEx, 4, eh, 1, 0, 0);
    CHECK(t1->state == TH_WAIT, "the second thread waits");
    X->cur = me;
    K(X360_xboxkrnl_NtPulseEvent, 2, eh, 0);
    CHECK(t1->state == TH_READY && t1->cpu.gpr[3] == 0, "pulsed awake");
    CHECK(wait_now(eh) == X_STATUS_TIMEOUT, "and the event is clear");
    /* timer: due in 1 ms */
    uint32_t due = x360_kalloc(X, 8);
    x360_w64(X, due, (uint64_t)-10000);
    CHECK(K(X360_xboxkrnl_NtCreateTimer, 3, hp + 12, 0, 1) == 0, "NtCreateTimer");
    uint32_t tm = x360_r32(X, hp + 12);
    CHECK(K(X360_xboxkrnl_NtSetTimerEx, 8, tm, due, 0, 1, 0, 0, 0, 0) == 0, "NtSetTimerEx");
    CHECK(wait_now(tm) == X_STATUS_TIMEOUT, "not due yet");
    X->time_skip += 20000;
    x360_timers_fire(X);
    CHECK(wait_now(tm) == 0, "due");
    /* an absolute timeout, against KeQuerySystemTime */
    uint32_t now = x360_kalloc(X, 8), when = x360_kalloc(X, 8);
    K(X360_xboxkrnl_KeQuerySystemTime, 1, now);
    x360_w64(X, when, x360_r64(X, now) + 5000);
    K(X360_xboxkrnl_NtWaitForSingleObjectEx, 4, eh, 1, 0, when);
    settle();
    CHECK(x360_cur(X)->state == TH_WAIT, "absolute timeout not reached");
    X->time_skip += 6000;
    settle();
    CHECK(x360_cur(X)->state == TH_READY && cpu()->gpr[3] == X_STATUS_TIMEOUT, "absolute timeout reached");
    /* TLS */
    uint32_t slot = K(X360_xboxkrnl_KeTlsAlloc, 0);
    K(X360_xboxkrnl_KeTlsSetValue, 2, slot, 0xABCD);
    CHECK(K(X360_xboxkrnl_KeTlsGetValue, 1, slot) == 0xABCD, "TLS");
}

static uint32_t XM(unsigned ord, int n, ...)
{
    va_list ap;
    va_start(ap, n);
    uint32_t r = callv(X360_LIB_XAM, ord, n, ap);
    va_end(ap);
    return r;
}

static void test_system(void)
{
    uint32_t p = x360_kalloc(X, 64);
    /* 2000-01-01 00:00:00.000, a Saturday: FILETIME 125911584000000000 */
    x360_w64(X, p, 125911584000000000ull);
    K(X360_xboxkrnl_RtlTimeToTimeFields, 2, p, p + 16);
    CHECK(x360_r16(X, p + 16) == 2000 && x360_r16(X, p + 18) == 1 && x360_r16(X, p + 20) == 1 &&
              x360_r16(X, p + 22) == 0 && x360_r16(X, p + 30) == 6, "time fields %u-%u-%u weekday %u",
          x360_r16(X, p + 16), x360_r16(X, p + 18), x360_r16(X, p + 20), x360_r16(X, p + 30));
    x360_w16(X, p + 16, 2026);
    x360_w16(X, p + 18, 11);
    x360_w16(X, p + 20, 18);
    x360_w16(X, p + 22, 7);
    x360_w16(X, p + 24, 30);
    x360_w16(X, p + 26, 15);
    x360_w16(X, p + 28, 250);
    CHECK(K(X360_xboxkrnl_RtlTimeFieldsToTime, 2, p + 16, p + 32) == 1, "time fields to time");
    K(X360_xboxkrnl_RtlTimeToTimeFields, 2, p + 32, p + 48);
    CHECK(!memcmp(x360_p(X, p + 16), x360_p(X, p + 48), 14), "round trip");
    K(X360_xboxkrnl_VdQueryVideoMode, 1, p);
    CHECK(x360_r32(X, p) == 1280 && x360_r32(X, p + 4) == 720 && x360_r32(X, p + 20) == 0x42700000u, "video mode");
    CHECK(XM(X360_xam_XamUserGetSigninState, 1, 0) == 1 && XM(X360_xam_XamUserGetSigninState, 1, 1) == 0, "sign-in");
    CHECK(XM(X360_xam_XamUserGetXUID, 3, 0, 1, p) == 0 && x360_r64(X, p) == 0xB13EBABEBABEBABEull, "xuid");
    CHECK(XM(X360_xam_XamUserGetName, 3, 0, p, 16) == 0 && !strcmp((char *)x360_p(X, p), "Player"), "user name");
    CHECK(XM(X360_xam_XamInputGetState, 3, 0, 0, p) == 0x48F, "no controller yet");
    CHECK(XM(X360_xam_XamAlloc, 3, 0, 100, p) == 0 && x360_r32(X, p) >= 0x10000, "XamAlloc");
    CHECK(XM(X360_xam_XamFree, 1, x360_r32(X, p)) == 0, "XamFree");
}

static void test_gpu(void)
{
    uint32_t ring = K(X360_xboxkrnl_MmAllocatePhysicalMemoryEx, 6, 0, 0x2000, 4, 0, 0xFFFFFFFFu, 0);
    uint32_t misc = K(X360_xboxkrnl_MmAllocatePhysicalMemoryEx, 6, 0, 0x1000, 4, 0, 0xFFFFFFFFu, 0);
    uint32_t ring_phys = K(X360_xboxkrnl_MmGetPhysicalAddress, 1, ring);
    /* what the GPU writes at a physical address, the title reads through its 4 KB view pointer */
    uint32_t misc_phys = K(X360_xboxkrnl_MmGetPhysicalAddress, 1, misc), fence = misc + 0x100, wb = misc + 0x104;
    K(X360_xboxkrnl_VdInitializeRingBuffer, 2, ring_phys, 10);
    K(X360_xboxkrnl_VdEnableRingBufferRPtrWriteBack, 2, misc_phys + 0x104, 6);
    /* a fence through EVENT_WRITE_SHD (8in32 endian mode), then the 64 words VdSwap fills */
    x360_w32(X, ring, 3u << 30 | 2u << 16 | (uint32_t)PM4_EVENT_WRITE_SHD << 8);
    x360_w32(X, ring + 4, 5);
    x360_w32(X, ring + 8, (misc_phys + 0x100) | 2);
    x360_w32(X, ring + 12, 0x1234);
    uint32_t args = x360_kalloc(X, 32);
    x360_w32(X, args, 0xA1000000u); /* front buffer */
    x360_w32(X, args + 4, 1280);
    x360_w32(X, args + 8, 720);
    K(X360_xboxkrnl_VdSwap, 10, ring + 16, args + 16, 0, 0, 0, args, args + 20, args + 24, args + 4, args + 8);
    /* a pixel shader's microcode, dumped once to a directory */
    char sdir[] = "/tmp/x360_shaders_XXXXXX";
    int have_dir = mkdtemp(sdir) != NULL;
    if (have_dir)
        x360_set_shader_dump(X, sdir);
    uint32_t sp = ring + 4 * (4 + 64);
    x360_w32(X, sp, 3u << 30 | (5u - 1) << 16 | (uint32_t)PM4_IM_LOAD_IMMEDIATE << 8);
    x360_w32(X, sp + 4, 1);
    x360_w32(X, sp + 8, 3);
    x360_w32(X, sp + 12, 0x11111111u);
    x360_w32(X, sp + 16, 0x22222222u);
    x360_w32(X, sp + 20, 0x33333333u);
    x360_w32(X, 0x7FC80000u + 4 * XENOS_REG_CP_RB_WPTR, 4 + 64 + 6);
    x360_gpu_poll(X);
    x360_stats st;
    x360_get_stats(X, &st);
    CHECK(x360_r32(X, fence) == 0x1234, "fence %08X", x360_r32(X, fence));
    CHECK(x360_r32(X, wb) == 74, "read pointer written back: %u", x360_r32(X, wb));
    if (have_dir) {
        uint8_t want[12] = {0x11, 0x11, 0x11, 0x11, 0x22, 0x22, 0x22, 0x22, 0x33, 0x33, 0x33, 0x33};
        char path[128];
        snprintf(path, sizeof path, "%s/ps_%08X.bin", sdir, x360_crc32(want, 12));
        FILE *f = fopen(path, "rb");
        uint8_t got[16];
        size_t n = f ? fread(got, 1, sizeof got, f) : 0;
        CHECK(n == 12 && !memcmp(got, want, 12), "shader dumped to %s (%zu bytes)", path, n);
        if (f)
            fclose(f);
        unlink(path);
        rmdir(sdir);
        x360_set_shader_dump(X, NULL);
    }
    CHECK(st.gpu_swaps == 1 && st.gpu_packets >= 3, "swap seen: %llu swaps, %llu packets",
          (unsigned long long)st.gpu_swaps, (unsigned long long)st.gpu_packets);
    /* the vertical blank interrupt runs the callback on its own thread */
    K(X360_xboxkrnl_VdSetGraphicsInterruptCallback, 2, 0x82001000u, 0x55);
    int before = X->nthreads;
    X->time_skip += 200000;
    x360_gpu_poll(X);
    x360_thread *irq = NULL;
    for (int k = 0; k < X->nthreads; k++)
        if (X->threads[k].interrupt && X->threads[k].state == TH_READY)
            irq = &X->threads[k];
    CHECK(irq && irq->cpu.pc == 0x82001000u && irq->cpu.gpr[3] == 0 && irq->cpu.gpr[4] == 0x55,
          "vblank interrupt thread (threads %d -> %d)", before, X->nthreads);
    /* what its handler reads first: the display interrupt status says the vertical blank occurred, also after the
     * handler wrote the register to acknowledge it */
    CHECK(x360_r32(X, 0x7FC80000u + 4 * 0x1951) & 1, "the vertical blank status is set");
    x360_w32(X, 0x7FC80000u + 4 * 0x1951, 0);
    x360_gpu_poll(X);
    CHECK(x360_r32(X, 0x7FC80000u + 4 * 0x1951) & 1, "and set again for the next interrupt");
    /* ten thousand vertical blanks (about three minutes of virtual time) reuse that one context */
    int fired = 0, nthreads = X->nthreads;
    uint32_t kfree = X->kalloc_cur;
    for (int k = 0; k < 10000 && irq; k++) {
        if (irq->state == TH_READY) {
            fired++;
            x360_thread_exit(X, irq, 0); /* the callback returned */
        }
        X->time_skip += 166667;
        x360_gpu_poll(X);
    }
    CHECK(fired >= 9999 && X->nthreads == nthreads && X->kalloc_cur == kfree && X->status == X360_RUNNING,
          "vblanks: %d callbacks, threads %d -> %d", fired, nthreads, X->nthreads);
    /* Tiled rendering's predicate is in save states: bin select 0 (no bin selected), saved; select 1 again; the
     * state loaded: a predicated fence does not run, the one after it does */
    uint32_t rp = 4 + 64 + 6;
    x360_w32(X, ring + 4 * rp++, 3u << 30 | (uint32_t)PM4_SET_BIN_SELECT_LO << 8);
    x360_w32(X, ring + 4 * rp++, 0);
    x360_w32(X, 0x7FC80000u + 4 * XENOS_REG_CP_RB_WPTR, rp);
    x360_gpu_poll(X);
    size_t cap = x360_state_size(X), n = 0;
    uint8_t *blob = malloc(cap);
    char why[128] = "";
    CHECK(blob && (n = x360_state_save(X, NULL, blob, cap)) > 0, "state saved with the bin registers");
    x360_w32(X, ring + 4 * rp, 3u << 30 | (uint32_t)PM4_SET_BIN_SELECT_LO << 8);
    x360_w32(X, ring + 4 * rp + 4, 1);
    x360_w32(X, 0x7FC80000u + 4 * XENOS_REG_CP_RB_WPTR, rp + 2);
    x360_gpu_poll(X);
    CHECK(blob && x360_state_load(X, NULL, blob, n, why, sizeof why) == 0, "state loaded: %s", why);
    free(blob);
    x360_w32(X, ring + 4 * rp++, 3u << 30 | 2u << 16 | (uint32_t)PM4_EVENT_WRITE_SHD << 8 | 1);
    x360_w32(X, ring + 4 * rp++, 5);
    x360_w32(X, ring + 4 * rp++, (misc_phys + 0x100) | 2);
    x360_w32(X, ring + 4 * rp++, 0x5678);
    x360_w32(X, ring + 4 * rp++, 3u << 30 | 2u << 16 | (uint32_t)PM4_EVENT_WRITE_SHD << 8);
    x360_w32(X, ring + 4 * rp++, 5);
    x360_w32(X, ring + 4 * rp++, (misc_phys + 0x108) | 2);
    x360_w32(X, ring + 4 * rp++, 0x9ABC);
    x360_w32(X, 0x7FC80000u + 4 * XENOS_REG_CP_RB_WPTR, rp);
    x360_gpu_poll(X);
    CHECK(x360_r32(X, fence) == 0x1234 && x360_r32(X, misc + 0x108) == 0x9ABC,
          "after the load: the predicated fence %08X (not written), the plain one %08X", x360_r32(X, fence),
          x360_r32(X, misc + 0x108));
    /* back to the initial value for the tests that follow */
    x360_w32(X, ring + 4 * rp++, 3u << 30 | (uint32_t)PM4_SET_BIN_SELECT_LO << 8);
    x360_w32(X, ring + 4 * rp++, 0xFFFFFFFFu);
    x360_w32(X, 0x7FC80000u + 4 * XENOS_REG_CP_RB_WPTR, rp);
    x360_gpu_poll(X);
}

static void hex(const uint8_t *p, int n, char *out)
{
    for (int i = 0; i < n; i++)
        sprintf(out + 2 * i, "%02x", p[i]);
}

static void test_crypt(void)
{
    char h[80];
    uint32_t buf = x360_kalloc(X, 0x400), st = buf + 0x100, out = buf + 0x200;
    memcpy(x360_p(X, buf), "abc", 3);
    K(X360_xboxkrnl_XeCryptSha, 8, buf, 3, 0, 0, 0, 0, out, 20);
    hex(x360_p(X, out), 20, h);
    CHECK(!strcmp(h, "a9993e364706816aba3e25717850c26c9cd0d89d"), "SHA-1(abc) %s", h); /* FIPS 180-4 example */
    /* incremental, across blocks: 'a' * 1000 split oddly */
    memset(x360_p(X, buf), 'a', 1000 > 0x100 ? 0x100 : 1000);
    K(X360_xboxkrnl_XeCryptShaInit, 1, st);
    for (int k = 0; k < 10; k++)
        K(X360_xboxkrnl_XeCryptShaUpdate, 3, st, buf, 100);
    K(X360_xboxkrnl_XeCryptShaFinal, 3, st, out, 20);
    hex(x360_p(X, out), 20, h);
    CHECK(!strcmp(h, "291e9a6c66994949b57ba5e650361e98fc36b1ba"), "SHA-1('a' x 1000) %s", h);
    memcpy(x360_p(X, buf), "abc", 3);
    K(X360_xboxkrnl_XeCryptSha256Init, 1, st);
    K(X360_xboxkrnl_XeCryptSha256Update, 3, st, buf, 3);
    K(X360_xboxkrnl_XeCryptSha256Final, 3, st, out, 32);
    hex(x360_p(X, out), 32, h);
    CHECK(!strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "SHA-256(abc) %s", h);
    /* RFC 2202 test case 2: key "Jefe", data "what do ya want for nothing?" */
    memcpy(x360_p(X, buf), "Jefe", 4);
    memcpy(x360_p(X, buf + 16), "what do ya want for nothing?", 28);
    K(X360_xboxkrnl_XeCryptHmacSha, 10, buf, 4, buf + 16, 28, 0, 0, 0, 0, out, 20);
    hex(x360_p(X, out), 20, h);
    CHECK(!strcmp(h, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79"), "HMAC-SHA1 %s", h);
    /* RC4: key "Key", plaintext "Plaintext" -> BBF316E8D940AF0AD3 */
    memcpy(x360_p(X, buf), "Key", 3);
    memcpy(x360_p(X, buf + 16), "Plaintext", 9);
    K(X360_xboxkrnl_XeCryptRc4, 4, buf, 3, buf + 16, 9);
    hex(x360_p(X, buf + 16), 9, h);
    CHECK(!strcmp(h, "bbf316e8d940af0ad3"), "RC4 %s", h);
}

static void test_more(const char *dir)
{
    uint32_t p = x360_kalloc(X, 0x400);
    /* strings */
    uint32_t u = p, ustr = p + 0x40, astr = p + 0x50;
    const char *w = "Save1";
    for (int k = 0; k < 5; k++)
        x360_w16(X, u + 2 * (uint32_t)k, (uint8_t)w[k]);
    x360_w16(X, ustr, 10);
    x360_w16(X, ustr + 2, 12);
    x360_w32(X, ustr + 4, u);
    CHECK(K(X360_xboxkrnl_RtlUnicodeStringToAnsiString, 3, astr, ustr, 1) == 0 && x360_r16(X, astr) == 5 &&
              !memcmp(x360_p(X, x360_r32(X, astr + 4)), "Save1", 6), "unicode to ANSI string");
    CHECK(K(X360_xboxkrnl_RtlFreeAnsiString, 1, astr) == 0 && x360_r32(X, astr + 4) == 0, "free ANSI string");
    CHECK(K(X360_xboxkrnl_RtlUpcaseUnicodeChar, 1, 'q') == 'Q', "upcase");
    /* memory statistics */
    x360_w32(X, p + 0x100, 104);
    CHECK(K(X360_xboxkrnl_MmQueryStatistics, 1, p + 0x100) == 0 && x360_r32(X, p + 0x104) == 0x20000 &&
              x360_r32(X, p + 0x10C) > 0 && x360_r32(X, p + 0x10C) <= 0x20000, "memory statistics");
    /* save data: content created in the data directory, mounted on its root name */
    x360_set_data_dir(X, dir);
    uint32_t cd = x360_kalloc(X, 0x134), disp = x360_kalloc(X, 4);
    x360_w32(X, cd, 1);
    memcpy(x360_p(X, cd + 0x108), "slot0", 6);
    uint32_t root = x360_kalloc(X, 8);
    memcpy(x360_p(X, root), "save", 5);
    uint32_t r = XM(X360_xam_XamContentCreateEx, 9, 0, root, cd, 4, disp, 0, 0, 0, 0); /* OPEN_ALWAYS */
    CHECK(r == 0 && x360_r32(X, disp) == 1, "content created: %X, disposition %u", r, x360_r32(X, disp));
    uint32_t hp = x360_kalloc(X, 8), iosb = x360_kalloc(X, 8);
    CHECK(K(X360_xboxkrnl_NtCreateFile, 9, hp, 0xC0000000u, attrs("save:\\data.bin"), iosb, 0, 0, 0, 5, 0x40) == 0,
          "a file in the content");
    K(X360_xboxkrnl_NtClose, 1, x360_r32(X, hp));
    char host[600];
    snprintf(host, sizeof host, "%s/content/slot0/data.bin", dir);
    CHECK(access(host, F_OK) == 0, "it is %s", host);
    unlink(host);
    snprintf(host, sizeof host, "%s/content/slot0", dir);
    rmdir(host);
    snprintf(host, sizeof host, "%s/content", dir);
    rmdir(host);
    /* profile settings: the size first, then defaults */
    uint32_t ids = p + 0x200, sizep = p + 0x210, buf = x360_kalloc(X, 0x100);
    x360_w32(X, ids, 0x10040002u);
    x360_w32(X, ids + 4, 0x50040011u);
    x360_w32(X, sizep, 0);
    CHECK(XM(X360_xam_XamUserReadProfileSettings, 9, 0, 0, 0, 0, 2, ids, sizep, 0, 0) == 0x7A &&
              x360_r32(X, sizep) == 8 + 80, "profile settings: size");
    CHECK(XM(X360_xam_XamUserReadProfileSettings, 9, 0, 0, 0, 0, 2, ids, sizep, buf, 0) == 0 &&
              x360_r32(X, buf) == 2 && x360_r32(X, buf + 8 + 40 + 16) == 0x50040011u &&
              x360_r8(X, buf + 8 + 40 + 24) == 5, "profile settings: defaults");
    /* a message box answered at once through its overlapped, which signals the event */
    uint32_t ov = x360_kalloc(X, 0x1C), res = x360_kalloc(X, 4), evh = x360_kalloc(X, 4);
    K(X360_xboxkrnl_NtCreateEvent, 4, evh, 0, 0, 0);
    x360_w32(X, ov + 0xC, x360_r32(X, evh));
    x360_w32(X, res, 7);
    CHECK(XM(X360_xam_XamShowMessageBoxUI, 9, 0, 0, 0, 1, 0, 0, 0, res, ov) == 0x3E5 && x360_r32(X, ov) == 0 &&
              x360_r32(X, res) == 0 && x360_r32(X, x360_handle_obj(X, x360_r32(X, evh)) + 4) == 1,
          "message box through an overlapped");
    /* the audio driver's client callback runs every frame on a guest context, given the address of a word that holds
     * its argument (XAudio2's routine starts with lwz r3,0(r3)) */
    uint32_t cb = x360_kalloc(X, 8), drv = x360_kalloc(X, 4);
    x360_w32(X, cb, 0x82002000u);
    x360_w32(X, cb + 4, 0x1234);
    CHECK(K(X360_xboxkrnl_XAudioRegisterRenderDriverClient, 2, cb, drv) == 0 &&
              (x360_r32(X, drv) & 0xFFFF0000u) == 0x41550000u, "audio client registered");
    X->time_skip += 60000;
    x360_audio_poll(X);
    x360_audio_client *a = &X->audio[x360_r32(X, drv) & 0xFFFF];
    uint32_t cell = a->thread >= 0 ? (uint32_t)X->threads[a->thread].cpu.gpr[3] : 0;
    CHECK(a->thread >= 0 && X->threads[a->thread].cpu.pc == 0x82002000u && cell && x360_r32(X, cell) == 0x1234,
          "audio callback context: r3 %08X holds %08X", cell, cell ? x360_r32(X, cell) : 0);
    /* registered again (another argument): the slot's word again, no new block */
    uint32_t kcur = X->kalloc_cur;
    K(X360_xboxkrnl_XAudioUnregisterRenderDriverClient, 1, x360_r32(X, drv));
    x360_w32(X, cb + 4, 0x5678);
    K(X360_xboxkrnl_XAudioRegisterRenderDriverClient, 2, cb, drv);
    CHECK(X->kalloc_cur == kcur, "registering again allocates nothing (%u bytes)", X->kalloc_cur - kcur);
    X->time_skip += 60000;
    x360_audio_poll(X);
    a = &X->audio[x360_r32(X, drv) & 0xFFFF];
    CHECK(a->thread >= 0 && X->threads[a->thread].cpu.gpr[3] == cell && x360_r32(X, cell) == 0x5678,
          "audio client registered again: r3 %08X holds %08X", (uint32_t)X->threads[a->thread].cpu.gpr[3],
          x360_r32(X, cell));
    CHECK(XM(X360_xam_NetDll_inet_addr, 2, 0, attrs("10.1.2.3") ? x360_r32(X, x360_r32(X, attrs("10.1.2.3") + 4) + 4)
                                                                 : 0) == 0x0A010203u, "inet_addr");
    CHECK(XM(X360_xam_NetDll_socket, 4, 0, 2, 1, 6) == 0xFFFFFFFFu, "no network: sockets fail");
}

static void test_audio_out(void)
{
    /* a client, one submitted frame: channel c, sample s = c + s / 1000 */
    char path[] = "/tmp/x360_audio_XXXXXX";
    int fd = mkstemp(path);
    FILE *f = fd >= 0 ? fdopen(fd, "w+b") : NULL;
    CHECK(f != NULL, "temporary WAV");
    if (!f)
        return;
    x360_set_audio_out(X, f);
    uint32_t cb = x360_kalloc(X, 8), drv = x360_kalloc(X, 4), buf = x360_kalloc(X, 4 * 6 * 256);
    x360_w32(X, cb, 0x82000000u), x360_w32(X, cb + 4, 0);
    K(X360_xboxkrnl_XAudioRegisterRenderDriverClient, 2, cb, drv);
    for (int ch = 0; ch < 6; ch++)
        for (int s = 0; s < 256; s++) {
            float v = (float)ch + (float)s / 1000.0f;
            uint32_t bits;
            memcpy(&bits, &v, 4);
            x360_w32(X, buf + 4 * (uint32_t)(ch * 256 + s), bits);
        }
    K(X360_xboxkrnl_XAudioSubmitRenderDriverFrame, 2, x360_r32(X, drv), buf);
    K(X360_xboxkrnl_XAudioUnregisterRenderDriverClient, 1, x360_r32(X, drv));
    x360_audio_close(X);
    uint8_t h[44];
    float samp[12];
    fseek(f, 0, SEEK_SET);
    CHECK(fread(h, 1, 44, f) == 44 && fread(samp, 4, 12, f) == 12, "WAV read back");
    uint32_t data = h[40] | h[41] << 8 | h[42] << 16 | (uint32_t)h[43] << 24;
    CHECK(!memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVEfmt ", 8) && h[20] == 3 && h[22] == 6 && data == 6144,
          "WAV header (format %u, channels %u, data %u)", h[20], h[22], data);
    CHECK(samp[0] == 0.0f && samp[5] == 5.0f && samp[6] == 0.001f && samp[11] == 5.001f,
          "interleaved samples %g %g %g %g", samp[0], samp[5], samp[6], samp[11]);
    fclose(f);
    unlink(path);
}

/* XMA contexts (XBA-280): synthetic XMA2 packets (headers only: the default backend gives each frame 512 samples
 * of silence), the input buffers' flow, the output ring and its read offset, the APU kick register; the render
 * driver's mix; the motors */
/* XMA loops (XBA-299): a backend that notes which packets it is given and when it is opened */
static uint8_t loop_seen[32];
static int loop_n, loop_opens;
static void *loop_open(int ch, int rate) { (void)ch, (void)rate; loop_opens++; return &loop_seen; }
static int loop_decode(void *d, const uint8_t *p, int16_t *out, int max)
{
    (void)d, (void)out, (void)max;
    if (loop_n < 32)
        loop_seen[loop_n++] = p[4];
    return 0;
}
static void loop_close(void *d) { (void)d; }
static int loop_drains;
static int loop_drain(void *d, int16_t *out, int max)
{
    (void)d, (void)max;
    loop_drains++;
    memset(out, 0, 128 * 2);
    return 128; /* one output block */
}

static void test_xma_loop(void)
{
    static const x360_xma_backend rec = {"loop test", loop_open, loop_decode, loop_close, loop_drain};
    x360_xma_set_backend(&rec);
    uint32_t pctx = x360_kalloc(X, 4);
    K(X360_xboxkrnl_XMACreateContext, 1, pctx);
    uint32_t ctx = x360_r32(X, pctx);
    uint32_t in = K(X360_xboxkrnl_MmAllocatePhysicalMemoryEx, 6, 0, 0x2000, 4, 0, 0xFFFFFFFFu, 0x1000);
    uint32_t out = K(X360_xboxkrnl_MmAllocatePhysicalMemoryEx, 6, 0, 0x2000, 4, 0, 0xFFFFFFFFu, 0x1000);
    for (uint32_t p = 0; p < 4; p++)
        x360_w8(X, in + 2048 * p + 4, (uint8_t)(0x10 + p)); /* each packet says which it is */
    uint32_t init = x360_kalloc(X, 64);
    /* four packets, loop from the middle of packet 1 to the end of packet 2, twice */
    uint32_t iv[13] = {in, 4, 0, 0, 0, out, 31, 0, 0, 1, 3, 2048 * 8 + 40, 3 * 2048 * 8};
    for (int k = 0; k < 13; k++)
        x360_w32(X, init + 4 * (uint32_t)k, iv[k]);
    x360_w8(X, init + 52, 2);
    K(X360_xboxkrnl_XMAInitializeContext, 2, ctx, init);
    K(X360_xboxkrnl_XMASetInputBuffer0Valid, 1, ctx);
    K(X360_xboxkrnl_XMASetOutputBufferValid, 1, ctx);
    K(X360_xboxkrnl_XMAEnableContext, 1, ctx);
    static const uint8_t want[] = {0x10, 0x11, 0x12, 0x11, 0x12, 0x11, 0x12, 0x13};
    CHECK(loop_n == (int)sizeof want && !memcmp(loop_seen, want, sizeof want), "XMA loop: %d packets, the 4th is %02X",
          loop_n, loop_seen[3]);
    CHECK(loop_opens == 3 && loop_drains == 2, "XMA loop: a fresh decoder after each jump (%d opens, %d drains)",
          loop_opens, loop_drains);
    /* the input has ended: not at the first poll (a title may be refilling), then the decoder's last samples go out */
    uint32_t wo = K(X360_xboxkrnl_XMAGetOutputBufferWriteOffset, 1, ctx);
    x360_xma_poll(X);
    CHECK(loop_drains == 2, "XMA end of input: the decoder is left alone at first");
    for (int k = 0; k < 16; k++)
        x360_xma_poll(X);
    CHECK(loop_drains == 3 && K(X360_xboxkrnl_XMAGetOutputBufferWriteOffset, 1, ctx) == wo + 1,
          "XMA end of input: %d drains, write offset %u after %u", loop_drains,
          K(X360_xboxkrnl_XMAGetOutputBufferWriteOffset, 1, ctx), wo);
    K(X360_xboxkrnl_XMAReleaseContext, 1, ctx);
    x360_xma_set_backend(NULL);
}

static void test_xma(void)
{
    x360_xma_set_backend(NULL); /* these packets are headers only: the silent backend */
    uint32_t pctx = x360_kalloc(X, 4);
    CHECK(K(X360_xboxkrnl_XMACreateContext, 1, pctx) == 0 && x360_r32(X, pctx) >= 0xA0000000u, "XMA context");
    uint32_t ctx = x360_r32(X, pctx);
    uint32_t in = K(X360_xboxkrnl_MmAllocatePhysicalMemoryEx, 6, 0, 0x2000, 4, 0, 0xFFFFFFFFu, 0x1000);
    uint32_t out = K(X360_xboxkrnl_MmAllocatePhysicalMemoryEx, 6, 0, 0x2000, 4, 0, 0xFFFFFFFFu, 0x1000);
    for (int p = 0; p < 2; p++) { /* two packets of 3 frames: header frame count 3, metadata 1, no skip */
        x360_w8(X, in + 2048 * (uint32_t)p, 3u << 2);
        x360_w8(X, in + 2048 * (uint32_t)p + 2, 1);
        x360_w8(X, in + 2048 * (uint32_t)p + 3, 0);
    }
    for (uint32_t k = 0; k < 0x2000; k += 4)
        x360_w32(X, out + k, 0x55555555u);
    uint32_t init = x360_kalloc(X, 64);
    uint32_t iv[11] = {in, 2, 0, 0, 0, out, 31, 0, 0, 1, 0}; /* 31 blocks: a 5-bit count */
    for (int k = 0; k < 11; k++)
        x360_w32(X, init + 4 * (uint32_t)k, iv[k]);
    K(X360_xboxkrnl_XMAInitializeContext, 2, ctx, init);
    K(X360_xboxkrnl_XMASetInputBuffer0Valid, 1, ctx);
    K(X360_xboxkrnl_XMASetOutputBufferValid, 1, ctx);
    CHECK(K(X360_xboxkrnl_XMAIsInputBuffer0Valid, 1, ctx) == 1 && K(X360_xboxkrnl_XMAIsOutputBufferValid, 1, ctx) == 1,
          "XMA valid bits");
    K(X360_xboxkrnl_XMAEnableContext, 1, ctx);
    /* 2 packets x 3 frames x 512 samples x 2 bytes = 24 blocks of 256 bytes; buffer 0 read to its end */
    uint64_t packets, frames, blocks;
    unsigned nctx;
    x360_xma_get_stats(X, &packets, &frames, &blocks, &nctx);
    CHECK(K(X360_xboxkrnl_XMAGetOutputBufferWriteOffset, 1, ctx) == 24 && packets == 2 && frames == 6 &&
              K(X360_xboxkrnl_XMAIsInputBuffer0Valid, 1, ctx) == 0 && nctx == 1 &&
              K(X360_xboxkrnl_XMAGetPacketMetadata, 1, ctx) == 1,
          "XMA decode: write offset %u, packets %llu, frames %llu, buffer 0 valid %u",
          K(X360_xboxkrnl_XMAGetOutputBufferWriteOffset, 1, ctx), (unsigned long long)packets,
          (unsigned long long)frames, K(X360_xboxkrnl_XMAIsInputBuffer0Valid, 1, ctx));
    CHECK(x360_r32(X, out) == 0 && x360_r32(X, out + 24 * 256 - 4) == 0 && x360_r32(X, out + 24 * 256) == 0x55555555u,
          "XMA output: 24 blocks of silence, the rest untouched");
    /* again: the ring fills up to the block before the read offset (0): 6 more blocks, the rest waits */
    K(X360_xboxkrnl_XMASetInputBuffer0Valid, 1, ctx);
    x360_xma_poll(X);
    CHECK(K(X360_xboxkrnl_XMAGetOutputBufferWriteOffset, 1, ctx) == 30, "XMA ring full at 30 (got %u)",
          K(X360_xboxkrnl_XMAGetOutputBufferWriteOffset, 1, ctx));
    K(X360_xboxkrnl_XMASetOutputBufferReadOffset, 2, ctx, 20);
    x360_xma_poll(X);
    CHECK(K(X360_xboxkrnl_XMAGetOutputBufferWriteOffset, 1, ctx) == 17 &&
              K(X360_xboxkrnl_XMAIsInputBuffer0Valid, 1, ctx) == 0,
          "XMA ring drained to the read offset: write offset %u", K(X360_xboxkrnl_XMAGetOutputBufferWriteOffset, 1, ctx));
    /* the APU's kick register enables a context, as titles' XAudio2 does without the export */
    K(X360_xboxkrnl_XMADisableContext, 1, ctx);
    K(X360_xboxkrnl_XMASetInputBuffer0Valid, 1, ctx);
    K(X360_xboxkrnl_XMASetOutputBufferReadOffset, 2, ctx, 17);
    x360_xma_poll(X);
    CHECK(K(X360_xboxkrnl_XMAIsInputBuffer0Valid, 1, ctx) == 1, "XMA disabled: nothing decoded");
    x360_w32(X, 0x7FEA0000u + 4 * 0x650, 1u); /* context 0 */
    x360_xma_poll(X);
    CHECK(K(X360_xboxkrnl_XMAIsInputBuffer0Valid, 1, ctx) == 0 && x360_r32(X, 0x7FEA0000u + 4 * 0x650) == 0,
          "XMA kick register");
    K(X360_xboxkrnl_XMAReleaseContext, 1, ctx);
    x360_xma_get_stats(X, &packets, &frames, &blocks, &nctx);
    CHECK(nctx == 0, "XMA context released");

    /* the render driver's mix: front left 0.25, centre 0.1, surround right 0.5 -> L 0.3207, R 0.4243 */
    int16_t pcm[600 * 2];
    x360_audio_tick(X);
    while (x360_audio_pull(X, pcm, 600))
        ;
    uint32_t cb = x360_kalloc(X, 8), drv = x360_kalloc(X, 4), buf = x360_kalloc(X, 4 * 6 * 256);
    x360_w32(X, cb, 0x82000000u), x360_w32(X, cb + 4, 0);
    K(X360_xboxkrnl_XAudioRegisterRenderDriverClient, 2, cb, drv);
    static const float level[6] = {0.25f, 0, 0.1f, 0, 0, 0.5f};
    for (int ch = 0; ch < 6; ch++)
        for (int s = 0; s < 256; s++) {
            uint32_t bits;
            memcpy(&bits, &level[ch], 4);
            x360_w32(X, buf + 4 * (uint32_t)(ch * 256 + s), bits);
        }
    K(X360_xboxkrnl_XAudioSubmitRenderDriverFrame, 2, x360_r32(X, drv), buf);
    K(X360_xboxkrnl_XAudioSubmitRenderDriverFrame, 2, x360_r32(X, drv), buf);
    x360_audio_tick(X);
    size_t got = x360_audio_pull(X, pcm, 600);
    CHECK(got == 512 && abs(pcm[0] - 10509) <= 2 && abs(pcm[1] - 13903) <= 2 && pcm[2 * 511] == pcm[0],
          "audio mix: %zu frames, L %d R %d", got, pcm[0], pcm[1]);
    K(X360_xboxkrnl_XAudioUnregisterRenderDriverClient, 1, x360_r32(X, drv));

    /* vibration: XamInputSetState's motor speeds */
    x360_pad pad = {0};
    x360_set_pad(X, 0, &pad);
    uint32_t vib = x360_kalloc(X, 4);
    x360_w16(X, vib, 0x8000), x360_w16(X, vib + 2, 0xFF00);
    unsigned ml, mr;
    CHECK(XM(X360_xam_XamInputSetState, 3, 0, 0, vib) == 0, "XamInputSetState");
    x360_pad_motors(X, 0, &ml, &mr);
    CHECK(ml == 128 && mr == 255, "motors %u %u", ml, mr);
    x360_set_pad(X, 0, NULL);
}

static void test_input_and_cheats(void)
{
    uint32_t st = x360_kalloc(X, 32);
    x360_pad p = {0x1010, 200, 5, -32768, 1234, 32767, -1};
    x360_set_pad(X, 1, &p);
    CHECK(XM(X360_xam_XamInputGetState, 3, 1, 0, st) == 0 && x360_r16(X, st + 4) == 0x1010 &&
              x360_r8(X, st + 6) == 200 && x360_r8(X, st + 7) == 5 && x360_r16(X, st + 8) == 0x8000 &&
              x360_r16(X, st + 10) == 1234 && x360_r16(X, st + 12) == 0x7FFF && x360_r16(X, st + 14) == 0xFFFF,
          "pad state");
    uint32_t packet = x360_r32(X, st);
    CHECK(XM(X360_xam_XamInputGetState, 3, 2, 0, st) == 0x48F, "user 2 not connected");
    CHECK(XM(X360_xam_XamInputGetCapabilities, 3, 1, 0, st) == 0 && x360_r8(X, st) == 1, "capabilities");
    /* a script: A pressed at 5 ms, the pad unplugged at 10 ms */
    uint64_t now_ms = x360_now(X) / 10000;
    x360_pad_event ev[2] = {{(uint32_t)now_ms + 5, 1, 1, {0x1000, 0, 0, 0, 0, 0, 0}},
                            {(uint32_t)now_ms + 10, 1, 0, {0}}};
    x360_set_pad_script(X, ev, 2);
    X->time_skip += 60000;
    x360_input_poll(X);
    CHECK(XM(X360_xam_XamInputGetState, 3, 1, 0, st) == 0 && x360_r16(X, st + 4) == 0x1000 &&
              x360_r32(X, st) != packet, "scripted state, a new packet number");
    X->time_skip += 60000;
    x360_input_poll(X);
    CHECK(XM(X360_xam_XamInputGetState, 3, 1, 0, st) == 0x48F, "scripted unplug");
    /* pads and the script position survive a save state: rewind the script, save, run it on, load */
    x360_set_pad(X, 3, &p);
    x360_set_pad_script(X, ev, 2);
    size_t cap = x360_state_size(X), n;
    uint8_t *blob = malloc(cap);
    CHECK(blob && (n = x360_state_save(X, NULL, blob, cap)) > 0, "state saved");
    X->time_skip += 120000;
    x360_input_poll(X);
    x360_set_pad(X, 3, NULL);
    char why[128];
    CHECK(blob && x360_state_load(X, NULL, blob, n, why, sizeof why) == 0, "state loaded");
    CHECK(XM(X360_xam_XamInputGetState, 3, 3, 0, st) == 0 && x360_r16(X, st + 4) == 0x1010, "pad 3 restored");
    CHECK(X->pad_script_at == 0, "script position restored");
    free(blob);
    x360_set_pad(X, 3, NULL);
    x360_set_pad(X, 1, NULL);
    x360_set_pad_script(X, NULL, 0);
    /* cheats: allocated memory only */
    x360_cheat_write32(X, st, 0xCAFEF00Du);
    CHECK(x360_cheat_read32(X, st) == 0xCAFEF00Du, "cheat word");
    CHECK(x360_cheat_read32(X, 0x00001000u) == 0, "unallocated: 0");
    x360_cheat_write32(X, 0x00001000u, 1); /* dropped, no fault */
}

static void test_strict_imports(void)
{
    X->strict_imports = 1;
    XM(X360_xam_XamSwapDisc, 0);
    CHECK(X->status == X360_ERROR && strstr(X->error, "XamSwapDisc"), "strict imports: %s",
          X->error);
    X->status = X360_RUNNING;
    X->strict_imports = 0;
}

static void test_files(const char *dir)
{
    x360_set_data_dir(X, dir);
    uint32_t hp = x360_kalloc(X, 8), iosb = x360_kalloc(X, 8), info = x360_kalloc(X, 64), buf = x360_kalloc(X, 64);
    uint32_t link = x360_r32(X, attrs("\\??\\save:") + 4);
    uint32_t target = x360_r32(X, attrs("\\Device\\Harddisk0\\Partition1") + 4);
    CHECK(K(X360_xboxkrnl_ObCreateSymbolicLink, 2, link, target) == 0, "ObCreateSymbolicLink");
    /* NtCreateFile: create options are the ninth argument (stack) */
    uint32_t st = K(X360_xboxkrnl_NtCreateFile, 9, hp, 0xC0000000u, attrs("save:\\dir\\..\\x"), iosb, 0, 0, 0, 5,
                    0x40);
    CHECK(st == 0xC000003Au, "a path with .. is refused: %08X", st);
    st = K(X360_xboxkrnl_NtCreateFile, 9, hp, 0xC0000000u, attrs("save:\\test.bin"), iosb, 0, 0, 0, 5, 0x40);
    CHECK(st == 0 && x360_r32(X, iosb + 4) == 2, "created: %08X, information %u", st, x360_r32(X, iosb + 4));
    uint32_t h = x360_r32(X, hp);
    memcpy(x360_p(X, buf), "hello world", 11);
    CHECK(K(X360_xboxkrnl_NtWriteFile, 8, h, 0, 0, 0, iosb, buf, 11, 0) == 0 && x360_r32(X, iosb + 4) == 11,
          "NtWriteFile");
    CHECK(K(X360_xboxkrnl_NtQueryInformationFile, 5, h, iosb, info, 8, 14) == 0 && x360_r64(X, info) == 11,
          "position after the write");
    x360_w64(X, info, 6);
    CHECK(K(X360_xboxkrnl_NtSetInformationFile, 5, h, iosb, info, 8, 14) == 0, "seek");
    memset(x360_p(X, buf), 0, 64);
    CHECK(K(X360_xboxkrnl_NtReadFile, 8, h, 0, 0, 0, iosb, buf, 64, 0) == 0 && x360_r32(X, iosb + 4) == 5 &&
              !memcmp(x360_p(X, buf), "world", 5), "read from the position");
    CHECK(K(X360_xboxkrnl_NtReadFile, 8, h, 0, 0, 0, iosb, buf, 64, 0) == 0xC0000011u, "end of file");
    CHECK(K(X360_xboxkrnl_NtClose, 1, h) == 0, "close");
    CHECK(K(X360_xboxkrnl_NtQueryFullAttributesFile, 2, attrs("\\Device\\Harddisk0\\Partition1\\test.bin"), info) ==
              0 && x360_r64(X, info + 40) == 11 && x360_r32(X, info + 48) == 0x80, "full attributes");
    CHECK(K(X360_xboxkrnl_NtOpenFile, 5, hp, 1, attrs("game:\\nothing"), iosb, 0x40) == 0xC000003Au,
          "no disc: path not found");
    char host[600];
    snprintf(host, sizeof host, "%s/test.bin", dir);
    FILE *f = fopen(host, "rb");
    char got[16] = "";
    CHECK(f && fread(got, 1, 11, f) == 11 && !memcmp(got, "hello world", 11), "the host file");
    if (f)
        fclose(f);
    unlink(host);
}

/* x360_hle_test --render DIR (ctest x360_render, after make_draw.py): the renderer through the machine, the way
 * x360_run --render drives it -- x360_set_renderer, a ring the title fills, VdSwap with the front buffer's fetch
 * constant, x360_gpu_poll between slices: a red quad resolved (with a green clear) to the front buffer, which the
 * frame comes from; then a save state is loaded and the renderer still draws. 77 without EGL / GLES 3.0. */
static uint32_t *load_words(const char *dir, const char *name, uint32_t *n)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    uint8_t b[4096];
    size_t len = fread(b, 1, sizeof b, f);
    fclose(f);
    uint32_t *w = malloc(len + 4);
    for (size_t i = 0; i + 4 <= len; i += 4)
        w[i / 4] = (uint32_t)b[i] << 24 | (uint32_t)b[i + 1] << 16 | (uint32_t)b[i + 2] << 8 | b[i + 3];
    *n = (uint32_t)(len / 4);
    return w;
}

static uint32_t ring_va, ring_at; /* the ring's guest address, the next dword */

static void rput(uint32_t v)
{
    x360_w32(X, ring_va + 4 * ring_at++, v);
}

static void rreg(uint32_t index, uint32_t v)
{
    rput(index);
    rput(v);
}

static uint32_t fbits32(float f)
{
    uint32_t v;
    memcpy(&v, &f, 4);
    return v;
}

static int render_test(const char *dir)
{
    char why[256] = "", cache[1024];
    snprintf(cache, sizeof cache, "%s/cache", dir);
    if (x360_set_renderer(X, cache, NULL, 0, 64, 64, why, sizeof why)) {
        printf("skipped: %s\n", why);
        return 77;
    }
    uint32_t nvs, nps;
    uint32_t *vs = load_words(dir, "vs.bin", &nvs), *ps = load_words(dir, "ps.bin", &nps);
    CHECK(vs && ps, "make_draw.py's shaders in %s", dir);
    if (!vs || !ps)
        return 1;
    uint32_t ring = K(X360_xboxkrnl_MmAllocatePhysicalMemoryEx, 6, 0, 0x4000, 4, 0, 0xFFFFFFFFu, 0);
    uint32_t data = K(X360_xboxkrnl_MmAllocatePhysicalMemoryEx, 6, 0, 0x20000, 4, 0, 0xFFFFFFFFu, 0x1000);
    uint32_t ring_phys = K(X360_xboxkrnl_MmGetPhysicalAddress, 1, ring);
    uint32_t data_phys = K(X360_xboxkrnl_MmGetPhysicalAddress, 1, data);
    uint32_t fb = data + 0x10000, fb_phys = data_phys + 0x10000; /* the front buffer: 64x64 8_8_8_8, tiled */
    K(X360_xboxkrnl_VdInitializeRingBuffer, 2, ring_phys, 12);
    ring_va = ring, ring_at = 0;
    /* a red full-target quad (strip), then the resolve rectangle's vertices */
    static const float quad[4][8] = {{-1, -1, 0.5f, 1, 1, 0, 0, 1}, {1, -1, 0.5f, 1, 1, 0, 0, 1},
                                     {-1, 1, 0.5f, 1, 1, 0, 0, 1}, {1, 1, 0.5f, 1, 1, 0, 0, 1}};
    for (int v = 0; v < 4; v++)
        for (int i = 0; i < 8; i++)
            x360_w32(X, data + 32 * (uint32_t)v + 4 * (uint32_t)i, fbits32(quad[v][i]));
    static const float rect[6] = {0, 0, 64, 0, 64, 64};
    for (int i = 0; i < 6; i++)
        x360_w32(X, data + 0x200 + 4 * (uint32_t)i, fbits32(rect[i]));
    rput(3u << 30 | (2 + nvs - 1) << 16 | (uint32_t)PM4_IM_LOAD_IMMEDIATE << 8);
    rput(0), rput(nvs);
    for (uint32_t i = 0; i < nvs; i++)
        rput(vs[i]);
    rput(3u << 30 | (2 + nps - 1) << 16 | (uint32_t)PM4_IM_LOAD_IMMEDIATE << 8);
    rput(1), rput(nps);
    for (uint32_t i = 0; i < nps; i++)
        rput(ps[i]);
    rreg(0x2104, 0xF), rreg(0x2201, 0x00010001u), rreg(0x2208, 4);
    rreg(0x2206, 0x3F), rreg(0x210F, fbits32(32.0f)), rreg(0x2110, fbits32(32.0f)), rreg(0x2111, fbits32(-32.0f)),
        rreg(0x2112, fbits32(32.0f)); /* Direct3D's full-target viewport */
    for (int i = 0; i < 4; i++)
        rreg(0x4400 + (uint32_t)i, fbits32(1.0f));
    rreg(0x4800, data_phys | 3u), rreg(0x4801, 2u | 32u << 2);
    rput(3u << 30 | 0u << 16 | (uint32_t)PM4_DRAW_INDX_2 << 8);
    rput(6u | 2u << 6 | 4u << 16);
    /* the resolve to the front buffer, clearing the target to green */
    rreg(0x4800, (data_phys + 0x200) | 3u), rreg(0x4801, 2u | 6u << 2);
    rreg(0x2208, 6), rreg(0x2318, 1u << 8), rreg(0x2319, fb_phys), rreg(0x231A, 64u | 64u << 16),
        rreg(0x231B, 2u | 6u << 7), rreg(0x231E, 0xFF00FF00u);
    rput(3u << 30 | 0u << 16 | (uint32_t)PM4_DRAW_INDX_2 << 8);
    rput(8u | 2u << 6 | 3u << 16);
    rreg(0x2208, 4);
    /* VdSwap: the front buffer's Direct3D texture header fetch constant (virtual base address) */
    uint32_t args = x360_kalloc(X, 64), fetch = args + 32;
    uint32_t fcw[6] = {2u | 2u << 22 | 1u << 31, 6u | 2u << 6 | (fb & 0xFFFFF000u), 63u | 63u << 13,
                       (0u | 1u << 3 | 2u << 6 | 3u << 9) << 1, 0, 1u << 9};
    for (int i = 0; i < 6; i++)
        x360_w32(X, fetch + 4 * (uint32_t)i, fcw[i]);
    x360_w32(X, args, fb), x360_w32(X, args + 4, 64), x360_w32(X, args + 8, 64), x360_w32(X, args + 12, 6);
    x360_w32(X, args + 16, 0);
    uint32_t swap_at = ring + 4 * ring_at;
    ring_at += 64;
    K(X360_xboxkrnl_VdSwap, 10, swap_at, fetch, 0, 0, 0, args, args + 12, args + 16, args + 4, args + 8);
    x360_w32(X, 0x7FC80000u + 4 * XENOS_REG_CP_RB_WPTR, ring_at);
    x360_gpu_poll(X);
    int w = 0, h = 0;
    const uint8_t *f = x360_render_frame(X, &w, &h);
    uint64_t drawn, frames, fbf;
    x360_render_counts(X, &drawn, &frames, &fbf);
    CHECK(f && w == 64 && f[4 * (32 * 64 + 32)] == 255 && f[4 * (32 * 64 + 32) + 1] == 0 && drawn == 1 &&
              frames == 1 && fbf == 1,
          "frame from the front buffer: %u,%u,%u, drawn %llu, frames %llu, from the front buffer %llu",
          f ? f[4 * (32 * 64 + 32)] : 0, f ? f[4 * (32 * 64 + 32) + 1] : 0, f ? f[4 * (32 * 64 + 32) + 2] : 0,
          (unsigned long long)drawn, (unsigned long long)frames, (unsigned long long)fbf);
    /* a save state, loaded back: the renderer is host state and keeps drawing (the hooks survive) */
    size_t cap = x360_state_size(X), n = 0;
    uint8_t *blob = malloc(cap);
    CHECK(blob && (n = x360_state_save(X, NULL, blob, cap)) > 0, "state saved");
    CHECK(blob && x360_state_load(X, NULL, blob, n, why, sizeof why) == 0, "state loaded: %s", why);
    free(blob);
    uint32_t start = ring_at;
    rreg(0x4800, data_phys | 3u), rreg(0x4801, 2u | 32u << 2);
    rput(3u << 30 | 0u << 16 | (uint32_t)PM4_DRAW_INDX_2 << 8);
    rput(6u | 2u << 6 | 4u << 16);
    x360_w32(X, 0x7FC80000u + 4 * XENOS_REG_CP_RB_WPTR, ring_at);
    x360_gpu_poll(X);
    x360_render_counts(X, &drawn, &frames, &fbf);
    CHECK(drawn == 2 && ring_at > start, "drawn after the state was loaded: %llu", (unsigned long long)drawn);
    free(vs);
    free(ps);
    return 0;
}

/* a section's bytes in a state blob (72-byte header, then tag, version, length, bytes), or NULL */
static uint8_t *state_section(uint8_t *blob, size_t n, const char *tag, uint32_t *len)
{
    for (size_t p = 72; p + 12 <= n;) {
        uint32_t l = (uint32_t)blob[p + 8] | (uint32_t)blob[p + 9] << 8 | (uint32_t)blob[p + 10] << 16 |
                     (uint32_t)blob[p + 11] << 24;
        if (!memcmp(blob + p, tag, 4)) {
            *len = l;
            return blob + p + 12;
        }
        p += 12 + l;
    }
    return NULL;
}

/* A damaged state is refused and leaves the machine as it was (no dead machine, no write outside the pages in use) */
static void test_damaged_states(void)
{
    char why[256] = "";
    x360_w32(X, scratch + 0x100, 0xC0FFEE01u);
    size_t cap = x360_state_size(X), n = 0;
    uint8_t *blob = malloc(cap), *bad = malloc(cap + 4);
    CHECK(blob && bad && (n = x360_state_save(X, NULL, blob, cap)) > 0, "state saved");
    if (!blob || !bad || !n) {
        free(blob);
        free(bad);
        return;
    }
    x360_w32(X, scratch + 0x100, 0xC0FFEE02u); /* the machine moved on after the save */
    uint32_t len = 0;
    /* MEM naming a page no heap holds: refused before anything is written there */
    memcpy(bad, blob, n);
    uint8_t *mem = state_section(bad, n, "MEM ", &len);
    CHECK(mem && len >= 4 + 4096, "the state has a MEM section");
    if (mem) {
        uint32_t nowhere = 0x3FF00000u; /* in the 4 KB heap, far past what the tests allocate */
        memcpy(mem, &nowhere, 4);    /* little-endian host: the stream's order */
        int rc = x360_state_load_undo(X, NULL, bad, n, why, sizeof why);
        CHECK(rc == X360_STATE_ECORRUPT && strstr(why, "MEM") && strstr(why, "back as it was"),
              "a page outside the heaps: %d (%s)", rc, why);
        CHECK(X->status == X360_RUNNING && x360_r32(X, scratch + 0x100) == 0xC0FFEE02u,
              "the machine is as it was before the load (status %d, %08X)", X->status, x360_r32(X, scratch + 0x100));
    }
    /* KERN one field longer (another build's layout): refused, the machine as it was */
    uint8_t *kern = state_section(blob, n, "KERN", &len);
    CHECK(kern != NULL, "the state has a KERN section");
    if (kern) {
        size_t at = (size_t)(kern - blob), end = at + len;
        memcpy(bad, blob, end);
        memset(bad + end, 0, 4);
        memcpy(bad + end + 4, blob + end, n - end);
        uint32_t l2 = len + 4;
        for (int i = 0; i < 4; i++)
            bad[at - 4 + i] = (uint8_t)(l2 >> (8 * i));
        int rc = x360_state_load_undo(X, NULL, bad, n + 4, why, sizeof why);
        CHECK(rc == X360_STATE_ECORRUPT && strstr(why, "KERN"), "a longer KERN section: %d (%s)", rc, why);
        CHECK(X->status == X360_RUNNING && x360_r32(X, scratch + 0x100) == 0xC0FFEE02u,
              "the machine is as it was before the load (status %d, %08X)", X->status, x360_r32(X, scratch + 0x100));
    }
    /* without the undo, a damaged section stops the machine, as before */
    memcpy(bad, blob, n);
    mem = state_section(bad, n, "MEM ", &len);
    if (mem) {
        uint32_t nowhere = 0x3FF00000u;
        memcpy(mem, &nowhere, 4);
        int rc = x360_state_load(X, NULL, bad, n, why, sizeof why);
        CHECK(rc == X360_STATE_ECORRUPT && X->status == X360_ERROR, "no undo: %d, status %d", rc, X->status);
        X->status = X360_RUNNING; /* the load below sets every section again */
    }
    /* the state itself still loads */
    CHECK(x360_state_load(X, NULL, blob, n, why, sizeof why) == X360_STATE_OK &&
              x360_r32(X, scratch + 0x100) == 0xC0FFEE01u,
          "the undamaged state loads: %s", why);
    free(blob);
    free(bad);
}

/* the audio clients' argument cells in states: a state keeps their block; one whose KERN section is version 1 (made
 * before them) loads without it, and the next poll makes a new block holding each client's argument */
static void test_audio_state(void)
{
    char why[256] = "";
    uint32_t cb = x360_kalloc(X, 8), drv = x360_kalloc(X, 4);
    x360_w32(X, cb, 0x82003000u);
    x360_w32(X, cb + 4, 0x9ABC);
    K(X360_xboxkrnl_XAudioRegisterRenderDriverClient, 2, cb, drv);
    uint32_t k = x360_r32(X, drv) & 0xFFFF, cells = X->audio_cells;
    CHECK(k < 8 && cells && x360_r32(X, cells + 4 * k) == 0x9ABC, "audio cell %08X", cells + 4 * k);
    size_t cap = x360_state_size(X), n = 0;
    uint8_t *blob = malloc(cap), *old = malloc(cap);
    CHECK(blob && old && (n = x360_state_save(X, NULL, blob, cap)) > 0, "state saved");
    uint32_t len = 0;
    uint8_t *kern = blob && n ? state_section(blob, n, "KERN", &len) : NULL;
    CHECK(kern && kern[-8] == 2 && len >= 4, "KERN is version 2");
    if (old && kern && len >= 4) {
        x360_w32(X, cells + 4 * k, 0); /* the state's memory has the cell: it comes back with it */
        CHECK(x360_state_load(X, NULL, blob, n, why, sizeof why) == X360_STATE_OK && X->audio_cells == cells &&
                  x360_r32(X, cells + 4 * k) == 0x9ABC, "version 2: the same cells (%s)", why);
        /* the same state as a version 1 KERN: its last field (the cells) left out */
        size_t at = (size_t)(kern - blob), end = at + len;
        memcpy(old, blob, end - 4);
        memcpy(old + end - 4, blob + end, n - end);
        uint32_t l1 = len - 4;
        old[at - 8] = 1;
        for (int i = 0; i < 4; i++)
            old[at - 4 + i] = (uint8_t)(l1 >> (8 * i));
        CHECK(x360_state_load(X, NULL, old, n - 4, why, sizeof why) == X360_STATE_OK && X->audio_cells == 0,
              "version 1 KERN loads without the cells (%s)", why);
        X->time_skip += 60000;
        x360_audio_poll(X);
        x360_audio_client *a = &X->audio[k];
        uint32_t r3 = a->thread >= 0 ? (uint32_t)X->threads[a->thread].cpu.gpr[3] : 0;
        CHECK(X->audio_cells && r3 == X->audio_cells + 4 * k && x360_r32(X, r3) == 0x9ABC,
              "a new cell at the next poll: r3 %08X holds %08X", r3, r3 ? x360_r32(X, r3) : 0);
    }
    K(X360_xboxkrnl_XAudioUnregisterRenderDriverClient, 1, x360_r32(X, drv));
    free(blob);
    free(old);
}

int main(int argc, char **argv)
{
    char why[256] = "";
    X = x360_create_bare(why, sizeof why);
    if (!X) {
        printf("cannot create the machine: %s\n", why);
        return 1;
    }
    X->dbg = stdout;
    X->log = stdout;
    x360_install_fault_handler(X); /* a guest fault prints its address instead of a bare SIGSEGV */
    scratch = x360_heap_alloc(X, &X->heap_v4k, 0x10000, 0);
    stack_top = scratch + 0x8000;
    if (argc == 2 && !strcmp(argv[1], "--list-hle")) {
        /* the real dispatch: compared with python3 -m x360_tools.hle_coverage --implemented (x360_hle_table) */
        static const struct {
            int lib;
            unsigned ordinal;
            const char *lib_name, *name, *kind;
        } exports[] = {
#define X360_LIB_NAME_xboxkrnl X360_LIB_XBOXKRNL
#define X360_LIB_NAME_xam X360_LIB_XAM
#define X360_EXPORT(lib, ord, name, kind) {X360_LIB_NAME_##lib, ord, #lib, #name, #kind},
#include "x360_exports.inc"
#undef X360_EXPORT
        };
        for (size_t k = 0; k < sizeof exports / sizeof exports[0]; k++) {
            int fn = !strcmp(exports[k].kind, "FN");
            if (fn ? x360_hle_lookup(X, exports[k].lib, exports[k].ordinal) != NULL
                   : X->vars[exports[k].lib][exports[k].ordinal] != 0)
                printf("%s %s %s\n", exports[k].lib_name, exports[k].name, fn ? "function" : "variable");
        }
        x360_destroy(X);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--render")) {
        int rc = render_test(argv[2]);
        x360_destroy(X);
        if (rc == 77)
            return 77;
        printf("%s (%d failure%s)\n", failures ? "FAILED" : "renderer through the machine: all checks passed",
               failures, failures == 1 ? "" : "s");
        return failures || rc ? 1 : 0;
    }
    test_formatters();
    test_memory();
    test_host_page();
    test_objects();
    test_threads_and_time();
    test_system();
    test_gpu();
    test_damaged_states();
    test_strict_imports();
    test_input_and_cheats();
    test_audio_out();
    test_xma();
    test_xma_loop();
    test_crypt();
    char dir[] = "/tmp/x360_hle_XXXXXX";
    if (mkdtemp(dir)) {
        test_files(dir);
        test_more(dir);
        rmdir(dir);
        test_audio_state();
    } else {
        CHECK(0, "mkdtemp");
    }
    x360_destroy(X);
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "all HLE checks passed", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
