/* RECOMP_PACING=auto (src/kernel/frame_pacing.h, XBA-107 / XBA-135) on a
 * fake clock: a synthetic title flips with a programmable frame cost, the
 * executor's cap (flip_cap_book / flip_cap_wait in nv2a_pb_exec.c: an
 * absolute schedule, a frame later than a whole period restarts it) sleeps
 * to the rate the pacer picks, and a vsynced window shows each flip at the
 * display's next refresh, as the SDL presenter does. Each case checks the
 * steady rate, the [FBWIN] frame-time standard deviation of the presents
 * (under 2 ms), how often the rate changed, and how fast it reacted.
 *
 * The cases against tonight's profile values:
 *   10 ms, interval ONE        60  (engines/renderware RECOMP_GPU_FPS_CAP=60)
 *   20 ms                      30  (Fable's env_default_android cap of 30)
 *   40 ms                      20  (the profile's 30 cap would wander at ~25)
 *   12-22 ms, varying          30, no flapping
 *   flip-locked (Burnout 3)    60 kept through a heavy scene (never divided)
 * and the switches: the ask (TWO, IMMEDIATE, PAL 50 Hz, a 120 Hz panel),
 * RECOMP_PACING_LOCK, RECOMP_PACING=ask. */
#include "frame_pacing.h"
#include "frame_times.h"

#define _USE_MATH_DEFINES
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <stdio.h>
#include <stdlib.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

#define MS 1000000ull
#define SEC 1000000000ull

/* Interval codes as frame_pacing_swap takes them (payload bits 0-3). */
#define IV_ONE 1u
#define IV_TWO 2u
#define IV_IMMEDIATE 8u

typedef struct {
    const char *name;
    uint32_t display_hz, vblank_hz;
    int lock_force, adaptive;
    uint32_t swap;                          /* the payload every swap carries */
    /* Frame cost at time t (seconds since start) of frame i: CPU part (may
     * run while the executor sleeps at the cap) and GPU part (after the
     * executor wakes). */
    void (*cost)(double t, uint32_t i, uint64_t *cpu_ns, uint64_t *gpu_ns);
    double seconds;
} Sim;

typedef struct {
    uint32_t rate;                          /* at the end */
    uint32_t changes_after;                 /* rate changes after `settle` s */
    double sd_ms, mean_ms;                  /* presents over the last 10 s, display phase-aligned */
    double raw_sd_ms;                       /* the same at a fixed display phase */
    double flip_sd_ms;
    double t_rate[8];                       /* first time the rate reached rate_mark[k] */
    int locked;
} Result;

static uint32_t s_seed = 12345u;
static double frand(void)                   /* 0..1, reproducible */
{
    s_seed = s_seed * 1103515245u + 12345u;
    return (double)((s_seed >> 8) & 0xFFFFu) / 65535.0;
}

/* The cap as the executor books it. Returns the slot the next walk waits
 * for (0: none). */
static uint64_t book(uint64_t now, uint32_t rate, uint64_t *next, uint32_t *last_rate)
{
    uint64_t period = SEC / rate;
    if (rate != *last_rate) {
        *last_rate = rate;
        *next = 0;
    }
    if (!*next) {
        *next = now + period;
    } else if (now >= *next + period) {
        *next = now;
        return 0;
    } else {
        *next += period;
    }
    return *next;
}

