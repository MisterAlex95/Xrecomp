/* Unit tests of ps1_cdrom.c (P-9, XBA-167): scripted command sequences on a synthetic disc (a data track with XA
 * real-time audio sectors and a licence string, an audio track), checking response bytes, the interrupt order
 * (queued until acknowledged) and timing windows, data delivery by register and DMA3, XA-ADPCM and CD-DA audio with
 * the volume matrix, and the CD audio reaching the SPU. Our own code. */
#include "dev_test.h"

static ps1_cdrom *cd;
static int raised;
static uint64_t now;

static void raise_cb(void *ctx, int irq)
{
    (void)ctx;
    if (irq == PS1_IRQ_CDROM)
        raised++;
}

static void tick(uint32_t n)
{
    ps1_cdrom_tick(cd, n);
    now += n;
}

static void reg_w(int bank, int reg, uint8_t v)
{
    ps1_cdrom_write(cd, 0, (uint8_t)bank);
    ps1_cdrom_write(cd, (uint32_t)reg, v);
}

static uint8_t reg_r(int bank, int reg)
{
    ps1_cdrom_write(cd, 0, (uint8_t)bank);
    return ps1_cdrom_read(cd, (uint32_t)reg);
}

static int iflag(void) { return reg_r(1, 3) & 7; }
static void ack(void) { reg_w(1, 3, 0x07); }

static void command(uint8_t c, const uint8_t *p, int n)
{
    ps1_cdrom_write(cd, 0, 0);
    for (int i = 0; i < n; i++)
        ps1_cdrom_write(cd, 2, p[i]);
    ps1_cdrom_write(cd, 1, c);
}

/* Ticks (in steps of `step` cycles) until an interrupt is flagged or `max` cycles pass; returns its type (0 none)
 * and the cycles it took. */
static int wait_int(uint64_t max, uint64_t *took, uint32_t step)
{
    uint64_t t0 = now;
    while (now - t0 < max) {
        if (iflag())
            break;
        tick(step);
    }
    if (took)
        *took = now - t0;
    return iflag();
}

static int response(uint8_t *buf)
{
    int n = 0;
    ps1_cdrom_write(cd, 0, 0);
    while ((ps1_cdrom_read(cd, 0) & 0x20) && n < 16)
        buf[n++] = ps1_cdrom_read(cd, 1);
    return n;
}

/* Sends a command and checks its first interrupt; returns the response length. */
static int expect(uint8_t c, const uint8_t *p, int np, int want_int, uint8_t *resp)
{
    command(c, p, np);
    int got = wait_int(PS1_CPU_HZ, NULL, 256);
    int n = response(resp);
    CHECK(got == want_int, "command %02X: INT%d, expected INT%d", c, got, want_int);
    ack();
    return n;
}

/* ---- the disc --------------------------------------------------------------------------------------------- */

#define DATA_SECTORS 40
#define AUDIO_SECTORS 100
#define XA_FIRST 20
#define XA_COUNT 4

static void make_xa_sector(uint8_t *s, uint32_t lba, uint8_t channel)
{
    make_data_sector(s, lba, 0x64); /* real-time | form 2 | audio */
    s[17] = s[21] = channel;
    s[19] = s[23] = 0x00;           /* mono, 37800 Hz, 4-bit */
    for (int g = 0; g < 18; g++) {
        uint8_t *grp = s + 24 + g * 128;
        memset(grp, 0, 16);         /* shift 0, filter 0 */
        memset(grp + 16, 0x33, 112); /* every nibble 3: 3000h */
    }
}

