/* recomp_states (XBA-182): slot files, thumbnails, the runtime's attach / save / load path, fast-forward. */
#define _POSIX_C_SOURCE 200809L
#include "recomp_states.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                               \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static char s_dir[256];

/* A fake runtime: its "machine" is a counter, the blob its 8 bytes. */
static uint64_t s_machine;
static int fake_save(int slot, void *user)
{
    char path[400];
    CHECK(user == &s_machine);
    if (recomp_states_slot_path(s_dir, slot, path, sizeof path) != 0)
        return RECOMP_STATE_ESLOT;
    recomp_state_meta m;
    memset(&m, 0, sizeof m);
    strcpy(m.console, "gba");
    strcpy(m.key, "TEST-0000");
    m.frames = s_machine;
    uint16_t thumb[4] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
    m.thumb_w = 2;
    m.thumb_h = 2;
    return recomp_states_write(path, &m, thumb, (const uint8_t *)&s_machine, sizeof s_machine);
}
static int fake_load(int slot, void *user)
{
    char path[400];
    uint8_t *blob = NULL;
    size_t n = 0;
    recomp_state_meta m;
    (void)user;
    if (recomp_states_slot_path(s_dir, slot, path, sizeof path) != 0)
        return RECOMP_STATE_ESLOT;
    int rc = recomp_states_read(path, &m, NULL, &blob, &n);
    if (rc == RECOMP_STATE_OK && (n != sizeof s_machine || strcmp(m.key, "TEST-0000") != 0))
        rc = RECOMP_STATE_EGAME;
    if (rc == RECOMP_STATE_OK)
        memcpy(&s_machine, blob, n);
    free(blob);
    return rc;
}

static void test_files(void)
{
    char path[400];
    CHECK(recomp_states_slot_path(s_dir, 1, path, sizeof path) == 0);
    CHECK(strstr(path, "/state1.st") != NULL);
    CHECK(recomp_states_slot_path(s_dir, RECOMP_STATE_AUTO, path, sizeof path) == 0);
    CHECK(strstr(path, "/auto.st") != NULL);
    CHECK(recomp_states_slot_path(s_dir, -1, path, sizeof path) == -1);
    CHECK(recomp_states_slot_path(s_dir, RECOMP_STATE_SLOTS + 1, path, sizeof path) == -1);
    CHECK(recomp_states_slot_path(NULL, 1, path, sizeof path) == -1);
    CHECK(recomp_states_slot_path(s_dir, 1, path, 8) == -1);

    /* Written (in a folder made on the way) and read back whole. */
    snprintf(path, sizeof path, "%s/sub/dir/state3.st", s_dir);
    recomp_state_meta m, r;
    memset(&m, 0, sizeof m);
    strcpy(m.console, "ps1");
    strcpy(m.key, "SLUS-00594");
    m.saved_ms = 1700000000123ll;
    m.frames = 4242;
    m.thumb_w = 3;
    m.thumb_h = 1;
    uint16_t thumb[3] = {1, 2, 3}, back[RECOMP_STATE_THUMB_W * RECOMP_STATE_THUMB_H];
    uint8_t blob[100];
    for (int i = 0; i < 100; i++)
        blob[i] = (uint8_t)(i * 7);
    CHECK(recomp_states_write(path, &m, thumb, blob, sizeof blob) == RECOMP_STATE_OK);
    uint8_t *got = NULL;
    size_t n = 0;
    CHECK(recomp_states_read(path, &r, back, &got, &n) == RECOMP_STATE_OK);
    CHECK(!strcmp(r.console, "ps1") && !strcmp(r.key, "SLUS-00594") && r.saved_ms == 1700000000123ll);
    CHECK(r.frames == 4242 && r.thumb_w == 3 && r.thumb_h == 1 && back[0] == 1 && back[2] == 3);
    CHECK(n == sizeof blob && got && !memcmp(got, blob, n));
    free(got);
    /* Header only. */
    CHECK(recomp_states_read(path, &r, NULL, NULL, NULL) == RECOMP_STATE_OK);
    /* saved_ms 0: now. */
    m.saved_ms = 0;
    CHECK(recomp_states_write(path, &m, NULL, blob, sizeof blob) == RECOMP_STATE_OK);
    CHECK(recomp_states_read(path, &r, NULL, NULL, NULL) == RECOMP_STATE_OK && r.saved_ms > 1600000000000ll);
    CHECK(r.thumb_w == 0 && r.thumb_h == 0);

    /* Missing, truncated, not a state, another slot version. */
    snprintf(path, sizeof path, "%s/none.st", s_dir);
    CHECK(recomp_states_read(path, &r, NULL, NULL, NULL) == RECOMP_STATE_EEMPTY);
    snprintf(path, sizeof path, "%s/sub/dir/state3.st", s_dir);
    FILE *f = fopen(path, "r+b");
    CHECK(f != NULL);
    if (f) {
        CHECK(ftruncate(fileno(f), 120) == 0);
        fclose(f);
    }
    CHECK(recomp_states_read(path, &r, NULL, &got, &n) == RECOMP_STATE_ECORRUPT);
    f = fopen(path, "wb");
    if (f) {
        fputs("not a state at all, but long enough to hold a header of ninety-six bytes...................", f);
        fclose(f);
    }
    CHECK(recomp_states_read(path, &r, NULL, NULL, NULL) == RECOMP_STATE_ECORRUPT);
    CHECK(recomp_states_write(path, &m, NULL, blob, sizeof blob) == RECOMP_STATE_OK);
    f = fopen(path, "r+b");
    if (f) {
        fseek(f, 8, SEEK_SET);
        fputc(99, f); /* slot version 99 */
        fclose(f);
    }
    CHECK(recomp_states_read(path, &r, NULL, NULL, NULL) == RECOMP_STATE_EVERSION);
    CHECK(recomp_states_write(path, &m, NULL, NULL, 0) == RECOMP_STATE_EIO);
    CHECK(strcmp(recomp_states_error(RECOMP_STATE_EGAME), "?") != 0);
}

