/* The Nintendo 64 runtime (XBA-146, XBA-174): its pure helpers (pad mapping, stick, colour conversion, save paths,
 * fallback key, frame length, pak names) and headless runs (SDL's dummy drivers) of our homebrew: gfx.z64's picture,
 * the same ROM through "fd:N" (the app shell's documents) and byte-swapped (.v64), saves on disk read back and left
 * alone while unchanged, the app shell's entry, and the errors.
 *
 *   n64_runtime_test [HOMEBREW_DIR]     (the runs skip, the helpers still run, when gfx.z64 is absent) */
#include "host_pad.h"
#include "host_window.h"
#include "host_time.h"
#include "n64_core.h"
#include "n64_runtime.h"

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

/* The display options (XBA-181): the runtime's own keys win over the shared ones, which win over its defaults;
 * every key accepts both filters and the three scales. */
static void test_display(void)
{
    n64_runtime_opts o;
    int scale = -1, filter = -1;

    n64_runtime_opts_init(&o);
    unsetenv("RECOMP_N64_SCALE");
    unsetenv("RECOMP_N64_FILTER");
    unsetenv("RECOMP_SCALE");
    unsetenv("RECOMP_FILTER");
    n64_runtime_display(&o, &scale, &filter);
    CHECK(scale == HOST_SCALE_ASPECT && filter == HOST_FILTER_LINEAR);
    setenv("RECOMP_SCALE", "stretch", 1);
    setenv("RECOMP_FILTER", "nearest", 1);
    n64_runtime_display(&o, &scale, &filter);
    CHECK(scale == HOST_SCALE_STRETCH && filter == HOST_FILTER_NEAREST);
    setenv("RECOMP_N64_SCALE", "integer", 1);
    setenv("RECOMP_N64_FILTER", "linear", 1);
    n64_runtime_display(&o, &scale, &filter);
    CHECK(scale == HOST_SCALE_INTEGER && filter == HOST_FILTER_LINEAR);
    setenv("RECOMP_N64_FILTER", "nearest", 1);
    n64_runtime_display(NULL, NULL, &filter);
    CHECK(filter == HOST_FILTER_NEAREST);
    o.scale = HOST_SCALE_ASPECT;   /* the caller's choice (a player's --scale) wins over every key */
    o.filter = HOST_FILTER_LINEAR;
    n64_runtime_display(&o, &scale, &filter);
    CHECK(scale == HOST_SCALE_ASPECT && filter == HOST_FILTER_LINEAR);
    unsetenv("RECOMP_N64_SCALE");
    unsetenv("RECOMP_N64_FILTER");
    unsetenv("RECOMP_SCALE");
    unsetenv("RECOMP_FILTER");
}