static ps1_disc *make_disc(void)
{
    static uint8_t data[DATA_SECTORS * 2352], audio[AUDIO_SECTORS * 2352];
    for (uint32_t l = 0; l < DATA_SECTORS; l++)
        make_data_sector(data + l * 2352, l, 0x08);
    memcpy(data + 4 * 2352 + 24, "          Licensed  by          Sony Computer Entertainment Euro pe   ", 70);
    for (uint32_t l = XA_FIRST; l < XA_FIRST + XA_COUNT; l++)
        make_xa_sector(data + l * 2352, l, 0);
    for (uint32_t f = 0; f < AUDIO_SECTORS; f++)
        make_audio_sector(audio + f * 2352, 1000 + f);
    write_file("data.bin", data, sizeof data);
    write_file("audio.bin", audio, sizeof audio);
    const char *cue = "FILE \"data.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n"
                      "FILE \"audio.bin\" BINARY\n  TRACK 02 AUDIO\n    INDEX 01 00:00:00\n";
    write_file("disc.cue", cue, strlen(cue));
    char p[700], err[256] = "";
    path_in(p, sizeof p, "disc.cue");
    ps1_disc *d = ps1_disc_open(p, err, sizeof err);
    if (!d) {
        printf("cannot open the synthetic disc: %s\n", err);
        exit(1);
    }
    return d;
}

static void fresh(ps1_disc *d)
{
    if (cd)
        ps1_cdrom_destroy(cd);
    ps1_irq_sink sink = {raise_cb, NULL};
    cd = ps1_cdrom_create(sink, d);
    raised = 0;
    reg_w(1, 2, 0x1F); /* IE */
}

/* ---- tests ------------------------------------------------------------------------------------------------ */

static void test_getstat(ps1_disc *d)
{
    fresh(d);
    uint8_t r[16];
    command(0x01, NULL, 0);
    CHECK(reg_r(0, 0) & 0x80, "busy after the command write");
    uint64_t took;
    int i = wait_int(PS1_CPU_HZ, &took, 64);
    CHECK(i == 3, "Getstat INT%d", i);
    CHECK(took >= 0xC4E1 - 64 && took <= 0xC4E1 + 64, "Getstat first response after %llu cycles", (unsigned long long)took);
    CHECK(!(reg_r(0, 0) & 0x80), "busy after the response");
    CHECK(raised == 1, "IRQ raised %d times", raised);
    CHECK((reg_r(1, 3) & 0xE0) == 0xE0, "IF bits 5-7 read as 1");
    CHECK(reg_r(0, 0) & 0x20, "RSLRRDY");
    int n = response(r);
    CHECK(n == 1 && r[0] == 0x02, "Getstat response %d bytes, %02X", n, r[0]);
    CHECK(!(reg_r(0, 0) & 0x20), "RSLRRDY after the last byte");
    /* past the end the 16-byte FIFO wraps (zero padded) */
    uint8_t pad = ps1_cdrom_read(cd, 1);
    CHECK(pad == 0x00, "padding %02X", pad);
    ack();
    CHECK(iflag() == 0, "acknowledged");
    /* IE masks the IRQ, not the flag */
    reg_w(1, 2, 0x00);
    raised = 0;
    command(0x01, NULL, 0);
    CHECK(wait_int(PS1_CPU_HZ, NULL, 256) == 3 && raised == 0, "masked INT3 raised the IRQ");
    reg_w(1, 2, 0x1F);
    CHECK(raised == 1, "enabling IE with a pending flag raises the IRQ");
    ack();
}

static void test_errors(ps1_disc *d)
{
    fresh(d);
    uint8_t r[16], p[3] = {0x55, 0x00, 0x00};
    int n = expect(0x1F, p, 1, 5, r);
    CHECK(n == 2 && r[0] == 0x03 && r[1] == 0x40, "invalid command: %d %02X %02X", n, r[0], r[1]);
    CHECK(!(reg_r(0, 0) & 0x08), "the parameter stays in the FIFO after an invalid command");
    reg_w(1, 3, 0x40); /* CLRPRM */
    CHECK(reg_r(0, 0) & 0x08, "CLRPRM empties the parameter FIFO");
    n = expect(0x02, p, 2, 5, r);
    CHECK(n == 2 && r[1] == 0x20, "Setloc with 2 parameters: %02X", r[1]);
    uint8_t bad[3] = {0x00, 0x1A, 0x00};
    n = expect(0x02, bad, 3, 5, r);
    CHECK(n == 2 && r[1] == 0x10, "Setloc non-BCD: %02X", r[1]);
    n = expect(0x07, NULL, 0, 5, r);
    CHECK(n == 2 && r[1] == 0x20, "MotorOn with the motor on: %02X", r[1]);
}

