/* The title key of a .chd PlayStation disc (XBA-159): recomp_pack_rom_identify (src/platform/recomp_pack.c) with the
 * PS1 disc reader as its disc hook (ps1_identify.c: ps1_disc.c + the vendored libchdr). The CHDs are written here
 * around a tiny ISO 9660 « PLAYSTATION » volume of our own (never game data): an uncompressed v5 one, and a v5 one in
 * chdman's shape (huffman-coded map, CD zlib codec « cdzl » hunks whose deflate streams use stored blocks, so the test
 * needs no zlib), plus broken ones that must fail cleanly. */
#include "dev_test.h"
#include "ps1_identify.h"
#include "recomp_pack.h"

#include <fcntl.h>

enum { FRAME = 2448, FPH = 8, HUNK = FPH * FRAME, NFR = 24, NHUNK = NFR / FPH };

static uint8_t frames[NFR * FRAME];   /* the disc as frames (sector + 96 bytes of subcode, zero) */

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v >> 16), put16(p + 2, v); }
static void put48(uint8_t *p, uint64_t v) { put16(p, (uint32_t)(v >> 32)), put32(p + 2, (uint32_t)v); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)(v >> 32)), put32(p + 4, (uint32_t)v); }
static void le_be32(uint8_t *p, uint32_t v)   /* ISO 9660 both-endian */
{
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
    put32(p + 4, v);
}

/* CRC-16/CCITT-FALSE, the CHD v5 map and hunk check. */
static uint16_t crc16(const uint8_t *p, size_t n)
{
    uint16_t c = 0xFFFF;
    while (n--) {
        c ^= (uint16_t)(*p++ << 8);
        for (int b = 0; b < 8; b++)
            c = (uint16_t)(c & 0x8000 ? (c << 1) ^ 0x1021 : c << 1);
    }
    return c;
}

static void dir_record(uint8_t *r, const char *name, uint32_t lba, uint32_t len, int dir)
{
    size_t nl = name[0] == 0 || name[0] == 1 ? 1 : strlen(name);   /* "\0" ., "\1" .. */
    r[0] = (uint8_t)(33 + nl + !(nl & 1));
    le_be32(r + 2, lba);
    le_be32(r + 10, len);
    r[25] = dir ? 2 : 0;
    r[32] = (uint8_t)nl;
    memcpy(r + 33, name, nl);
}

/* Frames 0..23, mode 2 form 1: PVD at 16 (system ID sys), terminator at 17, root directory at 18 (., .., SYSTEM.CNF;1),
 * SYSTEM.CNF at 19. */
static void make_disc(const char *sys, const char *cnf)
{
    memset(frames, 0, sizeof frames);
    for (uint32_t l = 0; l < NFR; l++) {
        uint8_t *s = frames + l * FRAME;
        make_data_sector(s, l, 0x08);
        memset(s + 24, 0, 2048);
    }
    uint8_t *pvd = frames + 16 * FRAME + 24;
    pvd[0] = 1;
    memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    memset(pvd + 8, ' ', 32);
    memcpy(pvd + 8, sys, strlen(sys));
    dir_record(pvd + 156, "\0", 18, 2048, 1);
    uint8_t *term = frames + 17 * FRAME + 24;
    term[0] = 255;
    memcpy(term + 1, "CD001", 5);
    uint8_t *root = frames + 18 * FRAME + 24;
    dir_record(root, "\0", 18, 2048, 1);
    dir_record(root + 34, "\1", 18, 2048, 1);
    dir_record(root + 68, "SYSTEM.CNF;1", 19, (uint32_t)strlen(cnf), 0);
    memcpy(frames + 19 * FRAME + 24, cnf, strlen(cnf));
}

static const char *META = "TRACK:1 TYPE:MODE2_RAW SUBTYPE:NONE FRAMES:24 PREGAP:0 PGTYPE:MODE2_RAW PGSUB:NONE POSTGAP:0";

/* The CHT2 metadata entry at off. */
static size_t put_meta(uint8_t *f, size_t off)
{
    memcpy(f + off, "CHT2", 4);
    put32(f + off + 4, (uint32_t)strlen(META) + 1);
    put64(f + off + 8, 0);
    memcpy(f + off + 16, META, strlen(META) + 1);
    return off + 16 + strlen(META) + 1;
}

