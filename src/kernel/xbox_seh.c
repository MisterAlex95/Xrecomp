/*
 * xbox_seh.c - guest structured exception handling (XBA-112).
 *
 * See xbox_seh.h for the model. In short: the guest's own handlers
 * (_except_handler3, __CxxFrameHandler, a title's own) are lifted code and do
 * the real work; this file is the kernel's part -- walk the registration chain
 * at fs:[0], call the handlers with the documented arguments, unwind, and turn
 * a handler's jump into the middle of a live function into a native longjmp
 * to that function's anchor.
 *
 * Everything the chain walk reads is guest stack memory a title can corrupt,
 * so every record is checked before it is followed: 4-byte aligned, on the
 * current guest stack, above the stack pointer (a record below it belongs to
 * a frame that has returned), and climbing (no cycles). A chain that breaks
 * any of those ends the walk; the exception is then unhandled, and said so.
 */
#include "xbox_seh.h"
#include "kernel_park.h"
#include "kernel.h"
#include "recomp_discovery.h"
#include "xbox_memory_layout.h"
#include "xbox_interp.h"
/* The interpreter (XBA-303) is optional here: tests/seh_dispatch's anchor
 * bench links this file alone. xbox_interp.c fills these at load
 * (xbox_interp.h « optional links »); NULL without it. */
xbox_interp_fn (*xbox_seh_interp_resolve)(uint32_t va, int kind, uint32_t ret);
uint32_t (*xbox_seh_interp_current_va)(void);

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern ptrdiff_t g_xbox_mem_offset;

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);

#define SEH_MEM32(va) (*(volatile uint32_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))

/* Frames walked per dispatch or unwind. A real chain is a few dozen deep;
 * anything past this is a cycle the climbing check somehow missed. */
#define SEH_MAX_FRAMES  256
/* Live anchors per thread: frames with an exception frame that are on the
 * stack at once. Past this the oldest is forgotten (and a resume into it is
 * refused, reported). */
#define SEH_MAX_ANCHORS 64
/* Handler calls in flight per thread (a raise inside a filter, inside ...). */
#define SEH_MAX_NEST    16
/* Without known stack bounds, how far above the raising esp a frame may be. */
#define SEH_DEFAULT_SPAN 0x00100000u
/* Guest stacks a thread enters and leaves (worker slices) at once. */
#define SEH_MAX_PUSHED  4
/* A host fault's dispatch builds its frames this far below the published
 * guest esp. At an 8- or 32-bit guest access the published registers are
 * exact (see xbox_SehRaiseHostFault); at a 16-bit, 64-bit or floating-point
 * one the compiler may still hold the latest `sub esp, n` in a host register,
 * and MSVC calls _chkstk for any frame of a page or more, so one pending
 * allocation is under 4 KB. Twice that, for two. */
#define SEH_FAULT_GAP   0x2000u
/* What the dispatch itself needs below that: record, CONTEXT, handler
 * frames. Below this much room the gap shrinks rather than leaving the
 * stack. */
#define SEH_FAULT_ROOM  0x1000u

typedef struct {
    uint32_t               reg;      /* the registration record (fs:[0] value) */
    uint32_t               ebp;      /* the function's frame pointer */
    const recomp_seh_desc *desc;
    uintptr_t              host;     /* host stack token when taken */
    int                    depth;    /* dispatch nesting when taken */
    int                    faults;   /* host-fault trampolines in flight when taken */
    uint64_t               runtime;  /* xbox_SehRuntimeSnapshot() when taken */
    jmp_buf                jb;
} seh_anchor;

typedef struct {
    uint32_t head;          /* fs:[0] when the outer handler was called */
    uint32_t established;   /* the frame whose handler it is */
} seh_outer;

typedef struct {
    seh_anchor   a[SEH_MAX_ANCHORS];
    int          n;
    uint32_t     stack_lo, stack_hi;   /* 0, 0: unknown */
    uintptr_t    host;
    int          depth;                /* handler calls in flight */
    seh_outer    outer[SEH_MAX_NEST];
    uint32_t     pending;              /* resume target the lookup matched */
    int          refused;              /* a resume in this dispatch was refused */
    int          faults;               /* host-fault trampolines in flight */
    struct { uint32_t lo, hi; } pushed[SEH_MAX_PUSHED];   /* xbox_SehPushGuestStack */
    int          npushed;
    XboxSehStats st;
} seh_thread;

static RECOMP_TLS seh_thread *t_seh;

/* xbox_seh_fault.c: the thread's host-fault state, made here so the signal
 * handler never has to allocate. */
void xbox_SehFaultThreadInit(void);

static seh_thread *seh_self(void)
{
    seh_thread *t = t_seh;

    if (!t) {
        /* Heap, not TLS: a jmp_buf is up to ~400 bytes and there are 64 of
         * them, which is more static TLS than a dlopen'd library (Android's
         * libmain.so) should ask for. Never freed: one per guest thread. */
        t = (seh_thread *)calloc(1, sizeof *t);
        if (!t) {
            fprintf(stderr, "  [SEH] out of memory for the thread's anchors\n");
            abort();
        }
        t_seh = t;
        xbox_SehFaultThreadInit();
    }
    return t;
}

/* xbox_seh.h « Runtime code on this thread ». Plain TLS: read in the fault
 * handler, before the thread may have dispatcher state. */
#define SEH_CB_NEST 16
static RECOMP_TLS int t_rt_depth;               /* bridges entered */
static RECOMP_TLS int t_rt_cb;                  /* guest callbacks from inside them */
static RECOMP_TLS int t_rt_saved[SEH_CB_NEST];

