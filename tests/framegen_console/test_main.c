/* tests/framegen_console: the console window's frame generation front end (src/framegen/framegen_console.c,
 * XBA-186) over the real pacer with the fake generator and a fake clock: a console presenting at 60 Hz, games that
 * draw a new picture every 2nd or 3rd field (distinct-frame detection, generated frames filling the repeats at even
 * intervals), a game at 60 (pass-through), fast-forward (bypass, fresh history after), a state load and a pause
 * (reset), the size cap, a failing generator (fallback) and the hash. */
#define _POSIX_C_SOURCE 200809L
#include "framegen_console.h"
#include "framegen_pacer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int s_fail;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); s_fail++; } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { fprintf(stderr, "%s:%d: %s = %lld, expected %lld\n", __FILE__, __LINE__, #a, _a, _b); s_fail++; } } while (0)
#define CHECK_NEAR(a, b, tol) do { long long _a = (long long)(a), _b = (long long)(b); \
    if (_a - _b > (long long)(tol) || _b - _a > (long long)(tol)) { \
        fprintf(stderr, "%s:%d: %s = %lld, expected %lld +- %lld\n", __FILE__, __LINE__, #a, _a, _b, (long long)(tol)); \
        s_fail++; } } while (0)

#define MS 1000000ull
#define FIELD 16666667ull   /* a 60 Hz console field */
#define COST (2 * MS)       /* the generator's cost per pair */

/* ── Fake clock and generator (the fake blend, plus a cost and failures) ── */
static uint64_t s_clock;
static uint64_t clk(void *u) { (void)u; return s_clock; }
static int s_gen_error;

static int t_open(void *u, uint32_t w, uint32_t h, uint32_t m, char *err, size_t n)
{
    (void)u;
    return framegen_fake_generator.open(framegen_fake_generator.user, w, h, m, err, n);
}

static int t_generate(void *u, const uint8_t *f, uint32_t pitch, uint8_t *const *out, uint32_t op)
{
    (void)u;
    if (out) {
        s_clock += COST;
        if (s_gen_error)
            return -1;
    }
    return framegen_fake_generator.generate(framegen_fake_generator.user, f, pitch, out, op);
}

static void t_reset(void *u) { (void)u; framegen_fake_generator.reset(framegen_fake_generator.user); }
static void t_close(void *u) { (void)u; framegen_fake_generator.close(framegen_fake_generator.user); }
static const FramegenGenerator s_gen = {"test", NULL, t_open, t_generate, t_reset, t_close};

static void start(uint32_t mult, uint32_t max_width)
{
    FramegenConsoleConfig c;
    memset(&c, 0, sizeof c);
    c.pacer.gen = &s_gen;
    c.pacer.multiplier = mult;
    c.pacer.cap_fps = 60 / mult;   /* the cap rule at 60 Hz */
    c.pacer.budget_us = 1000000;   /* never over budget here */
    c.pacer.threaded = 0;          /* the pacer runs inside fgc_frame */
    c.pacer.now_ns = clk;
    c.max_width = max_width;
    s_clock = 1000 * MS;
    s_gen_error = 0;
    CHECK_EQ(fgc_start(&c), 0);
}

/* ── A console picture: every pixel (v, v, v, 255) ── */
#define W 32
#define H 8
static uint8_t s_px[W * H * 4];
static void fill(uint8_t v)
{
    for (int i = 0; i < W * H; i++) {
        s_px[i * 4 + 0] = s_px[i * 4 + 1] = s_px[i * 4 + 2] = v;
        s_px[i * 4 + 3] = 255;
    }
}

/* ── The window: takes what is due every half millisecond until the next field ── */
typedef struct Drawn { uint64_t due; int generated; uint8_t v; int w, h; } Drawn;
static Drawn s_drawn[1024];
static int s_ndrawn;

