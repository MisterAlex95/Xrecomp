/* host_perf_line.c -- see host_perf_line.h (XBA-243). */
#include "host_perf_line.h"

#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static void sample_due(host_perf *p, uint64_t now_ns);
static void bench_requests(host_perf *p, uint64_t now_ns);
static void summary_due(host_perf *p, uint64_t now_ns, int force);

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
    sample_due(p, now_ns);
    bench_requests(p, now_ns);
    summary_due(p, now_ns, 0);
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
        summary_due(p, now_ns, 1); /* a held game the launcher ends later: its figures are on disk */
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
    p->win[HOST_PERF_WIN_BENCH].t += away;
    window_start(p, HOST_PERF_WIN_LINE, now_ns);
    window_start(p, HOST_PERF_WIN_SAMPLE, now_ns); /* the sample in progress is dropped */
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

/* the runtime's own figures after the shared ones (host_perf_config.extra) */
static void append_extra(const host_perf *p, const host_perf_figures *f, int run, char *line, size_t n)
{
    char extra[512];
    size_t len = strlen(line);

    if (!p->cfg.extra)
        return;
    extra[0] = 0;
    p->cfg.extra(p->cfg.extra_ctx, f, run, extra, sizeof extra);
    if (extra[0] && len + 2 < n)
        snprintf(line + len, n - len, "; %s", extra);
}

