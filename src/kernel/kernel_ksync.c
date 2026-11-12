/*
 * kernel_ksync.c - SLists, kernel queues, device queues, read-write locks and
 * the APC queue in guest memory (XBA-113). See kernel_ksync.h.
 *
 * These exports used to be empty: InterlockedPopEntrySList returned NULL and
 * Push dropped the node, so any lock-free free list lost everything put on
 * it; KeRemoveQueue returned at once with nothing; the read-write lock let
 * every caller in. Layouts are nxdk's xboxkrnl.h.
 */
#include "kernel.h"
#include "kernel_ksync.h"

#include <string.h>

extern ptrdiff_t g_xbox_mem_offset;

#define KS_PTR(va) ((void *)((uintptr_t)(va) + g_xbox_mem_offset))
#define M8(va)     (*(volatile uint8_t  *)KS_PTR(va))
#define M16(va)    (*(volatile uint16_t *)KS_PTR(va))
#define M32(va)    (*(volatile uint32_t *)KS_PTR(va))

#define STATUS_TIMEOUT_ 0x00000102u

/* ── One host lock and condition variable for everything but the SLists ── */

static CRITICAL_SECTION   s_lock;
static CONDITION_VARIABLE s_cv;
static volatile LONG      s_init;

static void ks_lock(void)
{
    if (s_init != 2) {
        if (InterlockedCompareExchange(&s_init, 1, 0) == 0) {
            InitializeCriticalSection(&s_lock);
            InitializeConditionVariable(&s_cv);
            InterlockedExchange(&s_init, 2);
        } else {
            while (InterlockedCompareExchange(&s_init, 2, 2) != 2)
                SwitchToThread();
        }
    }
    EnterCriticalSection(&s_lock);
}

static void ks_unlock(void) { LeaveCriticalSection(&s_lock); }

/* Wait for a wake-up with the lock held; 0 when `ms` ran out. */
static int ks_wait(DWORD ms)
{
    return SleepConditionVariableCS(&s_cv, &s_lock, ms) ? 1 : 0;
}

static void ks_wake_all(void) { WakeAllConditionVariable(&s_cv); }

/* ── Guest LIST_ENTRY helpers (lock held) ── */

static void list_init(uint32_t h) { M32(h) = h; M32(h + 4) = h; }
static int  list_empty(uint32_t h) { return M32(h) == h; }

static void list_insert_tail(uint32_t h, uint32_t e)
{
    uint32_t last = M32(h + 4);
    M32(e) = h; M32(e + 4) = last;
    M32(last) = e; M32(h + 4) = e;
}

static void list_insert_head(uint32_t h, uint32_t e)
{
    uint32_t first = M32(h);
    M32(e) = first; M32(e + 4) = h;
    M32(first + 4) = e; M32(h) = e;
}

static void list_remove(uint32_t e)
{
    uint32_t f = M32(e), b = M32(e + 4);
    M32(b) = f; M32(f + 4) = b;
}

static uint32_t list_remove_head(uint32_t h)
{
    uint32_t e = M32(h);
    list_remove(e);
    return e;
}

/* A list head nobody initialised (zeroed memory) reads as empty. */
static void list_fix(uint32_t h)
{
    if (M32(h) == 0 || M32(h + 4) == 0)
        list_init(h);
}

/* ── NT timeout -> remaining milliseconds ── */

static uint64_t ks_now_ms(void) { return (uint64_t)GetTickCount64(); }

/* INFINITE for no timeout; otherwise the absolute host deadline in ms. */
static uint64_t ks_deadline(uint32_t timeout_va)
{
    int64_t t;
    if (!timeout_va)
        return (uint64_t)-1;
    memcpy(&t, KS_PTR(timeout_va), 8);
    if (t <= 0)
        return ks_now_ms() + (uint64_t)((-t + 9999) / 10000);
    {
        LARGE_INTEGER now;
        int64_t diff;
        GetSystemTimeAsFileTime((LPFILETIME)&now);
        diff = t - now.QuadPart;
        return ks_now_ms() + (diff > 0 ? (uint64_t)(diff / 10000) : 0);
    }
}

static DWORD ks_remaining(uint64_t deadline)
{
    uint64_t now;
    if (deadline == (uint64_t)-1)
        return INFINITE;
    now = ks_now_ms();
    if (now >= deadline)
        return 0;
    return (DWORD)(deadline - now > 0x7FFFFFFFu ? 0x7FFFFFFFu : deadline - now);
}