static void drain_until(uint64_t t)
{
    FgcShown e;
    for (uint64_t now = s_clock; now < t; now += MS / 2) {
        if (fgc_take(now, &e) && s_ndrawn < 1024) {
            CHECK(e.due_ns <= now + MS);
            s_drawn[s_ndrawn++] = (Drawn){e.due_ns, e.generated, e.px[0], e.w, e.h};
        }
    }
    if (t > s_clock)
        s_clock = t;
}

/* n fields; the game draws a new picture every `every` fields (value 4, 8, 12, ...). Returns the results. */
static void run_fields(int n, int every, uint64_t *t, uint8_t *v, int *res, unsigned flags)
{
    for (int i = 0; i < n; i++) {
        if (i % every == 0)
            *v = (uint8_t)(*v + 4);
        fill(*v);
        if (s_clock < *t)
            s_clock = *t;
        res[i] = fgc_frame(s_px, W, H, W * 4, *t, flags);
        *t += FIELD;
        drain_until(*t);
    }
}

/* 30 fps at 60 Hz, 2x: distinct pictures only reach the pacer, a generated frame fills each repeat, 60 shown per
 * second, evenly 16.7 ms apart, G the midpoint of its neighbours, a real frame shown P/2 + c after its present. */
static void test_30fps_fills_repeats(void)
{
    int res[120];
    uint64_t t = 1000 * MS;
    uint8_t v = 0;
    FramegenPacerCounters p;
    FgcCounters c;

    start(2, 0);
    s_ndrawn = 0;
    run_fields(120, 2, &t, &v, res, 0);
    for (int i = 0; i < 120; i++)
        CHECK_EQ(res[i], i % 2 == 0 ? FGC_QUEUED : FGC_REPEAT);
    framegen_pacer_counters(&p);
    fgc_counters(&c);
    CHECK_EQ(p.submitted, 60);
    CHECK_EQ(c.distinct, 60);
    CHECK_EQ(c.repeats, 60);
    CHECK_EQ(p.generated, 59);   /* the first frame only primes */
    CHECK_EQ(c.late, 0);
    CHECK_EQ(c.passthrough, 0);
    CHECK_NEAR(c.distinct_period_ns, 2 * FIELD, 1000);
    /* What the window drew: the first real frame, then G, N alternating; 119 frames for 120 fields. */
    CHECK_EQ(s_ndrawn, 119);
    CHECK_EQ(s_drawn[0].generated, 0);
    CHECK_EQ(s_drawn[0].v, 4);
    for (int i = 1; i < s_ndrawn; i++) {
        CHECK_EQ(s_drawn[i].generated, i % 2 == 1);
        if (i % 2 == 1)   /* G(N-1, N): the blend */
            CHECK_EQ(s_drawn[i].v, (uint8_t)(4 * ((i + 1) / 2) + 2));
        else
            CHECK_EQ(s_drawn[i].v, (uint8_t)(4 * (i / 2 + 1)));
        if (i >= 2)   /* even intervals: one field */
            CHECK_NEAR(s_drawn[i].due - s_drawn[i - 1].due, FIELD, 2000);
    }
    /* Latency: real frame k (present at 1000 ms + 2k fields) shown at its present + P/2 + c. */
    CHECK_NEAR(s_drawn[4].due, 1000 * MS + 4 * FIELD + FIELD + COST, 2000);
    /* 60 shown in a second once running. */
    int in_second = 0;
    for (int i = 0; i < s_ndrawn; i++)
        in_second += s_drawn[i].due >= 1100 * MS && s_drawn[i].due < 2100 * MS;
    CHECK_NEAR(in_second, 60, 1);
    fgc_stop();
}

/* 20 fps at 60 Hz: 2x gives 40 shown, 25 ms apart; 3x gives 60, one field apart. */
static void test_20fps(void)
{
    int res[90];
    for (uint32_t m = 2; m <= 3; m++) {
        uint64_t t = 1000 * MS;
        uint8_t v = 0;
        start(m, 0);
        s_ndrawn = 0;
        run_fields(90, 3, &t, &v, res, 0);
        FramegenPacerCounters p;
        framegen_pacer_counters(&p);
        CHECK_EQ(p.submitted, 30);
        CHECK_EQ(p.skipped, 0);
        CHECK_EQ(s_ndrawn, 1 + 29 * (int)m);
        for (int i = 2; i < s_ndrawn; i++)
            CHECK_NEAR(s_drawn[i].due - s_drawn[i - 1].due, m == 2 ? 25 * MS : FIELD, 2000);
        fgc_stop();
    }
}

