/*
 * xbox_seh_fault.c - host faults on guest addresses become guest exceptions
 * (XBA-132). The model is in xbox_seh.h (« host faults on guest addresses »);
 * this is the signal-handler and per-architecture half.
 *
 * The signal handler runs on the thread's alternate signal stack (bionic
 * gives every thread a small one) with guest registers that may still live in
 * host registers, so it does not dispatch there. It:
 *
 *  1. saves the faulting host context (integer registers, FP/SIMD state,
 *     signal mask) in the thread's fault level;
 *  2. points the context at seh_fault_tramp, with the stack pointer a little
 *     below the interrupted one, on the thread's own stack (below the x86-64
 *     red zone), and returns. The kernel's sigreturn lands in the trampoline,
 *     an ordinary C frame of the faulting thread.
 *
 * The trampoline raises the guest exception (xbox_SehRaiseHostFault). A
 * handler that resumes longjmps to its anchor, above the faulting frame on
 * the same host stack, and the trampoline is gone with the frames below the
 * anchor. When the raise returns (continue-execution, or nothing took it),
 * the trampoline reads the restore trap page, a PROT_NONE page of ours; the
 * handler recognises that fault and copies the saved context over the new
 * one, so sigreturn puts the thread back on the faulting instruction with
 * every register as it was. When nothing took it, the next fault at that pc
 * and address is first marked to go down the chain: the instruction faults
 * again and ends as it always did (the previous handler, ART's on Android,
 * debuggerd's tombstone with the original context, or the default action).
 *
 * Nesting: a fault inside a handler's dispatch takes the next level (3 deep;
 * past that it goes down the chain). The depth lives in xbox_seh.c's thread
 * state, so a resume puts back the depth its anchor was taken at.
 *
 * Declined, straight down the chain as before: RECOMP_SEH_FAULTS=0, a host
 * without an implementation here, an address outside the guest window, a
 * thread without dispatcher state or with an empty guest chain (nothing could
 * handle it), a fault on the alternate signal stack itself, and FP state
 * this file cannot carry back (x86-64 XSAVE areas over 4 KB, arm64 SVE / SME
 * register data).
 */
#define _GNU_SOURCE     /* glibc: REG_RIP / REG_RSP / REG_ERR / REG_EFL */
#include "xbox_seh.h"
#include "xbox_memory_layout.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* macOS is left out: its arm64 context code below has never been compiled
 * (no macOS toolchain where XBA-132 was written) and the reference titles run
 * on a Mac; there every fault goes down the chain as before, said once. */
#if !defined(_WIN32) && ((defined(__x86_64__) && defined(__linux__)) \
        || (defined(__aarch64__) && defined(__linux__)))
#define SEH_FAULT_HOST 1
#endif

extern ptrdiff_t g_xbox_mem_offset;

void xbox_SehFaultThreadInit(void);

#ifdef SEH_FAULT_HOST

#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif
#if defined(__aarch64__) && defined(__linux__)
#include <asm/sigcontext.h>     /* struct _aarch64_ctx, fpsimd_context */
#endif

#define SEH_FAULT_NEST 3

/* ---- the saved host context, per architecture -------------------------- */

#if defined(__x86_64__)
/* gregs, the signal mask, and the FP area fpregs points at: the 512-byte
 * FXSAVE image, or the whole XSAVE area when the kernel says one follows
 * (sw_reserved at +464: FP_XSTATE_MAGIC1, then its extended size). */
#define SEH_FP_MAX        4096
#define SEH_FP_SW_OFF     464
#define SEH_FP_XMAGIC1    0x46505853u
typedef struct {
    gregset_t     gregs;
    sigset_t      mask;
    size_t        fp_size;
    unsigned char fp[SEH_FP_MAX] __attribute__((aligned(64)));
} seh_hostctx;

