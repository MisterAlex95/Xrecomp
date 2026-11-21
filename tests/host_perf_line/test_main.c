/*
 * The performance line every console logs (host_perf_line.c, XBA-243), driven with made-up clocks:
 *   format      the text of a filled window, field by field; the speed without a full rate; the shared fields the
 *               N64 line uses (their text unchanged from 0.8.0)
 *   frames      host frames: their work less their sleep, the slow ones against the budget, the worst per window,
 *               the stages per guest frame and the rest, the kinds' sums for the overlay
 *   ff          fast-forward: several guest frames per host frame, the stages per guest frame
 *   pause       a time away counts in no window; the frame in progress is dropped
 *   due         the 10 s line logged once due, its window started again; the run's line; the last lines kept
 *   extra       a runtime's own figures appended to both lines ("; <text>"), from the window's figures; nothing
 *               appended when it writes nothing
 *   live        the overlay's figures published every 500 ms, stale after 2 s, cleared
 *   budget      the budget follows the frame period unless the config sets one
 *   frame time  the host frames' length, top to top: mean and 99th percentile per window, the histogram's fine and
 *               coarse buckets and the one above, a window taken after a restart (XBA-36)
 */
#include "host_perf_line.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MS 1000000ull

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-66s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok)
        failures++;
}

static int near(double a, double b)
{
    return fabs(a - b) < 1e-6 * (fabs(b) > 1 ? fabs(b) : 1) + 1e-9;
}

static host_perf_config gba_cfg(void)
{
    host_perf_config c = {
        .console = "gba", .unit = "fps", .frame_ns = 16742706, .nstages = 3,
        .stage = {"emu", "audio", "present"}, .kind = {HOST_PERF_EMU, HOST_PERF_OTHER, HOST_PERF_RENDER},
        .rest = "other", .rest_kind = HOST_PERF_OTHER,
    };
    return c;
}

/* One host frame of the shape a console loop has: emu, audio, present, then sleep to `period`. */
static uint64_t frame(host_perf *p, uint64_t t, uint64_t emu, uint64_t audio, uint64_t present, uint64_t period,
                      int guest)
{
    host_perf_frame(p, t, NULL);
    host_perf_stage(p, 0, emu);
    host_perf_guest(p, (uint64_t)guest);
    host_perf_stage(p, 1, audio);
    host_perf_stage(p, 2, present);
    host_perf_shown(p, 1);
    uint64_t work = emu + audio + present;
    if (period > work)
        host_perf_idle(p, period - work);
    return t + (period > work ? period : work);
}

static void test_format(void)
{
    host_perf_figures f;
    char line[1024];
    memset(&f, 0, sizeof f);
    f.cfg = gba_cfg();
    f.guest_fps = 59.7;
    f.full_hz = 59.7275;
    f.stage_ms[0] = 3.14;
    f.stage_ms[1] = 0.1;
    f.stage_ms[2] = 0.4;
    f.idle_ms = 12.6;
    f.rest_ms = 0.44;
    f.worst_ms = 9.2;
    f.over_1 = 3;
    f.over_2 = 1;
    f.budget_ms = 16.742706;
    f.host_frames = 597;
    f.shown_fps = 59.7;
    f.busy_ms = 3.9;
    f.frame_mean_ms = 16.74;
    f.frame_p99_ms = 17.1;
    host_perf_format(&f, line, sizeof line);
    printf("    %s\n", line);
    check(strcmp(line, "speed 100 % (59.7 of 59.7 fps); emu 3.1 audio 0.1 present 0.4 idle 12.6 other 0.4 ms/frame; "
                       "worst 9 ms, 3 > 16.7 ms, 1 > 33.5 ms of 597 frames; shown 59.7 fps, busy 3.9 ms/frame; "
                       "frame time mean 16.7 ms, p99 17.1 ms") == 0,
          "format: the fields in their order");
    host_perf_format_frame_time(line, sizeof line, 66.66, 70.1);
    check(strcmp(line, "frame time mean 66.7 ms, p99 70.1 ms") == 0, "format: the frame time field");
    f.full_hz = 0;
    f.cfg.rest = "guest";
    host_perf_format(&f, line, sizeof line);
    check(strncmp(line, "speed ? (59.7 fps); ", 20) == 0 && strstr(line, " other ") == NULL &&
              strstr(line, "idle 12.6 guest 0.4 ms/frame") != NULL,
          "format: no full rate, the rest's own name");
    host_perf_format_speed(line, sizeof line, 15.0, 60.0, "VI/s");
    check(strcmp(line, "speed 25 % (15.0 of 60.0 VI/s)") == 0, "format: the N64's speed field");
    host_perf_format_frames(line, sizeof line, 41.2, 120, 30, 16.7, 150);
    check(strcmp(line, "worst 41 ms, 120 > 16.7 ms, 30 > 33.4 ms of 150 frames") == 0,
          "format: the N64's frames field");
    char small[16];
    int n = host_perf_format(&f, small, sizeof small);
    check(n > (int)sizeof small && strlen(small) == sizeof small - 1, "format: truncated like snprintf");
}