static Result run(const Sim *s, double settle, const uint32_t *marks, int nmarks)
{
    FramePacer p;
    Result r;
    FrameTimes ft = {0}, fft = {0}, rft = {0};
    static uint64_t flips[4096];
    uint32_t nflips = 0;
    uint64_t t0 = 1000 * SEC, now = t0, flip = t0, next = 0, slept = 0;
    uint64_t dper = SEC / s->display_hz, phase = 7 * MS + 300000ull;
    uint32_t last_rate = 0, i, rate, prev_rate = 0, n;
    double mean, sd, worst;
    int k;

    frame_pacing_init(&p, s->display_hz, s->lock_force, s->adaptive);
    for (k = 0; k < 8; k++)
        r.t_rate[k] = -1;
    r.changes_after = 0;
    r.sd_ms = r.mean_ms = r.flip_sd_ms = r.raw_sd_ms = -1;
    for (i = 0; now - t0 < (uint64_t)(s->seconds * SEC); i++) {
        double t = (double)(now - t0) / SEC;
        uint64_t cpu, gpu, slot, release, present;

        s->cost(t, i, &cpu, &gpu);
        /* The walk of this frame: the executor woke at `now`; the title's
         * CPU part started at the previous flip. */
        release = now;
        flip = (flip + cpu > release ? flip + cpu : release) + gpu;
        now = flip;
        frame_pacing_swap(&p, s->swap);
        frame_pacing_vblank(&p, (uint32_t)((now - t0) * s->vblank_hz / SEC), now);
        rate = frame_pacing_flip(&p, now, slept);
        if (prev_rate && rate != prev_rate && t >= settle)
            r.changes_after++;
        prev_rate = rate;
        for (k = 0; k < nmarks; k++)
            if (r.t_rate[k] < 0 && rate == marks[k] && (k == 0 || r.t_rate[k - 1] >= 0))
                r.t_rate[k] = t;
        /* The window shows it at the display's next refresh. */
        present = ((now - phase) / dper + 1) * dper + phase;
        if (t >= s->seconds - 10.0) {
            frame_times_add(&rft, present);
            frame_times_add(&fft, now);
            if (nflips < 4096)
                flips[nflips++] = now;
        }
        slot = book(now, rate, &next, &last_rate);
        slept = 0;
        if (slot > now) {
            /* A host sleep wakes up to 1 ms late (Android, macOS). */
            uint64_t late = (uint64_t)(frand() * 1.0 * MS);
            slept = slot - now + late;
            now = slot + late;
        }
    }
    /* The display's refresh relative to the host schedule is not ours to
     * choose (see docs: present-time alignment is a follow-up): the
     * presents at a fixed phase are printed, and the check is on the
     * presents with the refresh edge half a refresh away from the flips'
     * mean phase, i.e. the cadence itself. */
    {
        double cs = 0, sn = 0, m;
        uint64_t aligned;
        uint32_t j;
        for (j = 0; j < nflips; j++) {
            double a = 2.0 * M_PI * (double)(flips[j] % dper) / (double)dper;
            cs += cos(a);
            sn += sin(a);
        }
        m = atan2(sn, cs);
        if (m < 0)
            m += 2.0 * M_PI;
        aligned = ((uint64_t)(m / (2.0 * M_PI) * (double)dper) + dper - dper / 2) % dper;
        for (j = 0; j < nflips; j++)
            frame_times_add(&ft, ((flips[j] - aligned) / dper + 1) * dper + aligned);
    }
    if (frame_times_take(&ft, now + 20 * SEC, 1, &n, &mean, &sd, &worst)) {
        r.sd_ms = sd;
        r.mean_ms = mean;
    }
    if (frame_times_take(&rft, now + 20 * SEC, 1, &n, &mean, &sd, &worst))
        r.raw_sd_ms = sd;
    if (frame_times_take(&fft, now + 20 * SEC, 1, &n, &mean, &sd, &worst))
        r.flip_sd_ms = sd;
    r.rate = p.rate;
    r.locked = p.locked;
    printf("  %-40s %3u fps, presents mean %6.2f ms sd %5.2f (fixed phase %5.2f), flips sd %4.2f, "
           "changes after %4.1f s: %u, verdict %d\n", s->name, r.rate, r.mean_ms, r.sd_ms,
           r.raw_sd_ms, r.flip_sd_ms, settle, r.changes_after, r.locked);
    return r;
}

/* ── Frame costs ─────────────────────────────────────────────────────────── */

static double jitter(double ms) { return (frand() - 0.5) * 2.0 * ms; }

