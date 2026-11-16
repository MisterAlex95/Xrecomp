/* hello: the 3DS spike's test program (XBA-228). libctru's console on the top screen: three lines of text, the
 * verdict of an ARMv6K + VFPv2 check (instructions the DS's ARM946E-S does not have, each result against the value
 * the ARM ARM gives), then a counter for 30 frames of the usual libctru main loop (aptMainLoop, hidScanInput,
 * gfxFlushBuffers, gfxSwapBuffers, gspWaitForVBlank) and "done". The picture after that never changes, so a headless
 * run compares it with an expected image. The loop also ends on START, as any libctru program does. */
#include <3ds.h>
#include <stdint.h>
#include <stdio.h>

/* ARMv6K and VFPv2 in ARM state: a failure mask, bit n = check n. */
__attribute__((noinline)) static unsigned v6_ops(volatile float *fin, volatile double *din)
{
    unsigned f = 0, r, a, b;
    a = 0x11223344;
    __asm__ volatile("rev %0, %1" : "=r"(r) : "r"(a));
    f |= (r != 0x44332211u) << 0;
    __asm__ volatile("rev16 %0, %1" : "=r"(r) : "r"(a));
    f |= (r != 0x22114433u) << 1;
    a = 0x000080FF;
    __asm__ volatile("revsh %0, %1" : "=r"(r) : "r"(a));
    f |= (r != 0xFFFFFF80u) << 2;
    a = 0x12345680;
    __asm__ volatile("sxtb %0, %1" : "=r"(r) : "r"(a));
    f |= (r != 0xFFFFFF80u) << 3;
    __asm__ volatile("uxtb %0, %1, ror #8" : "=r"(r) : "r"(a));
    f |= (r != 0x56u) << 4;
    a = 0x0000F00D;
    __asm__ volatile("sxth %0, %1" : "=r"(r) : "r"(a));
    f |= (r != 0xFFFFF00Du) << 5;
    b = 0x10;
    __asm__ volatile("uxtah %0, %1, %2" : "=r"(r) : "r"(b), "r"(a));
    f |= (r != 0xF01Du) << 6;
    /* LDREX / STREX: the store succeeds after a load-exclusive; CLREX makes the next one fail. */
    static volatile uint32_t word = 5;
    unsigned v, st;
    __asm__ volatile("ldrex %0, [%2]\n\tadd %0, %0, #1\n\tstrex %1, %0, [%2]" : "=&r"(v), "=&r"(st) : "r"(&word)
                     : "memory");
    f |= (st != 0 || word != 6u) << 7;
    __asm__ volatile("ldrex %0, [%2]\n\tclrex\n\tstrex %1, %0, [%2]" : "=&r"(v), "=&r"(st) : "r"(&word) : "memory");
    f |= (st != 1) << 8;
    /* SEL with the GE flags from UADD8, USAT / SSAT, PKHBT */
    a = 0x80FF0102;
    b = 0x80020301;
    __asm__ volatile("uadd8 %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    f |= (r != 0x00010403u) << 9;
    unsigned x = 0xAAAAAAAA, y = 0x55555555;
    __asm__ volatile("sel %0, %1, %2" : "=r"(r) : "r"(x), "r"(y));
    f |= (r != 0xAAAA5555u) << 10; /* GE = 1100: bytes 3, 2 from x, 1, 0 from y */
    a = (unsigned)-5;
    __asm__ volatile("usat %0, #8, %1" : "=r"(r) : "r"(a));
    f |= (r != 0u) << 11;
    a = 300;
    __asm__ volatile("ssat %0, #8, %1" : "=r"(r) : "r"(a));
    f |= (r != 127u) << 12;
    a = 0x00001234;
    b = 0x00005678;
    __asm__ volatile("pkhbt %0, %1, %2, lsl #16" : "=r"(r) : "r"(a), "r"(b));
    f |= (r != 0x56781234u) << 13;
    /* VFPv2: single and double arithmetic, conversions, compare */
    float s = fin[0] * fin[1] + fin[2]; /* 1.5 * 4 + 0.25 */
    f |= (s != 6.25f) << 14;
    double d = din[0] / din[1]; /* 10 / 4 */
    f |= (d != 2.5) << 15;
    f |= ((int)(d * 3.0) != 7) << 16;
    f |= (__builtin_sqrt(din[2]) != 3.0) << 17; /* 9 */
    f |= ((float)din[0] != 10.0f) << 18;
    f |= !(fin[3] < 0.0f) << 19; /* -2 */
    unsigned u = (unsigned)fin[1];
    f |= (u != 4u) << 20;
    return f;
}

int main(void)
{
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    printf("Hello 3DS from XRecomp\n");
    printf("ARM11 recompiled to C\n");
    printf("\x1b[32mgreen text\x1b[0m\n");
    static volatile float fin[4] = {1.5f, 4.0f, 0.25f, -2.0f};
    static volatile double din[3] = {10.0, 4.0, 9.0};
    unsigned f = v6_ops(fin, din);
    if (f)
        printf("\x1b[31mARMv6K: FAIL %06X\x1b[0m\n", f);
    else
        printf("ARMv6K + VFPv2: ok\n");
    int frame = 0;
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START)
            break;
        if (frame < 30) {
            frame++;
            printf("\x1b[10;0HFrame %d", frame);
            if (frame == 30)
                printf("\x1b[12;0Hdone (keys %08lX)", hidKeysHeld());
        }
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    gfxExit();
    return 0;
}
