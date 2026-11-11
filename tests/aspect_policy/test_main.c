/* tests/aspect_policy: the presenter's aspect modes (src/video/aspect_policy.h):
 * the setting, per-frame classes from triangle counts with their hysteresis,
 * the letterbox scan and ZOOM on synthetic frames (32-bit, R5G6B5, X1R5G5B5,
 * 640x480 and 1440x1080), and the screenshot sizes. */
#define _POSIX_C_SOURCE 200809L   /* setenv */
#include "aspect_policy.h"
#include "video_player.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_fail;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); s_fail++; } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { fprintf(stderr, "%s:%d: %s = %lld, expected %lld\n", __FILE__, __LINE__, #a, _a, _b); s_fail++; } } while (0)

/* ── Synthetic frames ───────────────────────────────────────────────── */

/* w x h, bypp bytes per pixel, every pixel `fill` (a 32-bit pixel's bytes,
 * or a 16-bit value), rows [pic0, pic1) `pic` instead. */
static uint8_t *frame(uint32_t w, uint32_t h, uint32_t bypp, uint32_t fill, uint32_t pic,
                      uint32_t pic0, uint32_t pic1)
{
    uint8_t *px = (uint8_t *)malloc((size_t)w * h * bypp);
    uint32_t x, y;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            uint32_t v = y >= pic0 && y < pic1 ? pic : fill;
            uint8_t *q = px + ((size_t)y * w + x) * bypp;
            q[0] = (uint8_t)v;
            q[1] = (uint8_t)(v >> 8);
            if (bypp == 4) {
                q[2] = (uint8_t)(v >> 16);
                q[3] = (uint8_t)(v >> 24);
            }
        }
    return px;
}

static void set_px(uint8_t *px, uint32_t w, uint32_t bypp, uint32_t x, uint32_t y, uint32_t v)
{
    uint8_t *q = px + ((size_t)y * w + x) * bypp;
    q[0] = (uint8_t)v;
    q[1] = (uint8_t)(v >> 8);
    if (bypp == 4) {
        q[2] = (uint8_t)(v >> 16);
        q[3] = (uint8_t)(v >> 24);
    }
}

/* ── Setting ────────────────────────────────────────────────────────── */

static void test_setting(void)
{
    CHECK_EQ(aspect_parse_setting(NULL), ASPECT_SETTING_AUTO);
    CHECK_EQ(aspect_parse_setting(""), ASPECT_SETTING_AUTO);
    CHECK_EQ(aspect_parse_setting("auto"), ASPECT_SETTING_AUTO);
    CHECK_EQ(aspect_parse_setting("bogus"), ASPECT_SETTING_AUTO);
    CHECK_EQ(aspect_parse_setting("4:3"), ASPECT_SETTING_4_3);
    CHECK_EQ(aspect_parse_setting("16:9"), ASPECT_SETTING_16_9);

    unsetenv("RECOMP_ASPECT");
    CHECK_EQ(xbox_AspectSetting(), 0);
    setenv("RECOMP_ASPECT", "4:3", 1);
    CHECK_EQ(xbox_AspectSetting(), 1);
    setenv("RECOMP_ASPECT", "16:9", 1);
    CHECK_EQ(xbox_AspectSetting(), 2);

    {
        AspectPolicy p;
        setenv("RECOMP_ASPECT_MIN_TRIS", "1234", 1);
        setenv("RECOMP_ASPECT_ZOOM", "0", 1);
        aspect_policy_init_from_env(&p);
        CHECK_EQ(p.setting, ASPECT_SETTING_16_9);
        CHECK_EQ(p.min_tris, 1234);
        CHECK_EQ(p.zoom_on, 0);
        unsetenv("RECOMP_ASPECT");
        unsetenv("RECOMP_ASPECT_MIN_TRIS");
        unsetenv("RECOMP_ASPECT_ZOOM");
        aspect_policy_init_from_env(&p);
        CHECK_EQ(p.setting, ASPECT_SETTING_AUTO);
        CHECK_EQ(p.min_tris, ASPECT_MIN_TRIS_DEFAULT);
        CHECK_EQ(p.zoom_on, 1);
    }
}

/* ── Classes from triangle counts ───────────────────────────────────── */