static void test_info(ps1_disc *d)
{
    fresh(d);
    uint8_t r[16], p;
    p = 0x20;
    int n = expect(0x19, &p, 1, 3, r);
    CHECK(n == 4 && r[0] == 0x97 && r[1] == 0x01 && r[2] == 0x10 && r[3] == 0xC2, "Test 20h");
    p = 0x22;
    n = expect(0x19, &p, 1, 3, r);
    CHECK(n == 7 && memcmp(r, "for U/C", 7) == 0, "Test 22h");
    n = expect(0x13, NULL, 0, 3, r);
    CHECK(n == 3 && r[0] == 0x02 && r[1] == 0x01 && r[2] == 0x02, "GetTN %02X %02X %02X", r[0], r[1], r[2]);
    p = 0x00;
    n = expect(0x14, &p, 1, 3, r);
    /* end: LBA 140 + 150 = 290 = 00:03:65 -> 00:03 */
    CHECK(n == 3 && r[1] == 0x00 && r[2] == 0x03, "GetTD 0: %02X:%02X", r[1], r[2]);
    p = 0x02;
    n = expect(0x14, &p, 1, 3, r);
    CHECK(n == 3 && r[1] == 0x00 && r[2] == 0x02, "GetTD 2: %02X:%02X", r[1], r[2]);
    p = 0x03;
    n = expect(0x14, &p, 1, 5, r);
    CHECK(n == 2 && r[1] == 0x10, "GetTD 3");
    uint8_t f[2] = {0x01, 0x02};
    expect(0x0D, f, 2, 3, r);
    uint8_t m = 0x80;
    expect(0x0E, &m, 1, 3, r);
    n = expect(0x0F, NULL, 0, 3, r);
    CHECK(n == 5 && r[1] == 0x80 && r[2] == 0 && r[3] == 0x01 && r[4] == 0x02, "Getparam");
}

static void test_getid(ps1_disc *d)
{
    fresh(d);
    uint8_t r[16];
    command(0x1A, NULL, 0);
    CHECK(wait_int(PS1_CPU_HZ, NULL, 256) == 3, "GetID INT3");
    response(r);
    /* not acknowledged: the second response waits */
    tick(1000000);
    CHECK(iflag() == 3, "INT2 delivered over an unacknowledged INT3 (IF %d)", iflag());
    ack();
    uint64_t took;
    int i = wait_int(PS1_CPU_HZ, &took, 64);
    CHECK(i == 2, "GetID second INT%d", i);
    CHECK(took <= 256, "queued INT2 came %llu cycles after the acknowledge", (unsigned long long)took);
    int n = response(r);
    CHECK(n == 8 && r[0] == 0x02 && r[1] == 0x00 && r[2] == 0x20 && r[3] == 0x00 && memcmp(r + 4, "SCEE", 4) == 0,
          "GetID licensed (SCEE from the licence text): %d %02X %02X %02X %c%c%c%c", n, r[0], r[1], r[2], r[4], r[5],
          r[6], r[7]);
    ack();
    /* timing of the second response once the first is acknowledged at once */
    command(0x1A, NULL, 0);
    wait_int(PS1_CPU_HZ, NULL, 64);
    ack();
    i = wait_int(PS1_CPU_HZ, &took, 64);
    CHECK(i == 2 && took >= 0x4A00 - 128 && took <= 0x4A00 + 128, "GetID INT2 after %llu cycles",
          (unsigned long long)took);
    ack();

    /* no disc */
    ps1_irq_sink sink = {raise_cb, NULL};
    ps1_cdrom_destroy(cd);
    cd = ps1_cdrom_create(sink, NULL);
    reg_w(1, 2, 0x1F);
    expect(0x1A, NULL, 0, 3, r);
    i = wait_int(PS1_CPU_HZ, NULL, 256);
    n = response(r);
    CHECK(i == 5 && n == 8 && r[0] == 0x08 && r[1] == 0x40, "GetID without a disc: INT%d %02X %02X", i, r[0], r[1]);
    ack();
    n = expect(0x06, NULL, 0, 5, r);
    CHECK(n == 2 && (r[0] & 1) && r[1] == 0x80, "ReadN without a disc: %02X %02X", r[0], r[1]);
    ps1_cdrom_destroy(cd);
    cd = NULL;
}

