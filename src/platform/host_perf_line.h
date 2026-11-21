/* One performance line for every console (XBA-243).
 *
 * Every runtime (the Xbox presenter, the GBA, PS1 and N64 frame loops) logs the same line every 10 s and once over
 * the whole run when the game ends, as the N64 did first (0.6.0):
 *
 *   gba: perf speed 100 % (59.7 of 59.7 fps); emu 3.1 audio 0.1 present 0.4 idle 12.6 other 0.4 ms/frame;
 *        worst 9 ms, 0 > 16.7 ms, 0 > 33.5 ms of 597 frames; shown 59.7 fps, busy 3.9 ms/frame;
 *        frame time mean 16.7 ms, p99 17.1 ms
 *   gba: perf over the run: ...
 *
 *   speed      guest frames per second against the guest's full rate ("speed ? (29.9 fps)" without one: the Xbox,
 *              whose titles choose their own rate)
 *   stages     wall time per guest frame in each of the runtime's stages, its sleep to the next deadline (idle) and
 *              the rest no stage covers (named per runtime: "other", or "guest" on the Xbox, where the game's own
 *              work is what the presenter does not time)
 *   frames     the slowest host frame (its work, not its sleep), host frames longer than the budget and than twice
 *              it, out of the host frames
 *   shown      pictures handed to the window per second, and the mean work time of a host frame
 *   frame time the host frames' length, top to top (their work and their sleep: the pacing a player sees): the mean
 *              and the 99th percentile (XBA-36), from a histogram of 0.1 ms buckets to 40 ms, 2 ms buckets to 200 ms
 *              and one above; the percentile is its bucket's upper edge (200 for the last)
 *
 * A guest frame is one the console ran (a VI field on the N64); a host frame is one turn of the runtime's loop (one
 * flip on the Xbox), which runs several guest frames while fast-forwarding.
 *
 * The runtime feeds a host_perf from its loop: host_perf_frame at the top of every host frame, host_perf_stage /
 * host_perf_idle as it times its work and its sleep, host_perf_guest / host_perf_shown as frames run and pictures go
 * out, host_perf_pause / host_perf_resume around a time away (the app in the background). host_perf_log_due then logs
 * the 10 s line and host_perf_log_run the run's. The figures of a short window (about half a second) are published
 * for the on-screen overlay (perf_overlay.c, host_perf_live_get), so it shows the same fields on every console.
 *
 * libc and pthreads only, no SDL and no console source; one host_perf per frame loop, used from that loop's thread
 * (the live figures and the last lines logged are safe to read from any thread). Times are the caller's monotonic
 * nanoseconds (xbox_HostNowNs, CLOCK_MONOTONIC). */
#ifndef XBOXRECOMP_HOST_PERF_LINE_H
#define XBOXRECOMP_HOST_PERF_LINE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HOST_PERF_STAGES 6                    /* stages a runtime may time */
#define HOST_PERF_LINE_NS 10000000000ull      /* the line's period */
#define HOST_PERF_LIVE_NS 500000000ull        /* the overlay's window */

/* The host frames' length histogram (the frame time's percentile): 0.1 ms buckets to 40 ms, 2 ms buckets to 200 ms,
 * one bucket above. */
#define HOST_PERF_HIST_FINE 400
#define HOST_PERF_HIST_COARSE 80
#define HOST_PERF_HIST (HOST_PERF_HIST_FINE + HOST_PERF_HIST_COARSE + 1)

/* What a stage's time is, for the overlay's two sums: the console's own work (emulation, the Xbox game), drawing
 * and presenting the picture, anything else (sound). */
enum { HOST_PERF_EMU = 0, HOST_PERF_RENDER = 1, HOST_PERF_OTHER = 2 };

struct host_perf_figures;

typedef struct host_perf_config {
    const char *console;                      /* the log lines' prefix: "gba" -> "gba: perf ..." */
    const char *unit;                         /* the guest frame rate's unit in the speed field: "fps", "VI/s" */
    uint64_t frame_ns;                        /* the guest's full-speed frame (0: none, "speed ?") */
    uint64_t budget_ns;                       /* a slow host frame is longer than this (0: frame_ns, else 60 Hz) */
    int nstages;
    const char *stage[HOST_PERF_STAGES];      /* the stages' names, in the line's order (string literals) */
    int kind[HOST_PERF_STAGES];               /* HOST_PERF_EMU / RENDER / OTHER */
    const char *rest;                         /* the time no stage covers (NULL: "other") */
    int rest_kind;
    /* Optional: the runtime's own figures, appended to the logged lines as "; <text>" (not to the live figures).
     * Called with the window's figures (run = 1: the run's line); writes nothing to leave the line as it is. */
    void (*extra)(void *ctx, const struct host_perf_figures *f, int run, char *out, size_t n);
    void *extra_ctx;
} host_perf_config;

/* Running totals since host_perf_init. */
typedef struct host_perf_totals {
    uint64_t guest_frames, host_frames, shown;
    uint64_t over_1, over_2;                  /* host frames longer than the budget, than twice it */
    uint64_t busy_ns;                         /* the host frames' work (their length less their sleep) */
    uint64_t idle_ns;
    uint64_t stage_ns[HOST_PERF_STAGES];
    uint64_t frame_ns;                        /* the host frames' length (work and sleep) */
    uint32_t frame_hist[HOST_PERF_HIST];      /* their lengths (a window's: the difference, modulo 2^32) */
} host_perf_totals;

