/*
 * kernel_park.c - the safe-point park detector (see kernel_park.h, XBA-244)
 *
 * Per thread: its kernel-call depth and its bridge-callback depth (thread-local, every host thread),
 * and for a registered guest thread the depths at which its routine started. The thread is parked
 * when it is exactly one kernel call deeper than that and in no further callback. A host wait inside
 * lifted code (xbox_ParkHostWaitEnter, XBA-251) counts as one kernel-call level, so it parks exactly as a
 * top-level kernel call does; its own nesting (a kick wait inside a poll wait) is counted apart (t_hw).
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
#define FENCE()        MemoryBarrier()
#else
#define ADD32(p, v)    __atomic_add_fetch((p), (int32_t)(v), __ATOMIC_SEQ_CST)
#define LOAD32(p)      __atomic_load_n((p), __ATOMIC_SEQ_CST)
#define STORE32(p, v)  __atomic_store_n((p), (int32_t)(v), __ATOMIC_SEQ_CST)
#define CAS32(p, o, n) __extension__ ({ int32_t _o = (o); \
        __atomic_compare_exchange_n((p), &_o, (int32_t)(n), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); })
#define ADD64(p, v)    ((void)__atomic_fetch_add((p), (uint64_t)(v), __ATOMIC_RELAXED))
#define LOAD64(p)      __atomic_load_n((p), __ATOMIC_RELAXED)
#define STORE64(p, v)  __atomic_store_n((p), (uint64_t)(v), __ATOMIC_RELAXED)
#define FENCE()        __atomic_thread_fence(__ATOMIC_SEQ_CST)
#endif

typedef char park_cpu_fits[sizeof(RecompCpu) <= XBOX_PARK_CPU_BYTES ? 1 : -1];

typedef struct {
    volatile int32_t state;          /* XBOX_PARK_*; written by the owner, read by anyone */
    volatile uint32_t ordinal, ret_va;
    volatile int32_t dispatch;
    volatile unsigned long tid;
    volatile uint64_t kernel_calls;
    /* The park record (kernel_park.h): written by the owner between two seq increments. */
    volatile uint32_t seq;
    volatile int32_t returned;
    volatile uint32_t status;
    volatile uint32_t site;          /* XBOX_PARK_SITE_* of the current park point */
    volatile uint32_t call_esp;      /* esp at the call's Enter (the return address's slot), kept by Leave */
    RecompCpu cpu;
    /* Same-run restore (XBA-249): the owner's own g_cpu (thread-local, valid while it is registered), and a
     * call to issue again once the freeze lets it go (xbox_ParkRestoreThread / xbox_ParkTakeReissue). */
    RecompCpu *live;
    volatile int32_t reissue;
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
static XBOX_THREAD_LOCAL int t_hw;       /* host waits this thread is inside (nested ones count once) */
static XBOX_THREAD_LOCAL int t_hw_counted;   /* the outermost one is a kernel-call level (park_enter) */

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

const char *xbox_ParkSiteName(uint32_t site)
{
    switch (site) {
    case XBOX_PARK_SITE_KERNEL:  return "kernel call";
    case XBOX_PARK_SITE_POLL:    return "poll loop";
    case XBOX_PARK_SITE_KICK:    return "NV2A kick wait";
    case XBOX_PARK_SITE_HALT:    return "hlt";
    case XBOX_PARK_SITE_PREEMPT: return "guest CPU preempt";
    default:                     return "?";
    }
}

/* The parked state for a park point: WAITING where it can block for long. */
static int site_state(uint32_t site, uint32_t ordinal)
{
    int blocks = site == XBOX_PARK_SITE_KERNEL ? xbox_ParkOrdinalBlocks(ordinal) : site != XBOX_PARK_SITE_POLL;
    return blocks ? XBOX_PARK_WAITING : XBOX_PARK_KERNEL;
}

/* The owner rewrites its record: seq odd meanwhile (readers wait, xbox_ParkRecords). Enter: the call and
 * the registers as the guest made it; Leave (returned): the registers as the call left them. */