void xbox_SehRuntimeEnter(void) { t_rt_depth++; }
void xbox_SehRuntimeLeave(void) { if (t_rt_depth > 0) t_rt_depth--; }

void xbox_SehCallbackEnter(void)
{
    if (t_rt_cb < SEH_CB_NEST)
        t_rt_saved[t_rt_cb] = t_rt_depth;
    t_rt_cb++;
    t_rt_depth = 0;
}

void xbox_SehCallbackLeave(void)
{
    if (t_rt_cb <= 0)
        return;
    t_rt_cb--;
    if (t_rt_cb < SEH_CB_NEST)
        t_rt_depth = t_rt_saved[t_rt_cb];
}

int xbox_SehInRuntime(void) { return t_rt_depth > 0; }

/* Callbacks and bridges, 8 bits each (both nest a handful deep), then the
 * park detector's kernel-call and callback depths (XBA-244), which a jump
 * over bridge frames must put back the same way; in the high half the frame
 * shadow stack's count (XBA-247): the frames of the C frames jumped over are
 * gone with them. */
uint64_t xbox_SehRuntimeSnapshot(void)
{
    return ((uint64_t)recomp_frames_depth(&g_cpu) << 32)
         | (((uint32_t)t_rt_cb & 0xFFu) << 24) | (((uint32_t)t_rt_depth & 0xFFu) << 16)
         | (xbox_ParkDepthSnapshot() & 0xFFFFu);
}

void xbox_SehRuntimeRestore(uint64_t snap)
{
    uint32_t lo = (uint32_t)snap;
    t_rt_cb = (int)(lo >> 24);
    t_rt_depth = (int)((lo >> 16) & 0xFFu);
    xbox_ParkDepthRestore(lo & 0xFFFFu);
    recomp_frames_set_depth(&g_cpu, (uint32_t)(snap >> 32));
}

int *xbox_SehFaultDepthSlot(void)
{
    seh_thread *t = t_seh;
    return t ? &t->faults : NULL;
}

int xbox_SehDispatchEnabled(void)
{
    static int on = -1;

    if (on < 0) {
        const char *e = getenv("RECOMP_SEH_DISPATCH");
        on = !(e && e[0] == '0' && e[1] == '\0');
    }
    return on;
}

static int seh_log_budget(void)
{
    static int budget = 64;   /* racy on purpose: it only bounds a log */
    if (budget <= 0)
        return 0;
    budget--;
    return 1;
}

void xbox_SehSetGuestStack(uint32_t lo, uint32_t hi)
{
    seh_thread *t = seh_self();
    t->stack_lo = lo;
    t->stack_hi = hi;
}

void xbox_SehGetGuestStack(uint32_t *lo, uint32_t *hi)
{
    seh_thread *t = seh_self();
    *lo = t->stack_lo;
    *hi = t->stack_hi;
}

void xbox_SehPushGuestStack(uint32_t lo, uint32_t hi)
{
    seh_thread *t = seh_self();

    if (t->npushed < SEH_MAX_PUSHED) {
        t->pushed[t->npushed].lo = t->stack_lo;
        t->pushed[t->npushed].hi = t->stack_hi;
        t->stack_lo = lo;
        t->stack_hi = hi;
    }
    t->npushed++;           /* counted even past the limit, so pops pair */
}

void xbox_SehPopGuestStack(void)
{
    seh_thread *t = seh_self();

    if (t->npushed <= 0)
        return;
    t->npushed--;
    if (t->npushed < SEH_MAX_PUSHED) {
        t->stack_lo = t->pushed[t->npushed].lo;
        t->stack_hi = t->pushed[t->npushed].hi;
    }
}

void xbox_SehSetHostStack(uintptr_t token)
{
    seh_self()->host = token;
}

void xbox_SehStatsGet(XboxSehStats *out, int reset)
{
    seh_thread *t = seh_self();
    *out = t->st;
    if (reset)
        memset(&t->st, 0, sizeof t->st);
}

/* ---- the chain --------------------------------------------------------- */

/* Where a live record may be: on this thread's guest stack and not below
 * the stack pointer. `esp` is the lowest live address the caller knows of. */
static void seh_bounds(const seh_thread *t, uint32_t esp,
                       uint32_t *lo, uint32_t *hi)
{
    if (t->stack_hi > t->stack_lo) {
        *lo = esp > t->stack_lo ? esp : t->stack_lo;
        *hi = t->stack_hi;
    } else {
        *lo = esp;
        *hi = esp > 0xFFFFFFFFu - SEH_DEFAULT_SPAN ? 0xFFFFFFFFu
                                                   : esp + SEH_DEFAULT_SPAN;
    }
}

static int seh_on_other_stack(const seh_thread *t, uint32_t reg)
{
    return t->stack_hi > t->stack_lo
        && (reg < t->stack_lo || reg >= t->stack_hi);
}

/* 1 when reg is a record the walk may follow, after prev (0 at the head). */
static int seh_frame_ok(uint32_t reg, uint32_t prev, uint32_t lo, uint32_t hi)
{
    if (reg & 3u)
        return 0;
    if (reg < lo || hi < 8 || reg > hi - 8)
        return 0;
    if (prev && reg <= prev)
        return 0;
    return 1;
}

static void seh_report_bad_frame(const seh_thread *t, const char *what,
                                 uint32_t reg, uint32_t lo, uint32_t hi)
{
    if (!seh_log_budget())
        return;
    if (seh_on_other_stack(t, reg))
        fprintf(stderr, "  [SEH] %s: frame 0x%08X is on another guest stack "
                "(this one is 0x%08X-0x%08X); not followed\n",
                what, reg, t->stack_lo, t->stack_hi);
    else
        fprintf(stderr, "  [SEH] %s: chain broken at 0x%08X (live range "
                "0x%08X-0x%08X); not followed\n", what, reg, lo, hi);
    fflush(stderr);
}