static void test_thumb(void)
{
    /* 240 x 160 (GBA, 3:2) -> 160 x 106; 320 x 240 shown 4:3 -> 160 x 120; 640 x 240 shown 4:3 -> 160 x 120. */
    static uint8_t rgba[640 * 480 * 4];
    static uint16_t out[RECOMP_STATE_THUMB_W * RECOMP_STATE_THUMB_H];
    int w, h;
    for (int i = 0; i < 640 * 480; i++) {
        rgba[4 * i] = 255;   /* red */
        rgba[4 * i + 3] = 255;
    }
    recomp_states_thumb(rgba, 240, 160, 240 * 4, 3, 2, out, &w, &h);
    CHECK(w == 160 && h == 106);
    CHECK(out[0] == 0xF800 && out[w * h - 1] == 0xF800);
    recomp_states_thumb(rgba, 320, 240, 320 * 4, 4, 3, out, &w, &h);
    CHECK(w == 160 && h == 120);
    recomp_states_thumb(rgba, 640, 240, 640 * 4, 4, 3, out, &w, &h);
    CHECK(w == 160 && h == 120);
    recomp_states_thumb(rgba, 100, 400, 100 * 4, 0, 0, out, &w, &h);   /* the picture's own aspect: 1:4 */
    CHECK(h == 120 && w == 30);
    /* Box filter: a white / black checker averages to grey. */
    for (int i = 0; i < 320 * 240; i++) {
        uint8_t v = ((i % 320) + (i / 320)) & 1 ? 255 : 0;
        rgba[4 * i] = rgba[4 * i + 1] = rgba[4 * i + 2] = v;
    }
    recomp_states_thumb(rgba, 320, 240, 320 * 4, 4, 3, out, &w, &h);
    unsigned r = out[0] >> 11;
    CHECK(r >= 14 && r <= 17);
    recomp_states_thumb(NULL, 320, 240, 320 * 4, 4, 3, out, &w, &h);
    CHECK(w == 0 && h == 0);
}

