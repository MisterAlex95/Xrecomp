/* xbox_interp.c - the x86 interpreter's runtime side (XBA-303): what the core
 * asks (xbox_interp_hooks), callable thunks for missed addresses, the
 * discovery records, the counters. Contract: xbox_interp.h.
 *
 * A missed address gets one thunk for the whole run (a pool of plain
 * void(void) functions, each bound to one address on first use), so every
 * place that wants a recomp_func_t for a guest address can take one: the
 * indirect-call miss path (recomp_types.h), thread start routines, DPC / APC /
 * timer / IRP routines and SEH handlers the kernel bridge calls. A thunk runs
 * one activation of the core (xbox_InterpRun) from the guest state the call
 * left.
 *
 * What is written for the next Prepare: the address resolved (an "icall"
 * entry with the return address, or the caller's own record: "thread",
 * "smc" exec_outside_image), and every call the interpreted code makes to an
 * address with no lifted function ("icall"). One run names them all. */
#include "xbox_interp.h"
#include "recomp_discovery.h"
#include "recomp_smc.h"
#include "kernel.h"
#include "xbox_memory_layout.h"
#include "xbox_seh.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
/* The core needs GNU C (it is built from recomp_types.h's generated-code
 * path); an MSVC build keeps the old behaviour. */
int xbox_InterpEnabled(void) { return 0; }
xbox_interp_fn xbox_InterpResolve(uint32_t va, int kind, uint32_t ret) { (void)va; (void)kind; (void)ret; return 0; }
uint32_t xbox_InterpThunkVa(xbox_interp_fn fn) { (void)fn; return 0; }
uint32_t xbox_InterpCurrentVa(void) { return 0; }
void xbox_InterpGetStats(XboxInterpStats *out) { memset(out, 0, sizeof *out); }
int xbox_InterpDepth(void) { return 0; }
void xbox_InterpResumeAt(uint32_t va) { (void)va; }
xbox_interp_fn xbox_InterpIcallMiss(uint32_t va, int outside) { (void)va; (void)outside; return 0; }
#else

typedef void (*interp_fn)(void);
extern ptrdiff_t g_xbox_mem_offset;      /* guest memory as generated code (and the core) reach it */
interp_fn recomp_lookup(uint32_t xbox_va);
interp_fn recomp_lookup_manual(uint32_t xbox_va);
interp_fn recomp_lookup_kernel(uint32_t xbox_va);

#define GUEST32(va) (*(volatile uint32_t *)((uintptr_t)(uint32_t)(va) + (uintptr_t)g_xbox_mem_offset))

int xbox_InterpEnabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_INTERP");
        on = !(e && e[0] == '0');
    }
    return on;
}

/* ---- the counters ------------------------------------------------------------ */

static volatile uint64_t s_insns, s_activations;
static volatile uint32_t s_unimpl;

/* ---- the thunks ----------------------------------------------------------------- */

#define THUNKS 4096u
#define HASH 8192u

static uint32_t s_va[THUNKS];
static volatile uint64_t s_hot[THUNKS];        /* instructions per entry, for the hot list */
static volatile uint32_t s_used;
static uint32_t s_hash_va[HASH];
static uint16_t s_hash_k[HASH];
static volatile int s_lock;

static void lock(void) { while (__sync_lock_test_and_set(&s_lock, 1)) ; }
static void unlock(void) { __sync_lock_release(&s_lock); }

static void hot_report(void)
{
    /* the three entries that ran the most, once per doubling of the total */
    static uint64_t next = 10000000ull;
    uint64_t total = s_insns;
    uint32_t i, n = s_used, best[3] = { 0, 0, 0 };
    uint64_t bv[3] = { 0, 0, 0 };
    if (total < next)
        return;
    next = total * 2;
    for (i = 0; i < n && i < THUNKS; i++) {
        uint64_t v = s_hot[i];
        int k;
        for (k = 0; k < 3; k++)
            if (v > bv[k]) {
                int j;
                for (j = 2; j > k; j--) { bv[j] = bv[j - 1]; best[j] = best[j - 1]; }
                bv[k] = v; best[k] = i;
                break;
            }
    }
    fprintf(stderr, "[INTERP] %llu instructions interpreted so far (%u entries); hottest: 0x%08X (%llu)",
            (unsigned long long)total, n, s_va[best[0]], (unsigned long long)bv[0]);
    if (bv[1]) fprintf(stderr, ", 0x%08X (%llu)", s_va[best[1]], (unsigned long long)bv[1]);
    if (bv[2]) fprintf(stderr, ", 0x%08X (%llu)", s_va[best[2]], (unsigned long long)bv[2]);
    fprintf(stderr, ". The next Prepare lifts them (discovered.jsonl).\n");
    fflush(stderr);
}

