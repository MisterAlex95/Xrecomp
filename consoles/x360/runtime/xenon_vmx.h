/* xenon_vmx.h -- the rest of VMX / VMX128 for lifted code (XBA-270), included by xenon_ops.h: non-Java mode, the
 * integer element ops (bytes, halfwords, words; modulo and saturating), packs / unpacks, sums, multiply-sums, the
 * float estimates vexptefp / vlogefp, and the D3D pack / unpack ops of VMX128 (vpkd3d128 / vupkd3d128).
 *
 * Non-Java mode: Xenon's VMX runs with VSCR[NJ] set (the kernel starts threads that way): denormal inputs and results
 * of float vector ops are flushed to a zero of the same sign. The lifter wraps the inputs and the result of every
 * float vector op in xv_daz(vscr, v), which does that while VSCR[NJ] (0x00010000) is set. VSCR[SAT] (bit 0) is set
 * by every saturating op that clamps (the helpers take a pointer to VSCR).
 *
 * Estimates: vrefp / vrsqrtefp (xenon_ops.h) are exact 1/x and 1/sqrt(x), vexptefp / vlogefp exact 2^x and log2(x):
 * the hardware's estimate tables are not reproduced (they cannot be checked here), so results may differ from a
 * console in the low bits, within the 1/4096 relative error the architecture allows for the estimates.
 *
 * vpkd3d128 / vupkd3d128 follow Xenia's description of them (src/xenia/cpu/ppc/ppc_emit_altivec.cc and the PACK /
 * UNPACK sequences of its x64 back end with their constants, BSD licence: the semantics only, no code taken). */
#ifndef XENON_VMX_H
#define XENON_VMX_H

/* ---- non-Java mode ---- */

#define XV_VSCR_NJ 0x00010000u
#define XV_VSCR_SAT 0x00000001u

/* Denormal lanes to a zero of the same sign while VSCR[NJ] is set. */
static inline xe_v128 xv_daz(uint32_t vscr, xe_v128 v)
{
    if (vscr & XV_VSCR_NJ)
        for (int k = 0; k < 4; k++)
            if ((v.u32[k] & 0x7F800000u) == 0)
                v.u32[k] &= 0x80000000u;
    return v;
}

static inline xe_v128 xv_exptefp(xe_v128 b)
{
    xe_v128 r;
    for (int k = 0; k < 4; k++)
        r.f32[k] = exp2f(b.f32[k]);
    return r;
}

static inline xe_v128 xv_logefp(xe_v128 b)
{
    xe_v128 r;
    for (int k = 0; k < 4; k++)
        r.f32[k] = log2f(b.f32[k]);
    return r;
}

/* ---- byte / halfword elements (PowerPC numbering: element 0 is the most significant of word 0) ---- */

static inline uint32_t xv_gb(xe_v128 v, int i)
{
    return (v.u32[i >> 2] >> (24 - 8 * (i & 3))) & 0xFFu;
}

static inline void xv_pb(xe_v128 *v, int i, uint32_t x)
{
    int s = 24 - 8 * (i & 3);
    v->u32[i >> 2] = (v->u32[i >> 2] & ~(0xFFu << s)) | ((x & 0xFFu) << s);
}

static inline uint32_t xv_gh(xe_v128 v, int i)
{
    return (v.u32[i >> 1] >> ((i & 1) ? 0 : 16)) & 0xFFFFu;
}

static inline void xv_ph(xe_v128 *v, int i, uint32_t x)
{
    int s = (i & 1) ? 0 : 16;
    v->u32[i >> 1] = (v->u32[i >> 1] & ~(0xFFFFu << s)) | ((x & 0xFFFFu) << s);
}

static inline int64_t xv_sat(int64_t v, int64_t lo, int64_t hi, uint32_t *vscr)
{
    if (v < lo) {
        *vscr |= XV_VSCR_SAT;
        return lo;
    }
    if (v > hi) {
        *vscr |= XV_VSCR_SAT;
        return hi;
    }
    return v;
}

