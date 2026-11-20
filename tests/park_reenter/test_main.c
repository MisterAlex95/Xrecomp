/*
 * Xbox save states in a run, XBA-294: a load whose state has guest threads parked at other call chains than the
 * ones they stand at now. Such a thread leaves its host frames (a longjmp to where the runtime started its
 * routine, on the same host thread) and re-enters the state's chain through the resume entries of XBA-250, as a
 * thread of a fresh process does; a thread at the same chain gets its registers back in place (XBA-249).
 *
 * The guest functions are host routines written the way tools/recomp lifts code with resume entries (as in
 * tests/park_resume): RECOMP_FRAME_ENTRY_R, ebp in a C local published before every call, a label before every
 * call that may park and after every host wait, the translator's resume block at the end; the test registers
 * the call-site table the lifter would emit. A word of guest memory, PHASE, sends three of the threads down one
 * of two call chains, so a state captured in phase 0 and loaded in phase 1 finds them elsewhere:
 *
 *   main     the INLINE main thread (its routine runs inside its creator's PsCreateSystemThreadEx, on a host thread
 *            of the test, under a lifted entry frame that stays below it): worker -> a -> kernel (phase 0),
 *            worker -> b -> b2 -> kernel (phase 1)
 *   loader   the same shape on a spawned worker (2 ms sleeps)
 *   long     worker -> a / b -> a 120 ms sleep: mostly captured inside its call, so it is made again
 *   render   worker -> main -> frame -> present -> kernel: main's ebp flips every turn, so its chain differs by
 *            an ebp now and then (refused before XBA-294)
 *   halt     worker -> poll -> a hlt loop's host wait: always at the same chain (registers put back in place)
 *   seh      worker -> fn through a call site in a function with __try (SEH_ANCHOR): fn sleeps at one of two
 *            sites (SEHSITE); its chain cannot be re-entered, so a load that finds it at the other site refuses
 *
 * Every function stores the registers and the ebp it holds in guest memory before its call and compares them after
 * it; the innermost ones count their turns in guest memory (CNT) and remember on the host the count they left: a
 * count that is not the one they left is a load, and the first one seen after it must be the state's.
 *
 *   Refused   seh at its other site: the load refuses naming the exception handler, nothing changes
 *   Reenter   (seh gone) a state of phase 0 loaded in phase 1: loader, long and main re-enter their chains (and
 *             render when its ebp differs), halt is put back in place; each thread's first turn after the load
 *             finds the state's count, registers and every frame's ebp are right, the three go back to phase 0
 *   Again     ten rounds of capture, phase flip, load, and a capture right after the load; two loads back to back
 *   End       every thread ends, the main thread back through its creator's frame
 */
#include "kernel.h"
#include "kernel_park.h"
#include "xbox_memory_layout.h"
#include "xbox_resume.h"
#include "xbox_state.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef void (*recomp_func_t)(void);
extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t va);
extern ptrdiff_t g_xbox_mem_offset;

#define G32(va) (*(volatile uint32_t *)((uintptr_t)(va) + g_xbox_mem_offset))

enum { ORD_DELAY = 99, ORD_PSCREATE = 255, ORD_PSTERM = 258 };
static const uint32_t ORDS[] = { ORD_DELAY, ORD_PSCREATE, ORD_PSTERM };
#define NORDS (sizeof ORDS / sizeof ORDS[0])

enum { K_MAIN, K_LOADER, K_LONG, K_RENDER, K_HALT, K_SEH, K_COUNT };
#define K_PLAY 5                    /* the threads of the re-entry rounds: all but seh */
static const char *const NAMES[K_COUNT] = { "main", "loader", "long", "render", "halt", "seh" };

/* Function start VAs and call sites (return VAs). */
#define F_MWORKER 0x00012000u
#define F_MA      0x00012100u
#define F_MB      0x00012200u
#define F_MB2     0x00012300u
#define F_LWORKER 0x00012400u
#define F_LA      0x00012500u
#define F_LB      0x00012600u
#define F_LB2     0x00012700u
#define F_GWORKER 0x00012800u
#define F_GA      0x00012900u
#define F_GB      0x00012A00u
#define F_RWORKER 0x00012B00u
#define F_RMAIN   0x00012C00u
#define F_RFRAME  0x00012D00u
#define F_RPRES   0x00012E00u
#define F_PWORKER 0x00012F00u
#define F_POLL    0x00013000u
#define F_SWORKER 0x00013100u
#define F_SFN     0x00013200u
#define F_ENTRY   0x00013300u       /* the main thread's creator: a lifted frame below its routine */

