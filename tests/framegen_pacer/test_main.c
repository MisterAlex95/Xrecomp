/* tests/framegen_pacer: the frame generation pacer (src/framegen) with the
 * fake generator and a fake clock: scripted flips (steady 30 fps, a 50 ms
 * frame, a stall, a pause) and the due times, drop, skip, fallback and reset
 * rules from framegen_pacer.h; plus the cap / present-mode policy. */
#define _POSIX_C_SOURCE 200809L   /* clock_gettime, nanosleep, setenv */
#include "framegen_pacer.h"
#include "framegen_policy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int s_fail;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); s_fail++; } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { fprintf(stderr, "%s:%d: %s = %lld, expected %lld\n", __FILE__, __LINE__, #a, _a, _b); s_fail++; } } while (0)

#define MS 1000000ull
#define CAP_PERIOD (1000000000ull / 30)     /* 33 333 333 ns */
#define W 8
#define H 4

/* ── Fake clock ─────────────────────────────────────────────────────── */
static uint64_t s_clock;
static uint64_t clk(void *u) { (void)u; return __atomic_load_n(&s_clock, __ATOMIC_SEQ_CST); }
static void clk_set(uint64_t t) { __atomic_store_n(&s_clock, t, __ATOMIC_SEQ_CST); }

/* ── Test generator: the fake one, plus a cost, failures, a gate ──── */
static uint64_t s_cost = 5 * MS;
static int s_gen_error;                 /* generate() returns this when < 0 */
static int s_open_error;
static int s_generate_calls, s_prime_calls, s_open_calls;

static int t_open(void *u, uint32_t w, uint32_t h, uint32_t m, char *err, size_t n)
{
    (void)u;
    s_open_calls++;
    if (s_open_error) {
        snprintf(err, n, "no vulkan");
        return -1;
    }
    return framegen_fake_generator.open(framegen_fake_generator.user, w, h, m, err, n);
}

static int t_generate(void *u, const uint8_t *f, uint32_t pitch, uint8_t *const *out, uint32_t op)
{
    (void)u;
    if (!out) {
        s_prime_calls++;
        return framegen_fake_generator.generate(framegen_fake_generator.user, f, pitch, out, op);
    }
    s_generate_calls++;
    clk_set(clk(NULL) + s_cost);
    if (s_gen_error < 0)
        return s_gen_error;
    return framegen_fake_generator.generate(framegen_fake_generator.user, f, pitch, out, op);
}

static void t_reset(void *u) { (void)u; framegen_fake_generator.reset(framegen_fake_generator.user); }
static void t_close(void *u) { (void)u; framegen_fake_generator.close(framegen_fake_generator.user); }

static const FramegenGenerator s_gen = { "test", NULL, t_open, t_generate, t_reset, t_close };

/* ── Sink: records what was shown ───────────────────────────────────── */
typedef struct Shown { uint8_t v; uint8_t px4[4]; uint64_t due; uint32_t bpp, w, h; } Shown;
static Shown s_shown[512];
static _Atomic int s_nshown;
static _Atomic int s_gate_show;       /* show() waits while set */
static _Atomic int s_in_show;

static void sink_show(const uint8_t *px, uint32_t w, uint32_t h, uint32_t bpp, uint64_t due)
{
    s_in_show = 1;
    while (s_gate_show) {
        struct timespec ts = { 0, 1000000 };
        nanosleep(&ts, NULL);
    }
    s_in_show = 0;
    if (s_nshown < 512) {
        s_shown[s_nshown].v = px[0];
        memcpy(s_shown[s_nshown].px4, px, 4);
        s_shown[s_nshown].w = w;
        s_shown[s_nshown].h = h;
        s_shown[s_nshown].due = due;
        s_shown[s_nshown].bpp = bpp;
        s_nshown++;
    }
}
static const FramegenSink s_sink = { sink_show };

static uint8_t s_px[W * H * 4];

/* Submit a frame of value v flipped at t (the clock is moved to t first,
 * unless it is already past: a frame that waited in the queue). */
static void flip(uint8_t v, uint64_t t)
{
    memset(s_px, v, sizeof s_px);
    if (clk(NULL) < t)
        clk_set(t);
    framegen_pacer_submit(s_px, W, H, 4, W * 4, t);
}

