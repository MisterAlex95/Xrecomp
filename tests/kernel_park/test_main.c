/*
 * The safe-point park detector (XBA-244), through the path a title takes:
 * PsCreateSystemThreadEx (255) and KeDelayExecutionThread (99) by ordinal,
 * against a real guest window (the synthetic XBE).
 *
 * A game-shaped set of guest workers stands in for a title's threads (our
 * synthetic XBE has no runnable code): a "render" thread that works 4 ms and
 * sleeps 12 ms, an "audio" thread that works 0.3 ms every 5 ms, a "loader"
 * that sleeps 50 ms and then works 8 ms without a kernel call. "Work" is host
 * code spinning, which is what lifted code is to the detector.
 *
 *   Statistics      the share of time all three were parked at once, the
 *                   windows, the longest one (printed: the measurement the
 *                   save-state design quotes).
 *   Freeze          20 freezes at odd moments: each reaches every thread at
 *                   its next kernel call, the threads show the call they are
 *                   in (ordinal, guest return address), and none runs guest
 *                   code until the thaw.
 *   Guest pages     the resident and non-zero pages of the guest window a
 *                   snapshot would carry (printed).
 *   Never parks     a thread spinning without kernel calls defeats the freeze,
 *                   which times out, lets the others go and names it.
 *   Thread ends     by returning and by PsTerminateSystemThread: unregistered.
 *   Nesting         a guest routine a bridge runs inside a kernel call (an
 *                   APC) is not parked, and its own kernel calls are not.
 */
#include "kernel.h"
#include "kernel_park.h"
#include "xbox_memory_layout.h"

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

enum { ORD_DELAY = 99, ORD_NTCLOSE = 187, ORD_PSCREATE = 255, ORD_PSTERM = 258 };
static const uint32_t ORDS[] = { ORD_DELAY, ORD_NTCLOSE, ORD_PSCREATE, ORD_PSTERM };
#define NORDS (sizeof ORDS / sizeof ORDS[0])

enum { K_RENDER, K_AUDIO, K_LOADER, K_SPIN, K_TERM, K_COUNT };
static const char *const KNAME[K_COUNT] = { "render", "audio", "loader", "spin", "term" };
#define RET_VA(kind) (0x00011000u + 0x10u * (uint32_t)(kind))

static int failures;
static uint32_t g_thunks, g_stack, g_handles;
static volatile int g_stop, g_spin_stop;
static volatile unsigned long g_progress;   /* guest "instructions" run */

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-64s %s", what, ok ? "PASS" : "FAIL");
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

/* A stdcall kernel call from guest code on this thread's own guest stack:
 * the return address, then the arguments, below the current esp. */
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
    fn();
    g_esp = sp0;
    return g_eax;
}
#define GCALL(ord, ret, ...) guest_call((ord), (ret), \
    (int)(sizeof((uint32_t[]){__VA_ARGS__}) / 4), (uint32_t[]){__VA_ARGS__})

/* The interval lives below the call frame on the thread's guest stack. */
static void guest_sleep(int kind, uint32_t ms)
{
    uint32_t iv = (g_esp - 256u) & ~7u;
    int64_t rel = -(int64_t)ms * 10000;
    G32(iv) = (uint32_t)rel;
    G32(iv + 4) = (uint32_t)((uint64_t)rel >> 32);
    GCALL(ORD_DELAY, RET_VA(kind), 0 /* KernelMode */, 0 /* not alertable */, iv);
}

/* Lifted code running: no kernel call. */
static void guest_work(uint64_t us)
{
    uint64_t end = now_ns() + us * 1000u;
    while (now_ns() < end)
        __atomic_add_fetch(&g_progress, 1, __ATOMIC_RELAXED);
}

static void worker(void)
{
    int kind = (int)G32(g_esp + 4);   /* ctx1 (return address, ctx1, ctx2) */

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
    case K_SPIN:
        while (!g_spin_stop)
            __atomic_add_fetch(&g_progress, 1, __ATOMIC_RELAXED);
        break;
    case K_TERM:
        guest_sleep(kind, 5);
        GCALL(ORD_PSTERM, RET_VA(kind), 0);
        printf("  PsTerminateSystemThread returned\n");
        failures++;
        break;
    }
}

static uint32_t spawn(int i, int kind)
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
    return g_eax;
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

