/* x360_vmx_ref: the VMX helpers as the packs build them (host-order lanes, GCC / clang vector ops: xenon_ops.h XV_LE /
 * XV_VEC; NEON / SSE2 / SSSE3 intrinsics: XV_NEON / XV_SSE2 / XV_SSSE3; build it with -mssse3 for the SSSE3 vperm)
 * against the same helpers as plain C (x360_vmx_ref_scalar.c, -DXV_SCALAR): equal results and VSCR on
 * random vectors and on the edges (saturation bounds, the sticky SAT bit, shift counts 0 / 15 and above, permute
 * controls with bits 5-7 set), and the float helpers' NaN lanes against the AltiVec rule (float_checks). The interpreter runs the same helpers, so the interpreter-vs-lifted tests cannot see a
 * mistake in them: this test can. */
#include <stdio.h>
#include <stdlib.h>

#include "x360_vmx_ref.h"

XV_REF_SET(xv_ref_fast)

static uint64_t rng = 0x9E3779B97F4A7C15ull;

static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t)(rng >> 16);
}

/* a lane value that is often an edge: 0, 1, -1, the signed / unsigned bounds and their neighbours, small shifts */
static uint32_t edge16(void)
{
    static const uint16_t e[] = {0, 1, 2, 15, 16, 17, 0x7FFE, 0x7FFF, 0x8000, 0x8001, 0xFFFE, 0xFFFF, 0x00FF, 0x0100,
                                 0x7F, 0x80, 0x81, 0xFF7F, 0xFF80};
    return (rnd() & 3) ? e[rnd() % (sizeof e / sizeof e[0])] : rnd() & 0xFFFF;
}

static xe_v128 vec(int mode)
{
    xe_v128 v;
    for (int k = 0; k < 4; k++) {
        if (mode == 0)
            v.u32[k] = rnd() ^ rnd() << 16;
        else if (mode == 1)
            v.u32[k] = edge16() << 16 | edge16();
        else { /* bytes at the edges */
            static const uint8_t e[] = {0, 1, 7, 8, 9, 0x7F, 0x80, 0x81, 0xFE, 0xFF, 0x1F, 0x20, 0x3F, 0xE0};
            uint32_t w = 0;
            for (int j = 0; j < 4; j++)
                w = w << 8 | ((rnd() & 3) ? e[rnd() % sizeof e] : (rnd() & 0xFF));
            v.u32[k] = w;
        }
    }
    return v;
}

static int fails;

static void cmpv(const char *what, int n, xe_v128 a, xe_v128 b, xe_v128 c, xe_v128 f, xe_v128 r, uint32_t vf,
                 uint32_t vr)
{
    if (!memcmp(&f, &r, 16) && vf == vr)
        return;
    if (++fails <= 20)
        printf("FAIL %s #%d: a %08X%08X%08X%08X b %08X%08X%08X%08X c %08X%08X%08X%08X: fast %08X%08X%08X%08X vscr "
               "%X, reference %08X%08X%08X%08X vscr %X\n",
               what, n, a.u32[0], a.u32[1], a.u32[2], a.u32[3], b.u32[0], b.u32[1], b.u32[2], b.u32[3], c.u32[0],
               c.u32[1], c.u32[2], c.u32[3], f.u32[0], f.u32[1], f.u32[2], f.u32[3], vf, r.u32[0], r.u32[1], r.u32[2],
               r.u32[3], vr);
}

static const char *const bin_names[] = {
#define XV_REF_NAME(n) #n,
    XV_REF_BIN(XV_REF_NAME)};
static const char *const sat_names[] = {XV_REF_SAT(XV_REF_NAME)};

/* halfword lanes for a pack mode, mostly in its range (VSCR[SAT] stays as it was), now and then one at or past an
 * edge of it */
static xe_v128 pack_vec(int mode)
{
    static const int16_t lo[4] = {-32768, 0, 0, -128}, hi[4] = {32767, 255, 255, 127};
    static const uint16_t out[] = {0x0100, 0x0101, 0x7FFF, 0x8000, 0xFFFF, 0xFF7F, 0xFF80, 0x0080, 0x00FF, 0x007F};
    xe_v128 v;
    for (int k = 0; k < 8; k++) {
        int span = hi[mode] - lo[mode] + 1;
        v.u16[k] = (uint16_t)(lo[mode] + (int)(rnd() % (uint32_t)span));
    }
    if (rnd() % 4 == 0)
        v.u16[rnd() & 7] = out[rnd() % (sizeof out / sizeof out[0])];
    return v;
}