#define XV_SB(x) ((int64_t)(int8_t)(x))
#define XV_SH(x) ((int64_t)(int16_t)(x))
#define XV_SW(x) ((int64_t)(int32_t)(x))

/* Element-wise ops on bytes (B), halfwords (H) and words (W): `expr` sees x (from a) and y (from b), unsigned. */
#define XV_EB(name, expr)                                                                                              \
    static inline xe_v128 name(xe_v128 a, xe_v128 b)                                                                   \
    {                                                                                                                  \
        xe_v128 r = a;                                                                                                 \
        for (int i = 0; i < 16; i++) {                                                                                 \
            uint32_t x = xv_gb(a, i), y = xv_gb(b, i);                                                                 \
            xv_pb(&r, i, (uint32_t)(expr));                                                                            \
        }                                                                                                              \
        return r;                                                                                                      \
    }
#define XV_EH(name, expr)                                                                                              \
    static inline xe_v128 name(xe_v128 a, xe_v128 b)                                                                   \
    {                                                                                                                  \
        xe_v128 r = a;                                                                                                 \
        for (int i = 0; i < 8; i++) {                                                                                  \
            uint32_t x = xv_gh(a, i), y = xv_gh(b, i);                                                                 \
            xv_ph(&r, i, (uint32_t)(expr));                                                                            \
        }                                                                                                              \
        return r;                                                                                                      \
    }
#define XV_EW(name, expr)                                                                                              \
    static inline xe_v128 name(xe_v128 a, xe_v128 b)                                                                   \
    {                                                                                                                  \
        xe_v128 r;                                                                                                     \
        for (int i = 0; i < 4; i++) {                                                                                  \
            uint32_t x = a.u32[i], y = b.u32[i];                                                                       \
            r.u32[i] = (uint32_t)(expr);                                                                               \
        }                                                                                                              \
        return r;                                                                                                      \
    }
/* Saturating versions: `expr` is an int64_t, clamped to [lo, hi] (VSCR[SAT] set when it is). */
#define XV_SATB(name, expr, lo, hi)                                                                                    \
    static inline xe_v128 name(xe_v128 a, xe_v128 b, uint32_t *vscr)                                                   \
    {                                                                                                                  \
        xe_v128 r = a;                                                                                                 \
        for (int i = 0; i < 16; i++) {                                                                                 \
            uint32_t x = xv_gb(a, i), y = xv_gb(b, i);                                                                 \
            xv_pb(&r, i, (uint32_t)xv_sat((expr), (lo), (hi), vscr));                                                  \
        }                                                                                                              \
        return r;                                                                                                      \
    }
#define XV_SATH(name, expr, lo, hi)                                                                                    \
    static inline xe_v128 name(xe_v128 a, xe_v128 b, uint32_t *vscr)                                                   \
    {                                                                                                                  \
        xe_v128 r = a;                                                                                                 \
        for (int i = 0; i < 8; i++) {                                                                                  \
            uint32_t x = xv_gh(a, i), y = xv_gh(b, i);                                                                 \
            xv_ph(&r, i, (uint32_t)xv_sat((expr), (lo), (hi), vscr));                                                  \
        }                                                                                                              \
        return r;                                                                                                      \
    }
#define XV_SATW(name, expr, lo, hi)                                                                                    \
    static inline xe_v128 name(xe_v128 a, xe_v128 b, uint32_t *vscr)                                                   \
    {                                                                                                                  \
        xe_v128 r;                                                                                                     \
        for (int i = 0; i < 4; i++) {                                                                                  \
            uint32_t x = a.u32[i], y = b.u32[i];                                                                       \
            r.u32[i] = (uint32_t)xv_sat((expr), (lo), (hi), vscr);                                                     \
        }                                                                                                              \
        return r;                                                                                                      \
    }

