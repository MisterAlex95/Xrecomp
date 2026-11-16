/*
 * Xbox save states, part 3 (XBA-247): a parked thread's guest call chain, read
 * from its frame shadow stack.
 *
 * The guest workers are host routines written the way tools/recomp lifts code:
 * each "function" that may park records itself on entry and drops its frame
 * on the way out (RECOMP_FRAME_ENTRY / LEAVE, xbox_memory_layout.h, the same
 * macros recomp_types.h gives the generated code), keeps ebp in a C local and
 * publishes it before a call; the deepest one calls KeDelayExecutionThread
 * through the kernel thunk by ordinal. The test registers the call-site table
 * the lifter would emit for them.
 *
 *   render   worker -> main -> frame -> present -> KeDelayExecutionThread:
 *            four frames; frame uses ebp as its frame pointer, main as a
 *            scratch register taking one of two values every turn
 *   audio    worker -> mix -> KeDelayExecutionThread, worker calling mix from
 *            one of two call sites alternately (the same kernel call at the
 *            same esp): two chain signatures
 *   deep     recursion 1100 frames deep (past RECOMP_FRAME_SLOTS), then a
 *            sleep: the chain is flagged OVERFLOW, and the frame top comes back
 *            exactly on the way up
 *   fiber    one frame whose call site is in a stack-switching function: the
 *            chain is flagged STACK_SWITCH
 *
 *   Chain      xbox_StateChain on the frozen threads: frames, return VAs, esp,
 *              ebp, functions, flags; the signature is the same over freezes
 *              at the same chain and differs between the audio thread's two
 *              sites
 *   State      xbox_StateCapture: CPU v4 carries each thread's chain; the
 *              reader returns the same frames, and refuses a flipped frame
 *              word (signature), a truncated chain, trailing bytes
 *   Snapshot   xbox_SehRuntimeSnapshot / Restore carry the frame top (a
 *              longjmp or an SEH resume over lifted frames)
 *   No table   with no call-site table registered, chains say NO_TABLE
 *   Load       xbox_StateLoad (same run, XBA-249) compares chains: a state with
 *              the deep thread refuses (overflow), then with the fiber thread
 *              (stack switch); with render and audio only, a load is accepted
 *              when every chain and frame ebp matches, refused naming the
 *              chain when the audio thread is at its other site, and naming the
 *              frame when main holds its other ebp
 */
#include "kernel.h"
#include "kernel_park.h"
#include "xbox_memory_layout.h"
#include "xbox_seh.h"
#include "xbox_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef void (*recomp_func_t)(void);
static void worker(void);
#define WORKER_VA 0x00010000u
recomp_func_t recomp_lookup(uint32_t va) { return va == WORKER_VA ? worker : NULL; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }

extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
extern ptrdiff_t g_xbox_mem_offset;

#define G32(va) (*(volatile uint32_t *)((uintptr_t)(va) + g_xbox_mem_offset))

enum { ORD_DELAY = 99, ORD_PSCREATE = 255, ORD_PSTERM = 258 };
static const uint32_t ORDS[] = { ORD_DELAY, ORD_PSCREATE, ORD_PSTERM };
#define NORDS (sizeof ORDS / sizeof ORDS[0])

enum { K_RENDER, K_AUDIO, K_DEEP, K_FIBER, K_COUNT };

/* The guest functions and their call sites (return VAs), as the lifter would name them. */
#define F_WORKER  0x00012000u
#define F_MAIN    0x00012100u
#define F_FRAME   0x00012200u
#define F_PRESENT 0x00012300u
#define F_MIX     0x00012400u
#define F_RECURSE 0x00012500u
#define F_FIBER   0x00012600u
#define RET_MAIN      0x00012005u   /* worker: call main */
#define RET_FRAME     0x00012110u   /* main: call frame */
#define RET_PRESENT   0x00012210u   /* frame: call present */
#define RET_P_SLEEP   0x00012310u   /* present: call [KeDelayExecutionThread] */
#define RET_MIX       0x00012015u   /* worker: call mix, site A */
#define RET_MIX2      0x0001201Au   /* worker: call mix, site B (same esp) */
#define RET_MIX_SLEEP 0x00012410u   /* mix: call [KeDelay...] */
#define RET_RECURSE0  0x00012025u   /* worker: call recurse */
#define RET_RECURSE   0x00012510u   /* recurse: call recurse */
#define RET_R_SLEEP   0x00012520u   /* recurse: call [KeDelay...] */
#define RET_FIBER     0x00012035u   /* worker: call fiber */
#define RET_F_SLEEP   0x00012610u   /* fiber: call [KeDelay...] */