/* ---- float helpers: NaNs, infinities, denormals ----
 * The plain C and the packs' build are compared like the rest; on top of that each NaN lane is checked against the
 * AltiVec rule worked out from the operands' bits alone (no host float arithmetic, so the host's NaN conventions cannot
 * leak into the expectation), and every result goes into a digest printed at the end: the same on every compiler,
 * -O level and host (x86-64, aarch64), which run_vmx_matrix-style comparisons check. Lanes as the lifter feeds them:
 * inputs and result through xv_daz, VSCR[NJ] on and off. */

static const uint32_t fspecial[] = {
    0x00000000u, 0x80000000u, 0x3F800000u, 0xBF800000u, 0x40000000u, 0x3F000000u, 0xC0400000u, 0x42F6E979u,
    0x00000001u, 0x807FFFFFu, 0x00400000u, 0x80000010u,                         /* denormals */
    0x00800000u, 0x80800000u, 0x7F7FFFFFu, 0xFF7FFFFFu, 0x5E800000u, 0xDE800000u, /* min normal, max, 2^62 */
    0x7F800000u, 0xFF800000u,                                                   /* infinities */
    0x7FC00000u, 0xFFC00000u, 0x7FC12345u, 0xFFE54321u, 0x7FFFFFFFu,            /* quiet NaNs */
    0x7F800001u, 0xFF812345u, 0x7FA00000u, 0xFFBFFFFFu,                         /* signalling NaNs */
};
#define NFSPECIAL (int)(sizeof fspecial / sizeof fspecial[0])

static uint32_t flane(void)
{
    if (rnd() & 3)
        return fspecial[rnd() % NFSPECIAL];
    uint32_t u = rnd(); /* one call per statement: the digest needs the same draws on every compiler */
    u ^= rnd() << 16;
    return (rnd() & 1) ? (u & 0x83FFFFFFu) | 0x3C000000u : u; /* now and then near 1.0, else anything */
}

static xe_v128 fvec(void)
{
    xe_v128 v;
    for (int k = 0; k < 4; k++)
        v.u32[k] = flane();
    return v;
}

static int f_nan(uint32_t u) { return (u & 0x7FFFFFFFu) > 0x7F800000u; }
static int f_inf(uint32_t u) { return (u & 0x7FFFFFFFu) == 0x7F800000u; }
static int f_zero(uint32_t u) { return (u & 0x7FFFFFFFu) == 0; }
static int f_big(uint32_t u) { return ((u >> 23) & 0xFF) >= 127 + 60 && !f_inf(u) && !f_nan(u); }

#define XV_REF_FLOAT_DIGEST 0x6ACA0947u
#define O_QNAN 0x7FC00000u /* AltiVec PEM 3.2.5.5: the NaN an operation makes */
#define NOTNAN 1u /* the oracle's "no NaN here" (1 is not a NaN's bit pattern) */

static uint32_t q(uint32_t u) { return u | 0x00400000u; }

static uint32_t o_pick(uint32_t a, uint32_t b, uint32_t c, int na)
{
    if (na > 0 && f_nan(a))
        return q(a);
    if (na > 1 && f_nan(b))
        return q(b);
    if (na > 2 && f_nan(c))
        return q(c);
    return 0;
}

/* the expected NaN of a lane (inputs already flushed), or NOTNAN */
static uint32_t o_add(uint32_t a, uint32_t b, int sub)
{
    uint32_t n = o_pick(a, b, 0, 2);
    if (n)
        return n;
    if (f_inf(a) && f_inf(b) && ((a ^ b ^ (sub ? 0x80000000u : 0)) >> 31))
        return O_QNAN;
    return NOTNAN;
}

static uint32_t o_mul(uint32_t a, uint32_t b)
{
    uint32_t n = o_pick(a, b, 0, 2);
    if (n)
        return n;
    return (f_inf(a) && f_zero(b)) || (f_zero(a) && f_inf(b)) ? O_QNAN : NOTNAN;
}

