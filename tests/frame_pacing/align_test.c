/* Paced flips in phase with the display (XBA-136): the executor's schedule
 * (flip_cap_book / flip_cap_wait in nv2a_pb_exec.c, with frame_pacing_align_
 * slot), the display grid estimated from vsync samples (display_clock.h)
 * and a fake display, on a fake clock.
 *
 * The fake display refreshes every R ns from a starting phase; a present
 * submitted at s shows at the first refresh whose latch edge (the grid's
 * reference, the frame deadline Android 13 reports) comes after s. The title
 * walks for a jittered cost after each slot (the executor's wake-up up to
 * 1 ms late), the presenter submits ~1 ms after the flip. The check is the
 * standard deviation of the displayed frame times over the last 10 s (the
 * [FBWIN] frame-times line, as the panel would show them), for every
 * starting phase of the display (32 of a refresh), at 30 and 60 fps on a 60
 * and a 120 Hz panel (the AYN Thor's), with the display's true rate slightly
 * off its nominal one (59.94 / 119.88: the host and display clocks drift).
 * Without the aligner some phases fail (the XBA-107 open point), which the
 * test also checks, so it cannot pass by accident.
 *
 * Then the edge cases: a 90 Hz panel (30 aligned; 60 is 1.5 refreshes: left
 * alone), a variable-refresh display (no grid: left alone, flips even),
 * missed vsync samples, a pause and resume (samples stop, the schedule
 * restarts), a panel switching 120 -> 60 Hz, a grid that follows the flips
 * (gives up), a noisy desktop clock, RECOMP_PACING_ALIGN=0, and the full
 * RECOMP_PACING=auto pacer choosing 30 for a 20 ms title. */
#include "frame_pacing.h"
#include "display_clock.h"
#include "frame_times.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

#define MS 1000000ull
#define SEC 1000000000ull

static uint32_t s_seed = 4242u;
static double frand(void)
{
    s_seed = s_seed * 1103515245u + 12345u;
    return (double)((s_seed >> 8) & 0xFFFFu) / 65535.0;
}
static double jit(double ms) { return (frand() - 0.5) * 2.0 * ms; }

enum { SRC_EXACT, SRC_NOISY, SRC_FOLLOW, SRC_NONE };

typedef struct {
    const char *name;
    double display_hz;          /* the panel's true rate */
    uint32_t nominal_hz;        /* what it reports */
    uint32_t fps;               /* the cap's rate (0: RECOMP_PACING=auto picks) */
    double cost_ms, cost_jit_ms;
    double phase;               /* the display's starting phase, in refreshes */
    int align;                  /* RECOMP_PACING_ALIGN */
    int source;
    int vrr;                    /* the display shows each present at once */
    double seconds;
    double miss_samples;        /* fraction of vsync callbacks lost */
    double pause_at, pause_s;   /* a pause: no flips, no samples */
    double switch_at;           /* the panel goes to 60 Hz */
} Case;

typedef struct {
    double sd, mean, flip_sd;
    int state;
    uint32_t snaps;
    double worst_step_ms;       /* the largest slow correction (not a snap) */
} Out;

static double ns_d(uint64_t v) { return (double)v; }

