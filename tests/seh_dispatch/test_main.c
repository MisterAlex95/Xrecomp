/*
 * Guest exception dispatch, unwind and resume (XBA-112), end to end on lifted
 * code: fixture.s is our own x86 with MSVC-shaped EH3 and C++ frames and our
 * own minimal frame handlers; fixture_gen.c is what tools/recomp makes of it
 * (tools/recomp/seh_fixture.py). This file is the kernel's side -- RAISE and
 * UNWIND bridge into src/kernel/xbox_seh.c as RtlRaiseException and
 * RtlUnwind do -- and the scenarios:
 *
 *  - __except resumes across frames, and the __finally of the frame in
 *    between runs during the unwind;
 *  - a filter that continues execution: the raise returns, and the normal
 *    path calls the __finally directly (a funclet entry);
 *  - a filter that searches on: unhandled, the raise returns;
 *  - a resume whose anchor is on another host stack is refused, reported,
 *    and the raise returns instead of jumping;
 *  - a frame on another guest stack is not followed;
 *  - C++: a throw across a frame with a destructor (unwind funclet) into a
 *    catch-all whose continuation is resumed.
 *
 * And where XBA-112 meets the other runtime pieces:
 *
 *  - stack switching (XBA-101): an exception raised and handled on a guest
 *    stack the runtime switched to (its own host stack), and one raised on a
 *    fiber whose handler frame is on the stack that switched to it: refused
 *    and reported, never jumped to, whether the fiber's top is known or not;
 *  - a lifted div by zero (XBA-110) inside an __except frame resumes in the
 *    handler, or carries on when the filter continues execution;
 *  - a guest longjmp out of exception frames unwinds them first (__finally
 *    runs, fs:[0] is the setjmp-time head again), as the CRT's longjmp does.
 *
 * XBA-132:
 *
 *  - host faults on guest addresses: a 32-bit load or store of lifted code
 *    on a PROT_NONE guest page under __except becomes
 *    STATUS_ACCESS_VIOLATION with { read/write, address } and the registers
 *    of the fault; resumed at the __except block, or (a guard page the filter
 *    commits) continued so the access runs again and succeeds; nothing
 *    handles it: the previous SIGSEGV handler gets the original fault, and
 *    with the default action the process dies of SIGSEGV;
 *    RECOMP_SEH_FAULTS=0 (the faults-off run) sends it down the chain at
 *    once;
 *  - a guest longjmp also unwinds the try levels of the setjmp caller's own
 *    frame: through the jmp_buf's UnwindFunc ('VC20' buffer) or the native
 *    _local_unwind2 (an old buffer);
 *  - a worker stack slice gives the thread exact guest stack bounds while it
 *    runs there: a raise on the slice resumes.
 *
 * Each checks the result, the guest stack pointer, the chain head and the
 * dispatcher's counters. Runs the same on x86-64 and arm64 (qemu in CI).
 */
#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/mman.h>
#include <sys/wait.h>
#endif

#include "xbox_seh.h"
#include "recomp_stack_switch.h"
#include <setjmp.h>

/* The guest state, as the generated code and the runtime both see it. */
#include "xbox_memory_layout.h"

extern ptrdiff_t g_xbox_mem_offset;

typedef void (*recomp_func_t)(void);
extern const uint32_t seh_fixture_text_va, seh_fixture_text_size;
extern const unsigned char seh_fixture_text[];
extern const uint32_t seh_fixture_rdata_va, seh_fixture_rdata_size;
extern const unsigned char seh_fixture_rdata[];
extern const uint32_t seh_fixture_f_outer;
extern const uint32_t seh_fixture_my_lj_unwind;
void f_outer(void);
void f_cxx(void);
void f_div(void);
void f_probe(void);
int  xbox_PosixInstallMmioFaultHandler(void);
int  xbox_worker_stack_alloc(void);
void xbox_worker_stack_free(int slot);
jmp_buf *recomp_setjmp_slot(uint32_t buf_va);
int recomp_guest_longjmp(uint32_t buf_va, uint32_t value);

#define GUEST_SIZE   0x01000000u    /* the worker stack slices too (0x00780000+) */
#define TIB_VA       0x00004000u
#define STACK_LO     0x00100000u
#define STACK_HI     0x00180000u
#define COUNTER      0x00030000u
#define FILTER       0x00030004u
#define SEEN_CODE    0x00030008u
#define DIVISOR      0x0003000Cu
#define SLOT         0x00030010u    /* the guest stack switch's esp slot */
#define JMPBUF       0x00030100u    /* a guest jmp_buf (MSVC layout) */
#define FIBER_HI_TOP 0x001C0000u    /* a fiber stack above the main one */
#define FIBER_LO_TOP 0x000F0000u    /* one below it, within the dispatcher's
                                     * window when its bounds are unknown */
