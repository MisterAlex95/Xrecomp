/* Unit tests of ps1_mdec.c (P-9, XBA-167): hand-made macroblocks with known output. Monochrome DC-only and DC + AC
 * blocks against a floating-point IDCT, the 4 / 8 / 15 / 24-bit outputs, signed / unsigned, bit 15, YUV -> RGB, the
 * q_scale = 0 mode, FE00h padding, quant / scale table commands, the status register and DMA. Our own code. */
#include "dev_test.h"

#include <math.h>

#define PI 3.14159265358979323846
#define SQRT1_2 0.70710678118654752440

/* psx-spx « set_scale_table »: the standard IDCT matrix. */
static const uint16_t std_scale[64] = {
    0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x7D8A, 0x6A6D, 0x471C, 0x18F8, 0xE707, 0xB8E3,
    0x9592, 0x8275, 0x7641, 0x30FB, 0xCF04, 0x89BE, 0x89BE, 0xCF04, 0x30FB, 0x7641, 0x6A6D, 0xE707, 0x8275, 0xB8E3,
    0x471C, 0x7D8A, 0x18F8, 0x9592, 0x5A82, 0xA57D, 0xA57D, 0x5A82, 0x5A82, 0xA57D, 0xA57D, 0x5A82, 0x471C, 0x8275,
    0x18F8, 0x6A6D, 0x9592, 0xE707, 0x7D8A, 0xB8E3, 0x30FB, 0x89BE, 0x7641, 0xCF04, 0xCF04, 0x7641, 0x89BE, 0x30FB,
    0x18F8, 0xB8E3, 0x6A6D, 0x8275, 0x7D8A, 0x9592, 0x471C, 0xE707,
};

static ps1_mdec *md;

static void send(const uint32_t *w, size_t n) { ps1_mdec_dma_write(md, w, n); }

/* Quant tables: luminance qy[0] = 2, the rest 8; colour the same. */
static void setup(uint8_t q0)
{
    if (md)
        ps1_mdec_destroy(md);
    md = ps1_mdec_create();
    ps1_mdec_write(md, 1, 0x80000000u);
    uint32_t w[1 + 32];
    w[0] = 0x40000001u; /* quant, luminance + colour */
    uint8_t q[128];
    memset(q, 8, sizeof q);
    q[0] = q[64] = q0;
    memcpy(w + 1, q, 128);
    send(w, 33);
    w[0] = 0x60000000u; /* scale */
    memcpy(w + 1, std_scale, 128);
    send(w, 33);
}

/* Halfwords -> a decode command (padded with FE00h to whole words). */
static void decode(uint32_t cmdbits, const uint16_t *h, size_t nh)
{
    uint32_t w[512];
    size_t nw = (nh + 1) / 2;
    w[0] = 0x20000000u | cmdbits | (uint32_t)nw;
    for (size_t i = 0; i < nw; i++) {
        uint16_t lo = h[2 * i], hi = 2 * i + 1 < nh ? h[2 * i + 1] : 0xFE00;
        w[1 + i] = lo | (uint32_t)hi << 16;
    }
    send(w, nw + 1);
}

static uint16_t rl(int run_or_q, int val) { return (uint16_t)((run_or_q << 10) | (val & 0x3FF)); }

static double ref_idct(const double F[64], int x, int y)
{
    double s = 0;
    for (int v = 0; v < 8; v++)
        for (int u = 0; u < 8; u++) {
            double cu = u ? 1 : SQRT1_2, cv = v ? 1 : SQRT1_2;
            s += cu * cv * F[u + v * 8] * cos((2 * x + 1) * u * PI / 16) * cos((2 * y + 1) * v * PI / 16);
        }
    return s / 4;
}

