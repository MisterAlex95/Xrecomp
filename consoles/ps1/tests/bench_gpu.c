/* PS1 GPU micro benchmark (XBA-165): Mpixels/s of gouraud and textured gouraud triangles (4 / 8 / 15 bpp, dithered,
 * opaque and semi-transparent), and frames/s of a busy 320x240 scene: 2000 mixed primitives of 4..31 pixels, about
 * 0.5 Mpixels, around what the PS1 GPU's own fill rate allows in a 60 Hz frame.
 *   ps1_gpu_bench [seconds per case]   (default 1) */
#define _POSIX_C_SOURCE 199309L
#include "gpu_scene.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* Triangles of 128x128 / 2 = 8128 pixels (top-left rule: 128 + 127 + ... + 1 - 128). */
static void tri_case(ps1_gpu *g, const char *name, uint32_t op, int depth, double secs)
{
    const double px_per_tri = 128.0 * 129.0 / 2.0 - 128.0;
    uint32_t tp = TPAGE(10 + depth, 0, 1, depth), clut = CLUT(0, 480 + (depth == 1));
    long n = 0;
    double t0 = now(), t;
    do {
        for (int i = 0; i < 64; i++) {
            int x = (i * 37) % 180, y = (i * 53) % 100;
            if (op & 4)
                GP0(g, op << 24 | 0x806040, XY(x, y), UV(0, 0, (int)clut), 0x4080C0, XY(x + 128, y),
                    UV(63, 0, (int)tp), 0xC08040, XY(x, y + 128), UV(0, 63, 0));
            else
                GP0(g, op << 24 | 0x806040, XY(x, y), 0x4080C0, XY(x + 128, y), 0xC08040, XY(x, y + 128));
        }
        n += 64;
        t = now() - t0;
    } while (t < secs);
    printf("%-34s %7.1f Mpixels/s\n", name, n * px_per_tri / t / 1e6);
}

int main(int argc, char **argv)
{
    double secs = argc > 1 ? atof(argv[1]) : 1.0;
    ps1_gpu *g = ps1_gpu_create((ps1_irq_sink){ 0 });
    gpu_open(g);
    scene_textures(g);
    GP0(g, 0xE1000200);
    tri_case(g, "gouraud (untextured, dithered)", 0x30, 2, secs);
    tri_case(g, "textured gouraud 4 bpp", 0x34, 0, secs);
    tri_case(g, "textured gouraud 8 bpp", 0x34, 1, secs);
    tri_case(g, "textured gouraud 15 bpp", 0x34, 2, secs);
    tri_case(g, "textured gouraud 4 bpp semi", 0x36, 0, secs);

    long frames = 0;
    double t0 = now(), t;
    do {
        scene_busy(g, 2000, 777);
        frames++;
        t = now() - t0;
    } while (t < secs * 2);
    uint32_t h = fnv1a16(ps1_gpu_vram(g), 0, 0, 320, 240);
    printf("busy scene (2000 primitives)       %7.0f frames/s (%.1fx 60 Hz), %.2f ms/frame [hash %08X]\n",
           frames / t, frames / t / 60.0, 1e3 * t / frames, h);
    ps1_gpu_destroy(g);
    return 0;
}
