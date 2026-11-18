/* x360_kdpc.c -- the kernel's deferred work and scheduling objects (XBA-296): DPCs, APCs, timers, queues and I/O
 * completion ports, SLists, read-write locks, mutants, alerts and IRQLs.
 *
 * Guest structures keep the layouts titles compile against (Xenia's descriptions, the NT kernel's for KTIMER and
 * KQUEUE), big-endian in guest memory:
 *   KDPC    type +0 (u16), flink +4, blink +8, routine +0xC, context +0x10, arguments +0x14 / +0x18
 *   KAPC    type +0, mode +2, enqueued +3, thread +4, flink +8, blink +0xC, kernel / rundown / normal routines +0x10 /
 *           +0x14 / +0x18, normal context +0x1C, arguments +0x20 / +0x24
 *   KTIMER  header +0, due time +0x10, DPC +0x20, period +0x24
 *   KQUEUE  header +0 (signal state: entries queued), entries +0x10, current +0x18, maximum +0x1C, threads +0x20
 *   ERWLOCK lock count +0, writers waiting +4, readers waiting +8, readers in +0xC, writer event +0x10, reader
 *           semaphore +0x20
 *   SLIST_HEADER  first entry +0, depth +4 (u16), sequence +6 (u16): one 64-bit word
 *
 * Everything runs on the scheduler of x360_thread.c, in virtual time:
 * - A timer fires at its due time (x360_timers_fire, earliest first): it signals its object, queues its DPC and its
 *   APC, and a periodic timer rearms from its previous due time.
 * - A DPC runs its routine on one guest context of its own, ahead of every ordinary thread in the cooperative mode,
 *   one at a time, in queue order.
 * - An APC runs on its target thread's own context. A user APC waits for an alertable wait (which then returns
 *   STATUS_USER_APC) or KeTestAlertThread. A kernel APC also breaks into a non-alertable wait, which resumes after it,
 *   and runs when an HLE call of a ready thread returns, unless the thread is at a raised IRQL or in a critical region.
 *   A routine is entered from an HLE boundary, where the volatile registers are free by the calling convention, so
 *   only PC, LR, r1 and r3 are kept; returning to X360_APC_RETURN comes back here. An APC still queued when its thread
 *   ends is dropped: its rundown routine is not called.
 * - A queue wakes its waiters last in, first out, as NT does, within the concurrency limit.
 *
 * With guest threads on host threads the kernel lock covers all of this but SLists: lifted code touches their headers
 * without the lock, so they are one 64-bit compare-and-swap on guest memory, as xe_stcx's reservations are.
 * NtProtectVirtualMemory records protections (a query answers them) and does not enforce them: the HLE writes to guest
 * memory directly, and a real read-only page would stop those writes. */
#include <string.h>

#include "x360_internal.h"

#define A(n) ((uint32_t)c->gpr[3 + (n)])
#define RET(v) (c->gpr[3] = (uint64_t)(uint32_t)(v))

#define KAPC_POOLED 0x52    /* KAPC +1: a block of the runtime's own (NtQueueApcThread, timers), recycled after use */
#define APC_DUMMY_KERNEL 0xF00DFF00u
#define APC_DUMMY_RUNDOWN 0xF00DFF01u
#define DISPATCH_LEVEL 2u
#define KTHREAD_APC_DISABLE 0xB0u
#define KPCR_DPC_ACTIVE 0x150u

/* ---- guest lists (X_LIST_ENTRY: flink, blink) ---- */

static void list_init(x360_machine *x, uint32_t h)
{
    x360_w32(x, h, h);
    x360_w32(x, h + 4, h);
}

static void list_add(x360_machine *x, uint32_t h, uint32_t e, int at_head)
{
    uint32_t prev = at_head ? h : x360_r32(x, h + 4), next = x360_r32(x, prev);
    x360_w32(x, e, next);
    x360_w32(x, e + 4, prev);
    x360_w32(x, prev, e);
    x360_w32(x, next + 4, e);
}

static uint32_t list_pop(x360_machine *x, uint32_t h)
{
    uint32_t e = x360_r32(x, h), next = x360_r32(x, e);
    x360_w32(x, h, next);
    x360_w32(x, next + 4, h);
    return e;
}

/* ---- time ---- */

uint64_t x360_due_time(x360_machine *x, int64_t v)
{
    uint64_t now = x360_now(x);
    if (v < 0)
        return now + (uint64_t)(-v);
    uint64_t at = (uint64_t)v > X360_EPOCH ? (uint64_t)v - X360_EPOCH : 0; /* absolute: a system time */
    return at > now ? at : now;
}

/* ---- DPCs ---- */

static int dpc_insert(x360_machine *x, uint32_t dpc, uint32_t arg1, uint32_t arg2)
{
    x360_kobj *k = &x->kobj;
    if (x360_r32(x, dpc + 8))
        return 0; /* already queued (the blink of a queued DPC is never 0) */
    x360_w32(x, dpc + 0x14, arg1);
    x360_w32(x, dpc + 0x18, arg2);
    x360_w32(x, dpc + 4, 0);
    x360_w32(x, dpc + 8, k->dpc_tail ? k->dpc_tail : dpc);
    if (k->dpc_tail)
        x360_w32(x, k->dpc_tail + 4, dpc);
    else
        k->dpc_head = dpc;
    k->dpc_tail = dpc;
    return 1;
}

static int dpc_remove(x360_machine *x, uint32_t dpc)
{
    x360_kobj *k = &x->kobj;
    if (!x360_r32(x, dpc + 8))
        return 0;
    uint32_t prev = 0;
    for (uint32_t d = k->dpc_head; d; prev = d, d = x360_r32(x, d + 4)) {
        if (d != dpc)
            continue;
        uint32_t next = x360_r32(x, d + 4);
        if (prev)
            x360_w32(x, prev + 4, next);
        else
            k->dpc_head = next;
        if (next)
            x360_w32(x, next + 8, prev ? prev : next);
        if (k->dpc_tail == d)
            k->dpc_tail = prev;
        break;
    }
    x360_w32(x, dpc + 4, 0);
    x360_w32(x, dpc + 8, 0);
    return 1;
}

void x360_kdpc_poll(x360_machine *x)
{
    x360_kobj *k = &x->kobj;
    while (k->dpc_head) {
        int slot = k->dpc_slot - 1;
        if (slot >= 0 && x->threads[slot].state != TH_DONE)
            return; /* the one before it still runs: DPCs do not overlap */
        uint32_t dpc = k->dpc_head, routine = x360_r32(x, dpc + 0xC);
        if (routine && x360_callback_run(x, &slot, routine, dpc, x360_r32(x, dpc + 0x10)))
            return; /* no context to be had yet: it stays first in the queue */
        k->dpc_head = x360_r32(x, dpc + 4);
        if (k->dpc_head)
            x360_w32(x, k->dpc_head + 8, k->dpc_head);
        else
            k->dpc_tail = 0;
        x360_w32(x, dpc + 4, 0);
        x360_w32(x, dpc + 8, 0);
        if (!routine)
            continue;
        k->dpc_slot = slot + 1;
        x360_thread *d = &x->threads[slot];
        d->cpu.gpr[5] = x360_r32(x, dpc + 0x14);
        d->cpu.gpr[6] = x360_r32(x, dpc + 0x18);
        x360_w32(x, d->pcr + KPCR_DPC_ACTIVE, 1);
        return;
    }
}

