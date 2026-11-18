/* xenon_ops.h -- Xenon semantics for lifted code (XBA-230 spike): the 64-bit integer forms and VMX / VMX128.
 *
 * Sits next to ppc_ops.h (consoles/gc/runtime, the 32-bit PowerPC helpers shared with the GameCube; it is
 * console-agnostic and should move to a shared directory once a second runtime links it). The Xenon lifter
 * (consoles/x360/tools/x360_tools/xenon.py) emits calls to these by name; no memory access here except through the
 * xv_* load / store helpers, which take the runtime's accessors as macros (XV_LOAD_BYTES / XV_STORE_BYTES).
 *
 * Vector registers: union xe_v128, elements in PowerPC order (element 0 = the first in memory, the most significant
 * on a big-endian machine), each word in host order. Byte i (PowerPC numbering) is bits 24 - 8 (i % 4) of word i / 4.
 *
 * VMX float ops run in host single precision with fused multiply-adds (fmaf); denormals are flushed in non-Java mode
 * by the lifter (xv_daz, xenon_vmx.h, XBA-270). Known gaps: NaN payloads are the host's; vrefp / vrsqrtefp are exact
 * (hardware gives estimates); vmsum3fp128 / vmsum4fp128 round after each product and sum. divd / divdu by zero give 0
 * (undefined on hardware). The integer element ops, packs, sums and the D3D pack ops are in xenon_vmx.h. */
#ifndef XENON_OPS_H
#define XENON_OPS_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "ppc_ops.h"

/* ---- 64-bit integer ---- */

static inline uint32_t ppc_cmps64(int64_t a, int64_t b, uint32_t so)
{
    return (a < b ? PPC_CR_LT : a > b ? PPC_CR_GT : PPC_CR_EQ) | (so & 1);
}

static inline uint32_t ppc_cmpu64(uint64_t a, uint64_t b, uint32_t so)
{
    return (a < b ? PPC_CR_LT : a > b ? PPC_CR_GT : PPC_CR_EQ) | (so & 1);
}

/* Carry out of a + b + cin (64-bit mode: XER[CA] is the carry out of bit 0). */
static inline uint32_t ppc_carry64(uint64_t a, uint64_t b, uint32_t cin)
{
    uint64_t s = a + b;
    uint32_t c = s < a;
    uint64_t t = s + cin;
    return c | (t < s);
}

/* Titles run in 32-bit mode (MSR[SF] = 0, XBA-270): 64-bit registers and instructions, but XER[CA] is the carry
 * out of the low word and record forms set CR0 from the low word (as Xenia's and XenonRecomp's code generators
 * do: AddDidCarry / UpdateCR truncate to 32 bits, compare<int32_t>). The add / subtract family uses this. */
static inline uint32_t xe_carry(uint64_t a, uint64_t b, uint32_t cin)
{
    uint32_t x = (uint32_t)a, y = (uint32_t)b;
    uint32_t s = x + y;
    return (s < x) | ((uint32_t)(s + cin) < s);
}

static inline uint64_t ppc_rotl64(uint64_t v, unsigned n)
{
    n &= 63;
    return n ? (v << n) | (v >> (64 - n)) : v;
}

static inline uint64_t ppc_mulhd(uint64_t a, uint64_t b)
{
    return (uint64_t)(((__int128)(int64_t)a * (__int128)(int64_t)b) >> 64);
}

static inline uint64_t ppc_mulhdu(uint64_t a, uint64_t b)
{
    return (uint64_t)(((unsigned __int128)a * b) >> 64);
}

static inline uint64_t ppc_divd(uint64_t a, uint64_t b)
{
    if (b == 0)
        return 0;
    if (a == 0x8000000000000000ull && b == ~0ull)
        return a;
    return (uint64_t)((int64_t)a / (int64_t)b);
}

static inline uint64_t ppc_divdu(uint64_t a, uint64_t b)
{
    return b ? a / b : 0;
}

