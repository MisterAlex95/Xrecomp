/* Unit tests of the GBA APU (G-6, XBA-155): known register writes, then properties of the 48 kHz output or of the
 * channel state: square period / duty / amplitude, envelope steps, length expiry, sweep steps and overflow, wave
 * RAM banks and volume, the noise LFSR sequence and its periods, Direct Sound from a synthetic FIFO stream fed by
 * DMA1 on timer 0, SOUNDBIAS resolution, the master enable, register read masks and the output ring. The CPU sits
 * halted; frames still run (timers, DMA, the APU). Our own code. */
#include "gba_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...)                                                                                       \
    do {                                                                                                       \
        if (!(cond)) {                                                                                         \
            failures++;                                                                                        \
            printf("FAIL %s:%d: ", __func__, __LINE__);                                                        \
            printf(__VA_ARGS__);                                                                               \
            printf("\n");                                                                                      \
        }                                                                                                      \
    } while (0)

#define MAXS (GBA_AUDIO_RATE * 3)
static int16_t pcm[2 * MAXS];

static gba_machine *machine(void)
{
    static const uint8_t rom[256] = {0};
    gba_machine *m = gba_create(rom, sizeof rom, NULL, 0);
    if (!m) {
        printf("FAIL: gba_create\n");
        exit(1);
    }
    m->cpu.halted = true; /* no IRQ enabled: it stays halted, frames only run the hardware */
    return m;
}

static void io8(gba_machine *m, uint32_t r, uint8_t v) { gba_io_write8(m, 0x04000000u | r, v); }
static void io16(gba_machine *m, uint32_t r, uint16_t v) { gba_io_write16(m, 0x04000000u | r, v); }
static void io32(gba_machine *m, uint32_t r, uint32_t v)
{
    io16(m, r, (uint16_t)v);
    io16(m, r + 2, (uint16_t)(v >> 16));
}
static uint16_t rio16(gba_machine *m, uint32_t r) { return gba_io_read16(m, 0x04000000u | r); }

/* Sound on, every PSG channel on both sides at master volume 7, PSG at 100 %. */
static void sound_on(gba_machine *m)
{
    io16(m, 0x84, 0x80);
    io16(m, 0x80, 0xFF77);
    io16(m, 0x82, 0x0002);
}

/* Runs `frames` frames and collects the output (stereo); returns the frame count. */
static size_t run(gba_machine *m, int frames)
{
    size_t n = 0;
    for (int f = 0; f < frames; f++) {
        gba_run_frame(m, NULL);
        n += gba_audio_pull(m, pcm + 2 * n, MAXS - n);
    }
    return n;
}

/* Frequency (Hz) from the rising zero crossings of the left channel in [from, to). */
static double frequency(size_t from, size_t to)
{
    long first = -1, last = -1, count = 0;
    for (size_t i = from + 1; i < to; i++) {
        if (pcm[2 * (i - 1)] < 0 && pcm[2 * i] >= 0) {
            if (first < 0)
                first = (long)i;
            last = (long)i;
            count++;
        }
    }
    if (count < 2)
        return 0;
    return (double)(count - 1) * GBA_AUDIO_RATE / (double)(last - first);
}

static int peak(size_t from, size_t to)
{
    int p = 0;
    for (size_t i = from; i < to; i++) {
        int v = abs(pcm[2 * i]);
        if (v > p)
            p = v;
    }
    return p;
}

static double positive_fraction(size_t from, size_t to)
{
    size_t pos = 0;
    for (size_t i = from; i < to; i++)
        pos += pcm[2 * i] > 0;
    return (double)pos / (double)(to - from);
}

static bool near(double v, double want, double tol) { return fabs(v - want) <= tol * want; }

