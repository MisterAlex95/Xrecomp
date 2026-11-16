/* host_perf_line.c -- see host_perf_line.h (XBA-243). */
#include "host_perf_line.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define DEFAULT_BUDGET_NS 16666667ull   /* 60 Hz */
#define LIVE_STALE_NS 2000000000ull

/* The live figures and the last lines logged: written by a frame loop, read by the overlay's thread or a test. */
static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static host_perf_figures s_live;
static uint64_t s_live_t;
static int s_live_ok;
static char s_last[2][1024];

static uint64_t budget_for(const host_perf_config *c, uint64_t frame_ns)
{
    if (c->budget_ns)
        return c->budget_ns;
    return frame_ns ? frame_ns : DEFAULT_BUDGET_NS;
}

static int hist_bucket(uint64_t ns)
{
    if (ns < (uint64_t)HOST_PERF_HIST_FINE * 100000ull)
        return (int)(ns / 100000ull);
    ns -= (uint64_t)HOST_PERF_HIST_FINE * 100000ull;
    if (ns < (uint64_t)HOST_PERF_HIST_COARSE * 2000000ull)
        return HOST_PERF_HIST_FINE + (int)(ns / 2000000ull);
    return HOST_PERF_HIST - 1;
}

/* A bucket's upper edge, ms (the last one's lower edge). */
static double hist_edge_ms(int i)
{
    if (i < HOST_PERF_HIST_FINE)
        return (double)(i + 1) * 0.1;
    if (i < HOST_PERF_HIST - 1)
        return HOST_PERF_HIST_FINE * 0.1 + (double)(i - HOST_PERF_HIST_FINE + 1) * 2.0;
    return HOST_PERF_HIST_FINE * 0.1 + HOST_PERF_HIST_COARSE * 2.0;
}

void host_perf_frame_time(const host_perf_totals *a, const host_perf_totals *b, double *mean_ms, double *p99_ms)
{
    uint64_t n = 0, seen = 0;
    *mean_ms = 0;
    *p99_ms = 0;
    for (int i = 0; i < HOST_PERF_HIST; i++)
        n += (uint32_t)(b->frame_hist[i] - a->frame_hist[i]);
    if (!n)
        return;
    *mean_ms = (double)(b->frame_ns - a->frame_ns) / 1e6 / (double)n;
    uint64_t rank = (99 * n + 99) / 100; /* ceil(0.99 n): the frame at or under which 99 % of them are */
    for (int i = 0; i < HOST_PERF_HIST; i++) {
        seen += (uint32_t)(b->frame_hist[i] - a->frame_hist[i]);
        if (seen >= rank) {
            *p99_ms = hist_edge_ms(i);
            return;
        }
    }
}

static void window_start(host_perf *p, int which, uint64_t now_ns)
{
    p->win[which].t = now_ns;
    p->win[which].at = p->tot;
    p->win[which].worst_ns = 0;
}

void host_perf_init(host_perf *p, const host_perf_config *cfg, uint64_t now_ns)
{
    memset(p, 0, sizeof *p);
    p->cfg = *cfg;
    if (p->cfg.nstages < 0)
        p->cfg.nstages = 0;
    if (p->cfg.nstages > HOST_PERF_STAGES)
        p->cfg.nstages = HOST_PERF_STAGES;
    if (!p->cfg.console)
        p->cfg.console = "host";
    if (!p->cfg.unit)
        p->cfg.unit = "fps";
    if (!p->cfg.rest)
        p->cfg.rest = "other";
    p->budget_auto = cfg->budget_ns == 0;
    p->budget_ns = budget_for(cfg, cfg->frame_ns);
    for (int w = 0; w < HOST_PERF_WINDOWS; w++)
        window_start(p, w, now_ns);
}

void host_perf_set_frame_ns(host_perf *p, uint64_t frame_ns)
{
    p->cfg.frame_ns = frame_ns;
    if (p->budget_auto)
        p->budget_ns = frame_ns ? frame_ns : DEFAULT_BUDGET_NS;
}

static void publish_live(host_perf *p, uint64_t now_ns)
{
    host_perf_figures f;
    host_perf_take(p, HOST_PERF_WIN_LIVE, now_ns, &f, 1);
    pthread_mutex_lock(&s_mu);
    s_live = f;
    s_live_t = now_ns;
    s_live_ok = 1;
    pthread_mutex_unlock(&s_mu);
}