/* ═════════════════════════════ SLists ═════════════════════════════
 *
 * SLIST_HEADER: Next (+0), Depth (+4, 16 bits), Sequence (+6, 16 bits).
 * Push adds 1 to Depth and Sequence, Pop takes 1 from Depth, Flush clears
 * Next and Depth -- the console's x86 code. Sequence is what defeats ABA: a
 * Pop that read Next from an entry which was popped and pushed back between
 * its read and its swap sees a different Sequence and retries.
 *
 * The header is swapped with one 64-bit CAS (a full barrier, which arm64
 * needs: the entry's Next must be visible before the header names it). The
 * console requires 8-byte alignment; a header without it goes under the
 * host lock, since arm64 has no unaligned 64-bit atomic -- the lifter's own
 * cmpxchg8b makes the same split. */

static int slist_aligned(uint32_t head)
{
    return ((uintptr_t)KS_PTR(head) & 7u) == 0;
}

static uint64_t slist_cas(uint32_t head, uint64_t cmp, uint64_t val)
{
    if (slist_aligned(head))
        return (uint64_t)InterlockedCompareExchange64(
            (volatile LONGLONG *)KS_PTR(head), (LONGLONG)val, (LONGLONG)cmp);
    {
        uint64_t cur;
        ks_lock();
        memcpy(&cur, KS_PTR(head), 8);
        if (cur == cmp)
            memcpy(KS_PTR(head), &val, 8);
        ks_unlock();
        return cur;
    }
}

static uint64_t slist_read(uint32_t head)
{
    /* A torn read only makes the first CAS fail, which hands back the
     * real value. */
    uint64_t v;
    memcpy(&v, KS_PTR(head), 8);
    return v;
}

uint32_t ks_slist_push(uint32_t head, uint32_t entry)
{
    uint64_t old, seen;

    if (!head || !entry)
        return 0;
    old = slist_read(head);
    for (;;) {
        uint32_t ds = (uint32_t)(old >> 32);
        uint32_t nds = ((ds + 1) & 0xFFFFu) | ((((ds >> 16) + 1) & 0xFFFFu) << 16);
        M32(entry) = (uint32_t)old;                     /* entry->Next */
        seen = slist_cas(head, old, ((uint64_t)nds << 32) | entry);
        if (seen == old)
            return (uint32_t)old;
        old = seen;
    }
}

uint32_t ks_slist_pop(uint32_t head)
{
    uint64_t old, seen;

    if (!head)
        return 0;
    old = slist_read(head);
    for (;;) {
        uint32_t first = (uint32_t)old, ds = (uint32_t)(old >> 32), next, nds;
        if (!first)
            return 0;
        /* The entry may be popped and reused under us; guest memory stays
         * mapped, and the Sequence check below throws the stale read away. */
        next = M32(first);
        nds = (ds & 0xFFFF0000u) | ((ds - 1) & 0xFFFFu);
        seen = slist_cas(head, old, ((uint64_t)nds << 32) | next);
        if (seen == old)
            return first;
        old = seen;
    }
}

uint32_t ks_slist_flush(uint32_t head)
{
    uint64_t old, seen;

    if (!head)
        return 0;
    old = slist_read(head);
    for (;;) {
        if (!(uint32_t)old)
            return 0;
        seen = slist_cas(head, old, old & 0xFFFF000000000000ull);
        if (seen == old)
            return (uint32_t)old;
        old = seen;
    }
}

/* ═════════════════════════════ KQUEUE ═════════════════════════════
 *
 *   +0x00 Header: Type 4 (QueueObject), Size 10, SignalState (+4) = number
 *         of entries, WaitListHead (+8)
 *   +0x10 EntryListHead   +0x18 CurrentCount   +0x1C MaximumCount
 *   +0x20 ThreadListHead
 *
 * Not modelled: the concurrency limit. The console counts a queue's active
 * threads and stops handing out entries at MaximumCount -- 1 on a one-CPU
 * Xbox -- but it also knows when such a thread blocks on something else and
 * lets the next one go. Without that second half, enforcing the first would
 * deadlock any title whose queue worker waits on an event, so every waiter
 * may take an entry. CurrentCount stays 0. A queue waited on with
 * KeWaitForSingleObject is not supported either. */

#define Q_SIGNAL  0x04u
#define Q_ENTRIES 0x10u
#define Q_CURRENT 0x18u
#define Q_MAX     0x1Cu
#define Q_THREADS 0x20u

