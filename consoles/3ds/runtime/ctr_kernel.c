/* ctr_kernel.c -- the Horizon kernel calls of the 3DS spike (XBA-228), high-level: kernel objects and the handle
 * table, threads and their scheduler, waits (events, mutexes, semaphores, threads, timeouts), address arbiters,
 * memory (svcControlMemory, shared memory blocks), IPC to the HLE services (ctr_services.c) and the resource limits
 * libctru sizes its heaps from. Register ABI and result codes: 3dbrew « SVC » and « Error codes »; behaviour as
 * libctru (2.7) relies on it.
 *
 * Threads. One core: the running thread keeps the CPU until it blocks, yields (svcSleepThread) or a thread of a
 * higher priority (lower number) becomes ready, like the application core's scheduler. Everything happens at an
 * instruction boundary (a kernel call is always run by the interpreter), so a switch is a copy of the CPU struct:
 * h->cpu is the running thread's, threads[i].cpu everyone else's. Time is the instruction clock; when no thread is
 * ready the core skips to the next timeout or VBlank (ctr_core.c), so a run is deterministic. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ctr_internal.h"

#define CPU (m->h.cpu)
#define H_CUR_THREAD 0xFFFF8000u
#define H_CUR_PROCESS 0xFFFF8001u

void ctr_stop(ctr_machine *m, const char *fmt, ...)
{
    if (m->error[0])
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m->error, sizeof m->error, fmt, ap);
    va_end(ap);
}

/* ---- objects and handles ---- */

ctr_obj *ctr_obj_new(ctr_machine *m, int type, const char *name)
{
    for (int i = 0; i < CTR_MAX_OBJECTS; i++) {
        ctr_obj *o = &m->objs[i];
        if (o->type == CTR_OBJ_FREE) {
            memset(o, 0, sizeof *o);
            o->type = type;
            snprintf(o->name, sizeof o->name, "%s", name ? name : "");
            return o;
        }
    }
    ctr_stop(m, "out of kernel objects");
    return NULL;
}

uint32_t ctr_handle_new(ctr_machine *m, ctr_obj *o)
{
    if (!o)
        return 0;
    for (int i = 1; i < CTR_MAX_HANDLES; i++) {
        if (!m->handles[i]) {
            m->handles[i] = o;
            return 0x10000u | (uint32_t)i << 4; /* any value that is not 0 nor a pseudo-handle */
        }
    }
    ctr_stop(m, "out of handles");
    return 0;
}

ctr_obj *ctr_handle_get(ctr_machine *m, uint32_t handle)
{
    if (handle == H_CUR_PROCESS)
        return m->process;
    if (handle == H_CUR_THREAD)
        return m->cur >= 0 ? m->threads[m->cur].obj : NULL;
    if ((handle & 0xFFFF000Fu) != 0x10000u)
        return NULL;
    uint32_t i = (handle >> 4) & 0xFFF;
    return i < CTR_MAX_HANDLES ? m->handles[i] : NULL;
}

static void handle_close(ctr_machine *m, uint32_t handle)
{
    if ((handle & 0xFFFF000Fu) != 0x10000u)
        return;
    uint32_t i = (handle >> 4) & 0xFFF;
    if (i < CTR_MAX_HANDLES)
        m->handles[i] = NULL;
}

uint8_t *ctr_shm_ptr(ctr_machine *m, ctr_obj *shm)
{
    return shm->u.shm.addr ? m->h.mem + shm->u.shm.addr : shm->u.shm.buf;
}

/* ---- threads ---- */

static ctr_cpu *cpu_of(ctr_machine *m, int t)
{
    return t == m->cur ? &m->h.cpu : &m->threads[t].cpu;
}

