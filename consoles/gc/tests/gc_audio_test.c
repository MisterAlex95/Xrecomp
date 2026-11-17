/* gc_audio_test -- the GameCube sound without a toolchain or any game data (XBA-263).
 *
 *   gc_audio_test OUTDIR      (writes OUTDIR/ax.wav, OUTDIR/dtk.wav for audio_check.py; run_audio.sh runs both)
 *
 * Unit checks of the pure pieces against values worked out by hand from the formats: the drive's stream ADPCM
 * (filters, shifts, history), the DSP accelerator (AX ADPCM frames and headers, PCM8 / PCM16 gain, the end / loop
 * exception), the resampler (exact length, DC, identity, a 32 -> 48 kHz sine), the ucode hash; encoders of ours
 * round-trip a sine through both decoders.
 *
 * Then the machine, through its registers only (the DSP interface, the audio interface, the DVD interface):
 *   AX   the DSP reset and booted with an image of ours through the ROM's mail protocol (its hash is unknown: the
 *        log line, and AX as the fallback), DSP_INIT, then 400 frames of hand-written command lists (setup, PB
 *        list, process, output) for three voices: ADPCM (looped, 16 -> 32 kHz), PCM16 (a volume ramp), PCM8
 *        (8 -> 32 kHz), each list answered DSP_YIELD and continued; the 2 s it rendered played by the audio DMA
 *        (32 kHz, its interrupt, the blocks-left register) and recorded at 48 kHz -> ax.wav
 *   DTK  a disc image in memory with a stream of ours (the drive's ADPCM, 750 Hz left, 1250 Hz right), the DVD
 *        play command, the AI's play bit, volume 255 / 128, its sample counter and interrupt -> dtk.wav
 * Exit 0, 1 on a mismatch. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gc_audio.h"
#include "gc_di.h"
#include "gc_dsp_internal.h"

static int failures;

#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        if (!(cond)) {                                                                                                \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                               \
            printf(__VA_ARGS__);                                                                                      \
            printf("\n");                                                                                             \
            failures++;                                                                                               \
        }                                                                                                             \
    } while (0)

static int16_t sat(double v)
{
    return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : lrint(v));
}

static double snr_db(const int16_t *ref, const int16_t *got, int n, int stride)
{
    double s = 0, e = 0;
    for (int i = 0; i < n; i++) {
        double r = ref[i * stride], d = got[i * stride] - r;
        s += r * r;
        e += d * d;
    }
    return e > 0 ? 10 * log10(s / e) : 200;
}

/* ---- DTK: our model of the format, an encoder, hand-made blocks ---- */

static int16_t dtk_model(int nib, int hdr, int32_t *h1, int32_t *h2)
{
    static const int k1[4] = {0, 60, 115, 98}, k2[4] = {0, 0, -52, -55};
    int f = (hdr >> 4) & 3, sh = hdr & 15;
    int64_t p = ((int64_t)*h1 * k1[f] + (int64_t)*h2 * k2[f] + 32) >> 6;
    if (p < -0x200000)
        p = -0x200000;
    if (p > 0x1FFFFF)
        p = 0x1FFFFF;
    int32_t s = (nib >= 8 ? nib - 16 : nib) * 4096; /* the nibble at the top of a 16-bit word */
    s >>= sh;
    int32_t cur = s * 64 + (int32_t)p;
    *h2 = *h1;
    *h1 = cur;
    int32_t o = cur >> 6;
    return (int16_t)(o < -32768 ? -32768 : o > 32767 ? 32767 : o);
}

/* One channel of a block: the filter / shift that decodes closest, nibbles chosen greedily. */
static void dtk_encode_channel(const int16_t *x, int stride, int32_t *h, uint8_t *hdr_out, uint8_t nib[28])
{
    double best = 1e300;
    for (int f = 0; f < 4; f++)
        for (int sh = 0; sh <= 12; sh++) {
            int32_t h1 = h[0], h2 = h[1];
            uint8_t nb[28];
            double err = 0;
            for (int i = 0; i < 28; i++) {
                double tgt = x[i * stride];
                int bn = 0;
                double be = 1e300;
                for (int n = 0; n < 16; n++) { /* the nibble whose decode is closest */
                    int32_t a = h1, b = h2;
                    double d = dtk_model(n, f << 4 | sh, &a, &b) - tgt;
                    if (d * d < be) {
                        be = d * d;
                        bn = n;
                    }
                }
                nb[i] = (uint8_t)bn;
                err += be;
                dtk_model(bn, f << 4 | sh, &h1, &h2);
            }
            if (err < best) {
                best = err;
                *hdr_out = (uint8_t)(f << 4 | sh);
                memcpy(nib, nb, 28);
            }
        }
    int32_t h1 = h[0], h2 = h[1];
    for (int i = 0; i < 28; i++)
        dtk_model(nib[i], *hdr_out, &h1, &h2);
    h[0] = h1;
    h[1] = h2;
}

static void dtk_encode_block(const int16_t *lr, int32_t hist[4], uint8_t out[32])
{
    uint8_t nl[28], nr[28];
    memset(out, 0, 32);
    dtk_encode_channel(lr, 2, &hist[0], &out[0], nl);
    dtk_encode_channel(lr + 1, 2, &hist[2], &out[1], nr);
    out[2] = out[0];
    out[3] = out[1];
    for (int i = 0; i < 28; i++)
        out[4 + i] = (uint8_t)(nl[i] | nr[i] << 4);
}