static void c10(double t, uint32_t i, uint64_t *cpu, uint64_t *gpu)
{ (void)t; (void)i; *cpu = 0; *gpu = (uint64_t)((10.0 + jitter(1.0)) * MS); }
static void c20(double t, uint32_t i, uint64_t *cpu, uint64_t *gpu)
{ (void)t; (void)i; *cpu = 0; *gpu = (uint64_t)((20.0 + jitter(1.5)) * MS); }
static void c40(double t, uint32_t i, uint64_t *cpu, uint64_t *gpu)
{ (void)t; (void)i; *cpu = 0; *gpu = (uint64_t)((40.0 + jitter(2.0)) * MS); }
static void cvary(double t, uint32_t i, uint64_t *cpu, uint64_t *gpu)
{ (void)i; *cpu = 0; *gpu = (uint64_t)((17.0 + 5.0 * sin(t * 1.3) + jitter(2.0)) * MS); }
/* 17 ms, most of it CPU work that overlaps the cap's sleep: the cost the
 * pacer measures at 30 is only the GPU part (5 ms), and every step up to 60
 * misses. */
static void c17_overlap(double t, uint32_t i, uint64_t *cpu, uint64_t *gpu)
{ (void)t; (void)i; *cpu = (uint64_t)((12.0 + jitter(0.5)) * MS); *gpu = 5 * MS; }
static void c17(double t, uint32_t i, uint64_t *cpu, uint64_t *gpu)
{ (void)t; (void)i; *cpu = 0; *gpu = (uint64_t)((17.0 + jitter(0.5)) * MS); }
/* A sustained change: 10 ms, then 25 ms from 40 s, then 8 ms from 60 s. */
static void cstep(double t, uint32_t i, uint64_t *cpu, uint64_t *gpu)
{
    double ms = t < 40.0 ? 10.0 : t < 60.0 ? 25.0 : 8.0;
    (void)i;
    *cpu = 0;
    *gpu = (uint64_t)((ms + jitter(1.0)) * MS);
}
/* Burnout 3: fast through the front end, then a heavy race scene at 18 ms. */
static void clocked(double t, uint32_t i, uint64_t *cpu, uint64_t *gpu)
{
    double ms = t < 40.0 ? 9.0 : 18.0;
    (void)i;
    *cpu = 0;
    *gpu = (uint64_t)((ms + jitter(1.0)) * MS);
}
static void c5(double t, uint32_t i, uint64_t *cpu, uint64_t *gpu)
{ (void)t; (void)i; *cpu = 0; *gpu = (uint64_t)((5.0 + jitter(0.5)) * MS); }
static void c12(double t, uint32_t i, uint64_t *cpu, uint64_t *gpu)
{ (void)t; (void)i; *cpu = 0; *gpu = (uint64_t)((12.0 + jitter(1.0)) * MS); }
static void c30(double t, uint32_t i, uint64_t *cpu, uint64_t *gpu)
{ (void)t; (void)i; *cpu = 0; *gpu = (uint64_t)((30.0 + jitter(1.5)) * MS); }

/* ── Unit checks of the pieces ───────────────────────────────────────────── */

