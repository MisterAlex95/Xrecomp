/* recomp_states (XBA-182): slot files, thumbnails, the runtime's attach / save / load path, fast-forward; and the
 * hotkeys over them (XBA-190, recomp_hotkeys.h): the text forms, the pad chords and keys per frame, what they run. */
#define _POSIX_C_SOURCE 200809L
#include "recomp_states.h"
#include "recomp_hotkeys.h"
#include "console_pad.h"

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

/* ---- Hotkeys (XBA-190, recomp_hotkeys.h) ----------------------------------------------------------------------- */

#define SRC(name) (1u << recomp_hotkeys_source(name))

/* The names agree with console_pad.h's (the runtimes hand recomp_hotkeys_pad_update console_pad_sources' bits). */
static void test_hotkey_names(void)
{
    for (int i = 0; i < CPS_SOURCES; i++)
        CHECK(console_pad_source_name(i) && recomp_hotkeys_source_name(i) &&
              !strcmp(console_pad_source_name(i), recomp_hotkeys_source_name(i)));
    CHECK(RECOMP_HK_SOURCES == CPS_SOURCES);
    CHECK(recomp_hotkeys_source("rt") == CPS_RIGHT_TRIGGER && recomp_hotkeys_source("SELECT") == CPS_BACK &&
          recomp_hotkeys_source("lb") == CPS_LEFT_SHOULDER && recomp_hotkeys_source("nope") == -1);
    for (int a = 0; a < RECOMP_HK_COUNT; a++)
        CHECK(recomp_hotkeys_action(recomp_hotkeys_action_name(a)) == a);
    CHECK(recomp_hotkeys_action("bogus") == -1 && !recomp_hotkeys_action_name(RECOMP_HK_COUNT));
    /* Keys: HID usages, SDL's scancodes (SDL_SCANCODE_TAB 43, _F1 58, _A 4, _1 30, _0 39). */
    CHECK(recomp_hotkeys_key("tab") == 43 && recomp_hotkeys_key("F1") == 58 && recomp_hotkeys_key("f12") == 69);
    CHECK(recomp_hotkeys_key("a") == 4 && recomp_hotkeys_key("Z") == 29 && recomp_hotkeys_key("1") == 30 &&
          recomp_hotkeys_key("0") == 39);
    CHECK(recomp_hotkeys_key("f13") == 0 && recomp_hotkeys_key("") == 0 && recomp_hotkeys_key("xx") == 0);
    for (int k = 1; k < 256; k++) {
        const char *n = recomp_hotkeys_key_name(k);
        if (n)
            CHECK(recomp_hotkeys_key(n) == k);
    }
}

