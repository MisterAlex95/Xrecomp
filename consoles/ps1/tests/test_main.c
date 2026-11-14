/* The PlayStation runtime (XBA-146, XBA-168): its pure helpers (pad mapping, sticks, colour conversion, card path,
 * EXE magic, fallback key, frame length) and headless runs (SDL's dummy drivers) of our homebrew through OpenBIOS:
 * an EXE fast-booted, the same through "fd:N" (the app shell's documents), a disc booted by the BIOS from its .cue,
 * memory cards read back and left alone while unchanged, the app shell's entry, the errors, and a game's save
 * (the mcard homebrew: BIOS memory card functions) written to disk after the debounce, then reloaded; save states
 * (XBA-182) saved and loaded through recomp_states.h while the game runs on its own thread, another game's refused;
 * the internal resolution (XBA-184): its keys, and gfx.exe's last picture at 2x and 4x (size, golden CRC).
 *
 *   ps1_runtime_test [BIOS HOMEBREW_DIR]     (the runs skip, the helpers still run, when either is absent) */
#include "host_pad.h"
#include "host_window.h"
#include "host_time.h"
#include "ps1_core.h"
#include "ps1_devices.h"
#include "ps1_runtime.h"
#include "recomp_lifecycle.h"
#include "recomp_states.h"

#include <pthread.h>

#include <SDL.h>

#include <fcntl.h>
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

static uint32_t crc32_ref(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xffffffffu;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = crc & 1 ? 0xedb88320u ^ (crc >> 1) : crc >> 1;
    }
    return ~crc;
}

/* The display options (XBA-181): the runtime's own keys win over the shared ones, which win over its defaults;
 * every key accepts both filters and the three scales. */
static void test_display(void)
{
    ps1_runtime_opts o;
    int scale = -1, filter = -1;

    ps1_runtime_opts_init(&o);
    unsetenv("RECOMP_PS1_SCALE");
    unsetenv("RECOMP_PS1_FILTER");
    unsetenv("RECOMP_SCALE");
    unsetenv("RECOMP_FILTER");
    ps1_runtime_display(&o, &scale, &filter);
    CHECK(scale == HOST_SCALE_ASPECT && filter == HOST_FILTER_LINEAR);
    setenv("RECOMP_SCALE", "stretch", 1);
    setenv("RECOMP_FILTER", "nearest", 1);
    ps1_runtime_display(&o, &scale, &filter);
    CHECK(scale == HOST_SCALE_STRETCH && filter == HOST_FILTER_NEAREST);
    setenv("RECOMP_PS1_SCALE", "integer", 1);
    setenv("RECOMP_PS1_FILTER", "linear", 1);
    ps1_runtime_display(&o, &scale, &filter);
    CHECK(scale == HOST_SCALE_INTEGER && filter == HOST_FILTER_LINEAR);
    setenv("RECOMP_PS1_FILTER", "nearest", 1);
    ps1_runtime_display(NULL, NULL, &filter);
    CHECK(filter == HOST_FILTER_NEAREST);
    o.scale = HOST_SCALE_ASPECT;   /* the caller's choice (a player's --scale) wins over every key */
    o.filter = HOST_FILTER_LINEAR;
    ps1_runtime_display(&o, &scale, &filter);
    CHECK(scale == HOST_SCALE_ASPECT && filter == HOST_FILTER_LINEAR);
    unsetenv("RECOMP_PS1_SCALE");
    unsetenv("RECOMP_PS1_FILTER");
    unsetenv("RECOMP_SCALE");
    unsetenv("RECOMP_FILTER");
}

/* The internal resolution (XBA-184): RECOMP_PS1_UPSCALE over the shared RECOMP_UPSCALE, the caller's over both;
 * 8 (the N64 renderer's most) is 4 here, 3 is 2, nonsense is skipped. */