static const uint32_t SITES[] = {
    RET_MAIN, F_WORKER, 0,
    RET_MIX, F_WORKER, 0,
    RET_MIX2, F_WORKER, 0,
    RET_RECURSE0, F_WORKER, 0,
    RET_FIBER, F_WORKER, XBOX_CALL_SITE_STACK_SWITCH,   /* a chain through a stack switch */
    RET_FRAME, F_MAIN, 0,
    RET_PRESENT, F_FRAME, 0,
    RET_P_SLEEP, F_PRESENT, XBOX_CALL_SITE_INDIRECT | XBOX_CALL_SITE_NO_EBP,
    RET_MIX_SLEEP, F_MIX, XBOX_CALL_SITE_INDIRECT,
    RET_RECURSE, F_RECURSE, 0,
    RET_R_SLEEP, F_RECURSE, XBOX_CALL_SITE_INDIRECT,
    RET_F_SLEEP, F_FIBER, XBOX_CALL_SITE_INDIRECT,
};
#define NSITES (sizeof SITES / sizeof SITES[0] / 3)

#define DEEP 1100u
#define MAIN_EBP  0xE0000001u       /* main's ebp: a scratch value (FPO), one of two */
#define MAIN_EBP2 0xE0000002u
#define WORKER_EBP(kind) (0xEB000000u | (uint32_t)(kind))

static int failures;
static uint32_t g_thunks, g_stack, g_handles;
static volatile int g_stop, g_fiber_stop;
static volatile int g_audio_site;            /* 0: A, 1: B, flipped every call */
static volatile uint32_t g_frame_ebp_before;
static volatile uint32_t g_deep_bad;         /* functions that found the frame top wrong on their way out */
static volatile int g_deep_done;
static RecompCpu *volatile g_render_cpu;      /* the render thread's g_cpu */

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-70s %s", what, ok ? "PASS" : "FAIL");
    if (!ok && detail) printf(" -- %s", detail);
    putchar('\n');
    if (!ok) failures++;
}

static recomp_func_t thunk(uint32_t ordinal)
{
    unsigned i;
    for (i = 0; i < NORDS; i++)
        if (ORDS[i] == ordinal)
            return recomp_lookup_kernel(G32(g_thunks + 4 * i));
    return NULL;
}

#define GPUSH(v) do { g_esp -= 4; G32(g_esp) = (uint32_t)(v); } while (0)

/* KeDelayExecutionThread(KernelMode, FALSE, &interval): the arguments and the return address pushed, the
 * thunk called; the caller's ebp published first when it has one (what lifted code does before a call). */
#define SLEEP_CALL(ret, ms) do { \
        uint32_t _sp0 = g_esp, _iv = (g_esp - 64u) & ~7u; \
        int64_t _rel = -(int64_t)(ms) * 10000; \
        G32(_iv) = (uint32_t)_rel; G32(_iv + 4) = (uint32_t)((uint64_t)_rel >> 32); \
        g_esp = _iv - 16u; \
        GPUSH(_iv); GPUSH(0); GPUSH(0); GPUSH(ret); \
        thunk(ORD_DELAY)(); \
        g_esp = _sp0; \
    } while (0)
/* A guest call from a function with an ebp local: ebp published, the return address pushed. */
#define GCALL(ret, call) do { g_ebp = ebp; GPUSH(ret); call; } while (0)

/* present: no ebp local; it may park, so it records itself. */
static void f_present(void)
{
    RECOMP_FRAME_ENTRY();
    SLEEP_CALL(RET_P_SLEEP, 3);
    g_esp += 4;                                  /* ret */
    RECOMP_FRAME_LEAVE();
}

