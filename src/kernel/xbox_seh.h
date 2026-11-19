/*
 * xbox_seh.h - guest structured exception handling (XBA-112).
 *
 * The title's exception machinery is guest code: __SEH_prolog, the scope
 * tables, _except_handler3, __CxxFrameHandler and the funclets they call are
 * all lifted. What the console's kernel adds is small, and is what lives here:
 *
 *  - dispatch: RtlRaiseException / ExRaiseException / ExRaiseStatus, and any
 *    fault the runtime turns into a guest exception, walk the registration
 *    chain at fs:[0] and call each frame's handler with the documented
 *    (record, frame, context, dispatcher context) arguments, honouring
 *    ExceptionContinueExecution, ExceptionContinueSearch and
 *    ExceptionNestedException;
 *  - unwind: RtlUnwind pops the frames above a target frame, calling each
 *    handler with EXCEPTION_UNWINDING so __finally blocks and destructors run;
 *  - resume: a handler that decides to resume jumps into the middle of a live
 *    function (an __except block, a catch continuation). A lifted function is
 *    a C function, so that jump has to leave the C frames in between for real.
 *    Each function with an exception frame takes a native setjmp "anchor"
 *    right after it registers the frame (code emitted by tools/recomp), and a
 *    jump to one of its resume targets becomes a longjmp to that anchor plus
 *    a goto to the target's label.
 *
 * RECOMP_SEH_DISPATCH=0 restores the old behaviour: a raise is logged and
 * returns, nothing is dispatched.
 *
 * Guest stacks and host stacks. A guest stack switch (a fiber, a coroutine,
 * XBA-101) gives each guest stack its own host stack on the same thread, and
 * a longjmp must never cross host stacks. The dispatcher therefore only walks
 * frames that lie on the current guest stack (xbox_SehSetGuestStack), and a
 * resume only lands on an anchor taken on the current host stack
 * (xbox_SehSetHostStack). A frame or an anchor on another stack is refused and
 * reported, and the exception is treated as unhandled rather than jumped to.
 */
#ifndef XBOX_SEH_H
#define XBOX_SEH_H

#include <stdint.h>
#include <setjmp.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Status codes and flags (ntstatus.h / winnt.h values). */
#define XBOX_STATUS_ACCESS_VIOLATION        0xC0000005u
#define XBOX_STATUS_INTEGER_DIVIDE_BY_ZERO  0xC0000094u
#define XBOX_STATUS_INTEGER_OVERFLOW        0xC0000095u
#define XBOX_STATUS_NONCONTINUABLE          0xC0000025u
#define XBOX_STATUS_INVALID_DISPOSITION     0xC0000026u
#define XBOX_STATUS_UNWIND                  0xC0000027u

#define XBOX_EXCEPTION_NONCONTINUABLE  0x01u
#define XBOX_EXCEPTION_UNWINDING       0x02u
#define XBOX_EXCEPTION_EXIT_UNWIND     0x04u
#define XBOX_EXCEPTION_STACK_INVALID   0x08u
#define XBOX_EXCEPTION_NESTED_CALL     0x10u
#define XBOX_EXCEPTION_TARGET_UNWIND   0x20u
#define XBOX_EXCEPTION_COLLIDED_UNWIND 0x40u

/* EXCEPTION_DISPOSITION */
#define XBOX_EXCEPTION_CONTINUE_EXECUTION 0u
#define XBOX_EXCEPTION_CONTINUE_SEARCH    1u
#define XBOX_EXCEPTION_NESTED_EXCEPTION   2u
#define XBOX_EXCEPTION_COLLIDED_UNWIND_D  3u

/* EXCEPTION_RECORD, as the guest sees it (x86, 0x50 bytes). */
#define XBOX_EXCREC_CODE      0x00u
#define XBOX_EXCREC_FLAGS     0x04u
#define XBOX_EXCREC_RECORD    0x08u
#define XBOX_EXCREC_ADDRESS   0x0Cu
#define XBOX_EXCREC_NPARAMS   0x10u
#define XBOX_EXCREC_PARAMS    0x14u
#define XBOX_EXCREC_MAXPARAMS 15u
#define XBOX_EXCREC_SIZE      0x50u