static void read_dma_sector(uint8_t *out, size_t bytes)
{
    uint32_t w[0x924 / 4 + 1];
    reg_w(0, 3, 0x80); /* BFRD */
    CHECK(reg_r(0, 0) & 0x40, "DRQSTS after BFRD");
    size_t n = ps1_cdrom_dma_read(cd, w, bytes / 4);
    CHECK(n == bytes / 4, "dma %zu words", n);
    memcpy(out, w, bytes);
}

static void test_read(ps1_disc *d)
{
    fresh(d);
    uint8_t r[16], s[2352], got[0x924];
    uint8_t m = 0x80; /* double speed, 800h */
    expect(0x0E, &m, 1, 3, r);
    uint8_t loc[3] = {0x00, 0x02, 0x05}; /* LBA 5 */
    expect(0x02, loc, 3, 3, r);
    expect(0x06, NULL, 0, 3, r);
    uint64_t prev = 0;
    for (uint32_t k = 0; k < 4; k++) {
        uint64_t took;
        int i = wait_int(PS1_CPU_HZ, &took, 128);
        int n = response(r);
        CHECK(i == 1 && n == 1 && (r[0] & 0x20), "sector %u: INT%d stat %02X", k, i, r[0]);
        if (k >= 2)
            CHECK(prev + took >= 225792 - 512 && prev + took <= 225792 + 512, "INT1 interval %llu",
                  (unsigned long long)(prev + took));
        read_dma_sector(got, 0x800);
        make_data_sector(s, 5 + k, 0x08);
        CHECK(memcmp(got, s + 24, 0x800) == 0, "sector %u payload", 5 + k);
        reg_w(0, 3, 0x00);
        ack();
        prev = 0;
        /* time since this INT1, carried into the next interval */
        uint64_t t0 = now;
        (void)t0;
    }
    /* GetlocL: the newest sector's header (the drive is ahead by the time the response comes) */
    command(0x10, NULL, 0);
    int i;
    while ((i = wait_int(PS1_CPU_HZ, NULL, 128)) == 1) {
        ack();
    }
    int n = response(r);
    CHECK(i == 3 && n == 8 && r[3] == 0x02 && r[6] == 0x08, "GetlocL INT%d %d bytes mode %02X sub %02X", i, n, r[3],
          r[6]);
    ack();
    /* Pause: INT3 with the read bit still set, INT2 with it clear, after about 5 sectors at 2x */
    command(0x09, NULL, 0);
    while ((i = wait_int(PS1_CPU_HZ, NULL, 128)) == 1)
        ack();
    n = response(r);
    CHECK(i == 3 && (r[0] & 0x20), "Pause INT3 stat %02X", r[0]);
    ack();
    uint64_t took;
    i = wait_int(PS1_CPU_HZ * 2, &took, 256);
    n = response(r);
    CHECK(i == 2 && r[0] == 0x02, "Pause INT2 %d stat %02X", i, r[0]);
    CHECK(took + 0xC4E1 >= 0x10BD93 - 0x2000 && took <= 0x10BD93 + 0x2000, "Pause second response after %llu",
          (unsigned long long)took);
    ack();
    tick(PS1_CPU_HZ / 10);
    CHECK(iflag() == 0, "no INT1 after Pause");

    /* 924h: the sector from its header; the data FIFO pads with byte 924h-4 */
    m = 0xA0;
    expect(0x0E, &m, 1, 3, r);
    uint8_t loc2[3] = {0x00, 0x02, 0x10}; /* LBA 10 */
    expect(0x02, loc2, 3, 3, r);
    expect(0x1B, NULL, 0, 3, r); /* ReadS */
    i = wait_int(PS1_CPU_HZ, NULL, 128);
    response(r);
    CHECK(i == 1, "ReadS INT%d", i);
    read_dma_sector(got, 0x924);
    make_data_sector(s, 10, 0x08);
    CHECK(memcmp(got, s + 12, 0x924) == 0, "924h sector");
    CHECK(ps1_cdrom_read(cd, 2) == s[12 + 0x924 - 4], "924h padding byte");
    reg_w(0, 3, 0x00);
    ack();
    expect(0x09, NULL, 0, 3, r);
    while ((i = wait_int(PS1_CPU_HZ * 2, NULL, 256)) != 0) {
        ack();
        if (i == 2)
            break;
    }

    /* Setloc, SeekL, then ReadN (no Setloc pending): reading starts at the seek target, not at the last sector
     * read (OpenBIOS reads files this way) */
    uint8_t loc3[3] = {0x00, 0x02, 0x20}; /* LBA 20 is XA; read 30 */
    loc3[2] = 0x30;
    m = 0x80;
    expect(0x0E, &m, 1, 3, r);
    expect(0x02, loc3, 3, 3, r);
    command(0x15, NULL, 0);
    CHECK(wait_int(PS1_CPU_HZ, NULL, 256) == 3, "SeekL INT3");
    ack();
    i = wait_int(PS1_CPU_HZ, NULL, 256);
    response(r);
    CHECK(i == 2 && r[0] == 0x02, "SeekL INT2 %d stat %02X", i, r[0]);
    ack();
    expect(0x06, NULL, 0, 3, r);
    i = wait_int(PS1_CPU_HZ, NULL, 128);
    response(r);
    read_dma_sector(got, 0x800);
    make_data_sector(s, 30, 0x08);
    CHECK(i == 1 && memcmp(got, s + 24, 0x800) == 0, "ReadN after SeekL reads the seek target");
    ack();
    expect(0x09, NULL, 0, 3, r);
    while ((i = wait_int(PS1_CPU_HZ * 2, NULL, 256)) != 0) {
        ack();
        if (i == 2)
            break;
    }

    /* 800h by register: padding repeats byte 800h-8 */
    m = 0x80;
    expect(0x0E, &m, 1, 3, r);
    expect(0x02, loc, 3, 3, r);
    expect(0x06, NULL, 0, 3, r);
    i = wait_int(PS1_CPU_HZ, NULL, 128);
    response(r);
    reg_w(0, 3, 0x80);
    make_data_sector(s, 5, 0x08);
    int same = 1;
    for (int k = 0; k < 0x800; k++)
        same &= ps1_cdrom_read(cd, 2) == s[24 + k];
    CHECK(same, "800h sector by register");
    CHECK(!(reg_r(0, 0) & 0x40), "DRQSTS after the last byte");
    CHECK(ps1_cdrom_read(cd, 2) == s[24 + 0x800 - 8], "800h padding byte");
    ack();
}