static void header(uint8_t *f, uint32_t codec, uint64_t map, uint64_t meta)
{
    memcpy(f, "MComprHD", 8);
    put32(f + 8, 124);
    put32(f + 12, 5);
    put32(f + 16, codec);
    put64(f + 32, (uint64_t)NHUNK * HUNK);
    put64(f + 40, map);
    put64(f + 48, meta);
    put32(f + 56, HUNK);
    put32(f + 60, FRAME);
}

static uint8_t file[(NHUNK + 2) * HUNK];

/* Uncompressed v5: a map of 4-byte hunk numbers (hunk h at h * HUNK). */
static size_t chd_raw(void)
{
    memset(file, 0, sizeof file);
    header(file, 0, 124, 124 + NHUNK * 4);
    for (int h = 0; h < NHUNK; h++)
        put32(file + 124 + h * 4, (uint32_t)(h + 1));
    put_meta(file, 124 + NHUNK * 4);
    memcpy(file + HUNK, frames, sizeof frames);
    return (NHUNK + 1) * HUNK;
}

/* MSB-first bit writer (libchdr's bitstream). */
typedef struct { uint8_t *p; size_t bits; } bitw;
static void bits(bitw *w, uint32_t v, int n)
{
    while (n--) {
        if ((v >> n) & 1)
            w->p[w->bits >> 3] |= (uint8_t)(0x80 >> (w->bits & 7));
        w->bits++;
    }
}

/* A raw deflate stream in one fixed-Huffman block (BTYPE 01): literals, and runs of the previous byte as matches at
 * distance 1, which is all these mostly-zero sectors need to shrink below a hunk (chdman stores a hunk raw otherwise,
 * and libchdr refuses a compressed hunk larger than one). LSB-first bit packing, Huffman codes most significant bit
 * first (RFC 1951). */
typedef struct { uint8_t *p; size_t bits; } lsbw;
static void lsb(lsbw *w, uint32_t v, int n)   /* n bits of v, least significant first */
{
    for (int i = 0; i < n; i++, w->bits++)
        if ((v >> i) & 1)
            w->p[w->bits >> 3] |= (uint8_t)(1 << (w->bits & 7));
}
static void huff(lsbw *w, uint32_t code, int n)   /* a Huffman code, most significant bit first */
{
    for (int i = n - 1; i >= 0; i--)
        lsb(w, (code >> i) & 1, 1);
}
static void litlen(lsbw *w, unsigned v)
{
    if (v < 144) huff(w, 0x30 + v, 8);
    else if (v < 256) huff(w, 0x190 + v - 144, 9);
    else if (v < 280) huff(w, v - 256, 7);
    else huff(w, 0xC0 + v - 280, 8);
}
static size_t deflate_fixed(uint8_t *out, const uint8_t *in, size_t n)
{
    static const uint16_t base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83,
                                       99, 115, 131, 163, 195, 227, 258 };
    static const uint8_t extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5,
                                       5, 0 };
    lsbw w = { out, 0 };
    memset(out, 0, n + n / 8 + 16);
    lsb(&w, 1, 1);   /* BFINAL */
    lsb(&w, 1, 2);   /* BTYPE 01 */
    for (size_t i = 0; i < n;) {
        size_t run = 0;
        while (i > 0 && i + run < n && run < 258 && in[i + run] == in[i - 1])
            run++;
        if (run >= 3) {
            int c = 28;
            while (base[c] > run) c--;
            litlen(&w, 257 + (unsigned)c);
            lsb(&w, (uint32_t)(run - base[c]), extra[c]);
            huff(&w, 0, 5);   /* distance code 0: distance 1 */
            i += run;
        } else {
            litlen(&w, in[i++]);
        }
    }
    litlen(&w, 256);
    return (w.bits + 7) / 8;
}

/* One cdzl hunk: ECC flags (none: the sectors keep their own sync and EDC), the base's compressed length (2 bytes:
 * the hunk is under 64 KiB), deflate(8 sectors), deflate(8 subcodes). */