static void test_square(void)
{
    gba_machine *m = machine();
    sound_on(m);
    io8(m, 0x68, 0x80);           /* duty 50 % */
    io8(m, 0x69, 0xF0);           /* volume 15, no envelope */
    io16(m, 0x6C, 0x8000 | 1750); /* 131072 / (2048 - 1750) = 439.8 Hz */
    size_t n = run(m, 60);
    double f = frequency(2000, n);
    CHECK(near(f, 131072.0 / 298, 0.005), "square 2 frequency %.2f", f);
    int p = peak(2000, n);
    CHECK(near(p, 15 * 8 * 64, 0.03), "square peak %d (want %d)", p, 15 * 8 * 64);
    double d = positive_fraction(2000, n);
    CHECK(near(d, 0.5, 0.03), "duty 50 %%: %.3f", d);
    CHECK(rio16(m, 0x84) == 0x82, "SOUNDCNT_X %04X: channel 2 on", rio16(m, 0x84));

    static const double duty[4] = {0.125, 0.25, 0.5, 0.75};
    for (int k = 0; k < 4; k++) {
        io8(m, 0x68, (uint8_t)(k << 6));
        run(m, 2);
        n = run(m, 30);
        d = positive_fraction(0, n);
        CHECK(fabs(d - duty[k]) < 0.02, "duty %d: %.3f (want %.3f)", k, d, duty[k]);
    }

    /* Right only: the left output is silent. */
    io16(m, 0x80, 0x0277);
    run(m, 2);
    n = run(m, 10);
    CHECK(peak(0, n) == 0, "left disabled: peak %d", peak(0, n));
    gba_destroy(m);
}

static void test_envelope_length(void)
{
    gba_machine *m = machine();
    sound_on(m);
    io8(m, 0x68, 0x80);
    io8(m, 0x69, 0xF1); /* 15, decreasing, one step per 1/64 s */
    io16(m, 0x6C, 0x8000 | 1750);
    int vols[40];
    for (int f = 0; f < 40; f++) {
        run(m, 1);
        vols[f] = m->apu.sq[1].vol;
    }
    bool mono = true;
    for (int f = 1; f < 40; f++)
        mono &= vols[f] <= vols[f - 1] && vols[f - 1] - vols[f] <= 2;
    CHECK(mono, "envelope steps down by 1-2 per frame (64 Hz vs 59.7 Hz)");
    /* 15 steps of 1/64 s = 0.234 s = 14 frames */
    CHECK(vols[11] > 0 && vols[15] == 0, "envelope reaches 0 after 15/64 s: frame 11 %d, frame 15 %d", vols[11],
          vols[15]);
    CHECK(rio16(m, 0x84) & 2, "an envelope at 0 keeps the channel on");

    /* Increasing envelope from 0. */
    io8(m, 0x69, 0x09); /* 0, increasing, 1/64 s */
    io8(m, 0x6D, 0x80);
    run(m, 20);
    CHECK(m->apu.sq[1].vol == 15, "increasing envelope saturates at 15: %d", m->apu.sq[1].vol);

    /* Length: 64 - 0 = 64 steps of 1/256 s = 0.25 s (15 frames). */
    io8(m, 0x69, 0xF0);
    io8(m, 0x68, 0x80);
    io8(m, 0x6D, 0xC0 | (1750 >> 8));
    run(m, 13);
    CHECK(rio16(m, 0x84) & 2, "still on at 13 frames");
    run(m, 3);
    CHECK(!(rio16(m, 0x84) & 2), "length expired after 0.25 s: %04X", rio16(m, 0x84));
    /* Length 48: 16 steps = 1/16 s = 3.7 frames. */
    io8(m, 0x68, 0x80 | 48);
    io8(m, 0x6D, 0xC0 | (1750 >> 8));
    run(m, 3);
    CHECK(rio16(m, 0x84) & 2, "length 48: on at 3 frames");
    run(m, 2);
    CHECK(!(rio16(m, 0x84) & 2), "length 48: off at 5 frames");

    /* DAC off (volume 0, decreasing) disables the channel. */
    io8(m, 0x69, 0xF0);
    io8(m, 0x6D, 0x80);
    io8(m, 0x69, 0x00);
    CHECK(!(rio16(m, 0x84) & 2), "DAC off disables channel 2");
    gba_destroy(m);
}