#define RET_MA        0x00012010u
#define RET_MB        0x00012020u
#define RET_MA_SLEEP  0x00012110u
#define RET_MB2       0x00012210u
#define RET_MB2_SLEEP 0x00012310u
#define RET_LA        0x00012410u
#define RET_LB        0x00012420u
#define RET_LA_SLEEP  0x00012510u
#define RET_LB2       0x00012610u
#define RET_LB2_SLEEP 0x00012710u
#define RET_GA        0x00012810u
#define RET_GB        0x00012820u
#define RET_GA_SLEEP  0x00012910u
#define RET_GB_SLEEP  0x00012A10u
#define RET_RMAIN     0x00012B10u
#define RET_RFRAME    0x00012C10u
#define RET_RPRES     0x00012D10u
#define RET_RP_SLEEP  0x00012E10u
#define RET_POLL      0x00012F10u
#define SITE_HALT     0x00013020u
#define RET_SFN       0x00013110u
#define RET_S_SLEEP1  0x00013210u
#define RET_S_SLEEP2  0x00013220u

static uint32_t SITES[] = {
    RET_MA, F_MWORKER, 0,
    RET_MB, F_MWORKER, 0,
    RET_MA_SLEEP, F_MA, XBOX_CALL_SITE_INDIRECT,
    RET_MB2, F_MB, 0,
    RET_MB2_SLEEP, F_MB2, XBOX_CALL_SITE_INDIRECT,
    RET_LA, F_LWORKER, 0,
    RET_LB, F_LWORKER, 0,
    RET_LA_SLEEP, F_LA, XBOX_CALL_SITE_INDIRECT,
    RET_LB2, F_LB, 0,
    RET_LB2_SLEEP, F_LB2, XBOX_CALL_SITE_INDIRECT,
    RET_GA, F_GWORKER, 0,
    RET_GB, F_GWORKER, 0,
    RET_GA_SLEEP, F_GA, XBOX_CALL_SITE_INDIRECT,
    RET_GB_SLEEP, F_GB, XBOX_CALL_SITE_INDIRECT,
    RET_RMAIN, F_RWORKER, 0,
    RET_RFRAME, F_RMAIN, 0,
    RET_RPRES, F_RFRAME, 0,
    RET_RP_SLEEP, F_RPRES, XBOX_CALL_SITE_INDIRECT | XBOX_CALL_SITE_NO_EBP,
    RET_POLL, F_PWORKER, 0,
    XBOX_RESUME_HOST_WAIT | SITE_HALT, F_POLL, XBOX_CALL_SITE_HOST_WAIT,
    RET_SFN, F_SWORKER, XBOX_CALL_SITE_SEH_ANCHOR,
    RET_S_SLEEP1, F_SFN, XBOX_CALL_SITE_INDIRECT,
    RET_S_SLEEP2, F_SFN, XBOX_CALL_SITE_INDIRECT,
};
#define NSITES (sizeof SITES / sizeof SITES[0] / 3)

#define RMAIN_EBP  0xE0000001u
#define RMAIN_EBP2 0xE0000002u

static int failures;
static uint32_t g_thunks, g_stack, g_mstack, g_handles, g_chk;
static volatile int g_stop, g_seh_stop, g_main_done;
static volatile int g_iters[K_COUNT], g_bad_regs[K_COUNT], g_bad_ebp[K_COUNT], g_jumps[K_COUNT];
static volatile int g_turns_a[K_COUNT], g_turns_b[K_COUNT];
static volatile uint32_t g_first_cnt[K_COUNT], g_last[K_COUNT];

/* Guest memory the threads keep their checks in (g_chk, one page). */
#define CNT(k)        (g_chk + 0x00u + 4u * (uint32_t)(k))
#define CHK_ESI(k)    (g_chk + 0x20u + 4u * (uint32_t)(k))
#define CHK_EDI(k)    (g_chk + 0x40u + 4u * (uint32_t)(k))
#define CHK_EBP(k, l) (g_chk + 0x80u + 0x20u * (uint32_t)(k) + 4u * (uint32_t)(l))
#define PHASE         (g_chk + 0x300u)
#define SEHSITE       (g_chk + 0x304u)

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-74s %s", what, ok ? "PASS" : "FAIL");
    if (!ok && detail) printf(" -- %s", detail);
    putchar('\n');
    if (!ok) failures++;
}

#define GPUSH(v) do { g_esp -= 4; G32(g_esp) = (uint32_t)(v); } while (0)