int x360_kdpc_ready(x360_machine *x)
{
    int slot = x->kobj.dpc_slot - 1;
    return slot >= 0 && x->threads[slot].state == TH_READY ? slot : -1;
}

static void k_KeInitializeDpc(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t d = A(0);
    x360_w16(x, d, 19);
    x360_w16(x, d + 2, 0);
    x360_w32(x, d + 4, 0);
    x360_w32(x, d + 8, 0);
    x360_w32(x, d + 0xC, A(1));
    x360_w32(x, d + 0x10, A(2));
    x360_w32(x, d + 0x14, 0);
    x360_w32(x, d + 0x18, 0);
}

static void k_KeInsertQueueDpc(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    RET(dpc_insert(x, A(0), A(1), A(2)));
}

static void k_KeRemoveQueueDpc(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    RET(dpc_remove(x, A(0)));
}

/* ---- APCs ---- */

static int apc_kernel_mode(x360_machine *x, uint32_t apc) { return x360_r8(x, apc + 2) == 0; }

static void apc_unlink(x360_machine *x, x360_thread *t, uint32_t apc)
{
    uint32_t next = x360_r32(x, apc + 8), prev = x360_r32(x, apc + 0xC);
    if (prev)
        x360_w32(x, prev + 8, next);
    else
        t->apc_head = next;
    if (next)
        x360_w32(x, next + 0xC, prev);
    else
        t->apc_tail = prev;
    x360_w32(x, apc + 8, 0);
    x360_w32(x, apc + 0xC, 0);
    x360_w8(x, apc + 3, 0);
}

static void apc_recycle(x360_machine *x, uint32_t apc)
{
    if (x360_r8(x, apc + 1) != KAPC_POOLED)
        return;
    x360_w32(x, apc + 8, x->kobj.apc_free);
    x->kobj.apc_free = apc;
}

static void apc_link(x360_machine *x, x360_thread *t, uint32_t apc)
{
    x360_w32(x, apc + 8, 0);
    x360_w32(x, apc + 0xC, t->apc_tail);
    if (t->apc_tail)
        x360_w32(x, t->apc_tail + 8, apc);
    else
        t->apc_head = apc;
    t->apc_tail = apc;
    x360_w8(x, apc + 3, 1);
}

static int apc_insert(x360_machine *x, uint32_t apc, uint32_t arg1, uint32_t arg2)
{
    x360_thread *t = x360_thread_by_kthread(x, x360_r32(x, apc + 4));
    if (!t || t->state == TH_DONE || x360_r8(x, apc + 3))
        return 0;
    x360_w32(x, apc + 0x20, arg1);
    x360_w32(x, apc + 0x24, arg2);
    apc_link(x, t, apc);
    return 1;
}

/* a user APC of the runtime's own: routine(context, arg1, arg2) on the thread */
static int apc_queue_user(x360_machine *x, uint32_t kthread, uint32_t routine, uint32_t ctx, uint32_t a1, uint32_t a2)
{
    uint32_t apc = x->kobj.apc_free;
    if (apc)
        x->kobj.apc_free = x360_r32(x, apc + 8);
    else if (!(apc = x360_kalloc(x, 0x28)))
        return 0;
    memset(x->base + apc, 0, 0x28);
    x360_w8(x, apc, 18);
    x360_w8(x, apc + 1, KAPC_POOLED);
    x360_w8(x, apc + 2, 1);
    x360_w32(x, apc + 4, kthread);
    x360_w32(x, apc + 0x10, APC_DUMMY_KERNEL);
    x360_w32(x, apc + 0x14, APC_DUMMY_RUNDOWN);
    x360_w32(x, apc + 0x18, routine);
    x360_w32(x, apc + 0x1C, ctx);
    if (apc_insert(x, apc, a1, a2))
        return 1;
    apc_recycle(x, apc);
    return 0;
}

static int apc_can_run(x360_machine *x, x360_thread *t)
{
    return t->irql == 0 && (int32_t)x360_r32(x, t->kthread + KTHREAD_APC_DISABLE) == 0;
}

static uint32_t apc_first(x360_machine *x, x360_thread *t, int kernel_only)
{
    for (uint32_t a = t->apc_head; a; a = x360_r32(x, a + 8))
        if (!kernel_only || apc_kernel_mode(x, a))
            return a;
    return 0;
}

static void apc_finish(x360_machine *x, x360_thread *t)
{
    xe_cpu *c = &t->cpu;
    c->pc = t->apc_ret_pc;
    c->lr = t->apc_ret_lr;
    c->gpr[1] = t->apc_ret_r1;
    c->gpr[3] = t->apc_ret_r3;
    t->apc_stage = 0;
    t->apc_cur = 0;
    if (t->apc_rewait) { /* a kernel APC broke into a wait that is not over */
        t->apc_rewait = 0;
        t->state = TH_WAIT;
        x->m.slice = -1;
    }
}

static void apc_call(xe_cpu *c, uint32_t pc)
{
    c->pc = pc;
    c->lr = X360_APC_RETURN;
    c->gpr[1] = ((c->gpr[1] & ~15ull) - 0x100) & 0xFFFFFFFFu;
}

/* the normal routine of the APC in hand, or the next APC, or back to where the thread was */
static void apc_step(x360_machine *x, x360_thread *t)
{
    xe_cpu *c = &t->cpu;
    for (;;) {
        if (t->apc_stage == 1) { /* its kernel routine returned (or it has none): the values it may have changed */
            uint32_t s = t->apc_scratch, routine = x360_r32(x, s);
            t->apc_stage = 2;
            if (routine) {
                c->gpr[1] = t->apc_ret_r1;
                c->gpr[3] = x360_r32(x, s + 4);
                c->gpr[4] = x360_r32(x, s + 8);
                c->gpr[5] = x360_r32(x, s + 12);
                apc_call(c, routine);
                return;
            }
        }
        if (t->apc_cur)
            apc_recycle(x, t->apc_cur);
        t->apc_cur = 0;
        uint32_t apc = apc_can_run(x, t) ? apc_first(x, t, t->apc_konly) : 0;
        if (!apc) {
            apc_finish(x, t);
            return;
        }
        apc_unlink(x, t, apc);
        t->apc_cur = apc;
        if (!t->apc_scratch)
            t->apc_scratch = x360_kalloc(x, 16);
        uint32_t s = t->apc_scratch, kr = x360_r32(x, apc + 0x10);
        x360_w32(x, s, x360_r32(x, apc + 0x18));
        x360_w32(x, s + 4, x360_r32(x, apc + 0x1C));
        x360_w32(x, s + 8, x360_r32(x, apc + 0x20));
        x360_w32(x, s + 12, x360_r32(x, apc + 0x24));
        t->apc_stage = 1;
        if (kr && kr != APC_DUMMY_KERNEL) {
            c->gpr[1] = t->apc_ret_r1;
            c->gpr[3] = apc;
            c->gpr[4] = s;
            c->gpr[5] = s + 4;
            c->gpr[6] = s + 8;
            c->gpr[7] = s + 12;
            apc_call(c, kr);
            return;
        }
    }
}

