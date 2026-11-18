/* kobjects -- a C test title for the kernel objects of XBA-296, built with devkitPPC like atomics. Lifted code against
 * the HLE: an SList shared by threads that use the kernel's calls and threads that pop inline with ldarx / stdcx., a
 * read-write lock, a queue and an I/O completion port served by several threads, a DPC, an APC and a periodic timer.
 * The totals must be exact whether the threads run cooperatively (x360_run) or in parallel (x360_run --host-threads).
 * kobjects.txt is what it prints through DbgPrint. */
#include "../kernel.h"

typedef unsigned long long u64;

#define NODES 256
#define SL_ROUNDS 20000
#define SL_KERNEL 3
#define SL_INLINE 2
#define RW_THREADS 4
#define RW_ROUNDS 3000
#define Q_THREADS 3
#define Q_ITEMS 300

static LARGE_TIME forever = {-1, 0x80000000u}; /* relative, a long time */

/* ---- SList ---- */

typedef struct node {
    struct node *next;
    u32 pad;
} node;

static volatile u64 sl_head __attribute__((aligned(8)));
static node nodes[NODES] __attribute__((aligned(8)));

static node *pop_inline(void)
{
    for (;;) {
        u64 old = __sync_val_compare_and_swap(&sl_head, 0ull, 0ull);
        node *e = (node *)(u32)(old >> 32);
        if (!e)
            return 0;
        u64 next = (u64)(u32)e->next << 32 | ((old - 0x10000u) & 0xFFFF0000u) | (old & 0xFFFFu);
        if (__sync_bool_compare_and_swap(&sl_head, old, next))
            return e;
    }
}

static u32 sl_kernel(void *arg)
{
    (void)arg;
    for (int k = 0; k < SL_ROUNDS; k++) {
        node *e = InterlockedPopEntrySList((void *)&sl_head);
        if (e)
            InterlockedPushEntrySList((void *)&sl_head, e);
    }
    return 0;
}

static u32 sl_inline(void *arg)
{
    (void)arg;
    for (int k = 0; k < SL_ROUNDS; k++) {
        node *e = pop_inline();
        if (e)
            InterlockedPushEntrySList((void *)&sl_head, e);
    }
    return 0;
}

static void test_slist(void)
{
    HANDLE th[SL_KERNEL + SL_INLINE];
    static u8 seen[NODES];
    for (int k = 0; k < NODES; k++)
        InterlockedPushEntrySList((void *)&sl_head, &nodes[k]);
    for (int k = 0; k < SL_KERNEL + SL_INLINE; k++)
        ExCreateThread(&th[k], 0, 0, 0, k < SL_KERNEL ? sl_kernel : sl_inline, 0, 0);
    NtWaitForMultipleObjectsEx(SL_KERNEL + SL_INLINE, th, 0, 1, 0, 0);
    u32 count = 0, twice = 0;
    node *e;
    while ((e = InterlockedPopEntrySList((void *)&sl_head)) != 0) {
        u32 at = (u32)(e - nodes);
        twice += seen[at]++ != 0;
        count++;
    }
    DbgPrint("slist: %u nodes, %u twice, depth %u\n", count, twice, (u32)(sl_head >> 16) & 0xFFFF);
    for (int k = 0; k < SL_KERNEL + SL_INLINE; k++)
        NtClose(th[k]);
}

/* ---- read-write lock ---- */

static u32 rw_lock[14];
static volatile u32 rw_a, rw_b, rw_torn;

static u32 rw_worker(void *arg)
{
    u32 me = (u32)arg;
    for (u32 k = 0; k < RW_ROUNDS; k++) {
        if ((k + me) % 4 == 0) {
            ExAcquireReadWriteLockExclusive(rw_lock);
            rw_a = rw_a + 1;
            rw_b = rw_b + 1;
            ExReleaseReadWriteLock(rw_lock);
        } else {
            ExAcquireReadWriteLockShared(rw_lock);
            if (rw_a != rw_b)
                __sync_fetch_and_add(&rw_torn, 1);
            ExReleaseReadWriteLock(rw_lock);
        }
    }
    return 0;
}

static void test_rwlock(void)
{
    HANDLE th[RW_THREADS];
    ExInitializeReadWriteLock(rw_lock);
    for (u32 k = 0; k < RW_THREADS; k++)
        ExCreateThread(&th[k], 0, 0, 0, rw_worker, (void *)k, 0);
    NtWaitForMultipleObjectsEx(RW_THREADS, th, 0, 1, 0, 0);
    DbgPrint("rwlock: %u writes, %u torn reads\n", rw_a, rw_torn);
    for (int k = 0; k < RW_THREADS; k++)
        NtClose(th[k]);
}

/* ---- queue and I/O completion port ---- */

static u32 queue[10];
static u32 entries[Q_ITEMS + Q_THREADS][2];
static volatile u32 q_sum, q_got;

