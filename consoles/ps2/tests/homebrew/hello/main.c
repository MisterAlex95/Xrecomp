/* PS2 spike hello world (XBA-226): our own test program for the EE recompiler, built with PS2SDK's crt0, libkernel
 * and newlib. Everything it prints goes to the EE's SIO (sio_puts: the TX FIFO at 0x1000F180), the one output that
 * needs neither the IOP nor the GS. Each line is a value the R5900 computed; consoles/ps2/tests/hello.txt holds the
 * values the R5900's definitions give (64-bit GPRs, 128-bit MMI, the non-IEEE FPU: no infinities, no NaNs, no
 * denormals, rounding toward zero, results clamped to +-0x7FFFFFFF), and the test compares the two. */
#include <kernel.h>
#include <sio.h>
#include <stdio.h>
#include <string.h>
#include <tamtypes.h>

static char line[160];

#define say(...)                                                                                                     \
    do {                                                                                                             \
        snprintf(line, sizeof line, __VA_ARGS__);                                                                    \
        sio_putsn(line);                                                                                             \
    } while (0)

/* Keep the compiler from folding the checks: the values come from memory. */
static volatile u64 v64[4] = {0x0123456789ABCDEFull, 0xFEDCBA9876543210ull, 7, 0xFFFFFFFF80000000ull};
static volatile s32 v32[4] = {123456789, -98765, 0x7FFFFFFF, -3};
static volatile u32 fbits[8] = {
    0x7F000000u, /* 1.7e38 */
    0x41200000u, /* 10.0 */
    0x7F800000u, /* +Inf on an IEEE FPU, 2^128 on the R5900 */
    0x3F000000u, /* 0.5 */
    0x00000001u, /* a denormal: 0 on the R5900 */
    0x3F800000u, /* 1.0 */
    0x40400000u, /* 3.0 */
    0xFF800000u, /* -Inf / -2^128 */
};

static float fget(int i)
{
    u32 b = fbits[i];
    float f;
    memcpy(&f, &b, 4);
    return f;
}

static u32 fput(float f)
{
    u32 b;
    memcpy(&b, &f, 4);
    return b;
}

/* 128-bit operands in memory: GCC keeps a u128 in a pair of 64-bit registers, never in one 128-bit GPR, so each
 * MMI check loads its operands with LQ, runs one MMI instruction and stores the result with SQ, all in one asm. */
static u32 q_a[4] __attribute__((aligned(16))) = {1, 0xFFFFFFFFu, 0x80000000u, 0x12345678u};
static u32 q_b[4] __attribute__((aligned(16))) = {10, 2, 0x80000000u, 0x01010101u};
static u32 q_out[4] __attribute__((aligned(16)));

static void q_show(const char *name)
{
    say("%s = %08x %08x %08x %08x\n", name, (unsigned)q_out[3], (unsigned)q_out[2], (unsigned)q_out[1],
        (unsigned)q_out[0]);
}

/* rd = OP(rs = q_a, rt = q_b) */
#define MMI3(OP)                                                                                                     \
    do {                                                                                                             \
        __asm__ volatile("lq $8, 0(%0)\n\tlq $9, 0(%1)\n\t" OP " $10, $8, $9\n\tsq $10, 0(%2)"                       \
                         :                                                                                           \
                         : "r"(q_a), "r"(q_b), "r"(q_out)                                                            \
                         : "$8", "$9", "$10", "memory");                                                             \
        q_show(OP);                                                                                                  \
    } while (0)
/* rd = OP(rt = q_a) */
#define MMI1(OP)                                                                                                     \
    do {                                                                                                             \
        __asm__ volatile("lq $8, 0(%0)\n\t" OP " $10, $8\n\tsq $10, 0(%1)"                                           \
                         :                                                                                           \
                         : "r"(q_a), "r"(q_out)                                                                      \
                         : "$8", "$10", "memory");                                                                   \
        q_show(OP);                                                                                                  \
    } while (0)
