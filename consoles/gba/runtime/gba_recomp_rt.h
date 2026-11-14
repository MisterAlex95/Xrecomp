/* gba_recomp_rt.h -- the ABI between lifted GBA code (consoles/gba/tools gba_tools.recomp) and the core
 * (XBA-151 / XBA-152). Part of GBA_RECOMP_ABI (gba_recomp.h): change this layout, these macros' meaning or the
 * symbols below and bump GBA_RECOMP_ABI.
 *
 * Lifted code sees the machine through gba_rc_head, the first members of gba_machine (gba_internal.h checks every
 * offset with static asserts): the CPU registers, the cycle clock and the next scheduler event, the wait-state
 * tables, the IRQ line. Memory goes through the core's bus functions, which add the same wait states as for the
 * interpreter. The host executable (gba_run, gba_play, the Android shell's libmain.so) must export these symbols
 * for the pack to bind to: gba_read8, gba_read16, gba_read32, gba_write8, gba_write16, gba_write32.
 *
 * Lifted code is cycle-exact with the interpreter (gba_cpu.c): every instruction costs the same fetch, data and
 * internal cycles, in the same order, and before each instruction the lifted code checks `now < next_event`, as the
 * interpreter's run loop does; after a store it checks the IRQ line and HALTCNT. When it must stop (scheduler
 * event, pending IRQ, an indirect jump or a call: the dispatcher picks the next function), it writes the registers
 * back, sets r15 (+ 8 / + 4, the interpreter's convention) and returns. Rare or mode-changing instructions (SWI,
 * MSR, exception returns, LDM/STM with the S bit, SWP, undefined encodings) are run by the interpreter: the lifted
 * code returns with rc_exit = GBA_RC_EXIT_INTERP and PC at that instruction.
 *
 * Entry: the dispatcher calls a table function with PC at any address. The function resumes at that instruction
 * when it lifted it (every instruction is a resume point: events can stop lifted code anywhere and IRQ handlers
 * return anywhere); otherwise it returns at once with rc_exit = GBA_RC_EXIT_NOT_COVERED and nothing changed. */
#ifndef GBA_RECOMP_RT_H
#define GBA_RECOMP_RT_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "lifted GBA code assumes a little-endian host"
#endif

#include "gba_recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gba_cpu {
    uint32_t r[16];         /* current bank; between instructions r[15] = next instruction + 8 (ARM) / + 4 (Thumb) */
    uint32_t n, z, c, v;    /* flags, 0 or 1 */
    uint32_t cpsr_ctl;      /* CPSR bits 0-7 (mode, T, F, I); flags live in n z c v */
    uint32_t bank_r13[6], bank_r14[6], spsr[6];
    uint32_t fiq_r8_12[5], usr_r8_12[5];
    bool branched;          /* set by an instruction that wrote r15 */
    bool halted;
    bool stopped;
} gba_cpu;

typedef struct gba_rc_head {
    gba_cpu cpu;
    uint64_t now;           /* cycles since creation */
    uint64_t next_event;    /* the CPU runs until now reaches it */
    uint8_t wait16[16][2];  /* cycles of one 8/16-bit access [region][seq] */
    uint8_t wait32[16][2];  /* cycles of one 32-bit access [region][seq] */
    uint8_t fetch16[16];    /* cycles of a sequential Thumb opcode fetch per region (cartridge prefetch included) */
    uint8_t fetch32[16];    /* the same for ARM opcodes */
    bool prefetch;
    bool irq_line;          /* IME && (IE & IF) */
    uint8_t rc_exit;        /* GBA_RC_EXIT_*: why lifted code returned */
    uint8_t rc_reserved;
    uint32_t open_bus;      /* last fetched opcode (what unmapped reads return) */
    uint64_t rc_native;     /* instructions run natively (counted when the pack is built with -DGBA_RC_COUNT) */
    uint8_t *rc_ewram;      /* the machine's EWRAM (256 KB) and IWRAM (32 KB): lifted code reads and writes them */
    uint8_t *rc_iwram;      /* directly, with the bus's wait states (no side effect there) */
} gba_rc_head;

enum {
    GBA_RC_EXIT_NORMAL = 0,      /* PC (and T) say where execution continues */
    GBA_RC_EXIT_INTERP = 1,      /* the interpreter runs the instruction at PC, then dispatch goes on */
    GBA_RC_EXIT_NOT_COVERED = 2, /* this function does not hold PC: nothing ran */
};

