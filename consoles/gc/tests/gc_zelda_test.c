/* gc_zelda_test -- the Zelda / JAudio microcode family and libogc's AESND at a high level, with no game data and no
 * toolchain (XBA-266).
 *
 *   gc_zelda_test OUTDIR      (writes OUTDIR/zelda.wav for audio_check.py; run_audio.sh zelda runs both)
 *
 * The ucodes are recognised by the hash of the image a game uploads; we have no Nintendo image and want none, so each
 * run boots 2 KB of our own bytes whose last 32 bytes are chosen so the hash is the version's (the hash is xor and
 * rotate: linear, one free bit per byte covers all 32). Everything then goes through the DSP interface's registers:
 *
 *   protocol   the standard greeting (DSP_INIT, 0xF3551111), command words with their counts, every acknowledgement
 *              (DSP_SYNC then 0xF355 | sync), voices released by sync mails per 16 voices (a voice whose bit is
 *              clear is left alone), per frame on Super Mario Sunshine's version (two masks per mail), each frame's
 *              0xFF00 | frame and DSP_FRAME_END, commands held until 0xCDD10003, a crashing command halting it; the
 *              light protocol (Pikmin's greeting 0x88881111, fixed word counts, handler-address acknowledgements
 *              without interrupt, one zero mail renders everything and raises an interrupt with no mail) and the NTSC
 *              IPL's 0x80-word voice blocks; a switch to another ucode and back (DSP_RESUME, state kept)
 *   exact      voices read back sample by sample against our own models of the formats: AFC 4-bit and 2-bit (our
 *              encoder; a loop point inside a block, a short last block), PCM16 / PCM8 from ARAM with loops, PCM16
 *              from main memory, the square / 25 % square / saw synths, a pattern voice, the constant voice; the
 *              reverb ring (the echo one ring later, filtered, then its own echo at half)
 *   zelda.wav  2 s of hand-built voices rendered 4 frames per command at 32 kHz, interleaved and played through the
 *              audio DMA as JAudio does: PCM16 1000 Hz left + AFC 500 Hz (16 -> 32 kHz) right; then both fade out
 *              (end requested) and a PCM8 750 Hz (8 -> 32 kHz) plays on both; then a positional voice front left at
 *              1000 Hz and a main-memory PCM16 1250 Hz right that ends by itself; then silence
 *   state      a save state taken in the middle of a render (a frame done, its acknowledgement in the mailbox, the
 *              audio DMA playing): the next 0.5 s of rendering and 48 kHz output identical after loading it; a
 *              damaged DSP section refused
 *   aesnd      the mails prefixed 0xFACE: voice block address, first / next voice, send samples, terminate; mono16,
 *              stereo8 unsigned at half rate, the delay, the "finished" flag, the 2012+ channel order
 * Exit 0, 1 on a mismatch. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gc_audio.h"
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

static int16_t sat64(int64_t v)
{
    return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

/* ---- the machine, through its registers ---- */

static gc_machine *machine(void)
{
    char why[100];
    gc_machine *m = gc_alloc_machine(why, sizeof why);
    if (!m) {
        printf("FAIL: %s\n", why);
        exit(1);
    }
    rc_put32(m->h.ram + 0x3000, 0x48000000u); /* b . (the CPU never runs: time moves by advance()) */
    m->h.cpu.pc = 0x80003000u;
    return m;
}