static void test_classes(void)
{
    AspectPolicy p;
    int i;

    /* auto, title not wide: always NORMAL. */
    aspect_policy_init(&p, ASPECT_SETTING_AUTO, 5000, 1);
    CHECK(!aspect_policy_wide(&p));
    CHECK_EQ(aspect_policy_frame(&p, 300000, NULL, 0, 0, 0, ASPECT_PX_32), ASPECT_NORMAL);

    /* 16:9, title not wide: every frame WIDE, menus too. */
    aspect_policy_init(&p, ASPECT_SETTING_16_9, 5000, 1);
    CHECK(aspect_policy_wide(&p));
    CHECK_EQ(aspect_policy_frame(&p, 2, NULL, 0, 0, 0, ASPECT_PX_32), ASPECT_WIDE);
    CHECK_EQ(aspect_policy_frame(&p, 300000, NULL, 0, 0, 0, ASPECT_PX_32), ASPECT_WIDE);

    /* 4:3: NORMAL even when the title is wide. */
    aspect_policy_init(&p, ASPECT_SETTING_4_3, 5000, 1);
    aspect_policy_set_title_wide(&p, 1);
    CHECK(!aspect_policy_wide(&p));
    for (i = 0; i < 4; i++)
        CHECK_EQ(aspect_policy_frame(&p, 300000, NULL, 0, 0, 0, ASPECT_PX_32), ASPECT_NORMAL);

    /* auto, title wide: by triangles, two frames of hysteresis. */
    aspect_policy_init(&p, ASPECT_SETTING_AUTO, 5000, 1);
    aspect_policy_set_title_wide(&p, 1);
    CHECK(aspect_policy_wide(&p));
    CHECK_EQ(aspect_policy_frame(&p, 690, NULL, 0, 0, 0, ASPECT_PX_32), ASPECT_NORMAL);
    CHECK_EQ(aspect_policy_frame(&p, 178000, NULL, 0, 0, 0, ASPECT_PX_32), ASPECT_NORMAL);
    CHECK_EQ(aspect_policy_frame(&p, 178000, NULL, 0, 0, 0, ASPECT_PX_32), ASPECT_WIDE);
    CHECK_EQ(aspect_policy_frame(&p, 375000, NULL, 0, 0, 0, ASPECT_PX_32), ASPECT_WIDE);
    /* One light frame (a transition) does not flash 4:3. */
    CHECK_EQ(aspect_policy_frame(&p, 1622, NULL, 0, 0, 0, ASPECT_PX_32), ASPECT_WIDE);
    CHECK_EQ(aspect_policy_frame(&p, 200000, NULL, 0, 0, 0, ASPECT_PX_32), ASPECT_WIDE);
    /* Two do (the pause menu). */
    CHECK_EQ(aspect_policy_frame(&p, 764, NULL, 0, 0, 0, ASPECT_PX_32), ASPECT_WIDE);
    CHECK_EQ(aspect_policy_frame(&p, 430, NULL, 0, 0, 0, ASPECT_PX_32), ASPECT_NORMAL);
    /* Threshold inclusive, and settable. */
    aspect_policy_init(&p, ASPECT_SETTING_16_9, 100, 1);
    aspect_policy_set_title_wide(&p, 1);   /* declared: by triangles, not a stretch */
    CHECK_EQ(aspect_policy_frame(&p, 99, NULL, 0, 0, 0, ASPECT_PX_32), ASPECT_NORMAL);
    CHECK_EQ(aspect_policy_frame(&p, 100, NULL, 0, 0, 0, ASPECT_PX_32), ASPECT_NORMAL);
    CHECK_EQ(aspect_policy_frame(&p, 100, NULL, 0, 0, 0, ASPECT_PX_32), ASPECT_WIDE);
    /* Withdrawn: 16:9 stretches every frame again. */
    aspect_policy_set_title_wide(&p, 0);
    CHECK_EQ(aspect_policy_frame(&p, 0, NULL, 0, 0, 0, ASPECT_PX_32), ASPECT_WIDE);
}

/* ── Letterbox scan ─────────────────────────────────────────────────── */