/* The core's bus (gba_bus.c): the access's wait states are added to now; seq = sequential access. */
uint8_t gba_read8(gba_machine *m, uint32_t addr, bool seq);
uint16_t gba_read16(gba_machine *m, uint32_t addr, bool seq);
uint32_t gba_read32(gba_machine *m, uint32_t addr, bool seq);
void gba_write8(gba_machine *m, uint32_t addr, uint8_t v, bool seq);
void gba_write16(gba_machine *m, uint32_t addr, uint16_t v, bool seq);
void gba_write32(gba_machine *m, uint32_t addr, uint32_t v, bool seq);

/* ---- helpers for lifted code (the interpreter's semantics, gba_cpu.c) ---- */

static inline uint32_t rc_ror(uint32_t v, unsigned n)
{
    n &= 31;
    return n ? (v >> n) | (v << (32 - n)) : v;
}

/* Internal cycles of a multiply by rs. */
static inline int rc_mul_cycles(uint32_t rs, int sign)
{
    if (sign && (int32_t)rs < 0)
        rs = ~rs;
    if ((rs >> 8) == 0)
        return 1;
    if ((rs >> 16) == 0)
        return 2;
    if ((rs >> 24) == 0)
        return 3;
    return 4;
}

/* Shift by a register's bottom byte (amt = 0..255); cin = C. */
static inline uint32_t rc_shift_reg(uint32_t type, uint32_t v, uint32_t amt, uint32_t cin, uint32_t *carry)
{
    if (amt == 0) {
        *carry = cin;
        return v;
    }
    switch (type) {
    case 0:
        if (amt < 32) {
            *carry = (v >> (32 - amt)) & 1;
            return v << amt;
        }
        *carry = amt == 32 ? (v & 1) : 0;
        return 0;
    case 1:
        if (amt < 32) {
            *carry = (v >> (amt - 1)) & 1;
            return v >> amt;
        }
        *carry = amt == 32 ? (v >> 31) : 0;
        return 0;
    case 2:
        if (amt < 32) {
            *carry = (v >> (amt - 1)) & 1;
            return (uint32_t)((int32_t)v >> amt);
        }
        *carry = v >> 31;
        return (uint32_t)((int32_t)v >> 31);
    default:
        amt &= 31;
        if (amt == 0) {
            *carry = v >> 31;
            return v;
        }
        *carry = (v >> (amt - 1)) & 1;
        return rc_ror(v, amt);
    }
}

/* ---- the shape of a lifted function (generated by gba_tools.recomp) ----
 *
 *   void f(gba_machine *m_) {
 *       RC_ENTER(thumb, region);
 *       switch (RC_PC()) { case 0x08000100: goto L_08000100; ... default: RC_NOT_COVERED(); }
 *   L_08000100: RC_I(0x08000100); ... one instruction ...
 *       ...
 *       RC_EPILOGUE();
 *   }
 *
 * Guest registers and flags live in locals between bus calls; `now` and `ne` (next event) too, synced around every
 * call; `fc` is the opcode fetch cost, reloaded after stores (WAITCNT). */
#define RC_ENTER(T, REGION)                                                                                          \
    gba_rc_head *const h = (gba_rc_head *)(void *)m_;                                                                \
    enum { RC_T = (T), RC_PIPE = (T) ? 4 : 8, RC_REGION = (REGION) };                                                \
    uint32_t r0 = h->cpu.r[0], r1 = h->cpu.r[1], r2 = h->cpu.r[2], r3 = h->cpu.r[3], r4 = h->cpu.r[4];             \
    uint32_t r5 = h->cpu.r[5], r6 = h->cpu.r[6], r7 = h->cpu.r[7], r8 = h->cpu.r[8], r9 = h->cpu.r[9];             \
    uint32_t r10 = h->cpu.r[10], r11 = h->cpu.r[11], r12 = h->cpu.r[12], r13 = h->cpu.r[13], r14 = h->cpu.r[14];    \
    uint32_t fN = h->cpu.n, fZ = h->cpu.z, fC = h->cpu.c, fV = h->cpu.v;                                             \
    uint64_t now = h->now, ne = h->next_event;                                                                       \
    uint32_t fc = RC_FETCH_COST();                                                                                   \
    uint32_t epc = 0

