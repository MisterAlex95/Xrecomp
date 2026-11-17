/* gc_zelda.c -- the Zelda / JAudio microcode family at a high level (XBA-266): Nintendo's "DAC" ucode that the JAudio
 * sound system drives (The Wind Waker, Twilight Princess GC, Super Mario Sunshine, Pikmin 1 / 2, Luigi's Mansion,
 * Mario Kart: Double Dash, Four Swords Adventures, Animal Crossing, the IPL...). Its versions differ in protocol and
 * layout; they are told apart by the image's hash (gc_dsp.c) and described here by flags.
 *
 * Protocol (the standard one): the ucode greets with DSP_INIT and 0xF3551111. The CPU sends commands as a word count
 * (n, MSB clear) then n words; the first has bit 31 set, the command in bits 24..30, a "sync" byte in 16..23 and
 * 16 bits of data. Each command is acknowledged DSP_SYNC (interrupt) then 0xF355xxxx (xxxx = its top 16 bits).
 *   01 setup: voice count (data), the voice parameter blocks' (VPB) base, a table block (resampling filter, the four
 *      pattern voices, the panning sine), the AFC predictor table, the reverb blocks' base
 *   02 render: frame count (sync byte), output volume 4.12 (data), left and right output buffers (+ two words on
 *      versions that merged command 0D into it)
 *   0C / 0D / 0E version-dependent (GBA crypto, unknown, Wii ARAM base), 00 0A 0B 0F (and 03) no-ops, 04..09 and
 *      above 0F crash the ucode (halted)
 * During a render, voices are processed only as the CPU releases them by sync mails (a mail with a zero low half,
 * then one per group of 16 voices: group << 16 | a bit per voice, bit 15 = the group's first; or, on SYNC_PER_FRAME
 * versions, two mails of two 16-bit masks each per frame). Each frame ends with an acknowledgement 0xFF00 | frame,
 * the render with DSP_FRAME_END (0xDCD10005); further commands wait for the CPU's 0xCDD10003 (0xCDD10001: a new ucode
 * follows, 0xCDD10002: back to the ROM). The "light" versions (IPL, Pikmin 1, Luigi's Mansion, Animal Crossing)
 * greet 0x88881111, take a fixed number of words per command, acknowledge with 0x80000000 | the command handler's
 * address (no interrupt), and render all frames on one zero mail, raising an interrupt without a mail.
 *
 * A frame is 80 samples at 32 kHz (2.5 ms). Each enabled voice (VPB, 0xC0 words; 0x80 on the NTSC IPL's version)
 * gets 80 input samples from its source: AFC (Nintendo's ADPCM: 16 samples per 9-byte block with 4-bit samples or
 * per 5-byte block with 2-bit ones, a scale and a predictor pair per block), PCM8 / PCM16 from ARAM, PCM16 from main
 * memory, square / 25 % square / saw synths, four 64-sample patterns (two of them noise that moves every frame), or a
 * constant; resampled with the game's 4-tap polyphase filter (4.12 ratio); an optional one-pole low-pass and biquad;
 * then mixed with volume ramps either into up to six of the ucode's buffers (named by their DSP memory addresses) or
 * positionally (front / back x left / right from a sine table, plus a reverb send). The frame's front left / right
 * buffers, scaled by the output volume, go to the two output buffers (the CPU interleaves them for the audio DMA).
 * Four reverb blocks keep a ring of frames in main memory: each frame what went into a reverb buffer is stored, and
 * the frame from one ring length ago comes back (an 8-tap filter before or after) into two buffers and into the
 * reverb buffer itself (the feedback).
 *
 * Written from the ucode's documented behaviour (Dolphin's notes and source read for facts only, never copied). Not
 * modelled: the variable FIR filter, the Dolby surround downmix of the back buffers, TP's extra output filtering, the
 * DSP's processing time (acknowledgements come a moment after the mail). */
#include <string.h>

#include "gc_dsp_internal.h"

/* Version flags */
enum {
    ZF_NO_ARAM = 1u << 0,          /* Wii versions: "ARAM" is main memory at a base (command 0E) */
    ZF_DOLBY_LOUDER = 1u << 1,     /* positional volumes x2 */
    ZF_LIGHT = 1u << 2,            /* the light protocol (no positional mixing either) */
    ZF_FOUR_DESTS = 1u << 3,       /* 4 mixing destinations per voice instead of 6 */
    ZF_TINY_VPB = 1u << 4,         /* 0x80-word VPBs */
    ZF_EXPLICIT_STEP = 1u << 5,    /* a destination's first volume word is the ramp step, not the target */
    ZF_SYNC_PER_FRAME = 1u << 6,   /* two sync mails per frame instead of one per 16 voices */
    ZF_NO_CMD_0D = 1u << 7,
    ZF_GBA_CRYPTO = 1u << 8,       /* command 0C: the GBA link's key exchange (one word) */
    ZF_WEIRD_CMD_0C = 1u << 9,     /* command 0C takes two words, purpose unknown */
    ZF_COMBINED_0D = 1u << 10,     /* command 02 carries command 0D's two words */
};

static const struct {
    uint32_t crc, flags;
} k_versions[] = {
    /* IPL NTSC */
    {0x24B22038u, ZF_LIGHT | ZF_FOUR_DESTS | ZF_TINY_VPB | ZF_EXPLICIT_STEP | ZF_NO_CMD_0D | ZF_WEIRD_CMD_0C},
    {0x6BA3B3EAu, ZF_LIGHT | ZF_FOUR_DESTS | ZF_NO_CMD_0D},                    /* IPL PAL */
    {0xDF059F68u, ZF_LIGHT | ZF_NO_CMD_0D | ZF_GBA_CRYPTO},                    /* Pikmin 1 NTSC demo */
    {0x4BE6A5CBu, ZF_LIGHT | ZF_NO_CMD_0D | ZF_GBA_CRYPTO},                    /* Pikmin 1 NTSC, Animal Crossing */
    {0x42F64AC4u, ZF_LIGHT | ZF_NO_CMD_0D | ZF_WEIRD_CMD_0C},                  /* Luigi's Mansion */
    {0x267FD05Au, ZF_SYNC_PER_FRAME | ZF_NO_CMD_0D},                           /* Pikmin 1 PAL */
    {0x56D36052u, ZF_SYNC_PER_FRAME | ZF_NO_CMD_0D},                           /* Super Mario Sunshine */
    {0x86840740u, 0},                                                          /* The Wind Waker */
    {0x2FCDF1ECu, ZF_DOLBY_LOUDER},                                            /* MKDD, FSA, Pikmin 2, Zelda CE */
    {0x6CA33A6Du, ZF_DOLBY_LOUDER | ZF_COMBINED_0D},                           /* Twilight Princess GC, DKJB */
};