static void phase_statistics(void)
{
    XboxParkStats st;
    char d[200];

    printf("Statistics (render 4/16 ms, audio 0.3/5 ms, loader 8 ms burst per 58 ms), 2 s\n");
    xbox_ParkStatsGet(&st, 1);
    usleep(2000000);
    xbox_ParkStatsGet(&st, 0);
    printf("  all parked %.1f %% of %.0f ms, %llu windows, longest %.2f ms, mean %.2f ms\n",
           st.window_ns ? 100.0 * (double)st.all_parked_ns / (double)st.window_ns : 0.0,
           st.window_ns / 1e6, (unsigned long long)st.windows, st.longest_ns / 1e6,
           st.windows ? st.all_parked_ns / 1e6 / (double)st.windows : 0.0);
    snprintf(d, sizeof d, "%d threads", st.threads);
    check(st.threads == 3, "three guest threads registered", d);
    snprintf(d, sizeof d, "%llu windows", (unsigned long long)st.windows);
    check(st.windows >= 20 && st.all_parked_ns > 0, "all three are parked at once, often", d);
    check(st.all_parked_ns < st.window_ns, "and not all the time (they do work)", NULL);
    {
        char buf[512] = "";
        FILE *f = tmpfile();
        if (f) {
            xbox_ParkReport(f);
            rewind(f);
            if (!fgets(buf, sizeof buf, f)) buf[0] = 0;
            fclose(f);
        }
        printf("%s", buf);
        check(strstr(buf, "[KERNEL] park: 3 guest threads, all parked ") != NULL,
              "the summary line names them", buf);
    }
}

static void phase_freeze(void)
{
    XboxParkStats st;
    int i, ok = 1, states_ok = 1, still = 1, sites = 1;
    uint64_t worst = 0, total = 0;
    char d[200];

    printf("Freeze, 20 times at odd moments\n");
    for (i = 0; i < 20; i++) {
        uint64_t t0;
        unsigned long p0;
        int k;

        usleep(7000 + 3100 * (unsigned)i);
        t0 = now_ns();
        if (!xbox_ParkFreeze(200, &st)) {
            ok = 0;
            continue;
        }
        t0 = now_ns() - t0;
        total += t0;
        if (t0 > worst) worst = t0;
        for (k = 0; k < st.threads; k++) {
            if (st.t[k].state == XBOX_PARK_RUNNING || st.t[k].state == XBOX_PARK_FREE)
                states_ok = 0;
            if (st.t[k].ordinal != ORD_DELAY || kind_of_ret(st.t[k].ret_va) < 0)
                sites = 0;
        }
        p0 = __atomic_load_n(&g_progress, __ATOMIC_RELAXED);
        usleep(20000);
        if (__atomic_load_n(&g_progress, __ATOMIC_RELAXED) != p0)
            still = 0;
        if (i == 0) {
            uint32_t res = 0, nz = 0, pages = xbox_ParkGuestPages(&res, &nz);
            printf("  guest window %u pages (%u MB): %u resident (%.1f MB), %u not zero (%.1f MB)\n",
                   pages, pages / 256, res, res / 256.0, nz, nz / 256.0);
            check(pages > 0 && nz > 0 && nz <= res && res <= pages,
                  "guest pages counted (resident >= non-zero > 0)", NULL);
            for (k = 0; k < st.threads; k++)
                printf("  thread %lu: %s in ordinal %u from 0x%08X (%s)\n", st.t[k].tid,
                       st.t[k].state == XBOX_PARK_WAITING ? "waiting" :
                       st.t[k].state == XBOX_PARK_HELD ? "held" : "in kernel",
                       st.t[k].ordinal, st.t[k].ret_va,
                       kind_of_ret(st.t[k].ret_va) >= 0 ? KNAME[kind_of_ret(st.t[k].ret_va)] : "?");
        }
        xbox_ParkThaw();
    }
    printf("  freeze reached in %.2f ms on average, %.2f ms at worst\n",
           total / 1e6 / 20.0, worst / 1e6);
    check(ok, "every freeze reaches all threads within 200 ms", NULL);
    check(states_ok, "frozen: every thread waiting, in a kernel call or held", NULL);
    check(sites, "each shows its call: KeDelayExecutionThread and its return address", NULL);
    check(still, "no guest code runs while frozen (20 ms each)", NULL);
    {
        unsigned long p0 = __atomic_load_n(&g_progress, __ATOMIC_RELAXED);
        usleep(50000);
        check(__atomic_load_n(&g_progress, __ATOMIC_RELAXED) != p0, "after the thaw they run again", NULL);
    }
}

