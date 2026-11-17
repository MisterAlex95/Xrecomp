/* gc_cardtest -- our libogc memory card test (consoles/gc/tests/homebrew/cardtest, XBA-264) on the core, headless,
 * read back from what the program itself saw: its `report` (magic "XBACARDT"), found in MEM1.
 *
 *   gc_cardtest CARDTEST.dol
 *
 *   1. a blank 251-block card in slot A, the RTC fixed, SRAM in Spanish: libogc finds the card (16 Mbit, 8 KB
 *      sectors), its mount says broken, the format and the second mount work, the file is created, written and read
 *      back the same; the RTC, the time base libogc set from it, the language, NTSC, stereo are what went in
 *   2. a new machine with the card the first run left: the mount works at once, the file is there with run 1's
 *      pattern, run 2 is written
 *   3. blank 59- and 1019-block cards (4 / 64 Mbit: 58 / 1018 blocks free after the file), the second in PAL
 *   4. no card: the probe says no card
 * Exit 0, 1 on a mismatch, 77 (skipped) when the DOL is not built (tools/consoles/build_homebrew.sh gc). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gc_core.h"
#include "gc_exi.h"

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

/* The program's struct report (cardtest/main.c): 32-bit fields after the magic, big-endian in MEM1. */
enum {
    R_DONE, R_PROBE, R_MEM, R_SECT, R_MOUNT1, R_FORMAT, R_MOUNT2, R_OPEN, R_CREATE, R_WRITE, R_READ, R_PREV_RUNS,
    R_PREV_OK, R_RUNS, R_VERIFY, R_RTC, R_TB_SECS, R_LANG, R_VIDEO, R_SOUND, R_FREE, R_N
};

static uint8_t dol[4 << 20];
static size_t dol_n;
#define RTC_START 849000000u /* 2026-11-26, seconds since 2000 */

static uint32_t find_report(gc_machine *m)
{
    for (uint32_t a = 0x80003100u; a < 0x81800000u - 128; a += 4)
        if (gc_peek32(m, a) == 0x58424143u && gc_peek32(m, a + 4) == 0x41524454u) /* "XBACARDT" */
            return a;
    return 0;
}

/* Runs the program until its report says done (or 900 fields); r[] gets the report. Returns the machine. */
static gc_machine *run(int slot_mbit, const uint8_t *image, int pal, int32_t *r, const char *what)
{
    char why[160];
    gc_machine *m = gc_create(dol, dol_n, why, sizeof why);
    if (!m) {
        printf("FAIL %s: %s\n", what, why);
        exit(1);
    }
    gc_exi_set_rtc(m, RTC_START);
    gc_exi_set_language(m, 3);
    if (pal)
        gc_set_tv_mode(m, GC_TV_PAL);
    if (slot_mbit)
        gc_card_insert(m, GC_CARD_SLOT_A, slot_mbit, image);
    uint32_t at = find_report(m);
    CHECK(at, "%s: no report in memory", what);
    memset(r, 0, sizeof(int32_t) * R_N);
    int f;
    for (f = 0; f < 900; f++) {
        if (gc_run_frame(m)) {
            printf("FAIL %s: the machine stopped: %s\n", what, gc_error(m));
            gc_dump_state(m, stdout);
            exit(1);
        }
        if (at && gc_peek32(m, at + 8))
            break;
        if (at && !slot_mbit && f > 60 && (int32_t)gc_peek32(m, at + 12) != 0)
            break; /* no card: the probe answered */
    }
    for (int i = 0; at && i < R_N; i++)
        r[i] = (int32_t)gc_peek32(m, at + 8 + 4 * i);
    printf("%s: %d fields, probe %d, %d Mbit, mount %d / format %d / mount %d, open %d, create %d, run %d (found %d,"
           " %s), verify %d, RTC %u, TB %u s, lang %d video %d sound %d, %d free\n",
           what, f, r[R_PROBE], r[R_MEM], r[R_MOUNT1], r[R_FORMAT], r[R_MOUNT2], r[R_OPEN], r[R_CREATE], r[R_RUNS],
           r[R_PREV_RUNS], r[R_PREV_OK] ? "intact" : "-", r[R_VERIFY], (unsigned)r[R_RTC], (unsigned)r[R_TB_SECS],
           r[R_LANG], r[R_VIDEO], r[R_SOUND], r[R_FREE]);
    return m;
}