/* rd = OP(rt = q_a, sa) */
#define MMISH(OP, SA)                                                                                                \
    do {                                                                                                             \
        __asm__ volatile("lq $8, 0(%0)\n\t" OP " $10, $8, " #SA "\n\tsq $10, 0(%1)"                                  \
                         :                                                                                           \
                         : "r"(q_a), "r"(q_out)                                                                      \
                         : "$8", "$10", "memory");                                                                   \
        q_show(OP);                                                                                                  \
    } while (0)

static void check_int(void)
{
    u64 a = v64[0], b = v64[1];
    say("add64 = %016llx\n", (unsigned long long)(a + b));
    say("sub64 = %016llx\n", (unsigned long long)(a - b));
    say("shl64 = %016llx\n", (unsigned long long)(a << 37));
    say("sra64 = %016llx\n", (unsigned long long)((s64)b >> 41));
    say("mul64 = %016llx\n", (unsigned long long)(a * v64[2]));
    say("div64 = %016llx\n", (unsigned long long)(b / v64[2]));
    say("mod64 = %llu\n", (unsigned long long)(b % v64[2]));
    say("sext = %016llx\n", (unsigned long long)(s64)(s32)(u32)v64[3]);
    s32 x = v32[0], y = v32[1];
    say("mul32 = %d\n", x * y);
    say("div32 = %d %d\n", x / y, x % y);
    say("add32 = %d\n", v32[2] + v32[3]);
    s64 wide = (s64)x * y;
    say("mulw = %lld\n", (long long)wide);
}

/* What only the R5900 has, written out (the compiler would not pick most of these on its own). */
static void check_r5900(void)
{
    u32 lo, hi, r;
    /* three-operand MULT: rd = LO */
    __asm__ volatile("mult %0, %3, %4\n\tmflo %1\n\tmfhi %2"
                     : "=&r"(r), "=&r"(lo), "=&r"(hi)
                     : "r"(v32[0]), "r"(v32[1])
                     : "hi", "lo");
    say("mult3 = %08x %08x %08x\n", (unsigned)r, (unsigned)hi, (unsigned)lo);
    /* MADD rd: HI:LO += rs * rt, rd = LO */
    __asm__ volatile("mtlo %3\n\tmthi %4\n\tmadd %0, %5, %6\n\tmflo %1\n\tmfhi %2"
                     : "=&r"(r), "=&r"(lo), "=&r"(hi)
                     : "r"(100), "r"(0), "r"(v32[0]), "r"(v32[3])
                     : "hi", "lo");
    say("madd = %08x %08x %08x\n", (unsigned)r, (unsigned)hi, (unsigned)lo);
    /* MULT1 / DIV1 / MFLO1 / MFHI1: the second multiplier pipeline */
    __asm__ volatile("mult1 %0, %3, %4\n\tmflo1 %1\n\tmfhi1 %2"
                     : "=&r"(r), "=&r"(lo), "=&r"(hi)
                     : "r"(v32[2]), "r"(v32[2]));
    say("mult1 = %08x %08x %08x\n", (unsigned)r, (unsigned)hi, (unsigned)lo);
    __asm__ volatile("div1 $0, %2, %3\n\tmflo1 %0\n\tmfhi1 %1" : "=&r"(lo), "=&r"(hi) : "r"(v32[0]), "r"(v32[1]));
    say("div1 = %08x %08x\n", (unsigned)lo, (unsigned)hi);
    /* PLZCW: leading bits equal to the sign, minus one, for each word of the low doubleword */
    u64 z;
    __asm__ volatile("plzcw %0, %1" : "=r"(z) : "r"(0x00000F00FFFF0000ull));
    say("plzcw = %016llx\n", (unsigned long long)z);
    /* BEQL not taken: its delay slot does not run */
    u32 k = 5;
    __asm__ volatile(".set noreorder\n\tbeql %1, $0, 1f\n\taddiu %0, %0, 100\n\taddiu %0, %0, 1\n1:\n\t.set reorder"
                     : "+r"(k)
                     : "r"(v32[3]));
    say("beql = %u\n", (unsigned)k);
    /* MOVN / MOVZ */
    u32 mz = 1, mn = 2;
    __asm__ volatile("movz %0, %2, %3\n\tmovn %1, %2, %3" : "+r"(mz), "+r"(mn) : "r"(77u), "r"(v32[3]));
    say("movzn = %u %u\n", (unsigned)mz, (unsigned)mn);
}