static void interp_thunk_run(unsigned k)
{
    uint32_t va = s_va[k];
    uint32_t entry_esp = g_cpu.r_esp;
    XboxInterpStop stop;
    int r;

    memset(&stop, 0, sizeof stop);
    r = xbox_InterpRun(va, 0, &stop);
    __atomic_fetch_add(&s_insns, xbox_InterpTakeInsns(), __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_activations, 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_hot[k], stop.insns, __ATOMIC_RELAXED);
    if (r == XBOX_INTERP_UNIMPL) {
        char text[64];
        unsigned i, n = stop.len ? stop.len : 4;
        int at = snprintf(text, sizeof text, "interp:");
        for (i = 0; i < n && i < 15 && at < (int)sizeof text - 3; i++)
            at += snprintf(text + at, sizeof text - (size_t)at, " %02X", stop.bytes[i]);
        if (__atomic_fetch_add(&s_unimpl, 1u, __ATOMIC_RELAXED) < 32u) {
            fprintf(stderr, "[INTERP] cannot run the instruction at 0x%08X (%s), reached from 0x%08X: "
                    "the call returns as if it had been skipped\n", stop.va, text + 8, va);
            fflush(stderr);
        }
        recomp_discovery_unimpl(stop.va, text);
        /* as a call that could not be made: its return address popped */
        g_cpu.r_esp = entry_esp + 4u;
        g_cpu.r_eax = 0;
    }
    hot_report();
}

