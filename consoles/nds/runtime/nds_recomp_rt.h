/* nds_recomp_rt.h -- the ABI between lifted DS ARM9 code (consoles/nds/tools nds_tools.recomp) and the core (XBA-223
 * spike). Part of NDS_RECOMP_ABI (nds_recomp.h): change it together with the tools and bump NDS_RECOMP_ABI.
 *
 * The lifter is gba_tools' (consoles/gba/tools), extended to ARMv5TE (nds_tools/lift.py): it emits the RC_* macro
 * vocabulary of consoles/gba/runtime/gba_recomp_rt.h, and this header gives those macros their DS meaning. Same shape
 * (registers, flags and the clock in C locals, synced around bus calls; blocks with one event check per segment;
 * calls, returns and indirect jumps back to the dispatcher with PC set; rare instructions handed to the interpreter
 * with NDS_RC_EXIT_INTERP), different machine:
 * - no cycle accuracy: one cycle per instruction (RC_I), the GBA's wait-state terms are tables of zeros, no
 *   cartridge prefetch buffer, no open bus;
 * - main RAM (4 MB at 0x02000000, the first mirror only) is accessed inline, everything else through the core's bus
 *   (nds_read8 / 16 / 32, nds_write8 / 16 / 32), DTCM and ITCM included;
 * - long multiplies with S leave C alone (ARMv5), the Q flag lives in cpu.q.
 * The host executable (nds_run) exports the bus functions for the pack to bind to. */
#ifndef NDS_RECOMP_RT_H
#define NDS_RECOMP_RT_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "lifted DS code assumes a little-endian host"
#endif

#include "nds_recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nds_cpu {
    uint32_t r[16];         /* current bank; between instructions r[15] = next instruction + 8 (ARM) / + 4 (Thumb) */
    uint32_t n, z, c, v;    /* flags, 0 or 1 */
    uint32_t cpsr_ctl;      /* CPSR bits 0-7 (mode, T, F, I); flags live in n z c v q */
    uint32_t q;             /* sticky saturation flag (CPSR bit 27), 0 or 1 */
    uint32_t bank_r13[6], bank_r14[6], spsr[6];
    uint32_t fiq_r8_12[5], usr_r8_12[5];
    bool halted;            /* CP15 wait for interrupt */
} nds_cpu;

typedef struct nds_rc_head {
    nds_cpu cpu;
    uint64_t now;           /* cycles (instructions) since creation */
    uint64_t next_event;    /* the CPU runs until now reaches it */
    uint8_t wait16[16][2];  /* all zero: the GBA lifter's wait-state terms */
    uint8_t wait32[16][2];
    bool irq_line;          /* IME && (IE & IF) */
    uint8_t rc_exit;        /* NDS_RC_EXIT_* */
    uint8_t rc_hint;        /* with NDS_RC_EXIT_INTERP: instructions the interpreter may run before native again */
    uint8_t pad_;
    uint64_t rc_native;     /* instructions run natively (packs built with -DNDS_RC_COUNT) */
    uint8_t *rc_main;       /* main RAM, 4 MB */
} nds_rc_head;

enum {
    NDS_RC_EXIT_NORMAL = 0,      /* PC (and T) say where execution continues */
    NDS_RC_EXIT_INTERP = 1,      /* the interpreter runs the instruction(s) at PC, then dispatch goes on */
    NDS_RC_EXIT_NOT_COVERED = 2, /* this function does not hold PC: nothing ran */
};

/* The core's bus (nds_bus.c): the CPU c's view (ARM9 or ARM7). */
uint8_t nds_read8(nds_arm *c, uint32_t addr);
uint16_t nds_read16(nds_arm *c, uint32_t addr);
uint32_t nds_read32(nds_arm *c, uint32_t addr);
void nds_write8(nds_arm *c, uint32_t addr, uint8_t v);
void nds_write16(nds_arm *c, uint32_t addr, uint16_t v);
void nds_write32(nds_arm *c, uint32_t addr, uint32_t v);

/* ---- helpers for lifted code (the interpreter's semantics, nds_cpu.c) ---- */

static inline uint32_t rc_ror(uint32_t v, unsigned n)
{
    n &= 31;
    return n ? (v >> n) | (v << (32 - n)) : v;
}

