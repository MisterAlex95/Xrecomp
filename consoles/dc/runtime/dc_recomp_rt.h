/* dc_recomp_rt.h -- the macros lifted Dreamcast code is written in (consoles/dc/tools dc_tools.lift, XBA-227).
 * Part of DC_RECOMP_ABI (dc_recomp.h): change it together with the lifter and bump DC_RECOMP_ABI.
 *
 * Every lifted function takes the CPU (c) and starts with a switch on c->pc. State lives in *c only (no host
 * locals across instructions), so resuming at any table entry, in any host frame, is the same machine state:
 *   RC_CY(n)              n instructions about to run (cycles; DC_RC_COUNT also counts them as native)
 *   RC_CHECK(a)           at table entries: an event is due, back to the dispatcher with PC = a
 *   RC_EXIT(a) / RC_RET(t)  back to the dispatcher with PC = a / t (interpreter fallback, returns, RTE)
 *   RC_CALL(T, F, R, L)   call T (lifted as F) with PR = R already set; when it comes back with PC = R, goto L
 *   RC_CALL_DYN[_G](...)  the same through a register (the guessed target G checked at run time, else the table)
 *   RC_TAIL / RC_JUMP_DYN[_G]  a jump into another function: it runs nested, then this one returns
 * Nesting stops at DC_RC_MAX_DEPTH: deeper, the dispatcher takes over (PC already set). */
#ifndef DC_RECOMP_RT_H
#define DC_RECOMP_RT_H

#include "dc_recomp.h"
#include "sh4_ops.h"

#ifdef DC_RC_COUNT
#define RC_CY(n) (c->cycles += (n), c->rc_native += (n))
#else
#define RC_CY(n) (c->cycles += (n))
#endif

#define RC_CHECK(a)                                                                                                  \
    do {                                                                                                             \
        if (c->cycles >= c->next_event) {                                                                            \
            c->pc = (a);                                                                                             \
            return;                                                                                                  \
        }                                                                                                            \
    } while (0)

#define RC_EXIT(a)                                                                                                   \
    do {                                                                                                             \
        c->pc = (a);                                                                                                 \
        return;                                                                                                      \
    } while (0)

#define RC_RET(t) RC_EXIT(t)

#define RC_NEST(F)                                                                                                   \
    do {                                                                                                             \
        c->depth++;                                                                                                  \
        (F)(c);                                                                                                      \
        c->depth--;                                                                                                  \
    } while (0)

#define RC_CALL(T, F, R, L)                                                                                          \
    do {                                                                                                             \
        c->pc = (T);                                                                                                 \
        if (c->depth < DC_RC_MAX_DEPTH) {                                                                            \
            RC_NEST(F);                                                                                              \
            if (c->pc == (R))                                                                                        \
                goto L;                                                                                              \
        }                                                                                                            \
        return;                                                                                                      \
    } while (0)

#define RC_CALL_DYN_G(t, G, F, R, L)                                                                                 \
    do {                                                                                                             \
        c->pc = (t);                                                                                                 \
        if (c->depth < DC_RC_MAX_DEPTH) {                                                                            \
            dc_recomp_fn f_ = (t) == (G) ? (F) : dc_rc_find(c, (t));                                                 \
            if (f_) {                                                                                                \
                RC_NEST(f_);                                                                                         \
                if (c->pc == (R))                                                                                    \
                    goto L;                                                                                          \
            }                                                                                                        \
        }                                                                                                            \
        return;                                                                                                      \
    } while (0)

#define RC_CALL_DYN(t, R, L)                                                                                         \
    do {                                                                                                             \
        c->pc = (t);                                                                                                 \
        if (c->depth < DC_RC_MAX_DEPTH) {                                                                            \
            dc_recomp_fn f_ = dc_rc_find(c, (t));                                                                    \
            if (f_) {                                                                                                \
                RC_NEST(f_);                                                                                         \
                if (c->pc == (R))                                                                                    \
                    goto L;                                                                                          \
            }                                                                                                        \
        }                                                                                                            \
        return;                                                                                                      \
    } while (0)

#define RC_TAIL(T, F)                                                                                                \
    do {                                                                                                             \
        c->pc = (T);                                                                                                 \
        if (c->depth < DC_RC_MAX_DEPTH)                                                                              \
            RC_NEST(F);                                                                                              \
        return;                                                                                                      \
    } while (0)

#define RC_JUMP_DYN_G(t, G, F)                                                                                       \
    do {                                                                                                             \
        c->pc = (t);                                                                                                 \
        if (c->depth < DC_RC_MAX_DEPTH) {                                                                            \
            dc_recomp_fn f_ = (t) == (G) ? (F) : dc_rc_find(c, (t));                                                 \
            if (f_)                                                                                                  \
                RC_NEST(f_);                                                                                         \
        }                                                                                                            \
        return;                                                                                                      \
    } while (0)

#define RC_JUMP_DYN(t)                                                                                               \
    do {                                                                                                             \
        c->pc = (t);                                                                                                 \
        if (c->depth < DC_RC_MAX_DEPTH) {                                                                            \
            dc_recomp_fn f_ = dc_rc_find(c, (t));                                                                    \
            if (f_)                                                                                                  \
                RC_NEST(f_);                                                                                         \
        }                                                                                                            \
        return;                                                                                                      \
    } while (0)

#endif
