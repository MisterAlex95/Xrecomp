/* Unit tests of ps1_spu.c (P-8, XBA-166): ADPCM decoding of every filter against a reference written from psx-spx
 * (through the Gaussian interpolation, read back from the voice 1 capture buffer), loop flags / ENDX / repeat
 * address, ADSR curves (linear and exponential attack, decay to sustain, exponential release, all-ones rates), sweep
 * volumes, noise, pitch modulation, the reverb's impulse response, transfers (manual FIFO, DMA write / read), the SPU
 * IRQ and the output rate. Our own code. */
#include "dev_test.h"

static int irqs;
static void raise_cb(void *ctx, int irq)
{
    (void)ctx;
    if (irq == PS1_IRQ_SPU)
        irqs++;
}

static const int16_t gauss_ref[512] = {
#include "ps1_spu_gauss.inc"
};

static ps1_spu *spu_new(void)
{
    ps1_irq_sink sink = {raise_cb, NULL};
    ps1_spu *s = ps1_spu_create(sink);
    irqs = 0;
    ps1_spu_write16(s, 0x1AC, 0x0004);
    ps1_spu_write16(s, 0x1AA, 0xC000);
    ps1_spu_write16(s, 0x180, 0x3FFF);
    ps1_spu_write16(s, 0x182, 0x3FFF);
    return s;
}

static void ram_write(ps1_spu *s, uint32_t addr, const void *data, size_t bytes)
{
    uint16_t cnt = ps1_spu_read16(s, 0x1AA);
    ps1_spu_write16(s, 0x1AA, cnt & ~0x30);
    ps1_spu_write16(s, 0x1A6, (uint16_t)(addr >> 3));
    ps1_spu_write16(s, 0x1AA, (cnt & ~0x30) | 0x20); /* DMA write */
    uint32_t w[4096];
    memcpy(w, data, bytes);
    ps1_spu_dma_write(s, w, (bytes + 3) / 4);
    ps1_spu_write16(s, 0x1AA, cnt & ~0x30);
}

static void ram_read(ps1_spu *s, uint32_t addr, void *data, size_t bytes)
{
    uint16_t cnt = ps1_spu_read16(s, 0x1AA);
    ps1_spu_write16(s, 0x1A6, (uint16_t)(addr >> 3));
    ps1_spu_write16(s, 0x1AA, (cnt & ~0x30) | 0x30); /* DMA read */
    uint32_t w[4096];
    ps1_spu_dma_read(s, w, (bytes + 3) / 4);
    memcpy(data, w, bytes);
    ps1_spu_write16(s, 0x1AA, cnt & ~0x30);
}

static void samples(ps1_spu *s, int n) { ps1_spu_tick(s, 768u * (uint32_t)n, NULL); }

static void voice(ps1_spu *s, int v, uint32_t start, uint16_t pitch, uint16_t adsr1, uint16_t adsr2)
{
    uint32_t b = (uint32_t)v * 16;
    ps1_spu_write16(s, b + 0x0, 0x3FFF);
    ps1_spu_write16(s, b + 0x2, 0x3FFF);
    ps1_spu_write16(s, b + 0x4, pitch);
    ps1_spu_write16(s, b + 0x6, (uint16_t)(start >> 3));
    ps1_spu_write16(s, b + 0x8, adsr1);
    ps1_spu_write16(s, b + 0xA, adsr2);
}

static void key_on(ps1_spu *s, int v)
{
    if (v < 16)
        ps1_spu_write16(s, 0x188, (uint16_t)(1u << v));
    else
        ps1_spu_write16(s, 0x18A, (uint16_t)(1u << (v - 16)));
}

/* Reference SPU-ADPCM decoder (psx-spx « SPU ADPCM Samples »). */
static void ref_decode(const uint8_t *blocks, int nblocks, int16_t *out)
{
    static const int pos[5] = {0, 60, 115, 98, 122}, neg[5] = {0, 0, -52, -55, -60};
    int old = 0, older = 0;
    for (int b = 0; b < nblocks; b++) {
        const uint8_t *p = blocks + b * 16;
        int shift = p[0] & 15, f = (p[0] >> 4) & 7;
        if (shift > 12)
            shift = 9;
        for (int i = 0; i < 28; i++) {
            int nib = (p[2 + i / 2] >> ((i & 1) * 4)) & 15;
            int v = ((int16_t)(nib << 12)) >> shift;
            v += (old * pos[f] + older * neg[f] + 32) >> 6;
            v = v < -32768 ? -32768 : v > 32767 ? 32767 : v;
            out[b * 28 + i] = (int16_t)v;
            older = old;
            old = v;
        }
    }
}