static void record_write(Slot *s, int returned, uint32_t site, uint32_t ordinal, uint32_t ret_va)
{
    STORE32((volatile int32_t *)&s->seq, (int32_t)(s->seq + 1));
    if (!returned) {
        s->site = site;
        s->ordinal = ordinal;
        s->ret_va = ret_va;
        s->call_esp = g_esp;
    }
    s->returned = returned;
    s->status = returned ? g_eax : 0;
    memcpy(&s->cpu, (const void *)&g_cpu, sizeof s->cpu);
    STORE32((volatile int32_t *)&s->seq, (int32_t)(s->seq + 1));
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
            s->returned = 0;
            s->status = 0;
            s->site = XBOX_PARK_SITE_KERNEL;
            s->call_esp = 0;
            s->live = (RecompCpu *)&g_cpu;
            s->reissue = 0;
            memset(&s->cpu, 0, sizeof s->cpu);
            /* A new generation of the slot: a capture that read the previous thread's record sees it change. */
            STORE32((volatile int32_t *)&s->seq, (int32_t)((s->seq + 2) & ~1u));
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

/* A park point (a kernel call or a host wait) begins / ends: one kernel-call level. */
static void park_enter(uint32_t site, uint32_t ordinal, uint32_t ret_va)
{
    int was = t_slot >= 0 && parked_now();

    t_depth++;
    if (t_slot < 0 || was || !parked_now())
        return;
    {
        Slot *s = &s_slots[t_slot];
        record_write(s, 0, site, ordinal, ret_va);
        if (site == XBOX_PARK_SITE_KERNEL)
            s->kernel_calls++;
        STORE32(&s->state, site_state(site, ordinal));
    }
    if (site == XBOX_PARK_SITE_KERNEL && ordinal == 258) {   /* PsTerminateSystemThread: it ends in there */
        t_nest = 0;
        unregister(1);
        return;
    }
    go_idle();
}

static void park_leave(void)
{
    int was = t_slot >= 0 && parked_now();

    if (was)
        record_write(&s_slots[t_slot], 1, 0, 0, 0);   /* the registers as the call left them */
    t_depth--;
    if (was && !parked_now())
        go_busy(XBOX_PARK_RUNNING);
}

void xbox_ParkKernelEnter(uint32_t ordinal, uint32_t ret_va)
{
    park_enter(XBOX_PARK_SITE_KERNEL, ordinal, ret_va);
}

void xbox_ParkKernelLeave(void)
{
    park_leave();
}

void xbox_ParkKernelReturned(void)
{
    if (t_slot >= 0 && parked_now())
        record_write(&s_slots[t_slot], 1, 0, 0, 0);
}

/* Only a guest thread's host wait outside a kernel call is a park point of its own (counted as one
 * kernel-call level); one inside a kernel call (a bridge's helper) leaves that call the park point. */
void xbox_ParkHostWaitEnter(uint32_t site, uint32_t va)
{
    if (t_hw++ > 0)
        return;
    t_hw_counted = t_slot >= 0 && !parked_now();
    if (!t_hw_counted)
        return;
    if (site == XBOX_PARK_SITE_KERNEL || site >= XBOX_PARK_SITE_COUNT)
        site = XBOX_PARK_SITE_POLL;
    park_enter(site, 0, va);
}

int xbox_ParkHostWaitLeave(void)
{
    int again = 0;

    if (t_hw <= 0 || --t_hw > 0)
        return 0;
    if (t_hw_counted) {
        park_leave();
        again = xbox_ParkTakeReissue();   /* a same-run load put the thread back inside this wait (XBA-249) */
    }
    t_hw_counted = 0;
    return again;
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
        STORE32(&s->state, site_state(s->site, s->ordinal));
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
        STORE32(&s->state, site_state(s->site, s->ordinal));
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
        out->t[n].site = s->site;
        if (st != XBOX_PARK_RUNNING) {
            out->parked++;
            if (s->site != XBOX_PARK_SITE_KERNEL)
                out->host_waits++;
        }
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
    int i, running = 0, hw = -1, r = -1;
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
            if (!running)
                r = i;
            running++;
        } else if (st.t[i].site != XBOX_PARK_SITE_KERNEL && hw < 0) {
            hw = i;
        }
    fprintf(out, "  [KERNEL] park: %d guest thread%s, all parked %.1f%% of %.0f ms (%llu windows, longest so far"
                 " %.1f ms); now %d running", st.threads, st.threads == 1 ? "" : "s",
            window ? 100.0 * (double)all / (double)window : 0.0,
            (double)window / 1e6, (unsigned long long)windows, (double)st.longest_ns / 1e6, running);
    if (r >= 0 && st.t[r].site != XBOX_PARK_SITE_KERNEL)
        fprintf(out, " (one last in %s at 0x%08X)", xbox_ParkSiteName(st.t[r].site), st.t[r].ret_va);
    else if (r >= 0)
        fprintf(out, " (one last called %u from 0x%08X)", st.t[r].ordinal, st.t[r].ret_va);
    fprintf(out, ", %d waiting", st.waiting);
    if (hw >= 0)
        fprintf(out, ", %d at a host wait (%s at 0x%08X)", st.host_waits, xbox_ParkSiteName(st.t[hw].site),
                st.t[hw].ret_va);
    fputc('\n', out);
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

int xbox_ParkRecords(XboxParkRecord *out, int max)
{
    int i, n = 0;

    for (i = 0; i < XBOX_PARK_MAX_THREADS && n < max; i++) {
        Slot *s = &s_slots[i];
        XboxParkRecord *r = &out[n];
        uint32_t s1, s2;
        int tries = 0;

        if (LOAD32(&s->state) == XBOX_PARK_FREE)
            continue;
        for (;;) {
            s1 = (uint32_t)LOAD32((volatile int32_t *)&s->seq);
            if (s1 & 1u) {          /* being rewritten: the owner is inside its Leave hook */
                if (++tries > 1000)
                    SwitchToThread();
                continue;
            }
            memset(r, 0, sizeof *r);
            r->slot = i;
            r->state = LOAD32(&s->state);
            r->ordinal = s->ordinal;
            r->ret_va = s->ret_va;
            r->returned = s->returned;
            r->status = s->status;
            r->call_esp = s->call_esp;
            r->dispatch = s->dispatch;
            r->tid = s->tid;
            r->kernel_calls = s->kernel_calls;
            r->site = s->site;
            r->reissue = LOAD32(&s->reissue);
            r->cpu_bytes = (uint32_t)sizeof(RecompCpu);
            memcpy(r->cpu, (const void *)&s->cpu, sizeof(RecompCpu));
            FENCE();
            s2 = (uint32_t)LOAD32((volatile int32_t *)&s->seq);
            if (s1 == s2)
                break;
        }
        r->seq = s1;
        if (r->state == XBOX_PARK_FREE)     /* it ended while being read */
            continue;
        n++;
    }
    return n;
}

int xbox_ParkRecordsChanged(const XboxParkRecord *r, int n)
{
    int i, live = 0;

    for (i = 0; i < XBOX_PARK_MAX_THREADS; i++)
        if (LOAD32(&s_slots[i].state) != XBOX_PARK_FREE)
            live++;
    if (live != n)
        return 1;
    for (i = 0; i < n; i++) {
        Slot *s = &s_slots[r[i].slot];
        if (LOAD32(&s->state) == XBOX_PARK_FREE || (uint32_t)LOAD32((volatile int32_t *)&s->seq) != r[i].seq)
            return 1;
    }
    return 0;
}

int xbox_ParkRestoreThread(int slot, const void *cpu_bytes, int reissue)
{
    const RecompCpu *cpu = (const RecompCpu *)cpu_bytes;
    Slot *s;

    if (slot < 0 || slot >= XBOX_PARK_MAX_THREADS || !cpu || !LOAD32(&s_freeze))
        return -1;
    s = &s_slots[slot];
    if (!s->live)
        return -1;
    /* A host wait inside lifted code still waiting (XBA-251): the helper does not touch g_cpu while it waits,
     * and the freeze holds the thread at its Leave once the wait ends, so its registers can be set now; the
     * record is rewritten by that Leave. Reissue: the helper waits once more after that. */
    if (s->site != XBOX_PARK_SITE_KERNEL && !s->returned
        && (LOAD32(&s->state) == XBOX_PARK_WAITING || LOAD32(&s->state) == XBOX_PARK_KERNEL)) {
        memcpy(s->live, cpu, sizeof *cpu);
        STORE32(&s->reissue, reissue ? 1 : 0);
        return 0;
    }
    /* Held at the end of its call: the owner sits in go_busy (Leave) until the thaw, and reads its g_cpu only
     * after it, through the gate lock the thaw takes. Or still queued for the guest CPU after its bridge
     * returned (xbox_ParkKernelReturned): it reads g_cpu only once it has the CPU and is past the freeze. Or
     * held still from a previous load that set it to make its call again. */
    if (LOAD32(&s->state) == XBOX_PARK_RUNNING || LOAD32(&s->state) == XBOX_PARK_FREE
        || !(s->returned || (LOAD32(&s->reissue))))
        return -1;
    memcpy(s->live, cpu, sizeof *cpu);
    STORE32((volatile int32_t *)&s->seq, (int32_t)(s->seq + 1));
    memcpy(&s->cpu, cpu, sizeof s->cpu);
    s->returned = !reissue;
    s->status = reissue ? 0 : cpu->r_eax;
    s->call_esp = reissue ? cpu->r_esp : s->call_esp;
    STORE32((volatile int32_t *)&s->seq, (int32_t)(s->seq + 1));
    STORE32(&s->reissue, reissue ? 1 : 0);
    return 0;
}

int xbox_ParkTakeReissue(void)
{
    if (t_slot < 0)
        return 0;
    return InterlockedExchange((volatile LONG *)&s_slots[t_slot].reissue, 0) != 0;
}

int xbox_ParkDropped(void)
{
    return LOAD32(&s_dropped);
}

int xbox_ParkThisThreadIsGuest(void)
{
    return t_slot >= 0;
}

void xbox_ParkReset(void)
{
    memset((void *)s_slots, 0, sizeof s_slots);
    s_threads = s_busy = s_freeze = s_freezer = s_dropped = 0;
    s_since_ns = s_all_ns = s_windows = s_longest_ns = s_reset_ns = 0;
    t_depth = t_cb = t_hw = t_hw_counted = 0;
    t_slot = -1;
}