/* KeDelayExecutionThread(KernelMode, FALSE, &interval) through the thunk, as lifted code calls an import (see
 * tests/park_resume); `label` is the call's resume entry. */
#define SLEEP_CALL(label, ret, ms) do { \
        g_esp -= 8; \
        G32(g_esp) = (uint32_t)(-(int64_t)(ms) * 10000); G32(g_esp + 4) = 0xFFFFFFFFu; \
        { uint32_t _iv = g_esp; GPUSH(_iv); } \
        GPUSH(0); GPUSH(0); \
        { uint32_t _icall_target = G32(g_thunks); \
          GPUSH(ret); \
          if (0) { label: _icall_target = recomp_resume_target(); } \
          recomp_lookup_kernel(_icall_target)(); } \
        g_esp += 8; \
    } while (0)
#define GCALL_R(label, ret, call) do { g_ebp = ebp; GPUSH(ret); label: call; } while (0)
#define RESUME_BLOCK(fva, cases) \
    return; \
_recomp_resume: \
    { \
    uint32_t _rs_site = recomp_resume_site(RECOMP_OPAQUE32(fva)); \
    uint32_t _rs_ebp = recomp_resume_ebp(); \
    (void)_rs_ebp; \
    switch (_rs_site) { cases default: break; } \
    recomp_resume_lost((fva), _rs_site); \
    } \
    RECOMP_FRAME_LEAVE(); return;

/* After a kernel call or a host wait: the registers stored before it, the turn counted. A count that is not the one
 * this thread left is a load: the first one seen after it is noted. */
static void after_wait(int k)
{
    uint32_t c = G32(CNT(k));
    if (g_esi != G32(CHK_ESI(k)) || g_edi != G32(CHK_EDI(k)))
        g_bad_regs[k]++;
    if (c != g_last[k]) {
        g_first_cnt[k] = c;
        g_jumps[k]++;
    }
    G32(CNT(k)) = c + 1;
    g_last[k] = c + 1;
    g_iters[k]++;
}

static void before_wait(int k, uint32_t tag)
{
    g_esi = tag | (G32(CNT(k)) & 0xFFFFu);
    g_edi = ~g_esi;
    g_ebx = 0xB0000000u | (uint32_t)k;
    G32(CHK_ESI(k)) = g_esi;
    G32(CHK_EDI(k)) = g_edi;
}

/* A function with an ebp local whose only call is a sleep; `turns` counts its turns per thread. */
#define LEAF(name, k, fva, ebp0, ret, ms, tag, turns) \
static void name(void) \
{ \
    RECOMP_FRAME_ENTRY_R(); \
    uint32_t ebp = (ebp0); \
    G32(CHK_EBP(k, 3)) = ebp; \
    before_wait(k, tag); \
    g_ebp = ebp; \
    SLEEP_CALL(_rs_s, ret, ms); \
    after_wait(k); \
    turns[k]++; \
    if (ebp != G32(CHK_EBP(k, 3))) \
        g_bad_ebp[k]++; \
    g_esp += 4; \
    RECOMP_FRAME_LEAVE(); \
    RESUME_BLOCK(fva, case ret: ebp = _rs_ebp; goto _rs_s;) \
}

/* A function with an ebp local that calls one other. */
#define MID(name, k, fva, ebp0, ret, callee) \
static void name(void) \
{ \
    RECOMP_FRAME_ENTRY_R(); \
    uint32_t ebp = (ebp0); \
    G32(CHK_EBP(k, 1)) = ebp; \
    GCALL_R(_rs_c, ret, callee()); \
    if (ebp != G32(CHK_EBP(k, 1))) \
        g_bad_ebp[k]++; \
    g_esp += 4; \
    RECOMP_FRAME_LEAVE(); \
    RESUME_BLOCK(fva, case ret: ebp = _rs_ebp; goto _rs_c;) \
}

/* A thread routine that goes down one of two chains, as PHASE says, until the test stops it. */
#define PWORKER(name, k, fva, ebp0, ra, fa, rb, fb) \
static void name(void) \
{ \
    RECOMP_FRAME_ENTRY_R(); \
    uint32_t ebp = (ebp0); \
    while (!g_stop) { \
        G32(CHK_EBP(k, 0)) = ebp; \
        if (G32(PHASE) == 0) { \
            GCALL_R(_rs_a, ra, fa()); \
        } else { \
            GCALL_R(_rs_b, rb, fb()); \
        } \
        if (ebp != G32(CHK_EBP(k, 0))) \
            g_bad_ebp[k]++; \
    } \
    RECOMP_FRAME_LEAVE(); \
    RESUME_BLOCK(fva, case ra: ebp = _rs_ebp; goto _rs_a; case rb: ebp = _rs_ebp; goto _rs_b;) \
}