static int thread_new(ctr_machine *m, uint32_t entry, uint32_t arg, uint32_t sp, int prio)
{
    for (int i = 0; i < CTR_MAX_THREADS; i++) {
        ctr_thread *t = &m->threads[i];
        if (t->state != CTR_TH_FREE)
            continue;
        memset(t, 0, sizeof *t);
        t->obj = ctr_obj_new(m, CTR_OBJ_THREAD, "thread");
        if (!t->obj)
            return -1;
        t->obj->u.th.thread = i;
        t->state = CTR_TH_READY;
        t->prio = prio;
        t->id = m->next_thread_id++;
        t->wake_at = UINT64_MAX;
        t->cpu.r[0] = arg;
        t->cpu.r[13] = sp;
        t->cpu.cpsr_ctl = 0x10 | ((entry & 1) ? 0x20u : 0u);
        t->cpu.r[15] = (entry & 1) ? (entry & ~1u) + 4 : (entry & ~3u) + 8;
        t->cpu.tls = CTR_TLS_BASE + 0x200u * (uint32_t)i;
        t->cpu.fpscr = 0x03C00000u;
        memset(m->h.mem + t->cpu.tls, 0, 0x200);
        return i;
    }
    return -1;
}

static void switch_to(ctr_machine *m, int t)
{
    if (t == m->cur)
        return;
    if (m->cur >= 0)
        m->threads[m->cur].cpu = m->h.cpu;
    m->cur = t;
    if (t >= 0) {
        m->h.cpu = m->threads[t].cpu;
        m->h.cpu.excl = 0;
    }
    m->branched = true;
    if (m->verbose >= 2)
        fprintf(stderr, "ctr: switch to thread %d at 0x%08X\n", t, t >= 0 ? m->h.cpu.r[15] - 8 : 0);
}

static void schedule_yield(ctr_machine *m, bool yield)
{
    m->resched = false;
    int best = -1;
    for (int i = 0; i < CTR_MAX_THREADS; i++)
        if (m->threads[i].state == CTR_TH_READY && (best < 0 || m->threads[i].prio < m->threads[best].prio))
            best = i;
    if (best < 0) {
        switch_to(m, -1);
        return;
    }
    int cur = m->cur;
    if (cur >= 0 && m->threads[cur].state == CTR_TH_READY) {
        if (m->threads[cur].prio < m->threads[best].prio || (m->threads[cur].prio == m->threads[best].prio && !yield))
            return;
        if (yield && m->threads[cur].prio == m->threads[best].prio) { /* round robin among equals */
            for (int k = 1; k <= CTR_MAX_THREADS; k++) {
                int i = (cur + k) % CTR_MAX_THREADS;
                if (m->threads[i].state == CTR_TH_READY && m->threads[i].prio == m->threads[cur].prio) {
                    switch_to(m, i);
                    return;
                }
            }
            return;
        }
    }
    switch_to(m, best);
}

void ctr_schedule(ctr_machine *m)
{
    schedule_yield(m, false);
}

/* ---- waiting ---- */

static bool obj_ready(ctr_machine *m, ctr_obj *o, int t)
{
    switch (o->type) {
    case CTR_OBJ_EVENT:
    case CTR_OBJ_TIMER:
        return o->u.ev.signaled;
    case CTR_OBJ_MUTEX:
        return o->u.mtx.owner < 0 || o->u.mtx.owner == t;
    case CTR_OBJ_SEMAPHORE:
        return o->u.sem.count > 0;
    case CTR_OBJ_THREAD:
        return m->threads[o->u.th.thread].state == CTR_TH_DEAD;
    case CTR_OBJ_PROCESS:
        return m->exited;
    default:
        return false; /* sessions, ports, shared memory: never signaled here */
    }
}

static void obj_acquire(ctr_obj *o, int t)
{
    switch (o->type) {
    case CTR_OBJ_EVENT:
    case CTR_OBJ_TIMER:
        if (o->u.ev.reset == CTR_RESET_ONESHOT)
            o->u.ev.signaled = false;
        break;
    case CTR_OBJ_MUTEX:
        o->u.mtx.owner = t;
        o->u.mtx.count++;
        break;
    case CTR_OBJ_SEMAPHORE:
        o->u.sem.count--;
        break;
    default:
        break;
    }
}