static void test_hotkey_parse(void)
{
    recomp_hotkeys hk, d;
    char buf[512];
    recomp_hotkeys_defaults(&d);
    /* The defaults: XBA-182's chords and keys. */
    CHECK(d.pad[RECOMP_HK_FF_HOLD].mod == CPS_BACK && d.pad[RECOMP_HK_FF_HOLD].button == CPS_RIGHT_TRIGGER);
    CHECK(d.pad[RECOMP_HK_QSAVE].button == CPS_RIGHT_SHOULDER && d.pad[RECOMP_HK_QLOAD].button == CPS_LEFT_SHOULDER);
    CHECK(d.pad[RECOMP_HK_MENU].mod == CPS_BACK && d.pad[RECOMP_HK_MENU].button == CPS_START);
    CHECK(d.pad[RECOMP_HK_FF_TOGGLE].button == -1 && d.pad[RECOMP_HK_SLOT_NEXT].button == -1);
    /* Rewind (XBA-194): Back + left trigger, R on a desktop (not Backspace: the GBA's Select). */
    CHECK(d.pad[RECOMP_HK_REWIND].mod == CPS_BACK && d.pad[RECOMP_HK_REWIND].button == CPS_LEFT_TRIGGER);
    CHECK(!strcmp(recomp_hotkeys_key_chord_text(d.key[RECOMP_HK_REWIND], buf, sizeof buf), "r"));
    CHECK(!strcmp(recomp_hotkeys_action_name(RECOMP_HK_REWIND), "rewind"));
    hk = d;
    CHECK(recomp_hotkeys_parse_pad(&hk, "rewind:back+lb,qload:none") == 0 &&
          hk.pad[RECOMP_HK_REWIND].button == CPS_LEFT_SHOULDER);
    CHECK(!strcmp(recomp_hotkeys_format_pad(&hk, buf, sizeof buf), "qload:none,rewind:back+left_shoulder"));
    CHECK(!strcmp(recomp_hotkeys_format_pad(&d, buf, sizeof buf), ""));
    CHECK(!strcmp(recomp_hotkeys_format_keys(&d, buf, sizeof buf), ""));
    CHECK(!strcmp(recomp_hotkeys_key_chord_text(d.key[RECOMP_HK_LOAD2], buf, sizeof buf), "shift+f2"));
    CHECK(!strcmp(recomp_hotkeys_pad_chord_text(d.pad[RECOMP_HK_QSAVE], buf, sizeof buf), "back+right_shoulder"));

    /* A remap: short names, '=' or ':', none, spaces / semicolons; the text lists only what differs. */
    hk = d;
    CHECK(recomp_hotkeys_parse_pad(&hk, "qsave:select+y; ff_toggle=l3+r3 qload:none,slot_next:dpad_right") == 0);
    CHECK(hk.pad[RECOMP_HK_QSAVE].mod == CPS_BACK && hk.pad[RECOMP_HK_QSAVE].button == CPS_NORTH);
    CHECK(hk.pad[RECOMP_HK_QLOAD].button == -1);
    CHECK(hk.pad[RECOMP_HK_SLOT_NEXT].mod == -1 && hk.pad[RECOMP_HK_SLOT_NEXT].button == CPS_DPAD_RIGHT);
    CHECK(!strcmp(recomp_hotkeys_format_pad(&hk, buf, sizeof buf),
                  "ff_toggle:left_stick_click+right_stick_click,qsave:back+north,qload:none,slot_next:dpad_right"));
    recomp_hotkeys back = d;
    CHECK(recomp_hotkeys_parse_pad(&back, buf) == 0 && !memcmp(back.pad, hk.pad, sizeof hk.pad));
    /* Rejected entries count; the good ones still apply. */
    hk = d;
    CHECK(recomp_hotkeys_parse_pad(&hk, "warp:back+a,qsave:back+back,qload:back+nothing,menu,ff_hold:start+a") == 4);
    CHECK(hk.pad[RECOMP_HK_FF_HOLD].mod == CPS_START && hk.pad[RECOMP_HK_FF_HOLD].button == CPS_SOUTH);
    CHECK(hk.pad[RECOMP_HK_QSAVE].button == CPS_RIGHT_SHOULDER);
    CHECK(recomp_hotkeys_parse_pad(&hk, NULL) == 0 && recomp_hotkeys_parse_pad(&hk, "") == 0);
    CHECK(recomp_hotkeys_parse_pad(NULL, "x") == -1);

    /* Keys. */
    hk = d;
    CHECK(recomp_hotkeys_parse_keys(&hk, "qsave:f5,qload:shift+f5,ff_toggle:ctrl+alt+q,ff_hold:none,save1:bad+f1") == 1);
    CHECK(hk.key[RECOMP_HK_QSAVE].key == 62 && hk.key[RECOMP_HK_QSAVE].mods == 0);
    CHECK(hk.key[RECOMP_HK_QLOAD].mods == RECOMP_HK_SHIFT);
    CHECK(hk.key[RECOMP_HK_FF_TOGGLE].mods == (RECOMP_HK_CTRL | RECOMP_HK_ALT) && hk.key[RECOMP_HK_FF_HOLD].key == 0);
    CHECK(!strcmp(recomp_hotkeys_format_keys(&hk, buf, sizeof buf),
                  "ff_hold:none,ff_toggle:ctrl+alt+q,qsave:f5,qload:shift+f5"));
    back = d;
    CHECK(recomp_hotkeys_parse_keys(&back, buf) == 0 && !memcmp(back.key, hk.key, sizeof hk.key));
    /* A buffer too small keeps whole entries. */
    char small[24];
    recomp_hotkeys_format_keys(&hk, small, sizeof small);
    CHECK(!strcmp(small, "ff_hold:none"));

    /* This run's: the environment, read once until reset. */
    setenv("RECOMP_HOTKEYS", "qsave:back+north", 1);
    setenv("RECOMP_HOTKEYS_KEYS", "qsave:f5", 1);
    recomp_hotkeys_reset();
    const recomp_hotkeys *g = recomp_hotkeys_get();
    CHECK(g->pad[RECOMP_HK_QSAVE].button == CPS_NORTH && g->key[RECOMP_HK_QSAVE].key == 62);
    setenv("RECOMP_HOTKEYS", "qsave:back+east", 1);
    CHECK(recomp_hotkeys_get()->pad[RECOMP_HK_QSAVE].button == CPS_NORTH);
    unsetenv("RECOMP_HOTKEYS");
    unsetenv("RECOMP_HOTKEYS_KEYS");
    recomp_hotkeys_reset();
    CHECK(!memcmp(recomp_hotkeys_get(), &d, sizeof d));
}