/* The Xbox CONTEXT (xboxkrnl.h): ContextFlags, a 0x204-byte
 * FLOATING_SAVE_AREA (fxsave layout plus Cr0NpxState), then the integer and
 * control registers. It is not the Win32 x86 CONTEXT: there are no debug or
 * segment registers, and the float area is where Win32 has Dr0..Dr7. */
#define XBOX_CTX_FLAGS   0x000u
#define XBOX_CTX_FLOAT   0x004u
#define XBOX_CTX_EDI     0x208u
#define XBOX_CTX_ESI     0x20Cu
#define XBOX_CTX_EBX     0x210u
#define XBOX_CTX_EDX     0x214u
#define XBOX_CTX_ECX     0x218u
#define XBOX_CTX_EAX     0x21Cu
#define XBOX_CTX_EBP     0x220u
#define XBOX_CTX_EIP     0x224u
#define XBOX_CTX_SEGCS   0x228u
#define XBOX_CTX_EFLAGS  0x22Cu
#define XBOX_CTX_ESP     0x230u
#define XBOX_CTX_SEGSS   0x234u
#define XBOX_CTX_SIZE    0x238u
#define XBOX_CONTEXT_FULL 0x00010007u   /* i386 | CONTROL | INTEGER | SEGMENTS */

#define XBOX_SEH_END_OF_CHAIN 0xFFFFFFFFu

/* 1 unless RECOMP_SEH_DISPATCH=0. Read once. */
int xbox_SehDispatchEnabled(void);

/* Raise a guest exception from the current guest state, as RtlRaiseException
 * would from guest code: build an EXCEPTION_RECORD and a CONTEXT below the
 * guest stack pointer and dispatch. `address` is the faulting guest EIP; it
 * is also the context's Eip. params may be NULL when nparams is 0.
 *
 * The entry point for runtime-detected faults: XBA-110's divide helper, the
 * host fault handler (stage 4). Returns 1 when a handler continued execution
 * (the registers it left in the CONTEXT are back in g_cpu), 0 when nothing
 * handled it (logged). A handler that resumes at an __except block or a catch
 * continuation does not return here at all. */
int xbox_RaiseGuestException(uint32_t code, uint32_t flags, uint32_t address,
                             uint32_t nparams, const uint32_t *params);

/* RtlRaiseException / ExRaiseException with a record the guest built.
 * eip is the call's return address and esp_after the guest esp once the call
 * has returned (arguments popped): the context a real raise captures.
 * Returns as xbox_RaiseGuestException does. */
int xbox_SehRaiseRecord(uint32_t record_va, uint32_t eip, uint32_t esp_after);

/* RtlUnwind's body: pop and call (with EXCEPTION_UNWINDING) every frame above
 * target_frame (0: all of them, EXCEPTION_EXIT_UNWIND). exc_record may be 0;
 * eip is the caller's return address, for the context the handlers see. */
void xbox_SehUnwind(uint32_t target_frame, uint32_t exc_record, uint32_t eip);

/* 1 when reg is a record on the current thread's chain (fs:[0] onwards, every
 * record checked as the dispatcher checks it). */
int xbox_SehOnChain(uint32_t reg);

/* The current thread's guest stack, [lo, hi). Set by the runtime when a guest
 * thread starts, and by whatever switches guest stacks (XBA-101) on every
 * switch. Unset, the dispatcher accepts frames from the raising esp up to
 * 1 MB above it. */
void xbox_SehSetGuestStack(uint32_t lo, uint32_t hi);

/* The bounds xbox_SehSetGuestStack set on this thread (0, 0: unknown). A
 * stack switch saves them with the side it leaves and gives them back when it
 * resumes that side. lo = 0 with hi set means "the top is known, the bottom
 * is not": records above hi are on another stack, records below esp are dead
 * anyway. */
void xbox_SehGetGuestStack(uint32_t *lo, uint32_t *hi);

