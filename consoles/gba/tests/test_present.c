/* The screenshot of a GBA run in a real window (HOST-001): host_window_screenshot_bmp from another thread and the F12
 * screenshot from the frame thread, while the picture changes and a state is saved and loaded, then at the end of the
 * run and in a second run. The cartridge below turns the backdrop red and blue in turn, during the vertical blank:
 * every picture is one colour, the whole of one frame.
 *
 *   gba_present_test     exits 77 (skipped) without a display */
#include "gba_runtime.h"
#include "host_window.h"
#include "recomp_states.h"

#include <pthread.h>

#include <dirent.h>
#include <stdint.h>
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

static int write_rom(const char *path)
{
    static const uint32_t code[] = {
        0xE3A00301, /* mov r0, #0x04000000 */
        0xE3A01000, /* mov r1, #0 */
        0xE1C010B0, /* strh r1, [r0]: DISPCNT, mode 0, no layer: the backdrop */
        0xE3A02405, /* mov r2, #0x05000000 */
        0xE3A04000, /* mov r4, #0 */
        0xE1D030B6, /* loop: ldrh r3, [r0, #6]: VCOUNT */
        0xE35300A0, /* cmp r3, #160 */
        0x1AFFFFFC, /* bne loop */
        0xE2844001, /* add r4, r4, #1 */
        0xE3140008, /* tst r4, #8: eight frames of each colour */
        0x03A0501F, /* moveq r5, #0x001F: red */
        0x13A05B1F, /* movne r5, #0x7C00: blue */
        0xE1C250B0, /* strh r5, [r2]: the backdrop */
        0xE1D030B6, /* out: ldrh r3, [r0, #6] */
        0xE35300A0, /* cmp r3, #160 */
        0x0AFFFFFC, /* beq out */
        0xEAFFFFF3, /* b loop */
    };
    uint8_t rom[1024], sum = 0;
    uint32_t entry = 0xEA00002E; /* b 0x080000C0 */

    memset(rom, 0, sizeof rom);
    memcpy(rom, &entry, 4);
    memcpy(rom + 0xA0, "XRECOMPTEST ", 12);
    memcpy(rom + 0xAC, "XPRS", 4);
    memcpy(rom + 0xB0, "01", 2);
    rom[0xB2] = 0x96;
    for (int i = 0xA0; i <= 0xBC; i++)
        sum = (uint8_t)(sum - rom[i]);
    rom[0xBD] = (uint8_t)(sum - 0x19);
    memcpy(rom + 0xC0, code, sizeof code);
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    size_t put = fwrite(rom, 1, sizeof rom, f);
    return fclose(f) == 0 && put == sizeof rom ? 0 : -1;
}

/* What a BMP holds: 0 not one colour (or not a picture), else 'k' black, 'r' red, 'b' blue, '?' another colour. */
static int bmp_colour(const char *path)
{
    static uint8_t d[1024 * 1024];
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    size_t n = fread(d, 1, sizeof d, f);
    fclose(f);
    int32_t w = 0, h = 0;
    if (n < 54 || d[0] != 'B' || d[1] != 'M')
        return 0;
    memcpy(&w, d + 18, 4);
    memcpy(&h, d + 22, 4);
    size_t row = ((size_t)w * 3 + 3) & ~(size_t)3;
    if (w != 240 || h != 160 || n < 54 + row * (size_t)h)
        return 0;
    const uint8_t *p = d + 54;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (memcmp(d + 54 + row * (size_t)y + (size_t)x * 3, p, 3) != 0)
                return 0;
    int b = p[0], g = p[1], r = p[2];
    if (r < 60 && g < 60 && b < 60)
        return 'k';
    if (r > 150 && g < 60 && b < 60)
        return 'r';
    if (b > 150 && g < 60 && r < 60)
        return 'b';
    return '?';
}

static volatile int s_done;
static int s_rc;

static void *run_thread(void *arg)
{
    s_rc = gba_runtime_run(arg);
    __atomic_store_n(&s_done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void one_run(const char *dir, const char *rom, int nth)
{
    char bmp[400], shots[400], path[800];
    int red = 0, blue = 0, taken = 0, state_done = 0, f12 = 0;
    gba_runtime_opts o;

    snprintf(bmp, sizeof bmp, "%s/shot.bmp", dir);
    snprintf(shots, sizeof shots, "%s/f12-%d", dir, nth);
    mkdir(shots, 0755);
    setenv("RECOMP_SCREENSHOT_DIR", shots, 1);

    gba_runtime_opts_init(&o);
    o.rom = rom;
    o.title_key = "XPRS-TEST";
    o.save_dir = dir;
    o.frames = 1200;
    o.unpaced = 1;

    pthread_t th;
    s_done = 0;
    CHECK(pthread_create(&th, NULL, run_thread, &o) == 0);
    while (!__atomic_load_n(&s_done, __ATOMIC_ACQUIRE)) {
        if (host_window_screenshot_bmp(bmp) == 0) {
            int c = bmp_colour(bmp);
            taken++;
            if (c != 'k' && c != 'r' && c != 'b')
                fprintf(stderr, "picture %d: colour %d\n", taken, c);
            CHECK(c == 'k' || c == 'r' || c == 'b');
            red += c == 'r';
            blue += c == 'b';
        }
        if (taken && f12 < 5 && taken % 8 == 0) {
            host_window_request_screenshot(); /* written by the frame thread, at its next present */
            f12++;
        }
        if (!state_done && red && blue && recomp_states_available()) {
            recomp_state_meta meta;
            static uint16_t thumb[RECOMP_STATE_THUMB_W * RECOMP_STATE_THUMB_H];
            state_done = 1;
            CHECK(recomp_states_save(1) == RECOMP_STATE_OK);
            CHECK(recomp_states_load(1) == RECOMP_STATE_OK);
            CHECK(recomp_states_info(1, &meta, thumb) == RECOMP_STATE_OK);
            CHECK(meta.thumb_w > 0 && meta.thumb_h > 0);
        }
    }
    CHECK(pthread_join(th, NULL) == 0);
    CHECK(s_rc == 0);
    printf("run %d: %d screenshots (%d red, %d blue)\n", nth, taken, red, blue);
    CHECK(taken > 0 && red > 0 && blue > 0);
    CHECK(state_done);
    CHECK(host_window_screenshot_bmp(bmp) == -1); /* the picture went with the window */

    /* The F12 screenshots: each one is a whole frame too. */
    int files = 0;
    DIR *d = opendir(shots);
    CHECK(d != NULL);
    for (struct dirent *e; d && (e = readdir(d));) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(path, sizeof path, "%s/%s", shots, e->d_name);
        int c = bmp_colour(path);
        CHECK(c == 'k' || c == 'r' || c == 'b');
        files++;
    }
    if (d)
        closedir(d);
    CHECK(f12 == 0 || files > 0);
}

int main(void)
{
    char dir[] = "/tmp/gba_present_test.XXXXXX", rom[300], cmd[400];

    if (host_window_headless()) {
        printf("gba_present: no display, skipped\n");
        return 77;
    }
    setenv("SDL_AUDIODRIVER", "dummy", 1);
    if (!mkdtemp(dir))
        return 1;
    snprintf(rom, sizeof rom, "%s/colours.gba", dir);
    CHECK(write_rom(rom) == 0);
    one_run(dir, rom, 1);
    one_run(dir, rom, 2);
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0)
        fprintf(stderr, "note: could not remove %s\n", dir);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("gba_present: ok\n");
    return 0;
}
