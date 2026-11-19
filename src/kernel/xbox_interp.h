/* xbox_interp.h - the x86-32 interpreter: a fallback for guest code the lifter
 * did not translate (XBA-303).
 *
 * Static recompilation lifts what the analysis found. What it missed used to
 * be skipped: an indirect call to a function with no dispatch entry popped
 * its return address and went on (« maybe wrong »), a thread whose start
 * routine was not lifted never ran, code made at run time (exec_outside_image)
 * was only named. Each miss cost a whole Prepare round to find, and the next
 * one only showed up once the first was lifted.
 *
 * Now such code is interpreted, from the guest CPU state in g_cpu, until it
 * reaches lifted code again: a call into a lifted function calls it (a C
 * call, as lifted code makes it), a ret past the activation's own return
 * address returns to the lifted caller. The miss is still written to the
 * discovery file (discovered.jsonl), and so is every call the interpreted
 * code makes to another unlifted address, so one run names every miss and the
 * next Prepare lifts them all.
 *
 * Two halves:
 *   - the core (xbox_interp_core.c): decode and execute one activation over
 *     g_cpu. Integer instructions are written by hand with exact flags; the
 *     x87, SSE, MMX and system instructions are generated from the lifter
 *     (tools/recomp/gen_interp.py -> xbox_interp_gen.inc), so both stay in
 *     step. It depends on templates/runtime/recomp_types.h only, plus the
 *     hooks below, so a test links it with lifted code (tools/recomp
 *     test_interp_diff.py).
 *   - the glue (xbox_interp.c): the hooks for the runtime (what is lifted,
 *     the discovery file, poll waits and preemption), callable thunks for
 *     missed addresses (xbox_InterpResolve), the activation stack, SEH and
 *     save-state boundaries, the counters.
 *
 * RECOMP_INTERP=0 restores the old behaviour (skip, report).
 * Design and limits: docs/technical/interpreter.md. */
#ifndef XBOX_INTERP_H
#define XBOX_INTERP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*xbox_interp_fn)(void);

/* ---- core ---------------------------------------------------------------- */

/* What the core asks its host. Any member may be NULL (the test harness's
 * default): then nothing is native, nothing is recorded, a poll loop spins. */
typedef struct XboxInterpHooks {
    /* The lifted (or manual, or kernel thunk) function entered at va, or NULL
     * when va has none and is to be interpreted. */
    xbox_interp_fn (*native)(uint32_t va);
    /* Interpreted code called `target` (no native function) from a call whose
     * return address is `ret`. */
    void (*note_call)(uint32_t target, uint32_t ret);
    /* A taken backward branch at va, every XBOX_INTERP_EDGE_EVERY of them:
     * the guest CPU's quantum check (RECOMP_BACK_EDGE). */
    void (*back_edge)(uint32_t va);
    /* A loop that only polls one word (`cmp/test [m], imm; jcc back`), as
     * RECOMP_POLL_WAIT. */
    void (*poll_wait)(uint32_t addr, uint32_t mask, int while_set, uint32_t va);
    /* Called before a native call and after it returns (depth: the number of
     * native calls in progress in this activation, after the change). */
    void (*native_edge)(int entering, uint32_t va);
} XboxInterpHooks;

extern XboxInterpHooks xbox_interp_hooks;

#define XBOX_INTERP_EDGE_EVERY 256u

/* How an activation ended. */
enum {
    XBOX_INTERP_RETURNED = 0,   /* a ret popped the entry's return address */
    XBOX_INTERP_UNIMPL = 1,     /* an instruction the interpreter cannot run; stop->va names it */
    XBOX_INTERP_LIMIT = 2       /* the instruction budget ran out (tests) */
};

typedef struct XboxInterpStop {
    uint32_t va;                /* the instruction it stopped at (UNIMPL) */
    uint32_t len;               /* its decoded length, 0 when undecodable */
    uint8_t bytes[15];          /* its first bytes */
    uint64_t insns;             /* instructions executed by this activation */
} XboxInterpStop;

/* Interpret from va. The guest stack is as a call left it: [esp] is the
 * return address (a call into va), or the jumping function's own return
 * address (a tail jump). Runs until a ret pops past that slot (esp above it),
 * or an instruction it cannot run, or `limit` instructions (0: no limit).
 * ebp is g_ebp throughout (published, as before a lifted call); the
 * arithmetic flags are g_eflags, DF is g_df. */
int xbox_InterpRun(uint32_t va, uint64_t limit, XboxInterpStop *stop);

/* The guest address of the instruction the calling thread's innermost
 * activation is executing, 0 when it is not interpreting. The fault path
 * reports it as the exception's address. */
uint32_t xbox_InterpCurrentVa(void);

/* Decode-only: the length of the instruction at va, 0 when the interpreter
 * cannot decode it (the entry gate, tests). */
int xbox_InterpDecodeLength(uint32_t va);

/* ---- glue (the runtime) ---------------------------------------------------- */

/* 1 unless RECOMP_INTERP=0 (read once). */
int xbox_InterpEnabled(void);

/* A callable for guest address va that has no lifted function: a thunk that
 * interprets from va (the same thunk for the same va for the whole run).
 * NULL when the interpreter is off, when va does not look like code (the
 * entry gate: undecodable, zeros, unmapped) or when the thunks ran out.
 * kind names the miss for the log and the discovery file:
 *   'c' an indirect call or tail jump inside the image (icall)
 *   'o' outside the image's code (exec_outside_image, recomp_smc.h)
 *   't' a thread start routine
 *   'r' a routine the runtime calls (DPC, APC, timer, notify)
 * ret: the return address on the guest stack, for the record (0: none). */
xbox_interp_fn xbox_InterpResolve(uint32_t va, int kind, uint32_t ret);

/* The guest address a thunk from xbox_InterpResolve interprets, 0 for any
 * other function. */
uint32_t xbox_InterpThunkVa(xbox_interp_fn fn);

/* Counters, process-wide: instructions interpreted, activations, distinct
 * addresses resolved to thunks. */
typedef struct XboxInterpStats {
    uint64_t insns;
    uint64_t activations;
    uint32_t entries;
    uint32_t unimpl;
} XboxInterpStats;
void xbox_InterpGetStats(XboxInterpStats *out);

/* Activations of the calling thread in progress (0: not interpreting). The
 * save-state code refuses a thread parked inside one. */
int xbox_InterpDepth(void);

/* Called from the indirect-call miss path (recomp_types.h): a resumed SEH
 * handler or a guest longjmp that jumps back into interpreted code whose
 * native frames are below it. If the calling thread has an interpreter
 * activation that made a native call the jump abandons (the target's frame,
 * esp, is above that call), longjmps to it and goes on interpreting at va:
 * does not return. Returns otherwise. */
void xbox_InterpResumeAt(uint32_t va);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_INTERP_H */