static inline uint64_t ppc_cntlzd(uint64_t v)
{
    return v ? (uint64_t)__builtin_clzll(v) : 64u;
}

static inline uint64_t ppc_sld(uint64_t v, uint64_t rb)
{
    return (rb & 0x40) ? 0 : v << (rb & 63);
}

static inline uint64_t ppc_srd(uint64_t v, uint64_t rb)
{
    return (rb & 0x40) ? 0 : v >> (rb & 63);
}

static inline uint64_t ppc_sradi(uint64_t v, unsigned sh, uint32_t *ca)
{
    sh &= 63;
    uint64_t r = (uint64_t)((int64_t)v >> sh);
    *ca = ((int64_t)v < 0 && sh && (v & ((1ull << sh) - 1))) ? 1u : 0u;
    return r;
}

static inline uint64_t ppc_srad(uint64_t v, uint64_t rb, uint32_t *ca)
{
    if (rb & 0x40) {
        *ca = (int64_t)v < 0 ? 1u : 0u;
        return (int64_t)v < 0 ? ~0ull : 0;
    }
    return ppc_sradi(v, (unsigned)(rb & 63), ca);
}

/* fctid / fctidz: saturating, NaN -> 0x8000000000000000 (round to nearest even: the FPSCR mode is not modelled). */
static inline int64_t ppc_fctid(double d, int toward_zero)
{
    if (d != d)
        return INT64_MIN;
    if (d >= 9223372036854775807.0)
        return INT64_MAX;
    if (d < -9223372036854775808.0)
        return INT64_MIN;
    return (int64_t)(toward_zero ? trunc(d) : nearbyint(d));
}

/* ---- VMX ---- */

typedef union xe_v128 {
    uint32_t u32[4];
    int32_t s32[4];
    float f32[4];
} xe_v128;

static inline xe_v128 xv_words(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    xe_v128 r;
    r.u32[0] = a, r.u32[1] = b, r.u32[2] = c, r.u32[3] = d;
    return r;
}

static inline xe_v128 xv_splat32(uint32_t v)
{
    return xv_words(v, v, v, v);
}

static inline void xv_to_bytes(xe_v128 v, uint8_t b[16])
{
    for (int k = 0; k < 4; k++) {
        b[4 * k] = (uint8_t)(v.u32[k] >> 24);
        b[4 * k + 1] = (uint8_t)(v.u32[k] >> 16);
        b[4 * k + 2] = (uint8_t)(v.u32[k] >> 8);
        b[4 * k + 3] = (uint8_t)v.u32[k];
    }
}

static inline xe_v128 xv_from_bytes(const uint8_t b[16])
{
    xe_v128 r;
    for (int k = 0; k < 4; k++)
        r.u32[k] = (uint32_t)b[4 * k] << 24 | (uint32_t)b[4 * k + 1] << 16 | (uint32_t)b[4 * k + 2] << 8 | b[4 * k + 3];
    return r;
}

static inline float xv_f(uint32_t u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}

#define XV_MAP2(name, expr)                                                                                            \
    static inline xe_v128 name(xe_v128 a, xe_v128 b)                                                                   \
    {                                                                                                                  \
        xe_v128 r;                                                                                                     \
        for (int k = 0; k < 4; k++)                                                                                    \
            r.expr;                                                                                                    \
        return r;                                                                                                      \
    }

XV_MAP2(xv_addfp, f32[k] = a.f32[k] + b.f32[k])
XV_MAP2(xv_subfp, f32[k] = a.f32[k] - b.f32[k])
XV_MAP2(xv_mulfp, f32[k] = a.f32[k] * b.f32[k])
/* vmaxfp / vminfp: a NaN operand gives a NaN (a + b propagates it); +0 is greater than -0 (AltiVec PEM: max(+0, -0)
 * = +0, min(+0, -0) = -0), so equal operands combine their sign bits. */
