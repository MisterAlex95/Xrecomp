/* Internal resolution policy (nv2a_gl_scale.c): which surfaces scale, the
 * scale that sticks to a scaled target's memory, the rectangle rounding,
 * visibility counts and RECOMP_GPU_SCALE_TARGETS parsing. */
#include "nv2a_gl_scale.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int s_fail;

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); s_fail++; } } while (0)
#define FEQ(a, b) (fabsf((a) - (b)) < 1e-6f)

/* Fable's back buffer: 0x80A7E000, pitch 1280, X1R5G5B5, zeta Z24S8. */
static Nv2aSurface back_buffer(uint32_t format)
{
    Nv2aSurface s;
    uint32_t aa = (format >> 12) & 0xF;

    memset(&s, 0, sizeof s);
    s.color_va = 0x80A7E000u;
    s.pitch = 1280;
    s.bytes_per_pixel = 2;
    s.format = format;
    s.aa_sx = s.aa_sy = aa == 2 ? 2 : 1;
    s.width = 640;                      /* real pixels whatever the AA mode */
    s.height = 480;
    s.color_valid = 1;
    return s;
}

static void test_policy(void)
{
    Nv2aGlScaleConfig cfg;
    Nv2aSurface s;

    memset(&cfg, 0, sizeof cfg);
    cfg.scale = 2.0f;

    /* 0x121 (640x480) and 0x2121 (AA 2x2: 320x240 logical, the same real
     * pixels) are the same target: both scale. */
    s = back_buffer(0x121);
    CHECK(nv2a_gl_frame_surface(&s));
    CHECK(FEQ(nv2a_gl_surface_scale(&cfg, &s), 2.0f));
    s = back_buffer(0x2121);
    CHECK(nv2a_gl_frame_surface(&s));
    CHECK(FEQ(nv2a_gl_surface_scale(&cfg, &s), 2.0f));
    /* A title drawing 640x480 logical with AA 2x2 into 1280x960. */
    s = back_buffer(0x2121);
    s.width = 1280;
    s.height = 960;
    s.pitch = 2560;
    CHECK(nv2a_gl_frame_surface(&s));
    /* A clip rectangle inside the frame (x, y origin) still sizes it. */
    s = back_buffer(0x121);
    s.clip_x = 40;
    s.width = 600;
    s.clip_y = 30;
    s.height = 450;
    CHECK(FEQ(nv2a_gl_surface_scale(&cfg, &s), 2.0f));
    /* Smaller: a 320x240 clip without AA is not the frame. */
    s = back_buffer(0x121);
    s.width = 320;
    s.height = 240;
    CHECK(!nv2a_gl_frame_surface(&s));
    CHECK(FEQ(nv2a_gl_surface_scale(&cfg, &s), 1.0f));

    /* Swizzled 256x256 effect target (bloom): 1. */
    memset(&s, 0, sizeof s);
    s.color_va = 0x80CF9080u;
    s.bytes_per_pixel = 4;
    s.format = 0x08080205u;             /* swizzled, log2 8 x 8, A8R8G8B8 */
    s.width = s.height = 256;
    s.aa_sx = s.aa_sy = 1;
    s.color_valid = 1;
    CHECK(!nv2a_gl_frame_surface(&s));
    CHECK(FEQ(nv2a_gl_surface_scale(&cfg, &s), 1.0f));
    /* Even a swizzled 640x480-sized clip is not the frame. */
    s.width = 640;
    s.height = 480;
    CHECK(!nv2a_gl_frame_surface(&s));

    /* Linear 256x256 (the scene copy at 0x80D38080, pitch 1024): 1. */
    memset(&s, 0, sizeof s);
    s.color_va = 0x80D38080u;
    s.pitch = 1024;
    s.bytes_per_pixel = 4;
    s.format = 0x105;
    s.width = s.height = 256;
    s.aa_sx = s.aa_sy = 1;
    s.color_valid = 1;
    CHECK(FEQ(nv2a_gl_surface_scale(&cfg, &s), 1.0f));
    /* ... unless RECOMP_GPU_SCALE_TARGETS lists it. */
    CHECK(nv2a_gl_scale_parse_targets("256x256", &cfg) == 1);
    CHECK(FEQ(nv2a_gl_surface_scale(&cfg, &s), 2.0f));
    cfg.ntargets = 0;

    /* Depth-only (the 768x768 shadow map at offset 0): 1 unless shadow. */
    memset(&s, 0, sizeof s);
    s.width = s.height = 768;
    s.format = 0x20;
    s.aa_sx = s.aa_sy = 1;
    s.color_valid = 0;
    CHECK(FEQ(nv2a_gl_surface_scale(&cfg, &s), 1.0f));
    cfg.shadow = 1;
    CHECK(FEQ(nv2a_gl_surface_scale(&cfg, &s), 2.0f));
    cfg.shadow = 0;

    /* A masked batch on the back buffer keeps the back buffer's scale. */
    s = back_buffer(0x121);
    s.color_masked = 1;
    CHECK(FEQ(nv2a_gl_surface_scale(&cfg, &s), 2.0f));

    /* Scale 1: everything 1. */
    cfg.scale = 1.0f;
    s = back_buffer(0x121);
    CHECK(FEQ(nv2a_gl_surface_scale(&cfg, &s), 1.0f));
}

