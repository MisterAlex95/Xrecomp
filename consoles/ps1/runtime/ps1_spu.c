/* ps1_spu.c -- the PlayStation SPU (P-8, XBA-166). Behaviour from psx-spx « Sound Processing Unit (SPU) »; our own
 * code (ideas from ares, ISC).
 *
 * One sample every 768 CPU cycles (44100 Hz): noise generator, 24 voices (SPU-ADPCM blocks of 28 samples with the
 * five prediction filters, loop start / end / repeat flags, ENDX, pitch counter with pitch modulation by the previous
 * voice, 4-point Gaussian interpolation with the hardware table, ADSR envelope, sweep or fixed L / R volume), the CD
 * input (ps1_cdrom_audio_pull, one frame per sample, CD volume), capture buffers (CD left / right before volume, voice
 * 1 and 3 after ADSR, at 0x000 / 0x400 / 0x800 / 0xC00 of SPU RAM, 512 samples each), reverb, main volume (sweep or
 * fixed). The SPU IRQ (I_STAT bit 9) fires when enabled (SPUCNT bits 15 and 6) and a voice reads the ADPCM block
 * holding the IRQ address, a transfer or capture write touches it, or a reverb access touches it.
 *
 * Envelopes (ADSR and sweep volumes) follow psx-spx « Envelope Operation depending on Shift/Step/Mode/Direction »
 * exactly (step, counter increment, exponential increase above 6000h and exponential decrease, the all-ones rate that
 * never steps). Reverb follows psx-spx « SPU Reverb Formula » at 22050 Hz (left on one half, right on the other,
 * computed together here), work area from ESA to 7FFFEh, with the 39-tap FIR (psx-spx « Reverb Buffer Resampling »)
 * to downsample the input and upsample the output. Transfers: manual writes through the 32-halfword FIFO (flushed
 * when SPUCNT selects manual write), DMA4 write and read; the transfer type in 1F801DACh is stored but only the normal
 * type (2 = 0004h) is modelled.
 *
 * Mix (per sample, psx-spx « SPU Overview » / Mednafen / DuckStation): voices + CD (times the CD volume, when SPUCNT
 * bit 0) + reverb output (times EVOL), clamped, then times the main volume: the main volume applies to the CD input
 * and to the reverb too (CD audio has no path around the mixer). SPUCNT bit 14 = 0 mutes the voices and the reverb
 * output, not the CD.
 *
 * Timing. Register writes land between two samples: the core brings the SPU up to the CPU's cycle before every SPU
 * access (ps1_devices_sync), and the SPU only changes state on its 44.1 kHz ticks. KON and KOFF are latched and
 * applied on the next tick (KOFF first, so both in one tick restart the voice). Key on: the voice decodes its first
 * block, clears ENDX, then stays silent for four samples (Mednafen's decode-play delay: the pitch counter and the
 * envelope hold) before its attack starts. Reverb: the same-side / different-side reflections use the hardware's
 * IIR form (input x vIIR + previous x (8000h - vIIR), halved from two 14-bit products), including the vIIR = -8000h
 * quirk where the second factor overflows and the previous sample's sign flips (Mednafen's IIASM). Not modelled: the
 * hardware's per-voice stagger within a sample, the transfer types other than normal, the external audio input. */
#include "ps1_devices.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define RAM_SIZE (512 * 1024)
#define RAM_MASK (RAM_SIZE - 1)
#define CYCLES_PER_SAMPLE 768u
#define KON_DELAY 4 /* samples between a key on and the voice's first output (Mednafen) */
#define OUT_RING 8192u /* frames, power of two: more than 8 video frames at 44.1 kHz */

static const int16_t gauss[512] = {
#include "ps1_spu_gauss.inc"
};

/* SPU-ADPCM prediction filters (psx-spx « SPU ADPCM Samples »): pos, neg (/ 64). */
static const int8_t filt_pos[5] = {0, 60, 115, 98, 122};
static const int8_t filt_neg[5] = {0, 0, -52, -55, -60};

/* 39-tap reverb resampling FIR, psx-spx « Reverb Buffer Resampling ». */
static const int16_t rev_fir[39] = {
    -0x0001, 0, 0x0002, 0, -0x000A, 0, 0x0023, 0, -0x0067, 0, 0x010A, 0, -0x0268, 0, 0x0534, 0, -0x0B90, 0, 0x2806,
    0x4000, 0x2806, 0, -0x0B90, 0, 0x0534, 0, -0x0268, 0, 0x010A, 0, -0x0067, 0, 0x0023, 0, -0x000A, 0, 0x0002, 0,
    -0x0001,
};

enum { PH_OFF, PH_ATTACK, PH_DECAY, PH_SUSTAIN, PH_RELEASE };

