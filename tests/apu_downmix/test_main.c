/* XBA-122 stages 2 and 3: the mixbins are folded to stereo by role, and the
 * native I3DL2 reverb behind RECOMP_APU_REVERB is fed from the I3DL2 send.
 *
 * Every check reads numbers: coefficients, WAV dumps written through the
 * RECOMP_AUDIO_WAV path (apu_wav, after mcpx_apu_monitor_frame, as a run
 * writes them) and read back, decay times of the reverb's impulse response,
 * and the host CPU cost of one 32-sample GP frame. Synthetic signals only. */
#include "apu_state.h"
#include "apu_shim.h"
#include "apu_mix.h"
#include "apu_wav.h"
#include "apu_dsp_watch.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#define getpid _getpid
static void set_env(const char *k, const char *v) { _putenv_s(k, v); }
static void unset_env(const char *k) { _putenv_s(k, ""); }
static double now_s(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}
#else
#include <time.h>
#include <unistd.h>
static void set_env(const char *k, const char *v) { setenv(k, v, 1); }
static void unset_env(const char *k) { unsetenv(k); }
static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void *recomp_lookup(unsigned long address) { (void)address; abort(); }
void *recomp_lookup_manual(unsigned long address) { (void)address; abort(); }

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-68s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static float bins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
static char wav_path[300];

/* ── Rendering through the runtime's own path into a WAV ────────────────── */

typedef void (*fill_fn)(int frame);

/* Render n frames through mcpx_apu_dsp_frame + mcpx_apu_monitor_frame into
 * the WAV tap, as the frame thread does; returns the samples read back. */
static int16_t *render(int n, fill_fn fill, long *count)
{
    MCPXAPUState *d = (MCPXAPUState *)calloc(1, sizeof(*d));
    FILE *f;
    unsigned char h[44];
    long bytes;
    int16_t *pcm;

    mcpx_apu_dsp_init(d);              /* re-reads RECOMP_APU_MIXDOWN/_REVERB */
    remove(wav_path);
    if (!apu_wav_open(wav_path, 0.0)) {
        fprintf(stderr, "cannot open %s\n", wav_path);
        exit(2);
    }
    for (int i = 0; i < n; i++) {
        memset(bins, 0, sizeof bins);
        fill(i);
        mcpx_apu_dsp_frame(d, bins);
        mcpx_apu_monitor_frame(d);
        d->ep_frame_div++;
    }
    apu_wav_close();
    mcpx_apu_dsp_finalize(d);
    free(d);

    f = fopen(wav_path, "rb");
    if (!f || fread(h, 1, 44, f) != 44 || memcmp(h + 36, "data", 4)) {
        fprintf(stderr, "bad WAV %s\n", wav_path);
        exit(2);
    }
    bytes = h[40] | (h[41] << 8) | (h[42] << 16) | ((long)h[43] << 24);
    pcm = (int16_t *)malloc((size_t)bytes);
    *count = (long)fread(pcm, 1, (size_t)bytes, f) / 4;
    fclose(f);
    remove(wav_path);
    return pcm;
}

/* Four segments of a 1 kHz tone at 0.5, 0.25 s each: centre, LFE, back
 * left, I3DL2 send. */
#define SEG_FRAMES 375                  /* 0.25 s of 32-sample frames */
static const int k_seg_bin[4] = { 2, 3, 4, APU_MIXBIN_I3DL2 };

static void fill_segments(int frame)
{
    int seg = frame / SEG_FRAMES;
    if (seg > 3)
        return;
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        long n = (long)frame * NUM_SAMPLES_PER_FRAME + i;
        bins[k_seg_bin[seg]][i] = 0.5f * (float)sin(2.0 * M_PI * 1000.0 * n / 48000.0);
    }
}

/* Peak of each channel over a segment, skipping its first 20 ms (the reverb
 * of the segment before, when on). */
static void seg_peak(const int16_t *pcm, int seg, int *pl, int *pr)
{
    long a = (long)seg * SEG_FRAMES * NUM_SAMPLES_PER_FRAME + 960;
    long b = (long)(seg + 1) * SEG_FRAMES * NUM_SAMPLES_PER_FRAME;
    *pl = *pr = 0;
    for (long i = a; i < b; i++) {
        int l = abs(pcm[2 * i]), r = abs(pcm[2 * i + 1]);
        if (l > *pl) *pl = l;
        if (r > *pr) *pr = r;
    }
}

static int near(int got, double want)
{
    return fabs(got - want * 32767.0) <= 3.0;
}

static int s_impulse_frame;
static void fill_impulse(int frame)
{
    if (frame == s_impulse_frame)
        bins[APU_MIXBIN_I3DL2][0] = 1.0f;
    bins[0][0] = 0.0f;
}

