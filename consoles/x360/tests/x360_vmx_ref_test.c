/* x360_vmx_ref: the VMX helpers as the packs build them (host-order lanes, GCC / clang vector ops: xenon_ops.h XV_LE /
 * XV_VEC; NEON / SSE2 / SSSE3 intrinsics: XV_NEON / XV_SSE2 / XV_SSSE3; build it with -mssse3 for the SSSE3 vperm)
 * against the same helpers as plain C (x360_vmx_ref_scalar.c, -DXV_SCALAR): equal results and VSCR on
 * random vectors and on the edges (saturation bounds, the sticky SAT bit, shift counts 0 / 15 and above, permute
 * controls with bits 5-7 set). The interpreter runs the same helpers, so the interpreter-vs-lifted tests cannot see a
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
    if (fails) {
        printf("x360_vmx_ref: %d of %ld checks differ\n", fails, checks);
        return 1;
    }
    printf("x360_vmx_ref: %ld checks, the fast helpers equal the plain C\n", checks);
    return 0;
}