typedef struct sweep_vol {
    uint16_t reg;   /* as written */
    int16_t level;  /* current (VOLX) */
    int32_t counter;
} sweep_vol;

typedef struct voice {
    sweep_vol vol[2];
    uint16_t pitch;
    uint16_t start;  /* x8 */
    uint16_t adsr1, adsr2;
    uint16_t repeat; /* x8 */
    int16_t env;     /* ENVX */
    int phase;
    int32_t env_counter;
    uint32_t addr;     /* current block, bytes */
    uint32_t counter;  /* pitch counter: bits 12+ sample, 4..11 interpolation */
    int16_t s[28 + 3]; /* s[0..2]: last three samples of the previous block; s[3..30]: this block */
    int16_t old, older;
    uint8_t flags;     /* this block's flag byte */
    int16_t out;       /* output after ADSR (pitch modulation of the next voice, capture) */
    int delay;         /* samples of silence left after a key on */
} voice;

struct ps1_spu {
    ps1_irq_sink irq;
    uint8_t *ram;
    voice v[24];
    sweep_vol main[2];
    int16_t evol[2];
    uint32_t kon, koff, pmon, non, eon, endx;
    uint16_t kon_reg[2], koff_reg[2];
    uint16_t esa;
    uint16_t irq_addr;
    uint16_t tsa;
    uint32_t taddr; /* current transfer address, bytes */
    uint16_t fifo[32];
    int fifo_n;
    uint16_t cnt;
    uint16_t stat_irq; /* STAT bit 6 */
    uint16_t tctrl;    /* 1F801DACh */
    int16_t cd_vol[2], ext_vol[2];
    uint16_t rev[32];  /* 1F801DC0h.. */
    uint16_t regs[0x200]; /* raw copy for the registers without behaviour (reads back what was written) */

    int32_t noise_timer;
    uint16_t noise;
    uint32_t capture; /* 0..511 */
    uint32_t cycles;

    /* reverb */
    uint32_t rev_addr; /* current buffer address, bytes */
    int16_t rev_in[2][39];  /* the last 39 reverb inputs (44.1 kHz), newest at [38] */
    int16_t rev_out[2][20]; /* reverb outputs (22.05 kHz), newest at [19] */
    int rev_phase;

    int16_t ring[OUT_RING * 2];
    uint32_t ring_head, ring_count;
};

/* ---- helpers ------------------------------------------------------------------------------------------------ */