/* start delivering on t, which is at an HLE boundary: r3 comes back as `status` afterwards */
static void apc_begin(x360_machine *x, x360_thread *t, int kernel_only, int rewait, uint32_t status)
{
    xe_cpu *c = &t->cpu;
    t->apc_ret_pc = c->pc;
    t->apc_ret_lr = c->lr;
    t->apc_ret_r1 = c->gpr[1];
    t->apc_ret_r3 = status;
    t->apc_konly = kernel_only;
    t->apc_rewait = rewait;
    t->apc_stage = 3; /* no APC in hand yet */
    t->apc_cur = 0;
    apc_step(x, t);
}

void x360_apc_return(x360_machine *x, x360_thread *t)
{
    if (!t->apc_stage) {
        x360_fail(x, "a return to the APC dispatcher with no APC running (thread %u)", t->id);
        return;
    }
    apc_step(x, t);
}

/* after an HLE call: a kernel APC for this thread runs as soon as nothing holds it back */
void x360_apc_after_hle(x360_machine *x, x360_thread *t)
{
    if (t->apc_head && !t->apc_stage && t->state == TH_READY && !t->seh.depth && apc_can_run(x, t) &&
        apc_first(x, t, 1))
        apc_begin(x, t, 1, 0, (uint32_t)t->cpu.gpr[3]);
}

/* a wait that nothing satisfied: 1 when an alert or an APC ends it (*status set), or breaks into it */
int x360_apc_wait_check(x360_machine *x, x360_thread *t, uint32_t *status)
{
    if (t->apc_stage)
        return 0;
    if (t->wait_alertable && t->alerted) {
        t->alerted = 0;
        *status = X_STATUS_ALERTED;
        return 1;
    }
    if (!t->apc_head || !apc_can_run(x, t))
        return 0;
    if (t->wait_alertable) {
        *status = X_STATUS_USER_APC;
        return 1;
    }
    if (!t->suspend && apc_first(x, t, 1)) {
        t->state = TH_READY;
        apc_begin(x, t, 1, 1, 0);
    }
    return 0;
}

void x360_apc_wait_done(x360_machine *x, x360_thread *t, uint32_t status)
{
    if (status == X_STATUS_USER_APC)
        apc_begin(x, t, 0, 0, status);
}

static void k_KeInitializeApc(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t a = A(0);
    memset(x->base + a, 0, 0x28);
    x360_w8(x, a, 18);
    x360_w8(x, a + 2, A(5) & 0xFF);
    x360_w32(x, a + 4, A(1));
    x360_w32(x, a + 0x10, A(2));
    x360_w32(x, a + 0x14, A(3));
    x360_w32(x, a + 0x18, A(4));
    x360_w32(x, a + 0x1C, A(4) ? A(6) : 0);
}

static void k_KeInsertQueueApc(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    RET(apc_insert(x, A(0), A(1), A(2)));
}

static void k_KeRemoveQueueApc(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t a = A(0);
    x360_thread *n = x360_thread_by_kthread(x, x360_r32(x, a + 4));
    if (!n || !x360_r8(x, a + 3)) {
        RET(0);
        return;
    }
    apc_unlink(x, n, a);
    RET(1);
}

static void k_NtQueueApcThread(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t kth = x360_handle_obj(x, A(0));
    x360_thread *n = kth ? x360_thread_by_kthread(x, kth) : NULL;
    if (!n) {
        RET(X_STATUS_INVALID_HANDLE);
        return;
    }
    RET(apc_queue_user(x, kth, A(1), A(2), A(3), A(4)) ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL);
}

/* ---- alerts, suspension, IRQL ---- */

static void k_KeAlertThread(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    x360_thread *n = x360_thread_by_kthread(x, A(0));
    RET(n ? n->alerted : 0);
    if (n)
        n->alerted = 1;
}

static void k_KeAlertResumeThread(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    x360_thread *n = x360_thread_by_kthread(x, A(0));
    if (!n) {
        RET(0);
        return;
    }
    RET(n->suspend);
    n->alerted = 1;
    if (n->suspend > 0 && --n->suspend == 0 && n->state == TH_SUSPENDED)
        n->state = TH_READY;
}

static void k_KeTestAlertThread(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    uint32_t alerted = (uint32_t)t->alerted; /* a BOOLEAN, as NT: whether an alert was pending */
    t->alerted = 0;
    RET(alerted);
    if (!alerted && !t->apc_stage && t->apc_head && apc_can_run(x, t))
        apc_begin(x, t, 0, 0, alerted);
}

static void k_KeSuspendThread(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    x360_thread *n = x360_thread_by_kthread(x, A(0));
    if (!n) {
        RET(0);
        return;
    }
    RET(n->suspend);
    n->suspend++;
    if (n->state == TH_READY)
        n->state = TH_SUSPENDED;
    if (n == t)
        x->m.slice = -1;
}

static void k_KfRaiseIrql(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x;
    uint32_t old = t->irql;
    t->irql = A(0) & 0xFF;
    RET(old);
}

static void k_KeRaiseIrqlToDpcLevel(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x;
    uint32_t old = t->irql;
    t->irql = DISPATCH_LEVEL;
    RET(old);
}

static void k_KfLowerIrql(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x;
    t->irql = A(0) & 0xFF;
    RET(0);
}

static void k_KeEnterCriticalRegion(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)c;
    x360_w32(x, t->kthread + KTHREAD_APC_DISABLE, x360_r32(x, t->kthread + KTHREAD_APC_DISABLE) - 1);
}

static void k_KeLeaveCriticalRegion(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)c;
    x360_w32(x, t->kthread + KTHREAD_APC_DISABLE, x360_r32(x, t->kthread + KTHREAD_APC_DISABLE) + 1);
}

static void k_KeStallExecutionProcessor(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    x->time_skip += (uint64_t)A(0) * 10; /* microseconds of virtual time */
    RET(0);
}

