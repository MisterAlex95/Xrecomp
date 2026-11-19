/* x360_kdpc_test -- DPCs, APCs, timers, queues, SLists and read-write locks on the real scheduler (XBA-296).
 *
 * Small PowerPC routines in guest memory run under the interpreter (an empty pack): each appends a value to a guest log,
 * so the order and the virtual time of what the kernel started can be read back. The main thread sleeps; the clock
 * jumps from deadline to deadline (x360_run_until), which makes every time below exact. */
#include <pthread.h>

#include "x360_hle_util.h"

#define MS 10000ull /* virtual 100 ns units */

static uint32_t code, log_addr;
static uint32_t r_log;      /* log(r3) */
static uint32_t r_log_r4;   /* log(r4): a DPC routine logging its context */
static uint32_t r_log_r5;   /* log(r5): a DPC routine logging its first system argument */
static uint32_t r_kernel;   /* an APC kernel routine: adds 1 to the normal context */
static uint32_t r_kcancel;  /* an APC kernel routine: clears the normal routine */

static uint32_t emit_log(uint32_t at, uint32_t first)
{
    uint32_t hi = log_addr >> 16, lo = log_addr & 0xFFFF;
    const uint32_t body[] = {first, 0x3D600000u | hi, 0x616B0000u | lo, 0x818B0000u, 0x7C6B612Eu, 0x398C0004u,
                             0x918B0000u, 0x4E800020u};
    for (unsigned k = 0; k < sizeof body / sizeof body[0]; k++)
        x360_w32(X, at + 4 * k, body[k]);
    return at;
}

static void machine(void)
{
    if (X)
        x360_destroy(X);
    X = NULL;
    if (hle_util_start())
        exit(1);
    static FILE *quiet;
    if (!quiet)
        quiet = fopen("/dev/null", "w");
    X->log = quiet; /* the interpreter names each routine it meets */
    code = x360_heap_alloc(X, &X->heap_v4k, 0x1000, 0);
    log_addr = x360_heap_alloc(X, &X->heap_v4k, 0x1000, 0);
    x360_w32(X, log_addr, 4);
    r_log = emit_log(code, 0x60000000u);          /* nop */
    r_log_r4 = emit_log(code + 0x40, 0x7C832378u); /* mr r3, r4 */
    r_log_r5 = emit_log(code + 0x80, 0x7CA32B78u); /* mr r3, r5 */
    r_kernel = code + 0xC0;
    static const uint32_t kr[] = {0x81650000u, 0x396B0001u, 0x91650000u, 0x4E800020u};
    for (unsigned k = 0; k < 4; k++)
        x360_w32(X, r_kernel + 4 * k, kr[k]);
    r_kcancel = code + 0xE0;
    static const uint32_t kc[] = {0x39600000u, 0x91640000u, 0x4E800020u}; /* li r11, 0; stw r11, 0(r4); blr */
    for (unsigned k = 0; k < 3; k++)
        x360_w32(X, r_kcancel + 4 * k, kc[k]);
    X->image_base = code;
    X->image_size = 0x1000;
    x360_pack pack;
    memset(&pack, 0, sizeof pack);
    CHECK(x360_bind(X, &pack) == 0, "the empty pack");
    /* the main thread sleeps for the whole test */
    uint32_t iv = galloc(8);
    x360_w64(X, iv, (uint64_t) - (int64_t)(3600 * 1000 * MS));
    K(X360_xboxkrnl_KeDelayExecutionThread, 3, 1, 0, iv);
}

static unsigned log_n(void) { return (x360_r32(X, log_addr) - 4) / 4; }
static uint32_t log_at(unsigned k) { return x360_r32(X, log_addr + 4 + 4 * k); }

static void log_clear(void) { x360_w32(X, log_addr, 4); }

/* the log as text, to compare in one go */
static const char *log_text(void)
{
    static char buf[256];
    size_t o = 0;
    buf[0] = 0;
    for (unsigned k = 0; k < log_n() && o + 12 < sizeof buf; k++)
        o += (size_t)snprintf(buf + o, sizeof buf - o, "%s%X", k ? " " : "", log_at(k));
    return buf;
}

#define LOG_IS(want) CHECK(!strcmp(log_text(), want), "log \"%s\", expected \"%s\"", log_text(), want)

static void run_ms(uint64_t ms)
{
    int r = x360_run_until(X, x360_time(X) + ms * MS);
    CHECK(r == X360_FRAME, "the run ended with %d: %s", r, x360_error(X));
}

/* a thread that logs r3 (the wait's result) and ends once its wait is over: call as `on(t), wait..., off()` */
static x360_thread *worker(void)
{
    x360_thread *t = x360_thread_create(X, 0x4000, r_log, 0, 0, 0);
    CHECK(t != NULL, "a worker thread");
    return t;
}

static void on(x360_thread *t)
{
    X->cur = (int)(t - X->threads);
    X->m.cur = &t->cpu;
}

static void off(void)
{
    X->cur = 0;
    X->m.cur = &X->threads[0].cpu;
}

static uint32_t timeout_of(int64_t v)
{
    uint32_t a = galloc(8);
    x360_w64(X, a, (uint64_t)v);
    return a;
}

static uint32_t new_event(int sync)
{
    uint32_t e = galloc(16);
    K(X360_xboxkrnl_KeInitializeEvent, 3, e, sync, 0);
    return e;
}

static uint32_t new_dpc(uint32_t routine, uint32_t ctx)
{
    uint32_t d = galloc(0x20);
    K(X360_xboxkrnl_KeInitializeDpc, 3, d, routine, ctx);
    return d;
}

