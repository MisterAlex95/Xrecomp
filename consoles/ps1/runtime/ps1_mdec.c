/* ps1_mdec.c -- the PlayStation macroblock decoder (P-9, XBA-167). Behaviour from psx-spx « Macroblock Decoder
 * (MDEC) »; our own code.
 *
 * Commands (written to 1F801820h or sent by DMA0): 1 decode macroblocks (output depth 4 / 8-bit monochrome, 15 / 24-bit
 * colour; signed or unsigned; bit 15 for 15-bit), 2 set the luminance (and colour) quant tables, 3 set the IDCT scale
 * table, 0 and 4..7 no function. The input is parsed one halfword at a time (run-length codes with the FE00h end /
 * padding code, quant scale, zigzag order, the q_scale = 0 mode without quantisation or zigzag), so a command may
 * arrive in any number of DMA blocks; a block ends at its end code or its 64th coefficient. Each block goes through
 * the IDCT with the scale table of command 3 (two passes of the 8x8 matrix product, 13 fractional bits of the table,
 * coefficients with 4 fractional bits, 15 bits rounded off per pass), then YUV -> RGB (R = Y + 1.402 Cr,
 * G = Y - 0.344 Cb - 0.714 Cr, B = Y + 1.772 Cb in 8-bit fixed point, the 9-bit clip, saturated to signed 8 bits; 15-bit
 * output rounded) or the 9-bit clip and saturation of monochrome output (4-bit rounded). That precision reproduces
 * the hardware's output of ps1-tests mdec/4bit exactly and mdec/8bit / frame to within one unit (XBA-200).
 *
 * Output (read from 1F801820h or DMA1): a colour macroblock comes out as its four 8x8 blocks, each row by row (what
 * reading the data register gets), and DMA1 writes it to RAM as the 16x16 picture in raster order (XBA-200, ps1-tests
 * mdec/frame's PIO and DMA builds); 24-bit pixels are 3 bytes R, G, B packed into words, 15-bit pixels are
 * halfwords; a monochrome block is 8x8 bytes (8-bit) or nibbles (4-bit, low nibble first). Status: output empty (31),
 * input full (30, never: the input is consumed at once), busy (29, parameters remaining or a decode's output not
 * all read), data-in / data-out requests (28 / 27, when enabled by the control register), the command's depth /
 * signed / bit-15 bits (26..23), the current block (18..16), parameters remaining minus one (15..0). Decoding is
 * instantaneous (the hardware takes time per block and holds 32-word FIFOs: mdec/step-by-step-log's stepping is not
 * reproduced). */
#include "ps1_devices.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t zigzag[64] = {
    0,  1,  5,  6,  14, 15, 27, 28, 2,  4,  7,  13, 16, 26, 29, 42, 3,  8,  12, 17, 25, 30, 41, 43, 9,  11, 18, 24,
    31, 40, 44, 53, 10, 19, 23, 32, 39, 45, 52, 54, 20, 22, 33, 38, 46, 51, 55, 60, 21, 34, 37, 47, 50, 56, 59, 61,
    35, 36, 48, 49, 57, 58, 62, 63,
};

enum { CMD_NONE, CMD_DECODE, CMD_QUANT, CMD_SCALE };

struct ps1_mdec {
    uint8_t zagzig[64];
    uint8_t iq_y[64], iq_uv[64];
    int16_t scale[64];

    int cmd;
    uint32_t cmd_word;
    uint32_t remaining; /* parameter words left */
    uint32_t got;       /* parameter bytes / halfwords received (quant / scale) */
    int dma_in, dma_out;

    /* decode */
    int depth, sign, bit15;
    int blk_index; /* colour: 0 Cr, 1 Cb, 2..5 Y1..Y4; mono: 0 */
    int in_block;
    int k, qscale;
    int32_t blk[64];
    int16_t cr[64], cb[64];
    int16_t y[4][64];

    uint32_t *out;
    size_t out_len, out_pos, out_cap;
};

/* ---- output ------------------------------------------------------------------------------------------------- */

/* Words in a colour macroblock (15-bit 128, 24-bit 192; 1 for monochrome output, which is never reordered). The
 * output keeps colour macroblocks at multiples of it, so a read's place in its macroblock is out_pos % mb_words. */
static size_t mb_words(const ps1_mdec *d)
{
    return d->depth == 3 ? 128 : d->depth == 2 ? 192 : 1;
}