static void k_success(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(X_STATUS_SUCCESS);
}

/* ---- timers ---- */

static x360_timer *timer_of(x360_machine *x, uint32_t obj)
{
    for (int k = 0; k < X360_MAX_TIMERS; k++)
        if (x->timers[k].obj == obj)
            return &x->timers[k];
    return NULL;
}

/* arm a timer: 1 when it was armed before, 0 when not, -1 when no slot is free (nothing is armed then) */
int x360_timer_arm(x360_machine *x, uint32_t obj, uint64_t due, uint32_t period, uint32_t dpc, uint32_t apc_routine,
                   uint32_t apc_ctx, uint32_t kthread)
{
    x360_timer *tm = timer_of(x, obj);
    int was = tm && tm->due;
    for (int k = 0; !tm && k < X360_MAX_TIMERS; k++)
        if (!x->timers[k].obj || !x->timers[k].due) /* free, or a timer that is not armed any more */
            tm = &x->timers[k];
    if (!tm) {
        x360_logf(x, "x360: more than %d timers armed: this one is refused\n", X360_MAX_TIMERS);
        return -1;
    }
    *tm = (x360_timer){obj, due ? due : 1, period, apc_routine, apc_ctx, kthread};
    x360_w32(x, obj + 4, 0);
    x360_w64(x, obj + 0x10, X360_EPOCH + (due ? due : 1));
    x360_w32(x, obj + 0x20, dpc);
    x360_w32(x, obj + 0x24, period);
    return was;
}

void x360_timer_set(x360_machine *x, uint32_t obj, uint64_t due, uint32_t period)
{
    x360_timer_arm(x, obj, due, period, 0, 0, 0, 0);
}

int x360_timer_cancel(x360_machine *x, uint32_t obj)
{
    x360_timer *tm = timer_of(x, obj);
    int was = tm && tm->due;
    if (tm)
        tm->due = 0;
    return was;
}

void x360_timers_fire(x360_machine *x)
{
    uint64_t now = x360_now(x);
    for (;;) {
        x360_timer *tm = NULL;
        for (int k = 0; k < X360_MAX_TIMERS; k++) {
            x360_timer *e = &x->timers[k];
            if (e->obj && e->due && now >= e->due && (!tm || e->due < tm->due))
                tm = e;
        }
        if (!tm)
            return;
        uint64_t at = X360_EPOCH + tm->due;
        uint32_t dpc = x360_r32(x, tm->obj + 0x20);
        x360_w32(x, tm->obj + 4, 1);
        if (dpc)
            dpc_insert(x, dpc, (uint32_t)at, (uint32_t)(at >> 32));
        if (tm->apc_routine)
            apc_queue_user(x, tm->apc_thread, tm->apc_routine, tm->apc_ctx, (uint32_t)at, (uint32_t)(at >> 32));
        if (tm->period) { /* a late timer does not fire in a burst to catch up */
            uint64_t step = (uint64_t)tm->period * 10000;
            tm->due += step * ((now - tm->due) / step + 1);
        } else {
            tm->due = 0;
        }
    }
}

static void k_KeInitializeTimerEx(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t o = A(0);
    memset(x->base + o, 0, 0x28);
    x360_w8(x, o, A(1) ? X_OBJ_TIMER_SYNC : X_OBJ_TIMER_NOTIFICATION);
    x360_w8(x, o + 2, 0x28 / 4);
    list_init(x, o + 8);
}

static void set_timer(x360_machine *x, xe_cpu *c, uint32_t period, uint32_t dpc)
{
    int was = x360_timer_arm(x, A(0), x360_due_time(x, (int64_t)c->gpr[4]), period, dpc, 0, 0, 0);
    RET(was > 0);
}

static void k_KeSetTimerEx(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    set_timer(x, c, A(2), A(3)); /* (timer, due time, period ms, dpc) */
}

static void k_KeSetTimer(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    set_timer(x, c, 0, A(2)); /* (timer, due time, dpc) */
}

static void k_KeCancelTimer(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    RET(x360_timer_cancel(x, A(0)));
}

static void k_NtQueryTimer(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t o = x360_handle_obj(x, A(0)), info = A(1);
    if (!o) {
        RET(X_STATUS_INVALID_HANDLE);
        return;
    }
    x360_timer *tm = timer_of(x, o);
    uint64_t now = x360_now(x), left = tm && tm->due > now ? tm->due - now : 0;
    if (info) { /* TIMER_BASIC_INFORMATION: time remaining, signalled */
        x360_w64(x, info, left);
        x360_w32(x, info + 8, x360_r32(x, o + 4) != 0);
    }
    RET(X_STATUS_SUCCESS);
}

/* ---- mutants, and what the Nt objects answer ---- */

static void k_KeInitializeMutant(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    uint32_t o = A(0), owned = A(1) & 0xFF;
    memset(x->base + o, 0, 0x20);
    x360_w8(x, o, X_OBJ_MUTANT);
    x360_w8(x, o + 2, 0x20 / 4);
    x360_w32(x, o + 4, owned ? 0 : 1);
    list_init(x, o + 8);
    x360_w32(x, o + 0x18, owned ? t->kthread : 0);
}

static void k_KeReleaseMutant(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    /* (mutant, increment, abandoned, wait) */
    uint32_t o = A(0), abandoned = A(2) & 0xFF;
    int32_t prev = (int32_t)x360_r32(x, o + 4);
    RET(prev);
    if (abandoned) {
        x360_w32(x, o + 4, 1);
        x360_w32(x, o + 0x18, 0);
        return;
    }
    if (x360_r32(x, o + 0x18) != t->kthread)
        return; /* NT raises STATUS_MUTANT_NOT_OWNED here; the mutant stays as it is */
    x360_w32(x, o + 4, (uint32_t)(prev + 1));
    if (prev + 1 > 0)
        x360_w32(x, o + 0x18, 0);
}

static void k_NtQueryMutant(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    uint32_t o = x360_handle_obj(x, A(0)), info = A(1);
    if (!o) {
        RET(X_STATUS_INVALID_HANDLE);
        return;
    }
    if (info) { /* MUTANT_BASIC_INFORMATION: count, owned by the caller, abandoned */
        x360_w32(x, info, x360_r32(x, o + 4));
        x360_w32(x, info + 4, x360_r32(x, o + 0x18) == t->kthread);
        x360_w32(x, info + 8, 0);
    }
    RET(X_STATUS_SUCCESS);
}

static void k_NtQuerySemaphore(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t o = x360_handle_obj(x, A(0)), info = A(1);
    if (!o) {
        RET(X_STATUS_INVALID_HANDLE);
        return;
    }
    if (info) { /* SEMAPHORE_BASIC_INFORMATION: count, limit */
        x360_w32(x, info, x360_r32(x, o + 4));
        x360_w32(x, info + 4, x360_r32(x, o + 16));
    }
    RET(X_STATUS_SUCCESS);
}