static void expect_written(const int32_t *r, int mbit, const char *what)
{
    CHECK(r[R_DONE] == 1 && r[R_VERIFY] == 1, "%s: done %d verify %d", what, r[R_DONE], r[R_VERIFY]);
    CHECK(r[R_PROBE] == 0 && r[R_MEM] == mbit && r[R_SECT] == 8192, "%s: probe %d, %d Mbit, sector %d", what,
          r[R_PROBE], r[R_MEM], r[R_SECT]);
    CHECK(r[R_WRITE] == 0 && r[R_READ] == 0, "%s: write %d read %d", what, r[R_WRITE], r[R_READ]);
    CHECK((uint32_t)r[R_RTC] >= RTC_START && (uint32_t)r[R_RTC] < RTC_START + 30, "%s: RTC %u, want %u + a few s",
          what, (unsigned)r[R_RTC], RTC_START);
    CHECK((uint32_t)r[R_TB_SECS] >= RTC_START && (uint32_t)r[R_TB_SECS] < RTC_START + 30, "%s: time base %u s",
          what, (unsigned)r[R_TB_SECS]);
    CHECK(r[R_LANG] == 3 && r[R_SOUND] == 1, "%s: language %d sound %d, want 3 (Spanish) 1 (stereo)", what,
          r[R_LANG], r[R_SOUND]);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: gc_cardtest CARDTEST.dol\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        printf("skipped: %s not built (tools/consoles/build_homebrew.sh gc)\n", argv[1]);
        return 77;
    }
    dol_n = fread(dol, 1, sizeof dol, f);
    fclose(f);

    int32_t r[R_N];
    /* 1. blank card: format, create, write */
    gc_machine *m = run(16, NULL, 0, r, "blank 251");
    expect_written(r, 16, "blank 251");
    CHECK(r[R_MOUNT1] == -6 && r[R_FORMAT] == 0 && r[R_MOUNT2] == 0, "blank: mount %d format %d mount %d, want -6 0 0",
          r[R_MOUNT1], r[R_FORMAT], r[R_MOUNT2]);
    CHECK(r[R_OPEN] == -4 && r[R_CREATE] == 0 && r[R_RUNS] == 1, "blank: open %d create %d run %d", r[R_OPEN],
          r[R_CREATE], r[R_RUNS]);
    CHECK(r[R_VIDEO] == 0 && r[R_FREE] == 250, "blank: video %d free %d, want 0 250", r[R_VIDEO], r[R_FREE]);
    size_t n = 0;
    const uint8_t *img = gc_card_data(m, GC_CARD_SLOT_A, &n);
    CHECK(img && n == 2u << 20 && gc_card_writes(m, GC_CARD_SLOT_A) > 0, "card after run 1: %zu bytes, %llu writes",
          n, (unsigned long long)gc_card_writes(m, GC_CARD_SLOT_A));
    uint8_t *saved = malloc(2u << 20);
    memcpy(saved, img, 2u << 20);
    gc_destroy(m);

    /* 2. the same card in a new machine: the file is there */
    m = run(16, saved, 0, r, "relaunch");
    expect_written(r, 16, "relaunch");
    CHECK(r[R_MOUNT1] == 0 && r[R_OPEN] == 0 && r[R_PREV_RUNS] == 1 && r[R_PREV_OK] == 1 && r[R_RUNS] == 2,
          "relaunch: mount %d open %d found run %d intact %d, now run %d", r[R_MOUNT1], r[R_OPEN], r[R_PREV_RUNS],
          r[R_PREV_OK], r[R_RUNS]);
    gc_destroy(m);
    free(saved);

    /* 3. the other sizes */
    m = run(4, NULL, 0, r, "blank 59");
    expect_written(r, 4, "blank 59");
    CHECK(r[R_FREE] == 58, "59 blocks: %d free, want 58", r[R_FREE]);
    gc_destroy(m);
    m = run(64, NULL, 1, r, "blank 1019 PAL");
    expect_written(r, 64, "blank 1019 PAL");
    CHECK(r[R_FREE] == 1018 && r[R_VIDEO] == 1, "1019 blocks: %d free, video %d, want 1018 1 (PAL)", r[R_FREE],
          r[R_VIDEO]);
    gc_destroy(m);

    /* 4. no card */
    m = run(0, NULL, 0, r, "no card");
    CHECK(r[R_PROBE] == -3 && !r[R_DONE], "no card: probe %d, want -3", r[R_PROBE]);
    gc_destroy(m);

    if (failures) {
        printf("gc_cardtest: %d failure(s)\n", failures);
        return 1;
    }
    printf("gc_cardtest: all passed\n");
    return 0;
}
