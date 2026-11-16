/*
 * Xbox save states, part 6 (XBA-250): a state saved in one process resumes in another.
 *
 * The guest workers are host routines written the way tools/recomp lifts code with resume entries: each
 * function that may park enters through RECOMP_FRAME_ENTRY_R (xbox_memory_layout.h, the same macro recomp_types.h
 * gives the generated code), keeps ebp in a C local and publishes it before a call, puts a label before every
 * call that may park (an indirect call takes its target from recomp_resume_target there) and after every host
 * wait, and ends with the resume block the translator emits (_resume_block). Nothing that lives across a call is
 * in a C local but ebp: everything else is in registers (g_cpu) and guest memory, as in lifted code. The test
 * registers the call-site table the lifter would emit for them.
 *
 *   render   worker -> main -> frame -> present -> KeDelayExecutionThread: a chain four calls deep; frame has a
 *            frame pointer, main a scratch ebp taking one of two values every turn
 *   audio    worker -> [table] mix -> KeDelayExecutionThread: the worker calls mix through a pointer in guest
 *            memory (an indirect call re-made with the target the runtime hands over)
 *   halt     worker -> halt -> a hlt loop's host wait (xbox_Halt, XBA-251: the guest CPU let go for 1 ms)
 *   long     worker -> wait -> KeDelayExecutionThread(250 ms): captured inside its call (not returned), the call is
 *            made again in the second process
 *   seh      worker -> fn, a call site in a function with an exception handler (flagged SEH_ANCHOR): a state
 *            taken while it lives says it cannot be loaded in another run, naming why
 *
 * Every function stores the registers and the ebp it holds in guest memory before its call and compares them
 * after it, and the innermost ones count their turns in guest memory.
 *
 *   Save     (first process) the seh thread's state is refused for another run with its reason; without it, a
 *            state is captured, its verdict says it resumes, and it is written as a slot file. The process then
 *            runs itself again (fork + exec) with "resume <slot>" and takes its exit code.
 *   Resume   (second process: no guest thread, nothing of the first run but the slot file) xbox_StateResume makes
 *            the three threads again; each re-enters its chain and its kernel call or host wait, and goes on: the
 *            first turn after the resume finds its counter where the state had it, the registers and every
 *            frame's ebp are the ones stored before the call, the threads keep running; a second resume is
 *            refused; a state captured in the resumed process loads in that process (same run)
 */
#include "kernel.h"
#include "kernel_park.h"
#include "xbox_memory_layout.h"
#include "xbox_resume.h"
#include "xbox_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
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

enum { K_RENDER, K_AUDIO, K_POLL, K_LONG, K_SEH, K_COUNT };
#define K_RESUMED 4                 /* render, audio, halt, long */

/* The guest functions (their start VAs: routines are dispatched by them) and their call sites (return VAs). */
#define F_RWORKER 0x00012000u
#define F_MAIN    0x00012100u
#define F_FRAME   0x00012200u
#define F_PRESENT 0x00012300u
#define F_AWORKER 0x00012400u
#define F_MIX     0x00012500u
#define F_PWORKER 0x00012600u
#define F_POLL    0x00012700u
#define F_LWORKER 0x00012A00u
#define F_LWAIT   0x00012B00u
#define F_SWORKER 0x00012800u
#define F_SFN     0x00012900u
#define RET_MAIN      0x00012010u   /* rworker: call main */
#define RET_FRAME     0x00012110u   /* main: call frame */
#define RET_PRESENT   0x00012210u   /* frame: call present */
#define RET_P_SLEEP   0x00012310u   /* present: call [KeDelayExecutionThread] */
#define RET_MIX       0x00012410u   /* aworker: call [table] (mix) */
#define RET_MIX_SLEEP 0x00012510u   /* mix: call [KeDelay...] */
#define RET_POLL      0x00012610u   /* pworker: call poll */
#define SITE_HALT     0x00012720u   /* halt: the hlt */
#define RET_LWAIT     0x00012A10u   /* lworker: call wait */
#define RET_L_SLEEP   0x00012B10u   /* wait: call [KeDelay...] */
#define RET_SFN       0x00012810u   /* sworker: call fn (in a function with __try) */
#define RET_S_SLEEP   0x00012910u   /* fn: call [KeDelay...] */