/* a * c + b (neg: a * c - b, the NaN not negated); precedence a, b, c */
static uint32_t o_fma(uint32_t a, uint32_t c, uint32_t b, int neg)
{
    uint32_t n = o_pick(a, b, c, 3);
    if (n)
        return n;
    if ((f_inf(a) && f_zero(c)) || (f_zero(a) && f_inf(c)))
        return O_QNAN;
    if ((f_inf(a) || f_inf(c)) && f_inf(b) && ((a ^ c ^ b ^ (neg ? 0x80000000u : 0)) >> 31))
        return O_QNAN;
    return NOTNAN;
}

/* our vmsum rule (xenon_ops.h): products, then sums left to right; F stands for a finite value */
#define O_F 2u
static uint32_t o_msum(xe_v128 a, xe_v128 b, int n)
{
    uint32_t s = 0;
    for (int k = 0; k < n; k++) {
        uint32_t x = a.u32[k], y = b.u32[k], p;
        if (f_nan(x) || f_nan(y))
            p = o_pick(x, y, 0, 2);
        else if ((f_inf(x) && f_zero(y)) || (f_zero(x) && f_inf(y)))
            p = O_QNAN;
        else if (f_inf(x) || f_inf(y))
            p = 0x7F800000u | ((x ^ y) & 0x80000000u);
        else
            p = O_F;
        if (k == 0)
            s = p;
        else if (f_nan(s))
            s = q(s);
        else if (f_nan(p))
            s = q(p);
        else if (f_inf(s) && f_inf(p))
            s = ((s ^ p) >> 31) ? O_QNAN : s;
        else if (f_inf(p))
            s = p;
    }
    return f_nan(s) ? s : NOTNAN;
}

static uint32_t o_one(int op, uint32_t b)
{
    if (f_nan(b))
        return q(b);
    if ((op == 1 || op == 3) && (b >> 31) && !f_zero(b)) /* vrsqrtefp, vlogefp of a negative number */
        return O_QNAN;
    return NOTNAN;
}

static uint32_t fdigest = 2166136261u;

static void dig(uint32_t u)
{
    for (int i = 0; i < 4; i++) {
        fdigest ^= (u >> (8 * i)) & 0xFF;
        fdigest *= 16777619u;
    }
}

static void dig_v(xe_v128 v)
{
    for (int k = 0; k < 4; k++)
        dig(v.u32[k]);
}

static int ffails;

/* lane k of r against the oracle's e (NOTNAN: r must not be a NaN) */
static void fcheck(const char *what, int n, int k, uint32_t e, uint32_t r, xe_v128 a, xe_v128 b, xe_v128 c)
{
    if (e == NOTNAN ? !f_nan(r) : r == e)
        return;
    if (++ffails <= 20)
        printf("FAIL %s #%d lane %d: a %08X b %08X c %08X: %08X, the rule says %s%08X\n", what, n, k, a.u32[k], b.u32[k],
               c.u32[k], r, e == NOTNAN ? "not a NaN, not " : "", e == NOTNAN ? r : e);
}

