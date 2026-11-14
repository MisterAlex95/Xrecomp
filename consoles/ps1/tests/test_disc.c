/* Unit tests of ps1_disc.c (P-1, XBA-159): tiny synthetic images written by the test itself (never game data):
 * a raw .bin, an .iso, .cue sheets (several files, INDEX 00, PREGAP, relative paths, case-insensitive names, MODE1/2048,
 * MOTOROLA audio), fd:N, and CHD (an uncompressed v5 CHD generated here; a compressed one from chdman when it is on
 * the PATH). Our own code. */
#include "dev_test.h"

#include <fcntl.h>

static uint8_t sec[2352], got[2352];

static void test_raw_bin(void)
{
    enum { N = 20 };
    static uint8_t img[N * 2352];
    for (uint32_t l = 0; l < N; l++)
        make_data_sector(img + l * 2352, l, 0x08);
    write_file("raw.bin", img, sizeof img);
    char p[700], err[256] = "";
    path_in(p, sizeof p, "raw.bin");
    ps1_disc *d = ps1_disc_open(p, err, sizeof err);
    CHECK(d, "open raw.bin: %s", err);
    if (!d)
        return;
    CHECK(ps1_disc_track_count(d) == 1, "tracks %d", ps1_disc_track_count(d));
    CHECK(ps1_disc_size(d) == N, "size %u", ps1_disc_size(d));
    uint32_t start = 99;
    int audio = 9;
    CHECK(ps1_disc_track(d, 1, &start, &audio) == 0 && start == 0 && audio == 0, "track 1 %u %d", start, audio);
    CHECK(ps1_disc_track(d, 2, &start, &audio) == -1, "track 2 exists");
    for (uint32_t l = 0; l < N; l++) {
        CHECK(ps1_disc_read_sector(d, l, got) == 0, "read %u", l);
        CHECK(memcmp(got, img + l * 2352, 2352) == 0, "sector %u differs", l);
    }
    CHECK(ps1_disc_read_sector(d, N, got) == -1, "read past the end");
    ps1_disc_close(d);

    /* the same image through fd: */
    int fd = open(p, O_RDONLY);
    char fdp[32];
    snprintf(fdp, sizeof fdp, "fd:%d", fd);
    d = ps1_disc_open(fdp, err, sizeof err);
    close(fd); /* the disc keeps its own descriptor */
    CHECK(d, "open %s: %s", fdp, err);
    if (d) {
        CHECK(ps1_disc_size(d) == N, "fd size %u", ps1_disc_size(d));
        CHECK(ps1_disc_read_sector(d, 7, got) == 0 && memcmp(got, img + 7 * 2352, 2352) == 0, "fd sector 7");
        ps1_disc_close(d);
    }
}

static void check_synth_header(const uint8_t *s, uint32_t lba)
{
    uint32_t a = lba + 150;
    CHECK(s[0] == 0 && s[1] == 0xFF && s[10] == 0xFF && s[11] == 0, "sync of %u", lba);
    CHECK(s[12] == to_bcd(a / 4500) && s[13] == to_bcd((a / 75) % 60) && s[14] == to_bcd(a % 75), "MSF of %u", lba);
    CHECK(s[15] == 2, "mode of %u: %d", lba, s[15]);
    CHECK(s[16] == 0 && s[17] == 0 && s[18] == 8 && s[19] == 0 && s[22] == 8, "subheader of %u", lba);
}