static uint32_t SITES[] = {
    RET_MAIN, F_RWORKER, 0,
    RET_FRAME, F_MAIN, 0,
    RET_PRESENT, F_FRAME, 0,
    RET_P_SLEEP, F_PRESENT, XBOX_CALL_SITE_INDIRECT | XBOX_CALL_SITE_NO_EBP,
    RET_MIX, F_AWORKER, XBOX_CALL_SITE_INDIRECT,
    RET_MIX_SLEEP, F_MIX, XBOX_CALL_SITE_INDIRECT,
    RET_POLL, F_PWORKER, 0,
    XBOX_RESUME_HOST_WAIT | SITE_HALT, F_POLL, XBOX_CALL_SITE_HOST_WAIT,
    RET_LWAIT, F_LWORKER, 0,
    RET_L_SLEEP, F_LWAIT, XBOX_CALL_SITE_INDIRECT,
    RET_SFN, F_SWORKER, XBOX_CALL_SITE_SEH_ANCHOR,
    RET_S_SLEEP, F_SFN, XBOX_CALL_SITE_INDIRECT,
};
#define NSITES (sizeof SITES / sizeof SITES[0] / 3)

#define MAIN_EBP  0xE0000001u
#define MAIN_EBP2 0xE0000002u

static int failures;
static uint32_t g_thunks, g_stack, g_handles, g_chk;
static volatile int g_stop, g_seh_stop, g_resume_mode;
static volatile int g_iters[K_COUNT], g_bad_regs[K_COUNT], g_bad_ebp[K_COUNT], g_first_seen[K_COUNT];
static volatile uint32_t g_first_cnt[K_COUNT];

/* Guest memory the threads keep their checks in (g_chk, one page). */
#define CNT(k)       (g_chk + 0x00u + 4u * (uint32_t)(k))
#define CHK_ESI(k)   (g_chk + 0x20u + 4u * (uint32_t)(k))
#define CHK_EDI(k)   (g_chk + 0x40u + 4u * (uint32_t)(k))
#define CHK_EBP(k, l) (g_chk + 0x80u + 0x20u * (uint32_t)(k) + 4u * (uint32_t)(l))
#define FTAB         (g_chk + 0x204u)

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-74s %s", what, ok ? "PASS" : "FAIL");
    if (!ok && detail) printf(" -- %s", detail);
    putchar('\n');
    if (!ok) failures++;
}

#define GPUSH(v) do { g_esp -= 4; G32(g_esp) = (uint32_t)(v); } while (0)

/* KeDelayExecutionThread(KernelMode, FALSE, &interval) through the thunk, as lifted code calls an import: the
 * interval below the arguments, every esp change relative (stdcall: the call pops its arguments), the target read
 * before the return address is pushed; `label` is the call's resume entry. */
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
/* A direct guest call from a function with an ebp local, with its resume entry. */
#define GCALL_R(label, ret, call) do { g_ebp = ebp; GPUSH(ret); label: call; } while (0)
/* The resume block (tools/recomp/translator.py _resume_block). */
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

/* After a kernel call or a host wait: the registers stored before it, the turn counted. The first turn after a
 * resume notes the counter: the one in the state if the thread went on from where it was. */