int host_perf_frame(host_perf *p, uint64_t now_ns, uint64_t *busy_ns)
{
    int ended = 0;
    if (p->paused)
        return 0;
    if (p->in_frame && now_ns >= p->frame_t) {
        uint64_t len = now_ns - p->frame_t;
        uint64_t busy = len > p->frame_idle ? len - p->frame_idle : 0;
        p->tot.host_frames++;
        p->tot.busy_ns += busy;
        p->tot.over_1 += busy > p->budget_ns;
        p->tot.over_2 += busy > 2 * p->budget_ns;
        p->tot.frame_ns += len;
        p->tot.frame_hist[hist_bucket(len)]++;
        for (int w = 0; w < HOST_PERF_WINDOWS; w++)
            if (busy > p->win[w].worst_ns)
                p->win[w].worst_ns = busy;
        if (busy_ns)
            *busy_ns = busy;
        ended = 1;
    }
    p->frame_t = now_ns;
    p->frame_idle = 0;
    p->in_frame = 1;
    if (now_ns - p->win[HOST_PERF_WIN_LIVE].t >= HOST_PERF_LIVE_NS)
        publish_live(p, now_ns);
    return ended;
}

void host_perf_stage(host_perf *p, int stage, uint64_t ns)
{
    if (stage >= 0 && stage < p->cfg.nstages)
        p->tot.stage_ns[stage] += ns;
}

void host_perf_idle(host_perf *p, uint64_t ns)
{
    p->tot.idle_ns += ns;
    p->frame_idle += ns;
}

void host_perf_guest(host_perf *p, uint64_t frames)
{
    p->tot.guest_frames += frames;
}

void host_perf_shown(host_perf *p, uint64_t pictures)
{
    p->tot.shown += pictures;
}

void host_perf_pause(host_perf *p, uint64_t now_ns)
{
    if (!p->paused) {
        p->paused = 1;
        p->paused_at = now_ns;
    }
}

void host_perf_resume(host_perf *p, uint64_t now_ns)
{
    if (!p->paused)
        return;
    uint64_t away = now_ns > p->paused_at ? now_ns - p->paused_at : 0;
    p->paused = 0;
    /* The run's and the live window's clocks skip the time away; the line starts again (as the N64's did). */
    p->win[HOST_PERF_WIN_RUN].t += away;
    p->win[HOST_PERF_WIN_LIVE].t += away;
    window_start(p, HOST_PERF_WIN_LINE, now_ns);
    p->in_frame = 0;
    p->frame_idle = 0;
}

static double ms_per(uint64_t ns, uint64_t n)
{
    return n ? (double)ns / 1e6 / (double)n : 0.0;
}

void host_perf_take(host_perf *p, int which, uint64_t now_ns, host_perf_figures *out, int restart)
{
    if (which < 0 || which >= HOST_PERF_WINDOWS)
        which = HOST_PERF_WIN_LINE;
    host_perf_window *w = &p->win[which];
    const host_perf_totals *a = &w->at, *b = &p->tot;
    uint64_t end = p->paused ? p->paused_at : now_ns;
    uint64_t wall = end > w->t ? end - w->t : 0;
    uint64_t f = b->guest_frames - a->guest_frames, hf = b->host_frames - a->host_frames;

    memset(out, 0, sizeof *out);
    out->cfg = p->cfg;
    out->seconds = (double)wall / 1e9;
    out->guest_frames = f;
    out->host_frames = hf;
    out->shown = b->shown - a->shown;
    out->guest_fps = wall ? (double)f * 1e9 / (double)wall : 0.0;
    out->shown_fps = wall ? (double)out->shown * 1e9 / (double)wall : 0.0;
    out->full_hz = p->cfg.frame_ns ? 1e9 / (double)p->cfg.frame_ns : 0.0;
    out->speed_pct = out->full_hz > 0 ? 100.0 * out->guest_fps / out->full_hz : 0.0;
    out->frame_ms = ms_per(wall, f);
    double staged = 0;
    for (int i = 0; i < p->cfg.nstages; i++) {
        out->stage_ms[i] = ms_per(b->stage_ns[i] - a->stage_ns[i], f);
        staged += out->stage_ms[i];
    }
    out->idle_ms = ms_per(b->idle_ns - a->idle_ns, f);
    out->rest_ms = out->frame_ms - staged - out->idle_ms;
    if (out->rest_ms < 0)
        out->rest_ms = 0;
    for (int i = 0; i < p->cfg.nstages; i++) {
        if (p->cfg.kind[i] == HOST_PERF_EMU)
            out->emu_ms += out->stage_ms[i];
        else if (p->cfg.kind[i] == HOST_PERF_RENDER)
            out->render_ms += out->stage_ms[i];
    }
    if (p->cfg.rest_kind == HOST_PERF_EMU)
        out->emu_ms += out->rest_ms;
    else if (p->cfg.rest_kind == HOST_PERF_RENDER)
        out->render_ms += out->rest_ms;
    out->busy_ms = ms_per(b->busy_ns - a->busy_ns, hf);
    out->worst_ms = (double)w->worst_ns / 1e6;
    out->budget_ms = (double)p->budget_ns / 1e6;
    out->over_1 = b->over_1 - a->over_1;
    out->over_2 = b->over_2 - a->over_2;
    host_perf_frame_time(a, b, &out->frame_mean_ms, &out->frame_p99_ms);
    if (restart)
        window_start(p, which, now_ns);
}

