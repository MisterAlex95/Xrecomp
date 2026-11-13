/* frame_pacing.h -- RECOMP_PACING=auto: the rate the executor paces flips at,
 * from what the title asks for and what the host sustains (XBA-107, XBA-135).
 *
 * The executor's frame cap (RECOMP_GPU_FPS_CAP, flip_cap_book in
 * nv2a_pb_exec.c) sleeps after each flip until the frame's slot on an
 * absolute host-clock schedule. Without a fixed RECOMP_GPU_FPS_CAP, auto
 * picks that rate at every flip:
 *
 *  1. The title's ask. Every XDK Swap carries the presentation interval in
 *     its software-method NOP (bits 0-2 the interval, DEFAULT read as ONE;
 *     bit 3 IMMEDIATE). The ask is the vblank rate divided by the interval
 *     the title has used most over its last PACING_ASK_SWAPS swaps: 60 for
 *     ONE, 30 for TWO (50 and 25 at a PAL-I mode's 50 Hz). IMMEDIATE is a
 *     title the console never held: the display's rate bounds it.
 *     The vblank rate is measured from the guest vblank count against the
 *     host clock (so it follows the video standard, XBA-121), 60 until
 *     vblanks run.
 *
 *  2. The adaptive divisor, for a title free to run below its ask: the
 *     highest rate (ask base / k, at least PACING_MIN_FPS, never above the
 *     ask) that the measured frame cost sustains. Every PACING_EVAL_NS the
 *     window of frames is judged:
 *       - down: more than PACING_MISS_PCT % of frames missed their slot for
 *         PACING_DOWN_EVALS windows in a row (about a second): the highest
 *         rate whose period holds the window's 90th percentile frame cost
 *         with PACING_DOWN_MARGIN_PCT % to spare (at least one step down);
 *       - up: at most 2 % of frames missed and the 90th percentile fits in
 *         PACING_UP_ROOM_PCT % of a higher rate's period for
 *         PACING_UP_EVALS windows: the highest such rate. A frame cost
 *         measured while the executor sleeps at the cap can be low (the
 *         title builds its next frame during the sleep), so a step up is a
 *         probe: one that falls back down within PACING_PROBE_OK_NS doubles
 *         the wait before the next one (PACING_PROBE_NS up to
 *         PACING_PROBE_MAX_NS). A 17 ms scene thus stays at 30 instead of
 *         flapping between 30 and 60.
 *     The schedule is absolute, so the presents are evenly paced: a steady
 *     30 rather than a rate wandering between 20 and 40.
 *
 *  3. Guest time. Vblanks keep the console's rate whatever the flip rate
 *     (the kernel's schedule, kernel_bridge.c): game logic that counts
 *     vblanks or reads a timer does not slow down at 30. Logic that
 *     advances once per flip does, so a title "locked to its ask" is never
 *     divided. A title is judged once, after PACING_LOCK_SKIP_NS of flips
 *     (logos, movies), over PACING_LOCK_WINDOW_NS: locked when at least
 *     90 % of its swaps ask for a fixed interval (not IMMEDIATE) and it
 *     sustained that rate on at least 90 % of its frames -- a title that
 *     asks the hardware for a fixed rate and holds it is treated as built
 *     around it (Burnout 3: interval ONE, 60). Until judged, a fixed-
 *     interval title runs at its ask. A title that asks IMMEDIATE, or did
 *     not hold its ask while judged, is free and gets the adaptive divisor.
 *     RECOMP_PACING_LOCK=1 / =0 forces the verdict (A/B).
 *
 * Pure and header-only (counts and times come in as arguments), so
 * tests/frame_pacing drives it on a fake clock and the executor needs no
 * new link dependency. */
#ifndef XBOXRECOMP_FRAME_PACING_H
#define XBOXRECOMP_FRAME_PACING_H

#include <stdint.h>
#include <string.h>

