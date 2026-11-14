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

/* A disc with a pregap: data track 1 LBA 0..44 (45 sectors), audio track 2 with INDEX 00 at 45 and INDEX 01 at 50,
 * 150 sectors in all. */
static ps1_disc *make_pregap_disc(void)
{
    static uint8_t img[150 * 2352];
    for (uint32_t l = 0; l < 45; l++)
        make_data_sector(img + l * 2352, l, 0x08);
    for (uint32_t l = 45; l < 150; l++)
        make_audio_sector(img + l * 2352, 2000 + l);
    write_file("pregap.bin", img, sizeof img);
    const char *cue = "FILE \"pregap.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n"
                      "  TRACK 02 AUDIO\n    INDEX 00 00:00:45\n    INDEX 01 00:00:50\n";
    write_file("pregap.cue", cue, strlen(cue));
    char p[700], err[256] = "";
    path_in(p, sizeof p, "pregap.cue");
    ps1_disc *dd = ps1_disc_open(p, err, sizeof err);
    if (!dd) {
        printf("cannot open the pregap disc: %s\n", err);
        exit(1);
    }
    return dd;
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
    CHECK(took + 0xC4E1 >= 1036808 - 0x2000 && took <= 1036808 + 0x2000, "Pause second response after %llu",
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

/* ---- OpenBIOS's file reads (the trace of its SYSTEM.CNF boot) ---------------------------------------------- */

/* Like OpenBIOS's IRQ handler: acknowledge first, then read the response. Returns the interrupt type. */
static int bios_wait(uint8_t *r, int *n)
{
    int i = wait_int(PS1_CPU_HZ * 2, NULL, 256);
    ack();
    *n = response(r);
    return i;
}

static void bios_cmd(uint8_t c, const uint8_t *p, int np, int second)
{
    uint8_t r[16];
    int n;
    command(c, p, np);
    int i = bios_wait(r, &n);
    CHECK(i == 3 && n >= 1, "command %02X: INT%d (%d bytes)", c, i, n);
    if (second) {
        i = bios_wait(r, &n);
        CHECK(i == 2, "command %02X: second INT%d", c, i);
    }
}

/* Setmode 80h, ReadN, one sector by DMA as OpenBIOS's initiateDMA does (BFRD 0, then 80h), then Pause. */
static void bios_read(uint32_t want_lba)
{
    uint8_t r[16], s[2352];
    uint32_t w[512];
    int n;
    uint8_t m = 0x80;
    bios_cmd(0x0E, &m, 1, 0);
    bios_cmd(0x06, NULL, 0, 0);
    int i = bios_wait(r, &n);
    CHECK(i == 1, "INT%d instead of INT1", i);
    reg_w(0, 3, 0x00);
    reg_w(0, 3, 0x80);
    ps1_cdrom_dma_read(cd, w, 512);
    make_data_sector(s, want_lba, 0x08);
    int ok = memcmp(w, s + 24, 0x800) == 0;
    CHECK(ok, "read of LBA %u delivered another sector", want_lba);
    if (!ok) {
        for (uint32_t l = 0; l < DATA_SECTORS; l++) {
            make_data_sector(s, l, 0x08);
            if (memcmp(w, s + 24, 0x800) == 0)
                printf("  (it is LBA %u)\n", l);
        }
    }
    command(0x09, NULL, 0);
    while ((i = bios_wait(r, &n)) == 1) {
    }
    CHECK(i == 3, "Pause INT%d", i);
    i = bios_wait(r, &n);
    CHECK(i == 2, "Pause second INT%d", i);
}

static void bios_setloc(uint32_t lba)
{
    uint32_t a = lba + 150;
    uint8_t p[3] = {to_bcd(a / 4500), to_bcd((a / 75) % 60), to_bcd(a % 75)};
    bios_cmd(0x02, p, 3, 0);
}

/* The command sequence OpenBIOS sends to boot a disc (traced on our homebrew cpu.cue): a Read after SeekL / SeekP
 * must start at the seek target, not where the last Read stopped. */
static void test_openbios_sequence(ps1_disc *d)
{
    fresh(d);
    bios_cmd(0x0A, NULL, 0, 1); /* Init */
    bios_setloc(16);            /* Setloc 00:02:16, Setmode 80h, ReadN: the PVD */
    bios_read(16);
    bios_setloc(26);            /* Setloc, Setmode, ReadN: "the root directory" (LBA 22 on cpu.cue; 20..23 are XA here) */
    bios_read(26);
    bios_cmd(0x0A, NULL, 0, 1); /* Init, Setloc 00:02:16, SeekL, Setmode, ReadN: the PVD again, */
    bios_setloc(16);            /* not the root directory */
    bios_cmd(0x15, NULL, 0, 1);
    bios_read(16);
    bios_cmd(0x01, NULL, 0, 0); /* Getstat, Setloc 00:02:00, SeekL, Setmode, ReadN */
    bios_setloc(0);
    bios_cmd(0x15, NULL, 0, 1);
    bios_read(0);
    bios_cmd(0x01, NULL, 0, 0); /* Getstat, Setloc, SeekL, Setmode, ReadN: "SYSTEM.CNF" */
    bios_setloc(27);
    bios_cmd(0x15, NULL, 0, 1);
    bios_read(27);
    bios_setloc(25);            /* SeekP then a Read: the same rule */
    bios_cmd(0x16, NULL, 0, 1);
    bios_read(25);
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
        good += pcm[2 * k] == pcm[2 * k + 1] && pcm[2 * k] >= 0x2A00 && pcm[2 * k] <= 0x2C00;
    /* the zigzag tables' gain is about 0.906 (psx-spx): 3000h comes out near 2B80h */
    CHECK(good == (int)n - 100, "XA samples: %d of %zu near 2B80h (sample 200: %04X)", good, n - 100,
          (uint16_t)pcm[400]);
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
    /* the lid: a disc swap opens it (INT5 01h 08h) and sets the shell-open bit until Getstat reads it */
    ps1_cdrom_set_disc(cd, d);
    i = wait_int(PS1_CPU_HZ, NULL, 256);
    n = response(r);
    CHECK(i == 5 && n == 2 && r[0] == 0x01 && r[1] == 0x08, "swap: INT%d %02X %02X", i, r[0], r[1]);
    ack();
    n = expect(0x01, NULL, 0, 3, r);
    CHECK((r[0] & 0x10) && (r[0] & 0x02), "shell open, motor on after a swap: %02X", r[0]);
    n = expect(0x01, NULL, 0, 3, r);
    CHECK(!(r[0] & 0x10), "shell bit cleared by Getstat: %02X", r[0]);
}

/* Drains interrupts (acknowledging) until none comes for a while. */
static void drain(void)
{
    uint8_t r[16];
    while (wait_int(PS1_CPU_HZ * 2, NULL, 1024)) {
        response(r);
        ack();
    }
}

/* The lid (XBA-166): open while reading stops the drive with INT5 08h, commands needing the disc fail with 80h,
 * GetID answers no disc, Getstat keeps bit 4 while it is open; closed again, bit 4 clears on the next Getstat and the
 * disc reads. */
static void test_lid(ps1_disc *d)
{
    fresh(d);
    uint8_t r[16];
    uint8_t loc[3] = {0x00, 0x02, 0x10};
    expect(0x02, loc, 3, 3, r);
    expect(0x06, NULL, 0, 3, r);
    int i = wait_int(PS1_CPU_HZ, NULL, 256);
    CHECK(i == 1, "reading: INT%d", i);
    ack();
    ps1_cdrom_set_lid(cd, 1);
    i = wait_int(PS1_CPU_HZ, NULL, 256);
    int n = response(r);
    CHECK(i == 5 && n == 2 && r[0] == 0x01 && r[1] == 0x08, "lid open: INT%d %02X %02X", i, r[0], r[1]);
    ack();
    tick(451584 * 3);
    CHECK(iflag() == 0, "no INT1 with the lid open");
    n = expect(0x01, NULL, 0, 3, r);
    CHECK(r[0] == 0x10, "Getstat with the lid open %02X", r[0]);
    n = expect(0x01, NULL, 0, 3, r);
    CHECK(r[0] == 0x10, "Getstat with the lid still open %02X", r[0]);
    n = expect(0x06, NULL, 0, 5, r);
    CHECK(n == 2 && r[0] == 0x11 && r[1] == 0x80, "ReadN with the lid open %02X %02X", r[0], r[1]);
    command(0x1A, NULL, 0);
    wait_int(PS1_CPU_HZ, NULL, 256);
    response(r);
    ack();
    i = wait_int(PS1_CPU_HZ, NULL, 256);
    n = response(r);
    CHECK(i == 5 && r[0] == 0x08 && r[1] == 0x40, "GetID with the lid open: INT%d %02X %02X", i, r[0], r[1]);
    ack();
    ps1_cdrom_set_lid(cd, 0);
    n = expect(0x01, NULL, 0, 3, r);
    CHECK(r[0] == 0x12, "Getstat once closed %02X", r[0]);
    n = expect(0x01, NULL, 0, 3, r);
    CHECK(r[0] == 0x02, "Getstat after that %02X", r[0]);
    n = expect(0x10, NULL, 0, 5, r);
    CHECK(r[1] == 0x80, "GetlocL before a read after closing: %02X", r[1]);
    expect(0x02, loc, 3, 3, r);
    expect(0x06, NULL, 0, 3, r);
    i = wait_int(PS1_CPU_HZ, NULL, 256);
    CHECK(i == 1, "reading after the lid closed: INT%d", i);
    ack();
    /* a swap with the lid open changes the disc and nothing else */
    ps1_cdrom_set_lid(cd, 1);
    drain();
    ps1_cdrom_set_disc(cd, NULL);
    tick(100000);
    CHECK(iflag() == 0, "no interrupt for a swap with the lid open");
    ps1_cdrom_set_lid(cd, 0);
    n = expect(0x01, NULL, 0, 3, r);
    CHECK(r[0] == 0x10, "no disc: Getstat %02X (motor off)", r[0]);
    ps1_cdrom_set_disc(cd, d);
    drain();
}

static void setloc_lba(uint32_t lba)
{
    uint8_t r[16];
    uint32_t a = lba + 150;
    uint8_t loc[3] = {to_bcd(a / 4500), to_bcd((a / 75) % 60), to_bcd(a % 75)};
    expect(0x02, loc, 3, 3, r);
}

/* Sub-channel Q (XBA-166): GetlocP in a pregap (index 0, counting down, the next track's number), while playing (the
 * sector playing); GetQ's TOC entries. */
static void test_subq(ps1_disc *d)
{
    fresh(d);
    uint8_t r[16];
    /* track 2 of the pregap disc starts at LBA 50 (INDEX 01), its INDEX 00 at 45 */
    setloc_lba(47);
    command(0x15, NULL, 0);
    wait_int(PS1_CPU_HZ, NULL, 256);
    ack();
    int i = wait_int(PS1_CPU_HZ, NULL, 256);
    ack();
    CHECK(i == 2, "SeekL INT%d", i);
    int n = expect(0x11, NULL, 0, 3, r);
    CHECK(n == 8 && r[0] == 0x02 && r[1] == 0x00 && r[2] == 0 && r[3] == 0 && r[4] == 0x03 && r[5] == 0x00 &&
              r[6] == 0x02 && r[7] == 0x47,
          "GetlocP in the pregap: %02X %02X %02X:%02X:%02X %02X:%02X:%02X", r[0], r[1], r[2], r[3], r[4], r[5], r[6],
          r[7]);
    /* Play from there (1x): GetlocP follows the sector playing */
    expect(0x03, NULL, 0, 3, r);
    uint64_t t0 = now;
    while (now - t0 < 451584u * 14)
        if (wait_int(451584u * 14 - (now - t0), NULL, 1024))
            ack();
    n = expect(0x11, NULL, 0, 3, r);
    int ff = (r[7] >> 4) * 10 + (r[7] & 15);
    CHECK(n == 8 && r[0] == 0x02 && r[1] == 0x01 && r[5] == 0 && r[6] == 0x02 && ff >= 50 && ff <= 60,
          "GetlocP while playing: track %02X index %02X abs %02X:%02X ff %d", r[0], r[1], r[5], r[6], ff);
    expect(0x09, NULL, 0, 3, r);
    drain();
    /* the lead-out: past the last sector */
    setloc_lba(ps1_disc_size(d) + 1);
    n = expect(0x11, NULL, 0, 3, r);
    CHECK(n == 8, "GetlocP %d", n);
    /* GetQ: TOC entries */
    uint8_t q[2] = {0x01, 0x02};
    expect(0x1D, q, 2, 3, r);
    i = wait_int(PS1_CPU_HZ, NULL, 256);
    n = response(r);
    CHECK(i == 2 && n == 10 && r[0] == 0x01 && r[2] == 0x02 && r[7] == 0x00 && r[8] == 0x02 && r[9] == 0x50,
          "GetQ track 2: INT%d %d bytes %02X %02X %02X:%02X:%02X", i, n, r[0], r[2], r[7], r[8], r[9]);
    ack();
    q[1] = 0xA1;
    expect(0x1D, q, 2, 3, r);
    i = wait_int(PS1_CPU_HZ, NULL, 256);
    n = response(r);
    CHECK(i == 2 && r[2] == 0xA1 && r[7] == 0x02 && r[8] == 0x20, "GetQ A1: %02X last %02X type %02X", r[2], r[7],
          r[8]);
    ack();
    q[1] = 0xA0;
    expect(0x1D, q, 2, 3, r);
    i = wait_int(PS1_CPU_HZ, NULL, 256);
    n = response(r);
    CHECK(i == 2 && r[0] == 0x41 && r[7] == 0x01, "GetQ A0: %02X first %02X", r[0], r[7]);
    ack();
    q[1] = 0xA2;
    expect(0x1D, q, 2, 3, r);
    i = wait_int(PS1_CPU_HZ, NULL, 256);
    n = response(r);
    CHECK(i == 2 && r[7] == 0x00 && r[8] == 0x04 && r[9] == 0x00, "GetQ A2: lead-out %02X:%02X:%02X", r[7], r[8],
          r[9]);
    ack();
    q[1] = 0x09;
    expect(0x1D, q, 2, 3, r);
    i = wait_int(PS1_CPU_HZ, NULL, 256);
    n = response(r);
    CHECK(i == 5 && r[1] == 0x80, "GetQ of a track that is not there: INT%d %02X", i, r[1]);
    ack();
}

/* CD-DA at 2x (XBA-166): sectors twice as fast, half the frames (double pitch); report peaks. */
static void test_cdda_speed(ps1_disc *d)
{
    fresh(d);
    uint8_t r[16];
    static int16_t pcm[2 * 60000];
    uint8_t m = 0x80 | 0x04; /* 2x, report */
    expect(0x0E, &m, 1, 3, r);
    uint8_t t = 0x02;
    expect(0x03, &t, 1, 3, r);
    uint64_t t0 = now;
    int reports = 0, peaks = 0, right = 0;
    size_t n = 0;
    while (now - t0 < PS1_CPU_HZ / 2) {
        n += ps1_cdrom_audio_pull(cd, pcm + 2 * n, 60000 - n);
        int i = wait_int(PS1_CPU_HZ / 2 - (now - t0), NULL, 1024);
        if (!i)
            break;
        response(r);
        if (i == 1) {
            reports++;
            uint16_t pk = (uint16_t)(r[6] | r[7] << 8);
            peaks += (pk & 0x7FFF) > 0;
            right += (pk & 0x8000) != 0;
            CHECK(((pk & 0x8000) != 0) == ((r[4] & 0x80) != 0), "peak channel bit %04X with seconds %02X", pk, r[4]);
        }
        ack();
    }
    n += ps1_cdrom_audio_pull(cd, pcm + 2 * n, 60000 - n);
    /* about 75 sectors in 0.5 s (150 a second, less the seek), 294 frames each: half of 1x's frames per second */
    CHECK(n >= 294 * 60 && n <= 294 * 76, "2x CD-DA frames %zu", n);
    CHECK(reports >= 8 && peaks == reports && right > 0 && right < reports, "%d reports, %d with a peak, %d right",
          reports, peaks, right);
    uint8_t s0[2352];
    make_audio_sector(s0, 1000);
    int16_t l0 = (int16_t)(s0[0] | s0[1] << 8), l1 = (int16_t)(s0[4] | s0[5] << 8);
    CHECK(pcm[0] == (int16_t)((l0 + l1) >> 1), "2x: frames averaged in pairs (%d, want %d)", pcm[0], (l0 + l1) >> 1);
}

/* Seek timing (XBA-166): a 100-sector seek then the first sector, at 1x and 2x, against the ps1-tests cdrom/timing
 * hardware log (ReadN to the first INT1: about 0.67 - 0.98 M cycles at 1x, 0.55 - 1.02 M at 2x). */
static void test_seek_timing(ps1_disc *d)
{
    for (int fast = 0; fast < 2; fast++) {
        fresh(d);
        uint8_t r[16];
        uint8_t m = fast ? 0x80 : 0x00;
        expect(0x0E, &m, 1, 3, r);
        setloc_lba(116);
        command(0x15, NULL, 0);
        while (wait_int(PS1_CPU_HZ, NULL, 256) != 2) {
            response(r);
            ack();
        }
        ack();
        setloc_lba(16);
        expect(0x06, NULL, 0, 3, r);
        uint64_t took;
        int i = wait_int(PS1_CPU_HZ, &took, 256);
        CHECK(i == 1 && took >= (fast ? 550000u : 650000u) && took <= 1000000u, "%dx seek + first sector: %llu",
              fast + 1, (unsigned long long)took);
        ack();
        expect(0x09, NULL, 0, 3, r);
        drain();
    }
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
    test_openbios_sequence(d);
    test_xa(d);
    test_play(d);
    test_init_stop(d);
    test_lid(d);
    test_spu_mix(d);
    test_cdda_speed(d);
    ps1_disc *pg = make_pregap_disc();
    test_subq(pg);
    test_seek_timing(pg);
    ps1_cdrom_destroy(cd);
    cd = NULL;
    ps1_disc_close(pg);
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