static void units(void)
{
    FramePacer p;

    frame_pacing_init(&p, 0, -1, 1);
    CHECK(frame_pacing_ask(&p) == 60);                  /* no swap yet: ONE at 60 */
    CHECK(frame_pacing_step(&p, 60, -1) == 30);
    CHECK(frame_pacing_step(&p, 30, -1) == 20);
    CHECK(frame_pacing_step(&p, 20, -1) == 15);
    CHECK(frame_pacing_step(&p, 15, -1) == 15);         /* the floor */
    CHECK(frame_pacing_step(&p, 30, 1) == 60);
    CHECK(frame_pacing_step(&p, 60, 1) == 60);          /* never above the ask */
    CHECK(frame_pacing_fit(&p, 17 * MS) == 30);
    CHECK(frame_pacing_fit(&p, 16 * MS) == 60);
    CHECK(frame_pacing_fit(&p, 44 * MS) == 20);
    CHECK(frame_pacing_fit(&p, 200 * MS) == 15);

    /* The swap payload: interval bits 0-2 (DEFAULT 0 reads as ONE), IMMEDIATE bit 3. */
    frame_pacing_swap(&p, 0u);
    CHECK(p.cur_iv == 1);
    frame_pacing_swap(&p, 2u);
    CHECK(p.cur_iv == 2);
    frame_pacing_swap(&p, 8u);
    CHECK(p.cur_iv == 0);
    frame_pacing_swap(&p, 9u);                          /* ONE_OR_IMMEDIATE: ONE */
    CHECK(p.cur_iv == 1);
    frame_pacing_swap(&p, 0x1234560u | 2u);             /* framebuffer address bits above */
    CHECK(p.cur_iv == 2);

    /* 120 Hz panel, IMMEDIATE: base 120: 120, 60, 40, 30, 24, 20, 15. */
    frame_pacing_init(&p, 120, -1, 1);
    p.ask_iv = 0;
    CHECK(frame_pacing_ask(&p) == 120);
    CHECK(frame_pacing_step(&p, 120, -1) == 60);
    CHECK(frame_pacing_step(&p, 60, -1) == 40);
    CHECK(frame_pacing_step(&p, 40, -1) == 30);
    CHECK(frame_pacing_step(&p, 30, -1) == 24);
    CHECK(frame_pacing_step(&p, 20, -1) == 15);
    /* The same panel, a fixed interval: the vblank's 60, 30, 20, 15. */
    p.ask_iv = 1;
    CHECK(frame_pacing_ask(&p) == 60);
    CHECK(frame_pacing_step(&p, 60, -1) == 30);

    /* PAL-I: 50 Hz vblanks, measured. */
    frame_pacing_init(&p, 60, -1, 1);
    frame_pacing_vblank(&p, 0, 1 * SEC);
    frame_pacing_vblank(&p, 50, 2 * SEC);
    CHECK(p.vblank_hz == 50);
    CHECK(frame_pacing_ask(&p) == 50);
    CHECK(frame_pacing_step(&p, 50, -1) == 25);
    CHECK(frame_pacing_step(&p, 25, -1) == 25);         /* 50/3, 50/4 not integral; 10 under the floor */
    frame_pacing_vblank(&p, 50 + 60, 3 * SEC);
    CHECK(p.vblank_hz == 60);
    frame_pacing_vblank(&p, 110, 4 * SEC);              /* vblanks stopped: keep the rate */
    CHECK(p.vblank_hz == 60);

    /* The ask is the mode of the recent swaps. */
    frame_pacing_init(&p, 60, -1, 1);
    {
        uint64_t t = SEC;
        int i;
        for (i = 0; i < 40; i++) {
            frame_pacing_swap(&p, IV_TWO);
            frame_pacing_flip(&p, t += 33 * MS, 0);
        }
        CHECK(frame_pacing_ask(&p) == 30 && p.rate == 30);
        for (i = 0; i < 10; i++) {                      /* a few ONE: still TWO */
            frame_pacing_swap(&p, IV_ONE);
            frame_pacing_flip(&p, t += 33 * MS, 0);
        }
        CHECK(frame_pacing_ask(&p) == 30);
        for (i = 0; i < 60; i++) {
            frame_pacing_swap(&p, IV_ONE);
            frame_pacing_flip(&p, t += 16 * MS, 0);
        }
        CHECK(frame_pacing_ask(&p) == 60 && p.rate == 60);
    }
}

