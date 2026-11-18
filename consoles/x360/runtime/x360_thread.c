/* x360_thread.c -- guest threads, waits, the dispatcher and the machine's lifetime (XBA-270).
 *
 * Threads are cooperative on one host thread: each guest thread has its own context (xe_cpu: 64-bit GPRs, FPRs,
 * the 128 VMX registers...), its stack in the stack heap, a KTHREAD and a PCR (r13) in kernel memory. The scheduler
 * runs the ready threads round robin, a time slice each (X360_SLICE blocks: RC_BLOCK hands the host thread back at a
 * resume point), and switches at every blocking wait. The order is deterministic, which keeps runs reproducible and
 * the state serialisable (save states, rewind); real parallel hardware threads are a later step (XBA-229). A thread
 * switch clears the reservation of lwarx / ldarx, so a stwcx. / stdcx. after another thread ran fails and the guest
 * loop retries, as on hardware.
 *
 * Waits are polled: a waiting thread is checked each time the scheduler looks for work. Objects are the guest's
 * dispatcher headers (events, semaphores, mutants, timers, threads; X_DISPATCH_HEADER: type at +0, signal state at
 * +4), so the Ke* calls on guest structures and the Nt* calls on handles meet. Time is virtual: instructions run
 * (one 100 ns tick per 320) plus what was skipped when every thread was waiting for a timeout. */
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "x360_internal.h"

void x360_logf(x360_machine *x, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(x->log ? x->log : stderr, fmt, ap);
    va_end(ap);
}

void x360_fail(x360_machine *x, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(x->error, sizeof x->error, fmt, ap);
    va_end(ap);
    x->status = X360_ERROR;
}

uint64_t x360_now(x360_machine *x)
{
    return x->m.rc_native / 320 + x->st.interpreted / 320 + x->st.hle_calls * 2 + x->time_skip;
}

static void sync_tb(x360_machine *x)
{
    /* the time base (mftb, 50 MHz) follows the virtual clock: 5 ticks per 100 ns, minus what rc_native gives */
    uint64_t want = x360_now(x) * 5, have = x->m.rc_native / 64;
    x->m.tb_base = want > have ? want - have : 0;
}

/* ---- handles ---- */

uint32_t x360_handle_new(x360_machine *x, uint32_t obj)
{
    for (int k = 1; k < X360_MAX_HANDLES; k++)
        if (!x->handles[k].used) {
            x->handles[k].used = 1;
            x->handles[k].obj = obj;
            return 0xF8000000u | (uint32_t)k << 2;
        }
    return 0;
}

uint32_t x360_handle_obj(x360_machine *x, uint32_t h)
{
    if (h == 0xFFFFFFFEu) /* NtCurrentThread() */
        return x360_cur(x) ? x360_cur(x)->kthread : 0;
    if ((h & 0xFF000003u) != 0xF8000000u)
        return 0;
    uint32_t k = (h & 0x00FFFFFFu) >> 2;
    return k < X360_MAX_HANDLES && x->handles[k].used ? x->handles[k].obj : 0;
}

int x360_handle_close(x360_machine *x, uint32_t h)
{
    uint32_t k = (h & 0x00FFFFFFu) >> 2;
    if ((h & 0xFF000003u) != 0xF8000000u || k >= X360_MAX_HANDLES || !x->handles[k].used)
        return -1;
    x->handles[k].used = 0;
    return 0;
}

/* ---- threads ---- */

x360_thread *x360_cur(x360_machine *x)
{
    return x->cur >= 0 ? &x->threads[x->cur] : NULL;
}

x360_thread *x360_thread_by_kthread(x360_machine *x, uint32_t kthread)
{
    for (int k = 0; k < x->nthreads; k++)
        if (x->threads[k].state != TH_FREE && x->threads[k].kthread == kthread)
            return &x->threads[k];
    return NULL;
}

