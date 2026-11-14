/* gba_audio.c -- the GBA APU (G-6, XBA-155) and the audio output of the core. GBATEK « GBA Sound Controller ».
 *
 * PSG: square 1 (sweep, envelope, length), square 2, wave (2 x 32 4-bit samples in two banks, one or both played,
 * volume 0 / 25 / 50 / 75 / 100 %), noise (15- or 7-bit LFSR, envelope, length), clocked by a 512 Hz frame
 * sequencer (length 256 Hz, sweep 128 Hz, envelope 64 Hz). Direct Sound A / B: two 32-byte FIFOs of signed 8-bit
 * samples, each popped on the overflow of timer 0 or 1 (gba_io.c calls gba_apu_fifo_tick, then DMA1 / DMA2 refill
 * a FIFO holding 16 bytes or less). SOUNDCNT_L / H / X with the master enable (off: the PSG registers clear and
 * ignore writes), SOUNDBIAS (bias level and amplitude resolution).
 *
 * The APU runs lazily: gba_apu_sync catches up to a cycle on each sound register access, FIFO tick and at the end
 * of each frame. It mixes in windows of GBA_APU_WINDOW cycles (131072 Hz): every channel's output is integrated over
 * the window (an exact box filter, so a square wave above the window rate does not alias), the window's mix goes
 * through SOUNDCNT / SOUNDBIAS like the hardware's 10-bit DAC input (bias added, clipped to 0..3FFh, reduced to
 * the resolution, bias taken off again so the output has no DC), then a one-pole low-pass (about 16 kHz) and an area
 * average resample it to GBA_AUDIO_RATE. The stream is continuous, so there are no clicks at buffer edges.
 *
 * Simplifications: the hardware's own sampling rate (32..262 kHz from SOUNDBIAS) is not modelled (we always mix at
 * 131 kHz; the resolution bits only reduce the bit depth); PSG channels are centred (+vol / -vol) instead of the
 * hardware's unipolar DAC, so enabling or muting one does not pop; the GB-only quirks (zombie envelope writes, sweep
 * negate lock-out, length clocking on trigger) are not modelled; a FIFO holds bytes, not the hardware's words. */
#include "gba_internal.h"

#include <string.h>

#define GBA_CLOCK 16777216u                                  /* 2^24 Hz */
#define FS_WINDOWS (32768 / GBA_APU_WINDOW)                  /* windows per frame-sequencer step (512 Hz) */
#define WINDOW_UNITS ((uint32_t)GBA_APU_WINDOW * GBA_AUDIO_RATE) /* one window, in cycles x output rate */
#define LPF_K 35080                                          /* 1 - exp(-2 pi 16 kHz / 131072 Hz), Q16 */

/* Duty patterns 12.5 / 25 / 50 / 75 %: bit `pos` is the output at step pos. */
static const uint8_t duty_masks[4] = {0x80, 0x81, 0xE1, 0x7E};

static inline bool master_on(const gba_machine *m)
{
    return (m->io[0x84] & 0x80) != 0;
}

/* ---- channels: output integrated over n cycles ------------------------------------------------------------- */

static int32_t square_run(gba_psg_square *c, int32_t n)
{
    if (!c->on)
        return 0;
    uint8_t pat = duty_masks[c->duty];
    int32_t out = ((pat >> c->pos) & 1) ? c->vol : -c->vol, acc = 0;
    while (c->timer <= n) {
        acc += out * c->timer;
        n -= c->timer;
        c->timer = c->period;
        c->pos = (c->pos + 1) & 7;
        out = ((pat >> c->pos) & 1) ? c->vol : -c->vol;
    }
    c->timer -= n;
    return acc + out * n;
}

/* Wave sample at the playing position, centred (-15..15), times the volume in quarters. */
static inline int32_t wave_out(const gba_machine *m)
{
    const gba_psg_wave *w = &m->apu.wave;
    int bank = ((m->io[0x70] >> 6) & 1) ^ (w->pos >> 5);
    uint8_t b = m->apu.wave_ram[bank][(w->pos & 31) >> 1];
    int s = (w->pos & 1) ? (b & 15) : (b >> 4); /* high nibble first */
    return (2 * s - 15) * w->vol_mul;
}

