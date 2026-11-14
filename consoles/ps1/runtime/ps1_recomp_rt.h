/* ps1_recomp_rt.h -- the ABI between lifted PS1 code (consoles/ps1/tools ps1_tools.recomp) and the core (XBA-161 /
 * XBA-162). Part of PS1_RECOMP_ABI (ps1_recomp.h): change this layout, these macros' meaning or the symbols below and
 * bump PS1_RECOMP_ABI.
 *
 * Lifted code sees the machine through ps1_rc_head, the first members of ps1_machine (ps1_recomp.c checks every
 * offset with static asserts): the CPU registers and delay-slot state, COP0, the cycle clock and the next scheduler
 * event, main RAM and the scratchpad, the RAM page write counters. Everything else goes through functions of the core,
 * which the host executable (ps1_run, the Android shell's libmain.so) must export for the pack to bind to:
 * ps1_rc_lw, ps1_rc_read, ps1_rc_write, ps1_rc_swlr, ps1_rc_gte_cmd, ps1_rc_gte_read, ps1_rc_gte_write.
 *
 * Lifted code is cycle-exact with the interpreter (ps1_cpu.c): every instruction costs the same fetch cycles, LW-class
 * loads the same load cycles, at the same point; it stops where the run loop would (`cycles < next_event` before each
 * instruction): exactly at the start of a block, and once per segment inside it (ps1_tools lift.py « blocks and event
 * checks »: when the next event falls inside a segment, the interpreter runs the rest of the block). An access that
 * left main RAM (IO: it may raise an interrupt) and left the IRQ line up, or a store into the function's own code,
 * stops it after that instruction. When it must stop (scheduler event, pending IRQ, a call, a return, an indirect
 * jump, a jump out of the function: the dispatcher picks the next function), it writes the registers back with pc /
 * next_pc / the delay-slot flags exactly as the interpreter would hold them between two instructions, and returns.
 *
 * Load delay slots: a load's value lands after the next instruction. The lifter commits it at once when the next
 * instruction neither reads the register nor loads into it (then nothing can tell the difference: an exception or an
 * interrupt before that instruction lands the load too); otherwise it stops with the load in flight (ld_reg /
 * ld_val) and the interpreter runs that instruction. Branch delay slots run inline; a branch in a delay slot, and
 * every instruction that raises an exception (overflow, address / bus error, SYSCALL, BREAK, reserved, coprocessor
 * unusable) or touches COP0 (MFC0, MTC0, RFE) is run by the interpreter: the lifted code returns with rc_exit =
 * PS1_RC_EXIT_INTERP and the state in front of that instruction, nothing of it done.
 *
 * Entry: the dispatcher calls a table function with pc at any instruction of its range, never in a delay slot, never
 * with a load in flight, in kernel mode with the cache not isolated (it interprets otherwise). The function resumes
 * there when it is the start of one of its blocks (the resume points: its entry, jump targets, return points, every
 * few instructions). At another of its instructions it returns at once with PS1_RC_EXIT_INTERP and rc_hint = the
 * number of instructions to its block's end, nothing changed: the interpreter runs them (as when an event stopped
 * lifted code there, or an interrupt handler returns there). If pc is not one of its own it returns at once with
 * rc_exit = PS1_RC_EXIT_NOT_COVERED. (Packs from before rc_hint resumed at every instruction; both kinds run on every
 * core of PS1_RECOMP_ABI 1: an older core ignores rc_hint and calls the function back for each instruction.) */
#ifndef PS1_RECOMP_RT_H
#define PS1_RECOMP_RT_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "lifted PS1 code assumes a little-endian host"
#endif

#include "ps1_recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ps1_rc_head {
    uint32_t r[32];
    uint32_t pc;            /* the next instruction */
    uint32_t next_pc;       /* the one after it (a branch target when pc is a taken branch's delay slot) */
    uint32_t hi, lo;
    uint32_t cur_pc;        /* interpreter scratch */
    uint32_t ld_reg, ld_val; /* the load in flight: lands after the instruction at pc */
    uint32_t pend_reg, pend_val; /* interpreter scratch */
    bool in_delay;          /* interpreter scratch */
    bool next_delay;        /* pc is a branch delay slot */
    bool branch_taken;      /* interpreter scratch */
    bool next_taken;        /* that branch was taken */
    bool irq_pending;       /* SR.IEc && (SR.IM & CAUSE.IP): the run loop takes the interrupt before pc */
    uint32_t hook_at;       /* a jump went to a BIOS hook address (0xA0 / 0xB0 / 0x30000, physical) */
    uint32_t cop0[32];
    uint64_t cycles;
    uint64_t next_event;    /* the CPU runs until cycles reaches it */
    uint8_t *ram;           /* 2 MB main RAM */
    uint8_t *scratch;       /* 1 KB scratchpad */
    uint32_t *ram_gen;      /* write counter per 1 KB RAM page: lifted stores bump it like the core's */
    uint64_t rc_native;     /* instructions run natively (counted when the pack is built with -DPS1_RC_COUNT) */
    uint8_t rc_exit;        /* PS1_RC_EXIT_*: why lifted code returned */
    uint8_t rc_hint;        /* with PS1_RC_EXIT_INTERP: see below (0 before each call; older cores ignore it) */
} ps1_rc_head;