/* main (the INLINE main thread), loader and long */
LEAF(m_a, K_MAIN, F_MA, 0x3A000000u, RET_MA_SLEEP, 2, 0x3A000000u, g_turns_a)
LEAF(m_b2, K_MAIN, F_MB2, 0x3B200000u, RET_MB2_SLEEP, 2, 0x3B000000u, g_turns_b)
MID(m_b, K_MAIN, F_MB, 0x3B100000u, RET_MB2, m_b2)
PWORKER(m_worker, K_MAIN, F_MWORKER, 0xEB000030u, RET_MA, m_a, RET_MB, m_b)

LEAF(l_a, K_LOADER, F_LA, 0x1A000000u, RET_LA_SLEEP, 2, 0x1A000000u, g_turns_a)
LEAF(l_b2, K_LOADER, F_LB2, 0x1B200000u, RET_LB2_SLEEP, 2, 0x1B000000u, g_turns_b)
MID(l_b, K_LOADER, F_LB, 0x1B100000u, RET_LB2, l_b2)
PWORKER(l_worker, K_LOADER, F_LWORKER, 0xEB000010u, RET_LA, l_a, RET_LB, l_b)

LEAF(g_a, K_LONG, F_GA, 0x6A000000u, RET_GA_SLEEP, 120, 0x6A000000u, g_turns_a)
LEAF(g_b, K_LONG, F_GB, 0x6B000000u, RET_GB_SLEEP, 120, 0x6B000000u, g_turns_b)
PWORKER(g_worker, K_LONG, F_GWORKER, 0xEB000020u, RET_GA, g_a, RET_GB, g_b)

/* render: worker -> main -> frame -> present -> kernel (tests/park_resume's) */
static void r_present(void)
{
    RECOMP_FRAME_ENTRY_R();
    before_wait(K_RENDER, 0x5E000000u);
    SLEEP_CALL(_rs_p_sleep, RET_RP_SLEEP, 3);
    after_wait(K_RENDER);
    g_esp += 4;
    RECOMP_FRAME_LEAVE();
    RESUME_BLOCK(F_RPRES, case RET_RP_SLEEP: goto _rs_p_sleep;)
}

static void r_frame(void)
{
    RECOMP_FRAME_ENTRY_R();
    uint32_t ebp = g_ebp;
    GPUSH(ebp);                                      /* push ebp; mov ebp, esp */
    ebp = g_esp;
    G32(CHK_EBP(K_RENDER, 2)) = ebp;
    GCALL_R(_rs_present, RET_RPRES, r_present());
    if (ebp != G32(CHK_EBP(K_RENDER, 2)))
        g_bad_ebp[K_RENDER]++;
    g_esp = ebp;                                     /* leave; ret */
    ebp = G32(g_esp);
    g_esp += 4;
    g_esp += 4;
    RECOMP_FRAME_LEAVE();
    RESUME_BLOCK(F_RFRAME, case RET_RPRES: ebp = _rs_ebp; goto _rs_present;)
}

static void r_main(void)
{
    RECOMP_FRAME_ENTRY_R();
    uint32_t ebp = RMAIN_EBP;
    while (!g_stop) {
        G32(CHK_EBP(K_RENDER, 1)) = ebp;
        GCALL_R(_rs_frame, RET_RFRAME, r_frame());
        if (ebp != G32(CHK_EBP(K_RENDER, 1)))
            g_bad_ebp[K_RENDER]++;
        ebp = ebp == RMAIN_EBP ? RMAIN_EBP2 : RMAIN_EBP;
    }
    g_esp += 4;
    RECOMP_FRAME_LEAVE();
    RESUME_BLOCK(F_RMAIN, case RET_RFRAME: ebp = _rs_ebp; goto _rs_frame;)
}

static void r_worker(void)
{
    RECOMP_FRAME_ENTRY_R();
    uint32_t ebp = 0xEB000000u;
    G32(CHK_EBP(K_RENDER, 0)) = ebp;
    GCALL_R(_rs_main, RET_RMAIN, r_main());
    if (ebp != G32(CHK_EBP(K_RENDER, 0)))
        g_bad_ebp[K_RENDER]++;
    RECOMP_FRAME_LEAVE();
    RESUME_BLOCK(F_RWORKER, case RET_RMAIN: ebp = _rs_ebp; goto _rs_main;)
}

