/* ps2_hle.c -- the PS2 spike's HLE of the EE kernel (XBA-226): the syscalls a PS2SDK program makes, in C. No BIOS
 * image is ever loaded.
 *
 * A SYSCALL runs with the call number in v1 (negative: the interrupt-context `i` form, same behaviour here but never
 * a thread switch), arguments in a0-a3 / t0-t3 (EABI: r4-r11), the result in v0. What is there:
 * - start-up: SetupThread (the main thread's stack, argc = 0), SetupHeap / EndOfHeap, FlushCache, GetMemorySize,
 *   the OSD configuration (all zero: English, 4:3, ...), the TLB calls (no TLB modelled), Exit;
 * - SetSyscall (`setup`): the handler address is recorded and the syscall then runs it as guest code (ra = a stub
 *   that returns to the caller), which is how PS2SDK installs its kernel patches (kCopy, GetEntryAddress, the alarm
 *   and TLB patches): they run as written, copying their code into kernel memory;
 * - threads: a strict-priority scheduler (0 is the highest) that switches at syscalls only: CreateThread /
 *   StartThread / ExitThread / ExitDeleteThread / TerminateThread / DeleteThread, ChangeThreadPriority,
 *   RotateThreadReadyQueue, GetThreadId, ReferThreadStatus, SleepThread / WakeupThread, Suspend / ResumeThread
 *   (suspension only marks the thread);
 * - semaphores: CreateSema / DeleteSema / SignalSema / WaitSema (blocking) / PollSema / ReferSemaStatus;
 * - the SIF: SifSetReg / SifGetReg read and write a register file nobody answers (no IOP), SifSetDma returns a
 *   transfer id and moves nothing, SifDmaStat says it is done;
 * - interrupt and DMA handlers: accepted and never called (no interrupts in the spike); alarms: refused.
 * Any other call returns 0 and is reported once (`ps2: HLE: syscall N not implemented`). When every thread waits,
 * nothing can wake one (no interrupt, no alarm): the machine stops with a deadlock message. */
#include <stdio.h>
#include <string.h>

#include "ps2_internal.h"

#define A0 (m->h.r[4])
#define A1 (m->h.r[5])
#define A2 (m->h.r[6])
#define A3 (m->h.r[7])
#define T0 (m->h.r[8])
#define V0 (m->h.r[2])

#define KE_OK 0
#define KE_ERROR (-1)
#define KE_ILLEGAL_THID (-406)
#define KE_UNKNOWN_THID (-407)
#define KE_NOT_DORMANT (-414)
#define KE_UNKNOWN_SEMID (-408)
#define KE_SEMA_ZERO (-419)

static void ret(ps2_machine *m, int64_t v)
{
    V0 = (uint64_t)v;
}

void ps2_hle_init(ps2_machine *m)
{
    /* thread exit stub: li v1, ExitThread (0x23) ; syscall ; and the SetSyscall return stub */
    ps2_w32(m, PS2_STUB_THREAD_EXIT, 0x24030023u);
    ps2_w32(m, PS2_STUB_THREAD_EXIT + 4, 0x0000000Cu);
    ps2_w32(m, PS2_STUB_SYSRET, 0x24030000u | PS2_SC_SYSRET);
    ps2_w32(m, PS2_STUB_SYSRET + 4, 0x0000000Cu);
    ps2_thread *t = &m->threads[1];
    memset(t, 0, sizeof *t);
    t->state = TH_RUNNING;
    t->prio = t->init_prio = 0;
    t->func = m->entry;
    m->cur = 1;
    m->cop0[12] = 0x70030C13u; /* Status as a user program sees it: EIE, user mode, COP0 / COP1 usable */
    m->cop0[15] = 0x00002E20u; /* PRId: R5900 */
}

/* ---- threads ------------------------------------------------------------------------------------------------------ */

static void save_ctx(ps2_machine *m, ps2_thread *t)
{
    ps2_rc_head *h = &m->h;
    memcpy(t->r, h->r, sizeof t->r);
    memcpy(t->rh, h->rh, sizeof t->rh);
    memcpy(t->f, h->f, sizeof t->f);
    t->hi = h->hi;
    t->lo = h->lo;
    t->hi1 = h->hi1;
    t->lo1 = h->lo1;
    t->sa = h->sa;
    t->acc = h->acc;
    t->fcr31 = h->fcr31;
    t->pc = h->pc;
    t->next_pc = h->next_pc;
}

