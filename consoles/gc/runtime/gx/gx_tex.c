/* gx_tex.c -- GameCube texture formats (XBA-262): decoding to RGBA8 for the renderer, encoding back for EFB copies to
 * RAM, the texture registers of a map, a content hash for the cache.
 *
 * Formats are stored in tiles (blocks) row by row, big-endian:
 *   I4 / C4 / CMPR 8x8 texels per 32-byte block, I8 / IA4 / C8 8x4, IA8 / RGB565 / RGB5A3 / C14X2 4x4,
 *   RGBA8 4x4 in 64 bytes (16 AR pairs, then 16 GB pairs).
 * I4 high nibble first; IA4 alpha in the high nibble; IA8 alpha byte first; RGB5A3 bit 15 set = RGB555 opaque, else
 * 3-bit alpha + RGB444; CMPR = four DXT1-style 4x4 sub-blocks (colours RGB565 big-endian, 2-bit indices MSB =
 * leftmost texel; c0 > c1: two interpolated colours, else their average and transparent black). Colour-indexed
 * formats read a palette (TLUT) from TMEM: IA8, RGB565 or RGB5A3 entries. */
#include <string.h>

#include "gx_internal.h"

static void block_dims(unsigned fmt, unsigned *bw, unsigned *bh, unsigned *bytes)
{
    *bytes = 32;
    switch (fmt) {
    case TF_I4: case TF_C4: case TF_CMPR:
        *bw = 8, *bh = 8;
        break;
    case TF_I8: case TF_IA4: case TF_C8:
        *bw = 8, *bh = 4;
        break;
    case TF_RGBA8:
        *bw = 4, *bh = 4, *bytes = 64;
        break;
    default:
        *bw = 4, *bh = 4;
        break;
    }
}

uint32_t gx_tex_level_size(unsigned fmt, unsigned w, unsigned h)
{
    unsigned bw, bh, bytes;
    block_dims(fmt, &bw, &bh, &bytes);
    return ((w + bw - 1) / bw) * ((h + bh - 1) / bh) * bytes;
}

