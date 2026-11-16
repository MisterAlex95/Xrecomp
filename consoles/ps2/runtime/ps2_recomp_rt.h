/* ps2_recomp_rt.h -- the ABI between lifted EE code (consoles/ps2/tools ps2_tools.recomp) and the core (XBA-226).
 * Part of PS2_RECOMP_ABI (ps2_recomp.h).
 *
 * ps2_tools lifts with ps1_tools' lifter, subclassed (consoles/ps2/tools/ps2_tools/lift.py), so the generated C has
 * the PS1's shape and vocabulary (consoles/ps1/runtime/ps1_recomp_rt.h: RC_ENTER, RC_SEG, RC_I, RC_EXIT,
 * RC_INTERP, RC_HAND, RC_INTERP_SLOT, RC_MID_TABLE, RC_EPILOGUE ...). This header gives that vocabulary its EE
 * meaning:
 * - the head: the GPRs' low 64 bits (r[], in locals while a function runs: uint64_t) and high 64 bits (rh[], used
 *   in place by the MMI ops, LQ / SQ), pc / next_pc (the interpreter's model: next_pc is the branch target while pc
 *   is a delay slot), HI / LO and their pipeline-1 halves HI1 / LO1, the SA register, the FPU (f[] as bit patterns,
 *   ACC, FCR31: used in place), cycles and the next event, main RAM;
 * - no load delay on the R5900: lifted code never leaves a load in flight (RC_EXIT_LD exists for the PS1 shape);
 * - memory: main RAM (32 MB, the low 32 MB of every segment: KUSEG as PS2SDK maps it, the uncached and
 *   uncached-accelerated mirrors at 0x20000000 / 0x30000000, KSEG0 / KSEG1) inline; anything else (the scratchpad
 *   at 0x70000000, IO, the GS) through ps2_rc_read / ps2_rc_write, the core's bus; only a fault (an unaligned or
 *   unmapped address) hands the instruction to the interpreter, which stops the machine on it;
 * - cycles: one per instruction, only for the event checks: the spike has no timing to keep. */
#ifndef PS2_RECOMP_RT_H
#define PS2_RECOMP_RT_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "lifted EE code assumes a little-endian host"
#endif

#include "ps2_alu.h"
#include "ps2_recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ps2_rc_head {
    uint64_t r[32];   /* GPRs, low 64 bits */
    uint64_t rh[32];  /* GPRs, high 64 bits */
    uint32_t pc;      /* the next instruction */
    uint32_t next_pc; /* the one after it (a branch target when pc is a taken branch's delay slot) */
    uint64_t hi, lo;   /* HI / LO, low 64 bits */
    uint64_t hi1, lo1; /* their high 64 bits (MULT1 / DIV1 / MADD1 write them) */
    uint32_t sa;       /* the funnel shift amount (MTSA / QFSRV): kept, never used by the spike */
    uint32_t f[32];    /* FPU registers, bit patterns (ps2_alu.h: not IEEE) */
    uint32_t acc;      /* the FPU accumulator (ADDA.S / MULA.S / MADDA.S ...) */
    uint32_t fcr31;    /* FPU control / status: the condition in bit 23 */
    uint32_t ld_reg;   /* RC_EXIT_LD (never emitted for the R5900) */
    uint64_t ld_val;
    bool next_delay; /* written by lifted code (the PS1's shape), not read by this core */
    bool next_taken;
    uint64_t cycles;
    uint64_t next_event;
    uint8_t *ram;       /* 32 MB main RAM, physical 0 */
    uint64_t rc_native; /* instructions run natively (packs built with -DPS2_RC_COUNT) */
    uint8_t rc_exit;    /* PS2_RC_EXIT_* */
    uint8_t rc_hint;    /* with PS2_RC_EXIT_INTERP: instructions the interpreter may run in sequence (0 = 1) */
} ps2_rc_head;

enum {
    PS2_RC_EXIT_NORMAL = 0,      /* the state says where execution continues */
    PS2_RC_EXIT_INTERP = 1,      /* the interpreter runs the instruction at pc (rc_hint: and those after it) */
    PS2_RC_EXIT_NOT_COVERED = 2, /* this function does not hold pc: nothing ran */
};

/* The core's bus (ps2_bus.c), for accesses off main RAM: width 1 / 2 / 4 / 8 / 16 bytes, little-endian, at an
 * aligned address. 0, or nonzero for a fault (nothing done). */
int ps2_rc_read(ps2_machine *m, uint32_t addr, int width, void *out);
int ps2_rc_write(ps2_machine *m, uint32_t addr, int width, const void *in);
/* LWL / LWR / LDL / LDR (kind 0-3: *out = the new rt) and SWL / SWR / SDL / SDR (4-7) anywhere. 0 or a fault. */
int ps2_rc_unaligned(ps2_machine *m, int kind, uint32_t addr, uint64_t rt, uint64_t *out);

