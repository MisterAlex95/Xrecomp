/**
 * Aspect modes of the presented picture (aspect_policy.h).
 */
#include "aspect_policy.h"
#include "video_player.h"

#include <stdlib.h>
#include <string.h>

/* Black for the letterbox scan: video black (16) with some noise. */
#define LB_MAX 24u

int aspect_parse_setting(const char *s)
{
    if (!s)
        return ASPECT_SETTING_AUTO;
    if (!strcmp(s, "4:3") || !strcmp(s, "4/3") || !strcmp(s, "43"))
        return ASPECT_SETTING_4_3;
    if (!strcmp(s, "16:9") || !strcmp(s, "16/9") || !strcmp(s, "169"))
        return ASPECT_SETTING_16_9;
    return ASPECT_SETTING_AUTO;
}

/* RECOMP_ASPECT: 0 auto (default), 1 4:3, 2 16:9. Read at each call: the
 * Android shell sets the environment from recomp.cfg before the guest. */
int xbox_AspectSetting(void)
{
    return aspect_parse_setting(getenv("RECOMP_ASPECT"));
}

void aspect_policy_init(AspectPolicy *p, int setting, uint32_t min_tris, int zoom_on)
{
    memset(p, 0, sizeof *p);
    p->setting = setting;
    p->min_tris = min_tris;
    p->zoom_on = zoom_on;
    p->tri_class = ASPECT_NORMAL;
}

void aspect_policy_init_from_env(AspectPolicy *p)
{
    const char *e = getenv("RECOMP_ASPECT_MIN_TRIS");
    uint32_t min_tris = ASPECT_MIN_TRIS_DEFAULT;
    int zoom_on = 1;

    if (e && *e) {
        char *end;
        unsigned long v = strtoul(e, &end, 10);
        if (end != e && v <= 0xFFFFFFFFul)
            min_tris = (uint32_t)v;
    }
    e = getenv("RECOMP_ASPECT_ZOOM");
    if (e && *e)
        zoom_on = *e != '0';
    aspect_policy_init(p, xbox_AspectSetting(), min_tris, zoom_on);
}

void aspect_policy_set_title_wide(AspectPolicy *p, int on)
{
    p->title_wide = on != 0;
}

int aspect_policy_wide(const AspectPolicy *p)
{
    return p->setting != ASPECT_SETTING_4_3
        && (p->title_wide || p->setting == ASPECT_SETTING_16_9);
}

/* Largest colour channel of a pixel, 0..255. */
static uint32_t px_max(const uint8_t *q, int layout)
{
    uint32_t v, r, g, b;

    if (layout == ASPECT_PX_32) {
        r = q[0]; g = q[1]; b = q[2];
    } else {
        v = (uint32_t)q[0] | (uint32_t)q[1] << 8;
        if (layout == ASPECT_PX_555) {
            r = (v >> 10 & 0x1F) * 255u / 31u;
            g = (v >> 5 & 0x1F) * 255u / 31u;
        } else {
            r = (v >> 11 & 0x1F) * 255u / 31u;
            g = (v >> 5 & 0x3F) * 255u / 63u;
        }
        b = (v & 0x1F) * 255u / 31u;
    }
    v = r > g ? r : g;
    return v > b ? v : b;
}

/* 1 when the sampled pixels of row y are all black. */
static int row_black(const uint8_t *px, uint32_t w, uint32_t pitch, int layout,
                     uint32_t y, uint32_t xstep)
{
    uint32_t bypp = layout == ASPECT_PX_32 ? 4u : 2u, x;
    const uint8_t *row = px + (size_t)y * pitch;

    for (x = 0; x < w; x += xstep)
        if (px_max(row + (size_t)x * bypp, layout) > LB_MAX)
            return 0;
    return 1;
}

/* Row r of the 480-line frame, in a frame h rows high. */
static uint32_t scale_row(uint32_t r, uint32_t h)
{
    return (uint32_t)((uint64_t)r * h / 480u);
}