static size_t fp_area_size(const void *fp)
{
    uint32_t magic, size;

    memcpy(&magic, (const char *)fp + SEH_FP_SW_OFF, 4);
    memcpy(&size, (const char *)fp + SEH_FP_SW_OFF + 4, 4);
    return magic == SEH_FP_XMAGIC1 ? size : 512;
}

static int ctx_save(const ucontext_t *uc, seh_hostctx *c)
{
    const void *fp = uc->uc_mcontext.fpregs;

    c->fp_size = 0;
    if (fp) {
        size_t n = fp_area_size(fp);
        if (n < 512 || n > SEH_FP_MAX)
            return 0;
        memcpy(c->fp, fp, n);
        c->fp_size = n;
    }
    memcpy(c->gregs, uc->uc_mcontext.gregs, sizeof c->gregs);
    c->mask = uc->uc_sigmask;
    return 1;
}

static void ctx_load(ucontext_t *uc, const seh_hostctx *c)
{
    void *fp = uc->uc_mcontext.fpregs;

    memcpy(uc->uc_mcontext.gregs, c->gregs, sizeof c->gregs);
    uc->uc_sigmask = c->mask;
    if (fp && c->fp_size) {
        if (fp_area_size(fp) == c->fp_size)
            memcpy(fp, c->fp, c->fp_size);
        else    /* not the same layout: the legacy image, this frame's header */
            memcpy(fp, c->fp, SEH_FP_SW_OFF);
    }
}

static uintptr_t ctx_pc(const ucontext_t *uc) { return (uintptr_t)uc->uc_mcontext.gregs[REG_RIP]; }
static uintptr_t ctx_sp(const ucontext_t *uc) { return (uintptr_t)uc->uc_mcontext.gregs[REG_RSP]; }

/* Page-fault error code: bit 1 write, bit 4 instruction fetch. */
static uint32_t ctx_access(const ucontext_t *uc)
{
    greg_t err = uc->uc_mcontext.gregs[REG_ERR];
    return (err & 0x10) ? 8u : (err & 0x2) ? 1u : 0u;
}

static void ctx_redirect(ucontext_t *uc, void (*fn)(void))
{
    /* Below the 128-byte red zone, aligned as just after a call. Nothing is
     * written there now: the signal frame may still be using that memory
     * (no alternate stack), and sigreturn frees it before fn runs. */
    uintptr_t sp = ((ctx_sp(uc) - 128 - 256) & ~(uintptr_t)15) - 8;
    uc->uc_mcontext.gregs[REG_RSP] = (greg_t)sp;
    uc->uc_mcontext.gregs[REG_RIP] = (greg_t)(uintptr_t)fn;
    uc->uc_mcontext.gregs[REG_EFL] &= ~(greg_t)0x400;     /* DF clear, as the ABI wants */
}

#elif defined(__aarch64__) && defined(__linux__)
/* x0..x30, sp, pc, pstate, the mask, and the FP/SIMD records of
 * uc_mcontext.__reserved: fpsimd_context always, and the sve_context when it
 * carries register data. That record supersedes the V registers on
 * sigreturn, so it has to be right in the frame the restore writes:
 *  - the fault had SVE state live (a real kernel says so only after the
 *    thread ran SVE code since its last system call; qemu-user always does):
 *    the record is carried whole, and the trampoline runs one SVE
 *    instruction (RDVL) just before the restore trap so the trap's frame has
 *    a record of the same size to receive it;
 *  - it had none, but the trampoline's own code left SVE state live (an SVE
 *    memcpy, say): the trap's record is rewritten as the kernel would see a
 *    non-SVE thread, Z = the saved V registers zero-extended, P and FFR 0.
 * Streaming SVE / ZA / ZT state, or an extra_context (state past the 4 KB
 * area), is declined. */