static void test_hotkey_pad(void)
{
    recomp_hotkeys d, hk;
    recomp_hotkeys_state st = {0};
    uint32_t pr = 0, sw = 0, held;
    recomp_hotkeys_defaults(&d);
    const uint32_t BACK = SRC("back"), RB = SRC("rb"), LB = SRC("lb"), RT = SRC("rt"), START = SRC("start");

    /* The default chords, as recomp_states_chord's test: once per press, in either order, swallowed while held. */
    CHECK(recomp_hotkeys_pad_update(&d, &st, RB, &pr, &sw) == 0 && !pr && !sw);
    CHECK(recomp_hotkeys_pad_update(&d, &st, BACK, &pr, &sw) == 0 && !pr && !sw);
    held = recomp_hotkeys_pad_update(&d, &st, BACK | RB, &pr, &sw);
    CHECK(held == RECOMP_HK_BIT(RECOMP_HK_QSAVE) && pr == held && sw == (BACK | RB));
    held = recomp_hotkeys_pad_update(&d, &st, BACK | RB, &pr, &sw);
    CHECK(held == RECOMP_HK_BIT(RECOMP_HK_QSAVE) && !pr && sw == (BACK | RB));
    held = recomp_hotkeys_pad_update(&d, &st, BACK | RB | LB, &pr, &sw);
    CHECK(pr == RECOMP_HK_BIT(RECOMP_HK_QLOAD) && sw == (BACK | RB | LB));
    CHECK(recomp_hotkeys_pad_update(&d, &st, BACK, &pr, &sw) == 0 && !pr && !sw);
    /* Back + right trigger: held while held, its buttons swallowed. */
    held = recomp_hotkeys_pad_update(&d, &st, RT | BACK, &pr, &sw);
    CHECK(held == RECOMP_HK_BIT(RECOMP_HK_FF_HOLD) && sw == (BACK | RT));
    CHECK(recomp_hotkeys_pad_update(&d, &st, RT, &pr, &sw) == 0 && !sw);
    /* The menu chord is the shell's: never held, never swallowed. */
    CHECK(recomp_hotkeys_pad_update(&d, &st, BACK | START, &pr, &sw) == 0 && !pr && !sw);
    /* Back + left trigger: rewind, held while held (XBA-194); not recomp_hotkeys_run's (a hold). */
    held = recomp_hotkeys_pad_update(&d, &st, BACK | SRC("lt"), &pr, &sw);
    CHECK(held == RECOMP_HK_BIT(RECOMP_HK_REWIND) && pr == held && sw == (BACK | SRC("lt")));
    CHECK(recomp_hotkeys_run(pr) == 0);
    CHECK(recomp_hotkeys_pad_update(&d, &st, 0, &pr, &sw) == 0);

    /* Remapped: quick save on Back + north, fast-forward hold on L3 + R3, slot next on the D-pad right alone. */
    hk = d;
    CHECK(recomp_hotkeys_parse_pad(&hk, "qsave:back+north,ff_hold:l3+r3,slot_next:right,slot_prev:back+right") == 0);
    st.held = 0;
    CHECK(recomp_hotkeys_pad_update(&hk, &st, BACK | RB, &pr, &sw) == 0 && !sw);   /* the old chord: nothing */
    held = recomp_hotkeys_pad_update(&hk, &st, BACK | SRC("y"), &pr, &sw);
    CHECK(pr == RECOMP_HK_BIT(RECOMP_HK_QSAVE) && sw == (BACK | SRC("y")));
    held = recomp_hotkeys_pad_update(&hk, &st, SRC("l3") | SRC("r3"), &pr, &sw);
    CHECK(held == RECOMP_HK_BIT(RECOMP_HK_FF_HOLD) && sw == (SRC("l3") | SRC("r3")));
    CHECK(recomp_hotkeys_pad_update(&hk, &st, RT | BACK, &pr, &sw) == 0);   /* the old hold: nothing */
    /* A button alone fires on its own, not while a two-button chord holds it. */
    held = recomp_hotkeys_pad_update(&hk, &st, SRC("right"), &pr, &sw);
    CHECK(pr == RECOMP_HK_BIT(RECOMP_HK_SLOT_NEXT) && sw == SRC("right"));
    recomp_hotkeys_pad_update(&hk, &st, 0, &pr, &sw);
    held = recomp_hotkeys_pad_update(&hk, &st, BACK | SRC("right"), &pr, &sw);
    CHECK(held == RECOMP_HK_BIT(RECOMP_HK_SLOT_PREV) && pr == held);
    /* NULL out-parameters and state are fine. */
    CHECK(recomp_hotkeys_pad_update(&hk, NULL, BACK | SRC("y"), NULL, NULL) == RECOMP_HK_BIT(RECOMP_HK_QSAVE));
    CHECK(recomp_hotkeys_pad_update(NULL, &st, BACK | SRC("y"), &pr, &sw) == 0 && !sw);
}

