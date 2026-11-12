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

/* Counters, for tests and the status report. */
typedef struct {
    uint32_t raises;        /* dispatches started */
    uint32_t handled;       /* a handler continued execution */
    uint32_t unhandled;     /* the chain ran out, or was refused */
    uint32_t resumes;       /* longjmps to an anchor */
    uint32_t refused;       /* resumes / frames refused (other stack, dead frame) */
    uint32_t anchors;       /* anchors taken */
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