#ifndef SVE_MAGIC
#define SVE_MAGIC   0x53564501u
#endif
#ifndef ZA_MAGIC
#define ZA_MAGIC    0x54366345u
#endif
#ifndef ZT_MAGIC
#define ZT_MAGIC    0x5a544e01u
#endif
#ifndef EXTRA_MAGIC
#define EXTRA_MAGIC 0x45585401u
#endif
#ifndef ESR_MAGIC
#define ESR_MAGIC   0x45535201u
#endif
#define SEH_A64_CTX_HDR  16u    /* an SVE/ZA record with no register data */
#define SEH_A64_SVE_MAX  4096u  /* VL up to 896 bits (qemu's default is 512) */
#define SEH_A64_SVE_VL   8u     /* struct sve_context: u16 vl, u16 flags */
#define SEH_A64_SVE_FLAGS 10u
#define SEH_A64_SVE_SM   1u     /* SVE_SIG_FLAG_SM: streaming mode */

typedef struct {
    uint64_t regs[31], sp, pc, pstate;
    sigset_t mask;
    int      have_fp;
    struct fpsimd_context fp;
    uint32_t sve_size;          /* 0: no SVE register data at the fault */
    unsigned char sve[SEH_A64_SVE_MAX] __attribute__((aligned(16)));
} seh_hostctx;

/* The record with this magic in uc_mcontext.__reserved, or NULL. */
static struct _aarch64_ctx *a64_record(const ucontext_t *uc, uint32_t magic)
{
    unsigned char *p = (unsigned char *)uc->uc_mcontext.__reserved;
    unsigned char *end = p + sizeof(uc->uc_mcontext.__reserved);

    while (p + sizeof(struct _aarch64_ctx) <= end) {
        struct _aarch64_ctx *h = (struct _aarch64_ctx *)p;
        if (h->magic == 0 || h->size < sizeof(*h) || p + h->size > end)
            break;
        if (h->magic == magic)
            return h;
        p += h->size;
    }
    return NULL;
}

static uint16_t a64_u16(const struct _aarch64_ctx *h, unsigned off)
{
    uint16_t v;
    memcpy(&v, (const char *)h + off, sizeof v);
    return v;
}

static int ctx_save(const ucontext_t *uc, seh_hostctx *c)
{
    struct _aarch64_ctx *h;

    if (a64_record(uc, EXTRA_MAGIC) || a64_record(uc, ZT_MAGIC))
        return 0;
    if ((h = a64_record(uc, ZA_MAGIC)) && h->size > SEH_A64_CTX_HDR)
        return 0;
    c->sve_size = 0;
    if ((h = a64_record(uc, SVE_MAGIC)) && h->size > SEH_A64_CTX_HDR) {
        if (h->size > SEH_A64_SVE_MAX || (a64_u16(h, SEH_A64_SVE_FLAGS) & SEH_A64_SVE_SM))
            return 0;
        memcpy(c->sve, h, h->size);
        c->sve_size = h->size;
    }
    h = a64_record(uc, FPSIMD_MAGIC);
    c->have_fp = h && h->size >= sizeof c->fp;
    if (c->have_fp)
        memcpy(&c->fp, h, sizeof c->fp);
    memcpy(c->regs, uc->uc_mcontext.regs, sizeof c->regs);
    c->sp = uc->uc_mcontext.sp;
    c->pc = uc->uc_mcontext.pc;
    c->pstate = uc->uc_mcontext.pstate;
    c->mask = uc->uc_sigmask;
    return 1;
}

/* A live SVE record in the restore frame that the fault did not have: what a
 * thread without SVE state has, Z = V zero-extended, P = FFR = 0 (the
 * Linux layout: Z0..Z31 of VL bytes from +16, then P0..P15 and FFR of VL/8). */
