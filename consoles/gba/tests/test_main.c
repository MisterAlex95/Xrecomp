/* The Game Boy Advance runtime (XBA-146, XBA-157): its pure helpers (title key, pad mapping, colour conversion, save
 * path) and headless runs of a tiny ROM made here (no window: SDL's dummy drivers), checking the save it writes, that
 * the save is read back, and the "fd:N" form the app shell uses for documents. */
#include "gba_core.h"
#include "gba_runtime.h"
#include "host_pad.h"
#include "host_window.h"
#include "host_time.h"
#include "recomp_lifecycle.h"

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

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* A 1 KB cartridge: the header's branch to 0xC0, which stores `value` at SRAM 0x0E000000 and spins. */
static void make_rom(uint8_t *rom, size_t n, uint8_t value)
{
    uint8_t sum = 0;
    memset(rom, 0, n);
    put32(rom + 0x00, 0xEA00002E);   /* b 0x080000C0 */
    memcpy(rom + 0xA0, "XRECOMPTEST ", 12);
    memcpy(rom + 0xAC, "XRGB", 4);
    memcpy(rom + 0xB0, "01", 2);
    rom[0xB2] = 0x96;
    for (int i = 0xA0; i <= 0xBC; i++)
        sum = (uint8_t)(sum - rom[i]);
    rom[0xBD] = (uint8_t)(sum - 0x19);
    put32(rom + 0xC0, 0xE3A0040E);   /* mov r0, #0x0E000000 */
    put32(rom + 0xC4, 0xE3A01000u | value); /* mov r1, #value */
    put32(rom + 0xC8, 0xE5C01000);   /* strb r1, [r0] */
    put32(rom + 0xCC, 0xEAFFFFFE);   /* b . */
}

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
    gba_runtime_opts o;
    int scale = -1, filter = -1;

    gba_runtime_opts_init(&o);
    unsetenv("RECOMP_GBA_SCALE");
    unsetenv("RECOMP_GBA_FILTER");
    unsetenv("RECOMP_SCALE");
    unsetenv("RECOMP_FILTER");
    gba_runtime_display(&o, &scale, &filter);
    CHECK(scale == HOST_SCALE_ASPECT && filter == HOST_FILTER_NEAREST);
    setenv("RECOMP_SCALE", "stretch", 1);
    setenv("RECOMP_FILTER", "linear", 1);
    gba_runtime_display(&o, &scale, &filter);
    CHECK(scale == HOST_SCALE_STRETCH && filter == HOST_FILTER_LINEAR);
    setenv("RECOMP_GBA_SCALE", "integer", 1);
    setenv("RECOMP_GBA_FILTER", "nearest", 1);
    gba_runtime_display(&o, &scale, &filter);
    CHECK(scale == HOST_SCALE_INTEGER && filter == HOST_FILTER_NEAREST);
    setenv("RECOMP_GBA_FILTER", "linear", 1);
    gba_runtime_display(NULL, NULL, &filter);
    CHECK(filter == HOST_FILTER_LINEAR);
    o.scale = HOST_SCALE_ASPECT;   /* the caller's choice (a player's --scale) wins over every key */
    o.filter = HOST_FILTER_NEAREST;
    gba_runtime_display(&o, &scale, &filter);
    CHECK(scale == HOST_SCALE_ASPECT && filter == HOST_FILTER_NEAREST);
    unsetenv("RECOMP_GBA_SCALE");
    unsetenv("RECOMP_GBA_FILTER");
    unsetenv("RECOMP_SCALE");
    unsetenv("RECOMP_FILTER");
}

