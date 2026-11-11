/* framegen_pacer.c -- frame generation pacer: input queue, thread, timing,
 * statistics and fallback. The rules are in framegen_pacer.h. */
#define _POSIX_C_SOURCE 200809L   /* clock_gettime, nanosleep, setenv */
#include "framegen_pacer.h"
#include "framegen_policy.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PACER_QUEUE     2       /* real frames waiting for the thread */
#define PACER_AVG       8       /* flip intervals averaged for P */
#define PACER_MAX_OUT   3       /* generated frames per pair (multiplier 4) */
#define STATS_WINDOW_NS 1000000000ull

typedef struct Frame {
    uint8_t *px;                /* BGRX, packed */
    size_t cap;
    uint32_t w, h;
    uint64_t flip_ns;
} Frame;

static struct {
    int running;
    int active;
    FramegenSink sink;
    FramegenPacerConfig cfg;
    uint64_t cap_period_ns;
    uint64_t budget_ns;

    pthread_t thread;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int stop_req;
    int reset_req;
    Frame queue[PACER_QUEUE];   /* queue[0] is the oldest */
    int nqueue;
    Frame work;                 /* the frame the thread is on */

    /* thread only */
    int gen_open;
    uint32_t gen_w, gen_h;
    uint8_t *out[PACER_MAX_OUT];
    size_t out_cap;
    int have_prev;
    uint64_t prev_flip;
    uint64_t iv[PACER_AVG];
    int niv, iv_pos;
    uint64_t period_ns;
    uint32_t bad_streak;

    /* under mu */
    FramegenPacerCounters cnt;
    uint64_t real_win_start, real_win_n;
    uint64_t shown_win_start, shown_win_n, gen_win_ns, gen_win_n;
    uint32_t real_fps_centi, shown_fps_centi, gen_us;
    char off_reason[320];
} P;

static const FramegenGenerator *s_registered;

static uint64_t mono_ns(void *user)
{
    struct timespec ts;
    (void)user;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t now(void)
{
    return P.cfg.now_ns(P.cfg.clock_user);
}

static int reserve(uint8_t **buf, size_t *cap, size_t need)
{
    if (*cap >= need)
        return 0;
    uint8_t *n = (uint8_t *)realloc(*buf, need);
    if (!n)
        return -1;
    *buf = n;
    *cap = need;
    return 0;
}

static uint32_t format_bpp(uint32_t format)
{
    switch (format) {
    case FRAMEGEN_FMT_BGRX8888:
    case FRAMEGEN_FMT_RGBX8888:
        return 4;
    case FRAMEGEN_FMT_R5G6B5:
    case FRAMEGEN_FMT_X1R5G5B5:
        return 2;
    default:
        return 0;
    }
}

/* Copy (converting RGBX, expanding R5G6B5 / X1R5G5B5) into dst as packed BGRX. */
static int frame_fill(Frame *dst, const uint8_t *px, uint32_t w, uint32_t h,
                      uint32_t format, uint32_t pitch, uint64_t flip_ns)
{
    if (reserve(&dst->px, &dst->cap, (size_t)w * h * 4))
        return -1;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *s = px + (size_t)y * pitch;
        uint8_t *d = dst->px + (size_t)y * w * 4;
        switch (format) {
        case FRAMEGEN_FMT_BGRX8888:
            memcpy(d, s, (size_t)w * 4);
            break;
        case FRAMEGEN_FMT_RGBX8888:
            for (uint32_t x = 0; x < w; x++) {
                d[4 * x + 0] = s[4 * x + 2];
                d[4 * x + 1] = s[4 * x + 1];
                d[4 * x + 2] = s[4 * x + 0];
                d[4 * x + 3] = 0xFF;
            }
            break;
        case FRAMEGEN_FMT_R5G6B5:
            for (uint32_t x = 0; x < w; x++) {
                uint16_t v = (uint16_t)(s[2 * x] | (s[2 * x + 1] << 8));
                uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
                d[4 * x + 0] = (uint8_t)((b << 3) | (b >> 2));
                d[4 * x + 1] = (uint8_t)((g << 2) | (g >> 4));
                d[4 * x + 2] = (uint8_t)((r << 3) | (r >> 2));
                d[4 * x + 3] = 0xFF;
            }
            break;
        default:   /* FRAMEGEN_FMT_X1R5G5B5 */
            for (uint32_t x = 0; x < w; x++) {
                uint16_t v = (uint16_t)(s[2 * x] | (s[2 * x + 1] << 8));
                uint32_t r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
                d[4 * x + 0] = (uint8_t)((b << 3) | (b >> 2));
                d[4 * x + 1] = (uint8_t)((g << 3) | (g >> 2));
                d[4 * x + 2] = (uint8_t)((r << 3) | (r >> 2));
                d[4 * x + 3] = 0xFF;
            }
            break;
        }
    }
    dst->w = w;
    dst->h = h;
    dst->flip_ns = flip_ns;
    return 0;
}

