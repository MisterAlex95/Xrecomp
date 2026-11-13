/* display_vsync_android.c -- the display's refresh grid from AChoreographer
 * (XBA-136).
 *
 * A thread of its own with an ALooper (SDL's main thread has none) gets the
 * process's AChoreographer and asks it for frame callbacks:
 *  - Android 13+ (API 33, looked up at run time since the app's minimum is
 *    30): AChoreographer_postVsyncCallback; the sample is the preferred
 *    frame timeline's deadline, the time by which a buffer must be queued
 *    to show at the next presentation -- the latch edge the flips keep half
 *    a refresh away from.
 *  - Android 11-12: AChoreographer_postFrameCallback64's frame time (the
 *    vsync tick, whose distance to the latch edge is the device's own;
 *    RECOMP_PACING_ALIGN=<phase> moves the target there).
 * A callback every refresh until the grid locks (display_clock.h), then one
 * every 100 ms (the grid drifts by microseconds per second), so the thread
 * wakes ~10 times a second. The refresh-rate callback (API 30) gives the
 * nominal period and restarts the grid when the panel switches rate. Each
 * sample republishes the grid to the sink given at start (the Xbox
 * presenter passes the executor's nv2a_pb_exec_display_clock, XBA-145);
 * a grid lost is published as none.
 *
 * Paused (app in the background), callbacks may stop: the grid then goes
 * stale for the executor (PACING_ALIGN_STALE_NS) and comes back with the
 * next callbacks. */
#include "display_vsync_android.h"
#include "display_clock.h"

#include <android/choreographer.h>
#include <android/looper.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

static display_vsync_sink s_sink;      /* who gets the grid (the Xbox executor: nv2a_pb_exec_display_clock) */

typedef void (*post_vsync_fn)(AChoreographer *, AChoreographer_vsyncCallback, void *);
typedef size_t (*preferred_fn)(const AChoreographerFrameCallbackData *);
typedef int64_t (*deadline_fn)(const AChoreographerFrameCallbackData *, size_t);

static post_vsync_fn s_post_vsync;
static preferred_fn s_preferred;
static deadline_fn s_deadline;

static pthread_t s_thread;
static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cv = PTHREAD_COND_INITIALIZER;
static int s_started;                   /* 1 up, -1 failed, 0 not yet */
static volatile int s_run;
static ALooper *s_looper;
static AChoreographer *s_ch;
static DisplayClock s_dc;
static uint64_t s_nominal_ns;
static int s_pending;                   /* a callback is posted */
static uint64_t s_next_post_ns;         /* 0: at once */

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void publish(int was_locked, uint32_t was_gen)
{
    uint64_t ref, per;
    if (display_clock_get(&s_dc, &ref, &per))
        s_sink(ref, per, s_dc.gen);
    else if (was_locked || was_gen != s_dc.gen)
        s_sink(0, 0, s_dc.gen);
}

static void sample(uint64_t t_ns)
{
    int was = s_dc.locked;
    uint32_t gen = s_dc.gen;

    display_clock_sample(&s_dc, t_ns, 1);
    publish(was, gen);
    s_pending = 0;
    s_next_post_ns = s_dc.locked ? mono_ns() + 100000000ull : 0;
}

static void on_frame(int64_t frame_ns, void *data)
{
    (void)data;
    sample((uint64_t)frame_ns);
}

static void on_vsync(const AChoreographerFrameCallbackData *d, void *data)
{
    (void)data;
    sample((uint64_t)s_deadline(d, s_preferred(d)));
}

static void on_refresh_rate(int64_t period_ns, void *data)
{
    int was = s_dc.locked;
    uint32_t gen = s_dc.gen;

    (void)data;
    if (period_ns <= 0)
        return;
    display_clock_set_nominal(&s_dc, (uint64_t)period_ns);
    publish(was, gen);
    if (!s_dc.locked)
        s_next_post_ns = 0;
}

static void post(void)
{
    if (s_post_vsync)
        s_post_vsync(s_ch, on_vsync, NULL);
    else
        AChoreographer_postFrameCallback64(s_ch, on_frame, NULL);
    s_pending = 1;
}

static void *vsync_thread(void *arg)
{
    (void)arg;
    s_looper = ALooper_prepare(0);
    s_ch = s_looper ? AChoreographer_getInstance() : NULL;
    pthread_mutex_lock(&s_mu);
    s_started = s_ch ? 1 : -1;
    pthread_cond_broadcast(&s_cv);
    pthread_mutex_unlock(&s_mu);
    if (!s_ch)
        return NULL;
    ALooper_acquire(s_looper);
    display_clock_init(&s_dc, s_nominal_ns);
    AChoreographer_registerRefreshRateCallback(s_ch, on_refresh_rate, NULL);
    while (s_run) {
        uint64_t now = mono_ns();
        int timeout = 250;
        if (!s_pending && now >= s_next_post_ns)
            post();
        if (!s_pending && s_next_post_ns > now) {
            uint64_t ms = (s_next_post_ns - now) / 1000000ull + 1u;
            timeout = ms < 250u ? (int)ms : 250;
        }
        ALooper_pollOnce(timeout, NULL, NULL, NULL);
    }
    AChoreographer_unregisterRefreshRateCallback(s_ch, on_refresh_rate, NULL);
    s_sink(0, 0, s_dc.gen + 1u);
    ALooper_release(s_looper);
    return NULL;
}

int display_vsync_android_start(uint64_t nominal_ns, display_vsync_sink sink)
{
    void *lib;
    int ok;

    if (s_run)
        return 1;
    if (!sink)
        return 0;
    s_sink = sink;
    lib = dlopen("libandroid.so", RTLD_NOW);
    if (lib) {
        s_post_vsync = (post_vsync_fn)dlsym(lib, "AChoreographer_postVsyncCallback");
        s_preferred = (preferred_fn)dlsym(lib, "AChoreographerFrameCallbackData_getPreferredFrameTimelineIndex");
        s_deadline = (deadline_fn)dlsym(lib, "AChoreographerFrameCallbackData_getFrameTimelineDeadlineNanos");
        if (!s_preferred || !s_deadline)
            s_post_vsync = NULL;
    }
    s_nominal_ns = nominal_ns;
    s_started = 0;
    s_run = 1;
    if (pthread_create(&s_thread, NULL, vsync_thread, NULL) != 0) {
        s_run = 0;
        return 0;
    }
    pthread_mutex_lock(&s_mu);
    while (!s_started)
        pthread_cond_wait(&s_cv, &s_mu);
    ok = s_started > 0;
    pthread_mutex_unlock(&s_mu);
    if (!ok) {
        s_run = 0;
        pthread_join(s_thread, NULL);
    }
    return ok;
}

void display_vsync_android_stop(void)
{
    if (!s_run)
        return;
    s_run = 0;
    if (s_looper)
        ALooper_wake(s_looper);
    pthread_join(s_thread, NULL);
}

const char *display_vsync_android_source(void)
{
    return s_post_vsync ? "frame deadlines" : "vsync ticks";
}