static void test_frames(void)
{
    host_perf p;
    host_perf_config c = gba_cfg();
    host_perf_figures f;
    uint64_t t = 1000 * MS, busy = 0;
    host_perf_init(&p, &c, t);
    check(host_perf_frame(&p, t, &busy) == 0, "frames: the first top ends no frame");
    /* 4 ms of work, slept to 16 ms: busy 4 ms */
    host_perf_stage(&p, 0, 3 * MS);
    host_perf_guest(&p, 1);
    host_perf_stage(&p, 2, 1 * MS);
    host_perf_idle(&p, 12 * MS);
    t += 16 * MS;
    check(host_perf_frame(&p, t, &busy) == 1 && busy == 4 * MS, "frames: work = length less sleep");
    /* 20 ms of work (slow), then 40 ms (slower than twice the budget) */
    host_perf_stage(&p, 0, 20 * MS);
    host_perf_guest(&p, 1);
    t += 20 * MS;
    host_perf_frame(&p, t, &busy);
    host_perf_stage(&p, 0, 38 * MS);
    host_perf_stage(&p, 2, 2 * MS);
    host_perf_guest(&p, 1);
    t += 40 * MS;
    host_perf_frame(&p, t, &busy);
    host_perf_take(&p, HOST_PERF_WIN_LINE, t, &f, 0);
    check(f.host_frames == 3 && f.guest_frames == 3, "frames: host and guest frames counted");
    check(f.over_1 == 2 && f.over_2 == 1, "frames: slow against the budget and twice it");
    check(near(f.worst_ms, 40.0), "frames: the worst frame's work");
    check(near(f.busy_ms, (4.0 + 20 + 40) / 3), "frames: mean work per host frame");
    check(near(f.frame_ms, 76.0 / 3) && near(f.stage_ms[0], 61.0 / 3) && near(f.stage_ms[2], 3.0 / 3) &&
              near(f.idle_ms, 12.0 / 3) && near(f.rest_ms, 0.0),
          "frames: stages per guest frame, the rest");
    check(near(f.emu_ms, 61.0 / 3) && near(f.render_ms, 1.0), "frames: the overlay's emu and render sums");
    check(near(f.seconds, 0.076) && near(f.guest_fps, 3 / 0.076) && near(f.speed_pct, 100.0 * f.guest_fps / f.full_hz),
          "frames: wall time, fps, speed");
    /* the rest, with a stage-less config whose rest is the guest's work (the Xbox) */
    host_perf_config x = {.console = "xbox", .nstages = 2, .stage = {"render", "present"},
                          .kind = {HOST_PERF_RENDER, HOST_PERF_RENDER}, .rest = "guest", .rest_kind = HOST_PERF_EMU};
    host_perf_init(&p, &x, 0);
    t = 0;
    for (int i = 0; i < 4; i++) {
        host_perf_frame(&p, t, NULL);
        host_perf_stage(&p, 0, 5 * MS);
        host_perf_stage(&p, 1, 1 * MS);
        host_perf_guest(&p, 1);
        host_perf_shown(&p, 1);
        t += 33 * MS;
    }
    host_perf_take(&p, HOST_PERF_WIN_RUN, t, &f, 0);
    check(near(f.rest_ms, 27.0) && near(f.emu_ms, 27.0) && near(f.render_ms, 6.0) && f.full_hz == 0 &&
              f.speed_pct == 0 && near(f.budget_ms, 16.666667),
          "frames: rest as the guest's work, no full rate, 60 Hz budget");
    check(f.host_frames == 3 && f.over_1 == 3 && f.over_2 == 0, "frames: a 30 fps title against 60 Hz");
}