enum {
    PS1_RC_EXIT_NORMAL = 0,      /* the state says where execution continues */
    PS1_RC_EXIT_INTERP = 1,      /* the interpreter runs the instruction at pc, then dispatch goes on */
    PS1_RC_EXIT_NOT_COVERED = 2, /* this function does not hold pc: nothing ran */
};

/* rc_hint with PS1_RC_EXIT_INTERP: how many instructions, in sequence from pc, the interpreter may run before calling
 * lifted code again (the rest of a lifted block: no resume point inside); 0 = 1. The core stops counting as soon as
 * execution leaves the sequence (a taken branch, an interrupt). */

/* The core's side (ps1_recomp.c). Each returns 0, or the exception the access raises (EXC_* code) with nothing done
 * and no cycle spent: the lifted code then hands the instruction to the interpreter, which raises it. */
int ps1_rc_lw(ps1_machine *m, uint32_t addr, uint32_t *v);               /* LW / LWL / LWR / LWC2 off RAM: load cycles */
int ps1_rc_read(ps1_machine *m, uint32_t addr, int width, uint32_t *v);  /* LB / LH / LBU / LHU off RAM (zero-extended) */
int ps1_rc_write(ps1_machine *m, uint32_t addr, int width, uint32_t v);  /* SB / SH / SW / SWC2 off RAM */
int ps1_rc_swlr(ps1_machine *m, uint32_t addr, uint32_t rt, int right);  /* SWL (right = 0) / SWR anywhere */
int ps1_rc_gte_cmd(ps1_machine *m, uint32_t cmd);                        /* COP2 imm25: returns its cycles */
uint32_t ps1_rc_gte_read(ps1_machine *m, int r);                         /* r 0-31 data (MFC2), 32-63 control (CFC2) */
void ps1_rc_gte_write(ps1_machine *m, int r, uint32_t v);                /* MTC2 / LWC2, CTC2 */

/* ---- the shape of a lifted function (generated by ps1_tools.recomp) ----
 *
 *   RC_FN f(ps1_machine *m_) {
 *       RC_ENTER(cpi, plo, plen);
 *       uint32_t r2 = h->r[2], r29 = h->r[29];           (the registers it writes)
 *       const uint32_t r4 = h->r[4];                      (the ones it only reads)
 *       switch (h->pc) {
 *       case 0x80010000u: goto L_80010000;               (the resume points)
 *       default: { static const uint8_t mid_[] = {...}; RC_MID_TABLE(0x80010000u, mid_); RC_NOT_COVERED(); }
 *       }
 *   L_80010000:
 *       RC_SEG(0x80010000u, 2, 5);                       (a segment: its static cycles, instructions to the block end)
 *       RC_I(); ... one instruction ...
 *       RC_I(); ... a memory access ...
 *       RC_SEG(0x8001000Cu, 0, 3);                       (the next segment)
 *       ...
 *       RC_EXIT(end);
 *       RC_EPILOGUE();
 *       h->r[2] = r2; h->r[29] = r29;
 *   }
 *
 * cpi: cycles per instruction fetch (the interpreter's, by segment); plo / plen: the function's code in RAM (offset
 * in the 2 MB, bytes), for the self-modification check after stores. Guest registers live in locals, declared by the
 * lifted function after RC_ENTER: the ones it writes as `uint32_t r5 = h->r[5];`, written back after RC_EPILOGUE
 * (`h->r[5] = r5;`), the ones it only reads as `const uint32_t`, not written back (the core's functions never change
 * the CPU's registers); the same for hi / lo. `now` and `ne` (next event) live in locals too, synced around every call
 * into the core. A store that hit this function's own code, or an IO
 * access that left an interrupt pending, sets `ne` to 0: the check of the next segment (one follows every access)
 * leaves for the dispatcher. */