static int32_t wave_run(gba_machine *m, int32_t n)
{
    gba_psg_wave *w = &m->apu.wave;
    if (!w->on)
        return 0;
    int mask = (m->io[0x70] & 0x20) ? 63 : 31;
    int32_t out = wave_out(m), acc = 0;
    while (w->timer <= n) {
        acc += out * w->timer;
        n -= w->timer;
        w->timer = w->period;
        w->pos = (w->pos + 1) & mask;
        out = wave_out(m);
    }
    w->timer -= n;
    return acc + out * n;
}

static inline void noise_clock(gba_psg_noise *c, bool w7)
{
    bool carry = c->lfsr & 1;
    c->lfsr >>= 1;
    if (carry)
        c->lfsr ^= w7 ? 0x60 : 0x6000;
    c->out = carry;
}

static int32_t noise_run(gba_machine *m, int32_t n)
{
    gba_psg_noise *c = &m->apu.noise;
    if (!c->on)
        return 0;
    int32_t out = c->out ? c->vol : -c->vol;
    if (!c->period)
        return out * n;
    bool w7 = (m->io[0x7C] & 8) != 0;
    int32_t acc = 0;
    while (c->timer <= n) {
        acc += out * c->timer;
        n -= c->timer;
        c->timer = c->period;
        noise_clock(c, w7);
        out = c->out ? c->vol : -c->vol;
    }
    c->timer -= n;
    return acc + out * n;
}

/* ---- frame sequencer ------------------------------------------------------------------------------------- */

static inline void length_clock(bool *on, int *length, bool len_en)
{
    if (len_en && *length > 0 && --*length == 0)
        *on = false;
}

static inline void envelope_clock(int *vol, int *timer, int period, bool up)
{
    if (!period)
        return;
    if (--*timer > 0)
        return;
    *timer = period;
    if (up && *vol < 15)
        (*vol)++;
    else if (!up && *vol > 0)
        (*vol)--;
}

static int sweep_calc(gba_psg_square *c)
{
    int delta = c->shadow >> c->sw_shift;
    int nf = c->sw_down ? c->shadow - delta : c->shadow + delta;
    if (nf > 2047)
        c->on = false;
    return nf;
}

static inline int32_t square_period(uint16_t freq) { return (2048 - freq) * 16; }
static inline int32_t wave_period(uint16_t freq) { return (2048 - freq) * 8; }

static void sweep_clock(gba_psg_square *c)
{
    if (--c->sw_timer > 0)
        return;
    c->sw_timer = c->sw_period ? c->sw_period : 8;
    if (!c->sw_en || !c->sw_period)
        return;
    int nf = sweep_calc(c);
    if (nf <= 2047 && c->sw_shift) {
        c->freq = c->shadow = (uint16_t)nf;
        c->period = square_period(c->freq);
        sweep_calc(c); /* the next step is checked for overflow at once */
    }
}

static void fs_step(gba_apu *a)
{
    int st = a->fs_step;
    if (!(st & 1)) {
        length_clock(&a->sq[0].on, &a->sq[0].length, a->sq[0].len_en);
        length_clock(&a->sq[1].on, &a->sq[1].length, a->sq[1].len_en);
        length_clock(&a->wave.on, &a->wave.length, a->wave.len_en);
        length_clock(&a->noise.on, &a->noise.length, a->noise.len_en);
    }
    if (st == 2 || st == 6)
        sweep_clock(&a->sq[0]);
    if (st == 7) {
        for (int i = 0; i < 2; i++)
            envelope_clock(&a->sq[i].vol, &a->sq[i].env_timer, a->sq[i].env_period, a->sq[i].env_up);
        envelope_clock(&a->noise.vol, &a->noise.env_timer, a->noise.env_period, a->noise.env_up);
    }
    a->fs_step = (st + 1) & 7;
}

/* ---- mixing and output ----------------------------------------------------------------------------------- */

