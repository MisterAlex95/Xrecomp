/*
 * d3d8_dxt_decode_block decodes the same sixteen texels as sixteen calls to
 * d3d8_dxt_decode_texel, for DXT1, DXT3 and DXT5.
 *
 * Random blocks plus the edge cases that pick a different palette: DXT1 with
 * c0 <= c1 (three colours and one-bit alpha), equal endpoints, and DXT5 with
 * a0 <= a1 (six alphas plus 0 and 255).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "d3d8_swizzle.h"

static uint32_t s_rng = 0x12345678u;
static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return s_rng;
}

static int check(const uint8_t *blk, uint32_t fmt, const char *what)
{
    uint32_t block[16], i;
    if (!d3d8_dxt_decode_block(blk, fmt, block)) {
        printf("FAIL %s: block decode refused fmt 0x%02X\n", what, fmt);
        return 1;
    }
    for (i = 0; i < 16; i++) {
        uint32_t one = 0;
        d3d8_dxt_decode_texel(blk, fmt, i & 3, i >> 2, 4, &one);
        if (one != block[i]) {
            printf("FAIL %s fmt 0x%02X texel %u: block 0x%08X, texel 0x%08X\n",
                   what, fmt, i, block[i], one);
            return 1;
        }
    }
    return 0;
}

int main(void)
{
    static const uint32_t fmts[3] = { 0x0C, 0x0E, 0x0F };
    uint8_t blk[16];
    int fail = 0, n, f, k;
    uint32_t px[16];

    for (f = 0; f < 3; f++) {
        uint32_t bb = d3d8_format_dxt_block_bytes(fmts[f]);
        for (n = 0; n < 20000; n++) {
            for (k = 0; k < 16; k++)
                blk[k] = (uint8_t)rnd();
            fail += check(blk, fmts[f], "random");
            /* Force each palette mode: endpoints ordered both ways, equal. */
            {
                uint8_t *cb = blk + (bb == 16 ? 8 : 0);
                uint16_t c0 = (uint16_t)(cb[0] | cb[1] << 8), c1 = (uint16_t)(cb[2] | cb[3] << 8);
                cb[0] = (uint8_t)c1; cb[1] = (uint8_t)(c1 >> 8);
                cb[2] = (uint8_t)c0; cb[3] = (uint8_t)(c0 >> 8);
                fail += check(blk, fmts[f], "swapped endpoints");
                cb[2] = cb[0]; cb[3] = cb[1];
                fail += check(blk, fmts[f], "equal endpoints");
            }
            if (fmts[f] == 0x0F) {
                uint8_t a = blk[0];
                blk[0] = blk[1]; blk[1] = a;
                fail += check(blk, fmts[f], "swapped alpha endpoints");
            }
            if (fail > 10)
                break;
        }
    }
    if (d3d8_dxt_decode_block(blk, 0x12, px)) {
        printf("FAIL: a non-DXT format was decoded\n");
        fail++;
    }
    printf("%s\n", fail ? "FAILED" : "dxt_decode: block == 16 x texel for DXT1/3/5");
    return fail ? 1 : 0;
}
