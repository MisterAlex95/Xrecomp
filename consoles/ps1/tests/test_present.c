/* The screenshot of a PS1 run in a real window (HOST-001): host_window_screenshot_bmp from another thread and the F12
 * screenshot from the frame thread, while the game changes its display mode and a state is saved and loaded, then at
 * the end of the run and in a second run at another internal resolution. The program below alternates a red 256 x 240
 * picture and a blue 320 x 240 one: both come out 320 x 240 from the screenshot (the 4:3 shape), so the check is on
 * the pixels: every picture is one colour, the whole of one frame.
 *
 *   ps1_present_test     exits 77 (skipped) without a display */
#include "host_window.h"
#include "ps1_runtime.h"
#include "recomp_states.h"

#include <pthread.h>

#include <dirent.h>
#include <fcntl.h>
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

enum { T0 = 8, T1 = 9, T2 = 10, T3 = 11, T4 = 12, GP0 = 0x1810, GP1 = 0x1814, LOAD = 0x80010000u, TEXT = 0x800 };

static uint32_t lui(int rt, uint32_t v) { return 0x3C000000u | (uint32_t)rt << 16 | v >> 16; }
static uint32_t ori(int rt, uint32_t v) { return 0x34000000u | (uint32_t)rt << 21 | (uint32_t)rt << 16 | (v & 0xFFFF); }
static uint32_t sw(int rt, int reg) { return 0xAC000000u | (uint32_t)T0 << 21 | (uint32_t)rt << 16 | (uint32_t)reg; }

/* One phase: the display mode, the whole of VRAM filled with a colour, a few frames' wait. */
static size_t phase(uint32_t *c, size_t n, uint32_t mode, uint32_t bgr)
{
    c[n++] = lui(T1, mode);
    c[n++] = ori(T1, mode);
    c[n++] = sw(T1, GP1);
    c[n++] = lui(T2, 0x02000000u | bgr);
    c[n++] = ori(T2, bgr);
    c[n++] = sw(T2, GP0);
    c[n++] = sw(0, GP0);          /* at 0, 0 */
    c[n++] = lui(T3, 480u << 16);
    c[n++] = ori(T3, 640);
    c[n++] = sw(T3, GP0);         /* 640 x 480 */
    c[n++] = lui(T4, 0x00200000u);
    c[n++] = 0x258CFFFFu;         /* addiu t4, t4, -1 */
    c[n++] = 0x1580FFFEu;         /* bne t4, zero, the addiu */
    c[n++] = 0;
    return n;
}

static int write_exe(const char *path)
{
    static uint8_t exe[0x800 + TEXT];
    uint32_t code[64];
    size_t n = 0;

    code[n++] = lui(T0, 0x1F800000u);
    code[n++] = sw(0, GP1);       /* GP1(00h): reset */
    code[n++] = lui(T1, 0x03000000u);
    code[n++] = sw(T1, GP1);      /* GP1(03h): display on */
    size_t loop = n;
    n = phase(code, n, 0x08000000u, 0x0000FFu); /* 256 x 240, red */
    n = phase(code, n, 0x08000001u, 0xFF0000u); /* 320 x 240, blue */
    code[n++] = 0x08000000u | ((LOAD + (uint32_t)loop * 4) & 0x0FFFFFFFu) >> 2; /* j loop */
    code[n++] = 0;

    memset(exe, 0, sizeof exe);
    memcpy(exe, "PS-X EXE", 8);
    uint32_t head[] = {LOAD, 0, LOAD, TEXT};
    memcpy(exe + 0x10, head, sizeof head);
    uint32_t sp = 0x801FFF00u;
    memcpy(exe + 0x30, &sp, 4);
    memcpy(exe + 0x800, code, n * sizeof code[0]);
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    size_t put = fwrite(exe, 1, sizeof exe, f);
    return fclose(f) == 0 && put == sizeof exe ? 0 : -1;
}

/* What a BMP holds: 0 not one colour (or not a picture), else 'k' black, 'r' red, 'b' blue, '?' another colour. */
static int bmp_colour(const char *path)
{
    static uint8_t d[8 * 1024 * 1024];
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
    if (w <= 0 || h <= 0 || n < 54 + row * (size_t)h)
        return 0;
    const uint8_t *p = d + 54;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (memcmp(d + 54 + row * (size_t)y + (size_t)x * 3, p, 3) != 0)
                return 0;
    int b = p[0], g = p[1], r = p[2];
    if (r < 40 && g < 40 && b < 40)
        return 'k';
    if (r > 200 && g < 40 && b < 40)
        return 'r';
    if (b > 200 && g < 40 && r < 40)
        return 'b';
    return '?';
}

static volatile int s_done;
static int s_rc;

static void *run_thread(void *arg)
{
    s_rc = ps1_runtime_run(arg);
    __atomic_store_n(&s_done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void one_run(const char *dir, const char *exe, int upscale)
{
    char bmp[400], shots[400], path[800];
    int red = 0, blue = 0, taken = 0, state_done = 0, f12 = 0;
    ps1_runtime_opts o;

    snprintf(bmp, sizeof bmp, "%s/shot.bmp", dir);
    snprintf(shots, sizeof shots, "%s/f12-%d", dir, upscale);
    mkdir(shots, 0755);
    setenv("RECOMP_SCREENSHOT_DIR", shots, 1);

    ps1_runtime_opts_init(&o);
    o.game = exe;
    o.bios = "hle";
    o.title_key = "TEST-00002";
    o.save_dir = dir;
    o.frames = 900;
    o.unpaced = 1;
    o.upscale = upscale;

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
            int same = 1;
            for (int i = 0; i < meta.thumb_w * meta.thumb_h; i++)
                same &= thumb[i] == thumb[0];
            CHECK(same); /* the thumbnail is one frame too */
        }
    }
    CHECK(pthread_join(th, NULL) == 0);
    CHECK(s_rc == 0);
    printf("upscale %d: %d screenshots (%d red, %d blue)\n", upscale, taken, red, blue);
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
    char dir[] = "/tmp/ps1_present_test.XXXXXX", exe[300], cmd[400];

    if (host_window_headless()) {
        printf("ps1_present: no display, skipped\n");
        return 77;
    }
    setenv("SDL_AUDIODRIVER", "dummy", 1);
    if (!mkdtemp(dir))
        return 1;
    snprintf(exe, sizeof exe, "%s/modes.exe", dir);
    CHECK(write_exe(exe) == 0);
    one_run(dir, exe, 1);
    one_run(dir, exe, 2); /* a new run, its buffers another size */
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0)
        fprintf(stderr, "note: could not remove %s\n", dir);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("ps1_present: ok\n");
    return 0;
}