/* frame: push ebp; mov ebp, esp; call present; leave; ret */
static void f_frame(void)
{
    RECOMP_FRAME_ENTRY();
    uint32_t ebp = g_ebp;
    GPUSH(ebp);
    ebp = g_esp;
    g_frame_ebp_before = ebp;
    GCALL(RET_PRESENT, f_present());
    g_esp = ebp;
    g_esp += 4;
    g_esp += 4;                                  /* ret */
    RECOMP_FRAME_LEAVE();
}

/* main: ebp a scratch register, one of two values, flipped every turn */
static void f_main(void)
{
    RECOMP_FRAME_ENTRY();
    uint32_t ebp = MAIN_EBP;
    while (!g_stop) {
        GCALL(RET_FRAME, f_frame());
        ebp = ebp == MAIN_EBP ? MAIN_EBP2 : MAIN_EBP;
    }
    g_esp += 4;
    RECOMP_FRAME_LEAVE();
}

static void f_mix(void)
{
    RECOMP_FRAME_ENTRY();
    uint32_t ebp = 0xA0D10000u;
    g_ebp = ebp;
    SLEEP_CALL(RET_MIX_SLEEP, 2);
    g_esp += 4;
    RECOMP_FRAME_LEAVE();
}

static void f_recurse(uint32_t level)
{
    RECOMP_FRAME_ENTRY();
    uint32_t ebp = 0xDE000000u | level;
    if (level + 1 < DEEP) {
        GCALL(RET_RECURSE, f_recurse(level + 1));
    } else {
        g_ebp = ebp;
        while (!g_stop && !g_deep_done)
            SLEEP_CALL(RET_R_SLEEP, 5);
    }
    if (_gff < g_cpu.r_flimit && g_cpu.r_ftop != _gff + 1)   /* every deeper frame dropped */
        g_deep_bad++;
    g_esp += 4;
    RECOMP_FRAME_LEAVE();
}

static void f_fiber(void)
{
    RECOMP_FRAME_ENTRY();
    uint32_t ebp = 0xF1BE0000u;
    g_ebp = ebp;
    while (!g_stop && !g_fiber_stop)
        SLEEP_CALL(RET_F_SLEEP, 4);
    g_esp += 4;
    RECOMP_FRAME_LEAVE();
}

/* The thread's start routine (lifted code too). */
static void worker(void)
{
    RECOMP_FRAME_ENTRY();
    int kind = (int)G32(g_esp + 4);
    uint32_t ebp = WORKER_EBP(kind);

    switch (kind) {
    case K_RENDER:
        g_render_cpu = &g_cpu;
        GCALL(RET_MAIN, f_main());
        break;
    case K_AUDIO:
        while (!g_stop) {
            GCALL(g_audio_site ? RET_MIX2 : RET_MIX, f_mix());   /* the same call at the same depth */
            g_audio_site ^= 1;
        }
        break;
    case K_DEEP:
        GCALL(RET_RECURSE0, f_recurse(0));
        g_deep_done = 2;
        break;
    case K_FIBER:
        GCALL(RET_FIBER, f_fiber());
        break;
    }
    if (g_cpu.r_ftop != _gff + 1)
        g_deep_bad++;
    RECOMP_FRAME_LEAVE();
}

static void spawn(int i, int kind)
{
    uint32_t hva = g_handles + 4u * (uint32_t)i;
    uint32_t argv[10] = { hva, 0, 0, 0, 0, (uint32_t)kind, 0, 0, 0, WORKER_VA };
    recomp_func_t fn = thunk(ORD_PSCREATE);
    unsigned k;

    G32(g_stack) = 0xBEEF0001u;
    for (k = 0; k < 10; k++)
        G32(g_stack + 4 + 4 * k) = argv[k];
    g_esp = g_stack;
    fn();
}

static int wait_threads(int n, int ms)
{
    XboxParkStats st;
    int t;
    for (t = 0; t < ms; t++) {
        xbox_ParkStatsGet(&st, 0);
        if (st.threads == n)
            return 1;
        usleep(1000);
    }
    return 0;
}

