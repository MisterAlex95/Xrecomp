/* N64 RDP benchmark (XBA-173): Mpixels/s per mode on 320x240 RGBA16 (fill, copy, 1-cycle flat / shaded / textured,
 * bilinear + Z, 2-cycle fog + blend + Z), and frames/s of the busy scene of the tests (1500 mixed primitives, about
 * 0.9 Mpixels: far more than a typical libultra frame) with 1, 2 and 4 threads, and the time per frame of that scene
 * streamed under emulation work (the renderer threads drawing while the commands come).
 *   n64_rdp_bench [seconds per case [case name substring]]   (default 1, every case) */
#define _POSIX_C_SOURCE 200809L
#include "rdp_cmd.h"

#include <stdio.h>
#include <time.h>

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

static rdp_list lst;
static const char *only; /* run only the cases whose name contains it */
static int wanted(const char *name) { return !only || strstr(name, only); }

static void setup(rdp_list *l)
{
    l->n = 0;
    L(l, c_color_image(F_RGBA, SZ16, 320, FB_ADDR));
    L(l, c_z_image(ZB_ADDR));
    L(l, c_scissor(0, 0, 320 * 4, 240 * 4));
    L(l, c_tex_image(F_RGBA, SZ16, 32, TEX_ADDR));
    L(l, c_set_tile(F_RGBA, SZ16, 8, 0, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0));
    L(l, c_load_tile(7, 0, 0, 31 * 4, 31 * 4));
    L(l, c_set_tile(F_RGBA, SZ16, 8, 0, 0, 0, 0, 0, 5, 0, 0, 0, 5, 0));
    L(l, c_tile_size(0, 0, 0, 31 * 4, 31 * 4));
    L(l, c_fog(0x8090A0FF));
    L(l, c_prim(0, 0, 0xC08040FF));
    /* Z cleared to the far plane each run, so Z cases draw rather than reject */
    L(l, c_color_image(F_RGBA, SZ16, 320, ZB_ADDR));
    L(l, c_modes(OM_FILL));
    L(l, c_fill_color(0xFFFCFFFC));
    L(l, c_fill_rect(0, 0, 319 * 4, 239 * 4));
    L(l, c_color_image(F_RGBA, SZ16, 320, FB_ADDR));
}

/* Runs the list (after setup) repeatedly; px = pixels drawn per run. */
static void run_case(rdp_rig *g, const char *name, double px, double secs)
{
    if (!wanted(name))
        return;
    L(&lst, c_sync_full());
    long n = 0;
    double t0 = now(), t;
    do {
        rig_run(g, &lst);
        n++;
        t = now() - t0;
    } while (t < secs);
    printf("%-44s %8.1f Mpixels/s\n", name, n * px / t / 1e6);
}

/* 32 triangles of 120x120 / 2 pixels. */
static double tris(int shade, int tex, int z)
{
    for (int i = 0; i < 32; i++) {
        float x = (float)(i * 37 % 190), y = (float)(i * 53 % 110), zz = (float)(0x7000 - i * 500); /* nearer each time: Z passes */
        vtx a = { x, y, 255, 80, 40, 255, 0, 0, 1, zz }, b = { x + 120, y, 40, 255, 80, 200, 64, 0, 1, zz + 100 },
            c = { x, y + 120, 80, 40, 255, 128, 0, 64, 1, zz + 200 };
        c_triangle(&lst, shade, tex, z, 0, 0, &a, &b, &c);
    }
    return 32 * 120.0 * 121.0 / 2.0;
}

