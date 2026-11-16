/* psp_recomp_rt.h -- the ABI between lifted PSP code (consoles/psp/tools psp_tools.recomp) and the core (XBA-224).
 * Part of PSP_RECOMP_ABI (psp_recomp.h).
 *
 * psp_tools lifts with ps1_tools' lifter, subclassed (consoles/psp/tools/psp_tools/lift.py), so the generated C has
 * the PS1's shape and vocabulary (consoles/ps1/runtime/ps1_recomp_rt.h: RC_ENTER, RC_SEG, RC_I, RC_EXIT,
 * RC_INTERP, RC_HAND, RC_INTERP_SLOT, RC_MID_TABLE, RC_*_RAM, RC_EPILOGUE ...). This header gives that vocabulary its
 * PSP meaning:
 * - the head: CPU registers, pc / next_pc (the interpreter's model: next_pc is the branch target while pc is a delay
 *   slot), HI / LO, the FPU (f[] and FCR31, used in place by lifted code), cycles and the next event, main RAM;
 * - no load delay: RC_EXIT_LD (a load in the slot of JR / JALR) leaves the value in ld_reg / ld_val, which the
 *   dispatcher lands as soon as the function returns, before anything else runs;
 * - memory: main RAM (32 MB at 0x08000000, in every segment: KUSEG, the 0x40000000 uncached mirror, KSEG0 / 1) inline;
 *   anything else (VRAM, the scratchpad, IO) through psp_rc_read / psp_rc_write, the core's bus; only a fault (an
 *   unaligned or unmapped address) hands the instruction to the interpreter, which stops the machine on it;
 * - cycles: one per instruction, only for the event checks (VBlank, thread wake-ups): the spike has no cycle-exact
 *   timing to keep. */
#ifndef PSP_RECOMP_RT_H
#define PSP_RECOMP_RT_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "lifted PSP code assumes a little-endian host"
#endif

#include "psp_alu.h"
#include "psp_recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct psp_rc_head {
    uint32_t r[32];
    uint32_t pc;      /* the next instruction */
    uint32_t next_pc; /* the one after it (a branch target when pc is a taken branch's delay slot) */
    uint32_t hi, lo;
    float f[32];      /* FPU registers */
    uint32_t fcr31;   /* FPU control / status: RM in bits 0-1, the condition in bit 23 */
    uint32_t ld_reg, ld_val; /* RC_EXIT_LD: lands at once after the lifted function returns */
    bool next_delay;  /* written by lifted code (the PS1's shape), not read by this core */
    bool next_taken;
    uint32_t hook_at; /* unused (the PS1 lifter's BIOS hooks: no PSP address matches) */
    uint64_t cycles;
    uint64_t next_event;
    uint8_t *ram;       /* 32 MB main RAM, physical 0x08000000 */
    uint64_t rc_native; /* instructions run natively (packs built with -DPSP_RC_COUNT) */
    uint8_t rc_exit;    /* PSP_RC_EXIT_* */
    uint8_t rc_hint;    /* with PSP_RC_EXIT_INTERP: instructions the interpreter may run in sequence (0 = 1) */
} psp_rc_head;

enum {
    PSP_RC_EXIT_NORMAL = 0,      /* the state says where execution continues */
    PSP_RC_EXIT_INTERP = 1,      /* the interpreter runs the instruction at pc (rc_hint: and those after it) */
    PSP_RC_EXIT_NOT_COVERED = 2, /* this function does not hold pc: nothing ran */
};

/* The core's bus (psp_bus.c), for accesses off main RAM. 0, or nonzero for a fault (nothing done). */
int psp_rc_read(psp_machine *m, uint32_t addr, int width, uint32_t *v); /* zero-extended */
int psp_rc_write(psp_machine *m, uint32_t addr, int width, uint32_t v);
int psp_rc_swlr(psp_machine *m, uint32_t addr, uint32_t rt, int right); /* SWL (right = 0) / SWR */

#define RC_ENTER(CPI, PLO, PLEN)                                                                                     \
    psp_rc_head *const h = (psp_rc_head *)(void *)m_;                                                                \
    enum { RC_CPI = (CPI) };                                                                                         \
    uint8_t *const rc_ram = h->ram;                                                                                  \
    uint64_t now = h->cycles, ne = h->next_event;                                                                    \
    uint32_t epc = 0, enp = 0, tk = 0, jt = 0, ldv = 0, ehint = 0;                                                   \
    bool edl = false, etk = false;                                                                                   \
    (void)rc_ram

#define RC_M ((psp_machine *)(void *)h)