static void test_letterbox(void)
{
    uint8_t *f;

    /* A 16:9 movie: black bars, picture rows 61..420. */
    f = frame(640, 480, 4, 0x00101010, 0x00806040, 61, 421);
    CHECK_EQ(aspect_letterbox_top(f, 640, 480, 640 * 4, ASPECT_PX_32), 61);
    /* Bar noise within the threshold (24) still counts as black. */
    set_px(f, 640, 4, 8, 4, 0x00181818);
    CHECK_EQ(aspect_letterbox_top(f, 640, 480, 640 * 4, ASPECT_PX_32), 61);
    /* One bright sampled pixel in a bar: not letterboxed. */
    set_px(f, 640, 4, 16, 8, 0x00190000);
    CHECK_EQ(aspect_letterbox_top(f, 640, 480, 640 * 4, ASPECT_PX_32), -1);
    free(f);
    /* Bottom bar too. */
    f = frame(640, 480, 4, 0, 0x00FFFFFF, 61, 421);
    set_px(f, 640, 4, 0, 476, 0x00FFFFFF);
    CHECK_EQ(aspect_letterbox_top(f, 640, 480, 640 * 4, ASPECT_PX_32), -1);
    free(f);
    /* Picture starting outside 56..64: origin 60. */
    f = frame(640, 480, 4, 0, 0x00FFFFFF, 70, 410);
    CHECK_EQ(aspect_letterbox_top(f, 640, 480, 640 * 4, ASPECT_PX_32), 60);
    free(f);
    /* A full-frame picture (a menu): not letterboxed. */
    f = frame(640, 480, 4, 0x00303030, 0, 0, 0);
    CHECK_EQ(aspect_letterbox_top(f, 640, 480, 640 * 4, ASPECT_PX_32), -1);
    free(f);
    /* 1440x1080 (RECOMP_GPU_SCALE=2.25): rows scale; picture at 61 * 2.25. */
    f = frame(1440, 1080, 4, 0, 0x00406080, 137, 947);
    CHECK_EQ(aspect_letterbox_top(f, 1440, 1080, 1440 * 4, ASPECT_PX_32), 137);
    free(f);
    /* R5G6B5: 0x18E3 is (24, 28, 24) -> not black; 0x10A2 (16, 20, 16) is. */
    f = frame(640, 480, 2, 0x10A2, 0xFFFF, 61, 421);
    CHECK_EQ(aspect_letterbox_top(f, 640, 480, 640 * 2, ASPECT_PX_565), 61);
    set_px(f, 640, 2, 0, 0, 0x18E3);
    CHECK_EQ(aspect_letterbox_top(f, 640, 480, 640 * 2, ASPECT_PX_565), -1);
    free(f);
    /* X1R5G5B5: 0x0C63 is (24, 24, 24) after expansion (3 * 255 / 31)... */
    f = frame(640, 480, 2, 0x0C63, 0x7FFF, 61, 421);
    CHECK_EQ(aspect_letterbox_top(f, 640, 480, 640 * 2, ASPECT_PX_555), 61);
    /* ...read as R5G6B5 the same bits have green 35 (141): the layout matters. */
    CHECK_EQ(aspect_letterbox_top(f, 640, 480, 640 * 2, ASPECT_PX_565), -1);
    free(f);
    CHECK_EQ(aspect_letterbox_top(NULL, 640, 480, 0, ASPECT_PX_32), -1);
}

/* ── ZOOM ───────────────────────────────────────────────────────────── */