static void test_dtk(void)
{
    /* by hand: filter 0 shift 0, nibble 1 -> 4096; shift 4 -> 256; then filter 1 with history 4096: 60/64 of it */
    uint8_t b[32] = {0x00, 0x00};
    int32_t h[4] = {0};
    int16_t o[56];
    b[4] = 0xF1; /* left 1, right -1 */
    gc_dtk_decode_block(b, o, h);
    CHECK(o[0] == 4096 && o[1] == -4096, "dtk filter 0: %d %d", o[0], o[1]);
    CHECK(o[2] == 0 && h[0] == 0 && h[1] == 0, "dtk zeros after: %d, history %d %d", o[2], h[0], h[1]);
    memset(h, 0, sizeof h);
    b[0] = 0x04;
    b[1] = 0x10;
    b[4] = 0x01;
    b[5] = 0x00;
    gc_dtk_decode_block(b, o, h);
    CHECK(o[0] == 256, "dtk shift 4: %d", o[0]);
    CHECK(o[1] == 0 && o[3] == 0, "dtk right silent: %d %d", o[1], o[3]);
    memset(h, 0, sizeof h);
    b[0] = 0x00;
    b[1] = 0x00;
    b[4] = 0x00;
    b[31] = 0x01;
    gc_dtk_decode_block(b, o, h); /* left: zeros, then 4096 last */
    CHECK(o[54] == 4096 && h[0] == 4096 * 64, "dtk last sample %d, history %d", o[54], h[0]);
    b[0] = 0x10;
    b[31] = 0x00;
    gc_dtk_decode_block(b, o, h); /* filter 1: 4096 * 60 / 64 = 3840, then 3600 */
    CHECK(o[0] == 3840 && o[2] == 3600, "dtk filter 1: %d %d", o[0], o[2]);
    b[0] = 0x20; /* filter 2 from (3600 .. ) history: model and decoder agree */
    b[4] = 0x37;
    int32_t hm[2] = {h[0], h[1]};
    gc_dtk_decode_block(b, o, h);
    int16_t want = dtk_model(7, 0x20, &hm[0], &hm[1]);
    CHECK(o[0] == want, "dtk filter 2: %d, model %d", o[0], want);

    /* a 1 kHz / 1.5 kHz stereo sine through our encoder and the decoder */
    enum { NB = 200 };
    static int16_t src[NB * 56], dec[NB * 56];
    for (int i = 0; i < NB * 28; i++) {
        src[2 * i] = sat(12000 * sin(2 * M_PI * 1000 * i / 48000.0));
        src[2 * i + 1] = sat(12000 * sin(2 * M_PI * 1500 * i / 48000.0));
    }
    int32_t he[4] = {0}, hd[4] = {0};
    for (int k = 0; k < NB; k++) {
        uint8_t blk[32];
        dtk_encode_block(src + 56 * k, he, blk);
        gc_dtk_decode_block(blk, dec + 56 * k, hd);
    }
    CHECK(memcmp(he, hd, sizeof he) == 0, "dtk encoder / decoder histories differ");
    double sl = snr_db(src, dec, NB * 28, 2), sr = snr_db(src + 1, dec + 1, NB * 28, 2);
    CHECK(sl > 30 && sr > 30, "dtk round trip SNR %.1f / %.1f dB", sl, sr);
    printf("dtk: by-hand blocks ok, round trip %.1f / %.1f dB\n", sl, sr);
}

/* ---- the accelerator: AX ADPCM, PCM ---- */

static const int16_t k_coefs[16] = {0, 0, 2048, 0, 4096, -2048, 3584, -1536, 3072, -1024, 3840, -1920, 1536, 0,
                                    2560, -512};

