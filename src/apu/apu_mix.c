/*
 * apu_mix.c -- the mixbin downmix and the native I3DL2-style reverb that
 * stand in for the GP's effects image (XBA-122). apu_mix.h has the bin map.
 *
 * This project's own code (MIT, see LICENSE). The reverb is a textbook
 * feedback delay network (Jot and Chaigne, 1991; Householder feedback,
 * per-line gain from the decay time, a one-pole low-pass per line for the
 * HF decay ratio) driven by the I3DL2 listener parameters. It is not the
 * reverb of DirectSound's standard image and does not claim to sound like
 * it: docs/technical/apu-dsp.md lists what differs.
 */
#include "apu_mix.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ── Configuration ─────────────────────────────────────────────────────── */

static int s_mode = -1;
static int s_reverb = -1;           /* -1 unread, 0 off, 1 on */
static ApuI3dl2 s_reverb_params;
static const char *s_reverb_name;

void apu_mix_reset_config(void)
{
    s_mode = -1;
    s_reverb = -1;
    s_reverb_name = NULL;
}

int apu_mix_mode(void)
{
    if (s_mode < 0) {
        const char *m = getenv("RECOMP_APU_MIXDOWN");
        const char *all = getenv("RECOMP_APU_MIXDOWN_ALL");
        if (m && !strcmp(m, "two"))
            s_mode = APU_MIXDOWN_TWO;
        else if (m && !strcmp(m, "sum"))
            s_mode = APU_MIXDOWN_SUM;
        else if (m && !strcmp(m, "routed"))
            s_mode = APU_MIXDOWN_ROUTED;
        else if (all && *all && atoi(all) == 0)
            s_mode = APU_MIXDOWN_TWO;   /* the switch that predates this one */
        else
            s_mode = APU_MIXDOWN_ROUTED;
        if (m && *m && strcmp(m, "two") && strcmp(m, "sum") && strcmp(m, "routed"))
            fprintf(stderr, "[APU-MIX] RECOMP_APU_MIXDOWN=%s unknown (two|sum|routed):"
                            " routed\n", m);
    }
    return s_mode;
}

/* The routed mode's coefficients.
 *
 * The 5.1 bed is folded with ITU-R BS.775's stereo downmix: front at unity,
 * centre and back at -3 dB (0.7071) into their side(s). ITU drops the LFE;
 * here it is folded at -6 dB (0.5) into both sides, because on a stereo
 * downmix a voice a title sends only to the LFE bin would otherwise vanish.
 * The XTLK bins carry the HRTF'd 3D voices (front and back pairs) and fold
 * like the bed's front and back. With no reverb running, the I3DL2 send has
 * nothing to feed and is kept at -6 dB in both sides as a dry stand-in (it was
 * full scale in the left channel only before); with the reverb it has no dry
 * path, as on hardware. The FX sends go where the effects image routes them,
 * which this runtime cannot know: they keep the pairing they had (even bins
 * left, odd right, unity).
 *
 * No bin is louder than before in total power (gl^2 + gr^2 <= 1): the test
 * apu_downmix checks it bin by bin. */
#define C3 0.70710678f
#define C6 0.5f

void apu_mix_bin_gain(int mode, int b, int reverb_on, float *gl, float *gr)
{
    float l = 0.0f, r = 0.0f;

    if (mode == APU_MIXDOWN_TWO) {
        l = (b == 0) ? 1.0f : 0.0f;
        r = (b == 1) ? 1.0f : 0.0f;
    } else if (mode == APU_MIXDOWN_SUM) {
        l = (b & 1) ? 0.0f : 1.0f;
        r = (b & 1) ? 1.0f : 0.0f;
    } else {
        switch (b) {
        case 0: case 6: l = 1.0f; break;
        case 1: case 7: r = 1.0f; break;
        case 2:         l = r = C3; break;
        case 3:         l = r = C6; break;
        case 4: case 8: l = C3; break;
        case 5: case 9: r = C3; break;
        case APU_MIXBIN_I3DL2:
            l = r = reverb_on ? 0.0f : C6;
            break;
        default:
            l = (b & 1) ? 0.0f : 1.0f;
            r = (b & 1) ? 1.0f : 0.0f;
            break;
        }
    }
    *gl = l;
    *gr = r;
}