static void test_iso(void)
{
    enum { N = 300 }; /* 300 * 2048 = 614400, not a multiple of 2352: no confusion with raw */
    static uint8_t img[N * 2048];
    for (int i = 0; i < (int)sizeof img; i++)
        img[i] = (uint8_t)(i * 13 + (i >> 11));
    write_file("game.iso", img, sizeof img);
    char p[700], err[256] = "";
    path_in(p, sizeof p, "game.iso");
    ps1_disc *d = ps1_disc_open(p, err, sizeof err);
    CHECK(d, "open game.iso: %s", err);
    if (!d)
        return;
    CHECK(ps1_disc_size(d) == N, "iso size %u", ps1_disc_size(d));
    for (uint32_t l = 0; l < N; l += 37) {
        CHECK(ps1_disc_read_sector(d, l, got) == 0, "iso read %u", l);
        check_synth_header(got, l);
        CHECK(memcmp(got + 24, img + l * 2048, 2048) == 0, "iso payload %u", l);
    }
    ps1_disc_close(d);
    /* fd: recognises a 2048-byte image by the missing sync pattern */
    int fd = open(p, O_RDONLY);
    char fdp[32];
    snprintf(fdp, sizeof fdp, "fd:%d", fd);
    d = ps1_disc_open(fdp, err, sizeof err);
    close(fd);
    CHECK(d && ps1_disc_size(d) == N, "iso through fd: %s", err);
    if (d) {
        CHECK(ps1_disc_read_sector(d, 5, got) == 0 && memcmp(got + 24, img + 5 * 2048, 2048) == 0, "iso fd 5");
        ps1_disc_close(d);
    }
}

/* data.bin in sub/ (track 1, 30 sectors MODE2/2352); Audio.bin (named audio.BIN in the sheet) with track 2
 * (INDEX 00 at 0, INDEX 01 at 10 frames) and track 3 (PREGAP 5 frames, INDEX 01 at 30), 50 frames in all. */
static void test_cue(void)
{
    static uint8_t data[30 * 2352], audio[50 * 2352];
    for (uint32_t l = 0; l < 30; l++)
        make_data_sector(data + l * 2352, l, 0x08);
    for (uint32_t f = 0; f < 50; f++)
        make_audio_sector(audio + f * 2352, 1000 + f);
    char p[700];
    path_in(p, sizeof p, "sub");
    mkdir(p, 0755);
    write_file("sub/data.bin", data, sizeof data);
    write_file("Audio.bin", audio, sizeof audio);
    const char *cue = "\xEF\xBB\xBFREM synthetic test sheet\r\n"
                      "CATALOG 0000000000000\r\n"
                      "FILE \"sub/data.bin\" BINARY\r\n"
                      "  TRACK 01 MODE2/2352\r\n"
                      "    INDEX 01 00:00:00\r\n"
                      "FILE \"audio.BIN\" BINARY\r\n"
                      "  TRACK 02 AUDIO\r\n"
                      "    FLAGS DCP\r\n"
                      "    INDEX 00 00:00:00\r\n"
                      "    INDEX 01 00:00:10\r\n"
                      "  TRACK 03 AUDIO\r\n"
                      "    PREGAP 00:00:05\r\n"
                      "    INDEX 01 00:00:30\r\n";
    write_file("game.cue", cue, strlen(cue));
    char err[256] = "";
    path_in(p, sizeof p, "game.cue");
    ps1_disc *d = ps1_disc_open(p, err, sizeof err);
    CHECK(d, "open game.cue: %s", err);
    if (!d)
        return;
    CHECK(ps1_disc_track_count(d) == 3, "cue tracks %d", ps1_disc_track_count(d));
    uint32_t s1 = 1, s2 = 0, s3 = 0;
    int a1 = 1, a2 = 0, a3 = 0;
    ps1_disc_track(d, 1, &s1, &a1);
    ps1_disc_track(d, 2, &s2, &a2);
    ps1_disc_track(d, 3, &s3, &a3);
    CHECK(s1 == 0 && !a1, "track 1 at %u audio %d", s1, a1);
    CHECK(s2 == 40 && a2, "track 2 at %u audio %d", s2, a2);
    CHECK(s3 == 65 && a3, "track 3 at %u audio %d", s3, a3);
    CHECK(ps1_disc_size(d) == 85, "cue size %u", ps1_disc_size(d));
    CHECK(ps1_disc_read_sector(d, 29, got) == 0 && memcmp(got, data + 29 * 2352, 2352) == 0, "lba 29");
    CHECK(ps1_disc_read_sector(d, 30, got) == 0 && memcmp(got, audio, 2352) == 0, "lba 30 = track 2 INDEX 00");
    CHECK(ps1_disc_read_sector(d, 40, got) == 0 && memcmp(got, audio + 10 * 2352, 2352) == 0, "lba 40");
    CHECK(ps1_disc_read_sector(d, 59, got) == 0 && memcmp(got, audio + 29 * 2352, 2352) == 0, "lba 59");
    static const uint8_t zero[2352];
    for (uint32_t l = 60; l < 65; l++)
        CHECK(ps1_disc_read_sector(d, l, got) == 0 && memcmp(got, zero, 2352) == 0, "PREGAP lba %u not silent", l);
    CHECK(ps1_disc_read_sector(d, 65, got) == 0 && memcmp(got, audio + 30 * 2352, 2352) == 0, "lba 65");
    CHECK(ps1_disc_read_sector(d, 84, got) == 0 && memcmp(got, audio + 49 * 2352, 2352) == 0, "lba 84");
    CHECK(ps1_disc_read_sector(d, 85, got) == -1, "lba 85 past the end");
    ps1_disc_close(d);

    /* MODE1/2048 + a MOTOROLA (big-endian) audio file in one sheet */
    static uint8_t iso[10 * 2048], be[4 * 2352];
    for (int i = 0; i < (int)sizeof iso; i++)
        iso[i] = (uint8_t)(i ^ (i >> 8));
    for (uint32_t f = 0; f < 4; f++) {
        make_audio_sector(sec, 7 + f);
        for (int i = 0; i < 2352; i += 2) {
            be[f * 2352 + i] = sec[i + 1];
            be[f * 2352 + i + 1] = sec[i];
        }
    }
    write_file("d.iso", iso, sizeof iso);
    write_file("m.raw", be, sizeof be);
    const char *cue2 = "FILE d.iso BINARY\nTRACK 1 MODE1/2048\nINDEX 1 00:00:00\n"
                       "FILE m.raw MOTOROLA\nTRACK 2 AUDIO\nINDEX 1 00:00:00\n";
    write_file("two.cue", cue2, strlen(cue2));
    path_in(p, sizeof p, "two.cue");
    d = ps1_disc_open(p, err, sizeof err);
    CHECK(d, "open two.cue: %s", err);
    if (d) {
        CHECK(ps1_disc_size(d) == 14, "two.cue size %u", ps1_disc_size(d));
        CHECK(ps1_disc_read_sector(d, 3, got) == 0 && memcmp(got + 24, iso + 3 * 2048, 2048) == 0, "2048 track");
        check_synth_header(got, 3);
        make_audio_sector(sec, 9);
        CHECK(ps1_disc_read_sector(d, 12, got) == 0 && memcmp(got, sec, 2352) == 0, "MOTOROLA audio swapped back");
        ps1_disc_close(d);
    }
}