static void test_helpers(void)
{
    char path[256], key[32];
    int8_t x, y;

    /* Xbox layout onto the N64 controller. */
    CHECK(n64_runtime_pad_buttons(HOST_PAD_A, 0, 0, 0, 0) == N64_BTN_A);
    CHECK(n64_runtime_pad_buttons(HOST_PAD_B, 0, 0, 0, 0) == N64_BTN_B);
    CHECK(n64_runtime_pad_buttons(HOST_PAD_X, 0, 0, 0, 0) == N64_BTN_B);
    CHECK(n64_runtime_pad_buttons(HOST_PAD_Y, 0, 0, 0, 0) == N64_BTN_CUP);
    CHECK(n64_runtime_pad_buttons(HOST_PAD_LSHOULDER | HOST_PAD_RSHOULDER, 0, 0, 0, 0) == (N64_BTN_L | N64_BTN_R));
    CHECK(n64_runtime_pad_buttons(0, 0, 0, 30000, 0) == N64_BTN_Z);
    CHECK(n64_runtime_pad_buttons(0, 0, 0, 0, 30000) == N64_BTN_Z);
    CHECK(n64_runtime_pad_buttons(0, 0, 0, 4000, 4000) == 0);
    CHECK(n64_runtime_pad_buttons(HOST_PAD_START | HOST_PAD_BACK, 0, 0, 0, 0) == N64_BTN_START);
    CHECK(n64_runtime_pad_buttons(HOST_PAD_DPAD_UP | HOST_PAD_DPAD_LEFT, 0, 0, 0, 0) == (N64_BTN_DUP | N64_BTN_DLEFT));
    CHECK(n64_runtime_pad_buttons(HOST_PAD_DPAD_DOWN | HOST_PAD_DPAD_RIGHT, 0, 0, 0, 0)
          == (N64_BTN_DDOWN | N64_BTN_DRIGHT));
    CHECK(n64_runtime_pad_buttons(HOST_PAD_DPAD_UP | HOST_PAD_DPAD_DOWN, 0, 0, 0, 0) == 0); /* opposites cancel */
    /* The right stick: the C buttons past half its travel (+y down on SDL = C-down). */
    CHECK(n64_runtime_pad_buttons(0, 0, -30000, 0, 0) == N64_BTN_CUP);
    CHECK(n64_runtime_pad_buttons(0, 0, 30000, 0, 0) == N64_BTN_CDOWN);
    CHECK(n64_runtime_pad_buttons(0, -30000, 0, 0, 0) == N64_BTN_CLEFT);
    CHECK(n64_runtime_pad_buttons(0, 30000, 0, 0, 0) == N64_BTN_CRIGHT);
    CHECK(n64_runtime_pad_buttons(0, 10000, -10000, 0, 0) == 0);
    CHECK(n64_runtime_pad_buttons(HOST_PAD_Y, 0, 30000, 0, 0) == 0); /* C-up and C-down at once */

    /* The left stick: dead zone, range, +y up, the octagon's diagonals. */
    n64_runtime_stick(0, 0, 0, -1, &x, &y);
    CHECK(x == 0 && y == 0);
    n64_runtime_stick(2000, -2000, 0, -1, &x, &y); /* inside 8 % */
    CHECK(x == 0 && y == 0);
    n64_runtime_stick(32767, 0, 0, -1, &x, &y);
    CHECK(x == 80 && y == 0);
    n64_runtime_stick(-32768, 0, 0, -1, &x, &y);
    CHECK(x == -80 && y == 0);
    n64_runtime_stick(0, -32768, 0, -1, &x, &y);
    CHECK(x == 0 && y == 80);
    n64_runtime_stick(0, 32767, 0, -1, &x, &y);
    CHECK(x == 0 && y == -80);
    n64_runtime_stick(23170, -23170, 0, -1, &x, &y); /* full diagonal, up-right */
    CHECK(x >= 64 && x <= 72 && y >= 64 && y <= 72);
    n64_runtime_stick(32767, 32767, 0, -1, &x, &y); /* a square-gated pad's corner: clamped to the range */
    CHECK(x <= 80 && x >= 64 && y >= -80 && y <= -64);
    n64_runtime_stick(16384, 0, 0, 0, &x, &y); /* half travel, no dead zone: half the range */
    CHECK(x == 40 && y == 0);
    n64_runtime_stick(32767, 0, 100, 0, &x, &y);
    CHECK(x == 100);
    n64_runtime_stick(32767, 0, 200, 0, &x, &y);
    CHECK(x == 127);

    /* XRGB to RGBA. */
    uint32_t px[2] = {0x00112233u, 0x00FF8000u};
    uint8_t rgba[8];
    n64_runtime_xrgb_to_rgba(px, rgba, 2);
    CHECK(rgba[0] == 0x11 && rgba[1] == 0x22 && rgba[2] == 0x33 && rgba[3] == 255);
    CHECK(rgba[4] == 0xFF && rgba[5] == 0x80 && rgba[6] == 0x00 && rgba[7] == 255);

    /* Save paths. */
    CHECK(n64_runtime_save_path("/s", "NSME-635A2BFF", N64_SAVE_EEPROM4K, path, sizeof path) == 0
          && !strcmp(path, "/s/n64-NSME-635A2BFF/game.eep"));
    CHECK(n64_runtime_save_path("/s", "K", N64_SAVE_EEPROM16K, path, sizeof path) == 0
          && !strcmp(path, "/s/n64-K/game.eep"));
    CHECK(n64_runtime_save_path("/s", "K", N64_SAVE_SRAM, path, sizeof path) == 0 && !strcmp(path, "/s/n64-K/game.sra"));
    CHECK(n64_runtime_save_path("/s", "K", N64_SAVE_FLASH, path, sizeof path) == 0 && !strcmp(path, "/s/n64-K/game.fla"));
    CHECK(n64_runtime_save_path("/s", "K", N64_SAVE_PAK0, path, sizeof path) == 0 && !strcmp(path, "/s/n64-K/pak1.mpk"));
    CHECK(n64_runtime_save_path("/s", "K", N64_SAVE_PAK3, path, sizeof path) == 0 && !strcmp(path, "/s/n64-K/pak4.mpk"));
    CHECK(n64_runtime_save_path("/s", "K", N64_SAVE_NONE, path, sizeof path) == -1);
    CHECK(n64_runtime_save_path(NULL, "K", N64_SAVE_SRAM, path, sizeof path) == -1);
    CHECK(n64_runtime_save_path("/s", "", N64_SAVE_SRAM, path, sizeof path) == -1);
    CHECK(n64_runtime_save_path("/s", "K", N64_SAVE_SRAM, path, 8) == -1);

    /* The fallback key: recomp_pack_rom_identify's formula, from each byte order. */
    uint8_t h[0x40] = {0x80, 0x37, 0x12, 0x40}, v[0x40];
    h[0x10] = 0x63; h[0x11] = 0x5A; h[0x12] = 0x2B; h[0x13] = 0xFF;
    memcpy(h + 0x3B, "NSME", 4);
    for (int order = 0; order < 3; order++) {
        for (size_t i = 0; i < sizeof h; i++)
            v[i] = order == 0 ? h[i] : order == 1 ? h[i ^ 1] : h[(i & ~(size_t)3) + 3 - (i & 3)];
        CHECK(n64_runtime_fallback_key(v, sizeof v, key, sizeof key) == 0 && !strcmp(key, "NSME-635A2BFF"));
    }
    h[0x3C] = 0;
    CHECK(n64_runtime_fallback_key(h, sizeof h, key, sizeof key) == 0 && !strcmp(key, "N_ME-635A2BFF"));
    CHECK(n64_runtime_fallback_key(h, 0x20, key, sizeof key) == -1);
    h[0] = 0x12;
    CHECK(n64_runtime_fallback_key(h, sizeof h, key, sizeof key) == -1);

    /* Frame length from the cycles run: NTSC / PAL, clamped. */
    CHECK(n64_runtime_frame_ns(93750000ull / 60) / 1000 == 16666);
    CHECK(n64_runtime_frame_ns(93750000ull / 50) / 1000 == 20000);
    CHECK(n64_runtime_frame_ns(0) == 1000000000ull / 75);
    CHECK(n64_runtime_frame_ns(93750000ull) == 1000000000ull / 45);

    /* Pak names. */
    CHECK(n64_runtime_pak_parse("controller") == N64_RT_PAK_CONTROLLER);
    CHECK(n64_runtime_pak_parse("Rumble") == N64_RT_PAK_RUMBLE);
    CHECK(n64_runtime_pak_parse("none") == N64_RT_PAK_NONE);
    CHECK(n64_runtime_pak_parse("transfer") == N64_RT_PAK_DEFAULT);
    CHECK(n64_runtime_pak_parse(NULL) == N64_RT_PAK_DEFAULT);

    /* Frame skip: parse, auto (late by more than half a frame or audio low, 3 in a row at most), fixed, off. */
    CHECK(n64_runtime_frameskip_parse("auto") == N64_RT_FRAMESKIP_AUTO);
    CHECK(n64_runtime_frameskip_parse(NULL) == N64_RT_FRAMESKIP_AUTO);
    CHECK(n64_runtime_frameskip_parse("0") == 0 && n64_runtime_frameskip_parse("2") == 2);
    {
        const uint64_t f = 16666666;
        int run = 0, n = 0;
        CHECK(n64_runtime_frameskip(N64_RT_FRAMESKIP_AUTO, 0, f, 0, &run) == 0);
        CHECK(n64_runtime_frameskip(N64_RT_FRAMESKIP_AUTO, (int64_t)f / 4, f, 0, &run) == 0);
        for (int i = 0; i < 8; i++)
            n += n64_runtime_frameskip(N64_RT_FRAMESKIP_AUTO, (int64_t)f, f, 0, &run);
        CHECK(n == 6); /* skip 3, show 1, skip 3, show 1 */
        run = 0;
        CHECK(n64_runtime_frameskip(N64_RT_FRAMESKIP_AUTO, -5000000, f, 1, &run) == 1); /* audio about to run dry */
        CHECK(n64_runtime_frameskip(N64_RT_FRAMESKIP_AUTO, 0, f, 0, &run) == 0 && run == 0);
        n = 0;
        for (int i = 0; i < 9; i++)
            n += n64_runtime_frameskip(2, 0, f, 0, &run);
        CHECK(n == 6); /* fixed 2: two of every three */
        run = 0;
        CHECK(n64_runtime_frameskip(0, (int64_t)f * 10, f, 1, &run) == 0);
    }
}