/* halt: worker -> poll -> a hlt loop's host wait */
void xbox_Halt(uint32_t va);

static void p_poll(void)
{
    RECOMP_FRAME_ENTRY_R();
    uint32_t ebp = 0x90110000u;
    for (;;) {
        before_wait(K_HALT, 0x9E000000u);
        if (g_stop)
            break;
        g_ebp = ebp;
        xbox_Halt(SITE_HALT);
        _rh_halt: ;
        after_wait(K_HALT);
        if (g_ebp != ebp)
            g_bad_ebp[K_HALT]++;
    }
    g_esp += 4;
    RECOMP_FRAME_LEAVE();
    RESUME_BLOCK(F_POLL, case XBOX_RESUME_HOST_WAIT | SITE_HALT: ebp = _rs_ebp; goto _rh_halt;)
}

static void p_worker(void)
{
    RECOMP_FRAME_ENTRY_R();
    uint32_t ebp = 0xEB000002u;
    GCALL_R(_rs_poll, RET_POLL, p_poll());
    RECOMP_FRAME_LEAVE();
    RESUME_BLOCK(F_PWORKER, case RET_POLL: ebp = _rs_ebp; goto _rs_poll;)
}

/* seh: worker -> fn (called from a function with __try), sleeping at one of two sites */
static void s_fn(void)
{
    RECOMP_FRAME_ENTRY_R();
    uint32_t ebp = 0x5EF00000u;
    while (!g_stop && !g_seh_stop) {
        before_wait(K_SEH, 0x5F000000u);
        g_ebp = ebp;
        if (G32(SEHSITE) == 0) {
            SLEEP_CALL(_rs_s1, RET_S_SLEEP1, 3);
            g_turns_a[K_SEH]++;
        } else {
            SLEEP_CALL(_rs_s2, RET_S_SLEEP2, 3);
            g_turns_b[K_SEH]++;
        }
        after_wait(K_SEH);
    }
    g_esp += 4;
    RECOMP_FRAME_LEAVE();
    RESUME_BLOCK(F_SFN, case RET_S_SLEEP1: ebp = _rs_ebp; goto _rs_s1; case RET_S_SLEEP2: ebp = _rs_ebp; goto _rs_s2;)
}

static void s_worker(void)
{
    RECOMP_FRAME_ENTRY_R();
    uint32_t ebp = 0xEB000003u;
    GCALL_R(_rs_sfn, RET_SFN, s_fn());
    RECOMP_FRAME_LEAVE();
    RESUME_BLOCK(F_SWORKER, case RET_SFN: ebp = _rs_ebp; goto _rs_sfn;)
}

recomp_func_t recomp_lookup(uint32_t va)
{
    switch (va) {
    case F_MWORKER: return m_worker;
    case F_MA: return m_a;
    case F_MB: return m_b;
    case F_MB2: return m_b2;
    case F_LWORKER: return l_worker;
    case F_LA: return l_a;
    case F_LB: return l_b;
    case F_LB2: return l_b2;
    case F_GWORKER: return g_worker;
    case F_GA: return g_a;
    case F_GB: return g_b;
    case F_RWORKER: return r_worker;
    case F_RMAIN: return r_main;
    case F_RFRAME: return r_frame;
    case F_RPRES: return r_present;
    case F_PWORKER: return p_worker;
    case F_POLL: return p_poll;
    case F_SWORKER: return s_worker;
    case F_SFN: return s_fn;
    }
    return NULL;
}
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }

static recomp_func_t thunk(uint32_t ordinal)
{
    unsigned i;
    for (i = 0; i < NORDS; i++)
        if (ORDS[i] == ordinal)
            return recomp_lookup_kernel(G32(g_thunks + 4 * i));
    return NULL;
}

/* PsCreateSystemThreadEx(handle, 0, 0, 0, 0, 0, 0, 0, 0, routine) from guest stack `stack`. */
static void create_thread(int i, uint32_t routine, uint32_t stack)
{
    uint32_t hva = g_handles + 4u * (uint32_t)i;
    uint32_t argv[10] = { hva, 0, 0, 0, 0, 0, 0, 0, 0, routine };
    unsigned k;

    G32(stack) = 0xBEEF0001u;
    for (k = 0; k < 10; k++)
        G32(stack + 4 + 4 * k) = argv[k];
    g_esp = stack;
    thunk(ORD_PSCREATE)();
}

/* The title's entry, lifted: its first PsCreateSystemThreadEx runs the main routine inline (INLINE thread mode), so
 * this frame stays below the main thread's routine for its whole life. */