static int16_t adpcm_model(int nib, int ps, const int16_t *coefs, int16_t *yn1, int16_t *yn2)
{
    int c1 = coefs[2 * ((ps >> 4) & 7)], c2 = coefs[2 * ((ps >> 4) & 7) + 1];
    int64_t v = (int64_t)(1 << (ps & 15)) * (nib >= 8 ? nib - 16 : nib) + ((0x400 + (int64_t)c1 * *yn1 + (int64_t)c2 * *yn2) >> 11);
    int16_t o = (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
    *yn2 = *yn1;
    *yn1 = o;
    return o;
}

/* 14 samples -> one 8-byte frame (best predictor / scale, greedy nibbles), the history carried */
static void adpcm_encode_frame(const int16_t *x, int16_t *yn1, int16_t *yn2, uint8_t out[8])
{
    double best = 1e300;
    uint8_t bestf[8] = {0};
    for (int p = 0; p < 8; p++)
        for (int s = 0; s <= 11; s++) {
            int16_t a = *yn1, b = *yn2;
            uint8_t f[8] = {(uint8_t)(p << 4 | s)};
            double err = 0;
            for (int i = 0; i < 14; i++) {
                int bn = 0;
                double be = 1e300;
                for (int n = 0; n < 16; n++) {
                    int16_t ta = a, tb = b;
                    double d = adpcm_model(n, p << 4 | s, k_coefs, &ta, &tb) - x[i];
                    if (d * d < be) {
                        be = d * d;
                        bn = n;
                    }
                }
                adpcm_model(bn, p << 4 | s, k_coefs, &a, &b);
                err += be;
                f[1 + i / 2] |= (uint8_t)(i & 1 ? bn : bn << 4);
            }
            if (err < best) {
                best = err;
                memcpy(bestf, f, 8);
            }
        }
    memcpy(out, bestf, 8);
    for (int i = 0; i < 14; i++) {
        int n = (bestf[1 + i / 2] >> (i & 1 ? 0 : 4)) & 15;
        adpcm_model(n, bestf[0], k_coefs, yn1, yn2);
    }
}

static void test_accel(void)
{
    static uint8_t mem[64];
    memset(mem, 0, sizeof mem);
    /* frame 0: header 0x1B (predictor 1 = 1.0, scale 2048), samples 1 2 3 0 ... ; frame 1: header 0x02 */
    mem[0] = 0x1B;
    mem[1] = 0x12;
    mem[2] = 0x30;
    mem[8] = 0x02;
    mem[9] = 0x70;
    gc_accel a = {.mem = mem, .mem_mask = sizeof mem - 1, .start = 2, .end = 2 * 16 - 1, .cur = 2, .format = 0,
                  .pred_scale = 0x1B, .coefs = k_coefs};
    int16_t s1 = gc_accel_read(&a), s2 = gc_accel_read(&a), s3 = gc_accel_read(&a), s4 = gc_accel_read(&a);
    /* 2048*1 + 0; 2048*2 + 2048; 2048*3 + 6144; 0 + 12288 */
    CHECK(s1 == 2048 && s2 == 6144 && s3 == 12288 && s4 == 12288, "adpcm by hand: %d %d %d %d", s1, s2, s3, s4);
    for (int i = 4; i < 14; i++)
        gc_accel_read(&a);
    CHECK(a.cur == 18 && a.pred_scale == 0x02 && !a.ended, "adpcm next frame: cur %u ps %02X", a.cur, a.pred_scale);
    int16_t s15 = gc_accel_read(&a); /* 7 << 2 + ((0x400 + 0) >> 11): predictor 0 */
    CHECK(s15 == 28, "adpcm frame 2 sample: %d", s15);
    /* the end: a frame's last nibble -> back to the start, reads stop until yn2 is set */
    gc_accel e = {.mem = mem, .mem_mask = sizeof mem - 1, .start = 2, .end = 15, .cur = 14, .format = 0,
                  .pred_scale = 0x00, .coefs = k_coefs};
    gc_accel_read(&e);
    CHECK(!e.ended, "adpcm ended early");
    gc_accel_read(&e);
    CHECK(e.ended && e.stopped && e.cur == 2, "adpcm end: ended %d stopped %d cur %u", e.ended, e.stopped, e.cur);
    CHECK(gc_accel_read(&e) == 0, "reads after the end are 0");
    gc_accel_set_yn2(&e, 0);
    CHECK(!e.stopped, "yn2 restarts reads");

    /* PCM16: gain 0x800 / 2048 = 1; PCM8: gain 0x100, scale 1 -> the byte << 8, signed */
    uint8_t pcm[8] = {0x12, 0x34, 0xFF, 0xFE, 0x80, 0x7F, 0, 0};
    gc_accel p16 = {.mem = pcm, .mem_mask = 7, .start = 0, .end = 1, .cur = 0, .format = 0x0A, .gain = 0x800};
    int16_t a1 = gc_accel_read(&p16), a2 = gc_accel_read(&p16);
    CHECK(a1 == 0x1234 && a2 == -2 && p16.ended && p16.cur == 0, "pcm16: %d %d ended %d", a1, a2, p16.ended);
    gc_accel p8 = {.mem = pcm, .mem_mask = 7, .start = 4, .end = 5, .cur = 4, .format = 0x19, .gain = 0x100};
    int16_t b1 = gc_accel_read(&p8), b2 = gc_accel_read(&p8);
    CHECK(b1 == -32768 && b2 == 0x7F00 && p8.ended, "pcm8: %d %d", b1, b2);

    /* a 1 kHz sine at 32 kHz through our encoder and the accelerator */
    enum { NF = 300 };
    static uint8_t enc[NF * 8];
    static int16_t src[NF * 14], dec[NF * 14];
    for (int i = 0; i < NF * 14; i++)
        src[i] = sat(16000 * sin(2 * M_PI * 1000 * i / 32000.0));
    int16_t y1 = 0, y2 = 0;
    for (int f = 0; f < NF; f++)
        adpcm_encode_frame(src + 14 * f, &y1, &y2, enc + 8 * f);
    static uint8_t big[4096];
    memcpy(big, enc, sizeof enc);
    gc_accel r = {.mem = big, .mem_mask = sizeof big - 1, .start = 2, .end = 2 * 8 * NF - 1, .cur = 2, .format = 0,
                  .pred_scale = enc[0], .coefs = k_coefs};
    for (int i = 0; i < NF * 14; i++)
        dec[i] = gc_accel_read(&r);
    double snr = snr_db(src, dec, NF * 14, 1);
    CHECK(snr > 25 && r.ended, "adpcm round trip SNR %.1f dB, ended %d", snr, r.ended);
    printf("accelerator: by-hand frames ok, round trip %.1f dB\n", snr);
}

/* ---- resampler ---- */

typedef struct src_ctx {
    const int16_t *x;
    int n, at;
} src_ctx;

static void src_fetch(void *ctx, int16_t lr[2])
{
    src_ctx *c = ctx;
    int i = c->at < c->n ? c->at : c->n - 1;
    lr[0] = c->x[2 * i];
    lr[1] = c->x[2 * i + 1];
    c->at++;
}

static void test_resampler(void)
{
    enum { N = 32000 };
    static int16_t in[2 * (N + 16)], out[2 * 48000];
    for (int i = 0; i < N + 16; i++) {
        in[2 * i] = sat(16000 * sin(2 * M_PI * 1000 * i / 32000.0));
        in[2 * i + 1] = 1000; /* DC on the right */
    }
    gc_resampler r;
    gc_resampler_init(&r, 32000, 48000);
    src_ctx c = {in, N + 16, 0};
    for (int i = 0; i < 48000; i++)
        gc_resampler_next(&r, out + 2 * i, src_fetch, &c);
    CHECK(c.at == 32000, "32 -> 48 kHz: %d input frames for 48000 out", c.at);
    double e = 0, s = 0;
    int dc_bad = 0;
    for (int i = 100; i < 48000; i++) {
        /* output i sits at input position (i + 1) * 2/3 - 3 (between the 3rd and 2nd newest of what it read) */
        double want = 16000 * sin(2 * M_PI * 1000 * ((i + 1) * 2.0 / 3.0 - 3) / 32000.0);
        e += (out[2 * i] - want) * (out[2 * i] - want);
        s += want * want;
        dc_bad += out[2 * i + 1] != 1000;
    }
    double snr = 10 * log10(s / e);
    CHECK(snr > 40, "32 -> 48 kHz sine: %.1f dB", snr);
    CHECK(dc_bad == 0, "DC through the resampler: %d samples off", dc_bad);
    gc_resampler_init(&r, 48000, 48000);
    c = (src_ctx){in, N, 0};
    int same = 1;
    for (int i = 0; i < 1000; i++) {
        int16_t o[2];
        gc_resampler_next(&r, o, src_fetch, &c);
        if (i >= 2)
            same &= o[0] == in[2 * (i - 2)];
    }
    CHECK(same && c.at == 1000, "48 -> 48 kHz is the input two frames late");
    printf("resampler: 32 -> 48 kHz exact length, %.1f dB, DC and identity ok\n", snr);
}

static void test_hash(void)
{
    const uint8_t b[3] = {0x01, 0x02, 0x80};
    /* h = rotl(1, 3) = 8; rotl(8 ^ 2, 3) = 0x50; rotl(0x50 ^ 0x80, 3) = 0x680 */
    CHECK(gc_ucode_hash(b, 3) == 0x680u, "ucode hash %08X", gc_ucode_hash(b, 3));
}

/* ---- the machine ---- */

static gc_machine *machine(void)
{
    char why[100];
    gc_machine *m = gc_alloc_machine(why, sizeof why);
    if (!m) {
        printf("FAIL: %s\n", why);
        exit(1);
    }
    rc_put32(m->h.ram + 0x3000, 0x48000000u); /* b . (the CPU never runs here: time moves by advance()) */
    m->h.cpu.pc = 0x80003000u;
    return m;
}

/* Moves time on with the CPU stopped: audio / DSP / DVD events as they fall due. */
static void advance(gc_machine *m, uint64_t cycles)
{
    uint64_t end = m->h.now + cycles;
    while (m->h.now < end) {
        uint64_t t = gc_audio_next_event(m);
        if (m->di && gc_di_next_event(m) < t)
            t = gc_di_next_event(m);
        m->h.now = t < end ? t : end;
        if (m->h.now >= gc_audio_next_event(m))
            gc_audio_event(m);
        if (m->di && m->h.now >= gc_di_next_event(m))
            gc_di_event(m);
    }
}

static void w16(gc_machine *m, uint32_t off, uint16_t v)
{
    gc_hw_write(m, 0x0C000000u | off, v, 2);
}

static uint16_t r16(gc_machine *m, uint32_t off)
{
    return (uint16_t)gc_hw_read(m, 0x0C000000u | off, 2);
}

/* Acknowledges one of the DSP interface's interrupts the way libogc does (the other bits kept: clearing the init
 * bit would run the init code). */
static void csr_ack(gc_machine *m, uint16_t bit)
{
    w16(m, 0x500A, (uint16_t)((r16(m, 0x500A) & ~(DSPCR_AIDINT | DSPCR_ARINT | DSPCR_DSPINT)) | bit));
}

static void send_mail(gc_machine *m, uint32_t v)
{
    w16(m, 0x5000, (uint16_t)(v >> 16));
    w16(m, 0x5002, (uint16_t)v);
    CHECK(!(r16(m, 0x5000) & 0x8000), "the DSP did not take mail %08X", v);
}

static uint32_t read_mail(gc_machine *m)
{
    uint16_t hi = r16(m, 0x5004);
    if (!(hi & 0x8000))
        return 0;
    return (uint32_t)hi << 16 | r16(m, 0x5006);
}

static FILE *wav_open(const char *dir, const char *name)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "wb");
    if (!f) {
        printf("FAIL: cannot write %s\n", p);
        exit(1);
    }
    uint8_t z[44] = {0};
    fwrite(z, 1, 44, f);
    return f;
}