static void test_ff(void)
{
    host_perf p;
    host_perf_config c = gba_cfg();
    host_perf_figures f;
    uint64_t t = 0;
    host_perf_init(&p, &c, t);
    for (int i = 0; i < 10; i++)
        t = frame(&p, t, 16 * MS, 0, 1 * MS, 17 * MS, 4); /* 4 guest frames per 17 ms host frame */
    host_perf_frame(&p, t, NULL);
    host_perf_take(&p, HOST_PERF_WIN_LINE, t, &f, 0);
    check(f.guest_frames == 40 && f.host_frames == 10 && near(f.stage_ms[0], 4.0) && near(f.frame_ms, 17.0 / 4),
          "ff: stages per guest frame");
    check(f.speed_pct > 390 && f.speed_pct < 400 && near(f.shown_fps, 10 / 0.17), "ff: speed above 100 %");
}

static void test_pause(void)
{
    host_perf p;
    host_perf_config c = gba_cfg();
    host_perf_figures f;
    uint64_t t = 0, busy;
    host_perf_init(&p, &c, t);
    for (int i = 0; i < 5; i++)
        t = frame(&p, t, 2 * MS, 0, 0, 10 * MS, 1);
    host_perf_pause(&p, t + 1 * MS);
    check(host_perf_frame(&p, t + 500 * MS, &busy) == 0, "pause: no frame while paused");
    t += 60000 * MS; /* a minute away */
    host_perf_resume(&p, t);
    check(host_perf_frame(&p, t, &busy) == 0, "pause: the frame in progress is dropped");
    host_perf_stage(&p, 0, 2 * MS);
    host_perf_guest(&p, 1);
    host_perf_idle(&p, 8 * MS);
    t += 10 * MS;
    for (int i = 0; i < 4; i++)
        t = frame(&p, t, 2 * MS, 0, 0, 10 * MS, 1);
    host_perf_frame(&p, t, NULL);
    host_perf_take(&p, HOST_PERF_WIN_RUN, t, &f, 0);
    check(f.seconds < 0.2 && f.guest_frames == 10 && near(f.worst_ms, 2.0), "pause: the run skips the time away");
    check(f.host_frames == 9, "pause: 4 + 5 host frames ended");
    host_perf_take(&p, HOST_PERF_WIN_LINE, t, &f, 0);
    check(f.guest_frames == 5 && near(f.seconds, 0.05), "pause: the line starts again at the resume");
}

static void test_due(void)
{
    host_perf p;
    host_perf_config c = gba_cfg();
    host_perf_figures f;
    char line[1024];
    uint64_t t = 5 * MS;
    host_perf_last_line(0, line, sizeof line);
    check(line[0] == 0, "due: no line before the first");
    host_perf_init(&p, &c, t);
    int logged = 0;
    for (int i = 0; i < 700; i++) {
        t = frame(&p, t, 3 * MS, 0, 1 * MS, 16742706, 1);
        logged += host_perf_log_due(&p, t);
    }
    check(logged == 1, "due: one line in 11.7 s");
    host_perf_last_line(0, line, sizeof line);
    static const char head[] = "speed 100 % (59.7 of 59.7 fps); emu 3.0 audio 0.0 present 1.0 idle 12.7 other 0.0";
    check(strncmp(line, head, sizeof head - 1) == 0,
          "due: the line kept");
    host_perf_take(&p, HOST_PERF_WIN_LINE, t, &f, 0);
    check(f.guest_frames > 100 && f.guest_frames < 120, "due: the window started again");
    host_perf_log_run(&p, t, &f);
    host_perf_last_line(1, line, sizeof line);
    check(f.guest_frames == 700 && f.host_frames == 699 && strstr(line, " of 699 frames; shown 59.7 fps") != NULL,
          "due: the run's line");
}

