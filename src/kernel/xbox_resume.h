/*
 * xbox_resume.h - re-entering a saved guest call chain in a fresh process (XBA-250,
 * docs/technical/xbox-save-states.md, « Across runs »)
 *
 * A save state holds each guest thread parked at a kernel call or a host wait, with its registers and its guest
 * call chain (XBA-247: every lifted frame's entry esp and the ebp its caller published, the call sites from the
 * call-site table). Lifted code keeps its return path in host frames, which a fresh process does not have. As
 * Emscripten's Asyncify rewinds, the thread rebuilds them by calling its chain again: the lifter gives every
 * function that may park a resume entry (RECOMP_FRAME_ENTRY_R, recomp_types.h, and a resume block at its end),
 * and a label before every call that may park and after every host wait. A thread in resume mode enters its
 * routine; each function records its frame, asks recomp_resume_site where it was, gets its ebp back and jumps to
 * that call, which is made again into the next function of the chain (an indirect call's target comes from
 * recomp_resume_target); the innermost function re-enters the kernel call (kernel_thunk_dispatch completes it:
 * the status the call had returned, or the call made again from the registers it was made with) or continues
 * after its host wait with the saved registers. Then the thread runs on as if it had never stopped.
 *
 * This file is the engine (one plan per thread, thread-local); xbox_state.c builds the plans from a state
 * (xbox_StateResumePlan) and recreates the threads (xbox_StateResume). Engine only: no kernel dependency.
 */
#ifndef XBOX_RESUME_H
#define XBOX_RESUME_H

#include <stddef.h>
#include <stdint.h>

#include "xbox_memory_layout.h"   /* RecompCpu, RecompCpuRegs */

#ifdef __cplusplus
extern "C" {
#endif

/* A site key of a host wait inside lifted code (the call-site table's and the resume block's). */
#define XBOX_RESUME_HOST_WAIT 0x80000000u

typedef struct XboxResumeFrame {
    uint32_t func;           /* the lifted function this frame is (its start VA, the call-site table's) */
    uint32_t site;           /* where it re-enters: the return VA of the call it is in, or
                              * XBOX_RESUME_HOST_WAIT | the VA of its host wait (the innermost frame only) */
    uint32_t ebp;            /* its ebp there (what it published before that call or wait) */
    uint32_t next_esp;       /* the callee's entry esp (at its return address), the next frame's */
    uint32_t next_ebp;       /* the ebp the callee records (published by this frame, or before it) */
    uint32_t target;         /* an indirect call: the callee's VA; the innermost kernel call: the thunk's VA */
} XboxResumeFrame;

typedef struct XboxResumePlan {
    uint32_t n;              /* frames, the routine first */
    uint32_t at;             /* the next frame to re-enter */
    uint32_t entry_esp;      /* the routine's entry esp and the ebp it records (frame 0's) */
    uint32_t entry_ebp;
    uint32_t site_kind;      /* XBOX_PARK_SITE_* (kernel_park.h) of the park point */
    uint32_t ordinal;        /* the kernel call (site kind 0) */
    uint32_t returned;       /* the call had returned: its status is in regs, it is not made again */
    uint32_t call_esp;       /* esp when the call was made (its return address's slot) */
    RecompCpuRegs regs;      /* the park record's registers */
    volatile int32_t done;   /* 1 once the innermost frame was re-entered */
    XboxResumeFrame f[1];    /* n of them (xbox_ResumePlanAlloc) */
} XboxResumePlan;

/* A zeroed plan with room for n frames (free()). */
XboxResumePlan *xbox_ResumePlanAlloc(uint32_t n);

/* A frozen thread's chain as a save state holds it (XboxStateFrame without the table's columns): per frame the
 * word at its entry esp (its return VA; 0 for the routine, which the runtime entered), that esp and the ebp it
 * recorded. */
typedef struct XboxResumeChainFrame {
    uint32_t f_ret, f_esp, f_ebp;   /* not `esp`: generated code has a macro of that name */
} XboxResumeChainFrame;

/* The call-site table (kernel_park.h xbox_CallSiteLookup: key a return VA, or XBOX_RESUME_HOST_WAIT | a host
 * wait's VA): 1 with the function the site is in and its XBOX_CALL_SITE_* flags. */
typedef int (*XboxResumeLookup)(uint32_t key, uint32_t *func, uint32_t *flags);

/* The plan that re-enters a chain of n frames (n >= 1, the routine first) parked at a kernel call (site_kind 0:
 * site_va the call's return address, ordinal, the thunk VA the call reaches, returned, call_esp) or at a host wait
 * (site_kind its XBOX_PARK_SITE_*, site_va its VA), with the park record's registers. Returns the plan
 * (xbox_ResumePlanAlloc), or NULL with the reason the chain cannot be re-entered: a site with no resume entry or a
 * flag in XBOX_CALL_SITE_UNRESUMABLE, a tail call (two frames at one esp), a return VA no site names, a routine
 * not entered by the runtime. */
XboxResumePlan *xbox_ResumePlanBuild(const XboxResumeChainFrame *fr, uint32_t n, uint32_t site_kind,
                                     uint32_t site_va, uint32_t ordinal, uint32_t kernel_target, int returned,
                                     uint32_t call_esp, const RecompCpuRegs *regs, XboxResumeLookup lookup,
                                     char *why, size_t why_n);

/* What recomp_resume_site leaves for the lifted code (recomp_types.h): g_ebp is the ebp the function held at its
 * site (the resume block takes it from there: no second call, no address-taken local), and g_eax the target of an
 * indirect call made again (garbage on the resume path otherwise: the callee is entered in resume mode, the
 * innermost kernel call takes the record's registers). recomp_resume_ebp / recomp_resume_target give the same.

 * On the thread that resumes, just before calling its routine (frame 0's function): resume mode on (the frame
 * limit lowered so every RECOMP_FRAME_ENTRY_R takes its slow path), esp and ebp as the routine was entered. The
 * plan must live until the thread has re-entered its innermost frame (plan->done). */
void xbox_ResumeBegin(XboxResumePlan *p);
/* The same on a thread whose routine runs above `base` frames of its own (XBA-294: the INLINE main thread
 * re-entered in the same run keeps its creator's frames below its routine). */
void xbox_ResumeBeginAt(XboxResumePlan *p, uint32_t base);
/* 1 while the calling thread is re-entering its chain. */
int  xbox_ResumeActive(void);

/* kernel_thunk_dispatch, at every kernel call: 1 when the calling thread is re-entering the kernel call of its
 * plan now. Arrive then takes the call's registers: 1 when the call had returned (the dispatch returns at once,
 * the registers as the call left them), 0 when it is made again (the registers it was made with). */
int  xbox_ResumeKernelPending(void);
int  xbox_ResumeKernelArrive(uint32_t ordinal);

/* Threads that re-entered their innermost frame since the process started (logs, tests). */
uint32_t xbox_ResumeCompleted(void);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_RESUME_H */