x360_thread *x360_thread_create(x360_machine *x, uint32_t stack_size, uint32_t entry, uint32_t r3, uint32_t r4,
                                int suspended)
{
    /* a free slot, or one whose thread ended (its KTHREAD stays, signalled, for waits on it; its stack is freed);
     * never the main thread's, nor the interrupt context's (x360_thread_restart reuses it) */
    int k = 0;
    while (k < X360_MAX_THREADS && x->threads[k].state != TH_FREE &&
           (k == 0 || x->threads[k].state != TH_DONE || x->threads[k].interrupt))
        k++;
    if (k == X360_MAX_THREADS)
        return NULL;
    if (k >= x->nthreads)
        x->nthreads = k + 1;
    x360_thread *t = &x->threads[k];
    if (t->state == TH_DONE && t->stack_limit)
        x360_heap_free(x, &x->heap_stack, t->stack_limit - 0x1000);
    memset(t, 0, sizeof *t);
    if (stack_size < 0x4000)
        stack_size = 0x4000;
    stack_size = (stack_size + 0xFFF) & ~0xFFFu;
    uint32_t stack = x360_heap_alloc(x, &x->heap_stack, stack_size + 0x1000, 0); /* + a page below, unused */
    uint32_t kth = x360_kalloc(x, 0xAB0);
    uint32_t pcr = x360_kalloc(x, 0x2D8);
    uint32_t tls_static = x->tls_data_size ? (x->tls_data_size + 15) & ~15u : 16;
    uint32_t tls = x360_kalloc(x, X360_TLS_SLOTS * 4 + tls_static);
    if (!stack || !kth || !pcr || !tls)
        return NULL;
    t->id = ++x->next_thread_id;
    t->kthread = kth;
    t->pcr = pcr;
    t->tls_slots = tls;
    t->tls_static = tls + X360_TLS_SLOTS * 4;
    t->stack_limit = stack + 0x1000;
    t->stack_base = stack + 0x1000 + stack_size;
    if (x->tls_raw && x->tls_raw_size)
        memcpy(x->base + t->tls_static, x->base + x->tls_raw,
               x->tls_raw_size < x->tls_data_size ? x->tls_raw_size : x->tls_data_size);
    /* KTHREAD (Xenia's X_KTHREAD offsets): a thread dispatcher object, signalled when the thread ends */
    x360_w8(x, kth + 0, X_OBJ_THREAD);
    x360_w32(x, kth + 0x5C, t->stack_base);
    x360_w32(x, kth + 0x60, t->stack_limit);
    x360_w32(x, kth + 0x68, t->tls_static);
    x360_w8(x, kth + 0xBF, (uint8_t)(k % 6));
    x360_w32(x, kth + 0x14C, t->id);
    x360_w32(x, kth + 0x150, entry);
    /* PCR (X_KPCR): TLS pointer, itself, the stack, the current thread, the hardware thread */
    x360_w32(x, pcr + 0x00, t->tls_static);
    x360_w32(x, pcr + 0x30, pcr);
    x360_w32(x, pcr + 0x70, t->stack_base);
    x360_w32(x, pcr + 0x74, t->stack_limit);
    x360_w32(x, pcr + 0x100, kth);
    x360_w8(x, pcr + 0x10C, (uint8_t)(k % 6));
    xe_cpu *c = &t->cpu;
    c->pc = entry;
    c->lr = X360_THREAD_EXIT;
    c->gpr[1] = t->stack_base - 0x100;
    c->gpr[3] = r3;
    c->gpr[4] = r4;
    c->gpr[13] = pcr;
    c->vscr = 0x00010000u; /* VMX non-Java mode, as the console runs it */
    c->msr = 0x9030;
    t->state = suspended ? TH_SUSPENDED : TH_READY;
    t->suspend = suspended ? 1 : 0;
    t->handle = x360_handle_new(x, kth);
    x->st.threads++;
    return t;
}

int x360_callback_run(x360_machine *x, int *slot, uint32_t entry, uint32_t r3, uint32_t r4)
{
    x360_thread *t = *slot >= 0 ? &x->threads[*slot] : NULL;
    if (t && t->state != TH_DONE)
        return -1;
    if (t)
        return x360_thread_restart(x, t, entry, r3, r4);
    t = x360_thread_create(x, 0x4000, entry, r3, r4, 0);
    if (!t)
        return -1;
    t->interrupt = 1;
    *slot = (int)(t - x->threads);
    return 0;
}