void ks_queue_init(uint32_t q, uint32_t count)
{
    if (!q)
        return;
    ks_lock();
    M8(q + 0) = 4;                   /* QueueObject */
    M8(q + 1) = 0;
    M8(q + 2) = 0x28 / 4;
    M8(q + 3) = 0;
    M32(q + Q_SIGNAL) = 0;
    list_init(q + 8);
    list_init(q + Q_ENTRIES);
    M32(q + Q_CURRENT) = 0;
    M32(q + Q_MAX) = count ? count : 1;   /* 0 = number of processors */
    list_init(q + Q_THREADS);
    ks_unlock();
}

int32_t ks_queue_insert(uint32_t q, uint32_t entry, int at_head)
{
    int32_t old;

    if (!q || !entry)
        return 0;
    ks_lock();
    list_fix(q + Q_ENTRIES);
    old = (int32_t)M32(q + Q_SIGNAL);
    if (at_head)
        list_insert_head(q + Q_ENTRIES, entry);
    else
        list_insert_tail(q + Q_ENTRIES, entry);
    M32(q + Q_SIGNAL) = (uint32_t)(old + 1);
    ks_unlock();
    ks_wake_all();
    return old;
}

uint32_t ks_queue_remove(uint32_t q, uint32_t timeout_va)
{
    uint64_t deadline = ks_deadline(timeout_va);
    uint32_t got = STATUS_TIMEOUT_;

    if (!q)
        return STATUS_TIMEOUT_;
    ks_lock();
    list_fix(q + Q_ENTRIES);
    for (;;) {
        DWORD ms;
        if (!list_empty(q + Q_ENTRIES)) {
            got = list_remove_head(q + Q_ENTRIES);
            M32(got) = 0;                 /* Entry->Flink = NULL, as NT does */
            M32(q + Q_SIGNAL) = M32(q + Q_SIGNAL) - 1;
            break;
        }
        ms = ks_remaining(deadline);
        if (ms == 0)
            break;
        ks_wait(ms);
    }
    ks_unlock();
    return got;
}

uint32_t ks_queue_rundown(uint32_t q)
{
    uint32_t first;

    if (!q)
        return 0;
    ks_lock();
    list_fix(q + Q_ENTRIES);
    if (list_empty(q + Q_ENTRIES)) {
        first = 0;
    } else {
        /* The entries stay linked to each other, minus the head. */
        first = M32(q + Q_ENTRIES);
        list_remove(q + Q_ENTRIES);
        list_init(q + Q_ENTRIES);
    }
    M32(q + Q_SIGNAL) = 0;
    list_init(q + Q_THREADS);
    ks_unlock();
    return first;
}

/* ═════════════════════════ KDEVICE_QUEUE ═════════════════════════
 *
 *   KDEVICE_QUEUE: Type (+0, 16 bits, 0x14), Size (+2), Busy (+3),
 *                  DeviceListHead (+4)
 *   KDEVICE_QUEUE_ENTRY: DeviceListEntry (+0), SortKey (+8), Inserted (+0xC)
 *
 * Insert on an idle queue marks it busy and returns FALSE: the caller starts
 * the packet itself. Remove on an empty queue marks it idle. */

#define DQ_BUSY 3u
#define DQ_LIST 4u

void ks_devq_init(uint32_t dq)
{
    if (!dq)
        return;
    ks_lock();
    M16(dq + 0) = 0x14;              /* DeviceQueueObject */
    M8(dq + 2) = 0x0C;
    M8(dq + DQ_BUSY) = 0;
    list_init(dq + DQ_LIST);
    ks_unlock();
}

int ks_devq_insert(uint32_t dq, uint32_t entry)
{
    int queued;

    if (!dq || !entry)
        return 0;
    ks_lock();
    list_fix(dq + DQ_LIST);
    if (!M8(dq + DQ_BUSY)) {
        M8(dq + DQ_BUSY) = 1;
        queued = 0;
    } else {
        list_insert_tail(dq + DQ_LIST, entry);
        queued = 1;
    }
    M8(entry + 0x0C) = (uint8_t)queued;
    ks_unlock();
    return queued;
}

int ks_devq_insert_by_key(uint32_t dq, uint32_t entry, uint32_t key)
{
    int queued;

    if (!dq || !entry)
        return 0;
    ks_lock();
    list_fix(dq + DQ_LIST);
    M32(entry + 8) = key;
    if (!M8(dq + DQ_BUSY)) {
        M8(dq + DQ_BUSY) = 1;
        queued = 0;
    } else {
        /* Before the first entry with a greater key: equal keys stay FIFO. */
        uint32_t h = dq + DQ_LIST, e = M32(h);
        while (e != h && M32(e + 8) <= key)
            e = M32(e);
        list_insert_tail(e, entry);   /* tail of "e" = just before e */
        queued = 1;
    }
    M8(entry + 0x0C) = (uint8_t)queued;
    ks_unlock();
    return queued;
}