#ifdef PSP_RC_COUNT
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
            h->rc_exit = PSP_RC_EXIT_INTERP;                                                                         \
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
        h->rc_exit = PSP_RC_EXIT_NOT_COVERED;                                                                        \
        return;                                                                                                      \
    } while (0)

#define RC_HOOK(T) ((void)(T))

/* Main RAM in any segment: physical 0x08000000-0x09FFFFFF (bits 25-28 = 0100), AL = alignment mask. */
#define RC_RAM_FAST(A, AL) (((A) & (0x1E000000u | (AL))) == 0x08000000u)
#define RC_RAM_OFF(A) ((A) & 0x01FFFFFFu)

#define RC_LW_RAM(A, AL, OUT, HAND)                                                                                  \
    do {                                                                                                             \
        uint32_t a_ = (A);                                                                                           \
        if (__builtin_expect(RC_RAM_FAST(a_, AL), 1)) {                                                              \
            memcpy(&(OUT), rc_ram + RC_RAM_OFF(a_), 4);                                                              \
        } else {                                                                                                     \
            uint32_t v_ = 0;                                                                                         \
            if ((a_ & (AL)) || psp_rc_read(RC_M, a_, 4, &v_)) {                                                      \
                HAND;                                                                                                \
            }                                                                                                        \
            (OUT) = v_;                                                                                              \
        }                                                                                                            \
    } while (0)
#define RC_LD_RAM(T, A, AL, OUT, HAND)                                                                               \
    do {                                                                                                             \
        uint32_t a_ = (A);                                                                                           \
        T t_;                                                                                                        \
        if (__builtin_expect(RC_RAM_FAST(a_, AL), 1)) {                                                              \
            memcpy(&t_, rc_ram + RC_RAM_OFF(a_), sizeof t_);                                                         \
        } else {                                                                                                     \
            uint32_t v_ = 0;                                                                                         \
            if ((a_ & (AL)) || psp_rc_read(RC_M, a_, (int)sizeof t_, &v_)) {                                         \
                HAND;                                                                                                \
            }                                                                                                        \
            t_ = (T)v_;                                                                                              \
        }                                                                                                            \
        (OUT) = (uint32_t)(int32_t)t_;                                                                               \
    } while (0)
#define RC_ST_RAM(T, A, AL, V, HAND)                                                                                 \
    do {                                                                                                             \
        uint32_t a_ = (A);                                                                                           \
        T t_ = (T)(V);                                                                                               \
        if (__builtin_expect(RC_RAM_FAST(a_, AL), 1)) {                                                              \
            memcpy(rc_ram + RC_RAM_OFF(a_), &t_, sizeof t_);                                                         \
        } else if ((a_ & (AL)) || psp_rc_write(RC_M, a_, (int)sizeof t_, (uint32_t)t_)) {                            \
            HAND;                                                                                                    \
        }                                                                                                            \
    } while (0)
/* The PS1's "known IO register" forms: psp_tools never emits them (no IO heuristic), defined for completeness. */
#define RC_LW(A, AL, OUT, FAULT) RC_LW_RAM(A, AL, OUT, FAULT)
#define RC_LD(T, A, AL, OUT, FAULT) RC_LD_RAM(T, A, AL, OUT, FAULT)
#define RC_ST(T, A, AL, V, FAULT) RC_ST_RAM(T, A, AL, V, FAULT)

#define RC_SWLR(A, V, RIGHT, FAULT)                                                                                  \
    do {                                                                                                             \
        if (psp_rc_swlr(RC_M, (A), (V), (RIGHT))) {                                                                  \
            FAULT;                                                                                                   \
        }                                                                                                            \
    } while (0)

#define RC_EPILOGUE()                                                                                                \
    x_seg: __attribute__((unused));                                                                                  \
    if (now < ne) {                                                                                                  \
        h->rc_exit = PSP_RC_EXIT_INTERP;                                                                             \
        h->rc_hint = (uint8_t)ehint;                                                                                 \
    }                                                                                                                \
    goto x_plain;                                                                                                    \
    x_interp_slot: __attribute__((unused));                                                                          \
    now -= RC_CPI;                                                                                                   \
    RC_UNCOUNT();                                                                                                    \
    h->rc_exit = PSP_RC_EXIT_INTERP;                                                                                 \
    edl = true;                                                                                                      \
    goto x_exit;                                                                                                     \
    x_hand: __attribute__((unused));                                                                                 \
    h->rc_hint = (uint8_t)ehint;                                                                                     \
    x_interp: __attribute__((unused));                                                                               \
    now -= RC_CPI;                                                                                                   \
    RC_UNCOUNT();                                                                                                    \
    h->rc_exit = PSP_RC_EXIT_INTERP;                                                                                 \
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
