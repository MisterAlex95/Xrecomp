/* N64 cpu test (XBA-149): VR4300 64-bit integer ops, branch-likely, big-endian unaligned
 * loads/stores and the FPU (rounding modes, conversions, FCSR flags), plus libgcc 64-bit helpers.
 * Each 64-bit result takes two table words: [2i] = high half, [2i+1] = low half.
 * Result protocol: README.md. Our own code. */
#include <libdragon.h>
#include <stdint.h>

#include "../common/xba_result.h"

#define MIPS64_COUNT 56

void mips64_tests(uint64_t *out); /* ops.S */

static const uint64_t expected[MIPS64_COUNT] = {
    0xFFFFFFFF80000000ull, 0xFFFFFFFF80000000ull, 0xFFFFFFFF80000000ull, /* 0-2 sign extension */
    0x8000000000000000ull, 0x8000000000000001ull, 0x00000001FFFFFFFEull, /* 3-5 add/sub */
    0x2345678000000000ull, 0x0000012345678000ull, 0xFFFFFFFFF8000000ull, /* 6-8 shifts */
    0x0000000008000000ull, 0xFF80000000000000ull, 0xF800000000000000ull, /* 9-11 */
    0x0000000123456780ull,                                               /* 12 */
    0xFFFFFFFFFFFFFFFEull, 0x0000000000000001ull,                        /* 13-14 DMULTU */
    0xFFFFFFFFFFFFFFF1ull, 0xFFFFFFFFFFFFFFFFull,                        /* 15-16 DMULT */
    0xFFFFFFFFFFFFFFF2ull, 0xFFFFFFFFFFFFFFFEull,                        /* 17-18 DDIV */
    0x1999999999999999ull, 0x0000000000000005ull,                        /* 19-20 DDIVU */
    0x00000000242D2080ull, 0xFFFFFFFFF8CC93D6ull,                        /* 21-22 MULT */
    0x0011223344556677ull, 0xFFFFFFFF8899AABBull, 0x000000008899AABBull, /* 23-25 LD LW LWU */
    0x1122334455667788ull, 0x0000000011223344ull,                        /* 26-27 LDL/LDR LWL/LWR */
    0xFFFFFFFFFFFFFF99ull, 0x000000000000AABBull,                        /* 28-29 LB LHU */
    0x0000000102030405ull, 0x0607080000000000ull,                        /* 30-31 SDL/SDR */
    0x0000000000000001ull, 0x0000000000000001ull,                        /* 32-33 SLTU SLT */
    0x0000000000000010ull, 0x0000000000000001ull, 0x0000000000000020ull, /* 34-36 BEQL BNEL */
    0x400E000000000000ull, 0xBFF8000000000000ull,                        /* 37-38 add.d mul.d */
    0x3FD5555555555555ull, 0x0000000000001004ull,                        /* 39-40 div.d, FCSR */
    0x3FF6A09E667F3BCDull,                                               /* 41 sqrt.d */
    0x0000000000000002ull, 0x0000000000000003ull, 0xFFFFFFFFFFFFFFFDull, /* 42-44 cvt.w.d RM */
    0xFFFFFFFFFFFFFFFEull, 0x0000000000000004ull,                        /* 45-46 trunc round */
    0xFFFFFFFFFFFFFFFFull, 0x0000000000000002ull,                        /* 47-48 floor ceil */
    0xC23CBE991A140000ull, 0x0000000000000002ull,                        /* 49-50 cvt.d.l trunc.l.d */
    0x0000000040C00000ull, 0x4018000000000000ull,                        /* 51-52 mul.s cvt.d.s */
    0x0000000000000000ull, 0x0000000000000001ull,                        /* 53-54 c.lt.d bc1t bc1fl */
    0x000013BA00000000ull,                                               /* 55 loop */
};

static volatile uint64_t v_big = 0x0123456789ABCDEFull;
static volatile uint64_t v_small = 12345;
static volatile int64_t v_neg = -1000000000000ll;

int main(void)
{
    static uint64_t out[MIPS64_COUNT];

    xba_begin(XBA_ID_CPU);
    disable_interrupts(); /* nothing may touch the FPU or HI/LO while the sequences run */
    mips64_tests(out);
    enable_interrupts();

    for (uint32_t i = 0; i < MIPS64_COUNT; i++) {
        xba_check(2 * i, (uint32_t)(out[i] >> 32), (uint32_t)(expected[i] >> 32));
        xba_check(2 * i + 1, (uint32_t)out[i], (uint32_t)expected[i]);
    }

    uint32_t b = 2 * MIPS64_COUNT;
    uint64_t q = v_big / v_small; /* inline DDIVU */
    xba_check(b + 0, (uint32_t)q, 0x45F5207Du);
    xba_check(b + 1, (uint32_t)(q >> 32), 0x0000060Au);
    xba_check(b + 2, (uint32_t)(v_neg % 7), (uint32_t)-1); /* -1e12 = -142857142857 * 7 - 1 */
    xba_check(b + 3, (uint32_t)(int32_t)(v_neg >> 40), (uint32_t)-1);

    XBA_RESULT->checksum = xba_fnv1a(XBA_RESULT->table, XBA_RESULT->count);

    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_RESAMPLE);
    surface_t *d = display_get();
    graphics_fill_screen(d, XBA_RESULT->detail ? graphics_make_color(160, 0, 0, 255)
                                               : graphics_make_color(0, 160, 0, 255));
    graphics_draw_text(d, 16, 16, XBA_RESULT->detail ? "XBA CPU N64: FAIL" : "XBA CPU N64: PASS");
    display_show(d);
    xba_finish();
}