static inline int16_t clamp16(int32_t v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

static void irq_hit(ps1_spu *s, uint32_t addr)
{
    if ((s->cnt & 0x8040) != 0x8040 || s->stat_irq)
        return;
    if (((addr & RAM_MASK) >> 3) == s->irq_addr) {
        s->stat_irq = 1;
        s->irq.raise(s->irq.ctx, PS1_IRQ_SPU);
    }
}

/* A 16-byte ADPCM block read: the IRQ fires when its address is inside. */
static void irq_block(ps1_spu *s, uint32_t addr)
{
    if ((s->cnt & 0x8040) != 0x8040 || s->stat_irq)
        return;
    uint32_t ia = (uint32_t)s->irq_addr << 3;
    if (ia >= addr && ia < addr + 16) {
        s->stat_irq = 1;
        s->irq.raise(s->irq.ctx, PS1_IRQ_SPU);
    }
}

static inline uint16_t ram16(const ps1_spu *s, uint32_t a)
{
    a &= RAM_MASK & ~1u;
    return (uint16_t)(s->ram[a] | (s->ram[a + 1] << 8));
}

static inline void wram16(ps1_spu *s, uint32_t a, uint16_t v)
{
    a &= RAM_MASK & ~1u;
    s->ram[a] = (uint8_t)v;
    s->ram[a + 1] = (uint8_t)(v >> 8);
}

/* ---- envelopes ---------------------------------------------------------------------------------------------- */

/* One 44.1 kHz step of an envelope (psx-spx « Envelope Operation »). rate_allones: the rate bits are all ones, so the
 * counter never advances. */
static int16_t env_step(int32_t level, int32_t *counter, int exp, int dec, int neg, int shift, int step,
                        int rate_allones)
{
    int32_t st = 7 - step;
    if (dec ^ neg)
        st = ~st;
    st = (int32_t)((uint32_t)st << (shift < 11 ? 11 - shift : 0));
    int32_t inc = 0x8000 >> (shift > 11 ? shift - 11 : 0);
    if (exp && !dec && level > 0x6000) {
        if (shift < 10) {
            st >>= 2;
        } else if (shift >= 11) {
            inc >>= 2;
        } else {
            st >>= 1;
            inc >>= 1;
        }
    } else if (exp && dec) {
        st = (st * level) >> 15; /* arithmetic: a falling level still reaches 0 */
    }
    if (!rate_allones && inc < 1)
        inc = 1;
    if (rate_allones)
        return (int16_t)level;
    *counter += inc;
    if (!(*counter & 0x8000))
        return (int16_t)level;
    *counter = 0;
    level += st;
    if (!dec) {
        level = level < -0x8000 ? -0x8000 : level > 0x7FFF ? 0x7FFF : level;
    } else if (neg) {
        level = level < -0x8000 ? -0x8000 : level > 0 ? 0 : level;
    } else if (level < 0) {
        level = 0;
    }
    return (int16_t)level;
}

static void sweep_tick(sweep_vol *v)
{
    uint16_t r = v->reg;
    if (!(r & 0x8000)) {
        v->level = (int16_t)(r << 1);
        return;
    }
    int exp = (r >> 14) & 1, dec = (r >> 13) & 1, neg = (r >> 12) & 1, shift = (r >> 2) & 31, step = r & 3;
    v->level = env_step(v->level, &v->counter, exp, dec, neg, shift, step, (r & 0x7F) == 0x7F);
}

static void adsr_tick(voice *v)
{
    int32_t lvl = v->env;
    /* decay ends once the level's top four bits are at or below the sustain level (checked before stepping) */
    if (v->phase == PH_DECAY && (lvl >> 11) <= (v->adsr1 & 15)) {
        v->phase = PH_SUSTAIN;
        v->env_counter = 0;
    }
    switch (v->phase) {
    case PH_ATTACK: {
        int shift = (v->adsr1 >> 10) & 31, step = (v->adsr1 >> 8) & 3;
        lvl = env_step(lvl, &v->env_counter, v->adsr1 >> 15, 0, 0, shift, step, ((shift << 2) | step) == 0x7F);
        if (lvl >= 0x7FFF) {
            v->phase = PH_DECAY;
            v->env_counter = 0;
        }
        break;
    }
    case PH_DECAY: {
        int shift = (v->adsr1 >> 4) & 15;
        lvl = env_step(lvl, &v->env_counter, 1, 1, 0, shift, 0, 0);
        break;
    }
    case PH_SUSTAIN: {
        int shift = (v->adsr2 >> 8) & 31, step = (v->adsr2 >> 6) & 3;
        lvl = env_step(lvl, &v->env_counter, v->adsr2 >> 15, (v->adsr2 >> 14) & 1, 0, shift, step,
                       ((shift << 2) | step) == 0x7F);
        break;
    }
    case PH_RELEASE: {
        int shift = v->adsr2 & 31;
        lvl = env_step(lvl, &v->env_counter, (v->adsr2 >> 5) & 1, 1, 0, shift, 0, shift == 31);
        if (lvl <= 0) {
            lvl = 0;
            v->phase = PH_OFF;
        }
        break;
    }
    default: break;
    }
    v->env = (int16_t)lvl;
}

/* ---- voices ------------------------------------------------------------------------------------------------- */

static void decode_block(ps1_spu *s, voice *v)
{
    uint32_t a = v->addr & RAM_MASK;
    irq_block(s, a);
    const uint8_t *b = s->ram + a;
    int shift = b[0] & 15, filter = (b[0] >> 4) & 7;
    if (shift > 12)
        shift = 9;
    if (filter > 4)
        filter = 4;
    v->flags = b[1];
    if (v->flags & 4)
        v->repeat = (uint16_t)(a >> 3);
    v->s[0] = v->s[28];
    v->s[1] = v->s[29];
    v->s[2] = v->s[30];
    int32_t old = v->old, older = v->older;
    for (int i = 0; i < 28; i++) {
        uint8_t byte = b[2 + (i >> 1)];
        int32_t n = (int16_t)(((i & 1) ? (byte >> 4) : (byte & 15)) << 12);
        int32_t x = (n >> shift) + ((old * filt_pos[filter] + older * filt_neg[filter] + 32) >> 6);
        int16_t y = clamp16(x);
        v->s[3 + i] = y;
        older = old;
        old = y;
    }
    v->old = (int16_t)old;
    v->older = (int16_t)older;
}

static void key_on(ps1_spu *s, int i)
{
    voice *v = &s->v[i];
    v->addr = (uint32_t)v->start << 3;
    v->counter = 0;
    v->old = v->older = 0;
    memset(v->s, 0, sizeof v->s);
    v->phase = PH_ATTACK;
    v->env = 0;
    v->env_counter = 0;
    v->delay = KON_DELAY;
    s->endx &= ~(1u << i);
    decode_block(s, v);
}

static void key_off(ps1_spu *s, int i)
{
    voice *v = &s->v[i];
    if (v->phase != PH_OFF) {
        v->phase = PH_RELEASE;
        v->env_counter = 0;
    }
}

/* One sample of voice i: returns its output after ADSR (before the L / R volumes). */
static int16_t voice_sample(ps1_spu *s, int i, int16_t prev_out)
{
    voice *v = &s->v[i];
    int32_t sample;
    if (v->delay) {
        v->delay--;
        v->out = 0;
        return 0;
    }
    if (s->non & (1u << i)) {
        sample = (int16_t)s->noise;
    } else {
        uint32_t idx = v->counter >> 12, g = (v->counter >> 4) & 0xFF;
        sample = (gauss[0x0FF - g] * v->s[idx]) >> 15;
        sample += (gauss[0x1FF - g] * v->s[idx + 1]) >> 15;
        sample += (gauss[0x100 + g] * v->s[idx + 2]) >> 15;
        sample += (gauss[0x000 + g] * v->s[idx + 3]) >> 15;
    }
    /* pitch counter (ADPCM keeps running for noise voices and silent ones: their reads trigger IRQs too) */
    int32_t step = v->pitch;
    if (i > 0 && (s->pmon & (1u << i))) {
        int32_t factor = (int32_t)prev_out + 0x8000;
        step = (int32_t)(int16_t)step; /* the hardware's sign glitch on pitches above 7FFFh */
        step = (step * factor) >> 15;
        step &= 0xFFFF;
    }
    if (step > 0x3FFF)
        step = 0x4000;
    v->counter += (uint32_t)step;
    while ((v->counter >> 12) >= 28) {
        v->counter -= 28u << 12;
        if (v->flags & 1) {
            s->endx |= 1u << i;
            v->addr = (uint32_t)v->repeat << 3;
            if (!(v->flags & 2) && !(s->non & (1u << i))) {
                v->phase = PH_RELEASE; /* end + mute */
                v->env = 0;
            }
        } else {
            v->addr = (v->addr + 16) & RAM_MASK;
        }
        decode_block(s, v);
    }
    adsr_tick(v);
    v->out = (int16_t)((sample * v->env) >> 15);
    return v->out;
}

/* ---- reverb ------------------------------------------------------------------------------------------------- */

enum { dAPF1, dAPF2, vIIR, vCOMB1, vCOMB2, vCOMB3, vCOMB4, vWALL, vAPF1, vAPF2, mLSAME, mRSAME, mLCOMB1, mRCOMB1,
       mLCOMB2, mRCOMB2, dLSAME, dRSAME, mLDIFF, mRDIFF, mLCOMB3, mRCOMB3, mLCOMB4, mRCOMB4, dLDIFF, dRDIFF, mLAPF1,
       mRAPF1, mLAPF2, mRAPF2, vLIN, vRIN };

static uint32_t rev_ea(const ps1_spu *s, int32_t off_bytes)
{
    uint32_t base = (uint32_t)s->esa << 3, size = RAM_SIZE - base;
    if (!size)
        return base;
    int64_t a = (int64_t)s->rev_addr - base + off_bytes;
    a %= (int64_t)size;
    if (a < 0)
        a += size;
    return (uint32_t)(base + (uint32_t)a) & ~1u;
}

static int16_t rev_rd(ps1_spu *s, int32_t off)
{
    uint32_t a = rev_ea(s, off);
    irq_hit(s, a);
    return (int16_t)ram16(s, a);
}

static void rev_wr(ps1_spu *s, int32_t off, int32_t v)
{
    uint32_t a = rev_ea(s, off);
    irq_hit(s, a);
    if (s->cnt & 0x80) /* reverb master enable: writes only */
        wram16(s, a, (uint16_t)clamp16(v));
}

#define R(x) ((int32_t)(int16_t)s->rev[x])
#define M(x) ((int32_t)s->rev[x] << 3)
#define MUL(a, b) (((int32_t)(a) * (int32_t)(b)) >> 15)

/* The reflections' IIR, as the hardware computes it (Mednafen): (in x vIIR + prev x (8000h - vIIR)) / 8000h, from two
 * 14-bit products halved. vIIR = -8000h overflows the second factor: prev x -10000h (0 for prev = -8000h). */
static int32_t iir(int32_t in, int32_t prev, int32_t alpha)
{
    in = clamp16(in);
    int32_t b;
    if (alpha == -32768)
        b = prev == -32768 ? 0 : prev * -65536;
    else
        b = prev * (32768 - alpha);
    return clamp16((((in * alpha) >> 14) + (b >> 14)) >> 1);
}

/* One 22.05 kHz reverb step: inputs lin / rin (already through the FIR); outputs to out[2]. */
static void reverb_step(ps1_spu *s, int32_t lin, int32_t rin, int32_t out[2])
{
    int32_t Lin = MUL(lin, R(vLIN)), Rin = MUL(rin, R(vRIN));
    if (s->cnt & 0x80) {
        /* same side reflection */
        int32_t ls = rev_rd(s, M(mLSAME) - 2), rs = rev_rd(s, M(mRSAME) - 2);
        rev_wr(s, M(mLSAME), iir(Lin + MUL(rev_rd(s, M(dLSAME)), R(vWALL)), ls, R(vIIR)));
        rev_wr(s, M(mRSAME), iir(Rin + MUL(rev_rd(s, M(dRSAME)), R(vWALL)), rs, R(vIIR)));
        /* different side reflection */
        int32_t ld = rev_rd(s, M(mLDIFF) - 2), rd = rev_rd(s, M(mRDIFF) - 2);
        rev_wr(s, M(mLDIFF), iir(Lin + MUL(rev_rd(s, M(dRDIFF)), R(vWALL)), ld, R(vIIR)));
        rev_wr(s, M(mRDIFF), iir(Rin + MUL(rev_rd(s, M(dLDIFF)), R(vWALL)), rd, R(vIIR)));
    }
    /* early echo */
    int32_t L = MUL(R(vCOMB1), rev_rd(s, M(mLCOMB1))) + MUL(R(vCOMB2), rev_rd(s, M(mLCOMB2))) +
                MUL(R(vCOMB3), rev_rd(s, M(mLCOMB3))) + MUL(R(vCOMB4), rev_rd(s, M(mLCOMB4)));
    int32_t Rr = MUL(R(vCOMB1), rev_rd(s, M(mRCOMB1))) + MUL(R(vCOMB2), rev_rd(s, M(mRCOMB2))) +
                 MUL(R(vCOMB3), rev_rd(s, M(mRCOMB3))) + MUL(R(vCOMB4), rev_rd(s, M(mRCOMB4)));
    /* APF1 */
    int32_t t = rev_rd(s, M(mLAPF1) - M(dAPF1));
    L = clamp16(L - MUL(R(vAPF1), t));
    rev_wr(s, M(mLAPF1), L);
    L = MUL(L, R(vAPF1)) + t;
    t = rev_rd(s, M(mRAPF1) - M(dAPF1));
    Rr = clamp16(Rr - MUL(R(vAPF1), t));
    rev_wr(s, M(mRAPF1), Rr);
    Rr = MUL(Rr, R(vAPF1)) + t;
    /* APF2 */
    t = rev_rd(s, M(mLAPF2) - M(dAPF2));
    L = clamp16(L - MUL(R(vAPF2), t));
    rev_wr(s, M(mLAPF2), L);
    L = MUL(L, R(vAPF2)) + t;
    t = rev_rd(s, M(mRAPF2) - M(dAPF2));
    Rr = clamp16(Rr - MUL(R(vAPF2), t));
    rev_wr(s, M(mRAPF2), Rr);
    Rr = MUL(Rr, R(vAPF2)) + t;
    out[0] = clamp16(L);
    out[1] = clamp16(Rr);
    /* next buffer address */
    uint32_t base = (uint32_t)s->esa << 3;
    uint32_t na = (s->rev_addr + 2) & 0x7FFFE;
    s->rev_addr = na < base ? base : na;
}

/* Reverb for one 44.1 kHz sample: input in, output out (both before EVOL). */
static void reverb(ps1_spu *s, int32_t inl, int32_t inr, int32_t out[2])
{
    for (int c = 0; c < 2; c++) {
        memmove(s->rev_in[c], s->rev_in[c] + 1, 38 * sizeof(int16_t));
        s->rev_in[c][38] = clamp16(c ? inr : inl);
    }
    s->rev_phase ^= 1;
    if (s->rev_phase) {
        int32_t in[2];
        for (int c = 0; c < 2; c++) {
            int32_t acc = 0;
            for (int k = 0; k < 39; k += 2) /* odd taps are zero, but for the centre (19) */
                acc += rev_fir[k] * s->rev_in[c][k];
            acc += rev_fir[19] * s->rev_in[c][19];
            in[c] = clamp16(acc >> 15);
        }
        int32_t o[2];
        reverb_step(s, in[0], in[1], o);
        for (int c = 0; c < 2; c++) {
            memmove(s->rev_out[c], s->rev_out[c] + 1, 19 * sizeof(int16_t));
            s->rev_out[c][19] = (int16_t)o[c];
        }
    }
    /* upsample: zero-stuffed 22.05 kHz stream through the FIR (gain 2). On the sample phase only the centre tap
     * lands on a value; between samples the even taps (non-zero) do. */
    for (int c = 0; c < 2; c++) {
        int32_t acc;
        if (s->rev_phase) {
            acc = rev_fir[19] * s->rev_out[c][9] * 2; /* the sample half the filter back */
        } else {
            acc = 0;
            for (int k = 0; k < 20; k++)
                acc += rev_fir[2 * k] * s->rev_out[c][k] * 2;
        }
        out[c] = clamp16(acc >> 15);
    }
}

/* ---- the sample loop ---------------------------------------------------------------------------------------- */

static void noise_tick(ps1_spu *s)
{
    int shift = (s->cnt >> 10) & 15, step = ((s->cnt >> 8) & 3) + 4;
    s->noise_timer -= step;
    uint16_t n = s->noise;
    int parity = ((n >> 15) ^ (n >> 12) ^ (n >> 11) ^ (n >> 10) ^ 1) & 1;
    if (s->noise_timer < 0) {
        s->noise = (uint16_t)(n * 2 + parity);
        s->noise_timer += 0x20000 >> shift;
        if (s->noise_timer < 0)
            s->noise_timer += 0x20000 >> shift;
    }
}

static void capture_write(ps1_spu *s, uint32_t base, int16_t v)
{
    uint32_t a = base + s->capture * 2;
    wram16(s, a, (uint16_t)v);
    if (s->tctrl & 0x0C)
        irq_hit(s, a);
}

static void sample(ps1_spu *s, ps1_cdrom *cd)
{
    noise_tick(s);
    int32_t dry[2] = {0, 0}, wet[2] = {0, 0};
    int16_t prev = 0;
    for (int i = 0; i < 24; i++) {
        voice *v = &s->v[i];
        if (s->koff & (1u << i)) {
            s->koff &= ~(1u << i);
            key_off(s, i);
        }
        if (s->kon & (1u << i)) {
            s->kon &= ~(1u << i);
            key_on(s, i);
        }
        int16_t o = voice_sample(s, i, prev);
        prev = o;
        sweep_tick(&v->vol[0]);
        sweep_tick(&v->vol[1]);
        int32_t l = (o * v->vol[0].level) >> 15, r = (o * v->vol[1].level) >> 15;
        dry[0] += l;
        dry[1] += r;
        if (s->eon & (1u << i)) {
            wet[0] += l;
            wet[1] += r;
        }
    }
    int16_t cdf[2] = {0, 0};
    if (cd)
        ps1_cdrom_audio_pull(cd, cdf, 1);
    capture_write(s, 0x000, cdf[0]);
    capture_write(s, 0x400, cdf[1]);
    capture_write(s, 0x800, s->v[1].out);
    capture_write(s, 0xC00, s->v[3].out);
    s->capture = (s->capture + 1) & 511;

    if (!(s->cnt & 0x8000) || !(s->cnt & 0x4000)) {
        dry[0] = dry[1] = 0; /* off or muted: the voices are silent (they keep running) */
        wet[0] = wet[1] = 0;
    }
    int32_t cdl = (cdf[0] * s->cd_vol[0]) >> 15, cdr = (cdf[1] * s->cd_vol[1]) >> 15;
    int32_t mixl = clamp16(dry[0]), mixr = clamp16(dry[1]);
    if (s->cnt & 0x0001) {
        mixl += cdl;
        mixr += cdr;
        if (s->cnt & 0x0004) {
            wet[0] += cdl;
            wet[1] += cdr;
        }
    }
    int32_t rv[2];
    reverb(s, clamp16(wet[0]), clamp16(wet[1]), rv);
    if (s->cnt & 0x4000) {
        mixl += (rv[0] * s->evol[0]) >> 15;
        mixr += (rv[1] * s->evol[1]) >> 15;
    }
    sweep_tick(&s->main[0]);
    sweep_tick(&s->main[1]);
    int32_t outl = (clamp16(mixl) * s->main[0].level) >> 15;
    int32_t outr = (clamp16(mixr) * s->main[1].level) >> 15;
    if (s->ring_count == OUT_RING) {
        s->ring_head = (s->ring_head + 1) & (OUT_RING - 1);
        s->ring_count--;
    }
    uint32_t w = (s->ring_head + s->ring_count) & (OUT_RING - 1);
    s->ring[2 * w] = clamp16(outl);
    s->ring[2 * w + 1] = clamp16(outr);
    s->ring_count++;
}

/* ---- transfers ---------------------------------------------------------------------------------------------- */

static void transfer_write(ps1_spu *s, uint16_t v)
{
    irq_hit(s, s->taddr);
    wram16(s, s->taddr, v);
    s->taddr = (s->taddr + 2) & RAM_MASK;
}

static uint16_t transfer_read(ps1_spu *s)
{
    irq_hit(s, s->taddr);
    uint16_t v = ram16(s, s->taddr);
    s->taddr = (s->taddr + 2) & RAM_MASK;
    return v;
}

static void fifo_flush(ps1_spu *s)
{
    for (int i = 0; i < s->fifo_n; i++)
        transfer_write(s, s->fifo[i]);
    s->fifo_n = 0;
}

/* ---- public ------------------------------------------------------------------------------------------------- */

ps1_spu *ps1_spu_create(ps1_irq_sink irq)
{
    ps1_spu *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->ram = calloc(1, RAM_SIZE);
    if (!s->ram) {
        free(s);
        return NULL;
    }
    s->irq = irq;
    s->tctrl = 4;
    return s;
}

void ps1_spu_destroy(ps1_spu *s)
{
    if (!s)
        return;
    free(s->ram);
    free(s);
}

uint16_t ps1_spu_read16(ps1_spu *s, uint32_t off)
{
    off &= 0x3FE;
    if (off < 0x180) {
        const voice *v = &s->v[off >> 4];
        switch (off & 15) {
        case 0x0: return v->vol[0].reg;
        case 0x2: return v->vol[1].reg;
        case 0x4: return v->pitch;
        case 0x6: return v->start;
        case 0x8: return v->adsr1;
        case 0xA: return v->adsr2;
        case 0xC: return (uint16_t)v->env;
        default: return v->repeat;
        }
    }
    if (off >= 0x200 && off < 0x260) {
        const voice *v = &s->v[(off - 0x200) >> 2];
        return (uint16_t)v->vol[(off >> 1) & 1].level;
    }
    if (off >= 0x1C0 && off < 0x200)
        return s->rev[(off - 0x1C0) >> 1];
    switch (off) {
    case 0x180: return s->main[0].reg;
    case 0x182: return s->main[1].reg;
    case 0x184: return (uint16_t)s->evol[0];
    case 0x186: return (uint16_t)s->evol[1];
    case 0x188: return s->kon_reg[0];
    case 0x18A: return s->kon_reg[1];
    case 0x18C: return s->koff_reg[0];
    case 0x18E: return s->koff_reg[1];
    case 0x190: return (uint16_t)s->pmon;
    case 0x192: return (uint16_t)(s->pmon >> 16);
    case 0x194: return (uint16_t)s->non;
    case 0x196: return (uint16_t)(s->non >> 16);
    case 0x198: return (uint16_t)s->eon;
    case 0x19A: return (uint16_t)(s->eon >> 16);
    case 0x19C: return (uint16_t)s->endx;
    case 0x19E: return (uint16_t)(s->endx >> 16);
    case 0x1A2: return s->esa;
    case 0x1A4: return s->irq_addr;
    case 0x1A6: return s->tsa;
    case 0x1AA: return s->cnt;
    case 0x1AC: return s->tctrl;
    case 0x1AE: {
        uint16_t st = (uint16_t)(s->cnt & 0x3F);
        if (s->stat_irq)
            st |= 0x40;
        if (s->cnt & 0x20)
            st |= 0x80;
        uint16_t mode = (s->cnt >> 4) & 3;
        if (mode == 2)
            st |= 0x100;
        if (mode == 3)
            st |= 0x200;
        if (s->capture >= 256)
            st |= 0x800;
        return st;
    }
    case 0x1B0: return (uint16_t)s->cd_vol[0];
    case 0x1B2: return (uint16_t)s->cd_vol[1];
    case 0x1B4: return (uint16_t)s->ext_vol[0];
    case 0x1B6: return (uint16_t)s->ext_vol[1];
    case 0x1B8: return (uint16_t)s->main[0].level;
    case 0x1BA: return (uint16_t)s->main[1].level;
    default: return s->regs[off >> 1];
    }
}

void ps1_spu_write16(ps1_spu *s, uint32_t off, uint16_t v)
{
    off &= 0x3FE;
    s->regs[off >> 1] = v;
    if (off < 0x180) {
        voice *vc = &s->v[off >> 4];
        switch (off & 15) {
        case 0x0: vc->vol[0].reg = v; break;
        case 0x2: vc->vol[1].reg = v; break;
        case 0x4: vc->pitch = v; break;
        case 0x6: vc->start = v; break;
        case 0x8: vc->adsr1 = v; break;
        case 0xA: vc->adsr2 = v; break;
        case 0xC: vc->env = (int16_t)v; break;
        default: vc->repeat = v; break;
        }
        return;
    }
    if (off >= 0x200 && off < 0x260) {
        s->v[(off - 0x200) >> 2].vol[(off >> 1) & 1].level = (int16_t)v;
        return;
    }
    if (off >= 0x1C0 && off < 0x200) {
        s->rev[(off - 0x1C0) >> 1] = v;
        return;
    }
    switch (off) {
    case 0x180: s->main[0].reg = v; break;
    case 0x182: s->main[1].reg = v; break;
    case 0x184: s->evol[0] = (int16_t)v; break;
    case 0x186: s->evol[1] = (int16_t)v; break;
    case 0x188:
    case 0x18A: {
        int hi = off == 0x18A;
        s->kon_reg[hi] = v;
        uint32_t bits = hi ? (uint32_t)(v & 0xFF) << 16 : v;
        s->kon |= bits;
        break;
    }
    case 0x18C:
    case 0x18E: {
        int hi = off == 0x18E;
        s->koff_reg[hi] = v;
        uint32_t bits = hi ? (uint32_t)(v & 0xFF) << 16 : v;
        s->koff |= bits; /* applied on the next sample */
        break;
    }
    case 0x190: s->pmon = (s->pmon & 0xFF0000u) | (v & 0xFFFEu); break; /* voice 0 has no modulator */
    case 0x192: s->pmon = (s->pmon & 0xFFFFu) | ((uint32_t)(v & 0xFF) << 16); break;
    case 0x194: s->non = (s->non & 0xFF0000u) | v; break;
    case 0x196: s->non = (s->non & 0xFFFFu) | ((uint32_t)(v & 0xFF) << 16); break;
    case 0x198: s->eon = (s->eon & 0xFF0000u) | v; break;
    case 0x19A: s->eon = (s->eon & 0xFFFFu) | ((uint32_t)(v & 0xFF) << 16); break;
    case 0x19C: case 0x19E: break; /* ENDX: read-only */
    case 0x1A2:
        s->esa = v;
        s->rev_addr = (uint32_t)v << 3;
        break;
    case 0x1A4: s->irq_addr = v; break;
    case 0x1A6:
        s->tsa = v;
        s->taddr = ((uint32_t)v << 3) & RAM_MASK;
        break;
    case 0x1A8:
        if (((s->cnt >> 4) & 3) == 1)
            transfer_write(s, v);
        else if (s->fifo_n < 32)
            s->fifo[s->fifo_n++] = v;
        break;
    case 0x1AA:
        s->cnt = v;
        if (!(v & 0x40))
            s->stat_irq = 0; /* acknowledge */
        if (((v >> 4) & 3) == 1)
            fifo_flush(s);
        break;
    case 0x1AC: s->tctrl = v; break;
    case 0x1B0: s->cd_vol[0] = (int16_t)v; break;
    case 0x1B2: s->cd_vol[1] = (int16_t)v; break;
    case 0x1B4: s->ext_vol[0] = (int16_t)v; break;
    case 0x1B6: s->ext_vol[1] = (int16_t)v; break;
    default: break;
    }
}

void ps1_spu_dma_write(ps1_spu *s, const uint32_t *words, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        transfer_write(s, (uint16_t)words[i]);
        transfer_write(s, (uint16_t)(words[i] >> 16));
    }
}