XV_EB(xv_addubm, x + y)
XV_EH(xv_adduhm, x + y)
XV_EB(xv_sububm, x - y)
XV_EH(xv_subuhm, x - y)
XV_SATB(xv_addubs, (int64_t)x + y, 0, 255)
XV_SATH(xv_adduhs, (int64_t)x + y, 0, 65535)
XV_SATW(xv_adduws, (int64_t)x + y, 0, 4294967295LL)
XV_SATB(xv_addsbs, XV_SB(x) + XV_SB(y), -128, 127)
XV_SATH(xv_addshs, XV_SH(x) + XV_SH(y), -32768, 32767)
XV_SATW(xv_addsws, XV_SW(x) + XV_SW(y), INT32_MIN, INT32_MAX)
XV_SATB(xv_sububs, (int64_t)x - y, 0, 255)
XV_SATH(xv_subuhs, (int64_t)x - y, 0, 65535)
XV_SATW(xv_subuws, (int64_t)x - y, 0, 4294967295LL)
XV_SATB(xv_subsbs, XV_SB(x) - XV_SB(y), -128, 127)
XV_SATH(xv_subshs, XV_SH(x) - XV_SH(y), -32768, 32767)
XV_SATW(xv_subsws, XV_SW(x) - XV_SW(y), INT32_MIN, INT32_MAX)
XV_EW(xv_addcuw, ((uint64_t)x + y) >> 32)
XV_EW(xv_subcuw, x >= y)
XV_EB(xv_maxub, x > y ? x : y)
XV_EH(xv_maxuh, x > y ? x : y)
XV_EW(xv_maxuw, x > y ? x : y)
XV_EB(xv_maxsb, XV_SB(x) > XV_SB(y) ? x : y)
XV_EH(xv_maxsh, XV_SH(x) > XV_SH(y) ? x : y)
XV_EB(xv_minub, x < y ? x : y)
XV_EH(xv_minuh, x < y ? x : y)
XV_EW(xv_minuw, x < y ? x : y)
XV_EB(xv_minsb, XV_SB(x) < XV_SB(y) ? x : y)
XV_EH(xv_minsh, XV_SH(x) < XV_SH(y) ? x : y)
XV_EB(xv_avgub, ((uint64_t)x + y + 1) >> 1)
XV_EH(xv_avguh, ((uint64_t)x + y + 1) >> 1)
XV_EW(xv_avguw, ((uint64_t)x + y + 1) >> 1)
XV_EB(xv_avgsb, (XV_SB(x) + XV_SB(y) + 1) >> 1)
XV_EH(xv_avgsh, (XV_SH(x) + XV_SH(y) + 1) >> 1)
XV_EW(xv_avgsw, (XV_SW(x) + XV_SW(y) + 1) >> 1)
XV_EB(xv_rlb, ((x << (y & 7)) | (x >> ((8 - (y & 7)) & 7))) & 0xFF)
XV_EH(xv_rlh, ((x << (y & 15)) | (x >> ((16 - (y & 15)) & 15))) & 0xFFFF)
XV_EB(xv_slb, x << (y & 7))
XV_EH(xv_slh, x << (y & 15))
XV_EB(xv_srb, x >> (y & 7))
XV_EH(xv_srh, x >> (y & 15))
XV_EB(xv_srab, XV_SB(x) >> (y & 7))
XV_EH(xv_srah, XV_SH(x) >> (y & 15))
XV_EB(xv_cmpequb, x == y ? 0xFF : 0)
XV_EH(xv_cmpequh, x == y ? 0xFFFF : 0)
XV_EB(xv_cmpgtub, x > y ? 0xFF : 0)
XV_EH(xv_cmpgtuh, x > y ? 0xFFFF : 0)
XV_EB(xv_cmpgtsb, XV_SB(x) > XV_SB(y) ? 0xFF : 0)
XV_EH(xv_cmpgtsh, XV_SH(x) > XV_SH(y) ? 0xFFFF : 0)

/* vsl / vsr: the whole register shifted by (vB byte 15 & 7) bits. */
static inline xe_v128 xv_sl(xe_v128 a, xe_v128 b)
{
    int sh = b.u32[3] & 7;
    xe_v128 r;
    for (int k = 0; k < 4; k++)
        r.u32[k] = sh ? (a.u32[k] << sh) | (k < 3 ? a.u32[k + 1] >> (32 - sh) : 0) : a.u32[k];
    return r;
}

