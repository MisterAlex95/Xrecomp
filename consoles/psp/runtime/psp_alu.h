/* psp_alu.h -- Allegrex operations shared by the interpreter (psp_cpu.c) and lifted code (psp_recomp_rt.h), so both
 * compute the same bits (XBA-224). Build both with -ffp-contract=off: a fused multiply-add (GCC's default on arm64)
 * would round MUL.S + ADD.S once instead of twice. */
#ifndef PSP_ALU_H
#define PSP_ALU_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#define RC_FCR0 0x00003351u /* FCR0 (implementation / revision) as CFC1 $0 reads it */

static inline uint32_t rc_clz(uint32_t x)
{
    return x ? (uint32_t)__builtin_clz(x) : 32u;
}

static inline uint32_t rc_rotr(uint32_t x, uint32_t n)
{
    return n ? (x >> n) | (x << (32 - n)) : x;
}

static inline uint32_t rc_wsbh(uint32_t x)
{
    return ((x & 0x00FF00FFu) << 8) | ((x >> 8) & 0x00FF00FFu);
}

static inline uint32_t rc_bitrev(uint32_t x)
{
    x = ((x >> 1) & 0x55555555u) | ((x & 0x55555555u) << 1);
    x = ((x >> 2) & 0x33333333u) | ((x & 0x33333333u) << 2);
    x = ((x >> 4) & 0x0F0F0F0Fu) | ((x & 0x0F0F0F0Fu) << 4);
    return __builtin_bswap32(x);
}

/* DIV / DIVU: a zero divisor and 0x80000000 / -1 give the R3000A's results (no trap). */
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

/* FPU registers hold floats; MFC1 / MTC1 / LWC1 / SWC1 and the .W formats move their bits. */
static inline uint32_t rc_fbits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}
static inline void rc_fset(float *f, uint32_t u)
{
    memcpy(f, &u, 4);
}
static inline float rc_sqrtf(float x)
{
    return sqrtf(x);
}
static inline float rc_fabsf(float x)
{
    return fabsf(x);
}

/* ROUND / TRUNC / CEIL / FLOOR .W.S (mode 0-3; CVT.W.S passes FCR31's RM): NaN, infinities and values out of range
 * give 0x7FFFFFFF (0x80000000 below the range). */
static inline uint32_t rc_cvt_w(float x, int mode)
{
    if (isnan(x))
        return 0x7FFFFFFFu;
    float r = mode == 0 ? rintf(x) : mode == 1 ? truncf(x) : mode == 2 ? ceilf(x) : floorf(x);
    if (r >= 2147483648.0f)
        return 0x7FFFFFFFu;
    if (r < -2147483648.0f)
        return 0x80000000u;
    return (uint32_t)(int32_t)r;
}

/* C.cond.S: cond bit 0 = true when unordered, bit 1 = when equal, bit 2 = when less; FCR31 bit 23 gets it. */
static inline uint32_t rc_fcmp(uint32_t fcr31, float a, float b, int cond)
{
    int un = isnan(a) || isnan(b);
    int c = ((cond & 1) && un) || ((cond & 2) && !un && a == b) || ((cond & 4) && !un && a < b);
    return c ? fcr31 | (1u << 23) : fcr31 & ~(1u << 23);
}

#endif