uint32_t aspect_zoom_height(uint32_t h)
{
    return scale_row(360, h);
}

int aspect_letterbox_top(const uint8_t *px, uint32_t w, uint32_t h, uint32_t pitch,
                         int layout)
{
    uint32_t ystep, xstep, y, top_end, bot_start, first_lo, first_hi;

    if (!px || w < 8 || h < 48)
        return -1;
    ystep = scale_row(4, h);
    if (!ystep)
        ystep = 1;
    xstep = (uint32_t)((uint64_t)8 * w / 640u);
    if (!xstep)
        xstep = 1;
    top_end = scale_row(56, h);
    bot_start = scale_row(424, h);
    for (y = 0; y < top_end; y += ystep)
        if (!row_black(px, w, pitch, layout, y, xstep))
            return -1;
    for (y = bot_start; y < h; y += ystep)
        if (!row_black(px, w, pitch, layout, y, xstep))
            return -1;
    /* Letterboxed: where the picture starts, if it starts where a 16:9
     * movie does. */
    first_lo = top_end;
    first_hi = scale_row(64, h);
    for (y = first_lo; y <= first_hi && y < h; y++)
        if (!row_black(px, w, pitch, layout, y, xstep))
            return (int)y;
    return (int)scale_row(60, h);
}

int aspect_policy_frame(AspectPolicy *p, uint32_t tris, const uint8_t *px,
                        uint32_t w, uint32_t h, uint32_t pitch, int layout)
{
    int cls, top;

    if (!aspect_policy_wide(p)) {
        p->lb_run = 0;
        p->zoom = 0;
        return ASPECT_NORMAL;
    }
    if (p->title_wide) {
        int raw = tris >= p->min_tris ? ASPECT_WIDE : ASPECT_NORMAL;
        if (raw == p->tri_class)
            p->tri_run = 0;
        else if (++p->tri_run >= ASPECT_HYSTERESIS) {
            p->tri_class = raw;
            p->tri_run = 0;
        }
        cls = p->tri_class;
    } else
        cls = ASPECT_WIDE;          /* 16:9 for a title that is not wide */

    if (cls != ASPECT_NORMAL || !p->zoom_on) {
        p->lb_run = 0;
        p->zoom = 0;
        return cls;
    }
    top = aspect_letterbox_top(px, w, h, pitch, layout);
    if (top < 0) {
        p->lb_run = 0;
        p->zoom = 0;
        return ASPECT_NORMAL;
    }
    if (p->lb_run < ASPECT_ZOOM_ENTER)
        p->lb_run++;
    if (p->lb_run < ASPECT_ZOOM_ENTER)
        return ASPECT_NORMAL;
    p->zoom = 1;
    p->zoom_y0 = (uint32_t)top;
    p->zoom_h = aspect_zoom_height(h);
    if (p->zoom_y0 + p->zoom_h > h)
        p->zoom_y0 = h - p->zoom_h;
    return ASPECT_ZOOM;
}

uint32_t aspect_wide_width(uint32_t h)
{
    return (uint32_t)(((uint64_t)h * 16u * 2u + 9u) / 18u);
}

void aspect_shot_size(int cls, uint32_t w, uint32_t h, uint32_t zoom_h,
                      uint32_t *out_w, uint32_t *out_h)
{
    if (cls == ASPECT_WIDE) {
        *out_w = aspect_wide_width(h);
        *out_h = h;
    } else if (cls == ASPECT_ZOOM && zoom_h && zoom_h <= h) {
        *out_w = aspect_wide_width(zoom_h);
        *out_h = zoom_h;
    } else {
        *out_w = w;
        *out_h = h;
    }
}

const char *aspect_class_name(int cls)
{
    return cls == ASPECT_WIDE ? "WIDE" : cls == ASPECT_ZOOM ? "ZOOM" : "NORMAL";
}