/* Run entry(r3, r4) again on an ended thread's context (its stack, KTHREAD, PCR, TLS): the interrupt context. */
int x360_thread_restart(x360_machine *x, x360_thread *t, uint32_t entry, uint32_t r3, uint32_t r4)
{
    if (t->state != TH_DONE)
        return -1;
    xe_cpu *c = &t->cpu;
    memset(c, 0, sizeof *c);
    c->pc = entry;
    c->lr = X360_THREAD_EXIT;
    c->gpr[1] = t->stack_base - 0x100;
    c->gpr[3] = r3;
    c->gpr[4] = r4;
    c->gpr[13] = t->pcr;
    c->vscr = 0x00010000u;
    c->msr = 0x9030;
    x360_w32(x, t->kthread + 4, 0);
    t->state = TH_READY;
    t->suspend = 0;
    t->exit_code = 0;
    return 0;
}

void x360_thread_exit(x360_machine *x, x360_thread *t, uint32_t code)
{
    t->state = TH_DONE;
    t->exit_code = code;
    x360_w32(x, t->kthread + 4, 1); /* signalled: waits on the thread end */
    x360_w32(x, t->kthread + 0x140, code);
    if (t->interrupt)
        return; /* an interrupt callback returned: its context waits for the next one */
    if (t == &x->threads[0]) {
        /* the title's first thread returned from the entry point: the title is over */
        x->status = X360_EXITED;
        x->exit_code = code;
    } else {
        int alive = 0;
        for (int k = 0; k < x->nthreads; k++)
            alive |= x->threads[k].state != TH_FREE && x->threads[k].state != TH_DONE && !x->threads[k].interrupt;
        if (!alive) {
            x->status = X360_EXITED;
            x->exit_code = code;
        }
    }
}

/* ---- waits ---- */

static int64_t sx64(uint64_t v) { return (int64_t)v; }

void x360_wait(x360_machine *x, x360_thread *t, int kind, const uint32_t *objs, int count, int wait_all,
               uint32_t timeout_ptr)
{
    t->wait_kind = kind;
    t->wait_all = wait_all;
    t->wait_count = count > X360_MAX_WAIT ? X360_MAX_WAIT : count;
    for (int k = 0; k < t->wait_count; k++)
        t->wait_obj[k] = objs[k];
    t->wait_deadline = 0;
    t->wait_timed = timeout_ptr != 0;
    if (timeout_ptr) {
        int64_t v = sx64(x360_r64(x, timeout_ptr));
        uint64_t now = x360_now(x);
        if (v < 0)
            t->wait_deadline = now + (uint64_t)(-v);
        else { /* absolute: a system time (KeQuerySystemTime's clock) */
            uint64_t at = (uint64_t)v > X360_EPOCH ? (uint64_t)v - X360_EPOCH : 0;
            t->wait_deadline = at > now ? at : now;
        }
    }
    t->state = TH_WAIT;
    x->m.slice = -1; /* give the host thread back */
}

static int obj_ready(x360_machine *x, x360_thread *t, uint32_t o)
{
    uint8_t type = x360_r8(x, o);
    int32_t state = (int32_t)x360_r32(x, o + 4);
    if (type == X_OBJ_MUTANT)
        return state > 0 || x360_r32(x, o + 0x18) == t->kthread;
    return state > 0 || (state != 0 && type != X_OBJ_SEMAPHORE);
}

static void obj_take(x360_machine *x, x360_thread *t, uint32_t o)
{
    uint8_t type = x360_r8(x, o);
    switch (type) {
    case X_OBJ_EVENT_SYNC:
    case X_OBJ_TIMER_SYNC:
        x360_w32(x, o + 4, 0);
        break;
    case X_OBJ_SEMAPHORE:
        x360_w32(x, o + 4, x360_r32(x, o + 4) - 1);
        break;
    case X_OBJ_MUTANT:
        x360_w32(x, o + 4, x360_r32(x, o + 4) - 1);
        x360_w32(x, o + 0x18, t->kthread);
        break;
    default:
        break; /* notification events and timers, threads: stay signalled */
    }
}