/* ── I3DL2 presets ─────────────────────────────────────────────────────────
 *
 * The I3DL2 guideline's environment presets, as DirectX's dsound.h publishes
 * them (I3DL2_ENVIRONMENT_PRESET_*): room, room HF, rolloff, decay time, HF
 * ratio, reflections, reflections delay, reverb, reverb delay, diffusion,
 * density, HF reference. */
static const struct { const char *name; ApuI3dl2 p; } k_presets[] = {
    { "generic",       { -1000,  -100, 0.0f,  1.49f, 0.83f, -2602, 0.007f,   200, 0.011f, 100.0f, 100.0f, 5000.0f } },
    { "paddedcell",    { -1000, -6000, 0.0f,  0.17f, 0.10f, -1204, 0.001f,   207, 0.002f, 100.0f, 100.0f, 5000.0f } },
    { "room",          { -1000,  -454, 0.0f,  0.40f, 0.83f, -1646, 0.002f,    53, 0.003f, 100.0f, 100.0f, 5000.0f } },
    { "bathroom",      { -1000, -1200, 0.0f,  1.49f, 0.54f,  -370, 0.007f,  1030, 0.011f, 100.0f,  60.0f, 5000.0f } },
    { "livingroom",    { -1000, -6000, 0.0f,  0.50f, 0.10f, -1376, 0.003f, -1104, 0.004f, 100.0f, 100.0f, 5000.0f } },
    { "stoneroom",     { -1000,  -300, 0.0f,  2.31f, 0.64f,  -711, 0.012f,    83, 0.017f, 100.0f, 100.0f, 5000.0f } },
    { "auditorium",    { -1000,  -476, 0.0f,  4.32f, 0.59f,  -789, 0.020f,  -289, 0.030f, 100.0f, 100.0f, 5000.0f } },
    { "concerthall",   { -1000,  -500, 0.0f,  3.92f, 0.70f, -1230, 0.020f,    -2, 0.029f, 100.0f, 100.0f, 5000.0f } },
    { "cave",          { -1000,     0, 0.0f,  2.91f, 1.30f,  -602, 0.015f,  -302, 0.022f, 100.0f, 100.0f, 5000.0f } },
    { "arena",         { -1000,  -698, 0.0f,  7.24f, 0.33f, -1166, 0.020f,    16, 0.030f, 100.0f, 100.0f, 5000.0f } },
    { "hangar",        { -1000, -1000, 0.0f, 10.05f, 0.23f,  -602, 0.020f,   198, 0.030f, 100.0f, 100.0f, 5000.0f } },
    { "hallway",       { -1000,  -300, 0.0f,  1.49f, 0.59f, -2219, 0.007f,   441, 0.011f, 100.0f, 100.0f, 5000.0f } },
    { "stonecorridor", { -1000,  -237, 0.0f,  2.70f, 0.79f, -1214, 0.013f,   395, 0.020f, 100.0f, 100.0f, 5000.0f } },
    { "alley",         { -1000,  -270, 0.0f,  1.49f, 0.86f, -1204, 0.007f,    -4, 0.011f, 100.0f, 100.0f, 5000.0f } },
    { "city",          { -1000,  -800, 0.0f,  1.49f, 0.67f, -2273, 0.007f, -1691, 0.011f, 100.0f, 100.0f, 5000.0f } },
    { "mountains",     { -1000, -2500, 0.0f,  1.49f, 0.21f, -2780, 0.300f, -1434, 0.100f,  27.0f, 100.0f, 5000.0f } },
    { "quarry",        { -1000, -1000, 0.0f,  1.49f, 0.83f,-10000, 0.061f,   500, 0.025f, 100.0f, 100.0f, 5000.0f } },
    { "plain",         { -1000, -2000, 0.0f,  1.49f, 0.50f, -2466, 0.179f, -1926, 0.100f,  21.0f, 100.0f, 5000.0f } },
    { "parkinglot",    { -1000,     0, 0.0f,  1.65f, 1.50f, -1363, 0.008f, -1153, 0.012f, 100.0f, 100.0f, 5000.0f } },
    { "sewerpipe",     { -1000, -1000, 0.0f,  2.81f, 0.14f,   429, 0.014f,  1023, 0.021f,  80.0f,  60.0f, 5000.0f } },
    { "underwater",    { -1000, -4000, 0.0f,  1.49f, 0.10f,  -449, 0.007f,  1700, 0.011f, 100.0f, 100.0f, 5000.0f } },
};