static size_t cdzl_hunk(uint8_t *out, const uint8_t *hunk)
{
    static uint8_t base[FPH * 2352], sub[FPH * 96];
    for (int i = 0; i < FPH; i++) {
        memcpy(base + i * 2352, hunk + i * FRAME, 2352);
        memcpy(sub + i * 96, hunk + i * FRAME + 2352, 96);
    }
    out[0] = 0;   /* ecc_bytes = (8 + 7) / 8 */
    size_t nb = deflate_fixed(out + 3, base, sizeof base);
    put16(out + 1, (uint32_t)nb);
    return 3 + nb + deflate_fixed(out + 3 + nb, sub, sizeof sub);
}

/* v5 the way chdman writes a CD: codec 0 = cdzl, a huffman-coded map (every compression type 4 bits long, so type t
 * is coded t; then per hunk 24 bits of length (none for a raw hunk) and the 16-bit CRC of the decoded hunk; the map's
 * own CRC16 over its decoded 12-byte entries). Hunks 0 and 2 are cdzl, hunk 1 is stored raw (COMPRESSION_NONE), as
 * chdman does with a hunk that does not compress. broken: a map CRC that does not match. */
static size_t chd_cdzl(int broken)
{
    memset(file, 0, sizeof file);
    size_t map = 124, meta, data;
    uint8_t mapbits[64] = {0}, raw[NHUNK * 12];
    size_t len[NHUNK];
    static uint8_t comp[NHUNK][2 * HUNK];
    uint8_t type[NHUNK];
    for (int h = 0; h < NHUNK; h++) {
        type[h] = h == 1 ? 4 : 0;   /* COMPRESSION_NONE, COMPRESSION_TYPE_0 (codec 0) */
        if (type[h] == 0) {
            len[h] = cdzl_hunk(comp[h], frames + (size_t)h * HUNK);
        } else {
            len[h] = HUNK;
            memcpy(comp[h], frames + (size_t)h * HUNK, HUNK);
        }
        if (len[h] > HUNK)
            return 0;
    }
    bitw w = { mapbits, 0 };
    for (int i = 0; i < 16; i++)
        bits(&w, 4, 4);          /* the tree: 16 code lengths of 4 bits (never 1, the escape) */
    for (int h = 0; h < NHUNK; h++)
        bits(&w, type[h], 4);
    meta = map + 16 + 64;
    data = (put_meta(file, meta) + 15) & ~(size_t)15;
    uint64_t off = data;
    for (int h = 0; h < NHUNK; h++) {
        uint16_t c = crc16(frames + (size_t)h * HUNK, HUNK);
        if (type[h] == 0)
            bits(&w, (uint32_t)len[h], 24);
        bits(&w, c, 16);
        raw[h * 12] = type[h];
        raw[h * 12 + 1] = (uint8_t)(len[h] >> 16), put16(raw + h * 12 + 2, (uint32_t)len[h]);
        put48(raw + h * 12 + 4, off);
        put16(raw + h * 12 + 10, c);
        memcpy(file + off, comp[h], len[h]);
        off += len[h];
    }
    header(file, ('c' << 24) | ('d' << 16) | ('z' << 8) | 'l', map, meta);
    put32(file + map, 64);                       /* mapbytes */
    put48(file + map + 4, data);                 /* first hunk's offset */
    put16(file + map + 10, crc16(raw, sizeof raw) ^ (broken ? 1 : 0));
    file[map + 12] = 24;                         /* lengthbits */
    file[map + 13] = 0;                          /* selfbits */
    file[map + 14] = 0;                          /* parentbits */
    memcpy(file + map + 16, mapbits, sizeof mapbits);
    return (size_t)off;
}

static int key_of(const char *name, char *key, size_t n)
{
    char p[700];
    int console = -1;
    path_in(p, sizeof p, name);
    key[0] = 0;
    if (recomp_pack_rom_identify(p, &console, key, n) != 0)
        return -1;
    return console == RECOMP_CONSOLE_PS1 ? 0 : -2;
}