#define RC_ENTER(CPI, PLO, PLEN)                                                                                     \
    ps1_rc_head *const h = (ps1_rc_head *)(void *)m_;                                                                \
    enum { RC_CPI = (CPI) };                                                                                         \
    const uint32_t rc_plo = (PLO), rc_plen = (PLEN);                                                                 \
    uint8_t *const rc_ram = h->ram, *const rc_scratch = h->scratch;                                                  \
    uint32_t *const rc_gen = h->ram_gen;                                                                             \
    uint64_t now = h->cycles, ne = h->next_event;                                                                    \
    uint32_t epc = 0, enp = 0, tk = 0, jt = 0, ldv = 0, ehint = 0;                                                   \
    bool edl = false, etk = false;                                                                                   \
    (void)rc_plo;                                                                                                    \
    (void)rc_plen;                                                                                                   \
    (void)rc_ram;                                                                                                    \
    (void)rc_scratch;                                                                                                \
    (void)rc_gen

#define RC_M ((ps1_machine *)(void *)h)

#ifdef PS1_RC_COUNT
#define RC_COUNT() (h->rc_native++)
#define RC_UNCOUNT() (h->rc_native--)
#else
#define RC_COUNT() ((void)0)
#define RC_UNCOUNT() ((void)0)
#endif

/* Leaves for the dispatcher: next instruction PC, then NPC; DL = PC is a delay slot, TK = its branch was taken. */
#define RC_GO(PC, NPC, DL, TK)                                                                                       \
    do {                                                                                                             \
        epc = (PC);                                                                                                  \
        enp = (NPC);                                                                                                 \
        edl = (DL);                                                                                                  \
        etk = (TK);                                                                                                  \
        goto x_exit;                                                                                                 \
    } while (0)
/* The common exits share their tails in RC_EPILOGUE (small code: one store and a jump per exit). */
#define RC_EXIT(A)                                                                                                   \
    do {                                                                                                             \
        epc = (A);                                                                                                   \
        goto x_plain;                                                                                                \
    } while (0)
/* ... with the load of the previous instruction still in flight (register RT, value V). */
#define RC_EXIT_LD(A, RT, V)                                                                                         \
    do {                                                                                                             \
        h->ld_reg = (RT);                                                                                            \
        h->ld_val = (V);                                                                                             \
        RC_EXIT(A);                                                                                                  \
    } while (0)

/* Start of an instruction: its fetch. */
#define RC_I()                                                                                                       \
    do {                                                                                                             \
        now += RC_CPI;                                                                                               \
        RC_COUNT();                                                                                                  \
    } while (0)

/* The event check of a segment starting at A (ps1_tools lift.py « blocks and event checks »): CYC static cycles before
 * its last instruction, N instructions from A to the end of its block. Due now: leave for the dispatcher (the run
 * loop's own check, in front of A); due inside the segment: the interpreter runs from A (N instructions: rc_hint). */
#define RC_SEG(A, CYC, N)                                                                                            \
    do {                                                                                                             \
        if (__builtin_expect(now + (CYC) >= ne, 0)) {                                                                \
            epc = (A);                                                                                               \
            ehint = (N);                                                                                             \
            goto x_seg;                                                                                              \
        }                                                                                                            \
    } while (0)

/* Entered at pc, not a resume point: TAB[(pc - LO) / 4] is the number of instructions to the end of pc's block (0:
 * not one of this function's instructions): the interpreter runs them, nothing done here. */
#define RC_MID_TABLE(LO, TAB)                                                                                        \
    do {                                                                                                             \
        uint32_t o__ = h->pc - (LO), i__ = o__ >> 2;                                                                 \
        if (!(o__ & 3u) && i__ < sizeof(TAB) && (TAB)[i__]) {                                                        \
            h->rc_hint = (TAB)[i__];                                                                                 \
            h->rc_exit = PS1_RC_EXIT_INTERP;                                                                         \
            return;                                                                                                  \
        }                                                                                                            \
    } while (0)

/* The interpreter runs the instruction at A instead (its fetch is undone: the interpreter counts it). */
#define RC_INTERP(A)                                                                                                 \
    do {                                                                                                             \
        epc = (A);                                                                                                   \
        goto x_interp;                                                                                               \
    } while (0)