static void out_word(ps1_mdec *d, uint32_t w)
{
    if (d->out_pos == d->out_len)
        d->out_pos = d->out_len = 0;
    if (d->out_len == d->out_cap) {
        size_t drop = d->out_pos - d->out_pos % mb_words(d);
        if (drop > 0) {
            memmove(d->out, d->out + drop, (d->out_len - drop) * sizeof *d->out);
            d->out_len -= drop;
            d->out_pos -= drop;
        }
        if (d->out_len == d->out_cap) {
            size_t cap = d->out_cap ? d->out_cap * 2 : 1024;
            uint32_t *n = realloc(d->out, cap * sizeof *n);
            if (!n)
                return;
            d->out = n;
            d->out_cap = cap;
        }
    }
    d->out[d->out_len++] = w;
}

static int sat8(int v) { return v < -128 ? -128 : v > 127 ? 127 : v; }

/* ---- IDCT --------------------------------------------------------------------------------------------------- */

/* The 9-bit result of the second pass, saturated to signed 8 bits. */
static int mask9(int32_t v)
{
    v &= 0x1FF;
    return sat8(v & 0x100 ? v - 0x200 : v);
}

/* Coefficients carry 4 fractional bits (see rle_halfword), the table 13 (its entries >> 3); each pass rounds off 15
 * bits, the first into 16 bits, the second into the 9-bit clip: the precision the hardware's output shows (ps1-tests
 * mdec/4bit, 8bit and frame VRAM dumps match, XBA-200). The result (signed 8 bits) is back in blk. */
static void idct(const ps1_mdec *d, int32_t blk[64])
{
    int32_t tmp[64];
    int32_t *src = blk, *dst = tmp;
    for (int pass = 0; pass < 2; pass++) {
        for (int x = 0; x < 8; x++) {
            for (int y = 0; y < 8; y++) {
                int32_t sum = 0;
                for (int z = 0; z < 8; z++)
                    sum += src[y + z * 8] * (d->scale[x + z * 8] >> 3);
                sum = (sum + 0x4000) >> 15;
                dst[x + y * 8] = pass ? mask9(sum) : (int16_t)sum;
            }
        }
        int32_t *t = src;
        src = dst;
        dst = t;
    }
    /* two passes: the result is back in blk */
}

/* ---- macroblock output -------------------------------------------------------------------------------------- */

static void emit_mono(ps1_mdec *d, const int32_t *blk)
{
    if (d->depth == 1) {
        uint8_t x = d->sign ? 0 : 0x80;
        for (int i = 0; i < 64; i += 4)
            out_word(d, (uint32_t)(uint8_t)(blk[i] ^ x) | (uint32_t)(uint8_t)(blk[i + 1] ^ x) << 8 |
                            (uint32_t)(uint8_t)(blk[i + 2] ^ x) << 16 | (uint32_t)(uint8_t)(blk[i + 3] ^ x) << 24);
    } else {
        /* 4 bits: the 8-bit value rounded (+8, capped at 127), its top nibble */
        uint32_t x = d->sign ? 0 : 8;
        for (int i = 0; i < 64; i += 8) {
            uint32_t w = 0;
            for (int j = 0; j < 8; j++) {
                int v = blk[i + j] + 8;
                w |= (((uint32_t)(uint8_t)(v > 127 ? 127 : v) >> 4) ^ x) << (4 * j);
            }
            out_word(d, w);
        }
    }
}

/* The raster word (16 x 16 picture, row by row) at block-order position i of an n-word macroblock (n = 128 or 192:
 * 4 or 6 words per block row). */
static size_t raster_word(size_t n, size_t i)
{
    size_t per = n / 32, blk = i / (n / 4), row = (i % (n / 4)) / per, w = i % per;
    return ((blk >> 1) * 8 + row) * (per * 2) + (blk & 1) * per + w;
}

/* The inverse: the block-order position of raster word r. */
static size_t block_word(size_t n, size_t r)
{
    size_t per = n / 32, row = r / (per * 2), c = r % (per * 2);
    return ((row >> 3) * 2 + c / per) * (n / 4) + (row & 7) * per + c % per;
}