static uint32_t new_timer(int sync)
{
    uint32_t o = galloc(0x28);
    K(X360_xboxkrnl_KeInitializeTimerEx, 2, o, sync);
    return o;
}

/* KeSetTimerEx's due time is a 64-bit value in one register */
static uint32_t set_timer(uint32_t timer, int64_t due, uint32_t period, uint32_t dpc)
{
    xe_cpu *c = cpu();
    c->gpr[1] = stack_top;
    c->gpr[3] = timer;
    c->gpr[4] = (uint64_t)due;
    c->gpr[5] = period;
    c->gpr[6] = dpc;
    x360_thunk th = {0, X360_xboxkrnl_KeSetTimerEx, X360_LIB_XBOXKRNL};
    x360_hle_call(X, &th);
    return (uint32_t)c->gpr[3];
}

/* ---- DPCs ---- */

static void test_dpc(void)
{
    machine();
    uint32_t a = new_dpc(r_log_r4, 0xA), b = new_dpc(r_log_r4, 0xB), d = new_dpc(r_log_r5, 0xD);
    CHECK(x360_r16(X, a) == 19 && x360_r32(X, a + 0xC) == r_log_r4 && x360_r32(X, a + 0x10) == 0xA, "KDPC layout");
    CHECK(K(X360_xboxkrnl_KeInsertQueueDpc, 3, a, 1, 2) == 1, "inserted");
    CHECK(K(X360_xboxkrnl_KeInsertQueueDpc, 3, a, 3, 4) == 0, "already queued");
    CHECK(x360_r32(X, a + 0x14) == 1 && x360_r32(X, a + 0x18) == 2, "the first arguments stay");
    CHECK(K(X360_xboxkrnl_KeInsertQueueDpc, 3, b, 0, 0) == 1, "second");
    CHECK(K(X360_xboxkrnl_KeInsertQueueDpc, 3, d, 0x77, 0) == 1, "third");
    CHECK(K(X360_xboxkrnl_KeRemoveQueueDpc, 1, b) == 1, "removed from the middle");
    CHECK(K(X360_xboxkrnl_KeRemoveQueueDpc, 1, b) == 0, "no longer queued");
    run_ms(1);
    LOG_IS("A 77"); /* in queue order, with the context and the system argument */
    CHECK(K(X360_xboxkrnl_KeInsertQueueDpc, 3, a, 0, 0) == 1, "queued again once it ran");
    run_ms(1);
    LOG_IS("A 77 A");
}

/* ---- timers in virtual time ---- */

static void test_timers(void)
{
    machine();
    uint32_t tm = new_timer(0), dpc = new_dpc(r_log_r4, 0x11);
    CHECK(x360_r8(X, tm) == X_OBJ_TIMER_NOTIFICATION, "notification timer");
    CHECK(set_timer(tm, -(int64_t)(10 * MS), 0, dpc) == 0, "not armed before");
    CHECK(x360_r32(X, tm + 0x20) == dpc, "KTIMER: the DPC");
    run_ms(9);
    CHECK(log_n() == 0 && x360_r32(X, tm + 4) == 0, "not due yet");
    run_ms(2);
    LOG_IS("11");
    CHECK(x360_r32(X, tm + 4) == 1, "signalled");
    run_ms(50);
    CHECK(log_n() == 1, "a one-shot timer fires once");

    /* periodic: 5 ms, then every 10 ms */
    log_clear();
    uint32_t per = new_timer(1);
    set_timer(per, -(int64_t)(5 * MS), 10, dpc);
    run_ms(4);
    CHECK(log_n() == 0, "periodic: before the first expiry");
    run_ms(2);
    CHECK(log_n() == 1, "periodic: 1 after 6 ms, got %u", log_n());
    run_ms(10);
    CHECK(log_n() == 2, "periodic: 2 after 16 ms, got %u", log_n());
    run_ms(20);
    CHECK(log_n() == 4, "periodic: 4 after 36 ms, got %u", log_n());
    CHECK(set_timer(per, -(int64_t)(1000 * MS), 0, 0) == 1, "it was armed");
    CHECK(K(X360_xboxkrnl_KeCancelTimer, 1, per) == 1, "cancelled while armed");
    CHECK(K(X360_xboxkrnl_KeCancelTimer, 1, per) == 0, "cancelled twice");
    run_ms(2000);
    CHECK(log_n() == 4, "a cancelled timer stays silent, got %u", log_n());

    /* two timers armed out of order fire by their due times */
    log_clear();
    uint32_t t1 = new_timer(0), t2 = new_timer(0);
    set_timer(t1, -(int64_t)(8 * MS), 0, new_dpc(r_log_r4, 0x81));
    set_timer(t2, -(int64_t)(3 * MS), 0, new_dpc(r_log_r4, 0x32));
    run_ms(20);
    LOG_IS("32 81");

    /* an absolute due time is a system time */
    log_clear();
    uint32_t t3 = new_timer(0);
    set_timer(t3, (int64_t)(X360_EPOCH + x360_time(X) + 4 * MS), 0, new_dpc(r_log_r4, 0x44));
    run_ms(3);
    CHECK(log_n() == 0, "absolute: not due yet");
    run_ms(2);
    LOG_IS("44");

    /* a synchronization timer wakes one waiter and resets */
    log_clear();
    uint32_t st = new_timer(1);
    x360_thread *w = worker();
    on(w);
    K(X360_xboxkrnl_KeWaitForSingleObject, 5, st, 0, 1, 0, 0);
    off();
    set_timer(st, -(int64_t)(2 * MS), 0, 0);
    run_ms(5);
    LOG_IS("0");
    CHECK(x360_r32(X, st + 4) == 0, "the waiter took the signal");
}