static void ring_push(gba_apu *a, int32_t l, int32_t r)
{
    if (l > 32767) l = 32767;
    if (l < -32768) l = -32768;
    if (r > 32767) r = 32767;
    if (r < -32768) r = -32768;
    if (a->ring_n == GBA_AUDIO_RING) { /* full: drop the oldest frame */
        a->ring_r = (a->ring_r + 1) & (GBA_AUDIO_RING - 1);
        a->ring_n--;
    }
    uint32_t w = (a->ring_r + a->ring_n) & (GBA_AUDIO_RING - 1);
    a->ring[2 * w] = (int16_t)l;
    a->ring[2 * w + 1] = (int16_t)r;
    a->ring_n++;
}

/* One window's sample (int16 scale) into the area-average resampler: output sample k covers cycles
 * [k, k + 1) * GBA_CLOCK / GBA_AUDIO_RATE; in units of cycles x GBA_AUDIO_RATE a window is WINDOW_UNITS long and an
 * output sample GBA_CLOCK = 2^24, so the weighted sum divides by a shift. */
static void resample(gba_apu *a, int32_t l, int32_t r)
{
    uint32_t left = WINDOW_UNITS;
    while (left >= a->out_rem) {
        a->out_l += (int64_t)l * a->out_rem;
        a->out_r += (int64_t)r * a->out_rem;
        left -= a->out_rem;
        ring_push(a, (int32_t)(a->out_l >> 24), (int32_t)(a->out_r >> 24));
        a->out_l = a->out_r = 0;
        a->out_rem = GBA_CLOCK;
    }
    a->out_l += (int64_t)l * left;
    a->out_r += (int64_t)r * left;
    a->out_rem -= left;
}

/* A window integral (10-bit DAC units x GBA_APU_WINDOW) through SOUNDBIAS to int16 scale. */
static inline int32_t dac(int32_t integral, int bias, int res_mask)
{
    int32_t v = (integral >> 7) + bias; /* GBA_APU_WINDOW == 128 */
    if (v < 0) v = 0;
    if (v > 0x3FF) v = 0x3FF;
    v &= ~res_mask;
    return (v - bias) * 64;
}

static void window_end(gba_machine *m)
{
    gba_apu *a = &m->apu;
    const uint8_t *io = m->io;
    int32_t l = 0, r = 0;
    if (master_on(m)) {
        uint16_t cl = rd16(&io[0x80]), ch = rd16(&io[0x82]);
        int32_t psg[4] = {a->acc[0], a->acc[1], a->acc[2] / 4, a->acc[3]};
        int32_t pl = 0, pr = 0;
        for (int i = 0; i < 4; i++) {
            if (cl & (0x100 << i))
                pr += psg[i];
            if (cl & (0x1000 << i))
                pl += psg[i];
        }
        pr *= (cl & 7) + 1;
        pl *= ((cl >> 4) & 7) + 1;
        int ratio = ch & 3; /* 0: 25 %, 1: 50 %, 2: 100 %, 3: prohibited (100 %) */
        int32_t div = ratio >= 2 ? 1 : ratio == 1 ? 2 : 4;
        pl /= div;
        pr /= div;
        int32_t da = a->acc[4] * ((ch & 4) ? 4 : 2), db = a->acc[5] * ((ch & 8) ? 4 : 2);
        if (ch & 0x100) r += da;
        if (ch & 0x200) l += da;
        if (ch & 0x1000) r += db;
        if (ch & 0x2000) l += db;
        l += pl;
        r += pr;
    }
    uint16_t sb = rd16(&io[0x88]);
    int bias = sb & 0x3FE, res_mask = (2 << (sb >> 14)) - 1;
    int32_t sl = dac(l, bias, res_mask), sr = dac(r, bias, res_mask);
    /* One-pole low-pass at the window rate (state in Q8). */
    a->lpf_l += (int32_t)((((int64_t)sl * 256 - a->lpf_l) * LPF_K + 32768) >> 16); /* rounded: settles exactly */
    a->lpf_r += (int32_t)((((int64_t)sr * 256 - a->lpf_r) * LPF_K + 32768) >> 16);
    resample(a, (a->lpf_l + 128) >> 8, (a->lpf_r + 128) >> 8);
    memset(a->acc, 0, sizeof a->acc);
    if (--a->fs_windows == 0) {
        a->fs_windows = FS_WINDOWS;
        if (master_on(m))
            fs_step(a);
    }
}