static void emit_colour(ps1_mdec *d)
{
    uint8_t rgb[256][3];
    for (int py = 0; py < 16; py++) {
        for (int px = 0; px < 16; px++) {
            int c = (px >> 1) + (py >> 1) * 8;
            int cr = d->cr[c], cb = d->cb[c];
            int r = (359 * cr + 0x80) >> 8;                                   /* 1.402 */
            int g = ((-88 * cb & ~0x1F) + (-183 * cr & ~0x07) + 0x80) >> 8;  /* -0.344, -0.714 */
            int b = (454 * cb + 0x80) >> 8;                                   /* 1.772 */
            int yv = d->y[(px >> 3) + (py >> 3) * 2][(px & 7) + (py & 7) * 8];
            /* unsigned values here (signed output: flipped again below) */
            rgb[px + py * 16][0] = (uint8_t)(mask9(yv + r) ^ 0x80);
            rgb[px + py * 16][1] = (uint8_t)(mask9(yv + g) ^ 0x80);
            rgb[px + py * 16][2] = (uint8_t)(mask9(yv + b) ^ 0x80);
        }
    }
    uint32_t words[192];
    if (d->depth == 2) {
        const uint8_t *p = &rgb[0][0];
        uint32_t x = d->sign ? 0x80808080u : 0;
        for (int i = 0; i < 768; i += 4)
            words[i / 4] = ((uint32_t)p[i] | (uint32_t)p[i + 1] << 8 | (uint32_t)p[i + 2] << 16 |
                            (uint32_t)p[i + 3] << 24) ^ x;
    } else {
        /* 5 bits rounded ((c + 4) >> 3, capped); signed output flips each channel's top bit */
        uint16_t x = (uint16_t)((d->bit15 ? 0x8000 : 0) | (d->sign ? 0x4210 : 0));
        uint16_t px[2];
        for (int i = 0; i < 256; i += 2) {
            for (int k = 0; k < 2; k++) {
                int c5[3];
                for (int ch = 0; ch < 3; ch++) {
                    int v = (rgb[i + k][ch] + 4) >> 3;
                    c5[ch] = v > 31 ? 31 : v;
                }
                px[k] = (uint16_t)((c5[0] | c5[1] << 5 | c5[2] << 10) ^ x);
            }
            words[i / 2] = px[0] | (uint32_t)px[1] << 16;
        }
    }
    /* the hardware hands the macroblock out as its four 8x8 blocks (top left, top right, bottom left, bottom
     * right), each row by row: what a read of the data register gets (ps1-tests mdec/frame's PIO build swizzles
     * that); DMA1 puts them back in raster order (ps1_mdec_dma_read) */
    size_t n = mb_words(d);
    for (size_t i = 0; i < n; i++)
        out_word(d, words[raster_word(n, i)]);
}

static int colour(const ps1_mdec *d) { return d->depth >= 2; }

static void block_done(ps1_mdec *d)
{
    idct(d, d->blk);
    if (!colour(d)) {
        emit_mono(d, d->blk);
        return;
    }
    int16_t *dst = d->blk_index == 0 ? d->cr : d->blk_index == 1 ? d->cb : d->y[d->blk_index - 2];
    for (int i = 0; i < 64; i++)
        dst[i] = (int16_t)d->blk[i]; /* signed 8 bits */
    if (++d->blk_index == 6) {
        d->blk_index = 0;
        emit_colour(d);
    }
}

static int32_t s10(uint16_t n) { return (int32_t)((n & 0x3FF) ^ 0x200) - 0x200; }

/* A coefficient with 4 fractional bits: c * q (AC: (c * q) >> 3), rounded half a unit toward zero, saturated to
 * 15 bits; q = 0 (q_scale 0 mode, or a 0 table entry): c * 2. */
static int32_t coef(int32_t c, int32_t q, int ac)
{
    if (!q)
        return c * 32;
    int32_t v = (ac ? (c * q) >> 3 : c * q) * 16;
    return v + (c > 0 ? -8 : c < 0 ? 8 : 0);
}

static void store(ps1_mdec *d, int32_t val)
{
    val = val < -0x4000 ? -0x4000 : val > 0x3FFF ? 0x3FFF : val;
    if (d->qscale)
        d->blk[d->zagzig[d->k]] = val;
    else
        d->blk[d->k] = val;
}