/* A guest stack this thread runs on for a while and then leaves: a worker
 * stack slice (xbox_worker_stack_alloc) taken for an interrupt routine, a DPC
 * or a host-driven tick. Push names its bounds, pop gives the previous ones
 * back. Nests a few deep (a slice taken on a thread that already runs on
 * one); past that the bounds are left as they are. */
void xbox_SehPushGuestStack(uint32_t lo, uint32_t hi);
void xbox_SehPopGuestStack(void);

/* An opaque token naming the host stack the thread is on now (0 by default).
 * A guest stack switch that also switches host stacks (XBA-101) sets it on
 * every switch: an anchor taken under one token is never longjmp'd to under
 * another, the jump is refused and reported instead. */
void xbox_SehSetHostStack(uintptr_t token);

/* recomp_lookup_kernel's fallback: a guest address that is a resume target of
 * a live anchored frame on this thread resolves to a thunk that longjmps
 * there. NULL for anything else. */
typedef void (*xbox_seh_func_t)(void);
xbox_seh_func_t xbox_SehLookup(uint32_t va);

/* The guest side of a native longjmp to guest jmp_buf buf_va (the MSVC
 * layout: +0x00 ebp ... +0x18 Registration, +0x1C TryLevel, +0x20 Cookie,
 * +0x24 UnwindFunc), as the CRT's longjmp does it before it moves esp:
 *  - global unwind: RtlUnwind up to Registration, so the frames the jump
 *    leaves run their __finally blocks and destructors and fs:[0] is that
 *    registration again;
 *  - local unwind, inside the setjmp caller's own frame: with the 'VC20'
 *    cookie, the UnwindFunc _setjmp3 recorded (the CRT's
 *    _seh_longjmp_unwind, or __CxxLongjmpUnwind for C++ frames: guest code,
 *    called stdcall with the buffer); without it (an old jmp_buf), the
 *    try-level unwind of an EH3 frame (_local_unwind2) down to TryLevel.
 * Nothing when dispatch is off or Registration is not on the chain. */
void xbox_SehLongjmpUnwind(uint32_t buf_va);

/* ---- host faults on guest addresses (XBA-132) ---------------------------
 *
 * A load or store of lifted code (or of the runtime on the guest's behalf)
 * that faults on a guest address that is not trapped MMIO becomes a guest
 * STATUS_ACCESS_VIOLATION, dispatched through the thread's chain as on the
 * console: guard pages, IsBadReadPtr-style probes, __try around a pointer
 * the title does not trust. The signal handler does not dispatch: it saves
 * the faulting host context, points the thread at a trampoline on the
 * thread's own stack and returns. The trampoline raises; then
 *  - a handler that resumes at an __except block or a catch longjmps to its
 *    anchor, as for any raise;
 *  - a handler that continues execution gets the faulting host context back,
 *    exactly, and the access runs again (a guard page it committed now
 *    succeeds; one it did not faults again, and is dispatched again);
 *  - nothing handled: the context is put back with the next fault at the same
 *    place marked to go down the chain, so the access faults again and ends
 *    as it did before this existed (ART's handler, debuggerd's tombstone with
 *    the original pc and registers, or the default action).
 * RECOMP_SEH_FAULTS=0 (or RECOMP_SEH_DISPATCH=0) turns it off: every such
 * fault goes down the chain at once. Linux x86-64, Linux/Android arm64 and
 * macOS arm64; elsewhere it is off. src/kernel/xbox_seh_fault.c. */
int xbox_SehFaultsEnabled(void);

/* Called by the POSIX SIGSEGV/SIGBUS handler (xbox_memory_layout.c) after
 * trapped MMIO declined the fault: 1 when it was taken (the context now
 * points at the trampoline, or the trampoline's restore was applied) and the
 * handler must return, 0 to go down the chain as before. */
int xbox_SehPosixFault(int sig, void *siginfo, void *ucontext);

/* Once, when the handler is installed (it maps the restore trap page). */
int xbox_SehFaultInit(void);