static Out run(const Case *c)
{
    PacingAlign a;
    DisplayClock dc;
    FramePacer p;
    FrameTimes ft = {0}, fft = {0};
    double R = 1e9 / c->display_hz, phase0 = c->phase * R, lat_ema = 0;
    uint64_t t0 = 1000 * SEC, now = t0, flip = t0, next = 0, slept = 0, slot_held = 0;
    uint64_t next_sample = t0, prev_slot = 0;
    uint32_t last_rate = 0, rate, i;
    int restart = 0, paused = 0;
    double switch_t = c->switch_at > 0 ? c->switch_at : 1e9;
    Out o;
    uint32_t n;
    double mean, sd, worst;

    frame_pacing_align_init(&a, c->align, PACING_ALIGN_PHASE_PM);
    display_clock_init(&dc, SEC / c->nominal_hz);
    frame_pacing_init(&p, c->nominal_hz, -1, 1);
    o.worst_step_ms = 0;
    o.sd = o.mean = o.flip_sd = -1;

    for (i = 0; now - t0 < (uint64_t)(c->seconds * SEC); i++) {
        double t = ns_d(now - t0) / 1e9, gridR;
        uint64_t slot, present, show, lat, booked, period;
        double gphase;

        /* The panel switches to 60 Hz (a refresh-rate callback). */
        if (t >= switch_t && switch_t < 1e8) {
            phase0 = fmod(ns_d(now - t0), R) + 0.37 * (1e9 / 60.0);
            R = 1e9 / 60.0;
            display_clock_set_nominal(&dc, SEC / 60);
            switch_t = 1e9;
        }
        gridR = R;
        gphase = phase0;

        /* A pause: the app goes to the background; no flips, no vsync
         * samples, then the title resumes (the schedule restarts). */
        if (c->pause_s > 0 && !paused && t >= c->pause_at) {
            paused = 1;
            now += (uint64_t)(c->pause_s * SEC);
            flip = now;
            next_sample = now;
        }

        /* Vsync samples up to now: every refresh while the grid is not
         * locked, then every 100 ms (display_vsync_android.c). */
        while (next_sample <= now && c->source != SRC_NONE) {
            double k = ceil((ns_d(next_sample - t0) - gphase) / gridR);
            uint64_t v = t0 + (uint64_t)(gphase + k * gridR);
            if (c->source == SRC_FOLLOW) {
                /* A present that returns at the caller's phase: the last flip. */
                display_clock_sample(&dc, flip + 1 * MS, 0);
            } else if (frand() >= c->miss_samples) {
                display_clock_sample(&dc, c->source == SRC_NOISY ? v + (uint64_t)(frand() * 0.6 * MS) : v,
                                     c->source == SRC_EXACT);
            }
            next_sample = v + (dc.locked ? 100 * MS : (uint64_t)gridR);
            if (c->source == SRC_FOLLOW)
                next_sample = now + 30 * MS;
        }

        /* The walk: the executor woke at `now` (its slot, or at once). */
        flip = now + (uint64_t)((c->cost_ms + jit(c->cost_jit_ms)) * MS);
        if (slot_held)
            frame_pacing_align_flip(&a, flip - slot_held);
        slot_held = 0;
        now = flip;

        /* What the presenter does with the flip: submits ~1 ms later. */
        lat = (uint64_t)((1.0 + jit(0.4)) * MS);
        lat_ema = lat_ema ? lat_ema + (ns_d(lat) - lat_ema) / 16.0 : ns_d(lat);
        present = flip + lat;
        if (c->vrr) {
            show = present;
        } else {
            double k = floor((ns_d(present - t0) - gphase) / gridR) + 1.0;
            show = t0 + (uint64_t)(gphase + k * gridR);
        }
        if (t >= c->seconds - 10.0) {
            frame_times_add(&ft, show);
            frame_times_add(&fft, flip);
        }

        /* The rate: fixed, or RECOMP_PACING=auto's. */
        if (c->fps) {
            rate = c->fps;
        } else {
            frame_pacing_swap(&p, 8u);
            frame_pacing_vblank(&p, (uint32_t)((now - t0) * 60 / SEC), now);
            rate = frame_pacing_flip(&p, now, slept);
        }
        /* flip_cap_book. */
        period = SEC / rate;
        if (rate != last_rate) {
            last_rate = rate;
            next = 0;
        }
        if (!next) {
            next = now + period;
            restart = 1;
        } else if (now >= next + period) {
            next = now;                         /* late: no wait, restart */
            restart = 1;
            slept = 0;
            continue;
        } else {
            next += period;
        }
        {
            uint64_t ref = 0, rper = 0;
            int have = display_clock_get(&dc, &ref, &rper);
            uint32_t snaps = a.snaps;
            booked = next;
            next = frame_pacing_align_slot(&a, next, period, restart, have, ref, rper, dc.gen,
                                           (uint64_t)lat_ema, now);
            if (!restart && prev_slot && a.snaps == snaps && a.refreshes) {
                /* The slow correction: beyond the display's frame period. */
                double st = fabs(ns_d(next) - ns_d(booked)
                                 - ((double)a.refreshes * ns_d(rper) - ns_d(period))) / 1e6;
                if (st > o.worst_step_ms)
                    o.worst_step_ms = st;
            }
            prev_slot = next;
            restart = 0;
        }
        /* flip_cap_wait: a host sleep wakes up to 1 ms late. */
        slot = next;
        slept = 0;
        if (slot > now) {
            uint64_t late = (uint64_t)(frand() * 1.0 * MS);
            slept = slot - now + late;
            now = slot + late;
            slot_held = slot;
        }
    }
    if (frame_times_take(&ft, now + 20 * SEC, 1, &n, &mean, &sd, &worst)) {
        o.sd = sd;
        o.mean = mean;
    }
    if (frame_times_take(&fft, now + 20 * SEC, 1, &n, &mean, &sd, &worst))
        o.flip_sd = sd;
    o.state = a.state;
    o.snaps = a.snaps;
    if (getenv("ALIGN_DEBUG"))
        fprintf(stderr, "    [%s] gen %u snaps %u run_hot %u err %.2f ms\n", c->name, dc.gen, a.snaps,
                a.run_hot, a.err_ns / 1e6);
    return o;
}

