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
 *   Host waits      (XBA-251) threads that wait inside lifted code with no
 *                   kernel call: one spinning in a marked poll loop on a guest
 *                   word (xbox_GuestPollWait returning at once), one kicking
 *                   the PFB flush and blocked until the NV2A ack thread clears
 *                   it (xbox_GuestPollWait -> xbox_Nv2aKickWait), one halting
 *                   (xbox_Halt). Each loop's back edge checks the guest CPU's
 *                   preempt flag as lifted code with --back-edge-yield does
 *                   (xbox_GuestCpuPreempt, a park point too). Freezes reach
 *                   them, the threads show their site (kind, guest VA), the
 *                   summary line names one, none runs while frozen, and they
 *                   run again after the thaw. The hooks themselves on this
 *                   thread: nesting counts once, a host wait inside a kernel
 *                   call is not a park point of its own.
 *   Frozen waits    (WP-1, KOTOR's pad) a guest thread in a 50 ms timed wait,
 *                   frozen 80 ms from 10 ms in: set 5 ms after the thaw it
 *                   succeeds; never set it times out after ~50 ms of unfrozen
 *                   time; its record does not change while frozen (a capture
 *                   needs one attempt); the same for KeWaitForMultipleObjects
 *                   and KeDelayExecutionThread; a freeze that lets waits expire
 *                   (a same-run load) gets the timeout during the freeze.
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

enum { K_RENDER, K_AUDIO, K_LOADER, K_SPIN, K_TERM, K_POLL, K_KICK, K_HALT, K_COUNT };
static const char *const KNAME[K_COUNT] = { "render", "audio", "loader", "spin", "term", "poll", "kick", "halt" };
#define RET_VA(kind) (0x00011000u + 0x10u * (uint32_t)(kind))
/* The host-wait sites (XBA-251): the guest VA lifted code passes. */
#define POLL_VA 0x0001A100u
#define KICK_VA 0x0001A200u
#define HALT_VA 0x0001A300u
#define PFB_FLUSH 0x00100410u
#define FLUSH_BIT 0x00010000u

static int failures;
static uint32_t g_thunks, g_stack, g_handles;
static volatile int g_stop, g_spin_stop, g_hw_stop;
static uint32_t g_pollword;                 /* the guest word the poll thread waits on */
static volatile uint64_t g_kicks, g_halts;
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

/* RECOMP_BACK_EDGE (recomp_types.h, lifted with --back-edge-yield): hand the guest CPU over when a
 * waiter asked. Without it a thread spinning in a poll loop would keep the CPU from the others under the
 * guest CPU lock. */
#define RECOMP_BACK_EDGE_TEST(va) \
    do { if (__atomic_load_n(&g_xbox_guest_cpu_preempt, __ATOMIC_RELAXED)) xbox_GuestCpuPreempt(va); } while (0)

void xbox_Halt(uint32_t va);

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
    case K_POLL:    /* `test [w], 1; je $` with RECOMP_POLL_WAIT on the back edge */
        while (!g_hw_stop) {
            while (!(G32(g_pollword) & 1u) && !g_hw_stop) {
                __atomic_add_fetch(&g_progress, 1, __ATOMIC_RELAXED);
                xbox_GuestPollWait(&G32(g_pollword), 1u, 0, POLL_VA);
                RECOMP_BACK_EDGE_TEST(POLL_VA);
            }
            guest_sleep(kind, 2);
        }
        break;
    case K_KICK: {  /* D3D's KickOff: set the flush bit, poll until the ack thread clears it */
        volatile uint32_t *kick = xbox_Nv2aRegPtr(PFB_FLUSH);
        while (!g_hw_stop && kick) {
            *kick |= FLUSH_BIT;
            while (*kick & FLUSH_BIT) {
                __atomic_add_fetch(&g_progress, 1, __ATOMIC_RELAXED);
                xbox_GuestPollWait(kick, FLUSH_BIT, 1, KICK_VA);
            }
            __atomic_add_fetch(&g_kicks, 1, __ATOMIC_RELAXED);
            RECOMP_BACK_EDGE_TEST(KICK_VA);
        }
        break;
    }
    case K_HALT:    /* hlt; jmp $-1 */
        while (!g_hw_stop) {
            __atomic_add_fetch(&g_progress, 1, __ATOMIC_RELAXED);
            xbox_Halt(HALT_VA);
            __atomic_add_fetch(&g_halts, 1, __ATOMIC_RELAXED);
        }
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

/* Liveness after a thaw: the counter moves within `ms`. A fixed short window failed on a loaded host (qemu-user
 * under a parallel ctest): six threads take turns on the guest CPU, and a woken thread can wait tens of ms for a
 * core. A thread that stays held never moves, so a bound of seconds still catches it. */
static int moves_within(volatile uint64_t *c, uint64_t c0, int ms)
{
    int t;
    for (t = 0; t < ms; t++) {
        if (__atomic_load_n(c, __ATOMIC_RELAXED) != c0)
            return 1;
        usleep(1000);
    }
    return 0;
}

static int progress_within(unsigned long p0, int ms)
{
    int t;
    for (t = 0; t < ms; t++) {
        if (__atomic_load_n(&g_progress, __ATOMIC_RELAXED) != p0)
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
        check(progress_within(p0, 2000), "after the thaw they run again", NULL);
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
    /* Running guest code before the freeze: a thread registers (its slot taken) before it counts as busy, and a
     * freeze reached in between holds it at its start, legitimately, before it ever spins. */
    for (k = 0; k < 2000; k++) {
        int r, spinning = 0;
        xbox_ParkStatsGet(&st, 0);
        for (r = 0; r < st.threads; r++)
            if (st.t[r].state == XBOX_PARK_RUNNING && st.t[r].kernel_calls == 0)
                spinning = 1;
        if (spinning)
            break;
        usleep(1000);
    }
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
    check(progress_within(p0, 2000), "the others ran on", NULL);
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

static int site_ok(const XboxParkThread *t, uint32_t *seen)
{
    if (t->site == XBOX_PARK_SITE_KERNEL)
        return t->ordinal == ORD_DELAY && kind_of_ret(t->ret_va) >= 0;
    if (t->ordinal != 0)
        return 0;
    *seen |= 1u << t->site;
    switch (t->site) {
    case XBOX_PARK_SITE_POLL:    return t->ret_va == POLL_VA;
    case XBOX_PARK_SITE_KICK:    return t->ret_va == KICK_VA;
    case XBOX_PARK_SITE_HALT:    return t->ret_va == HALT_VA;
    case XBOX_PARK_SITE_PREEMPT: return t->ret_va == POLL_VA || t->ret_va == KICK_VA;
    default:                     return 0;
    }
}

static void phase_host_waits_hooks(void)
{
    XboxParkStats st;

    printf("Host waits inside lifted code: the hooks (this thread)\n");
    xbox_ParkThreadStart();
    xbox_ParkHostWaitEnter(XBOX_PARK_SITE_KICK, KICK_VA);
    xbox_ParkStatsGet(&st, 0);
    check(xbox_ParkThisThreadParked() && st.threads == 1 && st.waiting == 1 && st.host_waits == 1
          && st.t[0].site == XBOX_PARK_SITE_KICK && st.t[0].ordinal == 0 && st.t[0].ret_va == KICK_VA
          && st.t[0].kernel_calls == 0, "in a kick wait: parked, waiting, at its site", NULL);
    xbox_ParkHostWaitEnter(XBOX_PARK_SITE_KICK, 0);   /* xbox_Nv2aKickWait inside xbox_GuestPollWait */
    xbox_ParkStatsGet(&st, 0);
    check(xbox_ParkThisThreadParked() && st.t[0].ret_va == KICK_VA, "a nested host wait counts once", NULL);
    xbox_ParkHostWaitLeave();
    check(xbox_ParkThisThreadParked(), "and leaving it leaves the outer one parked", NULL);
    xbox_ParkHostWaitLeave();
    check(!xbox_ParkThisThreadParked(), "out of the wait: running", NULL);
    xbox_ParkHostWaitEnter(XBOX_PARK_SITE_POLL, POLL_VA);
    xbox_ParkStatsGet(&st, 0);
    check(xbox_ParkThisThreadParked() && st.waiting == 0 && st.t[0].state == XBOX_PARK_KERNEL
          && st.t[0].site == XBOX_PARK_SITE_POLL, "a poll loop's turn: parked, not waiting", NULL);
    xbox_ParkHostWaitLeave();
    xbox_ParkKernelEnter(ORD_DELAY, 0x00012340u);
    xbox_ParkHostWaitEnter(XBOX_PARK_SITE_HALT, HALT_VA);
    xbox_ParkStatsGet(&st, 0);
    check(xbox_ParkThisThreadParked() && st.t[0].site == XBOX_PARK_SITE_KERNEL && st.t[0].ret_va == 0x00012340u,
          "a host wait inside a kernel call: the call stays the park point", NULL);
    xbox_ParkHostWaitLeave();
    xbox_ParkKernelLeave();
    xbox_ParkStatsGet(&st, 0);
    check(!xbox_ParkThisThreadParked() && st.t[0].site == XBOX_PARK_SITE_KERNEL && st.t[0].kernel_calls == 1,
          "out of both: running, one kernel call counted", NULL);
    xbox_ParkThreadEnd();
}

static void phase_host_waits(void)
{
    XboxParkStats st;
    int i, ok = 1, states_ok = 1, still = 1, sites = 1, named = 0;
    uint32_t seen = 0;
    uint64_t worst = 0, k0, h0;
    char d[256] = "", line[512] = "";

    printf("Host waits inside lifted code (poll loop, NV2A kick wait, hlt), 20 freezes\n");
    if (!xbox_Nv2aRegPtr(PFB_FLUSH)) {
        check(0, "NV2A register file mapped", NULL);
        return;
    }
    G32(g_pollword) = 0;
    spawn(5, K_POLL);
    spawn(6, K_KICK);
    spawn(7, K_HALT);
    check(wait_threads(6, 2000), "three more threads start (six)", NULL);
    usleep(20000);
    for (i = 0; i < 20; i++) {
        uint64_t t0;
        unsigned long p0;
        int k;

        usleep(5000 + 1700 * (unsigned)i);
        t0 = now_ns();
        if (!xbox_ParkFreeze(200, &st)) {
            int r;
            for (r = 0; r < st.threads; r++)
                if (st.t[r].state == XBOX_PARK_RUNNING)
                    snprintf(d, sizeof d, "thread %lu running, last site %u at 0x%08X", st.t[r].tid,
                             st.t[r].site, st.t[r].ret_va);
            ok = 0;
            continue;
        }
        t0 = now_ns() - t0;
        if (t0 > worst) worst = t0;
        for (k = 0; k < st.threads; k++) {
            if (st.t[k].state == XBOX_PARK_RUNNING || st.t[k].state == XBOX_PARK_FREE)
                states_ok = 0;
            if (!site_ok(&st.t[k], &seen))
                sites = 0;
        }
        p0 = __atomic_load_n(&g_progress, __ATOMIC_RELAXED);
        k0 = __atomic_load_n(&g_kicks, __ATOMIC_RELAXED);
        h0 = __atomic_load_n(&g_halts, __ATOMIC_RELAXED);
        usleep(10000);
        if (__atomic_load_n(&g_progress, __ATOMIC_RELAXED) != p0
            || __atomic_load_n(&g_kicks, __ATOMIC_RELAXED) != k0
            || __atomic_load_n(&g_halts, __ATOMIC_RELAXED) != h0)
            still = 0;
        if (st.host_waits > 0 && !named) {
            FILE *f = tmpfile();
            if (f) {
                xbox_ParkReport(f);
                rewind(f);
                if (!fgets(line, sizeof line, f)) line[0] = 0;
                fclose(f);
            }
            named = strstr(line, " at a host wait (") != NULL;
        }
        if (i == 0)
            for (k = 0; k < st.threads; k++)
                printf("  thread %lu: %s %s %u at 0x%08X\n", st.t[k].tid,
                       st.t[k].state == XBOX_PARK_WAITING ? "waiting" :
                       st.t[k].state == XBOX_PARK_HELD ? "held" : "in",
                       xbox_ParkSiteName(st.t[k].site), st.t[k].ordinal, st.t[k].ret_va);
        xbox_ParkThaw();
    }
    printf("  freeze reached in %.2f ms at worst; sites seen: poll %s, kick %s, hlt %s, preempt %s\n",
           worst / 1e6, seen & (1u << XBOX_PARK_SITE_POLL) ? "yes" : "no",
           seen & (1u << XBOX_PARK_SITE_KICK) ? "yes" : "no", seen & (1u << XBOX_PARK_SITE_HALT) ? "yes" : "no",
           seen & (1u << XBOX_PARK_SITE_PREEMPT) ? "yes" : "no");
    printf("%s", line);
    check(ok, "every freeze reaches all six threads within 200 ms", ok ? NULL : d);
    check(states_ok, "frozen: none running", NULL);
    check(sites, "each shows its site: kernel call or host wait kind and guest VA", NULL);
    check((seen & (1u << XBOX_PARK_SITE_POLL | 1u << XBOX_PARK_SITE_PREEMPT))
          && (seen & (1u << XBOX_PARK_SITE_KICK | 1u << XBOX_PARK_SITE_PREEMPT))
          && (seen & (1u << XBOX_PARK_SITE_HALT)), "the poll, kick and hlt threads are held at their waits", NULL);
    check(still, "no guest code runs while frozen (no poll turn, kick or hlt; 10 ms each)", NULL);
    check(named, "the [KERNEL] park line names a host wait", line);
    k0 = __atomic_load_n(&g_kicks, __ATOMIC_RELAXED);
    h0 = __atomic_load_n(&g_halts, __ATOMIC_RELAXED);
    {
        int kicked = moves_within(&g_kicks, k0, 2000), halted = moves_within(&g_halts, h0, 2000);
        snprintf(d, sizeof d, "%llu kicks, %llu hlts", (unsigned long long)(g_kicks - k0),
                 (unsigned long long)(g_halts - h0));
        check(kicked && halted, "after the thaw they kick and halt again", d);
    }
    G32(g_pollword) = 1;           /* the poll loop's word changes: it leaves the loop */
    g_hw_stop = 1;
    check(wait_threads(3, 3000), "they end", NULL);
}

/* ---- Frozen waits (WP-1) ---- */
enum { FW_SINGLE, FW_MULTI, FW_DELAY };
typedef struct {
    int kind;
    HANDLE ev;
    uint32_t ms;
    volatile NTSTATUS status;
    volatile uint64_t ns;
    volatile int done;
} FwArg;

static void *fw_thread(void *p)
{
    FwArg *a = (FwArg *)p;
    LARGE_INTEGER to;
    uint64_t t0;
    NTSTATUS r;

    to.QuadPart = -(LONGLONG)a->ms * 10000;
    xbox_ParkThreadStart();
    xbox_ParkKernelEnter(a->kind == FW_DELAY ? ORD_DELAY : a->kind == FW_MULTI ? 158u : 159u, 0x00012400u);
    t0 = now_ns();
    if (a->kind == FW_SINGLE)
        r = xbox_KeWaitForSingleObject(a->ev, 0, 0, FALSE, &to);
    else if (a->kind == FW_MULTI)
        r = xbox_KeWaitForMultipleObjects(1, (PVOID *)&a->ev, 1 /* WaitAny */, 0, 0, FALSE, &to, NULL);
    else
        r = xbox_KeDelayExecutionThread(0, FALSE, &to);
    a->ns = now_ns() - t0;
    a->status = r;
    xbox_ParkKernelLeave();      /* held here while a freeze is in force */
    xbox_ParkThreadEnd();
    a->done = 1;
    return NULL;
}

/* One run: the wait starts, a freeze at 10 ms for freeze_ms, thaw, the event set set_after_ms later (-1: never).
 * let_expire: the freezer lets timed waits expire (a load). Returns the frozen ns the park layer counted. */
static uint64_t fw_run(FwArg *a, int kind, int freeze_ms, int set_after_ms, int let_expire, int *unchanged,
                       int *returned_frozen)
{
    pthread_t th;
    XboxParkRecord r0[XBOX_PARK_MAX_THREADS], r1[XBOX_PARK_MAX_THREADS];
    uint64_t f0, f1;
    int n0 = 0, k;

    memset(a, 0, sizeof *a);
    a->kind = kind;
    a->ms = 50;
    a->ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    pthread_create(&th, NULL, fw_thread, a);
    /* In its wait before the freeze: a fixed 10 ms was not enough for the thread to start on a loaded host, and
     * the freeze then held it at its start, so it never waited while frozen. */
    {
        XboxParkStats st;
        int t;
        for (t = 0; t < 2000; t++) {
            xbox_ParkStatsGet(&st, 0);
            if (st.threads == 1 && st.waiting == 1)
                break;
            usleep(1000);
        }
    }
    usleep(10000);
    f0 = xbox_ParkFrozenNs();
    if (freeze_ms > 0) {
        if (!xbox_ParkFreeze(200, NULL)) {
            printf("  freeze failed\n");
            failures++;
        }
        if (let_expire)
            xbox_ParkFreezeLetWaitsExpire();
        n0 = xbox_ParkRecords(r0, XBOX_PARK_MAX_THREADS);
        usleep((useconds_t)freeze_ms * 1000u);
        if (unchanged)
            *unchanged = n0 == 1 && !xbox_ParkRecordsChanged(r0, n0);
        if (returned_frozen) {
            int m = xbox_ParkRecords(r1, XBOX_PARK_MAX_THREADS);
            *returned_frozen = 0;
            for (k = 0; k < m; k++)
                if (r1[k].returned)
                    *returned_frozen = 1;
        }
        xbox_ParkThaw();
    }
    f1 = xbox_ParkFrozenNs();
    if (set_after_ms >= 0) {
        usleep((useconds_t)set_after_ms * 1000u);
        SetEvent(a->ev);
    }
    pthread_join(th, NULL);
    CloseHandle(a->ev);
    return f1 - f0;
}

static void phase_frozen_waits(void)
{
    FwArg a;
    uint64_t fz, t0, t1;
    int unchanged = 0, ret = 0;
    char d[200];

    printf("Frozen waits (WP-1: frozen time does not count against a guest timeout)\n");
    t0 = xbox_ParkFrozenNs();
    fz = fw_run(&a, FW_SINGLE, 80, 5, 0, &unchanged, NULL);
    t1 = xbox_ParkFrozenNs();
    snprintf(d, sizeof d, "status 0x%08X after %.1f ms, %.1f ms frozen", (unsigned)a.status, a.ns / 1e6, fz / 1e6);
    check(a.status == 0, "50 ms wait, frozen 80 ms from 10 ms, set 5 ms after the thaw: success", d);
    check(fz >= 79000000ull && t1 - t0 >= fz, "the park layer counted the frozen time (monotonic)", d);
    check(unchanged, "its record did not change while frozen (one capture attempt)", NULL);

    fz = fw_run(&a, FW_SINGLE, 80, -1, 0, NULL, NULL);
    snprintf(d, sizeof d, "status 0x%08X after %.1f ms, %.1f ms frozen", (unsigned)a.status, a.ns / 1e6, fz / 1e6);
    check(a.status == STATUS_TIMEOUT && a.ns >= fz + 45000000ull && a.ns <= fz + 50000000ull + 60000000ull,
          "never set: STATUS_TIMEOUT after ~50 ms of unfrozen time", d);

    fz = fw_run(&a, FW_MULTI, 80, 5, 0, NULL, NULL);
    snprintf(d, sizeof d, "status 0x%08X after %.1f ms", (unsigned)a.status, a.ns / 1e6);
    check(a.status == 0, "KeWaitForMultipleObjects: the same, success", d);

    fz = fw_run(&a, FW_DELAY, 80, -1, 0, NULL, NULL);
    snprintf(d, sizeof d, "status 0x%08X after %.1f ms, %.1f ms frozen", (unsigned)a.status, a.ns / 1e6, fz / 1e6);
    check(a.status == 0 && a.ns >= fz + 45000000ull, "KeDelayExecutionThread 50 ms: ~50 ms of unfrozen time", d);

    fz = fw_run(&a, FW_SINGLE, 80, -1, 1, NULL, &ret);
    snprintf(d, sizeof d, "status 0x%08X after %.1f ms", (unsigned)a.status, a.ns / 1e6);
    check(ret && a.status == STATUS_TIMEOUT && a.ns < 75000000ull,
          "a freeze that lets waits expire (a load): the timeout comes during it", d);

    fz = fw_run(&a, FW_SINGLE, 0, -1, 0, NULL, NULL);
    snprintf(d, sizeof d, "status 0x%08X after %.1f ms", (unsigned)a.status, a.ns / 1e6);
    check(a.status == STATUS_TIMEOUT && a.ns >= 45000000ull && a.ns < 90000000ull,
          "no freeze: STATUS_TIMEOUT at ~50 ms, not extended", d);
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
    g_pollword = xbox_HeapAlloc(0x10, 16);
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
    phase_host_waits_hooks();
    phase_frozen_waits();
    spawn(0, K_RENDER);
    spawn(1, K_AUDIO);
    spawn(2, K_LOADER);
    check(wait_threads(3, 2000), "three workers start", NULL);
    phase_statistics();
    phase_freeze();
    phase_host_waits();
    phase_never_parks();
    phase_ends();

    snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmpl);
    (void)system(cmd);
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
