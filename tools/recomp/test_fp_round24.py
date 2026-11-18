"""recomp_fp_round24 (x87 precision control 24, recomp_types.h) tests the
range on the bits since XBA-273, and recomp_frndint rounds to nearest with
roundeven on arm64. Same results as the code they replaced, bit for bit:
edge patterns around 0, FLT_MIN, FLT_MAX, the float subnormals, inf and NaN
(both signs, neighbours one ulp either side), halves and integers up to
2^64, then random bit patterns and random values near the float range,
compiled at -O0 and -O2 against the old functions kept here as the reference
(frndint with the four rounding controls, fist at 16, 32 and 64 bits).
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

_ROOT = Path(__file__).resolve().parents[2]

_SRC = r'''
#define RECOMP_GENERATED_CODE 1
#include "recomp_types.h"
#include <stdio.h>
RECOMP_TLS RecompCpu g_cpu;

/* The function before XBA-273, verbatim. */
static double ref_round24(double x) {
    double ax = fabs(x);
    int e;
    if ((ax <= 3.4028234663852886e38 && ax >= 1.1754943508222875e-38) || ax == 0.0
        || x != x || ax == INFINITY)
        return (double)(float)x;
    x = frexp(x, &e);
    return ldexp((double)(float)x, e);
}

/* recomp_frndint and recomp_fist before XBA-273, verbatim. */
static double ref_frndint(double value, uint16_t control) {
    double rounded;
    if (!isfinite(value)) return value;
    switch((control>>10)&3) {
    case 1: rounded=floor(value); break;
    case 2: rounded=ceil(value); break;
    case 3: rounded=trunc(value); break;
    default: {
        double lo=floor(value), fraction=value-lo;
        rounded=lo;
        if(fraction>0.5 || (fraction==0.5 && fmod(lo,2.0)!=0.0)) rounded=lo+1.0;
        break;
    }
    }
    return rounded == 0.0 ? copysign(0.0, value) : rounded;
}
static int64_t ref_fist(double value, uint16_t control, unsigned bits) {
    double rounded = ref_frndint(value, control);
    double limit=ldexp(1.0,(int)bits-1);
    if(!isfinite(rounded) || rounded < -limit || rounded >= limit)
        return bits==64?INT64_MIN:-(INT64_C(1)<<(bits-1));
    return (int64_t)rounded;
}

static uint64_t bits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static double dbl(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
static unsigned long long s_n, s_bad;

static void check(uint64_t u)
{
    volatile double x = dbl(u);
    double a = recomp_fp_round24(x), b = ref_round24(x);
    s_n++;
    /* NaN payloads: both convert through float the same way; compare bits. */
    if (bits(a) != bits(b)) {
        if (s_bad++ < 10)
            printf("differs at %016llx: %016llx vs %016llx\n", (unsigned long long)u,
                   (unsigned long long)bits(a), (unsigned long long)bits(b));
    }
    {
        unsigned rc;
        for (rc = 0; rc < 4; rc++) {
            uint16_t cw = (uint16_t)(0x027F | (rc << 10));
            double fa = recomp_frndint(x, cw), fb = ref_frndint(x, cw);
            if (bits(fa) != bits(fb) && !(fa != fa && fb != fb)) {
                if (s_bad++ < 10)
                    printf("frndint rc %u differs at %016llx\n", rc, (unsigned long long)u);
            }
            if (recomp_fist(x, cw, 16) != ref_fist(x, cw, 16) || recomp_fist(x, cw, 32) != ref_fist(x, cw, 32) ||
                recomp_fist(x, cw, 64) != ref_fist(x, cw, 64)) {
                if (s_bad++ < 10)
                    printf("fist rc %u differs at %016llx\n", rc, (unsigned long long)u);
            }
        }
    }
}

static uint64_t s_rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17; return s_rng; }

int main(void)
{
    static const uint64_t edges[] = {
        0, 0x0000000000000001ull, 0x000FFFFFFFFFFFFFull, 0x0010000000000000ull,
        0x3810000000000000ull, 0x36A0000000000000ull, 0x3690000000000000ull,   /* FLT_MIN, float subnormals */
        0x47EFFFFFE0000000ull, 0x47EFFFFFF0000000ull, 0x47F0000000000000ull,   /* FLT_MAX, its rounding edge, 2^128 */
        0x3FF0000000000000ull, 0x7FEFFFFFFFFFFFFFull,
        0x7FF0000000000000ull, 0x7FF0000000000001ull, 0x7FF8000000000000ull, 0x7FFFFFFFFFFFFFFFull,
    };
    unsigned i;
    int d, sign;
    for (i = 0; i < sizeof edges / sizeof edges[0]; i++)
        for (sign = 0; sign < 2; sign++)
            for (d = -64; d <= 64; d++)
                check((edges[i] + (uint64_t)(int64_t)d) ^ (sign ? 0x8000000000000000ull : 0));
    for (i = 0; i < 4000000u; i++)
        check(rnd());
    for (i = 0; i < 2000000u; i++) {            /* halves, quarters and integers, small and up to 2^64 */
        uint64_t r = rnd();
        double v = (double)(int64_t)(r >> (r & 63)) / (double)(1u << (r >> 60 & 3));
        check(bits(v));
        check(bits(-v));
    }
    for (i = 0; i < 4000000u; i++) {            /* exponents around the float range */
        uint64_t u = rnd();
        uint64_t e = 0x360u + (u >> 54) % 0x130u;
        check((u & 0x800FFFFFFFFFFFFFull) | (e << 52));
    }
    printf("%llu values, %llu differences\n", s_n, s_bad);
    return s_bad != 0;
}
'''


class FpRound24Test(unittest.TestCase):
    def test_bits_test_matches_the_float_compares(self):
        cc = shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")
        if not cc or sys.platform == "win32":
            self.skipTest("no POSIX C compiler")
        with tempfile.TemporaryDirectory() as tmp:
            c = os.path.join(tmp, "t.c")
            with open(c, "w") as f:
                f.write(_SRC)
            for opt in ("-O0", "-O2"):
                exe = os.path.join(tmp, "t" + opt)
                r = subprocess.run([cc, "-std=gnu11", opt, "-I", str(_ROOT / "templates" / "runtime"),
                                    "-Wl,-undefined,dynamic_lookup" if sys.platform == "darwin"
                                    else "-Wl,--unresolved-symbols=ignore-all",
                                    c, "-o", exe, "-lm"], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stderr)
                run = subprocess.run([exe], capture_output=True, text=True, timeout=300)
                self.assertEqual(run.returncode, 0, opt + ": " + run.stdout)
                self.assertIn(" 0 differences", run.stdout)


if __name__ == "__main__":
    unittest.main()
