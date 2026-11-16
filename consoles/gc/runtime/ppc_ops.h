/* ppc_ops.h -- PowerPC semantics shared by the interpreter and lifted code (XBA-225).
 *
 * Console-agnostic: 32-bit PowerPC user and supervisor instructions as the 750 (Gekko, Broadway) runs them, with no
 * Gekko extension (paired singles live in gc_recomp_rt.h) and no memory access. The lifter (tools/ppc/lift.py)
 * emits calls to these helpers by name, and the interpreter (gc_cpu.c) runs the same ones, so both agree bit for
 * bit. The Xbox 360 (XBA-229: Xenon, 64-bit) is meant to reuse this file once it has a runtime; it would add the
 * 64-bit forms (rld*, mulld, cmpd...) next to these, not change them. */
#ifndef PPC_OPS_H
#define PPC_OPS_H

#include <math.h>
#include <stdint.h>
#include <string.h>

/* CR bit numbers count from the most significant bit (PowerPC numbering): field f's LT is bit 4f. */
#define PPC_CR_LT 8u
#define PPC_CR_GT 4u
#define PPC_CR_EQ 2u
#define PPC_CR_SO 1u

#define PPC_MSR_EE 0x8000u
#define PPC_MSR_PR 0x4000u
#define PPC_MSR_FP 0x2000u
#define PPC_MSR_ME 0x1000u
#define PPC_MSR_IP 0x0040u
#define PPC_MSR_IR 0x0020u
#define PPC_MSR_DR 0x0010u
#define PPC_MSR_RI 0x0002u

/* SPR numbers the core and lifted code know by name. */
enum {
    PPC_SPR_XER = 1, PPC_SPR_LR = 8, PPC_SPR_CTR = 9, PPC_SPR_DSISR = 18, PPC_SPR_DAR = 19, PPC_SPR_DEC = 22,
    PPC_SPR_SRR0 = 26, PPC_SPR_SRR1 = 27, PPC_SPR_TBL_R = 268, PPC_SPR_TBU_R = 269, PPC_SPR_TBL_W = 284,
    PPC_SPR_TBU_W = 285, PPC_SPR_HID0 = 1008, PPC_SPR_HID2 = 920, PPC_SPR_GQR0 = 912,
};

static inline uint32_t ppc_rotl(uint32_t v, unsigned n)
{
    n &= 31;
    return n ? (v << n) | (v >> (32 - n)) : v;
}

/* The rlw* mask from bit mb to bit me (wrapping when mb > me). */
static inline uint32_t ppc_mask(unsigned mb, unsigned me)
{
    uint32_t a = 0xFFFFFFFFu >> mb, b = me == 31 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> (me + 1));
    return mb <= me ? (a & b) : (a | b);
}

/* A CR field (4 bits, LT GT EQ SO) from a signed / unsigned compare. */
static inline uint32_t ppc_cmps(int32_t a, int32_t b, uint32_t so)
{
    return (a < b ? PPC_CR_LT : a > b ? PPC_CR_GT : PPC_CR_EQ) | (so & 1);
}

static inline uint32_t ppc_cmpu(uint32_t a, uint32_t b, uint32_t so)
{
    return (a < b ? PPC_CR_LT : a > b ? PPC_CR_GT : PPC_CR_EQ) | (so & 1);
}

/* CR with field f replaced by v (4 bits). */
static inline uint32_t ppc_cr_set(uint32_t cr, unsigned f, uint32_t v)
{
    unsigned sh = 28 - 4 * f;
    return (cr & ~(0xFu << sh)) | ((v & 15u) << sh);
}

static inline uint32_t ppc_cr_bit(uint32_t cr, unsigned bit)
{
    return (cr >> (31 - bit)) & 1u;
}

static inline uint32_t ppc_cr_setbit(uint32_t cr, unsigned bit, uint32_t v)
{
    return (cr & ~(1u << (31 - bit))) | ((v & 1u) << (31 - bit));
}

/* Carry out of a + b + cin. */
static inline uint32_t ppc_carry(uint32_t a, uint32_t b, uint32_t cin)
{
    uint64_t s = (uint64_t)a + b + cin;
    return (uint32_t)(s >> 32);
}

/* Signed overflow of a + b + cin (the sum is r). */
static inline uint32_t ppc_ovf_add(uint32_t a, uint32_t b, uint32_t r)
{
    return ((~(a ^ b) & (a ^ r)) >> 31) & 1u;
}

static inline uint32_t ppc_srawi(uint32_t v, unsigned sh, uint32_t *ca)
{
    sh &= 31;
    uint32_t r = (uint32_t)((int32_t)v >> sh);
    *ca = ((int32_t)v < 0 && sh && (v & ((1u << sh) - 1))) ? 1u : 0u;
    return r;
}