static inline xe_v128 xv_sr(xe_v128 a, xe_v128 b)
{
    int sh = b.u32[3] & 7;
    xe_v128 r;
    for (int k = 0; k < 4; k++)
        r.u32[k] = sh ? (a.u32[k] >> sh) | (k > 0 ? a.u32[k - 1] << (32 - sh) : 0) : a.u32[k];
    return r;
}

/* merges of bytes / halfwords: the high (lo = 0) or low (lo = 1) halves of a and b interleaved */
static inline xe_v128 xv_mrgb(xe_v128 a, xe_v128 b, int lo)
{
    xe_v128 r = a;
    for (int i = 0; i < 8; i++) {
        xv_pb(&r, 2 * i, xv_gb(a, i + 8 * lo));
        xv_pb(&r, 2 * i + 1, xv_gb(b, i + 8 * lo));
    }
    return r;
}

static inline xe_v128 xv_mrgh(xe_v128 a, xe_v128 b, int lo)
{
    xe_v128 r = a;
    for (int i = 0; i < 4; i++) {
        xv_ph(&r, 2 * i, xv_gh(a, i + 4 * lo));
        xv_ph(&r, 2 * i + 1, xv_gh(b, i + 4 * lo));
    }
    return r;
}

/* vmule* / vmulo*: products of the even (odd = 0) or odd (odd = 1) elements, double width. */
static inline xe_v128 xv_mulb(xe_v128 a, xe_v128 b, int odd, int sgn)
{
    xe_v128 r = a;
    for (int i = 0; i < 8; i++) {
        uint32_t x = xv_gb(a, 2 * i + odd), y = xv_gb(b, 2 * i + odd);
        xv_ph(&r, i, sgn ? (uint32_t)(XV_SB(x) * XV_SB(y)) : x * y);
    }
    return r;
}

static inline xe_v128 xv_mulh(xe_v128 a, xe_v128 b, int odd, int sgn)
{
    xe_v128 r;
    for (int i = 0; i < 4; i++) {
        uint32_t x = xv_gh(a, 2 * i + odd), y = xv_gh(b, 2 * i + odd);
        r.u32[i] = sgn ? (uint32_t)(XV_SH(x) * XV_SH(y)) : x * y;
    }
    return r;
}

/* packs: vA's elements, then vB's, each narrowed; mode 0 modulo, 1 unsigned -> unsigned saturating, 2 signed ->
 * unsigned saturating, 3 signed -> signed saturating. */
static inline xe_v128 xv_pkh(xe_v128 a, xe_v128 b, int mode, uint32_t *vscr)
{
    xe_v128 r = a;
    for (int i = 0; i < 16; i++) {
        uint32_t x = i < 8 ? xv_gh(a, i) : xv_gh(b, i - 8);
        int64_t v = mode == 0 ? (int64_t)(x & 0xFF)
                  : mode == 1 ? xv_sat(x, 0, 255, vscr)
                  : mode == 2 ? xv_sat(XV_SH(x), 0, 255, vscr)
                              : xv_sat(XV_SH(x), -128, 127, vscr);
        xv_pb(&r, i, (uint32_t)v);
    }
    return r;
}

static inline xe_v128 xv_pkw(xe_v128 a, xe_v128 b, int mode, uint32_t *vscr)
{
    xe_v128 r = a;
    for (int i = 0; i < 8; i++) {
        uint32_t x = i < 4 ? a.u32[i] : b.u32[i - 4];
        int64_t v = mode == 0 ? (int64_t)(x & 0xFFFF)
                  : mode == 1 ? xv_sat(x, 0, 65535, vscr)
                  : mode == 2 ? xv_sat(XV_SW(x), 0, 65535, vscr)
                              : xv_sat(XV_SW(x), -32768, 32767, vscr);
        xv_ph(&r, i, (uint32_t)v);
    }
    return r;
}