/* The harness's guest functions (seh_fixture_lookup_harness). */
#define VA_SW          0x0001F100u  /* xchg esp, [SLOT]; ret */
#define VA_FIBER_EXIT  0x0001F110u  /* where a fiber's body returns */
#define VA_FIBER_RAISE 0x0001F120u  /* a fiber that raises the pending record */
#define VA_FIBER_RET   0x0001F130u  /* return addresses only */
#define VA_MAIN_RET    0x0001F140u
#define VA_STUB_RET    0x0001F150u
#define END_OF_CHAIN 0xFFFFFFFFu
/* XBA-132 (fixture.s) */
#define PROBE        0x00040000u    /* 256 KB-aligned: a page on any host */
#define PROBE_MODE   0x00030020u
#define SEEN_INFO0   0x00030024u
#define SEEN_INFO1   0x00030028u
#define SEEN_ESI     0x0003002Cu
#define SEEN_ESP     0x00030030u
#define SEEN_EBP     0x00030034u
#define VC20         0x56433230u

#define G32(va) (*(volatile uint32_t *)((uintptr_t)(va) + g_xbox_mem_offset))

static int failures;
static int flip_host_stack;     /* RAISE runs under another host stack token */
static int other_guest_stack;   /* RAISE runs with another guest stack's bounds */
static int raise_on_fiber;      /* RAISE switches to a fiber that raises (1: its
                                 * top unknown, 2: known) */
static int longjmp_from_raise;  /* RAISE longjmps to JMPBUF instead (2: the
                                 * buffer names the current head as its
                                 * registration, as a setjmp inside f_inner's
                                 * frame would) */
static uint8_t *g_mem;
static size_t g_page = 4096;