static void extra_text(void *ctx, const struct host_perf_figures *f, int run, char *out, size_t n)
{
    int *calls = ctx;
    if (++*calls == 3)
        return; /* the third call writes nothing */
    snprintf(out, n, "gpu %s %llu frames", run ? "run" : "line", (unsigned long long)f->guest_frames);
}

static void test_extra(void)
{
    host_perf p;
    host_perf_config c = gba_cfg();
    char line[1024];
    int calls = 0;
    uint64_t t = 5 * MS;
    c.extra = extra_text, c.extra_ctx = &calls;
    host_perf_init(&p, &c, t);
    for (int i = 0; i < 700; i++) {
        t = frame(&p, t, 3 * MS, 0, 1 * MS, 16742706, 1);
        host_perf_log_due(&p, t);
    }
    host_perf_last_line(0, line, sizeof line);
    const char *e = strstr(line, "; gpu line ");
    check(calls == 1 && e && strtoull(e + 11, NULL, 10) > 590 && strcmp(e + strlen(e) - 7, " frames") == 0,
          "extra: appended to the 10 s line, its window's frames");
    host_perf_log_run(&p, t, NULL);
    host_perf_last_line(1, line, sizeof line);
    e = strstr(line, "; gpu run 700 frames");
    check(calls == 2 && e && e[20] == 0, "extra: appended to the run's line");
    host_perf_log_run(&p, t, NULL);
    host_perf_last_line(1, line, sizeof line);
    check(calls == 3 && !strstr(line, "; gpu") && line[strlen(line) - 1] != ' ',
          "extra: nothing written, nothing appended");
}

static void test_live(void)
{
    host_perf p;
    host_perf_config c = gba_cfg();
    host_perf_figures f;
    uint64_t t = 100 * MS;
    host_perf_live_clear();
    check(host_perf_live_get(t, &f) == 0, "live: none before a publish");
    host_perf_init(&p, &c, t);
    for (int i = 0; i < 20; i++)
        t = frame(&p, t, 8 * MS, 0, 2 * MS, 20 * MS, 1); /* 400 ms */
    check(host_perf_live_get(t, &f) == 0, "live: nothing before 500 ms");
    for (int i = 0; i < 10; i++)
        t = frame(&p, t, 8 * MS, 0, 2 * MS, 20 * MS, 1);
    check(host_perf_live_get(t, &f) == 1 && f.guest_frames == 25 && near(f.emu_ms, 8.0) && near(f.render_ms, 2.0) &&
              strcmp(f.cfg.console, "gba") == 0,
          "live: published at 500 ms with the window's figures");
    check(host_perf_live_get(t + 3000 * MS, &f) == 0, "live: stale after 2 s");
    host_perf_live_clear();
    check(host_perf_live_get(t, &f) == 0, "live: cleared");
}

static void test_budget(void)
{
    host_perf p;
    host_perf_config c = gba_cfg();
    host_perf_init(&p, &c, 0);
    host_perf_set_frame_ns(&p, 20000000);
    check(p.budget_ns == 20000000, "budget: follows the frame period");
    c.budget_ns = 33 * MS;
    host_perf_init(&p, &c, 0);
    host_perf_set_frame_ns(&p, 20000000);
    check(p.budget_ns == 33 * MS && p.cfg.frame_ns == 20000000, "budget: the config's own stays");
}

/* n host frames of `len` (work 2 ms, the rest slept), each ended by the next top (the first top: at init). */
static uint64_t frames_of(host_perf *p, uint64_t t, int n, uint64_t len)
{
    for (int i = 0; i < n; i++) {
        host_perf_stage(p, 0, 2 * MS);
        host_perf_guest(p, 1);
        if (len > 2 * MS)
            host_perf_idle(p, len - 2 * MS);
        t += len;
        host_perf_frame(p, t, NULL);
    }
    return t;
}