static void start(uint32_t mult, int threaded)
{
    FramegenPacerConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.gen = &s_gen;
    cfg.multiplier = mult;
    cfg.cap_fps = 30;
    cfg.threaded = threaded;
    cfg.now_ns = clk;
    s_nshown = 0;
    s_cost = 5 * MS;
    s_gen_error = s_open_error = 0;
    s_generate_calls = s_prime_calls = s_open_calls = 0;
    clk_set(1000 * MS);
    CHECK_EQ(framegen_pacer_start_ex(&s_sink, &cfg), 0);
}

static FramegenPacerCounters counters(void)
{
    FramegenPacerCounters c;
    framegen_pacer_counters(&c);
    return c;
}

static uint8_t blend(uint8_t a, uint8_t b, uint32_t k, uint32_t m)
{
    return (uint8_t)((a * (m - k) + b * k + m / 2) / m);
}

/* ── Scenarios ──────────────────────────────────────────────────────── */

static void test_steady_30(void)
{
    const uint64_t t0 = 1000 * MS;
    start(2, 0);
    for (int i = 0; i <= 40; i++)
        flip((uint8_t)(i * 4), t0 + (uint64_t)i * CAP_PERIOD);
    FramegenPacerCounters c = counters();
    CHECK_EQ(c.submitted, 41);
    CHECK_EQ(c.generated, 40);
    CHECK_EQ(c.shown_real, 41);
    CHECK_EQ(c.shown_generated, 40);
    CHECK_EQ(c.dropped_generated + c.dropped_real + c.skipped + c.failed + c.over_budget, 0);
    CHECK_EQ(c.period_ns, CAP_PERIOD);
    CHECK_EQ(c.last_cost_ns, 5 * MS);
    CHECK_EQ(s_prime_calls, 1);
    CHECK_EQ(s_nshown, 81);
    /* Frame 0 only primes and is due at its flip. */
    CHECK_EQ(s_shown[0].v, 0);
    CHECK_EQ(s_shown[0].due, t0);
    for (int i = 1; i <= 40; i++) {
        uint64_t T = t0 + (uint64_t)i * CAP_PERIOD;
        const Shown *g = &s_shown[2 * i - 1], *n = &s_shown[2 * i];
        CHECK_EQ(g->v, blend((uint8_t)((i - 1) * 4), (uint8_t)(i * 4), 1, 2));
        CHECK_EQ(g->due, T + 5 * MS);
        CHECK_EQ(n->v, (uint8_t)(i * 4));
        CHECK_EQ(n->due, T + 5 * MS + CAP_PERIOD / 2);
        CHECK_EQ(g->bpp, 4);
    }
    uint32_t real, shown, gen;
    framegen_pacer_stats(&real, &shown, &gen);
    printf("steady 30: real %u.%02u fps, shown %u.%02u fps, gen %u us\n",
           real / 100, real % 100, shown / 100, shown % 100, gen);
    CHECK(real >= 2990 && real <= 3010);
    CHECK(shown >= 5900 && shown <= 6100);
    CHECK_EQ(gen, 5000);
    CHECK(framegen_pacer_active());
    framegen_pacer_stop();
    CHECK(!framegen_pacer_active());
}

static void test_r5g6b5(void)
{
    uint16_t px[W * H];
    start(2, 0);
    for (int i = 0; i < W * H; i++)
        px[i] = 0xF800;         /* pure red */
    framegen_pacer_submit((const uint8_t *)px, W, H, 2, W * 2, 1000 * MS);
    for (int i = 0; i < W * H; i++)
        px[i] = 0x001F;         /* pure blue */
    framegen_pacer_submit((const uint8_t *)px, W, H, 2, W * 2, 1000 * MS + CAP_PERIOD);
    CHECK_EQ(s_nshown, 3);
    CHECK_EQ(s_shown[0].bpp, 4);
    CHECK_EQ(s_shown[0].v, 0);            /* B of red */
    CHECK_EQ(s_shown[1].v, 128);          /* B halfway */
    CHECK_EQ(s_shown[2].v, 255);          /* B of blue */
    framegen_pacer_stop();
}

/* RGBX (GLES read-backs) and X1R5G5B5 come out as BGRX; a frame of another
 * size (a host frame at the internal resolution) reopens the generator and
 * only primes it. */