int main(int argc, char **argv)
{
    double secs = argc > 1 ? atof(argv[1]) : 1.0;
    only = argc > 2 ? argv[2] : NULL;
    rdp_rig g;
    rig_open(&g);
    scene_textures(g.ram);

    setup(&lst);
    L(&lst, c_modes(OM_FILL));
    L(&lst, c_fill_color(0x12341234));
    for (int i = 0; i < 16; i++)
        L(&lst, c_fill_rect(0, 0, 319 * 4, 239 * 4));
    run_case(&g, "fill rectangles (16-bit)", 16 * 320.0 * 240.0, secs);

    setup(&lst);
    L(&lst, c_modes(OM_COPY));
    for (int i = 0; i < 64; i++)
        c_texrect(&lst, 0, 0, (i * 37 % 288) * 4, (i * 53 % 208) * 4, (i * 37 % 288 + 31) * 4, (i * 53 % 208 + 31) * 4, 0,
                  0, 4 << 10, 1 << 10);
    run_case(&g, "copy blits 32x32", 64 * 1024.0, secs);

    setup(&lst);
    L(&lst, c_modes(OM_STD));
    L(&lst, CC_PASS(CC_PRIM, AC_PRIM));
    run_case(&g, "1-cycle flat triangles (prim colour)", tris(0, 0, 0), secs);

    setup(&lst);
    L(&lst, c_modes(OM_STD));
    L(&lst, CC_PASS(CC_SHADE, AC_SHADE));
    run_case(&g, "1-cycle gouraud triangles", tris(1, 0, 0), secs);

    setup(&lst);
    L(&lst, c_modes(OM_STD));
    L(&lst, c_combine1(CC_TEX0, CC_ZERO_A, CC_SHADE, CC_ZERO_ADD, AC_ZERO, AC_ZERO, AC_ZERO, AC_SHADE));
    run_case(&g, "1-cycle texture * shade, point", tris(1, 1, 0), secs);

    setup(&lst);
    L(&lst, c_modes(OM_STD | OM_BILERP | OM_Z_COMPARE | OM_Z_UPDATE | OM_PERSP));
    L(&lst, c_combine1(CC_TEX0, CC_ZERO_A, CC_SHADE, CC_ZERO_ADD, AC_ZERO, AC_ZERO, AC_ZERO, AC_SHADE));
    run_case(&g, "1-cycle texture * shade, bilinear, persp, Z", tris(1, 1, 1), secs);

    setup(&lst);
    L(&lst, c_modes(OM_2CYC | OM_TF0_RGB | OM_TF1_RGB | OM_RGB_DITHER(0) | OM_ALPHA_DITHER(3) | OM_BILERP |
                    OM_IMAGE_READ | OM_FORCE_BLEND | OM_Z_COMPARE | OM_Z_UPDATE | OM_BLEND0(0, 2, 3, 0) |
                    OM_BLEND1(0, 0, 1, 0)));
    L(&lst, c_combine(CC_TEX0, CC_ZERO_A, CC_SHADE, CC_ZERO_ADD, AC_ZERO, AC_ZERO, AC_ZERO, AC_SHADE, CC_ZERO_A,
                      CC_ZERO_A, CC_ZERO_MUL, CC_COMBINED, AC_ZERO, AC_ZERO, AC_ZERO, AC_COMBINED));
    run_case(&g, "2-cycle tex * shade, fog, blend, dither, Z", tris(1, 1, 1), secs);

    static const int nth[] = { 1, 2, 4 };
    for (int i = 0; i < 3; i++) {
        int th = nth[i];
        char name[64];
        snprintf(name, sizeof name, "busy 320x240 scene, 1500 primitives, %d thread%s", th, th > 1 ? "s" : " ");
        if (!wanted(name))
            continue;
        n64_rdp_set_threads(g.dp, th);
        lst.n = 0;
        scene_busy(&lst, 1500, 777);
        long frames = 0;
        double t0 = now(), t;
        do {
            rig_run(&g, &lst);
            frames++;
            t = now() - t0;
        } while (t < secs * 2);
        printf("%s %6.1f frames/s\n", name, frames / t);
    }

    /* Streamed: the busy scene reaches the RDP in 64-word pieces with emulation work between them (a spin of 15 ms in
     * all per frame, as the CPU and the RSP would take to make the list), then SYNC_FULL: the time per frame on the
     * emulation thread. Drawn as the commands come, the RDP's work hides under the emulation's. */
    for (int i = 0; i < 3; i++) {
        int th = nth[i];
        char name[80];
        snprintf(name, sizeof name, "busy scene streamed under 15 ms of emulation, %d thread%s", th, th > 1 ? "s" : " ");
        if (!wanted(name))
            continue;
        n64_rdp_set_threads(g.dp, th);
        lst.n = 0;
        scene_busy(&lst, 1500, 777);
        for (int k = 0; k < lst.n; k++) {
            n64_wbe32(g.ram + CMD_BASE + k * 8, (uint32_t)(lst.w[k] >> 32));
            n64_wbe32(g.ram + CMD_BASE + k * 8 + 4, (uint32_t)lst.w[k]);
        }
        const int pieces = (lst.n + 63) / 64;
        const double work = 0.015 / pieces;
        long frames = 0;
        double t0 = now(), t;
        do {
            n64_rdp_write_reg(g.dp, 0, CMD_BASE);
            for (int p = 0; p < pieces; p++) {
                const int end = (p + 1) * 64 < lst.n ? (p + 1) * 64 : lst.n;
                n64_rdp_write_reg(g.dp, 1, CMD_BASE + (uint32_t)end * 8);
                n64_rdp_run(g.dp);
                for (double s = now(); now() - s < work;) {
                }
            }
            n64_rdp_flush(g.dp);
            frames++;
            t = now() - t0;
        } while (t < secs * 2);
        printf("%s %6.2f ms/frame\n", name, t * 1e3 / frames);
    }
    rig_close(&g);
    return 0;
}