static long file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static int ppm_size(const char *path, int *w, int *h)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    int ok = fscanf(f, "P6 %d %d 255", w, h) == 2;
    fclose(f);
    return ok ? 0 : -1;
}

/* The picture of gfx.z64 (consoles/n64/tests/homebrew/gfx): a red fill rectangle top left, black elsewhere there. */
static int ppm_pixel(const char *path, int x, int y, uint8_t rgb[3])
{
    FILE *f = fopen(path, "rb");
    int w, h;
    if (!f)
        return -1;
    int ok = fscanf(f, "P6 %d %d 255", &w, &h) == 2 && fgetc(f) != EOF && x < w && y < h;
    ok = ok && fseek(f, (long)(y * w + x) * 3, SEEK_CUR) == 0 && fread(rgb, 1, 3, f) == 3;
    fclose(f);
    return ok ? 0 : -1;
}

static void test_runs(const char *hb)
{
    char dir[] = "/tmp/n64_runtime_test.XXXXXX", rom[600], v64[400], shot[300], eep[400], pak[400], spec[32],
         cmd[3000];
    uint8_t rgb[3];
    int w = 0, h = 0;

    snprintf(rom, sizeof rom, "%s/gfx.z64", hb);
    if (access(rom, R_OK) != 0) {
        printf("runs skipped: no %s (tools/consoles/build_homebrew.sh n64)\n", rom);
        return;
    }
    if (!mkdtemp(dir)) {
        CHECK(0);
        return;
    }
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("SDL_AUDIODRIVER", "dummy", 1);
    CHECK(SDL_Init(SDL_INIT_EVENTS) == 0);

    /* 1: gfx.z64: its 320-wide picture, the red rectangle at (40, 30); no save file made for a save never written. */
    n64_runtime_opts o;
    n64_runtime_opts_init(&o);
    o.rom = rom;
    o.save_dir = dir;
    o.title_key = "TEST-00000001";
    o.frames = 120;
    o.unpaced = 1;
    snprintf(shot, sizeof shot, "%s/last.ppm", dir);
    o.screenshot = shot;
    CHECK(n64_runtime_run(&o) == 0);
    CHECK(ppm_size(shot, &w, &h) == 0 && w == 320 && h >= 224 && h <= 240);
    CHECK(ppm_pixel(shot, 40, 30, rgb) == 0 && rgb[0] > 200 && rgb[1] < 40 && rgb[2] < 40);
    CHECK(n64_runtime_save_path(dir, "TEST-00000001", N64_SAVE_PAK0, pak, sizeof pak) == 0);
    CHECK(file_size(pak) == -1);
    CHECK(n64_runtime_save_path(dir, "TEST-00000001", N64_SAVE_EEPROM4K, eep, sizeof eep) == 0);
    CHECK(file_size(eep) == -1);

    /* 2: saves on disk (an EEPROM and a controller pak) are loaded and, unchanged, not written again (same bytes, no
     * .tmp left); two RDP threads, a rumble pak, two players. Through "fd:N" with the descriptor's offset elsewhere:
     * read from 0, left open. */
    snprintf(cmd, sizeof cmd, "mkdir -p '%s/n64-TEST-00000001' && head -c 512 /dev/urandom > '%s' && "
                              "head -c 32768 /dev/urandom > '%s' && cp '%s' '%s.ref' && cp '%s' '%s.ref'",
             dir, eep, pak, eep, eep, pak, pak);
    CHECK(system(cmd) == 0);
    int fd = open(rom, O_RDONLY);
    CHECK(fd >= 0 && lseek(fd, 77, SEEK_SET) == 77);
    snprintf(spec, sizeof spec, "fd:%d", fd);
    o.rom = spec;
    o.rdp_threads = 2;
    o.pak = N64_RT_PAK_RUMBLE;
    o.players = 2;
    o.frames = 60;
    CHECK(n64_runtime_run(&o) == 0);
    CHECK(fcntl(fd, F_GETFD) != -1);
    close(fd);
    CHECK(ppm_pixel(shot, 40, 30, rgb) == 0 && rgb[0] > 200 && rgb[1] < 40 && rgb[2] < 40);
    snprintf(cmd, sizeof cmd, "cmp -s '%s' '%s.ref' && cmp -s '%s' '%s.ref'", eep, eep, pak, pak);
    CHECK(system(cmd) == 0);
    char tmp[420];
    snprintf(tmp, sizeof tmp, "%s.tmp", eep);
    CHECK(file_size(tmp) == -1);

    /* 3: the same ROM byte-swapped (.v64): the same picture. */
    snprintf(v64, sizeof v64, "%s/gfx.v64", dir);
    snprintf(cmd, sizeof cmd, "dd if='%s' of='%s' conv=swab status=none", rom, v64);
    CHECK(system(cmd) == 0);
    n64_runtime_opts_init(&o);
    o.rom = v64;
    o.frames = 120;
    o.unpaced = 1;
    o.screenshot = shot;
    CHECK(n64_runtime_run(&o) == 0);
    CHECK(ppm_size(shot, &w, &h) == 0 && w == 320);
    CHECK(ppm_pixel(shot, 40, 30, rgb) == 0 && rgb[0] > 200 && rgb[1] < 40 && rgb[2] < 40);

    /* 4: errors: no ROM, a missing file, a bad fd, a file that is no ROM. */
    n64_runtime_opts_init(&o);
    CHECK(n64_runtime_run(&o) == -1);
    o.rom = "/nonexistent/x.z64";
    CHECK(n64_runtime_run(&o) == -1);
    o.rom = "fd:-1";
    CHECK(n64_runtime_run(&o) == -1);
    o.rom = shot;
    CHECK(n64_runtime_run(&o) == -1);

    /* 5: the app shell's entry, bounded by RECOMP_N64_FRAMES, saves in n64_runtime_set_save_dir's dir (those of 2
     * are read there, and left alone); a pack's library is ignored. */
    n64_runtime_set_save_dir(dir);
    setenv("RECOMP_N64_FRAMES", "3", 1);
    CHECK(n64_runtime_start(rom, "libtitle.so", "TEST-00000001") == 0);
    unsetenv("RECOMP_N64_FRAMES");
    n64_runtime_set_save_dir(NULL);
    snprintf(cmd, sizeof cmd, "cmp -s '%s' '%s.ref' && cmp -s '%s' '%s.ref'", eep, eep, pak, pak);
    CHECK(system(cmd) == 0);
    SDL_Quit();

    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0)
        fprintf(stderr, "note: could not remove %s\n", dir);
}

int main(int argc, char **argv)
{
    const char *v = n64_runtime_version();

    if (!v || strncmp(v, "n64_runtime ", 12) != 0) {
        fprintf(stderr, "FAIL: version \"%s\"\n", v ? v : "(null)");
        return 1;
    }
    if (xbox_HostNowNs() == 0) { /* the host layer is linked in */
        fprintf(stderr, "FAIL: no host clock\n");
        return 1;
    }
    test_helpers();
    test_display();
    if (argc >= 2)
        test_runs(argv[1]);
    else
        printf("runs skipped: no homebrew directory argument\n");
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("%s: ok\n", v);
    return 0;
}