static void test_xa(ps1_disc *d)
{
    fresh(d);
    uint8_t r[16];
    uint8_t m = 0xC0; /* double speed, XA */
    expect(0x0E, &m, 1, 3, r);
    uint8_t loc[3] = {0x00, 0x02, to_bcd(XA_FIRST)};
    expect(0x02, loc, 3, 3, r);
    expect(0x06, NULL, 0, 3, r);
    /* the XA sectors go to the decoder: the first INT1 is the data sector after them */
    int i = wait_int(PS1_CPU_HZ, NULL, 128);
    response(r);
    CHECK(i == 1, "INT%d", i);
    CHECK(reg_r(0, 0) & 0x04, "ADPBUSY while XA plays");
    uint8_t got[0x800], s[2352];
    read_dma_sector(got, 0x800);
    make_data_sector(s, XA_FIRST + XA_COUNT, 0x08);
    CHECK(memcmp(got, s + 24, 0x800) == 0, "first INT1 is the sector after the XA ones");
    reg_w(0, 3, 0);
    ack();
    expect(0x09, NULL, 0, 3, r);
    static int16_t pcm[2 * 20000];
    size_t n = ps1_cdrom_audio_pull(cd, pcm, 20000);
    /* 4 sectors x 4032 mono samples at 37800 Hz -> 44100 Hz */
    CHECK(n >= 18800 && n <= 18830, "XA frames %zu", n);
    int good = 0;
    for (size_t k = 100; k < n; k++)
        good += pcm[2 * k] == 0x3000 && pcm[2 * k + 1] == 0x3000;
    CHECK(good == (int)n - 100, "XA samples: %d of %zu are 3000h", good, n - 100);
    CHECK(!(reg_r(0, 0) & 0x04), "ADPBUSY once the stream is drained");
    while ((i = wait_int(PS1_CPU_HZ * 2, NULL, 256)) != 0) {
        ack();
        if (i == 2)
            break;
    }

    /* the filter selects channel 5: nothing plays, and the XA sectors are not delivered as data */
    uint8_t f[2] = {0x01, 0x05};
    expect(0x0D, f, 2, 3, r);
    m = 0xC8;
    expect(0x0E, &m, 1, 3, r);
    expect(0x02, loc, 3, 3, r);
    expect(0x06, NULL, 0, 3, r);
    i = wait_int(PS1_CPU_HZ, NULL, 128);
    response(r);
    read_dma_sector(got, 0x800);
    CHECK(i == 1 && memcmp(got, s + 24, 0x800) == 0, "filtered XA: first INT1 is the data sector");
    reg_w(0, 3, 0);
    ack();
    CHECK(ps1_cdrom_audio_pull(cd, pcm, 20000) == 0, "filtered XA played");
    expect(0x09, NULL, 0, 3, r);
    while ((i = wait_int(PS1_CPU_HZ * 2, NULL, 256)) != 0) {
        ack();
        if (i == 2)
            break;
    }

    /* XA off: the real-time sectors are delivered as data */
    f[1] = 0x00;
    m = 0x80;
    expect(0x0E, &m, 1, 3, r);
    expect(0x02, loc, 3, 3, r);
    expect(0x06, NULL, 0, 3, r);
    i = wait_int(PS1_CPU_HZ, NULL, 128);
    response(r);
    read_dma_sector(got, 0x800);
    make_xa_sector(s, XA_FIRST, 0);
    CHECK(i == 1 && memcmp(got, s + 24, 0x800) == 0, "XA off: the XA sector as data");
    ack();
}