/* Burnout 3's back buffer: 0x80714000, pitch 2560, A8R8G8B8 (0x128). */
static Nv2aSurface bo3_back_buffer(uint32_t cx, uint32_t cy, uint32_t w, uint32_t h)
{
    Nv2aSurface s;

    memset(&s, 0, sizeof s);
    s.color_va = 0x80714000u;
    s.pitch = 2560;
    s.bytes_per_pixel = 4;
    s.format = 0x128;
    s.aa_sx = s.aa_sy = 1;
    s.clip_x = cx;
    s.clip_y = cy;
    s.width = w;
    s.height = h;
    s.color_valid = 1;
    return s;
}

static void test_sticky(void)
{
    Nv2aGlScaleConfig cfg;
    Nv2aGlHeldTarget held = { 2.0f, 2560, 0 }, native = { 1.0f, 2560, 0 };
    Nv2aSurface s;

    memset(&cfg, 0, sizeof cfg);
    cfg.scale = 2.0f;

    /* The full-frame bind scales, with or without a target held. */
    s = bo3_back_buffer(0, 0, 640, 480);
    CHECK(FEQ(nv2a_gl_target_scale(&cfg, &s, NULL), 2.0f));
    CHECK(FEQ(nv2a_gl_target_scale(&cfg, &s, &held), 2.0f));
    /* The HUD clip 0,16 640x448 on the scaled target stays scaled ... */
    s = bo3_back_buffer(0, 16, 640, 448);
    CHECK(FEQ(nv2a_gl_surface_scale(&cfg, &s), 1.0f));    /* the policy alone */
    CHECK(FEQ(nv2a_gl_target_scale(&cfg, &s, &held), 2.0f));
    /* ... and so does a menu's scissor, 160,16 480x447. */
    s = bo3_back_buffer(160, 16, 480, 447);
    CHECK(FEQ(nv2a_gl_target_scale(&cfg, &s, &held), 2.0f));
    /* Bound first with a sub-rectangle: native, nothing to stick to. */
    s = bo3_back_buffer(0, 16, 640, 448);
    CHECK(FEQ(nv2a_gl_target_scale(&cfg, &s, NULL), 1.0f));
    CHECK(FEQ(nv2a_gl_target_scale(&cfg, &s, &native), 1.0f));
    /* Then the full frame: promoted (never down) ... */
    s = bo3_back_buffer(0, 0, 640, 480);
    CHECK(FEQ(nv2a_gl_target_scale(&cfg, &s, &native), 2.0f));
    /* ... and the sub-rectangle stays at 2 from then on. */
    s = bo3_back_buffer(0, 16, 640, 448);
    CHECK(FEQ(nv2a_gl_target_scale(&cfg, &s, &held), 2.0f));
    /* Other scales stick as they are. */
    cfg.scale = 2.25f;
    held.sc = 2.25f;
    CHECK(FEQ(nv2a_gl_target_scale(&cfg, &s, &held), 2.25f));
    cfg.scale = 2.0f;
    held.sc = 2.0f;

    /* Another pitch on that memory: a different surface, the policy. */
    s = bo3_back_buffer(0, 0, 320, 240);
    s.pitch = 1280;
    CHECK(FEQ(nv2a_gl_target_scale(&cfg, &s, &held), 1.0f));
    /* Swizzled on that memory: an effect target, the policy. */
    s = bo3_back_buffer(0, 0, 256, 256);
    s.format = 0x08080208u;
    CHECK(FEQ(nv2a_gl_target_scale(&cfg, &s, &held), 1.0f));
    /* Held swizzled (last bound as an effect target): the policy. */
    {
        Nv2aGlHeldTarget swz = { 2.0f, 2560, 1 };
        s = bo3_back_buffer(0, 16, 640, 448);
        CHECK(FEQ(nv2a_gl_target_scale(&cfg, &s, &swz), 1.0f));
    }

    /* Fable's 0x121 / 0x2121 back buffer: 2 either way, as before. */
    {
        Nv2aGlHeldTarget fable = { 2.0f, 1280, 0 };
        s = back_buffer(0x121);
        CHECK(FEQ(nv2a_gl_target_scale(&cfg, &s, NULL), 2.0f));
        CHECK(FEQ(nv2a_gl_target_scale(&cfg, &s, &fable), 2.0f));
        s = back_buffer(0x2121);
        CHECK(FEQ(nv2a_gl_target_scale(&cfg, &s, NULL), 2.0f));
        CHECK(FEQ(nv2a_gl_target_scale(&cfg, &s, &fable), 2.0f));
    }

    /* Depth-only (no colour surface): the policy, held or not. */
    memset(&s, 0, sizeof s);
    s.width = s.height = 768;
    s.format = 0x20;
    s.aa_sx = s.aa_sy = 1;
    CHECK(FEQ(nv2a_gl_target_scale(&cfg, &s, &held), 1.0f));
    cfg.shadow = 1;
    CHECK(FEQ(nv2a_gl_target_scale(&cfg, &s, &held), 2.0f));
    cfg.shadow = 0;

    /* A masked batch on the HUD clip keeps the target's scale. */
    s = bo3_back_buffer(0, 16, 640, 448);
    s.color_masked = 1;
    CHECK(FEQ(nv2a_gl_target_scale(&cfg, &s, &held), 2.0f));

    /* RECOMP_GPU_SCALE_STICKY=0: the policy alone. */
    cfg.no_sticky = 1;
    CHECK(FEQ(nv2a_gl_target_scale(&cfg, &s, &held), 1.0f));
    cfg.no_sticky = 0;

    /* Scale 1: everything 1. */
    cfg.scale = 1.0f;
    s = bo3_back_buffer(0, 0, 640, 480);
    CHECK(FEQ(nv2a_gl_target_scale(&cfg, &s, &native), 1.0f));
}

