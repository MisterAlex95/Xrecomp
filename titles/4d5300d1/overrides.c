/* Hand-written replacements for generated functions (Fable: The Lost Chapters).
 *
 * tools.recomp --exclude-manual (tools.prepare passes this file) skips generating the
 * bodies defined here, so every caller, direct or indirect, reaches these. */
#define RECOMP_GENERATED_CODE   /* register names (eax, esp...) as in generated code */
#include "recomp/gen/recomp_types.h"
#include <stdlib.h>
#ifndef _WIN32
#include <sched.h>
#include <time.h>
#endif

/* XMVDecoder_GetNextFrame(IXMVDecoder *, IDirect3DSurface8 *, XMVRESULT *,
 *                         REFERENCE_TIME *)  -- stdcall, 16 bytes of args.
 *
 * The XMV decoder never produced a frame here, so the intro-movie loop
 * (sub_0003B530, lionhead_logo.xmv) polled XMV_NOFRAME (0) and slept for ever.
 * Report XMV_ENDOFFILE (2) instead: every movie ends at once and the title
 * moves on. Remove when XMV playback works. */
void sub_006440AB(void)
{
    uint32_t result_ptr = MEM32(esp + 0x0C);
    uint32_t time_ptr = MEM32(esp + 0x10);

    if (result_ptr)
        MEM32(result_ptr) = 2;           /* XMV_ENDOFFILE */
    if (time_ptr) {
        MEM32(time_ptr) = 0;
        MEM32(time_ptr + 4) = 0;
    }
    eax = 0;                             /* S_OK */
    esp += 4 + 16;                       /* ret 0x10 */
}

/* ── Fibers: sub_0004B440, the context switch ─────────────────────────────
 *
 * Fable runs its loader and its scripts as fibers on one shared guest stack
 * area. A fiber is resumed by sub_0004B490 (ebx = the fiber): it copies the
 * fiber's saved stack bytes under the top of that area, stores the fiber at
 * 0x00990A88 and the stack pointer to switch to at 0x00990A8C, and calls
 * sub_0004B440:
 *
 *     cli; push ebx, ebp, esi, edi, fs:[0]
 *     xchg esp, [0x00990A8C]
 *     pop fs:[0], edi, esi, ebp, ebx; sti; ret
 *
 * The ret continues on the other stack: in a new fiber at the entry its
 * creator stored there (0x0004B470, or 0x000939E0 for a queued task; both run
 * the fiber's work and then switch away for ever), otherwise after the call to
 * sub_0004B440 that left it. The fiber calls the same function to give the CPU
 * back, and sub_0004B490 then saves its stack bytes again.
 *
 * Lifted, that ret is a C return, so both sides stayed in one C call chain:
 * the resumer came straight back with the fiber's registers and the fiber
 * never ran. The first in-game load waits on the loader fiber at 0x00269210,
 * so it hung there (edi = 0x0004B470, the new fiber's entry address).
 *
 * Here the guest half is the same and the C half switches host stacks: each
 * fiber owns a host stack, and the switch is taken on the same host thread, so
 * the guest registers and the thread identity (critical sections) carry over.
 * A fiber that has finished waits in a loop around the switch (0x0004B480,
 * 0x00093A66); its host stack is released when it reaches that loop and
 * rebuilt if it is ever resumed.
 *
 * The host switch is written for arm64 and x86-64 POSIX hosts. Elsewhere only
 * the guest half runs, as the lifted function did. */
void sub_0004B440(void);
void sub_0004B470(void);

#if (defined(__aarch64__) || defined(__x86_64__)) && !defined(_WIN32)
#define FABLE_HOST_FIBERS 1
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

#define FABLE_FIBER_CURRENT  0x00990A88u   /* the fiber being resumed */
#define FABLE_FIBER_ESP      0x00990A8Cu   /* esp of the side not running */
/* The three calls a fiber makes to sub_0004B440 return here: the yield in
 * sub_000939D0, and the two loops a finished fiber waits in. Any other address
 * a fiber is resumed at is the entry of a new one. */
#define FABLE_FIBER_YIELD    0x000939D8u
#define FABLE_FIBER_IDLE_A   0x0004B485u
#define FABLE_FIBER_IDLE_B   0x00093A6Bu
#define FABLE_FIBER_IS_IDLE(va) \
    ((va) == FABLE_FIBER_IDLE_A || (va) == FABLE_FIBER_IDLE_B)
#define FABLE_FIBER_STACK    (8u << 20)    /* host stack, committed on use */
#define FABLE_FIBER_MAX      1024

/* Save the callee-saved registers on this stack, leave its pointer in *save,
 * and continue on the stack `load` names. fable_ctx_boot starts a new one. */
void fable_ctx_switch(void **save, void *load) __asm__("fable_ctx_switch");
void fable_ctx_boot(void) __asm__("fable_ctx_boot");