static void check(int ok, const char *what)
{
    printf("  %-66s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok)
        failures++;
}

/* The game project provides these (templates/new-game/src/recomp_manual.c). */
void recomp_icall_fail_log(uint32_t va);
void recomp_icall_not_code_log(uint32_t va);
void recomp_icall_fail_log(uint32_t va)
{
    printf("  [icall] unresolved 0x%08X\n", va);
}
void recomp_icall_not_code_log(uint32_t va)
{
    printf("  [icall] not code 0x%08X\n", va);
}

/* RtlRaiseException(record), stdcall: what bridge_raise_record does. */
void sub_0001F000(void)
{
    uint32_t ret = G32(g_esp), rec = G32(g_esp + 4);

    uint32_t lo, hi;
    void raise_from_fiber(uint32_t rec, uint32_t ret);

    if (longjmp_from_raise == 2) {
        uint32_t head = G32(TIB_VA);
        G32(JMPBUF + 0x00) = head + 0x10;       /* that frame's ebp */
        G32(JMPBUF + 0x18) = head;              /* Registration */
    }
    if (longjmp_from_raise)
        recomp_guest_longjmp(JMPBUF, 1);    /* does not return */
    if (raise_on_fiber) {
        raise_from_fiber(rec, ret);
    } else {
        xbox_SehGetGuestStack(&lo, &hi);
        if (flip_host_stack)
            xbox_SehSetHostStack(7);
        if (other_guest_stack)
            xbox_SehSetGuestStack(0x00001000u, 0x00002000u);
        xbox_SehRaiseRecord(rec, ret, g_esp + 8);
        xbox_SehSetHostStack(0);
        xbox_SehSetGuestStack(lo, hi);
    }
    g_esp += 8;
    g_eax = 0;
}

/* RtlUnwind(frame, ip, record, value), stdcall: what bridge_RtlUnwind does. */
void sub_0001F010(void)
{
    uint32_t ret = G32(g_esp), frame = G32(g_esp + 4);
    uint32_t rec = G32(g_esp + 12), value = G32(g_esp + 16);

    xbox_SehUnwind(frame, rec, ret);
    if (frame && frame != END_OF_CHAIN)
        G32(TIB_VA) = frame;
    g_esp += 20;
    g_eax = value;
}

/* ---- host faults (XBA-132) ---------------------------------------------- */

#ifndef _WIN32
static void probe_protect(int prot)
{
    if (mprotect(g_mem + PROBE, g_page, prot) != 0) {
        perror("mprotect");
        exit(2);
    }
}

/* COMMIT, stdcall, no arguments: the guard page's filter commits it. */
void sub_0001F020(void)
{
    probe_protect(PROT_READ | PROT_WRITE);
    G32(PROBE) = 0x41;
    g_esp += 4;
}

/* The handler that was there before the toolkit's (debuggerd's, ART's). */
static sigjmp_buf s_prev_jump;
static volatile int s_prev_calls;
static void *volatile s_prev_addr;

static void previous_handler(int sig, siginfo_t *si, void *uc)
{
    (void)sig; (void)uc;
    s_prev_calls++;
    s_prev_addr = si->si_addr;
    siglongjmp(s_prev_jump, 1);
}

static void install_previous(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = previous_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}
#else
void sub_0001F020(void) { g_esp += 4; }
#endif

/* ---- a guest stack switch, as tools/recomp lifts one (XBA-101) ---------
 *
 *     push ebx; push ebp; push esi; push edi
 *     xchg esp, [SLOT]
 *     pop edi; pop esi; pop ebp; pop ebx
 *     ret
 */
static void guest_switch(void)
{
    uint32_t entry = G32(g_esp), save, load;

    g_esp -= 4; G32(g_esp) = g_ebx;
    g_esp -= 4; G32(g_esp) = g_ebp;
    g_esp -= 4; G32(g_esp) = g_esi;
    g_esp -= 4; G32(g_esp) = g_edi;
    save = g_esp;
    load = G32(SLOT);
    G32(SLOT) = save;
    g_esp = load;
    g_edi = G32(g_esp); g_esp += 4;
    g_esi = G32(g_esp); g_esp += 4;
    g_ebp = G32(g_esp); g_esp += 4;
    g_ebx = G32(g_esp); g_esp += 4;
    g_seh_ebp = g_ebp;
    recomp_stack_switch_ret(save, load, entry, 0u, VA_SW);
    g_esp += 4;
}

/* A new fiber's stack: what the switch pops, the address its ret goes to,
 * and the return address of the function it starts. */
static void make_fiber(uint32_t top, uint32_t start, uint32_t then)
{
    uint32_t sp = top - 4;
    G32(sp) = 0;
    sp -= 4; G32(sp) = then;
    sp -= 4; G32(sp) = start;
    sp -= 16;
    memset((void *)((uintptr_t)sp + g_xbox_mem_offset), 0, 16);
    G32(SLOT) = sp;
}

/* What the fiber saw, for main to check once it is back. */
static uint32_t fib_eax, fib_ctx, fib_rec, fib_ret;
static int fib_handled;
static XboxSehStats fib_st;

/* Scenario 7: f_outer ran on the fiber and returned here. Back to main for
 * good (this context stays parked). */
static void fiber_exit(void)
{
    fib_eax = g_eax;
    fib_ctx = recomp_stack_context();
    xbox_SehStatsGet(&fib_st, 0);
    g_esp -= 4; G32(g_esp) = VA_FIBER_RET;
    guest_switch();
    printf("  fiber resumed after its exit\n");
    abort();
}

/* Scenario 8: raise, on this fiber, the record main's RAISE was given; the
 * chain's frames are main's. */
static void fiber_raise(void)
{
    fib_ctx = recomp_stack_context();
    if (raise_on_fiber == 2)
        xbox_SehSetGuestStack(0, FIBER_LO_TOP);   /* its top is known */
    fib_handled = xbox_SehRaiseRecord(fib_rec, fib_ret, g_esp);
    g_esp -= 4; G32(g_esp) = VA_FIBER_RET;
    guest_switch();
    printf("  fiber resumed after its raise\n");
    abort();
}

void raise_from_fiber(uint32_t rec, uint32_t ret)
{
    fib_rec = rec;
    fib_ret = ret;
    make_fiber(FIBER_LO_TOP, VA_FIBER_RAISE, 0);
    g_esp -= 4; G32(g_esp) = VA_STUB_RET;
    guest_switch();
}

recomp_func_t seh_fixture_lookup_harness(uint32_t va);
recomp_func_t seh_fixture_lookup_harness(uint32_t va)
{
    switch (va) {
    case VA_SW:          return guest_switch;
    case VA_FIBER_EXIT:  return fiber_exit;
    case VA_FIBER_RAISE: return fiber_raise;
    default:             return 0;
    }
}

/* A guest call from the host: push a return address, call, check balance. */
static uint32_t guest_call_at(recomp_func_t fn, uint32_t top, uint32_t *esp_after)
{
    g_esp = top;
    g_esp -= 4;
    G32(g_esp) = 0xDEAD0000u;
    fn();
    *esp_after = g_esp;
    return g_eax;
}

static uint32_t guest_call(recomp_func_t fn, uint32_t *esp_after)
{
    return guest_call_at(fn, STACK_HI - 0x100, esp_after);
}

static void reset(int32_t filter)
{
    G32(COUNTER) = 0;
    G32(FILTER) = (uint32_t)filter;
    G32(SEEN_CODE) = 0;
    G32(TIB_VA) = END_OF_CHAIN;
    flip_host_stack = other_guest_stack = raise_on_fiber = longjmp_from_raise = 0;
    G32(DIVISOR) = 0;
    G32(PROBE_MODE) = 0;
    G32(SEEN_INFO0) = G32(SEEN_INFO1) = G32(SEEN_ESI) = G32(SEEN_ESP) = G32(SEEN_EBP) = 0;
    {
        XboxSehStats s;
        xbox_SehStatsGet(&s, 1);
    }
}

/* The discovery file's text (a refused cross-stack resume is recorded). */
static char disc_path[256];
static const char *disc_text(void)
{
    static char buf[8192];
    FILE *f = fopen(disc_path, "rb");
    size_t n = 0;
    buf[0] = 0;
    if (f) {
        n = fread(buf, 1, sizeof buf - 1, f);
        buf[n] = 0;
        fclose(f);
    }
    return buf;
}

#ifndef _WIN32
/* f_probe's access: its entry esp (the return address pushed) less ebp, the
 * four EH3 dwords, 8 bytes of locals, three registers and esi. */
#define PROBE_ENTRY (STACK_HI - 0x100 - 4)
#define PROBE_ESP   (PROBE_ENTRY - 4 - 16 - 8 - 12 - 4)

static void test_host_faults(void)
{
    uint32_t eax, esp;
    XboxSehStats st;
    int *depth;
    int status = 0;
    pid_t pid;

    printf("host faults on guest addresses (XBA-132)\n");

    /* 11. A load from a PROT_NONE guest page inside f_probe's __try: the
     *     filter sees STATUS_ACCESS_VIOLATION { read, address } and the
     *     registers of the access, and the __except block runs. */
    reset(1);
    probe_protect(PROT_NONE);
    g_esi = 0xABCD0123u;
    eax = guest_call(f_probe, &esp);
    xbox_SehStatsGet(&st, 0);
    depth = xbox_SehFaultDepthSlot();
    check(eax == 0x777, "read fault: __except block ran (eax 0x777)");
    check(G32(SEEN_CODE) == XBOX_STATUS_ACCESS_VIOLATION && G32(SEEN_INFO0) == 0
          && G32(SEEN_INFO1) == PROBE, "filter saw STATUS_ACCESS_VIOLATION { read, 0x00040000 }");
    check(G32(SEEN_ESI) == 0x5EED0001u && G32(SEEN_ESP) == PROBE_ESP
          && G32(SEEN_EBP) == PROBE_ENTRY - 4, "CONTEXT: esi, esp and ebp of the faulting access");
    check(st.host_faults == 1 && st.raises == 1 && st.resumes == 1, "one host fault, raised once, resumed once");
    check(esp == STACK_HI - 0x100 && G32(TIB_VA) == END_OF_CHAIN && g_esi == 0xABCD0123u,
          "stack, chain and esi balanced");
    check(depth && *depth == 0, "no trampoline left in flight after the resume");

    /* 12. A store. */
    reset(1);
    G32(PROBE_MODE) = 1;
    probe_protect(PROT_NONE);
    eax = guest_call(f_probe, &esp);
    check(eax == 0x777 && G32(SEEN_INFO0) == 1 && G32(SEEN_INFO1) == PROBE + 4,
          "write fault: { write, 0x00040004 }, __except block ran");

    /* 13. A guard page: the filter commits it and continues execution; the
     *     faulting host context comes back and the access runs again. */
    reset(-1);
    probe_protect(PROT_NONE);
    g_esi = 0xABCD0123u;
    eax = guest_call(f_probe, &esp);
    xbox_SehStatsGet(&st, 0);
    check(eax == 0x42, "guard page: committed by the filter, the load ran again (0x41 + 1)");
    check(st.host_faults == 1 && st.handled == 1 && st.resumes == 0, "handled by continuing, no resume");
    check(esp == STACK_HI - 0x100 && G32(TIB_VA) == END_OF_CHAIN && g_esi == 0xABCD0123u,
          "stack, chain and esi balanced");
    check(depth && *depth == 0, "the trampoline put the context back");

    reset(-1);
    G32(PROBE_MODE) = 1;
    probe_protect(PROT_NONE);
    eax = guest_call(f_probe, &esp);
    check(eax == 0x5EED0002u && G32(PROBE + 4) == 0x5EED0001u,
          "guard page, store: it ran again and landed");

    /* 14. Nothing handles it: the handler that was there before ours gets
     *     the original fault, once, with its address. */
    reset(0);
    probe_protect(PROT_NONE);
    s_prev_calls = 0;
    s_prev_addr = NULL;
    if (sigsetjmp(s_prev_jump, 1) == 0) {
        guest_call(f_probe, &esp);
        check(0, "unhandled: the fault reached the previous handler");
    } else {
        xbox_SehStatsGet(&st, 0);
        check(s_prev_calls == 1 && s_prev_addr == (void *)(g_mem + PROBE),
              "unhandled: the previous handler got the original fault, once");
        check(st.host_faults == 1 && st.unhandled == 1
              && G32(SEEN_CODE) == XBOX_STATUS_ACCESS_VIOLATION, "after the filter declined it");
        check(depth && *depth == 0, "no trampoline left in flight");
    }
    probe_protect(PROT_READ | PROT_WRITE);

    /* 15. ... and with the default action the process dies of the fault. */
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        struct sigaction dfl;
        memset(&dfl, 0, sizeof dfl);
        dfl.sa_handler = SIG_DFL;
        sigaction(SIGSEGV, &dfl, NULL);
        sigaction(SIGBUS, &dfl, NULL);
        alarm(10);
        if (xbox_PosixInstallMmioFaultHandler() != 0)   /* back in front, SIG_DFL behind */
            _exit(3);
        reset(0);
        probe_protect(PROT_NONE);
        guest_call(f_probe, &esp);
        _exit(0);
    }
    if (pid > 0)
        waitpid(pid, &status, 0);
    check(pid > 0 && WIFSIGNALED(status)
          && (WTERMSIG(status) == SIGSEGV || WTERMSIG(status) == SIGBUS),
          "unhandled, default action: the process dies of the original signal");
}