#define PACING_MAX_SAMPLES     256u            /* frames kept per window */
#define PACING_ASK_SWAPS       60u             /* swaps the ask is the mode of */
#define PACING_EVAL_NS         500000000ull    /* one decision per 0.5 s */
#define PACING_DOWN_EVALS      2               /* ~1 s of missed slots */
#define PACING_UP_EVALS        2               /* ~1 s of room */
#define PACING_MISS_PCT        20u
#define PACING_DOWN_MARGIN_PCT 110u
#define PACING_UP_ROOM_PCT     80u
#define PACING_MIN_FPS         15u
#define PACING_MISS_TOL_NS     200000ull       /* a frame later than its period by more: missed */
#define PACING_GAP_NS          250000000ull    /* longer: a load or a pause, not a frame */
#define PACING_PROBE_NS        4000000000ull
#define PACING_PROBE_MAX_NS    64000000000ull
#define PACING_PROBE_OK_NS     10000000000ull
#define PACING_LOCK_SKIP_NS    20000000000ull
#define PACING_LOCK_WINDOW_NS  5000000000ull
#define PACING_LOCK_MIN_FRAMES 60u
#define PACING_VBL_WINDOW_NS   1000000000ull

typedef struct {
    /* Set by the caller before the first flip. */
    uint32_t display_hz;        /* the display's refresh, 0: unknown (60) */
    int lock_force;             /* -1 judge, 0 never locked, 1 always locked */
    int adaptive;               /* 0: the ask only (RECOMP_PACING=ask) */

    /* The vblank rate, measured. */
    uint32_t vblank_hz;         /* 50 or 60; 60 until measured */
    uint32_t vbl_c0;
    uint64_t vbl_t0;
    int vbl_have;

    /* Swaps: the interval of the latest since the last flip, and the ask. */
    int have_swap;
    uint8_t cur_iv;             /* 0 IMMEDIATE, 1..7 */
    uint8_t iv_ring[PACING_ASK_SWAPS];
    uint32_t iv_n, iv_pos;
    uint32_t ask_iv;            /* the mode of iv_ring; 1 before any swap */

    uint64_t last_flip_ns;

    /* The window being measured. */
    uint64_t win_t0;
    uint32_t n, missed;
    uint32_t cost_us[PACING_MAX_SAMPLES];
    int down_streak, up_streak;

    /* The decision. */
    uint32_t rate;              /* fps the flips are paced at, 0 before the first */
    uint32_t probe_from;        /* the rate a step up left, 0: none under test */
    uint64_t probe_t, backoff_ns, retry_t;

    /* The lock verdict. */
    uint64_t first_flip_ns, lock_t0;
    uint32_t lk_flips, lk_fixed, lk_info, lk_ok;
    int locked;                 /* 0 undecided, 1 locked to its ask, -1 free */

    /* For the caller's log and report. */
    uint32_t changes;           /* bumped on each change of rate or verdict */
    const char *why;
    uint32_t last_p90_us, last_miss_pct;
} FramePacer;

static inline void frame_pacing_init(FramePacer *p, uint32_t display_hz, int lock_force, int adaptive)
{
    memset(p, 0, sizeof *p);
    p->display_hz = display_hz;
    p->lock_force = lock_force;
    p->adaptive = adaptive;
    p->vblank_hz = 60;
    p->ask_iv = 1;
}

/* A swap NOP's software-method payload (param >> 5). */
static inline void frame_pacing_swap(FramePacer *p, uint32_t payload)
{
    uint32_t iv = payload & 7u;
    p->cur_iv = (uint8_t)((payload & 8u) && !iv ? 0u : iv ? iv : 1u);
    p->have_swap = 1;
}

/* The guest vblank count at host time now_ns, sampled at each flip. */
static inline void frame_pacing_vblank(FramePacer *p, uint32_t count, uint64_t now_ns)
{
    uint64_t dt;
    uint32_t d, hz;

    if (!p->vbl_have) {
        p->vbl_have = 1;
        p->vbl_c0 = count;
        p->vbl_t0 = now_ns;
        return;
    }
    dt = now_ns - p->vbl_t0;
    if (dt < PACING_VBL_WINDOW_NS)
        return;
    d = count - p->vbl_c0;
    if (d) {
        hz = (uint32_t)(((uint64_t)d * 1000000000ull + dt / 2) / dt);
        if (hz >= 46 && hz <= 54)
            p->vblank_hz = 50;
        else if (hz >= 56 && hz <= 64)
            p->vblank_hz = 60;
    }
    p->vbl_c0 = count;
    p->vbl_t0 = now_ns;
}

static inline uint32_t frame_pacing_display(const FramePacer *p)
{
    return p->display_hz ? p->display_hz : 60u;
}

/* The rate the title asks for. */
static inline uint32_t frame_pacing_ask(const FramePacer *p)
{
    if (!p->ask_iv)
        return frame_pacing_display(p);
    return p->vblank_hz / p->ask_iv ? p->vblank_hz / p->ask_iv : 1u;
}