static void phase_never_parks(void)
{
    XboxParkStats st;
    int k, running = 0, spin_running = 0, frozen;
    unsigned long p0;
    char d[200];

    printf("A thread that never makes a kernel call\n");
    spawn(3, K_SPIN);
    check(wait_threads(4, 2000), "it registers", NULL);
    frozen = xbox_ParkFreeze(100, &st);
    for (k = 0; k < st.threads; k++)
        if (st.t[k].state == XBOX_PARK_RUNNING) {
            running++;
            if (st.t[k].kernel_calls == 0) spin_running = 1;
        }
    snprintf(d, sizeof d, "freeze %d, %d running", frozen, running);
    check(!frozen && running == 1 && spin_running,
          "the freeze times out and names it (running, no kernel call)", d);
    xbox_ParkStatsGet(&st, 0);
    check(!st.frozen, "the freeze is lifted", NULL);
    p0 = __atomic_load_n(&g_progress, __ATOMIC_RELAXED);
    g_spin_stop = 1;
    check(wait_threads(3, 2000), "it ends and unregisters", NULL);
    usleep(30000);
    check(__atomic_load_n(&g_progress, __ATOMIC_RELAXED) != p0, "the others ran on", NULL);
}

static void phase_ends(void)
{
    printf("Threads end\n");
    spawn(4, K_TERM);
    check(wait_threads(3, 2000), "PsTerminateSystemThread unregisters its thread", NULL);
    g_stop = 1;
    check(wait_threads(0, 3000), "returning routines unregister theirs", NULL);
}

static void phase_nesting(void)
{
    XboxParkStats st;

    printf("Nesting (hooks on this thread)\n");
    xbox_ParkThreadStart();
    check(!xbox_ParkThisThreadParked(), "a started thread runs guest code", NULL);
    xbox_ParkKernelEnter(ORD_DELAY, 0x00012340u);
    xbox_ParkStatsGet(&st, 0);
    check(xbox_ParkThisThreadParked() && st.threads == 1 && st.waiting == 1
          && st.t[0].ret_va == 0x00012340u, "in a blocking kernel call: parked, waiting", NULL);
    xbox_ParkCallbackEnter();
    check(!xbox_ParkThisThreadParked(), "a guest routine run from the call: not parked", NULL);
    xbox_ParkKernelEnter(ORD_NTCLOSE, 0x00012350u);
    check(!xbox_ParkThisThreadParked(), "a kernel call made from that routine: not parked", NULL);
    xbox_ParkKernelLeave();
    xbox_ParkCallbackLeave();
    xbox_ParkStatsGet(&st, 0);
    check(xbox_ParkThisThreadParked() && st.t[0].state == XBOX_PARK_WAITING
          && st.t[0].ordinal == ORD_DELAY, "back in the outer call: parked in it again", NULL);
    xbox_ParkKernelLeave();
    check(!xbox_ParkThisThreadParked(), "out of the call: running", NULL);
    {   /* An SEH resume from inside an APC's kernel call back to guest code
         * skips three Leave hooks (xbox_SehRuntimeRestore). */
        uint32_t snap = xbox_ParkDepthSnapshot();
        xbox_ParkKernelEnter(ORD_DELAY, 0x00012340u);
        xbox_ParkCallbackEnter();
        xbox_ParkKernelEnter(ORD_NTCLOSE, 0x00012350u);
        xbox_ParkDepthRestore(snap);
        xbox_ParkStatsGet(&st, 0);
        check(!xbox_ParkThisThreadParked() && st.parked == 0,
              "a jump over bridge frames puts the depths back: running", NULL);
        xbox_ParkKernelEnter(ORD_DELAY, 0x00012360u);
        xbox_ParkStatsGet(&st, 0);
        check(xbox_ParkThisThreadParked() && st.parked == 1 && st.t[0].ret_va == 0x00012360u,
              "and its next kernel call parks it", NULL);
        xbox_ParkKernelLeave();
    }
    xbox_ParkThreadEnd();
    xbox_ParkStatsGet(&st, 0);
    check(st.threads == 0, "ended: unregistered", NULL);
}

int main(int argc, char **argv)
{
    char tmpl[] = "/tmp/kparkXXXXXX", cmd[300];
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
    for (i = 0; i < NORDS; i++)
        G32(g_thunks + 4 * i) = 0x80000000u | ORDS[i];
    G32(g_thunks + 4 * NORDS) = 0;
    xbox_kernel_set_thunk_address(g_thunks, NORDS + 1);
    xbox_kernel_bridge_init();
    xbox_SetThreadMode(XBOX_THREAD_MODE_SPAWN);
    /* This thread only drives the test: with the guest CPU lock on it must
     * not hold the guest CPU while it sleeps, or no worker runs. */
    xbox_GuestCpuPart();
    printf("guest CPU lock %s\n", xbox_GuestCpuOn() ? "on" : "off");

    phase_nesting();
    spawn(0, K_RENDER);
    spawn(1, K_AUDIO);
    spawn(2, K_LOADER);
    check(wait_threads(3, 2000), "three workers start", NULL);
    phase_statistics();
    phase_freeze();
    phase_never_parks();
    phase_ends();

    snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmpl);
    (void)system(cmd);
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