static void after_wait(int k)
{
    if (g_esi != G32(CHK_ESI(k)) || g_edi != G32(CHK_EDI(k)))
        g_bad_regs[k]++;
    if (g_resume_mode && !g_first_seen[k]) {
        g_first_cnt[k] = G32(CNT(k));
        g_first_seen[k] = 1;
    }
    G32(CNT(k)) = G32(CNT(k)) + 1;
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

/* ---- render: worker -> main -> frame -> present -> kernel ---- */

static void r_present(void)
{
    RECOMP_FRAME_ENTRY_R();
    before_wait(K_RENDER, 0x5E000000u);
    SLEEP_CALL(_rs_p_sleep, RET_P_SLEEP, 3);
    after_wait(K_RENDER);
    g_esp += 4;                                      /* ret */
    RECOMP_FRAME_LEAVE();
    RESUME_BLOCK(F_PRESENT, case RET_P_SLEEP: goto _rs_p_sleep;)
}

static void r_frame(void)
{
    RECOMP_FRAME_ENTRY_R();
    uint32_t ebp = g_ebp;
    GPUSH(ebp);                                      /* push ebp; mov ebp, esp */
    ebp = g_esp;
    G32(CHK_EBP(K_RENDER, 2)) = ebp;
    GCALL_R(_rs_present, RET_PRESENT, r_present());
    if (ebp != G32(CHK_EBP(K_RENDER, 2)))
        g_bad_ebp[K_RENDER]++;
    g_esp = ebp;                                     /* leave; ret */
    ebp = G32(g_esp);
    g_esp += 4;
    g_esp += 4;
    RECOMP_FRAME_LEAVE();
    RESUME_BLOCK(F_FRAME, case RET_PRESENT: ebp = _rs_ebp; goto _rs_present;)
}

static void r_main(void)
{
    RECOMP_FRAME_ENTRY_R();
    uint32_t ebp = MAIN_EBP;
    while (!g_stop) {
        G32(CHK_EBP(K_RENDER, 1)) = ebp;
        GCALL_R(_rs_frame, RET_FRAME, r_frame());
        if (ebp != G32(CHK_EBP(K_RENDER, 1)))
            g_bad_ebp[K_RENDER]++;
        ebp = ebp == MAIN_EBP ? MAIN_EBP2 : MAIN_EBP;
    }
    g_esp += 4;
    RECOMP_FRAME_LEAVE();
    RESUME_BLOCK(F_MAIN, case RET_FRAME: ebp = _rs_ebp; goto _rs_frame;)
}

static void r_worker(void)
{
    RECOMP_FRAME_ENTRY_R();
    uint32_t ebp = 0xEB000000u;
    G32(CHK_EBP(K_RENDER, 0)) = ebp;
    GCALL_R(_rs_main, RET_MAIN, r_main());
    if (ebp != G32(CHK_EBP(K_RENDER, 0)))
        g_bad_ebp[K_RENDER]++;
    RECOMP_FRAME_LEAVE();
    RESUME_BLOCK(F_RWORKER, case RET_MAIN: ebp = _rs_ebp; goto _rs_main;)
}

/* ---- audio: worker -> [table] mix -> kernel ---- */

static void a_mix(void)
{
    RECOMP_FRAME_ENTRY_R();
    uint32_t ebp = 0xA0D10000u;
    G32(CHK_EBP(K_AUDIO, 1)) = ebp;
    before_wait(K_AUDIO, 0xA0000000u);
    g_ebp = ebp;                                     /* published before the call */
    SLEEP_CALL(_rs_m_sleep, RET_MIX_SLEEP, 2);
    after_wait(K_AUDIO);
    if (ebp != G32(CHK_EBP(K_AUDIO, 1)))
        g_bad_ebp[K_AUDIO]++;
    g_esp += 4;
    RECOMP_FRAME_LEAVE();
    RESUME_BLOCK(F_MIX, case RET_MIX_SLEEP: ebp = _rs_ebp; goto _rs_m_sleep;)
}

static void a_worker(void)
{
    RECOMP_FRAME_ENTRY_R();
    uint32_t ebp = 0xEB000001u;
    while (!g_stop) {
        G32(CHK_EBP(K_AUDIO, 0)) = ebp;
        {
            uint32_t _icall_target = G32(FTAB);
            g_ebp = ebp;
            GPUSH(RET_MIX);
            if (0) { _rs_mix: _icall_target = recomp_resume_target(); }
            recomp_lookup(_icall_target)();
        }
        if (ebp != G32(CHK_EBP(K_AUDIO, 0)))
            g_bad_ebp[K_AUDIO]++;
        ebp ^= 0x10u;
    }
    RECOMP_FRAME_LEAVE();
    RESUME_BLOCK(F_AWORKER, case RET_MIX: ebp = _rs_ebp; goto _rs_mix;)
}

/* ---- halt: worker -> halt -> a hlt loop's host wait ---- */

void xbox_Halt(uint32_t va);

static void p_poll(void)
{
    RECOMP_FRAME_ENTRY_R();
    uint32_t ebp = 0x90110000u;
    for (;;) {
        before_wait(K_POLL, 0x9E000000u);
        if (g_stop)
            break;
        g_ebp = ebp;                                 /* published before the hlt, as lifted code does */
        xbox_Halt(SITE_HALT);
        _rh_halt: ;
        after_wait(K_POLL);
        if (g_ebp != ebp)
            g_bad_ebp[K_POLL]++;
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

/* ---- long: worker -> wait -> a long kernel wait ---- */

static void l_wait(void)
{
    RECOMP_FRAME_ENTRY_R();
    uint32_t ebp = 0x10A60000u;
    while (!g_stop) {
        before_wait(K_LONG, 0x1A000000u);
        g_ebp = ebp;
        SLEEP_CALL(_rs_l_sleep, RET_L_SLEEP, 250);
        after_wait(K_LONG);
        if (g_ebp != ebp)
            g_bad_ebp[K_LONG]++;
    }
    g_esp += 4;
    RECOMP_FRAME_LEAVE();
    RESUME_BLOCK(F_LWAIT, case RET_L_SLEEP: ebp = _rs_ebp; goto _rs_l_sleep;)
}

static void l_worker(void)
{
    RECOMP_FRAME_ENTRY_R();
    uint32_t ebp = 0xEB000004u;
    GCALL_R(_rs_lwait, RET_LWAIT, l_wait());
    RECOMP_FRAME_LEAVE();
    RESUME_BLOCK(F_LWORKER, case RET_LWAIT: ebp = _rs_ebp; goto _rs_lwait;)
}

/* ---- seh: worker -> fn, called from a function with an exception handler ---- */

static void s_fn(void)
{
    RECOMP_FRAME_ENTRY_R();
    while (!g_stop && !g_seh_stop)
        SLEEP_CALL(_rs_s_sleep, RET_S_SLEEP, 4);
    g_esp += 4;
    RECOMP_FRAME_LEAVE();
    RESUME_BLOCK(F_SFN, case RET_S_SLEEP: goto _rs_s_sleep;)
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
    case F_RWORKER: return r_worker;
    case F_MAIN: return r_main;
    case F_FRAME: return r_frame;
    case F_PRESENT: return r_present;
    case F_AWORKER: return a_worker;
    case F_MIX: return a_mix;
    case F_PWORKER: return p_worker;
    case F_POLL: return p_poll;
    case F_LWORKER: return l_worker;
    case F_LWAIT: return l_wait;
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

static void spawn(int i, uint32_t routine)
{
    uint32_t hva = g_handles + 4u * (uint32_t)i;
    uint32_t argv[10] = { hva, 0, 0, 0, 0, 0, 0, 0, 0, routine };
    unsigned k;

    G32(g_stack) = 0xBEEF0001u;
    for (k = 0; k < 10; k++)
        G32(g_stack + 4 + 4 * k) = argv[k];
    g_esp = g_stack;
    thunk(ORD_PSCREATE)();
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

static int cmp_site(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
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

/* ---- the first process ---- */

static int save_and_run_again(const char *self, const char *xbe)
{
    char dir[] = "/tmp/park_resume_XXXXXX", path[128], why[256], d[300];
    uint8_t *b = NULL;
    size_t n = 0;
    XboxStateStats st;
    pid_t pid;
    int rc, status = 0, k;

    spawn(0, F_RWORKER);
    spawn(1, F_AWORKER);
    spawn(2, F_PWORKER);
    spawn(3, F_LWORKER);
    spawn(4, F_SWORKER);
    check(wait_threads(5, 3000), "five workers start", NULL);
    usleep(80000);

    /* With the seh thread: captured, but its verdict refuses another run. */
    rc = xbox_StateCapture(1000, &b, &n, &st);
    check(rc == XBOX_STATE_OK, "a state is captured with the seh thread", st.reason);
    if (rc == XBOX_STATE_OK) {
        int ok = xbox_StateResumable(b, n, why, sizeof why);
        snprintf(d, sizeof d, "%s", why);
        check(!ok && strstr(why, "exception handler"), "its verdict: not resumable, through a function with __try",
              d);
    }
    free(b);
    b = NULL;
    g_seh_stop = 1;
    check(wait_threads(4, 3000), "the seh thread ends", NULL);
    usleep(30000);

    for (k = 0; k < 10; k++) {
        rc = xbox_StateCapture(1000, &b, &n, &st);
        if (rc == XBOX_STATE_OK)
            break;
        free(b);
        b = NULL;
    }
    check(rc == XBOX_STATE_OK, "render, audio, halt and long: a state is captured", st.reason);
    if (rc != XBOX_STATE_OK)
        return 0;
    rc = xbox_StateResumable(b, n, why, sizeof why);
    check(rc == 1, "its verdict: every chain can be re-entered in another run", why);
    if (!mkdtemp(dir)) {
        check(0, "a temporary folder", NULL);
        free(b);
        return 0;
    }
    snprintf(path, sizeof path, "%s/auto.st", dir);
    check(xbox_StateWriteSlot(path, b, n, NULL, 0, 0) == XBOX_STATE_OK, "written as a slot file", path);
    free(b);

    /* The process runs itself again: nothing of this one but the slot file. */
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        execl(self, self, xbe, "resume", path, (char *)NULL);
        _exit(127);
    }
    check(pid > 0 && waitpid(pid, &status, 0) == pid, "a second process runs", NULL);
    snprintf(d, sizeof d, "exit status %d", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the second process resumed the state (its checks passed)",
          d);
    unlink(path);
    rmdir(dir);
    return 1;
}

/* ---- the second process ---- */

static void resume_here(const char *path)
{
    char d[400], why[256];
    uint8_t *b = NULL;
    size_t n = 0;
    XboxStateInfo info;
    XboxStateLoadStats ls;
    const uint8_t *rp, *page = NULL;
    size_t rl;
    uint32_t saved_cnt[K_COUNT];
    int rc, k, t, all;

    printf("  -- second process (pid %d): resume %s\n", (int)getpid(), path);
    g_resume_mode = 1;
    rc = xbox_StateReadSlot(path, &b, &n, &info);
    check(rc == XBOX_STATE_OK, "the slot file reads back in another process (same build)", info.reason);
    if (rc != XBOX_STATE_OK)
        return;
    xbox_StateSection(b, n, XBOX_STATE_TAG_RAM, &rp, &rl, NULL);
    memset(saved_cnt, 0, sizeof saved_cnt);
    if (xbox_StateRamPage(rp, rl, g_chk & ~0xFFFu, &page) == 1 && page)
        for (k = 0; k < K_COUNT; k++)
            memcpy(&saved_cnt[k], page + (CNT(k) - (g_chk & ~0xFFFu)), 4);
    printf("  turns in the state: render %u, audio %u, halt %u, long %u\n", saved_cnt[K_RENDER], saved_cnt[K_AUDIO],
           saved_cnt[K_POLL], saved_cnt[K_LONG]);
    check(xbox_StateResumable(b, n, why, sizeof why) == 1, "the state resumes here", why);

    memset(&ls, 0, sizeof ls);
    rc = xbox_StateResume(b, n, &ls);
    snprintf(d, sizeof d, "%s: %s", xbox_StateError(rc), ls.reason);
    check(rc == XBOX_STATE_OK && ls.threads == K_RESUMED, "xbox_StateResume makes the four threads again", d);
    snprintf(d, sizeof d, "%d made again", ls.reissued);
    check(rc != XBOX_STATE_OK || ls.reissued >= 1, "the long wait, captured inside its call, is made again", d);
    if (rc != XBOX_STATE_OK) {
        free(b);
        return;
    }
    for (t = 0, all = 0; t < 5000 && !all; t++) {
        all = 1;
        for (k = 0; k < K_RESUMED; k++)
            if (g_iters[k] < (k == K_LONG ? 2 : 4))
                all = 0;
        if (!all)
            usleep(1000);
    }
    snprintf(d, sizeof d, "turns since: render %d, audio %d, halt %d, long %d", g_iters[K_RENDER], g_iters[K_AUDIO],
             g_iters[K_POLL], g_iters[K_LONG]);
    check(all, "every thread re-entered its chain and runs on", d);
    check(xbox_ResumeCompleted() == K_RESUMED && xbox_StateResumed(), "four chains re-entered to their innermost "
          "frame", NULL);
    for (k = 0; k < K_RESUMED; k++) {
        snprintf(d, sizeof d, "thread %d: first turn found %u, %u in the state", k, g_first_cnt[k], saved_cnt[k]);
        check(g_first_seen[k] && g_first_cnt[k] == saved_cnt[k], k == K_RENDER
              ? "render: its first turn goes on from the state's count (the same point)"
              : k == K_AUDIO ? "audio: the same" : k == K_POLL ? "halt: the same" : "long: the same", d);
    }
    snprintf(d, sizeof d, "registers wrong %d / %d / %d / %d, ebp wrong %d / %d / %d / %d", g_bad_regs[0],
             g_bad_regs[1], g_bad_regs[2], g_bad_regs[3], g_bad_ebp[0], g_bad_ebp[1], g_bad_ebp[2], g_bad_ebp[3]);
    check(!g_bad_regs[0] && !g_bad_regs[1] && !g_bad_regs[2] && !g_bad_regs[3],
          "the registers after each wait are the ones stored before it (the record's)", d);
    check(!g_bad_ebp[0] && !g_bad_ebp[1] && !g_bad_ebp[2] && !g_bad_ebp[3],
          "every frame's ebp is the one it held at its call (four frames deep included)", d);

    memset(&ls, 0, sizeof ls);
    rc = xbox_StateResume(b, n, &ls);
    check(rc == XBOX_STATE_ECHAIN && strstr(ls.reason, "already running"), "a second resume is refused: the game"
          " runs", ls.reason);
    free(b);

    /* A state taken in the resumed process loads in it (same run). */
    /* (main's ebp flips every turn: a load is refused when render has moved on; tests/park_chain covers that) */
    for (k = 0, rc = -1; k < 60 && rc != XBOX_STATE_OK; k++) {
        XboxStateStats st;
        b = NULL;
        if (xbox_StateCapture(1000, &b, &n, &st) == XBOX_STATE_OK) {
            usleep(1000u * (unsigned)(k % 7));
            memset(&ls, 0, sizeof ls);
            rc = xbox_StateLoad(b, n, 500, &ls);
            snprintf(d, sizeof d, "%s: %s", xbox_StateError(rc), ls.reason);
        }
        free(b);
    }
    check(rc == XBOX_STATE_OK, "a state captured after the resume loads in the same run", d);

    g_stop = 1;
    check(wait_threads(0, 5000), "the resumed threads end", NULL);
}

int main(int argc, char **argv)
{
    size_t len = 0;
    unsigned char *xbe;
    unsigned i;
    const char *xbe_path = argc > 1 ? argv[1] : "tools/conformance/test.xbe";
    int resume = argc > 3 && !strcmp(argv[2], "resume");

    setvbuf(stdout, NULL, _IONBF, 0);
    setenv("RECOMP_KERNEL_TRACE", "0", 0);
    xbe = load(xbe_path, &len);
    if (!xbe || !xbox_MemoryLayoutInit(xbe, len)) {
        printf("cannot map guest memory (run tools/conformance/mkxbe.py)\n");
        return 2;
    }
    g_thunks  = xbox_HeapAlloc(0x1000, 4096);
    g_stack   = xbox_HeapAlloc(0x1000, 4096);
    g_handles = xbox_HeapAlloc(0x100, 4096);
    g_chk     = xbox_HeapAlloc(0x1000, 4096);
    for (i = 0; i < NORDS; i++)
        G32(g_thunks + 4 * i) = 0x80000000u | ORDS[i];
    G32(g_thunks + 4 * NORDS) = 0;
    G32(FTAB) = F_MIX;
    xbox_kernel_set_thunk_address(g_thunks, NORDS + 1);
    xbox_kernel_bridge_init();
    xbox_SetThreadMode(XBOX_THREAD_MODE_SPAWN);
    xbox_GuestCpuPart();
    qsort(SITES, NSITES, 3 * sizeof SITES[0], cmp_site);
    recomp_call_sites_register(SITES, NSITES);

    if (resume) {
        resume_here(argv[3]);
    } else {
        save_and_run_again("/proc/self/exe", xbe_path);
        g_stop = 1;
        check(wait_threads(0, 5000), "the first process's workers end", NULL);
    }
    printf("\n%s: %d failure(s)\n", resume ? "second process" : "first process", failures);
    return failures ? 1 : 0;
}
