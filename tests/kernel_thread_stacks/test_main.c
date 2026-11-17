/*
 * Worker thread stacks (XBA-104), through the path a title takes:
 * PsCreateSystemThreadEx (255) by ordinal, against a real guest window.
 *
 *   64 workers alive at once  each on its own host thread and 512 KB stack of
 *                             guest heap. The pool used to stop at 8, and the
 *                             9th worker ran inline on its creator: a worker
 *                             that waits for its creator never gave it back
 *                             (Fable's 12th loader worker).
 *   Guest heap exhausted      the next create fails with
 *                             STATUS_INSUFFICIENT_RESOURCES and a zero
 *                             handle; the routine does not run.
 *   Stacks come back          once the workers end, a new one starts.
 *   RECOMP_THREAD_STACKS=8    (second ctest entry) the cap refuses the 9th.
 *
 * Every worker waits for g_go, which this thread sets only after the
 * creates: a worker run inline on its creator says so and returns instead of
 * blocking the run.
 */
#include "kernel.h"
#include "xbox_memory_layout.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef void (*recomp_func_t)(void);
static void worker(void);
#define WORKER_VA 0x00010000u
recomp_func_t recomp_lookup(uint32_t va) { return va == WORKER_VA ? worker : NULL; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }

extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
extern ptrdiff_t g_xbox_mem_offset;

#define G32(va) (*(volatile uint32_t *)((uintptr_t)(va) + g_xbox_mem_offset))

enum { ORD_NTCLOSE = 187, ORD_NTWAIT = 233, ORD_PSCREATE = 255 };
static const uint32_t ORDS[] = { ORD_NTCLOSE, ORD_NTWAIT, ORD_PSCREATE };
#define NORDS (sizeof ORDS / sizeof ORDS[0])
#define MAX_WORKERS 2048
#define STATUS_INSUFFICIENT_RESOURCES 0xC000009Au

static int failures;
static uint32_t g_thunks, g_stack, g_handles;
static pthread_t g_main;
static volatile int g_go, g_started, g_inline;
static volatile uint32_t g_tibs[64];

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-62s %s", what, ok ? "PASS" : "FAIL");
    if (!ok && detail) printf(" -- %s", detail);
    putchar('\n');
    if (!ok) failures++;
}

/* stdcall: return address, then the arguments. */
static uint32_t call(uint32_t ordinal, int argc, const uint32_t *argv)
{
    unsigned i;
    recomp_func_t fn = NULL;

    for (i = 0; i < NORDS; i++)
        if (ORDS[i] == ordinal)
            fn = recomp_lookup_kernel(G32(g_thunks + 4 * i));
    if (!fn) {
        printf("  ordinal %u did not resolve\n", ordinal);
        failures++;
        return 0xFFFFFFFFu;
    }
    G32(g_stack) = 0xBEEF0001u;
    for (i = 0; i < (unsigned)argc; i++)
        G32(g_stack + 4 + 4 * i) = argv[i];
    g_esp = g_stack;
    g_eax = 0xDEADBEEFu;
    fn();
    return g_eax;
}
#define CALL(ord, ...) call((ord), \
    (int)(sizeof((uint32_t[]){__VA_ARGS__}) / 4), (uint32_t[]){__VA_ARGS__})

/* Runs on the worker's own thread and stack, or inline on ours (the bug). */
static void worker(void)
{
    if (pthread_equal(pthread_self(), g_main)) {
        __atomic_add_fetch(&g_inline, 1, __ATOMIC_SEQ_CST);
        return;
    }
    {
        int k = __atomic_fetch_add(&g_started, 1, __ATOMIC_SEQ_CST);
        if (k < 64)
            g_tibs[k] = g_fs_base;              /* its own TIB (KRN-8) */
    }
    while (!__atomic_load_n(&g_go, __ATOMIC_SEQ_CST))
        usleep(1000);
}

/* Status of PsCreateSystemThreadEx; the handle lands in slot i. */
static uint32_t spawn(int i)
{
    uint32_t hva = g_handles + 4u * (uint32_t)i;

    G32(hva) = 0xCCCCCCCCu;
    /* ThreadHandle, ExtraSize, KStack, Tls, ThreadId, ctx1, ctx2,
     * CreateSuspended, DebugStack, StartRoutine */
    return CALL(ORD_PSCREATE, hva, 0, 0, 0, 0, (uint32_t)i, 0, 0, 0, WORKER_VA);
}

static int wait_started(int n)
{
    int t;
    for (t = 0; t < 10000 && __atomic_load_n(&g_started, __ATOMIC_SEQ_CST) < n; t++)
        usleep(1000);
    return __atomic_load_n(&g_started, __ATOMIC_SEQ_CST);
}