static const char *state_name(int s)
{
    static const char *n[] = {"off", "no-clock", "not-divisor", "seeking", "locked", "gave-up"};
    return s >= 0 && s <= 5 ? n[s] : "?";
}

/* Every starting phase: the worst sd with and without the aligner. */
static void sweep(double hz, uint32_t nominal, uint32_t fps, double cost, double cost_jit)
{
    double worst_on = 0, worst_off = 0, worst_step = 0, mean = 0;
    int k, bad_off = 0, all_locked = 1;

    for (k = 0; k < 32; k++) {
        Case c = {"sweep", hz, nominal, fps, cost, cost_jit, k / 32.0, 1, SRC_EXACT, 0, 30,
                  0, 0, 0, 0};
        Out on = run(&c), off;
        c.align = 0;
        off = run(&c);
        if (on.sd > worst_on)
            worst_on = on.sd;
        if (off.sd > worst_off)
            worst_off = off.sd;
        if (on.worst_step_ms > worst_step)
            worst_step = on.worst_step_ms;
        bad_off += off.sd >= 2.0;
        all_locked &= on.state == PACING_ALIGN_LOCKED;
        mean = on.mean;
    }
    printf("  %3u fps on %6.2f Hz, cost %4.1f+-%.1f ms: 32 phases, aligned worst sd %5.2f ms "
           "(mean %.2f, slow step <= %.2f ms), unaligned worst sd %5.2f ms (%d phases >= 2 ms)\n",
           fps, hz, cost, cost_jit, worst_on, mean, worst_step, worst_off, bad_off);
    CHECK(worst_on < 2.0);
    CHECK(all_locked);
    CHECK(fabs(mean - 1000.0 / fps) < 0.2);
    CHECK(worst_step <= 1000.0 / hz / PACING_ALIGN_STEP_DIV + 0.01);
    CHECK(bad_off > 0);                     /* the problem is real without it */
}