/* The trampoline's guest half: raise STATUS_ACCESS_VIOLATION for guest
 * address va, ExceptionInformation = { access (0 read, 1 write, 8 execute),
 * va }. Returns 1 when a handler continued execution, 0 when nothing handled
 * it; a resume does not return. g_cpu is as it was at the fault on both
 * returns: registers a handler changed in the CONTEXT are not applied (the
 * lifted code holds some of them in host registers; said once in the log).
 * host_pc only names the place in the log. */
int xbox_SehRaiseHostFault(uint32_t va, uint32_t access, uintptr_t host_pc);

/* How many host faults this thread is inside (trampolines on its stack).
 * NULL until the thread has dispatcher state; async-signal-safe. A resume
 * puts back the depth its anchor was taken at. */
int *xbox_SehFaultDepthSlot(void);

/* Runtime code on this thread: a host fault is raised to the guest only from
 * lifted code, never from inside one of the runtime's own bridges (a kernel
 * call copying into a bad guest buffer would be resumed into the guest with
 * the runtime's locks held). kernel_thunk_dispatch enters and leaves around
 * every bridge; a guest routine a bridge runs (a handler, an APC, a DPC: the
 * xbox_GuestCpuEnterCallback / LeaveCallback pair) is lifted code again for
 * its length. A text range of the title's code could not tell them apart on
 * a desktop, where the runtime is linked into the title's executable.
 * Snapshot / Restore: an SEH resume or a guest longjmp that leaves bridge
 * frames behind puts back the state of the frame it lands in. */
void     xbox_SehRuntimeEnter(void);
void     xbox_SehRuntimeLeave(void);
void     xbox_SehCallbackEnter(void);
void     xbox_SehCallbackLeave(void);
int      xbox_SehInRuntime(void);          /* async-signal-safe */
uint64_t xbox_SehRuntimeSnapshot(void);
void     xbox_SehRuntimeRestore(uint64_t snap);

/* A resume point that is not an anchor (XBA-303): the interpreter takes one
 * where an interpreted activation calls native code (and at its entry), and
 * puts it back before it longjmps there over the runtime's frames: the
 * dispatcher's depth, the anchors and fault trampolines above it, the runtime
 * snapshot, the guest CPU held. */
typedef struct XboxSehPoint {
    uint64_t runtime;
    uintptr_t host;
    int anchors, depth, faults;
} XboxSehPoint;
void     xbox_SehPointTake(XboxSehPoint *p);
/* 1: restored, longjmp to the point now (va: the resume target, for the
 * report; func: where the frame's code was entered); 0: refused as an anchor on another host stack would be (counted,
 * reported, the exception goes on unhandled). */
int      xbox_SehPointResume(const XboxSehPoint *p, uint32_t va, uint32_t func);

/* Counters, for tests and the status report. */
typedef struct {
    uint32_t raises;        /* dispatches started */
    uint32_t handled;       /* a handler continued execution */
    uint32_t unhandled;     /* the chain ran out, or was refused */
    uint32_t resumes;       /* longjmps to an anchor */
    uint32_t refused;       /* resumes / frames refused (other stack, dead frame) */
    uint32_t anchors;       /* anchors taken */
    uint32_t host_faults;   /* host faults raised as guest exceptions */
} XboxSehStats;
void xbox_SehStatsGet(XboxSehStats *out, int reset);

/* Generated-code side (also declared in templates/runtime/recomp_types.h). */
#ifndef RECOMP_SEH_DESC_DEFINED
#define RECOMP_SEH_DESC_DEFINED
/* One per lifted function with an exception frame: which of its labels a
 * handler may jump to. A longjmp to the function's anchor returns 1 + the
 * target's index in `resume`, and the function goes to that label. */
typedef struct recomp_seh_desc {
    uint32_t        func_va;
    uint32_t        n_resume;
    const uint32_t *resume;
} recomp_seh_desc;
#endif

/* Take an anchor for the frame just registered at `reg` (the new fs:[0]),
 * whose frame pointer is `ebp`. Returns the native jmp_buf to setjmp into. */
jmp_buf *recomp_seh_anchor(uint32_t reg, uint32_t ebp, const recomp_seh_desc *d);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_SEH_H */