static void test_helpers(void)
{
    uint8_t rom[1024];
    char key[32], want[32], path[256];

    make_rom(rom, sizeof rom, 0x5A);
    CHECK(gba_runtime_key(rom, sizeof rom, key, sizeof key) == 0);
    snprintf(want, sizeof want, "XRGB-%08X", (unsigned)crc32_ref(rom, sizeof rom));
    CHECK(!strcmp(key, want));
    rom[0xBD] ^= 1;   /* a bad header: no game code */
    CHECK(gba_runtime_key(rom, sizeof rom, key, sizeof key) == 0 && !strncmp(key, "____-", 5));
    CHECK(gba_runtime_key(rom, sizeof rom, key, 8) == -1);

    /* Xbox layout onto the GBA's keys. */
    CHECK(gba_runtime_pad_keys(HOST_PAD_A, 0, 0, 0, 0) == GBA_KEY_A);
    CHECK(gba_runtime_pad_keys(HOST_PAD_B, 0, 0, 0, 0) == GBA_KEY_B);
    CHECK(gba_runtime_pad_keys(HOST_PAD_X, 0, 0, 0, 0) == GBA_KEY_B);
    CHECK(gba_runtime_pad_keys(HOST_PAD_Y, 0, 0, 0, 0) == GBA_KEY_A);
    CHECK(gba_runtime_pad_keys(HOST_PAD_LSHOULDER | HOST_PAD_RSHOULDER, 0, 0, 0, 0) == (GBA_KEY_L | GBA_KEY_R));
    CHECK(gba_runtime_pad_keys(0, 0, 0, 30000, 0) == GBA_KEY_L);
    CHECK(gba_runtime_pad_keys(0, 0, 0, 0, 10000) == 0);
    CHECK(gba_runtime_pad_keys(HOST_PAD_START | HOST_PAD_BACK, 0, 0, 0, 0) == (GBA_KEY_START | GBA_KEY_SELECT));
    CHECK(gba_runtime_pad_keys(HOST_PAD_DPAD_UP | HOST_PAD_DPAD_LEFT, 0, 0, 0, 0) == (GBA_KEY_UP | GBA_KEY_LEFT));
    CHECK(gba_runtime_pad_keys(HOST_PAD_DPAD_DOWN | HOST_PAD_DPAD_RIGHT, 0, 0, 0, 0)
          == (GBA_KEY_DOWN | GBA_KEY_RIGHT));
    CHECK(gba_runtime_pad_keys(0, -32768, 0, 0, 0) == GBA_KEY_LEFT);
    CHECK(gba_runtime_pad_keys(0, 20000, 20000, 0, 0) == (GBA_KEY_RIGHT | GBA_KEY_DOWN));
    CHECK(gba_runtime_pad_keys(0, 5000, -5000, 0, 0) == 0);   /* inside the dead zone */
    CHECK(gba_runtime_pad_keys(HOST_PAD_DPAD_LEFT, 32767, 0, 0, 0) == 0); /* left + right: neither */
    CHECK(gba_runtime_pad_keys(HOST_PAD_GUIDE | HOST_PAD_LSTICK, 0, 0, 0, 0) == 0);

    uint16_t px[3] = {0x001F, 0x03E0, 0x7FFF};
    uint8_t out[12];
    gba_runtime_rgb555_to_rgba(px, out, 3);
    CHECK(out[0] == 255 && out[1] == 0 && out[2] == 0 && out[3] == 255);
    CHECK(out[4] == 0 && out[5] == 255 && out[6] == 0);
    CHECK(out[8] == 255 && out[9] == 255 && out[10] == 255 && out[11] == 255);

    CHECK(gba_runtime_save_path("/data/save", "AXVE-1A2B3C4D", path, sizeof path) == 0
          && !strcmp(path, "/data/save/gba-AXVE-1A2B3C4D/game.sav"));
    CHECK(gba_runtime_save_path(NULL, "K", path, sizeof path) == -1);
    CHECK(gba_runtime_save_path("/s", "K", path, 8) == -1);
    CHECK(GBA_FRAME_NS == 1000000000ull * GBA_CYCLES_PER_FRAME / 16777216ull);
}

static long file_size(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 ? (long)st.st_size : -1;
}

static int first_byte(const char *p)
{
    FILE *f = fopen(p, "rb");
    int c = f ? fgetc(f) : -1;
    if (f)
        fclose(f);
    return c;
}

static void *run_thread(void *arg)
{
    static int rc;
    rc = gba_runtime_run((const gba_runtime_opts *)arg);
    return &rc;
}

static void write_rom(const char *path, uint8_t value)
{
    uint8_t rom[1024];
    make_rom(rom, sizeof rom, value);
    FILE *f = fopen(path, "wb");
    CHECK(f && fwrite(rom, 1, sizeof rom, f) == sizeof rom);
    if (f)
        fclose(f);
}