/* 1 when reg is a record on the current chain. */
static int seh_on_chain(const seh_thread *t, uint32_t reg)
{
    uint32_t lo, hi, cur = SEH_MEM32(XBOX_FS_BASE), prev = 0;
    int guard;

    seh_bounds(t, g_esp, &lo, &hi);
    for (guard = 0; guard < SEH_MAX_FRAMES; guard++) {
        if (cur == XBOX_SEH_END_OF_CHAIN || !seh_frame_ok(cur, prev, lo, hi))
            return 0;
        if (cur == reg)
            return 1;
        prev = cur;
        cur = SEH_MEM32(cur);
    }
    return 0;
}

int xbox_SehOnChain(uint32_t reg)
{
    return seh_on_chain(seh_self(), reg);
}

static recomp_func_t seh_resolve(uint32_t va)
{
    recomp_func_t fn = recomp_lookup_manual(va);
    if (!fn) fn = recomp_lookup(va);
    if (!fn) fn = recomp_lookup_kernel(va);
    if (!fn && xbox_seh_interp_resolve)
        fn = xbox_seh_interp_resolve(va, 'r', 0);        /* a handler the analysis missed (XBA-303) */
    return fn;
}

/* EXCEPTION_DISPOSITION handler(record, frame, context, dispatcher context),
 * cdecl. The handler is the title's code on its own thread, so it holds the
 * guest CPU the raising kernel call let go of. */
static uint32_t seh_call_handler(seh_thread *t, uint32_t handler, uint32_t rec,
                                 uint32_t reg, uint32_t ctx, uint32_t dc,
                                 uint32_t head)
{
    recomp_func_t fn = handler ? seh_resolve(handler) : NULL;
    uint32_t esp0 = g_esp, disp;
    int took;

    if (!fn) {
        if (seh_log_budget()) {
            fprintf(stderr, "  [SEH] handler 0x%08X of frame 0x%08X is not "
                    "lifted; skipped\n", handler, reg);
            fflush(stderr);
        }
        return XBOX_EXCEPTION_CONTINUE_SEARCH;
    }
    if (t->depth < SEH_MAX_NEST) {
        t->outer[t->depth].head = head;
        t->outer[t->depth].established = reg;
    }
    t->depth++;

    g_esp -= 4; SEH_MEM32(g_esp) = dc;
    g_esp -= 4; SEH_MEM32(g_esp) = ctx;
    g_esp -= 4; SEH_MEM32(g_esp) = reg;
    g_esp -= 4; SEH_MEM32(g_esp) = rec;
    g_esp -= 4; SEH_MEM32(g_esp) = 0;      /* return address */
    took = xbox_GuestCpuEnterCallback();
    fn();
    xbox_GuestCpuLeaveCallback(took);
    disp = g_eax;
    g_esp = esp0;

    t->depth--;
    return disp;
}

/* RtlDispatchException. 1 when a handler continued execution. */
static int seh_dispatch(seh_thread *t, uint32_t rec, uint32_t ctx)
{
    uint32_t lo, hi, head, reg, prev = 0, nested = 0, dc;
    int guard, crossed = 0;

    /* The dispatcher context: the one dword a handler hands back (the frame
     * a nested exception or a collided unwind refers to). */
    g_esp -= 16;
    dc = g_esp;
    SEH_MEM32(dc) = 0;

    t->st.raises++;
    t->refused = 0;
    seh_bounds(t, g_esp, &lo, &hi);
    head = reg = SEH_MEM32(XBOX_FS_BASE);

    for (guard = 0; guard < SEH_MAX_FRAMES; guard++) {
        uint32_t disp, flags;

        if (reg == XBOX_SEH_END_OF_CHAIN)
            break;
        if (!seh_frame_ok(reg, prev, lo, hi)) {
            seh_report_bad_frame(t, "dispatch", reg, lo, hi);
            SEH_MEM32(rec + XBOX_EXCREC_FLAGS) |= XBOX_EXCEPTION_STACK_INVALID;
            if (seh_on_other_stack(t, reg))
                t->st.refused++;
            break;
        }

        /* A raise inside a handler: once the walk reaches the frames that
         * were already on the chain when that handler was called, it is
         * walking the outer dispatch's frames again, as NT does through
         * RtlpExecuteHandlerForException's own frame. */
        if (!crossed && t->depth > 0 && t->depth <= SEH_MAX_NEST
                && reg == t->outer[t->depth - 1].head) {
            crossed = 1;
            SEH_MEM32(rec + XBOX_EXCREC_FLAGS) |= XBOX_EXCEPTION_NESTED_CALL;
            if (t->outer[t->depth - 1].established > nested)
                nested = t->outer[t->depth - 1].established;
        }

        disp = seh_call_handler(t, SEH_MEM32(reg + 4), rec, reg, ctx, dc, head);
        flags = SEH_MEM32(rec + XBOX_EXCREC_FLAGS);

        if (t->refused) {
            /* The handler tried to resume in a frame it may not reach
             * (reported where it was refused). Whatever it did to the chain
             * on the way stays done; the raise is unhandled. */
            break;
        }
        if (nested == reg) {
            SEH_MEM32(rec + XBOX_EXCREC_FLAGS) = flags & ~XBOX_EXCEPTION_NESTED_CALL;
            nested = 0;
        }

        if (disp == XBOX_EXCEPTION_CONTINUE_EXECUTION) {
            if (flags & XBOX_EXCEPTION_NONCONTINUABLE) {
                if (seh_log_budget()) {
                    fprintf(stderr, "  [SEH] handler of frame 0x%08X continued "
                            "a noncontinuable exception 0x%08X; unhandled\n",
                            reg, SEH_MEM32(rec + XBOX_EXCREC_CODE));
                    fflush(stderr);
                }
                break;
            }
            g_esp = dc + 16;
            t->st.handled++;
            return 1;
        }
        if (disp == XBOX_EXCEPTION_NESTED_EXCEPTION) {
            uint32_t est = SEH_MEM32(dc);
            SEH_MEM32(rec + XBOX_EXCREC_FLAGS) |= XBOX_EXCEPTION_NESTED_CALL;
            if (est > nested)
                nested = est;
        } else if (disp != XBOX_EXCEPTION_CONTINUE_SEARCH) {
            if (seh_log_budget()) {
                fprintf(stderr, "  [SEH] handler of frame 0x%08X returned "
                        "invalid disposition %u; unhandled\n", reg, disp);
                fflush(stderr);
            }
            break;
        }
        prev = reg;
        reg = SEH_MEM32(reg);
    }

    g_esp = dc + 16;
    t->st.unhandled++;
    return 0;
}

