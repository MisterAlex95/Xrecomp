/* framegen_console.c -- see framegen_console.h (XBA-186). */
#define _POSIX_C_SOURCE 200809L   /* clock_gettime, pthread_condattr_setclock */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE          /* pthread_cond_timedwait_relative_np, hidden by _POSIX_C_SOURCE */
#endif
#include "framegen_console.h"
#include "framegen_policy.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FGC_FIFO 4      /* real + generated frames waiting for their due time (multiplier 4: one pair) */
#define FGC_AVG 8       /* distinct intervals averaged for the pass-through rule */
#define FGC_MIN_AVG 4   /* intervals needed before pass-through is decided */

typedef struct Entry {
    uint8_t *px;
    size_t cap;
    int w, h, generated;
    uint64_t due_ns;
} Entry;

static struct {
    int running;
    FramegenConsoleConfig cfg;
    uint64_t cap_period_ns;

    /* the presenting thread */
    uint64_t hash;
    int have_hash, hash_w, hash_h;
    uint64_t last_distinct_ns;
    uint64_t iv[FGC_AVG];
    int niv, iv_pos;
    int passthrough, bypassed;
    uint8_t *small;
    size_t small_cap;

    /* FIFO (the pacer's thread fills it) */
    pthread_mutex_t mu;
    pthread_cond_t cv;
    Entry fifo[FGC_FIFO];
    int head, count;
    Entry front;
    uint64_t seq;           /* entries in so far: fgc_wait's wake-up */
    int reset_req;          /* fgc_reset from another thread: the history goes at the next frame */
    FgcCounters cnt;
} S;

static uint64_t fgc_mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline uint64_t rotl64(uint64_t v, int r)
{
    return (v << r) | (v >> (64 - r));
}

static inline uint64_t load64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, sizeof v);
    return v;
}

/* Four independent lanes (multiply latency hidden), every byte of each row. */
uint64_t fgc_hash(const uint8_t *px, int w, int h, int pitch)
{
    const uint64_t k = 0x9E3779B97F4A7C15ull;
    uint64_t a = 0x243F6A8885A308D3ull, b = 0x13198A2E03707344ull, c = 0xA4093822299F31D0ull,
             d = 0x082EFA98EC4E6C89ull;
    size_t row = (size_t)w * 4;

    if (!px || w <= 0 || h <= 0)
        return 0;
    for (int y = 0; y < h; y++) {
        const uint8_t *p = px + (size_t)y * (size_t)pitch;
        size_t i = 0;
        for (; i + 32 <= row; i += 32) {
            a = rotl64((a ^ load64(p + i)) * k, 29);
            b = rotl64((b ^ load64(p + i + 8)) * k, 29);
            c = rotl64((c ^ load64(p + i + 16)) * k, 29);
            d = rotl64((d ^ load64(p + i + 24)) * k, 29);
        }
        for (; i + 8 <= row; i += 8)
            a = rotl64((a ^ load64(p + i)) * k, 29);
        for (; i < row; i++)
            b = rotl64((b ^ p[i]) * k, 29);
    }
    uint64_t hsh = a ^ rotl64(b, 16) ^ rotl64(c, 32) ^ rotl64(d, 48) ^ ((uint64_t)w << 32 | (uint32_t)h);
    hsh ^= hsh >> 33;
    hsh *= 0xFF51AFD7ED558CCDull;
    hsh ^= hsh >> 33;
    return hsh;
}

/* ── FIFO (the pacer's sink) ──────────────────────────────────────────── */

static void sink_show(const uint8_t *px, uint32_t w, uint32_t h, uint32_t bpp, uint64_t due_ns, int generated)
{
    size_t need = (size_t)w * h * 4;

    if (bpp != 4)
        return;
    pthread_mutex_lock(&S.mu);
    if (S.count == FGC_FIFO) {   /* nobody took them: the oldest goes */
        S.head = (S.head + 1) % FGC_FIFO;
        S.count--;
        S.cnt.fifo_full++;
    }
    Entry *e = &S.fifo[(S.head + S.count) % FGC_FIFO];
    if (need > e->cap) {
        uint8_t *n = realloc(e->px, need);
        if (!n) {
            pthread_mutex_unlock(&S.mu);
            return;
        }
        e->px = n;
        e->cap = need;
    }
    /* BGRX -> RGBA (the window's texture format). */
    const uint8_t *s = px;
    uint8_t *o = e->px;
    for (size_t i = 0; i < (size_t)w * h; i++, s += 4, o += 4) {
        o[0] = s[2];
        o[1] = s[1];
        o[2] = s[0];
        o[3] = 255;
    }
    e->w = (int)w;
    e->h = (int)h;
    e->generated = generated;
    e->due_ns = due_ns;
    S.count++;
    S.seq++;
    S.cnt.fifo_in++;
    pthread_cond_broadcast(&S.cv);
    pthread_mutex_unlock(&S.mu);
}

