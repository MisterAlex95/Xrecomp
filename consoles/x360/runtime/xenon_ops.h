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

/* OE forms (XBA-284, x360_tools/xenon.py OE_FORMS): the overflow XER[OV] takes. The add / subtract family in 32-bit
 * mode: x + y (+ carry in) overflows the low word when both addends have the same sign and the result the other. */
static inline uint32_t xe_ov_add(uint64_t x, uint64_t y, uint64_t r)
{
    return (((uint32_t)x ^ (uint32_t)r) & ((uint32_t)y ^ (uint32_t)r)) >> 31;
}

static inline uint32_t xe_ov_mullw(uint64_t a, uint64_t b)
{
    int64_t p = (int64_t)(int32_t)a * (int32_t)b;
    return p != (int64_t)(int32_t)p;
}

static inline uint32_t xe_ov_divw(uint64_t a, uint64_t b)
{
    return (uint32_t)b == 0 || ((uint32_t)a == 0x80000000u && (uint32_t)b == 0xFFFFFFFFu);
}

static inline uint32_t xe_ov_mulld(uint64_t a, uint64_t b)
{
    int64_t p;
    return __builtin_mul_overflow((int64_t)a, (int64_t)b, &p);
}

static inline uint32_t xe_ov_divd(uint64_t a, uint64_t b)
{
    return b == 0 || (a == 0x8000000000000000ull && b == ~0ull);
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
    /* the same 16 bytes in host order, for the helpers that work lane by lane (no element order involved) or know the
     * host's byte order (XV_LE): no member may change the union's size or alignment (xe_cpu, save states, old packs) */
    uint16_t u16[8];
    uint8_t u8[16];
} xe_v128;
#ifndef __cplusplus
_Static_assert(sizeof(xe_v128) == 16 && _Alignof(xe_v128) == 4, "xe_v128 is 16 bytes, word-aligned");
#endif

/* XV_LE: a little-endian host, where PowerPC byte element i of a vector is u8[i ^ 3] and halfword element i u16[i ^ 1]
 * (every host the runtime has: x86-64, arm64). XV_VEC: the compiler has GCC / clang vector extensions, which the
 * hottest lane-wise ops use (SSE2 / NEON at any -O level; lane loops are not vectorized at the packs' -O1).
 * -DXV_SCALAR leaves both out: the plain C of every helper (the reference the tests compare them with). */
#if !defined(XV_SCALAR) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define XV_LE 1
#else
#define XV_LE 0
#endif
#if !defined(XV_SCALAR) && (defined(__GNUC__) || defined(__clang__))
#define XV_VEC 1
typedef uint8_t xv_u8x16 __attribute__((vector_size(16)));
typedef int8_t xv_s8x16 __attribute__((vector_size(16)));
typedef uint16_t xv_u16x8 __attribute__((vector_size(16)));
typedef int16_t xv_s16x8 __attribute__((vector_size(16)));
typedef uint32_t xv_u32x4 __attribute__((vector_size(16)));
#else
#define XV_VEC 0
#endif
/* XV_HOT: the small helpers of the hot paths, inlined even at -O1 where GCC keeps "static inline" functions out of
 * line once their body passes a few instructions (a call per vector op, its operands through the stack) */
#if !defined(XV_SCALAR) && (defined(__GNUC__) || defined(__clang__))
#define XV_HOT static inline __attribute__((always_inline))
#else
#define XV_HOT static inline
#endif

XV_HOT xe_v128 xv_words(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    xe_v128 r;
    r.u32[0] = a, r.u32[1] = b, r.u32[2] = c, r.u32[3] = d;
    return r;
}

XV_HOT xe_v128 xv_splat32(uint32_t v)
{
    return xv_words(v, v, v, v);
}

#if XV_LE && defined(__SIZEOF_INT128__)
/* the 16 bytes of a vector as one big-endian 128-bit number (byte 0 the most significant) and back: lvlx / lvrx and
 * vsldoi are shifts of it */
XV_HOT unsigned __int128 xv_to_u128(xe_v128 v)
{
    return (unsigned __int128)((uint64_t)v.u32[0] << 32 | v.u32[1]) << 64 | ((uint64_t)v.u32[2] << 32 | v.u32[3]);
}

