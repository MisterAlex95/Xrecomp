/* hello: the GameCube spike's test program (XBA-225). libogc's console over the XFB, then a check of the PowerPC
 * integer / FPU instructions and of the Gekko's paired singles and quantised loads / stores, each against the value
 * the Gekko manual gives. The screen it leaves is consoles/gc/tests/hello_xfb.png:
 *
 *   Hello World!
 *   XRecomp GameCube spike (XBA-225)
 *   PowerPC: ok
 *   Gekko: ok
 *
 * On a mismatch a line says FAIL and the mask of the failed checks (bit n = check n below). Nothing on the screen
 * depends on time, so the picture is the same on every run. */
#include <gccore.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void *xfb;
static GXRModeObj *rmode;

static inline void set_gqr2(uint32_t v) { __asm__ volatile("mtspr 914, %0" : : "r"(v)); }
static inline void set_gqr3(uint32_t v) { __asm__ volatile("mtspr 915, %0" : : "r"(v)); }

/* Integer and FPU: ordinary PowerPC (the frontend shared with the Xbox 360 lifts these). */
static unsigned ppc_checks(void)
{
    unsigned fail = 0;
    volatile uint32_t a = 0x12345678, b = 0xF0000001, z = 0;
    uint32_t r, ca;

    __asm__("rlwinm %0, %1, 8, 16, 23" : "=r"(r) : "r"(a));
    if (r != 0x00007800) /* rotl(0x12345678, 8) = 0x34567812, mask bits 16..23 */
        fail |= 1u << 0;
    r = 0xFFFFFFFF;
    __asm__("rlwimi %0, %1, 4, 0, 3" : "+r"(r) : "r"(a));
    if (r != 0x2FFFFFFF) /* rotl(a, 4) = 0x23456781, top nibble inserted */
        fail |= 1u << 1;
    __asm__("mulhw %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    if (r != (uint32_t)(((int64_t)(int32_t)a * (int32_t)b) >> 32))
        fail |= 1u << 2;
    __asm__("mulhwu %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    if (r != (uint32_t)(((uint64_t)a * b) >> 32))
        fail |= 1u << 3;
    __asm__("divw %0, %1, %2" : "=r"(r) : "r"(b), "r"(a + 0u * z));
    if ((int32_t)r != (int32_t)b / (int32_t)a)
        fail |= 1u << 4;
    __asm__("cntlzw %0, %1" : "=r"(r) : "r"(a >> 5));
    if (r != 8)
        fail |= 1u << 5;
    /* 64-bit add from addc / adde: 0xFFFFFFFF_F0000001 + 0x00000000_12345678 */
    {
        uint32_t lo, hi;
        __asm__("addc %0, %2, %3\n\tadde %1, %4, %5" : "=&r"(lo), "=&r"(hi)
                : "r"(b), "r"(a), "r"(0xFFFFFFFFu), "r"(z));
        if (lo != 0x02345679 || hi != 0)
            fail |= 1u << 6;
    }
    /* srawi sets CA when a negative value loses one bits */
    __asm__("srawi %0, %2, 4\n\tli %1, 0\n\taddze %1, %1" : "=r"(r), "=&r"(ca) : "r"(b));
    if (r != 0xFF000000 || ca != 1)
        fail |= 1u << 7;
    /* compare, CR logic, mfcr */
    {
        uint32_t cr;
        __asm__("cmpw cr6, %1, %2\n\tcmplw cr7, %1, %2\n\tcrxor 2, 25, 29\n\tmfcr %0"
                : "=r"(cr) : "r"(b), "r"(a) : "cr0", "cr6", "cr7");
        /* cr6: b < a signed -> LT; cr7: b > a unsigned -> GT; CR bit 2 (cr0 EQ) = bit 25 ^ bit 29 = 0 ^ 1 */
        if ((cr & 0xFF) != 0x84 || ((cr >> 29) & 1) != 1)
            fail |= 1u << 8;
    }
    /* FPU: fmadd, frsp, fctiwz, fsel, fabs / fneg, fdivs */
    {
        volatile double x = 1.5, y = -2.25, w = 10.0;
        double d;
        __asm__("fmadd %0, %1, %2, %3" : "=f"(d) : "f"(x), "f"(y), "f"(w));
        if (d != 6.625)
            fail |= 1u << 9;
        union { double d; int64_t i; } u;
        __asm__("fctiwz %0, %1" : "=f"(u.d) : "f"(-7.9));
        if ((int32_t)u.i != -7)
            fail |= 1u << 10;
        __asm__("fsel %0, %1, %2, %3" : "=f"(d) : "f"(y), "f"(x), "f"(w));
        if (d != 10.0)
            fail |= 1u << 11;
        float q;
        volatile float fa = 1.0f, fb = 3.0f;
        __asm__("fdivs %0, %1, %2" : "=f"(q) : "f"(fa), "f"(fb));
        if (q != 1.0f / 3.0f)
            fail |= 1u << 12;
        __asm__("fnabs %0, %1" : "=f"(d) : "f"(x));
        if (d != -1.5)
            fail |= 1u << 13;
    }
    return fail;
}

/* Gekko paired singles and quantised loads / stores (Gekko only: the GameCube layer of the lifter). */
static unsigned gekko_checks(void)
{
    unsigned fail = 0;
    static float in[4] __attribute__((aligned(8))) = {1.5f, -2.0f, 4.0f, 0.25f};
    static float out[2] __attribute__((aligned(8)));
    static uint8_t bytes[2] = {10, 200};
    static int16_t shorts[2];
    static float one[1] = {3.0f};

    set_gqr2(0); /* f32 both ways */
    /* ps_add, ps_mul, ps_madd on (1.5, -2) and (4, 0.25) */
    __asm__ volatile(
        "psq_l 1, 0(%0), 0, 2\n\t"
        "psq_l 2, 8(%0), 0, 2\n\t"
        "ps_add 3, 1, 2\n\t"     /* (5.5, -1.75) */
        "ps_mul 4, 3, 1\n\t"     /* (8.25, 3.5) */
        "ps_madd 5, 4, 2, 1\n\t" /* (8.25*4+1.5, 3.5*0.25-2) = (34.5, -1.125) */
        "psq_st 5, 0(%1), 0, 2\n\t"
        : : "b"(in), "b"(out) : "fr1", "fr2", "fr3", "fr4", "fr5", "memory");
    if (out[0] != 34.5f || out[1] != -1.125f)
        fail |= 1u << 0;
    /* ps_merge10, ps_sum0, ps_muls1, ps_neg */
    __asm__ volatile(
        "psq_l 1, 0(%0), 0, 2\n\t"
        "psq_l 2, 8(%0), 0, 2\n\t"
        "ps_merge10 3, 1, 2\n\t"   /* (-2, 4) */
        "ps_sum0 4, 3, 2, 1\n\t"   /* (ps0 f3 + ps1 f1, ps1 f2) = (-2 + -2, 0.25) */
        "ps_muls1 5, 4, 2\n\t"     /* f4 * ps1 f2 = (-4 * 0.25, 0.25 * 0.25) */
        "ps_neg 5, 5\n\t"          /* (1, -0.0625) */
        "psq_st 5, 0(%1), 0, 2\n\t"
        : : "b"(in), "b"(out) : "fr1", "fr2", "fr3", "fr4", "fr5", "memory");
    if (out[0] != 1.0f || out[1] != -0.0625f)
        fail |= 1u << 1;
    /* quantised load: u8, scale 2 (values / 4); quantised store: s16, scale 4 (values * 16), clamped */
    set_gqr3((2u << 24) | (4u << 16) | (4u << 8) | 7u);
    __asm__ volatile(
        "psq_l 6, 0(%0), 0, 3\n\t"   /* (2.5, 50) */
        "psq_st 6, 0(%2), 0, 3\n\t"  /* (40, 800) as s16 */
        "psq_st 6, 0(%1), 0, 2\n\t"
        : : "b"(bytes), "b"(out), "b"(shorts) : "fr6", "memory");
    if (out[0] != 2.5f || out[1] != 50.0f)
        fail |= 1u << 2;
    if (shorts[0] != 40 || shorts[1] != 800)
        fail |= 1u << 3;
    /* W = 1: one value, ps1 = 1.0 */
    __asm__ volatile(
        "psq_l 7, 0(%0), 1, 2\n\t"
        "psq_st 7, 0(%1), 0, 2\n\t"
        : : "b"(one), "b"(out) : "fr7", "memory");
    if (out[0] != 3.0f || out[1] != 1.0f)
        fail |= 1u << 4;
    /* clamp on store: 50 * 16 fits, 4000 * 16 does not (s16 max 32767) */
    {
        static float big[2] __attribute__((aligned(8))) = {-4000.0f, 4000.0f};
        __asm__ volatile(
            "psq_l 8, 0(%0), 0, 2\n\t"
            "psq_st 8, 0(%1), 0, 3\n\t"
            : : "b"(big), "b"(shorts) : "fr8", "memory");
        if (shorts[0] != -32768 || shorts[1] != 32767)
            fail |= 1u << 5;
    }
    /* ps_cmpo0 and ps_sel */
    {
        uint32_t cr;
        __asm__ volatile(
            "psq_l 1, 0(%1), 0, 2\n\t"
            "psq_l 2, 8(%1), 0, 2\n\t"
            "ps_cmpo0 cr1, 1, 2\n\t" /* 1.5 < 4 -> LT */
            "ps_sel 3, 2, 1, 2\n\t"  /* 4 >= 0 -> 1.5 ; 0.25 >= 0 -> -2 */
            "psq_st 3, 0(%2), 0, 2\n\t"
            "mfcr %0\n\t"
            : "=r"(cr) : "b"(in), "b"(out) : "fr1", "fr2", "fr3", "cr1", "memory");
        if (((cr >> 24) & 15) != 8 || out[0] != 1.5f || out[1] != -2.0f)
            fail |= 1u << 6;
    }
    return fail;
}

int main(void)
{
    VIDEO_Init();
    rmode = VIDEO_GetPreferredMode(NULL);
    xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    console_init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight, rmode->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE)
        VIDEO_WaitVSync();

    printf("\x1b[2;0H");
    printf("Hello World!\n");
    printf("XRecomp GameCube spike (XBA-225)\n");
    unsigned p = ppc_checks();
    if (p)
        printf("PowerPC: FAIL %04X\n", p);
    else
        printf("PowerPC: ok\n");
    unsigned g = gekko_checks();
    if (g)
        printf("Gekko: FAIL %04X\n", g);
    else
        printf("Gekko: ok\n");

    for (;;)
        VIDEO_WaitVSync();
    return 0;
}
