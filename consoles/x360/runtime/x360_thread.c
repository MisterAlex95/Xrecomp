/* x360_thread.c -- guest threads, waits, the dispatcher and the machine's lifetime (XBA-270).
 *
 * Threads are cooperative on one host thread: each guest thread has its own context (xe_cpu: 64-bit GPRs, FPRs,
 * the 128 VMX registers...), its stack in the stack heap, a KTHREAD and a PCR (r13) in kernel memory. The scheduler
 * runs the ready threads round robin, a time slice each (X360_SLICE blocks: RC_BLOCK hands the host thread back at a
 * resume point), and switches at every blocking wait. The order is deterministic, which keeps runs reproducible and
 * the state serialisable (save states, rewind); guest threads on host threads are an experimental switch (XBA-283,
 * x360_set_host_threads, « guest threads on host threads » below). A thread
 * switch clears the reservation of lwarx / ldarx, so a stwcx. / stdcx. after another thread ran fails and the guest
 * loop retries, as on hardware.
 *
 * Waits are polled: a waiting thread is checked each time the scheduler looks for work. Objects are the guest's
 * dispatcher headers (events, semaphores, mutants, timers, threads; X_DISPATCH_HEADER: type at +0, signal state at
 * +4), so the Ke* calls on guest structures and the Nt* calls on handles meet. Time is virtual: instructions run
 * (one 100 ns tick per 320) plus what was skipped when every thread was waiting for a timeout. */
#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#if defined(__linux__)
#include <ucontext.h> /* the host pc of a fault (macOS refuses the header without _XOPEN_SOURCE) */
#endif
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

static uint64_t host_now_100ns(void);