/* The rates the divisor picks from lie on base / k: the vblank rate for a
 * fixed interval, the display's for IMMEDIATE. */
static inline uint32_t frame_pacing_base(const FramePacer *p)
{
    return p->ask_iv ? p->vblank_hz : frame_pacing_display(p);
}

/* The candidate rate next above (dir > 0) or below (dir < 0) r; r itself
 * when there is none. Candidates: base / k, integral, not above the ask, not
 * below PACING_MIN_FPS (the ask always is one). */
static inline uint32_t frame_pacing_step(const FramePacer *p, uint32_t r, int dir)
{
    uint32_t base = frame_pacing_base(p), ask = frame_pacing_ask(p), k, c, best = r;

    for (k = 1; k <= base; k++) {
        if (base % k)
            continue;
        c = base / k;
        if (c > ask || (c < PACING_MIN_FPS && c != ask))
            continue;
        if (dir > 0 && c > r && (best == r || c < best))
            best = c;
        if (dir < 0 && c < r && (best == r || c > best))
            best = c;
    }
    return best;
}

/* The highest candidate whose period is at least need_ns; the lowest
 * candidate when none is. */
static inline uint32_t frame_pacing_fit(const FramePacer *p, uint64_t need_ns)
{
    uint32_t r = frame_pacing_ask(p), lower;

    for (;;) {
        if (1000000000ull / r >= need_ns)
            return r;
        lower = frame_pacing_step(p, r, -1);
        if (lower == r)
            return r;
        r = lower;
    }
}

/* May the divisor take this title below its ask? */
static inline int frame_pacing_free(const FramePacer *p)
{
    if (!p->adaptive)
        return 0;
    if (p->lock_force >= 0)
        return !p->lock_force;
    return p->locked < 0 || (p->locked == 0 && p->ask_iv == 0);
}

static inline void frame_pacing_set(FramePacer *p, uint32_t rate, const char *why)
{
    if (rate == p->rate)
        return;
    p->rate = rate;
    p->why = why;
    p->changes++;
}

static inline uint32_t frame_pacing_p90(FramePacer *p)
{
    uint32_t i, j, v, n = p->n < PACING_MAX_SAMPLES ? p->n : PACING_MAX_SAMPLES;
    uint32_t *a = p->cost_us;

    for (i = 1; i < n; i++) {                   /* n <= 256, twice a second */
        v = a[i];
        for (j = i; j > 0 && a[j - 1] > v; j--)
            a[j] = a[j - 1];
        a[j] = v;
    }
    return n ? a[(n * 9u) / 10u < n ? (n * 9u) / 10u : n - 1u] : 0u;
}

static inline void frame_pacing_judge_lock(FramePacer *p, uint64_t now_ns, int missed, uint32_t iv)
{
    if (p->locked || p->lock_force >= 0)
        return;
    if (now_ns - p->first_flip_ns < PACING_LOCK_SKIP_NS)
        return;
    if (!p->lock_t0)
        p->lock_t0 = now_ns;
    p->lk_flips++;
    p->lk_fixed += iv != 0;
    /* At its ask, every frame tells whether the title holds it. Below
     * (a free title the divisor has taken down), a frame held at the cap
     * says nothing, and a frame that missed even that slot says no. */
    if (p->rate == frame_pacing_ask(p)) {
        p->lk_info++;
        p->lk_ok += !missed;
    } else if (missed) {
        p->lk_info++;
    }
    if (now_ns - p->lock_t0 < PACING_LOCK_WINDOW_NS || p->lk_flips < PACING_LOCK_MIN_FRAMES)
        return;
    p->locked = (p->lk_fixed * 10u >= p->lk_flips * 9u && p->lk_info
                 && p->lk_ok * 10u >= p->lk_info * 9u) ? 1 : -1;
    p->why = p->locked > 0 ? "the title holds the fixed rate it asks for: locked to it, never divided"
                           : "the title is free to run below its ask: adaptive divisor";
    p->changes++;
}