static void swap_frames(Frame *a, Frame *b)
{
    Frame t = *a;
    *a = *b;
    *b = t;
}

/* ── Statistics ───────────────────────────────────────────────────────── */

static void stats_shown(uint64_t t)    /* under mu */
{
    if (!P.shown_win_start)
        P.shown_win_start = t;
    P.shown_win_n++;
    if (t - P.shown_win_start >= STATS_WINDOW_NS) {
        uint64_t el = t - P.shown_win_start;
        P.shown_fps_centi = (uint32_t)(P.shown_win_n * 100ull * 1000000000ull / el);
        P.gen_us = P.gen_win_n ? (uint32_t)(P.gen_win_ns / P.gen_win_n / 1000) : 0;
        P.shown_win_start = t;
        P.shown_win_n = P.gen_win_ns = P.gen_win_n = 0;
    }
}

static void stats_real(uint64_t flip_ns)    /* under mu */
{
    if (!P.real_win_start || flip_ns < P.real_win_start)
        P.real_win_start = flip_ns;
    P.real_win_n++;
    if (flip_ns - P.real_win_start >= STATS_WINDOW_NS) {
        uint64_t el = flip_ns - P.real_win_start;
        /* n flips span n - 1 intervals from the window's first flip; the
         * last one starts the next window. */
        P.real_fps_centi = (uint32_t)((P.real_win_n - 1) * 100ull * 1000000000ull / el);
        P.real_win_start = flip_ns;
        P.real_win_n = 1;
    }
}

/* ── Generator handling ───────────────────────────────────────────────── */

static void go_off(const char *reason)
{
    pthread_mutex_lock(&P.mu);
    if (P.active) {
        P.active = 0;
        snprintf(P.off_reason, sizeof P.off_reason, "%s", reason);
        fprintf(stderr, "[LSFG] off: %s\n", reason);
    }
    pthread_mutex_unlock(&P.mu);
    if (P.gen_open) {
        P.cfg.gen->close(P.cfg.gen->user);
        P.gen_open = 0;
    }
}

static void history_clear(void)
{
    P.have_prev = 0;
    P.niv = P.iv_pos = 0;
    P.period_ns = P.cap_period_ns;
    P.bad_streak = 0;
}

static int gen_ensure(uint32_t w, uint32_t h)
{
    const FramegenGenerator *g = P.cfg.gen;
    char err[256];
    if (P.gen_open && (P.gen_w != w || P.gen_h != h)) {
        g->close(g->user);
        P.gen_open = 0;
        history_clear();
    }
    if (P.gen_open)
        return 0;
    size_t need = (size_t)w * h * 4;
    if (P.out_cap < need) {
        for (int i = 0; i < PACER_MAX_OUT; i++) {
            uint8_t *n = (uint8_t *)realloc(P.out[i], need);
            if (!n) {
                go_off("out of memory");
                return -1;
            }
            P.out[i] = n;
        }
        P.out_cap = need;
    }
    err[0] = 0;
    if (g->open(g->user, w, h, P.cfg.multiplier, err, sizeof err) < 0) {
        char reason[320];
        g->close(g->user);
        snprintf(reason, sizeof reason, "%s: %s", g->name ? g->name : "generator",
                 err[0] ? err : "open failed");
        go_off(reason);
        return -1;
    }
    P.gen_open = 1;
    P.gen_w = w;
    P.gen_h = h;
    history_clear();
    return 0;
}

static void gen_reset(void)
{
    const FramegenGenerator *g = P.cfg.gen;
    history_clear();
    if (!P.gen_open)
        return;
    if (g->reset) {
        g->reset(g->user);
    } else {
        g->close(g->user);
        P.gen_open = 0;
        gen_ensure(P.gen_w, P.gen_h);
    }
}