static void test_zoom(void)
{
    AspectPolicy p;
    uint8_t *movie = frame(640, 480, 4, 0, 0x00808080, 61, 421);
    uint8_t *menu = frame(640, 480, 4, 0x00404040, 0, 0, 0);
    uint8_t *big = frame(1440, 1080, 4, 0, 0x00808080, 137, 947);
    int i;

    aspect_policy_init(&p, ASPECT_SETTING_AUTO, 5000, 1);
    aspect_policy_set_title_wide(&p, 1);
    /* Five letterboxed frames: still NORMAL; the sixth zooms. */
    for (i = 0; i < 5; i++)
        CHECK_EQ(aspect_policy_frame(&p, 2, movie, 640, 480, 2560, ASPECT_PX_32), ASPECT_NORMAL);
    CHECK_EQ(aspect_policy_frame(&p, 2, movie, 640, 480, 2560, ASPECT_PX_32), ASPECT_ZOOM);
    CHECK_EQ(p.zoom_y0, 61);
    CHECK_EQ(p.zoom_h, 360);
    CHECK_EQ(aspect_policy_frame(&p, 2, movie, 640, 480, 2560, ASPECT_PX_32), ASPECT_ZOOM);
    /* A frame that is not letterboxed leaves at once... */
    CHECK_EQ(aspect_policy_frame(&p, 2, menu, 640, 480, 2560, ASPECT_PX_32), ASPECT_NORMAL);
    /* ...and coming back takes six frames again. */
    for (i = 0; i < 5; i++)
        CHECK_EQ(aspect_policy_frame(&p, 2, movie, 640, 480, 2560, ASPECT_PX_32), ASPECT_NORMAL);
    CHECK_EQ(aspect_policy_frame(&p, 2, movie, 640, 480, 2560, ASPECT_PX_32), ASPECT_ZOOM);
    /* A 3D scene with bars (in-engine cutscene) is WIDE, never ZOOM. */
    CHECK_EQ(aspect_policy_frame(&p, 200000, movie, 640, 480, 2560, ASPECT_PX_32), ASPECT_ZOOM);
    CHECK_EQ(aspect_policy_frame(&p, 200000, movie, 640, 480, 2560, ASPECT_PX_32), ASPECT_WIDE);
    CHECK_EQ(p.zoom, 0);
    /* Scaled frames. */
    aspect_policy_init(&p, ASPECT_SETTING_AUTO, 5000, 1);
    aspect_policy_set_title_wide(&p, 1);
    for (i = 0; i < 6; i++)
        (void)aspect_policy_frame(&p, 2, big, 1440, 1080, 5760, ASPECT_PX_32);
    CHECK(p.zoom);
    CHECK_EQ(p.zoom_y0, 137);
    CHECK_EQ(p.zoom_h, 810);
    /* RECOMP_ASPECT_ZOOM=0. */
    aspect_policy_init(&p, ASPECT_SETTING_AUTO, 5000, 0);
    aspect_policy_set_title_wide(&p, 1);
    for (i = 0; i < 8; i++)
        CHECK_EQ(aspect_policy_frame(&p, 2, movie, 640, 480, 2560, ASPECT_PX_32), ASPECT_NORMAL);
    /* Not wide (auto, no declaration; or 4:3): never ZOOM. */
    aspect_policy_init(&p, ASPECT_SETTING_AUTO, 5000, 1);
    for (i = 0; i < 8; i++)
        CHECK_EQ(aspect_policy_frame(&p, 2, movie, 640, 480, 2560, ASPECT_PX_32), ASPECT_NORMAL);
    /* 16:9 without a declaration stretches movies too (every frame WIDE). */
    aspect_policy_init(&p, ASPECT_SETTING_16_9, 5000, 1);
    for (i = 0; i < 8; i++)
        CHECK_EQ(aspect_policy_frame(&p, 2, movie, 640, 480, 2560, ASPECT_PX_32), ASPECT_WIDE);
    free(movie);
    free(menu);
    free(big);
}

/* ── Screenshot sizes ───────────────────────────────────────────────── */

static void test_shot(void)
{
    uint32_t w, h;

    CHECK_EQ(aspect_wide_width(1080), 1920);
    CHECK_EQ(aspect_wide_width(480), 853);
    CHECK_EQ(aspect_wide_width(720), 1280);
    CHECK_EQ(aspect_wide_width(360), 640);
    CHECK_EQ(aspect_wide_width(810), 1440);
    CHECK_EQ(aspect_zoom_height(480), 360);
    CHECK_EQ(aspect_zoom_height(1080), 810);

    aspect_shot_size(ASPECT_WIDE, 1440, 1080, 0, &w, &h);
    CHECK_EQ(w, 1920); CHECK_EQ(h, 1080);
    aspect_shot_size(ASPECT_WIDE, 640, 480, 0, &w, &h);
    CHECK_EQ(w, 853); CHECK_EQ(h, 480);
    aspect_shot_size(ASPECT_ZOOM, 640, 480, 360, &w, &h);
    CHECK_EQ(w, 640); CHECK_EQ(h, 360);
    aspect_shot_size(ASPECT_ZOOM, 1440, 1080, 810, &w, &h);
    CHECK_EQ(w, 1440); CHECK_EQ(h, 810);
    aspect_shot_size(ASPECT_NORMAL, 1440, 1080, 0, &w, &h);
    CHECK_EQ(w, 1440); CHECK_EQ(h, 1080);
    aspect_shot_size(ASPECT_ZOOM, 640, 480, 0, &w, &h);    /* no crop: as is */
    CHECK_EQ(w, 640); CHECK_EQ(h, 480);
    CHECK(!strcmp(aspect_class_name(ASPECT_ZOOM), "ZOOM"));
}

int main(void)
{
    test_setting();
    test_classes();
    test_letterbox();
    test_zoom();
    test_shot();
    if (s_fail) {
        fprintf(stderr, "aspect_policy: %d check(s) failed\n", s_fail);
        return 1;
    }
    printf("aspect_policy: all checks passed\n");
    return 0;
}
