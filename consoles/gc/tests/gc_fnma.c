/* gc_fnma: fnmadd / fnmsub / fnmadds / fnmsubs / ps_nmadd / ps_nmsub keep the sign of an exact zero (XBA-254).
 *
 * Each function below has the shape the lifter (tools/ppc/lift.py, gc_tools/gekko.py) and the interpreter (gc_cpu.c)
 * use. Operands come through volatiles and the functions are noinline, so the compiler cannot fold the result: on
 * AArch64, GCC turned the old -fma(a, c, -b) into fmsub, which gives +0 where the 750 gives -0. A NaN result keeps
 * its sign (750 manual: « QNaNs propagate with no effect on their sign bit »). Build at -O0 and -O2 on x86-64 and
 * arm64 (aarch64 gcc + qemu) to cover both hosts. */
#include <stdio.h>

#include "ppc_ops.h"

#define NOINL __attribute__((noinline))

static NOINL double fnmsub(double a, double c, double b) { return ppc_fnma(fma(a, c, -b)); }
static NOINL double fnmadd(double a, double c, double b) { return ppc_fnma(fma(a, c, b)); }
/* fnmsubs / fnmadds: RC_FSET_S rounds the negated value to single */
static NOINL double fnmsubs(double a, double c, double b) { return ppc_single(ppc_fnma(fma(a, c, -b))); }
static NOINL double fnmadds(double a, double c, double b) { return ppc_single(ppc_fnma(fma(a, c, b))); }
/* ps_nmsub / ps_nmadd: rounded to single per slot (RC_PS), then negated */
static NOINL double ps_nmsub(double a, double c, double b) { return ppc_fnma(ppc_single(fma(a, c, -b))); }
static NOINL double ps_nmadd(double a, double c, double b) { return ppc_fnma(ppc_single(fma(a, c, b))); }

static int fails;

static void check(const char *what, double got, uint64_t want)
{
    uint64_t u = ppc_f2u(got);
    if (u != want) {
        printf("FAIL %s: %016llx, want %016llx\n", what, (unsigned long long)u, (unsigned long long)want);
        fails++;
    }
}

int main(void)
{
    volatile double two = 2.0, three = 3.0, six = 6.0, mthree = -3.0, half = 0.5;
    volatile double qnan = ppc_u2f(0x7FF8000000001234ull), nqnan = ppc_u2f(0xFFF8000000005678ull);
    const uint64_t mzero = 0x8000000000000000ull;

    /* exact zeros: -(2 * 3 - 6) and -(2 * 3 + -6) are -0 */
    check("fnmsub zero", fnmsub(two, three, six), mzero);
    check("fnmadd zero", fnmadd(two, three, -six), mzero);
    check("fnmsubs zero", fnmsubs(two, three, six), mzero);
    check("fnmadds zero", fnmadds(two, three, -six), mzero);
    check("ps_nmsub zero", ps_nmsub(two, three, six), mzero);
    check("ps_nmadd zero", ps_nmadd(two, three, -six), mzero);
    /* -(-0) = +0: a * c = -0 with b = +0 for fnmsub (-0 - +0 = -0), with b = -0 for fnmadd */
    check("fnmsub +0", fnmsub(-0.0 * two, three, 0.0), 0);
    check("fnmadd +0", fnmadd(-0.0 * two, three, -0.0), 0);
    /* ordinary values */
    check("fnmsub", fnmsub(two, three, half), ppc_f2u(-5.5));
    check("fnmadd", fnmadd(two, mthree, half), ppc_f2u(5.5));
    check("ps_nmadd", ps_nmadd(three, three, half), ppc_f2u(-9.5));
    /* NaN operands propagate with their sign and payload, not negated */
    check("fnmsub qnan", fnmsub(qnan, three, six), 0x7FF8000000001234ull);
    check("fnmadd -qnan", fnmadd(two, nqnan, six), 0xFFF8000000005678ull);
    check("ppc_fnma inf", ppc_fnma(ppc_u2f(0x7FF0000000000000ull)), 0xFFF0000000000000ull);
    if (fails)
        return 1;
    printf("gc_fnma: all checks passed\n");
    return 0;
}