#define RC_FETCH_COST() ((uint32_t)(RC_T ? h->fetch16[RC_REGION] : h->fetch32[RC_REGION]))
#define RC_PC() (h->cpu.r[15] - RC_PIPE)
#define RC_M ((gba_machine *)(void *)h)

#ifdef GBA_RC_COUNT
#define RC_COUNT() (h->rc_native++)
#else
#define RC_COUNT() ((void)0)
#endif

/* Start of an instruction at A: stop before it when the scheduler is due (the interpreter's loop test), then its
 * opcode fetch. */
#define RC_I(A)                                                                                                      \
    do {                                                                                                             \
        if (__builtin_expect(now >= ne, 0)) {                                                                        \
            epc = (A);                                                                                               \
            goto x_exit;                                                                                             \
        }                                                                                                            \
        now += fc;                                                                                                   \
        RC_COUNT();                                                                                                  \
    } while (0)

/* Leaves for the dispatcher at A (same state). */
#define RC_EXIT(A)                                                                                                   \
    do {                                                                                                             \
        epc = (A);                                                                                                   \
        goto x_exit;                                                                                                 \
    } while (0)

/* The interpreter runs the instruction at A (before its fetch: the interpreter counts it). */
#define RC_INTERP(A)                                                                                                 \
    do {                                                                                                             \
        epc = (A);                                                                                                   \
        goto x_interp;                                                                                               \
    } while (0)

#define RC_NOT_COVERED()                                                                                             \
    do {                                                                                                             \
        h->rc_exit = GBA_RC_EXIT_NOT_COVERED;                                                                        \
        return;                                                                                                      \
    } while (0)

/* Bus accesses; OB is this instruction's opcode as the fetch left it in the open-bus latch. */
#define RC_SYNC_OUT(OB) (h->now = now, h->open_bus = (OB))
#define RC_SYNC_IN() (now = h->now, ne = h->next_event)
/* EWRAM / IWRAM inline (what gba_bus.c does for them: the wait states of the region, the address masked and
 * aligned), everything else through the core's bus functions. */
static inline uint8_t *rc_ram(gba_rc_head *h, uint32_t a, uint32_t align)
{
    switch (a >> 24) {
    case 2: return h->rc_ewram + (a & 0x3FFFFu & ~align);
    case 3: return h->rc_iwram + (a & 0x7FFFu & ~align);
    default: return 0;
    }
}
#define RC_RD(BITS, W, ALIGN, A, SEQ, OB)                                                                            \
    ({                                                                                                               \
        uint32_t a__ = (A), rv__;                                                                                    \
        uint8_t *p__ = rc_ram(h, a__, ALIGN);                                                                        \
        if (__builtin_expect(p__ != 0, 1)) {                                                                         \
            uint##BITS##_t t__;                                                                                      \
            now += h->W[a__ >> 24][SEQ];                                                                             \
            memcpy(&t__, p__, sizeof t__);                                                                           \
            rv__ = t__;                                                                                              \
        } else {                                                                                                     \
            RC_SYNC_OUT(OB);                                                                                         \
            rv__ = gba_read##BITS(RC_M, a__, (SEQ));                                                                 \
            RC_SYNC_IN();                                                                                            \
        }                                                                                                            \
        rv__;                                                                                                        \
    })
#define RC_WR(BITS, W, ALIGN, A, V, SEQ, OB)                                                                         \
    do {                                                                                                             \
        uint32_t a__ = (A);                                                                                          \
        uint##BITS##_t t__ = (uint##BITS##_t)(V);                                                                    \
        uint8_t *p__ = rc_ram(h, a__, ALIGN);                                                                        \
        if (__builtin_expect(p__ != 0, 1)) {                                                                         \
            now += h->W[a__ >> 24][SEQ];                                                                             \
            memcpy(p__, &t__, sizeof t__);                                                                           \
        } else {                                                                                                     \
            RC_SYNC_OUT(OB);                                                                                         \
            gba_write##BITS(RC_M, a__, t__, (SEQ));                                                                  \
            RC_SYNC_IN();                                                                                            \
        }                                                                                                            \
    } while (0)
#define RD8(A, SEQ, OB) RC_RD(8, wait16, 0u, A, SEQ, OB)
#define RD16(A, SEQ, OB) RC_RD(16, wait16, 1u, A, SEQ, OB)
#define RD32(A, SEQ, OB) RC_RD(32, wait32, 3u, A, SEQ, OB)
#define WR8(A, V, SEQ, OB) RC_WR(8, wait16, 0u, A, V, SEQ, OB)
#define WR16(A, V, SEQ, OB) RC_WR(16, wait16, 1u, A, V, SEQ, OB)
#define WR32(A, V, SEQ, OB) RC_WR(32, wait32, 3u, A, V, SEQ, OB)