#define RC_ENTER(CPI, PLO, PLEN)                                                                                     \
    ps2_rc_head *const h = (ps2_rc_head *)(void *)m_;                                                                \
    enum { RC_CPI = (CPI) };                                                                                         \
    uint8_t *const rc_ram = h->ram;                                                                                  \
    uint64_t now = h->cycles, ne = h->next_event;                                                                    \
    uint32_t epc = 0, enp = 0, tk = 0, jt = 0, ehint = 0;                                                            \
    uint64_t ldv = 0;                                                                                                \
    bool edl = false, etk = false;                                                                                   \
    (void)rc_ram

#define RC_M ((ps2_machine *)(void *)h)

#ifdef PS2_RC_COUNT
#define RC_COUNT() (h->rc_native++)
#define RC_UNCOUNT() (h->rc_native--)
#else
#define RC_COUNT() ((void)0)
#define RC_UNCOUNT() ((void)0)
#endif

#define RC_EXIT(A)                                                                                                   \
    do {                                                                                                             \
        epc = (A);                                                                                                   \
        goto x_plain;                                                                                                \
    } while (0)
#define RC_EXIT_LD(A, RT, V)                                                                                         \
    do {                                                                                                             \
        h->ld_reg = (RT);                                                                                            \
        h->ld_val = (V);                                                                                             \
        RC_EXIT(A);                                                                                                  \
    } while (0)

#define RC_I()                                                                                                       \
    do {                                                                                                             \
        now += RC_CPI;                                                                                               \
        RC_COUNT();                                                                                                  \
    } while (0)

/* The event check of a segment starting at A: due now, leave for the dispatcher; due inside the segment, the
 * interpreter runs from A (N instructions to the block's end). */
#define RC_SEG(A, CYC, N)                                                                                            \
    do {                                                                                                             \
        if (__builtin_expect(now + (CYC) >= ne, 0)) {                                                                \
            epc = (A);                                                                                               \
            ehint = (N);                                                                                             \
            goto x_seg;                                                                                              \
        }                                                                                                            \
    } while (0)

#define RC_MID_TABLE(LO, TAB)                                                                                        \
    do {                                                                                                             \
        uint32_t o__ = h->pc - (LO), i__ = o__ >> 2;                                                                 \
        if (!(o__ & 3u) && i__ < sizeof(TAB) && (TAB)[i__]) {                                                        \
            h->rc_hint = (TAB)[i__];                                                                                 \
            h->rc_exit = PS2_RC_EXIT_INTERP;                                                                         \
            return;                                                                                                  \
        }                                                                                                            \
    } while (0)

#define RC_INTERP(A)                                                                                                 \
    do {                                                                                                             \
        epc = (A);                                                                                                   \
        goto x_interp;                                                                                               \
    } while (0)
#define RC_HAND(A, N)                                                                                                \
    do {                                                                                                             \
        epc = (A);                                                                                                   \
        ehint = (N);                                                                                                 \
        goto x_hand;                                                                                                 \
    } while (0)
#define RC_INTERP_SLOT(A, NPC, TK)                                                                                   \
    do {                                                                                                             \
        epc = (A);                                                                                                   \
        enp = (NPC);                                                                                                 \
        etk = (TK);                                                                                                  \
        goto x_interp_slot;                                                                                          \
    } while (0)

#define RC_NOT_COVERED()                                                                                             \
    do {                                                                                                             \
        h->rc_exit = PS2_RC_EXIT_NOT_COVERED;                                                                        \
        return;                                                                                                      \
    } while (0)

/* A 128-bit GPR value: its low half from the local L (or 0 for r0), its high half from the head. */
#define RC_Q(L, N) ((N) ? ps2_q_make((L), h->rh[(N)]) : ps2_q_make(0, 0))
/* Its halves (named so that the PS1 lifter's register regex never takes a member for the hi / lo locals). */
#define RC_QLO(Q) ((Q).lo)
#define RC_QHI(Q) ((Q).hi)

/* Main RAM in any segment: the low 32 MB (bits 25-28 clear), AL = alignment mask. */
#define RC_RAM_FAST(A, AL) (((A) & (0x1E000000u | (AL))) == 0)
#define RC_RAM_OFF(A) ((A) & 0x01FFFFFFu)

/* T: the access type; OUT gets it sign- or zero-extended to 64 bits as T is signed or not. */
#define RC_LD_RAM(T, A, AL, OUT, HAND)                                                                               \
    do {                                                                                                             \
        uint32_t a_ = (uint32_t)(A);                                                                                 \
        T t_;                                                                                                        \
        if (__builtin_expect(RC_RAM_FAST(a_, AL), 1)) {                                                              \
            memcpy(&t_, rc_ram + RC_RAM_OFF(a_), sizeof t_);                                                         \
        } else if ((a_ & (AL)) || ps2_rc_read(RC_M, a_, (int)sizeof t_, &t_)) {                                      \
            HAND;                                                                                                    \
        }                                                                                                            \
        (OUT) = (uint64_t)(int64_t)t_;                                                                               \
    } while (0)