/* ---- raising ----------------------------------------------------------- */

static void seh_capture(uint32_t ctx, uint32_t eip, uint32_t esp)
{
    memset((void *)((uintptr_t)ctx + g_xbox_mem_offset), 0, XBOX_CTX_SIZE);
    SEH_MEM32(ctx + XBOX_CTX_FLAGS)  = XBOX_CONTEXT_FULL;
    SEH_MEM32(ctx + XBOX_CTX_EDI)    = g_edi;
    SEH_MEM32(ctx + XBOX_CTX_ESI)    = g_esi;
    SEH_MEM32(ctx + XBOX_CTX_EBX)    = g_ebx;
    SEH_MEM32(ctx + XBOX_CTX_EDX)    = g_edx;
    SEH_MEM32(ctx + XBOX_CTX_ECX)    = g_ecx;
    SEH_MEM32(ctx + XBOX_CTX_EAX)    = g_eax;
    SEH_MEM32(ctx + XBOX_CTX_EBP)    = g_ebp;
    SEH_MEM32(ctx + XBOX_CTX_EIP)    = eip;
    SEH_MEM32(ctx + XBOX_CTX_SEGCS)  = 0x08;
    SEH_MEM32(ctx + XBOX_CTX_EFLAGS) = g_eflags;
    SEH_MEM32(ctx + XBOX_CTX_ESP)    = esp;
    SEH_MEM32(ctx + XBOX_CTX_SEGSS)  = 0x10;
}

/* ExceptionContinueExecution: NtContinue(context). The integer registers
 * come back; a context that moves Eip or Esp cannot be honoured by lifted
 * code (there is no "continue at an arbitrary address"), so that is said. */
static void seh_restore(uint32_t ctx, uint32_t eip, uint32_t esp)
{
    g_edi    = SEH_MEM32(ctx + XBOX_CTX_EDI);
    g_esi    = SEH_MEM32(ctx + XBOX_CTX_ESI);
    g_ebx    = SEH_MEM32(ctx + XBOX_CTX_EBX);
    g_edx    = SEH_MEM32(ctx + XBOX_CTX_EDX);
    g_ecx    = SEH_MEM32(ctx + XBOX_CTX_ECX);
    g_eax    = SEH_MEM32(ctx + XBOX_CTX_EAX);
    g_eflags = SEH_MEM32(ctx + XBOX_CTX_EFLAGS);
    if ((SEH_MEM32(ctx + XBOX_CTX_EIP) != eip
            || SEH_MEM32(ctx + XBOX_CTX_ESP) != esp) && seh_log_budget()) {
        fprintf(stderr, "  [SEH] continue-execution moved Eip 0x%08X->0x%08X "
                "Esp 0x%08X->0x%08X; lifted code continues at 0x%08X\n",
                eip, SEH_MEM32(ctx + XBOX_CTX_EIP), esp,
                SEH_MEM32(ctx + XBOX_CTX_ESP), eip);
        fflush(stderr);
    }
}

typedef struct {
    uint32_t eax, ecx, edx, ebx, esi, edi, ebp, seh_ebp, eflags;
} seh_regs;

static void seh_regs_save(seh_regs *r)
{
    r->eax = g_eax; r->ecx = g_ecx; r->edx = g_edx; r->ebx = g_ebx;
    r->esi = g_esi; r->edi = g_edi; r->ebp = g_ebp; r->seh_ebp = g_seh_ebp;
    r->eflags = g_eflags;
}

static void seh_regs_load(const seh_regs *r)
{
    g_eax = r->eax; g_ecx = r->ecx; g_edx = r->edx; g_ebx = r->ebx;
    g_esi = r->esi; g_edi = r->edi; g_ebp = r->ebp; g_seh_ebp = r->seh_ebp;
    g_eflags = r->eflags;
}

/* quiet: the caller reports an unhandled exception itself (a host fault,
 * which does not go on after the raise). */