static void test_nt_timer_apc(void)
{
    machine();
    uint32_t hp = galloc(8), iv = timeout_of(-(int64_t)(2 * MS));
    CHECK(K(X360_xboxkrnl_NtCreateTimer, 3, hp, 0, 0) == 0, "NtCreateTimer");
    uint32_t h = x360_r32(X, hp);
    x360_thread *w = worker();
    on(w);
    uint32_t forever = timeout_of(-(int64_t)(1000 * MS));
    CHECK(K(X360_xboxkrnl_NtSetTimerEx, 8, h, iv, r_log, 1, 0x5A, 0, 0, 0) == 0, "NtSetTimerEx");
    K(X360_xboxkrnl_KeDelayExecutionThread, 3, 1, 1, forever);
    off();
    run_ms(5);
    LOG_IS("5A C0"); /* the routine gets its context, then the alertable wait ends with STATUS_USER_APC */
}

/* ---- APCs ---- */

static void test_apc(void)
{
    machine();
    uint32_t ev = new_event(0), hp = galloc(8);
    x360_thread *w = worker();
    uint32_t forever = timeout_of(-(int64_t)(1000 * MS));

    /* a user APC waits for an alertable wait */
    on(w);
    K(X360_xboxkrnl_KeWaitForSingleObject, 5, ev, 0, 1, 0, forever); /* not alertable */
    off();
    CHECK(K(X360_xboxkrnl_NtDuplicateObject, 3, w->handle, hp, 0) == 0, "a handle on the thread");
    uint32_t th = x360_r32(X, hp);
    CHECK(K(X360_xboxkrnl_NtQueueApcThread, 5, th, r_log, 0x21, 0, 0) == 0, "first");
    CHECK(K(X360_xboxkrnl_NtQueueApcThread, 5, th, r_log, 0x22, 0, 0) == 0, "second");
    CHECK(K(X360_xboxkrnl_NtQueueApcThread, 5, 0xF8FFFFFCu, r_log, 0, 0, 0) == X_STATUS_INVALID_HANDLE, "bad handle");
    run_ms(5);
    CHECK(log_n() == 0 && w->state == TH_WAIT, "a non-alertable wait does not run user APCs");
    K(X360_xboxkrnl_KeSetEvent, 3, ev, 0, 0);
    run_ms(1);
    LOG_IS("0"); /* the wait ended by its object: the worker's log routine ran with the status */
    CHECK(w->state == TH_DONE && w->apc_head == 0, "the thread ended with its APCs undelivered, and they are dropped");

    /* a new thread: queued first, then an alertable wait delivers them in order and returns STATUS_USER_APC */
    machine();
    w = worker();
    forever = timeout_of(-(int64_t)(1000 * MS));
    hp = galloc(8);
    K(X360_xboxkrnl_NtDuplicateObject, 3, w->handle, hp, 0);
    th = x360_r32(X, hp);
    K(X360_xboxkrnl_NtQueueApcThread, 5, th, r_log, 0x31, 0, 0);
    K(X360_xboxkrnl_NtQueueApcThread, 5, th, r_log, 0x32, 0, 0);
    on(w);
    K(X360_xboxkrnl_KeDelayExecutionThread, 3, 1, 1, forever);
    off();
    run_ms(1);
    LOG_IS("31 32 C0");

    /* a kernel APC: its kernel routine changes the context, it breaks into a non-alertable wait, which goes on */
    machine();
    ev = new_event(0);
    w = worker();
    forever = timeout_of(-(int64_t)(1000 * MS));
    on(w);
    K(X360_xboxkrnl_KeWaitForSingleObject, 5, ev, 0, 1, 0, forever);
    off();
    uint32_t apc = galloc(0x28);
    K(X360_xboxkrnl_KeInitializeApc, 7, apc, w->kthread, r_kernel, 0, r_log, 0, 0x40);
    CHECK(x360_r8(X, apc) == 18 && x360_r32(X, apc + 4) == w->kthread, "KAPC layout");
    CHECK(K(X360_xboxkrnl_KeInsertQueueApc, 4, apc, 0, 0, 0) == 1, "inserted");
    CHECK(K(X360_xboxkrnl_KeInsertQueueApc, 4, apc, 0, 0, 0) == 0, "already queued");
    run_ms(1);
    LOG_IS("41"); /* 0x40 + 1 by the kernel routine */
    CHECK(w->state == TH_WAIT, "the wait goes on after a kernel APC");
    CHECK(x360_r8(X, apc + 3) == 0, "no longer enqueued");
    K(X360_xboxkrnl_KeSetEvent, 3, ev, 0, 0);
    run_ms(1);
    LOG_IS("41 0");

    /* a kernel routine can cancel the normal routine; removal before delivery */
    machine();
    w = worker();
    forever = timeout_of(-(int64_t)(1000 * MS));
    apc = galloc(0x28);
    uint32_t gone = galloc(0x28);
    K(X360_xboxkrnl_KeInitializeApc, 7, apc, w->kthread, r_kcancel, 0, r_log, 1, 0x50);
    K(X360_xboxkrnl_KeInitializeApc, 7, gone, w->kthread, 0, 0, r_log, 1, 0x60);
    K(X360_xboxkrnl_KeInsertQueueApc, 4, gone, 0, 0, 0);
    K(X360_xboxkrnl_KeInsertQueueApc, 4, apc, 0, 0, 0);
    CHECK(K(X360_xboxkrnl_KeRemoveQueueApc, 1, gone) == 1, "removed");
    CHECK(K(X360_xboxkrnl_KeRemoveQueueApc, 1, gone) == 0, "removed twice");
    on(w);
    K(X360_xboxkrnl_KeDelayExecutionThread, 3, 1, 1, forever);
    off();
    run_ms(1);
    LOG_IS("C0");

    /* an alert ends an alertable wait */
    machine();
    w = worker();
    forever = timeout_of(-(int64_t)(1000 * MS));
    on(w);
    K(X360_xboxkrnl_KeDelayExecutionThread, 3, 1, 1, forever);
    off();
    CHECK(K(X360_xboxkrnl_KeAlertThread, 2, w->kthread, 1) == 0, "not alerted before");
    run_ms(1);
    LOG_IS("101");

    /* a critical region holds a kernel APC back until it is left */
    machine();
    w = worker();
    on(w);
    K(X360_xboxkrnl_KeEnterCriticalRegion, 0);
    uint32_t held = galloc(0x28);
    K(X360_xboxkrnl_KeInitializeApc, 7, held, w->kthread, 0, 0, r_log, 0, 0x70);
    K(X360_xboxkrnl_KeInsertQueueApc, 4, held, 0, 0, 0);
    CHECK(w->apc_stage == 0, "held back in a critical region");
    w->cpu.pc = r_log; /* where the thread goes on afterwards */
    w->cpu.lr = X360_THREAD_EXIT;
    K(X360_xboxkrnl_KeLeaveCriticalRegion, 0);
    CHECK(w->apc_stage != 0 && w->cpu.pc == r_log && w->cpu.lr == X360_APC_RETURN, "delivered on leaving the region");
    off();
    run_ms(1);
    CHECK(log_n() == 2 && log_at(0) == 0x70, "the routine ran first: \"%s\"", log_text());
}