static void fifo_clear_locked(void)
{
    S.head = S.count = 0;
}

int fgc_take(uint64_t now_ns, FgcShown *out)
{
    int got = 0;

    if (!S.running || !out)
        return 0;
    pthread_mutex_lock(&S.mu);
    while (S.count && S.fifo[S.head].due_ns <= now_ns + 1000000ull) {
        Entry t = S.front;
        if (got)
            S.cnt.late++;
        S.front = S.fifo[S.head];
        S.fifo[S.head] = t;
        S.head = (S.head + 1) % FGC_FIFO;
        S.count--;
        got = 1;
    }
    if (got) {
        S.cnt.taken++;
        if (S.front.generated)
            S.cnt.taken_generated++;
        out->px = S.front.px;
        out->w = S.front.w;
        out->h = S.front.h;
        out->generated = S.front.generated;
        out->due_ns = S.front.due_ns;
    }
    pthread_mutex_unlock(&S.mu);
    return got;
}

uint64_t fgc_next_due(void)
{
    uint64_t d = 0;

    if (!S.running)
        return 0;
    pthread_mutex_lock(&S.mu);
    if (S.count)
        d = S.fifo[S.head].due_ns;
    pthread_mutex_unlock(&S.mu);
    return d;
}

void fgc_wait(uint64_t until_ns)
{
    uint64_t now = fgc_mono_ns();

    if (until_ns <= now)
        return;
    if (!S.running) {
        uint64_t d = until_ns - now;
        struct timespec ts = {(time_t)(d / 1000000000ull), (long)(d % 1000000000ull)};
        nanosleep(&ts, NULL);
        return;
    }
    pthread_mutex_lock(&S.mu);
    uint64_t seq = S.seq;
    while (S.seq == seq && (now = fgc_mono_ns()) < until_ns) {
        uint64_t d = until_ns - now;
#if defined(__APPLE__)
        struct timespec rel = {(time_t)(d / 1000000000ull), (long)(d % 1000000000ull)};
        if (pthread_cond_timedwait_relative_np(&S.cv, &S.mu, &rel) == ETIMEDOUT)
            break;
#else
        uint64_t t = now + d;
        struct timespec abs = {(time_t)(t / 1000000000ull), (long)(t % 1000000000ull)};
        if (pthread_cond_timedwait(&S.cv, &S.mu, &abs) == ETIMEDOUT)
            break;
#endif
    }
    pthread_mutex_unlock(&S.mu);
}

/* ── History ──────────────────────────────────────────────────────────── */

/* A fresh history: the pacer forgets its frames, the FIFO and the distinct intervals go. The last picture's hash
 * stays (a repeat after a reset is still a repeat; FGC_RESET forces its frame distinct itself). */
static void history_reset(void)
{
    framegen_pacer_reset();
    pthread_mutex_lock(&S.mu);
    fifo_clear_locked();
    S.reset_req = 0;
    S.cnt.resets++;
    pthread_mutex_unlock(&S.mu);
    S.niv = S.iv_pos = 0;
    S.last_distinct_ns = 0;
}

void fgc_reset(void)
{
    if (!S.running)
        return;
    framegen_pacer_reset();
    pthread_mutex_lock(&S.mu);
    fifo_clear_locked();
    S.reset_req = 1;
    pthread_mutex_unlock(&S.mu);
}

/* A distinct frame at t: its interval joins the mean (not a stall beyond 2 x the cap period, as the pacer skips
 * those), and the pass-through rule is applied. Returns 1 when pass-through just ended (a fresh history). */
static int distinct_at(uint64_t t)
{
    int left = 0;

    if (S.last_distinct_ns && t > S.last_distinct_ns && t - S.last_distinct_ns <= 2 * S.cap_period_ns) {
        S.iv[S.iv_pos] = t - S.last_distinct_ns;
        S.iv_pos = (S.iv_pos + 1) % FGC_AVG;
        if (S.niv < FGC_AVG)
            S.niv++;
    }
    S.last_distinct_ns = t;
    if (S.niv >= FGC_MIN_AVG) {
        uint64_t sum = 0;
        for (int i = 0; i < S.niv; i++)
            sum += S.iv[i];
        uint64_t mean = sum / (uint64_t)S.niv;
        int fast = mean * 4 < S.cap_period_ns * 3;
        if (fast && !S.passthrough) {
            S.passthrough = 1;
            framegen_pacer_reset();
            pthread_mutex_lock(&S.mu);
            fifo_clear_locked();
            S.cnt.passthrough_on++;
            pthread_mutex_unlock(&S.mu);
            fprintf(stderr, "[LSFG] console: the game draws %.1f distinct fps, above %.1f: shown as it comes\n",
                    1e9 / (double)mean, 1e9 / (double)S.cap_period_ns);
        } else if (!fast && S.passthrough) {
            S.passthrough = 0;
            left = 1;
            fprintf(stderr, "[LSFG] console: the game draws %.1f distinct fps: frame generation again\n",
                    1e9 / (double)mean);
        }
    }
    return left;
}