/* A sync wait that can complete now: acquires, returns the index (wait all: 0), else -1. */
static int try_sync(ctr_machine *m, int t)
{
    ctr_thread *th = &m->threads[t];
    if (th->wait_all) {
        for (int i = 0; i < th->nwaits; i++)
            if (!obj_ready(m, th->waits[i], t))
                return -1;
        for (int i = 0; i < th->nwaits; i++)
            obj_acquire(th->waits[i], t);
        return 0;
    }
    for (int i = 0; i < th->nwaits; i++) {
        if (obj_ready(m, th->waits[i], t)) {
            obj_acquire(th->waits[i], t);
            return i;
        }
    }
    return -1;
}

static void wake(ctr_machine *m, int t, uint32_t result, int32_t index)
{
    ctr_thread *th = &m->threads[t];
    ctr_cpu *c = cpu_of(m, t);
    c->r[0] = result;
    if (th->wait_n)
        c->r[1] = (uint32_t)index;
    th->state = CTR_TH_READY;
    th->wait = CTR_WAIT_NONE;
    th->wake_at = UINT64_MAX;
    if (m->cur < 0 || th->prio < m->threads[m->cur].prio)
        m->resched = true;
}

/* Wakes the sync waiters that can go on now, by priority (a released mutex goes to the best waiter). */
static void wake_sync_waiters(ctr_machine *m)
{
    for (bool again = true; again;) {
        again = false;
        int best = -1, idx = -1;
        for (int i = 0; i < CTR_MAX_THREADS; i++) {
            ctr_thread *th = &m->threads[i];
            if (th->state != CTR_TH_WAIT || th->wait != CTR_WAIT_SYNC)
                continue;
            if (best >= 0 && th->prio >= m->threads[best].prio)
                continue;
            /* readiness only; acquire below for the chosen one */
            bool ok = th->wait_all;
            for (int k = 0; k < th->nwaits; k++) {
                bool r = obj_ready(m, th->waits[k], i);
                if (th->wait_all && !r) {
                    ok = false;
                    break;
                }
                if (!th->wait_all && r) {
                    ok = true;
                    break;
                }
            }
            if (ok)
                best = i;
        }
        if (best >= 0) {
            idx = try_sync(m, best);
            if (idx >= 0) {
                wake(m, best, CTR_RES_OK, idx);
                again = true;
            }
        }
    }
}

void ctr_event_signal(ctr_machine *m, ctr_obj *ev)
{
    ev->u.ev.signaled = true;
    wake_sync_waiters(m);
    if (ev->u.ev.reset == CTR_RESET_PULSE)
        ev->u.ev.signaled = false;
}

uint64_t ctr_next_timeout(const ctr_machine *m)
{
    uint64_t t = UINT64_MAX;
    for (int i = 0; i < CTR_MAX_THREADS; i++)
        if (m->threads[i].state == CTR_TH_WAIT && m->threads[i].wake_at < t)
            t = m->threads[i].wake_at;
    return t;
}

void ctr_timeouts(ctr_machine *m)
{
    for (int i = 0; i < CTR_MAX_THREADS; i++) {
        ctr_thread *th = &m->threads[i];
        if (th->state != CTR_TH_WAIT || th->wake_at > m->h.now)
            continue;
        if (th->wait == CTR_WAIT_SLEEP)
            wake(m, i, CTR_RES_OK, 0);
        else
            wake(m, i, CTR_RES_TIMEOUT, -1);
    }
}

static uint64_t ns_to_cycles(int64_t ns)
{
    if (ns < 0)
        return UINT64_MAX;
    uint64_t n = (uint64_t)ns;
    return n / 1000000000u * CTR_CPU_HZ + n % 1000000000u * CTR_CPU_HZ / 1000000000u;
}

/* Blocks the running thread (its result registers are written when it wakes). */
static void block(ctr_machine *m, int kind, int64_t timeout_ns)
{
    ctr_thread *th = &m->threads[m->cur];
    th->state = CTR_TH_WAIT;
    th->wait = kind;
    uint64_t d = ns_to_cycles(timeout_ns);
    th->wake_at = d == UINT64_MAX ? UINT64_MAX : m->h.now + d;
    ctr_schedule(m);
}