/* vpkpx: each word 8:8:8:8 to a 1:5:5:5 halfword. */
static inline xe_v128 xv_pkpx(xe_v128 a, xe_v128 b)
{
    xe_v128 r = a;
    for (int i = 0; i < 8; i++) {
        uint32_t w = i < 4 ? a.u32[i] : b.u32[i - 4];
        xv_ph(&r, i, ((w >> 24) & 1) << 15 | ((w >> 19) & 0x1F) << 10 | ((w >> 11) & 0x1F) << 5 | ((w >> 3) & 0x1F));
    }
    return r;
}

/* unpacks of the high (lo = 0) or low (lo = 1) half of vB, sign-extended */
static inline xe_v128 xv_upkb(xe_v128 b, int lo)
{
    xe_v128 r = b;
    for (int i = 0; i < 8; i++)
        xv_ph(&r, i, (uint32_t)XV_SB(xv_gb(b, i + 8 * lo)));
    return r;
}

static inline xe_v128 xv_upkh(xe_v128 b, int lo)
{
    xe_v128 r;
    for (int i = 0; i < 4; i++)
        r.u32[i] = (uint32_t)XV_SH(xv_gh(b, i + 4 * lo));
    return r;
}

static inline xe_v128 xv_upkpx(xe_v128 b, int lo)
{
    xe_v128 r;
    for (int i = 0; i < 4; i++) {
        uint32_t h = xv_gh(b, i + 4 * lo);
        r.u32[i] = ((h & 0x8000) ? 0xFF000000u : 0) | ((h >> 10) & 0x1F) << 16 | ((h >> 5) & 0x1F) << 8 | (h & 0x1F);
    }
    return r;
}

/* sums across */
static inline xe_v128 xv_sum4ubs(xe_v128 a, xe_v128 b, uint32_t *vscr)
{
    xe_v128 r;
    for (int i = 0; i < 4; i++) {
        int64_t s = b.u32[i];
        for (int j = 0; j < 4; j++)
            s += xv_gb(a, 4 * i + j);
        r.u32[i] = (uint32_t)xv_sat(s, 0, 4294967295LL, vscr);
    }
    return r;
}

static inline xe_v128 xv_sum4sbs(xe_v128 a, xe_v128 b, uint32_t *vscr)
{
    xe_v128 r;
    for (int i = 0; i < 4; i++) {
        int64_t s = XV_SW(b.u32[i]);
        for (int j = 0; j < 4; j++)
            s += XV_SB(xv_gb(a, 4 * i + j));
        r.u32[i] = (uint32_t)xv_sat(s, INT32_MIN, INT32_MAX, vscr);
    }
    return r;
}

static inline xe_v128 xv_sum4shs(xe_v128 a, xe_v128 b, uint32_t *vscr)
{
    xe_v128 r;
    for (int i = 0; i < 4; i++) {
        int64_t s = XV_SW(b.u32[i]) + XV_SH(xv_gh(a, 2 * i)) + XV_SH(xv_gh(a, 2 * i + 1));
        r.u32[i] = (uint32_t)xv_sat(s, INT32_MIN, INT32_MAX, vscr);
    }
    return r;
}

static inline xe_v128 xv_sum2sws(xe_v128 a, xe_v128 b, uint32_t *vscr)
{
    xe_v128 r = xv_words(0, 0, 0, 0);
    for (int i = 0; i < 2; i++) {
        int64_t s = XV_SW(a.u32[2 * i]) + XV_SW(a.u32[2 * i + 1]) + XV_SW(b.u32[2 * i + 1]);
        r.u32[2 * i + 1] = (uint32_t)xv_sat(s, INT32_MIN, INT32_MAX, vscr);
    }
    return r;
}

static inline xe_v128 xv_sumsws(xe_v128 a, xe_v128 b, uint32_t *vscr)
{
    int64_t s = XV_SW(b.u32[3]);
    for (int i = 0; i < 4; i++)
        s += XV_SW(a.u32[i]);
    return xv_words(0, 0, 0, (uint32_t)xv_sat(s, INT32_MIN, INT32_MAX, vscr));
}