#define XV_NAN2(a, b) ((a) != (a) || (b) != (b))
XV_MAP2(xv_maxfp, f32[k] = XV_NAN2(a.f32[k], b.f32[k]) ? a.f32[k] + b.f32[k]
                         : a.f32[k] == b.f32[k] ? xv_f(a.u32[k] & b.u32[k])
                                                : (a.f32[k] > b.f32[k] ? a.f32[k] : b.f32[k]))
XV_MAP2(xv_minfp, f32[k] = XV_NAN2(a.f32[k], b.f32[k]) ? a.f32[k] + b.f32[k]
                         : a.f32[k] == b.f32[k] ? xv_f(a.u32[k] | b.u32[k])
                                                : (a.f32[k] < b.f32[k] ? a.f32[k] : b.f32[k]))
XV_MAP2(xv_and, u32[k] = a.u32[k] & b.u32[k])
XV_MAP2(xv_andc, u32[k] = a.u32[k] & ~b.u32[k])
XV_MAP2(xv_or, u32[k] = a.u32[k] | b.u32[k])
XV_MAP2(xv_nor, u32[k] = ~(a.u32[k] | b.u32[k]))
XV_MAP2(xv_xor, u32[k] = a.u32[k] ^ b.u32[k])
XV_MAP2(xv_adduwm, u32[k] = a.u32[k] + b.u32[k])
XV_MAP2(xv_subuwm, u32[k] = a.u32[k] - b.u32[k])
XV_MAP2(xv_slw, u32[k] = a.u32[k] << (b.u32[k] & 31))
XV_MAP2(xv_srw, u32[k] = a.u32[k] >> (b.u32[k] & 31))
XV_MAP2(xv_sraw, s32[k] = a.s32[k] >> (b.u32[k] & 31))
XV_MAP2(xv_rlw, u32[k] = ppc_rotl(a.u32[k], b.u32[k] & 31))
XV_MAP2(xv_maxsw, s32[k] = a.s32[k] > b.s32[k] ? a.s32[k] : b.s32[k])
XV_MAP2(xv_minsw, s32[k] = a.s32[k] < b.s32[k] ? a.s32[k] : b.s32[k])
XV_MAP2(xv_cmpeqfp, u32[k] = a.f32[k] == b.f32[k] ? ~0u : 0)
XV_MAP2(xv_cmpgefp, u32[k] = a.f32[k] >= b.f32[k] ? ~0u : 0)
XV_MAP2(xv_cmpgtfp, u32[k] = a.f32[k] > b.f32[k] ? ~0u : 0)
XV_MAP2(xv_cmpequw, u32[k] = a.u32[k] == b.u32[k] ? ~0u : 0)
XV_MAP2(xv_cmpgtuw, u32[k] = a.u32[k] > b.u32[k] ? ~0u : 0)
XV_MAP2(xv_cmpgtsw, u32[k] = a.s32[k] > b.s32[k] ? ~0u : 0)
XV_MAP2(xv_cmpbfp, u32[k] = (a.f32[k] <= b.f32[k] ? 0 : 0x80000000u) | (a.f32[k] >= -b.f32[k] ? 0 : 0x40000000u))

/* CR6 of a record-form compare: all elements true -> LT position (0b1000), none -> EQ position (0b0010). */
static inline uint32_t xv_cr6(xe_v128 v)
{
    uint32_t all = v.u32[0] & v.u32[1] & v.u32[2] & v.u32[3];
    uint32_t any = v.u32[0] | v.u32[1] | v.u32[2] | v.u32[3];
    return (all == ~0u ? 8u : 0u) | (any == 0 ? 2u : 0u);
}

static inline xe_v128 xv_maddfp(xe_v128 a, xe_v128 c, xe_v128 b)
{
    xe_v128 r;
    for (int k = 0; k < 4; k++)
        r.f32[k] = fmaf(a.f32[k], c.f32[k], b.f32[k]);
    return r;
}

/* -(a * c - b), negated after rounding: an exact zero comes out as -0. The negation is a sign-bit flip, because
 * compilers may turn -fmaf(a, c, -b) into fmaf(-a, c, b) (AArch64 fmsub), which gives +0 there. */