static void test_sweep(void)
{
    gba_machine *m = machine();
    sound_on(m);
    io8(m, 0x60, 0x13); /* time 1 (1/128 s), increase, shift 3 */
    io8(m, 0x63, 0xF0);
    io16(m, 0x64, 0x8000 | 1024);
    /* 1024 -> 1152 -> 1296 -> 1458 -> 1640 -> 1845, whose next step (2075) overflows: off after 5 steps. */
    static const int seq[] = {1024, 1152, 1296, 1458, 1640, 1845};
    int seen = m->apu.sq[0].freq == seq[0];
    uint64_t t0 = m->apu.time;
    while (seen && m->apu.sq[0].on && m->apu.time - t0 < 16777216u / 4) {
        gba_apu_sync(m, m->apu.time + 1024);
        int f = m->apu.sq[0].freq;
        if (seen < 6 && f == seq[seen])
            seen++;
        else if (f != seq[seen - 1])
            break;
    }
    uint64_t dt = m->apu.time - t0;
    CHECK(seen == 6, "sweep sequence: %d of 6 steps seen (freq %d)", seen, m->apu.sq[0].freq);
    CHECK(!m->apu.sq[0].on, "sweep overflow turns channel 1 off");
    /* 5 sweep steps of 1/128 s (131072 cycles), the first one 0..1 step after the trigger. */
    CHECK(dt > 4 * 131072u && dt <= 5 * 131072u + 1024, "sweep took %llu cycles", (unsigned long long)dt);

    /* Decrease: 1024 - 1024 >> 1 ... never overflows. */
    io8(m, 0x60, 0x19); /* time 1, decrease, shift 1 */
    io16(m, 0x64, 0x8000 | 1024);
    gba_apu_sync(m, m->apu.time + 3 * 131072);
    CHECK(m->apu.sq[0].on && m->apu.sq[0].freq < 1024, "decreasing sweep: on, freq %d", m->apu.sq[0].freq);

    /* Overflow detected at trigger already: 2000 + 1000 > 2047. */
    io8(m, 0x60, 0x11);
    io16(m, 0x64, 0x8000 | 2000);
    CHECK(!m->apu.sq[0].on, "overflow at trigger");
    gba_destroy(m);
}

static void test_wave(void)
{
    gba_machine *m = machine();
    sound_on(m);
    /* Bank 0 plays: the CPU writes bank 1. A triangle 0..15..0 over 32 samples. */
    io8(m, 0x70, 0x00);
    for (int i = 0; i < 16; i++) {
        int s0 = i < 8 ? 2 * i : 31 - 2 * i, s1 = i < 8 ? 2 * i + 1 : 30 - 2 * i;
        io8(m, 0x90 + (uint32_t)i, (uint8_t)(s0 << 4 | s1));
    }
    CHECK(m->apu.wave_ram[1][1] == 0x23 && m->apu.wave_ram[0][1] == 0, "CPU writes the bank not playing");
    io8(m, 0x70, 0xC0); /* play bank 1, DAC on */
    CHECK(rio16(m, 0x90) == 0, "CPU now reads bank 0: %04X", rio16(m, 0x90));
    io16(m, 0x72, 0x2000); /* 100 % */
    io16(m, 0x74, 0x8000 | 1898); /* 2097152 / 150 / 32 = 436.9 Hz */
    size_t n = run(m, 60);
    double f = frequency(2000, n);
    CHECK(near(f, 2097152.0 / 150 / 32, 0.005), "wave frequency %.2f", f);
    int p100 = peak(2000, n);
    CHECK(near(p100, 15 * 8 * 64, 0.05), "wave peak at 100 %%: %d", p100);
    CHECK(rio16(m, 0x84) & 4, "wave on");

    static const struct { uint16_t reg; double ratio; } vols[] = {
        {0x6000, 0.25}, {0x4000, 0.5}, {0x8000, 0.75}, {0x0000, 0.0}};
    for (int k = 0; k < 4; k++) {
        io16(m, 0x72, vols[k].reg);
        run(m, 2);
        n = run(m, 20);
        int p = peak(0, n);
        CHECK(fabs(p - vols[k].ratio * p100) <= 0.06 * p100 + 64, "wave volume %.2f: peak %d of %d", vols[k].ratio,
              p, p100);
    }

    /* Two banks (64 samples): half the frequency. */
    io16(m, 0x72, 0x2000);
    io8(m, 0x70, 0xE0);
    io16(m, 0x74, 0x8000 | 1898);
    n = run(m, 60);
    f = frequency(2000, n);
    CHECK(near(f, 2097152.0 / 150 / 64, 0.01) || near(f, 2097152.0 / 150 / 32, 0.01), "two banks: %.2f Hz", f);

    /* DAC off stops it. */
    io8(m, 0x70, 0x00);
    CHECK(!(rio16(m, 0x84) & 4), "wave DAC off");
    gba_destroy(m);
}