uint64_t x360_now(x360_machine *x)
{
    if (x->host) /* XBA-283: threads run in parallel, the sum of their instructions is no clock */
        return host_now_100ns() - x->host_t0 + x->time_skip;
    /* interpreted instructions are in rc_native too (RC_BLOCK counts an RC_INTERP one, x360_interp_run the rest) */
    return x->m.rc_native / 320 + x->st.hle_calls * 2 + x->time_skip;
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
        memcpy(x360_p(x, t->tls_static), x360_p(x, x->tls_raw),
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
    memset(&t->seh, 0, sizeof t->seh);
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
    x360_queue_thread_exit(x, t);
    t->apc_head = t->apc_tail = 0; /* what is still queued is dropped (x360_kdpc.c) */
    t->apc_stage = 0;
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
    t->wait_alertable = 0; /* x360_wait_alertable sets it */
    if (timeout_ptr)
        t->wait_deadline = x360_due_time(x, sx64(x360_r64(x, timeout_ptr)));
    x360_queue_parked(x, t, kind);
    t->state = TH_WAIT;
    x->m.slice = -1; /* give the host thread back */
}

void x360_wait_alertable(x360_machine *x, x360_thread *t, int kind, const uint32_t *objs, int count, int wait_all,
                         uint32_t timeout_ptr, int alertable)
{
    x360_wait(x, t, kind, objs, count, wait_all, timeout_ptr);
    t->wait_alertable = alertable != 0;
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
    } else if (t->wait_kind == WAIT_QUEUE) {
        x360_queue_wait_check(x, t, &status);
    }
    if (status == 0xFFFFFFFFu) { /* nothing satisfied it: an alert or an APC may end the wait, or break into it */
        x360_apc_wait_check(x, t, &status);
        if (t->state != TH_WAIT)
            return 0;
    }
    if (status == 0xFFFFFFFFu && t->wait_timed && x360_now(x) >= t->wait_deadline)
        status = t->wait_kind == WAIT_DELAY ? X_STATUS_SUCCESS : X_STATUS_TIMEOUT;
    if (status == 0xFFFFFFFFu)
        return 0;
    t->cpu.gpr[3] = status;
    t->wait_iocp = 0;
    t->state = t->suspend ? TH_SUSPENDED : TH_READY;
    x360_queue_unparked(x, t);
    x360_apc_wait_done(x, t, status);
    return 1;
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

const x360_thunk *x360_thunk_at(x360_machine *x, uint32_t pc)
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

void x360_note_missed(x360_machine *x, uint32_t pc)
{
    if ((pc & 3) || pc < x->image_base || pc - x->image_base >= x->image_size)
        return; /* a jump through a bad pointer: not code to lift (x360_interp_run stops on it) */
    unsigned lo = 0, hi = x->nhelpers;
    while (lo < hi) { /* a save / restore helper the lifter inlines: not a function */
        unsigned mid = (lo + hi) / 2;
        if (x->helpers[mid] < pc)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < x->nhelpers && x->helpers[lo] == pc)
        return;
    for (unsigned k = 0; k < x->nmissed; k++)
        if (x->missed[k] == pc)
            return;
    if (x->nmissed < X360_MAX_MISSED)
        x->missed[x->nmissed++] = pc;
    if (x->nmissed <= 64 || x->verbose)
        x360_logf(x, "x360: no lifted code at %08X%s (discovered.txt for the next Prepare)\n", pc,
                  x->stop_at_miss ? "" : ", interpreted");
}

static void missed(x360_machine *x, uint32_t pc)
{
    x360_note_missed(x, pc);
    x->status = X360_MISSED;
    snprintf(x->error, sizeof x->error, "no lifted code at %08X (add it with x360_tools.recomp --discovered)", pc);
}

static void run_thread(x360_machine *x, x360_thread *t)
{
    xe_cpu *c = &t->cpu;
    x->m.cur = c;
    x->m.slice = X360_SLICE;
    x->m.rc_depth = 0; /* WP-16: no direct call on the host stack (RC_CALL) */
    while (t->state == TH_READY && x->status == X360_RUNNING &&
           (x->m.slice >= 0 || (!(c->msr & 0x8000u) && x->m.slice >= -XE_EE_GRACE))) {
        uint32_t pc = c->pc;
        int resumed = t->interp;
        t->interp = 0;
        if (pc == X360_THREAD_EXIT) {
            x360_thread_exit(x, t, (uint32_t)c->gpr[3]);
            break;
        }
        if (pc == X360_SEH_RETURN) { /* an exception handler returned (XBA-282) */
            x360_seh_return(x, t);
            continue;
        }
        if (pc == X360_APC_RETURN) { /* an APC routine returned (XBA-296) */
            x360_apc_return(x, t);
            continue;
        }
        const x360_thunk *th = x360_thunk_at(x, pc);
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
            if (x->stop_at_miss) {
                missed(x, pc);
                break;
            }
            /* XBA-283: interpret the code the pack does not hold, record its entry, go on */
            int r = x360_interp_run(x, t, !resumed);
            if (r < 0)
                break;
            if (r > 0) {
                t->interp = 1;
                break;
            }
            continue;
        }
        x->m.rc_exit = 0;
        fault_pc = pc;
        fn(&x->m);
        if (x->m.rc_exit == XE_RC_EXIT_INTERP) {
            if (x360_interp_one(x))
                break;
        } else if (x->m.rc_exit == XE_RC_EXIT_NOT_COVERED) {
            /* a lifted function entered where it has no resume point: with the interpreter, run from there */
            if (x->stop_at_miss) {
                missed(x, pc);
                break;
            }
            int r = x360_interp_run(x, t, 1);
            if (r < 0)
                break;
            if (r > 0) {
                t->interp = 1;
                break;
            }
        }
    }
}

/* ---- guest faults ---- */

static x360_machine *fault_machine;
static struct sigaction prev_segv, prev_bus; /* what was there before on_fault: SIG_DFL when zeroed */

/* platform/android_log.h, linked in by the hosts that pump stdout / stderr into logcat */
int xbox_log_drain(int timeout_ms) __attribute__((weak));
#define X360_FAULT_LOG_DRAIN_MS 200

/* what the runtime was doing (x360_set_stage): a fault names it, so a crash before the first frame says where */
static const char *volatile fault_stage;

void x360_set_stage(const char *stage)
{
    fault_stage = stage;
}

static void *fault_host_pc(void *ctx)
{
#if defined(__linux__) && defined(__aarch64__)
    return ctx ? (void *)((ucontext_t *)ctx)->uc_mcontext.pc : NULL;
#elif defined(__linux__) && defined(__x86_64__) && defined(REG_RIP)
    return ctx ? (void *)((ucontext_t *)ctx)->uc_mcontext.gregs[REG_RIP] : NULL;
#else
    (void)ctx;
    return NULL;
#endif
}