/* The frozen threads (xbox_ParkRecords) as XboxStateThreads, by their first frame's call site. */
typedef struct {
    XboxStateThread t;
    XboxStateChain c;
    XboxStateFrame f[RECOMP_FRAME_SLOTS];
    int n;
} Th;
static Th g_th[K_COUNT];

static int kind_of(const XboxStateFrame *f, int n)
{
    if (n < 2) return -1;
    switch (f[1].ret_va) {
    case RET_MAIN: return K_RENDER;
    case RET_MIX: case RET_MIX2: return K_AUDIO;
    case RET_RECURSE0: return K_DEEP;
    case RET_FIBER: return K_FIBER;
    }
    return -1;
}

static int read_chains(void)
{
    XboxParkRecord rec[XBOX_PARK_MAX_THREADS];
    int n = xbox_ParkRecords(rec, XBOX_PARK_MAX_THREADS), i, got = 0;

    memset(g_th, 0, sizeof g_th);
    for (i = 0; i < n; i++) {
        XboxStateThread t;
        static XboxStateFrame f[RECOMP_FRAME_SLOTS];
        XboxStateChain c;
        int k, fn;
        memset(&t, 0, sizeof t);
        t.slot = rec[i].slot;
        t.ordinal = rec[i].ordinal;
        t.ret_va = rec[i].ret_va;
        t.returned = rec[i].returned;
        t.site = rec[i].site;
        memcpy(&t.cpu, rec[i].cpu, sizeof t.cpu);
        fn = xbox_StateChain(&t, f, RECOMP_FRAME_SLOTS, &c);
        k = kind_of(f, fn);
        if (k < 0)
            continue;
        g_th[k].t = t;
        g_th[k].c = c;
        g_th[k].n = fn;
        memcpy(g_th[k].f, f, sizeof f);
        got |= 1 << k;
    }
    return got;
}

static void phase_chain(void)
{
    char d[256];
    uint64_t render_sig = 0, audio_sig[2] = { 0, 0 };
    int round, same = 1, audio_seen = 0, ok;
    Th *r = &g_th[K_RENDER];

    for (round = 0; round < 12; round++) {
        if (!xbox_ParkFreeze(1000, NULL)) {
            check(0, "freeze", NULL);
            return;
        }
        ok = read_chains();
        xbox_ParkThaw();
        if (ok != (1 << K_COUNT) - 1) {
            snprintf(d, sizeof d, "threads found 0x%x", ok);
            check(0, "every worker's chain is read", d);
            return;
        }
        if (round == 0) {
            const XboxStateFrame *f = r->f;
            printf("  render chain: %u frames, flags 0x%x, signature %016llx\n", r->c.frames, r->c.flags,
                   (unsigned long long)r->c.signature);
            check(r->n == 4 && r->c.frames == 4 && r->c.base == 0,
                  "render: four frames, its routine and three calls deep", NULL);
            check(f[1].ret_va == RET_MAIN && f[2].ret_va == RET_FRAME
                  && f[3].ret_va == RET_PRESENT && f[1].func == F_WORKER && f[2].func == F_MAIN
                  && f[3].func == F_FRAME,
                  "render: return VAs and functions from the call-site table", NULL);
            check(r->c.flags == 0, "render: restorable (no flag; the kernel call is below the top frame)", NULL);
            check(f[1].ebp == WORKER_EBP(K_RENDER) && (f[2].ebp == MAIN_EBP || f[2].ebp == MAIN_EBP2)
                  && f[3].ebp == g_frame_ebp_before && r->t.cpu.r_ebp == f[3].ebp,
                  "render: each frame's ebp is its caller's, the innermost one in the record", NULL);
            check(f[0].esp > f[1].esp && f[1].esp > f[2].esp && f[2].esp > f[3].esp
                  && f[3].esp > r->t.cpu.r_esp, "render: esp falls frame by frame to the kernel call's", NULL);
            check(r->t.ordinal == ORD_DELAY && r->t.ret_va == RET_P_SLEEP, "render: in the kernel call", NULL);
            check(xbox_StateChainSignature(f, 4) == r->c.signature,
                  "render: the signature is xbox_StateChainSignature of the frames", NULL);
        }
        if (render_sig && render_sig != r->c.signature)
            same = 0;
        render_sig = r->c.signature;
        audio_sig[g_th[K_AUDIO].f[1].ret_va == RET_MIX2] = g_th[K_AUDIO].c.signature;
        audio_seen |= 1 << (g_th[K_AUDIO].f[1].ret_va == RET_MIX2);
        usleep(7000);
    }
    check(same, "render: the same signature over 12 freezes", NULL);
    check(audio_seen == 3 && audio_sig[0] != audio_sig[1],
          "audio: its two call sites give two signatures", NULL);

    check((g_th[K_DEEP].c.flags & XBOX_CHAIN_OVERFLOW) && g_th[K_DEEP].n == RECOMP_FRAME_SLOTS,
          "deep: past RECOMP_FRAME_SLOTS the chain says OVERFLOW", NULL);
    check(g_th[K_FIBER].c.flags == XBOX_CHAIN_STACK_SWITCH,
          "fiber: through a stack-switching function: STACK_SWITCH", NULL);
}