void gx_tex_info(gx_state *gx, unsigned map, gx_texinfo *ti)
{
    uint32_t img0 = gx->bp[bp_tex_reg(BP_TX_IMAGE0, map)];
    uint32_t img3 = gx->bp[bp_tex_reg(BP_TX_IMAGE3, map)];
    uint32_t mode0 = gx->bp[bp_tex_reg(BP_TX_MODE0, map)];
    uint32_t mode1 = gx->bp[bp_tex_reg(BP_TX_MODE1, map)];
    uint32_t tlut = gx->bp[bp_tex_reg(BP_TX_TLUT, map)];
    ti->w = BITS(img0, 0, 10) + 1;
    ti->h = BITS(img0, 10, 10) + 1;
    ti->fmt = BITS(img0, 20, 4);
    ti->addr = (img3 & 0xFFFFFFu) << 5;
    ti->tlut_addr = (0x80000u + ((tlut & 0x3FFu) << 9)) & (GX_TMEM_SIZE - 1);
    ti->tlut_fmt = BITS(tlut, 10, 2);
    ti->levels = 1;
    unsigned minf = BITS(mode0, 5, 3);
    if ((minf & 3) != 0) { /* a mip filter */
        unsigned maxlod = BITS(mode1, 8, 8) / 16;
        unsigned w = ti->w, h = ti->h;
        while (ti->levels <= maxlod && (w > 1 || h > 1) && ti->levels < 11) {
            w = w > 1 ? w / 2 : 1;
            h = h > 1 ? h / 2 : 1;
            ti->levels++;
        }
    }
    ti->size = 0;
    unsigned w = ti->w, h = ti->h;
    for (unsigned l = 0; l < ti->levels; l++) {
        ti->size += gx_tex_level_size(ti->fmt, w, h);
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
}

static inline uint8_t x5(unsigned v) { return (uint8_t)(v << 3 | v >> 2); }
static inline uint8_t x6(unsigned v) { return (uint8_t)(v << 2 | v >> 4); }
static inline uint8_t x4(unsigned v) { return (uint8_t)(v << 4 | v); }
static inline uint8_t x3(unsigned v) { return (uint8_t)(v << 5 | v << 2 | v >> 1); }

static void rgb565(unsigned v, uint8_t *o)
{
    o[0] = x5(v >> 11), o[1] = x6((v >> 5) & 63), o[2] = x5(v & 31), o[3] = 255;
}

static void rgb5a3(unsigned v, uint8_t *o)
{
    if (v & 0x8000) {
        o[0] = x5((v >> 10) & 31), o[1] = x5((v >> 5) & 31), o[2] = x5(v & 31), o[3] = 255;
    } else {
        o[0] = x4((v >> 8) & 15), o[1] = x4((v >> 4) & 15), o[2] = x4(v & 15), o[3] = x3((v >> 12) & 7);
    }
}

static void palette(gx_state *gx, const gx_texinfo *ti, unsigned idx, uint8_t *o)
{
    uint32_t a = (ti->tlut_addr + idx * 2) & (GX_TMEM_SIZE - 1);
    unsigned v = gx->tmem[a] << 8 | gx->tmem[(a + 1) & (GX_TMEM_SIZE - 1)];
    switch (ti->tlut_fmt) {
    case 0: /* IA8 */
        o[0] = o[1] = o[2] = (uint8_t)v, o[3] = (uint8_t)(v >> 8);
        break;
    case 1:
        rgb565(v, o);
        break;
    default:
        rgb5a3(v, o);
        break;
    }
}

static inline uint8_t blend38(uint8_t a, uint8_t b) /* 3/8 a + 5/8 b, as the hardware interpolates CMPR */
{
    return (uint8_t)((a * 3 + b * 5) >> 3);
}

void gx_tex_decode(gx_state *gx, const gx_texinfo *ti, const uint8_t *src, unsigned w, unsigned h, uint8_t *rgba)
{
    unsigned bw, bh, bytes;
    block_dims(ti->fmt, &bw, &bh, &bytes);
    unsigned bx_n = (w + bw - 1) / bw, by_n = (h + bh - 1) / bh;
    for (unsigned by = 0; by < by_n; by++) {
        for (unsigned bx = 0; bx < bx_n; bx++) {
            const uint8_t *b = src + (size_t)(by * bx_n + bx) * bytes;
            if (ti->fmt == TF_CMPR) {
                for (unsigned sb = 0; sb < 4; sb++) {
                    const uint8_t *d = b + sb * 8;
                    unsigned c0 = gx_be16(d), c1 = gx_be16(d + 2);
                    uint8_t pal[4][4];
                    rgb565(c0, pal[0]);
                    rgb565(c1, pal[1]);
                    if (c0 > c1) {
                        for (int k = 0; k < 3; k++) {
                            pal[2][k] = blend38(pal[1][k], pal[0][k]);
                            pal[3][k] = blend38(pal[0][k], pal[1][k]);
                        }
                        pal[2][3] = pal[3][3] = 255;
                    } else {
                        for (int k = 0; k < 3; k++) {
                            pal[2][k] = (uint8_t)((pal[0][k] + pal[1][k]) / 2);
                            pal[3][k] = 0;
                        }
                        pal[2][3] = 255;
                        pal[3][3] = 0;
                    }
                    unsigned ox = bx * 8 + (sb & 1) * 4, oy = by * 8 + (sb >> 1) * 4;
                    for (unsigned y = 0; y < 4; y++) {
                        uint8_t row = d[4 + y];
                        for (unsigned x = 0; x < 4; x++) {
                            unsigned px = ox + x, py = oy + y;
                            if (px < w && py < h)
                                memcpy(rgba + ((size_t)py * w + px) * 4, pal[(row >> (6 - 2 * x)) & 3], 4);
                        }
                    }
                }
                continue;
            }
            for (unsigned y = 0; y < bh; y++) {
                for (unsigned x = 0; x < bw; x++) {
                    unsigned px = bx * bw + x, py = by * bh + y;
                    uint8_t o[4];
                    unsigned i = y * bw + x;
                    switch (ti->fmt) {
                    case TF_I4: {
                        unsigned n = (b[i >> 1] >> ((i & 1) ? 0 : 4)) & 15;
                        o[0] = o[1] = o[2] = o[3] = x4(n);
                        break;
                    }
                    case TF_I8:
                        o[0] = o[1] = o[2] = o[3] = b[i];
                        break;
                    case TF_IA4:
                        o[0] = o[1] = o[2] = x4(b[i] & 15), o[3] = x4(b[i] >> 4);
                        break;
                    case TF_IA8:
                        o[0] = o[1] = o[2] = b[2 * i + 1], o[3] = b[2 * i];
                        break;
                    case TF_RGB565:
                        rgb565(gx_be16(b + 2 * i), o);
                        break;
                    case TF_RGB5A3:
                        rgb5a3(gx_be16(b + 2 * i), o);
                        break;
                    case TF_RGBA8:
                        o[3] = b[2 * i], o[0] = b[2 * i + 1], o[1] = b[32 + 2 * i], o[2] = b[32 + 2 * i + 1];
                        break;
                    case TF_C4:
                        palette(gx, ti, (b[i >> 1] >> ((i & 1) ? 0 : 4)) & 15, o);
                        break;
                    case TF_C8:
                        palette(gx, ti, b[i], o);
                        break;
                    case TF_C14X2:
                        palette(gx, ti, gx_be16(b + 2 * i) & 0x3FFF, o);
                        break;
                    default: /* unknown format: magenta, easy to spot */
                        o[0] = 255, o[1] = 0, o[2] = 255, o[3] = 255;
                        break;
                    }
                    if (px < w && py < h)
                        memcpy(rgba + ((size_t)py * w + px) * 4, o, 4);
                }
            }
        }
    }
}

void gx_tex_encode(unsigned fmt, const uint8_t *rgba, unsigned w, unsigned h, uint8_t *dst, uint32_t stride)
{
    unsigned bw, bh, bytes;
    block_dims(fmt, &bw, &bh, &bytes);
    unsigned bx_n = (w + bw - 1) / bw, by_n = (h + bh - 1) / bh;
    for (unsigned by = 0; by < by_n; by++) {
        for (unsigned bx = 0; bx < bx_n; bx++) {
            uint8_t *b = dst + (size_t)by * stride + (size_t)bx * bytes;
            memset(b, 0, bytes);
            for (unsigned y = 0; y < bh; y++) {
                for (unsigned x = 0; x < bw; x++) {
                    unsigned px = bx * bw + x, py = by * bh + y, i = y * bw + x;
                    static const uint8_t black[4] = {0, 0, 0, 0};
                    const uint8_t *s = (px < w && py < h) ? rgba + ((size_t)py * w + px) * 4 : black;
                    switch (fmt) {
                    case TF_I4:
                        b[i >> 1] |= (uint8_t)((s[0] >> 4) << ((i & 1) ? 0 : 4));
                        break;
                    case TF_I8:
                        b[i] = s[0];
                        break;
                    case TF_IA4:
                        b[i] = (uint8_t)((s[3] & 0xF0) | (s[0] >> 4));
                        break;
                    case TF_IA8:
                        b[2 * i] = s[3], b[2 * i + 1] = s[0];
                        break;
                    case TF_RGB565: {
                        unsigned v = (unsigned)(s[0] >> 3) << 11 | (unsigned)(s[1] >> 2) << 5 | (s[2] >> 3);
                        b[2 * i] = (uint8_t)(v >> 8), b[2 * i + 1] = (uint8_t)v;
                        break;
                    }
                    case TF_RGB5A3: {
                        unsigned v;
                        if (s[3] >= 0xE0)
                            v = 0x8000 | (unsigned)(s[0] >> 3) << 10 | (unsigned)(s[1] >> 3) << 5 | (s[2] >> 3);
                        else
                            v = (unsigned)(s[3] >> 5) << 12 | (unsigned)(s[0] >> 4) << 8 | (unsigned)(s[1] >> 4) << 4 |
                                (s[2] >> 4);
                        b[2 * i] = (uint8_t)(v >> 8), b[2 * i + 1] = (uint8_t)v;
                        break;
                    }
                    case TF_RGBA8:
                        b[2 * i] = s[3], b[2 * i + 1] = s[0], b[32 + 2 * i] = s[1], b[32 + 2 * i + 1] = s[2];
                        break;
                    default:
                        break;
                    }
                }
            }
        }
    }
}

uint64_t gx_hash(const uint8_t *p, size_t n, uint64_t seed)
{
    uint64_t h = seed ^ (0x9E3779B97F4A7C15ull * (n + 1));
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h ^= w * 0xC2B2AE3D27D4EB4Full;
        h = (h << 31 | h >> 33) * 0x9E3779B185EBCA87ull;
    }
    for (; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    h ^= h >> 29;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 32;
    return h;
}
