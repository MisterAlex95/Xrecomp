/* gc_audio_util.c -- the GameCube sound's pure pieces (XBA-263): the ucode hash, the drive's stream decoder, the
 * resampler to 48 kHz and the DSP accelerator's sample reader. No machine, no floating point (the core runs these
 * between guest instructions, under the guest's rounding mode). Written from the documented behaviour of the
 * hardware (facts only). See gc_audio.h. */
#include "gc_audio.h"

uint32_t gc_ucode_hash(const uint8_t *p, size_t n)
{
    uint32_t h = 0;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h = h << 3 | h >> 29;
    }
    return h;
}

static int32_t clamp32(int64_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : v > hi ? hi : (int32_t)v;
}

/* ---- DTK: the drive's 4-bit ADPCM (the CD-XA filters: 0, 60/64, 115/64 - 52/64, 98/64 - 55/64) ---- */

static int16_t dtk_sample(unsigned nibble, unsigned hdr, int32_t *h1, int32_t *h2)
{
    int64_t p;
    switch ((hdr >> 4) & 3) {
    case 1: p = (int64_t)*h1 * 60; break;
    case 2: p = (int64_t)*h1 * 115 - (int64_t)*h2 * 52; break;
    case 3: p = (int64_t)*h1 * 98 - (int64_t)*h2 * 55; break;
    default: p = 0; break;
    }
    int32_t pred = clamp32((p + 32) >> 6, -0x200000, 0x1FFFFF);
    int32_t s = (int16_t)(nibble << 12);   /* the nibble in the top of a 16-bit word, signed */
    s >>= hdr & 15;
    int32_t cur = s * 64 + pred;            /* 6 fraction bits of history */
    *h2 = *h1;
    *h1 = cur;
    return (int16_t)clamp32(cur >> 6, -32768, 32767);
}

void gc_dtk_decode_block(const uint8_t blk[32], int16_t out[56], int32_t hist[4])
{
    for (int i = 0; i < 28; i++) {
        uint8_t b = blk[4 + i];
        out[2 * i] = dtk_sample(b & 15, blk[0], &hist[0], &hist[1]);
        out[2 * i + 1] = dtk_sample(b >> 4, blk[1], &hist[2], &hist[3]);
    }
}

/* ---- Resampler ---- */

void gc_resampler_init(gc_resampler *r, uint32_t in_rate, uint32_t out_rate)
{
    *r = (gc_resampler){.in_rate = in_rate ? in_rate : 1, .out_rate = out_rate ? out_rate : 1};
}

void gc_resampler_set_rate(gc_resampler *r, uint32_t in_rate)
{
    if (in_rate && in_rate != r->in_rate) {
        r->phase = (uint32_t)((uint64_t)r->phase * in_rate / r->in_rate) % r->out_rate;
        r->in_rate = in_rate;
    }
}

void gc_resampler_next(gc_resampler *r, int16_t out[2], gc_resampler_fetch fetch, void *ctx)
{
    r->phase += r->in_rate;
    while (r->phase >= r->out_rate) {
        r->phase -= r->out_rate;
        int16_t in[2];
        fetch(ctx, in);
        for (int k = 0; k < 3; k++) {
            r->hist[k][0] = r->hist[k + 1][0];
            r->hist[k][1] = r->hist[k + 1][1];
        }
        r->hist[3][0] = in[0];
        r->hist[3][1] = in[1];
    }
    /* Catmull-Rom between hist[1] and hist[2], t in 1/65536 (coefficients doubled to stay integral) */
    int64_t t = (int64_t)r->phase * 65536 / r->out_rate;
    for (int c = 0; c < 2; c++) {
        int64_t p0 = r->hist[0][c], p1 = r->hist[1][c], p2 = r->hist[2][c], p3 = r->hist[3][c];
        int64_t c1 = p2 - p0, c2 = 2 * p0 - 5 * p1 + 4 * p2 - p3, c3 = (p3 - p0) + 3 * (p1 - p2);
        int64_t v = c3 * t / 65536 + c2;
        v = v * t / 65536 + c1;
        v = v * t / 65536;
        out[c] = (int16_t)clamp32(p1 + v / 2, -32768, 32767);
    }
}

/* ---- The DSP accelerator ---- */

void gc_accel_set_yn2(gc_accel *a, int16_t v)
{
    a->yn2 = v;
    a->stopped = 0;
}

static unsigned accel_raw(const gc_accel *a)
{
    uint32_t c = a->cur;
    switch (a->format & 3) {
    case 0: { /* 4-bit: nibble address */
        uint8_t b = a->mem[(c >> 1) & a->mem_mask];
        return (c & 1) ? b & 15 : b >> 4;
    }
    case 1:
        return a->mem[c & a->mem_mask];
    case 2:
        return (unsigned)a->mem[(c * 2) & a->mem_mask] << 8 | a->mem[(c * 2 + 1) & a->mem_mask];
    default:
        return 0;
    }
}

int16_t gc_accel_read(gc_accel *a)
{
    if (a->stopped)
        return 0;
    unsigned decode = (a->format >> 2) & 3;
    int32_t raw = (decode == 1 || decode == 3) ? 0 : (int16_t)accel_raw(a); /* no ACIN input in HLE */
    unsigned ci = (a->pred_scale >> 4) & 7;
    int32_t c1 = a->coefs ? a->coefs[2 * ci] : 0, c2 = a->coefs ? a->coefs[2 * ci + 1] : 0;
    int16_t val = 0;
    unsigned step = 2;
    if (decode == 0) { /* ADPCM */
        raw &= 15;
        if (raw >= 8)
            raw -= 16;
        int64_t v = (int64_t)(1 << (a->pred_scale & 15)) * raw + ((0x400 + (int64_t)c1 * a->yn1 + (int64_t)c2 * a->yn2) >> 11);
        val = (int16_t)clamp32(v, -32768, 32767);
        a->yn2 = a->yn1;
        a->yn1 = val;
        a->cur++;
        /* Two end alignments loop without the exception and without a new header */
        if ((a->end & 15) == 0 && a->cur == a->end) {
            a->cur = a->start + 1;
        } else if ((a->end & 15) == 1 && a->cur == a->end - 1) {
            a->cur = a->start;
        } else if ((a->cur & 15) == 0) { /* a new frame: its header, then the first sample */
            a->pred_scale = a->mem[((a->cur & ~15u) >> 1) & a->mem_mask] & 0x7F;
            a->cur += 2;
            step += 2;
        }
    } else { /* PCM: gain / 2048, 1 or 65536, then the same predictor */
        static const unsigned shifts[4] = {11, 0, 16, 0};
        unsigned sh = shifts[(a->format >> 4) & 3];
        int64_t v = (((int64_t)a->gain * raw) >> sh) + (((int64_t)c1 * a->yn1) >> sh) + (((int64_t)c2 * a->yn2) >> sh);
        val = (int16_t)v;
        a->yn2 = a->yn1;
        a->yn1 = val;
        if (decode != 1)
            a->cur++;
    }
    if (a->cur == a->end + step - 1) { /* past the end: back to the start, reads stop until the loop is set up */
        a->cur = a->start;
        a->stopped = 1;
        a->ended = 1;
    }
    a->cur &= 0xBFFFFFFFu;
    return val;
}