static void wav_close(FILE *f, uint32_t frames)
{
    uint8_t h[44] = "RIFF____WAVEfmt \x10\0\0\0\x01\0\x02\0________\x04\0\x10\0data____";
    uint32_t v[4] = {36 + frames * 4, 48000, 48000 * 4, frames * 4};
    int at[4] = {4, 24, 28, 40};
    for (int k = 0; k < 4; k++)
        for (int b = 0; b < 4; b++)
            h[at[k] + b] = (uint8_t)(v[k] >> (8 * b));
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
    fclose(f);
}

/* Advances `seconds`, writing what the machine plays. */
static uint32_t record(gc_machine *m, FILE *f, double seconds)
{
    uint32_t frames = 0;
    int steps = (int)(seconds * 1000);
    for (int i = 0; i < steps; i++) {
        advance(m, GC_CPU_HZ / 1000);
        int16_t pcm[2 * 256];
        size_t n;
        while ((n = gc_audio_pull(m, pcm, 256)) > 0) {
            for (size_t k = 0; k < 2 * n; k++) {
                uint8_t le[2] = {(uint8_t)pcm[k], (uint8_t)((uint16_t)pcm[k] >> 8)};
                fwrite(le, 1, 2, f);
            }
            frames += (uint32_t)n;
        }
    }
    return frames;
}