/* 1 when the wait of t is over (r3 then holds the status) */
int x360_wait_check(x360_machine *x, x360_thread *t)
{
    uint32_t status = 0xFFFFFFFFu;
    if (t->wait_kind == WAIT_CRITSEC) {
        uint32_t cs = t->wait_obj[0];
        if (x360_r32(x, cs + 0x18) == 0) {
            x360_w32(x, cs + 0x18, t->kthread);
            x360_w32(x, cs + 0x14, 1);
            status = 0;
        }
    } else if (t->wait_kind == WAIT_OBJECTS && t->wait_count) {
        if (t->wait_all) {
            int all = 1;
            for (int k = 0; k < t->wait_count; k++)
                all &= obj_ready(x, t, t->wait_obj[k]);
            if (all) {
                for (int k = 0; k < t->wait_count; k++)
                    obj_take(x, t, t->wait_obj[k]);
                status = X_STATUS_SUCCESS;
            }
        } else {
            for (int k = 0; k < t->wait_count; k++)
                if (obj_ready(x, t, t->wait_obj[k])) {
                    obj_take(x, t, t->wait_obj[k]);
                    status = X_STATUS_SUCCESS + (uint32_t)k;
                    break;
                }
        }
    }
    if (status == 0xFFFFFFFFu && t->wait_timed && x360_now(x) >= t->wait_deadline)
        status = t->wait_kind == WAIT_DELAY ? X_STATUS_SUCCESS : X_STATUS_TIMEOUT;
    if (status == 0xFFFFFFFFu)
        return 0;
    t->cpu.gpr[3] = status;
    t->state = t->suspend ? TH_SUSPENDED : TH_READY;
    return 1;
}

/* ---- timers ---- */

void x360_timer_set(x360_machine *x, uint32_t obj, uint64_t due, uint32_t period)
{
    int free_k = -1;
    for (int k = 0; k < X360_MAX_TIMERS; k++) {
        if (x->timers[k].obj == obj) {
            free_k = k;
            break;
        }
        if (!x->timers[k].obj && free_k < 0)
            free_k = k;
    }
    if (free_k < 0)
        return;
    x->timers[free_k] = (x360_timer){obj, due ? due : 1, period};
    x360_w32(x, obj + 4, 0);
}

void x360_timer_cancel(x360_machine *x, uint32_t obj)
{
    for (int k = 0; k < X360_MAX_TIMERS; k++)
        if (x->timers[k].obj == obj)
            x->timers[k].due = 0;
}

void x360_timers_fire(x360_machine *x)
{
    uint64_t now = x360_now(x);
    for (int k = 0; k < X360_MAX_TIMERS; k++) {
        x360_timer *tm = &x->timers[k];
        if (tm->obj && tm->due && now >= tm->due) {
            x360_w32(x, tm->obj + 4, 1);
            tm->due = tm->period ? tm->due + (uint64_t)tm->period * 10000 : 0;
        }
    }
}

/* the earliest deadline among waiting threads and armed timers, 0 when none */
static uint64_t next_deadline(x360_machine *x)
{
    uint64_t d = 0;
    for (int k = 0; k < x->nthreads; k++) {
        x360_thread *t = &x->threads[k];
        if (t->state == TH_WAIT && t->wait_timed && (!d || t->wait_deadline < d))
            d = t->wait_deadline ? t->wait_deadline : 1;
    }
    for (int k = 0; k < X360_MAX_TIMERS; k++)
        if (x->timers[k].obj && x->timers[k].due && (!d || x->timers[k].due < d))
            d = x->timers[k].due;
    uint64_t g = x360_gpu_next_event(x);
    if (g && (!d || g < d))
        d = g;
    g = x360_audio_next_event(x);
    if (g && (!d || g < d))
        d = g;
    g = x360_input_next_event(x);
    if (g && (!d || g < d))
        d = g;
    return d;
}

/* ---- dispatch ---- */

static uint32_t fault_pc; /* where the running lifted function was entered (for the fault report) */

static const x360_thunk *thunk_at(x360_machine *x, uint32_t pc)
{
    if (pc < x->thunk_lo || pc >= x->thunk_hi)
        return NULL;
    unsigned lo = 0, hi = x->nthunks;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (x->thunks[mid].addr < pc)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < x->nthunks && x->thunks[lo].addr == pc ? &x->thunks[lo] : NULL;
}

static void missed(x360_machine *x, uint32_t pc)
{
    for (unsigned k = 0; k < x->nmissed; k++)
        if (x->missed[k] == pc)
            goto out;
    if (x->nmissed < sizeof x->missed / sizeof x->missed[0])
        x->missed[x->nmissed++] = pc;
out:
    x->status = X360_MISSED;
    snprintf(x->error, sizeof x->error, "no lifted code at %08X (add it with x360_tools.recomp --discovered)", pc);
}

