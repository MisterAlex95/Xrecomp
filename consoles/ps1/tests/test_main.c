/* The PlayStation runtime (XBA-146, XBA-168): its pure helpers (pad mapping, sticks, colour conversion, card path,
 * EXE magic, fallback key, frame length) and headless runs (SDL's dummy drivers) of our homebrew through OpenBIOS:
 * an EXE fast-booted, the same through "fd:N" (the app shell's documents), a disc booted by the BIOS from its .cue,
 * memory cards read back and left alone while unchanged, the app shell's entry, and the errors.
 *
 *   ps1_runtime_test [BIOS HOMEBREW_DIR]     (the runs skip, the helpers still run, when either is absent) */
#include "host_pad.h"
#include "host_time.h"
#include "ps1_core.h"
#include "ps1_devices.h"
#include "ps1_runtime.h"
#include "recomp_lifecycle.h"

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