XV_HOT xe_v128 xv_from_u128(unsigned __int128 x)
{
    uint64_t hi = (uint64_t)(x >> 64), lo = (uint64_t)x;
    return xv_words((uint32_t)(hi >> 32), (uint32_t)hi, (uint32_t)(lo >> 32), (uint32_t)lo);
}
#define XV_U128 1
#else
#define XV_U128 0
#endif

XV_HOT void xv_to_bytes(xe_v128 v, uint8_t b[16])
{
#if XV_LE
    /* unrolled: constant offsets (a loop at -O1 leaves b + 4 k variable, and _FORTIFY_SOURCE then calls
     * __memcpy_chk for each word) */
    uint32_t w0 = __builtin_bswap32(v.u32[0]), w1 = __builtin_bswap32(v.u32[1]), w2 = __builtin_bswap32(v.u32[2]),
             w3 = __builtin_bswap32(v.u32[3]);
    memcpy(b, &w0, 4);
    memcpy(b + 4, &w1, 4);
    memcpy(b + 8, &w2, 4);
    memcpy(b + 12, &w3, 4);
    return;
#endif
    for (int k = 0; k < 4; k++) {
        b[4 * k] = (uint8_t)(v.u32[k] >> 24);
        b[4 * k + 1] = (uint8_t)(v.u32[k] >> 16);
        b[4 * k + 2] = (uint8_t)(v.u32[k] >> 8);
        b[4 * k + 3] = (uint8_t)v.u32[k];
    }
}

XV_HOT xe_v128 xv_from_bytes(const uint8_t b[16])
{
    xe_v128 r;
#if XV_LE
    uint32_t w0, w1, w2, w3;
    memcpy(&w0, b, 4);
    memcpy(&w1, b + 4, 4);
    memcpy(&w2, b + 8, 4);
    memcpy(&w3, b + 12, 4);
    r.u32[0] = __builtin_bswap32(w0), r.u32[1] = __builtin_bswap32(w1), r.u32[2] = __builtin_bswap32(w2);
    r.u32[3] = __builtin_bswap32(w3);
    return r;
#endif
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
#if XV_LE
    /* byte i of the result is byte (ctl's byte i & 31) of a:b, bytes in PowerPC order (u8[i ^ 3]) */
    xe_v128 r;
    for (int i = 0; i < 16; i++) {
        unsigned k = ctl.u8[i ^ 3] & 31u;
        r.u8[i ^ 3] = k < 16 ? a.u8[k ^ 3] : b.u8[(k - 16) ^ 3];
    }
    return r;
#endif
    uint8_t src[32], c[16], out[16];
    xv_to_bytes(a, src);
    xv_to_bytes(b, src + 16);
    xv_to_bytes(ctl, c);
    for (int i = 0; i < 16; i++)
        out[i] = src[c[i] & 31];
    return xv_from_bytes(out);
}

XV_HOT xe_v128 xv_sldoi(xe_v128 a, xe_v128 b, int sh)
{
#if XV_U128
    /* bytes sh .. sh + 15 of a:b */
    if (!(sh & 15))
        return a;
    return xv_from_u128(xv_to_u128(a) << (8 * (sh & 15)) | xv_to_u128(b) >> (128 - 8 * (sh & 15)));
#endif
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

XV_HOT xe_v128 xv_lvsl(uint32_t ea)
{
#if !defined(XV_SCALAR)
    /* bytes sh, sh + 1 ... sh + 15 (at most 30: no carry between bytes), word k = sh + 4k .. sh + 4k + 3 */
    uint32_t w = 0x00010203u + (ea & 15) * 0x01010101u;
    return xv_words(w, w + 0x04040404u, w + 0x08080808u, w + 0x0C0C0C0Cu);
#endif
    uint8_t x[16];
    for (int i = 0; i < 16; i++)
        x[i] = (uint8_t)((ea & 15) + i);
    return xv_from_bytes(x);
}

XV_HOT xe_v128 xv_lvsr(uint32_t ea)
{
#if !defined(XV_SCALAR)
    uint32_t w = 0x00010203u + (16 - (ea & 15)) * 0x01010101u; /* bytes 16 - sh ... 31 - sh */
    return xv_words(w, w + 0x04040404u, w + 0x08080808u, w + 0x0C0C0C0Cu);
#endif
    uint8_t x[16];
    for (int i = 0; i < 16; i++)
        x[i] = (uint8_t)(16 - (ea & 15) + i);
    return xv_from_bytes(x);
}

#include "xenon_vmx.h"

#endif