static void test_play(ps1_disc *d)
{
    fresh(d);
    uint8_t r[16], s[2352];
    uint8_t m = 0x04; /* report */
    expect(0x0E, &m, 1, 3, r);
    uint8_t t = 0x02;
    expect(0x03, &t, 1, 3, r);
    int reports = 0, i;
    uint64_t t0 = now;
    static int16_t pcm[2 * 60000];
    size_t n = 0;
    while (now - t0 < PS1_CPU_HZ / 2) {
        n += ps1_cdrom_audio_pull(cd, pcm + 2 * n, 60000 - n);
        i = wait_int(PS1_CPU_HZ / 2 - (now - t0), NULL, 1024);
        if (!i)
            break;
        int n = response(r);
        CHECK(i == 1 && n == 8 && (r[0] & 0x80) && r[1] == 0x02 && r[2] == 0x01, "report INT%d %d %02X %02X %02X", i,
              n, r[0], r[1], r[2]);
        reports++;
        ack();
    }
    CHECK(reports >= 3, "%d reports in 0.5 s", reports);
    n += ps1_cdrom_audio_pull(cd, pcm + 2 * n, 60000 - n);
    CHECK(n >= 588 * 30, "CD-DA frames %zu", n);
    int ok = 1;
    for (int f = 0; f < 3; f++) {
        make_audio_sector(s, 1000 + (uint32_t)f);
        for (int k = 0; k < 588; k++) {
            int16_t l = (int16_t)(s[4 * k] | s[4 * k + 1] << 8), rr = (int16_t)(s[4 * k + 2] | s[4 * k + 3] << 8);
            ok &= pcm[2 * (f * 588 + k)] == l && pcm[2 * (f * 588 + k) + 1] == rr;
        }
    }
    CHECK(ok, "CD-DA samples from the start of track 2");
    /* volume matrix: swap the channels (L->R, R->L full, the rest off) */
    reg_w(2, 2, 0x00);
    reg_w(2, 3, 0x80);
    reg_w(3, 1, 0x00);
    reg_w(3, 2, 0x80);
    reg_w(3, 3, 0x20);
    tick(451584 * 2);
    n = ps1_cdrom_audio_pull(cd, pcm, 30000);
    CHECK(n >= 588 && pcm[0] == -pcm[1] && pcm[0] < 0, "swapped: L %d R %d", pcm[0], pcm[1]);
    /* Mute */
    while (iflag())
        ack();
    command(0x0B, NULL, 0);
    while ((i = wait_int(PS1_CPU_HZ, NULL, 1024)) == 1) {
        response(r);
        ack();
    }
    ack();
    ps1_cdrom_audio_pull(cd, pcm, 30000);
    tick(451584 * 2);
    n = ps1_cdrom_audio_pull(cd, pcm, 30000);
    int zero = 1;
    for (size_t k = 0; k < n; k++)
        zero &= pcm[2 * k] == 0 && pcm[2 * k + 1] == 0;
    CHECK(n >= 588 && zero, "muted CD-DA not silent (%zu frames)", n);

    /* the end of the disc: INT4, play and motor off */
    fresh(d);
    uint8_t loc[3] = {0x00, 0x03, 0x60}; /* LBA 135 */
    expect(0x02, loc, 3, 3, r);
    expect(0x03, NULL, 0, 3, r);
    i = wait_int(PS1_CPU_HZ * 2, NULL, 1024);
    int nn = response(r);
    CHECK(i == 4 && nn == 1 && !(r[0] & 0x80) && !(r[0] & 0x02), "end of disc INT%d stat %02X", i, r[0]);
    ack();

    /* autopause at the end of track 1 (playing the data track: silence) */
    fresh(d);
    m = 0x02;
    expect(0x0E, &m, 1, 3, r);
    uint8_t loc1[3] = {0x00, 0x02, 0x38}; /* LBA 38 */
    expect(0x02, loc1, 3, 3, r);
    expect(0x03, NULL, 0, 3, r);
    i = wait_int(PS1_CPU_HZ * 2, NULL, 1024);
    response(r);
    CHECK(i == 4 && !(r[0] & 0x80) && (r[0] & 0x02), "autopause INT%d stat %02X", i, r[0]);
    ack();
    /* GetlocP after the pause: track 2 index 1? no: the drive stopped on the first sector of track 2 */
    nn = expect(0x11, NULL, 0, 3, r);
    CHECK(nn == 8 && (r[0] == 0x01 || r[0] == 0x02), "GetlocP track %02X", r[0]);
}