/* ---- queues ---- */

static void test_queue(void)
{
    machine();
    uint32_t q = galloc(0x28), e1 = galloc(8), e2 = galloc(8), e3 = galloc(8);
    K(X360_xboxkrnl_KeInitializeQueue, 2, q, 2);
    CHECK(x360_r8(X, q) == X_OBJ_QUEUE && x360_r32(X, q + 0x1C) == 2, "KQUEUE");
    CHECK(K(X360_xboxkrnl_KeInsertQueue, 2, q, e1) == 0, "was empty");
    CHECK(K(X360_xboxkrnl_KeInsertQueue, 2, q, e2) == 1, "one entry before");
    CHECK(K(X360_xboxkrnl_KeInsertHeadQueue, 2, q, e3) == 2, "two entries before");
    CHECK(x360_r32(X, q + 4) == 3, "signal state: the entries");
    CHECK(K(X360_xboxkrnl_KeRemoveQueue, 3, q, 0, 0) == e3, "the head insert comes first");
    CHECK(K(X360_xboxkrnl_KeRemoveQueue, 3, q, 0, 0) == e1, "then in order");
    CHECK(x360_r32(X, q + 0x18) == 1, "the caller counts once, however often it takes");
    uint32_t first = K(X360_xboxkrnl_KeRundownQueue, 1, q);
    CHECK(first == e2 && x360_r32(X, e2) == e2 && x360_r32(X, q + 4) == 0, "rundown: the rest, as a ring");
    CHECK(K(X360_xboxkrnl_KeRundownQueue, 1, q) == 0, "rundown of an empty queue");

    /* waiters are served last in, first out */
    machine();
    q = galloc(0x28);
    K(X360_xboxkrnl_KeInitializeQueue, 2, q, 4);
    x360_thread *a = worker(), *b = worker(), *d = worker();
    on(a);
    K(X360_xboxkrnl_KeRemoveQueue, 3, q, 0, 0);
    on(b);
    K(X360_xboxkrnl_KeRemoveQueue, 3, q, 0, 0);
    on(d);
    K(X360_xboxkrnl_KeRemoveQueue, 3, q, 0, 0);
    off();
    uint32_t e = galloc(8);
    K(X360_xboxkrnl_KeInsertQueue, 2, q, e);
    run_ms(1);
    CHECK(d->state == TH_DONE && a->state == TH_WAIT && b->state == TH_WAIT, "the last waiter got it");
    CHECK(log_n() == 1 && log_at(0) == e, "its result is the entry");
    uint32_t e4 = galloc(8);
    K(X360_xboxkrnl_KeInsertQueue, 2, q, e4);
    run_ms(1);
    CHECK(b->state == TH_DONE && a->state == TH_WAIT, "then the one before it");

    /* the concurrency limit: one active thread, the next entry waits until it blocks elsewhere */
    machine();
    q = galloc(0x28);
    K(X360_xboxkrnl_KeInitializeQueue, 2, q, 1);
    uint32_t ev = new_event(0);
    a = worker();
    b = worker();
    on(a);
    K(X360_xboxkrnl_KeInsertQueue, 2, q, galloc(8));
    CHECK(K(X360_xboxkrnl_KeRemoveQueue, 3, q, 0, 0) != 0, "a takes the entry");
    a->state = TH_SUSPENDED; /* it stays the active thread, without running to its end */
    on(b);
    K(X360_xboxkrnl_KeRemoveQueue, 3, q, 0, 0);
    off();
    uint32_t e5 = galloc(8);
    K(X360_xboxkrnl_KeInsertQueue, 2, q, e5);
    run_ms(1);
    CHECK(b->state == TH_WAIT && x360_r32(X, q + 4) == 1, "over the limit: the entry stays queued");
    a->state = TH_READY;
    on(a);
    K(X360_xboxkrnl_KeWaitForSingleObject, 5, ev, 0, 1, 0, 0); /* a blocks: b may run */
    off();
    run_ms(1);
    CHECK(b->state == TH_DONE && log_n() == 1 && log_at(0) == e5, "released when the active thread blocked");
    CHECK(x360_r32(X, q + 0x18) == 0, "b ended: current count back to 0");
    K(X360_xboxkrnl_KeSetEvent, 3, ev, 0, 0);
    run_ms(1);
    CHECK(x360_r32(X, q + 0x18) == 0 && a->state == TH_DONE, "a came back and ended: count %u",
          x360_r32(X, q + 0x18));

    /* a timeout gives STATUS_TIMEOUT as the result */
    machine();
    q = galloc(0x28);
    K(X360_xboxkrnl_KeInitializeQueue, 2, q, 0);
    CHECK(x360_r32(X, q + 0x1C) == 6, "count 0: one per hardware thread");
    a = worker();
    on(a);
    K(X360_xboxkrnl_KeRemoveQueue, 3, q, 0, timeout_of(-(int64_t)(2 * MS)));
    off();
    run_ms(1);
    CHECK(a->state == TH_WAIT, "still waiting");
    run_ms(2);
    LOG_IS("102");
}