static void a64_sve_from_fpsimd(struct _aarch64_ctx *h, const struct fpsimd_context *fp)
{
    uint32_t vl = a64_u16(h, SEH_A64_SVE_VL), n;
    unsigned char *z = (unsigned char *)h + SEH_A64_CTX_HDR;

    if (!vl || vl % 16 || SEH_A64_CTX_HDR + 32 * vl + 17 * (vl / 8) > h->size)
        return;
    memset(z, 0, 32 * vl + 17 * (vl / 8));
    for (n = 0; n < 32; n++)
        memcpy(z + n * vl, &fp->vregs[n], 16);
}

static void ctx_load(ucontext_t *uc, const seh_hostctx *c)
{
    struct _aarch64_ctx *h = a64_record(uc, FPSIMD_MAGIC);
    struct _aarch64_ctx *sve = a64_record(uc, SVE_MAGIC);

    memcpy(uc->uc_mcontext.regs, c->regs, sizeof c->regs);
    uc->uc_mcontext.sp = c->sp;
    uc->uc_mcontext.pc = c->pc;
    uc->uc_mcontext.pstate = c->pstate;
    uc->uc_sigmask = c->mask;
    if (c->have_fp && h && h->size >= sizeof c->fp)
        memcpy((char *)h + sizeof *h, (const char *)&c->fp + sizeof *h,
               sizeof c->fp - sizeof *h);
    if (sve && sve->size > SEH_A64_CTX_HDR) {
        if (c->sve_size == sve->size)
            memcpy(sve, c->sve, c->sve_size);
        else if (c->have_fp)
            a64_sve_from_fpsimd(sve, &c->fp);
    }
}

/* Makes the next signal frame carry SVE register data (a real kernel only
 * writes it once the thread has run SVE code since its last system call). */
static void a64_touch_sve(const seh_hostctx *c)
{
    if (c->sve_size)
        __asm__ volatile(".inst 0x04bf5020" ::: "x0");  /* rdvl x0, #1 */
}

static uintptr_t ctx_pc(const ucontext_t *uc) { return (uintptr_t)uc->uc_mcontext.pc; }
static uintptr_t ctx_sp(const ucontext_t *uc) { return (uintptr_t)uc->uc_mcontext.sp; }

static uint32_t a64_access(uint64_t esr)
{
    uint32_t ec = (uint32_t)(esr >> 26) & 0x3Fu;
    if (ec == 0x20 || ec == 0x21)
        return 8u;                                  /* instruction abort */
    if (ec == 0x24 || ec == 0x25)
        return (esr & (1u << 6)) ? 1u : 0u;         /* data abort, WnR */
    return 0u;
}

/* The kernel adds an esr_context to a data abort's frame; qemu-user does
 * not. Without it, the faulting instruction says: in the load/store class
 * (op0 x1x0) bit 22 is L for the register, immediate and pair forms. */
static uint32_t ctx_access(const ucontext_t *uc)
{
    struct _aarch64_ctx *h = a64_record(uc, ESR_MAGIC);
    uint32_t insn;

    if (h)
        return a64_access(((struct esr_context *)h)->esr);
    memcpy(&insn, (const void *)(uintptr_t)uc->uc_mcontext.pc, sizeof insn);
    if ((insn & 0x0A000000u) == 0x08000000u)
        return (insn & (1u << 22)) ? 0u : 1u;
    return 0u;
}

static void ctx_redirect(ucontext_t *uc, void (*fn)(void))
{
    /* lr = the faulting pc, so a backtrace from the trampoline names the
     * function that faulted; fp is left as it was. BTYPE cleared. */
    uc->uc_mcontext.regs[30] = uc->uc_mcontext.pc;
    uc->uc_mcontext.sp = (ctx_sp(uc) - 256) & ~(uint64_t)15;
    uc->uc_mcontext.pc = (uint64_t)(uintptr_t)fn;
    uc->uc_mcontext.pstate &= ~(uint64_t)(3u << 10);
}

#elif defined(__aarch64__) && defined(__APPLE__)
/* The whole machine context: thread, exception and NEON state. Built arm64,
 * not arm64e, so the thread state's fields are plain (mmio_decode_a64.h). */