static void test_upscale_keys(void)
{
    ps1_runtime_opts o;
    ps1_runtime_opts_init(&o);
    unsetenv("RECOMP_PS1_UPSCALE");
    unsetenv("RECOMP_UPSCALE");
    CHECK(o.upscale == -1 && ps1_runtime_upscale(&o) == 1 && ps1_runtime_upscale(NULL) == 1);
    setenv("RECOMP_UPSCALE", "2", 1);
    CHECK(ps1_runtime_upscale(&o) == 2);
    setenv("RECOMP_UPSCALE", "8", 1);
    CHECK(ps1_runtime_upscale(&o) == 4);
    setenv("RECOMP_UPSCALE", "3", 1);
    CHECK(ps1_runtime_upscale(&o) == 2);
    setenv("RECOMP_PS1_UPSCALE", "1", 1);
    CHECK(ps1_runtime_upscale(&o) == 1);
    setenv("RECOMP_PS1_UPSCALE", "4x", 1);
    CHECK(ps1_runtime_upscale(&o) == 4);
    setenv("RECOMP_PS1_UPSCALE", "native", 1); /* not a number: the shared key */
    setenv("RECOMP_UPSCALE", "2", 1);
    CHECK(ps1_runtime_upscale(&o) == 2);
    setenv("RECOMP_UPSCALE", "0", 1);
    CHECK(ps1_runtime_upscale(&o) == 1);
    o.upscale = 4;
    CHECK(ps1_runtime_upscale(&o) == 4);
    unsetenv("RECOMP_PS1_UPSCALE");
    unsetenv("RECOMP_UPSCALE");
}

