/* ps2_alu.h -- R5900 operations shared by the interpreter (ps2_cpu.c) and lifted code (ps2_recomp_rt.h), so both
 * compute the same bits (XBA-226).
 *
 * GPRs are 128 bits: the low 64 (what every non-MMI instruction reads and writes; 32-bit results are sign-extended)
 * and the high 64 (only MMI, LQ / SQ, PMTHI ... touch them). HI / LO are 128 bits too: their high halves are HI1 /
 * LO1, which the pipeline-1 multiplies and divides write.
 *
 * The FPU is not IEEE 754 (R5900 core manual, « FPU »): there are no infinities, NaNs or denormals. Exponent 255 is
 * an ordinary exponent (0x7F800000 is 2^128, the largest magnitude is 0x7FFFFFFF); a denormal operand reads as zero;
 * a result too large becomes +-0x7FFFFFFF, one too small +-0; every operation rounds toward zero. Registers hold the
 * bit patterns (uint32_t); each operation widens its operands to double (exact: a PS2 float always fits), computes,
 * and narrows with truncation and clamping. Not modelled: the adder's limited guard bits (an addition of operands
 * far apart in exponent can differ from the hardware in the last bit), the sticky flags (O, U, I, D) in FCR31. */
#ifndef PS2_ALU_H
#define PS2_ALU_H

#include <stdint.h>
#include <string.h>

typedef struct ps2_q {
    uint64_t lo, hi;
} ps2_q;

#define PS2_SX32(x) ((uint64_t)(int64_t)(int32_t)(uint32_t)(x))

static inline ps2_q ps2_q_make(uint64_t lo, uint64_t hi)
{
    ps2_q q = {lo, hi};
    return q;
}

/* ---- integer ---------------------------------------------------------------------------------------------------- */

/* MULT / MULTU (and the pipeline-1 forms): LO / HI get the sign-extended halves of the 64-bit product. */
static inline void ps2_mult(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo)
{
    int64_t p = (int64_t)(int32_t)a * (int64_t)(int32_t)b;
    *lo = PS2_SX32((uint32_t)p);
    *hi = PS2_SX32((uint32_t)((uint64_t)p >> 32));
}
static inline void ps2_multu(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo)
{
    uint64_t p = (uint64_t)(uint32_t)a * (uint64_t)(uint32_t)b;
    *lo = PS2_SX32((uint32_t)p);
    *hi = PS2_SX32((uint32_t)(p >> 32));
}
/* MADD / MADDU: HI:LO (their low words) += the product. */
static inline void ps2_madd(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo)
{
    uint64_t acc = ((uint64_t)(uint32_t)*hi << 32) | (uint32_t)*lo;
    acc += (uint64_t)((int64_t)(int32_t)a * (int64_t)(int32_t)b);
    *lo = PS2_SX32((uint32_t)acc);
    *hi = PS2_SX32((uint32_t)(acc >> 32));
}
static inline void ps2_maddu(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo)
{
    uint64_t acc = ((uint64_t)(uint32_t)*hi << 32) | (uint32_t)*lo;
    acc += (uint64_t)(uint32_t)a * (uint64_t)(uint32_t)b;
    *lo = PS2_SX32((uint32_t)acc);
    *hi = PS2_SX32((uint32_t)(acc >> 32));
}
/* DIV / DIVU: a zero divisor and 0x80000000 / -1 give the MIPS results (no trap; compilers add TEQ). */
static inline void ps2_div(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo)
{
    int32_t n = (int32_t)a, d = (int32_t)b;
    if (d == 0) {
        *hi = PS2_SX32((uint32_t)n);
        *lo = n >= 0 ? ~0ull : 1u;
    } else if ((uint32_t)n == 0x80000000u && d == -1) {
        *hi = 0;
        *lo = PS2_SX32(0x80000000u);
    } else {
        *lo = PS2_SX32((uint32_t)(n / d));
        *hi = PS2_SX32((uint32_t)(n % d));
    }
}
static inline void ps2_divu(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo)
{
    uint32_t n = (uint32_t)a, d = (uint32_t)b;
    if (d == 0) {
        *hi = PS2_SX32(n);
        *lo = ~0ull;
    } else {
        *lo = PS2_SX32(n / d);
        *hi = PS2_SX32(n % d);
    }
}