static void rle_halfword(ps1_mdec *d, uint16_t n)
{
    const uint8_t *qt = (colour(d) && d->blk_index < 2) ? d->iq_uv : d->iq_y;
    if (!d->in_block) {
        if (n == 0xFE00)
            return; /* padding */
        memset(d->blk, 0, sizeof d->blk);
        d->k = 0;
        d->qscale = (n >> 10) & 0x3F;
        d->in_block = 1;
        store(d, coef(s10(n), d->qscale ? qt[0] : 0, 0));
        return;
    }
    d->k += ((n >> 10) & 0x3F) + 1;
    if (d->k <= 63) {
        store(d, coef(s10(n), qt[d->k] * d->qscale, 1));
        if (d->k < 63)
            return;
        /* the 64th coefficient ends the block without an end code (an FE00h after it is padding): ps1-tests
         * mdec/4bit and 8bit's sanitized streams, decoded on the hardware (XBA-200) */
    }
    d->in_block = 0;
    block_done(d);
}

/* ---- command interface -------------------------------------------------------------------------------------- */

static void param(ps1_mdec *d, uint32_t w)
{
    switch (d->cmd) {
    case CMD_DECODE:
        rle_halfword(d, (uint16_t)w);
        rle_halfword(d, (uint16_t)(w >> 16));
        break;
    case CMD_QUANT:
        for (int b = 0; b < 4; b++, d->got++) {
            uint8_t v = (uint8_t)(w >> (8 * b));
            if (d->got < 64)
                d->iq_y[d->got] = v;
            else if (d->got < 128)
                d->iq_uv[d->got - 64] = v;
        }
        break;
    case CMD_SCALE:
        for (int h = 0; h < 2; h++, d->got++)
            if (d->got < 64)
                d->scale[d->got] = (int16_t)(w >> (16 * h));
        break;
    default: break;
    }
    if (--d->remaining == 0)
        d->cmd = CMD_NONE;
}

static void command(ps1_mdec *d, uint32_t w)
{
    d->cmd_word = w;
    d->got = 0;
    switch (w >> 29) {
    case 1:
        d->depth = (w >> 27) & 3;
        d->sign = (w >> 26) & 1;
        d->bit15 = (w >> 25) & 1;
        d->remaining = w & 0xFFFF;
        d->blk_index = 0;
        d->in_block = 0;
        d->cmd = d->remaining ? CMD_DECODE : CMD_NONE;
        break;
    case 2:
        d->remaining = (w & 1) ? 32 : 16;
        d->cmd = CMD_QUANT;
        break;
    case 3:
        d->remaining = 32;
        d->cmd = CMD_SCALE;
        break;
    default:
        d->remaining = 0;
        d->cmd = CMD_NONE;
        break;
    }
}

static void write_word(ps1_mdec *d, uint32_t w)
{
    if (d->cmd == CMD_NONE)
        command(d, w);
    else
        param(d, w);
}

static void reset(ps1_mdec *d)
{
    d->cmd = CMD_NONE;
    d->cmd_word = 0;
    d->remaining = 0;
    d->in_block = 0;
    d->blk_index = 0;
    d->depth = d->sign = d->bit15 = 0;
    d->out_len = d->out_pos = 0;
}

ps1_mdec *ps1_mdec_create(void)
{
    ps1_mdec *d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    for (int i = 0; i < 64; i++)
        d->zagzig[zigzag[i]] = (uint8_t)i;
    reset(d);
    return d;
}

void ps1_mdec_destroy(ps1_mdec *d)
{
    if (!d)
        return;
    free(d->out);
    free(d);
}

uint32_t ps1_mdec_read(ps1_mdec *d, uint32_t reg)
{
    if (reg == 0) {
        if (d->out_pos < d->out_len)
            return d->out[d->out_pos++];
        return 0; /* garbage on hardware */
    }
    uint32_t st = 0;
    int empty = d->out_pos >= d->out_len;
    if (empty)
        st |= 1u << 31;
    /* busy: parameters to come, or a decode's output not all read (ps1-tests mdec/step-by-step-log on the hardware:
     * set after the last parameter until the last word is read) */
    if (d->cmd != CMD_NONE || ((d->cmd_word >> 29) == 1 && !empty))
        st |= 1u << 29;
    if (d->dma_in && d->cmd != CMD_NONE)
        st |= 1u << 28;
    if (d->dma_out && !empty)
        st |= 1u << 27;
    st |= ((d->cmd_word >> 25) & 0xF) << 23;
    int blk = colour(d) ? (d->blk_index < 2 ? 4 + d->blk_index : d->blk_index - 2) : 4;
    st |= (uint32_t)blk << 16;
    st |= (d->remaining - 1) & 0xFFFF;
    if ((d->cmd_word >> 29) == 0 || (d->cmd_word >> 29) >= 4)
        st = (st & ~0xFFFFu) | (d->cmd_word & 0xFFFF);
    return st;
}