static void test_errors(void)
{
    char p[700], err[256];
    err[0] = 0;
    CHECK(ps1_disc_open("/nonexistent/x.bin", err, sizeof err) == NULL && err[0], "missing file");
    const char *bad = "FILE missing.bin BINARY\nTRACK 01 MODE2/2352\nINDEX 01 00:00:00\n";
    write_file("bad.cue", bad, strlen(bad));
    path_in(p, sizeof p, "bad.cue");
    err[0] = 0;
    CHECK(ps1_disc_open(p, err, sizeof err) == NULL && strstr(err, "missing.bin"), "missing track file: %s", err);
    const char *wave = "FILE x.wav WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n";
    write_file("wave.cue", wave, strlen(wave));
    path_in(p, sizeof p, "wave.cue");
    err[0] = 0;
    CHECK(ps1_disc_open(p, err, sizeof err) == NULL && strstr(err, "WAVE"), "WAVE: %s", err);
    const char *noidx = "FILE raw.bin BINARY\nTRACK 01 MODE2/2352\n";
    write_file("noidx.cue", noidx, strlen(noidx));
    path_in(p, sizeof p, "noidx.cue");
    err[0] = 0;
    CHECK(ps1_disc_open(p, err, sizeof err) == NULL && strstr(err, "INDEX"), "no INDEX 01: %s", err);
    CHECK(ps1_disc_open("fd:x", err, sizeof err) == NULL, "fd:x");
    ps1_disc_close(NULL);
}

/* ---- CHD ------------------------------------------------------------------------------------------------------ */