/* ---- queues ---- */

#define Q_ENTRIES 0x10u
#define Q_CURRENT 0x18u
#define Q_MAXIMUM 0x1Cu
#define Q_THREADS 0x20u
#define Q_SIZE 0x28u

static void queue_init(x360_machine *x, uint32_t q, uint32_t count)
{
    memset(x->base + q, 0, Q_SIZE);
    x360_w8(x, q, X_OBJ_QUEUE);
    x360_w8(x, q + 2, Q_SIZE / 4);
    list_init(x, q + 8);
    list_init(x, q + Q_ENTRIES);
    x360_w32(x, q + Q_MAXIMUM, count ? count : 6); /* 0: one per hardware thread */
    list_init(x, q + Q_THREADS);
}

static uint32_t queue_insert(x360_machine *x, uint32_t q, uint32_t entry, int at_head)
{
    uint32_t prev = x360_r32(x, q + 4);
    list_add(x, q + Q_ENTRIES, entry, at_head);
    x360_w32(x, q + 4, prev + 1);
    return prev;
}

static void queue_count(x360_machine *x, uint32_t q, int delta)
{
    x360_w32(x, q + Q_CURRENT, x360_r32(x, q + Q_CURRENT) + (uint32_t)delta);
}

/* t stops counting against the queue it serves */
static void queue_leave(x360_machine *x, x360_thread *t)
{
    if (t->queue && t->queue_active) {
        queue_count(x, t->queue, -1);
        t->queue_active = 0;
    }
}

/* x360_wait: a thread serving a queue blocks on something else, the queue may release another one meanwhile */
void x360_queue_parked(x360_machine *x, x360_thread *t, int kind)
{
    if (kind != WAIT_QUEUE && t->queue && t->queue_active) {
        queue_leave(x, t);
        t->queue_parked = 1;
    }
}

void x360_queue_unparked(x360_machine *x, x360_thread *t)
{
    if (t->queue_parked) {
        t->queue_parked = 0;
        t->queue_active = 1;
        queue_count(x, t->queue, 1);
    }
}

void x360_queue_thread_exit(x360_machine *x, x360_thread *t)
{
    queue_leave(x, t);
    t->queue = 0;
    t->queue_parked = 0;
}

static int queue_can_take(x360_machine *x, uint32_t q)
{
    return (int32_t)x360_r32(x, q + 4) > 0 && x360_r32(x, q + Q_CURRENT) < x360_r32(x, q + Q_MAXIMUM);
}

/* the first entry to t: its value in r3, or an I/O completion packet written out (status 0) */
static uint32_t queue_take(x360_machine *x, x360_thread *t, uint32_t q)
{
    uint32_t e = list_pop(x, q + Q_ENTRIES);
    x360_w32(x, q + 4, x360_r32(x, q + 4) - 1);
    queue_count(x, q, 1);
    t->queue_active = 1;
    if (!t->wait_iocp)
        return e;
    /* the packet: list entry, key, APC context, status, information */
    if (t->wait_out[0])
        x360_w32(x, t->wait_out[0], x360_r32(x, e + 8));
    if (t->wait_out[1])
        x360_w32(x, t->wait_out[1], x360_r32(x, e + 12));
    if (t->wait_out[2]) {
        x360_w32(x, t->wait_out[2], x360_r32(x, e + 16));
        x360_w32(x, t->wait_out[2] + 4, x360_r32(x, e + 20));
    }
    x360_w32(x, e, x->kobj.iocp_free);
    x->kobj.iocp_free = e;
    t->wait_iocp = 0;
    return X_STATUS_SUCCESS;
}

/* the newest waiter is served first (LIFO, as NT): only it may take an entry */
int x360_queue_wait_check(x360_machine *x, x360_thread *t, uint32_t *status)
{
    uint32_t q = t->wait_obj[0];
    if (!queue_can_take(x, q))
        return 0;
    for (int k = 0; k < x->nthreads; k++) {
        x360_thread *o = &x->threads[k];
        if (o != t && o->state == TH_WAIT && o->wait_kind == WAIT_QUEUE && o->wait_obj[0] == q &&
            o->wait_stamp > t->wait_stamp)
            return 0;
    }
    *status = queue_take(x, t, q);
    return 1;
}

/* KeRemoveQueue's start: leave what t served; 1 when an entry was taken at once (*r set) */
static int queue_enter(x360_machine *x, x360_thread *t, uint32_t q, uint32_t *r)
{
    queue_leave(x, t);
    t->queue = q;
    if (!queue_can_take(x, q))
        return 0;
    *r = queue_take(x, t, q);
    return 1;
}

static void queue_wait(x360_machine *x, x360_thread *t, uint32_t q, uint32_t timeout_ptr)
{
    x360_wait(x, t, WAIT_QUEUE, &q, 1, 0, timeout_ptr);
    t->wait_stamp = ++x->kobj.wait_stamp;
}

static void k_KeInitializeQueue(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    queue_init(x, A(0), A(1));
}

static void k_KeInsertQueue(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    RET(queue_insert(x, A(0), A(1), 0));
}

static void k_KeInsertHeadQueue(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    RET(queue_insert(x, A(0), A(1), 1));
}

static void k_KeRemoveQueue(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    /* (queue, wait mode, timeout*): the entry, or STATUS_TIMEOUT as the pointer's value */
    uint32_t q = A(0), r;
    t->wait_iocp = 0;
    if (queue_enter(x, t, q, &r))
        RET(r);
    else
        queue_wait(x, t, q, A(2));
}

static void k_KeRundownQueue(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* the entries stay linked in a ring without the head, as NT leaves them; the threads no longer serve the queue */
    uint32_t q = A(0), h = q + Q_ENTRIES, first = x360_r32(x, h);
    if (first == h) {
        first = 0;
    } else {
        uint32_t last = x360_r32(x, h + 4);
        x360_w32(x, first + 4, last);
        x360_w32(x, last, first);
    }
    list_init(x, h);
    x360_w32(x, q + 4, 0);
    x360_w32(x, q + Q_CURRENT, 0);
    for (int k = 0; k < x->nthreads; k++)
        if (x->threads[k].queue == q) {
            x->threads[k].queue = 0;
            x->threads[k].queue_active = x->threads[k].queue_parked = 0;
        }
    RET(first);
}

/* ---- I/O completion ports: a queue of packets ---- */

static void k_NtCreateIoCompletion(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (handle*, access, attributes, concurrent threads) */
    uint32_t q = x360_kalloc(x, Q_SIZE), h = q ? x360_handle_new(x, q) : 0;
    if (!h) {
        RET(X_STATUS_NO_MEMORY);
        return;
    }
    queue_init(x, q, A(3));
    x360_w32(x, A(0), h);
    RET(X_STATUS_SUCCESS);
}