static void test_frame_time(void)
{
    host_perf p;
    host_perf_config c = gba_cfg();
    host_perf_figures f;
    uint64_t t = 0;
    host_perf_init(&p, &c, t);
    host_perf_frame(&p, t, NULL);
    t = frames_of(&p, t, 99, 16 * MS);
    t = frames_of(&p, t, 1, 30 * MS);
    host_perf_take(&p, HOST_PERF_WIN_LINE, t, &f, 1);
    check(f.host_frames == 100 && near(f.frame_mean_ms, (99 * 16.0 + 30) / 100), "frame time: the mean, top to top");
    check(near(f.frame_p99_ms, 16.1), "frame time: p99 of 99 x 16 ms and one 30 ms");
    /* the next window: 97 x 16 ms, 3 x 30 ms (from the restart on) */
    t = frames_of(&p, t, 97, 16 * MS);
    t = frames_of(&p, t, 3, 30 * MS);
    host_perf_take(&p, HOST_PERF_WIN_LINE, t, &f, 1);
    check(f.host_frames == 100 && near(f.frame_p99_ms, 30.1), "frame time: p99 in the window after a restart");
    host_perf_take(&p, HOST_PERF_WIN_RUN, t, &f, 0);
    check(f.host_frames == 200 && near(f.frame_p99_ms, 30.1) && near(f.frame_mean_ms, (196 * 16.0 + 4 * 30) / 200),
          "frame time: the run's");
    /* the coarse buckets and the one above */
    host_perf_init(&p, &c, 0);
    host_perf_frame(&p, 0, NULL);
    t = frames_of(&p, 0, 10, 50 * MS);
    host_perf_take(&p, HOST_PERF_WIN_RUN, t, &f, 0);
    check(near(f.frame_p99_ms, 52.0) && near(f.frame_mean_ms, 50.0), "frame time: 50 ms in the 2 ms buckets");
    host_perf_init(&p, &c, 0);
    host_perf_frame(&p, 0, NULL);
    t = frames_of(&p, 0, 5, 300 * MS);
    host_perf_take(&p, HOST_PERF_WIN_RUN, t, &f, 0);
    check(near(f.frame_p99_ms, 200.0) && near(f.frame_mean_ms, 300.0), "frame time: above 200 ms, the last bucket");
    /* no frame: zeros */
    host_perf_init(&p, &c, 0);
    host_perf_take(&p, HOST_PERF_WIN_RUN, 0, &f, 0);
    check(f.frame_mean_ms == 0 && f.frame_p99_ms == 0, "frame time: none without a frame");
}

/* ---- The run summary and the Benchmark (game performance reports) ---------------------------------------------- */

static int summary_calls, summary_marks;

static void summary_cb(void *ctx, int which, const struct host_perf_figures *f, struct host_perf_kv *kv)
{
    (void)ctx;
    if (!f) {
        summary_marks++;
        return;
    }
    summary_calls++;
    host_perf_kv_num(kv, "gpu_ms", which == HOST_PERF_WIN_BENCH ? 2.5 : 1.5);
    host_perf_kv_word(kv, "gpu_thread", "off");
    host_perf_kv_word(kv, "bad", "Halo /storage"); /* not a word: dropped */
    host_perf_kv_num(kv, "Bad Key", 1);             /* not a key: dropped */
}

static double json_num(const char *json, const char *key)
{
    char k[64];
    snprintf(k, sizeof k, "\"%s\":", key);
    const char *at = strstr(json, k);
    return at ? atof(at + strlen(k)) : -1.0;
}

static char *read_text(const char *path)
{
    static char buf[8192];
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return buf;
}