static int seh_raise(uint32_t rec, uint32_t eip, uint32_t esp_after, int quiet)
{
    seh_thread *t = seh_self();
    uint32_t esp0 = g_esp, ctx, code;
    seh_regs saved;
    int handled;

    seh_regs_save(&saved);
    code = SEH_MEM32(rec + XBOX_EXCREC_CODE);

    /* Below everything live, 16-byte aligned, with room to spare. */
    g_esp = (g_esp - XBOX_CTX_SIZE - 0x40) & ~0xFu;
    ctx = g_esp;
    seh_capture(ctx, eip, esp_after);

    handled = seh_dispatch(t, rec, ctx);

    if (handled) {
        seh_restore(ctx, eip, esp_after);
        g_ebp = saved.ebp;
        g_seh_ebp = saved.seh_ebp;
    } else {
        seh_regs_load(&saved);
        if (!quiet)
            recomp_discovery_exception(SEH_MEM32(rec + XBOX_EXCREC_ADDRESS), code, "no guest handler took it");
        if (!quiet && seh_log_budget()) {
            fprintf(stderr, "  [SEH] unhandled exception 0x%08X at 0x%08X "
                    "(record 0x%08X); execution continues after the raise\n",
                    code, SEH_MEM32(rec + XBOX_EXCREC_ADDRESS), rec);
            fflush(stderr);
        }
    }
    g_esp = esp0;
    return handled;
}

int xbox_SehRaiseRecord(uint32_t record_va, uint32_t eip, uint32_t esp_after)
{
    if (!record_va)
        return 0;
    /* RtlRaiseException stores the caller's Eip as the exception address. */
    SEH_MEM32(record_va + XBOX_EXCREC_ADDRESS) = eip;
    return seh_raise(record_va, eip, esp_after, 0);
}

/* A new record below g_esp, then dispatch. ctx_esp is the Esp the CONTEXT
 * reports (the guest esp at the fault). */
static int seh_raise_new(uint32_t code, uint32_t flags, uint32_t address,
                         uint32_t nparams, const uint32_t *params,
                         uint32_t ctx_esp, int quiet)
{
    uint32_t esp0 = g_esp, rec, i;
    int handled;

    if (nparams > XBOX_EXCREC_MAXPARAMS)
        nparams = XBOX_EXCREC_MAXPARAMS;

    g_esp = (g_esp - XBOX_EXCREC_SIZE - 0x10) & ~0xFu;
    rec = g_esp;
    memset((void *)((uintptr_t)rec + g_xbox_mem_offset), 0, XBOX_EXCREC_SIZE);
    SEH_MEM32(rec + XBOX_EXCREC_CODE)    = code;
    SEH_MEM32(rec + XBOX_EXCREC_FLAGS)   = flags;
    SEH_MEM32(rec + XBOX_EXCREC_ADDRESS) = address;
    SEH_MEM32(rec + XBOX_EXCREC_NPARAMS) = nparams;
    for (i = 0; i < nparams; i++)
        SEH_MEM32(rec + XBOX_EXCREC_PARAMS + 4 * i) = params ? params[i] : 0;

    handled = seh_raise(rec, address, ctx_esp, quiet);
    g_esp = esp0;
    return handled;
}

int xbox_RaiseGuestException(uint32_t code, uint32_t flags, uint32_t address,
                             uint32_t nparams, const uint32_t *params)
{
    if (!xbox_SehDispatchEnabled())
        return 0;
    return seh_raise_new(code, flags, address, nparams, params, g_esp, 0);
}

/* Where a host fault's dispatch may build its frames: SEH_FAULT_GAP below
 * the published esp, shrunk when the thread's guest stack has less room. */
static uint32_t seh_fault_build_esp(const seh_thread *t, uint32_t esp)
{
    uint32_t gap = SEH_FAULT_GAP;

    if (t->stack_hi > t->stack_lo && esp > t->stack_lo && esp <= t->stack_hi) {
        uint32_t room = esp - t->stack_lo;
        if (room < gap + SEH_FAULT_ROOM)
            gap = room > SEH_FAULT_ROOM ? room - SEH_FAULT_ROOM : 0;
    }
    return (esp - gap) & ~0xFu;
}

/*
 * The registers a handler sees. Lifted code keeps the guest registers in
 * g_cpu (thread-local memory) and ebp in a C local, and the C compiler is
 * free to keep any of them in host registers between two accesses to that
 * memory. What is in g_cpu at a faulting guest access:
 *
 *  - every guest access may alias the registers: MEM8/MEM32 by the C
 *    aliasing rules, MEM16/SMEM16/SMEM64/MEMF/MEMD through may_alias types
 *    (recomp_types.h, XBA-138; before, gcc and clang sank an `esp -= 8` past
 *    a MEM16 load). So at a faulting LOAD every earlier write to eax..edi,
 *    esp, ebp (the published frame) and seh_ebp is in memory: exact;
 *  - at a faulting STORE, of any width, a register written before it and
 *    overwritten after it without being read in between may still hold its
 *    older value: the compiler drops the first write as dead, since in C a
 *    store cannot trap. Registers the code reads after the store (esp
 *    across a push, for one) are exact. esp can be too high by a dropped
 *    allocation (`sub esp; store; mov esp, ebp`), which SEH_FAULT_GAP
 *    covers. tests/seh_fault_regs pins both;
 *  - EFlags is the last image published (the arithmetic flags are lazy C
 *    locals), and Eip is 0, as is the record's ExceptionAddress: lifted code
 *    keeps no guest pc.
 *
 * None of that matters to the common handlers: _except_handler3 and
 * __CxxFrameHandler take the frame from the registration record, the
 * try level from the frame, and resume through the anchor; the faulting
 * address is in the record. A handler that continues execution gets the
 * host context back as it was, so the lifted code goes on with its own
 * registers, cached or not: that is why the CONTEXT's registers are not
 * applied, and g_cpu is put back as captured.
 */