static void test_helpers(void)
{
    char path[256], key[32], want[32];

    /* Xbox layout onto the PS1 pad. */
    CHECK(ps1_runtime_pad_buttons(HOST_PAD_A, 0, 0, 0, 0, 0) == PS1_PAD_CROSS);
    CHECK(ps1_runtime_pad_buttons(HOST_PAD_B, 0, 0, 0, 0, 0) == PS1_PAD_CIRCLE);
    CHECK(ps1_runtime_pad_buttons(HOST_PAD_X, 0, 0, 0, 0, 0) == PS1_PAD_SQUARE);
    CHECK(ps1_runtime_pad_buttons(HOST_PAD_Y, 0, 0, 0, 0, 0) == PS1_PAD_TRIANGLE);
    CHECK(ps1_runtime_pad_buttons(HOST_PAD_LSHOULDER | HOST_PAD_RSHOULDER, 0, 0, 0, 0, 0)
          == (PS1_PAD_L1 | PS1_PAD_R1));
    CHECK(ps1_runtime_pad_buttons(0, 0, 0, 30000, 30000, 0) == (PS1_PAD_L2 | PS1_PAD_R2));
    CHECK(ps1_runtime_pad_buttons(0, 0, 0, 10000, 0, 0) == 0);
    CHECK(ps1_runtime_pad_buttons(HOST_PAD_START | HOST_PAD_BACK, 0, 0, 0, 0, 0) == (PS1_PAD_START | PS1_PAD_SELECT));
    CHECK(ps1_runtime_pad_buttons(HOST_PAD_LSTICK | HOST_PAD_RSTICK, 0, 0, 0, 0, 1) == (PS1_PAD_L3 | PS1_PAD_R3));
    CHECK(ps1_runtime_pad_buttons(HOST_PAD_DPAD_UP | HOST_PAD_DPAD_LEFT, 0, 0, 0, 0, 0) == (PS1_PAD_UP | PS1_PAD_LEFT));
    CHECK(ps1_runtime_pad_buttons(HOST_PAD_DPAD_DOWN | HOST_PAD_DPAD_RIGHT, 0, 0, 0, 0, 1)
          == (PS1_PAD_DOWN | PS1_PAD_RIGHT));
    /* Digital: the left stick is a D-pad too; analog: it is the stick only. */
    CHECK(ps1_runtime_pad_buttons(0, -32768, 0, 0, 0, 0) == PS1_PAD_LEFT);
    CHECK(ps1_runtime_pad_buttons(0, 20000, 20000, 0, 0, 0) == (PS1_PAD_RIGHT | PS1_PAD_DOWN));
    CHECK(ps1_runtime_pad_buttons(0, 5000, -5000, 0, 0, 0) == 0);
    CHECK(ps1_runtime_pad_buttons(0, -32768, 32767, 0, 0, 1) == 0);
    CHECK(ps1_runtime_pad_buttons(HOST_PAD_DPAD_LEFT, 32767, 0, 0, 0, 0) == 0); /* left + right: neither */
    CHECK(ps1_runtime_pad_buttons(HOST_PAD_GUIDE, 0, 0, 0, 0, 0) == 0);

    CHECK(ps1_runtime_axis(0) == 0x80);
    CHECK(ps1_runtime_axis(-32768) == 0x00);
    CHECK(ps1_runtime_axis(32767) == 0xFF);
    CHECK(ps1_runtime_axis(-99999) == 0x00 && ps1_runtime_axis(99999) == 0xFF);

    uint32_t px[2] = {0x00FF8000u, 0x000000FFu};
    uint8_t out[8];
    ps1_runtime_xrgb_to_rgba(px, out, 2);
    CHECK(out[0] == 0xFF && out[1] == 0x80 && out[2] == 0x00 && out[3] == 255);
    CHECK(out[4] == 0 && out[5] == 0 && out[6] == 0xFF && out[7] == 255);

    CHECK(ps1_runtime_card_path("/data/save", "SLUS-00594", 0, path, sizeof path) == 0
          && !strcmp(path, "/data/save/ps1-SLUS-00594/card1.mcr"));
    CHECK(ps1_runtime_card_path("/s", "K", 1, path, sizeof path) == 0 && !strcmp(path, "/s/ps1-K/card2.mcr"));
    CHECK(ps1_runtime_card_path(NULL, "K", 0, path, sizeof path) == -1);
    CHECK(ps1_runtime_card_path("/s", "K", 2, path, sizeof path) == -1);
    CHECK(ps1_runtime_card_path("/s", "K", 0, path, 8) == -1);

    CHECK(ps1_runtime_is_exe((const uint8_t *)"PS-X EXE\0\0", 10));
    CHECK(!ps1_runtime_is_exe((const uint8_t *)"PS-X EX", 7));
    CHECK(!ps1_runtime_is_exe((const uint8_t *)"\0\xff\xff\xff\xff\xff\xff\xff", 8));

    uint8_t exe[64] = "PS-X EXE";
    CHECK(ps1_runtime_fallback_key(exe, sizeof exe, NULL, key, sizeof key) == 0);
    snprintf(want, sizeof want, "EXE-%08X", (unsigned)crc32_ref(exe, sizeof exe));
    CHECK(!strcmp(key, want));
    CHECK(ps1_runtime_fallback_key(NULL, 0, "/nonexistent.cue", key, sizeof key) == 0 && !strcmp(key, "PSX-UNKNOWN"));
    CHECK(ps1_runtime_fallback_key(NULL, 0, NULL, key, 8) == -1);

    /* NTSC: 263 lines x 3413 video cycles at 11/7 of the CPU clock; PAL 314 x 3406. Held to 45..75 Hz. */
    uint64_t ntsc = ps1_runtime_frame_ns(263ull * 3413 * 7 / 11), pal = ps1_runtime_frame_ns(314ull * 3406 * 7 / 11);
    CHECK(ntsc > 16800000 && ntsc < 16900000);
    CHECK(pal > 20000000 && pal < 20200000);
    CHECK(ps1_runtime_frame_ns(0) == 1000000000ull / 75);
    CHECK(ps1_runtime_frame_ns(PS1_CPU_HZ) == 1000000000ull / 45);
}