static void on_fault(int sig, siginfo_t *si, void *ctx)
{
    x360_machine *x = fault_machine;
    uint8_t *a = si->si_addr;
    const char *stage = fault_stage;
    char msg[300];
    int n;
    if (x && a >= x->reservation && a < x->reservation + x->reserved) {
        uint64_t o = (uint64_t)(a - x->reservation); /* x360_p backwards */
        if (o >= (uint64_t)X360_VIEW_4K + x->m.view4k_skew)
            o -= x->m.view4k_skew;
        /* WP8: inside an HLE, on the host thread running it: back to x360_hle_call, which stops the machine with the
         * export's name (hle_fault_stop). Only reads of this machine's fields, pthread_self and siglongjmp here:
         * async-signal-safe for a synchronous fault on this thread. A fault in lifted code running in parallel on
         * another host thread (host threads) is not this HLE's and takes the path below. */
        void *jb = atomic_load_explicit(&x->hle_jmp, memory_order_acquire);
        if (jb && pthread_equal(pthread_self(), x->hle_host)) {
            x->hle_fault_addr = (uint32_t)o;
            siglongjmp(*(sigjmp_buf *)jb, 1);
        }
        x360_thread *t = x360_cur(x);
        n = snprintf(msg, sizeof msg,
                     "x360: guest access fault at %08X (thread %u, in lifted code entered at %08X, stage %s)\n",
                     (uint32_t)o, t ? t->id : 0, fault_pc, stage ? stage : "?");
    } else {
        n = snprintf(msg, sizeof msg, "x360: host signal %d (code %d) at %p, host pc %p, stage %s (not a guest "
                     "address)\n", sig, si->si_code, (void *)a, fault_host_pc(ctx), stage ? stage : "?");
    }
    if (n > 0 && write(2, msg, (size_t)n) < 0)
        n = 0;
    /* On Android stderr is a pipe a thread hands to logcat: drain it, or the line above dies with the process
     * (the Xbox fault path does the same, XBA-52). Weak: x360_core alone (x360_run, the tests) has no log pump. */
    if (xbox_log_drain)
        xbox_log_drain(X360_FAULT_LOG_DRAIN_MS);
    /* Then the handler that was there before ours (debuggerd on Android, through the shell's: a tombstone with the
     * backtrace, and the system records a native crash), else the default action: restored, the instruction faults
     * again and the process dies of the original signal. No _exit(1): it hid the crash from both. */
    struct sigaction *prev = sig == SIGBUS ? &prev_bus : &prev_segv;
    if (prev->sa_flags & SA_SIGINFO) {
        if (prev->sa_sigaction) {
            prev->sa_sigaction(sig, si, ctx);
            return;
        }
    } else if (prev->sa_handler != SIG_DFL && prev->sa_handler != SIG_IGN) {
        prev->sa_handler(sig);
        return;
    }
    signal(sig, SIG_DFL);
}

void x360_install_fault_handler(x360_machine *x)
{
    struct sigaction sa, cur;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    fault_machine = x;
    /* x360_run installs it on every call (once a frame): keep the handler found the first time as the one to
     * chain to, never ours. */
    sigaction(SIGSEGV, NULL, &cur);
    if (!((cur.sa_flags & SA_SIGINFO) && cur.sa_sigaction == on_fault))
        prev_segv = cur;
    sigaction(SIGBUS, NULL, &cur);
    if (!((cur.sa_flags & SA_SIGINFO) && cur.sa_sigaction == on_fault))
        prev_bus = cur;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}

static int run_host(x360_machine *x, uint64_t insns);