void gba_apu_sync(gba_machine *m, uint64_t until)
{
    gba_apu *a = &m->apu;
    if (until <= a->time)
        return;
    uint64_t n = until - a->time;
    a->time = until;
    if (!master_on(m) && !a->lpf_l && !a->lpf_r) {
        /* Sound off and the filter settled: only zeros to emit, skip the per-window work. */
        uint64_t total = (uint64_t)a->win_pos + n;
        a->win_pos = (int)(total % GBA_APU_WINDOW);
        uint64_t units = total / GBA_APU_WINDOW * WINDOW_UNITS;
        while (units >= a->out_rem) {
            units -= a->out_rem;
            ring_push(a, (int32_t)(a->out_l >> 24), (int32_t)(a->out_r >> 24));
            a->out_l = a->out_r = 0;
            a->out_rem = GBA_CLOCK;
        }
        a->out_rem -= (uint32_t)units;
        return;
    }
    while (n) {
        int32_t take = GBA_APU_WINDOW - a->win_pos;
        if ((uint64_t)take > n)
            take = (int32_t)n;
        if (master_on(m)) {
            a->acc[0] += square_run(&a->sq[0], take);
            a->acc[1] += square_run(&a->sq[1], take);
            a->acc[2] += wave_run(m, take);
            a->acc[3] += noise_run(m, take);
            a->acc[4] += a->ds_sample[0] * take;
            a->acc[5] += a->ds_sample[1] * take;
        }
        a->win_pos += take;
        n -= (uint64_t)take;
        if (a->win_pos == GBA_APU_WINDOW) {
            a->win_pos = 0;
            window_end(m);
        }
    }
}

/* ---- Direct Sound FIFOs ---------------------------------------------------------------------------------- */

static void fifo_push(gba_apu *a, int f, uint8_t v)
{
    if (a->fifo_n[f] >= 32)
        return; /* full: the write is lost */
    a->fifo[f][(a->fifo_r[f] + a->fifo_n[f]) & 31] = (int8_t)v;
    a->fifo_n[f]++;
}

int gba_apu_fifo_tick(gba_machine *m, int f, uint64_t when)
{
    gba_apu *a = &m->apu;
    gba_apu_sync(m, when);
    if (a->fifo_n[f] > 0) { /* empty: the last sample keeps playing */
        a->ds_sample[f] = a->fifo[f][a->fifo_r[f]];
        a->fifo_r[f] = (a->fifo_r[f] + 1) & 31;
        a->fifo_n[f]--;
    }
    return a->fifo_n[f];
}

/* ---- registers ------------------------------------------------------------------------------------------- */

static void square_trigger(gba_machine *m, int i)
{
    gba_psg_square *c = &m->apu.sq[i];
    uint8_t env = m->io[i ? 0x69 : 0x63];
    c->on = (env & 0xF8) != 0; /* the DAC is off with volume 0 and a decreasing envelope */
    c->vol = env >> 4;
    c->env_period = env & 7;
    c->env_timer = c->env_period ? c->env_period : 8;
    c->env_up = (env & 8) != 0;
    if (c->length == 0)
        c->length = 64;
    c->period = square_period(c->freq);
    c->timer = c->period;
    if (i == 0) {
        c->shadow = c->freq;
        c->sw_timer = c->sw_period ? c->sw_period : 8;
        c->sw_en = c->sw_period || c->sw_shift;
        if (c->sw_shift)
            sweep_calc(c);
    }
}

static void noise_trigger(gba_machine *m)
{
    gba_psg_noise *c = &m->apu.noise;
    uint8_t env = m->io[0x79];
    c->on = (env & 0xF8) != 0;
    c->vol = env >> 4;
    c->env_period = env & 7;
    c->env_timer = c->env_period ? c->env_period : 8;
    c->env_up = (env & 8) != 0;
    if (c->length == 0)
        c->length = 64;
    c->lfsr = (m->io[0x7C] & 8) ? 0x40 : 0x4000;
    c->out = false;
    c->timer = c->period ? c->period : 1;
}