static void test_formats_and_size(void)
{
    uint8_t rgbx[W * H * 4];
    uint16_t x555[W * H];
    static uint8_t big[2 * W * 2 * H * 4];
    start(2, 0);
    for (int i = 0; i < W * H; i++) {
        rgbx[4 * i + 0] = 0xFF;   /* R */
        rgbx[4 * i + 1] = 0x40;   /* G */
        rgbx[4 * i + 2] = 0x00;   /* B */
        rgbx[4 * i + 3] = 0x00;   /* X: ignored */
    }
    framegen_pacer_submit_format(rgbx, W, H, FRAMEGEN_FMT_RGBX8888, W * 4, 1000 * MS);
    CHECK_EQ(s_nshown, 1);
    CHECK_EQ(s_shown[0].px4[0], 0x00);
    CHECK_EQ(s_shown[0].px4[1], 0x40);
    CHECK_EQ(s_shown[0].px4[2], 0xFF);
    CHECK_EQ(s_shown[0].px4[3], 0xFF);
    for (int i = 0; i < W * H; i++)
        x555[i] = 0x7C00 | 0x8000;   /* red, X bit set */
    framegen_pacer_submit_format((const uint8_t *)x555, W, H, FRAMEGEN_FMT_X1R5G5B5, W * 2,
                                 1000 * MS + CAP_PERIOD);
    CHECK_EQ(s_nshown, 3);
    CHECK_EQ(s_shown[1].px4[2], 0xFF);    /* G(red, red) */
    CHECK_EQ(s_shown[2].px4[0], 0x00);
    CHECK_EQ(s_shown[2].px4[1], 0x00);
    CHECK_EQ(s_shown[2].px4[2], 0xFF);
    CHECK_EQ(s_open_calls, 1);
    /* Bad formats and sizes are ignored. */
    framegen_pacer_submit_format(rgbx, W, H, 99, W * 4, 1000 * MS + 2 * CAP_PERIOD);
    framegen_pacer_submit_format(rgbx, W, H, FRAMEGEN_FMT_RGBX8888, W * 2, 1000 * MS + 2 * CAP_PERIOD);
    CHECK_EQ(counters().submitted, 2);
    /* Twice the size: reopened, primed, then generating at that size. */
    memset(big, 0x20, sizeof big);
    framegen_pacer_submit_format(big, 2 * W, 2 * H, FRAMEGEN_FMT_BGRX8888, 2 * W * 4,
                                 1000 * MS + 2 * CAP_PERIOD);
    CHECK_EQ(s_open_calls, 2);
    CHECK_EQ(s_nshown, 4);
    CHECK_EQ(s_shown[3].w, 2 * W);
    CHECK_EQ(s_shown[3].h, 2 * H);
    CHECK_EQ(s_shown[3].due, 1000 * MS + 2 * CAP_PERIOD);
    memset(big, 0x40, sizeof big);
    framegen_pacer_submit_format(big, 2 * W, 2 * H, FRAMEGEN_FMT_BGRX8888, 2 * W * 4,
                                 1000 * MS + 3 * CAP_PERIOD);
    CHECK_EQ(s_nshown, 6);
    CHECK_EQ(s_shown[4].v, 0x30);
    CHECK_EQ(s_shown[4].w, 2 * W);
    CHECK_EQ(s_shown[5].v, 0x40);
    CHECK(framegen_pacer_active());
    framegen_pacer_stop();
}

static void test_slow_frame(void)
{
    uint64_t t = 1000 * MS;
    start(2, 0);
    for (int i = 0; i <= 8; i++, t += CAP_PERIOD)
        flip((uint8_t)i, t);
    t -= CAP_PERIOD;
    /* One 50 ms frame: generated, P follows the average (7 x 33.3 + 50) / 8. */
    t += 50 * MS;
    flip(100, t);
    FramegenPacerCounters c = counters();
    uint64_t p = (7 * CAP_PERIOD + 50 * MS) / 8;
    CHECK_EQ(c.period_ns, p);
    CHECK_EQ(c.skipped, 0);
    CHECK_EQ(c.generated, 9);
    CHECK_EQ(s_shown[s_nshown - 2].due, t + 5 * MS);
    CHECK_EQ(s_shown[s_nshown - 1].due, t + 5 * MS + p / 2);
    printf("50 ms frame: P %.2f ms, G at +5.00 ms, N at +%.2f ms\n", p / 1e6, (5 * MS + p / 2) / 1e6);
    framegen_pacer_stop();
}