/* A game at 60 distinct fps: pass-through after 4 intervals (drawn directly, nothing more to the pacer); when it
 * drops to 30, frame generation again from a fresh history. */
static void test_passthrough(void)
{
    int res[64];
    uint64_t t = 1000 * MS;
    uint8_t v = 0;
    FgcCounters c;
    FramegenPacerCounters p;

    start(2, 0);
    s_ndrawn = 0;
    run_fields(20, 1, &t, &v, res, 0);
    for (int i = 0; i < 4; i++)
        CHECK_EQ(res[i], FGC_QUEUED);
    for (int i = 4; i < 20; i++)
        CHECK_EQ(res[i], FGC_DIRECT);
    fgc_counters(&c);
    CHECK_EQ(c.passthrough, 1);
    CHECK_EQ(c.passthrough_on, 1);
    CHECK_EQ(fgc_next_due(), 0);
    framegen_pacer_counters(&p);
    CHECK_EQ(p.submitted, 4);
    /* Down to 30: 4 slow intervals bring the mean back over 3/4 of the cap period. */
    run_fields(40, 2, &t, &v, res, 0);
    int back = -1;
    for (int i = 0; i < 40; i += 2)
        if (res[i] == FGC_QUEUED && back < 0)
            back = i;
    CHECK(back > 0 && back <= 10);
    for (int i = back; i < 40; i++)
        CHECK_EQ(res[i], i % 2 == 0 ? FGC_QUEUED : FGC_REPEAT);
    fgc_counters(&c);
    CHECK_EQ(c.passthrough, 0);
    CHECK(c.resets >= 1);
    fgc_stop();
}

/* Fast-forward: every picture drawn directly, the pacer sees none; after it the history starts again (the first
 * frame only primes: no generated frame blends a picture from before with one after). */
static void test_fast_forward(void)
{
    int res[40];
    uint64_t t = 1000 * MS;
    uint8_t v = 0;
    FramegenPacerCounters p0, p1;

    start(2, 0);
    s_ndrawn = 0;
    run_fields(20, 2, &t, &v, res, 0);
    framegen_pacer_counters(&p0);
    run_fields(20, 1, &t, &v, res, FGC_BYPASS);
    for (int i = 0; i < 20; i++)
        CHECK_EQ(res[i], FGC_DIRECT);
    framegen_pacer_counters(&p1);
    CHECK_EQ(p1.submitted, p0.submitted);
    CHECK_EQ(fgc_next_due(), 0);
    int before = s_ndrawn;
    uint64_t t_back = t;
    run_fields(8, 2, &t, &v, res, 0);
    CHECK_EQ(res[0], FGC_QUEUED);
    framegen_pacer_counters(&p1);
    CHECK_EQ(p1.resets, p0.resets + 1);
    CHECK(s_ndrawn > before);
    CHECK_EQ(s_drawn[before].generated, 0);       /* primed: shown at its present */
    CHECK_EQ(s_drawn[before].due, t_back);
    CHECK_EQ(s_drawn[before + 1].generated, 1);   /* then G between two post-FF pictures */
    CHECK_EQ(s_drawn[before + 1].v, (uint8_t)(s_drawn[before].v + 2));
    fgc_stop();
}

/* A state load (FGC_RESET), even on the same picture, and a pause (fgc_reset from another thread): FIFO cleared at
 * once, the next frame primes. */
