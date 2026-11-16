/* hello: the PSP spike's homebrew (XBA-224). PSPSDK's debug screen on the 480x272 framebuffer, an Allegrex check
 * (instructions the R3000 / R4300 lack: SPECIAL3 EXT / INS / SEB / SEH / WSBH / WSBW / BITREV, CLZ / CLO, MAX / MIN,
 * MOVZ / MOVN, MADD / MSUBU, ROTR / ROTRV, likely branches, the single-precision FPU), the pad read once, then 30
 * VBlanks and sceKernelExitGame. Fixed text: the expected picture is consoles/psp/tests/hello.png. */
#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspkernel.h>

PSP_MODULE_INFO("XBA hello", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);

static volatile unsigned in_a = 0x12345678u, in_b = 0x0000F00Fu;
static volatile float in_f = 2.25f, in_g = -0.5f;

/* Bit n set = check n failed. */
static unsigned allegrex_check(void)
{
    unsigned fail = 0, a = in_a, b = in_b, r;
    int s;

    __asm__ volatile("ext %0, %1, 4, 8" : "=r"(r) : "r"(a));
    if (r != 0x67u)
        fail |= 1u << 0;
    r = a;
    __asm__ volatile("ins %0, %1, 8, 12" : "+r"(r) : "r"(b));
    if (r != 0x12300F78u)
        fail |= 1u << 1;
    __asm__ volatile("seb %0, %1" : "=r"(s) : "r"(0x80u + b));
    if (s != (signed char)0x8Fu)
        fail |= 1u << 2;
    __asm__ volatile("seh %0, %1" : "=r"(s) : "r"(b));
    if (s != (short)0xF00F)
        fail |= 1u << 3;
    __asm__ volatile("wsbh %0, %1" : "=r"(r) : "r"(a));
    if (r != 0x34127856u)
        fail |= 1u << 4;
    __asm__ volatile("wsbw %0, %1" : "=r"(r) : "r"(a));
    if (r != 0x78563412u)
        fail |= 1u << 5;
    __asm__ volatile("bitrev %0, %1" : "=r"(r) : "r"(b));
    if (r != 0xF00F0000u)
        fail |= 1u << 6;
    __asm__ volatile("clz %0, %1" : "=r"(r) : "r"(b));
    if (r != 16)
        fail |= 1u << 7;
    __asm__ volatile("clo %0, %1" : "=r"(r) : "r"(~b));
    if (r != 16)
        fail |= 1u << 8;
    __asm__ volatile("max %0, %1, %2" : "=r"(s) : "r"(-5), "r"((int)b));
    if (s != (int)b)
        fail |= 1u << 9;
    __asm__ volatile("min %0, %1, %2" : "=r"(s) : "r"(-5), "r"((int)b));
    if (s != -5)
        fail |= 1u << 10;
    r = 7;
    __asm__ volatile("movz %0, %1, %2" : "+r"(r) : "r"(a), "r"(0));
    __asm__ volatile("movn %0, %1, %2" : "+r"(r) : "r"(b), "r"(0));
    if (r != a)
        fail |= 1u << 11;
    {
        unsigned hi, lo;
        __asm__ volatile("mtlo %2\n\tmthi %3\n\tmadd %4, %5\n\tmsubu %6, %7\n\tmfhi %0\n\tmflo %1"
                         : "=&r"(hi), "=&r"(lo)
                         : "r"(100u), "r"(0u), "r"(-3), "r"(1000), "r"(2u), "r"(25u)
                         : "hi", "lo");
        if (hi != 0xFFFFFFFFu || lo != (unsigned)(100 - 3000 - 50))
            fail |= 1u << 12;
    }
    __asm__ volatile("rotr %0, %1, 8" : "=r"(r) : "r"(a));
    if (r != 0x78123456u)
        fail |= 1u << 13;
    __asm__ volatile("rotrv %0, %1, %2" : "=r"(r) : "r"(a), "r"(36));
    if (r != 0x81234567u)
        fail |= 1u << 14;
    r = 0;
    __asm__ volatile(".set push\n\t.set noreorder\n\t"
                     "beql %1, $zero, 1f\n\taddiu %0, %0, 1\n\t"
                     "bnel %1, $zero, 1f\n\taddiu %0, %0, 2\n\t"
                     "addiu %0, %0, 4\n"
                     "1:\n\t.set pop"
                     : "+r"(r)
                     : "r"(a));
    if (r != 2)
        fail |= 1u << 15;
    {
        float f = in_f, g = in_g;
        float m = f * g + f / 0.25f;
        int t = (int)(f * 3.0f);
        if (m != 7.875f || t != 6 || !(g < f) || __builtin_sqrtf(f) != 1.5f)
            fail |= 1u << 16;
    }
    return fail;
}

int main(void)
{
    SceCtrlData pad;

    pspDebugScreenInit();
    pspDebugScreenSetXY(0, 1);
    pspDebugScreenPrintf("  Hello from XRecomp!\n\n");
    pspDebugScreenPrintf("  PSPSDK homebrew, Allegrex recompiled\n\n");
    unsigned fail = allegrex_check();
    if (fail)
        pspDebugScreenPrintf("  Allegrex: FAIL %05X\n", fail);
    else
        pspDebugScreenPrintf("  Allegrex: ok\n");
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    sceCtrlReadBufferPositive(&pad, 1);
    pspDebugScreenPrintf("  pad: %08X\n", (unsigned)pad.Buttons);
    pspDebugScreenSetTextColor(0xFF00FFFFu);
    pspDebugScreenPrintf("\n  480x272, 32-bit framebuffer\n");
    for (int i = 0; i < 30; i++)
        sceDisplayWaitVblankStart();
    pspDebugScreenSetTextColor(0xFF00FF00u);
    pspDebugScreenPrintf("  30 VBlanks: done\n");
    sceKernelExitGame();
    return 0;
}