static void float_checks(const xv_ref_set *F, const xv_ref_set *R, long *checks)
{
    static const char *const f2n[] = {XV_REF_F2(XV_REF_NAME)}, *const f3n[] = {XV_REF_F3(XV_REF_NAME)},
                             *const f1n[] = {XV_REF_F1(XV_REF_NAME)};
    const int nf2 = (int)(sizeof f2n / sizeof f2n[0]), nf3 = (int)(sizeof f3n / sizeof f3n[0]),
              nf1 = (int)(sizeof f1n / sizeof f1n[0]);
    rng = 0x2545F4914F6CDD1Dull; /* a stream of its own: the digest does not depend on the checks above */
    for (int n = 0; n < 100000; n++) {
        uint32_t vscr = (n & 1) ? XV_VSCR_NJ : 0;
        xe_v128 a0 = fvec(), b0 = fvec(), c0 = fvec();
        xe_v128 a = xv_daz(vscr, a0), b = xv_daz(vscr, b0), c = xv_daz(vscr, c0);
        for (int i = 0; i < nf2; i++, (*checks)++) {
            xe_v128 f = xv_daz(vscr, F->f2[i](a, b)), r = xv_daz(vscr, R->f2[i](a, b));
            cmpv(f2n[i], n, a, b, c, f, r, 0, 0);
            dig_v(r);
            if (i >= 5) { /* msum: the oracle knows no rounding, so not where a finite product could overflow */
                int big = 0;
                for (int k = 0; k < 4; k++)
                    big |= f_big(a.u32[k]) | f_big(b.u32[k]);
                if (!big)
                    for (int k = 0; k < 4; k++)
                        fcheck(f2n[i], n, k, o_msum(a, b, i == 5 ? 3 : 4), r.u32[k], a, b, c);
                continue;
            }
            for (int k = 0; k < 4; k++) {
                uint32_t x = a.u32[k], y = b.u32[k];
                uint32_t e = i == 0 ? o_add(x, y, 0) : i == 1 ? o_add(x, y, 1) : i == 2 ? o_mul(x, y) : o_pick(x, y, 0, 2);
                if (i >= 3 && !e)
                    e = NOTNAN;
                fcheck(f2n[i], n, k, e, r.u32[k], a, b, c);
            }
        }
        for (int i = 0; i < nf3; i++, (*checks)++) {
            xe_v128 f = xv_daz(vscr, F->f3[i](a, c, b)), r = xv_daz(vscr, R->f3[i](a, c, b));
            cmpv(f3n[i], n, a, b, c, f, r, 0, 0);
            dig_v(r);
            for (int k = 0; k < 4; k++)
                fcheck(f3n[i], n, k, o_fma(a.u32[k], c.u32[k], b.u32[k], i), r.u32[k], a, b, c);
        }
        for (int i = 0; i < nf1; i++, (*checks)++) {
            xe_v128 f = xv_daz(vscr, F->f1[i](b)), r = xv_daz(vscr, R->f1[i](b));
            cmpv(f1n[i], n, a, b, c, f, r, 0, 0);
            for (int k = 0; k < 4; k++) {
                uint32_t e = o_one(i, b.u32[k]);
                fcheck(f1n[i], n, k, e, r.u32[k], a, b, c);
                /* exp2f / log2f may differ in the last bit between C libraries: their finite results stay out */
                if (!(i == 2 || i == 3) || e != NOTNAN || f_inf(b.u32[k]) || f_zero(b.u32[k]))
                    dig(r.u32[k]);
            }
        }
        int imm = (int)(rnd() & 31);
        for (int s = 0; s < 2; s++, *checks += 2) {
            uint32_t vf = vscr | (n & 2 ? XV_VSCR_SAT : 0), vr = vf;
            xe_v128 f = s ? F->ctuxs(b, imm, &vf) : F->ctsxs(b, imm, &vf);
            xe_v128 r = s ? R->ctuxs(b, imm, &vr) : R->ctsxs(b, imm, &vr);
            cmpv(s ? "xv_ctuxs" : "xv_ctsxs", n, a, b, c, f, r, vf, vr);
            dig_v(r);
            dig(vr);
            int allnan = 1;
            for (int k = 0; k < 4; k++) {
                allnan &= f_nan(b.u32[k]);
                if (f_nan(b.u32[k]) && r.u32[k] && ++ffails <= 20)
                    printf("FAIL %s #%d lane %d: NaN %08X gives %08X, not 0\n", s ? "xv_ctuxs" : "xv_ctsxs", n, k,
                           b.u32[k], r.u32[k]);
            }
            if (allnan && vr != (vscr | (n & 2 ? XV_VSCR_SAT : 0)) && ++ffails <= 20)
                printf("FAIL %s #%d: NaN lanes changed VSCR to %X\n", s ? "xv_ctuxs" : "xv_ctsxs", n, vr);
            f = s ? F->cfux(a0, imm) : F->cfsx(a0, imm);
            r = s ? R->cfux(a0, imm) : R->cfsx(a0, imm);
            cmpv(s ? "xv_cfux" : "xv_cfsx", n, a0, b, c, f, r, 0, 0);
            dig_v(r);
        }
    }
    /* the cases the rule is about, spelled out: both operands NaNs, signalling after quiet, made NaNs */
    static const struct {
        const char *what;
        int op; /* 0 add, 1 sub, 2 mul, 3 max, 4 min, 5 madd, 6 nmsub */
        uint32_t a, b, c, want;
    } cases[] = {
        {"two NaNs: vA's", 0, 0x7FC11111u, 0x7FC22222u, 0, 0x7FC11111u},
        {"two NaNs: vA's (swapped)", 0, 0x7FC22222u, 0x7FC11111u, 0, 0x7FC22222u},
        {"quiet vA before signalling vB", 0, 0x7FC11111u, 0x7F822222u, 0, 0x7FC11111u},
        {"signalling vA quieted", 2, 0xFF811111u, 0x7FC22222u, 0, 0xFFC11111u},
        {"inf - inf", 1, 0x7F800000u, 0x7F800000u, 0, 0x7FC00000u},
        {"inf + -inf", 0, 0xFF800000u, 0x7F800000u, 0, 0x7FC00000u},
        {"0 * inf", 2, 0x80000000u, 0x7F800000u, 0, 0x7FC00000u},
        {"max of two NaNs", 3, 0xFFC33333u, 0x7FC44444u, 0, 0xFFC33333u},
        {"min of a number and a NaN", 4, 0x3F800000u, 0x7F855555u, 0, 0x7FC55555u},
        {"madd: vB (addend) before vC", 5, 0x3F800000u, 0x7FC66666u, 0x7FC77777u, 0x7FC66666u},
        {"madd: inf * 0 + NaN gives the NaN", 5, 0x7F800000u, 0x7FC66666u, 0, 0x7FC66666u},
        {"madd: inf * 0", 5, 0x7F800000u, 0x3F800000u, 0, 0x7FC00000u},
        {"nmsub: the NaN not negated", 6, 0x7FC88888u, 0x3F800000u, 0x3F800000u, 0x7FC88888u},
        {"nmsub: made NaN not negated", 6, 0x7F800000u, 0x7F800000u, 0x3F800000u, 0x7FC00000u},
    };
    for (int i = 0; i < (int)(sizeof cases / sizeof cases[0]); i++)
        for (int s = 0; s < 2; s++, (*checks)++) {
            const xv_ref_set *S = s ? R : F;
            xe_v128 a = xv_splat32(cases[i].a), b = xv_splat32(cases[i].b), c = xv_splat32(cases[i].c), r;
            switch (cases[i].op) {
            case 0: r = S->f2[0](a, b); break;
            case 1: r = S->f2[1](a, b); break;
            case 2: r = S->f2[2](a, b); break;
            case 3: r = S->f2[3](a, b); break;
            case 4: r = S->f2[4](a, b); break;
            case 5: r = S->f3[0](a, c, b); break;
            default: r = S->f3[1](a, c, b); break;
            }
            for (int k = 0; k < 4; k++)
                if (r.u32[k] != cases[i].want && ++ffails <= 40)
                    printf("FAIL %s (%s): %08X, want %08X\n", cases[i].what, s ? "plain C" : "fast", r.u32[k],
                           cases[i].want);
        }
    printf("x360_vmx_ref: float digest %08X\n", fdigest);
    /* the digest of every host, compiler and -O level so far (run_vmx_ref_matrix.sh): another means a float helper
     * depends on the build (or this test's draws changed: then the matrix again and the new value here) */
    if (fdigest != XV_REF_FLOAT_DIGEST && ++ffails)
        printf("FAIL float digest %08X, expected %08X\n", fdigest, XV_REF_FLOAT_DIGEST);
    fails += ffails;
}