/* RECOMP_SEH_FAULTS=0: the fault goes down the chain at once. */
static void test_faults_off(void)
{
    uint32_t esp;
    XboxSehStats st;

    printf("RECOMP_SEH_FAULTS=0\n");
    reset(1);
    probe_protect(PROT_NONE);
    s_prev_calls = 0;
    if (sigsetjmp(s_prev_jump, 1) == 0) {
        guest_call(f_probe, &esp);
        check(0, "faults off: the fault reached the previous handler");
    } else {
        xbox_SehStatsGet(&st, 0);
        check(s_prev_calls == 1 && s_prev_addr == (void *)(g_mem + PROBE),
              "faults off: straight to the previous handler");
        check(st.raises == 0 && st.host_faults == 0 && G32(SEEN_CODE) == 0,
              "nothing raised, no filter ran");
    }
    probe_protect(PROT_READ | PROT_WRITE);
}
#endif

/* 16. A guest longjmp from inside f_inner's __try (try level 0) to a buffer
 *     its setjmp filled before the __try (try level -1): the frame stays,
 *     its try levels are unwound -- by the jmp_buf's UnwindFunc (a 'VC20'
 *     buffer, guest code) or by the native _local_unwind2 (an old one). */
static void test_longjmp_local(int vc20)
{
    static volatile int landed;
    uint32_t esp, reg;

    reset(0);
    landed = 0;
    g_esp = STACK_HI - 0x100 - 4;
    G32(JMPBUF + 0x04) = g_ebx;
    G32(JMPBUF + 0x08) = g_edi;
    G32(JMPBUF + 0x0C) = g_esi;
    G32(JMPBUF + 0x10) = g_esp;
    G32(JMPBUF + 0x14) = 0xDEAD0002u;
    G32(JMPBUF + 0x1C) = 0xFFFFFFFFu;               /* TryLevel at setjmp */
    G32(JMPBUF + 0x20) = vc20 ? VC20 : 0;           /* Cookie */
    G32(JMPBUF + 0x24) = vc20 ? seh_fixture_my_lj_unwind : 0;
    if (setjmp(*recomp_setjmp_slot(JMPBUF)) == 0) {
        longjmp_from_raise = 2;
        guest_call(f_outer, &esp);
    } else {
        landed = 1;
    }
    longjmp_from_raise = 0;
    reg = G32(JMPBUF + 0x18);
    check(landed == 1 && g_esp == STACK_HI - 0x100, vc20 ? "VC20 buffer: guest longjmp landed"
                                                        : "old buffer: guest longjmp landed");
    check(G32(COUNTER) == (vc20 ? 0x101u : 1u),
          vc20 ? "the UnwindFunc ran f_inner's __finally, once"
               : "the native _local_unwind2 ran f_inner's __finally, once");
    check(G32(reg + 0xC) == 0xFFFFFFFFu && G32(TIB_VA) == reg,
          "the frame stays registered, at the setjmp-time try level");
}