typedef struct {
    struct __darwin_mcontext64 mc;
    sigset_t mask;
} seh_hostctx;

static int ctx_save(const ucontext_t *uc, seh_hostctx *c)
{
    c->mc = *uc->uc_mcontext;
    c->mask = uc->uc_sigmask;
    return 1;
}

static void ctx_load(ucontext_t *uc, const seh_hostctx *c)
{
    *uc->uc_mcontext = c->mc;
    uc->uc_sigmask = c->mask;
}

static uintptr_t ctx_pc(const ucontext_t *uc) { return (uintptr_t)uc->uc_mcontext->__ss.__pc; }
static uintptr_t ctx_sp(const ucontext_t *uc) { return (uintptr_t)uc->uc_mcontext->__ss.__sp; }

static uint32_t ctx_access(const ucontext_t *uc)
{
    uint64_t esr = uc->uc_mcontext->__es.__esr;
    uint32_t ec = (uint32_t)(esr >> 26) & 0x3Fu;
    if (ec == 0x20 || ec == 0x21)
        return 8u;
    if (ec == 0x24 || ec == 0x25)
        return (esr & (1u << 6)) ? 1u : 0u;
    return 0u;
}

static void ctx_redirect(ucontext_t *uc, void (*fn)(void))
{
    /* Darwin arm64 has a 128-byte red zone. */
    uc->uc_mcontext->__ss.__lr = uc->uc_mcontext->__ss.__pc;
    uc->uc_mcontext->__ss.__sp = (ctx_sp(uc) - 128 - 256) & ~(uint64_t)15;
    uc->uc_mcontext->__ss.__pc = (uint64_t)(uintptr_t)fn;
}
#endif

/* ---- per-thread state -------------------------------------------------- */

typedef struct {
    uintptr_t   pc, addr;       /* faulting host pc and address */
    uint32_t    va, access;     /* guest address; 0 read, 1 write, 8 execute */
    int         restore;        /* the raise returned: put ctx back at the trap */
    seh_hostctx ctx;
} seh_fault_level;

typedef struct {
    seh_fault_level lv[SEH_FAULT_NEST];
    uintptr_t       pass_pc, pass_addr;  /* one shot: the next fault here goes down the chain */
} seh_fault_thread;

static RECOMP_TLS seh_fault_thread *t_fault;
static void  *g_trap_page;
static size_t g_trap_size;
static int    g_faults_on;              /* decided outside the handler */

int xbox_SehFaultsEnabled(void)
{
    static int on = -1;

    if (on < 0) {
        const char *e = getenv("RECOMP_SEH_FAULTS");
        on = !(e && e[0] == '0' && e[1] == '\0') && xbox_SehDispatchEnabled();
    }
    return on;
}

void xbox_SehFaultThreadInit(void)
{
    if (!t_fault)
        t_fault = (seh_fault_thread *)calloc(1, sizeof *t_fault);  /* NULL: off here */
    g_faults_on = xbox_SehFaultsEnabled();
}

int xbox_SehFaultInit(void)
{
    if (!g_trap_page) {
        long ps = sysconf(_SC_PAGESIZE);
        void *p;
        if (ps <= 0)
            ps = 4096;
        p = mmap(NULL, (size_t)ps, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED)
            return -1;
        g_trap_size = (size_t)ps;
        g_trap_page = p;
    }
    g_faults_on = xbox_SehFaultsEnabled();
    return 0;
}

/* Where the redirected thread starts, on its own stack. Never returns: the
 * raise resumes elsewhere, or the restore trap puts the faulting context
 * back. */