static inline int rc_mul_cycles(uint32_t rs, int sign)
{
    (void)rs;
    (void)sign;
    return 1;
}

/* ARMv5: UMULLS / SMULLS / UMLALS / SMLALS leave C unchanged (the lifter writes `fC = rc_mull_carry(...)`). */
#define rc_mull_carry(RM, RS, SIGN) ((void)(RM), (void)(RS), fC)

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

static inline uint32_t rc_clz(uint32_t v)
{
    return v ? (uint32_t)__builtin_clz(v) : 32u;
}

/* Saturating add / subtract (QADD / QSUB); *q set on saturation. */
static inline int32_t rc_qadd(int32_t a, int32_t b, uint32_t *q)
{
    int64_t s = (int64_t)a + b;
    if (s > INT32_MAX) {
        *q = 1;
        return INT32_MAX;
    }
    if (s < INT32_MIN) {
        *q = 1;
        return INT32_MIN;
    }
    return (int32_t)s;
}

static inline int32_t rc_qsub(int32_t a, int32_t b, uint32_t *q)
{
    int64_t s = (int64_t)a - b;
    if (s > INT32_MAX) {
        *q = 1;
        return INT32_MAX;
    }
    if (s < INT32_MIN) {
        *q = 1;
        return INT32_MIN;
    }
    return (int32_t)s;
}

/* SMLAxy / SMLAWy: a wrapping add that sets Q on signed overflow. */
static inline int32_t rc_qadd_flag(int32_t a, int32_t b, uint32_t *q)
{
    uint32_t s = (uint32_t)a + (uint32_t)b;
    if (((uint32_t)a ^ s) & ((uint32_t)b ^ s) & 0x80000000u)
        *q = 1;
    return (int32_t)s;
}

/* ---- the shape of a lifted function (gba_tools' lifter, nds_tools' subclass) ---- */

#define RC_ENTER(T, REGION)                                                                                          \
    nds_rc_head *const h = (nds_rc_head *)(void *)m_;                                                                \
    enum { RC_T = (T), RC_PIPE = (T) ? 4 : 8 };                                                                      \
    uint32_t r0 = h->cpu.r[0], r1 = h->cpu.r[1], r2 = h->cpu.r[2], r3 = h->cpu.r[3], r4 = h->cpu.r[4];             \
    uint32_t r5 = h->cpu.r[5], r6 = h->cpu.r[6], r7 = h->cpu.r[7], r8 = h->cpu.r[8], r9 = h->cpu.r[9];             \
    uint32_t r10 = h->cpu.r[10], r11 = h->cpu.r[11], r12 = h->cpu.r[12], r13 = h->cpu.r[13], r14 = h->cpu.r[14];    \
    uint32_t fN = h->cpu.n, fZ = h->cpu.z, fC = h->cpu.c, fV = h->cpu.v;                                             \
    uint64_t now = h->now, ne = h->next_event;                                                                       \
    const uint32_t fb = 1;                                                                                           \
    uint32_t epc = 0;                                                                                                \
    (void)(REGION)

#define RC_PC() (h->cpu.r[15] - RC_PIPE)
#define RC_M ((nds_arm *)(void *)h)

#ifdef NDS_RC_COUNT
#define RC_COUNT() (h->rc_native++)
#else
#define RC_COUNT() ((void)0)
#endif

#define RC_I(A)                                                                                                      \
    do {                                                                                                             \
        now += 1;                                                                                                    \
        RC_COUNT();                                                                                                  \
    } while (0)

#define RC_STOP(A, NFC, EXTRA, N)                                                                                    \
    do {                                                                                                             \
        if (__builtin_expect(now + (uint32_t)(NFC) * fb + (uint32_t)(EXTRA) >= ne, 0)) {                             \
            epc = (A);                                                                                               \
            if (now >= ne)                                                                                           \
                goto x_exit;                                                                                         \
            h->rc_hint = (N);                                                                                        \
            goto x_interp;                                                                                           \
        }                                                                                                            \
    } while (0)

