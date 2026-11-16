/*
 * kernel_park.c - the safe-point park detector (see kernel_park.h, XBA-244)
 *
 * Per thread: its kernel-call depth and its bridge-callback depth (thread-local, every host thread),
 * and for a registered guest thread the depths at which its routine started. The thread is parked
 * when it is exactly one kernel call deeper than that and in no further callback.
 *
 * Global: s_busy, the registered threads not parked. A thread going busy adds one first and then
 * looks at the freeze flag; the freezer sets the flag first and then waits for s_busy to reach 0
 * (both sequentially consistent). So once the freezer has seen 0 with the flag set, any thread that
 * goes busy afterwards sees the flag, takes its one back and waits: no guest code runs until the
 * thaw.
 */
#include "kernel_park.h"
#include "kernel.h"
#include "xbox_memory_layout.h"
#include "platform/host_time.h"

#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/mman.h>
#include <unistd.h>
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#define ADD32(p, v)    ((int32_t)_InterlockedExchangeAdd((volatile long *)(p), (long)(v)) + (int32_t)(v))
#define LOAD32(p)      ((int32_t)_InterlockedOr((volatile long *)(p), 0))
#define STORE32(p, v)  ((void)_InterlockedExchange((volatile long *)(p), (long)(v)))
#define CAS32(p, o, n) ((int32_t)_InterlockedCompareExchange((volatile long *)(p), (long)(n), (long)(o)) == (int32_t)(o))
#define ADD64(p, v)    ((void)_InterlockedExchangeAdd64((volatile __int64 *)(p), (__int64)(v)))
#define LOAD64(p)      ((uint64_t)_InterlockedOr64((volatile __int64 *)(p), 0))
#define STORE64(p, v)  ((void)_InterlockedExchange64((volatile __int64 *)(p), (__int64)(v)))
#else
#define ADD32(p, v)    __atomic_add_fetch((p), (int32_t)(v), __ATOMIC_SEQ_CST)
#define LOAD32(p)      __atomic_load_n((p), __ATOMIC_SEQ_CST)
#define STORE32(p, v)  __atomic_store_n((p), (int32_t)(v), __ATOMIC_SEQ_CST)
#define CAS32(p, o, n) __extension__ ({ int32_t _o = (o); \
        __atomic_compare_exchange_n((p), &_o, (int32_t)(n), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); })
#define ADD64(p, v)    ((void)__atomic_fetch_add((p), (uint64_t)(v), __ATOMIC_RELAXED))
#define LOAD64(p)      __atomic_load_n((p), __ATOMIC_RELAXED)
#define STORE64(p, v)  __atomic_store_n((p), (uint64_t)(v), __ATOMIC_RELAXED)
#endif

typedef struct {
    volatile int32_t state;          /* XBOX_PARK_*; written by the owner, read by anyone */
    volatile uint32_t ordinal, ret_va;
    volatile int32_t dispatch;
    volatile unsigned long tid;
    volatile uint64_t kernel_calls;
} Slot;

static Slot s_slots[XBOX_PARK_MAX_THREADS];
static volatile int32_t s_threads;      /* registered */
static volatile int32_t s_busy;         /* registered and not parked */
static volatile int32_t s_freeze;       /* a freeze is in force or being reached */
static volatile int32_t s_freezer;      /* one freezer at a time */
static volatile int32_t s_dropped;      /* threads that found no slot */

/* All-parked statistics. */
static volatile uint64_t s_since_ns;    /* when s_busy last reached 0 (0: it is not 0) */
static volatile uint64_t s_all_ns, s_windows, s_longest_ns, s_reset_ns;

static CRITICAL_SECTION s_gate_lock;
static CONDITION_VARIABLE s_gate_cv;
static INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;

static XBOX_THREAD_LOCAL int t_depth;    /* kernel calls this thread is inside */
static XBOX_THREAD_LOCAL int t_cb;       /* bridge-run guest routines it is inside */
static XBOX_THREAD_LOCAL int t_slot = -1;
static XBOX_THREAD_LOCAL int t_base_depth, t_base_cb, t_nest;

static BOOL CALLBACK park_init(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&s_gate_lock);
    InitializeConditionVariable(&s_gate_cv);
    return TRUE;
}

static void park_init_once(void)
{
    InitOnceExecuteOnce(&s_once, park_init, NULL, NULL);
}