static void test_summary(void)
{
    host_perf p;
    host_perf_config c = gba_cfg();
    char json[4096];
    c.summary = summary_cb;
    host_perf_init(&p, &c, 0);
    uint64_t t = 0;
    /* 10 s at 60 fps, then 10 s at 30 fps (a frame of 33 ms and one guest frame each): 1 s samples of each */
    for (int i = 0; i < 600; i++)
        t = frame(&p, t, 2 * MS, 0, 0, 16666667ull, 1);
    for (int i = 0; i < 300; i++)
        t = frame(&p, t, 30 * MS, 0, 0, 33333333ull, 1);
    host_perf_frame(&p, t, NULL);
    int n = host_perf_summary_json(&p, HOST_PERF_WIN_RUN, t, 1, json, sizeof json);
    check(n > 0 && (size_t)n < sizeof json && json[0] == '{', "summary: JSON written");
    check(strstr(json, "\"console\":\"gba\"") && strstr(json, "\"final\":1") && strstr(json, "\"bench\":0"),
          "summary: console, final, not a benchmark");
    check(json_num(json, "samples") >= 18 && json_num(json, "samples") <= 20, "summary: one sample a second");
    double med = json_num(json, "fps_shown_median"), p5 = json_num(json, "fps_shown_p5");
    check(med > 29 && med < 61 && p5 > 29 && p5 < 31, "summary: shown fps median and 5th percentile");
    check(fabs(json_num(json, "seconds") - 20.0) < 0.1, "summary: the run's seconds");
    check(json_num(json, "frame_p99_ms") > 33 && json_num(json, "frame_p99_ms") < 34, "summary: frame time p99");
    check(strstr(json, "\"stages\":{\"emu\":") && strstr(json, "\"other\":"), "summary: stages by name, the rest");
    check(strstr(json, "\"gpu_ms\":1.500") && strstr(json, "\"gpu_thread\":\"off\""), "summary: the runtime's figures");
    check(!strstr(json, "Halo") && !strstr(json, "Bad Key") && !strstr(json, "storage"),
          "summary: anything but numbers and words dropped");

    /* notes: in every summary from now */
    host_perf_summary_note_num("rsp_ms", 4.25);
    host_perf_summary_json(&p, HOST_PERF_WIN_RUN, t, 0, json, sizeof json);
    check(json_num(json, "rsp_ms") > 4.24 && json_num(json, "rsp_ms") < 4.26, "summary: a note");

    /* the file: XBA_PERF_SUMMARY (refreshed by the frame loop every 10 s, at a pause and at the run's line) */
    char path[256];
    snprintf(path, sizeof path, "%s/host_perf_summary_%d.json", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp",
             (int)getpid());
    remove(path);
    host_perf_summary_path(path);
    host_perf_pause(&p, t);
    char *text = read_text(path);
    check(text && strstr(text, "\"final\":0") && strstr(text, "\"console\":\"gba\""), "summary: written at a pause");
    host_perf_resume(&p, t + 1000 * MS);
    t += 1000 * MS;
    remove(path);
    for (int i = 0; i < 700; i++)
        t = frame(&p, t, 2 * MS, 0, 0, 16666667ull, 1);
    text = read_text(path);
    check(text != NULL, "summary: refreshed by the frame loop");
    host_perf_log_run(&p, t, NULL);
    text = read_text(path);
    check(text && strstr(text, "\"final\":1"), "summary: final at the run's line");

    /* the Benchmark: its own window and samples; the run's window goes on */
    char bpath[300];
    snprintf(bpath, sizeof bpath, "%s.bench", path);
    remove(bpath);
    host_perf_bench_take(bpath);
    host_perf_frame(&p, t, NULL);
    check(read_text(bpath) == NULL, "bench: nothing before a mark");
    host_perf_bench_mark();
    host_perf_frame(&p, t, NULL);
    check(summary_marks == 1, "bench: the runtime is told the window starts");
    for (int i = 0; i < 300; i++)
        t = frame(&p, t, 30 * MS, 0, 0, 33333333ull, 1);
    host_perf_bench_take(bpath);
    host_perf_frame(&p, t, NULL);
    text = read_text(bpath);
    check(text && strstr(text, "\"bench\":1") && strstr(text, "\"gpu_ms\":2.500"), "bench: its summary");
    check(text && fabs(json_num(text, "seconds") - 10.0) < 0.1 && json_num(text, "fps_shown_median") < 31,
          "bench: only the benchmark's 10 s");
    host_perf_summary_json(&p, HOST_PERF_WIN_RUN, t, 0, json, sizeof json);
    check(json_num(json, "seconds") > 40, "bench: the run's window goes on");
    remove(path);
    remove(bpath);
    host_perf_summary_path(NULL);
}

int main(void)
{
    printf("host_perf_line\n");
    test_format();
    test_frames();
    test_ff();
    test_pause();
    test_due();
    test_extra();
    test_live();
    test_budget();
    test_frame_time();
    test_summary();
    printf("%s (%d failure%s)\n", failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