static void test_runs(void)
{
    char dir[] = "/tmp/gba_runtime_test.XXXXXX", rom_path[300], save[512], key[32], spec[32], shot[300];
    uint8_t rom[1024];

    if (!mkdtemp(dir)) {
        CHECK(0);
        return;
    }
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("SDL_AUDIODRIVER", "dummy", 1);
    unsetenv("RECOMP_GBA_BIOS");
    CHECK(SDL_Init(SDL_INIT_EVENTS) == 0);
    make_rom(rom, sizeof rom, 0x5A);
    snprintf(rom_path, sizeof rom_path, "%s/test.gba", dir);
    FILE *f = fopen(rom_path, "wb");
    CHECK(f && fwrite(rom, 1, sizeof rom, f) == sizeof rom);
    if (f)
        fclose(f);
    gba_runtime_key(rom, sizeof rom, key, sizeof key);
    CHECK(gba_runtime_save_path(dir, key, save, sizeof save) == 0);

    /* 1: the game writes its SRAM, the save is written at exit (32 KB SRAM, first byte 0x5A). */
    gba_runtime_opts o;
    gba_runtime_opts_init(&o);
    o.rom = rom_path;
    o.save_dir = dir;
    o.frames = 5;
    o.unpaced = 1;
    snprintf(shot, sizeof shot, "%s/last.ppm", dir);
    o.screenshot = shot;
    CHECK(gba_runtime_run(&o) == 0);
    CHECK(file_size(save) == 32768);
    CHECK(first_byte(save) == 0x5A);
    CHECK(file_size(shot) == (long)(15 + GBA_SCREEN_W * GBA_SCREEN_H * 3));

    /* 2: the save is read back before the first frame; a game that writes another value updates it. Through
     * "fd:N" (the app's documents) with the descriptor's offset elsewhere: read from 0, left open. */
    make_rom(rom, sizeof rom, 0x33);
    f = fopen(rom_path, "wb");
    CHECK(f && fwrite(rom, 1, sizeof rom, f) == sizeof rom);
    if (f)
        fclose(f);
    int fd = open(rom_path, O_RDONLY);
    CHECK(fd >= 0 && lseek(fd, 77, SEEK_SET) == 77);
    snprintf(spec, sizeof spec, "fd:%d", fd);
    o.rom = spec;
    o.title_key = key;   /* the same game's key, as the shell hands it */
    o.screenshot = NULL;
    CHECK(gba_runtime_run(&o) == 0);
    CHECK(fcntl(fd, F_GETFD) != -1);
    close(fd);
    CHECK(file_size(save) == 32768 && first_byte(save) == 0x33);

    /* 2b: a paced run until quit on another thread: the in-game menu's hold (the lifecycle's MENU reason) writes the
     * save at once, from the thread that held it (Android parks the pumping thread in the background); SDL_QUIT
     * ends the run. */
    write_rom(rom_path, 0x77);
    gba_runtime_opts_init(&o);
    o.rom = rom_path;
    o.save_dir = dir;
    o.title_key = key;   /* the same save as above */
    o.headless = 1;
    pthread_t th;
    CHECK(pthread_create(&th, NULL, run_thread, &o) == 0);
    usleep(300 * 1000);
    recomp_lifecycle_pause_for(RECOMP_HOLD_MENU);
    CHECK(first_byte(save) == 0x77);
    usleep(100 * 1000);
    recomp_lifecycle_resume_for(RECOMP_HOLD_MENU);
    usleep(100 * 1000);
    SDL_Event quit;
    memset(&quit, 0, sizeof quit);
    quit.type = SDL_QUIT;
    CHECK(SDL_PushEvent(&quit) == 1);
    void *ret = NULL;
    CHECK(pthread_join(th, &ret) == 0 && ret && *(int *)ret == 0);
    write_rom(rom_path, 0x33);

    /* 3: no ROM, a missing file, a bad fd: an error, nothing written. */
    gba_runtime_opts_init(&o);
    CHECK(gba_runtime_run(&o) == -1);
    o.rom = "/nonexistent/x.gba";
    CHECK(gba_runtime_run(&o) == -1);
    o.rom = "fd:-1";
    CHECK(gba_runtime_run(&o) == -1);

    /* 4: the app shell's entry, bounded by RECOMP_GBA_FRAMES, saves in gba_runtime_set_save_dir's dir. */
    char dir2[300];
    snprintf(dir2, sizeof dir2, "%s/shell", dir);
    gba_runtime_set_save_dir(dir2);
    setenv("RECOMP_GBA_FRAMES", "3", 1);
    CHECK(gba_runtime_start(rom_path, NULL, "XRGB-TEST") == 0);
    unsetenv("RECOMP_GBA_FRAMES");
    gba_runtime_set_save_dir(NULL);
    snprintf(save, sizeof save, "%s/gba-XRGB-TEST/game.sav", dir2);
    CHECK(first_byte(save) == 0x33);
    SDL_Quit();

    char cmd[400];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0)
        fprintf(stderr, "note: could not remove %s\n", dir);
}

int main(void)
{
    const char *v = gba_runtime_version();

    CHECK(v && strncmp(v, "gba_runtime ", 12) == 0);
    CHECK(xbox_HostNowNs() != 0);   /* the host layer is linked in */
    test_helpers();
    test_display();
    test_runs();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("%s: ok\n", v);
    return 0;
}