/* ---- the kernel calls ---- */

static void sync_wait(ctr_machine *m, const uint32_t *handles, int n, bool all, bool wait_n, int64_t timeout)
{
    ctr_thread *th = &m->threads[m->cur];
    if (n > CTR_MAX_WAIT || n < 0) {
        CPU.r[0] = 0xE0E01BFDu; /* out of range */
        return;
    }
    for (int i = 0; i < n; i++) {
        ctr_obj *o = ctr_handle_get(m, handles[i]);
        if (!o) {
            CPU.r[0] = CTR_RES_INVALID_HANDLE;
            return;
        }
        th->waits[i] = o;
    }
    th->nwaits = n;
    th->wait_all = all;
    th->wait_n = wait_n;
    int idx = try_sync(m, m->cur);
    if (idx >= 0) {
        CPU.r[0] = CTR_RES_OK;
        if (wait_n)
            CPU.r[1] = (uint32_t)idx;
        return;
    }
    if (timeout == 0) {
        CPU.r[0] = CTR_RES_TIMEOUT;
        return;
    }
    block(m, CTR_WAIT_SYNC, timeout);
}

static void arbitrate(ctr_machine *m, ctr_obj *arb, uint32_t addr, uint32_t type, int32_t value, int64_t timeout)
{
    if (!arb || arb->type != CTR_OBJ_ARBITER) {
        CPU.r[0] = CTR_RES_INVALID_HANDLE;
        return;
    }
    if (type == 0) { /* signal: wake up to value waiters (negative: all), best priority first */
        int left = value < 0 ? CTR_MAX_THREADS : value;
        while (left-- > 0) {
            int best = -1;
            for (int i = 0; i < CTR_MAX_THREADS; i++) {
                ctr_thread *th = &m->threads[i];
                if (th->state == CTR_TH_WAIT && th->wait == CTR_WAIT_ARBITER && th->arbiter == arb
                    && th->arb_addr == addr && (best < 0 || th->prio < m->threads[best].prio))
                    best = i;
            }
            if (best < 0)
                break;
            wake(m, best, CTR_RES_OK, 0);
        }
        CPU.r[0] = CTR_RES_OK;
        return;
    }
    if (type > 4) {
        CPU.r[0] = 0xD8E093EDu; /* invalid enum value */
        return;
    }
    if (!ctr_mem_mapped(m, addr, 4)) {
        CPU.r[0] = CTR_RES_INVALID_ADDRESS;
        return;
    }
    int32_t cur = (int32_t)ctr_r32(m, addr);
    CPU.r[0] = CTR_RES_OK;
    if (cur >= value)
        return;
    if (type == 2 || type == 4)
        ctr_w32(m, addr, (uint32_t)(cur - 1));
    ctr_thread *th = &m->threads[m->cur];
    th->arbiter = arb;
    th->arb_addr = addr;
    th->wait_n = false;
    block(m, CTR_WAIT_ARBITER, type >= 3 ? timeout : -1);
}

static uint32_t page_up(uint32_t v)
{
    return (v + 0xFFF) & ~0xFFFu;
}

static void control_memory(ctr_machine *m)
{
    uint32_t op = CPU.r[0], addr0 = CPU.r[1], size = CPU.r[3], perm = CPU.r[4];
    uint32_t kind = op & 0xFF;
    if (size & 0xFFF || addr0 & 0xFFF) {
        CPU.r[0] = 0xE0E01BF2u; /* misaligned */
        return;
    }
    switch (kind) {
    case 3: { /* COMMIT / ALLOC */
        uint32_t at = addr0;
        if (op & 0x10000) { /* linear: the kernel chooses */
            at = CTR_LINEAR_BASE + m->linear_size;
            m->linear_size += size;
        } else {
            m->heap_size += size;
        }
        if (size && ctr_mem_map(m, at, size, perm & 7, 5)) {
            CPU.r[0] = CTR_RES_OUT_OF_MEMORY;
            return;
        }
        if (m->verbose)
            fprintf(stderr, "ctr: svcControlMemory alloc 0x%08X + 0x%X%s\n", at, size, op & 0x10000 ? " (linear)" : "");
        CPU.r[0] = CTR_RES_OK;
        CPU.r[1] = at;
        return;
    }
    case 1: /* FREE: whole regions only */
        ctr_mem_unmap(m, addr0, size);
        CPU.r[0] = CTR_RES_OK;
        CPU.r[1] = addr0;
        return;
    case 6: /* PROTECT */
        ctr_mem_protect(m, addr0, size, perm & 7);
        CPU.r[0] = CTR_RES_OK;
        return;
    default: /* MAP / UNMAP (aliases) are not modelled */
        if (m->verbose)
            fprintf(stderr, "ctr: svcControlMemory op 0x%X not implemented\n", op);
        CPU.r[0] = CTR_RES_NOT_IMPLEMENTED;
        return;
    }
}