static void test_keys(void)
{
    char key[64], p[700], err[256];
    make_disc("PLAYSTATION", "BOOT = cdrom:\\SLUS_005.94;1\r\nTCB = 4\r\n");
    write_file("raw.chd", file, chd_raw());
    size_t zn = chd_cdzl(0);
    CHECK(zn > 0 && zn < (NHUNK + 1) * HUNK, "cdzl.chd: the hunks did not compress (%zu bytes)", zn);
    write_file("cdzl.chd", file, zn);

    recomp_pack_set_disc_hook(NULL);
    CHECK(key_of("raw.chd", key, sizeof key) == -1, "a CHD without the disc hook has no key (%s)", key);

    ps1_identify_install();
    CHECK(key_of("raw.chd", key, sizeof key) == 0 && !strcmp(key, "SLUS-00594"), "raw.chd: %s", key);
    CHECK(key_of("cdzl.chd", key, sizeof key) == 0 && !strcmp(key, "SLUS-00594"), "cdzl.chd: %s", key);
    write_file("cdzl.img", file, zn);   /* by content, whatever the name */
    CHECK(key_of("cdzl.img", key, sizeof key) == 0 && !strcmp(key, "SLUS-00594"), "cdzl.img: %s", key);

    /* fd:N, as the app hands a document over; the descriptor stays the caller's */
    path_in(p, sizeof p, "cdzl.chd");
    int fd = open(p, O_RDONLY);
    char fdp[32];
    snprintf(fdp, sizeof fdp, "fd:%d", fd);
    int console = -1;
    CHECK(recomp_pack_rom_identify(fdp, &console, key, sizeof key) == 0 && console == RECOMP_CONSOLE_PS1
              && !strcmp(key, "SLUS-00594"), "%s: %s", fdp, key);
    CHECK(fcntl(fd, F_GETFD) != -1, "fd still open");
    close(fd);

    /* every frame of the compressed disc reads back as written (the cdzl hunks, the huffman-coded map) */
    ps1_disc *d = ps1_disc_open(p, err, sizeof err);
    CHECK(d, "open cdzl.chd: %s", err);
    if (d) {
        uint8_t got[2352];
        CHECK(ps1_disc_size(d) == NFR, "size %u", ps1_disc_size(d));
        for (uint32_t l = 0; l < NFR; l++)
            CHECK(ps1_disc_read_sector(d, l, got) == 0 && !memcmp(got, frames + l * FRAME, 2352), "cdzl lba %u", l);
        ps1_disc_close(d);
    }

    /* no serial-shaped boot file: PSX-<CRC32 of the PVD>, as for a .bin */
    make_disc("PLAYSTATION", "BOOT = cdrom:\\PSX.EXE;1\r\n");
    write_file("exe.chd", file, chd_cdzl(0));
    CHECK(key_of("exe.chd", key, sizeof key) == 0 && !strncmp(key, "PSX-", 4) && strlen(key) == 12, "exe.chd: %s", key);

    /* another CD's volume, a broken map, a truncated file, a bare magic: no key, no crash */
    make_disc("WIN32", "BOOT = cdrom:\\SLUS_005.94;1\r\n");
    write_file("pc.chd", file, chd_raw());
    CHECK(key_of("pc.chd", key, sizeof key) == -1, "pc.chd: %s", key);
    make_disc("PLAYSTATION", "BOOT = cdrom:\\SLUS_005.94;1\r\n");
    write_file("badmap.chd", file, chd_cdzl(1));
    CHECK(key_of("badmap.chd", key, sizeof key) == -1, "badmap.chd: %s", key);
    size_t n = chd_cdzl(0);
    write_file("short.chd", file, n - HUNK / 2);
    CHECK(key_of("short.chd", key, sizeof key) == -1, "short.chd: %s", key);
    write_file("magic.chd", "MComprHD", 8);
    CHECK(key_of("magic.chd", key, sizeof key) == -1, "magic.chd: %s", key);
}

/* ---- Real chdman output (XBA-192) ------------------------------------------------------------------------------
 * The CHDs above come from this test's own writer. chdman (MAME's tool, the one players use) also writes multi-track
 * discs with CD-DA, stored and unstored pregaps, a lead-in pregap on track 1, 2048-byte tracks, and the cdzs / cdfl
 * codecs. CHDMAN_DISCS lists them, made from this test's volume (never game data) and a tone of our own:
 * `ps1_identify_test --make DIR` writes their sources and runs chdman (on the PATH, or $CHDMAN) into DIR. The fixtures
 * in consoles/ps1/tests/chdman/ were made that way (its README) and are checked on every run; when chdman is installed
 * the test makes and checks a fresh set too. */