/* 17. A worker stack slice: while the thread runs there, its guest stack
 *     bounds are the slice's, so a raise on it walks its frames and resumes;
 *     freeing the slice gives the thread's bounds back. */
static void test_worker_slice(void)
{
    uint32_t lo0, hi0, lo, hi, eax, esp, top;
    int slot;

    reset(1);
    xbox_SehGetGuestStack(&lo0, &hi0);
    slot = xbox_worker_stack_alloc();
    if (slot < 0) {
        check(0, "a worker stack slice is free");
        return;
    }
    top = XBOX_WORKER_STACK_TOP(slot);
    xbox_SehGetGuestStack(&lo, &hi);
    check(lo == top + 16 - XBOX_WORKER_STACK_SIZE && hi == top + 16,
          "worker slice: the thread's guest stack bounds are the slice");
    eax = guest_call_at(f_outer, top, &esp);
    check(eax == 0x222 && G32(COUNTER) == 1 && esp == top,
          "a raise on the slice resumes at f_outer's __except");
    xbox_worker_stack_free(slot);
    xbox_SehGetGuestStack(&lo, &hi);
    check(lo == lo0 && hi == hi0, "freeing the slice gives the thread's bounds back");
}

int main(int argc, char **argv)
{
    uint8_t *mem;
    uint32_t eax, esp;
    XboxSehStats st;
    int faults_off = argc > 1 && strcmp(argv[1], "faults-off") == 0;
#ifndef _WIN32
    mem = (uint8_t *)mmap(NULL, GUEST_SIZE, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == (uint8_t *)MAP_FAILED)
        mem = NULL;
    g_page = (size_t)sysconf(_SC_PAGESIZE);
#else
    mem = (uint8_t *)calloc(1, GUEST_SIZE);
#endif
    g_mem = mem;
#ifndef _WIN32
    {
        const char *tmp = getenv("TMPDIR");
        snprintf(disc_path, sizeof disc_path, "%s/xba_seh_dispatch_%d.jsonl",
                 tmp && *tmp ? tmp : "/tmp", (int)getpid());
        remove(disc_path);
        setenv("RECOMP_DISCOVERY", disc_path, 1);
    }
#endif

    if (!mem) {
        printf("no memory\n");
        return 1;
    }
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)mem;
    memcpy(mem + seh_fixture_text_va, seh_fixture_text, seh_fixture_text_size);
    memcpy(mem + seh_fixture_rdata_va, seh_fixture_rdata, seh_fixture_rdata_size);
    g_fs_base = TIB_VA;
    xbox_SehSetGuestStack(STACK_LO, STACK_HI);