/* PLZCW: for each word of the low doubleword, the leading bits equal to its sign bit, minus one. */
static inline uint64_t ps2_plzcw(uint64_t v)
{
    uint64_t out = 0;
    for (int k = 0; k < 2; k++) {
        uint32_t w = (uint32_t)(v >> (32 * k));
        if (w & 0x80000000u)
            w = ~w;
        uint32_t n = w ? (uint32_t)__builtin_clz(w) : 32u;
        out |= (uint64_t)(n - 1) << (32 * k);
    }
    return out;
}

/* LWL / LWR / LDL / LDR / SWL / SWR / SDL / SDR, little-endian. mem: the aligned word / doubleword at addr & ~3 (~7).
 * Loads return the new register value (32-bit forms: LWL sign-extends the whole word it builds, LWR only when it
 * loads the word's top byte, as on the R4000); stores the new memory value. */
static inline uint64_t ps2_lwl(uint64_t rt, uint32_t mem, uint32_t addr)
{
    unsigned sh = (3 - (addr & 3)) * 8;
    uint32_t keep = sh == 32 ? 0 : (uint32_t)((1ull << sh) - 1);
    return PS2_SX32(((uint32_t)rt & keep) | (mem << sh));
}
static inline uint64_t ps2_lwr(uint64_t rt, uint32_t mem, uint32_t addr)
{
    unsigned sh = (addr & 3) * 8;
    if (sh == 0)
        return PS2_SX32(mem);
    uint32_t keep = ~(0xFFFFFFFFu >> sh);
    return (rt & 0xFFFFFFFF00000000ull) | (((uint32_t)rt & keep) | (mem >> sh));
}
static inline uint64_t ps2_ldl(uint64_t rt, uint64_t mem, uint32_t addr)
{
    unsigned sh = (7 - (addr & 7)) * 8;
    uint64_t keep = sh ? (1ull << sh) - 1 : 0;
    return (rt & keep) | (mem << sh);
}
static inline uint64_t ps2_ldr(uint64_t rt, uint64_t mem, uint32_t addr)
{
    unsigned sh = (addr & 7) * 8;
    uint64_t keep = sh ? ~(~0ull >> sh) : 0;
    return (rt & keep) | (mem >> sh);
}
static inline uint32_t ps2_swl(uint64_t rt, uint32_t mem, uint32_t addr)
{
    unsigned sh = (3 - (addr & 3)) * 8;
    uint32_t keep = (addr & 3) == 3 ? 0 : ~(0xFFFFFFFFu >> sh);
    return (mem & keep) | ((uint32_t)rt >> sh);
}
static inline uint32_t ps2_swr(uint64_t rt, uint32_t mem, uint32_t addr)
{
    unsigned sh = (addr & 3) * 8;
    uint32_t keep = sh ? (uint32_t)((1ull << sh) - 1) : 0;
    return (mem & keep) | ((uint32_t)rt << sh);
}
static inline uint64_t ps2_sdl(uint64_t rt, uint64_t mem, uint32_t addr)
{
    unsigned sh = (7 - (addr & 7)) * 8;
    uint64_t keep = (addr & 7) == 7 ? 0 : ~(~0ull >> sh);
    return (mem & keep) | (rt >> sh);
}
static inline uint64_t ps2_sdr(uint64_t rt, uint64_t mem, uint32_t addr)
{
    unsigned sh = (addr & 7) * 8;
    uint64_t keep = sh ? (1ull << sh) - 1 : 0;
    return (mem & keep) | (rt << sh);
}