static void check_mmi(void)
{
    MMI3("paddw");
    MMI3("psubw");
    MMI3("pcgtw");
    MMI3("pmaxw");
    MMI3("pminw");
    MMI3("paddsw");
    MMI3("psubsw");
    MMI3("padduw");
    MMI3("paddh");
    MMI3("paddsh");
    MMI3("pcgtb");
    MMI3("pextlw");
    MMI3("pextuw");
    MMI3("ppacw");
    MMI3("pextlh");
    MMI3("ppach");
    MMI3("pinth");
    MMI3("pcpyld");
    MMI3("pcpyud");
    MMI3("pand");
    MMI3("por");
    MMI3("pxor");
    MMI3("pnor");
    MMI1("pabsw");
    MMI1("pexew");
    MMI1("pexcw");
    MMI1("prot3w");
    MMI1("prevh");
    MMI1("pcpyh");
    MMISH("psllh", 4);
    MMISH("psraw", 8);
    /* MULT / MULT1 then PMFHL.LW: the low words of LO / HI and LO1 / HI1 interleaved */
    __asm__ volatile("lw $8, 0(%0)\n\tlw $9, 4(%0)\n\tmult $8, $9\n\tmult1 $9, $9\n\tpmfhl.lw $10\n\tsq $10, 0(%1)"
                     :
                     : "r"(v32), "r"(q_out)
                     : "$8", "$9", "$10", "hi", "lo", "memory");
    q_show("pmfhl.lw");
    /* PMTLO / PMFLO round trip through the 128-bit LO */
    __asm__ volatile("lq $8, 0(%0)\n\tpmtlo $8\n\tpmflo $10\n\tsq $10, 0(%1)"
                     :
                     : "r"(q_b), "r"(q_out)
                     : "$8", "$10", "lo", "memory");
    q_show("pmflo");
}

static void check_fpu(void)
{
    say("ovf = %08x\n", (unsigned)fput(fget(0) * fget(1)));
    say("inf = %08x\n", (unsigned)fput(fget(2) * fget(3)));
    say("ninf = %08x\n", (unsigned)fput(fget(7) + fget(5)));
    say("den = %08x\n", (unsigned)fput(fget(4) + 0.0f * fget(5)));
    say("div = %08x\n", (unsigned)fput(fget(5) / fget(6)));
    say("dsub = %08x\n", (unsigned)fput(fget(5) - fget(0)));
    say("cvt = %d %d\n", (int)fget(0), (int)(fget(6) * -fget(3)));
    say("lt = %d %d\n", fget(2) > fget(0), fget(7) < fget(0));
    float acc;
    __asm__ volatile("mula.s %1, %2\n\tmadd.s %0, %3, %3" : "=&f"(acc) : "f"(fget(6)), "f"(fget(1)), "f"(fget(3)));
    say("madd.s = %08x\n", (unsigned)fput(acc));
    float mx, mn;
    __asm__ volatile("max.s %0, %2, %3\n\tmin.s %1, %2, %3" : "=&f"(mx), "=&f"(mn) : "f"(fget(7)), "f"(fget(6)));
    say("maxmin = %08x %08x\n", (unsigned)fput(mx), (unsigned)fput(mn));
}

int main(void)
{
    sio_puts("Hello from the Emotion Engine!");
    check_int();
    check_r5900();
    check_mmi();
    check_fpu();
    say("thread %d\n", GetThreadId());
    sio_puts("bye");
    return 0;
}