static void load_ctx(ps2_machine *m, ps2_thread *t)
{
    ps2_rc_head *h = &m->h;
    memcpy(h->r, t->r, sizeof t->r);
    memcpy(h->rh, t->rh, sizeof t->rh);
    memcpy(h->f, t->f, sizeof t->f);
    h->hi = t->hi;
    h->lo = t->lo;
    h->hi1 = t->hi1;
    h->lo1 = t->lo1;
    h->sa = t->sa;
    h->acc = t->acc;
    h->fcr31 = t->fcr31;
    h->pc = t->pc;
    h->next_pc = t->next_pc;
}

static bool runnable(const ps2_thread *t)
{
    return (t->state == TH_READY || t->state == TH_RUNNING) && !t->suspended;
}

/* Is thread a a better choice than thread b: a higher priority (lower number), the running thread against its
 * equals, else the one that has waited longer. */
static bool better(ps2_machine *m, int a, int b)
{
    const ps2_thread *x = &m->threads[a], *y = &m->threads[b];
    if (x->prio != y->prio)
        return x->prio < y->prio;
    if (a == m->cur && x->state == TH_RUNNING)
        return true;
    if (b == m->cur && y->state == TH_RUNNING)
        return false;
    return x->seq < y->seq;
}

/* Runs the highest-priority runnable thread. */
static void schedule(ps2_machine *m)
{
    int best = -1;
    for (int i = 1; i < PS2_MAX_THREADS; i++)
        if (runnable(&m->threads[i]) && (best < 0 || better(m, i, best)))
            best = i;
    if (best < 0) {
        ps2_fail(m, "every thread waits (deadlock: no interrupts or alarms in the spike)");
        return;
    }
    ps2_thread *cur = &m->threads[m->cur];
    if (best == m->cur) {
        cur->state = TH_RUNNING;
        return;
    }
    if (cur->state != TH_FREE) {
        save_ctx(m, cur);
        if (cur->state == TH_RUNNING) {
            cur->state = TH_READY;
            cur->seq = ++m->seq;
        }
    }
    ps2_thread *n = &m->threads[best];
    load_ctx(m, n);
    n->state = TH_RUNNING;
    m->cur = best;
    m->switched = true;
    if (m->verbose)
        fprintf(stderr, "ps2: HLE: thread %d runs (pc 0x%08X)\n", best, m->h.pc);
}

static ps2_thread *thread_of(ps2_machine *m, int64_t id)
{
    if (id == 0)
        id = m->cur;
    if (id < 1 || id >= PS2_MAX_THREADS || m->threads[id].state == TH_FREE)
        return NULL;
    return &m->threads[id];
}

static void create_thread(ps2_machine *m)
{
    uint32_t p = (uint32_t)A0;
    int id = -1;
    for (int i = 2; i < PS2_MAX_THREADS; i++)
        if (m->threads[i].state == TH_FREE) {
            id = i;
            break;
        }
    if (id < 0) {
        ret(m, KE_ERROR);
        return;
    }
    ps2_thread *t = &m->threads[id];
    memset(t, 0, sizeof *t);
    t->state = TH_DORMANT;
    t->func = ps2_r32(m, p + 4);
    t->stack = ps2_r32(m, p + 8);
    t->stack_size = ps2_r32(m, p + 12);
    t->gp = ps2_r32(m, p + 16);
    t->init_prio = t->prio = (int)ps2_r32(m, p + 20);
    t->attr = ps2_r32(m, p + 28);
    t->option = ps2_r32(m, p + 32);
    ret(m, id);
}