static void test_attach(void)
{
    recomp_state_meta m;
    static uint16_t thumb[RECOMP_STATE_THUMB_W * RECOMP_STATE_THUMB_H];
    recomp_states_detach();
    CHECK(!recomp_states_available());
    CHECK(!strcmp(recomp_states_console(), ""));
    CHECK(recomp_states_save(1) == RECOMP_STATE_ENOGAME);
    CHECK(recomp_states_load(1) == RECOMP_STATE_ENOGAME);
    CHECK(recomp_states_info(1, &m, NULL) == RECOMP_STATE_ENOGAME);

    recomp_state_ops ops = {"gba", fake_save, fake_load, &s_machine};
    recomp_states_attach(&ops, "");   /* no save dir: states off */
    CHECK(!recomp_states_available());
    CHECK(recomp_states_save(1) == RECOMP_STATE_ENOGAME);
    recomp_states_attach(&ops, s_dir);
    CHECK(recomp_states_available());
    CHECK(!strcmp(recomp_states_console(), "gba"));
    CHECK(recomp_states_save(-1) == RECOMP_STATE_ESLOT);
    CHECK(recomp_states_load(RECOMP_STATE_SLOTS + 1) == RECOMP_STATE_ESLOT);
    CHECK(recomp_states_info(2, &m, NULL) == RECOMP_STATE_EEMPTY);
    CHECK(recomp_states_load(2) == RECOMP_STATE_EEMPTY);

    s_machine = 1234;
    CHECK(recomp_states_save(2) == RECOMP_STATE_OK);
    s_machine = 99;
    CHECK(recomp_states_info(2, &m, thumb) == RECOMP_STATE_OK);
    CHECK(m.frames == 1234 && m.thumb_w == 2 && m.thumb_h == 2 && thumb[0] == 0xF800 && thumb[3] == 0xFFFF);
    CHECK(recomp_states_load(2) == RECOMP_STATE_OK && s_machine == 1234);
    recomp_states_detach();
    CHECK(!recomp_states_available());
    CHECK(recomp_states_load(2) == RECOMP_STATE_ENOGAME);
}

/* The automatic slot, the quick slot, the pad chords and the events. */
static void test_quick(void)
{
    recomp_state_ops ops = {"gba", fake_save, fake_load, &s_machine};
    recomp_state_event ev, before;
    char path[400];
    for (int i = RECOMP_STATE_AUTO; i <= RECOMP_STATE_SLOTS; i++)
        if (recomp_states_slot_path(s_dir, i, path, sizeof path) == 0)
            unlink(path);
    recomp_states_attach(&ops, s_dir);

    /* Nothing saved: the quick slot is 1, a quick load says empty, resume records nothing. */
    CHECK(recomp_states_quick_slot() == 1);
    recomp_states_last_event(&before);
    CHECK(recomp_states_resume() == RECOMP_STATE_EEMPTY);
    recomp_states_last_event(&ev);
    CHECK(ev.seq == before.seq);
    CHECK(recomp_states_quick_load() == RECOMP_STATE_EEMPTY);
    recomp_states_last_event(&ev);
    CHECK(ev.seq == before.seq + 1 && ev.kind == RECOMP_STATE_EV_LOADED && ev.slot == 1 &&
          ev.rc == RECOMP_STATE_EEMPTY);

    /* A numbered slot saved from the menu becomes the quick slot; the automatic slot never does. */
    s_machine = 7;
    CHECK(recomp_states_save(3) == RECOMP_STATE_OK);
    CHECK(recomp_states_quick_slot() == 3);
    s_machine = 8;
    CHECK(recomp_states_save(RECOMP_STATE_AUTO) == RECOMP_STATE_OK);
    CHECK(recomp_states_quick_slot() == 3);
    CHECK(recomp_states_info(RECOMP_STATE_AUTO, NULL, NULL) == RECOMP_STATE_OK);
    s_machine = 9;
    CHECK(recomp_states_quick_save() == RECOMP_STATE_OK);   /* slot 3 now holds 9 */
    s_machine = 0;
    CHECK(recomp_states_quick_load() == RECOMP_STATE_OK && s_machine == 9);
    recomp_states_last_event(&ev);
    CHECK(ev.kind == RECOMP_STATE_EV_LOADED && ev.slot == 3 && ev.rc == RECOMP_STATE_OK);

    /* Resume loads the automatic slot. */
    CHECK(recomp_states_resume() == RECOMP_STATE_OK && s_machine == 8);
    recomp_states_last_event(&ev);
    CHECK(ev.kind == RECOMP_STATE_EV_RESUMED && ev.slot == RECOMP_STATE_AUTO && ev.rc == RECOMP_STATE_OK);

    /* A new run: the quick slot is the most recently saved numbered slot. */
    recomp_states_attach(&ops, s_dir);
    CHECK(recomp_states_quick_slot() == 3);

    /* The chords: once per press, Back first, save wins over load, swallow while held. */
    int held = 0, sw = 0;
    CHECK(recomp_states_chord(0, 0, 1, &held, &sw) == RECOMP_CHORD_NONE && !sw);
    CHECK(recomp_states_chord(1, 0, 0, &held, &sw) == RECOMP_CHORD_NONE && !sw);
    CHECK(recomp_states_chord(1, 0, 1, &held, &sw) == RECOMP_CHORD_SAVE && sw);
    CHECK(recomp_states_chord(1, 0, 1, &held, &sw) == RECOMP_CHORD_NONE && sw);
    CHECK(recomp_states_chord(1, 1, 1, &held, &sw) == RECOMP_CHORD_LOAD && sw);
    CHECK(recomp_states_chord(1, 0, 0, &held, &sw) == RECOMP_CHORD_NONE && !sw);
    CHECK(recomp_states_chord(1, 1, 1, &held, &sw) == RECOMP_CHORD_SAVE && sw);
    CHECK(recomp_states_chord(0, 1, 1, &held, &sw) == RECOMP_CHORD_NONE && !sw);
    CHECK(recomp_states_chord_run(RECOMP_CHORD_NONE) == RECOMP_STATE_OK);

    /* RECOMP_STATE_RESUME. */
    unsetenv("RECOMP_STATE_RESUME");
    CHECK(!recomp_states_resume_enabled());
    setenv("RECOMP_STATE_RESUME", "1", 1);
    CHECK(recomp_states_resume_enabled());
    setenv("RECOMP_STATE_RESUME", "0", 1);
    CHECK(!recomp_states_resume_enabled());
    unsetenv("RECOMP_STATE_RESUME");
    recomp_states_detach();
    CHECK(recomp_states_quick_save() == RECOMP_STATE_ENOGAME);
}