/* Every filter and several shifts: blocks of a pseudo-random nibble stream, voice 1 at pitch 1000h (interpolation
 * index 0), envelope held at 7FFFh; capture buffer 2 (voice 1) compared with the reference through the Gaussian. */
static void test_adpcm(void)
{
    enum { NB = 15 };
    uint8_t blocks[NB * 16];
    uint32_t seed = 12345;
    for (int b = 0; b < NB; b++) {
        blocks[b * 16] = (uint8_t)(((b % 5) << 4) | (b * 3 % 13));
        blocks[b * 16 + 1] = 0;
        for (int i = 2; i < 16; i++) {
            seed = seed * 1103515245u + 12345u;
            blocks[b * 16 + i] = (uint8_t)(seed >> 16);
        }
    }
    blocks[(NB - 1) * 16 + 1] = 3; /* end + repeat (back to the start: repeat = start) */
    ps1_spu *s = spu_new();
    ram_write(s, 0x1000, blocks, sizeof blocks);
    /* attack: linear, shift 0 step 0 (+7 << 11 per sample); sustain level 15 (no decay), sustain rate all ones */
    voice(s, 1, 0x1000, 0x1000, 0x000F, 0x7FC0 | 0x1F);
    ps1_spu_write16(s, 0x1E, 0x1000 >> 3); /* repeat address */
    key_on(s, 1);
    samples(s, NB * 28 + 4);
    int16_t ref[NB * 28], cap[512];
    ref_decode(blocks, NB, ref);
    ram_read(s, 0x800, cap, sizeof cap);
    int bad = 0;
    for (int j = 3; j < NB * 28; j++) {
        /* sample j plays counter j * 1000h: new = ref[j], old = ref[j-1], ... */
        int32_t out = (gauss_ref[0x0FF] * ref[j - 3]) >> 15;
        out += (gauss_ref[0x1FF] * ref[j - 2]) >> 15;
        out += (gauss_ref[0x100] * ref[j - 1]) >> 15;
        out += (gauss_ref[0x000] * ref[j]) >> 15;
        int32_t want = (out * 0x7FFF) >> 15;
        if (cap[j] != want) {
            if (bad++ < 5)
                printf("  sample %d: got %d want %d\n", j, cap[j], want);
        }
    }
    CHECK(bad == 0, "%d ADPCM samples differ from the reference", bad);
    CHECK(ps1_spu_read16(s, 0x19C) & 2, "ENDX of voice 1 after the end flag");
    CHECK(ps1_spu_read16(s, 0x1C) == 0x7FFF, "ENVX %04X", ps1_spu_read16(s, 0x1C));
    key_on(s, 1);
    samples(s, 1);
    CHECK(!(ps1_spu_read16(s, 0x19C) & 2), "ENDX cleared by key on");
    ps1_spu_destroy(s);
}

/* Interpolation at a fractional pitch: 800h (index 80h on odd samples). */
static void test_gauss_fraction(void)
{
    uint8_t blk[32] = {0};
    blk[0] = 0x00; /* shift 0 filter 0 */
    for (int i = 2; i < 16; i++)
        blk[i] = (uint8_t)(i * 0x11);
    blk[16] = 0x00;
    blk[17] = 3;
    ps1_spu *s = spu_new();
    ram_write(s, 0x2000, blk, sizeof blk);
    voice(s, 1, 0x2000, 0x0800, 0x000F, 0x7FDF);
    ps1_spu_write16(s, 0x1E, 0x2010 >> 3);
    key_on(s, 1);
    samples(s, 40);
    int16_t ref[56], cap[40];
    ref_decode(blk, 2, ref);
    ram_read(s, 0x800, cap, sizeof cap);
    int bad = 0;
    for (int j = 8; j < 40; j++) {
        uint32_t counter = (uint32_t)j * 0x800, idx = counter >> 12, g = (counter >> 4) & 0xFF;
        int32_t o = (gauss_ref[0x0FF - g] * (idx >= 3 ? ref[idx - 3] : 0)) >> 15;
        o += (gauss_ref[0x1FF - g] * (idx >= 2 ? ref[idx - 2] : 0)) >> 15;
        o += (gauss_ref[0x100 + g] * (idx >= 1 ? ref[idx - 1] : 0)) >> 15;
        o += (gauss_ref[0x000 + g] * ref[idx]) >> 15;
        bad += cap[j] != (int16_t)((o * 0x7FFF) >> 15);
    }
    CHECK(bad == 0, "%d samples differ at pitch 800h", bad);
    ps1_spu_destroy(s);
}