#if defined(__aarch64__)
#define FABLE_CTX_WORDS 20
__asm__(
    ".text\n"
    ".p2align 2\n"
    ".globl fable_ctx_switch\n"
    "fable_ctx_switch:\n"
    "    sub  sp, sp, #0xA0\n"
    "    stp  x19, x20, [sp, #0x00]\n"
    "    stp  x21, x22, [sp, #0x10]\n"
    "    stp  x23, x24, [sp, #0x20]\n"
    "    stp  x25, x26, [sp, #0x30]\n"
    "    stp  x27, x28, [sp, #0x40]\n"
    "    stp  x29, x30, [sp, #0x50]\n"
    "    stp  d8,  d9,  [sp, #0x60]\n"
    "    stp  d10, d11, [sp, #0x70]\n"
    "    stp  d12, d13, [sp, #0x80]\n"
    "    stp  d14, d15, [sp, #0x90]\n"
    "    mov  x9, sp\n"
    "    str  x9, [x0]\n"
    "    mov  sp, x1\n"
    "    ldp  x19, x20, [sp, #0x00]\n"
    "    ldp  x21, x22, [sp, #0x10]\n"
    "    ldp  x23, x24, [sp, #0x20]\n"
    "    ldp  x25, x26, [sp, #0x30]\n"
    "    ldp  x27, x28, [sp, #0x40]\n"
    "    ldp  x29, x30, [sp, #0x50]\n"
    "    ldp  d8,  d9,  [sp, #0x60]\n"
    "    ldp  d10, d11, [sp, #0x70]\n"
    "    ldp  d12, d13, [sp, #0x80]\n"
    "    ldp  d14, d15, [sp, #0x90]\n"
    "    add  sp, sp, #0xA0\n"
    "    ret\n"
    ".globl fable_ctx_boot\n"
    "fable_ctx_boot:\n"
    "    mov  x0, x20\n"
    "    blr  x19\n"
    "    brk  #1\n");

static void *fable_ctx_make(void *top, void (*fn)(void *), void *arg)
{
    void **sp = (void **)top - FABLE_CTX_WORDS;
    for (int i = 0; i < FABLE_CTX_WORDS; i++)
        sp[i] = NULL;
    sp[0] = (void *)fn;                 /* x19 */
    sp[1] = arg;                        /* x20 */
    sp[11] = (void *)fable_ctx_boot;    /* x30 */
    return sp;
}
#else
__asm__(
    ".text\n"
    ".globl fable_ctx_switch\n"
    "fable_ctx_switch:\n"
    "    pushq %rbp\n"
    "    pushq %rbx\n"
    "    pushq %r12\n"
    "    pushq %r13\n"
    "    pushq %r14\n"
    "    pushq %r15\n"
    "    movq  %rsp, (%rdi)\n"
    "    movq  %rsi, %rsp\n"
    "    popq  %r15\n"
    "    popq  %r14\n"
    "    popq  %r13\n"
    "    popq  %r12\n"
    "    popq  %rbx\n"
    "    popq  %rbp\n"
    "    ret\n"
    ".globl fable_ctx_boot\n"
    "fable_ctx_boot:\n"
    "    movq  %r12, %rdi\n"
    "    callq *%r13\n"
    "    ud2\n");

static void *fable_ctx_make(void *top, void (*fn)(void *), void *arg)
{
    void **sp = (void **)top - 7;       /* r15 r14 r13 r12 rbx rbp, return */
    for (int i = 0; i < 7; i++)
        sp[i] = NULL;
    sp[2] = (void *)fn;                 /* r13 */
    sp[3] = arg;                        /* r12 */
    sp[6] = (void *)fable_ctx_boot;
    return sp;
}
#endif

typedef struct {
    uint32_t fiber;     /* guest address of the fiber; 0 = free slot */
    uint32_t entry;     /* guest address its host stack starts at */
    int      idle;      /* last left from the finished loop */
    void    *stack;     /* host stack, FABLE_FIBER_STACK bytes */
    void    *sp;        /* saved host stack pointer while not running */
} FableFiber;

/* One guest thread runs fibers (the shared stack area allows no more), so the
 * table needs no lock; the running fiber and the resumer's stack are per host
 * thread all the same. */
static FableFiber g_fable_fibers[FABLE_FIBER_MAX];
static RECOMP_TLS FableFiber *t_fable_running;
static RECOMP_TLS void *t_fable_resumer_sp;

static void fable_fiber_main(void *arg)
{
    FableFiber *f = arg;

    if (!FABLE_FIBER_IS_IDLE(f->entry)) {
        recomp_func_t entry = recomp_lookup(f->entry);

        if (entry)
            entry();                    /* does not return */
        fprintf(stderr, "[FABLE] fiber 0x%08X: entry 0x%08X %s\n", f->fiber,
                f->entry, entry ? "returned" : "is not a function");
        abort();
    }
    for (;;) {                          /* call sub_0004B440; jmp */
        PUSH32(esp, f->entry);
        sub_0004B440();
    }
}