static void phase_top_frame(void)
{
    RecompFrame fr[8];
    uint32_t depth = 0, base = 0;
    int n;
    Th *r;

    if (!xbox_ParkFreeze(1000, NULL)) {
        check(0, "freeze", NULL);
        return;
    }
    read_chains();
    r = &g_th[K_RENDER];
    n = xbox_ParkFrames(r->t.slot, fr, 8, &depth, &base);
    xbox_ParkThaw();
    check(n == 4 && depth == 4 && base == 0, "render: xbox_ParkFrames gives its four frames", NULL);
}

static void phase_state(void)
{
    char d[256];
    uint8_t *b = NULL, *copy;
    size_t n = 0, len = 0;
    const uint8_t *p;
    uint32_t ver = 0;
    XboxStateStats st;
    XboxStateThread th[XBOX_PARK_MAX_THREADS];
    static XboxStateFrame f[RECOMP_FRAME_SLOTS];
    int rc, cnt, k, render = -1;

    rc = xbox_StateCapture(1000, &b, &n, &st);
    snprintf(d, sizeof d, "%s", st.reason);
    check(rc == XBOX_STATE_OK, "a state is captured", d);
    if (rc != XBOX_STATE_OK)
        return;
    check(xbox_StateCheck(b, n, NULL) == XBOX_STATE_OK, "it reads back", NULL);
    if (xbox_StateSection(b, n, XBOX_STATE_TAG_CPU, &p, &len, &ver) != XBOX_STATE_OK) {
        check(0, "CPU section", NULL);
        free(b);
        return;
    }
    check(ver == 4, "CPU section version 4", NULL);
    cnt = xbox_StateCpuThreads(p, len, th, XBOX_PARK_MAX_THREADS);
    check(cnt == K_COUNT, "four threads", NULL);
    for (k = 0; k < cnt; k++) {
        int fn = xbox_StateCpuChain(p, len, k, f, RECOMP_FRAME_SLOTS);
        printf("  thread %d: %u frames, flags 0x%x, signature %016llx\n", k, th[k].chain.frames,
               th[k].chain.flags, (unsigned long long)th[k].chain.signature);
        if (fn == 4 && f[1].ret_va == RET_MAIN)
            render = k;
    }
    check(render >= 0 && th[render].chain.flags == 0 && th[render].chain.signature == g_th[K_RENDER].c.signature,
          "the render thread's chain in the state: the signature read live", NULL);
    copy = malloc(len);
    if (copy && render >= 0) {
        /* Its frames start after the threads before it: find RET_FRAME's word and flip it. */
        size_t at;
        memcpy(copy, p, len);
        for (at = 0; at + 4 <= len; at++)
            if (memcmp(copy + at, &(uint32_t){ RET_FRAME }, 4) == 0)
                break;
        if (at + 4 <= len) {
            copy[at] ^= 1;
            check(xbox_StateCpuThreads(copy, len, NULL, 0) < 0, "a flipped frame word is refused (signature)",
                  NULL);
        }
        memcpy(copy, p, len);
        check(xbox_StateCpuThreads(copy, len - 1, NULL, 0) < 0, "a truncated chain is refused", NULL);
        free(copy);
        copy = malloc(len + 4);
        if (copy) {
            memcpy(copy, p, len);
            memset(copy + len, 0, 4);
            check(xbox_StateCpuThreads(copy, len + 4, NULL, 0) < 0, "trailing bytes are refused", NULL);
        }
    }
    free(copy);
    free(b);
}