enum { ZS_WAITING, ZS_RENDERING, ZS_WRITING_CMD, ZS_HALTED };

#define ACK_DELAY 1000 /* cycles between a mail to the DSP and its answer's interrupt */
#define N GC_ZELDA_SAMPLES

/* VPB words (the full layout). The game owns the read-only half (0x80..0xBF); the ucode writes back 0x00..0x7F. */
enum {
    V_ENABLED = 0x00, V_DONE = 0x01, V_RATIO = 0x02, V_RESET = 0x04, V_END_REACHED = 0x05, V_USE_CONST = 0x06,
    V_CHANNELS = 0x08,             /* 6 x (buffer id, target volume, current volume, unused) */
    V_DOLBY_POS = 0x28, V_DOLBY_REVERB = 0x29, V_DOLBY_CUR = 0x2A, V_DOLBY_TARGET = 0x2B, V_USE_DOLBY = 0x2C,
    V_FRAC = 0x30, V_AFC_LEFT = 0x32, V_CONST = 0x33, V_POS = 0x34, V_BEFORE_LOOP = 0x36, V_ARAM = 0x38,
    V_REMAINING = 0x3A, V_HIST = 0x3C, V_BIQ_X1 = 0x54, V_BIQ_X2 = 0x55, V_BIQ_Y1 = 0x56, V_BIQ_Y2 = 0x57,
    V_AFC_CACHE = 0x58,            /* the last decoded block; its last two samples are the decoder's history */
    V_LP_Y1 = 0x68, V_LP_X1 = 0x69,
    V_SOURCE = 0x80, V_LOOPING = 0x81, V_LOOP_YN1 = 0x82, V_LOOP_YN2 = 0x83, V_FILTERS = 0x84, V_END_REQ = 0x85,
    V_LOOP_ADDR = 0x88, V_LOOP_START = 0x8A, V_BASE = 0x8C, V_BIQ_B1 = 0xA4, V_BIQ_B2 = 0xA5, V_BIQ_A1 = 0xA6,
    V_BIQ_A2 = 0xA7, V_LP_COEF = 0xA8, V_WORDS = 0xC0,
};

/* Sample sources (the number is also the bytes per 16 samples of the two AFC kinds) */
enum {
    SRC_SQUARE = 0, SRC_SAW = 1, SRC_SQUARE_25 = 3, SRC_PATTERN1 = 4, SRC_AFC_LQ = 5, SRC_PATTERN0 = 7,
    SRC_PCM8 = 8, SRC_AFC_HQ = 9, SRC_PATTERN0_VAR = 10, SRC_PATTERN2 = 11, SRC_PATTERN3 = 12, SRC_PCM16 = 16,
    SRC_PCM16_MRAM = 33,
};