static void advance(gc_machine *m, uint64_t cycles)
{
    uint64_t end = m->h.now + cycles;
    while (m->h.now < end) {
        uint64_t t = gc_audio_next_event(m);
        m->h.now = t < end ? t : end;
        if (m->h.now >= gc_audio_next_event(m))
            gc_audio_event(m);
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

static void csr_ack(gc_machine *m, uint16_t bit)
{
    w16(m, 0x500A, (uint16_t)((r16(m, 0x500A) & ~(DSPCR_AIDINT | DSPCR_ARINT | DSPCR_DSPINT)) | bit));
}

static void send_mail(gc_machine *m, uint32_t v)
{
    w16(m, 0x5000, (uint16_t)(v >> 16));
    w16(m, 0x5002, (uint16_t)v);
}

static uint32_t read_mail(gc_machine *m)
{
    uint16_t hi = r16(m, 0x5004);
    if (!(hi & 0x8000))
        return 0;
    return (uint32_t)hi << 16 | r16(m, 0x5006);
}

static void expect_mail(gc_machine *m, uint32_t want, const char *what)
{
    uint32_t got = read_mail(m);
    CHECK(got == want, "%s: mail %08X, expected %08X", what, got, want);
}

static void expect_none(gc_machine *m, const char *what)
{
    uint32_t got = read_mail(m);
    CHECK(got == 0, "%s: unexpected mail %08X", what, got);
}

/* The DSP interrupt came (some time after the mails), cleared the way libogc does. */
static void expect_int(gc_machine *m, const char *what)
{
    advance(m, 5000);
    CHECK(r16(m, 0x500A) & DSPCR_DSPINT, "%s: no DSP interrupt", what);
    csr_ack(m, DSPCR_DSPINT);
}

static void put16(gc_machine *m, uint32_t a, uint16_t v)
{
    m->h.ram[a] = (uint8_t)(v >> 8);
    m->h.ram[a + 1] = (uint8_t)v;
}

static uint16_t get16(gc_machine *m, uint32_t a)
{
    return (uint16_t)(m->h.ram[a] << 8 | m->h.ram[a + 1]);
}

static void put32(gc_machine *m, uint32_t a, uint32_t v)
{
    put16(m, a, (uint16_t)(v >> 16));
    put16(m, a + 2, (uint16_t)v);
}

static uint32_t get32(gc_machine *m, uint32_t a)
{
    return (uint32_t)get16(m, a) << 16 | get16(m, a + 2);
}

/* ---- images of ours with a chosen hash ---- */

enum { IMG_SIZE = 2048 };

/* Fills img with our own bytes whose gc_ucode_hash is target: after the prefix, 32 bytes of 0 / 1; byte i's bit 0
 * ends up at bit 3 * (32 - i) mod 32 of the hash, and 32 rotations by 3 bring the prefix's hash back where it was. */
static void forge(uint8_t *img, uint32_t target, uint32_t seed)
{
    for (int i = 0; i < IMG_SIZE - 32; i++)
        img[i] = (uint8_t)(((uint32_t)i + seed) * 2654435761u >> 24);
    uint32_t need = target ^ gc_ucode_hash(img, IMG_SIZE - 32);
    for (int i = 0; i < 32; i++)
        img[IMG_SIZE - 32 + i] = (uint8_t)((need >> ((3 * (32 - i)) % 32)) & 1);
}

static void check_ucode(gc_machine *m, uint32_t crc, const char *kind)
{
    gc_audio_info info;
    gc_audio_get_info(m, &info);
    CHECK(info.ucode_crc == crc && strcmp(info.ucode, kind) == 0, "running %08X %s, expected %08X %s",
          info.ucode_crc, info.ucode, crc, kind);
}

/* The DSP reset and booted with our image at `at` through the ROM's protocol (as libogc's DSP library does). */
static void boot(gc_machine *m, uint32_t crc, const char *kind)
{
    const uint32_t at = 0x00100000u;
    forge(m->h.ram + at, crc, crc);
    CHECK(gc_ucode_hash(m->h.ram + at, IMG_SIZE) == crc, "forged hash %08X", gc_ucode_hash(m->h.ram + at, IMG_SIZE));
    w16(m, 0x500A, DSPCR_RES | DSPCR_HALT | DSPCR_INIT);
    w16(m, 0x500A, DSPCR_INIT);
    expect_mail(m, 0x8071FEEDu, "ROM greeting");
    uint32_t mails[] = {0x80F3A001u, at, 0x80F3C002u, 0, 0x80F3A002u, IMG_SIZE, 0x80F3B002u, 0, 0x80F3D001u, 0x10};
    for (size_t i = 0; i < sizeof mails / sizeof mails[0]; i++)
        send_mail(m, mails[i]);
    check_ucode(m, crc, kind);
}

/* A running ucode told 0xCDD10001 and the next one's ten parameters (its image at `at`, ours, hashed to crc). */
static void switch_to(gc_machine *m, uint32_t crc, uint32_t at)
{
    forge(m->h.ram + at, crc, crc ^ 0x5555u);
    uint32_t mails[] = {CPU_MAIL_NEW_UCODE, 0x80000000u, 0x2000, 0, at, IMG_SIZE, 0, 0x10, 0, 0, 0};
    for (size_t i = 0; i < sizeof mails / sizeof mails[0]; i++)
        send_mail(m, mails[i]);
}

/* ---- the Zelda ucode's data: our tables ---- */

enum {
    TABLES = 0x00200000, AFC_COEFS = 0x00201000, RPBS = 0x00201100, VPBS = 0x00210000, OUT_L = 0x00300000,
    OUT_R = 0x00301000, RING = 0x00302000, MRAM_PCM = 0x00303000, DMA_OUT = 0x00400000,
};

/* VPB words (the full layout) */
enum {
    V_ENABLED = 0x00, V_DONE = 0x01, V_RATIO = 0x02, V_RESET = 0x04, V_USE_CONST = 0x06, V_CH = 0x08,
    V_DOLBY_POS = 0x28, V_DOLBY_REVERB = 0x29, V_DOLBY_CUR = 0x2A, V_DOLBY_TARGET = 0x2B, V_USE_DOLBY = 0x2C,
    V_FRAC = 0x30, V_CONST = 0x33, V_POS = 0x34, V_REMAINING = 0x3A, V_HIST = 0x3C, V_SOURCE = 0x80,
    V_LOOPING = 0x81, V_LOOP_YN1 = 0x82, V_LOOP_YN2 = 0x83, V_END_REQ = 0x85, V_LOOP_ADDR = 0x88, V_LOOP_START = 0x8A,
    V_BASE = 0x8C,
};
enum { ID_FL = 0x0D00, ID_FR = 0x0D60, ID_FL_REV = 0x0E80 };

static const int16_t k_afc[32] = {0, 0, 2048, 0, 0, 2048, 1024, 1024, 4096, -2048, 3584, -1536, 3072, -1024,
                                  4608, -2560, 4000, -2100, 3800, -1800, 2048, -1024, 1536, -512, 1024, 0,
                                  -1024, 0, 512, 512, 3500, -1900};
static int16_t g_pattern0[64];

static void tables(gc_machine *m)
{
    for (int ph = 0; ph < 64; ph++) { /* Catmull-Rom between taps 1 and 2 at t = phase / 64, x 0x7FFF */
        double t = ph / 64.0, w[4] = {(-t * t * t + 2 * t * t - t) / 2, (3 * t * t * t - 5 * t * t + 2) / 2,
                                      (-3 * t * t * t + 4 * t * t + t) / 2, (t * t * t - t * t) / 2};
        for (int k = 0; k < 4; k++)
            put16(m, TABLES + 2u * (uint32_t)(4 * ph + k), (uint16_t)sat(w[k] * 32767));
    }
    for (int i = 0; i < 64; i++) {
        g_pattern0[i] = sat(12000 * sin(2 * M_PI * i / 64.0));
        put16(m, TABLES + 0x200 + 2u * (uint32_t)i, (uint16_t)g_pattern0[i]);
        put16(m, TABLES + 0x280 + 2u * (uint32_t)i, (uint16_t)(i < 32 ? i * 700 : (64 - i) * 700));
        put16(m, TABLES + 0x300 + 2u * (uint32_t)i, (uint16_t)(i * 2654435761u >> 16));
        put16(m, TABLES + 0x380 + 2u * (uint32_t)i, (uint16_t)(i * 40503u));
    }
    for (int i = 0; i < 0x80; i++) /* panning: full at 0, nothing at 0x7F */
        put16(m, TABLES + 0x400 + 2u * (uint32_t)i, (uint16_t)sat(32767 * cos(i / 127.0 * M_PI / 2)));
    for (int i = 0; i < 32; i++)
        put16(m, AFC_COEFS + 2u * (uint32_t)i, (uint16_t)k_afc[i]);
    memset(m->h.ram + RPBS, 0, 4 * 32);
}

static uint32_t g_vpb_bytes = 0x180;

static void vput(gc_machine *m, int v, int word, uint16_t x)
{
    put16(m, VPBS + g_vpb_bytes * (uint32_t)v + 2u * (uint32_t)word, x);
}

static void vput32(gc_machine *m, int v, int word, uint32_t x)
{
    vput(m, v, word, (uint16_t)(x >> 16));
    vput(m, v, word + 1, (uint16_t)x);
}

static uint16_t vget(gc_machine *m, int v, int word)
{
    return get16(m, VPBS + g_vpb_bytes * (uint32_t)v + 2u * (uint32_t)word);
}

/* one mixing destination: buffer id, target, current */
static void vchan(gc_machine *m, int v, int c, uint16_t id, int16_t target, int16_t cur)
{
    vput(m, v, V_CH + 4 * c, id);
    vput(m, v, V_CH + 4 * c + 1, (uint16_t)target);
    vput(m, v, V_CH + 4 * c + 2, (uint16_t)cur);
}

/* standard protocol: the word count, then the words */
static void zsend(gc_machine *m, const uint32_t *w, unsigned n)
{
    send_mail(m, n);
    for (unsigned i = 0; i < n; i++)
        send_mail(m, w[i]);
}

static void zack(gc_machine *m, uint16_t sync, const char *what)
{
    expect_mail(m, DSP_MAIL_SYNC, what);
    expect_mail(m, 0xF3550000u | sync, what);
}

static void zsetup(gc_machine *m, unsigned voices)
{
    uint32_t w[5] = {0x81000000u | voices, VPBS, TABLES, AFC_COEFS, RPBS};
    zsend(m, w, 5);
    zack(m, 0x8100, "setup");
    expect_int(m, "setup");
}

/* One render command of `frames` frames, voices released per 16 (mask per group) or per frame (four masks). */
static void zrender(gc_machine *m, unsigned frames, uint16_t vol, const uint16_t *masks, unsigned groups,
                    bool per_frame, uint32_t l, uint32_t r)
{
    uint32_t w[3] = {0x82000000u | frames << 16 | vol, l, r};
    zsend(m, w, 3);
    expect_none(m, "render before any sync");
    for (unsigned f = 0; f < frames; f++) {
        if (per_frame) {
            send_mail(m, 0);
            send_mail(m, (uint32_t)masks[0] << 16 | masks[1]);
            if (f == 0)
                expect_none(m, "first half of a frame's sync");
            send_mail(m, (uint32_t)masks[2] << 16 | masks[3]);
        } else {
            for (unsigned g = 0; g < groups; g++) {
                send_mail(m, 0);
                send_mail(m, g << 16 | masks[g]);
            }
        }
        zack(m, (uint16_t)(0xFF00 | f), "frame");
    }
    expect_mail(m, DSP_MAIL_FRAME_END, "render end");
    expect_int(m, "render");
}

static void zcontinue(gc_machine *m)
{
    send_mail(m, CPU_MAIL_CONTINUE);
}

/* ---- our models of the sources ---- */

static int16_t afc_model(const uint8_t *blk, bool lq, int i, int16_t *y1, int16_t *y2)
{
    int step = 1 << (blk[0] >> 4), p = blk[0] & 15, code;
    if (lq) {
        int two = (blk[1 + i / 4] >> (6 - 2 * (i % 4))) & 3;
        code = (two >= 2 ? two - 4 : two) * 8192;
    } else {
        int nib = (i & 1) ? blk[1 + i / 2] & 15 : blk[1 + i / 2] >> 4;
        code = (nib >= 8 ? nib - 16 : nib) * 2048;
    }
    int16_t s = sat64(((int64_t)step * code + (int64_t)*y1 * k_afc[2 * p] + (int64_t)*y2 * k_afc[2 * p + 1]) >> 11);
    *y2 = *y1;
    *y1 = s;
    return s;
}

static void afc_set_code(uint8_t *blk, bool lq, int i, int c)
{
    if (lq) {
        int sh = 6 - 2 * (i % 4);
        blk[1 + i / 4] = (uint8_t)((blk[1 + i / 4] & ~(3 << sh)) | c << sh);
    } else {
        int sh = (i & 1) ? 0 : 4;
        blk[1 + i / 2] = (uint8_t)((blk[1 + i / 2] & ~(15 << sh)) | c << sh);
    }
}

/* One block for x[16]: every scale and predictor, codes chosen greedily, the closest kept. */
static void afc_encode(const int16_t *x, bool lq, int16_t *y1, int16_t *y2, uint8_t *out)
{
    int bytes = lq ? 5 : 9, codes = lq ? 4 : 16;
    double best = 1e300;
    uint8_t blk[9], keep[9];
    for (int sc = 0; sc < 16; sc++)
        for (int p = 0; p < 16; p++) {
            memset(blk, 0, sizeof blk);
            blk[0] = (uint8_t)(sc << 4 | p);
            int16_t a = *y1, b = *y2;
            double err = 0;
            for (int i = 0; i < 16; i++) {
                double be = 1e300;
                int bc = 0;
                for (int c = 0; c < codes; c++) {
                    afc_set_code(blk, lq, i, c);
                    int16_t ta = a, tb = b;
                    double e = afc_model(blk, lq, i, &ta, &tb) - x[i];
                    if (e * e < be) {
                        be = e * e;
                        bc = c;
                    }
                }
                afc_set_code(blk, lq, i, bc);
                afc_model(blk, lq, i, &a, &b);
                err += be;
            }
            if (err < best) {
                best = err;
                memcpy(keep, blk, sizeof keep);
            }
        }
    memcpy(out, keep, (size_t)bytes);
    for (int i = 0; i < 16; i++)
        afc_model(out, lq, i, y1, y2);
}

/* Encodes n samples (a multiple of 16) into ARAM at `at`; dec gets what decodes, hist[b] the history before block b. */
static void afc_stream(gc_machine *m, uint32_t at, const int16_t *x, int n, bool lq, int16_t *dec, int16_t (*hist)[2])
{
    int16_t y1 = 0, y2 = 0;
    int bytes = lq ? 5 : 9;
    for (int b = 0; b < n / 16; b++) {
        const uint8_t *blk = m->aram + at + (uint32_t)(bytes * b);
        hist[b][0] = y1;
        hist[b][1] = y2;
        afc_encode(x + 16 * b, lq, &y1, &y2, m->aram + at + (uint32_t)(bytes * b));
        int16_t a = hist[b][0], c = hist[b][1];
        for (int i = 0; i < 16; i++)
            dec[16 * b + i] = afc_model(blk, lq, i, &a, &c);
    }
}

/* What a sample becomes on the way out: a destination at constant volume cur (no ramp), output volume 4.12. */
static int16_t out_model(int16_t s, int16_t cur, uint16_t outvol)
{
    int16_t mixed = sat64(((int64_t)cur * s) >> 16);
    return sat64(((int64_t)mixed * outvol) >> 12);
}

/* ---- protocol + exact checks: The Wind Waker's version ---- */

static void exact_compare(gc_machine *m, uint32_t buf, const int16_t *want, int n, const char *what)
{
    int bad = 0, first = -1;
    for (int i = 0; i < n; i++)
        if ((int16_t)get16(m, buf + 2u * (uint32_t)i) != want[i]) {
            if (first < 0)
                first = i;
            bad++;
        }
    CHECK(bad == 0, "%s: %d of %d samples differ, first at %d: %d instead of %d", what, bad, n, first,
          first < 0 ? 0 : (int16_t)get16(m, buf + 2u * (uint32_t)first), first < 0 ? 0 : want[first]);
    if (!bad)
        printf("zelda: %s: %d samples exact\n", what, n);
}

/* A voice at 4:1 (nearest raw sample: output k is the stream's sample 4k), one destination at 0x7FFF. */
static void exact_voice(gc_machine *m, int v, unsigned src, uint32_t base, uint32_t len, uint32_t loop, uint16_t id)
{
    memset(m->h.ram + VPBS + g_vpb_bytes * (uint32_t)v, 0, g_vpb_bytes);
    vput(m, v, V_ENABLED, 1);
    vput(m, v, V_RESET, 1);
    vput(m, v, V_RATIO, 0x4000);
    vchan(m, v, 0, id, 0x7FFF, 0x7FFF);
    vput(m, v, V_SOURCE, (uint16_t)src);
    vput(m, v, V_LOOPING, 1);
    vput32(m, v, V_LOOP_ADDR, loop);
    vput32(m, v, V_LOOP_START, len);
    vput32(m, v, V_BASE, base);
}

static void test_exact(void)
{
    gc_machine *m = machine();
    boot(m, 0x86840740u, "zelda");
    expect_mail(m, DSP_MAIL_INIT, "greeting");
    expect_mail(m, 0xF3551111u, "greeting");
    expect_int(m, "greeting");
    tables(m);
    zsetup(m, 16);
    enum { FRAMES = 10, OUTS = FRAMES * 80, LEN = 1000, LOOP = 520 };
    static int16_t want_l[OUTS], want_r[OUTS];

    /* AFC 4-bit left, 2-bit right: 1008 samples of ours (two tones and a swell), played to 1000, looped from 520 */
    static int16_t x[1008], dec_hq[1008], dec_lq[1008], hist_hq[63][2], hist_lq[63][2];
    for (int i = 0; i < 1008; i++)
        x[i] = sat((6000 + 4000 * sin(i / 150.0)) * sin(2 * M_PI * i / 37.0) + 3000 * sin(2 * M_PI * i / 11.3));
    const uint32_t A_HQ = 0x10000, A_LQ = 0x20000, A_P16 = 0x30000, A_P8 = 0x40000;
    afc_stream(m, A_HQ, x, 1008, false, dec_hq, hist_hq);
    afc_stream(m, A_LQ, x, 1008, true, dec_lq, hist_lq);
    exact_voice(m, 0, 9, A_HQ, LEN, LOOP, ID_FL);
    vput(m, 0, V_LOOP_YN1, (uint16_t)hist_hq[LOOP / 16][0]);
    vput(m, 0, V_LOOP_YN2, (uint16_t)hist_hq[LOOP / 16][1]);
    exact_voice(m, 1, 5, A_LQ, LEN, LOOP, ID_FR);
    vput(m, 1, V_LOOP_YN1, (uint16_t)hist_lq[LOOP / 16][0]);
    vput(m, 1, V_LOOP_YN2, (uint16_t)hist_lq[LOOP / 16][1]);
    /* voice 2 is enabled but its sync bit stays clear: never touched */
    exact_voice(m, 2, 16, A_P16, 100, 0, ID_FL);
    vput(m, 2, V_FRAC, 0x0123);
    uint16_t mask = 0xFFFF & ~(1u << (15 - 2));
    for (int k = 0; k < OUTS; k++) { /* stream position 4k through the loop */
        int p = 4 * k, pos = p < LEN ? p : LOOP + (p - LEN) % (LEN - LOOP);
        want_l[k] = out_model(dec_hq[pos], 0x7FFF, 0x1000);
        want_r[k] = out_model(dec_lq[pos], 0x7FFF, 0x1000);
    }
    double snr_s = 0, snr_e = 0;
    for (int i = 0; i < 1008; i++) {
        snr_s += (double)x[i] * x[i];
        snr_e += (double)(x[i] - dec_hq[i]) * (x[i] - dec_hq[i]);
    }
    printf("zelda: our AFC 4-bit encoder: %.1f dB\n", 10 * log10(snr_s / snr_e));
    for (int r = 0; r < FRAMES / 2; r++) {
        uint16_t masks[1] = {mask};
        zrender(m, 2, 0x1000, masks, 1, false, OUT_L + 320u * (uint32_t)r, OUT_R + 320u * (uint32_t)r);
        if (r == 0) { /* a command while the CPU has not continued yet waits */
            uint32_t nop[1] = {0x80420000u};
            zsend(m, nop, 1);
            expect_none(m, "command before 0xCDD10003");
            zcontinue(m);
            zack(m, 0x8042, "held command");
            expect_int(m, "held command");
        } else {
            zcontinue(m);
        }
    }
    exact_compare(m, OUT_L, want_l, OUTS, "AFC 4-bit, loop inside a block, short last block");
    exact_compare(m, OUT_R, want_r, OUTS, "AFC 2-bit, same");
    CHECK(vget(m, 2, V_FRAC) == 0x0123 && vget(m, 2, V_RESET) == 1, "a voice not released by its sync bit changed");
    CHECK(vget(m, 0, V_RESET) == 0 && vget(m, 0, V_DONE) == 0, "voice 0: reset %u done %u", vget(m, 0, V_RESET),
          vget(m, 0, V_DONE));

    /* PCM16 left (looped 100 -> from 30), PCM8 right (one-shot of 1000: then silent and done) */
    static int16_t p16[100];
    static int8_t p8[1000];
    for (int i = 0; i < 100; i++) {
        p16[i] = sat(20000 * sin(2 * M_PI * i / 23.0));
        m->aram[A_P16 + 2 * i] = (uint8_t)((uint16_t)p16[i] >> 8);
        m->aram[A_P16 + 2 * i + 1] = (uint8_t)p16[i];
    }
    for (int i = 0; i < 1000; i++)
        m->aram[A_P8 + i] = (uint8_t)(p8[i] = (int8_t)lrint(100 * sin(2 * M_PI * i / 29.0)));
    exact_voice(m, 0, 16, A_P16, 100, 30, ID_FL);
    exact_voice(m, 1, 8, A_P8, 1000, 0, ID_FR);
    vput(m, 1, V_LOOPING, 0);
    for (int k = 0; k < OUTS; k++) {
        int p = 4 * k, pos = p < 100 ? p : 30 + (p - 100) % 70;
        want_l[k] = out_model(p16[pos], 0x7FFF, 0x1000);
        want_r[k] = p < 1000 ? out_model((int16_t)(p8[p] * 256), 0x7FFF, 0x1000) : 0;
    }
    for (int r = 0; r < FRAMES / 2; r++) {
        uint16_t masks[1] = {mask};
        zrender(m, 2, 0x1000, masks, 1, false, OUT_L + 320u * (uint32_t)r, OUT_R + 320u * (uint32_t)r);
        zcontinue(m);
    }
    exact_compare(m, OUT_L, want_l, OUTS, "PCM16 from ARAM, looped");
    exact_compare(m, OUT_R, want_r, OUTS, "PCM8 from ARAM, one-shot");
    CHECK(vget(m, 1, V_DONE) == 1, "the one-shot is not done");

    /* the synths: square left, saw right; then 25 % square left, pattern 0 right; then constant + PCM16 from main
     * memory (ratio 1: through the filter's phase 0, x 0x7FFF / 0x8000) */
    struct { unsigned src; uint16_t ratio; } synth[2][2] = {{{0, 0x0800}, {1, 0x0400}}, {{3, 0x0800}, {7, 0x0800}}};
    for (int pass = 0; pass < 2; pass++) {
        for (int c = 0; c < 2; c++) {
            exact_voice(m, c, synth[pass][c].src, 0, 0, 0, c ? ID_FR : ID_FL);
            vput(m, c, V_RATIO, synth[pass][c].ratio);
        }
        for (int k = 0; k < 4 * 80; k++) {
            int16_t s[2];
            for (int c = 0; c < 2; c++) {
                uint32_t r = synth[pass][c].ratio;
                switch (synth[pass][c].src) {
                case 0: s[c] = ((k * r >> 16) & 1) ? -0x4000 : 0x4000; break;
                case 3: s[c] = ((k * 2 * r >> 16) & 3) ? -0x4000 : 0x4000; break;
                case 1: s[c] = (int16_t)(uint16_t)(k * (r >> 1)); break;
                default: s[c] = g_pattern0[(k * (r << 5) >> 16) & 63]; break;
                }
            }
            want_l[k] = out_model(s[0], 0x7FFF, 0x1000);
            want_r[k] = out_model(s[1], 0x7FFF, 0x1000);
        }
        for (int r = 0; r < 2; r++) {
            uint16_t masks[1] = {mask};
            zrender(m, 2, 0x1000, masks, 1, false, OUT_L + 320u * (uint32_t)r, OUT_R + 320u * (uint32_t)r);
            zcontinue(m);
        }
        exact_compare(m, OUT_L, want_l, 320, pass ? "25 % square" : "square");
        exact_compare(m, OUT_R, want_r, 320, pass ? "pattern 0" : "saw");
    }
    exact_voice(m, 0, 16, 0, 0, 0, ID_FL);
    vput(m, 0, V_USE_CONST, 1);
    vput(m, 0, V_CONST, 0x1234);
    exact_voice(m, 1, 33, MRAM_PCM, 0, 0, ID_FR);
    vput(m, 1, V_RATIO, 0x1000);
    vput32(m, 1, V_REMAINING, 250);   /* 250 samples, then the last one held and the voice done */
    /* the loop point (high half only) in samples from the base, then on from the loop address; the ucode keeps the
     * new base in the read-only half (not written back), so a ring like this one has its loop address at the base */
    vput(m, 1, V_LOOP_START, 64);
    vput32(m, 1, V_LOOP_ADDR, MRAM_PCM);
    static int16_t mr[64];
    for (int i = 0; i < 64; i++) {
        mr[i] = sat(9000 * sin(2 * M_PI * i / 16.0));
        put16(m, MRAM_PCM + 2u * (uint32_t)i, (uint16_t)mr[i]);
    }
    for (int k = 0; k < 320; k++) {
        want_l[k] = out_model(0x1234, 0x7FFF, 0x1000);
        /* phase 0 of the filter: tap 1 x 0x7FFF, tap 1 = the sample one before (the 4-sample history leads) */
        int p = k - 3; /* the history holds 4 samples before the stream: output k is sample k - 3 */
        int16_t v = 0;
        if (p >= 0) {
            int q = p < 250 ? p : 249;
            v = mr[q % 64];
        }
        int16_t f = sat64((2 * (int64_t)32767 * v) >> 16);
        want_r[k] = out_model(f, 0x7FFF, 0x1000);
    }
    for (int r = 0; r < 2; r++) {
        uint16_t masks[1] = {mask};
        zrender(m, 2, 0x1000, masks, 1, false, OUT_L + 320u * (uint32_t)r, OUT_R + 320u * (uint32_t)r);
        zcontinue(m);
    }
    exact_compare(m, OUT_L, want_l, 320, "constant voice");
    exact_compare(m, OUT_R, want_r, 320, "PCM16 from main memory (loop, end held)");
    CHECK(vget(m, 1, V_DONE) == 1, "the main-memory voice is not done");
    CHECK(vget(m, 0, V_RESET) == 1, "the constant voice's reset flag was cleared");

    /* reverb: block 2 (front left's reverb buffer) with a ring of 3 frames, filtered before its send (one tap at
     * 0.5) into front left; one frame of a constant into the reverb buffer comes back 3 frames later at half, 8
     * samples late (the filter's window), and again 3 frames after that at a quarter */
    put16(m, RPBS + 64 + 0, 1);
    put16(m, RPBS + 64 + 2, 3);
    put32(m, RPBS + 64 + 4, RING);
    put16(m, RPBS + 64 + 8, ID_FL);
    put16(m, RPBS + 64 + 10, 0x7FFF);
    put16(m, RPBS + 64 + 16, 0x4000);
    memset(m->h.ram + RING, 0, 3 * 160);
    exact_voice(m, 0, 16, 0, 0, 0, ID_FL_REV);
    vput(m, 0, V_USE_CONST, 1);
    vput(m, 0, V_CONST, 0x2000);
    vput(m, 1, V_ENABLED, 0);
    uint16_t masks1[1] = {mask};
    zrender(m, 1, 0x1000, masks1, 1, false, OUT_L, OUT_R);
    zcontinue(m);
    CHECK((int16_t)get16(m, RING) == 4095 && (int16_t)get16(m, RING + 158) == 4095, "ring slot 0: %d",
          (int16_t)get16(m, RING));
    vput(m, 0, V_ENABLED, 0);
    zrender(m, 8, 0x1000, masks1, 1, false, OUT_L, OUT_R);
    zcontinue(m);
    int16_t e[8 * 80];
    for (int i = 0; i < 8 * 80; i++)
        e[i] = (int16_t)get16(m, OUT_L + 2u * (uint32_t)i);
    /* the 8 frames after the voice's: frames 1..8; the echo spans frame 3 from sample 8 to frame 4's sample 7 */
    bool ok = true;
    for (int i = 0; i < 8 * 80; i++) {
        int t = i + 80; /* in samples from the voice's frame */
        int16_t want = 0;
        if (t >= 3 * 80 + 8 && t < 4 * 80 + 8)
            want = 2046; /* (4095 x 0.5) x 0x7FFF / 0x8000 */
        else if (t >= 6 * 80 + 16 && t < 7 * 80 + 16)
            want = 1022;
        ok &= e[i] == want;
    }
    CHECK(ok, "reverb echoes: %d %d %d %d %d", e[2 * 80 + 7], e[2 * 80 + 8], e[3 * 80 + 7], e[3 * 80 + 8],
          e[5 * 80 + 20]);
    if (ok)
        printf("zelda: reverb ring: echo after 3 frames at half, its own echo at a quarter\n");
    put16(m, RPBS + 64, 0);

    /* a ucode switch (to AESND) and back: DSP_RESUME, the setup kept */
    switch_to(m, 0x002E5E41u, 0x00180000u);
    check_ucode(m, 0x002E5E41u, "aesnd");
    expect_mail(m, DSP_MAIL_INIT, "aesnd greeting");
    expect_int(m, "aesnd greeting");
    send_mail(m, CPU_MAIL_NEW_UCODE);
    uint32_t back[] = {0x80000000u, 0x2000, 0, 0x00100000u, IMG_SIZE, 0, 0x10, 0, 0, 0};
    for (size_t i = 0; i < sizeof back / sizeof back[0]; i++)
        send_mail(m, back[i]);
    check_ucode(m, 0x86840740u, "zelda");
    expect_mail(m, DSP_MAIL_RESUME, "back to zelda");
    expect_int(m, "resume");
    exact_voice(m, 0, 16, A_P16, 100, 30, ID_FL);
    zrender(m, 1, 0x1000, masks1, 1, false, OUT_L, OUT_R);
    CHECK((int16_t)get16(m, OUT_L + 4) == out_model(p16[4 * 2], 0x7FFF, 0x1000), "render after the resume");

    /* a command that crashes the ucode: halted, silent from then on */
    zcontinue(m);
    uint32_t crash[1] = {0x84000000u};
    zsend(m, crash, 1);
    expect_none(m, "crashing command");
    uint32_t nop[1] = {0x80000000u};
    zsend(m, nop, 1);
    expect_none(m, "command after the halt");
    gc_destroy(m);
}

/* ---- Super Mario Sunshine's version: per-frame syncs ---- */

static void test_sync_per_frame(void)
{
    gc_machine *m = machine();
    boot(m, 0x56D36052u, "zelda");
    expect_mail(m, DSP_MAIL_INIT, "greeting");
    expect_mail(m, 0xF3551111u, "greeting");
    tables(m);
    zsetup(m, 64);
    const uint32_t A = 0x1000;
    for (int i = 0; i < 400; i++) {
        int16_t v = (int16_t)(i * 37 - 7000);
        m->aram[A + 2 * i] = (uint8_t)((uint16_t)v >> 8);
        m->aram[A + 2 * i + 1] = (uint8_t)v;
    }
    exact_voice(m, 33, 16, A, 400, 0, ID_FL); /* voice 33: group 2, bit 15 - 1 */
    uint16_t masks[4] = {0, 0, 0x4000, 0};
    zrender(m, 1, 0x1000, masks, 0, true, OUT_L, OUT_R);
    zcontinue(m);
    bool ok = true;
    for (int k = 0; k < 80; k++)
        ok &= (int16_t)get16(m, OUT_L + 2u * (uint32_t)k) == out_model((int16_t)(4 * k * 37 - 7000), 0x7FFF, 0x1000);
    CHECK(ok, "voice 33 through per-frame masks");
    masks[2] = 0;
    put16(m, OUT_L, 0x5555);
    zrender(m, 1, 0x1000, masks, 0, true, OUT_L, OUT_R);
    CHECK(get16(m, OUT_L) == 0, "voice 33 played with its bit clear");
    if (ok)
        printf("zelda: per-frame syncs (Super Mario Sunshine's version) ok\n");
    gc_destroy(m);
}

/* ---- the light protocol: Pikmin 1's version, and the NTSC IPL's (0x80-word VPBs, explicit ramp steps) ---- */

static void test_light(uint32_t crc, bool tiny)
{
    gc_machine *m = machine();
    boot(m, crc, "zelda");
    expect_mail(m, 0x88881111u, "light greeting");
    advance(m, 5000);
    CHECK(!(r16(m, 0x500A) & DSPCR_DSPINT), "the light greeting raised an interrupt");
    tables(m);
    g_vpb_bytes = tiny ? 0x100 : 0x180;
    uint32_t setup[5] = {0x81000004u, VPBS, TABLES, AFC_COEFS, RPBS};
    for (int i = 0; i < 5; i++)
        send_mail(m, setup[i]);
    expect_mail(m, 0x80000064u, "light setup acknowledgement (the handler's address)");
    const uint32_t A = 0x1000;
    for (int i = 0; i < 2000; i++) {
        int16_t v = (int16_t)(sin(i / 9.0) * 15000);
        m->aram[A + 2 * i] = (uint8_t)((uint16_t)v >> 8);
        m->aram[A + 2 * i + 1] = (uint8_t)v;
    }
    memset(m->h.ram + VPBS, 0, 4 * g_vpb_bytes);
    /* full layout words, or where the IPL's 0x80-word blocks keep them */
    int src = tiny ? 0x40 : V_SOURCE, base = tiny ? 0x4C : V_BASE, start = tiny ? 0x4A : V_LOOP_START;
    int rem = tiny ? V_REMAINING - 0x18 : V_REMAINING;
    vput(m, 3, V_ENABLED, 1);
    vput(m, 3, V_RESET, 1);
    vput(m, 3, V_RATIO, 0x4000);
    vchan(m, 3, 0, ID_FR, 0, 0x7FFF); /* explicit step on the IPL: 0 = hold; elsewhere the target */
    if (!tiny)
        vput(m, 3, V_CH + 1, 0x7FFF);
    vput(m, 3, src, 16);
    vput32(m, 3, start, 2000);
    vput32(m, 3, base, A);
    uint32_t render[3] = {0x82040000u | 0x1000, OUT_L, OUT_R};
    for (int i = 0; i < 3; i++)
        send_mail(m, render[i]);
    expect_mail(m, 0x80000062u, "light render acknowledgement");
    advance(m, 5000);
    CHECK(!(r16(m, 0x500A) & DSPCR_DSPINT), "light acknowledgement raised an interrupt");
    for (int f = 0; f < 4; f++) { /* one zero mail per frame, each answered by an interrupt alone */
        send_mail(m, 0);
        expect_none(m, "light render");
        expect_int(m, "light render");
    }
    bool ok = true;
    for (int k = 0; k < 320; k++) {
        int16_t v = (int16_t)(sin(4 * k / 9.0) * 15000);
        ok &= (int16_t)get16(m, OUT_R + 2u * (uint32_t)k) == out_model(v, 0x7FFF, 0x1000);
    }
    CHECK(ok, "%08X: the light render's output", crc);
    uint32_t left = (uint32_t)vget(m, 3, rem) << 16 | vget(m, 3, rem + 1);
    CHECK(left == 2000 - 4 * 320, "%08X: remaining %u written back where the version keeps it", crc, left);
    /* and again: the next command is taken at once (no 0xCDD10003 in the light protocol) */
    for (int i = 0; i < 3; i++)
        send_mail(m, render[i]);
    expect_mail(m, 0x80000062u, "second light render");
    for (int f = 0; f < 4; f++) {
        send_mail(m, 0);
        expect_int(m, "second light render");
    }
    /* a stray zero mail once the render is over (an extra sync) is dropped, and the next command still works */
    send_mail(m, 0);
    expect_none(m, "stray mail");
    send_mail(m, 0x80000000u);
    expect_mail(m, 0x80000062u, "command after a stray mail");
    if (ok)
        printf("zelda: light protocol %08X%s ok\n", crc, tiny ? " (0x80-word VPBs, explicit steps)" : "");
    g_vpb_bytes = 0x180;
    gc_destroy(m);
}

/* ---- zelda.wav ---- */

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

static uint32_t record(gc_machine *m, FILE *f, double seconds)
{
    uint32_t frames = 0;
    for (int i = 0; i < (int)(seconds * 1000); i++) {
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

/* The voices of zelda.wav (also the save state test's): a machine booted, set up, voices 0 and 1 playing. */
static gc_machine *wav_setup(void)
{
    gc_machine *m = machine();
    boot(m, 0x86840740u, "zelda");
    expect_mail(m, DSP_MAIL_INIT, "greeting");
    expect_mail(m, 0xF3551111u, "greeting");
    tables(m);
    zsetup(m, 8);
    const uint32_t A_SINE = 0x10000, A_AFC = 0x20000, A_P8 = 0x30000;
    for (int i = 0; i < 320; i++) { /* 1000 Hz at 32 kHz, 10 periods */
        int16_t v = sat(16000 * sin(2 * M_PI * i / 32.0));
        m->aram[A_SINE + 2 * i] = (uint8_t)((uint16_t)v >> 8);
        m->aram[A_SINE + 2 * i + 1] = (uint8_t)v;
    }
    static int16_t x[512], dec[512], hist[32][2];
    for (int i = 0; i < 512; i++) /* 500 Hz at 16 kHz */
        x[i] = sat(16000 * sin(2 * M_PI * i / 32.0));
    afc_stream(m, A_AFC, x, 512, false, dec, hist);
    for (int i = 0; i < 96; i++) /* 750 Hz at 8 kHz, 9 periods, PCM8 at 100 */
        m->aram[A_P8 + i] = (uint8_t)(int8_t)lrint(100 * sin(2 * M_PI * 9 * i / 96.0));
    for (int i = 0; i < 128; i++) /* 1250 Hz at 32 kHz, 5 periods, in main memory */
        put16(m, MRAM_PCM + 2u * (uint32_t)i, (uint16_t)sat(16000 * sin(2 * M_PI * 5 * i / 128.0)));
    memset(m->h.ram + VPBS, 0, 8 * 0x180);
    /* 0: PCM16 left */
    vput(m, 0, V_ENABLED, 1);
    vput(m, 0, V_RESET, 1);
    vput(m, 0, V_RATIO, 0x1000);
    vchan(m, 0, 0, ID_FL, 0x7FFF, 0x7FFF);
    vput(m, 0, V_SOURCE, 16);
    vput(m, 0, V_LOOPING, 1);
    vput32(m, 0, V_LOOP_START, 320);
    vput32(m, 0, V_BASE, A_SINE);
    /* 1: AFC right, 16 -> 32 kHz, looped on its last period */
    vput(m, 1, V_ENABLED, 1);
    vput(m, 1, V_RESET, 1);
    vput(m, 1, V_RATIO, 0x0800);
    vchan(m, 1, 2, ID_FR, 0x7FFF, 0x7FFF);
    vput(m, 1, V_SOURCE, 9);
    vput(m, 1, V_LOOPING, 1);
    vput32(m, 1, V_LOOP_ADDR, 480);
    vput32(m, 1, V_LOOP_START, 512);
    vput(m, 1, V_LOOP_YN1, (uint16_t)hist[30][0]);
    vput(m, 1, V_LOOP_YN2, (uint16_t)hist[30][1]);
    vput32(m, 1, V_BASE, A_AFC);
    /* 2: PCM8 on both, 8 -> 32 kHz (from 0.5 s) */
    vput(m, 2, V_RESET, 1);
    vput(m, 2, V_RATIO, 0x0400);
    vchan(m, 2, 0, ID_FL, 0x7FFF, 0x7FFF);
    vchan(m, 2, 1, ID_FR, 0x7FFF, 0x7FFF);
    vput(m, 2, V_SOURCE, 8);
    vput(m, 2, V_LOOPING, 1);
    vput32(m, 2, V_LOOP_START, 96);
    vput32(m, 2, V_BASE, A_P8);
    /* 3: main memory PCM16 right, 0.5 s then it ends by itself (from 1 s) */
    vput(m, 3, V_RESET, 1);
    vput(m, 3, V_RATIO, 0x1000);
    vchan(m, 3, 5, ID_FR, 0x7FFF, 0x7FFF);
    vput(m, 3, V_SOURCE, 33);
    vput32(m, 3, V_REMAINING, 16000);
    vput(m, 3, V_LOOP_START, 128);
    vput32(m, 3, V_LOOP_ADDR, MRAM_PCM);
    vput32(m, 3, V_BASE, MRAM_PCM);
    /* 4: positional, front left, the 1000 Hz table (from 1 s) */
    vput(m, 4, V_RESET, 1);
    vput(m, 4, V_RATIO, 0x1000);
    vput(m, 4, V_USE_DOLBY, 1);
    vput(m, 4, V_DOLBY_POS, 0x7F7F);
    vput(m, 4, V_DOLBY_CUR, 0x7FFF);
    vput(m, 4, V_DOLBY_TARGET, 0x7FFF);
    vput(m, 4, V_SOURCE, 16);
    vput(m, 4, V_LOOPING, 1);
    vput32(m, 4, V_LOOP_START, 320);
    vput32(m, 4, V_BASE, A_SINE);
    return m;
}

enum { WAV_RENDERS = 200, WAV_FR = 4 };

/* What the CPU changes before render r (4 frames, 10 ms each), and the render's output interleaved for the audio
 * DMA (right then left) at its place in DMA_OUT. */
static void wav_changes(gc_machine *m, int r)
{
    if (r == 50) { /* 0.5 s */
            vput(m, 0, V_END_REQ, 1);
            vput(m, 1, V_END_REQ, 1);
            vput(m, 2, V_ENABLED, 1);
        }
        if (r == 100) { /* 1 s */
            CHECK(vget(m, 0, V_DONE) && vget(m, 1, V_DONE), "faded voices not done: %u %u", vget(m, 0, V_DONE),
                  vget(m, 1, V_DONE));
            vput(m, 2, V_END_REQ, 1);
            vput(m, 3, V_ENABLED, 1);
            vput(m, 4, V_ENABLED, 1);
        }
        if (r == 150)
            vput(m, 4, V_END_REQ, 1);
}

static void wav_interleave(gc_machine *m, int r)
{
    for (int i = 0; i < WAV_FR * 80; i++) {
        uint32_t d = DMA_OUT + 4u * (uint32_t)(r * WAV_FR * 80 + i);
        put16(m, d, get16(m, OUT_R + 2u * (uint32_t)i));
        put16(m, d + 2, get16(m, OUT_L + 2u * (uint32_t)i));
    }
}

static void wav_render(gc_machine *m, int r)
{
    uint16_t masks[1] = {0xFFFF};
    wav_changes(m, r);
    zrender(m, WAV_FR, 0x1000, masks, 1, false, OUT_L, OUT_R);
    zcontinue(m);
    wav_interleave(m, r);
}

static void wav_dma_start(gc_machine *m)
{
    uint32_t blocks = WAV_RENDERS * WAV_FR * 80 * 4 / 32;
    w16(m, 0x5030, (uint16_t)(DMA_OUT >> 16));
    w16(m, 0x5032, (uint16_t)DMA_OUT);
    w16(m, 0x5036, (uint16_t)(0x8000 | blocks));
}

static void test_wav(const char *dir)
{
    gc_machine *m = wav_setup();
    for (int r = 0; r < WAV_RENDERS; r++)
        wav_render(m, r);
    CHECK(vget(m, 3, V_DONE) && vget(m, 4, V_DONE), "voices 3 / 4 not done: %u %u", vget(m, 3, V_DONE),
          vget(m, 4, V_DONE));
    gc_audio_info info;
    gc_audio_get_info(m, &info);
    printf("zelda: %llu frames rendered, %llu mails from the DSP\n", (unsigned long long)info.cmd_lists,
           (unsigned long long)info.mails_from_dsp);
    wav_dma_start(m);
    FILE *f = wav_open(dir, "zelda.wav");
    uint32_t frames = record(m, f, 2.05);
    wav_close(f, frames);
    gc_destroy(m);
}

/* ---- save states: the sound hardware field by field (gc_state.c's DSP section, version 2) ---- */

enum { TAIL_RENDERS = 50, TAIL_PCM = 48 * 10 * TAIL_RENDERS + 4800 };

/* From the state on: the rest of render 60 (frame 0 already rendered, its acknowledgement still in the mailbox),
 * then renders 61..110, each followed by 10 ms of the audio DMA playing; pcm gets the machine's 48 kHz output. */
static size_t tail(gc_machine *m, int16_t *pcm)
{
    uint16_t masks[1] = {0xFFFF};
    size_t got = 0;
    zack(m, 0xFF00, "frame 0 after the state");
    for (unsigned f = 1; f < WAV_FR; f++) {
        send_mail(m, 0);
        send_mail(m, masks[0]);
        zack(m, (uint16_t)(0xFF00 | f), "frame");
    }
    expect_mail(m, DSP_MAIL_FRAME_END, "render end after the state");
    expect_int(m, "render after the state");
    zcontinue(m);
    wav_interleave(m, 60);
    for (int r = 61; r <= 60 + TAIL_RENDERS; r++) {
        wav_render(m, r);
        advance(m, GC_CPU_HZ / 100);
        size_t n;
        while (got < TAIL_PCM && (n = gc_audio_pull(m, pcm + 2 * got, (TAIL_PCM - got) < 256 ? TAIL_PCM - got : 256)))
            got += n;
    }
    return got;
}

static void test_state(void)
{
    gc_machine *m = wav_setup();
    for (int r = 0; r < 60; r++)
        wav_render(m, r);
    wav_dma_start(m);
    advance(m, GC_CPU_HZ / 5); /* the audio DMA well under way, its FIFO and resampler busy */
    int16_t drop[512];
    while (gc_audio_pull(m, drop, 256))
        ;
    /* in the middle of a render: frame 0 rendered, its acknowledgement queued, frames 1..3 waiting for their syncs */
    wav_changes(m, 60);
    uint32_t w[3] = {0x82000000u | WAV_FR << 16 | 0x1000, OUT_L, OUT_R};
    zsend(m, w, 3);
    send_mail(m, 0);
    send_mail(m, 0xFFFF);
    size_t size = gc_state_size(m);
    uint8_t *st = malloc(size);
    size_t n = st ? gc_state_save(m, "zelda", st, size) : 0;
    CHECK(n > 0, "state not saved (%zu bytes wanted)", size);
    static int16_t a[2 * TAIL_PCM], b[2 * TAIL_PCM];
    static uint8_t dma_a[WAV_RENDERS * WAV_FR * 80 * 4];
    size_t na = tail(m, a);
    memcpy(dma_a, m->h.ram + DMA_OUT, sizeof dma_a);
    char why[200] = "";
    int rc = n ? gc_state_load(m, "zelda", st, n, why, sizeof why) : -1;
    CHECK(rc == 0, "state not loaded: %d %s", rc, why);
    CHECK(gc_audio_queued(m) == 0, "the host ring kept frames across a load");
    size_t nb = tail(m, b);
    CHECK(na == nb && na > 48 * 10 * (TAIL_RENDERS - 2), "48 kHz frames after the state: %zu then %zu", na, nb);
    CHECK(memcmp(a, b, 2 * na * sizeof a[0]) == 0, "the 48 kHz output after a load differs");
    CHECK(memcmp(dma_a, m->h.ram + DMA_OUT, sizeof dma_a) == 0, "the rendered frames after a load differ");
    int loud = 0;
    for (size_t i = 0; i < 2 * na; i++)
        loud |= abs(a[i]) > 4000;
    CHECK(loud, "the run after the state is silent");
    /* a damaged DSP section is refused before anything changes: the mail queue's head past its end */
    if (n) {
        size_t at = 72;
        while (at + 12 <= n && memcmp(st + at, "DSP ", 4) != 0)
            at += 12 + (st[at + 8] | st[at + 9] << 8 | (size_t)st[at + 10] << 16 | (size_t)st[at + 11] << 24);
        CHECK(at + 12 <= n, "no DSP section in the state");
        if (at + 12 <= n) {
            size_t qh = at + 12 + 4 + 4 * (1 + 0x100) + 4 + 5 * GC_MAILQ; /* version, csr, regs, cpu mail, queue */
            st[qh] = 0xFF;
            rc = gc_state_load(m, "zelda", st, n, why, sizeof why);
            CHECK(rc != 0, "a damaged DSP section was accepted");
        }
    }
    if (!failures)
        printf("zelda: save state in the middle of a render: %zu frames of 48 kHz output identical after a load\n", na);
    free(st);
    gc_destroy(m);
}

/* ---- AESND ---- */

static void test_aesnd(void)
{
    gc_machine *m = machine();
    boot(m, 0x002E5E41u, "aesnd"); /* the 2023 release: left first, the 2020 flags */
    expect_mail(m, DSP_MAIL_INIT, "aesnd greeting");
    expect_int(m, "aesnd greeting");
    const uint32_t PB = 0x00200000, PB2 = 0x00200040, OUT = 0x00210000, A16 = 0x10000, A8 = 0x20000;
    for (int i = 0; i < 64; i++) {
        int16_t v = (int16_t)(i * 300 - 9000);
        m->aram[A16 + 2 * i] = (uint8_t)((uint16_t)v >> 8);
        m->aram[A16 + 2 * i + 1] = (uint8_t)v;
    }
    for (int i = 0; i < 128; i++) /* stereo unsigned 8-bit: left first (2012+), right */
        m->aram[A8 + i] = (uint8_t)((i & 1) ? 0x80 - 3 * (i / 2) : 0x80 + 2 * (i / 2));
    memset(m->h.ram + PB, 0, 0x80);
    /* voice 1: mono16, a step of 1, 64 samples looped by the accelerator, volume 255 left / 128 right */
    put32(m, PB + 0, OUT);
    put32(m, PB + 4, A16 / 2);
    put32(m, PB + 8, A16 / 2 + 63);
    put32(m, PB + 12, A16 / 2);
    put32(m, PB + 22, 0x10000);
    put16(m, PB + 32, 255);
    put16(m, PB + 34, 128);
    put32(m, PB + 40, 0x40000000u | 2);
    /* voice 2: stereo8 unsigned at half rate, volume 256 / 256, 10 frames of delay */
    put32(m, PB2 + 0, OUT);
    put32(m, PB2 + 4, A8);
    put32(m, PB2 + 8, A8 + 127);
    put32(m, PB2 + 12, A8);
    put32(m, PB2 + 22, 0x8000);
    put16(m, PB2 + 32, 256);
    put16(m, PB2 + 34, 256);
    put32(m, PB2 + 36, 10);
    put32(m, PB2 + 40, 0x40000000u | 5);
    send_mail(m, 0xFACE0080u);
    send_mail(m, PB);
    expect_none(m, "pb address");
    send_mail(m, 0xFACE0010u);
    expect_mail(m, DSP_MAIL_SYNC, "first voice");
    expect_int(m, "first voice");
    CHECK(get32(m, PB + 40) & 0x00100000u, "first voice not flagged finished");
    CHECK(get32(m, PB + 12) == A16 / 2 + 32, "accelerator at %08X after 96 samples of 64 looped", get32(m, PB + 12));
    send_mail(m, 0xFACE0080u);
    send_mail(m, PB2);
    send_mail(m, 0xFACE0020u);
    expect_mail(m, DSP_MAIL_SYNC, "next voice");
    CHECK(get32(m, PB2 + 36) == 0, "delay left %u", get32(m, PB2 + 36));
    send_mail(m, 0xFACE0100u);
    expect_mail(m, DSP_MAIL_SYNC, "send samples");
    bool ok = true;
    int16_t l2 = 0, r2 = 0;
    unsigned c2 = 0, idx2 = 0;
    for (int i = 0; i < 96; i++) {
        /* voice 1: frame i holds what the step before read (frame 0: the block's held value, 0) */
        int16_t x = i ? (int16_t)(((i - 1) % 64) * 300 - 9000) : 0;
        int r = (x * 128) >> 8, l = (x * 255) >> 8;
        /* voice 2: the delay eats 10 of the 96 frames but moves on by 9 (as the ucode counts): frames 9..94, a
         * sample pair every other frame */
        if (i >= 9 && i < 95) {
            r += r2;
            l += l2;
            c2 += 0x8000;
            if (c2 >> 16) {
                c2 &= 0xFFFF;
                int16_t first = (int16_t)(((m->aram[A8 + idx2] << 8) ^ 0x8000) & 0xFFFF);
                int16_t second = (int16_t)(((m->aram[A8 + idx2 + 1] << 8) ^ 0x8000) & 0xFFFF);
                idx2 += 2;
                l2 = (int16_t)((first * 256) >> 8); /* the 2012+ order: left first */
                r2 = (int16_t)((second * 256) >> 8);
            }
        }
        ok &= (int16_t)get16(m, OUT + 4u * (uint32_t)i) == sat64(r);
        ok &= (int16_t)get16(m, OUT + 4u * (uint32_t)i + 2) == sat64(l);
    }
    CHECK(ok, "aesnd output: frame 20 R %d L %d", (int16_t)get16(m, OUT + 80), (int16_t)get16(m, OUT + 82));
    send_mail(m, 0xFACEDEADu);
    expect_mail(m, DSP_MAIL_DONE, "terminate");
    if (ok)
        printf("aesnd: mono16 + stereo8 unsigned (half rate, delay) exact, terminate answered\n");
    gc_destroy(m);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    test_exact();
    test_sync_per_frame();
    test_light(0x4BE6A5CBu, false);
    test_light(0x24B22038u, true);
    test_aesnd();
    test_wav(dir);
    test_state();
    printf("%s\n", failures ? "gc_zelda_test: FAILED" : "gc_zelda_test: ok");
    return failures ? 1 : 0;
}