static void noise_set_period(gba_machine *m, uint8_t v)
{
    gba_psg_noise *c = &m->apu.noise;
    int r = v & 7, s = v >> 4;
    /* 524288 Hz / r / 2^(s+1), r = 0 counting as 0.5: r * 2^(s+6) cycles, or 2^(s+5); shifts 14 / 15 stop it */
    c->period = s >= 14 ? 0 : r ? r << (s + 6) : 1 << (s + 5);
    if (c->period && (c->timer <= 0 || c->timer > c->period))
        c->timer = c->period;
}

static void power_off(gba_machine *m)
{
    gba_apu *a = &m->apu;
    memset(&m->io[0x60], 0, 0x22); /* 0x60-0x81 */
    memset(a->sq, 0, sizeof a->sq);
    memset(&a->wave, 0, sizeof a->wave);
    memset(&a->noise, 0, sizeof a->noise);
}

void gba_apu_write8(gba_machine *m, uint32_t addr, uint8_t v)
{
    gba_apu *a = &m->apu;
    uint32_t r = addr & 0x3FF;
    gba_apu_sync(m, m->now);
    if (r >= 0xA0 && r < 0xA8) {
        fifo_push(a, r >= 0xA4, v);
        return;
    }
    if (r >= 0x90 && r < 0xA0) { /* the CPU sees the bank that is not playing */
        a->wave_ram[((m->io[0x70] >> 6) & 1) ^ 1][r - 0x90] = v;
        return;
    }
    if (r < 0x82 && !master_on(m))
        return; /* master off: the PSG registers ignore writes */
    uint8_t *io = m->io;
    switch (r) {
    case 0x60:
        io[r] = v & 0x7F;
        a->sq[0].sw_shift = v & 7;
        a->sq[0].sw_down = (v & 8) != 0;
        a->sq[0].sw_period = (v >> 4) & 7;
        return;
    case 0x62:
    case 0x68: {
        gba_psg_square *c = &a->sq[r == 0x68];
        io[r] = v;
        c->length = 64 - (v & 63);
        c->duty = v >> 6;
        return;
    }
    case 0x63:
    case 0x69:
        io[r] = v;
        if (!(v & 0xF8))
            a->sq[r == 0x69].on = false;
        return;
    case 0x64:
    case 0x6C: {
        gba_psg_square *c = &a->sq[r == 0x6C];
        io[r] = v;
        c->freq = (uint16_t)((c->freq & 0x700) | v);
        c->period = square_period(c->freq);
        return;
    }
    case 0x65:
    case 0x6D: {
        int i = r == 0x6D;
        gba_psg_square *c = &a->sq[i];
        io[r] = v & 0x47;
        c->freq = (uint16_t)((c->freq & 0xFF) | (v & 7) << 8);
        c->period = square_period(c->freq);
        c->len_en = (v & 0x40) != 0;
        if (v & 0x80)
            square_trigger(m, i);
        return;
    }
    case 0x70:
        io[r] = v & 0xE0;
        if (!(v & 0x80))
            a->wave.on = false;
        return;
    case 0x72:
        io[r] = v;
        a->wave.length = 256 - v;
        return;
    case 0x73: {
        static const int mul[4] = {0, 4, 2, 1};
        io[r] = v & 0xE0;
        a->wave.vol_mul = (v & 0x80) ? 3 : mul[(v >> 5) & 3];
        return;
    }
    case 0x74:
        io[r] = v;
        a->wave.freq = (uint16_t)((a->wave.freq & 0x700) | v);
        a->wave.period = wave_period(a->wave.freq);
        return;
    case 0x75:
        io[r] = v & 0x47;
        a->wave.freq = (uint16_t)((a->wave.freq & 0xFF) | (v & 7) << 8);
        a->wave.period = wave_period(a->wave.freq);
        a->wave.len_en = (v & 0x40) != 0;
        if (v & 0x80) {
            a->wave.on = (io[0x70] & 0x80) != 0;
            a->wave.pos = 0;
            a->wave.timer = a->wave.period;
            if (a->wave.length == 0)
                a->wave.length = 256;
        }
        return;
    case 0x78:
        io[r] = v;
        a->noise.length = 64 - (v & 63);
        return;
    case 0x79:
        io[r] = v;
        if (!(v & 0xF8))
            a->noise.on = false;
        return;
    case 0x7C:
        io[r] = v;
        noise_set_period(m, v);
        return;
    case 0x7D:
        io[r] = v & 0x40;
        a->noise.len_en = (v & 0x40) != 0;
        if (v & 0x80)
            noise_trigger(m);
        return;
    case 0x80:
    case 0x81:
        io[r] = v;
        return;
    case 0x82:
        io[r] = v & 0x0F;
        return;
    case 0x83:
        io[r] = v & 0x77;
        if (v & 0x08)
            a->fifo_n[0] = a->fifo_r[0] = 0;
        if (v & 0x80)
            a->fifo_n[1] = a->fifo_r[1] = 0;
        return;
    case 0x84: {
        bool was = master_on(m);
        io[r] = v & 0x80;
        if (was && !(v & 0x80))
            power_off(m);
        else if (!was && (v & 0x80)) {
            a->fs_step = 0;
            a->fs_windows = FS_WINDOWS;
        }
        return;
    }
    case 0x88:
        io[r] = v & 0xFE;
        return;
    case 0x89:
        io[r] = v & 0xC3;
        return;
    default:
        return; /* unused */
    }
}