int xbox_SehRaiseHostFault(uint32_t va, uint32_t access, uintptr_t host_pc)
{
    seh_thread *t = seh_self();
    RecompCpuRegs saved;            /* the registers; the frames above stay as they are */
    uint32_t params[2], esp = g_esp;
    int handled;

    memcpy(&saved, (const void *)&g_cpu, sizeof saved);
    params[0] = access;
    params[1] = va;
    t->st.host_faults++;
    if (seh_log_budget()) {
        fprintf(stderr, "  [SEH] host fault on guest 0x%08X (%s, host pc %p): "
                "raised to the guest's handlers as STATUS_ACCESS_VIOLATION\n",
                va, access == 1 ? "write" : access == 8 ? "execute" : "read",
                (void *)host_pc);
        fflush(stderr);
    }

    g_esp = seh_fault_build_esp(t, esp);
    /* Interpreted code (XBA-303) knows its pc: the record's address and the
     * context's Eip are exact there (0 in lifted code, which keeps none). */
    handled = seh_raise_new(XBOX_STATUS_ACCESS_VIOLATION, 0,
                            xbox_seh_interp_current_va ? xbox_seh_interp_current_va() : 0u, 2, params,
                            esp, 1);
    if (handled) {
        if ((g_eax != saved.r_eax || g_ecx != saved.r_ecx || g_edx != saved.r_edx
                || g_ebx != saved.r_ebx || g_esi != saved.r_esi
                || g_edi != saved.r_edi) && seh_log_budget()) {
            fprintf(stderr, "  [SEH] the handler of the fault on guest 0x%08X "
                    "changed registers in the CONTEXT; not applied, the access "
                    "runs again with the registers it had\n", va);
            fflush(stderr);
        }
    } else {
        /* The same discovery kind as an unhandled raise (XBA-129): the
         * blocker is named the same way, here before the crash it becomes. */
        recomp_discovery_exception(va, XBOX_STATUS_ACCESS_VIOLATION,
                                   "host fault on a guest address no guest handler took");
        if (seh_log_budget()) {
            fprintf(stderr, "  [SEH] host fault on guest 0x%08X: no guest handler "
                    "took it; it goes to the host's handler\n", va);
            fflush(stderr);
        }
    }
    memcpy((void *)&g_cpu, &saved, sizeof saved);
    return handled;
}

/* ---- unwinding --------------------------------------------------------- */

void xbox_SehUnwind(uint32_t target_frame, uint32_t exc_record, uint32_t eip)
{
    seh_thread *t = seh_self();
    uint32_t esp0 = g_esp, reg, prev = 0, lo, hi, ctx, dc, head;
    int guard;

    /* An unwind with no record of its own still has to tell the handlers it
     * is an unwind, so synthesise one below the stack pointer. */
    g_esp = (g_esp - XBOX_EXCREC_SIZE - XBOX_CTX_SIZE - 0x40) & ~0xFu;
    ctx = g_esp;
    dc = ctx + XBOX_CTX_SIZE;
    SEH_MEM32(dc) = 0;
    seh_capture(ctx, eip, esp0);
    if (!exc_record) {
        exc_record = dc + 0x10;
        memset((void *)((uintptr_t)exc_record + g_xbox_mem_offset), 0,
               XBOX_EXCREC_SIZE);
        SEH_MEM32(exc_record + XBOX_EXCREC_CODE) = XBOX_STATUS_UNWIND;
    }
    SEH_MEM32(exc_record + XBOX_EXCREC_FLAGS) |= XBOX_EXCEPTION_UNWINDING
        | (target_frame ? 0u : XBOX_EXCEPTION_EXIT_UNWIND);

    seh_bounds(t, g_esp, &lo, &hi);
    head = reg = SEH_MEM32(XBOX_FS_BASE);
    for (guard = 0; guard < SEH_MAX_FRAMES; guard++) {
        uint32_t next, handler, disp;

        if (reg == XBOX_SEH_END_OF_CHAIN || reg == target_frame)
            break;
        if (target_frame && target_frame < reg) {
            if (seh_log_budget()) {
                fprintf(stderr, "  [SEH] unwind target 0x%08X is below frame "
                        "0x%08X: not on the chain; stopped\n", target_frame, reg);
                fflush(stderr);
            }
            break;
        }
        if (!seh_frame_ok(reg, prev, lo, hi)) {
            seh_report_bad_frame(t, "unwind", reg, lo, hi);
            break;
        }

        next    = SEH_MEM32(reg);
        handler = SEH_MEM32(reg + 4);

        /* Pop before calling. The handler may raise, and it must not see
         * its own frame still on the chain. */
        SEH_MEM32(XBOX_FS_BASE) = next;

        disp = seh_call_handler(t, handler, exc_record, reg, ctx, dc, head);
        if (disp == XBOX_EXCEPTION_COLLIDED_UNWIND_D) {
            /* This unwind ran into another one in progress; carry on from
             * the frame that one had reached. */
            uint32_t est = SEH_MEM32(dc);
            if (est && seh_frame_ok(est, 0, lo, hi) && est > reg)
                next = SEH_MEM32(est);
        }
        prev = reg;
        reg = next;
    }

    if (reg == XBOX_SEH_END_OF_CHAIN || reg == target_frame)
        SEH_MEM32(XBOX_FS_BASE) = reg;
    g_esp = esp0;
}

/* ---- longjmp ----------------------------------------------------------- */

#define XBOX_JMPBUF_EBP          0x00u
#define XBOX_JMPBUF_EIP          0x14u
#define XBOX_JMPBUF_REGISTRATION 0x18u
#define XBOX_JMPBUF_TRYLEVEL     0x1Cu
#define XBOX_JMPBUF_COOKIE       0x20u
#define XBOX_JMPBUF_UNWINDFUNC   0x24u
#define XBOX_JMPBUF_VC20         0x56433230u   /* 'VC20': a _setjmp3 buffer */