/* ps1_runtime_m3u (XBA-166): BOM, CR LF, comments, blank lines, relative and absolute entries, the max. */
static void test_m3u(void)
{
    char dir[] = "/tmp/ps1_m3u_test.XXXXXX", path[300];
    if (!mkdtemp(dir)) {
        CHECK(0);
        return;
    }
    snprintf(path, sizeof path, "%s/Game (Multi).m3u", dir);
    FILE *f = fopen(path, "wb");
    if (!f) {
        CHECK(0);
        return;
    }
    fputs("\xEF\xBB\xBFGame (Disc 1).cue\r\n\r\n# comment\r\n  Game (Disc 2).chd  \r\n/abs/Disc 3.bin", f);
    fclose(f);
    static char out[4][PS1_RUNTIME_PATH_MAX];
    char want[400];
    CHECK(ps1_runtime_m3u(path, out, 4) == 3);
    snprintf(want, sizeof want, "%s/Game (Disc 1).cue", dir);
    CHECK(!strcmp(out[0], want));
    snprintf(want, sizeof want, "%s/Game (Disc 2).chd", dir);
    CHECK(!strcmp(out[1], want));
    CHECK(!strcmp(out[2], "/abs/Disc 3.bin"));
    CHECK(ps1_runtime_m3u(path, out, 2) == 2);
    CHECK(ps1_runtime_m3u("/nonexistent.m3u", out, 4) == 0);
    unlink(path);
    rmdir(dir);
}

static long file_size(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 ? (long)st.st_size : -1;
}

/* The size a binary PPM says it is. */
static int ppm_size(const char *p, int *w, int *h)
{
    FILE *f = fopen(p, "rb");
    int ok = f && fscanf(f, "P6 %d %d", w, h) == 2;
    if (f)
        fclose(f);
    return ok ? 0 : -1;
}

/* Runs o with stdout in log (the runtime's "ps1: ..." lines); returns ps1_runtime_run's result. */
static int run_logged(const ps1_runtime_opts *o, const char *log)
{
    fflush(stdout);
    int saved = dup(1), lf = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (saved < 0 || lf < 0 || dup2(lf, 1) < 0)
        return -2;
    close(lf);
    int rc = ps1_runtime_run(o);
    fflush(stdout);
    dup2(saved, 1);
    close(saved);
    return rc;
}

static int file_has(const char *path, const char *text)
{
    static char buf[1 << 16];
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return strstr(buf, text) != NULL;
}

/* A game's save (XBA-167): mcard.exe creates XBAMCARD through the BIOS card functions; the runtime writes card1.mcr a
 * second after the game's last write to the card (the run goes on for seconds after the save: the debounce wrote
 * it, not the exit), a valid card holding the file (header, directory entry, the 8 KB pattern). Run again on that
 * card: it is loaded, the game finds its save and reads it (the BIOS's card checks write nothing new), so the file is
 * not written again; without the reload the game would have created the save on a blank card and the card would
 * have been written. */
static void test_save(const char *dir, const char *hb)
{
    char exe[600], card[700], log[700], ref[720], cmd[1600];
    snprintf(exe, sizeof exe, "%s/mcard.exe", hb);
    if (access(exe, R_OK) != 0) {
        printf("save run skipped: no %s\n", exe);
        return;
    }
    ps1_runtime_opts o;
    ps1_runtime_opts_init(&o);
    o.game = exe;
    o.save_dir = dir;
    o.title_key = "MCRD-00001";
    o.frames = 6000; /* the save is done by frame ~450; seconds of unpaced frames follow */
    o.unpaced = 1;
    CHECK(ps1_runtime_card_path(dir, "MCRD-00001", 0, card, sizeof card) == 0);
    snprintf(log, sizeof log, "%s/save1.log", dir);
    CHECK(run_logged(&o, log) == 0);
    CHECK(file_has(log, "memory card 1 written (written by the game)"));
    CHECK(!file_has(log, "memory card 1 written (exit)"));
    FILE *f = fopen(card, "rb");
    static uint8_t d[PS1_CARD_SIZE + 1];
    size_t n = f ? fread(d, 1, sizeof d, f) : 0;
    if (f)
        fclose(f);
    CHECK(n == PS1_CARD_SIZE);
    if (n == PS1_CARD_SIZE) {
        uint8_t x = 0;
        for (int i = 0; i < 0x7F; i++)
            x ^= d[i];
        CHECK(d[0] == 'M' && d[1] == 'C' && d[0x7F] == x);
        const uint8_t *e = d + 128; /* directory frame 1: the first block */
        CHECK(e[0] == 0x51 && e[4] == 0x00 && e[5] == 0x20 && !memcmp(e + 10, "XBAMCARD", 9));
        int same = 1;
        for (unsigned i = 0; i < 8192; i++)
            same &= d[0x2000 + i] == (uint8_t)((i * 7u + 0x5Au) ^ (i >> 8));
        CHECK(same);
    }
    snprintf(ref, sizeof ref, "%s.ref", card);
    snprintf(cmd, sizeof cmd, "cp '%s' '%s'", card, ref);
    CHECK(system(cmd) == 0);

    o.frames = 900;
    snprintf(log, sizeof log, "%s/save2.log", dir);
    CHECK(run_logged(&o, log) == 0);
    CHECK(file_has(log, "memory card 1 loaded from"));
    CHECK(!file_has(log, "memory card 1 written"));
    snprintf(cmd, sizeof cmd, "cmp -s '%s' '%s'", card, ref);
    CHECK(system(cmd) == 0);
}