/* Loop start / end: blocks A B(start) C(end+repeat) play A B C B C ...; repeat register follows the start flag. */
static void test_loop(void)
{
    uint8_t blk[48] = {0};
    for (int b = 0; b < 3; b++) {
        blk[b * 16] = 0x00;
        memset(blk + b * 16 + 2, 0x11 * (b + 1), 14);
    }
    blk[16 + 1] = 4;
    blk[32 + 1] = 3;
    ps1_spu *s = spu_new();
    ram_write(s, 0x3000, blk, sizeof blk);
    voice(s, 1, 0x3000, 0x1000, 0x000F, 0x7FDF);
    key_on(s, 1);
    samples(s, 20);
    CHECK(ps1_spu_read16(s, 0x1E) == 0, "repeat before block B %04X (key on does not set it)", ps1_spu_read16(s, 0x1E));
    samples(s, 20);
    CHECK(ps1_spu_read16(s, 0x1E) == 0x3010 >> 3, "repeat after block B %04X", ps1_spu_read16(s, 0x1E));
    CHECK(!(ps1_spu_read16(s, 0x19C) & 2), "ENDX before the end block");
    samples(s, 220);
    CHECK(ps1_spu_read16(s, 0x19C) & 2, "ENDX after the end block");
    int16_t cap[300];
    ram_read(s, 0x800, cap, sizeof cap); /* the capture position started with the key on: sample j at j */
    int periodic = 1;
    for (int j = 100; j < 200; j++)
        periodic &= cap[j] == cap[j + 56];
    CHECK(periodic, "B C loop not periodic over 56 samples");

    /* end without repeat: release with the envelope at zero */
    blk[32 + 1] = 1;
    ram_write(s, 0x3000, blk, sizeof blk);
    key_on(s, 1);
    samples(s, 28 * 3 + 2);
    CHECK(ps1_spu_read16(s, 0x1C) == 0, "ENVX after end + mute: %04X", ps1_spu_read16(s, 0x1C));
    ps1_spu_destroy(s);
}

static int16_t envx(ps1_spu *s, int v) { return (int16_t)ps1_spu_read16(s, (uint32_t)v * 16 + 0xC); }

static void test_adsr(void)
{
    static const uint8_t silent_loop[16] = {0, 3};
    ps1_spu *s = spu_new();
    ram_write(s, 0x1000, silent_loop, 16);
    /* linear attack shift 10 step 0: +7 << 1 = 14 per sample */
    voice(s, 0, 0x1000, 0x1000, (10 << 10) | 0x000F, 0x7FDF);
    key_on(s, 0);
    samples(s, 100);
    CHECK(envx(s, 0) == 1400, "linear attack after 100 samples: %d", envx(s, 0));
    /* shift 13: one step of 7 every 4 samples */
    voice(s, 0, 0x1000, 0x1000, (13 << 10) | (0 << 8) | 0x000F, 0x7FDF);
    key_on(s, 0);
    samples(s, 400);
    CHECK(envx(s, 0) == 700, "attack shift 13 after 400 samples: %d", envx(s, 0));
    /* exponential attack: above 6000h it slows down (shift 4: step 7 << 7 = 896, divided by 4) */
    voice(s, 0, 0x1000, 0x1000, 0x8000 | (4 << 10) | 0x000F, 0x7FDF);
    key_on(s, 0);
    int n = 0;
    while (envx(s, 0) <= 0x6000 && n < 1000) {
        samples(s, 1);
        n++;
    }
    CHECK(n == 28, "exponential attack: %d samples to pass 6000h", n);
    int16_t a = envx(s, 0);
    samples(s, 1);
    CHECK(envx(s, 0) - a == 224, "exponential attack step above 6000h: %d", envx(s, 0) - a);
    /* attack to 7FFFh, decay (exponential, shift 2) to sustain level 7 (4000h), then hold */
    voice(s, 0, 0x1000, 0x1000, (0 << 10) | (2 << 4) | 7, 0x7FDF);
    key_on(s, 0);
    samples(s, 3);
    CHECK(envx(s, 0) == 0x7FFF, "attack end %04X", envx(s, 0));
    samples(s, 400);
    int16_t sus = envx(s, 0);
    CHECK((sus >> 11) <= 7 && (sus >> 11) >= 6, "decay stopped at %04X", sus);
    samples(s, 100);
    CHECK(envx(s, 0) == sus, "sustain with an all-ones rate held %04X -> %04X", sus, envx(s, 0));
    /* exponential release shift 0: about halves each sample (step -8 << 11 times level / 8000h) */
    ps1_spu_write16(s, 0xA, 0x7FC0 | 0x20 | 0);
    ps1_spu_write16(s, 0x18C, 1);
    samples(s, 1);
    int16_t r1 = envx(s, 0);
    samples(s, 1);
    int16_t r2 = envx(s, 0);
    CHECK(r1 > 0 && r2 > 0 && r2 * 2 >= r1 - 2 && r2 * 2 <= r1 + 2, "exponential release %d -> %d", r1, r2);
    samples(s, 40);
    CHECK(envx(s, 0) == 0, "released to %d", envx(s, 0));
    /* linear release shift 31 (all ones): never steps */
    voice(s, 0, 0x1000, 0x1000, 0x000F, 0x7FC0 | 0x1F);
    key_on(s, 0);
    samples(s, 5);
    ps1_spu_write16(s, 0x18C, 1);
    samples(s, 100);
    CHECK(envx(s, 0) == 0x7FFF, "release rate 1Fh stepped: %04X", envx(s, 0));
    ps1_spu_destroy(s);
}