void ps1_spu_dma_read(ps1_spu *s, uint32_t *words, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint32_t lo = transfer_read(s);
        words[i] = lo | ((uint32_t)transfer_read(s) << 16);
    }
}

void ps1_spu_tick(ps1_spu *s, uint32_t cpu_cycles, ps1_cdrom *cd)
{
    s->cycles += cpu_cycles;
    while (s->cycles >= CYCLES_PER_SAMPLE) {
        s->cycles -= CYCLES_PER_SAMPLE;
        sample(s, cd);
    }
}

size_t ps1_spu_audio_pull(ps1_spu *s, int16_t *stereo, size_t frames)
{
    size_t n = frames < s->ring_count ? frames : s->ring_count;
    for (size_t i = 0; i < n; i++) {
        stereo[2 * i] = s->ring[2 * s->ring_head];
        stereo[2 * i + 1] = s->ring[2 * s->ring_head + 1];
        s->ring_head = (s->ring_head + 1) & (OUT_RING - 1);
    }
    s->ring_count -= (uint32_t)n;
    return n;
}

/* ---- Save states (XBA-182) ------------------------------------------------------------------------------------ */

/* The registers, voices, reverb and output ring (the struct), then the 512 KB of sound RAM. */
size_t ps1_spu_state_size(ps1_spu *s)
{
    (void)s;
    return sizeof(ps1_spu) + RAM_SIZE;
}

void ps1_spu_state_save(ps1_spu *s, uint8_t *out)
{
    size_t ni = sizeof s->irq, nr = sizeof s->ram;
    memcpy(out, s, sizeof *s);
    memset(out + offsetof(ps1_spu, irq), 0, ni);
    memset(out + offsetof(ps1_spu, ram), 0, nr);
    memcpy(out + sizeof *s, s->ram, RAM_SIZE);
}

int ps1_spu_state_load(ps1_spu *s, const uint8_t *in, size_t n)
{
    if (n != sizeof *s + RAM_SIZE)
        return -1;
    ps1_irq_sink irq = s->irq;
    uint8_t *ram = s->ram;
    memcpy(s, in, sizeof *s);
    s->irq = irq;
    s->ram = ram;
    memcpy(s->ram, in + sizeof *s, RAM_SIZE);
    return 0;
}