/* multiply-adds and multiply-sums (VA form: a, b, c) */
static inline xe_v128 xv_mhaddshs(xe_v128 a, xe_v128 b, xe_v128 c, int round, uint32_t *vscr)
{
    xe_v128 r = a;
    for (int i = 0; i < 8; i++) {
        int64_t p = XV_SH(xv_gh(a, i)) * XV_SH(xv_gh(b, i)) + (round ? 0x4000 : 0);
        xv_ph(&r, i, (uint32_t)xv_sat((p >> 15) + XV_SH(xv_gh(c, i)), -32768, 32767, vscr));
    }
    return r;
}

static inline xe_v128 xv_mladduhm(xe_v128 a, xe_v128 b, xe_v128 c)
{
    xe_v128 r = a;
    for (int i = 0; i < 8; i++)
        xv_ph(&r, i, xv_gh(a, i) * xv_gh(b, i) + xv_gh(c, i));
    return r;
}

/* vmsumubm (kind 0: unsigned bytes), vmsummbm (1: signed bytes of a, unsigned bytes of b), modulo 2^32 */
static inline xe_v128 xv_msumb(xe_v128 a, xe_v128 b, xe_v128 c, int kind)
{
    xe_v128 r;
    for (int i = 0; i < 4; i++) {
        int64_t s = c.u32[i];
        for (int j = 0; j < 4; j++) {
            uint32_t x = xv_gb(a, 4 * i + j), y = xv_gb(b, 4 * i + j);
            s += (kind ? XV_SB(x) : (int64_t)x) * (int64_t)y;
        }
        r.u32[i] = (uint32_t)s;
    }
    return r;
}

/* vmsumuhm / vmsumuhs (sgn 0), vmsumshm / vmsumshs (sgn 1); sat: saturating, else modulo 2^32 */
static inline xe_v128 xv_msumh(xe_v128 a, xe_v128 b, xe_v128 c, int sgn, int sat, uint32_t *vscr)
{
    xe_v128 r;
    for (int i = 0; i < 4; i++) {
        int64_t s = sgn ? XV_SW(c.u32[i]) : (int64_t)c.u32[i];
        for (int j = 0; j < 2; j++) {
            uint32_t x = xv_gh(a, 2 * i + j), y = xv_gh(b, 2 * i + j);
            s += sgn ? XV_SH(x) * XV_SH(y) : (int64_t)x * y;
        }
        if (sat)
            s = sgn ? xv_sat(s, INT32_MIN, INT32_MAX, vscr) : xv_sat(s, 0, 4294967295LL, vscr);
        r.u32[i] = (uint32_t)s;
    }
    return r;
}

/* ---- D3D pack / unpack (vpkd3d128 / vupkd3d128) ----
 * Packing takes floats biased so that their low mantissa bits hold the integer, 3.0 + n * 2^-22 (0x40400000 + n),
 * clamps them as floats (a NaN clamps to the low bound) and keeps the low bits. Unpacking gives the biased floats
 * back (the most negative value of a signed field, which packing never makes, gives a quiet NaN). 32-bit results
 * sit in element w (3), 64-bit ones in z:w (z the high word). Types: 0 D3DCOLOR (A R G B = w x y z, unpack: 1.0 +
 * n * 2^-23), 1 NORMSHORT2, 2 NORMPACKED32 (w:2 unsigned, z y x: 10 signed), 3 FLOAT16_2, 4 NORMSHORT4, 5 FLOAT16_4,
 * 6 NORMPACKED64 (w:4 unsigned, z y x: 20 signed). */

static inline uint32_t xv_clampbits(uint32_t u, uint32_t lo, uint32_t hi)
{
    float f = xv_f(u);
    if (!(f >= xv_f(lo)))
        return lo;
    if (f > xv_f(hi))
        return hi;
    return u;
}