static void run_thread(x360_machine *x, x360_thread *t)
{
    xe_cpu *c = &t->cpu;
    x->m.cur = c;
    x->m.slice = X360_SLICE;
    while (t->state == TH_READY && x->status == X360_RUNNING &&
           (x->m.slice >= 0 || (!(c->msr & 0x8000u) && x->m.slice >= -XE_EE_GRACE))) {
        uint32_t pc = c->pc;
        if (pc == X360_THREAD_EXIT) {
            x360_thread_exit(x, t, (uint32_t)c->gpr[3]);
            break;
        }
        const x360_thunk *th = thunk_at(x, pc);
        if (th) {
            c->pc = (uint32_t)c->lr & ~3u;
            x->st.hle_calls++;
            sync_tb(x);
            x360_hle_call(x, th);
            x->m.slice -= 8;
            continue;
        }
        if (x->m.rc_native >= x->stop_at)
            break; /* the caller's instruction budget: stop between two lifted functions */
        xe_rc_fn fn = pc >= x->code_lo && pc < x->code_hi && !(pc & 3) ? x->fn_tab[(pc - x->code_lo) >> 2] : NULL;
        if (!fn) {
            missed(x, pc);
            break;
        }
        x->m.rc_exit = 0;
        fault_pc = pc;
        fn(&x->m);
        if (x->m.rc_exit == XE_RC_EXIT_INTERP) {
            if (x360_interp_one(x))
                break;
        } else if (x->m.rc_exit == XE_RC_EXIT_NOT_COVERED) {
            missed(x, pc);
            break;
        }
    }
}

/* ---- guest faults ---- */

static x360_machine *fault_machine;

static void on_fault(int sig, siginfo_t *si, void *ctx)
{
    (void)ctx;
    x360_machine *x = fault_machine;
    uint8_t *a = si->si_addr;
    char msg[200];
    int n;
    if (x && a >= x->base && a < x->base + (1ull << 32)) {
        x360_thread *t = x360_cur(x);
        n = snprintf(msg, sizeof msg, "x360: guest access fault at %08X (thread %u, in lifted code entered at %08X)\n",
                     (uint32_t)(a - x->base), t ? t->id : 0, fault_pc);
    } else {
        n = snprintf(msg, sizeof msg, "x360: host signal %d at %p (not a guest address)\n", sig, (void *)a);
    }
    if (n > 0 && write(2, msg, (size_t)n) < 0)
        n = 0;
    _exit(1);
}

void x360_install_fault_handler(x360_machine *x)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    fault_machine = x;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}

int x360_run(x360_machine *x, uint64_t insns)
{
    if (!x->fn_tab) {
        x360_fail(x, "no pack bound");
        return x->status;
    }
    x360_install_fault_handler(x);
    uint64_t stop = x->m.rc_native + insns;
    x->stop_at = stop;
    while (x->status == X360_RUNNING) {
        if (x->m.rc_native >= stop || (x->time_limit && x360_now(x) >= x->time_limit)) {
            x->status = X360_BUDGET;
            break;
        }
        x360_timers_fire(x);
        x360_gpu_poll(x);
        x360_audio_poll(x);
        x360_input_poll(x);
        if (x->timestamp_bundle) { /* KeTimeStampBundle: interrupt time, system time, tick count (ms) */
            uint64_t now = x360_now(x);
            x360_w64(x, x->timestamp_bundle, now);
            x360_w64(x, x->timestamp_bundle + 8, X360_EPOCH + now);
            x360_w32(x, x->timestamp_bundle + 16, (uint32_t)(now / 10000));
        }
        int picked = -1;
        for (int n = 1; n <= x->nthreads; n++) {
            int k = (x->cur + n + x->nthreads) % x->nthreads;
            x360_thread *t = &x->threads[k];
            if (t->state == TH_WAIT)
                x360_wait_check(x, t);
            if (t->state == TH_READY) {
                picked = k;
                break;
            }
        }
        if (picked < 0) {
            uint64_t d = next_deadline(x);
            if (!d) {
                x360_fail(x, "every thread is blocked with no timeout (deadlock)");
                break;
            }
            uint64_t now = x360_now(x);
            if (d > now)
                x->time_skip += d - now;
            continue;
        }
        if (picked != x->cur) {
            x->st.switches++;
            if (x->cur >= 0)
                x->threads[x->cur].cpu.reserve = 0;
            x->threads[picked].cpu.reserve = 0;
        }
        x->cur = picked;
        run_thread(x, &x->threads[picked]);
    }
    return x->status;
}