static inline uint32_t ppc_sraw(uint32_t v, uint32_t rb, uint32_t *ca)
{
    if (rb & 0x20) {
        *ca = (int32_t)v < 0 ? 1u : 0u;
        return (int32_t)v < 0 ? 0xFFFFFFFFu : 0;
    }
    return ppc_srawi(v, rb & 31, ca);
}

static inline uint32_t ppc_slw(uint32_t v, uint32_t rb)
{
    return (rb & 0x20) ? 0 : v << (rb & 31);
}

static inline uint32_t ppc_srw(uint32_t v, uint32_t rb)
{
    return (rb & 0x20) ? 0 : v >> (rb & 31);
}

static inline uint32_t ppc_cntlzw(uint32_t v)
{
    return v ? (uint32_t)__builtin_clz(v) : 32u;
}

/* divw / divwu: the 750 leaves rD undefined on a zero divisor or 0x80000000 / -1; these are the values the 750
 * gives (and Dolphin documents): 0 or -1 by the dividend's sign, 0 for divwu. */
static inline uint32_t ppc_divw(uint32_t a, uint32_t b)
{
    if (b == 0 || (a == 0x80000000u && b == 0xFFFFFFFFu))
        return (int32_t)a < 0 ? 0xFFFFFFFFu : 0;
    return (uint32_t)((int32_t)a / (int32_t)b);
}

static inline uint32_t ppc_divwu(uint32_t a, uint32_t b)
{
    return b ? a / b : 0;
}

static inline uint32_t ppc_mulhw(uint32_t a, uint32_t b)
{
    return (uint32_t)(((int64_t)(int32_t)a * (int32_t)b) >> 32);
}

static inline uint32_t ppc_mulhwu(uint32_t a, uint32_t b)
{
    return (uint32_t)(((uint64_t)a * b) >> 32);
}

/* ---- floating point: FPRs hold doubles; bit views by memcpy ---- */

static inline uint64_t ppc_f2u(double d)
{
    uint64_t u;
    memcpy(&u, &d, 8);
    return u;
}

static inline double ppc_u2f(uint64_t u)
{
    double d;
    memcpy(&d, &u, 8);
    return d;
}

static inline double ppc_lfs_bits(uint32_t bits)
{
    float f;
    memcpy(&f, &bits, 4);
    return (double)f;
}

static inline uint32_t ppc_stfs_bits(double d)
{
    float f = (float)d;
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static inline double ppc_single(double d)
{
    return (double)(float)d;
}

/* fcmpu / fcmpo: a CR field (FL FG FE FU). */
static inline uint32_t ppc_fcmp(double a, double b)
{
    if (a != a || b != b)
        return 1u;
    return a < b ? PPC_CR_LT : a > b ? PPC_CR_GT : PPC_CR_EQ;
}

/* fctiw (round to nearest even: the FPSCR rounding mode is not modelled) / fctiwz, with the 750's saturation;
 * the FPR gets 0xFFF80000 in its high word, as on hardware. */
static inline double ppc_fctiw(double d, int toward_zero)
{
    int32_t v;
    if (d != d)
        v = (int32_t)0x80000000u;
    else if (d >= 2147483647.5)
        v = 0x7FFFFFFF;
    else if (d < -2147483648.0)
        v = (int32_t)0x80000000u;
    else {
        double r = toward_zero ? trunc(d) : nearbyint(d);
        if (r > 2147483647.0)
            v = 0x7FFFFFFF;
        else if (r < -2147483648.0)
            v = (int32_t)0x80000000u;
        else
            v = (int32_t)r;
    }
    return ppc_u2f(0xFFF8000000000000ull | (uint32_t)v);
}

static inline double ppc_fsel(double a, double c, double b)
{
    return a >= 0.0 ? c : b;
}

static inline double ppc_fneg(double d)
{
    return ppc_u2f(ppc_f2u(d) ^ 0x8000000000000000ull);
}

static inline double ppc_fabs(double d)
{
    return ppc_u2f(ppc_f2u(d) & 0x7FFFFFFFFFFFFFFFull);
}

static inline double ppc_fnabs(double d)
{
    return ppc_u2f(ppc_f2u(d) | 0x8000000000000000ull);
}

/* fres / frsqrte: estimates on hardware; exact here (a spike limit: games that depend on the 750's estimate tables
 * will differ in low bits). */
static inline double ppc_fres(double d)
{
    return ppc_single(1.0 / d);
}

static inline double ppc_frsqrte(double d)
{
    return 1.0 / sqrt(d);
}

#endif