static u32 q_worker(void *arg)
{
    (void)arg;
    for (;;) {
        u32 *e = KeRemoveQueue(queue, 1, 0);
        u32 at = (u32)(e - entries[0]) / 2;
        if (at >= Q_ITEMS)
            return 0;
        __sync_fetch_and_add(&q_sum, at + 1);
        __sync_fetch_and_add(&q_got, 1);
    }
}

static HANDLE port;
static volatile u32 p_sum, p_got;

static u32 p_worker(void *arg)
{
    (void)arg;
    for (;;) {
        u32 key = 0, ctx = 0, iosb[2];
        NtRemoveIoCompletion(port, &key, &ctx, iosb, 0);
        if (!key)
            return 0;
        __sync_fetch_and_add(&p_sum, key + ctx + iosb[0] + iosb[1]);
        __sync_fetch_and_add(&p_got, 1);
    }
}

static void test_queues(void)
{
    HANDLE th[Q_THREADS];
    KeInitializeQueue(queue, Q_THREADS);
    for (int k = 0; k < Q_THREADS; k++)
        ExCreateThread(&th[k], 0, 0, 0, q_worker, 0, 0);
    for (int k = 0; k < Q_ITEMS + Q_THREADS; k++)
        KeInsertQueue(queue, entries[k]);
    NtWaitForMultipleObjectsEx(Q_THREADS, th, 0, 1, 0, 0);
    DbgPrint("queue: %u entries, sum %u\n", q_got, q_sum);
    for (int k = 0; k < Q_THREADS; k++)
        NtClose(th[k]);

    NtCreateIoCompletion(&port, 0, 0, 2);
    for (int k = 0; k < Q_THREADS; k++)
        ExCreateThread(&th[k], 0, 0, 0, p_worker, 0, 0);
    for (u32 k = 1; k <= Q_ITEMS; k++)
        NtSetIoCompletion(port, k, 1, 2, 3);
    for (int k = 0; k < Q_THREADS; k++)
        NtSetIoCompletion(port, 0, 0, 0, 0);
    NtWaitForMultipleObjectsEx(Q_THREADS, th, 0, 1, 0, 0);
    DbgPrint("port: %u packets, sum %u\n", p_got, p_sum);
    for (int k = 0; k < Q_THREADS; k++)
        NtClose(th[k]);
    NtClose(port);
}

/* ---- DPC, APC, timer ---- */

static u32 dpc[8], done_event[4];
static volatile u32 dpc_seen;

static void dpc_routine(void *d, void *ctx, void *a1, void *a2)
{
    dpc_seen = (d == (void *)dpc) + (u32)ctx + (u32)a1 + (u32)a2;
    KeSetEvent(done_event, 0, 0);
}

static volatile u32 apc_seen;

static void apc_routine(void *ctx, void *a1, void *a2)
{
    apc_seen = (u32)ctx + (u32)a1 + (u32)a2;
}

static volatile u32 apc_status;

static u32 apc_thread(void *arg)
{
    (void)arg;
    apc_status = (u32)KeDelayExecutionThread(1, 1, &forever); /* STATUS_USER_APC: 0xC0 */
    return 0;
}

static void test_deferred(void)
{
    KeInitializeEvent(done_event, 1, 0);
    KeInitializeDpc(dpc, dpc_routine, (void *)10);
    u32 first = KeInsertQueueDpc(dpc, (void *)200, (void *)3000);
    KeWaitForSingleObject(done_event, 0, 1, 0, 0);
    DbgPrint("dpc: inserted %u, routine saw %u\n", first, dpc_seen);

    HANDLE th;
    LARGE_TIME shortly = {-1, 0xFFFFFFFFu - 200000u};
    ExCreateThread(&th, 0, 0, 0, apc_thread, 0, 0);
    KeDelayExecutionThread(1, 0, &shortly);
    NTSTATUS st = NtQueueApcThread(th, apc_routine, (void *)1, (void *)20, (void *)300);
    NtWaitForSingleObjectEx(th, 1, 0, 0);
    DbgPrint("apc: queued %d, the delay ended with %X, routine saw %u\n", (int)st, apc_status, apc_seen);
    NtClose(th);

    HANDLE tm;
    LARGE_TIME due = {-1, 0xFFFFFFFFu - 10000u}; /* 1 ms */
    NtCreateTimer(&tm, 0, 1);
    NtSetTimerEx(tm, &due, 0, 1, 0, 0, 1, 0);
    u32 ticks = 0;
    for (int k = 0; k < 5; k++)
        ticks += NtWaitForSingleObjectEx(tm, 1, 0, 0) == 0;
    NtCancelTimer(tm, 0);
    DbgPrint("timer: %u periods\n", ticks);
    NtClose(tm);
}

int title_main(void)
{
    test_slist();
    test_rwlock();
    test_queues();
    test_deferred();
    return 0;
}

__attribute__((section(".text.start"))) int _start(void)
{
    return title_main();
}