/* Let workers [0, n) end; wait for each and close its handle. */
static int release(int n)
{
    int i, ok = 1;

    __atomic_store_n(&g_go, 1, __ATOMIC_SEQ_CST);
    for (i = 0; i < n; i++) {
        uint32_t h = G32(g_handles + 4u * (uint32_t)i);
        if (CALL(ORD_NTWAIT, h, 0, 0) != 0) ok = 0;
        CALL(ORD_NTCLOSE, h);
    }
    __atomic_store_n(&g_go, 0, __ATOMIC_SEQ_CST);
    return ok;
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

int main(int argc, char **argv)
{
    char tmpl[] = "/tmp/kthreadstacksXXXXXX", cmd[300], d[160];
    const char *capenv = getenv("RECOMP_THREAD_STACKS");
    int cap = capenv ? atoi(capenv) : 0, want, i, n, bad;
    uint32_t st = 0;
    size_t len = 0;
    unsigned char *xbe;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (!mkdtemp(tmpl)) return 2;
    setenv("XDG_DATA_HOME", tmpl, 1);
    g_main = pthread_self();

    xbe = load(argc > 1 ? argv[1] : "tools/conformance/test.xbe", &len);
    if (!xbe || !xbox_MemoryLayoutInit(xbe, len)) {
        printf("cannot map guest memory (run tools/conformance/mkxbe.py)\n");
        return 2;
    }
    g_thunks  = xbox_HeapAlloc(0x1000, 4096);
    g_stack   = xbox_HeapAlloc(0x1000, 4096);
    g_handles = xbox_HeapAlloc(4 * MAX_WORKERS, 4096);
    for (i = 0; i < (int)NORDS; i++)
        G32(g_thunks + 4 * i) = 0x80000000u | ORDS[i];
    G32(g_thunks + 4 * NORDS) = 0;
    xbox_kernel_set_thunk_address(g_thunks, NORDS + 1);
    xbox_kernel_bridge_init();
    xbox_SetThreadMode(XBOX_THREAD_MODE_SPAWN);

    want = cap > 0 && cap < 64 ? cap : 64;
    printf("%d guest workers alive at once (%s)\n", want,
           cap > 0 ? "RECOMP_THREAD_STACKS cap" : "no cap");
    for (i = 0, bad = 0; i < want; i++) {
        st = spawn(i);
        if (st != 0 || !G32(g_handles + 4u * (uint32_t)i)) bad++;
    }
    snprintf(d, sizeof d, "%d create(s) failed", bad);
    check(bad == 0, "every create succeeds with a handle", d);
    n = wait_started(want);
    snprintf(d, sizeof d, "%d of %d started, %d inline", n, want, g_inline);
    check(n == want && g_inline == 0,
          "each runs on its own host thread, all alive together", d);
    snprintf(d, sizeof d, "high-water mark %d", xbox_ThreadStacksHighWater());
    check(xbox_ThreadStacksHighWater() >= want,
          "xbox_ThreadStacksHighWater counts them", d);

    /* Past the end: the cap, or the guest heap. */
    n = want;
    if (cap > 0) {
        printf("Past the cap\n");
        st = spawn(n);
    } else {
        printf("Until the guest heap runs out\n");
        while (n < MAX_WORKERS && (st = spawn(n)) == 0)
            n++;
        snprintf(d, sizeof d, "%d workers before the first refusal", n);
        check(n > 64 && n < MAX_WORKERS, "the pool grows past 64", d);
        snprintf(d, sizeof d, "%d of %d started", wait_started(n), n);
        check(g_started == n, "every accepted worker started", d);
    }
    snprintf(d, sizeof d, "status 0x%08X handle 0x%08X", st,
             G32(g_handles + 4u * (uint32_t)n));
    check(st == STATUS_INSUFFICIENT_RESOURCES && G32(g_handles + 4u * (uint32_t)n) == 0,
          "the next create fails: STATUS_INSUFFICIENT_RESOURCES, no handle", d);
    snprintf(d, sizeof d, "%d run inline", g_inline);
    check(g_inline == 0, "the refused worker did not run inline", d);

    printf("Stacks come back\n");
    check(release(n), "every worker ends and can be waited on", NULL);
    g_started = 0;
    st = spawn(0);
    snprintf(d, sizeof d, "status 0x%08X", st);
    check(st == 0 && wait_started(1) == 1 && g_inline == 0,
          "a new worker starts once the others have ended", d);
    check(release(1), "and ends", NULL);

    /* KRN-8: a worker's TIB + TLS + KTHREAD block goes back with its stack.
     * It never did, so a title starting a thread per asset leaked one block
     * per thread until creates failed. */
    printf("TIBs come back\n");
    g_started = 0;
    memset((void *)g_tibs, 0, sizeof g_tibs);
    for (i = 0, bad = 0; i < 8; i++)
        bad += spawn(i) != 0;
    n = wait_started(8);
    check(bad == 0 && n == 8, "8 workers start", NULL);
    for (i = 0, bad = 0; i < 8; i++)
        bad += !g_tibs[i] || !xbox_HeapBlockSize(g_tibs[i]);
    snprintf(d, sizeof d, "%d without a live TIB block", bad);
    check(bad == 0, "each runs on a TIB of its own from the guest heap", d);
    check(release(8), "they end", NULL);
    for (i = 0, bad = 0; i < 8; i++)
        bad += xbox_HeapBlockSize(g_tibs[i]) != 0;
    snprintf(d, sizeof d, "%d of 8 still allocated", bad);
    check(bad == 0, "their TIB blocks are given back", d);

    snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmpl);
    (void)system(cmd);
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