enum { TONE_FRAMES = 30, GAP = 150 };

typedef struct {
    const char *name, *cue, *args;
    int tracks;
    uint32_t size, t2_start;   /* disc size; track 2's INDEX 01 (one track: 0) */
} chdman_disc;

static const chdman_disc CHDMAN_DISCS[] = {
    { "single", "single.cue", "", 1, NFR, 0 },   /* the default codecs: cdlz, cdzl, cdfl */
    { "single_cdzs", "single.cue", "-c cdzs", 1, NFR, 0 },
    { "single_cdlz", "single.cue", "-c cdlz", 1, NFR, 0 },
    { "single_cdzl", "single.cue", "-c cdzl", 1, NFR, 0 },
    { "iso2048", "iso.cue", "", 1, NFR, 0 },   /* MODE1/2048 */
    /* data + CD-DA, redump's shape: the audio track's INDEX 00 / 01, its pregap stored */
    { "audio", "audio.cue", "", 2, NFR + GAP + TONE_FRAMES, NFR + GAP },
    { "audio_cdzs", "audio.cue", "-c cdzs,cdfl", 2, NFR + GAP + TONE_FRAMES, NFR + GAP },
    { "pregap", "pregap.cue", "", 2, NFR + GAP + TONE_FRAMES, NFR + GAP },       /* PREGAP: not stored */
    { "t1pregap", "t1pregap.cue", "", 2, NFR + GAP + TONE_FRAMES, NFR + GAP },   /* track 1 PREGAP 00:02:00 */
    { "t1index0", "t1index0.cue", "", 2, NFR + GAP + TONE_FRAMES, NFR + GAP },   /* track 1 INDEX 00, stored */
};
#define N_CHDMAN (sizeof CHDMAN_DISCS / sizeof *CHDMAN_DISCS)

/* Tone frame i: a CD-DA sector of our own, little-endian as in a .bin. */
static void tone_sector(uint8_t *s, uint32_t i)
{
    make_audio_sector(s, 1000 + i);
}

static FILE *open_in(const char *dir, const char *name, const char *mode)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, mode);
    if (!f) {
        printf("cannot write %s\n", p);
        exit(1);
    }
    return f;
}

static void write_text(const char *dir, const char *name, const char *text)
{
    FILE *f = open_in(dir, name, "w");
    fputs(text, f);
    fclose(f);
}

/* data.bin (the volume, raw), data.iso (its 2048-byte user data), lead.bin (150 empty frames, then the volume),
 * tone.bin (150 frames of silence, then the tone), tone_np.bin (the tone alone), and the cue sheets. */
static void write_sources(const char *dir)
{
    uint8_t sec[2352];
    make_disc("PLAYSTATION", "BOOT = cdrom:\\SLUS_005.94;1\r\nTCB = 4\r\n");
    FILE *bin = open_in(dir, "data.bin", "wb"), *iso = open_in(dir, "data.iso", "wb");
    FILE *lead = open_in(dir, "lead.bin", "wb"), *tone = open_in(dir, "tone.bin", "wb");
    FILE *tnp = open_in(dir, "tone_np.bin", "wb");
    memset(sec, 0, sizeof sec);
    for (int i = 0; i < GAP; i++)
        fwrite(sec, 1, 2352, lead), fwrite(sec, 1, 2352, tone);
    for (uint32_t l = 0; l < NFR; l++) {
        fwrite(frames + l * FRAME, 1, 2352, bin);
        fwrite(frames + l * FRAME, 1, 2352, lead);
        fwrite(frames + l * FRAME + 24, 1, 2048, iso);
    }
    for (uint32_t i = 0; i < TONE_FRAMES; i++) {
        tone_sector(sec, i);
        fwrite(sec, 1, 2352, tone);
        fwrite(sec, 1, 2352, tnp);
    }
    fclose(bin), fclose(iso), fclose(lead), fclose(tone), fclose(tnp);
    write_text(dir, "single.cue", "FILE \"data.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n");
    write_text(dir, "iso.cue", "FILE \"data.iso\" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n");
    write_text(dir, "audio.cue",
               "FILE \"data.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n"
               "FILE \"tone.bin\" BINARY\n  TRACK 02 AUDIO\n    INDEX 00 00:00:00\n    INDEX 01 00:02:00\n");
    write_text(dir, "pregap.cue",
               "FILE \"data.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n"
               "FILE \"tone_np.bin\" BINARY\n  TRACK 02 AUDIO\n    PREGAP 00:02:00\n    INDEX 01 00:00:00\n");
    write_text(dir, "t1pregap.cue",
               "FILE \"data.bin\" BINARY\n  TRACK 01 MODE2/2352\n    PREGAP 00:02:00\n    INDEX 01 00:00:00\n"
               "FILE \"tone.bin\" BINARY\n  TRACK 02 AUDIO\n    INDEX 00 00:00:00\n    INDEX 01 00:02:00\n");
    write_text(dir, "t1index0.cue",
               "FILE \"lead.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 00 00:00:00\n    INDEX 01 00:02:00\n"
               "FILE \"tone.bin\" BINARY\n  TRACK 02 AUDIO\n    INDEX 00 00:00:00\n    INDEX 01 00:02:00\n");
}

