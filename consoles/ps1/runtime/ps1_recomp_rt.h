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
 * loads the same load cycles, at the same point; before each instruction it checks `cycles < next_event`, as the run
 * loop does; after an access that left main RAM (IO: it may raise an interrupt) it checks the IRQ line. When it must
 * stop (scheduler event, pending IRQ, a call, a return, an indirect jump, a jump out of the function: the dispatcher
 * picks the next function), it writes the registers back with pc / next_pc / the delay-slot flags exactly as the
 * interpreter would hold them between two instructions, and returns.
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
 * with a load in flight, in kernel mode with the cache not isolated (it interprets otherwise). The function resumes at
 * that instruction; if pc is not one of its own it returns at once with rc_exit = PS1_RC_EXIT_NOT_COVERED. */
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
} ps1_rc_head;

enum {
    PS1_RC_EXIT_NORMAL = 0,      /* the state says where execution continues */
    PS1_RC_EXIT_INTERP = 1,      /* the interpreter runs the instruction at pc, then dispatch goes on */
    PS1_RC_EXIT_NOT_COVERED = 2, /* this function does not hold pc: nothing ran */
};

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
 *       switch (h->pc) { case 0x80010000u: goto L_80010000; ... default: RC_NOT_COVERED(); }
 *   L_80010000: RC_I(0x80010000u); ... one instruction ...
 *       ...
 *       RC_EXIT(end);
 *       RC_EPILOGUE();
 *   }
 *
 * cpi: cycles per instruction fetch (the interpreter's, by segment); plo / plen: the function's code in RAM (offset
 * in the 2 MB, bytes), for the self-modification check after stores. Guest registers live in locals; `now` and `ne`
 * (next event) too, synced around every call into the core; `chk` asks to stop after the current instruction
 * (a store hit this function's own code, or an IO access left an interrupt pending). */
#define RC_ENTER(CPI, PLO, PLEN)                                                                                     \
    ps1_rc_head *const h = (ps1_rc_head *)(void *)m_;                                                                \
    enum { RC_CPI = (CPI) };                                                                                         \
    const uint32_t rc_plo = (PLO), rc_plen = (PLEN);                                                                 \
    uint32_t r1 = h->r[1], r2 = h->r[2], r3 = h->r[3], r4 = h->r[4], r5 = h->r[5], r6 = h->r[6], r7 = h->r[7];      \
    uint32_t r8 = h->r[8], r9 = h->r[9], r10 = h->r[10], r11 = h->r[11], r12 = h->r[12], r13 = h->r[13];           \
    uint32_t r14 = h->r[14], r15 = h->r[15], r16 = h->r[16], r17 = h->r[17], r18 = h->r[18], r19 = h->r[19];       \
    uint32_t r20 = h->r[20], r21 = h->r[21], r22 = h->r[22], r23 = h->r[23], r24 = h->r[24], r25 = h->r[25];       \
    uint32_t r26 = h->r[26], r27 = h->r[27], r28 = h->r[28], r29 = h->r[29], r30 = h->r[30], r31 = h->r[31];       \
    uint32_t hi = h->hi, lo = h->lo;                                                                                 \
    uint64_t now = h->cycles, ne = h->next_event;                                                                    \
    uint32_t epc = 0, enp = 0, tk = 0, jt = 0, ldv = 0, chk = 0;                                                     \
    bool edl = false, etk = false;                                                                                   \
    (void)rc_plo;                                                                                                    \
    (void)rc_plen

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

/* Start of the instruction at A: stop in front of it when the scheduler is due (the interpreter's loop test), then
 * its fetch. RC_I_SLOT: the same for a delay slot (NPC: where its branch goes, TK: taken). */
#define RC_I(A)                                                                                                      \
    do {                                                                                                             \
        if (__builtin_expect(now >= ne, 0))                                                                          \
            RC_EXIT(A);                                                                                              \
        now += RC_CPI;                                                                                               \
        RC_COUNT();                                                                                                  \
    } while (0)
#define RC_I_SLOT(A, NPC, TK)                                                                                        \
    do {                                                                                                             \
        if (__builtin_expect(now >= ne, 0))                                                                          \
            RC_GO((A), (NPC), true, (TK));                                                                           \
        now += RC_CPI;                                                                                               \
        RC_COUNT();                                                                                                  \
    } while (0)

/* The interpreter runs the instruction at A instead (its fetch is undone: the interpreter counts it). */
#define RC_INTERP(A)                                                                                                 \
    do {                                                                                                             \
        epc = (A);                                                                                                   \
        goto x_interp;                                                                                               \
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
#define RC_SYNC_IN() (now = h->cycles, ne = h->next_event, chk |= h->irq_pending)

/* LW-class loads (the interpreter's load32): main RAM (any segment but KSEG2 / 1-3 holes, as the interpreter) costs 3
 * cycles, the scratchpad nothing, the rest through ps1_rc_lw. A is 4-aligned. FAULT runs when the access raises. */
#define RC_LW(A, OUT, FAULT)                                                                                         \
    do {                                                                                                             \
        uint32_t a_ = (A), s_ = a_ >> 29, p_ = a_ & 0x1FFFFFFFu;                                                     \
        if (__builtin_expect(p_ < 0x00800000u && (s_ == 0 || (s_ >= 4 && s_ != 7)), 1)) {                            \
            memcpy(&(OUT), h->ram + (p_ & 0x1FFFFCu), 4);                                                            \
            now += 3;                                                                                                \
        } else if (s_ != 7 && (p_ & 0xFFFFFC00u) == 0x1F800000u) {                                                   \
            memcpy(&(OUT), h->scratch + (p_ & 0x3FCu), 4);                                                           \
        } else {                                                                                                     \
            uint32_t v_ = 0;                                                                                         \
            RC_SYNC_OUT();                                                                                           \
            int e_ = ps1_rc_lw(RC_M, a_, &v_);                                                                       \
            RC_SYNC_IN();                                                                                            \
            if (e_) {                                                                                                \
                FAULT;                                                                                               \
            }                                                                                                        \
            (OUT) = v_;                                                                                              \
        }                                                                                                            \
    } while (0)

/* LB / LH / LBU / LHU: the bus's decode (RAM in KUSEG / KSEG0 / KSEG1, scratchpad in KUSEG / KSEG0), no cycles.
 * T is the C type the value is read as (int8_t, uint16_t, ...); OUT gets it sign- or zero-extended. */
#define RC_LD(T, A, OUT, FAULT)                                                                                      \
    do {                                                                                                             \
        uint32_t a_ = (A), s_ = a_ >> 29, p_ = a_ & 0x1FFFFFFFu;                                                     \
        T t_;                                                                                                        \
        if (__builtin_expect(p_ < 0x00800000u && (s_ == 0 || s_ == 4 || s_ == 5), 1)) {                              \
            memcpy(&t_, h->ram + (p_ & 0x1FFFFFu), sizeof t_);                                                       \
        } else if ((s_ == 0 || s_ == 4) && (p_ & 0xFFFFFC00u) == 0x1F800000u) {                                      \
            memcpy(&t_, h->scratch + (p_ & 0x3FFu), sizeof t_);                                                      \
        } else {                                                                                                     \
            uint32_t v_ = 0;                                                                                         \
            RC_SYNC_OUT();                                                                                           \
            int e_ = ps1_rc_read(RC_M, a_, (int)sizeof t_, &v_);                                                     \
            RC_SYNC_IN();                                                                                            \
            if (e_) {                                                                                                \
                FAULT;                                                                                               \
            }                                                                                                        \
            t_ = (T)v_;                                                                                              \
        }                                                                                                            \
        (OUT) = (uint32_t)(int32_t)t_;                                                                               \
    } while (0)

/* SB / SH / SW: RAM stores bump the page's write counter and stop after the instruction when they hit this
 * function's own code; the scratchpad inline; the rest through ps1_rc_write. */
#define RC_ST(T, A, V, FAULT)                                                                                        \
    do {                                                                                                             \
        uint32_t a_ = (A), s_ = a_ >> 29, p_ = a_ & 0x1FFFFFFFu;                                                     \
        T t_ = (T)(V);                                                                                               \
        if (__builtin_expect(p_ < 0x00800000u && (s_ == 0 || s_ == 4 || s_ == 5), 1)) {                              \
            uint32_t o_ = p_ & 0x1FFFFFu;                                                                            \
            memcpy(h->ram + o_, &t_, sizeof t_);                                                                     \
            h->ram_gen[o_ >> 10]++;                                                                                  \
            if (__builtin_expect(o_ - rc_plo < rc_plen, 0))                                                          \
                chk = 1;                                                                                             \
        } else if ((s_ == 0 || s_ == 4) && (p_ & 0xFFFFFC00u) == 0x1F800000u) {                                      \
            memcpy(h->scratch + (p_ & 0x3FFu), &t_, sizeof t_);                                                      \
        } else {                                                                                                     \
            RC_SYNC_OUT();                                                                                           \
            int e_ = ps1_rc_write(RC_M, a_, (int)sizeof t_, (uint32_t)t_);                                           \
            RC_SYNC_IN();                                                                                            \
            if (e_) {                                                                                                \
                FAULT;                                                                                               \
            }                                                                                                        \
        }                                                                                                            \
    } while (0)

/* SWL / SWR: always through the core (rare); may modify code, so stop after them when they touched RAM. */
#define RC_SWLR(A, V, RIGHT, FAULT)                                                                                  \
    do {                                                                                                             \
        RC_SYNC_OUT();                                                                                               \
        int e_ = ps1_rc_swlr(RC_M, (A), (V), (RIGHT));                                                               \
        RC_SYNC_IN();                                                                                                \
        if (e_) {                                                                                                    \
            FAULT;                                                                                                   \
        }                                                                                                            \
        chk = 1;                                                                                                     \
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
    x_interp_slot: __attribute__((unused));                                                                          \
    now -= RC_CPI;                                                                                                   \
    RC_UNCOUNT();                                                                                                    \
    h->rc_exit = PS1_RC_EXIT_INTERP;                                                                                 \
    edl = true;                                                                                                      \
    goto x_exit;                                                                                                     \
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
    h->r[1] = r1; h->r[2] = r2; h->r[3] = r3; h->r[4] = r4; h->r[5] = r5; h->r[6] = r6; h->r[7] = r7;              \
    h->r[8] = r8; h->r[9] = r9; h->r[10] = r10; h->r[11] = r11; h->r[12] = r12; h->r[13] = r13; h->r[14] = r14;    \
    h->r[15] = r15; h->r[16] = r16; h->r[17] = r17; h->r[18] = r18; h->r[19] = r19; h->r[20] = r20;                \
    h->r[21] = r21; h->r[22] = r22; h->r[23] = r23; h->r[24] = r24; h->r[25] = r25; h->r[26] = r26;                \
    h->r[27] = r27; h->r[28] = r28; h->r[29] = r29; h->r[30] = r30; h->r[31] = r31;                                \
    h->hi = hi;                                                                                                      \
    h->lo = lo;                                                                                                      \
    h->cycles = now;                                                                                                 \
    (void)tk; (void)jt; (void)ldv; (void)chk

/* Marks the functions of a pack (hidden: only the table function is exported). */
#define RC_FN __attribute__((visibility("hidden"))) void
#define RC_EXPORT __attribute__((visibility("default")))

#ifdef __cplusplus
}
#endif

#endif