/* ... and the N - 1 instructions after it, the rest of its block (rc_hint). */
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
        h->rc_exit = PS1_RC_EXIT_NOT_COVERED;                                                                        \
        return;                                                                                                      \
    } while (0)

/* A jump to T sets the run loop's hook address when T is one (the interpreter's jump()). */
static inline bool rc_is_hook(uint32_t t)
{
    uint32_t p = t & 0x1FFFFFFFu;
    return p == 0xA0u || p == 0xB0u || p == 0x00030000u;
}
#define RC_HOOK(T)                                                                                                   \
    do {                                                                                                             \
        uint32_t t__ = (T);                                                                                          \
        if (__builtin_expect(rc_is_hook(t__), 0))                                                                    \
            h->hook_at = t__;                                                                                        \
    } while (0)

/* Calls into the core: the clock goes out and comes back with the next event; an IO access may raise an IRQ. */
#define RC_SYNC_OUT() (h->cycles = now)
#define RC_SYNC_IN() (now = h->cycles, ne = h->irq_pending ? 0 : h->next_event)

/* Memory accesses. A: the address; AL: its alignment mask (3 for a word, 1 for a halfword, 0 for a byte: a misaligned
 * address is an address error); FAULT: the statement that hands the instruction to the interpreter, which raises the
 * exception (written once: one exit per access). Main RAM inline (the fast test, alignment included), the scratchpad
 * inline, the rest through the core. RC_RAM_FAST's SEGS: the segments (A >> 29, a bit each) whose low 8 MB reach main
 * RAM for that access. rc_ram / rc_scratch / rc_gen: the head's pointers, in locals (RC_ENTER). */
#define RC_RAM_FAST(A, AL, SEGS) (((A) & (0x1F800000u | (AL))) == 0 && ((SEGS) >> ((A) >> 29) & 1u))

/* LW-class loads (the interpreter's load32): main RAM (any segment but KSEG2 / 1-3 holes, as the interpreter) costs 3
 * cycles, the scratchpad nothing, the rest through ps1_rc_lw. */
#define RC_LW(A, AL, OUT, FAULT)                                                                                     \
    do {                                                                                                             \
        uint32_t a_ = (A);                                                                                           \
        if (__builtin_expect(RC_RAM_FAST(a_, AL, 0x71u), 1)) {                                                       \
            memcpy(&(OUT), rc_ram + (a_ & 0x1FFFFCu), 4);                                                            \
            now += 3;                                                                                                \
        } else {                                                                                                     \
            uint32_t v_ = 0;                                                                                         \
            int e_ = (a_ & (AL)) != 0;                                                                               \
            if (e_) {                                                                                                \
            } else if ((a_ >> 29) != 7 && (a_ & 0x1FFFFC00u) == 0x1F800000u) {                                       \
                memcpy(&v_, rc_scratch + (a_ & 0x3FCu), 4);                                                          \
            } else {                                                                                                 \
                RC_SYNC_OUT();                                                                                       \
                e_ = ps1_rc_lw(RC_M, a_, &v_);                                                                       \
                RC_SYNC_IN();                                                                                        \
            }                                                                                                        \
            if (e_) {                                                                                                \
                FAULT;                                                                                               \
            }                                                                                                        \
            (OUT) = v_;                                                                                              \
        }                                                                                                            \
    } while (0)

/* LB / LH / LBU / LHU: the bus's decode (RAM in KUSEG / KSEG0 / KSEG1, scratchpad in KUSEG / KSEG0), no cycles.
 * T is the C type the value is read as (int8_t, uint16_t, ...); OUT gets it sign- or zero-extended. */
#define RC_LD(T, A, AL, OUT, FAULT)                                                                                  \
    do {                                                                                                             \
        uint32_t a_ = (A);                                                                                           \
        T t_;                                                                                                        \
        if (__builtin_expect(RC_RAM_FAST(a_, AL, 0x31u), 1)) {                                                       \
            memcpy(&t_, rc_ram + (a_ & 0x1FFFFFu), sizeof t_);                                                       \
        } else {                                                                                                     \
            uint32_t v_ = 0;                                                                                         \
            int e_ = (a_ & (AL)) != 0;                                                                               \
            if (e_) {                                                                                                \
            } else if ((0x11u >> (a_ >> 29) & 1u) && (a_ & 0x1FFFFC00u) == 0x1F800000u) {                            \
                memcpy(&t_, rc_scratch + (a_ & 0x3FFu), sizeof t_);                                                  \
                v_ = (uint32_t)t_;                                                                                   \
            } else {                                                                                                 \
                RC_SYNC_OUT();                                                                                       \
                e_ = ps1_rc_read(RC_M, a_, (int)sizeof t_, &v_);                                                     \
                RC_SYNC_IN();                                                                                        \
            }                                                                                                        \
            if (e_) {                                                                                                \
                FAULT;                                                                                               \
            }                                                                                                        \
            t_ = (T)v_;                                                                                              \
        }                                                                                                            \
        (OUT) = (uint32_t)(int32_t)t_;                                                                               \
    } while (0)