int host_perf_format_speed(char *out, size_t n, double guest_fps, double full_hz, const char *unit)
{
    if (full_hz > 0)
        return snprintf(out, n, "speed %.0f %% (%.1f of %.1f %s)", 100.0 * guest_fps / full_hz, guest_fps, full_hz,
                        unit);
    return snprintf(out, n, "speed ? (%.1f %s)", guest_fps, unit);
}

int host_perf_format_frames(char *out, size_t n, double worst_ms, uint64_t over_1, uint64_t over_2, double budget_ms,
                            uint64_t host_frames)
{
    return snprintf(out, n, "worst %.0f ms, %llu > %.1f ms, %llu > %.1f ms of %llu frames", worst_ms,
                    (unsigned long long)over_1, budget_ms, (unsigned long long)over_2, 2 * budget_ms,
                    (unsigned long long)host_frames);
}

int host_perf_format_frame_time(char *out, size_t n, double mean_ms, double p99_ms)
{
    return snprintf(out, n, "frame time mean %.1f ms, p99 %.1f ms", mean_ms, p99_ms);
}

int host_perf_format(const host_perf_figures *f, char *out, size_t n)
{
    char speed[96], stages[320], frames[160], ftime[96];
    size_t o = 0;

    host_perf_format_speed(speed, sizeof speed, f->guest_fps, f->full_hz, f->cfg.unit ? f->cfg.unit : "fps");
    stages[0] = 0;
    for (int i = 0; i < f->cfg.nstages && i < HOST_PERF_STAGES && o < sizeof stages; i++) {
        int k = snprintf(stages + o, sizeof stages - o, "%s %.1f ", f->cfg.stage[i] ? f->cfg.stage[i] : "?",
                         f->stage_ms[i]);
        if (k < 0)
            break;
        o += (size_t)k;
    }
    if (o < sizeof stages)
        snprintf(stages + o, sizeof stages - o, "idle %.1f %s %.1f", f->idle_ms, f->cfg.rest ? f->cfg.rest : "other",
                 f->rest_ms);
    host_perf_format_frames(frames, sizeof frames, f->worst_ms, f->over_1, f->over_2, f->budget_ms, f->host_frames);
    host_perf_format_frame_time(ftime, sizeof ftime, f->frame_mean_ms, f->frame_p99_ms);
    return snprintf(out, n, "%s; %s ms/frame; %s; shown %.1f fps, busy %.1f ms/frame; %s", speed, stages, frames,
                    f->shown_fps, f->busy_ms, ftime);
}

static void keep_line(int run, const char *line)
{
    pthread_mutex_lock(&s_mu);
    snprintf(s_last[run], sizeof s_last[run], "%s", line);
    pthread_mutex_unlock(&s_mu);
}

int host_perf_log_due(host_perf *p, uint64_t now_ns)
{
    host_perf_figures f;
    char line[1024];

    if (p->paused || now_ns - p->win[HOST_PERF_WIN_LINE].t < HOST_PERF_LINE_NS)
        return 0;
    host_perf_take(p, HOST_PERF_WIN_LINE, now_ns, &f, 1);
    host_perf_format(&f, line, sizeof line);
    printf("%s: perf %s\n", p->cfg.console, line);
    fflush(stdout);
    keep_line(0, line);
    return 1;
}

void host_perf_log_run(host_perf *p, uint64_t now_ns, host_perf_figures *out)
{
    host_perf_figures f;
    char line[1024];

    host_perf_take(p, HOST_PERF_WIN_RUN, now_ns, &f, 0);
    host_perf_format(&f, line, sizeof line);
    printf("%s: perf over the run: %s\n", p->cfg.console, line);
    fflush(stdout);
    keep_line(1, line);
    if (out)
        *out = f;
}

void host_perf_last_line(int run, char *out, size_t n)
{
    if (!n)
        return;
    pthread_mutex_lock(&s_mu);
    snprintf(out, n, "%s", s_last[run ? 1 : 0]);
    pthread_mutex_unlock(&s_mu);
}

int host_perf_live_get(uint64_t now_ns, host_perf_figures *out)
{
    int ok;
    pthread_mutex_lock(&s_mu);
    ok = s_live_ok && (now_ns < s_live_t || now_ns - s_live_t < LIVE_STALE_NS);
    if (ok)
        *out = s_live;
    pthread_mutex_unlock(&s_mu);
    return ok;
}

void host_perf_live_clear(void)
{
    pthread_mutex_lock(&s_mu);
    s_live_ok = 0;
    pthread_mutex_unlock(&s_mu);
}