#ifndef _WIN32
    /* What a title's process has: someone's handler first (debuggerd, ART),
     * the toolkit's in front of it. */
    install_previous();
    if (xbox_PosixInstallMmioFaultHandler() != 0) {
        printf("could not install the fault handler\n");
        return 1;
    }
    if (faults_off) {
        test_faults_off();
        munmap(mem, GUEST_SIZE);
        printf("%s (%d failure%s)\n", failures ? "FAIL" : "PASS", failures,
               failures == 1 ? "" : "s");
        return failures ? 1 : 0;
    }
#else
    if (faults_off)
        return 77;
#endif

    printf("seh_dispatch: lifted __try/__except, __finally, C++ catch\n");

    /* 1. Filter says execute: resume at f_outer's __except, across f_inner,
     *    whose __finally runs during the unwind. */
    reset(1);
    eax = guest_call(f_outer, &esp);
    xbox_SehStatsGet(&st, 0);
    check(eax == 0x222, "__except block ran (eax 0x222)");
    check(G32(SEEN_CODE) == 0xE0000001u, "filter saw the raised code");
    check(G32(COUNTER) == 1, "inner __finally ran once, during the unwind");
    check(esp == STACK_HI - 0x100, "guest stack balanced after the resume");
    check(G32(TIB_VA) == END_OF_CHAIN, "chain head restored");
    check(st.resumes == 1 && st.refused == 0, "one resume, nothing refused");
    check(st.anchors >= 1, "f_outer took an anchor");

    /* 2. Filter continues execution: the raise returns, the normal path
     *    calls the __finally directly. */
    reset(-1);
    eax = guest_call(f_outer, &esp);
    xbox_SehStatsGet(&st, 0);
    check(eax == 0x111, "continue-execution: the try body finished (0x111)");
    check(G32(COUNTER) == 1, "inner __finally ran once, on the normal path");
    check(esp == STACK_HI - 0x100 && G32(TIB_VA) == END_OF_CHAIN,
          "stack and chain balanced");
    check(st.handled == 1 && st.resumes == 0, "handled by continuing");

    /* 3. Filter searches on: nothing handles it, the raise returns. */
    reset(0);
    eax = guest_call(f_outer, &esp);
    xbox_SehStatsGet(&st, 0);
    check(eax == 0x111 && G32(COUNTER) == 1, "unhandled: the raise returned");
    check(st.unhandled == 1 && st.handled == 0, "counted as unhandled");
    check(esp == STACK_HI - 0x100 && G32(TIB_VA) == END_OF_CHAIN,
          "stack and chain balanced");

    /* 4. The resume target's anchor was taken on another host stack (a
     *    fiber, XBA-101): refused and reported, never jumped to. The
     *    handler's unwind already ran the __finally; the raise returns and
     *    f_inner's normal path runs it again, as it would on the console
     *    after an unhandled raise returns. */
    reset(1);
    flip_host_stack = 1;
    eax = guest_call(f_outer, &esp);
    xbox_SehStatsGet(&st, 0);
    check(eax == 0x111, "other host stack: refused, the raise returned");
    check(st.refused == 1 && st.resumes == 0, "counted as refused");