/* SB / SH / SW: RAM stores bump the page's write counter and stop after the instruction when they hit this
 * function's own code; the scratchpad inline; the rest through ps1_rc_write. */
#define RC_ST(T, A, AL, V, FAULT)                                                                                    \
    do {                                                                                                             \
        uint32_t a_ = (A);                                                                                           \
        T t_ = (T)(V);                                                                                               \
        if (__builtin_expect(RC_RAM_FAST(a_, AL, 0x31u), 1)) {                                                       \
            uint32_t o_ = a_ & 0x1FFFFFu;                                                                            \
            memcpy(rc_ram + o_, &t_, sizeof t_);                                                                     \
            rc_gen[o_ >> 10]++;                                                                                      \
            if (__builtin_expect(o_ - rc_plo < rc_plen, 0))                                                          \
                ne = 0;                                                                                              \
        } else {                                                                                                     \
            int e_ = (a_ & (AL)) != 0;                                                                               \
            if (e_) {                                                                                                \
            } else if ((0x11u >> (a_ >> 29) & 1u) && (a_ & 0x1FFFFC00u) == 0x1F800000u) {                            \
                memcpy(rc_scratch + (a_ & 0x3FFu), &t_, sizeof t_);                                                  \
            } else {                                                                                                 \
                RC_SYNC_OUT();                                                                                       \
                e_ = ps1_rc_write(RC_M, a_, (int)sizeof t_, (uint32_t)t_);                                           \
                RC_SYNC_IN();                                                                                        \
            }                                                                                                        \
            if (e_) {                                                                                                \
                FAULT;                                                                                               \
            }                                                                                                        \
        }                                                                                                            \
    } while (0)

/* RAM accesses (ps1_tools lift.py « memory accesses »: the address is not a known IO register): main RAM and the
 * scratchpad inline, as above; anything else (IO after all, an address or bus error, a store into this function's
 * own code) runs HAND, which gives the instruction to the interpreter, nothing of it done. No call into the core:
 * the clock and the next event only move by the static cycles. */
#define RC_SCRATCH_OK(A, SEGS) (((SEGS) >> ((A) >> 29) & 1u) && ((A) & 0x1FFFFC00u) == 0x1F800000u)
#define RC_LW_RAM(A, AL, OUT, HAND)                                                                                  \
    do {                                                                                                             \
        uint32_t a_ = (A);                                                                                           \
        if (__builtin_expect(RC_RAM_FAST(a_, AL, 0x71u), 1)) {                                                       \
            memcpy(&(OUT), rc_ram + (a_ & 0x1FFFFCu), 4);                                                            \
            now += 3;                                                                                                \
        } else if (!(a_ & (AL)) && RC_SCRATCH_OK(a_, 0x7Fu)) {                                                       \
            memcpy(&(OUT), rc_scratch + (a_ & 0x3FCu), 4);                                                           \
        } else {                                                                                                     \
            HAND;                                                                                                    \
        }                                                                                                            \
    } while (0)
#define RC_LD_RAM(T, A, AL, OUT, HAND)                                                                               \
    do {                                                                                                             \
        uint32_t a_ = (A);                                                                                           \
        T t_;                                                                                                        \
        if (__builtin_expect(RC_RAM_FAST(a_, AL, 0x31u), 1)) {                                                       \
            memcpy(&t_, rc_ram + (a_ & 0x1FFFFFu), sizeof t_);                                                       \
        } else if (!(a_ & (AL)) && RC_SCRATCH_OK(a_, 0x11u)) {                                                       \
            memcpy(&t_, rc_scratch + (a_ & 0x3FFu), sizeof t_);                                                      \
        } else {                                                                                                     \
            HAND;                                                                                                    \
        }                                                                                                            \
        (OUT) = (uint32_t)(int32_t)t_;                                                                               \
    } while (0)