static void test_iocp(void)
{
    machine();
    uint32_t hp = galloc(8), key = galloc(4), ctx = galloc(4), iosb = galloc(8), info = galloc(4), ret = galloc(4);
    CHECK(K(X360_xboxkrnl_NtCreateIoCompletion, 4, hp, 0, 0, 2) == 0, "NtCreateIoCompletion");
    uint32_t h = x360_r32(X, hp);
    CHECK(K(X360_xboxkrnl_NtSetIoCompletion, 5, h, 0x11, 0x22, 0x33, 0x44) == 0, "a packet");
    CHECK(K(X360_xboxkrnl_NtSetIoCompletion, 5, h, 0x55, 0x66, 0x77, 0x88) == 0, "another");
    CHECK(K(X360_xboxkrnl_NtQueryIoCompletion, 5, h, 0, info, 4, ret) == 0 && x360_r32(X, info) == 2, "depth 2");
    CHECK(K(X360_xboxkrnl_NtQueryIoCompletion, 5, h, 1, info, 4, ret) == X_STATUS_INVALID_PARAMETER, "bad class");
    CHECK(K(X360_xboxkrnl_NtRemoveIoCompletion, 5, h, key, ctx, iosb, 0) == 0, "removed");
    CHECK(x360_r32(X, key) == 0x11 && x360_r32(X, ctx) == 0x22 && x360_r32(X, iosb) == 0x33 &&
              x360_r32(X, iosb + 4) == 0x44, "the first packet");
    CHECK(K(X360_xboxkrnl_NtSetIoCompletion, 5, 0xF8FFFFFCu, 0, 0, 0, 0) == X_STATUS_INVALID_HANDLE, "bad handle");

    /* a thread waits for a packet that comes later, written out when it wakes */
    x360_thread *w = worker();
    uint32_t k2 = galloc(4), c2 = galloc(4), s2 = galloc(8);
    K(X360_xboxkrnl_NtRemoveIoCompletion, 5, h, k2, c2, s2, 0); /* takes the second packet: frees the slot */
    x360_w32(X, k2, 0);
    on(w);
    K(X360_xboxkrnl_NtRemoveIoCompletion, 5, h, k2, c2, s2, 0);
    off();
    CHECK(w->state == TH_WAIT, "no packet: it waits");
    K(X360_xboxkrnl_NtSetIoCompletion, 5, h, 0xAA, 0xBB, 0xCC, 0xDD);
    run_ms(1);
    CHECK(x360_r32(X, k2) == 0xAA && x360_r32(X, c2) == 0xBB && x360_r32(X, s2) == 0xCC &&
              x360_r32(X, s2 + 4) == 0xDD, "the packet written when it woke");
    LOG_IS("0");

    machine();
    hp = galloc(8);
    K(X360_xboxkrnl_NtCreateIoCompletion, 4, hp, 0, 0, 1);
    x360_thread *t = worker();
    on(t);
    K(X360_xboxkrnl_NtRemoveIoCompletion, 5, x360_r32(X, hp), 0, 0, 0, timeout_of(-(int64_t)(1 * MS)));
    off();
    run_ms(2);
    LOG_IS("102");
}

/* ---- SLists ---- */

static void test_slist(void)
{
    machine();
    uint32_t h = x360_heap_alloc(X, &X->heap_v4k, 0x1000, 0), a = h + 0x100, b = h + 0x108;
    CHECK(K(X360_xboxkrnl_InterlockedPopEntrySList, 1, h) == 0, "empty");
    CHECK(K(X360_xboxkrnl_InterlockedPushEntrySList, 2, h, a) == 0, "first push returns the old head");
    CHECK(K(X360_xboxkrnl_InterlockedPushEntrySList, 2, h, b) == a, "second push");
    CHECK(x360_r32(X, h) == b && x360_r16(X, h + 4) == 2 && x360_r16(X, h + 6) == 2, "next, depth, sequence");
    CHECK(x360_r32(X, b) == a && x360_r32(X, a) == 0, "the entries are linked");
    CHECK(K(X360_xboxkrnl_InterlockedPopEntrySList, 1, h) == b, "LIFO");
    CHECK(x360_r16(X, h + 4) == 1 && x360_r16(X, h + 6) == 2, "pop: depth down, sequence kept");
    K(X360_xboxkrnl_InterlockedPushEntrySList, 2, h, b);
    CHECK(K(X360_xboxkrnl_InterlockedFlushSList, 1, h) == b, "flush returns the chain");
    CHECK(x360_r64(X, h) == 0, "flushed");
    CHECK(K(X360_xboxkrnl_InterlockedFlushSList, 1, h) == 0, "flush of an empty list");
}