int apu_i3dl2_preset(const char *name, ApuI3dl2 *out)
{
    size_t i;
    for (i = 0; i < sizeof k_presets / sizeof k_presets[0]; i++)
        if (!strcmp(name, k_presets[i].name)) {
            if (out)
                *out = k_presets[i].p;
            return 1;
        }
    return 0;
}

const char *apu_i3dl2_preset_name(int i)
{
    if (i < 0 || (size_t)i >= sizeof k_presets / sizeof k_presets[0])
        return NULL;
    return k_presets[i].name;
}

int apu_mix_reverb_config(ApuI3dl2 *p, const char **name)
{
    if (s_reverb < 0) {
        const char *e = getenv("RECOMP_APU_REVERB");
        s_reverb = 0;
        if (e && *e && strcmp(e, "0") && strcmp(e, "off")) {
            const char *n = (!strcmp(e, "1") || !strcmp(e, "on")) ? "generic" : e;
            for (int i = 0; apu_i3dl2_preset_name(i); i++)
                if (!strcmp(n, apu_i3dl2_preset_name(i))) {
                    s_reverb_name = apu_i3dl2_preset_name(i);
                    s_reverb_params = k_presets[i].p;
                    s_reverb = 1;
                }
            if (!s_reverb) {
                fprintf(stderr, "[APU-MIX] RECOMP_APU_REVERB=%s: no such I3DL2 preset;"
                                " reverb off\n", e);
            }
        }
    }
    if (s_reverb) {
        if (p) *p = s_reverb_params;
        if (name) *name = s_reverb_name;
    }
    return s_reverb;
}

/* ── The reverb ──────────────────────────────────────────────────────────── */

#define RV_LINES   8
#define RV_LINE_SZ 4096              /* > the longest line, power of two */
#define RV_PRE_SZ  32768             /* > 0.3 s + 0.1 s at 48 kHz */
#define RV_AP_SZ   1024
#define RV_TAPS    4

/* Mutually prime line lengths, 43-72 ms at 48 kHz, scaled by the density. */
static const int k_line_len[RV_LINES] = { 2053, 2251, 2399, 2617, 2789, 3011, 3221, 3469 };
static const int k_ap_len[2] = { 563, 383 };
/* Early reflections: taps after the reflections delay, alternating sides. */
static const float k_tap_s[RV_TAPS] = { 0.0f, 0.0043f, 0.0089f, 0.0131f };
static const float k_tap_g[RV_TAPS] = { 0.62f, 0.50f, 0.40f, 0.33f };

struct ApuReverb {
    float pre[RV_PRE_SZ];
    unsigned pre_w;
    float line[RV_LINES][RV_LINE_SZ];
    unsigned line_w;
    float lp[RV_LINES];             /* per-line damping state */
    float ap[2][RV_AP_SZ];
    unsigned ap_w;
    float room_lp;                  /* room HF filter state */

    /* derived */
    int len[RV_LINES];
    float g[RV_LINES], damp[RV_LINES];
    int tap[RV_TAPS];
    int late_tap;
    float room_gain, room_a;
    float refl_gain, late_gain, ap_coef;
};

static float mb_to_gain(int32_t mb)
{
    if (mb <= -10000)
        return 0.0f;
    return powf(10.0f, (float)mb / 2000.0f);
}