int main(void)
{
    const xv_ref_set *F = &xv_ref_fast, *R = &xv_ref_scalar;
    const int nbin = (int)(sizeof bin_names / sizeof bin_names[0]), nsat = (int)(sizeof sat_names / sizeof sat_names[0]);
    long checks = 0;
    for (int n = 0; n < 200000; n++) {
        xe_v128 a = vec(n % 3), b = vec((n / 3) % 3), c = vec((n / 9) % 3);
        uint32_t vin = (rnd() & 1) ? XV_VSCR_SAT : 0; /* the SAT bit sticks: set before, it stays */
        vin |= (rnd() & 1) ? XV_VSCR_NJ : 0;
        for (int k = 0; k < nbin; k++, checks++)
            cmpv(bin_names[k], n, a, b, c, F->bin[k](a, b), R->bin[k](a, b), 0, 0);
        for (int k = 0; k < nsat; k++, checks++) {
            uint32_t vf = vin, vr = vin;
            xe_v128 f = F->sat[k](a, b, &vf), r = R->sat[k](a, b, &vr);
            cmpv(sat_names[k], n, a, b, c, f, r, vf, vr);
        }
        for (int lo = 0; lo < 2; lo++, checks += 2) {
            cmpv(lo ? "xv_mrgb lo" : "xv_mrgb hi", n, a, b, c, F->mrgb(a, b, lo), R->mrgb(a, b, lo), 0, 0);
            cmpv(lo ? "xv_mrgh lo" : "xv_mrgh hi", n, a, b, c, F->mrgh(a, b, lo), R->mrgh(a, b, lo), 0, 0);
        }
        for (int m = 0; m < 4; m++, checks++) {
            uint32_t vf = vin, vr = vin;
            xe_v128 f = F->pkh(a, b, m, &vf), r = R->pkh(a, b, m, &vr);
            cmpv("xv_pkh", n * 4 + m, a, b, c, f, r, vf, vr);
        }
        for (int m = 0; m < 4; m++, checks++) { /* lanes in the mode's range: the SAT bit only where one is not */
            xe_v128 pa = pack_vec(m), pb = pack_vec(m);
            uint32_t vf = vin, vr = vin;
            xe_v128 f = F->pkh(pa, pb, m, &vf), r = R->pkh(pa, pb, m, &vr);
            cmpv("xv_pkh in range", n * 4 + m, pa, pb, c, f, r, vf, vr);
        }
        cmpv("xv_perm", n, a, b, c, F->perm(a, b, c), R->perm(a, b, c), 0, 0);
        xe_v128 d = a; /* denormals, zeros and normals of both signs, NJ on and off */
        for (int k = 0; k < 4; k++)
            d.u32[k] = (rnd() & 1) ? (a.u32[k] & 0x807FFFFFu) | ((rnd() & 1) ? 0 : 0x00800000u) : a.u32[k];
        cmpv("xv_daz", n, d, b, c, F->daz(vin, d), R->daz(vin, d), vin, vin);
        cmpv("xv_lvsl", n, a, b, c, F->lvsl(a.u32[0]), R->lvsl(a.u32[0]), 0, 0);
        cmpv("xv_lvsr", n, a, b, c, F->lvsr(a.u32[0]), R->lvsr(a.u32[0]), 0, 0);
        cmpv("xv_sldoi", n, a, b, c, F->sldoi(a, b, n & 15), R->sldoi(a, b, n & 15), 0, 0);
        uint8_t bf[16], br[16];
        F->to_bytes(a, bf);
        R->to_bytes(a, br);
        if (memcmp(bf, br, 16) && ++fails <= 20)
            printf("FAIL xv_to_bytes #%d\n", n);
        cmpv("xv_from_bytes", n, a, b, c, F->from_bytes(bf), R->from_bytes(br), 0, 0);
        checks += 4;
        /* vector loads and stores: two machines with the same memory, one per build; ea anywhere in a 64-byte window
         * whose blocks partly fall outside the memory (the harness's xe_ptr refuses those: NULL) */
        uint8_t mf[64], mr[64];
        for (int k = 0; k < 64; k++)
            mf[k] = mr[k] = (uint8_t)rnd();
        xe_machine Mf, Mr;
        memset(&Mf, 0, sizeof Mf);
        memset(&Mr, 0, sizeof Mr);
        Mf.mem = mf, Mr.mem = mr;
        Mf.mem_base = Mr.mem_base = 0x1000;
        Mf.mem_size = Mr.mem_size = 64;
        uint32_t ea = 0x1000 - 16 + (rnd() % 96);
        xe_v128 z = vec(0);
        cmpv("xv_lvlx", n, a, b, c, F->lvlx(&Mf, ea), R->lvlx(&Mr, ea), 0, 0);
        cmpv("xv_lvrx", n, a, b, c, F->lvrx(&Mf, ea), R->lvrx(&Mr, ea), 0, 0);
        uint32_t sz = 1u << (rnd() % 3);
        xe_v128 lf = z, lr = z;
        F->lve(&Mf, &lf, ea, sz);
        R->lve(&Mr, &lr, ea, sz);
        cmpv("xv_lve", n, a, b, c, lf, lr, sz, sz);
        F->stvlx(&Mf, ea, a);
        R->stvlx(&Mr, ea, a);
        F->stvrx(&Mf, ea + 5, b);
        R->stvrx(&Mr, ea + 5, b);
        F->stve(&Mf, ea + 9, c, sz);
        R->stve(&Mr, ea + 9, c, sz);
        if ((memcmp(mf, mr, 64) || Mf.faults != Mr.faults) && ++fails <= 20)
            printf("FAIL vector stores #%d at %08X (element size %u)\n", n, ea, sz);
        checks += 6;
    }
    float_checks(F, R, &checks);
    if (fails) {
        printf("x360_vmx_ref: %d of %ld checks differ\n", fails, checks);
        return 1;
    }
    printf("x360_vmx_ref: %ld checks, the fast helpers equal the plain C\n", checks);
    return 0;
}