typedef struct host_perf_window {
    uint64_t t;                               /* when it started */
    host_perf_totals at;                      /* the totals then */
    uint64_t worst_ns;                        /* its slowest host frame so far */
} host_perf_window;

enum { HOST_PERF_WIN_LINE, HOST_PERF_WIN_RUN, HOST_PERF_WIN_LIVE, HOST_PERF_WINDOWS };

typedef struct host_perf {
    host_perf_config cfg;
    host_perf_totals tot;
    host_perf_window win[HOST_PERF_WINDOWS];
    uint64_t frame_t;                         /* the current host frame's start */
    uint64_t frame_idle;                      /* its sleep so far */
    int in_frame;                             /* frame_t is set (not before the first top, nor after a resume) */
    int paused;
    uint64_t paused_at;                       /* host_perf_pause's time */
    uint64_t budget_ns;                       /* the budget in use */
    int budget_auto;                          /* it follows frame_ns (the config's budget_ns was 0) */
} host_perf;

/* A window's figures (host_perf_take). Per guest frame unless said otherwise. */
typedef struct host_perf_figures {
    host_perf_config cfg;                     /* the names (a copy: the live figures outlive the host_perf) */
    double seconds;                           /* wall time covered */
    uint64_t guest_frames, host_frames, shown;
    double guest_fps, full_hz, speed_pct;     /* full_hz 0: no full rate (speed_pct 0) */
    double shown_fps;
    double stage_ms[HOST_PERF_STAGES], idle_ms, rest_ms;
    double emu_ms, render_ms;                 /* the stages (and the rest) of those kinds, summed */
    double frame_ms;                          /* wall time per guest frame */
    double busy_ms;                           /* work per host frame */
    double worst_ms;                          /* the slowest host frame's work */
    double budget_ms;
    uint64_t over_1, over_2;
    double frame_mean_ms, frame_p99_ms;       /* host frame length (top to top): mean, 99th percentile */
} host_perf_figures;

void host_perf_init(host_perf *p, const host_perf_config *cfg, uint64_t now_ns);
/* The guest's full-speed frame changed (a PAL game, a game that sets another VI rate); the budget follows it when
 * the config's budget_ns was 0. */
void host_perf_set_frame_ns(host_perf *p, uint64_t frame_ns);

/* The top of a host frame: the one before it ends (its work: the time since its start less its sleep). Returns 1
 * and its work in *busy_ns when one ended (not for the first, nor the first after a resume). Also publishes the live
 * figures every HOST_PERF_LIVE_NS. */
int host_perf_frame(host_perf *p, uint64_t now_ns, uint64_t *busy_ns);
void host_perf_stage(host_perf *p, int stage, uint64_t ns);
void host_perf_idle(host_perf *p, uint64_t ns);   /* sleep to the next deadline (counts in the frame's length) */
void host_perf_guest(host_perf *p, uint64_t frames);
void host_perf_shown(host_perf *p, uint64_t pictures);
/* A time away (the app in the background, the in-game menu's hold): it counts in no window, and the host frame in
 * progress is dropped. */
void host_perf_pause(host_perf *p, uint64_t now_ns);
void host_perf_resume(host_perf *p, uint64_t now_ns);

/* The figures of window `which` (HOST_PERF_WIN_*) up to now; restart != 0 starts it again from now. */
void host_perf_take(host_perf *p, int which, uint64_t now_ns, host_perf_figures *out, int restart);
/* The line's text after "<console>: perf " (see the top). Returns what snprintf returns. */
int host_perf_format(const host_perf_figures *f, char *out, size_t n);
/* Once HOST_PERF_LINE_NS have passed since the line's window started: logs "<console>: perf <line>" on stdout, starts
 * the window again and returns 1; else 0. */
int host_perf_log_due(host_perf *p, uint64_t now_ns);
/* At the end of the run: logs "<console>: perf over the run: <line>" on stdout (into *out too when given). */
void host_perf_log_run(host_perf *p, uint64_t now_ns, host_perf_figures *out);

/* The last lines logged by any host_perf (line = 0: the 10 s one, 1: the run's), "" before the first; for tests
 * and the logs a shell collects. */
void host_perf_last_line(int run, char *out, size_t n);

/* The latest live figures, when published less than 2 s before now_ns (by any host_perf). Returns 1 when filled. */
int host_perf_live_get(uint64_t now_ns, host_perf_figures *out);
/* No live figures until the next publish (a game ended). */
void host_perf_live_clear(void);

/* The line's shared fields, for a runtime that formats a longer line of its own around them (the N64's):
 *   "speed 25 % (15.0 of 60.0 VI/s)"   ("speed ? (15.0 fps)" when full_hz <= 0)
 *   "worst 41 ms, 120 > 16.7 ms, 30 > 33.4 ms of 150 frames" */
int host_perf_format_speed(char *out, size_t n, double guest_fps, double full_hz, const char *unit);
int host_perf_format_frames(char *out, size_t n, double worst_ms, uint64_t over_1, uint64_t over_2, double budget_ms,
                            uint64_t host_frames);
/*   "frame time mean 16.7 ms, p99 17.1 ms"   (the line's last field) */
int host_perf_format_frame_time(char *out, size_t n, double mean_ms, double p99_ms);
/* The host frames' mean length and 99th percentile between totals a and b (b taken after a); 0 without a frame. */
void host_perf_frame_time(const host_perf_totals *a, const host_perf_totals *b, double *mean_ms, double *p99_ms);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_HOST_PERF_LINE_H */