#ifdef XBR_PS1_CHD

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}
static void be64(uint8_t *p, uint64_t v)
{
    be32(p, (uint32_t)(v >> 32));
    be32(p + 4, (uint32_t)v);
}

/* An uncompressed CHD v5 (MAME layout: 124-byte header, a map of 4-byte hunk numbers, CHT2 metadata entries, hunks
 * of 8 frames of 2352 + 96 bytes; each track padded to 4 frames; audio stored big-endian). Track 1: MODE2_RAW, 22
 * frames (padded to 24); track 2: AUDIO, 6 frames with PREGAP 2 stored (PGTYPE VAUDIO), padded to 8. */
static int write_chd(const char *name, uint8_t *frames_out /* 32 * 2352, as the disc should read */)
{
    enum { FRAME = 2448, HUNK = 8 * FRAME, NFR = 32, NHUNK = NFR / 8 };
    static uint8_t file[(NHUNK + 1) * HUNK];
    memset(file, 0, sizeof file);
    memcpy(file, "MComprHD", 8);
    be32(file + 8, 124);
    be32(file + 12, 5);
    be64(file + 32, (uint64_t)NHUNK * HUNK);
    be64(file + 40, 124);                     /* map */
    uint32_t meta = 124 + NHUNK * 4;
    be64(file + 48, meta);
    be32(file + 56, HUNK);
    be32(file + 60, FRAME);
    for (int h = 0; h < NHUNK; h++)
        be32(file + 124 + h * 4, (uint32_t)(h + 1));
    const char *m1 = "TRACK:1 TYPE:MODE2_RAW SUBTYPE:NONE FRAMES:22 PREGAP:0 PGTYPE:MODE2_RAW PGSUB:NONE POSTGAP:0";
    const char *m2 = "TRACK:2 TYPE:AUDIO SUBTYPE:NONE FRAMES:6 PREGAP:2 PGTYPE:VAUDIO PGSUB:NONE POSTGAP:0";
    uint32_t e1 = meta, e2 = e1 + 16 + (uint32_t)strlen(m1) + 1;
    memcpy(file + e1, "CHT2", 4);
    be32(file + e1 + 4, (uint32_t)strlen(m1) + 1);
    be64(file + e1 + 8, e2);
    memcpy(file + e1 + 16, m1, strlen(m1));
    memcpy(file + e2, "CHT2", 4);
    be32(file + e2 + 4, (uint32_t)strlen(m2) + 1);
    be64(file + e2 + 8, 0);
    memcpy(file + e2 + 16, m2, strlen(m2));
    if (e2 + 16 + strlen(m2) + 1 > HUNK)
        return -1;
    /* frames: 0..21 data (LBA 0..21), 22..23 padding, 24..29 audio (LBA 22..27: 22, 23 = pregap, 24 = INDEX 01) */
    for (int f = 0; f < NFR; f++) {
        uint8_t *dst = file + HUNK + f * FRAME;
        if (f < 22) {
            make_data_sector(dst, (uint32_t)f, 0x08);
            memcpy(frames_out + f * 2352, dst, 2352);
        } else if (f >= 24 && f < 30) {
            make_audio_sector(sec, (uint32_t)(500 + f));
            memcpy(frames_out + (f - 2) * 2352, sec, 2352);
            for (int i = 0; i < 2352; i += 2) {
                dst[i] = sec[i + 1];
                dst[i + 1] = sec[i];
            }
        }
    }
    write_file(name, file, sizeof file);
    return 0;
}

#endif