/* T60 of a stereo float impulse response: Schroeder's backward-integrated
 * energy, the -5..-25 dB slope extended to -60 dB (T20 x 3). */
static double t60_of(const float *ir, long n)
{
    double *edc = (double *)malloc(sizeof(double) * (size_t)n), acc = 0.0, t5 = -1, t25 = -1;
    for (long i = n - 1; i >= 0; i--) {
        acc += (double)ir[i] * ir[i];
        edc[i] = acc;
    }
    for (long i = 0; i < n; i++) {
        double db = 10.0 * log10(edc[i] / edc[0] + 1e-30);
        if (t5 < 0 && db <= -5.0) t5 = (double)i / APU_MIX_RATE;
        if (t25 < 0 && db <= -25.0) { t25 = (double)i / APU_MIX_RATE; break; }
    }
    free(edc);
    return (t5 < 0 || t25 < 0) ? -1.0 : (t25 - t5) * 3.0;
}

static double reverb_t60(const char *preset, float *peak_out)
{
    ApuI3dl2 p;
    ApuReverb *rv = apu_reverb_create();
    long n = 12L * APU_MIX_RATE;
    float *ir = (float *)calloc((size_t)n, sizeof(float));
    float in[APU_MIX_SAMPLES], out[APU_MIX_SAMPLES][2], peak = 0.0f;
    double t;

    apu_i3dl2_preset(preset, &p);
    apu_reverb_set(rv, &p);
    for (long f = 0; f < n / APU_MIX_SAMPLES; f++) {
        memset(in, 0, sizeof in);
        memset(out, 0, sizeof out);
        if (f == 0) in[0] = 1.0f;
        apu_reverb_process(rv, in, out, APU_MIX_SAMPLES);
        for (int i = 0; i < APU_MIX_SAMPLES; i++) {
            ir[f * APU_MIX_SAMPLES + i] = out[i][0] + out[i][1];
            if (fabsf(out[i][0]) > peak) peak = fabsf(out[i][0]);
        }
    }
    t = t60_of(ir, n);
    if (peak_out) *peak_out = peak;
    free(ir);
    apu_reverb_destroy(rv);
    return t;
}