/* GBATEK's noise generator: X = X >> 1, on carry out = high and X ^= 6000h (15-bit) or 60h (7-bit). */
static uint16_t ref_noise(uint16_t x, bool w7, int *out)
{
    int c = x & 1;
    x >>= 1;
    if (c)
        x ^= w7 ? 0x60 : 0x6000;
    *out = c;
    return x;
}

static void test_noise(void)
{
    gba_machine *m = machine();
    sound_on(m);
    io8(m, 0x79, 0xF0);
    for (int w7 = 0; w7 < 2; w7++) {
        io8(m, 0x7C, (uint8_t)(w7 ? 0x08 : 0x00)); /* r = 0, s = 0: a clock every 32 cycles */
        io8(m, 0x7D, 0x80);
        CHECK(m->apu.noise.period == 32, "noise period %d", m->apu.noise.period);
        uint16_t x = w7 ? 0x40 : 0x4000, x0 = x;
        bool ok = true;
        int bit = 0;
        for (int k = 1; k <= 300 && ok; k++) {
            gba_apu_sync(m, m->apu.time + 32);
            x = ref_noise(x, w7, &bit);
            ok = m->apu.noise.lfsr == x && m->apu.noise.out == bit;
            if (!ok)
                CHECK(0, "%d-bit LFSR at step %d: %04X, want %04X", w7 ? 7 : 15, k, m->apu.noise.lfsr, x);
        }
        /* Period: 127 (7-bit) or 32767 (15-bit) clocks. */
        long period = 0;
        x = x0;
        do {
            x = ref_noise(x, w7, &bit);
            period++;
        } while (x != x0 && period < 70000);
        CHECK(period == (w7 ? 127 : 32767), "LFSR period %ld", period);
        io8(m, 0x7D, 0x80);
        gba_apu_sync(m, m->apu.time + 32 * (uint64_t)period);
        CHECK(m->apu.noise.lfsr == x0, "the core's LFSR is back after %ld clocks: %04X", period, m->apu.noise.lfsr);
    }
    /* Dividers: r = 3, s = 2: 524288 / 3 / 8 Hz = 3 * 2^8 cycles; s = 14 stops the clock. */
    io8(m, 0x7C, 0x23);
    CHECK(m->apu.noise.period == 3 << 8, "r 3 s 2: period %d", m->apu.noise.period);
    io8(m, 0x7C, 0xE0);
    CHECK(m->apu.noise.period == 0, "s 14: stopped");
    /* A running 15-bit noise averages to about 0 and fills both signs. */
    io8(m, 0x7C, 0x11);
    io8(m, 0x7D, 0x80);
    size_t n = run(m, 30);
    long sum = 0;
    for (size_t i = 0; i < n; i++)
        sum += pcm[2 * i];
    double pf = positive_fraction(0, n);
    CHECK(pf > 0.35 && pf < 0.65 && labs(sum / (long)n) < 1500, "noise balance: %.2f positive, mean %ld", pf,
          sum / (long)n);
    gba_destroy(m);
}