/* Writes the sources into dir and runs chdman on each of CHDMAN_DISCS, then `chdman verify` on what it wrote: a CHD
 * chdman itself cannot read back (chdman 0.276 now and then leaves a stray last byte in a tiny cdlz CHD's map) is
 * removed. Returns how many were removed, or -1 when chdman did not run. */
static int make_chdman(const char *dir)
{
    const char *chdman = getenv("CHDMAN");
    char cmd[4096], p[1024];
    int bad = 0;
    if (!chdman || !*chdman)
        chdman = "chdman";
    write_sources(dir);
    for (size_t i = 0; i < N_CHDMAN; i++) {
        const chdman_disc *c = &CHDMAN_DISCS[i];
        snprintf(p, sizeof p, "%s/%s.chd", dir, c->name);
        snprintf(cmd, sizeof cmd, "%s createcd -f %s -i '%s/%s' -o '%s' >'%s/chdman.log' 2>&1", chdman, c->args, dir,
                 c->cue, p, dir);
        if (system(cmd) != 0) {
            printf("note: %s failed (see %s/chdman.log)\n", cmd, dir);
            return -1;
        }
        snprintf(cmd, sizeof cmd, "%s verify -i '%s' >'%s/chdman.log' 2>&1", chdman, p, dir);
        if (system(cmd) != 0) {
            printf("note: chdman cannot read back its own %s.chd: left out\n", c->name);
            remove(p);
            bad++;
        }
    }
    return bad;
}

/* Every disc of CHDMAN_DISCS in dir: the title key (path and fd:N, as the app asks), the track layout, the volume's
 * sectors, the tone's first sector and the silence before it. fresh: chdman's output of this run, where a disc
 * make_chdman left out is skipped. */