/* Call guest code at va with nargs dword arguments (args[0] nearest the
 * return address), handing it ebp the way a lifted callee inherits a frame
 * (g_ebp, and g_seh_ebp for a funclet). 0 when va is not lifted. */
static int seh_call_guest(uint32_t va, const uint32_t *args, int nargs,
                          uint32_t ebp)
{
    recomp_func_t fn = va ? seh_resolve(va) : NULL;
    uint32_t esp0 = g_esp, ebp0 = g_ebp, seh_ebp0 = g_seh_ebp;
    int took, i;

    if (!fn)
        return 0;
    for (i = nargs - 1; i >= 0; i--) {
        g_esp -= 4;
        SEH_MEM32(g_esp) = args[i];
    }
    g_esp -= 4; SEH_MEM32(g_esp) = 0;      /* return address */
    g_ebp = g_seh_ebp = ebp;
    took = xbox_GuestCpuEnterCallback();
    fn();
    xbox_GuestCpuLeaveCallback(took);
    g_esp = esp0;
    g_ebp = ebp0;
    g_seh_ebp = seh_ebp0;
    return 1;
}

/* The CRT's _local_unwind2(reg, stop) for an EH3 frame: run the __finally
 * blocks of the try levels above `stop`, innermost first, each with the
 * frame's ebp (registration + 0x10). The level is lowered before its block
 * runs, as the CRT does, so a raise inside the block does not run it again. */
static void seh_local_unwind2(uint32_t reg, uint32_t stop)
{
    uint32_t scope = SEH_MEM32(reg + 8);
    int guard;

    for (guard = 0; guard < SEH_MAX_FRAMES; guard++) {
        uint32_t tl = SEH_MEM32(reg + 0xC), ent, handler;

        if (tl == 0xFFFFFFFFu || tl == stop || tl > 0xFFFFu)
            break;
        ent = scope + tl * 12u;
        SEH_MEM32(reg + 0xC) = SEH_MEM32(ent);           /* enclosing level */
        if (SEH_MEM32(ent + 4))
            continue;                                    /* an __except: nothing to run */
        handler = SEH_MEM32(ent + 8);
        if (!seh_call_guest(handler, NULL, 0, reg + 0x10) && seh_log_budget()) {
            fprintf(stderr, "  [SEH] longjmp: __finally 0x%08X of frame 0x%08X "
                    "is not lifted; skipped\n", handler, reg);
            fflush(stderr);
        }
    }
}

void xbox_SehLongjmpUnwind(uint32_t buf)
{
    uint32_t reg, head;

    if (!xbox_SehDispatchEnabled() || !g_fs_base)
        return;
    reg  = SEH_MEM32(buf + XBOX_JMPBUF_REGISTRATION);
    head = SEH_MEM32(XBOX_FS_BASE);

    /* Global: the frames between here and the setjmp caller's. The
     * registration must be on the chain (or its end), else nothing is
     * unwound: a buffer whose frame has gone says nothing reliable. */
    if (reg != head) {
        if (reg != XBOX_SEH_END_OF_CHAIN && !xbox_SehOnChain(reg))
            return;
        xbox_SehUnwind(reg == XBOX_SEH_END_OF_CHAIN ? 0 : reg, 0,
                       SEH_MEM32(buf + XBOX_JMPBUF_EIP));
        SEH_MEM32(XBOX_FS_BASE) = reg;
    }
    if (reg == 0 || reg == XBOX_SEH_END_OF_CHAIN)
        return;

    /* Local: the try levels the setjmp caller entered after its setjmp. */
    if (SEH_MEM32(buf + XBOX_JMPBUF_COOKIE) == XBOX_JMPBUF_VC20) {
        uint32_t fn = SEH_MEM32(buf + XBOX_JMPBUF_UNWINDFUNC);
        if (fn && !seh_call_guest(fn, &buf, 1, SEH_MEM32(buf + XBOX_JMPBUF_EBP))
                && seh_log_budget()) {
            fprintf(stderr, "  [SEH] longjmp: the jmp_buf's unwind function "
                    "0x%08X is not lifted; the setjmp caller's try levels are "
                    "not unwound\n", fn);
            fflush(stderr);
        }
        return;
    }
    seh_local_unwind2(reg, SEH_MEM32(buf + XBOX_JMPBUF_TRYLEVEL));
}

/* ---- anchors and resume ------------------------------------------------ */

jmp_buf *recomp_seh_anchor(uint32_t reg, uint32_t ebp, const recomp_seh_desc *d)
{
    seh_thread *t = seh_self();
    int n = t->n;
    seh_anchor *a;

    /* An anchor at or below this frame, on this host stack, belongs to a
     * frame that has returned (we are now shallower), or is this frame
     * being registered again. Frames on another host stack say nothing
     * about this one. */
    while (n > 0 && t->a[n - 1].host == t->host && t->a[n - 1].reg <= reg)
        n--;
    if (n == SEH_MAX_ANCHORS) {
        memmove(&t->a[0], &t->a[1], sizeof t->a[0] * (SEH_MAX_ANCHORS - 1));
        n--;
    }
    a = &t->a[n];
    a->reg   = reg;
    a->ebp   = ebp;
    a->desc  = d;
    a->host  = t->host;
    a->depth = t->depth;
    a->faults = t->faults;
    a->runtime = xbox_SehRuntimeSnapshot();
    t->n = n + 1;
    t->st.anchors++;
    return &a->jb;
}