static void test_status(void)
{
    md = ps1_mdec_create();
    ps1_mdec_write(md, 1, 0x80000000u);
    CHECK(ps1_mdec_read(md, 1) == 0x80040000u, "status after reset %08X", ps1_mdec_read(md, 1));
    ps1_mdec_write(md, 1, 0x60000000u); /* DMA in / out enabled */
    ps1_mdec_write(md, 0, 0x30000003u); /* decode, depth 2 (24-bit), 3 words */
    uint32_t st = ps1_mdec_read(md, 1);
    CHECK((st & (1u << 29)) && (st & (1u << 28)) && (st & 0xFFFF) == 2, "status while receiving %08X", st);
    CHECK(((st >> 25) & 3) == 2 && !((st >> 24) & 1), "depth bits %08X", st);
    ps1_mdec_destroy(md);
    md = NULL;
}

static void test_mono(void)
{
    setup(2);
    uint32_t out[16];
    /* DC 400 x qt 2 = 800 -> 100 everywhere; unsigned 8-bit: 100 + 128 */
    uint16_t blk[] = {0xFE00, 0xFE00, rl(1, 400), 0xFE00};
    decode(1u << 27, blk, 4);
    CHECK(!(ps1_mdec_read(md, 1) & (1u << 31)), "output empty after a block");
    CHECK(ps1_mdec_dma_read(md, out, 16) == 16, "16 words of 8-bit output");
    const uint8_t *px = (const uint8_t *)out;
    int bad = 0;
    for (int i = 0; i < 64; i++)
        bad += px[i] != 228;
    CHECK(bad == 0, "DC-only block: %d pixels differ (first %d)", bad, px[0]);
    CHECK(ps1_mdec_read(md, 1) & (1u << 31), "output empty after reading");

    /* DC + one AC coefficient (zigzag 1: horizontal frequency 1) against a float IDCT */
    uint16_t blk2[] = {rl(1, 320), rl(0, 100), 0xFE00};
    decode((1u << 27) | (1u << 26), blk2, 3); /* signed */
    ps1_mdec_dma_read(md, out, 16);
    double F[64] = {0};
    F[0] = 320 * 2;
    F[1] = (100 * 8 * 1 + 4) / 8;
    int worst = 0;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            int want = (int)lround(ref_idct(F, x, y));
            int got = (int8_t)((const uint8_t *)out)[x + y * 8];
            int d = abs(got - want);
            if (d > worst)
                worst = d;
        }
    CHECK(worst <= 1, "DC + AC block: worst error %d", worst);

    /* 4-bit: the high nibble of each unsigned pixel, low nibble first */
    decode(0, blk, 4);
    CHECK(ps1_mdec_dma_read(md, out, 8) == 8 && out[0] == 0xEEEEEEEEu, "4-bit output %08X", out[0]);

    /* q_scale 0: value x 2, no quant, no zigzag; and the 9-bit clip (DC 511 x 2 = 1022 -> 128 -> clipped 127) */
    uint16_t blk3[] = {rl(0, 200), 0xFE00, rl(0, 1000 / 2), 0xFE00};
    decode((1u << 27) | (1u << 26), blk3, 4);
    ps1_mdec_dma_read(md, out, 16);
    CHECK(((const int8_t *)out)[0] == 50 && ((const int8_t *)out)[63] == 50, "q_scale 0 DC: %d",
          ((const int8_t *)out)[0]);
    ps1_mdec_dma_read(md, out, 16);
    CHECK(((const int8_t *)out)[0] == 125, "q_scale 0 DC 500: %d", ((const int8_t *)out)[0]);

    /* a bigger luminance quant: DC doubles */
    setup(4);
    uint16_t blk4[] = {rl(1, 100), 0xFE00};
    decode(1u << 27, blk4, 2);
    ps1_mdec_dma_read(md, out, 16);
    CHECK(((const uint8_t *)out)[0] == (50 ^ 0x80), "quant table 4: %d", ((const uint8_t *)out)[0]);
    /* saturation: DC 400 x 4 = 1600 is clamped to 3FFh before the IDCT -> 127 */
    decode(1u << 27, blk, 4);
    ps1_mdec_dma_read(md, out, 16);
    CHECK(((const uint8_t *)out)[0] == 255, "saturated DC: %d", ((const uint8_t *)out)[0]);
}