static void *run_thread(void *arg)
{
    static int rc;
    rc = ps1_runtime_run((const ps1_runtime_opts *)arg);
    return &rc;
}

static void wait_attached(void)
{
    for (int i = 0; i < 300 && !recomp_states_available(); i++)
        usleep(10 * 1000);
}

/* Save states (XBA-182): gfx.exe plays paced on its own thread; slot 1 saved (with a 4:3 thumbnail of its 320 x 240
 * picture) and loaded as the in-game menu does, slot 2 while the game is held; fast-forward toggled; the slot copied
 * into another game's folder (cpu.exe) refused there. */
static void test_states(const char *dir, const char *hb)
{
    char exe[600], exe2[600], slot[800], other[800], cmd[1700];
    snprintf(exe, sizeof exe, "%s/gfx.exe", hb);
    snprintf(exe2, sizeof exe2, "%s/cpu.exe", hb);
    recomp_states_detach();
    CHECK(recomp_states_save(1) == RECOMP_STATE_ENOGAME);
    ps1_runtime_opts o;
    ps1_runtime_opts_init(&o);
    o.game = exe;
    o.save_dir = dir;
    o.title_key = "STAT-00001";
    o.headless = 1;
    pthread_t th;
    void *ret = NULL;
    SDL_Event quit;
    memset(&quit, 0, sizeof quit);
    quit.type = SDL_QUIT;
    CHECK(pthread_create(&th, NULL, run_thread, &o) == 0);
    wait_attached();
    CHECK(recomp_states_available() && !strcmp(recomp_states_console(), "ps1"));
    usleep(400 * 1000);
    recomp_state_meta meta;
    static uint16_t thumb[RECOMP_STATE_THUMB_W * RECOMP_STATE_THUMB_H];
    CHECK(recomp_states_info(1, &meta, NULL) == RECOMP_STATE_EEMPTY);
    CHECK(recomp_states_save(1) == RECOMP_STATE_OK);
    CHECK(recomp_states_info(1, &meta, thumb) == RECOMP_STATE_OK);
    CHECK(!strcmp(meta.console, "ps1") && !strcmp(meta.key, "STAT-00001") && meta.frames > 0);
    CHECK(meta.thumb_w == 160 && meta.thumb_h == 120);
    int lit = 0;
    for (int i = 0; i < meta.thumb_w * meta.thumb_h; i++)
        lit += thumb[i] != 0;
    CHECK(lit > 100); /* gfx.exe's picture, not black */
    snprintf(slot, sizeof slot, "%s/ps1-STAT-00001/state1.st", dir);
    struct stat sb;
    CHECK(stat(slot, &sb) == 0 && sb.st_size > 3000000);
    recomp_ff_set(1);
    usleep(100 * 1000);
    recomp_ff_set(0);
    CHECK(recomp_states_load(1) == RECOMP_STATE_OK);
    recomp_lifecycle_pause_for(RECOMP_HOLD_MENU);
    CHECK(recomp_states_save(2) == RECOMP_STATE_OK && recomp_states_load(2) == RECOMP_STATE_OK);
    recomp_lifecycle_resume_for(RECOMP_HOLD_MENU);
    usleep(50 * 1000);
    CHECK(SDL_PushEvent(&quit) == 1);
    CHECK(pthread_join(th, &ret) == 0 && ret && *(int *)ret == 0);
    CHECK(!recomp_states_available());

    if (access(exe2, R_OK) != 0)
        return;
    snprintf(other, sizeof other, "%s/ps1-OTHR-00002", dir);
    mkdir(other, 0755);
    snprintf(other, sizeof other, "%s/ps1-OTHR-00002/state3.st", dir);
    snprintf(cmd, sizeof cmd, "cp '%s' '%s'", slot, other);
    CHECK(system(cmd) == 0);
    ps1_runtime_opts_init(&o);
    o.game = exe2;
    o.save_dir = dir;
    o.title_key = "OTHR-00002";
    o.headless = 1;
    CHECK(pthread_create(&th, NULL, run_thread, &o) == 0);
    wait_attached();
    CHECK(recomp_states_info(3, &meta, NULL) == RECOMP_STATE_OK);
    CHECK(recomp_states_load(3) == RECOMP_STATE_EGAME);
    CHECK(SDL_PushEvent(&quit) == 1);
    CHECK(pthread_join(th, &ret) == 0 && ret && *(int *)ret == 0);
}