static inline xe_v128 xv_nmsubfp(xe_v128 a, xe_v128 c, xe_v128 b)
{
    xe_v128 r;
    for (int k = 0; k < 4; k++) {
        r.f32[k] = fmaf(a.f32[k], c.f32[k], -b.f32[k]);
        r.u32[k] ^= 0x80000000u;
    }
    return r;
}

static inline xe_v128 xv_msum3fp(xe_v128 a, xe_v128 b)
{
    float s = a.f32[0] * b.f32[0] + a.f32[1] * b.f32[1];
    s += a.f32[2] * b.f32[2];
    xe_v128 r;
    r.f32[0] = r.f32[1] = r.f32[2] = r.f32[3] = s;
    return r;
}

static inline xe_v128 xv_msum4fp(xe_v128 a, xe_v128 b)
{
    float s = a.f32[0] * b.f32[0] + a.f32[1] * b.f32[1];
    s += a.f32[2] * b.f32[2];
    s += a.f32[3] * b.f32[3];
    xe_v128 r;
    r.f32[0] = r.f32[1] = r.f32[2] = r.f32[3] = s;
    return r;
}

static inline xe_v128 xv_refp(xe_v128 b)
{
    xe_v128 r;
    for (int k = 0; k < 4; k++)
        r.f32[k] = 1.0f / b.f32[k];
    return r;
}

static inline xe_v128 xv_rsqrtefp(xe_v128 b)
{
    xe_v128 r;
    for (int k = 0; k < 4; k++)
        r.f32[k] = 1.0f / sqrtf(b.f32[k]);
    return r;
}

/* vrfin (0, nearest even), vrfiz (1), vrfip (2), vrfim (3). */
static inline xe_v128 xv_rfi(xe_v128 b, int mode)
{
    xe_v128 r;
    for (int k = 0; k < 4; k++) {
        float f = b.f32[k];
        r.f32[k] = mode == 0 ? nearbyintf(f) : mode == 1 ? truncf(f) : mode == 2 ? ceilf(f) : floorf(f);
    }
    return r;
}

static inline xe_v128 xv_cfsx(xe_v128 b, int uimm)
{
    xe_v128 r;
    for (int k = 0; k < 4; k++)
        r.f32[k] = ldexpf((float)b.s32[k], -uimm);
    return r;
}

static inline xe_v128 xv_cfux(xe_v128 b, int uimm)
{
    xe_v128 r;
    for (int k = 0; k < 4; k++)
        r.f32[k] = ldexpf((float)b.u32[k], -uimm);
    return r;
}

/* vctsxs / vcfpsxws128: float * 2^uimm to int32, saturating (sets VSCR[SAT]); NaN -> 0. */
static inline xe_v128 xv_ctsxs(xe_v128 b, int uimm, uint32_t *vscr)
{
    xe_v128 r;
    for (int k = 0; k < 4; k++) {
        double d = ldexp((double)b.f32[k], uimm);
        if (d != d) {
            r.s32[k] = 0;
            *vscr |= 1;
        } else if (d >= 2147483647.0) {
            r.s32[k] = INT32_MAX;
            *vscr |= d > 2147483647.0;
        } else if (d < -2147483648.0) {
            r.s32[k] = INT32_MIN;
            *vscr |= 1;
        } else
            r.s32[k] = (int32_t)d;
    }
    return r;
}

static inline xe_v128 xv_ctuxs(xe_v128 b, int uimm, uint32_t *vscr)
{
    xe_v128 r;
    for (int k = 0; k < 4; k++) {
        double d = ldexp((double)b.f32[k], uimm);
        if (d != d || d < 0) {
            r.u32[k] = 0;
            *vscr |= d != 0.0;
        } else if (d >= 4294967295.0) {
            r.u32[k] = UINT32_MAX;
            *vscr |= d > 4294967295.0;
        } else
            r.u32[k] = (uint32_t)d;
    }
    return r;
}