static void phase_snapshot(void)
{
    uint64_t s;
    RecompCpu keep = g_cpu;
    RecompFrame *f = calloc(RECOMP_FRAME_SLOTS + 2, sizeof *f);

    g_cpu.r_fbase = f;
    g_cpu.r_flimit = f + RECOMP_FRAME_SLOTS;
    g_cpu.r_ftop = f + 5;
    s = xbox_SehRuntimeSnapshot();
    g_cpu.r_ftop = f + 9;
    xbox_SehRuntimeRestore(s);
    check(g_cpu.r_ftop == f + 5, "xbox_SehRuntimeSnapshot / Restore carry the frame top", NULL);
    g_cpu.r_ftop = keep.r_ftop;
    g_cpu.r_fbase = keep.r_fbase;
    g_cpu.r_flimit = keep.r_flimit;
    free(f);
}

static void phase_no_table(void)
{
    recomp_call_sites_register(NULL, 0);
    if (!xbox_ParkFreeze(1000, NULL)) {
        check(0, "freeze", NULL);
        return;
    }
    {
        XboxParkRecord rec[XBOX_PARK_MAX_THREADS];
        int n = xbox_ParkRecords(rec, XBOX_PARK_MAX_THREADS), i, all = n > 0;
        for (i = 0; i < n; i++) {
            XboxStateThread t;
            XboxStateChain c;
            memset(&t, 0, sizeof t);
            t.slot = rec[i].slot;
            memcpy(&t.cpu, rec[i].cpu, sizeof t.cpu);
            t.ret_va = rec[i].ret_va;
            t.returned = rec[i].returned;
            t.site = rec[i].site;
            if (xbox_StateChain(&t, NULL, 0, &c) < 0 || !(c.flags & XBOX_CHAIN_NO_TABLE))
                all = 0;
        }
        check(all, "no call-site table: every chain says NO_TABLE", NULL);
    }
    xbox_ParkThaw();
    recomp_call_sites_register(SITES, NSITES);
}

/* Captures and loads until a load is refused for `want` (20 tries): 1 when it was. */
static int refused_for(const char *want, char *d, size_t dn)
{
    int k;
    for (k = 0; k < 20; k++) {
        uint8_t *b = NULL;
        size_t n = 0;
        XboxStateStats st;
        XboxStateLoadStats ls;
        int rc = xbox_StateCapture(1000, &b, &n, &st);
        if (rc == XBOX_STATE_OK) {
            memset(&ls, 0, sizeof ls);
            rc = xbox_StateLoad(b, n, 500, &ls);
            snprintf(d, dn, "%s: %s", xbox_StateError(rc), ls.reason);
            if (rc == XBOX_STATE_ECHAIN && strstr(ls.reason, want)) {
                free(b);
                return 1;
            }
        }
        free(b);
    }
    return 0;
}

