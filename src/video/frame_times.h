/* Frame times as the window presents them (XBA-29): the mean, standard
 * deviation and worst interval between two presents over a period, for the
 * "even frame times" check on a 120 Hz screen (standard deviation under
 * 2 ms). Header-only and pure, so a test drives it with numbers. */
#ifndef XBOXRECOMP_FRAME_TIMES_H
#define XBOXRECOMP_FRAME_TIMES_H

#include <math.h>
#include <stdint.h>

typedef struct {
    uint64_t last_ns, start_ns;
    uint32_t n;                         /* intervals since start_ns */
    double sum, sumsq, max;             /* milliseconds */
} FrameTimes;

/* A present at now_ns. Intervals over 250 ms (a pause, a load with nothing
 * new to show) are not frame times and are left out. */
static inline void frame_times_add(FrameTimes *f, uint64_t now_ns)
{
    if (!f->start_ns)
        f->start_ns = now_ns;
    if (f->last_ns && now_ns > f->last_ns) {
        double ms = (double)(now_ns - f->last_ns) / 1e6;
        if (ms <= 250.0) {
            f->n++;
            f->sum += ms;
            f->sumsq += ms * ms;
            if (ms > f->max)
                f->max = ms;
        }
    }
    f->last_ns = now_ns;
}

/* Once period_ns has passed since the period started: fill the results,
 * start a new period and return 1; else 0. */
static inline int frame_times_take(FrameTimes *f, uint64_t now_ns, uint64_t period_ns,
                                   uint32_t *n, double *mean, double *sd, double *max)
{
    double m, v;
    if (!f->start_ns || now_ns - f->start_ns < period_ns || !f->n)
        return 0;
    m = f->sum / f->n;
    v = f->sumsq / f->n - m * m;
    *n = f->n;
    *mean = m;
    *sd = v > 0 ? sqrt(v) : 0;
    *max = f->max;
    f->start_ns = now_ns;
    f->n = 0;
    f->sum = f->sumsq = f->max = 0;
    return 1;
}

#endif /* XBOXRECOMP_FRAME_TIMES_H */