/* CRC-32 of a file (at most 32 MB). */
static uint32_t file_crc(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    size_t cap = 32u << 20, n;
    uint8_t *buf = malloc(cap);
    n = buf ? fread(buf, 1, cap, f) : 0;
    fclose(f);
    uint32_t c = buf ? crc32_ref(buf, n) : 0;
    free(buf);
    return c;
}

#ifndef GOLDEN_GFX_SHOT_1X
#define GOLDEN_GFX_SHOT_1X 0xB6F0655Bu
#endif
#ifndef GOLDEN_GFX_SHOT_2X
#define GOLDEN_GFX_SHOT_2X 0x81A736DDu
#endif
#ifndef GOLDEN_GFX_SHOT_4X
#define GOLDEN_GFX_SHOT_4X 0x5694F051u
#endif

/* gfx.exe fast-booted, 60 frames, at 1x / 2x / 4x through RECOMP_UPSCALE (the key the app exports): the last picture
 * 320 x 240 times the factor, the same pixels on every run (golden CRC-32 of the PPM). */
static void test_upscale_runs(const char *dir, const char *exe)
{
    static const int factors[3] = { 1, 2, 4 };
    static const uint32_t golden[3] = { GOLDEN_GFX_SHOT_1X, GOLDEN_GFX_SHOT_2X, GOLDEN_GFX_SHOT_4X };
    char shot[300], log[300], val[8];
    for (int i = 0; i < 3; i++) {
        int n = factors[i], w = 0, h = 0;
        ps1_runtime_opts o;
        ps1_runtime_opts_init(&o);
        o.game = exe;
        o.title_key = "UPSC-00001";
        o.frames = 60;
        o.unpaced = 1;
        snprintf(shot, sizeof shot, "%s/up%d.ppm", dir, n);
        snprintf(log, sizeof log, "%s/up%d.log", dir, n);
        o.screenshot = shot;
        snprintf(val, sizeof val, "%d", n);
        setenv("RECOMP_UPSCALE", val, 1);
        CHECK(run_logged(&o, log) == 0);
        unsetenv("RECOMP_UPSCALE");
        CHECK(ppm_size(shot, &w, &h) == 0 && w == 320 * n && h == 240 * n);
        if (n > 1)
            CHECK(file_has(log, n == 2 ? "internal resolution 2x" : "internal resolution 4x"));
        uint32_t crc = file_crc(shot);
        printf("gfx.exe at %dx: %d x %d, CRC %08X\n", n, w, h, crc);
        if (golden[i] && crc != golden[i]) {
            fprintf(stderr, "FAIL gfx.exe at %dx: CRC %08X, want %08X\n", n, crc, golden[i]);
            failures++;
        }
    }
}