static void test_reset(void)
{
    int res[16];
    uint64_t t = 1000 * MS;
    uint8_t v = 0;
    FramegenPacerCounters p0, p1;

    start(2, 0);
    run_fields(10, 2, &t, &v, res, 0);
    /* A frame queued, its G and N waiting in the FIFO. */
    v += 10;
    fill(v);
    s_clock = t;
    CHECK_EQ(fgc_frame(s_px, W, H, W * 4, t, 0), FGC_QUEUED);
    CHECK(fgc_next_due() != 0);
    framegen_pacer_counters(&p0);
    /* The same picture, a state loaded: distinct anyway, the FIFO's older frames gone, the frame primes. */
    t += FIELD;
    s_clock = t;
    s_ndrawn = 0;
    CHECK_EQ(fgc_frame(s_px, W, H, W * 4, t, FGC_RESET), FGC_QUEUED);
    framegen_pacer_counters(&p1);
    CHECK_EQ(p1.resets, p0.resets + 1);
    CHECK_EQ(p1.generated, p0.generated);
    CHECK_EQ(fgc_next_due(), t);   /* only the primed frame, due at once */
    drain_until(t + FIELD);
    CHECK_EQ(s_ndrawn, 1);
    CHECK_EQ(s_drawn[0].generated, 0);
    CHECK_EQ(s_drawn[0].v, v);
    t += FIELD;

    /* A pause: fgc_reset clears the FIFO now; the next distinct frame primes. */
    v += 10;
    fill(v);
    CHECK_EQ(fgc_frame(s_px, W, H, W * 4, t, 0), FGC_QUEUED);
    CHECK(fgc_next_due() != 0);
    fgc_reset();
    CHECK_EQ(fgc_next_due(), 0);
    framegen_pacer_counters(&p0);
    t += 2 * FIELD;
    v += 10;
    fill(v);
    s_clock = t;
    CHECK_EQ(fgc_frame(s_px, W, H, W * 4, t, 0), FGC_QUEUED);
    framegen_pacer_counters(&p1);
    CHECK_EQ(p1.generated, p0.generated);
    FgcCounters c;
    fgc_counters(&c);
    CHECK(c.resets >= 2);
    fgc_stop();
}

/* A picture wider than max_width: box-filtered by a whole factor; every shown frame at that size. */
static void test_size_cap(void)
{
    enum { BW = 200, BH = 30 };
    static uint8_t big[BW * BH * 4];
    uint64_t t = 1000 * MS;
    FgcCounters c;

    start(2, 64);   /* 200 wide -> factor 4 -> 50 x 7 */
    s_ndrawn = 0;
    for (int f = 0; f < 6; f++) {
        for (int y = 0; y < BH; y++)
            for (int x = 0; x < BW; x++) {
                uint8_t *p = big + ((size_t)y * BW + x) * 4;
                p[0] = p[1] = p[2] = (uint8_t)(f * 20 + (x & 1) * 10);   /* columns alternate: mean +5 */
                p[3] = 255;
            }
        s_clock = t;
        CHECK_EQ(fgc_frame(big, BW, BH, BW * 4, t, 0), FGC_QUEUED);
        t += 2 * FIELD;
        drain_until(t);
    }
    fgc_counters(&c);
    CHECK_EQ(c.downscaled, 6);
    CHECK(s_ndrawn >= 10);
    for (int i = 0; i < s_ndrawn; i++) {
        CHECK_EQ(s_drawn[i].w, 50);
        CHECK_EQ(s_drawn[i].h, 7);
    }
    CHECK_EQ(s_drawn[0].v, 5);
    CHECK_EQ(s_drawn[1].v, 15);   /* G between 5 and 25 */
    fgc_stop();
}

/* A failing generator: the pacer falls back after 30 in a row, then every picture is drawn directly. */
static void test_fallback(void)
{
    int res[80];
    uint64_t t = 1000 * MS;
    uint8_t v = 0;

    start(2, 0);
    s_gen_error = 1;
    run_fields(80, 2, &t, &v, res, 0);
    CHECK_EQ(framegen_pacer_active(), 0);
    CHECK_EQ(res[0], FGC_QUEUED);
    CHECK_EQ(res[78], FGC_DIRECT);
    CHECK_EQ(res[79], FGC_DIRECT);
    fgc_stop();
}