static void test_skip(void)
{
    uint64_t t = 1000 * MS;
    start(2, 0);
    flip(0, t);
    flip(10, t += CAP_PERIOD);
    /* 70 ms > 2 x cap period: no generation, N due at its flip, not averaged. */
    flip(20, t += 70 * MS);
    FramegenPacerCounters c = counters();
    CHECK_EQ(c.skipped, 1);
    CHECK_EQ(c.generated, 1);
    CHECK_EQ(c.period_ns, CAP_PERIOD);
    CHECK_EQ(s_shown[s_nshown - 1].v, 20);
    CHECK_EQ(s_shown[s_nshown - 1].due, t);
    CHECK_EQ(s_prime_calls, 2);     /* frame 0 and the skipped frame */
    /* The next pair generates from the skipped frame: G(20, 30). */
    flip(30, t += CAP_PERIOD);
    CHECK_EQ(s_shown[s_nshown - 2].v, 25);
    CHECK_EQ(counters().generated, 2);
    /* Exactly 2 x cap period still generates. */
    flip(40, t += 2 * CAP_PERIOD);
    CHECK_EQ(counters().skipped, 1);
    framegen_pacer_stop();
}

static void test_drop_late_generated(void)
{
    uint64_t t = 1000 * MS;
    start(2, 0);
    flip(0, t);
    flip(10, t += CAP_PERIOD);
    /* The frame waited 40 ms (> P) before the pacer got to it: G is due
     * 40 + 5 - 5 = 40 ms ago -> dropped; N (late 40 - 16.7 ms) shown. */
    t += CAP_PERIOD;
    clk_set(t + 40 * MS);
    flip(20, t);
    FramegenPacerCounters c = counters();
    CHECK_EQ(c.dropped_generated, 1);
    CHECK_EQ(c.dropped_real, 0);
    CHECK_EQ(s_shown[s_nshown - 1].v, 20);
    /* 60 ms late: N is more than P late too, but nothing newer is queued,
     * so it is still shown (a real frame is never dropped for nothing). */
    t += CAP_PERIOD;
    clk_set(t + 60 * MS);
    flip(30, t);
    c = counters();
    CHECK_EQ(c.dropped_generated, 2);
    CHECK_EQ(c.dropped_real, 0);
    CHECK_EQ(s_shown[s_nshown - 1].v, 30);
    /* Within one period: kept. */
    t += CAP_PERIOD;
    clk_set(t + 25 * MS);
    flip(40, t);
    CHECK_EQ(counters().dropped_generated, 2);
    framegen_pacer_stop();
}

static int wait_for(int (*cond)(void))
{
    for (int i = 0; i < 5000; i++) {
        if (cond())
            return 1;
        struct timespec ts = { 0, 1000000 };
        nanosleep(&ts, NULL);
    }
    return 0;
}
static int in_show(void) { return s_in_show; }
static int all_handled(void)
{
    FramegenPacerCounters c = counters();
    return c.shown_real + c.dropped_real + c.dropped_input >= 4;
}

static void test_drop_threaded(void)
{
    uint64_t t0 = 1000 * MS, iv = 30 * MS;
    start(2, 1);
    /* The presenter is slow to take frame 0; meanwhile 1, 2 queue and 3
     * pushes 1 out. By the time the thread gets on, everything is 200 ms
     * late: generated frames are dropped, real ones too while a newer one
     * is queued, the newest real one is shown. */
    s_gate_show = 1;
    flip(0, t0);
    CHECK(wait_for(in_show));
    flip(1, t0 + iv);
    flip(2, t0 + 2 * iv);
    flip(3, t0 + 3 * iv);
    clk_set(t0 + 3 * iv + 200 * MS);
    s_gate_show = 0;
    CHECK(wait_for(all_handled));
    FramegenPacerCounters c = counters();
    printf("threaded behind: dropped input %llu, real %llu, generated %llu; shown real %llu\n",
           (unsigned long long)c.dropped_input, (unsigned long long)c.dropped_real,
           (unsigned long long)c.dropped_generated, (unsigned long long)c.shown_real);
    CHECK_EQ(c.dropped_input, 1);       /* frame 1 */
    CHECK_EQ(c.dropped_generated, 2);   /* G(0,2), G(2,3) */
    CHECK_EQ(c.dropped_real, 1);        /* frame 2: late, 3 queued */
    CHECK_EQ(c.shown_real, 2);          /* 0, 3 */
    CHECK_EQ(c.shown_generated, 0);
    CHECK_EQ(c.generated, 2);
    framegen_pacer_stop();
    CHECK_EQ(s_nshown, 2);
    CHECK_EQ(s_shown[0].v, 0);
    CHECK_EQ(s_shown[1].v, 3);
}