#ifndef _WIN32
    check(strstr(disc_text(), "{\"t\":\"stack_switch\",\"va\":\"0x000100B1\",\"to\":\"0x00010078\"") != NULL,
          "recorded as a stack_switch blocker in the discovery file");
#endif
    check(esp == STACK_HI - 0x100 && G32(TIB_VA) == END_OF_CHAIN,
          "stack and chain balanced");

    /* 5. The chain's frames lie outside the current guest stack: not
     *    followed, no filter runs. */
    reset(1);
    other_guest_stack = 1;
    eax = guest_call(f_outer, &esp);
    xbox_SehStatsGet(&st, 0);
    check(eax == 0x111 && G32(SEEN_CODE) == 0,
          "other guest stack: no handler called, the raise returned");
    check(st.refused == 1 && st.unhandled == 1, "counted as refused, unhandled");
    check(esp == STACK_HI - 0x100 && G32(TIB_VA) == END_OF_CHAIN,
          "stack and chain balanced");

    /* 6. C++: throw through f_cxx_inner (destructor funclet) into
     *    f_cxx's catch-all, resumed at the continuation it returns. */
    reset(0);
    eax = guest_call(f_cxx, &esp);
    xbox_SehStatsGet(&st, 0);
    check(eax == 0x444, "catch continuation ran (eax 0x444)");
    check(G32(COUNTER) == 0x10, "inner destructor ran once, during the unwind");
    check(esp == STACK_HI - 0x100 && G32(TIB_VA) == END_OF_CHAIN,
          "stack and chain balanced");
    check(st.resumes == 1, "one resume");

    /* 7. A guest stack switch (XBA-101) to a fiber that runs f_outer: the
     *    exception is raised, filtered, unwound and resumed on the fiber's
     *    own host stack. The switch gave the dispatcher the fiber's token
     *    and bounds (unknown here: not a heap block), and gives main's back
     *    when main resumes. */
    if (recomp_stack_switch_enabled()) {
        uint32_t lo, hi;
        reset(1);
        make_fiber(FIBER_HI_TOP, seh_fixture_f_outer, VA_FIBER_EXIT);
        g_esp = STACK_HI - 0x100;
        g_esp -= 4; G32(g_esp) = VA_MAIN_RET;
        guest_switch();
        xbox_SehGetGuestStack(&lo, &hi);
        check(fib_eax == 0x222 && G32(COUNTER) == 1,
              "on a switched-to stack: __except ran, __finally once");
        check(fib_st.resumes == 1 && fib_st.refused == 0, "one resume, nothing refused");
        check(fib_ctx != 0 && recomp_stack_context() == 0,
              "the fiber had its own host stack; main is back on its own");
        check(lo == STACK_LO && hi == STACK_HI, "main's guest stack bounds are back");
        check(g_esp == STACK_HI - 0x100 && G32(TIB_VA) == END_OF_CHAIN,
              "stack and chain balanced");

        /* 8. main registers f_outer's and f_inner's frames, then its RAISE
         *    switches to a fiber that raises with that chain: the handler
         *    frames live on main's stack. Their top unknown, the handler is
         *    called, but its resume (an anchor on main's host stack) is
         *    refused and reported, never jumped to; the raise returns on the
         *    fiber, which switches back, and main's normal path runs. */
        reset(1);
        raise_on_fiber = 1;
        eax = guest_call(f_outer, &esp);
        xbox_SehStatsGet(&st, 0);
        check(eax == 0x111 && fib_handled == 0 && G32(SEEN_CODE) == 0xE0000001u,
              "handler on another stack: filter ran, resume refused");
        check(st.refused == 1 && st.resumes == 0, "counted as refused, no resume");
        check(fib_ctx != 0 && recomp_stack_context() == 0, "raised on the fiber's host stack");
        check(esp == STACK_HI - 0x100 && G32(TIB_VA) == END_OF_CHAIN,
              "stack and chain balanced");

        /*    The same with the fiber's top known (a heap stack): the frames
         *    above it are not even followed. */
        reset(1);
        raise_on_fiber = 2;
        eax = guest_call(f_outer, &esp);
        xbox_SehStatsGet(&st, 0);
        xbox_SehGetGuestStack(&lo, &hi);
        check(eax == 0x111 && fib_handled == 0 && G32(SEEN_CODE) == 0,
              "fiber's top known: main's frames not followed");
        check(st.refused == 1 && st.unhandled == 1, "counted as refused, unhandled");
        check(lo == STACK_LO && hi == STACK_HI, "main's bounds back after the fiber's own");
        check(esp == STACK_HI - 0x100 && G32(TIB_VA) == END_OF_CHAIN,
              "stack and chain balanced");
    } else {
        printf("  7-8. no guest stack switch on this host: skipped\n");
    }

    /* 9. A lifted `div ecx` by zero (XBA-110) inside f_div's __try: the
     *    runtime raises STATUS_INTEGER_DIVIDE_BY_ZERO and the __except
     *    block runs. */
    reset(1);
    eax = guest_call(f_div, &esp);
    xbox_SehStatsGet(&st, 0);
    check(eax == 0x555, "div by zero: __except block ran (eax 0x555)");
    check(G32(SEEN_CODE) == XBOX_STATUS_INTEGER_DIVIDE_BY_ZERO,
          "filter saw STATUS_INTEGER_DIVIDE_BY_ZERO");
    check(st.raises == 1 && st.resumes == 1, "raised once, resumed once");
    check(esp == STACK_HI - 0x100 && G32(TIB_VA) == END_OF_CHAIN,
          "stack and chain balanced");

    /*    The filter continues execution: the div is skipped, the body goes on. */
    reset(-1);
    eax = guest_call(f_div, &esp);
    xbox_SehStatsGet(&st, 0);
    check(eax == 0x111 && st.handled == 1 && st.resumes == 0,
          "div by zero, continue-execution: the body went on");

    /*    A divisor that is not zero raises nothing. */
    reset(1);
    G32(DIVISOR) = 2;
    eax = guest_call(f_div, &esp);
    xbox_SehStatsGet(&st, 0);
    check(eax == 0x111 && st.raises == 0, "div by 2: no exception");

    /* 10. A guest longjmp (the native one, recomp_guest_longjmp) from inside
     *     f_outer -> f_inner -> g_raises to a setjmp taken before f_outer:
     *     the frames it leaves are unwound first, as the CRT's longjmp does
     *     (RtlUnwind to the registration setjmp saved at +0x18), so
     *     f_inner's __finally runs and fs:[0] is the end of the chain again
     *     rather than a dead frame below esp. */
    {
        static volatile int landed;
        reset(0);
        landed = 0;
        g_esp = STACK_HI - 0x100 - 4;               /* at setjmp's return address */
        G32(JMPBUF + 0x00) = g_ebp;
        G32(JMPBUF + 0x04) = g_ebx;
        G32(JMPBUF + 0x08) = g_edi;
        G32(JMPBUF + 0x0C) = g_esi;
        G32(JMPBUF + 0x10) = g_esp;
        G32(JMPBUF + 0x14) = 0xDEAD0001u;
        G32(JMPBUF + 0x18) = G32(TIB_VA);           /* Registration */
        G32(JMPBUF + 0x1C) = 0xFFFFFFFFu;           /* TryLevel */
        if (setjmp(*recomp_setjmp_slot(JMPBUF)) == 0) {
            longjmp_from_raise = 1;
            guest_call(f_outer, &esp);
        } else {
            landed = 1;
        }
        longjmp_from_raise = 0;
        xbox_SehStatsGet(&st, 0);
        check(landed == 1 && g_esp == STACK_HI - 0x100, "guest longjmp landed");
        check(G32(COUNTER) == 1, "the __finally of the frame it left ran");
        check(G32(TIB_VA) == END_OF_CHAIN, "fs:[0] is the setjmp-time head");
        check(st.raises == 0, "no exception raised");
    }

    /* XBA-132 */
#ifndef _WIN32
    test_host_faults();
#endif
    test_longjmp_local(1);
    test_longjmp_local(0);
    test_worker_slice();

#ifndef _WIN32
    remove(disc_path);
#endif
    printf("%s (%d failure%s)\n", failures ? "FAIL" : "PASS", failures,
           failures == 1 ? "" : "s");
#ifndef _WIN32
    munmap(mem, GUEST_SIZE);
#else
    free(mem);
#endif
    return failures ? 1 : 0;
}