uint32_t ks_devq_remove(uint32_t dq)
{
    uint32_t e = 0;

    if (!dq)
        return 0;
    ks_lock();
    list_fix(dq + DQ_LIST);
    if (list_empty(dq + DQ_LIST)) {
        M8(dq + DQ_BUSY) = 0;
    } else {
        e = list_remove_head(dq + DQ_LIST);
        M8(e + 0x0C) = 0;
    }
    ks_unlock();
    return e;
}

uint32_t ks_devq_remove_by_key(uint32_t dq, uint32_t key)
{
    uint32_t e = 0;

    if (!dq)
        return 0;
    ks_lock();
    list_fix(dq + DQ_LIST);
    if (list_empty(dq + DQ_LIST)) {
        M8(dq + DQ_BUSY) = 0;
    } else {
        /* The first entry whose key is at least SortKey, else the first. */
        uint32_t h = dq + DQ_LIST;
        e = M32(h);
        while (e != h && M32(e + 8) < key)
            e = M32(e);
        if (e == h)
            e = M32(h);
        list_remove(e);
        M8(e + 0x0C) = 0;
    }
    ks_unlock();
    return e;
}

int ks_devq_remove_entry(uint32_t dq, uint32_t entry)
{
    int was;

    if (!dq || !entry)
        return 0;
    ks_lock();
    was = M8(entry + 0x0C) != 0;
    if (was) {
        list_remove(entry);
        M8(entry + 0x0C) = 0;
    }
    ks_unlock();
    return was;
}

/* ═════════════════════════════ ERWLOCK ═════════════════════════════
 *
 *   +0x00 LockCount (-1 = free)     +0x04 WritersWaitingCount
 *   +0x08 ReadersWaitingCount       +0x0C ReadersEntryCount
 *   +0x10 WriterEvent (KEVENT, synchronization; SignalState +0x14)
 *   +0x20 ReaderSemaphore (KSEMAPHORE; SignalState +0x24, Limit +0x30)
 *
 * The console's algorithm, run under the host lock where the console runs
 * it with interrupts off. A waiting writer sleeps until the event's
 * SignalState is set and takes it; a waiting reader until the semaphore's
 * count is positive and takes one -- the same counts the console keeps, so a
 * title that reads them sees them move as they would there. */

#define RW_COUNT   0x00u
#define RW_WWAIT   0x04u
#define RW_RWAIT   0x08u
#define RW_RENTRY  0x0Cu
#define RW_EVENT   0x10u
#define RW_SEM     0x20u

void ks_rwlock_init(uint32_t l)
{
    if (!l)
        return;
    ks_lock();
    memset(KS_PTR(l), 0, 0x34);
    M32(l + RW_COUNT) = 0xFFFFFFFFu;
    M8(l + RW_EVENT + 0) = 1;               /* SynchronizationEvent */
    M8(l + RW_EVENT + 2) = 4;
    list_init(l + RW_EVENT + 8);
    M8(l + RW_SEM + 0) = 5;                 /* SemaphoreObject */
    M8(l + RW_SEM + 2) = 5;
    list_init(l + RW_SEM + 8);
    M32(l + RW_SEM + 0x10) = 0x7FFFFFFFu;   /* Limit */
    ks_unlock();
}

void ks_rwlock_acquire_exclusive(uint32_t l)
{
    if (!l)
        return;
    ks_lock();
    M32(l + RW_COUNT) = M32(l + RW_COUNT) + 1;
    if (M32(l + RW_COUNT) != 0) {
        M32(l + RW_WWAIT) = M32(l + RW_WWAIT) + 1;
        while (M32(l + RW_EVENT + 4) == 0)
            ks_wait(INFINITE);
        M32(l + RW_EVENT + 4) = 0;
    }
    ks_unlock();
}

void ks_rwlock_acquire_shared(uint32_t l)
{
    if (!l)
        return;
    ks_lock();
    M32(l + RW_COUNT) = M32(l + RW_COUNT) + 1;
    if (M32(l + RW_COUNT) != 0
        && (M32(l + RW_RENTRY) == 0 || M32(l + RW_WWAIT) != 0)) {
        M32(l + RW_RWAIT) = M32(l + RW_RWAIT) + 1;
        while ((int32_t)M32(l + RW_SEM + 4) <= 0)
            ks_wait(INFINITE);
        M32(l + RW_SEM + 4) = M32(l + RW_SEM + 4) - 1;
    } else {
        M32(l + RW_RENTRY) = M32(l + RW_RENTRY) + 1;
    }
    ks_unlock();
}