static void k_NtSetIoCompletion(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (handle, key, APC context, status, information) */
    uint32_t q = x360_handle_obj(x, A(0));
    if (!q || x360_r8(x, q) != X_OBJ_QUEUE) {
        RET(X_STATUS_INVALID_HANDLE);
        return;
    }
    uint32_t p = x->kobj.iocp_free;
    if (p)
        x->kobj.iocp_free = x360_r32(x, p);
    else if (!(p = x360_kalloc(x, 24))) {
        RET(X_STATUS_NO_MEMORY);
        return;
    }
    x360_w32(x, p + 8, A(1));
    x360_w32(x, p + 12, A(2));
    x360_w32(x, p + 16, A(3));
    x360_w32(x, p + 20, A(4));
    queue_insert(x, q, p, 0);
    RET(X_STATUS_SUCCESS);
}

static void k_NtRemoveIoCompletion(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    /* (handle, key*, APC context*, IO_STATUS_BLOCK*, timeout*) */
    uint32_t q = x360_handle_obj(x, A(0)), r;
    if (!q || x360_r8(x, q) != X_OBJ_QUEUE) {
        RET(X_STATUS_INVALID_HANDLE);
        return;
    }
    t->wait_iocp = 1;
    t->wait_out[0] = A(1);
    t->wait_out[1] = A(2);
    t->wait_out[2] = A(3);
    if (queue_enter(x, t, q, &r))
        RET(r);
    else
        queue_wait(x, t, q, A(4));
}

static void k_NtQueryIoCompletion(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (handle, information class, buffer, length, returned*): the packets queued */
    uint32_t q = x360_handle_obj(x, A(0));
    if (!q || x360_r8(x, q) != X_OBJ_QUEUE) {
        RET(X_STATUS_INVALID_HANDLE);
        return;
    }
    if (A(1) != 0 || A(3) < 4) {
        RET(A(1) != 0 ? X_STATUS_INVALID_PARAMETER : X_STATUS_INFO_LENGTH_MISMATCH);
        return;
    }
    x360_w32(x, A(2), x360_r32(x, q + 4));
    if (A(4))
        x360_w32(x, A(4), 4);
    RET(X_STATUS_SUCCESS);
}

/* ---- SLists: the header is one 64-bit word, changed by one compare-and-swap ---- */

static uint64_t slist_load(x360_machine *x, uint32_t h)
{
    return __builtin_bswap64(__atomic_load_n((uint64_t *)(x->base + h), __ATOMIC_ACQUIRE));
}

static int slist_cas(x360_machine *x, uint32_t h, uint64_t old, uint64_t val)
{
    uint64_t e = __builtin_bswap64(old);
    return __atomic_compare_exchange_n((uint64_t *)(x->base + h), &e, __builtin_bswap64(val), 0, __ATOMIC_ACQ_REL,
                                       __ATOMIC_ACQUIRE);
}

#define SL_NEXT(v) ((uint32_t)((v) >> 32))
#define SL_DEPTH(v) ((uint32_t)((v) >> 16) & 0xFFFF)
#define SL_SEQ(v) ((uint32_t)(v) & 0xFFFF)
#define SL_MAKE(next, depth, seq) ((uint64_t)(next) << 32 | ((uint64_t)(depth) & 0xFFFF) << 16 | ((seq) & 0xFFFF))

static int slist_aligned(x360_machine *x, xe_cpu *c, uint32_t h)
{
    if (!(h & 7))
        return 1;
    x360_fail(x, "an SLIST_HEADER at %08X is not 8-byte aligned", h);
    RET(0);
    return 0;
}

static void k_InterlockedPushEntrySList(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t h = A(0), e = A(1);
    if (!slist_aligned(x, c, h))
        return;
    uint64_t old;
    do {
        old = slist_load(x, h);
        x360_w32(x, e, SL_NEXT(old));
    } while (!slist_cas(x, h, old, SL_MAKE(e, SL_DEPTH(old) + 1, SL_SEQ(old) + 1)));
    RET(SL_NEXT(old));
}

static void k_InterlockedPopEntrySList(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t h = A(0);
    if (!slist_aligned(x, c, h))
        return;
    uint64_t old;
    uint32_t e;
    do {
        old = slist_load(x, h);
        e = SL_NEXT(old);
        if (!e)
            break;
    } while (!slist_cas(x, h, old, SL_MAKE(x360_r32(x, e), SL_DEPTH(old) - 1, SL_SEQ(old))));
    RET(e);
}

static void k_InterlockedFlushSList(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t h = A(0);
    if (!slist_aligned(x, c, h))
        return;
    uint64_t old;
    do {
        old = slist_load(x, h);
    } while (SL_NEXT(old) && !slist_cas(x, h, old, 0));
    RET(SL_NEXT(old));
}

/* ---- read-write locks ---- */

#define RW_COUNT 0u
#define RW_WRITERS 4u
#define RW_READERS 8u
#define RW_ENTRY 0xCu
#define RW_EVENT 0x10u
#define RW_SEM 0x20u

static void rw_add(x360_machine *x, uint32_t a, int32_t d) { x360_w32(x, a, x360_r32(x, a) + (uint32_t)d); }

static void k_ExInitializeReadWriteLock(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t l = A(0);
    memset(x->base + l, 0, 0x38);
    x360_w32(x, l + RW_COUNT, 0xFFFFFFFFu);
    x360_w8(x, l + RW_EVENT, X_OBJ_EVENT_SYNC);
    x360_w8(x, l + RW_EVENT + 2, 4);
    list_init(x, l + RW_EVENT + 8);
    x360_w8(x, l + RW_SEM, X_OBJ_SEMAPHORE);
    x360_w8(x, l + RW_SEM + 2, 5);
    list_init(x, l + RW_SEM + 8);
    x360_w32(x, l + RW_SEM + 16, 0x7FFFFFFFu);
}

static void k_ExAcquireReadWriteLockExclusive(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    uint32_t l = A(0), ev = l + RW_EVENT;
    rw_add(x, l + RW_COUNT, 1);
    if (x360_r32(x, l + RW_COUNT) == 0)
        return;
    (void)c;
    rw_add(x, l + RW_WRITERS, 1);
    x360_wait(x, t, WAIT_OBJECTS, &ev, 1, 0, 0);
}

static void k_ExTryToAcquireReadWriteLockExclusive(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t l = A(0);
    if ((int32_t)x360_r32(x, l + RW_COUNT) < 0) {
        x360_w32(x, l + RW_COUNT, 0);
        RET(1);
    } else {
        RET(0);
    }
}