static void colour_mb(int16_t cr, int16_t cb, const int y[4], uint32_t cmdbits)
{
    uint16_t h[32];
    int n = 0;
    h[n++] = rl(1, cr / 2 * 8 / 8); /* qt[0] = 2: DC 8 * value -> value after the IDCT */
    h[n++] = 0xFE00;
    h[n++] = rl(1, cb / 2 * 8 / 8);
    h[n++] = 0xFE00;
    for (int k = 0; k < 4; k++) {
        h[n++] = rl(1, y[k] * 4);
        h[n++] = 0xFE00;
    }
    decode(cmdbits, h, (size_t)n);
}

static void test_colour(void)
{
    setup(2);
    static uint32_t out[192];
    const int ys[4] = {10, 20, -30, 40};
    colour_mb(0, 0, ys, (3u << 27) | (1u << 25)); /* 15-bit, unsigned, bit 15 */
    CHECK(ps1_mdec_dma_read(md, out, 128) == 128, "15-bit macroblock: 128 words");
    const uint16_t *px = (const uint16_t *)out;
    int pts[4][2] = {{0, 0}, {15, 0}, {0, 15}, {15, 15}};
    for (int q = 0; q < 4; q++) {
        int v = (((ys[q] + 128) & 0xFF) + 4) >> 3; /* 5 bits rounded (XBA-200) */
        uint16_t want = (uint16_t)(v | v << 5 | v << 10 | 0x8000);
        uint16_t got = px[pts[q][0] + pts[q][1] * 16];
        CHECK(got == want, "quadrant %d: %04X, expected %04X", q, got, want);
    }
    /* 24-bit, signed, with colour: R = Y + 1.402 Cr, G = Y - 0.714 Cr in the hardware's fixed point (XBA-200) */
    const int yz[4] = {0, 0, 0, 0};
    colour_mb(16 * 8 / 2 * 2 / 2, 0, yz, (2u << 27) | (1u << 26));
    CHECK(ps1_mdec_dma_read(md, out, 192) == 192, "24-bit macroblock: 192 words");
    const int8_t *b = (const int8_t *)out;
    /* Cr DC: rl value 32 -> 64 after quant -> 8 after the IDCT */
    int cr = 8, r = (359 * cr + 0x80) >> 8, g = ((-183 * cr & ~7) + 0x80) >> 8;
    CHECK(b[0] == r && b[1] == g && b[2] == 0, "24-bit pixel 0: %d %d %d, expected %d %d 0", b[0], b[1], b[2], r, g);
    CHECK(b[255 * 3] == r && b[255 * 3 + 2] == 0, "24-bit last pixel");
    /* two macroblocks in one command, one DMA read each */
    uint16_t two[] = {rl(1, 0), 0xFE00, rl(1, 0), 0xFE00, rl(1, 4), 0xFE00, rl(1, 4), 0xFE00, rl(1, 4), 0xFE00,
                      rl(1, 4), 0xFE00, rl(1, 0), 0xFE00, rl(1, 0), 0xFE00, rl(1, 8), 0xFE00, rl(1, 8), 0xFE00,
                      rl(1, 8), 0xFE00, rl(1, 8), 0xFE00};
    decode(3u << 27, two, sizeof two / 2);
    ps1_mdec_dma_read(md, out, 128);
    uint16_t p1 = ((const uint16_t *)out)[0];
    ps1_mdec_dma_read(md, out, 128);
    uint16_t p2 = ((const uint16_t *)out)[0];
    CHECK((p1 & 31) == (129 >> 3) && (p2 & 31) == (130 >> 3), "two macroblocks: %04X %04X", p1, p2);
    CHECK(!(ps1_mdec_read(md, 1) & (1u << 29)), "busy after the last parameter");
    ps1_mdec_destroy(md);
    md = NULL;
}

int main(void)
{
    test_status();
    test_mono();
    test_colour();
    if (md)
        ps1_mdec_destroy(md);
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ps1_mdec: all tests passed\n");
    return 0;
}