/* float -> half, rounding toward zero (as Xenia: an overflow gives the largest finite half; inf, NaN kept) */
static inline uint32_t xv_f2h(uint32_t u)
{
    uint32_t sign = (u >> 16) & 0x8000u;
    uint32_t e = (u >> 23) & 0xFF, m = u & 0x7FFFFFu;
    if (e == 0xFF)
        return sign | 0x7C00u | (m ? 0x200u | (m >> 13) : 0);
    int32_t he = (int32_t)e - 127 + 15;
    if (he >= 31)
        return sign | 0x7BFFu;
    if (he <= 0) {
        if (he < -10)
            return sign;
        m |= 0x800000u;
        return sign | (m >> (14 - he));
    }
    return sign | (uint32_t)he << 10 | (m >> 13);
}

static inline uint32_t xv_h2f(uint32_t h)
{
    uint32_t sign = (h & 0x8000u) << 16, e = (h >> 10) & 0x1F, m = h & 0x3FF;
    if (e == 0x1F)
        return sign | 0x7F800000u | (m << 13);
    if (e == 0) {
        if (!m)
            return sign;
        int32_t ee = 1;
        while (!(m & 0x400)) {
            m <<= 1;
            ee--;
        }
        return sign | (uint32_t)(ee + 127 - 15) << 23 | (m & 0x3FF) << 13;
    }
    return sign | (e + 127 - 15) << 23 | m << 13;
}

static inline xe_v128 xv_d3dpack(xe_v128 v, int type)
{
    xe_v128 r = xv_words(0, 0, 0, 0);
    uint32_t c[4];
    switch (type) {
    case 0: /* D3DCOLOR */
        for (int k = 0; k < 4; k++)
            c[k] = xv_clampbits(v.u32[k], 0x40400000u, 0x404000FFu) & 0xFF;
        r.u32[3] = c[3] << 24 | c[0] << 16 | c[1] << 8 | c[2];
        break;
    case 1: /* NORMSHORT2 */
    case 4: /* NORMSHORT4 */
        for (int k = 0; k < 4; k++)
            c[k] = xv_clampbits(v.u32[k], 0x403F8001u, 0x40407FFFu) & 0xFFFF;
        if (type == 1) {
            r.u32[3] = c[0] << 16 | c[1];
        } else {
            r.u32[2] = c[0] << 16 | c[1];
            r.u32[3] = c[2] << 16 | c[3];
        }
        break;
    case 2: /* NORMPACKED32 */
        for (int k = 0; k < 3; k++)
            c[k] = xv_clampbits(v.u32[k], 0x403FFE01u, 0x404001FFu) & 0x3FF;
        c[3] = xv_clampbits(v.u32[3], 0x40400000u, 0x40400003u) & 3;
        r.u32[3] = c[0] | c[1] << 10 | c[2] << 20 | c[3] << 30;
        break;
    case 3: /* FLOAT16_2 */
        r.u32[3] = xv_f2h(v.u32[0]) << 16 | xv_f2h(v.u32[1]);
        break;
    case 5: /* FLOAT16_4 */
        r.u32[2] = xv_f2h(v.u32[0]) << 16 | xv_f2h(v.u32[1]);
        r.u32[3] = xv_f2h(v.u32[2]) << 16 | xv_f2h(v.u32[3]);
        break;
    case 6: { /* NORMPACKED64 */
        for (int k = 0; k < 3; k++)
            c[k] = xv_clampbits(v.u32[k], 0x40380001u, 0x4047FFFFu) & 0xFFFFF;
        c[3] = xv_clampbits(v.u32[3], 0x40400000u, 0x4040000Fu) & 15;
        uint64_t q = (uint64_t)c[0] | (uint64_t)c[1] << 20 | (uint64_t)c[2] << 40 | (uint64_t)c[3] << 60;
        r.u32[2] = (uint32_t)(q >> 32);
        r.u32[3] = (uint32_t)q;
        break;
    }
    default:
        break;
    }
    return r;
}