static void test_chd(void)
{
#ifdef XBR_PS1_CHD
    static uint8_t want[32 * 2352];
    char p[700], err[256] = "";
    if (write_chd("t.chd", want) != 0) {
        CHECK(0, "metadata does not fit");
        return;
    }
    path_in(p, sizeof p, "t.chd");
    for (int pass = 0; pass < 2; pass++) {
        ps1_disc *d;
        int fd = -1;
        if (pass == 0) {
            d = ps1_disc_open(p, err, sizeof err);
        } else {
            fd = open(p, O_RDONLY);
            char fdp[32];
            snprintf(fdp, sizeof fdp, "fd:%d", fd);
            d = ps1_disc_open(fdp, err, sizeof err);
        }
        CHECK(d, "open chd (pass %d): %s", pass, err);
        if (fd >= 0)
            close(fd);
        if (!d)
            continue;
        CHECK(ps1_disc_track_count(d) == 2, "chd tracks %d", ps1_disc_track_count(d));
        uint32_t s2 = 0;
        int a2 = 0;
        ps1_disc_track(d, 2, &s2, &a2);
        CHECK(s2 == 24 && a2, "chd track 2 at %u (audio %d)", s2, a2);
        CHECK(ps1_disc_size(d) == 28, "chd size %u", ps1_disc_size(d));
        for (uint32_t l = 0; l < 28; l++)
            CHECK(ps1_disc_read_sector(d, l, got) == 0 && memcmp(got, want + l * 2352, 2352) == 0, "chd lba %u", l);
        CHECK(ps1_disc_read_sector(d, 28, got) == -1, "chd past the end");
        ps1_disc_close(d);
    }

    /* chdman (MAME tools), when installed: a compressed CD CHD from a cue sheet */
    if (system("command -v chdman >/dev/null 2>&1") == 0) {
        /* the sheet of test_cue without what chdman does not take (BOM, a name in the wrong case, a second track in
         * the same file: chdman miscounts those tiny tracks) */
        const char *plain = "FILE \"sub/data.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n"
                            "FILE \"Audio.bin\" BINARY\n  TRACK 02 AUDIO\n    INDEX 00 00:00:00\n    INDEX 01 00:00:10\n";
        write_file("plain.cue", plain, strlen(plain));
        char cmd[3000], cue[700], out[700], log[700];
        path_in(cue, sizeof cue, "plain.cue");
        path_in(out, sizeof out, "c.chd");
        path_in(log, sizeof log, "chdman.log");
        snprintf(cmd, sizeof cmd, "chdman createcd -i '%s' -o '%s' >'%s' 2>&1", cue, out, log);
        if (system(cmd) == 0) {
            char cp[700];
            path_in(cp, sizeof cp, "plain.cue");
            ps1_disc *ref = ps1_disc_open(cp, err, sizeof err), *d = ps1_disc_open(out, err, sizeof err);
            CHECK(ref && d, "chdman chd: %s", err);
            if (ref && d) {
                CHECK(ps1_disc_track_count(d) == 2, "chdman tracks %d", ps1_disc_track_count(d));
                CHECK(ps1_disc_size(d) == ps1_disc_size(ref), "chdman size %u", ps1_disc_size(d));
                for (int t = 1; t <= 2; t++) {
                    uint32_t a = 0, b = 1;
                    ps1_disc_track(ref, t, &a, NULL);
                    ps1_disc_track(d, t, &b, NULL);
                    CHECK(a == b, "chdman track %d at %u, cue at %u", t, b, a);
                }
                for (uint32_t l = 0; l < ps1_disc_size(ref); l++) {
                    ps1_disc_read_sector(ref, l, sec);
                    CHECK(ps1_disc_read_sector(d, l, got) == 0 && memcmp(got, sec, 2352) == 0, "chdman lba %u", l);
                }
            }
            ps1_disc_close(ref);
            ps1_disc_close(d);
        } else {
            printf("note: chdman failed, compressed CHD not tested:\n");
            snprintf(cmd, sizeof cmd, "tail -5 '%s'", log);
            if (system(cmd) != 0)
                printf("(no log)\n");
        }
    } else {
        printf("note: chdman not installed, compressed CHD not tested\n");
    }
#else
    char p[700], err[256] = "";
    write_file("t.chd", "MComprHD", 8);
    path_in(p, sizeof p, "t.chd");
    CHECK(ps1_disc_open(p, err, sizeof err) == NULL && strstr(err, "XBR_PS1_CHD"), "chd without support: %s", err);
    printf("note: built without XBR_PS1_CHD\n");
#endif
}

int main(void)
{
    scratch();
    test_raw_bin();
    test_iso();
    test_cue();
    test_errors();
    test_chd();
    scratch_cleanup();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ps1_disc: all tests passed\n");
    return 0;
}