static void test_sweep_noise_pmon(void)
{
    ps1_spu *s = spu_new();
    /* main volume left: linear increase sweep, shift 10 step 0 (+14 per sample) from 0 */
    ps1_spu_write16(s, 0x180, 0x0000);
    samples(s, 1);
    ps1_spu_write16(s, 0x180, 0x8000 | (10 << 2));
    samples(s, 50);
    CHECK((int16_t)ps1_spu_read16(s, 0x1B8) == 700, "sweep after 50 samples: %d", (int16_t)ps1_spu_read16(s, 0x1B8));
    /* direct mode: the register is volume / 2 */
    ps1_spu_write16(s, 0x182, 0x2000);
    samples(s, 1);
    CHECK((int16_t)ps1_spu_read16(s, 0x1BA) == 0x4000, "direct volume %04X", ps1_spu_read16(s, 0x1BA));
    /* voice volume sweep: exponential decrease from 7FFEh */
    ps1_spu_write16(s, 0x00, 0x3FFF);
    samples(s, 1);
    ps1_spu_write16(s, 0x00, 0x8000 | 0x4000 | 0x2000 | (8 << 2));
    samples(s, 200);
    int16_t v = (int16_t)ps1_spu_read16(s, 0x200);
    CHECK(v > 0 && v < 0x7FFE, "voice sweep decreasing: %d", v);

    /* noise on voice 1 (captured): varying, full scale. A fresh SPU: the noise timer still counts down the long
     * reload of the default (lowest) frequency, as on hardware */
    ps1_spu_destroy(s);
    s = spu_new();
    static const uint8_t silent_loop[16] = {0, 3};
    ram_write(s, 0x1000, silent_loop, 16);
    voice(s, 1, 0x1000, 0x1000, 0x000F, 0x7FDF);
    ps1_spu_write16(s, 0x1AA, 0xC000 | (15 << 10) | (3 << 8));
    ps1_spu_write16(s, 0x194, 0x0002);
    key_on(s, 1);
    samples(s, 512);
    int16_t cap[512];
    ram_read(s, 0x800, cap, sizeof cap); /* the whole capture buffer: 512 samples of noise */
    int changes = 0, big = 0;
    for (int j = 1; j < 512; j++) {
        changes += cap[j] != cap[j - 1];
        big += cap[j] > 0x4000 || cap[j] < -0x4000;
    }
    CHECK(changes > 100 && big > 50, "noise: %d changes, %d large (envx %d, cap %d %d %d)", changes, big, envx(s, 1), cap[0], cap[100], cap[511]);
    ps1_spu_write16(s, 0x194, 0);

    /* pitch modulation: voice 1 modulated by a silent voice 0 runs at half its pitch */
    uint8_t blk[16 * 8] = {0};
    for (int b = 0; b < 8; b++)
        blk[b * 16 + 1] = b == 7 ? 3 : 0;
    ram_write(s, 0x4000, blk, sizeof blk);
    voice(s, 0, 0x1000, 0x1000, 0x000F, 0x7FDF);
    voice(s, 1, 0x4000, 0x1000, 0x000F, 0x7FDF);
    ps1_spu_write16(s, 0x1E, 0x4000 >> 3);
    ps1_spu_write16(s, 0x190, 0x0002);
    ps1_spu_write16(s, 0x188, 0x0003);
    samples(s, 28 * 7 + 5); /* 7 blocks at full rate, 3.5 at half */
    CHECK(!(ps1_spu_read16(s, 0x19C) & 2), "modulated voice reached its end too early");
    samples(s, 28 * 8);
    CHECK(ps1_spu_read16(s, 0x19C) & 2, "modulated voice never reached its end");
    ps1_spu_destroy(s);
}