static void x_entry(void)
{
    RECOMP_FRAME_ENTRY();
    create_thread(7, F_MWORKER, g_mstack);
    RECOMP_FRAME_LEAVE();
}

static void *main_host(void *arg)
{
    (void)arg;
    xbox_GuestCpuJoin();
    x_entry();
    xbox_GuestCpuPart();
    g_main_done = 1;
    return NULL;
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

/* Until every counter `c[k]` of the threads in [0, n) moved `by` from `from[k]` (ms at most). */
static int wait_turns(volatile int *c, const int *from, int n, int by, int ms)
{
    int t, k, all = 0;
    for (t = 0; t < ms && !all; t++) {
        all = 1;
        for (k = 0; k < n; k++)
            if (c[k] - from[k] < by)
                all = 0;
        if (!all)
            usleep(1000);
    }
    return all;
}

static void snap(volatile int *c, int *out)
{
    int k;
    for (k = 0; k < K_COUNT; k++)
        out[k] = c[k];
}

static int cmp_site(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static unsigned char *load_file(const char *path, size_t *n)
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

static int capture(uint8_t **b, size_t *n, char *why, size_t why_n)
{
    XboxStateStats st;
    int k, rc = -1;
    for (k = 0; k < 20; k++) {
        *b = NULL;
        rc = xbox_StateCapture(1000, b, n, &st);
        if (rc == XBOX_STATE_OK)
            return rc;
        snprintf(why, why_n, "%s: %s", xbox_StateError(rc), st.reason);
        free(*b);
        *b = NULL;
        usleep(2000);
    }
    return rc;
}

/* The counters CNT(k) in a state. */
static void state_counts(const uint8_t *b, size_t n, uint32_t *cnt)
{
    const uint8_t *rp, *page = NULL;
    size_t rl;
    int k;
    memset(cnt, 0, K_COUNT * sizeof *cnt);
    xbox_StateSection(b, n, XBOX_STATE_TAG_RAM, &rp, &rl, NULL);
    if (xbox_StateRamPage(rp, rl, g_chk & ~0xFFFu, &page) == 1 && page)
        for (k = 0; k < K_COUNT; k++)
            memcpy(&cnt[k], page + (CNT(k) - (g_chk & ~0xFFFu)), 4);
}

static void refused_case(void)
{
    uint8_t *b = NULL;
    size_t n = 0;
    char why[300], d[400];
    int jumps[K_COUNT], from[K_COUNT], k, rc, ok;
    XboxStateLoadStats ls;

    printf("  -- a thread whose state chain cannot be re-entered\n");
    G32(SEHSITE) = 0;
    snap(g_turns_a, from);
    check(wait_turns(g_turns_a + K_SEH, from + K_SEH, 1, 2, 3000), "seh sleeps at its first site", NULL);
    rc = capture(&b, &n, why, sizeof why);
    check(rc == XBOX_STATE_OK, "a state is captured with seh at its first site", why);
    if (rc != XBOX_STATE_OK)
        return;
    G32(SEHSITE) = 1;
    snap(g_turns_b, from);
    check(wait_turns(g_turns_b + K_SEH, from + K_SEH, 1, 2, 3000), "seh moved to its second site", NULL);
    snap(g_jumps, jumps);
    memset(&ls, 0, sizeof ls);
    rc = xbox_StateLoad(b, n, 500, &ls);
    snprintf(d, sizeof d, "%s: %s", xbox_StateError(rc), ls.reason);
    check(rc == XBOX_STATE_ECHAIN && strstr(ls.reason, "kernel call 99 from")
          && strstr(ls.reason, "exception handler") && ls.reentered == 0,
          "the load refuses: another chain, and the state's goes through __try", d);
    snap(g_iters, from);
    wait_turns(g_iters, from, K_PLAY, 2, 2000);
    for (k = 0, ok = 1; k < K_COUNT; k++)
        if (g_jumps[k] != jumps[k])
            ok = 0;
    check(ok, "nothing changed: no thread found its count moved", NULL);
    check(G32(SEHSITE) == 1, "guest memory is not the state's", NULL);
    free(b);
}

/* One round: capture in phase 0, phase 1 until every thread moved, load; each thread's first turn after the load
 * finds the state's count. Returns the number of threads that re-entered. */
static int round_trip(int verbose, int *reent)
{
    uint8_t *b = NULL;
    size_t n = 0;
    uint32_t cnt[K_COUNT];
    char why[300], d[400];
    int jumps[K_COUNT], from[K_COUNT], fa[K_COUNT], k, rc, ok;
    XboxStateLoadStats ls;
    uint32_t reentered0 = xbox_ParkReentered();

    G32(PHASE) = 0;
    snap(g_turns_a, from);
    if (!wait_turns(g_turns_a, from, 3, 1, 3000)) {
        check(0, "phase 0: main, loader and long go down chain a", NULL);
        return 0;
    }
    rc = capture(&b, &n, why, sizeof why);
    if (verbose || rc != XBOX_STATE_OK)
        check(rc == XBOX_STATE_OK, "a state is captured in phase 0", why);
    if (rc != XBOX_STATE_OK)
        return 0;
    state_counts(b, n, cnt);
    G32(PHASE) = 1;
    snap(g_turns_b, from);
    ok = wait_turns(g_turns_b, from, 3, 2, 3000);
    snap(g_iters, from);
    ok = ok && wait_turns(g_iters, from, K_PLAY, 2, 3000);
    if (verbose || !ok)
        check(ok, "phase 1: main, loader and long went down chain b, every thread moved on", NULL);
    snap(g_jumps, jumps);
    snap(g_turns_a, fa);
    memset(&ls, 0, sizeof ls);
    rc = xbox_StateLoad(b, n, 500, &ls);
    snprintf(d, sizeof d, "%s: %s", xbox_StateError(rc), ls.reason);
    if (verbose || rc != XBOX_STATE_OK)
        check(rc == XBOX_STATE_OK, "the state of phase 0 loads in phase 1", d);
    free(b);
    if (rc != XBOX_STATE_OK)
        return 0;
    *reent = ls.reentered;
    snprintf(d, sizeof d, "%d re-entered (%u by the park counter), %d made again", ls.reentered,
             xbox_ParkReentered() - reentered0, ls.reissued);
    if (verbose)
        check(ls.reentered >= 3 && xbox_ParkReentered() - reentered0 == (uint32_t)ls.reentered,
              "main, loader and long re-entered the state's chains (render too when its ebp differed)", d);
    else if (ls.reentered < 3)
        check(0, "main, loader and long re-entered the state's chains", d);
    if (verbose)
        printf("  %s\n", d);
    snap(g_iters, from);
    ok = wait_turns(g_jumps, jumps, K_PLAY, 1, 3000) && wait_turns(g_iters, from, K_PLAY, 2, 3000);
    if (verbose || !ok)
        check(ok, "every thread runs on after the load", NULL);
    for (k = 0; k < K_PLAY; k++) {
        snprintf(d, sizeof d, "%s: first turn found %u, %u in the state; %d load(s) seen", NAMES[k], g_first_cnt[k],
                 cnt[k], g_jumps[k] - jumps[k]);
        if (verbose || g_first_cnt[k] != cnt[k] || g_jumps[k] - jumps[k] != 1) {
            char what[96];
            snprintf(what, sizeof what, "%s: its first turn after the load goes on from the state's count", NAMES[k]);
            check(g_first_cnt[k] == cnt[k] && g_jumps[k] - jumps[k] == 1, what, d);
        }
    }
    ok = wait_turns(g_turns_a, fa, 3, 1, 3000) && G32(PHASE) == 0;
    if (verbose || !ok)
        check(ok, "main, loader and long are back down chain a (PHASE is the state's 0)", NULL);
    return 1;
}

static void reenter_cases(void)
{
    char why[300], d[400];
    int k, rc, reent = 0, total = 0, rounds = 0, bad_capture = 0;
    uint8_t *b = NULL;
    size_t n = 0;
    XboxStateLoadStats ls;

    printf("  -- threads at other chains re-enter the state's\n");
    round_trip(1, &reent);
    for (k = 0; k < 10; k++) {
        if (round_trip(0, &reent)) {
            XboxStateStats st;
            rounds++;
            total += reent;
            /* A capture right after the load: no thread still shows the chain it left. */
            b = NULL;
            rc = xbox_StateCapture(1000, &b, &n, &st);
            if (rc != XBOX_STATE_OK) {
                bad_capture++;
                snprintf(why, sizeof why, "%s: %s", xbox_StateError(rc), st.reason);
            }
            free(b);
        }
    }
    snprintf(d, sizeof d, "%d of 10 rounds, %d re-entries", rounds, total);
    check(rounds == 10, "ten more rounds of capture, phase flip and load", d);
    check(bad_capture == 0, "a capture right after each load succeeds", bad_capture ? why : NULL);

    /* Two loads back to back, the second while threads may still be re-entering. */
    G32(PHASE) = 0;
    usleep(20000);
    rc = capture(&b, &n, why, sizeof why);
    check(rc == XBOX_STATE_OK, "a state for two loads in a row", why);
    if (rc == XBOX_STATE_OK) {
        int rc2;
        G32(PHASE) = 1;
        usleep(30000);
        memset(&ls, 0, sizeof ls);
        rc = xbox_StateLoad(b, n, 500, &ls);
        G32(PHASE) = 1;                     /* send them away again at once */
        memset(&ls, 0, sizeof ls);
        rc2 = xbox_StateLoad(b, n, 500, &ls);
        snprintf(d, sizeof d, "%s / %s: %s", xbox_StateError(rc), xbox_StateError(rc2), ls.reason);
        check(rc == XBOX_STATE_OK && rc2 == XBOX_STATE_OK, "two loads back to back both load", d);
        free(b);
    }
    {
        int from[K_COUNT];
        snap(g_iters, from);
        check(wait_turns(g_iters, from, K_PLAY, 2, 3000), "every thread runs on after them", NULL);
    }
    snprintf(d, sizeof d, "registers wrong %d/%d/%d/%d/%d, ebp wrong %d/%d/%d/%d/%d", g_bad_regs[0], g_bad_regs[1],
             g_bad_regs[2], g_bad_regs[3], g_bad_regs[4], g_bad_ebp[0], g_bad_ebp[1], g_bad_ebp[2], g_bad_ebp[3],
             g_bad_ebp[4]);
    for (k = 0, rc = 1; k < K_PLAY; k++)
        if (g_bad_regs[k] || g_bad_ebp[k])
            rc = 0;
    check(rc, "the registers after each wait and every frame's ebp are the ones stored before (all rounds)", d);
}

int main(int argc, char **argv)
{
    size_t len = 0;
    unsigned char *xbe;
    unsigned i;
    pthread_t mt;
    const char *xbe_path = argc > 1 ? argv[1] : "tools/conformance/test.xbe";
    int t;

    setvbuf(stdout, NULL, _IONBF, 0);
    setenv("RECOMP_KERNEL_TRACE", "0", 0);
    xbe = load_file(xbe_path, &len);
    if (!xbe || !xbox_MemoryLayoutInit(xbe, len)) {
        printf("cannot map guest memory (run tools/conformance/mkxbe.py)\n");
        return 2;
    }
    g_thunks  = xbox_HeapAlloc(0x1000, 4096);
    g_stack   = xbox_HeapAlloc(0x1000, 4096);
    g_mstack  = xbox_HeapAlloc(0x10000, 4096) + 0xF000u;
    g_handles = xbox_HeapAlloc(0x100, 4096);
    g_chk     = xbox_HeapAlloc(0x1000, 4096);
    for (i = 0; i < 0x1000; i += 4)
        G32(g_chk + i) = 0;
    for (i = 0; i < NORDS; i++)
        G32(g_thunks + 4 * i) = 0x80000000u | ORDS[i];
    G32(g_thunks + 4 * NORDS) = 0;
    xbox_kernel_set_thunk_address(g_thunks, NORDS + 1);
    xbox_kernel_bridge_init();
    xbox_SetThreadMode(XBOX_THREAD_MODE_INLINE);
    xbox_GuestCpuPart();
    qsort(SITES, NSITES, 3 * sizeof SITES[0], cmp_site);
    recomp_call_sites_register(SITES, NSITES);

    /* The main thread first (the first PsCreateSystemThreadEx runs inline), then the workers (spawned). */
    if (pthread_create(&mt, NULL, main_host, NULL) != 0) {
        printf("no host thread\n");
        return 2;
    }
    check(wait_threads(1, 3000), "the main thread starts inline under its creator's frame", NULL);
    create_thread(0, F_LWORKER, g_stack);
    create_thread(1, F_GWORKER, g_stack);
    create_thread(2, F_RWORKER, g_stack);
    create_thread(3, F_PWORKER, g_stack);
    create_thread(4, F_SWORKER, g_stack);
    check(wait_threads(6, 3000), "five workers start", NULL);
    usleep(50000);

    refused_case();
    g_seh_stop = 1;
    check(wait_threads(5, 3000), "the seh thread ends", NULL);

    reenter_cases();

    g_stop = 1;
    check(wait_threads(0, 5000), "every thread ends", NULL);
    for (t = 0; t < 5000 && !g_main_done; t++)
        usleep(1000);
    check(g_main_done, "the main thread returned through its creator's frame", NULL);
    if (g_main_done)
        pthread_join(mt, NULL);
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