#define RC_CHECK(A, NFC, EXTRA, N)                                                                                   \
    do {                                                                                                             \
        if (__builtin_expect(now + (uint32_t)(NFC) * fb + (uint32_t)(EXTRA) >= ne, 0)) {                             \
            epc = (A);                                                                                               \
            h->rc_hint = (N);                                                                                        \
            goto x_interp;                                                                                           \
        }                                                                                                            \
    } while (0)

#define RC_MID(N)                                                                                                    \
    do {                                                                                                             \
        h->rc_hint = (N);                                                                                            \
        epc = RC_PC();                                                                                               \
        goto x_interp;                                                                                               \
    } while (0)

#define RC_EXIT(A)                                                                                                   \
    do {                                                                                                             \
        epc = (A);                                                                                                   \
        goto x_exit;                                                                                                 \
    } while (0)

#define RC_INTERP(A)                                                                                                 \
    do {                                                                                                             \
        epc = (A);                                                                                                   \
        goto x_interp;                                                                                               \
    } while (0)

#define RC_MID_TABLE(LO, TAB)                                                                                        \
    do {                                                                                                             \
        uint32_t o__ = RC_PC() - (LO), i__ = o__ >> (RC_T ? 1 : 2);                                                  \
        if (!(o__ & (RC_T ? 1u : 3u)) && i__ < sizeof(TAB) && (TAB)[i__])                                            \
            RC_MID((TAB)[i__]);                                                                                      \
    } while (0)

#define RC_NOT_COVERED()                                                                                             \
    do {                                                                                                             \
        h->rc_exit = NDS_RC_EXIT_NOT_COVERED;                                                                        \
        return;                                                                                                      \
    } while (0)

/* Bus accesses. OB (the GBA's open bus) is always 0 here. */
#define RC_SYNC_OUT(OB) (h->now = now, (void)(OB))
#define RC_SYNC_IN() (now = h->now, ne = h->next_event)
#define RC_IS_RAM(A) (((A) >> 22) == 8u) /* 0x02000000-0x023FFFFF */
#define RC_RD(BITS, ALIGN, A, OB)                                                                                    \
    ({                                                                                                               \
        uint32_t a__ = (A), rv__;                                                                                    \
        if (__builtin_expect(RC_IS_RAM(a__), 1)) {                                                                   \
            uint##BITS##_t t__;                                                                                      \
            memcpy(&t__, h->rc_main + (a__ & 0x3FFFFFu & ~(ALIGN)), sizeof t__);                                     \
            rv__ = t__;                                                                                              \
        } else {                                                                                                     \
            RC_SYNC_OUT(OB);                                                                                         \
            rv__ = nds_read##BITS(RC_M, a__);                                                                        \
            RC_SYNC_IN();                                                                                            \
        }                                                                                                            \
        rv__;                                                                                                        \
    })
#define RC_WR(BITS, ALIGN, A, V, OB)                                                                                 \
    do {                                                                                                             \
        uint32_t a__ = (A);                                                                                          \
        uint##BITS##_t t__ = (uint##BITS##_t)(V);                                                                    \
        if (__builtin_expect(RC_IS_RAM(a__), 1)) {                                                                   \
            memcpy(h->rc_main + (a__ & 0x3FFFFFu & ~(ALIGN)), &t__, sizeof t__);                                     \
        } else {                                                                                                     \
            RC_SYNC_OUT(OB);                                                                                         \
            nds_write##BITS(RC_M, a__, t__);                                                                         \
            RC_SYNC_IN();                                                                                            \
            RC_AFTER_BUS_WRITE();                                                                                    \
        }                                                                                                            \
    } while (0)
#define RD8(A, SEQ, OB) RC_RD(8, 0u, A, OB)
#define RD16(A, SEQ, OB) RC_RD(16, 1u, A, OB)
#define RD32(A, SEQ, OB) RC_RD(32, 3u, A, OB)
#define WR8(A, V, SEQ, OB) RC_WR(8, 0u, A, V, OB)
#define WR16(A, V, SEQ, OB) RC_WR(16, 1u, A, V, OB)
#define WR32(A, V, SEQ, OB) RC_WR(32, 3u, A, V, OB)