/* Reverb: a short burst on voice 0 with the "Room" preset (psx-spx « SPU Reverb Examples »); the output keeps
 * ringing after the voice stops, and decays. With the master enable off the work area is never written. */
static void test_reverb(void)
{
    static const uint16_t room[32] = {
        0x007D, 0x005B, 0x6D80, 0x54B8, 0xBED0, 0x0000, 0x0000, 0xBA80, 0x5800, 0x5300, 0x04D6, 0x0333, 0x03F0, 0x0227,
        0x0374, 0x01EF, 0x0334, 0x01B5, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x01B4, 0x0136,
        0x00B8, 0x005C, 0x8000, 0x8000,
    };
    for (int enable = 1; enable >= 0; enable--) {
        ps1_spu *s = spu_new();
        uint8_t blk[64] = {0};
        blk[0] = 0x00;
        memset(blk + 2, 0x77, 14); /* loud DC block */
        blk[16 + 1] = 0;
        memset(blk + 18, 0x99, 14);
        blk[32 + 1] = 1; /* end + mute after three blocks */
        blk[48 + 1] = 3;
        ram_write(s, 0x1000, blk, sizeof blk);
        ps1_spu_write16(s, 0x0E, 0x1030 >> 3);
        for (int r = 0; r < 32; r++)
            ps1_spu_write16(s, 0x1C0 + (uint32_t)r * 2, room[r]);
        ps1_spu_write16(s, 0x1A2, (uint16_t)((0x80000 - 0x26C0) >> 3));
        ps1_spu_write16(s, 0x184, 0x4000);
        ps1_spu_write16(s, 0x186, 0x4000);
        ps1_spu_write16(s, 0x198, 0x0001);
        ps1_spu_write16(s, 0x1AA, enable ? 0xC080 : 0xC000);
        voice(s, 0, 0x1000, 0x1000, 0x000F, 0x7FDF);
        key_on(s, 0);
        /* the delays reach the whole 26C0h-byte area (4960 steps at 22 kHz): energy per 0.1 s over 2 s */
        static int16_t out[2 * 4410];
        double e[20] = {0};
        static uint16_t work[0x26C0 / 2];
        int written = 0;
        for (int c = 0; c < 20; c++) {
            if (c == 2) { /* while it rings (it decays to exact zeros later) */
                ram_read(s, 0x80000 - 0x26C0, work, sizeof work);
                for (int k = 0; k < 0x26C0 / 2; k++)
                    written += work[k] != 0;
            }
            samples(s, 4410);
            size_t n = ps1_spu_audio_pull(s, out, 4410);
            CHECK(n == 4410, "pulled %zu", n);
            for (int j = 0; j < 4410; j++)
                e[c] += (double)out[2 * j] * out[2 * j];
        }
        double e1 = e[1] + e[2] + e[3], e2 = e[17] + e[18] + e[19];
        if (enable) {
            CHECK(e1 > 1e6, "reverb tail energy %.0f", e1);
            CHECK(e2 < e1 / 10, "reverb tail does not decay: %.0f then %.0f", e1, e2);
            CHECK(written, "reverb work area untouched");
        } else {
            CHECK(e1 == 0, "reverb output with the master enable off and an empty buffer: %.0f", e1);
            CHECK(!written, "work area written with the master enable off");
        }
        ps1_spu_destroy(s);
    }
}