/* Box filter by a whole factor so the width fits max_width; RGBA in, RGBA out (packed, S.small). */
static int downscale(const uint8_t *px, int w, int h, int pitch, int *ow, int *oh)
{
    int k = (int)(((uint32_t)w + S.cfg.max_width - 1) / S.cfg.max_width);
    int dw = w / k, dh = h / k;
    size_t need = (size_t)dw * dh * 4;
    uint32_t area = (uint32_t)(k * k);

    if (dw <= 0 || dh <= 0)
        return -1;
    if (need > S.small_cap) {
        uint8_t *n = realloc(S.small, need);
        if (!n)
            return -1;
        S.small = n;
        S.small_cap = need;
    }
    for (int y = 0; y < dh; y++) {
        uint8_t *o = S.small + (size_t)y * dw * 4;
        for (int x = 0; x < dw; x++, o += 4) {
            uint32_t r = 0, g = 0, b = 0;
            for (int j = 0; j < k; j++) {
                const uint8_t *p = px + (size_t)(y * k + j) * (size_t)pitch + (size_t)x * k * 4;
                for (int i = 0; i < k; i++, p += 4) {
                    r += p[0];
                    g += p[1];
                    b += p[2];
                }
            }
            o[0] = (uint8_t)((r + area / 2) / area);
            o[1] = (uint8_t)((g + area / 2) / area);
            o[2] = (uint8_t)((b + area / 2) / area);
            o[3] = 255;
        }
    }
    *ow = dw;
    *oh = dh;
    return 0;
}

int fgc_frame(const uint8_t *rgba, int w, int h, int pitch, uint64_t now_ns, unsigned flags)
{
    if (!rgba || w <= 0 || h <= 0 || pitch < w * 4)
        return FGC_DIRECT;
    if (!S.running)
        return FGC_DIRECT;

    pthread_mutex_lock(&S.mu);
    S.cnt.frames++;
    int reset = S.reset_req || (flags & FGC_RESET);
    pthread_mutex_unlock(&S.mu);

    uint64_t hsh = fgc_hash(rgba, w, h, pitch);
    int distinct = !S.have_hash || hsh != S.hash || w != S.hash_w || h != S.hash_h || (flags & FGC_RESET);
    S.hash = hsh;
    S.have_hash = 1;
    S.hash_w = w;
    S.hash_h = h;

    pthread_mutex_lock(&S.mu);
    if (distinct)
        S.cnt.distinct++;
    else
        S.cnt.repeats++;
    pthread_mutex_unlock(&S.mu);

    if (flags & FGC_BYPASS) {   /* fast-forward: the next frame after it starts a fresh history */
        if (!S.bypassed)
            fprintf(stderr, "[LSFG] console: fast-forward, frame generation paused\n");
        S.bypassed = 1;
        goto direct;
    }
    if (S.bypassed) {
        S.bypassed = 0;
        reset = 1;
        distinct = 1;
    }
    if (!framegen_pacer_active())
        goto direct;   /* fallen back ("[LSFG] off: ..."): as without frame generation */
    if (reset) {
        history_reset();
        distinct = 1;
    }
    if (!distinct) {
        if (S.passthrough)
            goto direct;
        return FGC_REPEAT;
    }
    if (distinct_at(now_ns)) {   /* pass-through just ended: the pacer starts again from this frame */
        history_reset();
        S.last_distinct_ns = now_ns;
    }
    if (S.passthrough)
        goto direct;

    if ((uint32_t)w > S.cfg.max_width) {
        int dw, dh;
        if (downscale(rgba, w, h, pitch, &dw, &dh) == 0) {
            pthread_mutex_lock(&S.mu);
            S.cnt.downscaled++;
            S.cnt.queued++;
            pthread_mutex_unlock(&S.mu);
            framegen_pacer_submit_format(S.small, (uint32_t)dw, (uint32_t)dh, FRAMEGEN_FMT_RGBX8888,
                                         (uint32_t)dw * 4, now_ns);
            return FGC_QUEUED;
        }
        goto direct;
    }
    pthread_mutex_lock(&S.mu);
    S.cnt.queued++;
    pthread_mutex_unlock(&S.mu);
    framegen_pacer_submit_format(rgba, (uint32_t)w, (uint32_t)h, FRAMEGEN_FMT_RGBX8888, (uint32_t)pitch, now_ns);
    return FGC_QUEUED;

direct:
    pthread_mutex_lock(&S.mu);
    S.cnt.direct++;
    if (S.count)
        fifo_clear_locked();   /* nothing older than this picture is shown after it */
    pthread_mutex_unlock(&S.mu);
    return FGC_DIRECT;
}