/* ── Hand-over with the drop rule ─────────────────────────────────────── */

static int newer_queued(void)
{
    int n;
    if (!P.cfg.threaded)
        return 0;
    pthread_mutex_lock(&P.mu);
    n = P.nqueue;
    pthread_mutex_unlock(&P.mu);
    return n > 0;
}

static void emit(const uint8_t *px, uint32_t w, uint32_t h, uint64_t due, int generated,
                 uint64_t period)
{
    uint64_t t = now();
    int late = t > due && t - due > period;
    if (late && (generated || newer_queued())) {
        pthread_mutex_lock(&P.mu);
        if (generated)
            P.cnt.dropped_generated++;
        else
            P.cnt.dropped_real++;
        pthread_mutex_unlock(&P.mu);
        return;
    }
    P.sink.show(px, w, h, 4, due);
    pthread_mutex_lock(&P.mu);
    if (generated)
        P.cnt.shown_generated++;
    else
        P.cnt.shown_real++;
    stats_shown(t);
    pthread_mutex_unlock(&P.mu);
}

static void bad_frame(int failed, int code, uint64_t cost)
{
    char reason[160];
    pthread_mutex_lock(&P.mu);
    if (failed)
        P.cnt.failed++;
    else
        P.cnt.over_budget++;
    pthread_mutex_unlock(&P.mu);
    if (++P.bad_streak < P.cfg.fallback_frames)
        return;
    if (failed)
        snprintf(reason, sizeof reason, "generation failed %u frames in a row (error %d)",
                 P.bad_streak, code);
    else
        snprintf(reason, sizeof reason,
                 "generation over budget %u frames in a row (last %.1f ms, budget %.1f ms)",
                 P.bad_streak, cost / 1e6, P.budget_ns / 1e6);
    go_off(reason);
}

/* ── One real frame ───────────────────────────────────────────────────── */

static void process(Frame *f)
{
    const FramegenGenerator *g = P.cfg.gen;
    const uint32_t m = P.cfg.multiplier;
    const uint32_t pitch = f->w * 4;
    uint64_t T = f->flip_ns, iv, sum, c, t0, period;
    int r, reset;

    pthread_mutex_lock(&P.mu);
    reset = P.reset_req;
    P.reset_req = 0;
    pthread_mutex_unlock(&P.mu);

    if (P.active && gen_ensure(f->w, f->h) == 0 && reset)
        gen_reset();
    if (!P.active) {
        emit(f->px, f->w, f->h, T, 0, P.period_ns);
        return;
    }

    if (!P.have_prev) {
        if (g->generate(g->user, f->px, pitch, NULL, 0) < 0)
            bad_frame(1, -1, 0);
        P.have_prev = 1;
        P.prev_flip = T;
        emit(f->px, f->w, f->h, T, 0, P.period_ns);
        return;
    }

    iv = T > P.prev_flip ? T - P.prev_flip : 0;
    P.prev_flip = T;
    if (iv > 2 * P.cap_period_ns) {
        /* A stall: no in-between frame worth showing; N keeps the history. */
        if (g->generate(g->user, f->px, pitch, NULL, 0) < 0)
            bad_frame(1, -1, 0);
        pthread_mutex_lock(&P.mu);
        P.cnt.skipped++;
        pthread_mutex_unlock(&P.mu);
        emit(f->px, f->w, f->h, T, 0, P.period_ns);
        return;
    }

    P.iv[P.iv_pos] = iv;
    P.iv_pos = (P.iv_pos + 1) % PACER_AVG;
    if (P.niv < PACER_AVG)
        P.niv++;
    sum = 0;
    for (int i = 0; i < P.niv; i++)
        sum += P.iv[i];
    period = sum / (uint64_t)P.niv;
    if (period < P.cap_period_ns)
        period = P.cap_period_ns;
    if (period > 2 * P.cap_period_ns)
        period = 2 * P.cap_period_ns;
    P.period_ns = period;

    t0 = now();
    r = g->generate(g->user, f->px, pitch, P.out, pitch);
    c = now() - t0;

    pthread_mutex_lock(&P.mu);
    P.cnt.period_ns = (uint32_t)period;
    P.cnt.last_cost_ns = (uint32_t)c;
    if (r > 0) {
        P.cnt.generated += (uint64_t)r;
        P.gen_win_ns += c;
        P.gen_win_n++;
    }
    pthread_mutex_unlock(&P.mu);

    if (r < 0) {
        bad_frame(1, r, c);
        emit(f->px, f->w, f->h, T, 0, period);
        return;
    }
    if (c > P.budget_ns)
        bad_frame(0, 0, c);
    else
        P.bad_streak = 0;

    if (r > (int)(m - 1))
        r = (int)(m - 1);
    for (int k = 1; k <= r; k++)
        emit(P.out[k - 1], f->w, f->h, T + c + (uint64_t)(k - 1) * period / m, 1, period);
    emit(f->px, f->w, f->h, r > 0 ? T + c + (uint64_t)r * period / m : T, 0, period);
}

