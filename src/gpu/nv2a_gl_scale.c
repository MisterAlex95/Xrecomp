/* Internal resolution scaling: policy and rectangle arithmetic
 * (nv2a_gl_scale.h). No GL. */
#include "nv2a_gl_scale.h"

#include <stdlib.h>
#include <string.h>

int nv2a_gl_scale_choose(const char *gpu_scale, const char *upscale, float *scale,
                         const char **key)
{
    const char *e = gpu_scale;
    char *end;
    double v;

    *scale = 1.0f;
    *key = NULL;
    if (e && *e) {
        *key = "RECOMP_GPU_SCALE";
        if (strcmp(e, "auto") == 0)
            return NV2A_GL_SCALE_AUTO;
        v = strtod(e, &end);
        if (end == e || *end || !(v >= 1.0 && v <= 4.0))
            return NV2A_GL_SCALE_BAD;
        *scale = (float)v;
        return NV2A_GL_SCALE_FIXED;
    }
    e = upscale;
    if (!e || !*e)
        return NV2A_GL_SCALE_UNSET;
    *key = "RECOMP_UPSCALE";
    v = strtod(e, &end);
    while (end != e && (*end == ' ' || *end == '\t'))
        end++;
    if (end == e || *end || !(v >= 1.0))
        return NV2A_GL_SCALE_BAD;
    *scale = v > 4.0 ? 4.0f : (float)v;
    return NV2A_GL_SCALE_FIXED;
}

uint32_t nv2a_gl_scale_parse_targets(const char *s, Nv2aGlScaleConfig *cfg)
{
    cfg->ntargets = 0;
    while (s && *s && cfg->ntargets < NV2A_GL_SCALE_MAX_TARGETS) {
        char *end;
        unsigned long w = strtoul(s, &end, 10), h = 0;
        if (end != s && (*end == 'x' || *end == 'X')) {
            const char *hs = end + 1;
            h = strtoul(hs, &end, 10);
            if (end == hs)
                h = 0;
        }
        if (w && h && w <= 4096 && h <= 4096) {
            cfg->target_w[cfg->ntargets] = (uint32_t)w;
            cfg->target_h[cfg->ntargets] = (uint32_t)h;
            cfg->ntargets++;
        }
        while (*end && *end != ',')
            end++;
        s = *end ? end + 1 : end;
    }
    return cfg->ntargets;
}

int nv2a_gl_frame_surface(const Nv2aSurface *s)
{
    uint32_t w = s->clip_x + s->width, h = s->clip_y + s->height;
    uint32_t ax = s->aa_sx ? s->aa_sx : 1, ay = s->aa_sy ? s->aa_sy : 1;

    if (((s->format >> 8) & 0xF) == 2)
        return 0;                       /* swizzled: an effect target */
    return (w == NV2A_GL_FRAME_W && h == NV2A_GL_FRAME_H)
        || (w == NV2A_GL_FRAME_W * ax && h == NV2A_GL_FRAME_H * ay);
}

float nv2a_gl_surface_scale(const Nv2aGlScaleConfig *cfg, const Nv2aSurface *s)
{
    uint32_t w, h, i;

    if (cfg->scale == 1.0f)
        return 1.0f;
    if (!s->color_valid)
        return cfg->shadow ? cfg->scale : 1.0f;
    if (nv2a_gl_frame_surface(s))
        return cfg->scale;
    w = s->clip_x + s->width;
    h = s->clip_y + s->height;
    for (i = 0; i < cfg->ntargets; i++)
        if (w == cfg->target_w[i] && h == cfg->target_h[i])
            return cfg->scale;
    return 1.0f;
}

float nv2a_gl_target_scale(const Nv2aGlScaleConfig *cfg, const Nv2aSurface *s,
                           const Nv2aGlHeldTarget *held)
{
    float sc = nv2a_gl_surface_scale(cfg, s);

    if (sc != 1.0f || cfg->scale == 1.0f || cfg->no_sticky || !held || !s->color_valid)
        return sc;
    if (((s->format >> 8) & 0xF) == 2 || held->swizzled || held->sc == 1.0f
        || held->pitch != s->pitch)
        return sc;
    return held->sc;
}

uint32_t nv2a_gl_scale_px(uint32_t v, float sc)
{
    return (uint32_t)((double)v * (double)sc + 0.5);
}

void nv2a_gl_scale_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, float sc,
                        int32_t out[4])
{
    uint32_t x0 = nv2a_gl_scale_px(x, sc), y0 = nv2a_gl_scale_px(y, sc);

    out[0] = (int32_t)x0;
    out[1] = (int32_t)y0;
    out[2] = (int32_t)(nv2a_gl_scale_px(x + w, sc) - x0);
    out[3] = (int32_t)(nv2a_gl_scale_px(y + h, sc) - y0);
}

uint32_t nv2a_gl_scale_count(uint32_t samples, float sc)
{
    double n;

    if (sc == 1.0f || !samples)
        return samples;
    n = (double)samples / ((double)sc * (double)sc) + 0.5;
    return n < 1.0 ? 1u : (uint32_t)n;
}

int nv2a_gl_present_needs_host(const Nv2aPresentSurface *s)
{
    return s->bpp && s->pitch != NV2A_GL_FRAME_W * s->bpp;
}

void nv2a_gl_aa_factors(const Nv2aPresentSurface *s, uint32_t *fx, uint32_t *fy)
{
    int aa = ((s->format >> 12) & 0xF) != 0;    /* SET_SURFACE_FORMAT anti-aliasing */

    *fx = aa && s->x + s->w == 2 * NV2A_GL_FRAME_W ? 2u : 1u;
    *fy = aa && s->y + s->h == 2 * NV2A_GL_FRAME_H ? 2u : 1u;
}
