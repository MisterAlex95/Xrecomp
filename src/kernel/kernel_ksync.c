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
#include "guest_clock.h"   /* fast-forward: guest spans on the host (XBA-240) */
#include "kernel_ksync.h"
#include "xbox_memory_layout.h"   /* RECOMP_TLS */

#include <stdlib.h>
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
    /* XBA-240: the timeout is guest time: the speed's share of it on the
     * host's clock (as is at speed 1); an absolute one is against the
     * guest's clock. */
    if (t <= 0)
        return ks_now_ms() + xbox_GuestClockHostSpan((uint64_t)((-t + 9999) / 10000),
                                                     xbox_GuestClockSpeed());
    {
        LARGE_INTEGER now;
        int64_t diff;
        GetSystemTimeAsFileTime((LPFILETIME)&now);
        now.QuadPart += (LONGLONG)xbox_GuestClockExtraUnits(10000000u);
        diff = t - now.QuadPart;
        return ks_now_ms() + xbox_GuestClockHostSpan(diff > 0 ? (uint64_t)(diff / 10000) : 0,
                                                     xbox_GuestClockSpeed());
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
 * The concurrency limit (XBA-133): a thread that took an entry is
 * associated with the queue and counts in CurrentCount while it runs; a
 * remover gets an entry only while CurrentCount < MaximumCount (1 on a
 * one-CPU Xbox, the default). The console also lets the next thread go when
 * an associated thread blocks on something else; here a thread counts as
 * not running for the length of every kernel call it makes
 * (ks_thread_kernel_enter / _leave, from kernel_thunk_dispatch), which
 * covers every wait and errs only towards letting more threads run --
 * during a call that does not block. A thread running guest code without
 * any kernel call counts as running, as on the console. RECOMP_QUEUE_LIMIT=0
 * hands an entry to every waiter, as before. A queue waited on with
 * KeWaitForSingleObject is not supported. */

/* ── Queue association of a thread, for the concurrency limit ──
 *
 * kind 1: a KQUEUE at guest `obj`, CurrentCount/MaximumCount in its body;
 * kind 2: an I/O completion object, slot `obj` of s_ports. `gen` tells a
 * queue initialised again (or a port closed) from the one the thread took
 * its entry from: a stale association is dropped, never counted. */
static RECOMP_TLS int      t_q_kind;
static RECOMP_TLS uint32_t t_q_obj;
static RECOMP_TLS unsigned t_q_gen;
static RECOMP_TLS int      t_q_counted;   /* counts in CurrentCount now */
static RECOMP_TLS int      t_q_depth;     /* kernel call nesting */

static int q_limit_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_QUEUE_LIMIT");
        on = !(e && *e == '0');
    }
    return on;
}

/* KQUEUE generations, by address: bumped by KeInitializeQueue and
 * KeRundownQueue. Lock held. */
#define KQ_GEN_MAX 128
static struct { uint32_t va; unsigned gen; } s_kq_gen[KQ_GEN_MAX];
static unsigned s_gen_seq;

static unsigned kq_gen(uint32_t q, int bump)
{
    int i, free_i = -1;
    for (i = 0; i < KQ_GEN_MAX; i++) {
        if (s_kq_gen[i].va == q) {
            if (bump) s_kq_gen[i].gen = ++s_gen_seq;
            return s_kq_gen[i].gen;
        }
        if (!s_kq_gen[i].va && free_i < 0) free_i = i;
    }
    if (free_i < 0)
        free_i = (int)(q / 8u % KQ_GEN_MAX);   /* full: evict, which only drops associations */
    s_kq_gen[free_i].va = q;
    s_kq_gen[free_i].gen = ++s_gen_seq;
    return s_kq_gen[free_i].gen;
}

static volatile int32_t *q_current(void);   /* the associated object's count, or NULL */

/* Lock held: the thread takes an entry from (kind, obj) and counts there. */
static void q_associate(int kind, uint32_t obj, unsigned gen)
{
    volatile int32_t *cur;
    if (t_q_counted && (cur = q_current()) != NULL)
        (*cur)--;                    /* moving from another queue */
    t_q_kind = kind; t_q_obj = obj; t_q_gen = gen;
    if ((cur = q_current()) != NULL)
        (*cur)++;
    t_q_counted = 1;
}

void ks_thread_kernel_enter(void)
{
    volatile int32_t *cur;
    if (t_q_depth++ != 0 || !t_q_kind || !t_q_counted)
        return;
    ks_lock();
    if ((cur = q_current()) != NULL)
        (*cur)--;
    t_q_counted = 0;
    ks_unlock();
    ks_wake_all();                   /* the next remover may go */
}