static void test_fallback_over_budget(void)
{
    uint64_t t = 1000 * MS;
    start(2, 0);
    flip(0, t);
    s_cost = 20 * MS;                   /* budget: 33.3 / 2 = 16.7 ms */
    for (int i = 1; i <= 29; i++)
        flip((uint8_t)i, t += CAP_PERIOD);
    CHECK(framegen_pacer_active());
    s_cost = 5 * MS;                    /* one good frame restarts the count */
    flip(30, t += CAP_PERIOD);
    s_cost = 20 * MS;
    for (int i = 1; i <= 29; i++)
        flip((uint8_t)(30 + i), t += CAP_PERIOD);
    CHECK(framegen_pacer_active());
    flip(60, t += CAP_PERIOD);          /* 30th in a row */
    CHECK(!framegen_pacer_active());
    printf("fallback: %s\n", framegen_pacer_off_reason());
    CHECK(strstr(framegen_pacer_off_reason(), "over budget 30 frames in a row") != NULL);
    /* Pass-through: no generation, each frame due at its flip. */
    int calls = s_generate_calls;
    flip(61, t += CAP_PERIOD);
    CHECK_EQ(s_generate_calls, calls);
    CHECK_EQ(s_shown[s_nshown - 1].v, 61);
    CHECK_EQ(s_shown[s_nshown - 1].due, t);
    CHECK_EQ(counters().over_budget, 59);
    framegen_pacer_stop();
}

static void test_fallback_failed(void)
{
    uint64_t t = 1000 * MS;
    start(2, 0);
    flip(0, t);
    s_gen_error = -5;
    for (int i = 1; i <= 30; i++) {
        flip((uint8_t)i, t += CAP_PERIOD);
        /* A failed generation still shows the real frame, at its flip. */
        CHECK_EQ(s_shown[s_nshown - 1].v, (uint8_t)i);
        CHECK_EQ(s_shown[s_nshown - 1].due, t);
    }
    CHECK(!framegen_pacer_active());
    CHECK(strstr(framegen_pacer_off_reason(), "failed 30 frames in a row (error -5)") != NULL);
    CHECK_EQ(counters().failed, 30);
    framegen_pacer_stop();
}

static void test_open_failure(void)
{
    start(2, 0);
    s_open_error = 1;
    flip(7, 1000 * MS);
    CHECK(!framegen_pacer_active());
    CHECK(strcmp(framegen_pacer_off_reason(), "test: no vulkan") == 0);
    CHECK_EQ(s_nshown, 1);
    CHECK_EQ(s_shown[0].v, 7);
    framegen_pacer_stop();
}

static void test_pause(void)
{
    uint64_t t = 1000 * MS;
    /* Without a reset, a 2 s gap is a stall: skipped. */
    start(2, 0);
    for (int i = 0; i < 10; i++, t += CAP_PERIOD)
        flip((uint8_t)i, t);
    flip(50, t += 2000 * MS);
    CHECK_EQ(counters().skipped, 1);
    framegen_pacer_stop();

    /* With the lifecycle reset: history gone, the next frame primes. */
    t = 1000 * MS;
    start(2, 0);
    for (int i = 0; i < 10; i++, t += CAP_PERIOD)
        flip((uint8_t)i, t);
    t -= CAP_PERIOD;
    t += 40 * MS;
    flip(10, t);                        /* P now above the cap period */
    CHECK(counters().period_ns > CAP_PERIOD);
    framegen_pacer_reset();
    int primes = s_prime_calls;
    flip(50, t += 2000 * MS);
    FramegenPacerCounters c = counters();
    CHECK_EQ(c.skipped, 0);
    CHECK_EQ(c.resets, 1);
    CHECK_EQ(s_prime_calls, primes + 1);
    CHECK_EQ(s_shown[s_nshown - 1].v, 50);
    CHECK_EQ(s_shown[s_nshown - 1].due, t);
    /* No ghost of the frame before the pause: G(50, 60), P back to the cap. */
    flip(60, t += CAP_PERIOD);
    c = counters();
    CHECK_EQ(s_shown[s_nshown - 2].v, 55);
    CHECK_EQ(c.period_ns, CAP_PERIOD);
    framegen_pacer_stop();
}

