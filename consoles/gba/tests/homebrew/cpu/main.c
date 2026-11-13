/* GBA cpu test (XBA-149): ARM + Thumb instruction sequences with interworking, BIOS math SWIs and
 * libgcc helpers. Result protocol: README.md. Our own code. */
#include <tonc.h>

#include "../common/xba_result.h"

#define ARM_COUNT 45
#define THUMB_COUNT 25

void arm_tests(u32 *out);   /* ops.s, ARM state */
void thumb_tests(u32 *out); /* ops.s, Thumb state */

static const u32 arm_expected[ARM_COUNT] = {
    0x00000002, 0x40000000, 0xC0000000, 0x18000000, 0xC0000000, /* 0-4 shifts, RRX */
    0x00000000, 0xFFFFFFFF, 0x18000000,                         /* 5-7 register shifts */
    0x00000000, 0x60000000, 0x80000000, 0x90000000,             /* 8-11 ADDS flags */
    0xFFFFFFFF, 0x80000000,                                     /* 12-13 SUBS flags */
    0x00000031, 0x0000000F, 0x0000000E, 0x000000EF,             /* 14-17 ADC SBC RSC */
    0x242D2080,                                                 /* 18 MUL */
    0x242D2080, 0x0B00EA4E,                                     /* 19-20 UMULL */
    0x242D2080, 0xF8CC93D6,                                     /* 21-22 SMULL */
    0x366176F8,                                                 /* 23 MLA */
    0x485A4100, 0x1601D49C,                                     /* 24-25 UMLAL */
    0x485A4100, 0xF19927AC,                                     /* 26-27 SMLAL */
    0x000000AA, 0xFFFFFFAA, 0x00008899, 0xFFFF8899,             /* 28-31 LDRB/SB/H/SH */
    0xBB8899AA, 0xCCDDEEFF, 0x00000004,                         /* 32-34 unaligned, writeback */
    0x00000010, 0x00000403, 0x00000008,                         /* 35-37 STM/LDM */
    0x00005501,                                                 /* 38 SWP */
    0x00000101, 0x00000077,                                     /* 39-40 conditions */
    0xF0000000,                                                 /* 41 MSR/MRS */
    0x00000043,                                                 /* 42 ARM->Thumb->ARM */
    0x60000000,                                                 /* 43 shifter carry-out */
    0x000013BA,                                                 /* 44 loop */
};

static const u32 thumb_expected[THUMB_COUNT] = {
    0x00000002, 0x40000000, 0xC0000000, 0x18000000, /* 0-3 shifts */
    0x0000000B, 0x00000010, 0xFFFFFFFB,             /* 4-6 ADC SBC NEG */
    0x242D2080,                                     /* 7 MUL */
    0xFFFFFFAA, 0xFFFF8899, 0x00008899, 0x000000CC, /* 8-11 loads */
    0x00000201, 0x00000010,                         /* 12-13 PUSH/POP, STM/LDM */
    0x00000037, 0x00000002, 0x00000003,             /* 14-16 branches */
    0x00000002,                                     /* 17 high registers */
    0xF000F000, 0x0FF00FF0, 0x00F000F0, 0x0F0F0F0F, /* 18-21 logic */
    0x00000045,                                     /* 22 Thumb->ARM->Thumb */
    0x00000000, 0xFFFFFFFF,                         /* 23-24 register shifts >= 32 */
};

/* Kept out of the optimiser's reach so libgcc and the BIOS really run. */
static volatile s32 v_num = -100, v_den = 7;
static volatile u32 v_sqrt = 1000000;
static volatile u64 v_big = 0x0123456789ABCDEFull;
static volatile u32 v_small = 12345;

int main(void)
{
    u32 out[ARM_COUNT > THUMB_COUNT ? ARM_COUNT : THUMB_COUNT];
    u32 base = 0;

    xba_begin(XBA_ID_CPU);

    arm_tests(out);
    for (int i = 0; i < ARM_COUNT; i++)
        xba_check(base + i, out[i], arm_expected[i]);
    base += ARM_COUNT;

    thumb_tests(out);
    for (int i = 0; i < THUMB_COUNT; i++)
        xba_check(base + i, out[i], thumb_expected[i]);
    base += THUMB_COUNT;

    /* BIOS SWIs (HLE in the runtime): Div (0x06) and Sqrt (0x08). */
    xba_check(base + 0, (u32)Div(v_num, v_den), (u32)-14);
    xba_check(base + 1, (u32)DivMod(v_num, v_den), (u32)-2);
    xba_check(base + 2, Sqrt(v_sqrt), 1000);
    /* libgcc: 32-bit and 64-bit division helpers compiled as Thumb. */
    xba_check(base + 3, (u32)(v_num / v_den), (u32)-14);
    xba_check(base + 4, (u32)(v_big / v_small), 0x45F5207Du);
    xba_check(base + 5, (u32)((v_big / v_small) >> 32), 0x0000060Au);

    XBA_RESULT->checksum = xba_fnv1a(XBA_RESULT->table, XBA_RESULT->count);

    /* Show the verdict: green backdrop on pass, red on fail. */
    REG_DISPCNT = DCNT_MODE0;
    pal_bg_mem[0] = XBA_RESULT->detail ? RGB15(31, 0, 0) : RGB15(0, 31, 0);
    xba_finish();
}