#define T1(n) static void interp_thunk_##n(void) { interp_thunk_run(0x##n); }
#define T16(p) T1(p##0) T1(p##1) T1(p##2) T1(p##3) T1(p##4) T1(p##5) T1(p##6) T1(p##7) \
               T1(p##8) T1(p##9) T1(p##A) T1(p##B) T1(p##C) T1(p##D) T1(p##E) T1(p##F)
#define T256(p) T16(p##0) T16(p##1) T16(p##2) T16(p##3) T16(p##4) T16(p##5) T16(p##6) T16(p##7) \
                T16(p##8) T16(p##9) T16(p##A) T16(p##B) T16(p##C) T16(p##D) T16(p##E) T16(p##F)
T256(0) T256(1) T256(2) T256(3) T256(4) T256(5) T256(6) T256(7)
T256(8) T256(9) T256(A) T256(B) T256(C) T256(D) T256(E) T256(F)
#define E1(n) interp_thunk_##n,
#define E16(p) E1(p##0) E1(p##1) E1(p##2) E1(p##3) E1(p##4) E1(p##5) E1(p##6) E1(p##7) \
               E1(p##8) E1(p##9) E1(p##A) E1(p##B) E1(p##C) E1(p##D) E1(p##E) E1(p##F)
#define E256(p) E16(p##0) E16(p##1) E16(p##2) E16(p##3) E16(p##4) E16(p##5) E16(p##6) E16(p##7) \
                E16(p##8) E16(p##9) E16(p##A) E16(p##B) E16(p##C) E16(p##D) E16(p##E) E16(p##F)
static interp_fn const s_thunk[THUNKS] = {
    E256(0) E256(1) E256(2) E256(3) E256(4) E256(5) E256(6) E256(7)
    E256(8) E256(9) E256(A) E256(B) E256(C) E256(D) E256(E) E256(F)
};

/* The thunk bound to va: *fresh set when it is bound now. -1: none left. */
static int thunk_of(uint32_t va, int *fresh)
{
    uint32_t h = (va * 2654435761u) >> 19, k;
    *fresh = 0;
    lock();
    for (k = 0; k < HASH; k++) {
        uint32_t slot = (h + k) & (HASH - 1u);
        if (s_hash_va[slot] == va && va) { unlock(); return s_hash_k[slot]; }
        if (!s_hash_va[slot]) {
            uint32_t idx = s_used;
            if (idx >= THUNKS) { unlock(); return -1; }
            s_va[idx] = va;
            s_hash_k[slot] = (uint16_t)idx;
            __atomic_store_n(&s_hash_va[slot], va, __ATOMIC_RELEASE);
            s_used = idx + 1u;
            *fresh = 1;
            unlock();
            return (int)idx;
        }
    }
    unlock();
    return -1;
}

uint32_t xbox_InterpThunkVa(xbox_interp_fn fn)
{
    uintptr_t a = (uintptr_t)fn;
    uint32_t i, n = s_used;
    for (i = 0; i < n && i < THUNKS; i++)
        if ((uintptr_t)s_thunk[i] == a)
            return s_va[i];
    return 0;
}

/* Does va look like code? The core decodes the first instructions (up to
 * the first transfer): a run of zeros (add [eax], al), an undecodable byte
 * or a fall off a page into nothing is not a function, whatever pointed at
 * it. Cheap: once per new address. */
static int gate(uint32_t va, int strict)
{
    const uint8_t *p;
    int i;
    if (va < 0x10000u || va >= 0xFE000000u)
        return 0;
    p = (const uint8_t *)((uintptr_t)va + (uintptr_t)g_xbox_mem_offset);
    if (p[0] == 0 && p[1] == 0)
        return 0;
    if (p[0] == 0xFF && p[1] == 0xFF)
        return 0;
    for (i = 0; i < 24; i++) {
        uint8_t b0;
        int n = xbox_InterpDecodeLength(va);
        if (n <= 0)
            return 0;
        b0 = *(const uint8_t *)((uintptr_t)va + (uintptr_t)g_xbox_mem_offset);
        /* outside the image (a pointer into data is likelier there): no port
         * I/O, hlt, cli / sti, int, before the first transfer */
        if (strict && ((b0 >= 0xE4 && b0 <= 0xE7) || (b0 >= 0xEC && b0 <= 0xEF) || b0 == 0xF4
                       || b0 == 0xFA || b0 == 0xFB || b0 == 0xCD || (b0 == 0xCC && i == 0)))
            return 0;
        if (b0 == 0xC3 || b0 == 0xC2 || b0 == 0xE9 || b0 == 0xEB || b0 == 0xCC)
            return 1;
        va += (uint32_t)n;
    }
    return 1;
}

static int in_image_code(uint32_t va)
{
    /* the range unknown (not set yet, a test): everything is, as RECOMP_ICALL_IS_CODE says */
    return g_xbox_code_hi == 0u || (va >= g_xbox_code_lo && va < g_xbox_code_hi);
}

static const char *kind_name(int kind)
{
    switch (kind) {
    case 'o': return "code outside the image";
    case 't': return "thread start routine";
    case 'r': return "routine the kernel calls";
    default:  return "indirect call target";
    }
}

static void interp_hooks_init(void);

xbox_interp_fn xbox_InterpResolve(uint32_t va, int kind, uint32_t ret)
{
    int fresh, k;
    if (!xbox_InterpEnabled())
        return 0;
    if (!xbox_interp_hooks.native)
        interp_hooks_init();
    /* known already: no gate, no record */
    {
        uint32_t h = (va * 2654435761u) >> 19, j;
        for (j = 0; j < 64; j++) {
            uint32_t slot = (h + j) & (HASH - 1u);
            uint32_t v = __atomic_load_n(&s_hash_va[slot], __ATOMIC_ACQUIRE);
            if (!v) break;
            if (v == va) return s_thunk[s_hash_k[slot]];
        }
    }
    if (!in_image_code(va) && kind != 'o') {
        /* code made at run time reached by a thread start or a routine: named as such (recomp_smc.h) */
        if (!xbox_SmcExecOutside(va, ret))
            return 0;
    }
    if (!gate(va, !in_image_code(va)))
        return 0;
    k = thunk_of(va, &fresh);
    if (k < 0) {
        static int said;
        if (!said++) {
            fprintf(stderr, "[INTERP] more than %u missed addresses: 0x%08X and later ones are skipped\n", THUNKS, va);
            fflush(stderr);
        }
        return 0;
    }
    if (fresh) {
        if (kind == 'c' || kind == 'r')
            recomp_discovery_icall(va, ret);
        if (s_used <= 256u) {
            fprintf(stderr, "[INTERP] no lifted code at 0x%08X (%s, return address 0x%08X): interpreted; "
                    "discovered.jsonl names it for the next Prepare\n", va, kind_name(kind), ret);
            fflush(stderr);
        }
    }
    return s_thunk[k];
}

/* The indirect-call miss path (recomp_types.h, RECOMP_ICALL_DISPATCH_): a
 * target with no dispatch entry. [esp] is the return address (a call) or the
 * jumping function's own (a tail jump). */
xbox_interp_fn xbox_InterpIcallMiss(uint32_t va, int outside)
{
    uint32_t ret;
    if (!xbox_InterpEnabled())
        return 0;
    /* An indirect tail jump publishes only g_seh_ebp (a call both): that is
     * the ebp the target starts with, or resumes with. */
    g_cpu.r_ebp = g_cpu.r_seh_ebp;
    xbox_InterpResumeAt(va);
    ret = GUEST32(g_cpu.r_esp);
    if (outside) {
        /* mapped guest memory outside the image's code: named (exec_outside_image) and run */
        if (!xbox_SmcExecOutside(va, ret))
            return 0;           /* a wild pointer: skipped, as before */
        return xbox_InterpResolve(va, 'o', ret);
    }
    return xbox_InterpResolve(va, 'c', ret);
}

/* ---- the core's hooks ------------------------------------------------------------- */

/* Lifted (or manual) entries never change over a run: kept per thread, hits
 * and misses. A kernel thunk VA and an SEH resume target are asked every
 * time: their lookups hand state to the function they return, which must be
 * called next (recomp_lookup_kernel's slot, xbox_SehLookup's pending target). */
typedef struct { uint32_t va; interp_fn fn; } native_slot;
#define NATIVE_SLOTS 1024u
#define NATIVE_NONE ((interp_fn)(uintptr_t)1)
static __thread native_slot *t_native;

static xbox_interp_fn hook_native(uint32_t va)
{
    native_slot *e = 0;
    interp_fn fn;
    if (va >= 0xFE000000u)
        return recomp_lookup_kernel(va);
    if (!t_native)
        t_native = (native_slot *)calloc(NATIVE_SLOTS, sizeof *t_native);
    if (t_native) {
        e = &t_native[(va * 2654435761u) >> 22];
        if (e->va == va && e->fn) {
            if (e->fn != NATIVE_NONE)
                return e->fn;
            return recomp_lookup_kernel(va);        /* xbox_SehLookup */
        }
    }
    fn = recomp_lookup_manual(va);
    if (!fn) fn = recomp_lookup(va);
    if (e) { e->va = va; e->fn = fn ? fn : NATIVE_NONE; }
    return fn ? fn : recomp_lookup_kernel(va);
}

static void hook_note_call(uint32_t target, uint32_t ret)
{
    /* a call interpreted code made to an address with no lifted function */
    static uint32_t seen[256];
    uint32_t h = (target * 2654435761u) >> 24;
    if (seen[h] == target)
        return;
    seen[h] = target;
    if (in_image_code(target)) {
        recomp_discovery_icall(target, ret);
    } else {
        (void)xbox_SmcExecOutside(target, ret);
    }
}

static void hook_back_edge(uint32_t va)
{
    /* a loop that never returns (an interpreted worker) still counts */
    uint64_t n = xbox_InterpTakeInsns();
    if (n) {
        __atomic_fetch_add(&s_insns, n, __ATOMIC_RELAXED);
        hot_report();
    }
    if (__atomic_load_n(&g_xbox_guest_cpu_preempt, __ATOMIC_RELAXED))
        xbox_GuestCpuPreempt(va);
}

static void hook_poll_wait(uint32_t addr, uint32_t mask, int while_set, uint32_t va)
{
    (void)xbox_GuestPollWait((volatile uint32_t *)((uintptr_t)addr + (uintptr_t)g_xbox_mem_offset),
                             mask, while_set, va);
}

static void hook_point_take(uint64_t buf[4])
{
    xbox_SehPointTake((XboxSehPoint *)buf);
}

static int hook_point_resume(const uint64_t buf[4], uint32_t va, uint32_t func)
{
    return xbox_SehPointResume((const XboxSehPoint *)buf, va, func);
}

static void interp_hooks_init(void)
{
    xbox_interp_hooks.point_take = hook_point_take;
    xbox_interp_hooks.point_resume = hook_point_resume;
    xbox_interp_hooks.native = hook_native;
    xbox_interp_hooks.note_call = hook_note_call;
    xbox_interp_hooks.back_edge = hook_back_edge;
    xbox_interp_hooks.poll_wait = hook_poll_wait;
}

void xbox_InterpGetStats(XboxInterpStats *out)
{
    out->insns = s_insns;
    out->activations = s_activations;
    out->entries = s_used;
    out->unimpl = s_unimpl;
}

/* The runtime files that also link without the interpreter (xbox_interp.h
 * « optional links »): filled at load, before main, so the first guest code
 * already finds them, as the weak references did. */
__attribute__((constructor)) static void interp_links_register(void)
{
    xbox_seh_interp_resolve = xbox_InterpResolve;
    xbox_seh_interp_current_va = xbox_InterpCurrentVa;
    recomp_stack_switch_interp_resolve = xbox_InterpResolve;
    recomp_status_interp_stats = xbox_InterpGetStats;
}

#endif /* !_MSC_VER */