/* After a store instruction: WAITCNT may have changed, an IRQ may be pending, HALTCNT may have stopped the CPU. */
#define RC_AFTER_STORE(NEXT)                                                                                         \
    do {                                                                                                             \
        fc = RC_FETCH_COST();                                                                                        \
        if (__builtin_expect((h->irq_line && !(h->cpu.cpsr_ctl & 0x80u)) || h->cpu.halted || h->cpu.stopped, 0))   \
            RC_EXIT(NEXT);                                                                                           \
    } while (0)

/* Pipeline refill after a write to PC whose new r15 (address + 8 / + 4) is R15, in the current state. */
#define RC_REFILL(R15)                                                                                               \
    do {                                                                                                             \
        uint32_t rg__ = ((R15) >> 24) & 15;                                                                           \
        now += RC_T ? (uint32_t)h->wait16[rg__][0] + h->wait16[rg__][1] : (uint32_t)h->wait32[rg__][0] + h->wait32[rg__][1]; \
    } while (0)

/* Write to PC without a state change (B, BL, data ops, LDR / LDM / POP to PC): refill, then go on at the target. */
#define RC_JUMP_DYN(T_)                                                                                              \
    do {                                                                                                             \
        uint32_t t__ = (T_) & (RC_T ? ~1u : ~3u);                                                                     \
        RC_REFILL(t__ + RC_PIPE);                                                                                     \
        RC_EXIT(t__);                                                                                                 \
    } while (0)
#define RC_JUMP_LOCAL(A, LABEL)                                                                                      \
    do {                                                                                                             \
        RC_REFILL((uint32_t)(A) + RC_PIPE);                                                                          \
        goto LABEL;                                                                                                  \
    } while (0)

/* BX: bit 0 picks the state. */
#define RC_BX(T_)                                                                                                    \
    do {                                                                                                             \
        uint32_t t__ = (T_);                                                                                          \
        if (t__ & 1) {                                                                                                \
            h->cpu.cpsr_ctl |= 0x20u;                                                                                \
            h->cpu.r[15] = (t__ & ~1u) + 4;                                                                           \
            uint32_t rg__ = (h->cpu.r[15] >> 24) & 15;                                                                \
            now += (uint32_t)h->wait16[rg__][0] + h->wait16[rg__][1];                                                  \
        } else {                                                                                                     \
            h->cpu.cpsr_ctl &= ~0x20u;                                                                               \
            h->cpu.r[15] = (t__ & ~3u) + 8;                                                                           \
            uint32_t rg__ = (h->cpu.r[15] >> 24) & 15;                                                                \
            now += (uint32_t)h->wait32[rg__][0] + h->wait32[rg__][1];                                                  \
        }                                                                                                            \
        goto x_flush;                                                                                                \
    } while (0)

#define RC_EPILOGUE()                                                                                                \
    x_interp: __attribute__((unused));                                                                               \
    h->rc_exit = GBA_RC_EXIT_INTERP;                                                                                 \
    x_exit: __attribute__((unused));                                                                                 \
    h->cpu.r[15] = epc + RC_PIPE;                                                                                    \
    x_flush: __attribute__((unused));                                                                                \
    h->cpu.r[0] = r0; h->cpu.r[1] = r1; h->cpu.r[2] = r2; h->cpu.r[3] = r3; h->cpu.r[4] = r4;                     \
    h->cpu.r[5] = r5; h->cpu.r[6] = r6; h->cpu.r[7] = r7; h->cpu.r[8] = r8; h->cpu.r[9] = r9;                     \
    h->cpu.r[10] = r10; h->cpu.r[11] = r11; h->cpu.r[12] = r12; h->cpu.r[13] = r13; h->cpu.r[14] = r14;            \
    h->cpu.n = fN; h->cpu.z = fZ; h->cpu.c = fC; h->cpu.v = fV;                                                     \
    h->now = now

/* Marks the functions of a pack (hidden: only the table function is exported). */
#define RC_FN __attribute__((visibility("hidden"))) void
#define RC_EXPORT __attribute__((visibility("default")))

#ifdef __cplusplus
}
#endif

#endif