static void test_init_stop(ps1_disc *d)
{
    fresh(d);
    uint8_t r[16];
    command(0x0A, NULL, 0);
    uint64_t took;
    int i = wait_int(PS1_CPU_HZ, &took, 64);
    CHECK(i == 3 && took >= 0x13CCE - 64 && took <= 0x13CCE + 64, "Init INT%d after %llu", i, (unsigned long long)took);
    response(r);
    ack();
    i = wait_int(PS1_CPU_HZ, &took, 64);
    CHECK(i == 2, "Init INT2: %d", i);
    ack();
    int n = expect(0x0F, NULL, 0, 3, r);
    CHECK(n == 5 && r[1] == 0x20, "mode after Init %02X", r[1]);
    command(0x08, NULL, 0);
    wait_int(PS1_CPU_HZ, NULL, 256);
    response(r);
    ack();
    i = wait_int(PS1_CPU_HZ, &took, 1024);
    n = response(r);
    CHECK(i == 2 && r[0] == 0x00, "Stop INT2 stat %02X", r[0]);
    CHECK(took >= 0xD38ACA - 2048 && took <= 0xD38ACA + 2048, "Stop second response after %llu",
          (unsigned long long)took);
    ack();
    n = expect(0x01, NULL, 0, 3, r);
    CHECK(r[0] == 0x00, "stat after Stop %02X", r[0]);
    /* the lid: a disc swap sets the shell-open bit until Getstat reads it */
    ps1_cdrom_set_disc(cd, d);
    n = expect(0x01, NULL, 0, 3, r);
    CHECK(r[0] & 0x10, "shell open after a swap: %02X", r[0]);
    n = expect(0x01, NULL, 0, 3, r);
    CHECK(!(r[0] & 0x10), "shell bit cleared by Getstat: %02X", r[0]);
}