static void query_memory(ctr_machine *m)
{
    uint32_t addr = CPU.r[2];
    const ctr_region *r = ctr_mem_region(m, addr);
    CPU.r[0] = CTR_RES_OK;
    if (r) {
        CPU.r[1] = r->base;
        CPU.r[2] = r->size;
        CPU.r[3] = r->perm;
        CPU.r[4] = r->state;
    } else {
        CPU.r[1] = addr & ~0xFFFu;
        CPU.r[2] = 0x1000;
        CPU.r[3] = 0;
        CPU.r[4] = 0; /* free */
    }
    CPU.r[5] = 0;
}

static void map_block(ctr_machine *m, ctr_obj *o, uint32_t addr, uint32_t perm)
{
    if (!o || o->type != CTR_OBJ_SHMEM) {
        CPU.r[0] = CTR_RES_INVALID_HANDLE;
        return;
    }
    uint32_t size = page_up(o->u.shm.size);
    if (o->u.shm.addr || ctr_mem_map(m, addr, size, 3, 6)) {
        CPU.r[0] = CTR_RES_INVALID_ADDRESS;
        return;
    }
    memcpy(m->h.mem + addr, o->u.shm.buf, o->u.shm.size);
    /* Read-only for the program only: the HLE services keep writing through the host mapping, so it stays
     * writable on the host (the region records the program's permission). */
    for (int i = 0; i < m->nregions; i++)
        if (m->regions[i].base == addr)
            m->regions[i].perm = perm & 3 ? perm & 3 : 3;
    o->u.shm.addr = addr;
    if (m->verbose)
        fprintf(stderr, "ctr: shared memory %s mapped at 0x%08X\n", o->name, addr);
    CPU.r[0] = CTR_RES_OK;
}

static void unmap_block(ctr_machine *m, ctr_obj *o, uint32_t addr)
{
    if (!o || o->type != CTR_OBJ_SHMEM || o->u.shm.addr != addr) {
        CPU.r[0] = CTR_RES_INVALID_HANDLE;
        return;
    }
    memcpy(o->u.shm.buf, m->h.mem + addr, o->u.shm.size);
    ctr_mem_unmap(m, addr, page_up(o->u.shm.size));
    o->u.shm.addr = 0;
    CPU.r[0] = CTR_RES_OK;
}

static void reslimit_values(ctr_machine *m, bool current)
{
    uint32_t out = CPU.r[0], names = CPU.r[2], n = CPU.r[3];
    for (uint32_t i = 0; i < n && i < 16; i++) {
        uint32_t name = ctr_r32(m, names + 4 * i);
        uint64_t v = 0;
        if (current) {
            switch (name) {
            case 1: v = (m->code_hi - m->code_lo) + CTR_STACK_SIZE + m->heap_size + m->linear_size; break;
            case 2: v = 1; break;
            default: v = 0; break;
            }
        } else {
            static const uint64_t limit[10] = {0x18, CTR_APP_MEMORY, 32, 32, 32, 8, 8, 16, 2, 1000};
            v = name < 10 ? limit[name] : 0;
        }
        ctr_w32(m, out + 8 * i, (uint32_t)v);
        ctr_w32(m, out + 8 * i + 4, (uint32_t)(v >> 32));
    }
    CPU.r[0] = CTR_RES_OK;
}