#define RC_ST_RAM(T, A, AL, V, HAND)                                                                                 \
    do {                                                                                                             \
        uint32_t a_ = (A), o_ = a_ & 0x1FFFFFu;                                                                      \
        T t_ = (T)(V);                                                                                               \
        if (__builtin_expect(RC_RAM_FAST(a_, AL, 0x31u) && o_ - rc_plo >= rc_plen, 1)) {                             \
            memcpy(rc_ram + o_, &t_, sizeof t_);                                                                     \
            rc_gen[o_ >> 10]++;                                                                                      \
        } else if (!(a_ & (AL)) && RC_SCRATCH_OK(a_, 0x11u)) {                                                       \
            memcpy(rc_scratch + (a_ & 0x3FFu), &t_, sizeof t_);                                                      \
        } else {                                                                                                     \
            HAND;                                                                                                    \
        }                                                                                                            \
    } while (0)

/* SWL / SWR: always through the core (rare); may modify code, so stop after them. */
#define RC_SWLR(A, V, RIGHT, FAULT)                                                                                  \
    do {                                                                                                             \
        RC_SYNC_OUT();                                                                                               \
        int e_ = ps1_rc_swlr(RC_M, (A), (V), (RIGHT));                                                               \
        RC_SYNC_IN();                                                                                                \
        if (e_) {                                                                                                    \
            FAULT;                                                                                                   \
        }                                                                                                            \
        ne = 0;                                                                                                      \
    } while (0)

/* LWL / LWR merge: CUR the register as the instruction sees it (the load in flight forwarded), W the aligned word. */
static inline uint32_t rc_lwl(uint32_t cur, uint32_t w, uint32_t a)
{
    uint32_t sh = a & 3;
    return sh == 3 ? w : (cur & (0x00FFFFFFu >> (sh * 8))) | (w << (24 - sh * 8));
}
static inline uint32_t rc_lwr(uint32_t cur, uint32_t w, uint32_t a)
{
    uint32_t sh = a & 3;
    return sh == 0 ? w : (cur & (0xFFFFFF00u << (24 - sh * 8))) | (w >> (sh * 8));
}

/* DIV / DIVU with the R3000A's results for a zero divisor and 0x80000000 / -1. */
static inline void rc_div(uint32_t a, uint32_t b, uint32_t *hi, uint32_t *lo)
{
    int32_t n = (int32_t)a, d = (int32_t)b;
    if (d == 0) {
        *hi = (uint32_t)n;
        *lo = n >= 0 ? 0xFFFFFFFFu : 1u;
    } else if (a == 0x80000000u && d == -1) {
        *hi = 0;
        *lo = 0x80000000u;
    } else {
        *lo = (uint32_t)(n / d);
        *hi = (uint32_t)(n % d);
    }
}
static inline void rc_divu(uint32_t n, uint32_t d, uint32_t *hi, uint32_t *lo)
{
    if (d == 0) {
        *hi = n;
        *lo = 0xFFFFFFFFu;
    } else {
        *lo = n / d;
        *hi = n % d;
    }
}

/* COP2 usable (SR.CU2); SR cannot change inside lifted code (MTC0 / RFE / exceptions all leave it). */
#define RC_CU2() ((h->cop0[12] & 0x40000000u) != 0)

#define RC_EPILOGUE()                                                                                                \
    x_seg: __attribute__((unused));                                                                                  \
    if (now < ne) {                                                                                                  \
        h->rc_exit = PS1_RC_EXIT_INTERP;                                                                             \
        h->rc_hint = (uint8_t)ehint;                                                                                 \
    }                                                                                                                \
    goto x_plain;                                                                                                    \
    x_interp_slot: __attribute__((unused));                                                                          \
    now -= RC_CPI;                                                                                                   \
    RC_UNCOUNT();                                                                                                    \
    h->rc_exit = PS1_RC_EXIT_INTERP;                                                                                 \
    edl = true;                                                                                                      \
    goto x_exit;                                                                                                     \
    x_hand: __attribute__((unused));                                                                                 \
    h->rc_hint = (uint8_t)ehint;                                                                                     \
    x_interp: __attribute__((unused));                                                                               \
    now -= RC_CPI;                                                                                                   \
    RC_UNCOUNT();                                                                                                    \
    h->rc_exit = PS1_RC_EXIT_INTERP;                                                                                 \
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

/* Marks the functions of a pack (hidden: only the table function is exported). */
#define RC_FN __attribute__((visibility("hidden"))) void
#define RC_EXPORT __attribute__((visibility("default")))

#ifdef __cplusplus
}
#endif

#endif