void ks_rwlock_release(uint32_t l)
{
    uint32_t n;

    if (!l)
        return;
    ks_lock();
    M32(l + RW_COUNT) = M32(l + RW_COUNT) - 1;
    if (M32(l + RW_COUNT) == 0xFFFFFFFFu) {         /* now free */
        M32(l + RW_RENTRY) = 0;
        ks_unlock();
        return;
    }
    if (M32(l + RW_RENTRY) == 0) {                  /* a writer leaves */
        n = M32(l + RW_RWAIT);
        if (n) {                                    /* readers go first */
            M32(l + RW_RENTRY) = n;
            M32(l + RW_RWAIT) = 0;
            M32(l + RW_SEM + 4) = M32(l + RW_SEM + 4) + n;
            ks_unlock();
            ks_wake_all();
            return;
        }
    } else {                                        /* a reader leaves */
        M32(l + RW_RENTRY) = M32(l + RW_RENTRY) - 1;
        if (M32(l + RW_RENTRY) != 0) {
            ks_unlock();
            return;
        }
    }
    M32(l + RW_WWAIT) = M32(l + RW_WWAIT) - 1;      /* hand to a writer */
    M32(l + RW_EVENT + 4) = 1;
    ks_unlock();
    ks_wake_all();
}

/* ═════════════════════════════ KAPC ═════════════════════════════
 *
 *   KAPC: Type (+0, 16 bits, 0x12), ApcMode (+2), Inserted (+3), Thread (+4),
 *         ApcListEntry (+8), KernelRoutine (+0x10), RundownRoutine (+0x14),
 *         NormalRoutine (+0x18), NormalContext (+0x1C),
 *         SystemArgument1 (+0x20), SystemArgument2 (+0x24)
 *   KTHREAD.ApcState: ApcListHead[KernelMode] (+0x34), [UserMode] (+0x3C),
 *         KernelApcPending (+0x49), UserApcPending (+0x4A)
 *
 * Queued here; run by the bridge (kernel_bridge.c, bridge_apc_safe_point) on
 * the target thread. */

static volatile LONG s_apc_pending;

int ks_apc_pending(void) { return s_apc_pending != 0; }

void ks_apc_init(uint32_t apc, uint32_t thread, uint32_t kernel_routine,
                 uint32_t rundown_routine, uint32_t normal_routine,
                 uint32_t mode, uint32_t normal_context)
{
    if (!apc)
        return;
    memset(KS_PTR(apc), 0, 0x28);
    M16(apc + 0) = 0x12;             /* ApcObject */
    M32(apc + 0x04) = thread;
    M32(apc + 0x10) = kernel_routine;
    M32(apc + 0x14) = rundown_routine;
    M32(apc + 0x18) = normal_routine;
    if (normal_routine) {
        M8(apc + 2) = (uint8_t)mode;
        M32(apc + 0x1C) = normal_context;
    }                                /* else: kernel mode, no context */
}

int ks_apc_insert(uint32_t apc, uint32_t arg1, uint32_t arg2)
{
    uint32_t t, mode;

    if (!apc)
        return 0;
    ks_lock();
    t = M32(apc + 4);
    if (!t || M8(apc + 3)) {
        ks_unlock();
        return 0;
    }
    mode = M8(apc + 2) ? 1u : 0u;
    list_fix(t + 0x34 + mode * 8);
    M32(apc + 0x20) = arg1;
    M32(apc + 0x24) = arg2;
    M8(apc + 3) = 1;
    list_insert_tail(t + 0x34 + mode * 8, apc + 8);
    M8(t + 0x49 + mode) = 1;
    InterlockedIncrement(&s_apc_pending);
    ks_unlock();
    return 1;
}

uint32_t ks_apc_take(uint32_t kthread, int mode)
{
    uint32_t h, apc = 0;

    if (!kthread || !s_apc_pending)
        return 0;
    h = kthread + 0x34 + (mode ? 8u : 0u);
    ks_lock();
    if (M32(h) && !list_empty(h)) {
        apc = list_remove_head(h) - 8;
        M8(apc + 3) = 0;
        InterlockedDecrement(&s_apc_pending);
    }
    if (M32(h) == 0 || list_empty(h))
        M8(kthread + 0x49 + (mode ? 1 : 0)) = 0;
    ks_unlock();
    return apc;
}