/* host threads against one guest header: the HLE pushes, the HLE and a guest lwarx-style loop pop */
enum { SL_THREADS = 4, SL_ITEMS = 20000 };
static uint32_t sl_head, sl_items;
static unsigned char *sl_seen;
static int sl_go;

static void hle_on(x360_hle_fn fn, xe_cpu *c, uint32_t a, uint32_t b)
{
    c->gpr[3] = a;
    c->gpr[4] = b;
    fn(X, NULL, c);
}

static void *sl_pusher(void *arg)
{
    unsigned n = (unsigned)(size_t)arg;
    xe_cpu c;
    memset(&c, 0, sizeof c);
    x360_hle_fn push = x360_hle_lookup(X, X360_LIB_XBOXKRNL, X360_xboxkrnl_InterlockedPushEntrySList);
    while (!__atomic_load_n(&sl_go, __ATOMIC_ACQUIRE))
        ;
    for (unsigned k = 0; k < SL_ITEMS; k++)
        hle_on(push, &c, sl_head, sl_items + 8 * (n * SL_ITEMS + k));
    return NULL;
}

static void sl_mark(uint32_t e)
{
    unsigned k = (e - sl_items) / 8;
    __atomic_add_fetch(&sl_seen[k], 1, __ATOMIC_RELAXED);
}

static int sl_done(int *dry)
{
    return ++*dry > 2000;
}

static void *sl_popper(void *arg)
{
    (void)arg;
    xe_cpu c;
    memset(&c, 0, sizeof c);
    x360_hle_fn pop = x360_hle_lookup(X, X360_LIB_XBOXKRNL, X360_xboxkrnl_InterlockedPopEntrySList);
    int dry = 0;
    while (!sl_done(&dry)) {
        hle_on(pop, &c, sl_head, 0);
        uint32_t e = (uint32_t)c.gpr[3];
        if (e)
            sl_mark(e), dry = 0;
    }
    return NULL;
}

/* what lifted code does: ldarx, the new header, stdcx. */
static void *sl_reserver(void *arg)
{
    (void)arg;
    xe_cpu c;
    xe_machine m;
    memset(&c, 0, sizeof c);
    memset(&m, 0, sizeof m);
    m.mem = X->m.mem;
    m.mem_base = X->m.mem_base;
    m.mem_size = X->m.mem_size;
    m.view4k_skew = X->m.view4k_skew;
    int dry = 0;
    while (!sl_done(&dry)) {
        uint64_t v = xe_ldarx(&m, &c, sl_head);
        uint32_t e = (uint32_t)(v >> 32);
        if (!e) {
            c.reserve = 0;
            continue;
        }
        uint64_t next = (uint64_t)x360_r32(X, e) << 32 | ((v - 0x10000u) & 0xFFFF0000u) | (v & 0xFFFF);
        xe_stcx(&m, &c, sl_head, next, 8);
        if (c.cr >> 28 & 2)
            sl_mark(e), dry = 0;
    }
    return NULL;
}

static void test_slist_threads(void)
{
    machine();
    sl_head = x360_heap_alloc(X, &X->heap_v4k, 0x1000, 0);
    sl_items = x360_heap_alloc(X, &X->heap_v4k, 8 * SL_THREADS * SL_ITEMS, 0);
    sl_seen = calloc(SL_THREADS, SL_ITEMS);
    pthread_t th[SL_THREADS + 3];
    sl_go = 0;
    for (size_t k = 0; k < SL_THREADS; k++)
        pthread_create(&th[k], NULL, sl_pusher, (void *)k);
    pthread_create(&th[SL_THREADS], NULL, sl_popper, NULL);
    pthread_create(&th[SL_THREADS + 1], NULL, sl_popper, NULL);
    pthread_create(&th[SL_THREADS + 2], NULL, sl_reserver, NULL);
    __atomic_store_n(&sl_go, 1, __ATOMIC_RELEASE);
    for (int k = 0; k < SL_THREADS + 3; k++)
        pthread_join(th[k], NULL);
    /* what the poppers left behind */
    uint32_t e;
    while ((e = K(X360_xboxkrnl_InterlockedPopEntrySList, 1, sl_head)) != 0)
        sl_mark(e);
    unsigned once = 0, other = 0;
    for (unsigned k = 0; k < SL_THREADS * SL_ITEMS; k++)
        sl_seen[k] == 1 ? once++ : other++;
    CHECK(once == SL_THREADS * SL_ITEMS && !other, "every entry popped exactly once: %u once, %u otherwise", once, other);
    CHECK(x360_r16(X, sl_head + 4) == 0, "depth 0 at the end, got %u", x360_r16(X, sl_head + 4));
    free(sl_seen);
}

/* ---- read-write locks ---- */