/* One-pole low-pass y += (1-a)(x-y) whose gain at f is g (DC gain 1). */
static float onepole_for_gain(float g, float f)
{
    float c, A, B, disc, a;
    if (g >= 0.9999f)
        return 0.0f;
    if (g < 0.001f)
        g = 0.001f;
    c = cosf(2.0f * (float)M_PI * f / APU_MIX_RATE);
    A = g * g - 1.0f;
    B = 2.0f - 2.0f * g * g * c;
    disc = B * B - 4.0f * A * A;
    if (disc < 0.0f)
        disc = 0.0f;
    a = (-B + sqrtf(disc)) / (2.0f * A);
    if (a < 0.0f) a = 0.0f;
    if (a > 0.999f) a = 0.999f;
    return a;
}

ApuReverb *apu_reverb_create(void)
{
    ApuReverb *rv = (ApuReverb *)calloc(1, sizeof(*rv));
    ApuI3dl2 p;
    if (rv && apu_i3dl2_preset("generic", &p))
        apu_reverb_set(rv, &p);
    return rv;
}

void apu_reverb_destroy(ApuReverb *rv)
{
    free(rv);
}

void apu_reverb_clear(ApuReverb *rv)
{
    memset(rv->pre, 0, sizeof rv->pre);
    memset(rv->line, 0, sizeof rv->line);
    memset(rv->lp, 0, sizeof rv->lp);
    memset(rv->ap, 0, sizeof rv->ap);
    rv->room_lp = 0.0f;
}

void apu_reverb_set(ApuReverb *rv, const ApuI3dl2 *p)
{
    float t60 = p->decay_time < 0.1f ? 0.1f : (p->decay_time > 20.0f ? 20.0f : p->decay_time);
    float hfr = p->decay_hf_ratio < 0.1f ? 0.1f : (p->decay_hf_ratio > 1.0f ? 1.0f : p->decay_hf_ratio);
    float dens = p->density < 0.0f ? 0.0f : (p->density > 100.0f ? 100.0f : p->density);
    float diff = p->diffusion < 0.0f ? 0.0f : (p->diffusion > 100.0f ? 100.0f : p->diffusion);
    float href = p->hf_reference < 20.0f ? 20.0f : (p->hf_reference > 20000.0f ? 20000.0f : p->hf_reference);
    float rdel = p->reflections_delay < 0.0f ? 0.0f : (p->reflections_delay > 0.3f ? 0.3f : p->reflections_delay);
    float ldel = p->reverb_delay < 0.0f ? 0.0f : (p->reverb_delay > 0.1f ? 0.1f : p->reverb_delay);
    int i;

    rv->room_gain = mb_to_gain(p->room > 0 ? 0 : p->room);
    rv->room_a = onepole_for_gain(mb_to_gain(p->room_hf > 0 ? 0 : p->room_hf), href);
    /* I3DL2 allows reflections up to +1000 mB and reverb up to +2000 mB. */
    rv->refl_gain = mb_to_gain(p->reflections > 1000 ? 1000 : p->reflections);
    rv->late_gain = mb_to_gain(p->reverb > 2000 ? 2000 : p->reverb) * 0.25f;
    rv->ap_coef = 0.6f * diff / 100.0f;
    for (i = 0; i < RV_TAPS; i++) {
        int t = (int)((rdel + k_tap_s[i]) * APU_MIX_RATE);
        rv->tap[i] = t < 1 ? 1 : (t >= RV_PRE_SZ ? RV_PRE_SZ - 1 : t);
    }
    rv->late_tap = (int)((rdel + ldel) * APU_MIX_RATE);
    if (rv->late_tap < 1) rv->late_tap = 1;
    if (rv->late_tap >= RV_PRE_SZ) rv->late_tap = RV_PRE_SZ - 1;
    for (i = 0; i < RV_LINES; i++) {
        /* Density 0..100 % scales the lines 0.5..1.0: sparser echoes when low. */
        int len = (int)(k_line_len[i] * (0.5f + 0.5f * dens / 100.0f));
        float g_lo, g_hi;
        rv->len[i] = len;
        /* -60 dB after t60 seconds: per pass of len samples. */
        g_lo = powf(10.0f, -3.0f * (float)len / (t60 * APU_MIX_RATE));
        g_hi = powf(10.0f, -3.0f * (float)len / (t60 * hfr * APU_MIX_RATE));
        rv->g[i] = g_lo;
        rv->damp[i] = onepole_for_gain(g_hi / g_lo, href);
    }
}