static int16_t sat16(int64_t v)
{
    return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

static uint32_t get32(const uint16_t *w, int at)
{
    return (uint32_t)w[at] << 16 | w[at + 1];
}

static void put32(uint16_t *w, int at, uint32_t v)
{
    w[at] = (uint16_t)(v >> 16);
    w[at + 1] = (uint16_t)v;
}

/* ---- mail out ---- */

static void ack(gc_machine *m, gc_zelda *z, uint16_t sync)
{
    if (z->flags & ZF_LIGHT) { /* the command handler's address in the ucode's jump table */
        gc_dsp_push_mail(m, 0x80000000u | (uint16_t)(2 * ((sync >> 8) & 0x7F) + 0x62), false, 0);
        return;
    }
    gc_dsp_push_mail(m, DSP_MAIL_SYNC, true, ACK_DELAY);
    gc_dsp_push_mail(m, 0xF3550000u | sync, false, 0);
}

/* ---- the ucode's buffers ---- */

/* A mixing destination as the game names it: the buffer's address in the ucode's data memory. */
static int16_t *buffer_for(gc_zelda *z, uint16_t id)
{
    static const struct {
        uint16_t id;
        uint8_t buf;
    } k_ids[] = {
        {0x0D00, ZB_FRONT_L}, {0x0D60, ZB_FRONT_R}, {0x0F40, ZB_BACK_L}, {0x0CA0, ZB_BACK_R},
        {0x0E80, ZB_FRONT_L_REV}, {0x0EE0, ZB_FRONT_R_REV}, {0x0C00, ZB_BACK_L_REV}, {0x0C50, ZB_BACK_R_REV},
        {0x0DC0, ZB_UNK0_REV}, {0x0E20, ZB_UNK1_REV}, {0x09A0, ZB_UNK0}, {0x0FA0, ZB_UNK1}, {0x0B00, ZB_UNK2},
    };
    for (size_t i = 0; i < sizeof k_ids / sizeof k_ids[0]; i++)
        if (k_ids[i].id == id)
            return z->buf[k_ids[i].buf];
    return NULL;
}

/* dst += src * vol (1.15), saturated */
static void mix_scaled(int16_t *dst, const int16_t *src, int n, int32_t vol)
{
    for (int i = 0; i < n; i++)
        dst[i] = sat16(dst[i] + sat16(((int32_t)src[i] * vol) >> 15));
}

static void scale(int16_t *b, int32_t vol, int shift)
{
    for (int i = 0; i < N; i++)
        b[i] = sat16(((int64_t)b[i] * vol) >> shift);
}

/* dst += src * volume ramping from vol by step per sample (16.16 of a 1.15 volume, i.e. half scale as the ucode
 * mixes); returns where the ramp ended. */
static int32_t mix_ramp(int16_t *dst, const int16_t *src, int32_t vol, int32_t step)
{
    if (!vol && !step)
        return vol;
    for (int i = 0; i < N; i++) {
        dst[i] = sat16(dst[i] + (((int64_t)(vol >> 16) * src[i]) >> 16));
        vol += step;
    }
    return vol;
}

/* ---- reverb ---- */

enum { R_ENABLED, R_RING_FRAMES, R_RING_ADDR, R_DEST = 4, R_COEFS = 8, R_WORDS = 16 };

/* x[i] = the 8 taps from x[i] on, for the first N samples (in place: each one is read before it is replaced) */
static void reverb_filter(int16_t *x, const uint16_t *coefs)
{
    for (int i = 0; i < N; i++) {
        int64_t acc = 0;
        for (int k = 0; k < 8; k++)
            acc += (int64_t)x[i + k] * (int16_t)coefs[k];
        x[i] = sat16(acc >> 15);
    }
}

static void reverb(gc_machine *m, gc_zelda *z, bool after)
{
    static const uint8_t k_rev_buf[4] = {ZB_UNK0_REV, ZB_UNK1_REV, ZB_FRONT_L_REV, ZB_FRONT_R_REV};
    if (!z->reverb_base)
        return;
    for (int r = 0; r < 4; r++) {
        uint16_t rb[R_WORDS];
        for (int i = 0; i < R_WORDS; i++)
            rb[i] = dsp_rd16(m, z->reverb_base + 2u * (uint32_t)(R_WORDS * r + i));
        if (!rb[R_ENABLED])
            continue;
        uint32_t at = get32(rb, R_RING_ADDR) + 2u * N * z->reverb_pos[r];
        int16_t *own = z->buf[k_rev_buf[r]];
        if (after) { /* this frame's reverb input joins the ring */
            for (int i = 0; i < N; i++)
                dsp_wr16(m, at + 2u * (uint32_t)i, (uint16_t)own[i]);
            uint16_t frames = rb[R_RING_FRAMES] ? rb[R_RING_FRAMES] : 1;
            z->reverb_pos[r] = (uint16_t)((z->reverb_pos[r] + 1) % frames);
            continue;
        }
        /* the frame from one ring ago, behind the last 8 samples of the one before (the filter's window) */
        int16_t x[8 + N];
        memcpy(x, z->reverb_last8[r], sizeof z->reverb_last8[r]);
        for (int i = 0; i < N; i++)
            x[8 + i] = (int16_t)dsp_rd16(m, at + 2u * (uint32_t)i);
        memcpy(z->reverb_last8[r], x + N, sizeof z->reverb_last8[r]);
        if (rb[R_ENABLED] & 1) /* bit 0: filtered before the sends */
            reverb_filter(x, rb + R_COEFS);
        for (int d = 0; d < 2; d++) {
            uint16_t id = rb[R_DEST + 2 * d];
            int16_t *dst = id ? buffer_for(z, id) : NULL;
            if (dst)
                mix_scaled(dst, x, N, rb[R_DEST + 2 * d + 1]);
        }
        if (rb[R_ENABLED] & 2) /* bit 1: after them (the feedback only) */
            reverb_filter(x, rb + R_COEFS);
        memcpy(own, x, N * sizeof *own); /* fed back: the voices' reverb sends add to it */
    }
}

/* ---- frames ---- */

/* The four pattern voices: 2 and 3 are noise the ucode moves on every frame. */
static void move_patterns(gc_zelda *z)
{
    int16_t *p = z->patterns + 2 * 0x40;
    int32_t y2 = p[0x3E], y1 = p[0x3F];
    for (int i = 0; i < 0x40; i += 2) {
        int32_t v = (int32_t)(uint32_t)((int64_t)y2 * y1 - (int64_t)p[i] * 65536);
        y2 = y1;
        y1 = p[i];
        p[i] = (int16_t)(v >> 16);
        v = (int32_t)(uint32_t)(2 * ((int64_t)y2 * y1 + (int64_t)p[i + 1] * 65536));
        y2 = y1;
        y1 = p[i + 1];
        p[i + 1] = (int16_t)(v >> 16);
    }
    p = z->patterns + 3 * 0x40;
    y2 = p[0x3E];
    y1 = p[0x3F];
    int16_t base = (int16_t)y1;
    int32_t mixed = (int32_t)(uint32_t)((int64_t)y1 * y2 + (int64_t)y2 * 65536 + y1);
    int16_t step = (int16_t)(p[0] + (mixed >> 16));
    step = (int16_t)((step & 0x1FF) | 0x2000);
    for (int i = 0; i < 0x40; i++)
        p[i] = (int16_t)(base + (i + 1) * step);
}

static void prepare_frame(gc_machine *m, gc_zelda *z)
{
    if (z->prepared)
        return;
    memset(z->buf[ZB_FRONT_L], 0, sizeof z->buf[0]);
    memset(z->buf[ZB_FRONT_R], 0, sizeof z->buf[0]);
    memset(z->buf[ZB_UNK0], 0, sizeof z->buf[0]); /* never output: kept from piling up */
    memset(z->buf[ZB_UNK1], 0, sizeof z->buf[0]);
    memset(z->buf[ZB_UNK2], 0, sizeof z->buf[0]);
    scale(z->buf[ZB_BACK_L], 0x6784, 15); /* the back buffers decay instead of starting over */
    scale(z->buf[ZB_BACK_R], 0x6784, 15);
    reverb(m, z, false);
    /* the back reverb sends fold into the front ones: each back side in full on its own side and at 0xB820 (taken
     * as the DSP's signed 1.15: -0.56) on the other, the right one from its second half only */
    mix_scaled(z->buf[ZB_FRONT_L_REV], z->buf[ZB_BACK_L_REV], N, 0x7FFF);
    mix_scaled(z->buf[ZB_FRONT_R_REV], z->buf[ZB_BACK_L_REV], N, (int16_t)0xB820);
    mix_scaled(z->buf[ZB_FRONT_L_REV], z->buf[ZB_BACK_R_REV] + N / 2, N / 2, (int16_t)0xB820);
    mix_scaled(z->buf[ZB_FRONT_R_REV], z->buf[ZB_BACK_R_REV] + N / 2, N / 2, 0x7FFF);
    memset(z->buf[ZB_BACK_L_REV], 0, sizeof z->buf[0]);
    memset(z->buf[ZB_BACK_R_REV], 0, sizeof z->buf[0]);
    move_patterns(z);
    z->prepared = true;
}

static void finalize_frame(gc_machine *m, gc_zelda *z)
{
    scale(z->buf[ZB_FRONT_L], z->out_vol, 12);
    scale(z->buf[ZB_FRONT_R], z->out_vol, 12);
    for (int i = 0; i < N; i++) {
        dsp_wr16(m, z->out_l + 2u * (uint32_t)i, (uint16_t)z->buf[ZB_FRONT_L][i]);
        dsp_wr16(m, z->out_r + 2u * (uint32_t)i, (uint16_t)z->buf[ZB_FRONT_R][i]);
    }
    z->out_l += 2 * N;
    z->out_r += 2 * N;
    reverb(m, z, true);
    z->prepared = false;
    z->frames++;
}

/* ---- sample sources ---- */

static void pcm_from_aram(gc_machine *m, uint16_t *w, int16_t *dst, unsigned n, unsigned bytes)
{
    if (w[V_DONE]) {
        memset(dst, 0, n * sizeof *dst);
        return;
    }
    if (w[V_RESET]) {
        put32(w, V_REMAINING, get32(w, V_LOOP_START) - get32(w, V_POS));
        put32(w, V_ARAM, get32(w, V_BASE) + get32(w, V_POS) * bytes);
    }
    w[V_END_REACHED] = 0;
    while (n) {
        if (w[V_END_REACHED]) {
            w[V_END_REACHED] = 0;
            uint32_t len = get32(w, V_LOOP_START) - get32(w, V_LOOP_ADDR);
            if (!w[V_LOOPING] || !len || len > 0x7FFFFFFFu) { /* a one-shot ends (or a loop of nothing) */
                memset(dst, 0, n * sizeof *dst);
                w[V_DONE] = 1;
                return;
            }
            put32(w, V_POS, get32(w, V_LOOP_ADDR));
            put32(w, V_REMAINING, len);
            put32(w, V_ARAM, get32(w, V_BASE) + get32(w, V_POS) * bytes);
        }
        uint32_t rem = get32(w, V_REMAINING), k = rem < n ? rem : n, a = get32(w, V_ARAM);
        for (uint32_t i = 0; i < k; i++, a += bytes) {
            const uint8_t *p = m->aram;
            uint32_t mk = GC_ARAM_SIZE - 1;
            *dst++ = bytes == 1 ? (int16_t)(p[a & mk] << 8) : (int16_t)(p[a & mk] << 8 | p[(a + 1) & mk]);
        }
        put32(w, V_REMAINING, rem - k);
        put32(w, V_ARAM, a);
        n -= k;
        if (rem == k)
            w[V_END_REACHED] = 1;
    }
}

/* One AFC block at the voice's ARAM address into out[16], with the history in the cache's last two words. */
static void afc_block(gc_machine *m, const gc_zelda *z, uint16_t *w, int16_t out[16])
{
    unsigned bytes = w[V_SOURCE];
    uint32_t a = get32(w, V_ARAM), mk = GC_ARAM_SIZE - 1;
    put32(w, V_ARAM, a + bytes);
    uint8_t hdr = m->aram[a & mk];
    int32_t step = 1 << (hdr >> 4);
    unsigned pair = hdr & 15;
    int32_t c1 = z->afc_coefs[2 * pair], c2 = z->afc_coefs[2 * pair + 1];
    int32_t y1 = (int16_t)w[V_AFC_CACHE + 15], y2 = (int16_t)w[V_AFC_CACHE + 14];
    for (int i = 0; i < 16; i++) {
        int32_t code; /* the sample's code, sign-extended, in 1/2048 of the step */
        if (bytes == SRC_AFC_HQ) {
            uint8_t b = m->aram[(a + 1 + (uint32_t)i / 2) & mk];
            unsigned nib = (i & 1) ? b & 15 : b >> 4;
            code = ((int32_t)nib ^ 8) - 8;
            code *= 2048;
        } else {
            uint8_t b = m->aram[(a + 1 + (uint32_t)i / 4) & mk];
            unsigned two = (b >> (6 - 2 * (i & 3))) & 3;
            code = ((int32_t)two ^ 2) - 2;
            code *= 8192;
        }
        int16_t s = sat16(((int64_t)step * code + (int64_t)y1 * c1 + (int64_t)y2 * c2) >> 11);
        out[i] = s;
        y2 = y1;
        y1 = s;
    }
    w[V_AFC_CACHE + 14] = (uint16_t)y2;
    w[V_AFC_CACHE + 15] = (uint16_t)y1;
}

/* AFC from ARAM: the voice's first loop_start samples, then (looping) from loop_addr on. The cache holds the last
 * decoded block, its unread samples at its end (V_AFC_LEFT of them). */
static void afc_from_aram(gc_machine *m, const gc_zelda *z, uint16_t *w, int16_t *dst, unsigned n)
{
    int16_t blk[16];
    if (w[V_RESET]) {
        w[V_AFC_CACHE + 14] = w[V_AFC_CACHE + 15] = 0;
        w[V_AFC_LEFT] = 0;
        put32(w, V_REMAINING, get32(w, V_LOOP_START));
        put32(w, V_ARAM, get32(w, V_BASE));
    }
    if (w[V_DONE]) {
        memset(dst, 0, n * sizeof *dst);
        return;
    }
    if (w[V_AFC_LEFT] > 16)
        w[V_AFC_LEFT] = 16;
    for (unsigned i = 0; i < n; i++) {
        if (!w[V_AFC_LEFT]) {
            uint32_t rem = get32(w, V_REMAINING);
            if (!rem) {
                if (!w[V_LOOPING]) {
                    w[V_DONE] = 1;
                    memset(dst + i, 0, (n - i) * sizeof *dst);
                    return;
                }
                uint32_t loop = get32(w, V_LOOP_ADDR), len = get32(w, V_LOOP_START);
                put32(w, V_ARAM, get32(w, V_BASE) + (loop >> 4) * w[V_SOURCE]);
                w[V_AFC_CACHE + 15] = w[V_LOOP_YN1];
                w[V_AFC_CACHE + 14] = w[V_LOOP_YN2];
                afc_block(m, z, w, blk);
                unsigned left = 16 - (loop & 15);
                uint32_t used = left + loop;
                put32(w, V_REMAINING, len > used ? len - used : 0);
                for (int k = 0; k < 16; k++)
                    w[V_AFC_CACHE + k] = (uint16_t)blk[k];
                w[V_AFC_LEFT] = (uint16_t)left;
            } else {
                afc_block(m, z, w, blk);
                unsigned k = rem < 16 ? rem : 16;
                for (unsigned j = 0; j < 16; j++)
                    w[V_AFC_CACHE + j] = (uint16_t)blk[j];
                for (unsigned j = 0; k < 16 && j < k; j++) /* a short last block: its first k samples at the end */
                    w[V_AFC_CACHE + 16 - k + j] = (uint16_t)blk[j];
                put32(w, V_REMAINING, rem - k);
                w[V_AFC_LEFT] = (uint16_t)k;
            }
        }
        dst[i] = (int16_t)w[V_AFC_CACHE + 16 - w[V_AFC_LEFT]];
        w[V_AFC_LEFT]--;
    }
}

static void pcm16_from_mram(gc_machine *m, uint16_t *w, int16_t *dst, unsigned n)
{
    uint32_t a = get32(w, V_BASE) + 2u * w[V_POS];
    uint32_t rem = get32(w, V_REMAINING);
    if (n > rem) { /* the end: what is left, then its last sample held */
        int16_t last = 0;
        for (uint32_t i = 0; i < rem; i++)
            last = dst[i] = (int16_t)dsp_rd16(m, a + 2 * i);
        for (uint32_t i = rem; i < n; i++)
            dst[i] = last;
        w[V_POS] = (uint16_t)(w[V_POS] + rem);
        put32(w, V_REMAINING, 0);
        w[V_DONE] = 1;
        return;
    }
    put32(w, V_REMAINING, rem - n);
    uint16_t before = (uint16_t)(w[V_LOOP_START] - w[V_POS]);
    w[V_BEFORE_LOOP] = before;
    if (n <= before) {
        for (unsigned i = 0; i < n; i++)
            dst[i] = (int16_t)dsp_rd16(m, a + 2 * i);
        w[V_POS] = (uint16_t)(w[V_POS] + n);
    } else { /* the loop point: on from the loop address */
        for (unsigned i = 0; i < before; i++)
            dst[i] = (int16_t)dsp_rd16(m, a + 2 * i);
        uint32_t loop = get32(w, V_LOOP_ADDR);
        put32(w, V_BASE, loop);
        w[V_POS] = (uint16_t)(n - before);
        for (unsigned i = before; i < n; i++)
            dst[i] = (int16_t)dsp_rd16(m, loop + 2 * (i - before));
    }
}

/* raw (4 history samples, then the new ones) to 80 samples at the voice's ratio */
static void resample(const gc_zelda *z, uint16_t *w, const int16_t *raw, int16_t *out)
{
    uint32_t ratio = w[V_RATIO], pos = w[V_FRAC];
    if (ratio >= 0x4000) { /* 4:1 or more: the nearest sample */
        for (int i = 0; i < N; i++) {
            pos += ratio;
            out[i] = raw[pos >> 12];
        }
    } else {
        for (int i = 0; i < N; i++) {
            const int16_t *c = z->resample_coefs + 4 * ((pos & 0xFFF) >> 6), *x = raw + (pos >> 12);
            int64_t acc = 0;
            for (int k = 0; k < 4; k++)
                acc += 2 * (int64_t)c[k] * x[k];
            out[i] = sat16(acc >> 16);
            pos += ratio;
        }
    }
    for (int k = 0; k < 4; k++)
        w[V_HIST + k] = (uint16_t)raw[(pos >> 12) + k];
    w[V_CONST] = (uint16_t)out[N - 1];
    w[V_FRAC] = (uint16_t)(pos & 0xFFF);
}

static void load_samples(gc_machine *m, gc_zelda *z, uint16_t *w, int16_t *out)
{
    if (w[V_USE_CONST]) {
        for (int i = 0; i < N; i++)
            out[i] = (int16_t)w[V_CONST];
        return;
    }
    unsigned src = w[V_SOURCE];
    switch (src) {
    case SRC_SQUARE:
    case SRC_SQUARE_25: {
        unsigned sh = src == SRC_SQUARE ? 1 : 2, mask = (1u << sh) - 1;
        uint32_t ratio = (uint32_t)w[V_RATIO] << (sh - 1), pos = (uint32_t)w[V_FRAC] << sh;
        for (int i = 0; i < N; i++) {
            out[i] = ((pos >> 16) & mask) ? -0x4000 : 0x4000;
            pos += ratio;
        }
        w[V_FRAC] = (uint16_t)(pos >> sh);
        return;
    }
    case SRC_SAW: {
        uint32_t pos = w[V_FRAC];
        for (int i = 0; i < N; i++) {
            out[i] = (int16_t)(uint16_t)pos;
            pos += w[V_RATIO] >> 1;
        }
        w[V_FRAC] = (uint16_t)pos;
        return;
    }
    case SRC_PATTERN0:
    case SRC_PATTERN0_VAR:
    case SRC_PATTERN1:
    case SRC_PATTERN2:
    case SRC_PATTERN3: {
        int idx = src == SRC_PATTERN1 ? 1 : src == SRC_PATTERN2 ? 2 : src == SRC_PATTERN3 ? 3 : 0;
        const int16_t *pat = z->patterns + 0x40 * idx;
        const int64_t wrap = 0x40 << 16;
        int64_t pos = (int64_t)w[V_FRAC] << 6, step = (int64_t)w[V_RATIO] << 5;
        for (int i = 0; i < N; i++) {
            out[i] = pat[(pos >> 16) & 0x3F];
            pos = (pos + step) % wrap;
            if (src == SRC_PATTERN0_VAR) { /* the step modulated by the back right buffer */
                pos = ((pos << 10) + (int64_t)z->buf[ZB_BACK_R][i] * w[V_RATIO]) >> 10;
                pos = ((pos % wrap) + wrap) % wrap;
            }
        }
        w[V_FRAC] = (uint16_t)(pos >> 6);
        return;
    }
    case SRC_PCM8:
    case SRC_PCM16:
    case SRC_AFC_LQ:
    case SRC_AFC_HQ:
    case SRC_PCM16_MRAM: {
        int16_t raw[4 + 0x510 + 0x10];
        unsigned need = (w[V_FRAC] + (uint32_t)N * w[V_RATIO]) >> 12;
        for (int k = 0; k < 4; k++)
            raw[k] = (int16_t)w[V_HIST + k];
        if (src == SRC_PCM8 || src == SRC_PCM16)
            pcm_from_aram(m, w, raw + 4, need, src == SRC_PCM8 ? 1 : 2);
        else if (src == SRC_PCM16_MRAM)
            pcm16_from_mram(m, w, raw + 4, need);
        else
            afc_from_aram(m, z, w, raw + 4, need);
        resample(z, w, raw, out);
        return;
    }
    default:
        if (!z->warned_src) {
            fprintf(stderr, "gc: zelda: voice sample source %u not emulated (silent)\n", src);
            z->warned_src = true;
        }
        memset(out, 0, N * sizeof *out);
        return;
    }
}

/* ---- one voice ---- */

static uint32_t vpb_bytes(const gc_zelda *z)
{
    return (z->flags & ZF_TINY_VPB) ? 0x100 : 0x180;
}

/* The NTSC IPL's 0x80-word VPB moved into the full layout and back: no positional part, its read-only half at 0x40. */
static void vpb_expand(uint16_t *w)
{
    for (int i = 0; i < 0x40; i++) {
        w[0x80 + i] = w[0x40 + i];
        w[0x40 + i] = 0;
    }
    for (int i = 0; i < 0x10; i++) {
        w[0x58 + i] = w[0x30 + i];
        w[0x30 + i] = 0;
    }
    for (int i = 0; i < 0x18; i++) {
        w[0x30 + i] = w[0x18 + i];
        w[0x18 + i] = 0;
    }
}

static void vpb_shrink(uint16_t *w)
{
    for (int i = 0; i < 0x18; i++)
        w[0x18 + i] = w[0x30 + i];
    for (int i = 0; i < 0x10; i++)
        w[0x30 + i] = w[0x58 + i];
}

static void lowpass(uint16_t *w, int16_t *s)
{
    int32_t y1 = w[V_RESET] ? 0 : (int16_t)w[V_LP_Y1], x1 = w[V_RESET] ? 0 : (int16_t)w[V_LP_X1];
    int64_t k = w[V_LP_COEF];
    for (int i = 0; i < N; i++) {
        int32_t x = s[i];
        int16_t y = sat16((((int64_t)(x - x1) * k) >> 7) + y1);
        s[i] = y;
        y1 = y;
        x1 = x;
    }
    w[V_LP_Y1] = (uint16_t)y1;
    w[V_LP_X1] = (uint16_t)x1;
}

static void biquad(uint16_t *w, int16_t *s)
{
    int32_t x1 = (int16_t)w[V_BIQ_X1], x2 = (int16_t)w[V_BIQ_X2], y1 = (int16_t)w[V_BIQ_Y1], y2 = (int16_t)w[V_BIQ_Y2];
    int64_t b1 = (int16_t)w[V_BIQ_B1], b2 = (int16_t)w[V_BIQ_B2], a1 = (int16_t)w[V_BIQ_A1], a2 = (int16_t)w[V_BIQ_A2];
    for (int i = 0; i < N; i++) {
        int32_t x = s[i];
        int16_t y = sat16((b1 * x1 + b2 * x2 + a1 * y1 + a2 * y2) >> 15);
        s[i] = y;
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = y;
    }
    w[V_BIQ_X1] = (uint16_t)x1;
    w[V_BIQ_X2] = (uint16_t)x2;
    w[V_BIQ_Y1] = (uint16_t)y1;
    w[V_BIQ_Y2] = (uint16_t)y2;
}

/* Positional mixing: the voice's x / y (0..0x7F, 0x7F = left / front) through the sine table to four quadrants, a
 * ramp from the current to the target volume, and the same into the reverb buffers by the reverb factor. */
static void mix_dolby(gc_zelda *z, uint16_t *w, const int16_t *s)
{
    if (w[V_END_REQ]) {
        w[V_DOLBY_TARGET] = (uint16_t)((int16_t)w[V_DOLBY_CUR] / 2);
        if (!w[V_DOLBY_TARGET])
            w[V_DONE] = 1;
    }
    unsigned x = (w[V_DOLBY_POS] >> 8) & 0x7F, y = w[V_DOLBY_POS] & 0x7F;
    int32_t right = z->sine[x], back = z->sine[y], left = z->sine[x ^ 0x7F], front = z->sine[y ^ 0x7F];
    int sh = (z->flags & ZF_DOLBY_LOUDER) ? 15 : 16;
    int16_t cur = (int16_t)w[V_DOLBY_CUR], target = (int16_t)w[V_DOLBY_TARGET], rev = (int16_t)w[V_DOLBY_REVERB];
    int16_t delta = (int16_t)(target - cur);
    int16_t quad[4] = {(int16_t)((left * front) >> sh), (int16_t)((left * back) >> sh),
                       (int16_t)((right * front) >> sh), (int16_t)((right * back) >> sh)};
    static const uint8_t dry[4] = {ZB_FRONT_L, ZB_BACK_L, ZB_FRONT_R, ZB_BACK_R};
    static const uint8_t wet[4] = {ZB_FRONT_L_REV, ZB_BACK_L_REV, ZB_FRONT_R_REV, ZB_BACK_R_REV};
    for (int q = 0; q < 4; q++) {
        int16_t d = (int16_t)(((int32_t)(uint16_t)quad[q] * delta) >> sh);
        int16_t v = (int16_t)((quad[q] * (int32_t)cur) >> sh);
        int16_t rv = (int16_t)((v * (int32_t)rev) >> sh), rd = (int16_t)((d * (int32_t)rev) >> sh);
        mix_ramp(z->buf[dry[q]], s, v * 65536, d * 65536 / N);
        mix_ramp(z->buf[wet[q]], s, rv * 65536, rd * 65536 / N);
    }
    w[V_DOLBY_CUR] = (uint16_t)target;
}

static void mix_channels(gc_zelda *z, uint16_t *w, const int16_t *s)
{
    int n = (z->flags & ZF_FOUR_DESTS) ? 4 : 6;
    if (w[V_END_REQ]) { /* a quick fade out, then the voice is done */
        bool silent = true;
        for (int c = 0; c < n; c++) {
            uint16_t *ch = w + V_CHANNELS + 4 * c;
            ch[1] = (uint16_t)((int16_t)ch[2] / 2);
            silent &= ch[1] == 0;
        }
        if (silent)
            w[V_DONE] = 1;
    }
    for (int c = 0; c < n; c++) {
        uint16_t *ch = w + V_CHANNELS + 4 * c;
        if (!ch[0])
            continue;
        int16_t cur = (int16_t)ch[2];
        int16_t delta = (z->flags & ZF_EXPLICIT_STEP) ? (int16_t)ch[1] : (int16_t)((int16_t)ch[1] - cur);
        int32_t step = delta * 65536 / N;
        if (!cur && !step)
            continue;
        int16_t *dst = buffer_for(z, ch[0]);
        if (!dst)
            continue;
        ch[2] = (uint16_t)(mix_ramp(dst, s, cur * 65536, step) >> 16);
    }
}

static void add_voice(gc_machine *m, gc_zelda *z, unsigned id)
{
    uint16_t w[V_WORDS] = {0};
    uint32_t size = vpb_bytes(z), at = z->vpb_base + id * size;
    for (uint32_t i = 0; i < size / 2; i++)
        w[i] = dsp_rd16(m, at + 2 * i);
    if (z->flags & ZF_TINY_VPB)
        vpb_expand(w);
    if (!w[V_ENABLED] || w[V_DONE])
        return;
    int16_t s[N];
    load_samples(m, z, w, s);
    if (w[V_LP_COEF])
        lowpass(w, s);
    if ((w[V_FILTERS] & 0x20) && (w[V_BIQ_A1] || w[V_BIQ_A2] || w[V_BIQ_B2] || w[V_BIQ_B1] != 0x7FFF))
        biquad(w, s);
    if (w[V_USE_DOLBY])
        mix_dolby(z, w, s);
    else
        mix_channels(z, w, s);
    if (!w[V_USE_CONST])
        w[V_RESET] = 0;
    if (z->flags & ZF_TINY_VPB)
        vpb_shrink(w);
    for (uint32_t i = 0; i < size / 2 - 0x40; i++) /* the writable part: all but the last 0x40 words */
        dsp_wr16(m, at + 2 * i, w[i]);
    z->voices++;
}

/* ---- rendering and commands ---- */

static bool rendering(const gc_zelda *z)
{
    return z->cur_frame != z->req_frames;
}

static void render(gc_machine *m, gc_zelda *z)
{
    if (!rendering(z))
        return;
    unsigned voices = z->voices_per_frame < 256 * 16 ? z->voices_per_frame : 256 * 16;
    while (z->cur_frame < z->req_frames) {
        if (z->cur_voice == 0)
            prepare_frame(m, z);
        for (; z->cur_voice < voices; z->cur_voice++) {
            if (z->cur_voice >= z->sync_max_voice)
                return; /* the CPU has not released this voice yet */
            if (z->skip[z->cur_voice >> 4] & (1u << (15 - (z->cur_voice & 15))))
                add_voice(m, z, z->cur_voice);
        }
        if (!(z->flags & ZF_LIGHT))
            ack(m, z, (uint16_t)(0xFF00 | z->cur_frame));
        finalize_frame(m, z);
        m->audio->cmd_lists++;
        z->cur_voice = 0;
        z->sync_max_voice = 0;
        z->cur_frame++;
    }
    if (!(z->flags & ZF_LIGHT)) {
        gc_dsp_push_mail(m, DSP_MAIL_FRAME_END, true, ACK_DELAY);
        z->can_execute = false; /* until the CPU says 0xCDD10003 */
    } else {
        z->state = ZS_WAITING;
    }
}

static uint32_t cmd_pop(gc_zelda *z)
{
    if (z->rd == z->wr)
        return 0;
    uint32_t v = z->cmd[z->rd];
    z->rd = (z->rd + 1) % 64;
    return v;
}

static void cmd_push(gc_zelda *z, uint32_t v)
{
    z->cmd[z->wr] = v;
    z->wr = (z->wr + 1) % 64;
}

static void read_table(gc_machine *m, int16_t *dst, uint32_t addr, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        dst[i] = (int16_t)dsp_rd16(m, addr + 2 * i);
}

static void halt(gc_machine *m, gc_zelda *z, const char *why, uint32_t v)
{
    if (!z->warned_cmd) {
        fprintf(stderr, "gc: zelda: %s %08X: the ucode halts (as on hardware)\n", why, v);
        z->warned_cmd = true;
    }
    (void)m;
    z->state = ZS_HALTED;
}

static void run_commands(gc_machine *m, gc_zelda *z)
{
    if (rendering(z) || !z->can_execute)
        return;
    while (z->pending) {
        if (z->rd == z->wr) { /* counted commands but nothing queued: never spin on an empty ring */
            z->pending = 0;
            break;
        }
        uint32_t c = cmd_pop(z);
        if (!(c & 0x80000000u))
            continue; /* not a command's first word: skipped, as the ucode does */
        unsigned cmd = (c >> 24) & 0x7F;
        uint16_t sync = (uint16_t)(c >> 16), data = (uint16_t)c;
        z->pending--;
        switch (cmd) {
        case 0x00:
        case 0x0A:
        case 0x0B:
        case 0x0F:
            ack(m, z, sync);
            break;
        case 0x03:
            if (!(z->flags & ZF_LIGHT))
                ack(m, z, sync);
            break;
        case 0x01: {
            z->voices_per_frame = data;
            z->vpb_base = cmd_pop(z);
            uint32_t tables = cmd_pop(z);
            read_table(m, z->resample_coefs, tables, 0x100);
            read_table(m, z->patterns, tables + 0x200, 0x100);
            if (!(z->flags & ZF_LIGHT))
                read_table(m, z->sine, tables + 0x400, 0x80);
            read_table(m, z->afc_coefs, cmd_pop(z), 0x20);
            z->reverb_base = cmd_pop(z);
            ack(m, z, sync);
            break;
        }
        case 0x02:
            z->req_frames = (c >> 16) & 0xFF;
            z->out_vol = data;
            z->out_l = cmd_pop(z);
            z->out_r = cmd_pop(z);
            if (z->flags & ZF_COMBINED_0D) {
                cmd_pop(z);
                cmd_pop(z);
            }
            z->cur_frame = 0;
            z->cur_voice = 0;
            if (z->flags & ZF_LIGHT) {
                ack(m, z, (uint16_t)z->req_frames);
                z->state = ZS_RENDERING;
            } else {
                render(m, z);
            }
            return; /* the rest waits for the render */
        case 0x0C:
            if (z->flags & ZF_GBA_CRYPTO)
                cmd_pop(z); /* the GBA link's key exchange: not emulated */
            else if (z->flags & ZF_WEIRD_CMD_0C) {
                cmd_pop(z);
                cmd_pop(z);
            }
            ack(m, z, sync);
            break;
        case 0x0D:
            if (!(z->flags & ZF_NO_CMD_0D))
                cmd_pop(z);
            ack(m, z, sync);
            break;
        case 0x0E:
            cmd_pop(z); /* the Wii's ARAM base: no Wii here */
            ack(m, z, sync);
            break;
        default:
            halt(m, z, "command", c);
            return;
        }
    }
}

static void mail_standard(gc_machine *m, gc_zelda *z, uint32_t mail)
{
    gc_audio *a = m->audio;
    switch (z->state) {
    case ZS_WAITING:
        if (mail & 0x80000000u) { /* a task mail (the ucode reads its low half only) */
            switch (mail & 0xFFFF) {
            case 0x0001: /* a new ucode: what is queued runs first */
                z->can_execute = true;
                run_commands(m, z);
                a->uploading = true;
                a->up_step = 0;
                break;
            case 0x0002:
                z->state = ZS_HALTED;
                gc_dsp_set_ucode(m, UC_ROM, 0);
                break;
            case 0x0003:
                z->can_execute = true;
                run_commands(m, z);
                break;
            default: /* 0xCDD10000 and the rest stop the ucode */
                halt(m, z, "task mail", mail);
                break;
            }
        } else if (!(mail & 0xFFFF)) {
            if (rendering(z))
                z->state = ZS_RENDERING;
            else
                halt(m, z, "sync mail outside a render", mail);
        } else {
            z->state = ZS_WRITING_CMD;
            z->expect = mail & 0xFFFF;
        }
        break;
    case ZS_RENDERING:
        if (z->flags & ZF_SYNC_PER_FRAME) {
            int at = z->sync_second_half ? 2 : 0;
            z->skip[at] = (uint16_t)(mail >> 16);
            z->skip[at + 1] = (uint16_t)mail;
            if (z->sync_second_half)
                z->sync_max_voice = 0xFFFF;
            render(m, z);
            if (z->sync_second_half)
                z->state = ZS_WAITING;
            z->sync_second_half = !z->sync_second_half;
        } else {
            z->sync_max_voice = (((mail >> 16) & 0xF) + 1) << 4;
            z->skip[(mail >> 16) & 0xFF] = (uint16_t)mail;
            render(m, z);
            z->state = ZS_WAITING;
        }
        break;
    case ZS_WRITING_CMD:
        cmd_push(z, mail);
        if (--z->expect == 0) {
            z->pending++;
            z->state = ZS_WAITING;
            run_commands(m, z);
        }
        break;
    default:
        if (m->verbose)
            fprintf(stderr, "gc: zelda: mail %08X while halted\n", mail);
        break;
    }
}

static void mail_light(gc_machine *m, gc_zelda *z, uint32_t mail)
{
    switch (z->state) {
    case ZS_WAITING: {
        bool add = true;
        if (!(mail & 0x80000000u)) { /* not a command's first word: dropped (queued, it would never run) */
            if (m->verbose)
                fprintf(stderr, "gc: zelda: light mail %08X outside a command, ignored\n", mail);
            break;
        }
        switch ((mail >> 24) & 0x7F) { /* each command's word count is fixed */
        case 0x00: z->expect = 0; break;
        case 0x01: z->expect = 4; break;
        case 0x02: z->expect = 2; break;
        case 0x03: z->expect = 0; add = false; break; /* back to the dispatcher, not queued, no acknowledgement */
        case 0x0C: z->expect = (z->flags & ZF_GBA_CRYPTO) ? 1 : (z->flags & ZF_WEIRD_CMD_0C) ? 2 : 0; break;
        default:
            z->expect = 0;
            if (m->verbose)
                fprintf(stderr, "gc: zelda: unknown light command %08X\n", mail);
            break;
        }
        if (add)
            cmd_push(z, mail);
        if (z->expect)
            z->state = ZS_WRITING_CMD;
        else if (add) {
            z->pending++;
            run_commands(m, z);
        }
        break;
    }
    case ZS_WRITING_CMD:
        cmd_push(z, mail);
        if (--z->expect == 0) {
            z->pending++;
            z->state = ZS_WAITING;
            run_commands(m, z);
        }
        break;
    case ZS_RENDERING: /* one (zero) mail renders every frame: no voice syncs in the light protocol */
        z->sync_max_voice = 0xFFFFFFFFu;
        memset(z->skip, 0xFF, sizeof z->skip);
        render(m, z);
        gc_dsp_schedule_int(m, ACK_DELAY);
        break;
    default:
        if (m->verbose)
            fprintf(stderr, "gc: zelda: mail %08X while halted\n", mail);
        break;
    }
}

/* ---- entry points ---- */

void gc_zelda_init(gc_machine *m, uint32_t crc)
{
    gc_zelda *z = &m->audio->zelda;
    memset(z, 0, sizeof *z);
    z->can_execute = true;
    z->flags = ZF_DOLBY_LOUDER; /* a version not in the table: the most common later one */
    for (size_t i = 0; i < sizeof k_versions / sizeof k_versions[0]; i++)
        if (k_versions[i].crc == crc)
            z->flags = k_versions[i].flags;
    if (z->flags & ZF_LIGHT) {
        gc_dsp_push_mail(m, 0x88881111u, false, 0);
    } else {
        gc_dsp_push_mail(m, DSP_MAIL_INIT, true, 0);
        gc_dsp_push_mail(m, 0xF3551111u, false, 0);
    }
}

void gc_zelda_mail(gc_machine *m, uint32_t mail)
{
    gc_zelda *z = &m->audio->zelda;
    if (z->flags & ZF_LIGHT)
        mail_light(m, z, mail);
    else
        mail_standard(m, z, mail);
}