void ps1_mdec_write(ps1_mdec *d, uint32_t reg, uint32_t v)
{
    if (reg == 0) {
        write_word(d, v);
        return;
    }
    if (v & (1u << 31))
        reset(d); /* status reads 80040000h */
    d->dma_in = (v >> 30) & 1;
    d->dma_out = (v >> 29) & 1;
}

void ps1_mdec_dma_write(ps1_mdec *d, const uint32_t *words, size_t n)
{
    for (size_t i = 0; i < n; i++)
        write_word(d, words[i]);
}

size_t ps1_mdec_dma_read(ps1_mdec *d, uint32_t *words, size_t n)
{
    size_t got = 0, mb = mb_words(d);
    if (mb > 1) {
        /* DMA1 writes a colour macroblock's blocks back in raster order */
        while (got < n && d->out_pos < d->out_len) {
            size_t base = d->out_pos - d->out_pos % mb, k = base + block_word(mb, d->out_pos % mb);
            words[got++] = k < d->out_len ? d->out[k] : 0;
            d->out_pos++;
        }
    }
    while (got < n && d->out_pos < d->out_len)
        words[got++] = d->out[d->out_pos++];
    for (size_t i = got; i < n; i++)
        words[i] = 0;
    return got;
}

size_t ps1_mdec_out_pending(ps1_mdec *d)
{
    return d->out_len - d->out_pos;
}

/* ---- Save states (XBA-182) ------------------------------------------------------------------------------------ */

/* The decoder (tables, command, block in progress), then the output words not read yet. */
size_t ps1_mdec_state_size(ps1_mdec *d)
{
    size_t base = d->out_pos - d->out_pos % mb_words(d); /* from the macroblock being read */
    return sizeof(ps1_mdec) + (d->out_len - base) * sizeof(uint32_t);
}

void ps1_mdec_state_save(ps1_mdec *d, uint8_t *out)
{
    ps1_mdec c = *d;
    size_t base = d->out_pos - d->out_pos % mb_words(d);
    c.out = NULL;
    c.out_len -= base;
    c.out_pos -= base;
    c.out_cap = 0;
    memcpy(out, &c, sizeof c);
    if (c.out_len)
        memcpy(out + sizeof c, d->out + base, c.out_len * sizeof(uint32_t));
}

/* The size (the output words that follow), then the fields the decoder indexes with: the inverse zigzag (a constant
 * table, blk[zagzig[k]]), the coefficient index of the block in progress, the block of the macroblock (cr, cb,
 * y[0..3]), the output depth (0..3: mb_words and the colour path know no other) and the quantizer scale (6 bits). */
int ps1_mdec_state_valid(const uint8_t *in, size_t n)
{
    ps1_mdec c;
    if (n < sizeof c)
        return -1;
    memcpy(&c, in, sizeof c);
    if (c.out_len > (n - sizeof c) / sizeof(uint32_t) || n != sizeof c + c.out_len * sizeof(uint32_t))
        return -1;
    if (c.out_pos > c.out_len)
        return -2;
    for (int i = 0; i < 64; i++)
        if (c.zagzig[zigzag[i]] != i)
            return -2;
    if ((c.in_block && (c.k < 0 || c.k > 62)) || c.blk_index < 0 || c.blk_index > 5 || c.depth < 0 || c.depth > 3 ||
        c.qscale < 0 || c.qscale > 63)
        return -2;
    return 0;
}

int ps1_mdec_state_load(ps1_mdec *d, const uint8_t *in, size_t n)
{
    ps1_mdec c;
    if (n < sizeof c)
        return -1;
    memcpy(&c, in, sizeof c);
    /* out_len comes from the file: bounded by what the file holds before it is multiplied (a huge value
     * wrapped the product, passed the check and left out_len far past the buffer). */
    if (c.out_len > (n - sizeof c) / sizeof(uint32_t) || c.out_pos > c.out_len
        || n != sizeof c + c.out_len * sizeof(uint32_t))
        return -1;
    uint32_t *buf = d->out;
    size_t cap = d->out_cap;
    if (c.out_len > cap) {
        uint32_t *nb = realloc(buf, c.out_len * sizeof *nb);
        if (!nb)
            return -1;
        buf = nb;
        cap = c.out_len;
    }
    *d = c;
    d->out = buf;
    d->out_cap = cap;
    if (c.out_len)
        memcpy(d->out, in + sizeof c, c.out_len * sizeof(uint32_t));
    return 0;
}