static int rw_readers_may_enter(x360_machine *x, uint32_t l)
{
    return x360_r32(x, l + RW_ENTRY) && !x360_r32(x, l + RW_WRITERS);
}

static void k_ExAcquireReadWriteLockShared(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)c;
    uint32_t l = A(0), sem = l + RW_SEM;
    rw_add(x, l + RW_COUNT, 1);
    if (x360_r32(x, l + RW_COUNT) == 0 || rw_readers_may_enter(x, l)) {
        rw_add(x, l + RW_ENTRY, 1);
        return;
    }
    rw_add(x, l + RW_READERS, 1);
    x360_wait(x, t, WAIT_OBJECTS, &sem, 1, 0, 0);
}

static void k_ExTryToAcquireReadWriteLockShared(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t l = A(0);
    if ((int32_t)x360_r32(x, l + RW_COUNT) < 0 || rw_readers_may_enter(x, l)) {
        rw_add(x, l + RW_COUNT, 1);
        rw_add(x, l + RW_ENTRY, 1);
        RET(1);
    } else {
        RET(0);
    }
}

static void k_ExReleaseReadWriteLock(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t, (void)c;
    uint32_t l = A(0);
    rw_add(x, l + RW_COUNT, -1);
    if ((int32_t)x360_r32(x, l + RW_COUNT) < 0) {
        x360_w32(x, l + RW_ENTRY, 0);
        return;
    }
    uint32_t readers = x360_r32(x, l + RW_READERS);
    if (!x360_r32(x, l + RW_ENTRY) && readers) { /* a writer left: the readers that waited go in together */
        x360_w32(x, l + RW_READERS, 0);
        x360_w32(x, l + RW_ENTRY, readers);
        rw_add(x, l + RW_SEM + 4, (int32_t)readers);
        return;
    }
    if (x360_r32(x, l + RW_ENTRY)) {
        rw_add(x, l + RW_ENTRY, -1);
        if (x360_r32(x, l + RW_ENTRY))
            return;
    }
    rw_add(x, l + RW_WRITERS, -1);
    x360_w32(x, l + RW_EVENT + 4, 1);
}

/* ---- memory protection: recorded, not enforced (see the top of this file) ---- */

static uint32_t prot_of(x360_machine *x, uint32_t a)
{
    for (unsigned k = 0; k < x->kobj.nprot; k++)
        if (a - x->kobj.prot[k].base < x->kobj.prot[k].size)
            return x->kobj.prot[k].prot;
    return 4; /* PAGE_READWRITE */
}

/* the range takes one record; what it overlaps is cut away (a record split in two needs a free slot) */
static int prot_set(x360_machine *x, uint32_t base, uint32_t size, uint32_t prot)
{
    x360_kobj *k = &x->kobj;
    for (unsigned i = 0; i < k->nprot;) {
        uint32_t b = k->prot[i].base, e = b + k->prot[i].size;
        if (e <= base || b >= base + size) {
            i++;
        } else if (b < base && e > base + size) {
            if (k->nprot == X360_MAX_PROT)
                return -1;
            k->prot[k->nprot++] = (x360_prot){base + size, e - (base + size), k->prot[i].prot};
            k->prot[i].size = base - b;
            i++;
        } else if (b < base) {
            k->prot[i++].size = base - b;
        } else if (e > base + size) {
            k->prot[i].size = e - (base + size);
            k->prot[i++].base = base + size;
        } else {
            k->prot[i] = k->prot[--k->nprot];
        }
    }
    if (prot == 4)
        return 0;
    if (k->nprot == X360_MAX_PROT)
        return -1;
    k->prot[k->nprot++] = (x360_prot){base, size, prot};
    return 0;
}

static int mapped(x360_machine *x, uint32_t a, uint32_t size)
{
    x360_heap *h = x360_heap_of(x, a);
    if (!h)
        return 0;
    uint32_t in = h->physical ? a & 0x1FFFFFFFu : a;
    return x360_heap_all_used(h, in & ~(h->page - 1), size ? size + (in & (h->page - 1)) : 1);
}

static void k_NtProtectVirtualMemory(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (base*, size*, new protection, old protection*, type) */
    uint32_t a = x360_r32(x, A(0)), size = x360_r32(x, A(1));
    uint32_t base = a & ~0xFFFu, len = ((a + size + 0xFFFu) & ~0xFFFu) - base;
    if (!size || !mapped(x, base, len)) {
        RET(X_STATUS_NOT_COMMITTED);
        return;
    }
    uint32_t old = prot_of(x, base);
    if (prot_set(x, base, len, A(2))) {
        RET(X_STATUS_NO_MEMORY);
        return;
    }
    x360_w32(x, A(0), base);
    x360_w32(x, A(1), len);
    if (A(3))
        x360_w32(x, A(3), old);
    RET(X_STATUS_SUCCESS);
}

static void k_MmSetAddressProtect(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t a = A(0) & ~0xFFFu, len = ((A(0) + A(1) + 0xFFFu) & ~0xFFFu) - a;
    if (A(1) && mapped(x, a, len))
        prot_set(x, a, len, A(2) & 0x3FFu);
    RET(0);
}

static void k_MmQueryAddressProtect(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    RET(mapped(x, A(0), 1) ? prot_of(x, A(0) & ~0xFFFu) : 0);
}

static void k_MmIsAddressValid(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    RET(mapped(x, A(0), 1));
}

/* ---- save states: section KOBJ (x360_state.c) ---- */

void x360_kobj_reset(x360_machine *x)
{
    memset(&x->kobj, 0, sizeof x->kobj);
    x360_vd_defaults(x);
    for (int k = 0; k < X360_MAX_TIMERS; k++) {
        x360_timer *tm = &x->timers[k];
        tm->apc_routine = tm->apc_ctx = tm->apc_thread = 0;
        if (k >= X360_KERN_TIMERS)
            tm->obj = 0, tm->due = 0, tm->period = 0;
    }
    for (int k = 0; k < X360_MAX_THREADS; k++) {
        x360_thread *t = &x->threads[k];
        t->wait_alertable = t->alerted = t->irql = 0;
        t->apc_head = t->apc_tail = t->apc_cur = t->apc_scratch = 0;
        t->apc_stage = t->apc_konly = t->apc_rewait = 0;
        t->apc_ret_pc = t->apc_ret_r3 = 0;
        t->apc_ret_lr = t->apc_ret_r1 = 0;
        t->queue = 0;
        t->queue_active = t->queue_parked = t->wait_iocp = 0;
        t->wait_out[0] = t->wait_out[1] = t->wait_out[2] = 0;
        t->wait_stamp = 0;
    }
}