/* ── Thread ───────────────────────────────────────────────────────────── */

static void *pacer_main(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&P.mu);
    for (;;) {
        while (!P.stop_req && P.nqueue == 0)
            pthread_cond_wait(&P.cv, &P.mu);
        if (P.stop_req)
            break;
        swap_frames(&P.work, &P.queue[0]);
        for (int i = 1; i < P.nqueue; i++)
            swap_frames(&P.queue[i - 1], &P.queue[i]);
        P.nqueue--;
        pthread_mutex_unlock(&P.mu);
        process(&P.work);
        pthread_mutex_lock(&P.mu);
    }
    pthread_mutex_unlock(&P.mu);
    return NULL;
}

/* ── Public API ───────────────────────────────────────────────────────── */

void framegen_pacer_set_generator(const FramegenGenerator *gen)
{
    s_registered = gen;
}

int framegen_pacer_start_ex(const FramegenSink *sink, const FramegenPacerConfig *cfg)
{
    if (P.running || !sink || !sink->show || !cfg || !cfg->gen || !cfg->gen->open
        || !cfg->gen->generate || !cfg->gen->close)
        return -1;
    memset(&P, 0, sizeof P);
    P.sink = *sink;
    P.cfg = *cfg;
    if (P.cfg.multiplier < 2)
        P.cfg.multiplier = 2;
    if (P.cfg.multiplier > PACER_MAX_OUT + 1)
        P.cfg.multiplier = PACER_MAX_OUT + 1;
    if (!P.cfg.cap_fps)
        P.cfg.cap_fps = 30;
    if (!P.cfg.fallback_frames)
        P.cfg.fallback_frames = 30;
    if (!P.cfg.now_ns)
        P.cfg.now_ns = mono_ns;
    P.cap_period_ns = 1000000000ull / P.cfg.cap_fps;
    P.budget_ns = P.cfg.budget_us ? (uint64_t)P.cfg.budget_us * 1000ull
                                  : P.cap_period_ns / P.cfg.multiplier;
    P.period_ns = P.cap_period_ns;
    P.active = 1;
    pthread_mutex_init(&P.mu, NULL);
    pthread_cond_init(&P.cv, NULL);
    if (P.cfg.threaded && pthread_create(&P.thread, NULL, pacer_main, NULL) != 0) {
        pthread_cond_destroy(&P.cv);
        pthread_mutex_destroy(&P.mu);
        fprintf(stderr, "[LSFG] off: cannot start the pacer thread\n");
        return -1;
    }
    P.running = 1;
    return 0;
}

int framegen_pacer_start(const FramegenSink *sink)
{
    FramegenPacerConfig cfg;
    if (!framegen_policy_enabled())
        return -1;
    memset(&cfg, 0, sizeof cfg);
    cfg.gen = framegen_policy_env_on("RECOMP_LSFG_FAKE") ? &framegen_fake_generator : s_registered;
    if (!cfg.gen) {
        fprintf(stderr, "[LSFG] off: no generator in this build\n");
        return -1;
    }
    cfg.multiplier = framegen_policy_multiplier();
    cfg.cap_fps = (uint32_t)framegen_policy_fps_cap(framegen_policy_env_fps_cap());
    cfg.budget_us = (uint32_t)framegen_policy_env_long("RECOMP_LSFG_BUDGET_MS", 0, 0, 1000) * 1000u;
    cfg.threaded = 1;
    if (framegen_pacer_start_ex(sink, &cfg) < 0)
        return -1;
    fprintf(stderr, "[LSFG] pacer: %s generator, %ux, cap %u fps, budget %.1f ms\n",
            cfg.gen->name ? cfg.gen->name : "?", P.cfg.multiplier, P.cfg.cap_fps,
            P.budget_ns / 1e6);
    return 0;
}