/* Direct Sound A: a 1024 Hz square (16 samples of +100, 16 of -100) at 32768 Hz in EWRAM, timer 0, DMA1. */
static void test_direct_sound(void)
{
    gba_machine *m = machine();
    for (uint32_t i = 0; i < GBA_EWRAM_SIZE; i++)
        m->ewram[i] = (uint8_t)((i & 16) ? -100 : 100);
    io16(m, 0x84, 0x80);
    io16(m, 0x82, 0x0B04);         /* A: 100 %, right + left, timer 0, reset */
    io32(m, 0xBC, 0x02000000);     /* DMA1 source */
    io32(m, 0xC0, 0x040000A0);     /* FIFO A */
    io16(m, 0xC6, 0xB600);         /* enable, special timing, 32-bit, repeat */
    io16(m, 0x100, 65536 - 512);   /* 2^24 / 512 = 32768 Hz */
    io16(m, 0x102, 0x80);
    uint64_t t0 = m->now;
    size_t n = run(m, 60);
    double f = frequency(2000, n);
    CHECK(near(f, 1024, 0.005), "Direct Sound frequency %.2f", f);
    int p = peak(2000, n);
    CHECK(near(p, 100 * 4 * 64, 0.05), "Direct Sound peak %d (want %d)", p, 100 * 4 * 64);
    /* Bytes moved by DMA1 = samples played (one per 512 cycles) + what the FIFO holds (17..32). */
    double played = (double)(m->dma[1].src - 0x02000000u) - m->apu.fifo_n[0];
    double want = (double)((m->now - t0) / 512);
    CHECK(fabs(played - want) <= 1, "FIFO played %.0f samples (want %.0f)", played, want);
    CHECK(m->apu.fifo_n[0] > 0 && m->apu.fifo_n[0] <= 32, "FIFO fill %d", m->apu.fifo_n[0]);

    /* 50 %: half the amplitude. */
    io16(m, 0x82, 0x0300);
    run(m, 2);
    n = run(m, 20);
    p = peak(0, n);
    CHECK(near(p, 100 * 2 * 64, 0.05), "Direct Sound at 50 %%: peak %d", p);

    /* FIFO reset and pushes. */
    io16(m, 0xC6, 0);
    io16(m, 0x82, 0x0B00);
    CHECK(m->apu.fifo_n[0] == 0, "FIFO A reset");
    io32(m, 0xA0, 0x04030201);
    io16(m, 0xA0, 0x0605);
    io8(m, 0xA0, 7);
    CHECK(m->apu.fifo_n[0] == 7 && m->apu.fifo[0][0] == 1 && m->apu.fifo[0][6] == 7, "FIFO pushes: %d",
          m->apu.fifo_n[0]);
    for (int i = 0; i < 40; i++)
        io8(m, 0xA4, (uint8_t)i);
    CHECK(m->apu.fifo_n[1] == 32, "FIFO B holds 32 bytes: %d", m->apu.fifo_n[1]);
    CHECK(rio16(m, 0xA0) == 0, "FIFO is write-only");
    gba_destroy(m);
}

/* SOUNDBIAS: a constant Direct Sound level of 50 (x4 = 200 DAC units) reduced to 9 or 6 bits. */
static void test_bias(void)
{
    gba_machine *m = machine();
    CHECK(rio16(m, 0x88) == 0x200, "SOUNDBIAS after boot %04X", rio16(m, 0x88));
    io16(m, 0x84, 0x80);
    io16(m, 0x82, 0x0304); /* A 100 %, both sides, timer 0 */
    for (int i = 0; i < 4; i++)
        io32(m, 0xA0, 0x32323232);
    io16(m, 0x100, 0xFF00);
    io16(m, 0x102, 0x80);
    run(m, 1); /* plays 50 from the FIFO, then keeps the last sample */
    size_t n = run(m, 4);
    CHECK(pcm[2 * (n - 1)] == 200 * 64, "9-bit: %d (want %d)", pcm[2 * (n - 1)], 200 * 64);
    io16(m, 0x88, 0xC200); /* 6-bit */
    run(m, 1);
    n = run(m, 4);
    CHECK(pcm[2 * (n - 1)] == 192 * 64, "6-bit: %d (want %d)", pcm[2 * (n - 1)], 192 * 64);
    io16(m, 0x88, 0x0380); /* bias 380h + 200 clips at 3FFh, 9-bit: 3FEh, i.e. 126 above the bias */
    run(m, 1);
    n = run(m, 4);
    CHECK(pcm[2 * (n - 1)] == 126 * 64, "clip at 3FFh: %d", pcm[2 * (n - 1)]);
    gba_destroy(m);
}