/* ── Start / stop ─────────────────────────────────────────────────────── */

int fgc_start(const FramegenConsoleConfig *cfg)
{
    static FramegenSink sink;

    if (S.running || !cfg || !cfg->pacer.gen)
        return -1;
    memset(&S, 0, sizeof S);
    S.cfg = *cfg;
    if (!S.cfg.max_width)
        S.cfg.max_width = FGC_DEFAULT_MAX_WIDTH;
    if (S.cfg.max_width < 64)
        S.cfg.max_width = 64;
    pthread_mutex_init(&S.mu, NULL);
#if defined(__APPLE__)
    pthread_cond_init(&S.cv, NULL);
#else
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&S.cv, &ca);
    pthread_condattr_destroy(&ca);
#endif
    memset(&sink, 0, sizeof sink);
    sink.show_ex = sink_show;
    if (framegen_pacer_start_ex(&sink, &S.cfg.pacer) < 0) {
        pthread_cond_destroy(&S.cv);
        pthread_mutex_destroy(&S.mu);
        return -1;
    }
    S.cap_period_ns = 1000000000ull / (S.cfg.pacer.cap_fps ? S.cfg.pacer.cap_fps : 30);
    S.running = 1;
    return 0;
}

int fgc_start_env(int display_hz)
{
    FramegenConsoleConfig cfg;

    if (!framegen_policy_enabled())
        return -1;
    memset(&cfg, 0, sizeof cfg);
    cfg.pacer.gen = framegen_pacer_env_generator();
    if (!cfg.pacer.gen) {
        fprintf(stderr, "[LSFG] off: no generator in this build\n");
        return -1;
    }
    uint32_t hz = 60;
    const char *e = getenv("RECOMP_LSFG_DISPLAY_HZ");
    if (e && *e)
        hz = framegen_policy_display_hz();
    else if (display_hz >= 24 && display_hz < 60)
        hz = (uint32_t)display_hz;
    cfg.pacer.multiplier = framegen_policy_multiplier();
    cfg.pacer.cap_fps = (uint32_t)framegen_policy_cap_rule(0, hz, cfg.pacer.multiplier);
    cfg.pacer.budget_us = (uint32_t)framegen_policy_env_long("RECOMP_LSFG_BUDGET_MS", 0, 0, 1000) * 1000u;
    cfg.pacer.threaded = 1;
    cfg.max_width = (uint32_t)framegen_policy_env_long("RECOMP_LSFG_MAX_WIDTH", FGC_DEFAULT_MAX_WIDTH, 64, 8192);
    if (fgc_start(&cfg) < 0) {
        fprintf(stderr, "[LSFG] off: cannot start the pacer\n");
        return -1;
    }
    fprintf(stderr,
            "[LSFG] console window: %s generator, %ux, cap %u fps (display %d Hz, pacing for %u Hz), frames up to %u "
            "wide\n",
            cfg.pacer.gen->name ? cfg.pacer.gen->name : "?", cfg.pacer.multiplier, cfg.pacer.cap_fps, display_hz,
            hz, S.cfg.max_width);
    return 0;
}

int fgc_running(void)
{
    return S.running;
}

void fgc_stop(void)
{
    if (!S.running)
        return;
    framegen_pacer_stop();   /* its thread is gone: nothing reaches the sink any more */
    for (int i = 0; i < FGC_FIFO; i++)
        free(S.fifo[i].px);
    free(S.front.px);
    free(S.small);
    pthread_cond_destroy(&S.cv);
    pthread_mutex_destroy(&S.mu);
    memset(&S, 0, sizeof S);
}

void fgc_counters(FgcCounters *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof *out);
    if (!S.running)
        return;
    pthread_mutex_lock(&S.mu);
    *out = S.cnt;
    pthread_mutex_unlock(&S.mu);
    out->passthrough = S.passthrough;
    out->bypassed = S.bypassed;
    out->max_width = S.cfg.max_width;
    if (S.niv) {
        uint64_t sum = 0;
        for (int i = 0; i < S.niv; i++)
            sum += S.iv[i];
        out->distinct_period_ns = (uint32_t)(sum / (uint64_t)S.niv);
    }
}
