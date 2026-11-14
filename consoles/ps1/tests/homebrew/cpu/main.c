/* PS1 cpu test (XBA-149): MIPS R3000A sequences (delay slots, mult/div incl. divide-by-zero results,
 * LWL/LWR/SWL/SWR, sign extension, shifts, set-on-less-than) plus libgcc helpers.
 * Result protocol: README.md. Our own code. */
#include <stdint.h>
#include <string.h>

#include "../common/xba_result.h"
#include "../common/screen.h"

#define MIPS_COUNT 51

void mips_tests(uint32_t *out); /* ops.s */

static const uint32_t expected[MIPS_COUNT] = {
    0x00000001, 0x00000022, 0x00000105,             /* 0-2 branch / JAL delay slots */
    0x00000055, 0x11223344,                         /* 3-4 load delay slot */
    0x242D2080, 0xF8CC93D6, 0x242D2080, 0x0B00EA4E, /* 5-8 MULT, MULTU */
    0xFFFFFFF2, 0xFFFFFFFE, 0x24924922, 0x00000002, /* 9-12 DIV, DIVU */
    0xFFFFFFFF, 0x00000005, 0x00000001, 0xFFFFFFFB, /* 13-16 DIV by zero */
    0xFFFFFFFF, 0x00000005,                         /* 17-18 DIVU by zero */
    0x80000000, 0x00000000,                         /* 19-20 DIV overflow */
    0x44332211, 0xAA332211, 0x1100CCDD,             /* 21-23 LWL/LWR */
    0xCCBBAA00, 0x000000DD,                         /* 24-25 SWL/SWR */
    0xFFFFFFAA, 0x000000AA, 0xFFFF8899, 0x00008899, /* 26-29 LB LBU LH LHU */
    0x00000001, 0x00000000, 0x00000001, 0x00000000, /* 30-33 SLT SLTU SLTIU SLTI */
    0xF8000000, 0x08000000, 0x80000000, 0x00000002, 0xF8000000, /* 34-38 shifts */
    0x000F000F, 0x0FF00FF0, 0x0000F0F0, 0x00008000, 0x80000000, /* 39-43 logic */
    0x80000000, 0x80000001,                         /* 44-45 wrapping */
    0x00000000,                                     /* 46 BLTZAL link */
    0x00000205,                                     /* 47 JALR */
    0x000013BA,                                     /* 48 loop */
    0x0000ABCD, 0x00001234,                         /* 49-50 MTHI/MTLO */
};

static volatile uint64_t v_big = 0x0123456789ABCDEFull;
static volatile uint32_t v_small = 12345;
static volatile int32_t v_num = -100, v_den = 7;

int main(void)
{
    uint32_t out[MIPS_COUNT];

    xba_begin(XBA_ID_CPU);
    mips_tests(out);
    for (uint32_t i = 0; i < MIPS_COUNT; i++)
        xba_check(i, out[i], expected[i]);

    uint32_t b = MIPS_COUNT;
    xba_check(b + 0, (uint32_t)(v_num / v_den), (uint32_t)-14);
    xba_check(b + 1, (uint32_t)(v_num % v_den), (uint32_t)-2);
    xba_check(b + 2, (uint32_t)(v_big / v_small), 0x45F5207Du);
    xba_check(b + 3, (uint32_t)((v_big / v_small) >> 32), 0x0000060Au);
    xba_check(b + 4, (uint32_t)strlen("homebrew"), 8);

    XBA_RESULT->checksum = xba_fnv1a(XBA_RESULT->table, XBA_RESULT->count);
    screen_verdict(XBA_RESULT->detail == 0);
    xba_finish();
}