uint8_t gba_apu_read8(gba_machine *m, uint32_t addr)
{
    uint32_t r = addr & 0x3FF;
    const uint8_t *io = m->io;
    if (r >= 0x90 && r < 0xA0)
        return m->apu.wave_ram[((io[0x70] >> 6) & 1) ^ 1][r - 0x90];
    switch (r) {
    case 0x60: case 0x63: case 0x69: case 0x79: case 0x7C: case 0x81:
        return io[r];
    case 0x62: case 0x68:
        return io[r] & 0xC0; /* length is write-only */
    case 0x65: case 0x6D: case 0x75: case 0x7D:
        return io[r] & 0x40;
    case 0x70: case 0x73:
        return io[r] & 0xE0;
    case 0x80: case 0x83:
        return io[r] & 0x77;
    case 0x82:
        return io[r] & 0x0F;
    case 0x84: {
        const gba_apu *a = &m->apu;
        gba_apu_sync(m, m->now);
        return (uint8_t)((io[r] & 0x80) | a->sq[0].on | a->sq[1].on << 1 | a->wave.on << 2 | a->noise.on << 3);
    }
    case 0x88: case 0x89:
        return io[r];
    default:
        return 0; /* write-only (frequencies, lengths, FIFOs) or unused */
    }
}

void gba_apu_reset(gba_machine *m)
{
    gba_apu *a = &m->apu;
    memset(a, 0, sizeof *a);
    a->time = m->now;
    a->fs_windows = FS_WINDOWS;
    a->out_rem = GBA_CLOCK;
    memset(&m->io[0x60], 0, 0x48);
    wr16(&m->io[0x88], 0x200); /* SOUNDBIAS as the BIOS leaves it */
}

/* ---- public API ------------------------------------------------------------------------------------------ */

size_t gba_audio_pull(gba_machine *m, int16_t *out, size_t max_frames)
{
    gba_apu *a = &m->apu;
    size_t n = a->ring_n < max_frames ? a->ring_n : max_frames;
    for (size_t i = 0; i < n; i++) {
        uint32_t k = (a->ring_r + (uint32_t)i) & (GBA_AUDIO_RING - 1);
        out[2 * i] = a->ring[2 * k];
        out[2 * i + 1] = a->ring[2 * k + 1];
    }
    a->ring_r = (a->ring_r + (uint32_t)n) & (GBA_AUDIO_RING - 1);
    a->ring_n -= (uint32_t)n;
    return n;
}