#define RC_ST_RAM(T, A, AL, V, HAND)                                                                                 \
    do {                                                                                                             \
        uint32_t a_ = (uint32_t)(A);                                                                                 \
        T t_ = (T)(V);                                                                                               \
        if (__builtin_expect(RC_RAM_FAST(a_, AL), 1)) {                                                              \
            memcpy(rc_ram + RC_RAM_OFF(a_), &t_, sizeof t_);                                                         \
        } else if ((a_ & (AL)) || ps2_rc_write(RC_M, a_, (int)sizeof t_, &t_)) {                                     \
            HAND;                                                                                                    \
        }                                                                                                            \
    } while (0)
/* LQ / SQ: the R5900 clears the address's low 4 bits. */
#define RC_LQ(A, OLO, OHI, HAND)                                                                                     \
    do {                                                                                                             \
        uint32_t a_ = (uint32_t)(A) & ~15u;                                                                          \
        uint64_t q_[2];                                                                                              \
        if (__builtin_expect(RC_RAM_FAST(a_, 0u), 1)) {                                                              \
            memcpy(q_, rc_ram + RC_RAM_OFF(a_), 16);                                                                 \
        } else if (ps2_rc_read(RC_M, a_, 16, q_)) {                                                                  \
            HAND;                                                                                                    \
        }                                                                                                            \
        (OLO) = q_[0];                                                                                               \
        (OHI) = q_[1];                                                                                               \
    } while (0)
#define RC_SQ(A, VLO, VHI, HAND)                                                                                     \
    do {                                                                                                             \
        uint32_t a_ = (uint32_t)(A) & ~15u;                                                                          \
        uint64_t q_[2] = {(VLO), (VHI)};                                                                             \
        if (__builtin_expect(RC_RAM_FAST(a_, 0u), 1)) {                                                              \
            memcpy(rc_ram + RC_RAM_OFF(a_), q_, 16);                                                                 \
        } else if (ps2_rc_write(RC_M, a_, 16, q_)) {                                                                 \
            HAND;                                                                                                    \
        }                                                                                                            \
    } while (0)
/* LWL ... SDR (kind as ps2_rc_unaligned) through the core. */
#define RC_LR(KIND, A, RT, OUT, HAND)                                                                                \
    do {                                                                                                             \
        uint64_t o_ = 0;                                                                                             \
        if (ps2_rc_unaligned(RC_M, (KIND), (uint32_t)(A), (RT), &o_)) {                                              \
            HAND;                                                                                                    \
        }                                                                                                            \
        (OUT) = o_;                                                                                                  \
    } while (0)

#define RC_EPILOGUE()                                                                                                \
    x_seg: __attribute__((unused));                                                                                  \
    if (now < ne) {                                                                                                  \
        h->rc_exit = PS2_RC_EXIT_INTERP;                                                                             \
        h->rc_hint = (uint8_t)ehint;                                                                                 \
    }                                                                                                                \
    goto x_plain;                                                                                                    \
    x_interp_slot: __attribute__((unused));                                                                          \
    now -= RC_CPI;                                                                                                   \
    RC_UNCOUNT();                                                                                                    \
    h->rc_exit = PS2_RC_EXIT_INTERP;                                                                                 \
    edl = true;                                                                                                      \
    goto x_exit;                                                                                                     \
    x_hand: __attribute__((unused));                                                                                 \
    h->rc_hint = (uint8_t)ehint;                                                                                     \
    x_interp: __attribute__((unused));                                                                               \
    now -= RC_CPI;                                                                                                   \
    RC_UNCOUNT();                                                                                                    \
    h->rc_exit = PS2_RC_EXIT_INTERP;                                                                                 \
    x_plain: __attribute__((unused));                                                                                \
    enp = epc + 4u;                                                                                                  \
    edl = false;                                                                                                     \
    etk = false;                                                                                                     \
    x_exit: __attribute__((unused));                                                                                 \
    h->pc = epc;                                                                                                     \
    h->next_pc = enp;                                                                                                \
    h->next_delay = edl;                                                                                             \
    h->next_taken = etk;                                                                                             \
    h->cycles = now;                                                                                                 \
    (void)tk; (void)jt; (void)ldv; (void)ehint

#define RC_FN __attribute__((visibility("hidden"))) void
#define RC_EXPORT __attribute__((visibility("default")))

#ifdef __cplusplus
}
#endif

#endif