static void phase_load(void)
{
    char d[300];
    uint8_t *b = NULL;
    size_t n = 0;
    XboxStateStats st;
    XboxStateLoadStats ls;
    int rc, k, ok = 0, chain_refused = 0, ebp_refused = 0, other = 0;

    /* Threads are checked in turn: the audio thread at its other site may refuse first. */
    check(refused_for("deeper than the frame shadow stack", d, sizeof d),
          "with the deep thread: its load refuses, the chain is deeper than the shadow stack", d);
    g_deep_done = 1;
    check(wait_threads(3, 3000), "the deep thread ends", NULL);
    check(refused_for("stack-switching", d, sizeof d),
          "with the fiber thread: its load refuses, a chain through a stack-switching function", d);
    g_fiber_stop = 1;
    check(wait_threads(2, 3000), "the fiber thread ends", NULL);
    for (k = 0; k < 60 && !(ok && chain_refused && ebp_refused); k++) {
        b = NULL;
        rc = xbox_StateCapture(1000, &b, &n, &st);
        if (rc != XBOX_STATE_OK) {
            free(b);
            other++;
            continue;
        }
        usleep(3000 + 1000 * (k % 5));
        memset(&ls, 0, sizeof ls);
        rc = xbox_StateLoad(b, n, 500, &ls);
        if (rc == XBOX_STATE_OK)
            ok++;
        else if (rc == XBOX_STATE_ECHAIN && strstr(ls.reason, "another call chain"))
            chain_refused++;
        else if (rc == XBOX_STATE_ECHAIN && strstr(ls.reason, "another ebp in frame"))
            ebp_refused++;
        else {
            printf("  load %d: %s (%s)\n", k, xbox_StateError(rc), ls.reason);
            other++;
        }
        free(b);
    }
    snprintf(d, sizeof d, "%d loaded, %d refused for the chain, %d for a frame's ebp, %d other", ok, chain_refused,
             ebp_refused, other);
    check(ok > 0, "render and audio: a load at the same chains and frame ebp is accepted", d);
    check(chain_refused > 0, "a load with the audio thread at its other call site refuses, naming the chain", d);
    check(ebp_refused > 0, "a load with main's other ebp in its frame refuses, naming the frame", d);
}

static unsigned char *load(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    unsigned char *b;
    long len;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)len);
    if (b && fread(b, 1, (size_t)len, f) != (size_t)len) { free(b); b = NULL; }
    fclose(f);
    *n = (size_t)len;
    return b;
}

int main(int argc, char **argv)
{
    size_t len = 0;
    unsigned char *xbe;
    unsigned i;

    setvbuf(stdout, NULL, _IONBF, 0);
    setenv("RECOMP_KERNEL_TRACE", "0", 0);
    xbe = load(argc > 1 ? argv[1] : "tools/conformance/test.xbe", &len);
    if (!xbe || !xbox_MemoryLayoutInit(xbe, len)) {
        printf("cannot map guest memory (run tools/conformance/mkxbe.py)\n");
        return 2;
    }
    g_thunks  = xbox_HeapAlloc(0x1000, 4096);
    g_stack   = xbox_HeapAlloc(0x1000, 4096);
    g_handles = xbox_HeapAlloc(0x100, 4096);
    for (i = 0; i < NORDS; i++)
        G32(g_thunks + 4 * i) = 0x80000000u | ORDS[i];
    G32(g_thunks + 4 * NORDS) = 0;
    xbox_kernel_set_thunk_address(g_thunks, NORDS + 1);
    xbox_kernel_bridge_init();
    xbox_SetThreadMode(XBOX_THREAD_MODE_SPAWN);
    xbox_GuestCpuPart();
    recomp_call_sites_register(SITES, NSITES);
    check(xbox_CallSiteCount() == NSITES, "the call-site table is registered", NULL);
    {
        uint32_t fn = 0, fl = 0;
        check(xbox_CallSiteLookup(RET_P_SLEEP, &fn, &fl) && fn == F_PRESENT
              && fl == (XBOX_CALL_SITE_INDIRECT | XBOX_CALL_SITE_NO_EBP) && !xbox_CallSiteLookup(0x12345u, NULL, NULL),
              "a call site is found by its return VA, an unknown one is not", NULL);
    }

    spawn(0, K_RENDER);
    spawn(1, K_AUDIO);
    spawn(2, K_DEEP);
    spawn(3, K_FIBER);
    check(wait_threads(K_COUNT, 3000), "four workers start", NULL);
    usleep(50000);

    phase_chain();
    phase_top_frame();
    phase_state();
    phase_snapshot();
    phase_no_table();
    phase_load();

    g_deep_done = 1;
    g_stop = 1;
    check(wait_threads(0, 5000), "the workers end", NULL);
    check(g_deep_bad == 0, "the frame top came back exactly at every level, 1100 deep included", NULL);
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
