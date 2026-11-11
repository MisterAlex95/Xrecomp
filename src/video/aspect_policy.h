/**
 * Aspect modes of the presented picture: which frames the window shows at
 * 4:3 and which at 16:9.
 *
 * The console always renders 640x480. A title can still make wide frames:
 * it widens its camera and draws the 3D scene anamorphically, squeezed into
 * 640x480, and the frame is right once stretched to 16:9. Its menus, HUD
 * layers drawn alone and full-screen movies stay 4:3. So the class is chosen
 * per frame:
 *
 *   ASPECT_NORMAL  4:3, as the console shows it (side bars on a wide screen).
 *   ASPECT_WIDE    the whole frame stretched to 16:9: an anamorphic 3D frame.
 *   ASPECT_ZOOM    rows 60..419 (at 480 lines; scaled with the frame height)
 *                  shown at 16:9: a letterboxed 16:9 movie inside a 4:3
 *                  frame, its black bars cropped.
 *
 * Rules (aspect_policy_frame):
 *   - setting 4:3: NORMAL always.
 *   - the title declared wide frames (xbox_FramebufferWindowSetWide(1)):
 *     WIDE when the frame drew at least min_tris triangles
 *     (RECOMP_ASPECT_MIN_TRIS, default 5000), else NORMAL; a change takes
 *     two consecutive frames (one odd frame does not flash the other shape).
 *     A 3D scene draws 10^5 triangles and more, a menu or a movie hundreds.
 *   - setting 16:9 and the title never declared wide: every frame WIDE (a
 *     plain stretch).
 *   - auto and no declaration: NORMAL.
 *   - ZOOM (RECOMP_ASPECT_ZOOM, default 1): only when the policy is wide
 *     and the frame is NORMAL by the rules above. A frame is letterboxed when
 *     every 4th row of rows 0..55 and 424..479 (scaled), every 8th pixel, has
 *     all channels <= 24. ZOOM after 6 consecutive letterboxed frames, back
 *     at the first frame that is not. The crop starts at the first row
 *     holding picture within rows 56..64 (movies start at 61), else at 60.
 *
 * Pure C, no SDL and no globals but the setting's cache (xbox_AspectSetting
 * lives here, so every configuration has it): tests/aspect_policy.
 */
#ifndef XBOXRECOMP_ASPECT_POLICY_H
#define XBOXRECOMP_ASPECT_POLICY_H

#include <stdint.h>

/* RECOMP_ASPECT (xbox_AspectSetting) */
#define ASPECT_SETTING_AUTO 0
#define ASPECT_SETTING_4_3  1
#define ASPECT_SETTING_16_9 2

/* Frame classes */
#define ASPECT_NORMAL 0
#define ASPECT_WIDE   1
#define ASPECT_ZOOM   2

#define ASPECT_MIN_TRIS_DEFAULT 5000u
#define ASPECT_HYSTERESIS       2     /* frames a triangle-count change needs */
#define ASPECT_ZOOM_ENTER       6     /* letterboxed frames before ZOOM */

/* Pixel layouts the letterbox scan reads. */
#define ASPECT_PX_32    0   /* 4 bytes, three colour channels and X in any order */
#define ASPECT_PX_565   1   /* little-endian R5G6B5 */
#define ASPECT_PX_555   2   /* little-endian X1R5G5B5 */

/* "auto" (or NULL, empty, unknown) 0, "4:3" 1, "16:9" 2. */
int aspect_parse_setting(const char *s);

typedef struct AspectPolicy {
    int      setting;       /* ASPECT_SETTING_* */
    int      title_wide;    /* the title declared wide 3D frames */
    uint32_t min_tris;
    int      zoom_on;       /* RECOMP_ASPECT_ZOOM */
    int      tri_class;     /* NORMAL / WIDE after hysteresis */
    int      tri_run;       /* frames the other class has been seen */
    int      lb_run;        /* consecutive letterboxed frames */
    int      zoom;          /* ZOOM now */
    uint32_t zoom_y0;       /* crop origin, frame rows */
    uint32_t zoom_h;        /* crop height, frame rows */
} AspectPolicy;

void aspect_policy_init(AspectPolicy *p, int setting, uint32_t min_tris, int zoom_on);
/* From RECOMP_ASPECT (xbox_AspectSetting), RECOMP_ASPECT_MIN_TRIS and
 * RECOMP_ASPECT_ZOOM. */
void aspect_policy_init_from_env(AspectPolicy *p);
void aspect_policy_set_title_wide(AspectPolicy *p, int on);
/* 1 when some frames may be shown at 16:9 (the window's 16:9 frames). */
int  aspect_policy_wide(const AspectPolicy *p);

/* A frame: px may be NULL (no letterbox scan: never ZOOM). Returns its
 * class; for ZOOM, p->zoom_y0 / p->zoom_h are the rows to show. */
int  aspect_policy_frame(AspectPolicy *p, uint32_t tris, const uint8_t *px,
                         uint32_t w, uint32_t h, uint32_t pitch, int layout);

/* Letterbox scan of one frame: the crop origin row (frame rows) when it is
 * letterboxed, -1 when it is not. */
int  aspect_letterbox_top(const uint8_t *px, uint32_t w, uint32_t h, uint32_t pitch,
                          int layout);
/* Rows a ZOOM crop shows, for a frame h rows high (360 at 480). */
uint32_t aspect_zoom_height(uint32_t h);

/* Width of a picture h rows high shown at 16:9, rounded: 1080 -> 1920,
 * 480 -> 853, 720 -> 1280. Screenshots are resampled to it. */
uint32_t aspect_wide_width(uint32_t h);
/* Size of the screenshot of a w x h frame of class cls (zoom_h: the crop's
 * rows for ZOOM). */
void aspect_shot_size(int cls, uint32_t w, uint32_t h, uint32_t zoom_h,
                      uint32_t *out_w, uint32_t *out_h);

const char *aspect_class_name(int cls);

#endif