static void start_thread(ps2_machine *m, int64_t id, uint32_t arg)
{
    ps2_thread *t = id >= 2 && id < PS2_MAX_THREADS ? &m->threads[id] : NULL;
    if (!t || t->state == TH_FREE) {
        ret(m, KE_UNKNOWN_THID);
        return;
    }
    if (t->state != TH_DORMANT) {
        ret(m, KE_NOT_DORMANT);
        return;
    }
    memset(t->r, 0, sizeof t->r);
    memset(t->rh, 0, sizeof t->rh);
    memset(t->f, 0, sizeof t->f);
    t->hi = t->lo = t->hi1 = t->lo1 = 0;
    t->sa = t->acc = t->fcr31 = 0;
    t->r[4] = arg;
    t->r[28] = t->gp;
    t->r[29] = (t->stack + t->stack_size - 0x40u) & ~15u;
    t->r[31] = PS2_STUB_THREAD_EXIT;
    t->pc = t->func;
    t->next_pc = t->func + 4;
    t->prio = t->init_prio;
    t->state = TH_READY;
    t->wait = WAIT_NONE;
    t->seq = ++m->seq;
    ret(m, (int64_t)id);
}

static void wait_here(ps2_machine *m, int wait, int id)
{
    ps2_thread *t = &m->threads[m->cur];
    t->state = TH_WAITING;
    t->wait = wait;
    t->wait_id = id;
}

static void wake(ps2_machine *m, ps2_thread *t, int64_t v0)
{
    t->state = TH_READY;
    t->wait = WAIT_NONE;
    t->seq = ++m->seq;
    t->r[2] = (uint64_t)v0;
}

static void refer_thread(ps2_machine *m, int64_t id, uint32_t p)
{
    ps2_thread *t = thread_of(m, id);
    if (!t) {
        ret(m, KE_UNKNOWN_THID);
        return;
    }
    static const uint32_t status[] = {0, 0x10, 0x02, 0x01, 0x04}; /* FREE DORMANT READY RUN WAIT */
    uint32_t st = status[t->state] | (t->suspended ? 0x08u : 0);
    uint32_t vals[12] = {st,          t->func, t->stack, t->stack_size, t->gp, (uint32_t)t->init_prio,
                         (uint32_t)t->prio, t->attr, t->option, (uint32_t)t->wait, (uint32_t)t->wait_id,
                         (uint32_t)t->wakeups};
    for (int k = 0; k < 12 && p; k++)
        ps2_w32(m, p + 4u * (uint32_t)k, vals[k]);
    ret(m, t->state == TH_RUNNING ? 1 : t->state == TH_READY ? 2 : t->state == TH_WAITING ? 4 : 0x10);
}

/* ---- semaphores ---------------------------------------------------------------------------------------------------- */

static ps2_sema *sema_of(ps2_machine *m, int64_t id)
{
    if (id < 0 || id >= PS2_MAX_SEMAS || !m->semas[id].used)
        return NULL;
    return &m->semas[id];
}

static void create_sema(ps2_machine *m)
{
    uint32_t p = (uint32_t)A0;
    for (int i = 1; i < PS2_MAX_SEMAS; i++) {
        ps2_sema *s = &m->semas[i];
        if (s->used)
            continue;
        memset(s, 0, sizeof *s);
        s->used = true;
        s->max = (int32_t)ps2_r32(m, p + 4);
        s->init = (int32_t)ps2_r32(m, p + 8);
        s->count = s->init;
        s->attr = ps2_r32(m, p + 16);
        s->option = ps2_r32(m, p + 20);
        ret(m, i);
        return;
    }
    ret(m, KE_ERROR);
}

static void signal_sema(ps2_machine *m, int64_t id, bool from_irq)
{
    ps2_sema *s = sema_of(m, id);
    if (!s) {
        ret(m, KE_UNKNOWN_SEMID);
        return;
    }
    ret(m, id);
    ps2_thread *w = NULL;
    for (int i = 1; i < PS2_MAX_THREADS; i++) {
        ps2_thread *t = &m->threads[i];
        if (t->state == TH_WAITING && t->wait == WAIT_SEMA && t->wait_id == id &&
            (!w || t->prio < w->prio || (t->prio == w->prio && t->seq < w->seq)))
            w = t;
    }
    if (w) {
        wake(m, w, id);
        if (!from_irq)
            schedule(m);
    } else {
        s->count++;
    }
}

/* ---- the syscall ---------------------------------------------------------------------------------------------------- */

static void unimplemented(ps2_machine *m, int32_t code)
{
    for (int i = 0; i < m->nunimpl; i++)
        if (m->unimpl[i] == code)
            return;
    if (m->nunimpl < (int)(sizeof m->unimpl / sizeof m->unimpl[0]))
        m->unimpl[m->nunimpl++] = code;
    fprintf(stderr, "ps2: HLE: syscall %d (0x%X) not implemented, returns 0 (from 0x%08X)\n", code,
            (unsigned)(code < 0 ? -code : code), m->h.pc - 4);
}