static void test_transfers_irq(void)
{
    ps1_spu *s = spu_new();
    /* DMA round trip, 4 KB at 0x10000 */
    static uint32_t w[1024], r[1024];
    for (int i = 0; i < 1024; i++)
        w[i] = (uint32_t)i * 2654435761u;
    ram_write(s, 0x10000, w, sizeof w);
    ram_read(s, 0x10000, r, sizeof r);
    CHECK(memcmp(w, r, sizeof w) == 0, "DMA round trip");
    /* manual writes through the FIFO: queued in stop mode, written when manual write is selected */
    ps1_spu_write16(s, 0x1AA, 0xC000);
    ps1_spu_write16(s, 0x1A6, 0x20000 >> 3);
    for (int i = 0; i < 32; i++)
        ps1_spu_write16(s, 0x1A8, (uint16_t)(0xA000 + i));
    uint16_t back[32];
    ram_read(s, 0x20000, back, sizeof back);
    CHECK(back[0] == 0, "FIFO written before manual mode");
    ps1_spu_write16(s, 0x1AA, 0xC000);
    ps1_spu_write16(s, 0x1A6, 0x20000 >> 3);
    for (int i = 0; i < 32; i++)
        ps1_spu_write16(s, 0x1A8, (uint16_t)(0xA000 + i));
    ps1_spu_write16(s, 0x1AA, 0xC010);
    CHECK((ps1_spu_read16(s, 0x1AE) & 0x3F) == 0x10, "STAT mode bits %04X", ps1_spu_read16(s, 0x1AE));
    ram_read(s, 0x20000, back, sizeof back);
    int ok = 1;
    for (int i = 0; i < 32; i++)
        ok &= back[i] == 0xA000 + i;
    CHECK(ok, "manual FIFO write");
    CHECK(ps1_spu_read16(s, 0x1A6) == 0x20000 >> 3, "TSA does not increment");

    /* IRQ from a DMA write over the IRQ address */
    ps1_spu_write16(s, 0x1A4, 0x10040 >> 3);
    ps1_spu_write16(s, 0x1AA, 0xC040);
    irqs = 0;
    ram_write(s, 0x10000, w, 256);
    CHECK(irqs == 1 && (ps1_spu_read16(s, 0x1AE) & 0x40), "transfer IRQ: %d, STAT %04X", irqs,
          ps1_spu_read16(s, 0x1AE));
    ram_write(s, 0x10000, w, 256);
    CHECK(irqs == 1, "IRQ raised again before the acknowledge");
    ps1_spu_write16(s, 0x1AA, 0xC000);
    CHECK(!(ps1_spu_read16(s, 0x1AE) & 0x40), "acknowledge");
    /* IRQ from a voice reading the block that holds the address */
    static const uint8_t blk[32] = {0, 0, [16] = 0, [17] = 3};
    ram_write(s, 0x5000, blk, sizeof blk);
    ps1_spu_write16(s, 0x1A4, 0x5018 >> 3);
    ps1_spu_write16(s, 0x1AA, 0xC040);
    irqs = 0;
    voice(s, 5, 0x5000, 0x1000, 0x000F, 0x7FDF);
    key_on(s, 5);
    samples(s, 10);
    CHECK(irqs == 0, "IRQ before the voice reached the block");
    samples(s, 30);
    CHECK(irqs == 1, "voice IRQ: %d", irqs);
    /* capture IRQ: the address in the CD capture buffer */
    ps1_spu_write16(s, 0x1AA, 0xC000);
    ps1_spu_write16(s, 0x1A4, 0x0100 >> 3);
    ps1_spu_write16(s, 0x1AA, 0xC040);
    irqs = 0;
    samples(s, 600);
    CHECK(irqs == 1, "capture IRQ: %d", irqs);
    /* output: one frame per 768 cycles, held until pulled (8192 frames) */
    ps1_spu_destroy(s);
    s = spu_new();
    ps1_spu_tick(s, 768 * 100 + 767, NULL);
    int16_t o[2 * 200];
    CHECK(ps1_spu_audio_pull(s, o, 200) == 100, "frames after 100 x 768 cycles");
    ps1_spu_tick(s, 1, NULL);
    CHECK(ps1_spu_audio_pull(s, o, 200) == 1, "the 768th cycle");
    samples(s, 10000);
    CHECK(ps1_spu_audio_pull(s, NULL, 0) == 0, "pull of 0");
    static int16_t big[2 * 10000];
    CHECK(ps1_spu_audio_pull(s, big, 10000) == 8192, "ring keeps 8192 frames");
    ps1_spu_destroy(s);
}

int main(void)
{
    test_adpcm();
    test_gauss_fraction();
    test_loop();
    test_adsr();
    test_sweep_noise_pmon();
    test_reverb();
    test_transfers_irq();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ps1_spu: all tests passed\n");
    return 0;
}