void x360_kobj_state(x360_machine *x, void *s, void (*u32)(void *, uint32_t *), void (*u64)(void *, uint64_t *),
                     void (*i32)(void *, int *))
{
    x360_kobj *k = &x->kobj;
    u32(s, &k->dpc_head), u32(s, &k->dpc_tail), i32(s, &k->dpc_slot);
    u32(s, &k->apc_free), u32(s, &k->iocp_free);
    u64(s, &k->wait_stamp);
    u32(s, &k->nprot);
    if (k->nprot > X360_MAX_PROT)
        k->nprot = X360_MAX_PROT;
    for (unsigned p = 0; p < k->nprot; p++)
        u32(s, &k->prot[p].base), u32(s, &k->prot[p].size), u32(s, &k->prot[p].prot);
    u32(s, &k->duck_enabled), u32(s, &k->duck_level), u32(s, &k->duck_threshold);
    u32(s, &k->duck_attack), u32(s, &k->duck_hold), u32(s, &k->duck_release);
    for (int n = 0; n < X360_MAX_TIMERS; n++) {
        x360_timer *tm = &x->timers[n];
        if (n >= X360_KERN_TIMERS)
            u32(s, &tm->obj), u64(s, &tm->due), u32(s, &tm->period);
        u32(s, &tm->apc_routine), u32(s, &tm->apc_ctx), u32(s, &tm->apc_thread);
    }
    for (int n = 0; n < x->nthreads && n < X360_MAX_THREADS; n++) {
        x360_thread *t = &x->threads[n];
        i32(s, &t->wait_alertable), i32(s, &t->alerted);
        u32(s, &t->irql);
        u32(s, &t->apc_head), u32(s, &t->apc_tail), u32(s, &t->apc_cur), u32(s, &t->apc_scratch);
        i32(s, &t->apc_stage), i32(s, &t->apc_konly), i32(s, &t->apc_rewait);
        u32(s, &t->apc_ret_pc), u32(s, &t->apc_ret_r3);
        u64(s, &t->apc_ret_lr), u64(s, &t->apc_ret_r1);
        u32(s, &t->queue);
        i32(s, &t->queue_active), i32(s, &t->queue_parked), i32(s, &t->wait_iocp);
        u32(s, &t->wait_out[0]), u32(s, &t->wait_out[1]), u32(s, &t->wait_out[2]);
        u64(s, &t->wait_stamp);
    }
}

const x360_hle_row x360_tab_kdpc[] = {
    X360_HK(KeInitializeDpc, k_KeInitializeDpc), X360_HK(KeInsertQueueDpc, k_KeInsertQueueDpc),
    X360_HK(KeRemoveQueueDpc, k_KeRemoveQueueDpc),
    X360_HK(KeInitializeApc, k_KeInitializeApc), X360_HK(KeInsertQueueApc, k_KeInsertQueueApc),
    X360_HK(KeRemoveQueueApc, k_KeRemoveQueueApc), X360_HK(NtQueueApcThread, k_NtQueueApcThread),
    X360_HK(KeAlertThread, k_KeAlertThread), X360_HK(KeAlertResumeThread, k_KeAlertResumeThread),
    X360_HK(KeTestAlertThread, k_KeTestAlertThread), X360_HK(KeSuspendThread, k_KeSuspendThread),
    X360_HK(KeBoostPriorityThread, k_success), X360_HK(KeSetPriorityClassThread, k_success),
    X360_HK(KfRaiseIrql, k_KfRaiseIrql), X360_HK(KeRaiseIrqlToDpcLevel, k_KeRaiseIrqlToDpcLevel),
    X360_HK(KfLowerIrql, k_KfLowerIrql), X360_HK(KeEnterCriticalRegion, k_KeEnterCriticalRegion),
    X360_HK(KeLeaveCriticalRegion, k_KeLeaveCriticalRegion),
    X360_HK(KeStallExecutionProcessor, k_KeStallExecutionProcessor),
    X360_HK(KeFlushCacheRange, k_success), X360_HK(KeSweepDcacheRange, k_success),
    X360_HK(KeSweepIcacheRange, k_success),
    X360_HK(KeSaveFloatingPointState, k_success), X360_HK(KeRestoreFloatingPointState, k_success),
    X360_HK(KeSaveVectorUnitState, k_success), X360_HK(KeRestoreVectorUnitState, k_success),
    X360_HK(KeInitializeTimerEx, k_KeInitializeTimerEx), X360_HK(KeSetTimerEx, k_KeSetTimerEx),
    X360_HK(KeSetTimer, k_KeSetTimer), X360_HK(KeCancelTimer, k_KeCancelTimer), X360_HK(NtQueryTimer, k_NtQueryTimer),
    X360_HK(KeInitializeMutant, k_KeInitializeMutant), X360_HK(KeReleaseMutant, k_KeReleaseMutant),
    X360_HK(NtQueryMutant, k_NtQueryMutant), X360_HK(NtQuerySemaphore, k_NtQuerySemaphore),
    X360_HK(KeInitializeQueue, k_KeInitializeQueue), X360_HK(KeInsertQueue, k_KeInsertQueue),
    X360_HK(KeInsertHeadQueue, k_KeInsertHeadQueue), X360_HK(KeRemoveQueue, k_KeRemoveQueue),
    X360_HK(KeRundownQueue, k_KeRundownQueue),
    X360_HK(NtCreateIoCompletion, k_NtCreateIoCompletion), X360_HK(NtSetIoCompletion, k_NtSetIoCompletion),
    X360_HK(NtRemoveIoCompletion, k_NtRemoveIoCompletion), X360_HK(NtQueryIoCompletion, k_NtQueryIoCompletion),
    X360_HK(InterlockedPushEntrySList, k_InterlockedPushEntrySList),
    X360_HK(InterlockedPopEntrySList, k_InterlockedPopEntrySList),
    X360_HK(InterlockedFlushSList, k_InterlockedFlushSList),
    X360_HK(ExInitializeReadWriteLock, k_ExInitializeReadWriteLock),
    X360_HK(ExAcquireReadWriteLockExclusive, k_ExAcquireReadWriteLockExclusive),
    X360_HK(ExTryToAcquireReadWriteLockExclusive, k_ExTryToAcquireReadWriteLockExclusive),
    X360_HK(ExAcquireReadWriteLockShared, k_ExAcquireReadWriteLockShared),
    X360_HK(ExTryToAcquireReadWriteLockShared, k_ExTryToAcquireReadWriteLockShared),
    X360_HK(ExReleaseReadWriteLock, k_ExReleaseReadWriteLock),
    X360_HK(NtProtectVirtualMemory, k_NtProtectVirtualMemory), X360_HK(MmSetAddressProtect, k_MmSetAddressProtect),
    X360_HK(MmQueryAddressProtect, k_MmQueryAddressProtect), X360_HK(MmIsAddressValid, k_MmIsAddressValid),
};
const unsigned x360_tab_kdpc_n = sizeof x360_tab_kdpc / sizeof x360_tab_kdpc[0];