void ps2_hle_syscall(ps2_machine *m, int32_t code)
{
    ps2_rc_head *h = &m->h;
    m->switched = false;
    int32_t n = code < 0 ? -code : code;
    bool irq = code < 0;
    if (m->verbose > 1)
        fprintf(stderr, "ps2: HLE: syscall %d a0 %08X a1 %08X a2 %08X (thread %d, from 0x%08X)\n", code,
                (unsigned)A0, (unsigned)A1, (unsigned)A2, m->cur, h->pc - 4);
    if (code == PS2_SC_SYSRET) {
        if (m->nsysframes <= 0) {
            ps2_fail(m, "SetSyscall return stub reached outside a handler");
            return;
        }
        ps2_sysframe *f = &m->sysframes[--m->nsysframes];
        h->pc = f->pc;
        h->next_pc = f->next_pc;
        h->r[31] = f->ra;
        return;
    }
    if (n < PS2_MAX_SYSCALL && m->syscall_override[n] && n != 0x74) {
        if (m->nsysframes >= PS2_SYSCALL_DEPTH) {
            ps2_fail(m, "SetSyscall handlers nested too deep (syscall %d)", code);
            return;
        }
        ps2_sysframe *f = &m->sysframes[m->nsysframes++];
        f->pc = h->pc;
        f->next_pc = h->next_pc;
        f->ra = h->r[31];
        h->r[31] = PS2_STUB_SYSRET;
        h->pc = m->syscall_override[n];
        h->next_pc = h->pc + 4;
        return;
    }
    switch (n) {
    case 0x02: ret(m, 0); return; /* SetGsCrt */
    case 0x04:                    /* Exit */
        m->exited = true;
        m->exit_code = (int)(int32_t)A0;
        return;
    case 0x10: case 0x12: ret(m, 1); return; /* AddIntcHandler / AddDmacHandler: never called */
    case 0x11: case 0x13: ret(m, 0); return; /* Remove... */
    case 0x14: case 0x15: case 0x16: case 0x17: case 0x1A: case 0x1B: case 0x1C: case 0x1D:
        ret(m, 1); /* (i)Enable / Disable Intc / Dmac: no interrupt ever comes */
        return;
    case 0x18: case 0x1E: ret(m, KE_ERROR); return; /* SetAlarm: no timers in the spike */
    case 0x19: case 0x1F: ret(m, KE_ERROR); return;
    case 0x20: create_thread(m); return;
    case 0x21: { /* DeleteThread */
        ps2_thread *t = thread_of(m, (int64_t)A0);
        if (!t || (int64_t)A0 == m->cur || t->state != TH_DORMANT) {
            ret(m, t ? KE_NOT_DORMANT : KE_UNKNOWN_THID);
            return;
        }
        t->state = TH_FREE;
        ret(m, (int64_t)A0);
        return;
    }
    case 0x22: start_thread(m, (int64_t)A0, (uint32_t)A1); if (V0 < 0x80000000u) schedule(m); return;
    case 0x23: case 0x24: { /* ExitThread / ExitDeleteThread */
        ps2_thread *t = &m->threads[m->cur];
        if (m->cur == 1) {
            m->exited = true; /* the main thread returning: the program is over */
            return;
        }
        t->state = n == 0x24 ? TH_FREE : TH_DORMANT;
        schedule(m);
        return;
    }
    case 0x25: case 0x26: { /* (i)TerminateThread */
        ps2_thread *t = thread_of(m, (int64_t)A0);
        if (!t || (int64_t)A0 == m->cur) {
            ret(m, KE_ILLEGAL_THID);
            return;
        }
        t->state = TH_DORMANT;
        ret(m, (int64_t)A0);
        return;
    }
    case 0x29: case 0x2A: { /* (i)ChangeThreadPriority */
        ps2_thread *t = thread_of(m, (int64_t)A0);
        if (!t) {
            ret(m, KE_UNKNOWN_THID);
            return;
        }
        int old = t->prio;
        t->prio = (int)(int32_t)A1;
        ret(m, old);
        if (!irq)
            schedule(m);
        return;
    }
    case 0x2B: case 0x2C: { /* (i)RotateThreadReadyQueue: the running thread goes behind its equals */
        ps2_thread *t = &m->threads[m->cur];
        ret(m, (int64_t)A0);
        if (t->prio == (int)(int32_t)A0 && !irq) {
            t->state = TH_READY;
            t->seq = ++m->seq;
            schedule(m);
        }
        return;
    }
    case 0x2F: ret(m, m->cur); return; /* (i)GetThreadId */
    case 0x30: case 0x31: refer_thread(m, (int64_t)A0, (uint32_t)A1); return;
    case 0x32: { /* SleepThread */
        ps2_thread *t = &m->threads[m->cur];
        ret(m, m->cur);
        if (t->wakeups > 0) {
            t->wakeups--;
            return;
        }
        wait_here(m, WAIT_SLEEP, 0);
        schedule(m);
        return;
    }
    case 0x33: case 0x34: { /* (i)WakeupThread */
        ps2_thread *t = thread_of(m, (int64_t)A0);
        if (!t || (int64_t)A0 == m->cur) {
            ret(m, KE_ILLEGAL_THID);
            return;
        }
        ret(m, (int64_t)A0);
        if (t->state == TH_WAITING && t->wait == WAIT_SLEEP) {
            wake(m, t, (int64_t)A0);
            if (!irq)
                schedule(m);
        } else {
            t->wakeups++;
        }
        return;
    }
    case 0x35: case 0x36: { /* (i)CancelWakeupThread */
        ps2_thread *t = thread_of(m, (int64_t)A0);
        ret(m, t ? t->wakeups : KE_UNKNOWN_THID);
        if (t)
            t->wakeups = 0;
        return;
    }
    case 0x37: case 0x38: case 0x39: case 0x3A: { /* (i)Suspend / Resume Thread */
        ps2_thread *t = thread_of(m, (int64_t)A0);
        if (!t || (int64_t)A0 == m->cur) {
            ret(m, KE_ILLEGAL_THID);
            return;
        }
        t->suspended = n <= 0x38;
        ret(m, (int64_t)A0);
        if (!irq)
            schedule(m);
        return;
    }
    case 0x3C: { /* SetupThread(gp, stack, stack_size, args, root): the main thread's stack pointer */
        uint32_t stack = (uint32_t)A1, size = (uint32_t)A2, args = (uint32_t)A3;
        if (stack == 0xFFFFFFFFu)
            stack = PS2_RAM_SIZE - size;
        m->stack_lo = stack;
        m->stack_hi = stack + size;
        m->threads[1].stack = stack;
        m->threads[1].stack_size = size;
        m->threads[1].gp = (uint32_t)A0;
        if (args)
            ps2_w32(m, args, 0); /* argc: no arguments */
        ret(m, (int64_t)(int32_t)(m->stack_hi - 0x2A0u));
        return;
    }
    case 0x3D: { /* SetupHeap(start, size) */
        uint32_t start = (uint32_t)A0, size = (uint32_t)A1;
        m->heap_end = size == 0xFFFFFFFFu ? m->stack_lo : start + size;
        ret(m, (int64_t)(int32_t)m->heap_end);
        return;
    }
    case 0x3E: ret(m, (int64_t)(int32_t)m->heap_end); return; /* EndOfHeap */
    case 0x40: create_sema(m); return;
    case 0x41: { /* DeleteSema */
        ps2_sema *s = sema_of(m, (int64_t)A0);
        if (!s) {
            ret(m, KE_UNKNOWN_SEMID);
            return;
        }
        s->used = false;
        ret(m, (int64_t)A0);
        for (int i = 1; i < PS2_MAX_THREADS; i++) {
            ps2_thread *t = &m->threads[i];
            if (t->state == TH_WAITING && t->wait == WAIT_SEMA && t->wait_id == (int)A0)
                wake(m, t, -425); /* KE_WAIT_DELETE */
        }
        schedule(m);
        return;
    }
    case 0x42: case 0x43: signal_sema(m, (int64_t)A0, irq); return;
    case 0x44: { /* WaitSema */
        ps2_sema *s = sema_of(m, (int64_t)A0);
        if (!s) {
            ret(m, KE_UNKNOWN_SEMID);
            return;
        }
        ret(m, (int64_t)A0);
        if (s->count > 0) {
            s->count--;
            return;
        }
        wait_here(m, WAIT_SEMA, (int)A0);
        schedule(m);
        return;
    }
    case 0x45: case 0x46: { /* (i)PollSema */
        ps2_sema *s = sema_of(m, (int64_t)A0);
        if (!s) {
            ret(m, KE_UNKNOWN_SEMID);
            return;
        }
        if (s->count > 0) {
            s->count--;
            ret(m, (int64_t)A0);
        } else {
            ret(m, KE_SEMA_ZERO);
        }
        return;
    }
    case 0x47: case 0x48: { /* (i)ReferSemaStatus */
        ps2_sema *s = sema_of(m, (int64_t)A0);
        uint32_t p = (uint32_t)A1;
        if (!s) {
            ret(m, KE_UNKNOWN_SEMID);
            return;
        }
        int waiting = 0;
        for (int i = 1; i < PS2_MAX_THREADS; i++)
            waiting += m->threads[i].state == TH_WAITING && m->threads[i].wait == WAIT_SEMA &&
                       m->threads[i].wait_id == (int)A0;
        uint32_t vals[6] = {(uint32_t)s->count, (uint32_t)s->max, (uint32_t)s->init, (uint32_t)waiting, s->attr,
                            s->option};
        for (int k = 0; k < 6; k++)
            ps2_w32(m, p + 4u * (uint32_t)k, vals[k]);
        ret(m, (int64_t)A0);
        return;
    }
    case 0x4A: m->osd_config = ps2_r32(m, (uint32_t)A0); ret(m, 0); return; /* SetOsdConfigParam */
    case 0x4B: ps2_w32(m, (uint32_t)A0, m->osd_config); ret(m, 0); return; /* GetOsdConfigParam */
    case 0x6E: ret(m, 0); return;                                           /* SetOsdConfigParam2 */
    case 0x6F: {                                                            /* GetOsdConfigParam2(buf, size, off) */
        uint8_t *p = ps2_ptr(m, (uint32_t)A0, (uint32_t)A1);
        if (p)
            memset(p, 0, (size_t)(uint32_t)A1);
        ret(m, 0);
        return;
    }
    case 0x56: case 0x82: ret(m, 0); return; /* _SetTLBEntry, _InitTLB: no TLB modelled (fixed map) */
    case 0x5A: ret(m, 0); return;            /* Copy (kCopy) before a SetSyscall: nothing to copy to */
    case 0x5B: ret(m, 0); return;            /* GetEntryAddress before a SetSyscall */
    case 0x64: ret(m, 0); return;            /* FlushCache: no caches modelled */
    case 0x70: ret(m, 0); return;            /* GsGetIMR */
    case 0x71: ret(m, 0); return;            /* GsPutIMR */
    case 0x73: ret(m, 0); return;            /* SetVSyncFlag */
    case 0x74:                               /* SetSyscall(n, handler) */
        if ((uint32_t)A0 < PS2_MAX_SYSCALL)
            m->syscall_override[(uint32_t)A0] = (uint32_t)A1;
        ret(m, 0);
        return;
    case 0x76: ret(m, 0); return; /* SifDmaStat: done */
    case 0x77: ret(m, 1); return; /* SifSetDma: a transfer id, nothing moves (no IOP) */
    case 0x78: ret(m, 0); return; /* SifSetDChain */
    case 0x79:                    /* SifSetReg(reg, value) */
        if ((uint32_t)A0 < 32)
            ps2_w32(m, 0x1000F200u + 0x10u * ((uint32_t)A0 & 7), (uint32_t)A1);
        ret(m, 0);
        return;
    case 0x7A: ret(m, (uint32_t)A0 < 8 ? (int64_t)(int32_t)ps2_r32(m, 0x1000F200u + 0x10u * (uint32_t)A0) : 0);
        return; /* SifGetReg */
    case 0x7C: ret(m, 0); return;                         /* Deci2Call */
    case 0x7E: ret(m, 0); return;                         /* MachineType */
    case 0x7F: ret(m, (int64_t)PS2_RAM_SIZE); return;     /* GetMemorySize */
    default: unimplemented(m, code); ret(m, 0); return;
    }
}