/* CD audio into the SPU: CD input enabled, CD volume, capture buffer of the left channel. */
static void test_spu_mix(ps1_disc *d)
{
    fresh(d);
    ps1_irq_sink sink = {raise_cb, NULL};
    ps1_spu *spu = ps1_spu_create(sink);
    ps1_spu_write16(spu, 0x1AA, 0xC001); /* on, unmuted, CD input */
    ps1_spu_write16(spu, 0x180, 0x3FFF);
    ps1_spu_write16(spu, 0x182, 0x3FFF);
    ps1_spu_write16(spu, 0x1B0, 0x7FFF);
    ps1_spu_write16(spu, 0x1B2, 0x7FFF);
    uint8_t r[16], t = 0x02;
    expect(0x03, &t, 1, 3, r);
    static int16_t out[2 * 30000];
    for (int k = 0; k < 400; k++) {
        tick(768 * 40);
        ps1_spu_tick(spu, 768 * 40, cd);
        if (iflag())
            ack();
    }
    size_t n = ps1_spu_audio_pull(spu, out, 30000);
    int nonzero = 0;
    for (size_t k = 0; k < n; k++)
        nonzero += out[2 * k] != 0;
    CHECK(n > 5000 && nonzero > (int)n / 4, "SPU output with CD audio: %d of %zu non-zero", nonzero, n);
    /* capture buffer 0 holds raw CD left samples: lba * 16 + i is a ramp, steps of 1 */
    uint32_t w[256];
    ps1_spu_write16(spu, 0x1A6, 0x0000);
    ps1_spu_write16(spu, 0x1AA, 0xC031); /* DMA read */
    ps1_spu_dma_read(spu, w, 256);
    int ramps = 0;
    for (int k = 1; k < 256; k++) {
        int16_t a = (int16_t)w[k - 1], b = (int16_t)(w[k - 1] >> 16);
        ramps += (int16_t)(b - a) == 1;
    }
    CHECK(ramps > 200, "capture buffer: %d ramp steps", ramps);
    ps1_spu_destroy(spu);
}

int main(void)
{
    scratch();
    ps1_disc *d = make_disc();
    test_getstat(d);
    test_errors(d);
    test_info(d);
    test_getid(d);
    test_read(d);
    test_xa(d);
    test_play(d);
    test_init_stop(d);
    test_spu_mix(d);
    if (cd)
        ps1_cdrom_destroy(cd);
    ps1_disc_close(d);
    scratch_cleanup();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ps1_cdrom: all tests passed\n");
    return 0;
}