/* ---- the machine ---- */

x360_machine *x360_create(const uint8_t *xex, size_t size, char *why, size_t why_len)
{
    x360_machine *x = calloc(1, sizeof *x);
    if (!x) {
        snprintf(why, why_len, "out of memory");
        return NULL;
    }
    x->memfd = -1;
    x->cur = -1;
    x->m.rt = x;
    if (x360_mem_init(x, why, why_len) || x360_gpu_init(x) || x360_kernel_init(x) ||
        x360_load(x, xex, size, why, why_len)) {
        if (!why[0])
            snprintf(why, why_len, "kernel initialisation failed");
        x360_destroy(x);
        return NULL;
    }
    /* X_LDR_DATA_TABLE_ENTRY (Xenia's xmodule.h): dll base 0, image base, sizes, entry point, XEX header */
    x360_w32(x, x->module + 0x1C, x->image_base);
    x360_w32(x, x->module + 0x20, x->image_size);
    x360_w32(x, x->module + 0x38, x->image_size);
    x360_w32(x, x->module + 0x3C, x->entry);
    x360_w32(x, x->module + 0x58, x->xex_header);
    if (!x360_thread_create(x, x->stack_size, x->entry, 0, 0, 0)) {
        snprintf(why, why_len, "cannot create the main thread");
        x360_destroy(x);
        return NULL;
    }
    return x;
}

x360_machine *x360_create_bare(char *why, size_t n)
{
    x360_machine *x = calloc(1, sizeof *x);
    if (!x)
        return NULL;
    x->memfd = -1;
    x->cur = -1;
    x->m.rt = x;
    x->stack_size = 0x10000;
    if (x360_mem_init(x, why, n) || x360_gpu_init(x) || x360_kernel_init(x) ||
        !x360_thread_create(x, 0x10000, 0x82000000u, 0, 0, 0)) {
        x360_destroy(x);
        return NULL;
    }
    x->cur = 0;
    x->m.cur = &x->threads[0].cpu;
    return x;
}

void x360_destroy(x360_machine *x)
{
    if (!x)
        return;
    x360_fs_free(x);
    x360_gpu_free(x);
    free(x->pad_script);
    x360_mem_free(x);
    free(x->thunks);
    free(x->fn_tab);
    free(x);
}

int x360_bind(x360_machine *x, const x360_pack *pack)
{
    if (pack->image_crc != x->image_crc) {
        x360_fail(x, "the pack was made from another image (CRC %08X, the XEX's is %08X)", pack->image_crc,
                  x->image_crc);
        return -1;
    }
    const xe_rc_entry *r = pack->resume;
    if (!pack->count)
        return -1;
    x->code_lo = r[0].entry & ~3u;
    x->code_hi = (r[pack->count - 1].entry & ~3u) + 4;
    free(x->fn_tab);
    x->fn_tab = calloc((x->code_hi - x->code_lo) / 4, sizeof *x->fn_tab);
    if (!x->fn_tab)
        return -1;
    for (unsigned k = 0; k < pack->count; k++)
        x->fn_tab[(r[k].entry - x->code_lo) / 4] = r[k].fn;
    return 0;
}

void x360_set_time_limit(x360_machine *x, uint32_t seconds)
{
    x->time_limit = (uint64_t)seconds * 10000000u;
}

void x360_set_strict_imports(x360_machine *x, int on)
{
    x->strict_imports = on;
}

void x360_set_output(x360_machine *x, FILE *dbg, FILE *log, int verbose)
{
    x->dbg = dbg;
    x->log = log;
    x->verbose = verbose;
}

const char *x360_error(const x360_machine *x) { return x->error; }
uint32_t x360_exit_code(const x360_machine *x) { return x->exit_code; }

unsigned x360_missed(const x360_machine *x, const uint32_t **addrs)
{
    *addrs = x->missed;
    return x->nmissed;
}

void x360_image_info(const x360_machine *x, uint32_t *base, uint32_t *entry, uint32_t *size)
{
    *base = x->image_base;
    *entry = x->entry;
    *size = x->image_size;
}

void x360_get_stats(const x360_machine *x, x360_stats *s)
{
    *s = x->st;
    s->native = x->m.rc_native;
    x360_gpu_stats((x360_machine *)x, &s->gpu_packets, &s->gpu_draws, &s->gpu_swaps, &s->vblanks);
}
