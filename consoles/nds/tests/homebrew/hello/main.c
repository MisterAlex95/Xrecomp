/* hello: the DS spike's test program (XBA-223). libnds's console on the main engine (engine A, BG0, 4 bpp text,
 * VRAM bank A), shown on the top screen: three lines of text, the verdict of an ARMv5TE check (instructions the
 * GBA's ARM7TDMI does not have, each result against the value the ARM ARM gives), then a counter for 30 VBlanks and
 * "done". The picture after that never changes, so a headless run compares it with an expected image. No touch, no
 * keys, no ARM7 service beyond what libnds's start-up asks for. */
#include <nds.h>
#include <stdint.h>
#include <stdio.h>

static volatile int frames;

static void on_vblank(void)
{
    frames++;
}

/* Called from ARM code through a register (BLX rN to Thumb). */
__attribute__((noinline)) static unsigned twice(unsigned v)
{
    return v * 2;
}

/* ARMv5TE in ARM state: a failure mask, bit n = check n. GCC emits the calls into and out of it as BLX. */
__attribute__((target("arm"), noinline)) static unsigned v5_ops(unsigned (*volatile fn)(unsigned))
{
    unsigned f = 0, r, a, b, c, psr;
    a = 0x00F00000;
    __asm__ volatile("clz %0, %1" : "=r"(r) : "r"(a));
    f |= (r != 8u) << 0;
    a = 0;
    __asm__ volatile("clz %0, %1" : "=r"(r) : "r"(a));
    f |= (r != 32u) << 1;
    __asm__ volatile("msr cpsr_f, #0" ::: "cc");
    a = 0x7FFFFFF0;
    b = 0x100;
    __asm__ volatile("qadd %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    f |= (r != 0x7FFFFFFFu) << 2;
    __asm__ volatile("mrs %0, cpsr" : "=r"(psr));
    f |= !((psr >> 27) & 1) << 3;
    a = 0x80000010;
    __asm__ volatile("qsub %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    f |= (r != 0x80000000u) << 4;
    a = 1;
    b = 0x40000000;
    __asm__ volatile("qdadd %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    f |= (r != 0x7FFFFFFFu) << 5;
    a = 0;
    b = 0xC0000000;
    __asm__ volatile("qdsub %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    f |= (r != 0x7FFFFFFFu) << 6;
    a = 0x00030002;
    b = 0xFFFF0004;
    __asm__ volatile("smulbb %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    f |= (r != 8u) << 7;
    __asm__ volatile("smultt %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    f |= (r != 0xFFFFFFFDu) << 8;
    __asm__ volatile("smulbt %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    f |= (r != 0xFFFFFFFEu) << 9;
    c = 100;
    __asm__ volatile("smlabb %0, %1, %2, %3" : "=r"(r) : "r"(a), "r"(b), "r"(c));
    f |= (r != 108u) << 10;
    a = 0x00010000;
    b = 3;
    __asm__ volatile("smulwb %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    f |= (r != 3u) << 11;
    a = 0x00020000;
    b = 0x00050000;
    c = 7;
    __asm__ volatile("smlawt %0, %1, %2, %3" : "=r"(r) : "r"(a), "r"(b), "r"(c));
    f |= (r != 17u) << 12;
    unsigned lo = 1, hi = 0;
    a = 0x0000FFFE;
    b = 3;
    __asm__ volatile("smlalbb %0, %1, %2, %3" : "+r"(lo), "+r"(hi) : "r"(a), "r"(b));
    f |= (lo != 0xFFFFFFFBu || hi != 0xFFFFFFFFu) << 13;
    static uint32_t buf[2] __attribute__((aligned(8)));
    uint64_t w = 0x5566778811223344ull, v;
    __asm__ volatile("strd %1, %H1, [%0]" : : "r"(buf), "r"(w) : "memory");
    f |= (buf[0] != 0x11223344u || buf[1] != 0x55667788u) << 14;
    __asm__ volatile("ldrd %0, %H0, [%1]" : "=r"(v) : "r"(buf) : "memory");
    f |= (v != w) << 15;
    f |= (fn(21) != 42u) << 16;
    return f;
}

int main(void)
{
    irqSet(IRQ_VBLANK, on_vblank);
    videoSetMode(MODE_0_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    consoleInit(NULL, 0, BgType_Text4bpp, BgSize_T_256x256, 31, 0, true, true);
    iprintf("Hello DS from XRecomp\n");
    iprintf("ARM9 recompiled to C\n");
    iprintf("\x1b[32mgreen text\x1b[39m\n");
    unsigned f = v5_ops(twice);
    if (f)
        iprintf("\x1b[31mARMv5TE: FAIL %05X\x1b[39m\n", f);
    else
        iprintf("ARMv5TE: ok\n");
    for (int i = 1; i <= 30; i++) {
        swiWaitForVBlank();
        iprintf("\x1b[10;0HFrame %d", i);
    }
    iprintf("\x1b[12;0Hdone (%s)", frames >= 30 ? "vblank irq ok" : "vblank irq missing");
    for (;;)
        swiWaitForVBlank();
    return 0;
}