static void units(void)
{
    DisplayClock dc;
    PacingAlign a;
    uint64_t ref, per, s;
    int i;

    /* The grid from exact samples: locks after 8, the period measured. */
    display_clock_init(&dc, SEC / 60);
    for (i = 0; i < 7; i++)
        display_clock_sample(&dc, 5 * SEC + (uint64_t)(i * 16683333.0), 1);
    CHECK(!display_clock_get(&dc, &ref, &per));
    for (; i < 40; i++)
        display_clock_sample(&dc, 5 * SEC + (uint64_t)(i * 16683333.0), 1);
    CHECK(display_clock_get(&dc, &ref, &per) && per > 16680000 && per < 16687000);
    /* A skipped vsync: n = 2, accepted. */
    CHECK(display_clock_sample(&dc, 5 * SEC + (uint64_t)(41 * 16683333.0), 1));
    /* An outlier: rejected, the lock kept. */
    CHECK(!display_clock_sample(&dc, 5 * SEC + (uint64_t)(41.5 * 16683333.0), 1));
    CHECK(dc.locked);
    /* A panel switching 60 -> 120: a new grid. */
    s = dc.gen;
    display_clock_set_nominal(&dc, SEC / 120);
    CHECK(!dc.locked && dc.gen != s);
    /* Random times (a present that never blocks, a VRR display): no lock. */
    display_clock_init(&dc, SEC / 60);
    for (i = 0, s = SEC; i < 400; i++)
        display_clock_sample(&dc, s += (uint64_t)((20.0 + jit(9.0)) * MS), 0);
    CHECK(!dc.locked);
    /* No nominal rate: consecutive exact vsyncs give it. */
    display_clock_init(&dc, 0);
    for (i = 0; i < 20; i++)
        display_clock_sample(&dc, SEC + (uint64_t)(i * 8333333.0), 1);
    CHECK(display_clock_get(&dc, &ref, &per) && per > 8330000 && per < 8337000);

    /* The aligner: a 30 fps slot half a refresh off moves a 32nd of a refresh,
     * or all of it at a restart; 60 on 90 Hz is not whole refreshes. */
    frame_pacing_align_init(&a, 1, 500);
    s = frame_pacing_align_slot(&a, 10 * SEC, SEC / 30, 0, 1, 10 * SEC, SEC / 60, 1, 0, 9 * SEC);
    CHECK(s == 10 * SEC + SEC / 120);                   /* a new grid: snap */
    s = frame_pacing_align_slot(&a, 10 * SEC + 2 * (SEC / 60), 2 * (SEC / 60), 0, 1, 10 * SEC,
                                SEC / 60, 1, 0, 10 * SEC);
    CHECK(s == 10 * SEC + 2 * (SEC / 60) + SEC / 60 / 32);  /* slow */
    CHECK(a.refreshes == 2 && a.state == PACING_ALIGN_SEEKING);
    (void)frame_pacing_align_slot(&a, 11 * SEC, SEC / 60, 0, 1, 10 * SEC, SEC / 90, 1, 0, 10 * SEC);
    CHECK(a.state == PACING_ALIGN_NOT_DIVISOR);
    (void)frame_pacing_align_slot(&a, 30 * SEC, SEC / 30, 0, 1, 10 * SEC, SEC / 60, 1, 0, 30 * SEC);
    CHECK(a.state == PACING_ALIGN_NO_CLOCK);            /* stale grid */
    /* A grid that moves with every slot: the steps keep one sign, given up
     * after two runs of 64 frames; a new grid tries again. */
    frame_pacing_align_init(&a, 1, 500);
    for (i = 0, s = 20 * SEC; i < 200 && a.state != PACING_ALIGN_GAVE_UP; i++) {
        s += 2 * (SEC / 60);
        s = frame_pacing_align_slot(&a, s, 2 * (SEC / 60), 0, 1, s - SEC / 60, SEC / 60, 7, 0, s);
    }
    CHECK(a.state == PACING_ALIGN_GAVE_UP && i > 100 && i <= 140);
    CHECK(frame_pacing_align_slot(&a, s + 1000, 2 * (SEC / 60), 0, 1, s, SEC / 60, 7, 0, s) == s + 1000);
    (void)frame_pacing_align_slot(&a, s + 1000, 2 * (SEC / 60), 0, 1, s, SEC / 60, 8, 0, s);
    CHECK(a.state == PACING_ALIGN_SEEKING);
    frame_pacing_align_init(&a, 0, 500);
    CHECK(frame_pacing_align_slot(&a, 12345, SEC / 30, 1, 1, 0, SEC / 60, 1, 0, 0) == 12345);
}