int xbox_ParkOrdinalBlocks(uint32_t ordinal)
{
    switch (ordinal) {
    case 99:                            /* KeDelayExecutionThread */
    case 158: case 159:                 /* KeWaitFor{Multiple,Single}Object(s) */
    case 190: case 202:                 /* NtCreateFile, NtOpenFile */
    case 196: case 198: case 200:       /* NtDeviceIoControlFile, NtFlushBuffersFile, NtFsControlFile */
    case 207:                           /* NtQueryDirectoryFile */
    case 219: case 220:                 /* NtReadFile, NtReadFileScatter */
    case 223:                           /* NtRemoveIoCompletion */
    case 230:                           /* NtSignalAndWaitForSingleObjectEx */
    case 231:                           /* NtSuspendThread */
    case 233: case 234: case 235:       /* NtWaitFor{Single,SingleEx,MultipleEx} */
    case 236: case 237:                 /* NtWriteFile, NtWriteFileGather */
        return 1;
    default:
        return 0;
    }
}

static int parked_now(void)
{
    return t_depth == t_base_depth + 1 && t_cb == t_base_cb;
}

/* s_busy reached 0 / left 0: the all-parked window opens / closes. Racy by one transition at worst. */
static void window_open(void)
{
    STORE64(&s_since_ns, xbox_HostNowNs());
}

static void window_close(void)
{
    uint64_t since = LOAD64(&s_since_ns), d;
    if (!since)
        return;
    STORE64(&s_since_ns, 0);
    d = xbox_HostNowNs() - since;
    ADD64(&s_all_ns, d);
    ADD64(&s_windows, 1);
    if (d > LOAD64(&s_longest_ns))
        STORE64(&s_longest_ns, d);
}

static void go_idle(void)
{
    if (ADD32(&s_busy, -1) == 0) {
        window_open();
        if (LOAD32(&s_freeze)) {
            EnterCriticalSection(&s_gate_lock);
            WakeAllConditionVariable(&s_gate_cv);
            LeaveCriticalSection(&s_gate_lock);
        }
    }
}

/* Going busy: wait out a freeze first. The slot shows HELD meanwhile. */
static void go_busy(int busy_state)
{
    Slot *s = &s_slots[t_slot];

    for (;;) {
        if (ADD32(&s_busy, 1) == 1)
            window_close();
        if (!LOAD32(&s_freeze))
            break;
        go_idle();
        STORE32(&s->state, XBOX_PARK_HELD);
        s->dispatch = xbox_IrqlThisThreadBlocksDpcs();
        EnterCriticalSection(&s_gate_lock);
        while (LOAD32(&s_freeze))
            SleepConditionVariableCS(&s_gate_cv, &s_gate_lock, 50);
        LeaveCriticalSection(&s_gate_lock);
    }
    STORE32(&s->state, busy_state);
}

void xbox_ParkThreadStart(void)
{
    int i;

    if (t_slot >= 0) {
        t_nest++;
        return;
    }
    park_init_once();
    for (i = 0; i < XBOX_PARK_MAX_THREADS; i++) {
        if (CAS32(&s_slots[i].state, XBOX_PARK_FREE, XBOX_PARK_HELD)) {
            Slot *s = &s_slots[i];
            s->ordinal = 0;
            s->ret_va = 0;
            s->dispatch = 0;
            s->tid = (unsigned long)GetCurrentThreadId();
            s->kernel_calls = 0;
            t_slot = i;
            t_nest = 0;
            t_base_depth = t_depth;
            t_base_cb = t_cb;
            ADD32(&s_threads, 1);
            go_busy(XBOX_PARK_RUNNING);    /* a thread starting during a freeze waits for the thaw */
            return;
        }
    }
    ADD32(&s_dropped, 1);
}

static void unregister(int was_busy)
{
    Slot *s = &s_slots[t_slot];

    if (was_busy)
        go_idle();
    if (ADD32(&s_threads, -1) == 0)
        STORE64(&s_since_ns, 0);        /* no thread, no all-parked window */
    t_slot = -1;
    STORE32(&s->state, XBOX_PARK_FREE);
    /* A freezer waiting for this thread to park sees it gone instead. */
    if (LOAD32(&s_freeze)) {
        EnterCriticalSection(&s_gate_lock);
        WakeAllConditionVariable(&s_gate_cv);
        LeaveCriticalSection(&s_gate_lock);
    }
}

void xbox_ParkThreadEnd(void)
{
    if (t_slot < 0)
        return;
    if (t_nest > 0) {
        t_nest--;
        return;
    }
    unregister(!parked_now());
}