__attribute__((noinline, noreturn, used))
static void seh_fault_tramp(void)
{
    seh_fault_thread *t = t_fault;
    int *depth = xbox_SehFaultDepthSlot();
    int d = *depth;
    seh_fault_level *lv = &t->lv[d - 1];
    int handled = xbox_SehRaiseHostFault(lv->va, lv->access, lv->pc);

    /* Back here: a handler continued execution, or nothing took it. Any
     * fault inside the dispatch has been put back already (depth d again). */
    if (!handled) {
        t->pass_pc = lv->pc;
        t->pass_addr = lv->addr;
    }
    *depth = d;
    lv->restore = 1;
#if defined(__aarch64__) && defined(__linux__)
    a64_touch_sve(&lv->ctx);
#endif
    (void)*(volatile char *)g_trap_page;
    abort();                                /* the handler did not restore */
}

static int on_alt_stack(uintptr_t sp)
{
    stack_t ss;

    if (sigaltstack(NULL, &ss) != 0 || (ss.ss_flags & SS_DISABLE) || !ss.ss_size)
        return 0;
    return sp - (uintptr_t)ss.ss_sp < (uintptr_t)ss.ss_size;
}

int xbox_SehPosixFault(int sig, void *siv, void *ucv)
{
    siginfo_t *si = (siginfo_t *)siv;
    ucontext_t *uc = (ucontext_t *)ucv;
    seh_fault_thread *t = t_fault;
    int *depth;
    uintptr_t addr, pc, base = (uintptr_t)g_xbox_mem_offset;
    uint64_t rel;
    uint32_t head;
    seh_fault_level *lv;

    (void)sig;
    if (!t || !si || !uc || !(depth = xbox_SehFaultDepthSlot()))
        return 0;
    addr = (uintptr_t)si->si_addr;

    /* The trampoline is done: the faulting context back, as it was. */
    if (g_trap_page && addr - (uintptr_t)g_trap_page < g_trap_size) {
        if (*depth <= 0 || *depth > SEH_FAULT_NEST || !t->lv[*depth - 1].restore)
            return 0;
        lv = &t->lv[*depth - 1];
        ctx_load(uc, &lv->ctx);
        lv->restore = 0;
        (*depth)--;
        return 1;
    }

    pc = ctx_pc(uc);
    if (t->pass_pc || t->pass_addr) {
        int again = t->pass_pc == pc && t->pass_addr == addr;
        t->pass_pc = t->pass_addr = 0;
        if (again)
            return 0;           /* nothing handled it: the original fault */
    }

    if (!g_faults_on || !g_trap_page)
        return 0;
    if (xbox_SehInRuntime())
        return 0;               /* inside a bridge: the old path, never the guest */
    rel = (uint64_t)addr - (uint64_t)base;
    if (addr < base || rel >= 0x100000000ull)
        return 0;
    if (*depth < 0 || *depth >= SEH_FAULT_NEST)
        return 0;
    if (!g_fs_base || on_alt_stack(ctx_sp(uc)))
        return 0;
    head = *(volatile uint32_t *)((uintptr_t)g_fs_base + base);
    if (head == XBOX_SEH_END_OF_CHAIN || head == 0)
        return 0;               /* no guest handler at all */

    lv = &t->lv[*depth];
    if (!ctx_save(uc, &lv->ctx))
        return 0;
    lv->pc = pc;
    lv->addr = addr;
    lv->va = (uint32_t)rel;
    lv->access = ctx_access(uc);
    lv->restore = 0;
    (*depth)++;
    ctx_redirect(uc, seh_fault_tramp);
    return 1;
}

#else   /* no implementation for this host: every fault goes down the chain */

int xbox_SehFaultsEnabled(void) { return 0; }
void xbox_SehFaultThreadInit(void) { }
int xbox_SehFaultInit(void)
{
#if defined(__APPLE__)
    static int said;
    if (!said++)
        fprintf(stderr, "  [SEH] host faults as guest exceptions: not on macOS yet\n");
#endif
    return -1;
}
int xbox_SehPosixFault(int sig, void *siginfo, void *ucontext)
{
    (void)sig; (void)siginfo; (void)ucontext;
    return 0;
}

#endif