int host_perf_log_due(host_perf *p, uint64_t now_ns)
{
    host_perf_figures f;
    char line[1024];

    if (p->paused || now_ns - p->win[HOST_PERF_WIN_LINE].t < HOST_PERF_LINE_NS)
        return 0;
    host_perf_take(p, HOST_PERF_WIN_LINE, now_ns, &f, 1);
    host_perf_format(&f, line, sizeof line);
    append_extra(p, &f, 0, line, sizeof line);
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
    append_extra(p, &f, 1, line, sizeof line);
    printf("%s: perf over the run: %s\n", p->cfg.console, line);
    fflush(stdout);
    keep_line(1, line);
    if (out)
        *out = f;
    host_perf_summary_write(p, now_ns, 1);
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

/* ---- The run summary ----------------------------------------------------------------------------------------- */

#define SAMPLES_MAX 7200 /* 2 h of 1 s samples (a ring: the newest) */

/* The fps samples: one store for the process, owned by the host_perf that last took one (one frame loop runs a game;
 * a test's second host_perf starts it again). */
static float s_samp_speed[SAMPLES_MAX], s_samp_shown[SAMPLES_MAX];
static const host_perf *s_samp_owner;
static char s_sum_path[512];
static int s_sum_path_set;
static host_perf_kv s_notes;
static unsigned s_mark_req, s_take_req; /* counters: a request is one more than what the loop applied */
static unsigned s_mark_done, s_take_done;
static char s_take_path[512];

static int key_ok(const char *k, int dash)
{
    if (!k || !*k || strlen(k) >= 24)
        return 0;
    for (const char *c = k; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_' || (dash && *c == '-')))
            return 0;
    return 1;
}

static int kv_slot(host_perf_kv *kv, const char *key)
{
    for (int i = 0; i < kv->n; i++)
        if (!strcmp(kv->e[i].key, key))
            return i;
    if (kv->n >= HOST_PERF_KV_MAX)
        return -1;
    snprintf(kv->e[kv->n].key, sizeof kv->e[0].key, "%s", key);
    return kv->n++;
}

void host_perf_kv_num(host_perf_kv *kv, const char *key, double v)
{
    if (!kv || !key_ok(key, 0) || !isfinite(v))
        return;
    int i = kv_slot(kv, key);
    if (i >= 0)
        kv->e[i].num = v, kv->e[i].word[0] = 0;
}

void host_perf_kv_word(host_perf_kv *kv, const char *key, const char *word)
{
    if (!kv || !key_ok(key, 0) || !key_ok(word, 1))
        return;
    int i = kv_slot(kv, key);
    if (i >= 0)
        kv->e[i].num = 0, snprintf(kv->e[i].word, sizeof kv->e[0].word, "%s", word);
}

void host_perf_summary_path(const char *path)
{
    pthread_mutex_lock(&s_mu);
    snprintf(s_sum_path, sizeof s_sum_path, "%s", path ? path : "");
    s_sum_path_set = path && *path;
    pthread_mutex_unlock(&s_mu);
}

void host_perf_summary_note_num(const char *key, double v)
{
    pthread_mutex_lock(&s_mu);
    host_perf_kv_num(&s_notes, key, v);
    pthread_mutex_unlock(&s_mu);
}

void host_perf_summary_note_word(const char *key, const char *word)
{
    pthread_mutex_lock(&s_mu);
    host_perf_kv_word(&s_notes, key, word);
    pthread_mutex_unlock(&s_mu);
}

static void sample_due(host_perf *p, uint64_t now_ns)
{
    if (now_ns - p->win[HOST_PERF_WIN_SAMPLE].t < HOST_PERF_SAMPLE_NS)
        return;
    host_perf_figures f;
    host_perf_take(p, HOST_PERF_WIN_SAMPLE, now_ns, &f, 1);
    pthread_mutex_lock(&s_mu);
    if (s_samp_owner != p) {
        s_samp_owner = p;
        p->samples = p->bench_samples = 0;
    }
    s_samp_speed[p->samples % SAMPLES_MAX] = (float)f.guest_fps;
    s_samp_shown[p->samples % SAMPLES_MAX] = (float)f.shown_fps;
    p->samples++;
    pthread_mutex_unlock(&s_mu);
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

/* The median and the 5th percentile (nearest rank: the sample at or under which 5 % of them are) of v[0..n). */
static void median_p5(float *v, int n, double *median, double *p5)
{
    *median = *p5 = 0;
    if (n <= 0)
        return;
    qsort(v, (size_t)n, sizeof *v, cmp_float);
    *median = n % 2 ? v[n / 2] : 0.5 * ((double)v[n / 2 - 1] + (double)v[n / 2]);
    int r = (5 * n + 99) / 100; /* ceil(0.05 n) */
    *p5 = v[r > 0 ? r - 1 : 0];
}

static size_t put(char *out, size_t n, size_t o, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static size_t put(char *out, size_t n, size_t o, const char *fmt, ...)
{
    if (o >= n)
        return o;
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(out + o, n - o, fmt, ap);
    va_end(ap);
    return k < 0 ? o : o + (size_t)k;
}

static double fin(double v)
{
    return isfinite(v) ? v : 0.0;
}

static size_t put_kv(char *out, size_t n, size_t o, const host_perf_kv *kv, int words, int *first)
{
    for (int i = 0; i < kv->n; i++) {
        if (!words != !kv->e[i].word[0])
            continue;
        o = put(out, n, o, *first ? "" : ",");
        *first = 0;
        if (words)
            o = put(out, n, o, "\"%s\":\"%s\"", kv->e[i].key, kv->e[i].word);
        else
            o = put(out, n, o, "\"%s\":%.3f", kv->e[i].key, fin(kv->e[i].num));
    }
    return o;
}

int host_perf_summary_json(host_perf *p, int which, uint64_t now_ns, int final, char *out, size_t n)
{
    host_perf_figures f;
    host_perf_kv kv;
    double sm = 0, s5 = 0, hm = 0, h5 = 0;
    int ns = 0;

    if (which != HOST_PERF_WIN_BENCH)
        which = HOST_PERF_WIN_RUN;
    host_perf_take(p, which, now_ns, &f, 0);
    memset(&kv, 0, sizeof kv);
    if (p->cfg.summary)
        p->cfg.summary(p->cfg.extra_ctx, which, &f, &kv);
    pthread_mutex_lock(&s_mu);
    for (int i = 0; i < s_notes.n; i++) {
        if (s_notes.e[i].word[0])
            host_perf_kv_word(&kv, s_notes.e[i].key, s_notes.e[i].word);
        else
            host_perf_kv_num(&kv, s_notes.e[i].key, s_notes.e[i].num);
    }
    if (s_samp_owner == p) {
        uint64_t from = which == HOST_PERF_WIN_BENCH ? p->bench_samples : 0;
        uint64_t cnt = p->samples - from;
        ns = cnt > SAMPLES_MAX ? SAMPLES_MAX : (int)cnt;
    }
    float *a = ns ? malloc(sizeof(float) * 2 * (size_t)ns) : NULL;
    if (a) {
        for (int i = 0; i < ns; i++) {
            uint64_t k = (p->samples - (uint64_t)ns + (uint64_t)i) % SAMPLES_MAX;
            a[i] = s_samp_speed[k];
            a[ns + i] = s_samp_shown[k];
        }
    } else {
        ns = 0;
    }
    pthread_mutex_unlock(&s_mu);
    if (a) {
        median_p5(a, ns, &sm, &s5);
        median_p5(a + ns, ns, &hm, &h5);
        free(a);
    }

    size_t o = 0;
    o = put(out, n, o, "{\"v\":1,\"pid\":%ld,\"console\":\"%s\",\"final\":%d,\"bench\":%d,\"seconds\":%.3f,",
            (long)getpid(), key_ok(f.cfg.console, 1) ? f.cfg.console : "host", final ? 1 : 0,
            which == HOST_PERF_WIN_BENCH, fin(f.seconds));
    o = put(out, n, o, "\"guest_frames\":%llu,\"host_frames\":%llu,\"speed_pct\":%.3f,\"guest_fps\":%.3f,"
            "\"full_hz\":%.3f,\"shown_fps\":%.3f,", (unsigned long long)f.guest_frames,
            (unsigned long long)f.host_frames, fin(f.speed_pct), fin(f.guest_fps), fin(f.full_hz), fin(f.shown_fps));
    o = put(out, n, o, "\"fps_speed_median\":%.3f,\"fps_speed_p5\":%.3f,\"fps_shown_median\":%.3f,"
            "\"fps_shown_p5\":%.3f,\"samples\":%d,", sm, s5, hm, h5, ns);
    o = put(out, n, o, "\"frame_mean_ms\":%.3f,\"frame_p99_ms\":%.3f,\"worst_ms\":%.3f,\"over_1\":%llu,"
            "\"over_2\":%llu,\"budget_ms\":%.3f,\"busy_ms\":%.3f,\"emu_ms\":%.3f,\"render_ms\":%.3f,"
            "\"idle_ms\":%.3f,\"stages\":{", fin(f.frame_mean_ms), fin(f.frame_p99_ms), fin(f.worst_ms),
            (unsigned long long)f.over_1, (unsigned long long)f.over_2, fin(f.budget_ms), fin(f.busy_ms),
            fin(f.emu_ms), fin(f.render_ms), fin(f.idle_ms));
    int first = 1;
    for (int i = 0; i < f.cfg.nstages && i < HOST_PERF_STAGES; i++) {
        if (!key_ok(f.cfg.stage[i], 0))
            continue;
        o = put(out, n, o, "%s\"%s\":%.3f", first ? "" : ",", f.cfg.stage[i], fin(f.stage_ms[i]));
        first = 0;
    }
    if (key_ok(f.cfg.rest, 0))
        o = put(out, n, o, "%s\"%s\":%.3f", first ? "" : ",", f.cfg.rest, fin(f.rest_ms));
    o = put(out, n, o, "},\"nums\":{");
    first = 1;
    o = put_kv(out, n, o, &kv, 0, &first);
    o = put(out, n, o, "},\"words\":{");
    first = 1;
    o = put_kv(out, n, o, &kv, 1, &first);
    o = put(out, n, o, "}}\n");
    return (int)o;
}

/* The JSON into path through a temporary file renamed over it (a reader never sees half of it). */
static int write_file(const char *path, const char *text, size_t len)
{
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return -1;
    int ok = fwrite(text, 1, len, f) == len;
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, path) != 0) {
        remove(tmp);
        return -1;
    }
    return 0;
}

static int summary_to(host_perf *p, int which, uint64_t now_ns, int final, const char *path)
{
    char json[4096];
    int len = host_perf_summary_json(p, which, now_ns, final, json, sizeof json);
    if (len <= 0 || (size_t)len >= sizeof json)
        return -1;
    return write_file(path, json, (size_t)len);
}

static int summary_file(char *out, size_t n)
{
    int set;
    pthread_mutex_lock(&s_mu);
    set = s_sum_path_set;
    if (set)
        snprintf(out, n, "%s", s_sum_path);
    pthread_mutex_unlock(&s_mu);
    if (!set) {
        const char *e = getenv("XBA_PERF_SUMMARY");
        snprintf(out, n, "%s", e ? e : "");
    }
    return out[0] != 0;
}

int host_perf_summary_write(host_perf *p, uint64_t now_ns, int final)
{
    char path[512];
    p->summary_t = now_ns;
    if (!summary_file(path, sizeof path))
        return -1;
    return summary_to(p, HOST_PERF_WIN_RUN, now_ns, final, path);
}

static void summary_due(host_perf *p, uint64_t now_ns, int force)
{
    if (!force && now_ns - p->summary_t < HOST_PERF_SUMMARY_NS)
        return;
    host_perf_summary_write(p, now_ns, 0);
}

void host_perf_bench_mark(void)
{
    __atomic_add_fetch(&s_mark_req, 1, __ATOMIC_SEQ_CST);
}

int host_perf_bench_take(const char *path)
{
    if (!path || !*path)
        return -1;
    pthread_mutex_lock(&s_mu);
    snprintf(s_take_path, sizeof s_take_path, "%s", path);
    pthread_mutex_unlock(&s_mu);
    __atomic_add_fetch(&s_take_req, 1, __ATOMIC_SEQ_CST);
    return 0;
}

static void bench_requests(host_perf *p, uint64_t now_ns)
{
    unsigned m = __atomic_load_n(&s_mark_req, __ATOMIC_SEQ_CST);
    if (m != s_mark_done) {
        s_mark_done = m;
        window_start(p, HOST_PERF_WIN_BENCH, now_ns);
        window_start(p, HOST_PERF_WIN_SAMPLE, now_ns);
        pthread_mutex_lock(&s_mu);
        if (s_samp_owner != p) {
            s_samp_owner = p;
            p->samples = 0;
        }
        p->bench_samples = p->samples;
        pthread_mutex_unlock(&s_mu);
        p->bench_on = 1;
        if (p->cfg.summary)
            p->cfg.summary(p->cfg.extra_ctx, HOST_PERF_WIN_BENCH, NULL, NULL);
    }
    unsigned t = __atomic_load_n(&s_take_req, __ATOMIC_SEQ_CST);
    if (t != s_take_done) {
        char path[512];
        s_take_done = t;
        pthread_mutex_lock(&s_mu);
        snprintf(path, sizeof path, "%s", s_take_path);
        pthread_mutex_unlock(&s_mu);
        if (p->bench_on)
            summary_to(p, HOST_PERF_WIN_BENCH, now_ns, 0, path);
    }
}