int main(void)
{
    const char *tmp = getenv("TMPDIR");
    int16_t *pcm;
    long count;
    int pl, pr, b;
    char line[160];

    setvbuf(stdout, NULL, _IONBF, 0);
    snprintf(wav_path, sizeof wav_path, "%s/xba_downmix_%d.wav", tmp && *tmp ? tmp : "/tmp", (int)getpid());
    unset_env("RECOMP_APU_MIXDOWN_ALL");
    unset_env("RECOMP_APU_REVERB");
    unset_env("RECOMP_APU_REVERB_IMAGES");
    unset_env("RECOMP_APU_MIXDOWN");

    puts("Routed coefficients");
    {
        int ok_power = 1, ok_pairs = 1;
        float gl, gr;
        for (b = 0; b < 32; b++) {
            apu_mix_bin_gain(APU_MIXDOWN_ROUTED, b, 0, &gl, &gr);
            if (gl * gl + gr * gr > 1.0001f) ok_power = 0;
        }
        apu_mix_bin_gain(APU_MIXDOWN_ROUTED, 0, 0, &gl, &gr); ok_pairs &= gl == 1.0f && gr == 0.0f;
        apu_mix_bin_gain(APU_MIXDOWN_ROUTED, 1, 0, &gl, &gr); ok_pairs &= gl == 0.0f && gr == 1.0f;
        apu_mix_bin_gain(APU_MIXDOWN_ROUTED, 6, 0, &gl, &gr); ok_pairs &= gl == 1.0f && gr == 0.0f;
        apu_mix_bin_gain(APU_MIXDOWN_ROUTED, 9, 0, &gl, &gr); ok_pairs &= gl == 0.0f && fabsf(gr - 0.7071f) < 1e-3f;
        apu_mix_bin_gain(APU_MIXDOWN_ROUTED, APU_MIXBIN_I3DL2, 1, &gl, &gr); ok_pairs &= gl == 0.0f && gr == 0.0f;
        check(ok_power, "no bin louder than before: gl^2 + gr^2 <= 1 for all 32");
        check(ok_pairs, "fronts unity, XTLK back -3 dB, I3DL2 send wet-only with reverb");
        check(apu_mix_mode() == APU_MIXDOWN_ROUTED, "routed is the default");
    }

    puts("WAV dump, routed (default)");
    pcm = render(4 * SEG_FRAMES + 8, fill_segments, &count);
    /* The tap takes whole EP periods: 8 frames, 256 samples. */
    check(count == (4 * SEG_FRAMES + 8) / 8 * 8 * NUM_SAMPLES_PER_FRAME,
          "WAV holds every whole EP period rendered");
    seg_peak(pcm, 0, &pl, &pr);
    snprintf(line, sizeof line, "centre 0.5 -> L %d R %d (want 0.354 both = 11585)", pl, pr);
    check(near(pl, 0.5 * 0.70710678) && near(pr, 0.5 * 0.70710678), line);
    seg_peak(pcm, 1, &pl, &pr);
    snprintf(line, sizeof line, "LFE 0.5 -> L %d R %d (want 0.25 both = 8192)", pl, pr);
    check(near(pl, 0.25) && near(pr, 0.25), line);
    seg_peak(pcm, 2, &pl, &pr);
    snprintf(line, sizeof line, "back L 0.5 -> L %d R %d (want 0.354, 0)", pl, pr);
    check(near(pl, 0.5 * 0.70710678) && pr == 0, line);
    seg_peak(pcm, 3, &pl, &pr);
    snprintf(line, sizeof line, "I3DL2 send 0.5, no reverb -> L %d R %d (want 0.25 both)", pl, pr);
    check(near(pl, 0.25) && near(pr, 0.25), line);
    free(pcm);

    puts("WAV dump, RECOMP_APU_MIXDOWN=sum (the behaviour before, negative control)");
    set_env("RECOMP_APU_MIXDOWN", "sum");
    pcm = render(4 * SEG_FRAMES + 8, fill_segments, &count);
    seg_peak(pcm, 0, &pl, &pr);
    snprintf(line, sizeof line, "centre 0.5 -> L %d R %d (left only, as before)", pl, pr);
    check(near(pl, 0.5) && pr == 0, line);
    seg_peak(pcm, 1, &pl, &pr);
    check(pl == 0 && near(pr, 0.5), "LFE in the right channel only, as before");
    free(pcm);
    set_env("RECOMP_APU_MIXDOWN_ALL", "0");
    unset_env("RECOMP_APU_MIXDOWN");
    pcm = render(SEG_FRAMES + 8, fill_segments, &count);
    seg_peak(pcm, 0, &pl, &pr);
    check(pl == 0 && pr == 0, "RECOMP_APU_MIXDOWN_ALL=0 still means bins 0/1 only");
    free(pcm);
    unset_env("RECOMP_APU_MIXDOWN_ALL");

    puts("Reverb: decay times of the impulse response (float)");
    {
        float peak;
        double tg = reverb_t60("generic", &peak), tr = reverb_t60("room", NULL);
        double tc = reverb_t60("cave", NULL), th = reverb_t60("hallway", NULL);
        snprintf(line, sizeof line, "generic T60 %.2f s (preset 1.49, within 30%%)", tg);
        check(tg > 1.49 * 0.7 && tg < 1.49 * 1.3, line);
        snprintf(line, sizeof line, "room %.2f s < hallway %.2f s < cave %.2f s", tr, th, tc);
        check(tr > 0 && tr < th && th < tc, line);
        snprintf(line, sizeof line, "cave T60 %.2f s (preset 2.91, within 30%%)", tc);
        check(tc > 2.91 * 0.7 && tc < 2.91 * 1.3, line);
        snprintf(line, sizeof line, "unit impulse peak %.3f (bounded)", peak);
        check(peak > 0.001f && peak < 1.0f, line);
    }

    puts("Reverb: WAV A/B through the runtime (RECOMP_APU_REVERB=generic)");
    {
        long tail_on = 0, early_on = 0;
        int16_t *off;
        long n_off;
        s_impulse_frame = 10;
        off = render(400, fill_impulse, &n_off);
        set_env("RECOMP_APU_REVERB", "generic");
        pcm = render(400, fill_impulse, &count);
        for (long i = 0; i < count; i++) {
            long t = i - 10L * NUM_SAMPLES_PER_FRAME;
            int a = abs(pcm[2 * i]) + abs(pcm[2 * i + 1]);
            if (t >= 0 && t < 300) early_on += a;
            if (t > 4800) tail_on += a > 0;
        }
        snprintf(line, sizeof line, "off: impulse dry at 0.5 (L %d), nothing after it",
                 off[2 * (10 * NUM_SAMPLES_PER_FRAME)]);
        {
            long after = 0;
            for (long i = 10L * NUM_SAMPLES_PER_FRAME + 1; i < n_off; i++)
                after += off[2 * i] != 0 || off[2 * i + 1] != 0;
            check(near(off[2 * (10 * NUM_SAMPLES_PER_FRAME)], 0.5) && after == 0, line);
        }
        check(early_on == 0, "on: no dry path, silence before the 7 ms reflections delay");
        snprintf(line, sizeof line, "on: tail still audible 100+ ms later (%ld non-zero samples)", tail_on);
        check(tail_on > 1000, line);
        free(off);
        free(pcm);
    }

    puts("Reverb: stability and silence");
    {
        ApuI3dl2 p;
        ApuReverb *rv = apu_reverb_create();
        float in[APU_MIX_SAMPLES], out[APU_MIX_SAMPLES][2], peak = 0.0f;
        uint32_t r = 1;
        int finite = 1, silent = 1;
        apu_i3dl2_preset("hangar", &p);       /* 10 s decay: the worst case */
        apu_reverb_set(rv, &p);
        for (long f = 0; f < 20L * 1500; f++) {
            for (int i = 0; i < APU_MIX_SAMPLES; i++) {
                r = r * 1664525u + 1013904223u;
                in[i] = 0.3f * ((float)(r >> 8) / 8388608.0f - 1.0f);
            }
            memset(out, 0, sizeof out);
            apu_reverb_process(rv, in, out, APU_MIX_SAMPLES);
            for (int i = 0; i < APU_MIX_SAMPLES; i++) {
                if (!isfinite(out[i][0]) || !isfinite(out[i][1])) finite = 0;
                if (fabsf(out[i][0]) > peak) peak = fabsf(out[i][0]);
            }
        }
        snprintf(line, sizeof line, "20 s of noise at 0.3 into 'hangar': finite, peak %.3f < 1", peak);
        check(finite && peak < 1.0f, line);
        memset(in, 0, sizeof in);
        for (long f = 0; f < 120L * 1500; f++) {
            memset(out, 0, sizeof out);
            apu_reverb_process(rv, in, out, APU_MIX_SAMPLES);
        }
        for (int i = 0; i < APU_MIX_SAMPLES; i++)
            if (out[i][0] != 0.0f || out[i][1] != 0.0f) silent = 0;
        check(silent, "then 120 s of silence: exactly zero (no denormal tail)");
        apu_reverb_destroy(rv);
    }

    puts("Reverb: bypassed under an unlisted GP image (RECOMP_APU_REVERB_IMAGES)");
    {
        uint64_t byp0;
        set_env("RECOMP_APU_REVERB_IMAGES", "deadbeefdeadbeef");
        s_impulse_frame = 10;
        pcm = render(100, fill_impulse, &count);
        byp0 = apu_dsp_watch_stats()->reverb_bypassed;
        check(near(pcm[2 * (10 * NUM_SAMPLES_PER_FRAME)], 0.5) && byp0 == 100,
              "no image listed: reverb bypassed (counted 100 frames), send dry at 0.5");
        free(pcm);
        unset_env("RECOMP_APU_REVERB_IMAGES");
    }

    puts("Cost of one 32-sample GP frame on this host");
    {
        ApuReverb *rv = apu_reverb_create();
        static float fb[APU_MIX_BINS][APU_MIX_SAMPLES];
        float out[APU_MIX_SAMPLES][2];
        uint32_t r = 7;
        const int N = 30000;
        double t0, t_mix, t_rv, sink = 0.0;
        for (b = 0; b < APU_MIX_BINS; b++)
            for (int i = 0; i < APU_MIX_SAMPLES; i++) {
                r = r * 1664525u + 1013904223u;
                fb[b][i] = 0.01f * ((float)(r >> 8) / 8388608.0f - 1.0f);
            }
        t0 = now_s();
        for (int f = 0; f < N; f++) {
            apu_mix_frame(APU_MIXDOWN_ROUTED, (const float (*)[APU_MIX_SAMPLES])fb, NULL, out);
            sink += out[f & 31][0];
        }
        t_mix = (now_s() - t0) / N;
        t0 = now_s();
        for (int f = 0; f < N; f++) {
            apu_mix_frame(APU_MIXDOWN_ROUTED, (const float (*)[APU_MIX_SAMPLES])fb, rv, out);
            sink += out[f & 31][0];
        }
        t_rv = (now_s() - t0) / N;
        printf("  routed mixdown %.2f us/frame, with reverb %.2f us/frame"
               " (budget 666.7 us; %.2f%% of one core at 1500 frames/s) [sink %g]\n",
               t_mix * 1e6, t_rv * 1e6, t_rv * 1500.0 * 100.0, sink);
        /* Generous: qemu-user runs this 10-30x slower than a phone. */
        check(t_rv < 300e-6, "mixdown + reverb well under the 667 us frame budget");
        apu_reverb_destroy(rv);
    }

    unset_env("RECOMP_APU_REVERB");
    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