static void test_ff(void)
{
    recomp_ff_reset();
    unsetenv("RECOMP_FF_SPEED");
    CHECK(recomp_ff_speed() == 4);
    const char *vals[][2] = {{"2", "2"}, {"x8", "8"}, {"max", "0"}, {"0", "0"}, {"1", "4"}, {"99", "4"}, {"abc", "4"}};
    for (size_t i = 0; i < sizeof vals / sizeof vals[0]; i++) {
        recomp_ff_reset();
        setenv("RECOMP_FF_SPEED", vals[i][0], 1);
        CHECK(recomp_ff_speed() == atoi(vals[i][1]));
    }
    unsetenv("RECOMP_FF_SPEED");
    recomp_ff_reset();
    CHECK(!recomp_ff_active() && !recomp_ff_toggled());
    recomp_ff_set(1);
    CHECK(recomp_ff_active() && recomp_ff_toggled());
    recomp_ff_set(0);
    recomp_ff_hold(1);
    CHECK(recomp_ff_active() && !recomp_ff_toggled());
    recomp_ff_hold(0);
    CHECK(!recomp_ff_active());

    /* Audio averaged down by 4, the remainder carried to the next call. */
    int16_t pcm[2 * 10];
    for (int i = 0; i < 10; i++) {
        pcm[2 * i] = (int16_t)(100 * i);
        pcm[2 * i + 1] = (int16_t)(-100 * i);
    }
    recomp_ff_carry c = {{0, 0}, 0};
    size_t n = recomp_ff_audio(pcm, 10, 4, &c);
    CHECK(n == 2 && pcm[0] == 150 && pcm[1] == -150 && pcm[2] == 550 && pcm[3] == -550);
    CHECK(c.n == 2);
    int16_t more[4] = {1000, -1000, 1000, -1000};
    n = recomp_ff_audio(more, 2, 4, &c);
    CHECK(n == 1 && more[0] == (800 + 900 + 2000) / 4);
    int16_t same[4] = {5, 6, 7, 8};
    CHECK(recomp_ff_audio(same, 2, 1, &c) == 2 && same[3] == 8);
}

int main(void)
{
    snprintf(s_dir, sizeof s_dir, "/tmp/recomp_states_test.XXXXXX");
    if (!mkdtemp(s_dir)) {
        perror("mkdtemp");
        return 1;
    }
    test_files();
    test_thumb();
    test_attach();
    test_quick();
    test_ff();
    char cmd[400];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", s_dir);
    if (system(cmd) != 0)
        fprintf(stderr, "note: could not remove %s\n", s_dir);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("recomp_states: ok\n");
    return 0;
}