/* The interpreter's resume points (XBA-303): what an anchor keeps, for a jump
 * back into interpreted frames (xbox_InterpResumeAt), which is a resume over the
 * same runtime frames. */
void xbox_SehPointTake(XboxSehPoint *p)
{
    seh_thread *t = seh_self();
    p->runtime = xbox_SehRuntimeSnapshot();
    p->host = t->host;
    p->anchors = t->n;
    p->depth = t->depth;
    p->faults = t->faults;
}

static void seh_refuse(seh_thread *t, const char *why, uint32_t va, const seh_anchor *a);

int xbox_SehPointResume(const XboxSehPoint *p, uint32_t va, uint32_t func)
{
    seh_thread *t = seh_self();
    if (p->host != t->host) {
        /* as for an anchor: never a longjmp across host stacks */
        seh_refuse(t, "its interpreted frame is on another host stack", va, NULL);
        recomp_discovery_stack_switch(va, func, "an exception handler resumes into an interpreted frame on "
                                      "another host stack (SEH, XBA-303)");
        return 0;
    }
    if (p->anchors < t->n)
        t->n = p->anchors;          /* anchors taken under the frames the jump leaves */
    t->depth = p->depth;
    t->faults = p->faults;
    xbox_SehRuntimeRestore(p->runtime);
    t->refused = 0;
    t->st.resumes++;
    t->st.handled++;
    xbox_GuestCpuAcquire();         /* guest code runs holding the guest CPU */
    return 1;
}

void xbox_SehPointRestore(const XboxSehPoint *p)
{
    seh_thread *t = seh_self();
    if (p->anchors < t->n)
        t->n = p->anchors;
    t->depth = p->depth;
    t->faults = p->faults;
    xbox_SehRuntimeRestore(p->runtime);
    t->refused = 0;
}

static int seh_desc_index(const recomp_seh_desc *d, uint32_t va)
{
    uint32_t i;

    if (!d)
        return -1;
    for (i = 0; i < d->n_resume; i++)
        if (d->resume[i] == va)
            return (int)i;
    return -1;
}

static void seh_refuse(seh_thread *t, const char *why, uint32_t va,
                       const seh_anchor *a)
{
    t->refused = 1;
    t->st.refused++;
    if (seh_log_budget()) {
        if (a)
            fprintf(stderr, "  [SEH] resume at 0x%08X in 0x%08X refused: %s "
                    "(frame 0x%08X, ebp 0x%08X); the exception is unhandled\n",
                    va, a->desc ? a->desc->func_va : 0, why, a->reg, a->ebp);
        else
            fprintf(stderr, "  [SEH] resume at 0x%08X refused: %s; the "
                    "exception is unhandled\n", va, why);
        fflush(stderr);
    }
}

/* What a handler's jump to a resume target lands in. The function the target
 * belongs to took an anchor when it registered its frame; longjmp there and
 * it goes to the target's label, with the C frames in between gone for real.
 *
 * The anchor must be on the host stack the thread is on now, and its frame
 * must still be on the chain. Otherwise the jump is refused and reported:
 * jumping to another host stack's jmp_buf is undefined behaviour, and a
 * frame no longer registered has returned. */
static void seh_resume_thunk(void)
{
    seh_thread *t = seh_self();
    uint32_t va = t->pending;
    int i, best = -1, idx = -1;

    t->pending = 0;
    for (i = t->n - 1; i >= 0; i--) {
        int k = seh_desc_index(t->a[i].desc, va);
        if (k < 0)
            continue;
        if (t->a[i].ebp == g_seh_ebp) {
            best = i; idx = k;
            break;
        }
        if (best < 0) {
            best = i; idx = k;
        }
    }
    if (best < 0) {
        seh_refuse(t, "no live anchor", va, NULL);
        return;
    }
    if (t->a[best].host != t->host) {
        seh_refuse(t, "its anchor is on another host stack", va, &t->a[best]);
        /* A stack switch the runtime cannot carry (XBA-101's blocker kind):
         * the compat report and the app name it. */
        recomp_discovery_stack_switch(va, t->a[best].desc ? t->a[best].desc->func_va : 0,
                                      "an exception handler resumes into a frame on another "
                                      "host stack (SEH, XBA-112)");
        return;
    }
    if (seh_on_other_stack(t, t->a[best].reg)) {
        seh_refuse(t, "its frame is on another guest stack", va, &t->a[best]);
        return;
    }
    if (!seh_on_chain(t, t->a[best].reg)) {
        seh_refuse(t, "its frame is no longer registered", va, &t->a[best]);
        return;
    }

    /* The handler set ebp for the target; the resumed function reads it back
     * from g_seh_ebp, as after any call. Everything inside it is gone: inner
     * anchors and the dispatches this jump abandons. */
    t->n = best + 1;
    t->depth = t->a[best].depth;
    /* The host-fault trampolines below the anchor go with the C frames. */
    t->faults = t->a[best].faults;
    xbox_SehRuntimeRestore(t->a[best].runtime);
    t->refused = 0;
    t->st.resumes++;
    t->st.handled++;
    xbox_GuestCpuAcquire();   /* guest code runs holding the guest CPU */
#if defined(_WIN32)
    longjmp(t->a[best].jb, idx + 1);
#else
    _longjmp(t->a[best].jb, idx + 1);
#endif
}

xbox_seh_func_t xbox_SehLookup(uint32_t va)
{
    seh_thread *t = t_seh;
    int i;

    if (!t || !t->n)
        return NULL;
    for (i = t->n - 1; i >= 0; i--) {
        if (seh_desc_index(t->a[i].desc, va) >= 0) {
            t->pending = va;
            return seh_resume_thunk;
        }
    }
    return NULL;
}