/* PB words (the layout with the low-pass filter) */
enum {
    PB_NEXT = 0, PB_SRC_TYPE = 4, PB_MIXER_CTRL = 6, PB_RUNNING = 7, PB_MIX_ML = 9, PB_MIX_MR = 11, PB_VE = 50,
    PB_LOOPING = 55, PB_FORMAT = 56, PB_LOOP_ADDR = 57, PB_END_ADDR = 59, PB_CUR_ADDR = 61, PB_COEFS = 63,
    PB_GAIN = 79, PB_PS = 80, PB_RATIO = 83, PB_LOOP_PS = 90, PB_LOOP_YN1 = 91, PB_LOOP_YN2 = 92, PB_WORDS = 122,
};

static void pb_put(gc_machine *m, uint32_t pb, int word, uint16_t v)
{
    m->h.ram[pb + 2 * word] = (uint8_t)(v >> 8);
    m->h.ram[pb + 2 * word + 1] = (uint8_t)v;
}

static uint16_t pb_get(gc_machine *m, uint32_t pb, int word)
{
    return (uint16_t)(m->h.ram[pb + 2 * word] << 8 | m->h.ram[pb + 2 * word + 1]);
}

static void pb_put32(gc_machine *m, uint32_t pb, int word, uint32_t v)
{
    pb_put(m, pb, word, (uint16_t)(v >> 16));
    pb_put(m, pb, word + 1, (uint16_t)v);
}