static void test_runs(const char *bios, const char *hb)
{
    char dir[] = "/tmp/ps1_runtime_test.XXXXXX", exe[600], cue[600], shot[300], card[400], spec[32];

    snprintf(exe, sizeof exe, "%s/gfx.exe", hb);
    snprintf(cue, sizeof cue, "%s/gfx.cue", hb);
    if (access(bios, R_OK) != 0 || access(exe, R_OK) != 0) {
        printf("runs skipped: no %s or %s (tools/consoles/build_ps1_bios.sh, build_homebrew.sh ps1)\n", bios, exe);
        return;
    }
    if (!mkdtemp(dir)) {
        CHECK(0);
        return;
    }
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("SDL_AUDIODRIVER", "dummy", 1);
    setenv("RECOMP_PS1_BIOS", bios, 1);
    CHECK(SDL_Init(SDL_INIT_EVENTS) == 0);

    /* 1: an EXE fast-booted: its 320 x 240 picture; no card file made for a card the game never wrote. */
    ps1_runtime_opts o;
    ps1_runtime_opts_init(&o);
    o.game = exe;
    o.save_dir = dir;
    o.title_key = "TEST-00001";
    o.frames = 60;
    o.unpaced = 1;
    snprintf(shot, sizeof shot, "%s/last.ppm", dir);
    o.screenshot = shot;
    int w = 0, h = 0;
    CHECK(ps1_runtime_run(&o) == 0);
    CHECK(ppm_size(shot, &w, &h) == 0 && w == 320 && h == 240);
    CHECK(ps1_runtime_card_path(dir, "TEST-00001", 0, card, sizeof card) == 0);
    CHECK(file_size(card) == -1);

    /* 2: a card on disk is loaded and, unchanged, not written again (same bytes, no .tmp left). Through "fd:N"
     * with the descriptor's offset elsewhere: read from 0, left open. */
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "mkdir -p '%s/ps1-TEST-00001' && head -c 131072 /dev/urandom > '%s'", dir, card);
    CHECK(system(cmd) == 0);
    snprintf(cmd, sizeof cmd, "cp '%s' '%s.ref'", card, card);
    CHECK(system(cmd) == 0);
    int fd = open(exe, O_RDONLY);
    CHECK(fd >= 0 && lseek(fd, 77, SEEK_SET) == 77);
    snprintf(spec, sizeof spec, "fd:%d", fd);
    o.game = spec;
    o.screenshot = NULL;
    CHECK(ps1_runtime_run(&o) == 0);
    CHECK(fcntl(fd, F_GETFD) != -1);
    close(fd);
    snprintf(cmd, sizeof cmd, "cmp -s '%s' '%s.ref'", card, card);
    CHECK(system(cmd) == 0);
    char tmp[420];
    snprintf(tmp, sizeof tmp, "%s.tmp", card);
    CHECK(file_size(tmp) == -1);

    /* 3: a disc booted by the BIOS from its .cue (SYSTEM.CNF): past the BIOS's 640 x 480 logo, the program's
     * 320 x 240 picture. */
    if (access(cue, R_OK) == 0) {
        ps1_runtime_opts_init(&o);
        o.game = cue;
        o.frames = 600;
        o.unpaced = 1;
        o.screenshot = shot;
        CHECK(ps1_runtime_run(&o) == 0);
        CHECK(ppm_size(shot, &w, &h) == 0 && w == 320 && h == 240);
    }

    /* 4: errors: no game, a missing file, a bad fd, a BIOS that is not 512 KB. */
    ps1_runtime_opts_init(&o);
    CHECK(ps1_runtime_run(&o) == -1);
    o.game = "/nonexistent/x.cue";
    CHECK(ps1_runtime_run(&o) == -1);
    o.game = "fd:-1";
    CHECK(ps1_runtime_run(&o) == -1);
    o.game = exe;
    o.bios = exe;
    CHECK(ps1_runtime_run(&o) == -1);

    /* 5: the app shell's entry, bounded by RECOMP_PS1_FRAMES, cards in ps1_runtime_set_save_dir's dir (the card
     * from 2 is read there). */
    ps1_runtime_set_save_dir(dir);
    setenv("RECOMP_PS1_FRAMES", "3", 1);
    CHECK(ps1_runtime_start(exe, "libtitle.so", "TEST-00001") == 0);
    unsetenv("RECOMP_PS1_FRAMES");
    ps1_runtime_set_save_dir(NULL);
    snprintf(cmd, sizeof cmd, "cmp -s '%s' '%s.ref'", card, card);
    CHECK(system(cmd) == 0);

    /* 6: a game's save written by the debounce, then reloaded; the internal resolution. */
    test_save(dir, hb);
    test_upscale_runs(dir, exe);
    test_states(dir, hb);

    /* 7: multi-disc (XBA-166): a .m3u plays its first disc (fast boot: the BIOS boots it without its shell); a swap
     * opens the lid, a second later closes it on the other disc; an image that does not open is refused. */
    char hcue[600];
    snprintf(hcue, sizeof hcue, "%s/hello.cue", hb);
    if (access(cue, R_OK) == 0 && access(hcue, R_OK) == 0) {
        char m3u[400], log[420];
        snprintf(m3u, sizeof m3u, "%s/Game.m3u", dir);
        FILE *mf = fopen(m3u, "w");
        if (mf) {
            fprintf(mf, "# two discs\r\n%s\r\n%s\r\n", cue, hcue);
            fclose(mf);
        }
        ps1_runtime_opts_init(&o);
        o.game = m3u;
        o.frames = 240;
        o.unpaced = 1;
        o.swap_frame = 100;
        o.swap_disc = hcue;
        snprintf(log, sizeof log, "%s/swap.log", dir);
        CHECK(run_logged(&o, log) == 0);
        CHECK(file_has(log, "playlist Game.m3u: 2 discs"));
        CHECK(file_has(log, "disc, BIOS boot without its shell"));
        CHECK(file_has(log, "lid opened at frame 100 (changing the disc to hello.cue)"));
        CHECK(file_has(log, "lid closed at frame 160, disc hello.cue in"));
        o.swap_disc = "/nonexistent/disc2.cue";
        o.frames = 120;
        CHECK(run_logged(&o, log) == 0);
        CHECK(!file_has(log, "lid opened"));
        /* no fast boot: the BIOS's shell runs */
        ps1_runtime_opts_init(&o);
        o.game = cue;
        o.frames = 2;
        o.unpaced = 1;
        o.fast_boot = 0;
        CHECK(run_logged(&o, log) == 0);
        CHECK(file_has(log, "(disc, BIOS boot)"));
        CHECK(ps1_runtime_change_disc(hcue) == -1); /* no game running */
    }
    SDL_Quit();

    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0)
        fprintf(stderr, "note: could not remove %s\n", dir);
}

int main(int argc, char **argv)
{
    const char *v = ps1_runtime_version();

    if (!v || strncmp(v, "ps1_runtime ", 12) != 0) {
        fprintf(stderr, "FAIL: version \"%s\"\n", v ? v : "(null)");
        return 1;
    }
    if (xbox_HostNowNs() == 0) { /* the host layer is linked in */
        fprintf(stderr, "FAIL: no host clock\n");
        return 1;
    }
    test_helpers();
    test_display();
    test_upscale_keys();
    test_m3u();
    if (argc >= 3)
        test_runs(argv[1], argv[2]);
    else
        printf("runs skipped: no BIOS / homebrew arguments\n");
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("%s: ok\n", v);
    return 0;
}