static void test_rwlock(void)
{
    machine();
    uint32_t l = galloc(0x38);
    K(X360_xboxkrnl_ExInitializeReadWriteLock, 1, l);
    CHECK(x360_r32(X, l) == 0xFFFFFFFFu && x360_r8(X, l + 0x10) == X_OBJ_EVENT_SYNC &&
              x360_r8(X, l + 0x20) == X_OBJ_SEMAPHORE, "ERWLOCK layout");
    /* readers share, a writer is refused */
    CHECK(K(X360_xboxkrnl_ExTryToAcquireReadWriteLockShared, 1, l) == 1, "first reader");
    CHECK(K(X360_xboxkrnl_ExTryToAcquireReadWriteLockShared, 1, l) == 1, "second reader");
    CHECK(K(X360_xboxkrnl_ExTryToAcquireReadWriteLockExclusive, 1, l) == 0, "a writer must wait");
    K(X360_xboxkrnl_ExReleaseReadWriteLock, 1, l);
    K(X360_xboxkrnl_ExReleaseReadWriteLock, 1, l);
    CHECK(x360_r32(X, l) == 0xFFFFFFFFu && x360_r32(X, l + 0xC) == 0, "free again");
    /* a writer excludes readers */
    CHECK(K(X360_xboxkrnl_ExTryToAcquireReadWriteLockExclusive, 1, l) == 1, "the writer");
    CHECK(K(X360_xboxkrnl_ExTryToAcquireReadWriteLockShared, 1, l) == 0, "no reader beside it");
    CHECK(K(X360_xboxkrnl_ExTryToAcquireReadWriteLockExclusive, 1, l) == 0, "no second writer");

    /* blocked readers go in together when the writer leaves; a writer behind them comes after */
    x360_thread *r1 = worker(), *r2 = worker(), *w = worker();
    on(r1);
    K(X360_xboxkrnl_ExAcquireReadWriteLockShared, 1, l);
    on(r2);
    K(X360_xboxkrnl_ExAcquireReadWriteLockShared, 1, l);
    off();
    run_ms(1);
    CHECK(r1->state == TH_WAIT && r2->state == TH_WAIT, "readers block behind the writer");
    K(X360_xboxkrnl_ExReleaseReadWriteLock, 1, l);
    run_ms(1);
    CHECK(r1->state == TH_DONE && r2->state == TH_DONE, "both readers released");
    CHECK(x360_r32(X, l + 0xC) == 2, "two readers inside");
    on(w);
    K(X360_xboxkrnl_ExAcquireReadWriteLockExclusive, 1, l);
    off();
    run_ms(1);
    CHECK(w->state == TH_WAIT, "the writer waits for the readers");
    CHECK(K(X360_xboxkrnl_ExTryToAcquireReadWriteLockShared, 1, l) == 0, "a waiting writer holds new readers back");
    K(X360_xboxkrnl_ExReleaseReadWriteLock, 1, l);
    run_ms(1);
    CHECK(w->state == TH_WAIT, "one reader is still inside");
    K(X360_xboxkrnl_ExReleaseReadWriteLock, 1, l);
    run_ms(1);
    CHECK(w->state == TH_DONE, "the last reader lets the writer in");
    K(X360_xboxkrnl_ExReleaseReadWriteLock, 1, l);
    CHECK(x360_r32(X, l) == 0xFFFFFFFFu, "free at the end: %08X", x360_r32(X, l));
}

/* ---- mutants, protection, IRQL ---- */

static void test_misc(void)
{
    machine();
    uint32_t m = galloc(0x20), info = galloc(16), hp = galloc(8);
    K(X360_xboxkrnl_KeInitializeMutant, 2, m, 1);
    CHECK(x360_r8(X, m) == X_OBJ_MUTANT && x360_r32(X, m + 4) == 0 && x360_r32(X, m + 0x18) == x360_cur(X)->kthread,
          "owned at creation");
    CHECK(K(X360_xboxkrnl_KeReleaseMutant, 4, m, 0, 0, 0) == 0, "previous state");
    CHECK(x360_r32(X, m + 4) == 1 && x360_r32(X, m + 0x18) == 0, "released");
    K(X360_xboxkrnl_NtCreateMutant, 3, hp, 0, 1);
    CHECK(K(X360_xboxkrnl_NtQueryMutant, 2, x360_r32(X, hp), info) == 0 && x360_r32(X, info) == 0 &&
              x360_r32(X, info + 4) == 1, "NtQueryMutant");
    K(X360_xboxkrnl_NtCreateSemaphore, 4, hp, 0, 3, 9);
    CHECK(K(X360_xboxkrnl_NtQuerySemaphore, 2, x360_r32(X, hp), info) == 0 && x360_r32(X, info) == 3 &&
              x360_r32(X, info + 4) == 9, "NtQuerySemaphore");
    CHECK(K(X360_xboxkrnl_NtQuerySemaphore, 2, 0xF8FFFFFCu, info) == X_STATUS_INVALID_HANDLE, "bad handle");

    /* IRQL */
    CHECK(K(X360_xboxkrnl_KfRaiseIrql, 1, 2) == 0, "raise from passive");
    CHECK(K(X360_xboxkrnl_KeRaiseIrqlToDpcLevel, 0) == 2, "already at dispatch level");
    K(X360_xboxkrnl_KfLowerIrql, 1, 0);
    uint32_t lock = galloc(4);
    uint32_t old = K(X360_xboxkrnl_KfAcquireSpinLock, 1, lock);
    CHECK(old == 0 && x360_cur(X)->irql == 2 && x360_r32(X, lock) != 0, "a spin lock raises the IRQL");
    K(X360_xboxkrnl_KfReleaseSpinLock, 2, lock, old);
    CHECK(x360_cur(X)->irql == 0 && x360_r32(X, lock) == 0, "and gives it back");

    /* protection is recorded and answered */
    uint32_t a = x360_heap_alloc(X, &X->heap_v4k, 0x4000, 0), bp = galloc(8), sp = bp + 4, oldp = galloc(4);
    CHECK(K(X360_xboxkrnl_MmIsAddressValid, 1, a) == 1 && K(X360_xboxkrnl_MmIsAddressValid, 1, 0x3F000000u) == 0,
          "MmIsAddressValid");
    x360_w32(X, bp, a + 0x1010);
    x360_w32(X, sp, 0x10);
    CHECK(K(X360_xboxkrnl_NtProtectVirtualMemory, 5, bp, sp, 2, oldp, 0) == 0, "protect one page");
    CHECK(x360_r32(X, bp) == a + 0x1000 && x360_r32(X, sp) == 0x1000 && x360_r32(X, oldp) == 4, "rounded, old value");
    CHECK(K(X360_xboxkrnl_MmQueryAddressProtect, 1, a + 0x1800) == 2, "the page answers its protection");
    CHECK(K(X360_xboxkrnl_MmQueryAddressProtect, 1, a) == 4 && K(X360_xboxkrnl_MmQueryAddressProtect, 1, a + 0x2000) == 4,
          "its neighbours do not");
    x360_w32(X, bp, a + 0x1000);
    x360_w32(X, sp, 0x1000);
    CHECK(K(X360_xboxkrnl_NtProtectVirtualMemory, 5, bp, sp, 4, oldp, 0) == 0 && x360_r32(X, oldp) == 2, "back");
    CHECK(X->kobj.nprot == 0, "read-write again: no record left");
    x360_w32(X, bp, 0x3F000000u);
    x360_w32(X, sp, 0x1000);
    CHECK(K(X360_xboxkrnl_NtProtectVirtualMemory, 5, bp, sp, 2, oldp, 0) == X_STATUS_NOT_COMMITTED, "unmapped");
}