static void test_ax(const char *dir)
{
    gc_machine *m = machine();
    gc_audio_info info;
    /* the DSP's ROM after a reset: its greeting once the halt bit is cleared */
    w16(m, 0x500A, DSPCR_RES | DSPCR_HALT | DSPCR_INIT);
    CHECK(read_mail(m) == 0, "mail while halted");
    w16(m, 0x500A, DSPCR_INIT);
    uint32_t greet = read_mail(m);
    CHECK(greet == 0x8071FEEDu, "ROM greeting %08X", greet);
    /* our "ucode": 4 KB of noise at 0x00100000, booted the way libogc's DSP library does it */
    for (int i = 0; i < 4096; i++)
        m->h.ram[0x100000 + i] = (uint8_t)((uint32_t)(i + 1) * 2654435761u >> 24);
    uint32_t boot[] = {0x80F3A001u, 0x00100000u, 0x80F3C002u, 0, 0x80F3A002u, 4096, 0x80F3B002u, 0, 0x80F3D001u, 0x10};
    for (size_t i = 0; i < sizeof boot / sizeof boot[0]; i++)
        send_mail(m, boot[i]);
    gc_audio_get_info(m, &info);
    CHECK(info.ucodes_booted == 1 && info.unknown_ucodes == 1, "boot: %u booted, %u unknown", info.ucodes_booted,
          info.unknown_ucodes);
    CHECK(info.ucode_crc == gc_ucode_hash(m->h.ram + 0x100000, 4096), "ucode hash %08X", info.ucode_crc);
    CHECK(strstr(info.ucode, "ax") != NULL, "unknown ucode runs as %s", info.ucode);
    uint32_t init = read_mail(m);
    CHECK(init == DSP_MAIL_INIT, "DSP_INIT: %08X", init);
    advance(m, 1000);
    CHECK(r16(m, 0x500A) & DSPCR_DSPINT, "DSP_INIT raised no interrupt");
    csr_ack(m, DSPCR_DSPINT);
    CHECK(!(r16(m, 0x500A) & DSPCR_DSPINT), "DSPINT not cleared");

    /* samples in ARAM, through the ARAM DMA from MEM1 */
    const uint32_t A_ADPCM = 0x10000, A_PCM16 = 0x20000, A_PCM8 = 0x30000, STAGE = 0x00500000;
    /* ADPCM: 1000 Hz at 16 kHz, 112-sample period (8 frames, 7 cycles) encoded 10 times in a row; the loop is the
     * last copy with the history the one before left */
    enum { COPIES = 10, FR = 8 };
    int16_t per[112];
    for (int i = 0; i < 112; i++)
        per[i] = sat(16000 * sin(2 * M_PI * 1000 * i / 16000.0));
    uint8_t enc[COPIES * FR * 8];
    int16_t y1 = 0, y2 = 0, loop_y1 = 0, loop_y2 = 0;
    for (int c = 0; c < COPIES; c++) {
        if (c == COPIES - 1) {
            loop_y1 = y1;
            loop_y2 = y2;
        }
        for (int f = 0; f < FR; f++)
            adpcm_encode_frame(per + 14 * f, &y1, &y2, enc + 8 * (c * FR + f));
    }
    memcpy(m->h.ram + STAGE, enc, sizeof enc);
    for (int i = 0; i < 64; i++) { /* PCM16: 1500 Hz at 32 kHz, 3 periods */
        int16_t v = sat(16000 * sin(2 * M_PI * 1500 * i / 32000.0));
        m->h.ram[STAGE + 0x2000 + 2 * i] = (uint8_t)((uint16_t)v >> 8);
        m->h.ram[STAGE + 0x2000 + 2 * i + 1] = (uint8_t)v;
    }
    for (int i = 0; i < 16; i++) /* PCM8: 500 Hz at 8 kHz, one period, amplitude 62 */
        m->h.ram[STAGE + 0x3000 + i] = (uint8_t)(int8_t)lrint(62 * sin(2 * M_PI * i / 16.0));
    struct { uint32_t mm, ar, n; } dmas[3] = {{STAGE, A_ADPCM, sizeof enc}, {STAGE + 0x2000, A_PCM16, 128},
                                             {STAGE + 0x3000, A_PCM8, 32}};
    for (int k = 0; k < 3; k++) {
        uint32_t n = (dmas[k].n + 31) & ~31u;
        w16(m, 0x5020, (uint16_t)(dmas[k].mm >> 16));
        w16(m, 0x5022, (uint16_t)dmas[k].mm);
        w16(m, 0x5024, (uint16_t)(dmas[k].ar >> 16));
        w16(m, 0x5026, (uint16_t)dmas[k].ar);
        w16(m, 0x5028, (uint16_t)(n >> 16)); /* direction 0: MEM1 -> ARAM */
        w16(m, 0x502A, (uint16_t)n);
        CHECK(r16(m, 0x500A) & DSPCR_DMABUSY, "ARAM DMA not busy");
        advance(m, (n / 32) * 246 + 10);
        CHECK(!(r16(m, 0x500A) & DSPCR_DMABUSY) && (r16(m, 0x500A) & DSPCR_ARINT), "ARAM DMA not done");
        csr_ack(m, DSPCR_ARINT);
    }
    CHECK(memcmp(m->aram + A_ADPCM, enc, sizeof enc) == 0, "ARAM holds the ADPCM");

    /* three PBs */
    const uint32_t PBA = 0x00200000, PBB = 0x00200100, PBC = 0x00200200, SETUP = 0x00210000, LIST = 0x00211000,
                   SURR = 0x00212000, OUT = 0x00400000;
    memset(m->h.ram + PBA, 0, 0x300);
    pb_put32(m, PBA, PB_NEXT, PBB);
    pb_put32(m, PBB, PB_NEXT, PBC);
    uint32_t pbs[3] = {PBA, PBB, PBC};
    for (int k = 0; k < 3; k++) {
        pb_put(m, pbs[k], PB_SRC_TYPE, 1);
        pb_put(m, pbs[k], PB_VE, 0x7FFF);
        pb_put(m, pbs[k], PB_LOOPING, 1);
    }
    /* A: ADPCM, main L at 0.5, 16 -> 32 kHz */
    uint32_t base = 2 * A_ADPCM;
    pb_put(m, PBA, PB_MIXER_CTRL, 0x0001);
    pb_put(m, PBA, PB_RUNNING, 1);
    pb_put(m, PBA, PB_MIX_ML, 0x4000);
    pb_put(m, PBA, PB_FORMAT, 0);
    pb_put32(m, PBA, PB_LOOP_ADDR, base + 2 * 8 * FR * (COPIES - 1) + 2);
    pb_put32(m, PBA, PB_END_ADDR, base + 2 * 8 * FR * COPIES - 1);
    pb_put32(m, PBA, PB_CUR_ADDR, base + 2);
    for (int i = 0; i < 16; i++)
        pb_put(m, PBA, PB_COEFS + i, (uint16_t)k_coefs[i]);
    pb_put(m, PBA, PB_PS, enc[0]);
    pb_put32(m, PBA, PB_RATIO, 0x8000);
    pb_put(m, PBA, PB_LOOP_PS, enc[8 * FR * (COPIES - 1)]);
    pb_put(m, PBA, PB_LOOP_YN1, (uint16_t)loop_y1);
    pb_put(m, PBA, PB_LOOP_YN2, (uint16_t)loop_y2);
    /* B: PCM16, main R ramping up 2 per sample from 0 */
    pb_put(m, PBB, PB_MIXER_CTRL, 0x0002 | 0x0008);
    pb_put(m, PBB, PB_RUNNING, 1);
    pb_put(m, PBB, PB_MIX_MR, 0);
    pb_put(m, PBB, PB_MIX_MR + 1, 2);
    pb_put(m, PBB, PB_FORMAT, 0x0A);
    pb_put(m, PBB, PB_GAIN, 0x800);
    pb_put32(m, PBB, PB_LOOP_ADDR, A_PCM16 / 2);
    pb_put32(m, PBB, PB_END_ADDR, A_PCM16 / 2 + 63);
    pb_put32(m, PBB, PB_CUR_ADDR, A_PCM16 / 2);
    pb_put32(m, PBB, PB_RATIO, 0x10000);
    /* C: PCM8, main L and R at 0.375, 8 -> 32 kHz, from 1 s */
    pb_put(m, PBC, PB_MIXER_CTRL, 0x0003);
    pb_put(m, PBC, PB_MIX_ML, 0x3000);
    pb_put(m, PBC, PB_MIX_MR, 0x3000);
    pb_put(m, PBC, PB_FORMAT, 0x19);
    pb_put(m, PBC, PB_GAIN, 0x100);
    pb_put32(m, PBC, PB_LOOP_ADDR, A_PCM8);
    pb_put32(m, PBC, PB_END_ADDR, A_PCM8 + 15);
    pb_put32(m, PBC, PB_CUR_ADDR, A_PCM8);
    pb_put32(m, PBC, PB_RATIO, 0x4000);
    memset(m->h.ram + SETUP, 0, 64);

    enum { FRAMES = 400 };
    for (int f = 0; f < FRAMES; f++) {
        if (f == 75) { /* the ramp has reached 24000 (12000 samples * 2): it holds there */
            CHECK(pb_get(m, PBB, PB_MIX_MR) == 24000, "ramp at %u", pb_get(m, PBB, PB_MIX_MR));
            pb_put(m, PBB, PB_MIX_MR + 1, 0);
        }
        if (f == 200) {
            pb_put(m, PBA, PB_RUNNING, 0);
            pb_put(m, PBB, PB_RUNNING, 0);
            pb_put(m, PBC, PB_RUNNING, 1);
        }
        if (f == 300)
            pb_put(m, PBC, PB_RUNNING, 0);
        uint32_t lr = OUT + 640u * (uint32_t)f;
        uint16_t list[] = {0x00, (uint16_t)(SETUP >> 16), (uint16_t)SETUP, 0x02, (uint16_t)(PBA >> 16), (uint16_t)PBA,
                           0x03, 0x0E, (uint16_t)(SURR >> 16), (uint16_t)SURR, (uint16_t)(lr >> 16), (uint16_t)lr,
                           0x0F};
        for (size_t i = 0; i < sizeof list / sizeof list[0]; i++) {
            m->h.ram[LIST + 2 * i] = (uint8_t)(list[i] >> 8);
            m->h.ram[LIST + 2 * i + 1] = (uint8_t)list[i];
        }
        send_mail(m, 0xBABE0000u | (uint32_t)(sizeof list / sizeof list[0]));
        send_mail(m, LIST);
        uint32_t y = read_mail(m);
        if (y != DSP_MAIL_YIELD) {
            CHECK(0, "frame %d: %08X instead of DSP_YIELD", f, y);
            break;
        }
        send_mail(m, CPU_MAIL_CONTINUE);
    }
    gc_audio_get_info(m, &info);
    CHECK(info.cmd_lists == FRAMES, "%llu command lists", (unsigned long long)info.cmd_lists);
    CHECK(pb_get(m, PBA, PB_RUNNING) == 0 && pb_get(m, PBC, PB_RUNNING) == 0, "voices still running");

    /* the 2 s it rendered, played by the audio DMA at 32 kHz (AICR's DMA rate bit as at power-on) */
    uint32_t blocks = FRAMES * 640 / 32;
    w16(m, 0x5030, (uint16_t)(OUT >> 16));
    w16(m, 0x5032, (uint16_t)OUT);
    w16(m, 0x5036, (uint16_t)(0x8000 | blocks));
    advance(m, 199);
    CHECK(!(r16(m, 0x500A) & DSPCR_AIDINT), "audio DMA interrupt early");
    advance(m, 2);
    CHECK(r16(m, 0x500A) & DSPCR_AIDINT, "no audio DMA interrupt when the buffer is latched");
    csr_ack(m, DSPCR_AIDINT);
    w16(m, 0x5036, 0); /* stops after this buffer? no: off now; then on again, from the start */
    gc_audio_pull(m, NULL, 0);
    w16(m, 0x5036, (uint16_t)(0x8000 | blocks));
    while (gc_audio_queued(m)) { /* drop what came before */
        int16_t tmp[512];
        gc_audio_pull(m, tmp, 256);
    }
    FILE *f = wav_open(dir, "ax.wav");
    uint32_t frames = record(m, f, 0.5);
    uint16_t left = r16(m, 0x503A);
    CHECK(left > 0 && left < blocks, "blocks left %u of %u", left, blocks);
    frames += record(m, f, 1.6);
    wav_close(f, frames);
    gc_audio_get_info(m, &info);
    printf("ax: %llu command lists, %llu DMA blocks, %u frames recorded, blocks left at 0.5 s: %u\n",
           (unsigned long long)info.cmd_lists, (unsigned long long)info.dma_blocks, frames, left);
    gc_destroy(m);
}