static inline float flush(float x)
{
    return (x > -1e-15f && x < 1e-15f) ? 0.0f : x;
}

void apu_reverb_process(ApuReverb *rv, const float *in, float (*out)[2], int n)
{
    int i, k;
    for (i = 0; i < n; i++) {
        float x, late, y[RV_LINES], s = 0.0f, l = 0.0f, r = 0.0f;
        unsigned pw;

        /* Room level and room HF on the send. */
        x = in[i] * rv->room_gain;
        rv->room_lp = flush(x + rv->room_a * (rv->room_lp - x));
        pw = rv->pre_w = (rv->pre_w + 1) & (RV_PRE_SZ - 1);
        rv->pre[pw] = rv->room_lp;

        /* Early reflections. */
        for (k = 0; k < RV_TAPS; k++) {
            float t = rv->pre[(pw - rv->tap[k]) & (RV_PRE_SZ - 1)] * k_tap_g[k];
            if (k & 1) r += t; else l += t;
        }
        l *= rv->refl_gain;
        r *= rv->refl_gain;

        /* Late reverb: diffused input into the FDN. */
        late = rv->pre[(pw - rv->late_tap) & (RV_PRE_SZ - 1)];
        rv->ap_w = (rv->ap_w + 1) & (RV_AP_SZ - 1);
        for (k = 0; k < 2; k++) {
            float d = rv->ap[k][(rv->ap_w - k_ap_len[k]) & (RV_AP_SZ - 1)];
            float v = late + rv->ap_coef * d;
            rv->ap[k][rv->ap_w] = flush(v);
            late = d - rv->ap_coef * v;
        }

        rv->line_w = (rv->line_w + 1) & (RV_LINE_SZ - 1);
        for (k = 0; k < RV_LINES; k++) {
            float d = rv->line[k][(rv->line_w - rv->len[k]) & (RV_LINE_SZ - 1)];
            /* HF damping, then the decay gain. */
            rv->lp[k] = flush(d + rv->damp[k] * (rv->lp[k] - d));
            y[k] = rv->lp[k] * rv->g[k];
            s += y[k];
        }
        s *= 2.0f / RV_LINES;               /* Householder: I - 2/N 11^T */
        for (k = 0; k < RV_LINES; k++) {
            float fb = y[k] - s + ((k & 2) ? -late : late);
            rv->line[k][rv->line_w] = flush(fb);
            if (k & 1) r += y[k] * rv->late_gain;
            else       l += y[k] * rv->late_gain;
        }
        out[i][0] += l;
        out[i][1] += r;
    }
}

/* ── The stage ───────────────────────────────────────────────────────────── */

void apu_mix_frame(int mode, const float bins[APU_MIX_BINS][APU_MIX_SAMPLES],
                   ApuReverb *rv, float out[APU_MIX_SAMPLES][2])
{
    int b, i;
    memset(out, 0, sizeof(float) * APU_MIX_SAMPLES * 2);
    for (b = 0; b < APU_MIX_BINS; b++) {
        float gl, gr;
        apu_mix_bin_gain(mode, b, rv != NULL, &gl, &gr);
        if (gl == 0.0f && gr == 0.0f)
            continue;
        for (i = 0; i < APU_MIX_SAMPLES; i++) {
            out[i][0] += gl * bins[b][i];
            out[i][1] += gr * bins[b][i];
        }
    }
    if (rv)
        apu_reverb_process(rv, bins[APU_MIXBIN_I3DL2], out, APU_MIX_SAMPLES);
}