int x360_run(x360_machine *x, uint64_t insns)
{
    if (!x->fn_tab && !x->pack_bound) {
        x360_fail(x, "no pack bound");
        return x->status;
    }
    x360_install_fault_handler(x);
    if (x->host)
        return run_host(x, insns);
    uint64_t stop = x->m.rc_native + insns;
    x->stop_at = stop;
    x->m.rc_stop = stop; /* WP-16: run_thread's budget, as direct calls (RC_CALL) see it */
    while (x->status == X360_RUNNING) {
        if (x->m.rc_native >= stop || (x->time_limit && x360_now(x) >= x->time_limit)) {
            x->status = X360_BUDGET;
            break;
        }
        if (x->run_until && x360_now(x) >= x->run_until) {
            x->status = X360_FRAME;
            break;
        }
        x360_timers_fire(x);
        x360_kdpc_poll(x);
        x360_gpu_poll(x);
        x360_audio_poll(x);
        x360_xma_poll(x);
        x360_input_poll(x);
        if (x->timestamp_bundle) { /* KeTimeStampBundle: interrupt time, system time, tick count (ms) */
            uint64_t now = x360_now(x);
            x360_w64(x, x->timestamp_bundle, now);
            x360_w64(x, x->timestamp_bundle + 8, X360_EPOCH + now);
            x360_w32(x, x->timestamp_bundle + 16, (uint32_t)(now / 10000));
        }
        int picked = x360_kdpc_ready(x); /* a DPC runs ahead of every thread */
        for (int n = 1; picked < 0 && n <= x->nthreads; n++) {
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
            if (x->run_until && d > x->run_until)
                d = x->run_until; /* a player's frame: the clock stops at its end */
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

uint64_t x360_time(x360_machine *x)
{
    return x360_now(x);
}

int x360_run_until(x360_machine *x, uint64_t t)
{
    if (x->status != X360_RUNNING)
        return x->status;
    x->run_until = t ? t : 1;
    int r = x360_run(x, ~0ull >> 2);
    x->run_until = 0;
    if (r == X360_FRAME) {
        x->status = X360_RUNNING;
        return X360_FRAME;
    }
    return r;
}

/* ---- guest threads on host threads (XBA-283, experimental: x360_run --host-threads) ----
 *
 * Each guest thread gets a host thread (a worker) that runs its lifted code in parallel with the others, on a private
 * xe_machine (its slice, instruction count, time base and reservation; guest memory is shared). Everything else is
 * serial under one lock, as in the cooperative mode and with the same code: the HLE calls, the interpreter, the
 * special return addresses, the waits. The scheduler (the caller's thread, in x360_run) polls the devices, timers
 * and waits, hands every ready thread to its worker, and sleeps on a condition while they run. Reservations are a
 * compare-and-swap (xenon_rt.h), so packs made before XBA-283 are refused. Virtual time follows the host clock (plus
 * what is skipped when every thread waits): runs do not repeat, and save states, rewind and the player stay on the
 * cooperative mode. */

#include <time.h>

typedef struct x360_worker {
    pthread_t th;
    pthread_cond_t cv;
    int created, run;
    x360_machine *x;
    int k;
} x360_worker;

struct x360_host {
    pthread_mutex_t mu;
    pthread_cond_t main;
    int stop, drain;
    x360_worker w[X360_MAX_THREADS];
};

static uint64_t host_now_100ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 10000000u + (uint64_t)ts.tv_nsec / 100;
}

/* One guest thread on its worker, the lock held on entry and on return: until it blocks, ends, or the scheduler
 * drains the workers. */
/* the lock to the others for a moment (the interpreter's reservation lives in the shared machine: dropped) */
static void host_yield(x360_machine *x, xe_cpu *c)
{
    c->reserve = 0;
    pthread_mutex_unlock(&x->host->mu);
    sched_yield();
    pthread_mutex_lock(&x->host->mu);
}

static void host_run_thread(x360_machine *x, int k)
{
    struct x360_host *h = x->host;
    x360_thread *t = &x->threads[k];
    xe_cpu *c = &t->cpu;
    xe_machine lm;
    memset(&lm, 0, sizeof lm);
    lm.mem = x->m.mem;
    lm.mem_base = x->m.mem_base;
    lm.mem_size = x->m.mem_size;
    lm.view4k_skew = x->m.view4k_skew;
    lm.rt = x;
    lm.cur = c;
    lm.rc_stop = UINT64_MAX; /* WP-16: the loop below has no budget of its own, as RC_CALL then sees it */
    lm.rc_fn_tab = x->m.rc_fn_tab;
    lm.rc_code_lo = x->m.rc_code_lo, lm.rc_code_span = x->m.rc_code_span;
    lm.rc_thunk_lo = x->m.rc_thunk_lo, lm.rc_thunk_span = x->m.rc_thunk_span;
    while (t->state == TH_READY && x->status == X360_RUNNING && !h->drain) {
        x->cur = k; /* the HLE, the interpreter and the SEH find the running thread here */
        x->m.cur = c;
        x->m.slice = X360_SLICE;
        uint32_t pc = c->pc;
        int resumed = t->interp;
        t->interp = 0;
        if (pc == X360_THREAD_EXIT) {
            x360_thread_exit(x, t, (uint32_t)c->gpr[3]);
            break;
        }
        if (pc == X360_SEH_RETURN) {
            x360_seh_return(x, t);
            continue;
        }
        if (pc == X360_APC_RETURN) {
            x360_apc_return(x, t);
            continue;
        }
        const x360_thunk *th = x360_thunk_at(x, pc);
        if (th) {
            c->pc = (uint32_t)c->lr & ~3u;
            x->st.hle_calls++;
            sync_tb(x);
            x360_hle_call(x, th);
            pthread_cond_signal(&h->main); /* it may have signalled an object a waiting thread polls */
            continue;
        }
        if (x->m.rc_native >= x->stop_at)
            break;
        xe_rc_fn fn = pc >= x->code_lo && pc < x->code_hi && !(pc & 3) ? x->fn_tab[(pc - x->code_lo) >> 2] : NULL;
        if (!fn) {
            if (x->stop_at_miss) {
                missed(x, pc);
                break;
            }
            int r = x360_interp_run(x, t, !resumed);
            if (r < 0)
                break;
            if (r > 0) { /* its slice ended in interpreted code: let the others have the lock */
                t->interp = 1;
                host_yield(x, c);
            }
            continue;
        }
        /* lifted code, in parallel: until it needs the runtime or its slice ends */
        lm.slice = X360_SLICE;
        lm.rc_native = 0;
        lm.tb_base = x360_now(x) * 5;
        pthread_mutex_unlock(&h->mu);
        for (;;) {
            lm.rc_exit = 0;
            fn(&lm);
            if (lm.rc_exit || lm.slice < 0)
                break;
            pc = c->pc;
            fn = pc >= x->code_lo && pc < x->code_hi && !(pc & 3) ? x->fn_tab[(pc - x->code_lo) >> 2] : NULL;
            if (!fn)
                break;
        }
        pthread_mutex_lock(&h->mu);
        x->cur = k;
        x->m.cur = c;
        x->m.rc_native += lm.rc_native;
        if (lm.rc_exit == XE_RC_EXIT_INTERP) {
            if (x360_interp_one(x))
                break;
        } else if (lm.rc_exit == XE_RC_EXIT_NOT_COVERED) {
            if (x->stop_at_miss) {
                missed(x, c->pc);
                break;
            }
            int r = x360_interp_run(x, t, 1);
            if (r < 0)
                break;
            if (r > 0) {
                t->interp = 1;
                host_yield(x, c);
            }
        }
    }
}

static void *host_worker(void *arg)
{
    x360_worker *w = arg;
    x360_machine *x = w->x;
    struct x360_host *h = x->host;
    pthread_mutex_lock(&h->mu);
    for (;;) {
        while (!w->run && !h->stop)
            pthread_cond_wait(&w->cv, &h->mu);
        if (h->stop)
            break;
        host_run_thread(x, w->k);
        w->run = 0;
        pthread_cond_signal(&h->main);
    }
    pthread_mutex_unlock(&h->mu);
    return NULL;
}

static int run_host(x360_machine *x, uint64_t insns)
{
    struct x360_host *h = x->host;
    pthread_mutex_lock(&h->mu);
    uint64_t stop = x->m.rc_native + insns;
    x->stop_at = stop;
    x->m.rc_stop = stop; /* WP-16: run_thread's budget, as direct calls (RC_CALL) see it */
    while (x->status == X360_RUNNING) {
        if (x->m.rc_native >= stop || (x->time_limit && x360_now(x) >= x->time_limit)) {
            x->status = X360_BUDGET;
            break;
        }
        if (x->run_until && x360_now(x) >= x->run_until) {
            x->status = X360_FRAME;
            break;
        }
        x360_timers_fire(x);
        x360_kdpc_poll(x);
        x360_gpu_poll(x);
        x360_audio_poll(x);
        x360_xma_poll(x);
        x360_input_poll(x);
        if (x->timestamp_bundle) {
            uint64_t now = x360_now(x);
            x360_w64(x, x->timestamp_bundle, now);
            x360_w64(x, x->timestamp_bundle + 8, X360_EPOCH + now);
            x360_w32(x, x->timestamp_bundle + 16, (uint32_t)(now / 10000));
        }
        int running = 0;
        for (int k = 0; k < x->nthreads; k++) {
            x360_thread *t = &x->threads[k];
            x360_worker *w = &h->w[k];
            if (w->run) {
                running++;
                continue;
            }
            if (t->state == TH_WAIT)
                x360_wait_check(x, t);
            if (t->state != TH_READY)
                continue;
            if (!w->created) {
                w->x = x;
                w->k = k;
                pthread_cond_init(&w->cv, NULL);
                if (pthread_create(&w->th, NULL, host_worker, w)) {
                    x360_fail(x, "cannot create a host thread for guest thread %d", k);
                    break;
                }
                w->created = 1;
            }
            t->cpu.reserve = 0;
            w->run = 1;
            x->st.switches++;
            pthread_cond_signal(&w->cv);
            running++;
        }
        if (x->status != X360_RUNNING)
            break;
        if (!running) {
            uint64_t d = next_deadline(x);
            if (!d) {
                x360_fail(x, "every thread is blocked with no timeout (deadlock)");
                break;
            }
            uint64_t now = x360_now(x);
            if (x->run_until && d > x->run_until)
                d = x->run_until;
            if (d > now)
                x->time_skip += d - now;
            continue;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 500000; /* 0.5 ms: waits and timers are polled at least this often */
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&h->main, &h->mu, &ts);
    }
    /* every worker back to the lock before the caller looks at (or frees) the machine */
    h->drain = 1;
    for (;;) {
        int busy = 0;
        for (int k = 0; k < x->nthreads; k++)
            busy |= h->w[k].run;
        if (!busy)
            break;
        pthread_cond_wait(&h->main, &h->mu);
    }
    h->drain = 0;
    pthread_mutex_unlock(&h->mu);
    return x->status;
}

static void host_free(x360_machine *x)
{
    struct x360_host *h = x->host;
    if (!h)
        return;
    pthread_mutex_lock(&h->mu);
    h->stop = 1;
    for (int k = 0; k < X360_MAX_THREADS; k++)
        if (h->w[k].created)
            pthread_cond_signal(&h->w[k].cv);
    pthread_mutex_unlock(&h->mu);
    for (int k = 0; k < X360_MAX_THREADS; k++)
        if (h->w[k].created) {
            pthread_join(h->w[k].th, NULL);
            pthread_cond_destroy(&h->w[k].cv);
        }
    pthread_cond_destroy(&h->main);
    pthread_mutex_destroy(&h->mu);
    free(h);
    x->host = NULL;
}

int x360_set_host_threads(x360_machine *x, int on)
{
    if (!on) {
        host_free(x);
        return 0;
    }
    if (x->host)
        return 0;
    if (!(x->pack_features & 1)) {
        x360_fail(x, "host threads need a pack made with atomic reservations (x360_tools.recomp of XBA-283 or later)");
        return -1;
    }
    struct x360_host *h = calloc(1, sizeof *h);
    if (!h)
        return -1;
    pthread_mutex_init(&h->mu, NULL);
    pthread_cond_init(&h->main, NULL);
    uint64_t now = x360_now(x);
    x->host = h;
    x->host_t0 = host_now_100ns() - now + x->time_skip; /* the clock goes on from where it was */
    return 0;
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
    if (x360_mem_init(x, why, why_len) || x360_gpu_init(x) || x360_xma_init(x) || x360_kernel_init(x) ||
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
    if (x360_mem_init(x, why, n) || x360_gpu_init(x) || x360_xma_init(x) || x360_kernel_init(x) ||
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
    host_free(x);
    x360_audio_close(x);
    x360_fs_free(x);
    x360_gpu_free(x);
    x360_xma_free(x);
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
    /* the resume table: one array, or parts (XBA-284, big packs) */
    xe_rc_part one = {pack->resume, pack->count};
    const xe_rc_part *parts = pack->parts ? pack->parts : &one;
    unsigned nparts = pack->parts ? pack->nparts : 1, total = 0;
    uint32_t lo = 0xFFFFFFFFu, hi = 0;
    for (unsigned p = 0; p < nparts; p++)
        if (parts[p].count) {
            total += parts[p].count;
            if ((parts[p].entries[0].entry & ~3u) < lo)
                lo = parts[p].entries[0].entry & ~3u;
            if ((parts[p].entries[parts[p].count - 1].entry & ~3u) + 4 > hi)
                hi = (parts[p].entries[parts[p].count - 1].entry & ~3u) + 4;
        }
    x->helpers = pack->helpers;
    x->nhelpers = pack->helpers ? pack->nhelpers : 0;
    x->pack_features = pack->features;
    if (pack->features & ~XE_RC_FEATS_KNOWN) {
        x360_fail(x, "the pack needs a newer runtime (features %X, this one knows %X): update the app, or Prepare again",
                  pack->features, XE_RC_FEATS_KNOWN);
        return -1;
    }
    if (total && x->m.view4k_skew && !(pack->features & XE_RC_FEAT_VIEW4K)) {
        x360_fail(x, "the pack was built for hosts of 4 KB pages (this one's are %u bytes): build it again, with "
                     "XE_VIEW4K_DYNAMIC=1", (unsigned)x->host_page);
        return -1;
    }
    if (!total) { /* XBA-283: nothing lifted, the interpreter runs it all (tests) */
        x->code_lo = x->code_hi = 0;
        x->pack_bound = 1;
        return 0;
    }
    x->code_lo = lo;
    x->code_hi = hi;
    free(x->fn_tab);
    x->fn_tab = calloc((x->code_hi - x->code_lo) / 4, sizeof *x->fn_tab);
    if (!x->fn_tab)
        return -1;
    for (unsigned p = 0; p < nparts; p++)
        for (unsigned k = 0; k < parts[p].count; k++)
            x->fn_tab[(parts[p].entries[k].entry - x->code_lo) / 4] = parts[p].entries[k].fn;
    /* WP-16: what a pack's indirect calls look up (xenon_rt.h RC_CALL_IND), as run_thread does */
    x->m.rc_fn_tab = x->fn_tab;
    x->m.rc_code_lo = x->code_lo, x->m.rc_code_span = x->code_hi - x->code_lo;
    x->m.rc_thunk_lo = x->thunk_lo, x->m.rc_thunk_span = x->thunk_hi - x->thunk_lo;
    x->helpers = pack->helpers;
    x->nhelpers = pack->helpers ? pack->nhelpers : 0;
    return 0;
}

int x360_pack_from_lib(void *lib, x360_pack *pk)
{
    memset(pk, 0, sizeof *pk);
    const unsigned *count = dlsym(lib, "xe_rc_resume_count");
    const uint32_t *crc = dlsym(lib, "xe_rc_image_crc");
    if (!crc) /* WP-16: a pack an older runtime must refuse names it so (xenon_rt.h XE_RC_FEAT_DIRECT_CALLS) */
        crc = dlsym(lib, "xe_rc_image_crc2");
    pk->resume = dlsym(lib, "xe_rc_resume");
    if (!pk->resume) { /* XBA-284: a big pack's table in parts */
        const unsigned *np = dlsym(lib, "xe_rc_resume_part_count");
        pk->parts = dlsym(lib, "xe_rc_resume_parts");
        pk->nparts = pk->parts && np ? *np : 0;
    }
    if ((!pk->resume && !pk->parts) || !count || !crc)
        return -1;
    pk->count = *count;
    pk->image_crc = *crc;
    const unsigned *nh = dlsym(lib, "xe_rc_helper_count");
    if (nh) {
        pk->helpers = dlsym(lib, "xe_rc_helpers");
        pk->nhelpers = pk->helpers ? *nh : 0;
    }
    const unsigned *feat = dlsym(lib, "xe_rc_features");
    pk->features = feat ? *feat : 0;
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

void x360_set_interp(x360_machine *x, int on)
{
    x->stop_at_miss = !on;
}

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