static void test_rects(void)
{
    int32_t c[4];

    CHECK(nv2a_gl_scale_px(640, 2.25f) == 1440);
    CHECK(nv2a_gl_scale_px(480, 2.25f) == 1080);
    CHECK(nv2a_gl_scale_px(640, 1.0f) == 640);
    CHECK(nv2a_gl_scale_px(1, 2.25f) == 2);       /* 2.25 -> 2 */
    CHECK(nv2a_gl_scale_px(3, 2.25f) == 7);       /* 6.75 -> 7 */

    /* Scissor rounding at 2.25: both edges rounded, so the size is not
     * round(w * 2.25) but what keeps adjacent rectangles adjacent. */
    nv2a_gl_scale_rect(1, 1, 1, 1, 2.25f, c);     /* 2.25..4.5 */
    CHECK(c[0] == 2 && c[1] == 2 && c[2] == 3 && c[3] == 3);  /* 2..5 (4.5 rounds up) */
    nv2a_gl_scale_rect(0, 0, 1, 1, 2.25f, c);     /* 0..2.25 */
    CHECK(c[0] == 0 && c[2] == 2);
    nv2a_gl_scale_rect(2, 0, 1, 1, 2.25f, c);     /* 4.5..6.75 */
    CHECK(c[0] == 5 && c[2] == 2);
    nv2a_gl_scale_rect(0, 0, 640, 480, 2.25f, c);
    CHECK(c[0] == 0 && c[1] == 0 && c[2] == 1440 && c[3] == 1080);
    nv2a_gl_scale_rect(0, 0, 320, 240, 2.25f, c); /* the AA 2x2 logical clip */
    CHECK(c[2] == 720 && c[3] == 540);
    {
        /* Adjacent rectangles tile without gap or overlap. */
        int32_t a[4], b[4];
        uint32_t x;
        for (x = 0; x < 64; x++) {
            nv2a_gl_scale_rect(x, 0, 1, 1, 2.25f, a);
            nv2a_gl_scale_rect(x + 1, 0, 1, 1, 2.25f, b);
            CHECK(a[0] + a[2] == b[0]);
        }
    }
    nv2a_gl_scale_rect(13, 7, 100, 50, 1.0f, c);
    CHECK(c[0] == 13 && c[1] == 7 && c[2] == 100 && c[3] == 50);
    nv2a_gl_scale_rect(13, 7, 100, 50, 2.0f, c);
    CHECK(c[0] == 26 && c[1] == 14 && c[2] == 200 && c[3] == 100);
}