static void test_registers(void)
{
    gba_machine *m = machine();
    io16(m, 0x62, 0xFFFF);
    CHECK(rio16(m, 0x62) == 0, "master off: writes ignored");
    sound_on(m);
    io16(m, 0x60, 0xFFFF);
    io16(m, 0x62, 0xFFFF);
    io16(m, 0x64, 0x47FF); /* no trigger */
    io16(m, 0x70, 0xFFFF);
    io16(m, 0x72, 0xFFFF);
    io16(m, 0x78, 0xFFFF);
    io16(m, 0x7C, 0x40FF);
    io16(m, 0x80, 0xFFFF);
    io16(m, 0x82, 0xFFFF & ~0x8800);
    io16(m, 0x88, 0xFFFF);
    CHECK(rio16(m, 0x60) == 0x007F, "SOUND1CNT_L %04X", rio16(m, 0x60));
    CHECK(rio16(m, 0x62) == 0xFFC0, "SOUND1CNT_H %04X", rio16(m, 0x62));
    CHECK(rio16(m, 0x64) == 0x4000, "SOUND1CNT_X %04X", rio16(m, 0x64));
    CHECK(rio16(m, 0x70) == 0x00E0, "SOUND3CNT_L %04X", rio16(m, 0x70));
    CHECK(rio16(m, 0x72) == 0xE000, "SOUND3CNT_H %04X", rio16(m, 0x72));
    CHECK(rio16(m, 0x78) == 0xFF00, "SOUND4CNT_L %04X", rio16(m, 0x78));
    CHECK(rio16(m, 0x7C) == 0x40FF, "SOUND4CNT_H %04X", rio16(m, 0x7C));
    CHECK(rio16(m, 0x80) == 0xFF77, "SOUNDCNT_L %04X", rio16(m, 0x80));
    CHECK(rio16(m, 0x82) == 0x770F, "SOUNDCNT_H %04X", rio16(m, 0x82));
    CHECK(rio16(m, 0x88) == 0xC3FE, "SOUNDBIAS %04X", rio16(m, 0x88));
    CHECK(rio16(m, 0x66) == 0 && rio16(m, 0x8A) == 0, "unused registers read 0");
    /* Master off clears 0x60-0x81, keeps SOUNDCNT_H and SOUNDBIAS. */
    io16(m, 0x84, 0);
    CHECK(rio16(m, 0x62) == 0 && rio16(m, 0x80) == 0 && rio16(m, 0x82) == 0x770F && rio16(m, 0x88) == 0xC3FE,
          "master off: %04X %04X %04X %04X", rio16(m, 0x62), rio16(m, 0x80), rio16(m, 0x82), rio16(m, 0x88));
    gba_destroy(m);
}

static void test_output(void)
{
    gba_machine *m = machine();
    /* Silence while the master is off, and the right number of frames: cycles x 48000 / 2^24. */
    size_t n = run(m, 60);
    uint64_t want = m->now * GBA_AUDIO_RATE / 16777216u;
    CHECK(n + 2 >= want && n <= want, "60 frames gave %zu samples (want %llu)", n, (unsigned long long)want);
    CHECK(peak(0, n) == 0, "silence: peak %d", peak(0, n));
    /* Not pulled: the ring keeps the newest GBA_AUDIO_RING frames (more than 8 video frames). */
    for (int f = 0; f < 20; f++)
        gba_run_frame(m, NULL);
    CHECK(m->apu.ring_n == GBA_AUDIO_RING && GBA_AUDIO_RING > 8 * 804, "ring capped at %u", m->apu.ring_n);
    CHECK(gba_audio_pull(m, pcm, MAXS) == GBA_AUDIO_RING && gba_audio_pull(m, pcm, MAXS) == 0, "pull drains it");

    /* A tone pulled in small, odd-sized chunks is continuous: no step larger than the waveform allows. */
    sound_on(m);
    io8(m, 0x68, 0x80);
    io8(m, 0x69, 0xF0);
    io16(m, 0x6C, 0x8000 | 1750);
    size_t total = 0;
    int16_t chunk[2 * 37];
    int maxstep = 0, prev = 0;
    for (int f = 0; f < 30; f++) {
        gba_run_frame(m, NULL);
        size_t k;
        while ((k = gba_audio_pull(m, chunk, 37)) > 0) {
            for (size_t i = 0; i < k; i++) {
                int d = abs(chunk[2 * i] - prev);
                if (d > maxstep && total > 0)
                    maxstep = d;
                prev = chunk[2 * i];
                total++;
            }
        }
    }
    CHECK(maxstep < 15 * 8 * 64 * 2, "largest step %d", maxstep);
    gba_destroy(m);
}

int main(void)
{
    test_square();
    test_envelope_length();
    test_sweep();
    test_wave();
    test_noise();
    test_direct_sound();
    test_bias();
    test_registers();
    test_output();
    if (failures) {
        printf("gba_apu_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("gba_apu_test: ok\n");
    return 0;
}