void xbox_ParkKernelEnter(uint32_t ordinal, uint32_t ret_va)
{
    int was = t_slot >= 0 && parked_now();

    t_depth++;
    if (t_slot < 0 || was || !parked_now())
        return;
    {
        Slot *s = &s_slots[t_slot];
        s->ordinal = ordinal;
        s->ret_va = ret_va;
        s->kernel_calls++;
        STORE32(&s->state, xbox_ParkOrdinalBlocks(ordinal) ? XBOX_PARK_WAITING : XBOX_PARK_KERNEL);
    }
    if (ordinal == 258) {               /* PsTerminateSystemThread: the thread ends in there */
        t_nest = 0;
        unregister(1);
        return;
    }
    go_idle();
}

void xbox_ParkKernelLeave(void)
{
    int was = t_slot >= 0 && parked_now();

    t_depth--;
    if (was && !parked_now())
        go_busy(XBOX_PARK_RUNNING);
}

void xbox_ParkCallbackEnter(void)
{
    int was = t_slot >= 0 && parked_now();

    t_cb++;
    if (was && !parked_now())
        go_busy(XBOX_PARK_RUNNING);
}

void xbox_ParkCallbackLeave(void)
{
    int was = t_slot >= 0 && parked_now();

    t_cb--;
    if (t_slot >= 0 && !was && parked_now()) {
        /* Back in the kernel call the routine was run from: that call's state again. */
        Slot *s = &s_slots[t_slot];
        STORE32(&s->state, xbox_ParkOrdinalBlocks(s->ordinal) ? XBOX_PARK_WAITING : XBOX_PARK_KERNEL);
        go_idle();
    }
}

uint32_t xbox_ParkDepthSnapshot(void)
{
    return (((uint32_t)t_depth & 0xFFu) << 8) | ((uint32_t)t_cb & 0xFFu);
}

void xbox_ParkDepthRestore(uint32_t snap)
{
    int was = t_slot >= 0 && parked_now();

    t_depth = (int)((snap >> 8) & 0xFFu);
    t_cb = (int)(snap & 0xFFu);
    if (t_slot < 0)
        return;
    if (was && !parked_now())
        go_busy(XBOX_PARK_RUNNING);
    else if (!was && parked_now()) {
        Slot *s = &s_slots[t_slot];
        STORE32(&s->state, xbox_ParkOrdinalBlocks(s->ordinal) ? XBOX_PARK_WAITING : XBOX_PARK_KERNEL);
        go_idle();
    }
}

int xbox_ParkThisThreadParked(void)
{
    return t_slot >= 0 && parked_now();
}

void xbox_ParkStatsGet(XboxParkStats *out, int reset)
{
    uint64_t now = xbox_HostNowNs(), since;
    int i, n = 0;

    memset(out, 0, sizeof *out);
    for (i = 0; i < XBOX_PARK_MAX_THREADS; i++) {
        Slot *s = &s_slots[i];
        int st = LOAD32(&s->state);
        if (st == XBOX_PARK_FREE)
            continue;
        out->t[n].state = st;
        out->t[n].ordinal = s->ordinal;
        out->t[n].ret_va = s->ret_va;
        out->t[n].dispatch = s->dispatch;
        out->t[n].tid = s->tid;
        out->t[n].kernel_calls = s->kernel_calls;
        if (st != XBOX_PARK_RUNNING)
            out->parked++;
        if (st == XBOX_PARK_WAITING)
            out->waiting++;
        n++;
    }
    out->threads = n;
    out->frozen = LOAD32(&s_freeze) != 0;
    out->all_parked_ns = LOAD64(&s_all_ns);
    since = LOAD64(&s_since_ns);
    if (since && LOAD32(&s_threads) > 0)
        out->all_parked_ns += now - since;     /* the window open now */
    out->windows = LOAD64(&s_windows);
    out->longest_ns = LOAD64(&s_longest_ns);
    if (!LOAD64(&s_reset_ns))
        STORE64(&s_reset_ns, now);
    out->window_ns = now - LOAD64(&s_reset_ns);
    if (reset) {
        STORE64(&s_all_ns, 0);
        STORE64(&s_windows, 0);
        STORE64(&s_longest_ns, 0);
        STORE64(&s_reset_ns, now);
        if (LOAD64(&s_since_ns))
            STORE64(&s_since_ns, now);
    }
}