/* ---- save states ---- */

static uint8_t *snapshot(size_t *n)
{
    *n = x360_state_size(X);
    uint8_t *b = malloc(*n);
    CHECK(x360_state_save(X, "kdpc", b, *n) == *n, "state saved");
    return b;
}

/* the same state without its KOBJ section, as a build before XBA-296 wrote it */
static size_t strip_kobj(uint8_t *b, size_t n)
{
    size_t p = 72;
    uint32_t count = b[64] | b[65] << 8 | b[66] << 16 | (uint32_t)b[67] << 24;
    for (uint32_t k = 0; k < count; k++) {
        uint32_t len = b[p + 8] | b[p + 9] << 8 | b[p + 10] << 16 | (uint32_t)b[p + 11] << 24;
        if (!memcmp(b + p, "KOBJ", 4)) {
            memmove(b + p, b + p + 12 + len, n - (p + 12 + len));
            count--;
            b[64] = (uint8_t)count, b[65] = (uint8_t)(count >> 8);
            return n - 12 - len;
        }
        p += 12 + len;
    }
    CHECK(0, "no KOBJ section in the state");
    return n;
}

static void test_state(void)
{
    machine();
    char why[160] = "";
    x360_thread *w = worker();
    uint32_t forever = timeout_of(-(int64_t)(1000 * MS)), hp = galloc(8);
    K(X360_xboxkrnl_NtDuplicateObject, 3, w->handle, hp, 0);
    uint32_t tm = new_timer(0), per = new_timer(0), dpc = new_dpc(r_log_r4, 0x91), pd = new_dpc(r_log_r4, 0x92);
    set_timer(tm, -(int64_t)(5 * MS), 0, dpc);
    set_timer(per, -(int64_t)(2 * MS), 4, pd);
    K(X360_xboxkrnl_NtQueueApcThread, 5, x360_r32(X, hp), r_log, 0x93, 0, 0);
    K(X360_xboxkrnl_KeInsertQueueDpc, 3, new_dpc(r_log_r4, 0x90), 0, 0);
    K(X360_xboxkrnl_MmSetAddressProtect, 3, code, 0x1000, 0x20);
    size_t n;
    uint8_t *state = snapshot(&n);

    on(w);
    K(X360_xboxkrnl_KeDelayExecutionThread, 3, 1, 1, forever);
    off();
    run_ms(8);
    char first[256];
    snprintf(first, sizeof first, "%s", log_text());
    CHECK(strstr(first, "90") == first && strstr(first, "93") && strstr(first, "91") && strstr(first, "92"),
          "everything pending ran, the queued DPC first: \"%s\"", first);

    /* back to the snapshot: everything pending runs again, the same way */
    CHECK(x360_state_load(X, "kdpc", state, n, why, sizeof why) == X360_STATE_OK, "load: %s", why);
    CHECK(x360_cur(X)->apc_head == 0 && w->apc_head != 0, "the queued APC came back");
    CHECK(X->kobj.dpc_head != 0 && X->kobj.nprot == 1, "the queued DPC and the protection came back");
    on(w);
    K(X360_xboxkrnl_KeDelayExecutionThread, 3, 1, 1, forever);
    off();
    run_ms(8);
    CHECK(!strcmp(log_text(), first), "after the load \"%s\", before \"%s\"", log_text(), first);

    /* a state without the section loads, without that state */
    size_t old = strip_kobj(state, n);
    CHECK(x360_state_load(X, "kdpc", state, old, why, sizeof why) == X360_STATE_OK, "old state: %s", why);
    CHECK(w->apc_head == 0 && X->kobj.dpc_head == 0 && X->kobj.nprot == 0, "no KOBJ: none of its state");
    CHECK(X->timers[0].obj != 0, "the timers of the KERN section are still there");
    free(state);
}

int main(void)
{
    test_dpc();
    test_timers();
    test_nt_timer_apc();
    test_apc();
    test_queue();
    test_iocp();
    test_slist();
    test_slist_threads();
    test_rwlock();
    test_misc();
    test_state();
    return hle_util_end("x360_kdpc");
}