static FableFiber *fable_fiber_find(uint32_t fiber)
{
    FableFiber *spare = NULL;

    for (int i = 0; i < FABLE_FIBER_MAX; i++) {
        if (g_fable_fibers[i].fiber == fiber)
            return &g_fable_fibers[i];
        if (!spare && !g_fable_fibers[i].fiber)
            spare = &g_fable_fibers[i];
    }
    return spare;                       /* a free slot, or NULL: table full */
}
#endif /* arm64 / x86-64, POSIX */

void sub_0004B440(void)
{
    uint32_t from = MEM32(esp);         /* where this side continues */
    uint32_t other, to;

    PUSH32(esp, ebx);
    PUSH32(esp, g_seh_ebp);
    PUSH32(esp, esi);
    PUSH32(esp, edi);
    PUSH32(esp, MEM32(XBOX_FS_BASE));
    other = MEM32(FABLE_FIBER_ESP);
    MEM32(FABLE_FIBER_ESP) = esp;
    esp = other;
    POP32(esp, eax);
    MEM32(XBOX_FS_BASE) = eax;
    POP32(esp, edi);
    POP32(esp, esi);
    POP32(esp, g_ebp);
    g_seh_ebp = g_ebp;
    POP32(esp, ebx);
    POP32(esp, to);                     /* ret */

#ifdef FABLE_HOST_FIBERS
    FableFiber *f = t_fable_running;

    if (f) {                            /* a fiber gives the CPU back */
        f->idle = FABLE_FIBER_IS_IDLE(from);
        t_fable_running = NULL;
        fable_ctx_switch(&f->sp, t_fable_resumer_sp);
        return;                         /* resumed */
    }

    f = fable_fiber_find(MEM32(FABLE_FIBER_CURRENT));
    if (!f) {
        fprintf(stderr, "[FABLE] fiber table full (%d)\n", FABLE_FIBER_MAX);
        abort();
    }
    if (to != FABLE_FIBER_YIELD) {
        /* A new fiber (over a stale entry when the game dropped a suspended
         * one and reused its address), or a finished one resumed again. */
        if (!f->stack) {
            f->stack = mmap(NULL, FABLE_FIBER_STACK, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANON, -1, 0);
            if (f->stack == MAP_FAILED) {
                perror("[FABLE] fiber stack");
                abort();
            }
        }
        f->fiber = MEM32(FABLE_FIBER_CURRENT);
        f->entry = to;
        f->idle = 0;
        f->sp = fable_ctx_make((char *)f->stack + FABLE_FIBER_STACK,
                               fable_fiber_main, f);
    }
    else if (!f->fiber) {
        fprintf(stderr, "[FABLE] fiber 0x%08X resumes at 0x%08X with no host "
                        "stack\n", MEM32(FABLE_FIBER_CURRENT), to);
        abort();
    }
    t_fable_running = f;
    fable_ctx_switch(&t_fable_resumer_sp, f->sp);
    if (f->idle)                        /* finished: the slot is free again */
        f->fiber = 0;
#else
    (void)from;
    (void)to;
#endif
}

/* ── D3D's flush kick: sub_008556D0, the wait the main thread spins in ──────
 *
 * XDK D3D's KickOff, before it writes DMA_PUT, sets bit 16 of PFB register
 * 0x100410 (nv = [device+0x1C28]) and spins until the GPU clears it. Here the
 * NV2A ack thread clears it, at the top of each pass and between commands of
 * the segment it executes -- so while the executor rasterises a big batch the
 * title waits for it, and Fable's main thread spent 92-100 % of a core in this
 * loop on the title screen and in-world (sampled under sub_00855520, its
 * caller, into which the shared build had inlined it), a core the
 * rasteriser's workers need, above all on a handheld's five fast cores.
 *
 * The same register traffic and the same flags out, but the wait yields, and
 * after 64 empty turns sleeps 50 us a turn. FABLE_D3D_SPIN=1 restores the
 * bare spin. */
void sub_008556D0(void)
{
    static int spin = -1;
    unsigned turn = 0;

    if (spin < 0) {
        const char *e = getenv("FABLE_D3D_SPIN");
        spin = e && e[0] == '1';
    }
    eax = MEM32(0x00862288u);
    eax = MEM32(eax + 0x1C28);
    ecx = MEM32(eax + 0x100410) | 0x10000u;
    MEM32(eax + 0x100410) = ecx;
    while (MEM32(eax + 0x100410) & 0x10000u) {
        if (spin)
            continue;
#ifndef _WIN32
        if (turn < 64) {
            turn++;
            sched_yield();
        } else {
            struct timespec ts = { 0, 50 * 1000 };
            nanosleep(&ts, NULL);
        }
#endif
    }
    /* test [nv+0x100410], 0x10000 came out zero: ZF and PF set, CF, SF,
     * OF clear. */
    g_eflags = (g_eflags & ~0x8C5u) | 0x4u | 0x40u;
    esp += 4;                                     /* ret */
}