static void check_chdman(const char *dir, int fresh)
{
    char p[1024], key[64], fdp[32], err[256];
    uint8_t got[2352], want[2352];
    make_disc("PLAYSTATION", "BOOT = cdrom:\\SLUS_005.94;1\r\nTCB = 4\r\n");
    ps1_identify_install();
    for (size_t i = 0; i < N_CHDMAN; i++) {
        const chdman_disc *c = &CHDMAN_DISCS[i];
        int console = -1;
        snprintf(p, sizeof p, "%s/%s.chd", dir, c->name);
        if (fresh && access(p, F_OK) != 0)
            continue;
        key[0] = 0;
        CHECK(recomp_pack_rom_identify(p, &console, key, sizeof key) == 0 && console == RECOMP_CONSOLE_PS1
                  && !strcmp(key, "SLUS-00594"), "%s: key '%s'", p, key);
        int fd = open(p, O_RDONLY);
        CHECK(fd >= 0, "cannot open %s", p);
        snprintf(fdp, sizeof fdp, "fd:%d", fd);
        console = -1, key[0] = 0;
        CHECK(recomp_pack_rom_identify(fdp, &console, key, sizeof key) == 0 && console == RECOMP_CONSOLE_PS1
                  && !strcmp(key, "SLUS-00594"), "%s as %s: key '%s'", c->name, fdp, key);
        ps1_disc *d = ps1_disc_open(fdp, err, sizeof err);
        CHECK(d, "%s as %s: %s", c->name, fdp, err);
        ps1_disc_close(d);
        CHECK(fd < 0 || fcntl(fd, F_GETFD) != -1, "%s: the caller's descriptor stays open", c->name);
        if (fd >= 0)
            close(fd);
        d = ps1_disc_open(p, err, sizeof err);
        CHECK(d, "%s: %s", p, err);
        if (!d)
            continue;
        uint32_t start = 99;
        int audio = -1;
        CHECK(ps1_disc_track_count(d) == c->tracks, "%s: %d tracks", c->name, ps1_disc_track_count(d));
        CHECK(ps1_disc_size(d) == c->size, "%s: size %u, want %u", c->name, ps1_disc_size(d), c->size);
        CHECK(ps1_disc_track(d, 1, &start, &audio) == 0 && start == 0 && !audio, "%s: track 1 at %u", c->name, start);
        for (uint32_t l = 16; l < 20; l++)
            CHECK(ps1_disc_read_sector(d, l, got) == 0 && !memcmp(got + 24, frames + l * FRAME + 24, 2048),
                  "%s: lba %u", c->name, l);
        if (c->tracks > 1) {
            CHECK(ps1_disc_track(d, 2, &start, &audio) == 0 && start == c->t2_start && audio, "%s: track 2 at %u",
                  c->name, start);
            tone_sector(want, 0);
            CHECK(ps1_disc_read_sector(d, c->t2_start, got) == 0 && !memcmp(got, want, 2352), "%s: tone at %u",
                  c->name, c->t2_start);
            memset(want, 0, sizeof want);
            CHECK(ps1_disc_read_sector(d, c->t2_start - 1, got) == 0 && !memcmp(got, want, 2352), "%s: pregap",
                  c->name);
        }
        ps1_disc_close(d);
    }
}

/* The sources' cue sheets through ps1_disc's own sheet reader: the layout of their CHDs. */
static void check_sources(const char *dir)
{
    char p[1024], err[256];
    uint8_t got[2352];
    for (size_t i = 0; i < N_CHDMAN; i++) {
        const chdman_disc *c = &CHDMAN_DISCS[i];
        uint32_t start = 99;
        snprintf(p, sizeof p, "%s/%s", dir, c->cue);
        ps1_disc *d = ps1_disc_open(p, err, sizeof err);
        CHECK(d, "%s: %s", p, err);
        if (!d)
            continue;
        CHECK(ps1_disc_size(d) == c->size, "%s: size %u", c->cue, ps1_disc_size(d));
        CHECK(ps1_disc_track(d, 1, &start, NULL) == 0 && start == 0, "%s: track 1 at %u", c->cue, start);
        CHECK(ps1_disc_read_sector(d, 16, got) == 0 && !memcmp(got + 24, frames + 16 * FRAME + 24, 2048),
              "%s: the PVD", c->cue);
        if (c->tracks > 1)
            CHECK(ps1_disc_track(d, 2, &start, NULL) == 0 && start == c->t2_start, "%s: track 2 at %u", c->cue,
                  start);
        ps1_disc_close(d);
    }
}

static void test_chdman(const char *fixtures)
{
    char dir[700];
    if (fixtures)
        check_chdman(fixtures, 0);
    else
        printf("note: no chdman fixture directory given\n");
    path_in(dir, sizeof dir, "chdman");
    mkdir(dir, 0755);
    write_sources(dir);
    check_sources(dir);
    if (system("command -v \"${CHDMAN:-chdman}\" >/dev/null 2>&1") != 0) {
        printf("note: chdman not installed, only the committed chdman CHDs were checked\n");
        return;
    }
    if (make_chdman(dir) >= 0)
        check_chdman(dir, 1);
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--make")) {   /* the fixtures: ps1_identify_test --make DIR */
        mkdir(argv[2], 0755);
        return make_chdman(argv[2]) ? 1 : 0;
    }
    scratch();
#ifdef XBR_PS1_CHD
    test_keys();
    test_chdman(argc > 1 ? argv[1] : NULL);
#else
    printf("note: built without XBR_PS1_CHD: no CHD title keys\n");
    scratch_cleanup();
    return 77;
#endif
    scratch_cleanup();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ps1_identify: all passed\n");
    return 0;
}