static void test_hotkey_keys(void)
{
    recomp_hotkeys d, hk;
    recomp_hotkeys_state st = {0};
    uint8_t ks[256];
    uint32_t pr = 0, held;
    recomp_hotkeys_defaults(&d);
    memset(ks, 0, sizeof ks);
    ks[58] = 1;   /* F1: save slot 1, once */
    CHECK(recomp_hotkeys_keys_update(&d, &st, ks, 256, &pr) == RECOMP_HK_BIT(RECOMP_HK_SAVE1) && pr);
    recomp_hotkeys_keys_update(&d, &st, ks, 256, &pr);
    CHECK(!pr);
    ks[58] = 0;
    recomp_hotkeys_keys_update(&d, &st, ks, 256, &pr);
    ks[229] = ks[59] = 1;   /* right Shift + F2: load slot 2, not save 2 */
    held = recomp_hotkeys_keys_update(&d, &st, ks, 256, &pr);
    CHECK(held == RECOMP_HK_BIT(RECOMP_HK_LOAD2) && pr == held);
    memset(ks, 0, sizeof ks);
    ks[43] = 1;   /* Tab: the hold */
    CHECK(recomp_hotkeys_keys_update(&d, &st, ks, 256, &pr) == RECOMP_HK_BIT(RECOMP_HK_FF_HOLD));
    /* A short array: keys past it are up. */
    CHECK(recomp_hotkeys_keys_update(&d, &st, ks, 40, &pr) == 0);
    /* Remapped keys. */
    hk = d;
    CHECK(recomp_hotkeys_parse_keys(&hk, "ff_hold:space,qsave:f5,save1:none") == 0);
    memset(ks, 0, sizeof ks);
    ks[43] = ks[58] = 1;
    CHECK(recomp_hotkeys_keys_update(&hk, &st, ks, 256, &pr) == 0);
    ks[44] = ks[62] = 1;
    held = recomp_hotkeys_keys_update(&hk, &st, ks, 256, &pr);
    CHECK(held == (RECOMP_HK_BIT(RECOMP_HK_FF_HOLD) | RECOMP_HK_BIT(RECOMP_HK_QSAVE)));
}

static void test_hotkey_run(void)
{
    recomp_state_ops ops = {"gba", fake_save, fake_load, &s_machine};
    recomp_state_event ev;
    recomp_states_attach(&ops, s_dir);
    recomp_ff_reset();
    s_machine = 41;
    CHECK(recomp_hotkeys_run(RECOMP_HK_BIT(RECOMP_HK_SAVE2)) == 1);
    CHECK(recomp_states_quick_slot() == 2);
    /* Slot next / previous go around and record an event. */
    CHECK(recomp_hotkeys_run(RECOMP_HK_BIT(RECOMP_HK_SLOT_NEXT)) == 1 && recomp_states_quick_slot() == 3);
    recomp_states_last_event(&ev);
    CHECK(ev.kind == RECOMP_STATE_EV_SLOT && ev.slot == 3 && ev.rc == RECOMP_STATE_OK);
    recomp_hotkeys_run(RECOMP_HK_BIT(RECOMP_HK_SLOT_NEXT));
    recomp_hotkeys_run(RECOMP_HK_BIT(RECOMP_HK_SLOT_NEXT));
    CHECK(recomp_states_quick_slot() == 1);
    recomp_hotkeys_run(RECOMP_HK_BIT(RECOMP_HK_SLOT_PREV));
    CHECK(recomp_states_quick_slot() == 4);
    /* Quick save / load act on the quick slot. */
    s_machine = 77;
    CHECK(recomp_hotkeys_run(RECOMP_HK_BIT(RECOMP_HK_QSAVE)) == 1);
    recomp_states_last_event(&ev);
    CHECK(ev.kind == RECOMP_STATE_EV_SAVED && ev.slot == 4 && ev.rc == RECOMP_STATE_OK);
    s_machine = 0;
    recomp_hotkeys_run(RECOMP_HK_BIT(RECOMP_HK_QLOAD));
    CHECK(s_machine == 77);
    recomp_hotkeys_run(RECOMP_HK_BIT(RECOMP_HK_LOAD2));
    CHECK(s_machine == 41);
    /* The toggle flips; the hold and the menu do not run here. */
    CHECK(recomp_hotkeys_run(RECOMP_HK_BIT(RECOMP_HK_FF_TOGGLE)) == 1 && recomp_ff_toggled());
    recomp_hotkeys_run(RECOMP_HK_BIT(RECOMP_HK_FF_TOGGLE));
    CHECK(!recomp_ff_toggled());
    CHECK(recomp_hotkeys_run(RECOMP_HK_BIT(RECOMP_HK_FF_HOLD) | RECOMP_HK_BIT(RECOMP_HK_MENU)) == 0);
    CHECK(!recomp_ff_active());
    recomp_states_detach();
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
    test_hotkey_names();
    test_hotkey_parse();
    test_hotkey_pad();
    test_hotkey_keys();
    test_hotkey_run();
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