#define RC_RUN_RAM(A, N) (RC_IS_RAM(A) && RC_IS_RAM((A) + 4u * ((N) - 1)))
#define RC_RUN_WAITS(A, N) ((void)0)
#define RC_RUN_BASE(A)                                                                                               \
    uint8_t *rb__ = h->rc_main;                                                                                      \
    const uint32_t rm__ = 0x3FFFFCu
#define RC_FRD32(A)                                                                                                  \
    ({                                                                                                               \
        uint32_t t__;                                                                                                \
        memcpy(&t__, rb__ + ((A) & rm__), 4);                                                                        \
        t__;                                                                                                         \
    })
#define RC_FWR32(A, V)                                                                                               \
    do {                                                                                                             \
        uint32_t t__ = (V);                                                                                          \
        memcpy(rb__ + ((A) & rm__), &t__, 4);                                                                        \
    } while (0)
#define RC_BRD32(A, SEQ, OB)                                                                                         \
    ({                                                                                                               \
        RC_SYNC_OUT(OB);                                                                                             \
        uint32_t t__ = nds_read32(RC_M, (A));                                                                        \
        RC_SYNC_IN();                                                                                                \
        t__;                                                                                                         \
    })
#define RC_BWR32(A, V, SEQ, OB)                                                                                      \
    do {                                                                                                             \
        RC_SYNC_OUT(OB);                                                                                             \
        nds_write32(RC_M, (A), (V));                                                                                 \
        RC_SYNC_IN();                                                                                                \
        RC_AFTER_BUS_WRITE();                                                                                        \
    } while (0)

/* After a bus write: an IRQ may be pending or the CPU halted: the local next event drops to 0. */
#define RC_AFTER_BUS_WRITE()                                                                                         \
    do {                                                                                                             \
        ne = ((h->irq_line & !(h->cpu.cpsr_ctl & 0x80u)) | h->cpu.halted) ? 0 : ne;                                  \
    } while (0)

#define RC_NEXTRA() 0u
#define RC_REFILL(R15) ((void)0)
#define RC_LIT(W) ((uint32_t)(W))

#define RC_JUMP_DYN(T_)                                                                                              \
    do {                                                                                                             \
        uint32_t t__ = (T_) & (RC_T ? ~1u : ~3u);                                                                     \
        RC_EXIT(t__);                                                                                                 \
    } while (0)
#define RC_JUMP_LOCAL(A, LABEL) goto LABEL

#define RC_BX(T_)                                                                                                    \
    do {                                                                                                             \
        uint32_t t__ = (T_);                                                                                          \
        if (t__ & 1) {                                                                                                \
            h->cpu.cpsr_ctl |= 0x20u;                                                                                \
            h->cpu.r[15] = (t__ & ~1u) + 4;                                                                           \
        } else {                                                                                                     \
            h->cpu.cpsr_ctl &= ~0x20u;                                                                               \
            h->cpu.r[15] = (t__ & ~3u) + 8;                                                                           \
        }                                                                                                            \
        goto x_flush;                                                                                                \
    } while (0)

#define RC_EPILOGUE()                                                                                                \
    x_interp: __attribute__((unused));                                                                               \
    h->rc_exit = NDS_RC_EXIT_INTERP;                                                                                 \
    x_exit: __attribute__((unused));                                                                                 \
    h->cpu.r[15] = epc + RC_PIPE;                                                                                    \
    x_flush: __attribute__((unused));                                                                                \
    h->cpu.r[0] = r0; h->cpu.r[1] = r1; h->cpu.r[2] = r2; h->cpu.r[3] = r3; h->cpu.r[4] = r4;                     \
    h->cpu.r[5] = r5; h->cpu.r[6] = r6; h->cpu.r[7] = r7; h->cpu.r[8] = r8; h->cpu.r[9] = r9;                     \
    h->cpu.r[10] = r10; h->cpu.r[11] = r11; h->cpu.r[12] = r12; h->cpu.r[13] = r13; h->cpu.r[14] = r14;            \
    h->cpu.n = fN; h->cpu.z = fZ; h->cpu.c = fC; h->cpu.v = fV;                                                     \
    h->now = now

#define RC_FN __attribute__((visibility("hidden"))) void
#define RC_EXPORT __attribute__((visibility("default")))

#ifdef __cplusplus
}
#endif

#endif