int main(void)
{
    static const uint32_t m60_30_60[] = {60, 30, 60};
    Result r;

    units();

    /* 10 ms, interval ONE: 60, judged locked (it holds its ask). */
    {
        Sim s = {"10 ms, ONE (RenderWare cap 60)", 60, 60, -1, 1, IV_ONE, c10, 60};
        r = run(&s, 5, NULL, 0);
        CHECK(r.rate == 60 && r.locked == 1 && r.changes_after == 0);
        CHECK(r.sd_ms >= 0 && r.sd_ms < 2.0 && fabs(r.mean_ms - 16.67) < 0.3);
    }
    /* 20 ms, interval ONE: at its ask until judged (20 s skip + 5 s), then
     * free and down to 30 within about a second. */
    {
        Sim s = {"20 ms, ONE (Fable cap 30)", 60, 60, -1, 1, IV_ONE, c20, 60};
        static const uint32_t m30[] = {30};
        r = run(&s, 30, m30, 1);
        CHECK(r.rate == 30 && r.locked == -1 && r.changes_after == 0);
        CHECK(r.t_rate[0] > 25.0 && r.t_rate[0] < 27.5);
        CHECK(r.sd_ms >= 0 && r.sd_ms < 2.0 && fabs(r.mean_ms - 33.33) < 0.5);
    }
    /* 20 ms, IMMEDIATE: free from the start, 30 within about a second. */
    {
        Sim s = {"20 ms, IMMEDIATE", 60, 60, -1, 1, IV_IMMEDIATE, c20, 40};
        static const uint32_t m30[] = {30};
        r = run(&s, 5, m30, 1);
        CHECK(r.rate == 30 && r.changes_after == 0 && r.t_rate[0] >= 0 && r.t_rate[0] < 1.6);
        CHECK(r.sd_ms >= 0 && r.sd_ms < 2.0);
    }
    /* 40 ms: 20, evenly. */
    {
        Sim s = {"40 ms, ONE", 60, 60, -1, 1, IV_ONE, c40, 60};
        r = run(&s, 30, NULL, 0);
        CHECK(r.rate == 20 && r.changes_after == 0);
        CHECK(r.sd_ms >= 0 && r.sd_ms < 2.0 && fabs(r.mean_ms - 50.0) < 0.5);
    }
    /* Varying 12-22 ms: 30 and stays there. */
    {
        Sim s = {"12-22 ms varying, IMMEDIATE", 60, 60, -1, 1, IV_IMMEDIATE, cvary, 120};
        r = run(&s, 5, NULL, 0);
        CHECK(r.rate == 30 && r.changes_after == 0);
        CHECK(r.sd_ms >= 0 && r.sd_ms < 2.0);
    }
    /* A steady 17 ms scene: 30, not 30-60-30. */
    {
        Sim s = {"17 ms steady", 60, 60, -1, 1, IV_IMMEDIATE, c17, 120};
        r = run(&s, 3, NULL, 0);
        CHECK(r.rate == 30 && r.changes_after == 0);
        CHECK(r.sd_ms >= 0 && r.sd_ms < 2.0);
    }
    /* 17 ms whose cost hides in the cap's sleep: each probe at 60 fails
     * within a second and the next waits twice as long (4, 8, 16, 32, 64 s):
     * six probes (12 changes) in 3 minutes, then one a minute. */
    {
        Sim s = {"17 ms, CPU-bound (probes back off)", 60, 60, -1, 1, IV_IMMEDIATE, c17_overlap, 180};
        r = run(&s, 3, NULL, 0);
        CHECK(r.rate == 30 && r.changes_after <= 12);
        CHECK(r.sd_ms >= 0 && r.sd_ms < 2.0);
    }
    /* A sustained change: 60 -> 30 within ~1.5 s of 40 s, back to 60 within
     * ~1.5 s of 60 s. */
    {
        Sim s = {"10 -> 25 -> 8 ms, IMMEDIATE", 60, 60, -1, 1, IV_IMMEDIATE, cstep, 80};
        r = run(&s, 0, m60_30_60, 3);
        CHECK(r.t_rate[1] >= 40.0 && r.t_rate[1] < 41.6);
        CHECK(r.t_rate[2] >= 60.0 && r.t_rate[2] < 61.6);
        CHECK(r.rate == 60 && r.sd_ms >= 0 && r.sd_ms < 2.0);
    }
    /* Flip-locked (Burnout 3): holds 60 through the front end, judged
     * locked, then an 18 ms race scene is NOT divided (it would run in slow
     * motion at 30): the rate stays 60. */
    {
        Sim s = {"flip-locked: 9 ms then 18 ms, ONE", 60, 60, -1, 1, IV_ONE, clocked, 70};
        r = run(&s, 1, NULL, 0);
        CHECK(r.locked == 1 && r.rate == 60 && r.changes_after == 0);
    }
    /* The same title with RECOMP_PACING_LOCK=0: divided to 30 at once. */
    {
        Sim s = {"same, RECOMP_PACING_LOCK=0", 60, 60, 0, 1, IV_ONE, clocked, 70};
        r = run(&s, 45, NULL, 0);
        CHECK(r.rate == 30 && r.changes_after == 0);
    }
    /* RECOMP_PACING_LOCK=1: a 20 ms ONE title stays at its ask. */
    {
        Sim s = {"20 ms, ONE, RECOMP_PACING_LOCK=1", 60, 60, 1, 1, IV_ONE, c20, 40};
        r = run(&s, 1, NULL, 0);
        CHECK(r.rate == 60 && r.changes_after == 0);
    }
    /* RECOMP_PACING=ask: the ask only, never divided. */
    {
        Sim s = {"20 ms, IMMEDIATE, RECOMP_PACING=ask", 60, 60, -1, 0, IV_IMMEDIATE, c20, 40};
        r = run(&s, 1, NULL, 0);
        CHECK(r.rate == 60 && r.changes_after == 0);
    }
    /* Interval TWO: the title asks for 30; 10 ms frames get 30, not 60. */
    {
        Sim s = {"10 ms, TWO", 60, 60, -1, 1, IV_TWO, c10, 40};
        r = run(&s, 1, NULL, 0);
        CHECK(r.rate == 30 && r.locked == 1 && r.sd_ms >= 0 && r.sd_ms < 2.0);
    }
    /* PAL-I 50 Hz: ONE asks 50; 30 ms frames go to 25 once judged free. */
    {
        Sim s = {"10 ms, ONE, PAL 50 Hz", 60, 50, -1, 1, IV_ONE, c10, 40};
        r = run(&s, 3, NULL, 0);
        CHECK(r.rate == 50 && r.changes_after == 0);
    }
    {
        Sim s = {"30 ms, ONE, PAL 50 Hz", 60, 50, -1, 1, IV_ONE, c30, 60};
        r = run(&s, 30, NULL, 0);
        CHECK(r.rate == 25 && r.changes_after == 0);
    }
    /* A 120 Hz panel (AYN Thor): a fixed interval keeps the vblank's
     * divisors (20 ms ONE: 30, every 4th refresh); IMMEDIATE goes up to the
     * panel's 120 (5 ms) or 60 (12 ms). */
    {
        Sim s = {"20 ms, ONE, 120 Hz panel", 120, 60, -1, 1, IV_ONE, c20, 60};
        r = run(&s, 30, NULL, 0);
        CHECK(r.rate == 30 && r.sd_ms >= 0 && r.sd_ms < 2.0);
    }
    {
        Sim s = {"5 ms, IMMEDIATE, 120 Hz panel", 120, 60, -1, 1, IV_IMMEDIATE, c5, 30};
        r = run(&s, 3, NULL, 0);
        CHECK(r.rate == 120 && r.sd_ms >= 0 && r.sd_ms < 2.0);
    }
    {
        Sim s = {"12 ms, IMMEDIATE, 120 Hz panel", 120, 60, -1, 1, IV_IMMEDIATE, c12, 30};
        r = run(&s, 3, NULL, 0);
        CHECK(r.rate == 60 && r.changes_after == 0 && r.sd_ms >= 0 && r.sd_ms < 2.0);
    }

    if (failures)
        fprintf(stderr, "frame_pacing: %d failure(s)\n", failures);
    else
        puts("frame_pacing: ok");
    return failures ? 1 : 0;
}