void framegen_pacer_submit(const uint8_t *px, uint32_t w, uint32_t h, uint32_t bpp,
                           uint32_t pitch, uint64_t flip_ns)
{
    if (bpp != 2 && bpp != 4)
        return;
    framegen_pacer_submit_format(px, w, h, bpp == 4 ? FRAMEGEN_FMT_BGRX8888 : FRAMEGEN_FMT_R5G6B5,
                                 pitch, flip_ns);
}

void framegen_pacer_submit_format(const uint8_t *px, uint32_t w, uint32_t h, uint32_t format,
                                  uint32_t pitch, uint64_t flip_ns)
{
    uint32_t bpp = format_bpp(format);
    if (!P.running || !px || !w || !h || !bpp || w > 8192 || h > 8192 || pitch < w * bpp)
        return;
    pthread_mutex_lock(&P.mu);
    P.cnt.submitted++;
    stats_real(flip_ns);
    if (!P.cfg.threaded) {
        pthread_mutex_unlock(&P.mu);
        if (frame_fill(&P.work, px, w, h, format, pitch, flip_ns) == 0)
            process(&P.work);
        return;
    }
    if (P.nqueue == PACER_QUEUE) {
        /* The thread is behind: the oldest waiting frame goes. */
        for (int i = 1; i < P.nqueue; i++)
            swap_frames(&P.queue[i - 1], &P.queue[i]);
        P.nqueue--;
        P.cnt.dropped_input++;
    }
    if (frame_fill(&P.queue[P.nqueue], px, w, h, format, pitch, flip_ns) == 0) {
        P.nqueue++;
        pthread_cond_signal(&P.cv);
    }
    pthread_mutex_unlock(&P.mu);
}

void framegen_pacer_stats(uint32_t *real_fps_centi, uint32_t *shown_fps_centi, uint32_t *gen_us)
{
    uint32_t a = 0, b = 0, c = 0;
    if (P.running) {
        pthread_mutex_lock(&P.mu);
        a = P.real_fps_centi;
        b = P.shown_fps_centi;
        c = P.gen_us;
        pthread_mutex_unlock(&P.mu);
    }
    if (real_fps_centi)
        *real_fps_centi = a;
    if (shown_fps_centi)
        *shown_fps_centi = b;
    if (gen_us)
        *gen_us = c;
}

void framegen_pacer_counters(FramegenPacerCounters *out)
{
    if (!out)
        return;
    if (!P.running) {
        memset(out, 0, sizeof *out);
        return;
    }
    pthread_mutex_lock(&P.mu);
    *out = P.cnt;
    pthread_mutex_unlock(&P.mu);
}

void framegen_pacer_reset(void)
{
    if (!P.running)
        return;
    pthread_mutex_lock(&P.mu);
    P.reset_req = 1;
    P.cnt.resets++;
    P.cnt.dropped_input += (uint64_t)P.nqueue;
    P.nqueue = 0;
    pthread_mutex_unlock(&P.mu);
}

int framegen_pacer_active(void)
{
    int a = 0;
    if (P.running) {
        pthread_mutex_lock(&P.mu);
        a = P.active;
        pthread_mutex_unlock(&P.mu);
    }
    return a;
}

const char *framegen_pacer_off_reason(void)
{
    return P.off_reason;
}

void framegen_pacer_stop(void)
{
    if (!P.running)
        return;
    if (P.cfg.threaded) {
        pthread_mutex_lock(&P.mu);
        P.stop_req = 1;
        pthread_cond_signal(&P.cv);
        pthread_mutex_unlock(&P.mu);
        pthread_join(P.thread, NULL);
    }
    if (P.gen_open)
        P.cfg.gen->close(P.cfg.gen->user);
    P.gen_open = 0;
    for (int i = 0; i < PACER_QUEUE; i++)
        free(P.queue[i].px);
    free(P.work.px);
    for (int i = 0; i < PACER_MAX_OUT; i++)
        free(P.out[i]);
    pthread_cond_destroy(&P.cv);
    pthread_mutex_destroy(&P.mu);
    P.running = 0;
    P.active = 0;
}