static inline void frame_pacing_eval(FramePacer *p, uint64_t now_ns)
{
    uint32_t p90 = frame_pacing_p90(p), miss_pct = p->n ? p->missed * 100u / p->n : 0u;
    uint32_t r = p->rate, up, target;

    p->last_p90_us = p90;
    p->last_miss_pct = miss_pct;
    if (!frame_pacing_free(p)) {
        p->down_streak = p->up_streak = 0;
        return;
    }
    if (p->probe_from && now_ns - p->probe_t >= PACING_PROBE_OK_NS) {
        p->probe_from = 0;                      /* the step up held */
        p->backoff_ns = 0;
    }
    if (miss_pct > PACING_MISS_PCT) {
        p->up_streak = 0;
        if (++p->down_streak < PACING_DOWN_EVALS)
            return;
        p->down_streak = 0;
        target = frame_pacing_fit(p, (uint64_t)p90 * 1000u * PACING_DOWN_MARGIN_PCT / 100u);
        if (target >= r)
            target = frame_pacing_step(p, r, -1);
        if (target >= r)
            return;
        if (p->probe_from) {                    /* the step up did not hold */
            p->backoff_ns = p->backoff_ns ? p->backoff_ns * 2u : PACING_PROBE_NS;
            if (p->backoff_ns > PACING_PROBE_MAX_NS)
                p->backoff_ns = PACING_PROBE_MAX_NS;
            p->retry_t = now_ns + p->backoff_ns;
            p->probe_from = 0;
        }
        frame_pacing_set(p, target, "frames miss their slot: lower even rate");
        return;
    }
    p->down_streak = 0;
    up = frame_pacing_step(p, r, 1);
    if (p->missed * 50u > p->n || up == r              /* over 2 % missed: no room */
            || (uint64_t)p90 * 1000u * 100u > (1000000000ull / up) * PACING_UP_ROOM_PCT) {
        p->up_streak = 0;
        return;
    }
    if (++p->up_streak < PACING_UP_EVALS || now_ns < p->retry_t)
        return;
    p->up_streak = 0;
    target = frame_pacing_fit(p, (uint64_t)p90 * 1000u * 100u / PACING_UP_ROOM_PCT);
    if (target <= r)
        target = up;
    p->probe_from = r;
    p->probe_t = now_ns;
    frame_pacing_set(p, target, "room for a higher even rate");
}

/* A flip at now_ns; slept_ns is what the executor slept at the cap since the
 * previous flip. Returns the rate to pace the next frame at (fps, > 0). */
static inline uint32_t frame_pacing_flip(FramePacer *p, uint64_t now_ns, uint64_t slept_ns)
{
    uint32_t iv = p->have_swap ? p->cur_iv : 1u, i, counts[8], ask;
    uint64_t span, cost, period;
    int missed;

    /* The ask: the interval most used over the last swaps (a flip without a
     * swap NOP is the triple-buffer Swap: ONE). */
    p->have_swap = 0;
    p->iv_ring[p->iv_pos] = (uint8_t)iv;
    p->iv_pos = (p->iv_pos + 1u) % PACING_ASK_SWAPS;
    if (p->iv_n < PACING_ASK_SWAPS)
        p->iv_n++;
    memset(counts, 0, sizeof counts);
    for (i = 0; i < p->iv_n; i++)
        counts[p->iv_ring[i] & 7u]++;
    p->ask_iv = 0;
    for (i = 1; i < 8; i++)
        if (counts[i] > counts[p->ask_iv])
            p->ask_iv = i;
    ask = frame_pacing_ask(p);

    if (!p->rate) {                             /* the first flip */
        p->first_flip_ns = p->last_flip_ns = p->win_t0 = now_ns;
        frame_pacing_set(p, ask, "the title's ask");
        return p->rate;
    }
    span = now_ns - p->last_flip_ns;
    p->last_flip_ns = now_ns;
    cost = span > slept_ns ? span - slept_ns : 0;
    period = 1000000000ull / p->rate;
    missed = !slept_ns && cost > period + PACING_MISS_TOL_NS;

    if (p->rate > ask || !frame_pacing_free(p))
        frame_pacing_set(p, ask, "the title's ask");
    frame_pacing_judge_lock(p, now_ns, missed, iv);

    if (cost <= PACING_GAP_NS) {
        if (p->n < PACING_MAX_SAMPLES)
            p->cost_us[p->n] = (uint32_t)(cost / 1000u);
        p->n++;
        p->missed += missed;
    }
    if (now_ns - p->win_t0 >= PACING_EVAL_NS) {
        if (p->n >= 4)
            frame_pacing_eval(p, now_ns);
        p->n = p->missed = 0;
        p->win_t0 = now_ns;
    }
    return p->rate;
}

#endif /* XBOXRECOMP_FRAME_PACING_H */
