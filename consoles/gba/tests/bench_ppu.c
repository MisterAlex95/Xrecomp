/* GBA PPU micro benchmark (XBA-154): lines per second on the busy mode 0 scene (4 text BGs, 128 sprites, alpha).
 *   gba_ppu_bench [seconds]   (default 2) */
#define _POSIX_C_SOURCE 200809L
#include "ppu_scene.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
    static scene s;
    static uint16_t fb[160 * 240];
    double secs = argc > 1 ? atof(argv[1]) : 2.0;

    scene_busy(&s);
    long frames = 0;
    double t0 = now(), t;
    do {
        for (int line = 0; line < 160; line++)
            gba_ppu_render_line(&s.in, line, fb + line * 240);
        frames++;
        t = now() - t0;
    } while (t < secs);
    uint32_t h = fnv1a(fb, sizeof fb, 2166136261u);
    double lps = frames * 160.0 / t;
    printf("busy mode 0: %.0f lines/s, %.0f frames/s (%.1fx real time), %.2f us/line [hash %08X]\n", lps,
           frames / t, frames / t / 59.73, 1e6 / lps, h);
    return 0;
}
