/* display_clock.h -- the display's refresh grid, estimated from vsync
 * timestamps, for flips paced in phase with the panel (XBA-136).
 *
 * The grid is a reference vsync time and a refresh period, both on the host
 * monotonic clock (xbox_HostNowNs, CLOCK_MONOTONIC on POSIX). Samples come
 * from one of two sources:
 *
 *  - exact: the display's own vsync times (Android: AChoreographer frame
 *    callbacks, display_vsync_android.c). One sample every 100 ms is enough
 *    once locked; a skipped vsync is a sample n refreshes later.
 *  - noisy: the time a vsynced present returned (SDL_RenderPresent on the
 *    desktop), taken only when the present blocked (it waited for the
 *    refresh, so it returned just after it). A present that does not block
 *    returns at the caller's own phase, which is no clock: the caller drops
 *    those, and residuals larger than a sixth of a refresh are rejected.
 *
 * Each accepted sample n refreshes after the reference corrects the phase
 * (fully for an exact sample, a tenth for a noisy one) and the period (a
 * tenth or a twentieth of the residual per refresh). The grid is locked
 * after DCLK_LOCK_SAMPLES accepted samples whose mean absolute residual is
 * under a twentieth of a refresh, and lost after DCLK_LOSE_SAMPLES rejected
 * ones in a row (then re-acquired from scratch): a variable-refresh display
 * or a present that never blocks never locks. The period stays within 2 %
 * of the nominal refresh (the refresh-rate callback, RECOMP_DISPLAY_HZ);
 * a new nominal period more than 1 % away restarts the estimate (a panel
 * switching from 120 to 60 Hz). `gen` counts the grids: it changes when a
 * grid is locked or restarted, so a user of the grid knows its phase moved.
 *
 * Pure and header-only (times come in as arguments): one writer thread
 * owns a DisplayClock; tests/frame_pacing drives it on a fake display. */
#ifndef XBOXRECOMP_DISPLAY_CLOCK_H
#define XBOXRECOMP_DISPLAY_CLOCK_H

#include <stdint.h>
#include <string.h>

#define DCLK_LOCK_SAMPLES  8u
#define DCLK_LOSE_SAMPLES  8u
#define DCLK_MAX_GAP       1200u    /* refreshes (10 s at 120 Hz): longer restarts */

typedef struct {
    uint64_t nominal_ns;        /* the refresh the display reports, 0: unknown */
    uint64_t ref_ns;            /* a vsync on the grid (the latest estimate) */
    double period_ns;           /* the measured refresh period */
    double jit_ns;              /* mean absolute residual (EMA) */
    uint64_t last_ns;           /* the last accepted sample, 0: none */
    uint32_t good, bad;         /* accepted since (re)start, rejected in a row */
    int locked;
    uint32_t gen;
} DisplayClock;

static inline void display_clock_init(DisplayClock *c, uint64_t nominal_ns)
{
    memset(c, 0, sizeof *c);
    c->nominal_ns = nominal_ns;
}

static inline void display_clock_restart(DisplayClock *c)
{
    if (c->locked)
        c->gen++;
    c->locked = 0;
    c->good = c->bad = 0;
    c->last_ns = 0;
    c->jit_ns = 0;
    c->period_ns = (double)c->nominal_ns;
}

/* The refresh the display reports (a refresh-rate callback, the SDL mode). */
static inline void display_clock_set_nominal(DisplayClock *c, uint64_t nominal_ns)
{
    uint64_t old = c->nominal_ns;

    if (!nominal_ns)
        return;
    c->nominal_ns = nominal_ns;
    if (!old || nominal_ns * 100u > old * 101u || nominal_ns * 101u < old * 100u)
        display_clock_restart(c);
}

/* A vsync time t_ns (exact) or the return of a blocking vsynced present
 * (noisy). Returns 1 when the sample was accepted. */
static inline int display_clock_sample(DisplayClock *c, uint64_t t_ns, int exact)
{
    double dt, n, resid, tol, per;

    if (!c->last_ns) {
        if (!c->period_ns)
            c->period_ns = (double)c->nominal_ns;
        c->ref_ns = c->last_ns = t_ns;
        c->good = 1;
        c->bad = 0;
        return 1;
    }
    if (t_ns <= c->ref_ns)
        return 0;
    dt = (double)(t_ns - c->ref_ns);
    if (!c->period_ns) {
        /* No nominal refresh: the first interval of consecutive exact
         * vsyncs gives it (Choreographer posts at every refresh while it
         * acquires). */
        if (!exact || dt > 50e6 || dt < 2e6) {
            c->ref_ns = c->last_ns = t_ns;
            return 0;
        }
        c->nominal_ns = (uint64_t)dt;
        c->period_ns = dt;
        c->ref_ns = c->last_ns = t_ns;
        c->good = 2;
        return 1;
    }
    per = c->period_ns;
    n = (double)(uint64_t)(dt / per + 0.5);
    if (n < 1.0)
        return 0;                               /* the same refresh again */
    if (n > (double)DCLK_MAX_GAP) {
        display_clock_restart(c);
        return display_clock_sample(c, t_ns, exact);
    }
    resid = dt - n * per;
    tol = per / (exact ? 8.0 : 6.0);
    if (resid > tol || resid < -tol) {
        if (++c->bad >= DCLK_LOSE_SAMPLES) {
            display_clock_restart(c);
            return display_clock_sample(c, t_ns, exact);
        }
        return 0;
    }
    c->bad = 0;
    if (n <= 64.0) {                            /* a long gap's residual is mostly phase */
        per += resid / n * (exact ? 0.1 : 0.05);
        if (c->nominal_ns && (per * 100.0 > (double)c->nominal_ns * 102.0
                              || per * 102.0 < (double)c->nominal_ns * 100.0)) {
            display_clock_restart(c);
            return display_clock_sample(c, t_ns, exact);
        }
        c->period_ns = per;
    }
    c->ref_ns = exact ? t_ns : c->ref_ns + (uint64_t)(n * c->period_ns + resid * 0.1);
    c->last_ns = t_ns;
    c->jit_ns += ((resid < 0 ? -resid : resid) - c->jit_ns) * 0.2;
    c->good++;
    if (!c->locked && c->good >= DCLK_LOCK_SAMPLES && c->jit_ns * 20.0 < c->period_ns) {
        c->locked = 1;
        c->gen++;
    }
    return 1;
}

/* The grid, when locked: a vsync time and the period. */
static inline int display_clock_get(const DisplayClock *c, uint64_t *ref_ns, uint64_t *period_ns)
{
    if (!c->locked)
        return 0;
    *ref_ns = c->ref_ns;
    *period_ns = (uint64_t)(c->period_ns + 0.5);
    return 1;
}

#endif /* XBOXRECOMP_DISPLAY_CLOCK_H */