/* vsel / vsel128: bit from b where the control bit is set, else from a. */
static inline xe_v128 xv_sel(xe_v128 a, xe_v128 b, xe_v128 ctl)
{
    xe_v128 r;
    for (int k = 0; k < 4; k++)
        r.u32[k] = (a.u32[k] & ~ctl.u32[k]) | (b.u32[k] & ctl.u32[k]);
    return r;
}

static inline xe_v128 xv_perm(xe_v128 a, xe_v128 b, xe_v128 ctl)
{
    uint8_t src[32], c[16], out[16];
    xv_to_bytes(a, src);
    xv_to_bytes(b, src + 16);
    xv_to_bytes(ctl, c);
    for (int i = 0; i < 16; i++)
        out[i] = src[c[i] & 31];
    return xv_from_bytes(out);
}

static inline xe_v128 xv_sldoi(xe_v128 a, xe_v128 b, int sh)
{
    uint8_t src[32];
    xv_to_bytes(a, src);
    xv_to_bytes(b, src + 16);
    return xv_from_bytes(src + (sh & 15));
}

/* vslo / vsro: shift the whole register by octets ((vB byte 15 >> 3) & 15). */
static inline xe_v128 xv_slo(xe_v128 a, xe_v128 b)
{
    uint8_t src[32] = {0};
    xv_to_bytes(a, src);
    return xv_from_bytes(src + ((b.u32[3] >> 3) & 15));
}

static inline xe_v128 xv_sro(xe_v128 a, xe_v128 b)
{
    uint8_t src[32] = {0};
    xv_to_bytes(a, src + 16);
    return xv_from_bytes(src + 16 - ((b.u32[3] >> 3) & 15));
}

static inline xe_v128 xv_mrghw(xe_v128 a, xe_v128 b)
{
    return xv_words(a.u32[0], b.u32[0], a.u32[1], b.u32[1]);
}

static inline xe_v128 xv_mrglw(xe_v128 a, xe_v128 b)
{
    return xv_words(a.u32[2], b.u32[2], a.u32[3], b.u32[3]);
}

static inline xe_v128 xv_spltw(xe_v128 b, int e)
{
    return xv_splat32(b.u32[e & 3]);
}

static inline xe_v128 xv_splth(xe_v128 b, int e)
{
    uint32_t h = (b.u32[(e & 7) >> 1] >> ((e & 1) ? 0 : 16)) & 0xFFFF;
    return xv_splat32(h << 16 | h);
}

static inline xe_v128 xv_spltb(xe_v128 b, int e)
{
    uint8_t x[16];
    xv_to_bytes(b, x);
    return xv_splat32(x[e & 15] * 0x01010101u);
}

/* vpermwi128: element k of the result is element sel_k of vB. */
static inline xe_v128 xv_permwi(xe_v128 b, int x, int y, int z, int w)
{
    return xv_words(b.u32[x], b.u32[y], b.u32[z], b.u32[w]);
}

/* vrlimi128: vB rotated left by z words, then element k replaces vD's where mask bit (3 - k) is set. */
static inline xe_v128 xv_rlimi(xe_v128 d, xe_v128 b, int mask, int z)
{
    xe_v128 r;
    for (int k = 0; k < 4; k++)
        r.u32[k] = (mask >> (3 - k)) & 1 ? b.u32[(k + z) & 3] : d.u32[k];
    return r;
}

static inline xe_v128 xv_lvsl(uint32_t ea)
{
    uint8_t x[16];
    for (int i = 0; i < 16; i++)
        x[i] = (uint8_t)((ea & 15) + i);
    return xv_from_bytes(x);
}

static inline xe_v128 xv_lvsr(uint32_t ea)
{
    uint8_t x[16];
    for (int i = 0; i < 16; i++)
        x[i] = (uint8_t)(16 - (ea & 15) + i);
    return xv_from_bytes(x);
}

#include "xenon_vmx.h"

#endif