static void di_cmd(gc_machine *m, uint32_t c0, uint32_t c1, uint32_t c2)
{
    gc_hw_write(m, 0x0C006008, c0, 4);
    gc_hw_write(m, 0x0C00600C, c1, 4);
    gc_hw_write(m, 0x0C006010, c2, 4);
    gc_hw_write(m, 0x0C00601C, 1, 4);
    advance(m, GC_CPU_HZ / 1000);
    CHECK(!(gc_hw_read(m, 0x0C00601C, 4) & 1), "DVD command %08X still busy", c0);
    gc_hw_write(m, 0x0C006000, 0x54, 4);
}

static void test_dtk_machine(const char *dir)
{
    /* a "disc": a header with the GameCube magic, then 1 s of stream at 0x100000 */
    enum { SOFF = 0x100000, NBLK = 48000 / 28 + 1 };
    size_t size = SOFF + NBLK * 32;
    uint8_t *disc = calloc(1, size);
    memcpy(disc, "GXXE01", 6);
    disc[0x1C] = 0xC2, disc[0x1D] = 0x33, disc[0x1E] = 0x9F, disc[0x1F] = 0x3D;
    disc[8] = 1; /* audio streaming */
    int32_t hist[4] = {0};
    int16_t lr[56];
    for (int b = 0; b < NBLK; b++) {
        for (int i = 0; i < 28; i++) {
            int n = b * 28 + i;
            lr[2 * i] = sat(12000 * sin(2 * M_PI * 750 * n / 48000.0));
            lr[2 * i + 1] = sat(12000 * sin(2 * M_PI * 1250 * n / 48000.0));
        }
        dtk_encode_block(lr, hist, disc + SOFF + 32 * b);
    }
    char why[100];
    gc_disc *d = gc_disc_open_mem(disc, size, why, sizeof why);
    CHECK(d != NULL, "stream disc: %s", why);
    if (!d) {
        free(disc);
        return;
    }
    gc_machine *m = machine();
    gc_disc_insert(m, d);
    advance(m, (uint64_t)GC_CPU_HZ * 6 / 10); /* the cover closes */
    di_cmd(m, 0xE1000000u, SOFF >> 2, NBLK * 32);
    di_cmd(m, 0xE2000000u, 0, 0);
    CHECK(gc_hw_read(m, 0x0C006020, 4) == 1, "stream status: not playing");
    /* the AI: volume L 255 R 128, the counter's interrupt at 24000 samples, play at 48 kHz */
    gc_hw_write(m, 0x0C006C04, 0x80FF, 4);
    gc_hw_write(m, 0x0C006C0C, 24000, 4);
    while (gc_audio_queued(m)) {
        int16_t tmp[512];
        gc_audio_pull(m, tmp, 256);
    }
    gc_hw_write(m, 0x0C006C00, 0x40 | 0x20 | 0x04 | 0x02 | 0x01, 4); /* DMA 32k, reset, mask, stream 48k, play */
    FILE *f = wav_open(dir, "dtk.wav");
    uint32_t frames = record(m, f, 0.49);
    uint32_t cnt = gc_hw_read(m, 0x0C006C08, 4);
    CHECK(cnt >= 23500 && cnt <= 23600, "sample counter %u at 0.49 s", cnt);
    CHECK(!(gc_hw_read(m, 0x0C006C00, 4) & 0x08), "AI interrupt early");
    frames += record(m, f, 0.02);
    CHECK(gc_hw_read(m, 0x0C006C00, 4) & 0x08, "no AI interrupt at the counter's match");
    CHECK(m->pi_intsr & GC_INT_AI, "AI interrupt not at the PI");
    di_cmd(m, 0xE2010000u, 0, 0); /* position: about half the stream */
    uint32_t pos = gc_hw_read(m, 0x0C006020, 4) << 2;
    CHECK(pos > SOFF + NBLK * 32 / 2 - 2048 && pos < SOFF + NBLK * 32 / 2 + 2048, "stream position %X", pos);
    frames += record(m, f, 0.49);
    wav_close(f, frames);
    gc_audio_info info;
    gc_audio_get_info(m, &info);
    /* stop at the track's end: it plays out, then stops */
    di_cmd(m, 0xE1000000u, 0, 0);
    advance(m, (uint64_t)GC_CPU_HZ * 11 / 10);
    di_cmd(m, 0xE2000000u, 0, 0);
    CHECK(gc_hw_read(m, 0x0C006020, 4) == 0, "the stream did not stop at its end");
    printf("dtk: %llu stream frames, counter %u at 0.49 s, %u frames recorded\n",
           (unsigned long long)info.stream_frames, cnt, frames);
    gc_destroy(m);
    free(disc);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    test_hash();
    test_dtk();
    test_accel();
    test_resampler();
    test_ax(dir);
    test_dtk_machine(dir);
    printf("%s\n", failures ? "gc_audio_test: FAILED" : "gc_audio_test: ok");
    return failures ? 1 : 0;
}