/* ---- MMI: rd = op(rs, rt) over lanes ----------------------------------------------------------------------------- */

#define PS2_LANES(T, N)                                                                                              \
    union {                                                                                                          \
        ps2_q q;                                                                                                     \
        T v[N];                                                                                                      \
    }

#define PS2_MMI_LANEWISE(NAME, T, N, EXPR)                                                                           \
    static inline ps2_q ps2_mmi_##NAME(ps2_q s_, ps2_q t_)                                                           \
    {                                                                                                                \
        PS2_LANES(T, N) s, t, d;                                                                                     \
        s.q = s_;                                                                                                    \
        t.q = t_;                                                                                                    \
        for (int k = 0; k < (N); k++) {                                                                              \
            T a = s.v[k], b = t.v[k];                                                                                \
            (void)a;                                                                                                 \
            (void)b;                                                                                                 \
            d.v[k] = (T)(EXPR);                                                                                      \
        }                                                                                                            \
        return d.q;                                                                                                  \
    }

static inline int64_t ps2_sat(int64_t v, int64_t lo, int64_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

PS2_MMI_LANEWISE(paddw, uint32_t, 4, a + b)
PS2_MMI_LANEWISE(psubw, uint32_t, 4, a - b)
PS2_MMI_LANEWISE(pcgtw, int32_t, 4, a > b ? -1 : 0)
PS2_MMI_LANEWISE(pmaxw, int32_t, 4, a > b ? a : b)
PS2_MMI_LANEWISE(paddh, uint16_t, 8, a + b)
PS2_MMI_LANEWISE(psubh, uint16_t, 8, a - b)
PS2_MMI_LANEWISE(pcgth, int16_t, 8, a > b ? -1 : 0)
PS2_MMI_LANEWISE(pmaxh, int16_t, 8, a > b ? a : b)
PS2_MMI_LANEWISE(paddb, uint8_t, 16, a + b)
PS2_MMI_LANEWISE(psubb, uint8_t, 16, a - b)
PS2_MMI_LANEWISE(pcgtb, int8_t, 16, a > b ? -1 : 0)
PS2_MMI_LANEWISE(paddsw, int32_t, 4, ps2_sat((int64_t)a + b, INT32_MIN, INT32_MAX))
PS2_MMI_LANEWISE(psubsw, int32_t, 4, ps2_sat((int64_t)a - b, INT32_MIN, INT32_MAX))
PS2_MMI_LANEWISE(paddsh, int16_t, 8, ps2_sat((int64_t)a + b, INT16_MIN, INT16_MAX))
PS2_MMI_LANEWISE(psubsh, int16_t, 8, ps2_sat((int64_t)a - b, INT16_MIN, INT16_MAX))
PS2_MMI_LANEWISE(paddsb, int8_t, 16, ps2_sat((int64_t)a + b, INT8_MIN, INT8_MAX))
PS2_MMI_LANEWISE(psubsb, int8_t, 16, ps2_sat((int64_t)a - b, INT8_MIN, INT8_MAX))
PS2_MMI_LANEWISE(pabsw, int32_t, 4, b == INT32_MIN ? INT32_MAX : b < 0 ? -b : b)
PS2_MMI_LANEWISE(pceqw, uint32_t, 4, a == b ? 0xFFFFFFFFu : 0)
PS2_MMI_LANEWISE(pminw, int32_t, 4, a < b ? a : b)
PS2_MMI_LANEWISE(pabsh, int16_t, 8, b == INT16_MIN ? INT16_MAX : b < 0 ? -b : b)
PS2_MMI_LANEWISE(pceqh, uint16_t, 8, a == b ? 0xFFFFu : 0)
PS2_MMI_LANEWISE(pminh, int16_t, 8, a < b ? a : b)
PS2_MMI_LANEWISE(pceqb, uint8_t, 16, a == b ? 0xFFu : 0)
PS2_MMI_LANEWISE(padduw, uint32_t, 4, ps2_sat((int64_t)a + b, 0, UINT32_MAX))
PS2_MMI_LANEWISE(psubuw, uint32_t, 4, ps2_sat((int64_t)a - b, 0, UINT32_MAX))
PS2_MMI_LANEWISE(padduh, uint16_t, 8, ps2_sat((int64_t)a + b, 0, UINT16_MAX))
PS2_MMI_LANEWISE(psubuh, uint16_t, 8, ps2_sat((int64_t)a - b, 0, UINT16_MAX))
PS2_MMI_LANEWISE(paddub, uint8_t, 16, ps2_sat((int64_t)a + b, 0, UINT8_MAX))
PS2_MMI_LANEWISE(psubub, uint8_t, 16, ps2_sat((int64_t)a - b, 0, UINT8_MAX))

static inline ps2_q ps2_mmi_pand(ps2_q s, ps2_q t)
{
    return ps2_q_make(s.lo & t.lo, s.hi & t.hi);
}
static inline ps2_q ps2_mmi_por(ps2_q s, ps2_q t)
{
    return ps2_q_make(s.lo | t.lo, s.hi | t.hi);
}
static inline ps2_q ps2_mmi_pxor(ps2_q s, ps2_q t)
{
    return ps2_q_make(s.lo ^ t.lo, s.hi ^ t.hi);
}
static inline ps2_q ps2_mmi_pnor(ps2_q s, ps2_q t)
{
    return ps2_q_make(~(s.lo | t.lo), ~(s.hi | t.hi));
}
static inline ps2_q ps2_mmi_pcpyld(ps2_q s, ps2_q t)
{
    return ps2_q_make(t.lo, s.lo);
}
static inline ps2_q ps2_mmi_pcpyud(ps2_q s, ps2_q t)
{
    return ps2_q_make(s.hi, t.hi);
}

/* Lane shuffles: d[k] = (from rs ? s : t)[index]. */
#define PS2_MMI_SHUFFLE(NAME, T, N, ...)                                                                             \
    static inline ps2_q ps2_mmi_##NAME(ps2_q s_, ps2_q t_)                                                           \
    {                                                                                                                \
        static const signed char map_[N] = {__VA_ARGS__}; /* >= 0: t lane, < 0: s lane -1-k */                       \
        PS2_LANES(T, N) s, t, d;                                                                                     \
        s.q = s_;                                                                                                    \
        t.q = t_;                                                                                                    \
        for (int k = 0; k < (N); k++)                                                                                \
            d.v[k] = map_[k] >= 0 ? t.v[map_[k]] : s.v[-1 - map_[k]];                                                \
        return d.q;                                                                                                  \
    }
#define S_(k) (-1 - (k))

PS2_MMI_SHUFFLE(pextlw, uint32_t, 4, 0, S_(0), 1, S_(1))
PS2_MMI_SHUFFLE(pextuw, uint32_t, 4, 2, S_(2), 3, S_(3))
PS2_MMI_SHUFFLE(ppacw, uint32_t, 4, 0, 2, S_(0), S_(2))
PS2_MMI_SHUFFLE(pextlh, uint16_t, 8, 0, S_(0), 1, S_(1), 2, S_(2), 3, S_(3))
PS2_MMI_SHUFFLE(pextuh, uint16_t, 8, 4, S_(4), 5, S_(5), 6, S_(6), 7, S_(7))
PS2_MMI_SHUFFLE(ppach, uint16_t, 8, 0, 2, 4, 6, S_(0), S_(2), S_(4), S_(6))
PS2_MMI_SHUFFLE(pinth, uint16_t, 8, 0, S_(4), 1, S_(5), 2, S_(6), 3, S_(7))
PS2_MMI_SHUFFLE(pinteh, uint16_t, 8, 0, S_(0), 2, S_(2), 4, S_(4), 6, S_(6))
PS2_MMI_SHUFFLE(pextlb, uint8_t, 16, 0, S_(0), 1, S_(1), 2, S_(2), 3, S_(3), 4, S_(4), 5, S_(5), 6, S_(6), 7, S_(7))
PS2_MMI_SHUFFLE(pextub, uint8_t, 16, 8, S_(8), 9, S_(9), 10, S_(10), 11, S_(11), 12, S_(12), 13, S_(13), 14, S_(14),
                15, S_(15))
PS2_MMI_SHUFFLE(ppacb, uint8_t, 16, 0, 2, 4, 6, 8, 10, 12, 14, S_(0), S_(2), S_(4), S_(6), S_(8), S_(10), S_(12),
                S_(14))
/* one operand (rt) */
PS2_MMI_SHUFFLE(pexew, uint32_t, 4, 2, 1, 0, 3)
PS2_MMI_SHUFFLE(pexcw, uint32_t, 4, 0, 2, 1, 3)
PS2_MMI_SHUFFLE(prot3w, uint32_t, 4, 1, 2, 0, 3)
PS2_MMI_SHUFFLE(pexeh, uint16_t, 8, 2, 1, 0, 3, 6, 5, 4, 7)
PS2_MMI_SHUFFLE(pexch, uint16_t, 8, 0, 2, 1, 3, 4, 6, 5, 7)
PS2_MMI_SHUFFLE(prevh, uint16_t, 8, 3, 2, 1, 0, 7, 6, 5, 4)
PS2_MMI_SHUFFLE(pcpyh, uint16_t, 8, 0, 0, 0, 0, 4, 4, 4, 4)
#undef S_

/* rd = op(rt, sa) */
#define PS2_MMI_SHIFT(NAME, T, N, EXPR)                                                                              \
    static inline ps2_q ps2_mmi_##NAME(ps2_q t_, unsigned sa)                                                        \
    {                                                                                                                \
        PS2_LANES(T, N) t, d;                                                                                        \
        t.q = t_;                                                                                                    \
        for (int k = 0; k < (N); k++) {                                                                              \
            T a = t.v[k];                                                                                            \
            d.v[k] = (T)(EXPR);                                                                                      \
        }                                                                                                            \
        return d.q;                                                                                                  \
    }
PS2_MMI_SHIFT(psllh, uint16_t, 8, (uint16_t)(a << (sa & 15)))
PS2_MMI_SHIFT(psrlh, uint16_t, 8, a >> (sa & 15))
PS2_MMI_SHIFT(psrah, int16_t, 8, a >> (sa & 15))
PS2_MMI_SHIFT(psllw, uint32_t, 4, a << (sa & 31))
PS2_MMI_SHIFT(psrlw, uint32_t, 4, a >> (sa & 31))
PS2_MMI_SHIFT(psraw, int32_t, 4, a >> (sa & 31))

/* PMFHL.LW / .UW: words of LO and HI interleaved (lo = LO:LO1, hi = HI:HI1 as 128-bit values). */
static inline ps2_q ps2_pmfhl_lw(ps2_q lo, ps2_q hi)
{
    return ps2_q_make((lo.lo & 0xFFFFFFFFu) | (hi.lo << 32), (lo.hi & 0xFFFFFFFFu) | (hi.hi << 32));
}
static inline ps2_q ps2_pmfhl_uw(ps2_q lo, ps2_q hi)
{
    return ps2_q_make((lo.lo >> 32) | (hi.lo & 0xFFFFFFFF00000000ull),
                      (lo.hi >> 32) | (hi.hi & 0xFFFFFFFF00000000ull));
}
/* PMTHL.LW: the even words of LO / HI from rs (the odd ones stay). */
static inline void ps2_pmthl_lw(ps2_q s, ps2_q *lo, ps2_q *hi)
{
    lo->lo = (lo->lo & 0xFFFFFFFF00000000ull) | (s.lo & 0xFFFFFFFFu);
    hi->lo = (hi->lo & 0xFFFFFFFF00000000ull) | (s.lo >> 32);
    lo->hi = (lo->hi & 0xFFFFFFFF00000000ull) | (s.hi & 0xFFFFFFFFu);
    hi->hi = (hi->hi & 0xFFFFFFFF00000000ull) | (s.hi >> 32);
}

/* The MMI ops of the form rd = op(rs, rt) by encoding (funct, sa): X(funct, sa, name). ps2_tools.isa.MMI_OPS is the
 * same table (test_ps2_tools checks it). */
#define PS2_MMI_LIST(X)                                                                                              \
    X(0x08, 0x00, paddw) X(0x08, 0x01, psubw) X(0x08, 0x02, pcgtw) X(0x08, 0x03, pmaxw) X(0x08, 0x04, paddh)         \
    X(0x08, 0x05, psubh) X(0x08, 0x06, pcgth) X(0x08, 0x07, pmaxh) X(0x08, 0x08, paddb) X(0x08, 0x09, psubb)         \
    X(0x08, 0x0A, pcgtb) X(0x08, 0x10, paddsw) X(0x08, 0x11, psubsw) X(0x08, 0x12, pextlw) X(0x08, 0x13, ppacw)      \
    X(0x08, 0x14, paddsh) X(0x08, 0x15, psubsh) X(0x08, 0x16, pextlh) X(0x08, 0x17, ppach) X(0x08, 0x18, paddsb)     \
    X(0x08, 0x19, psubsb) X(0x08, 0x1A, pextlb) X(0x08, 0x1B, ppacb)                                                 \
    X(0x28, 0x01, pabsw) X(0x28, 0x02, pceqw) X(0x28, 0x03, pminw) X(0x28, 0x05, pabsh) X(0x28, 0x06, pceqh)         \
    X(0x28, 0x07, pminh) X(0x28, 0x0A, pceqb) X(0x28, 0x10, padduw) X(0x28, 0x11, psubuw) X(0x28, 0x12, pextuw)      \
    X(0x28, 0x14, padduh) X(0x28, 0x15, psubuh) X(0x28, 0x16, pextuh) X(0x28, 0x18, paddub) X(0x28, 0x19, psubub)    \
    X(0x28, 0x1A, pextub)                                                                                            \
    X(0x09, 0x0A, pinth) X(0x09, 0x0E, pcpyld) X(0x09, 0x12, pand) X(0x09, 0x13, pxor) X(0x09, 0x1A, pexeh)          \
    X(0x09, 0x1B, prevh) X(0x09, 0x1E, pexew) X(0x09, 0x1F, prot3w)                                                  \
    X(0x29, 0x0A, pinteh) X(0x29, 0x0E, pcpyud) X(0x29, 0x12, por) X(0x29, 0x13, pnor) X(0x29, 0x1A, pexch)          \
    X(0x29, 0x1B, pcpyh) X(0x29, 0x1E, pexcw)

/* ---- FPU ---------------------------------------------------------------------------------------------------------- */

#define PS2_FMAX 0x7FFFFFFFu

/* A PS2 float as a double (exact). Exponent 0 (zero or denormal) is a signed zero; exponent 255 is a number. */
static inline double ps2_f2d(uint32_t f)
{
    uint32_t e = (f >> 23) & 0xFF;
    uint64_t b = (uint64_t)(f >> 31) << 63;
    if (e)
        b |= ((uint64_t)(e - 127 + 1023) << 52) | ((uint64_t)(f & 0x7FFFFF) << 29);
    double d;
    memcpy(&d, &b, 8);
    return d;
}

/* A double narrowed to a PS2 float: truncated toward zero, clamped to +-0x7FFFFFFF, flushed to +-0. */
static inline uint32_t ps2_d2f(double d)
{
    uint64_t b;
    memcpy(&b, &d, 8);
    uint32_t s = (uint32_t)(b >> 63) << 31;
    int e = (int)((b >> 52) & 0x7FF);
    if (e == 0)
        return s;
    if (e == 0x7FF) /* only from a division by zero or the like: the largest magnitude */
        return s | PS2_FMAX;
    int fe = e - 1023 + 127;
    if (fe >= 256)
        return s | PS2_FMAX;
    if (fe <= 0)
        return s;
    return s | ((uint32_t)fe << 23) | (uint32_t)((b >> 29) & 0x7FFFFF);
}

static inline uint32_t ps2_fadd(uint32_t a, uint32_t b)
{
    return ps2_d2f(ps2_f2d(a) + ps2_f2d(b));
}
static inline uint32_t ps2_fsub(uint32_t a, uint32_t b)
{
    return ps2_d2f(ps2_f2d(a) - ps2_f2d(b));
}
static inline uint32_t ps2_fmul(uint32_t a, uint32_t b)
{
    return ps2_d2f(ps2_f2d(a) * ps2_f2d(b));
}
/* DIV.S: x / 0 gives +-0x7FFFFFFF with the sign of the quotient (0 / 0 too). */
static inline uint32_t ps2_fdiv(uint32_t a, uint32_t b)
{
    if (!((b >> 23) & 0xFF))
        return ((a ^ b) & 0x80000000u) | PS2_FMAX;
    return ps2_d2f(ps2_f2d(a) / ps2_f2d(b));
}
/* SQRT.S: of the magnitude (a negative operand sets the I flag on the hardware); -0 stays -0. */
static inline uint32_t ps2_fsqrt(uint32_t t)
{
    if (!((t >> 23) & 0xFF))
        return t & 0x80000000u;
    return ps2_d2f(__builtin_sqrt(ps2_f2d(t & 0x7FFFFFFFu)));
}
/* RSQRT.S: fs / sqrt(|ft|). */
static inline uint32_t ps2_frsqrt(uint32_t s, uint32_t t)
{
    if (!((t >> 23) & 0xFF))
        return (s & 0x80000000u) | PS2_FMAX;
    return ps2_d2f(ps2_f2d(s) / __builtin_sqrt(ps2_f2d(t & 0x7FFFFFFFu)));
}
/* MAX.S / MIN.S compare the values (a denormal is zero) and give one operand unchanged. */
static inline uint32_t ps2_fmax(uint32_t a, uint32_t b)
{
    return ps2_f2d(a) >= ps2_f2d(b) ? a : b;
}
static inline uint32_t ps2_fmin(uint32_t a, uint32_t b)
{
    return ps2_f2d(a) <= ps2_f2d(b) ? a : b;
}
/* C.F / C.EQ / C.LT / C.LE (funct 0x30 / 0x32 / 0x34 / 0x36): FCR31 with the condition (bit 23) set or cleared. */
static inline uint32_t ps2_fcmp(uint32_t fcr31, uint32_t a, uint32_t b, unsigned funct)
{
    double x = ps2_f2d(a), y = ps2_f2d(b);
    int c = 0;
    switch (funct & 0x0E) {
    case 0x2: c = x == y; break;
    case 0x4: c = x < y; break;
    case 0x6: c = x <= y; break;
    default: c = 0;
    }
    return c ? fcr31 | (1u << 23) : fcr31 & ~(1u << 23);
}
/* CVT.W.S: truncated, saturated to the int32 range. */
static inline uint32_t ps2_cvt_w(uint32_t f)
{
    double d = ps2_f2d(f);
    if (d >= 2147483648.0)
        return 0x7FFFFFFFu;
    if (d <= -2147483648.0)
        return 0x80000000u;
    return (uint32_t)(int32_t)d;
}
/* CVT.S.W: rounded toward zero. */
static inline uint32_t ps2_cvt_s(uint32_t w)
{
    return ps2_d2f((double)(int32_t)w);
}

#endif