void ks_thread_kernel_leave(void)
{
    volatile int32_t *cur;
    if (--t_q_depth != 0 || !t_q_kind || t_q_counted)
        return;
    ks_lock();
    if ((cur = q_current()) != NULL) {
        (*cur)++;
        t_q_counted = 1;
    } else {
        t_q_kind = 0;                /* its queue is gone */
    }
    ks_unlock();
}

void ks_thread_exit(void)
{
    volatile int32_t *cur;
    if (!t_q_kind)
        return;
    ks_lock();
    if (t_q_counted && (cur = q_current()) != NULL)
        (*cur)--;
    t_q_kind = 0;
    t_q_counted = 0;
    ks_unlock();
    ks_wake_all();
}

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
    kq_gen(q, 1);
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
    {
        /* The caller stops counting where it did (it is waiting now). */
        volatile int32_t *cur;
        if (t_q_counted && (cur = q_current()) != NULL)
            (*cur)--;
        t_q_counted = 0;
    }
    for (;;) {
        DWORD ms;
        if (!list_empty(q + Q_ENTRIES)
                && (!q_limit_on() || (int32_t)M32(q + Q_CURRENT) < (int32_t)M32(q + Q_MAX))) {
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
    /* Associated with this queue from now on, entry or not, and counting
     * at once: an entry taken holds its slot before the lock goes. */
    q_associate(1, q, kq_gen(q, 0));
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
    M32(q + Q_CURRENT) = 0;          /* its threads are no longer associated */
    kq_gen(q, 1);
    ks_unlock();
    ks_wake_all();
    return first;
}

/* ══════════════════════ I/O completion objects ══════════════════════
 *
 * NtCreateIoCompletion and friends (XBA-114). On the console an I/O
 * completion object is a KQUEUE whose entries are completion packets the
 * kernel allocates: (KeyContext, ApcContext, IoStatus, Information). The
 * title never sees the queue or the packets, only a handle, so both live
 * host-side here, under the same lock and condition variable as the rest.
 *
 * `ident` is the host HANDLE the bridge hands the title a token for: a
 * manual-reset event that is signalled while packets are queued, the KQUEUE's
 * SignalState, so a plain wait on the handle answers as on the console.
 *
 * A file joins a port with NtSetInformationFile(FileCompletionInformation):
 * its requests that carry an ApcContext and no APC routine then post their
 * completion there (ks_port_post_for_file), with the file's key. */

#define KS_PORT_MAX  64
#define KS_PASSOC_MAX 128

typedef struct ks_pkt {
    struct ks_pkt *next;
    uint32_t key, ctx, status, info;
} ks_pkt;

typedef struct {
    void    *ident;          /* NULL: free slot */
    unsigned gen;            /* bumped on close: stale references see it */
    int32_t  depth;
    int32_t  max;            /* the creation Count (0 = processors = 1) */
    int32_t  cur;            /* associated threads running (the KQUEUE's CurrentCount) */
    ks_pkt  *head, *tail;
} ks_port;

static ks_port s_ports[KS_PORT_MAX];
static unsigned s_port_gen;

static volatile int32_t *q_current(void)   /* lock held */
{
    if (t_q_kind == 1 && kq_gen(t_q_obj, 0) == t_q_gen)
        return (volatile int32_t *)KS_PTR(t_q_obj + Q_CURRENT);
    if (t_q_kind == 2 && t_q_obj < KS_PORT_MAX && s_ports[t_q_obj].ident
            && s_ports[t_q_obj].gen == t_q_gen)
        return &s_ports[t_q_obj].cur;
    return NULL;
}

static struct {
    void    *file;           /* NULL: free */
    int      port;
    unsigned gen;
    uint32_t key;
} s_passoc[KS_PASSOC_MAX];

static int port_find(void *ident)   /* lock held; slot or -1 */
{
    int i;
    if (!ident)
        return -1;
    for (i = 0; i < KS_PORT_MAX; i++)
        if (s_ports[i].ident == ident)
            return i;
    return -1;
}

int ks_port_create(void *ident, uint32_t count)
{
    int i, ok = 0;
    if (!ident)
        return 0;
    ks_lock();
    for (i = 0; i < KS_PORT_MAX; i++)
        if (!s_ports[i].ident) {
            memset(&s_ports[i], 0, sizeof s_ports[i]);
            s_ports[i].ident = ident;
            s_ports[i].gen = ++s_port_gen;
            s_ports[i].max = count ? (int32_t)count : 1;
            ok = 1;
            break;
        }
    ks_unlock();
    return ok;
}

int ks_port_is(void *ident)
{
    int r;
    ks_lock();
    r = port_find(ident) >= 0;
    ks_unlock();
    return r;
}

/* Lock held. */
static int port_post_locked(int slot, uint32_t key, uint32_t ctx,
                            uint32_t status, uint32_t info)
{
    ks_port *p = &s_ports[slot];
    ks_pkt *k = (ks_pkt *)malloc(sizeof *k);
    if (!k)
        return -1;
    k->next = NULL;
    k->key = key; k->ctx = ctx; k->status = status; k->info = info;
    if (p->tail) p->tail->next = k; else p->head = k;
    p->tail = k;
    if (p->depth++ == 0)
        SetEvent((HANDLE)p->ident);
    return 0;
}

int ks_port_post(void *ident, uint32_t key, uint32_t ctx, uint32_t status,
                 uint32_t info)
{
    int slot, r = -1;
    ks_lock();
    slot = port_find(ident);
    if (slot >= 0)
        r = port_post_locked(slot, key, ctx, status, info);
    ks_unlock();
    if (r == 0)
        ks_wake_all();
    return r;
}

uint32_t ks_port_remove(void *ident, uint32_t timeout_va, uint32_t out[4])
{
    uint64_t deadline = ks_deadline(timeout_va);
    uint32_t rc;
    unsigned gen;
    int slot;

    ks_lock();
    slot = port_find(ident);
    if (slot < 0) {
        ks_unlock();
        return 0xC0000008u;                     /* STATUS_INVALID_HANDLE */
    }
    gen = s_ports[slot].gen;
    {
        volatile int32_t *cur;
        if (t_q_counted && (cur = q_current()) != NULL)
            (*cur)--;
        t_q_counted = 0;
    }
    for (;;) {
        ks_port *p = &s_ports[slot];
        DWORD ms;
        if (p->gen != gen || p->ident != ident) {
            /* Closed under us. The console keeps the object alive for its
             * waiters until they time out; here the packets went with it. */
            rc = 0xC0000008u;
            break;
        }
        if (p->head && (!q_limit_on() || p->cur < p->max)) {
            ks_pkt *k = p->head;
            p->head = k->next;
            if (!p->head) p->tail = NULL;
            if (--p->depth == 0)
                ResetEvent((HANDLE)p->ident);
            out[0] = k->key; out[1] = k->ctx; out[2] = k->status; out[3] = k->info;
            free(k);
            rc = 0;
            break;
        }
        ms = ks_remaining(deadline);
        if (ms == 0) {
            rc = STATUS_TIMEOUT_;
            break;
        }
        ks_wait(ms);
    }
    if (s_ports[slot].ident == ident && s_ports[slot].gen == gen)
        q_associate(2, (uint32_t)slot, gen);   /* as KeRemoveQueue */
    ks_unlock();
    return rc;
}

int32_t ks_port_depth(void *ident)
{
    int slot;
    int32_t d = -1;
    ks_lock();
    slot = port_find(ident);
    if (slot >= 0)
        d = s_ports[slot].depth;
    ks_unlock();
    return d;
}

int ks_port_associate(void *file, void *port, uint32_t key)
{
    int slot, i, free_i = -1, r = -1;
    if (!file)
        return -1;
    ks_lock();
    slot = port_find(port);
    if (slot >= 0) {
        for (i = 0; i < KS_PASSOC_MAX; i++) {
            if (s_passoc[i].file == file) { free_i = i; break; }
            if (!s_passoc[i].file && free_i < 0) free_i = i;
        }
        if (free_i >= 0) {
            s_passoc[free_i].file = file;
            s_passoc[free_i].port = slot;
            s_passoc[free_i].gen = s_ports[slot].gen;
            s_passoc[free_i].key = key;
            r = 0;
        }
    }
    ks_unlock();
    return r;
}

int ks_port_post_for_file(void *file, uint32_t ctx, uint32_t status, uint32_t info)
{
    int i, r = 0;
    if (!file)
        return 0;
    ks_lock();
    for (i = 0; i < KS_PASSOC_MAX; i++)
        if (s_passoc[i].file == file) {
            int slot = s_passoc[i].port;
            if (s_ports[slot].ident && s_ports[slot].gen == s_passoc[i].gen)
                r = port_post_locked(slot, s_passoc[i].key, ctx, status, info) == 0;
            break;
        }
    ks_unlock();
    if (r)
        ks_wake_all();
    return r;
}

void ks_port_forget(void *handle)
{
    int i, slot;
    if (!handle)
        return;
    ks_lock();
    for (i = 0; i < KS_PASSOC_MAX; i++)
        if (s_passoc[i].file == handle)
            s_passoc[i].file = NULL;
    slot = port_find(handle);
    if (slot >= 0) {
        ks_port *p = &s_ports[slot];
        while (p->head) {
            ks_pkt *k = p->head;
            p->head = k->next;
            free(k);
        }
        p->tail = NULL;
        p->depth = 0;
        p->ident = NULL;
        p->gen = ++s_port_gen;
        for (i = 0; i < KS_PASSOC_MAX; i++)
            if (s_passoc[i].file && s_passoc[i].port == slot)
                s_passoc[i].file = NULL;
    }
    ks_unlock();
    if (slot >= 0)
        ks_wake_all();
}

/* Save states (XBA-246, kernel_ksync.h). */
int ks_port_list(ks_port_info *out, int max)
{
    int i, n = 0;
    ks_lock();
    for (i = 0; i < KS_PORT_MAX; i++) {
        if (!s_ports[i].ident)
            continue;
        if (out && n < max) {
            out[n].ident = s_ports[i].ident;
            out[n].slot = i;
            out[n].gen = s_ports[i].gen;
            out[n].max = s_ports[i].max;
            out[n].cur = s_ports[i].cur;
            out[n].depth = s_ports[i].depth;
        }
        n++;
    }
    ks_unlock();
    return n;
}

int ks_port_packets(void *ident, ks_packet *out, int max)
{
    int slot, n = 0;
    ks_pkt *k;
    ks_lock();
    slot = port_find(ident);
    if (slot >= 0)
        for (k = s_ports[slot].head; k; k = k->next, n++)
            if (out && n < max) {
                out[n].key = k->key; out[n].ctx = k->ctx;
                out[n].status = k->status; out[n].info = k->info;
            }
    ks_unlock();
    return n;
}

int ks_port_restore(const ks_port_info *p, const ks_packet *pk, int n)
{
    ks_port *q;
    int i;
    if (!p || !p->ident || p->slot < 0 || p->slot >= KS_PORT_MAX)
        return 0;
    ks_lock();
    q = &s_ports[p->slot];
    if (q->ident || port_find(p->ident) >= 0) {
        ks_unlock();
        return 0;
    }
    memset(q, 0, sizeof *q);
    q->ident = p->ident;
    q->gen = p->gen;
    if (s_port_gen < p->gen)
        s_port_gen = p->gen;
    q->max = p->max ? p->max : 1;
    q->cur = p->cur;
    for (i = 0; i < n; i++) {
        ks_pkt *k = (ks_pkt *)malloc(sizeof *k);
        if (!k)
            break;
        k->next = NULL;
        k->key = pk[i].key; k->ctx = pk[i].ctx;
        k->status = pk[i].status; k->info = pk[i].info;
        if (q->tail) q->tail->next = k; else q->head = k;
        q->tail = k;
        q->depth++;
    }
    if (q->depth) SetEvent((HANDLE)q->ident); else ResetEvent((HANDLE)q->ident);
    ks_unlock();
    ks_wake_all();
    return 1;
}

int ks_passoc_list(ks_passoc_info *out, int max)
{
    int i, n = 0;
    ks_lock();
    for (i = 0; i < KS_PASSOC_MAX; i++) {
        int slot = s_passoc[i].port;
        if (!s_passoc[i].file || !s_ports[slot].ident || s_ports[slot].gen != s_passoc[i].gen)
            continue;
        if (out && n < max) {
            out[n].file = s_passoc[i].file;
            out[n].port = s_ports[slot].ident;
            out[n].key = s_passoc[i].key;
        }
        n++;
    }
    ks_unlock();
    return n;
}

void ks_port_reset(void)
{
    int i;
    ks_lock();
    for (i = 0; i < KS_PORT_MAX; i++) {
        ks_port *p = &s_ports[i];
        while (p->head) {
            ks_pkt *k = p->head;
            p->head = k->next;
            free(k);
        }
        if (p->ident)
            p->gen = ++s_port_gen;
        p->tail = NULL;
        p->depth = 0;
        p->ident = NULL;
    }
    memset(s_passoc, 0, sizeof s_passoc);
    ks_unlock();
    ks_wake_all();
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
long ks_apc_pending_get(void) { return (long)s_apc_pending; }
void ks_apc_pending_set(long n) { InterlockedExchange(&s_apc_pending, (LONG)n); }

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
    /* Unlocked first look: an APC queued for some other thread must not cost
     * every kernel call of this one the lock. One inserted meanwhile is seen
     * at the next call. */
    if (M32(h) == 0 || M32(h) == h)
        return 0;
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