/* Not started: everything is drawn directly. */
static void test_not_running(void)
{
    fill(1);
    CHECK_EQ(fgc_running(), 0);
    CHECK_EQ(fgc_frame(s_px, W, H, W * 4, 0, 0), FGC_DIRECT);
    FgcShown e;
    CHECK_EQ(fgc_take(~0ull >> 1, &e), 0);
    CHECK_EQ(fgc_next_due(), 0);
}

static void test_hash(void)
{
    static uint8_t a[W * H * 4], b[(W + 3) * H * 4];
    for (int i = 0; i < W * H * 4; i++)
        a[i] = (uint8_t)(i * 7);
    memset(b, 0xEE, sizeof b);
    for (int y = 0; y < H; y++)   /* the same picture at another pitch, other bytes in the padding */
        memcpy(b + (size_t)y * (W + 3) * 4, a + (size_t)y * W * 4, W * 4);
    CHECK_EQ(fgc_hash(a, W, H, W * 4), fgc_hash(b, W, H, (W + 3) * 4));
    uint64_t h0 = fgc_hash(a, W, H, W * 4);
    a[W * H * 4 - 1] ^= 1;   /* one bit of the last pixel */
    CHECK(fgc_hash(a, W, H, W * 4) != h0);
    a[W * H * 4 - 1] ^= 1;
    a[0] ^= 0x80;
    CHECK(fgc_hash(a, W, H, W * 4) != h0);
    CHECK(fgc_hash(a, W, H, W * 4) != fgc_hash(a, W / 2, H * 2, W * 2));
}


/* The real thing: the pacer's thread, the real clock, and the window's wait (host_window_sleep_until's loop over
 * fgc_wait / fgc_take): 30 fps at 60 Hz for 40 fields, every frame drawn in order, about one per field. */
static uint64_t mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void window_sleep_until(uint64_t deadline, int *drawn, int *gen, uint64_t *last_due)
{
    FgcShown e;
    for (;;) {
        uint64_t now = mono();
        if (fgc_take(now, &e)) {
            CHECK(e.due_ns >= *last_due);
            *last_due = e.due_ns;
            (*drawn)++;
            *gen += e.generated;
        }
        now = mono();
        if (now >= deadline)
            return;
        uint64_t due = fgc_next_due(), until = deadline;
        if (due && due < until)
            until = due > now + MS ? due - MS : now;
        if (until > now)
            fgc_wait(until);
    }
}

static void test_threaded(void)
{
    FramegenConsoleConfig c;
    memset(&c, 0, sizeof c);
    c.pacer.gen = &framegen_fake_generator;
    c.pacer.multiplier = 2;
    c.pacer.cap_fps = 30;
    c.pacer.budget_us = 1000000;
    c.pacer.threaded = 1;
    CHECK_EQ(fgc_start(&c), 0);
    int drawn = 0, gen = 0, queued = 0;
    uint64_t last_due = 0, t = mono();
    for (int i = 0; i < 40; i++) {
        fill((uint8_t)(i / 2 * 4));
        int r = fgc_frame(s_px, W, H, W * 4, mono(), 0);
        queued += r == FGC_QUEUED;
        t += FIELD;
        window_sleep_until(t, &drawn, &gen, &last_due);
    }
    window_sleep_until(t + 3 * FIELD, &drawn, &gen, &last_due);
    FgcCounters k;
    fgc_counters(&k);
    CHECK_EQ(queued, 20);
    CHECK(drawn >= 34 && drawn <= 40);   /* 39 when nothing is late on a loaded host */
    CHECK(gen >= 15);
    CHECK_EQ(k.fifo_full, 0);
    fgc_stop();
}

int main(void)
{
    test_not_running();
    test_hash();
    test_30fps_fills_repeats();
    test_20fps();
    test_passthrough();
    test_fast_forward();
    test_reset();
    test_size_cap();
    test_fallback();
    test_threaded();
    if (s_fail) {
        fprintf(stderr, "framegen_console: %d check(s) failed\n", s_fail);
        return 1;
    }
    printf("framegen_console: all checks passed\n");
    return 0;
}