int xbox_ParkReport(FILE *out)
{
    /* Deltas from the previous report: the statistics themselves are not reset (a test or a caller of
     * xbox_ParkStatsGet keeps its own window). One reporter at a time (the summary's thread). */
    static uint64_t prev_all, prev_windows, prev_window;
    XboxParkStats st;
    int i, running = 0;
    uint32_t r_ord = 0, r_ret = 0;
    uint64_t all, windows, window;

    xbox_ParkStatsGet(&st, 0);
    all = st.all_parked_ns - (st.all_parked_ns >= prev_all ? prev_all : st.all_parked_ns);
    windows = st.windows - (st.windows >= prev_windows ? prev_windows : st.windows);
    window = st.window_ns - (st.window_ns >= prev_window ? prev_window : st.window_ns);
    prev_all = st.all_parked_ns;
    prev_windows = st.windows;
    prev_window = st.window_ns;
    if (all > window)
        all = window;
    if (st.threads <= 0)
        return 0;
    for (i = 0; i < st.threads; i++)
        if (st.t[i].state == XBOX_PARK_RUNNING) {
            if (!running) {
                r_ord = st.t[i].ordinal;
                r_ret = st.t[i].ret_va;
            }
            running++;
        }
    fprintf(out, "  [KERNEL] park: %d guest thread%s, all parked %.1f%% of %.0f ms (%llu windows, longest so far"
                 " %.1f ms); now %d running", st.threads, st.threads == 1 ? "" : "s",
            window ? 100.0 * (double)all / (double)window : 0.0,
            (double)window / 1e6, (unsigned long long)windows, (double)st.longest_ns / 1e6, running);
    if (running)
        fprintf(out, " (one last called %u from 0x%08X)", r_ord, r_ret);
    fprintf(out, ", %d waiting\n", st.waiting);
    return 1;
}

int xbox_ParkFreeze(uint32_t timeout_ms, XboxParkStats *out)
{
    uint64_t deadline = xbox_HostNowNs() + (uint64_t)timeout_ms * 1000000ull;
    int ok;

    if (!CAS32(&s_freezer, 0, 1)) {
        if (out)
            xbox_ParkStatsGet(out, 0);
        return 0;
    }
    park_init_once();
    STORE32(&s_freeze, 1);
    EnterCriticalSection(&s_gate_lock);
    while (LOAD32(&s_busy) > 0 && xbox_HostNowNs() < deadline)
        SleepConditionVariableCS(&s_gate_cv, &s_gate_lock, 1);
    ok = LOAD32(&s_busy) <= 0;
    LeaveCriticalSection(&s_gate_lock);
    if (out)
        xbox_ParkStatsGet(out, 0);
    if (!ok)
        xbox_ParkThaw();
    return ok;
}

void xbox_ParkThaw(void)
{
    park_init_once();
    EnterCriticalSection(&s_gate_lock);
    STORE32(&s_freeze, 0);
    WakeAllConditionVariable(&s_gate_cv);
    LeaveCriticalSection(&s_gate_lock);
    STORE32(&s_freezer, 0);
}

uint32_t xbox_ParkGuestPages(uint32_t *resident, uint32_t *nonzero)
{
    size_t size = xbox_GetMappedSize();
    uint8_t *base = (uint8_t *)(uintptr_t)xbox_GetMemoryOffset();   /* guest VA 0 */
    uint32_t pages = (uint32_t)(size / 4096u), res = 0, nz = 0, p;
    unsigned char *vec = NULL;

    if (!size || !xbox_GetMemoryBase()) {
        if (resident) *resident = 0;
        if (nonzero) *nonzero = 0;
        return 0;
    }
#ifndef _WIN32
    vec = (unsigned char *)malloc(pages ? pages : 1);
    if (vec && mincore(base, (size_t)pages * 4096u, (void *)vec) != 0) {
        free(vec);
        vec = NULL;
    }
#endif
    for (p = 0; p < pages; p++) {
        const uint64_t *w = (const uint64_t *)(base + (size_t)p * 4096u);
        int k;
        if (vec && !(vec[p] & 1))
            continue;
        res++;
        for (k = 0; k < 512; k++)
            if (w[k]) { nz++; break; }
    }
    free(vec);
    if (resident) *resident = res;
    if (nonzero) *nonzero = nz;
    return pages;
}

void xbox_ParkReset(void)
{
    memset((void *)s_slots, 0, sizeof s_slots);
    s_threads = s_busy = s_freeze = s_freezer = s_dropped = 0;
    s_since_ns = s_all_ns = s_windows = s_longest_ns = s_reset_ns = 0;
    t_depth = t_cb = 0;
    t_slot = -1;
}