static void test_counts(void)
{
    CHECK(nv2a_gl_scale_count(0, 2.0f) == 0);
    CHECK(nv2a_gl_scale_count(1, 2.0f) == 1);     /* visible stays visible */
    CHECK(nv2a_gl_scale_count(3, 2.0f) == 1);
    CHECK(nv2a_gl_scale_count(400, 2.0f) == 100);
    CHECK(nv2a_gl_scale_count(507, 2.25f) == 100); /* 507 / 5.0625 = 100.1 */
    CHECK(nv2a_gl_scale_count(12345, 1.0f) == 12345);
}

static void test_parse(void)
{
    Nv2aGlScaleConfig cfg;

    memset(&cfg, 0, sizeof cfg);
    CHECK(nv2a_gl_scale_parse_targets("512x512,bad,1024X768,0x5", &cfg) == 2);
    CHECK(cfg.target_w[0] == 512 && cfg.target_h[0] == 512);
    CHECK(cfg.target_w[1] == 1024 && cfg.target_h[1] == 768);
    CHECK(nv2a_gl_scale_parse_targets(NULL, &cfg) == 0);
}

/* Anti-aliased presented surfaces (XBA-119): which ones the window cannot
 * read through guest memory, and the factors their host frame is resolved
 * by. */
static void test_aa_present(void)
{
    Nv2aPresentSurface p;
    uint32_t fx, fy;

    memset(&p, 0, sizeof p);
    p.bpp = 2; p.pitch = 1280; p.w = 640; p.h = 480;       /* Fable 0x121 */
    p.format = 0x121;
    CHECK(!nv2a_gl_present_needs_host(&p));
    nv2a_gl_aa_factors(&p, &fx, &fy);
    CHECK(fx == 1 && fy == 1);
    p.format = 0x2121;                                     /* its AA 2x2 binding: 640 real */
    nv2a_gl_aa_factors(&p, &fx, &fy);
    CHECK(fx == 1 && fy == 1);
    p.bpp = 4; p.pitch = 2560; p.y = 16; p.h = 448;        /* Burnout 3's HUD clip */
    CHECK(!nv2a_gl_present_needs_host(&p));
    nv2a_gl_aa_factors(&p, &fx, &fy);
    CHECK(fx == 1 && fy == 1);
    p.format = 0x0108;
    p.pitch = 5120; p.y = 0; p.w = 1280; p.h = 960;        /* 1280x960, no AA mode */
    CHECK(nv2a_gl_present_needs_host(&p));
    nv2a_gl_aa_factors(&p, &fx, &fy);
    CHECK(fx == 1 && fy == 1);
    p.format = 0x2108;                                     /* AA 2x2, 32-bit */
    nv2a_gl_aa_factors(&p, &fx, &fy);
    CHECK(fx == 2 && fy == 2);
    p.format = 0x1103;                                     /* AA 2x1, 16-bit */
    p.bpp = 2; p.pitch = 2560; p.h = 480;
    CHECK(nv2a_gl_present_needs_host(&p));
    nv2a_gl_aa_factors(&p, &fx, &fy);
    CHECK(fx == 2 && fy == 1);
}

int main(void)
{
    test_aa_present();
    test_policy();
    test_sticky();
    test_rects();
    test_counts();
    test_parse();
    if (s_fail) {
        fprintf(stderr, "nv2a_gl_scale: %d failure(s)\n", s_fail);
        return 1;
    }
    printf("nv2a_gl_scale: all checks passed\n");
    return 0;
}