/* vpkd3d128: the packed value merged into vD by pack (1: 32 bits, 2 / 3: 64 bits) and shift; per result element a
 * (source, element) pair, source 0 = vD, 1 = the packed value. Types 7 and pack 0 are invalid: vD unchanged. */
static inline xe_v128 xv_pkd3d(xe_v128 d, xe_v128 b, int type, int pack, int shift)
{
    static const uint8_t perm[3][4][8] = {
        {{0, 0, 0, 1, 0, 2, 1, 3}, {0, 0, 0, 1, 1, 3, 0, 3}, {0, 0, 1, 3, 0, 2, 0, 3}, {1, 3, 0, 1, 0, 2, 0, 3}},
        {{0, 0, 0, 1, 1, 2, 1, 3}, {0, 0, 1, 2, 1, 3, 0, 3}, {1, 2, 1, 3, 0, 2, 0, 3}, {1, 3, 0, 1, 0, 2, 0, 3}},
        {{0, 0, 0, 1, 1, 2, 1, 3}, {0, 0, 1, 2, 1, 3, 0, 3}, {1, 2, 1, 3, 0, 2, 0, 3}, {0, 0, 0, 1, 0, 2, 1, 2}},
    };
    if (pack < 1 || pack > 3 || type > 6)
        return d;
    xe_v128 p = xv_d3dpack(b, type), r;
    const uint8_t *t = perm[pack - 1][shift & 3];
    for (int k = 0; k < 4; k++)
        r.u32[k] = t[2 * k] ? p.u32[t[2 * k + 1]] : d.u32[t[2 * k + 1]];
    return r;
}

static inline uint32_t xv_unbias(int32_t n, uint32_t overflow)
{
    uint32_t u = 0x40400000u + (uint32_t)n;
    return u == overflow ? 0x7FC00000u : u;
}

static inline xe_v128 xv_upkd3d(xe_v128 b, int type)
{
    xe_v128 r = xv_words(0, 0, 0, 0x3F800000u);
    uint32_t w = b.u32[3];
    switch (type) {
    case 0:
        r = xv_words(0x3F800000u | ((w >> 16) & 0xFF), 0x3F800000u | ((w >> 8) & 0xFF), 0x3F800000u | (w & 0xFF),
                     0x3F800000u | (w >> 24));
        break;
    case 1:
        r.u32[0] = xv_unbias((int16_t)(w >> 16), 0x403F8000u);
        r.u32[1] = xv_unbias((int16_t)w, 0x403F8000u);
        break;
    case 4:
        r.u32[0] = xv_unbias((int16_t)(b.u32[2] >> 16), 0x403F8000u);
        r.u32[1] = xv_unbias((int16_t)b.u32[2], 0x403F8000u);
        r.u32[2] = xv_unbias((int16_t)(w >> 16), 0x403F8000u);
        r.u32[3] = xv_unbias((int16_t)w, 0x403F8000u);
        break;
    case 2:
        for (int k = 0; k < 3; k++)
            r.u32[k] = xv_unbias((int32_t)(w << (22 - 10 * k)) >> 22, 0x403FFE00u);
        r.u32[3] = 0x3F800000u + (w >> 30);
        break;
    case 3:
        r.u32[0] = xv_h2f(w >> 16);
        r.u32[1] = xv_h2f(w & 0xFFFF);
        break;
    case 5:
        r = xv_words(xv_h2f(b.u32[2] >> 16), xv_h2f(b.u32[2] & 0xFFFF), xv_h2f(w >> 16), xv_h2f(w & 0xFFFF));
        break;
    case 6: {
        uint64_t q = (uint64_t)b.u32[2] << 32 | w;
        for (int k = 0; k < 3; k++)
            r.u32[k] = xv_unbias((int32_t)((uint32_t)(q >> (20 * k)) << 12) >> 12, 0x40380000u);
        r.u32[3] = 0x3F800000u + (uint32_t)(q >> 60);
        break;
    }
    default:
        break;
    }
    return r;
}

#endif