static void test_multiplier_3(void)
{
    uint64_t t = 1000 * MS;
    start(3, 0);
    flip(0, t);
    flip(90, t += CAP_PERIOD);
    CHECK_EQ(s_nshown, 4);
    CHECK_EQ(s_shown[1].v, 30);
    CHECK_EQ(s_shown[2].v, 60);
    CHECK_EQ(s_shown[3].v, 90);
    CHECK_EQ(s_shown[1].due, t + 5 * MS);
    CHECK_EQ(s_shown[2].due, t + 5 * MS + CAP_PERIOD / 3);
    CHECK_EQ(s_shown[3].due, t + 5 * MS + 2 * CAP_PERIOD / 3);
    framegen_pacer_stop();
}

static void test_policy(void)
{
    CHECK_EQ(framegen_policy_cap_rule(30, 60, 2), 30);
    CHECK_EQ(framegen_policy_cap_rule(60, 60, 2), 30);
    CHECK_EQ(framegen_policy_cap_rule(0, 60, 2), 30);
    CHECK_EQ(framegen_policy_cap_rule(20, 60, 2), 20);
    CHECK_EQ(framegen_policy_cap_rule(0, 144, 2), 72);
    CHECK_EQ(framegen_policy_cap_rule(60, 120, 4), 30);
    unsetenv("RECOMP_LSFG");
    CHECK_EQ(framegen_policy_fps_cap(60), 60);
    CHECK_EQ(framegen_policy_fps_cap(0), 0);
    CHECK(!framegen_policy_force_present_sync());
    setenv("RECOMP_LSFG", "0", 1);
    CHECK_EQ(framegen_policy_fps_cap(60), 60);
    setenv("RECOMP_LSFG", "1", 1);
    CHECK_EQ(framegen_policy_fps_cap(60), 30);
    CHECK_EQ(framegen_policy_fps_cap(0), 30);
    CHECK(framegen_policy_force_present_sync());
    setenv("RECOMP_LSFG_DISPLAY_HZ", "90", 1);
    setenv("RECOMP_LSFG_MULT", "3", 1);
    CHECK_EQ(framegen_policy_fps_cap(60), 30);
    CHECK_EQ(framegen_policy_fps_cap(25), 25);
    unsetenv("RECOMP_LSFG_DISPLAY_HZ");
    unsetenv("RECOMP_LSFG_MULT");
    setenv("RECOMP_GPU_FPS_CAP", "45", 1);
    CHECK_EQ(framegen_policy_env_fps_cap(), 45);
    unsetenv("RECOMP_GPU_FPS_CAP");
    unsetenv("RECOMP_LSFG");
}

static void test_env_start(void)
{
    /* framegen_pacer_start(): off unless RECOMP_LSFG; no generator -> off;
     * RECOMP_LSFG_FAKE -> the fake generator on the real clock and thread. */
    unsetenv("RECOMP_LSFG");
    CHECK(framegen_pacer_start(&s_sink) < 0);
    setenv("RECOMP_LSFG", "1", 1);
    unsetenv("RECOMP_LSFG_FAKE");
    framegen_pacer_set_generator(NULL);
    CHECK(framegen_pacer_start(&s_sink) < 0);
    setenv("RECOMP_LSFG_FAKE", "1", 1);
    s_nshown = 0;
    CHECK_EQ(framegen_pacer_start(&s_sink), 0);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t t = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    memset(s_px, 0, sizeof s_px);
    framegen_pacer_submit(s_px, W, H, 4, W * 4, t);
    memset(s_px, 200, sizeof s_px);
    framegen_pacer_submit(s_px, W, H, 4, W * 4, t + CAP_PERIOD);
    for (int i = 0; i < 2000 && s_nshown < 3; i++) {
        struct timespec d = { 0, 1000000 };
        nanosleep(&d, NULL);
    }
    CHECK_EQ(s_nshown, 3);
    CHECK_EQ(s_shown[1].v, 100);
    framegen_pacer_stop();
    unsetenv("RECOMP_LSFG");
    unsetenv("RECOMP_LSFG_FAKE");
}

int main(void)
{
    test_policy();
    test_steady_30();
    test_r5g6b5();
    test_formats_and_size();
    test_slow_frame();
    test_skip();
    test_drop_late_generated();
    test_drop_threaded();
    test_fallback_over_budget();
    test_fallback_failed();
    test_open_failure();
    test_pause();
    test_multiplier_3();
    test_env_start();
    if (s_fail) {
        fprintf(stderr, "framegen_pacer: %d check(s) failed\n", s_fail);
        return 1;
    }
    printf("framegen_pacer: all checks passed\n");
    return 0;
}