void ctr_svc(ctr_machine *m, uint32_t num)
{
    m->h.next_event = 0; /* the frame loop looks at the threads and timeouts again after any kernel call */
    if (m->verbose >= 2)
        fprintf(stderr, "ctr: thread %d svc 0x%02X (r0 %08X r1 %08X r2 %08X r3 %08X)\n", m->cur, num, CPU.r[0],
                CPU.r[1], CPU.r[2], CPU.r[3]);
    switch (num) {
    case 0x01: control_memory(m); return;
    case 0x02: query_memory(m); return;
    case 0x03: /* ExitProcess */
        m->exited = true;
        for (int i = 0; i < CTR_MAX_THREADS; i++)
            if (m->threads[i].state != CTR_TH_FREE)
                m->threads[i].state = CTR_TH_DEAD;
        switch_to(m, -1);
        return;
    case 0x08: { /* CreateThread */
        int prio = (int)CPU.r[0];
        uint32_t entry = CPU.r[1];
        int t = thread_new(m, entry, CPU.r[2], CPU.r[3], prio);
        if (t < 0) {
            CPU.r[0] = 0xC860180Cu; /* out of threads */
            return;
        }
        CPU.r[0] = CTR_RES_OK;
        CPU.r[1] = ctr_handle_new(m, m->threads[t].obj);
        if (m->verbose)
            fprintf(stderr, "ctr: thread %d created (entry 0x%08X, priority 0x%X)\n", t, entry, prio);
        if (prio < m->threads[m->cur].prio)
            m->resched = true;
        return;
    }
    case 0x09: /* ExitThread */
        m->threads[m->cur].state = CTR_TH_DEAD;
        wake_sync_waiters(m);
        ctr_schedule(m);
        return;
    case 0x0A: { /* SleepThread */
        int64_t ns = (int64_t)((uint64_t)CPU.r[1] << 32 | CPU.r[0]);
        CPU.r[0] = CTR_RES_OK;
        if (ns <= 0) {
            schedule_yield(m, true);
            return;
        }
        m->threads[m->cur].wait_n = false;
        block(m, CTR_WAIT_SLEEP, ns);
        return;
    }
    case 0x0B: { /* GetThreadPriority */
        ctr_obj *o = ctr_handle_get(m, CPU.r[1]);
        if (!o || o->type != CTR_OBJ_THREAD) {
            CPU.r[0] = CTR_RES_INVALID_HANDLE;
            return;
        }
        CPU.r[0] = CTR_RES_OK;
        CPU.r[1] = (uint32_t)m->threads[o->u.th.thread].prio;
        return;
    }
    case 0x0C: { /* SetThreadPriority */
        ctr_obj *o = ctr_handle_get(m, CPU.r[0]);
        if (!o || o->type != CTR_OBJ_THREAD) {
            CPU.r[0] = CTR_RES_INVALID_HANDLE;
            return;
        }
        m->threads[o->u.th.thread].prio = (int)CPU.r[1];
        CPU.r[0] = CTR_RES_OK;
        m->resched = true;
        return;
    }
    case 0x13: { /* CreateMutex */
        ctr_obj *o = ctr_obj_new(m, CTR_OBJ_MUTEX, "mutex");
        if (!o)
            return;
        o->u.mtx.owner = CPU.r[1] ? m->cur : -1;
        o->u.mtx.count = CPU.r[1] ? 1 : 0;
        CPU.r[0] = CTR_RES_OK;
        CPU.r[1] = ctr_handle_new(m, o);
        return;
    }
    case 0x14: { /* ReleaseMutex */
        ctr_obj *o = ctr_handle_get(m, CPU.r[0]);
        if (!o || o->type != CTR_OBJ_MUTEX) {
            CPU.r[0] = CTR_RES_INVALID_HANDLE;
            return;
        }
        if (o->u.mtx.owner != m->cur) {
            CPU.r[0] = 0xD8E0041Fu; /* not owned */
            return;
        }
        if (--o->u.mtx.count <= 0) {
            o->u.mtx.count = 0;
            o->u.mtx.owner = -1;
            wake_sync_waiters(m);
        }
        CPU.r[0] = CTR_RES_OK;
        return;
    }
    case 0x15: { /* CreateSemaphore */
        ctr_obj *o = ctr_obj_new(m, CTR_OBJ_SEMAPHORE, "semaphore");
        if (!o)
            return;
        o->u.sem.count = (int32_t)CPU.r[1];
        o->u.sem.max = (int32_t)CPU.r[2];
        CPU.r[0] = CTR_RES_OK;
        CPU.r[1] = ctr_handle_new(m, o);
        return;
    }
    case 0x16: { /* ReleaseSemaphore */
        ctr_obj *o = ctr_handle_get(m, CPU.r[1]);
        if (!o || o->type != CTR_OBJ_SEMAPHORE) {
            CPU.r[0] = CTR_RES_INVALID_HANDLE;
            return;
        }
        int32_t old = o->u.sem.count;
        o->u.sem.count += (int32_t)CPU.r[2];
        if (o->u.sem.count > o->u.sem.max)
            o->u.sem.count = o->u.sem.max;
        wake_sync_waiters(m);
        CPU.r[0] = CTR_RES_OK;
        CPU.r[1] = (uint32_t)old;
        return;
    }
    case 0x17: { /* CreateEvent */
        ctr_obj *o = ctr_obj_new(m, CTR_OBJ_EVENT, "event");
        if (!o)
            return;
        o->u.ev.reset = (int)CPU.r[1];
        CPU.r[0] = CTR_RES_OK;
        CPU.r[1] = ctr_handle_new(m, o);
        return;
    }
    case 0x18: /* SignalEvent */
    case 0x19: { /* ClearEvent */
        ctr_obj *o = ctr_handle_get(m, CPU.r[0]);
        if (!o || (o->type != CTR_OBJ_EVENT && o->type != CTR_OBJ_TIMER)) {
            CPU.r[0] = CTR_RES_INVALID_HANDLE;
            return;
        }
        CPU.r[0] = CTR_RES_OK;
        if (num == 0x18)
            ctr_event_signal(m, o);
        else
            o->u.ev.signaled = false;
        return;
    }
    case 0x1F: map_block(m, ctr_handle_get(m, CPU.r[0]), CPU.r[1], CPU.r[2]); return;
    case 0x20: unmap_block(m, ctr_handle_get(m, CPU.r[0]), CPU.r[1]); return;
    case 0x21: { /* CreateAddressArbiter */
        ctr_obj *o = ctr_obj_new(m, CTR_OBJ_ARBITER, "arbiter");
        if (!o)
            return;
        CPU.r[0] = CTR_RES_OK;
        CPU.r[1] = ctr_handle_new(m, o);
        return;
    }
    case 0x22: /* ArbitrateAddress */
        arbitrate(m, ctr_handle_get(m, CPU.r[0]), CPU.r[1], CPU.r[2], (int32_t)CPU.r[3],
                  (int64_t)((uint64_t)CPU.r[5] << 32 | CPU.r[4]));
        return;
    case 0x23: /* CloseHandle */
        handle_close(m, CPU.r[0]);
        CPU.r[0] = CTR_RES_OK;
        return;
    case 0x24: { /* WaitSynchronization1 */
        uint32_t hd = CPU.r[0];
        sync_wait(m, &hd, 1, false, false, (int64_t)((uint64_t)CPU.r[3] << 32 | CPU.r[2]));
        return;
    }
    case 0x25: { /* WaitSynchronizationN */
        int32_t n = (int32_t)CPU.r[2];
        uint32_t hs[CTR_MAX_WAIT];
        for (int32_t i = 0; i < n && i < CTR_MAX_WAIT; i++)
            hs[i] = ctr_r32(m, CPU.r[1] + 4u * (uint32_t)i);
        sync_wait(m, hs, n, CPU.r[3] != 0, true, (int64_t)((uint64_t)CPU.r[4] << 32 | CPU.r[0]));
        return;
    }
    case 0x27: { /* DuplicateHandle */
        ctr_obj *o = ctr_handle_get(m, CPU.r[1]);
        if (!o) {
            CPU.r[0] = CTR_RES_INVALID_HANDLE;
            return;
        }
        CPU.r[0] = CTR_RES_OK;
        CPU.r[1] = ctr_handle_new(m, o);
        return;
    }
    case 0x28: /* GetSystemTick */
        CPU.r[0] = (uint32_t)m->h.now;
        CPU.r[1] = (uint32_t)(m->h.now >> 32);
        return;
    case 0x2D: { /* ConnectToPort */
        char name[12] = {0};
        for (int i = 0; i < 11; i++)
            name[i] = (char)m->h.mem[CPU.r[1] + (uint32_t)i];
        if (strcmp(name, "srv:")) {
            CPU.r[0] = 0xD88007FAu; /* port not found */
            return;
        }
        ctr_obj *o = ctr_obj_new(m, CTR_OBJ_SESSION, "srv:");
        if (!o)
            return;
        o->u.sess.service = CTR_SVC_SRV;
        CPU.r[0] = CTR_RES_OK;
        CPU.r[1] = ctr_handle_new(m, o);
        return;
    }
    case 0x32: { /* SendSyncRequest: the command buffer is at TLS + 0x80 */
        ctr_obj *o = ctr_handle_get(m, CPU.r[0]);
        if (!o || o->type != CTR_OBJ_SESSION) {
            CPU.r[0] = CTR_RES_INVALID_HANDLE;
            return;
        }
        CPU.r[0] = CTR_RES_OK;
        ctr_service_call(m, o, CPU.tls + 0x80);
        return;
    }
    case 0x35: /* GetProcessId */
        CPU.r[0] = CTR_RES_OK;
        CPU.r[1] = 0x28;
        return;
    case 0x37: { /* GetThreadId */
        ctr_obj *o = ctr_handle_get(m, CPU.r[1]);
        if (!o || o->type != CTR_OBJ_THREAD) {
            CPU.r[0] = CTR_RES_INVALID_HANDLE;
            return;
        }
        CPU.r[0] = CTR_RES_OK;
        CPU.r[1] = m->threads[o->u.th.thread].id;
        return;
    }
    case 0x38: /* GetResourceLimit */
        CPU.r[0] = CTR_RES_OK;
        CPU.r[1] = ctr_handle_new(m, m->reslimit);
        return;
    case 0x39: reslimit_values(m, false); return;
    case 0x3A: reslimit_values(m, true); return;
    case 0x3C: /* Break */
        ctr_stop(m, "svcBreak (reason %u) from 0x%08X", CPU.r[0], CPU.r[14]);
        return;
    case 0x3D: { /* OutputDebugString */
        if (m->verbose) {
            fprintf(stderr, "ctr: debug: ");
            for (uint32_t i = 0; i < CPU.r[1] && i < 256; i++)
                fputc(m->h.mem[CPU.r[0] + i], stderr);
            fputc('\n', stderr);
        }
        CPU.r[0] = CTR_RES_OK;
        return;
    }
    default:
        ctr_stop(m, "unsupported SVC 0x%02X from 0x%08X", num, CPU.r[15] - 12);
        return;
    }
}

void ctr_kernel_init(ctr_machine *m)
{
    m->cur = -1;
    m->next_thread_id = 0x100;
    m->process = ctr_obj_new(m, CTR_OBJ_PROCESS, "process");
    m->reslimit = ctr_obj_new(m, CTR_OBJ_RESLIMIT, "reslimit");
}

/* The main thread at the entry: priority 0x30, its stack below 0x10000000, lr = 0 (no return address: libctru's
 * 3DSX start-up then exits through svcExitProcess). */
int ctr_kernel_start(ctr_machine *m, uint32_t entry)
{
    int t = thread_new(m, entry, 0, CTR_STACK_TOP, 0x30);
    if (t < 0)
        return -1;
    m->threads[t].cpu.r[14] = 0;
    switch_to(m, t);
    return 0;
}