int main(void)
{
    Out o;

    units();

    /* Every starting phase: 30 and 60 on 60 and 120 Hz (true rates a
     * little off the nominal: the clocks drift). */
    sweep(59.94, 60, 30, 20.0, 1.5);
    sweep(59.94, 60, 60, 10.0, 1.0);
    sweep(119.88, 120, 30, 20.0, 1.5);
    sweep(119.88, 120, 60, 10.0, 1.0);
    sweep(60.0, 60, 30, 20.0, 1.5);
    sweep(120.0, 120, 60, 6.0, 1.0);

    /* 90 Hz: 30 is 3 refreshes (aligned); 60 is 1.5 (left alone: no phase
     * makes it even, sd ~5.5 ms whatever we do). */
    {
        Case c = {"90 Hz, 30 fps", 90.0, 90, 30, 20.0, 1.5, 0.5, 1, SRC_EXACT, 0, 30, 0, 0, 0, 0};
        o = run(&c);
        printf("  %-44s sd %5.2f ms, %s\n", c.name, o.sd, state_name(o.state));
        CHECK(o.sd < 2.0 && o.state == PACING_ALIGN_LOCKED);
        c.name = "90 Hz, 60 fps (1.5 refreshes)";
        c.fps = 60;
        c.cost_ms = 10.0;
        o = run(&c);
        printf("  %-44s sd %5.2f ms, %s\n", c.name, o.sd, state_name(o.state));
        CHECK(o.state == PACING_ALIGN_NOT_DIVISOR);
    }
    /* Variable refresh: the presents never form a grid (the desktop source
     * never locks): the slots are left alone and the flips are even. */
    {
        Case c = {"VRR display, 30 fps", 60.0, 60, 30, 20.0, 1.5, 0.3, 1, SRC_NONE, 1, 30, 0, 0, 0, 0};
        o = run(&c);
        printf("  %-44s sd %5.2f ms, %s\n", c.name, o.sd, state_name(o.state));
        CHECK(o.sd < 2.0 && o.state == PACING_ALIGN_NO_CLOCK);
    }
    /* Missed vsync callbacks (40 % lost): still aligned. */
    {
        Case c = {"40 % of vsync samples lost, 120 Hz", 119.88, 120, 30, 20.0, 1.5, 0.7, 1,
                  SRC_EXACT, 0, 30, 0.4, 0, 0, 0};
        o = run(&c);
        printf("  %-44s sd %5.2f ms, %s\n", c.name, o.sd, state_name(o.state));
        CHECK(o.sd < 2.0 && o.state == PACING_ALIGN_LOCKED);
    }
    /* Paused 5 s and resumed: the schedule restarts and snaps back. */
    {
        Case c = {"pause 5 s and resume, 60 Hz", 59.94, 60, 30, 20.0, 1.5, 0.45, 1, SRC_EXACT,
                  0, 40, 0, 12.0, 5.0, 0};
        o = run(&c);
        printf("  %-44s sd %5.2f ms, %s, %u snaps\n", c.name, o.sd, state_name(o.state), o.snaps);
        CHECK(o.sd < 2.0 && o.state == PACING_ALIGN_LOCKED && o.snaps >= 2);
    }
    /* The panel switches 120 -> 60 Hz at 15 s: a new grid, aligned again. */
    {
        Case c = {"panel 120 -> 60 Hz", 120.0, 120, 30, 20.0, 1.5, 0.2, 1, SRC_EXACT, 0, 35,
                  0, 0, 0, 15.0};
        o = run(&c);
        printf("  %-44s sd %5.2f ms, %s, %u snaps\n", c.name, o.sd, state_name(o.state), o.snaps);
        CHECK(o.sd < 2.0 && o.state == PACING_ALIGN_LOCKED);
    }
    /* The desktop source: blocking presents that return 0-0.6 ms after the
     * vsync. */
    {
        Case c = {"desktop present-return clock, 60 Hz", 59.94, 60, 30, 20.0, 1.5, 0.95, 1,
                  SRC_NOISY, 0, 30, 0, 0, 0, 0};
        o = run(&c);
        printf("  %-44s sd %5.2f ms, %s\n", c.name, o.sd, state_name(o.state));
        CHECK(o.sd < 2.0 && o.state == PACING_ALIGN_LOCKED);
    }
    /* A grid that follows the flips (a present returning at the caller's
     * phase, on a VRR display): it never settles (it re-acquires, or the
     * aligner gives up), and neither drags the rate nor jumps. */
    {
        Case c = {"clock follows the flips", 60.0, 60, 30, 20.0, 0.2, 0.5, 1, SRC_FOLLOW, 1, 30,
                  0, 0, 0, 0};
        o = run(&c);
        printf("  %-44s mean %6.2f ms, sd %5.2f, %s\n", c.name, o.mean, o.sd, state_name(o.state));
        CHECK(fabs(o.mean - 33.33) < 0.3 && o.sd < 2.0);  /* no drag, no jump */
    }
    /* The full RECOMP_PACING=auto: an IMMEDIATE 20 ms title on a 120 Hz
     * panel goes to 40 (3 refreshes, the panel's divisor) and lands in phase. */
    {
        Case c = {"auto: 20 ms IMMEDIATE, 120 Hz", 119.88, 120, 0, 20.0, 1.5, 0.6, 1, SRC_EXACT,
                  0, 40, 0, 0, 0, 0};
        o = run(&c);
        printf("  %-44s mean %6.2f ms, sd %5.2f, %s\n", c.name, o.mean, o.sd, state_name(o.state));
        CHECK(o.sd < 2.0 && fabs(o.mean - 25.0) < 0.3 && o.state == PACING_ALIGN_LOCKED);
    }

    if (failures)
        fprintf(stderr, "frame_pacing_align: %d failure(s)\n", failures);
    else
        puts("frame_pacing_align: ok");
    return failures ? 1 : 0;
}
