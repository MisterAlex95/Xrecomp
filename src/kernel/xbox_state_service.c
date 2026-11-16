/*
 * xbox_state_service.c - an Xbox title's save states in the console front end (XBA-249, xbox_state.h)
 *
 * The title attaches to recomp_states as console "xbox", slots in <save dir>/xbox-<TITLE ID>, so the in-game
 * menu's Save state / Load state, the quick save / load chords and the slot hotkeys reach xbox_StateSaveSlot /
 * xbox_StateLoadSlot as they reach a console runtime. A state is taken with every guest thread frozen at a
 * kernel call and loads within the run that saved it (xbox_StateLoad: every thread back at the same call), or,
 * when its verdict says so, in a fresh process before the game starts (xbox_StateResume, XBA-250): the automatic
 * slot (« Resume where I left off ») is saved only then, and loaded at start with RECOMP_STATE_RESUME
 * (xbox_StateResumeAtBoot).
 *
 * The callbacks run on whatever thread asked: the UI thread (the menu), this service's thread (the pad's
 * hotkeys: they come from the OHCI controller thread, which a capture holds, so they are queued here). The game
 * may be held by the in-game menu or the background (recomp_lifecycle: the flip parks, and threads waiting on
 * the GPU never reach a kernel call): the hold is lifted for the length of the capture or the load.
 *
 * Rewind (recomp_rewind.h): this thread captures every RECOMP_REWIND_MS (default 500 ms, at least 20 times the
 * last capture's cost so a capture never takes more than 5 % of the time) into an XOR-delta ring of the
 * XBXS blobs, and steps back one capture every 125 ms while the rewind chord is held. It arms on the first pad
 * report (a title playing) or xbox_StateRewindArm, and turns itself off, saying why, when a blob does not fit half
 * the ring's budget or captures fail three times in a row (a thread that never parks would otherwise be frozen
 * for the freeze's timeout at every attempt). RECOMP_REWIND=0: off; RECOMP_REWIND_MB: the ring's budget.
 *
 * POSIX hosts (recomp_states is not built on Windows).
 */
#include "xbox_state.h"
#include "kernel.h"
#include "kernel_park.h"
#include "recomp_stack_switch.h"
#include "platform/host_time.h"
#include "platform/recomp_hotkeys.h"
#include "platform/recomp_lifecycle.h"
#include "platform/recomp_rewind.h"
#include "platform/recomp_states.h"

int xbox_path_is_initialized(void);   /* kernel_path.c */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define OP_TIMEOUT_MS 500u       /* the freeze, and the waits in progress, for a slot */
#define RW_TIMEOUT_MS 20u        /* the freeze for a rewind capture: a stall the player would feel otherwise */
#define RW_STEP_MS 125u
#define RW_MIN_MS 250u
#define RW_FAILS_OFF 3

static char s_dir[1100];
static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cv = PTHREAD_COND_INITIALIZER;
static uint32_t s_pressed;           /* hotkey actions queued for this thread */
static volatile int s_rw_held, s_armed;

/* Rewind (this thread's, the stats under s_mu). */
static recomp_rewind *s_ring;
static XboxRewindStats s_rw;
static double s_cap_ms_sum, s_step_ms_sum;

static uint64_t now_ms(void)
{
    return xbox_HostNowNs() / 1000000ull;
}

/* ---- The slots ------------------------------------------------------------------------------------------- */

/* The game held by the menu or the background is let go for the length of fn. */
static unsigned unhold(void)
{
    unsigned r = recomp_lifecycle_reasons();
    if (r)
        recomp_lifecycle_resume_for(r);
    return r;
}

static void rehold(unsigned r)
{
    if (r)
        recomp_lifecycle_pause_for(r);
}

/* XBOX_STATE_* to what the menu shows: the first codes are recomp_states' own; a capture that cannot be taken
 * now (a thread that never parks, busy) is EREFUSED too. */
static int to_recomp(int rc)
{
    if (rc <= 0 && rc >= RECOMP_STATE_EREFUSED)
        return rc;
    if (rc == XBOX_STATE_EFREEZE || rc == XBOX_STATE_EBUSY || rc == XBOX_STATE_EUNSTABLE)
        return RECOMP_STATE_EREFUSED;
    return RECOMP_STATE_EIO;
}

static int op_save(int slot, void *user)
{
    char path[1200];
    XboxStateStats st;
    unsigned held;
    int rc;
    (void)user;

    if (recomp_states_slot_path(s_dir, slot, path, sizeof path) != 0)
        return RECOMP_STATE_ESLOT;
    held = unhold();
    if (slot == RECOMP_STATE_AUTO) {
        /* The automatic slot is loaded at the next start, in a fresh process (XBA-250): only a state that can be
         * resumed there is written; otherwise the older one goes too (it is not where the player left off). */
        uint8_t *blob = NULL;
        size_t n = 0;
        char why[220];
        rc = xbox_StateCapture(OP_TIMEOUT_MS, &blob, &n, &st);
        if (rc == XBOX_STATE_OK && !xbox_StateResumable(blob, n, why, sizeof why)) {
            snprintf(st.reason, sizeof st.reason, "not resumable in another run: %s", why);
            rc = XBOX_STATE_ECHAIN;
            remove(path);
        } else if (rc == XBOX_STATE_OK) {
            rc = xbox_StateWriteSlot(path, blob, n, NULL, 0, 0);
            st.blob_bytes = n;
        }
        free(blob);
    } else {
        rc = xbox_StateSaveSlot(path, OP_TIMEOUT_MS, NULL, 0, 0, &st);
    }
    rehold(held);
    if (rc == XBOX_STATE_OK)
        printf("xbox: state slot %d saved in %s (%.1f MB)\n", slot, path, st.blob_bytes / 1048576.0);
    else
        printf("xbox: state slot %d not saved: %s%s%s\n", slot, xbox_StateError(rc), st.reason[0] ? " -- " : "",
               st.reason);
    fflush(stdout);
    return to_recomp(rc);
}

static int op_load(int slot, void *user)
{
    char path[1200];
    XboxStateLoadStats st;
    unsigned held;
    int rc;
    (void)user;

    if (recomp_states_slot_path(s_dir, slot, path, sizeof path) != 0)
        return RECOMP_STATE_ESLOT;
    held = unhold();
    {
        /* Before the game started (no guest thread yet, nothing resumed): a fresh process, the state's threads
         * are made again and re-enter their chains (XBA-250). */
        XboxParkStats ps;
        xbox_ParkStatsGet(&ps, 0);
        if (ps.threads == 0 && !xbox_StateResumed())
            rc = xbox_StateResumeSlot(path, &st);
        else
            rc = xbox_StateLoadSlot(path, OP_TIMEOUT_MS, &st);
    }
    rehold(held);
    if (rc == XBOX_STATE_OK) {
        printf("xbox: state slot %d loaded (%s)\n", slot, path);
        pthread_mutex_lock(&s_mu);
        if (s_ring)
            recomp_rewind_clear(s_ring);   /* what came after the load is not this timeline's past */
        pthread_mutex_unlock(&s_mu);
    } else {
        printf("xbox: state slot %d not loaded: %s%s%s\n", slot, xbox_StateError(rc), st.reason[0] ? " -- " : "",
               st.reason);
    }
    fflush(stdout);
    return to_recomp(rc);
}

int xbox_StateAvailable(char *why, size_t why_n)
{
    XboxParkStats ps;
    const char *r = NULL;
    char buf[160];

    xbox_ParkStatsGet(&ps, 0);
    if (!s_dir[0])
        r = "no save folder";
    else if (!recomp_states_available() || strcmp(recomp_states_console(), "xbox") != 0)
        r = "the title is not attached to the save states";
    else if (recomp_stack_switch_started() > 0)
        r = "the title switches stacks (fibers or hand-made contexts): a state cannot hold them";
    else if (xbox_ParkDropped() > 0) {
        snprintf(buf, sizeof buf, "more than %d guest threads at once", XBOX_PARK_MAX_THREADS);
        r = buf;
    } else if (ps.threads == 0)
        r = "no guest thread runs yet";
    if (why && why_n)
        snprintf(why, why_n, "%s", r ? r : "");
    return r == NULL;
}

int xbox_StateResumeAvailable(char *why, size_t why_n)
{
    if (!xbox_StateAvailable(why, why_n))
        return 0;
    if (!xbox_CallSiteCount()) {
        if (why && why_n)
            snprintf(why, why_n, "generated code without resume entries (prepare the title again)");
        return 0;
    }
    return 1;
}

int xbox_StateResumeAtBoot(void)
{
    char path[1200];
    int rc;

    if (!recomp_states_resume_enabled() || !s_dir[0])
        return 0;
    if (recomp_states_slot_path(s_dir, RECOMP_STATE_AUTO, path, sizeof path) != 0 || access(path, R_OK) != 0)
        return 0;
    rc = recomp_states_resume();
    if (rc == RECOMP_STATE_OK && xbox_StateResumed()) {
        printf("xbox: resumed where the player left off (%s)\n", path);
        fflush(stdout);
        return 1;
    }
    if (xbox_StateResumed()) {
        /* Sections were loaded in part: nothing sane to start from. */
        fprintf(stderr, "  [STATE] resume failed after the threads were made: the game cannot start\n");
        exit(1);
    }
    printf("xbox: the automatic slot was not resumed (%s): the game starts from its beginning\n",
           recomp_states_error(rc));
    fflush(stdout);
    return 0;
}

/* ---- Rewind ---------------------------------------------------------------------------------------------- */

static void rw_off(const char *fmt, const char *a, double b)
{
    pthread_mutex_lock(&s_mu);
    s_rw.on = 0;
    snprintf(s_rw.off_reason, sizeof s_rw.off_reason, fmt, a, b);
    if (s_ring) {
        recomp_rewind_destroy(s_ring);
        s_ring = NULL;
    }
    pthread_mutex_unlock(&s_mu);
    fprintf(stderr, "  [STATE] rewind off: %s\n", s_rw.off_reason);
    fflush(stderr);
}

static int s_fails;

static void rw_capture(void)
{
    XboxStateStats st;
    uint8_t *blob = NULL;
    size_t n = 0;
    uint64_t t0 = xbox_HostNowNs();
    int rc;

    if (recomp_lifecycle_reasons())
        return;                       /* held by the menu or the background: nothing moves */
    rc = xbox_StateCapture(RW_TIMEOUT_MS, &blob, &n, &st);
    if (rc == XBOX_STATE_EBUSY && strstr(st.reason, "already running"))
        return;                       /* a slot save or load: not a failure */
    if (rc != XBOX_STATE_OK) {
        pthread_mutex_lock(&s_mu);
        s_rw.failed++;
        pthread_mutex_unlock(&s_mu);
        if (++s_fails >= RW_FAILS_OFF)
            rw_off("%s (three captures in a row failed)", st.reason, 0);
        return;
    }
    s_fails = 0;
    pthread_mutex_lock(&s_mu);
    if (s_ring && n > recomp_rewind_budget(s_ring) / 2) {
        pthread_mutex_unlock(&s_mu);
        free(blob);
        rw_off("a state of %s%.1f MB does not fit half the ring (RECOMP_REWIND_MB raises it)", "",
               n / 1048576.0);
        return;
    }
    if (s_ring && recomp_rewind_push(s_ring, blob, n) == 0) {
        double ms = (double)(xbox_HostNowNs() - t0) / 1e6;
        s_rw.captures++;
        s_cap_ms_sum += ms;
        if (ms > s_rw.capture_ms_max)
            s_rw.capture_ms_max = ms;
        s_rw.capture_ms_avg = s_cap_ms_sum / (double)s_rw.captures;
        s_rw.blob_bytes = n;
        s_rw.ring_bytes = recomp_rewind_bytes(s_ring);
        s_rw.ring_count = recomp_rewind_count(s_ring);
    }
    pthread_mutex_unlock(&s_mu);
    free(blob);
}

/* One capture back: the newest is loaded, then dropped unless it is the last (as recomp_rewinder_step). */
static void rw_step(void)
{
    XboxStateLoadStats st;
    const uint8_t *top;
    uint8_t *copy;
    size_t n = 0;
    uint64_t t0 = xbox_HostNowNs();
    int rc;

    pthread_mutex_lock(&s_mu);
    top = s_ring ? recomp_rewind_top(s_ring, &n) : NULL;
    copy = top ? (uint8_t *)malloc(n) : NULL;
    if (copy)
        memcpy(copy, top, n);
    pthread_mutex_unlock(&s_mu);
    if (!copy)
        return;
    rc = xbox_StateLoad(copy, n, OP_TIMEOUT_MS, &st);
    free(copy);
    pthread_mutex_lock(&s_mu);
    if (rc == XBOX_STATE_OK) {
        s_rw.steps++;
        s_step_ms_sum += (double)(xbox_HostNowNs() - t0) / 1e6;
        s_rw.step_ms_avg = s_step_ms_sum / (double)s_rw.steps;
        if (s_ring && recomp_rewind_count(s_ring) > 1)
            recomp_rewind_pop(s_ring);
    } else if (s_ring) {
        /* The game moved on to where that capture cannot load (another chain): the past before it neither. */
        recomp_rewind_clear(s_ring);
    }
    if (s_ring) {
        s_rw.ring_bytes = recomp_rewind_bytes(s_ring);
        s_rw.ring_count = recomp_rewind_count(s_ring);
    }
    pthread_mutex_unlock(&s_mu);
}

static int rw_interval_ms(void)
{
    static int base = -1;
    int ms;
    if (base < 0) {
        const char *e = getenv("RECOMP_REWIND_MS");
        base = e && atoi(e) > 0 ? atoi(e) : 500;
        if (base < (int)RW_MIN_MS)
            base = (int)RW_MIN_MS;
    }
    ms = base;
    pthread_mutex_lock(&s_mu);
    if (s_rw.capture_ms_avg * 20.0 > (double)ms)
        ms = (int)(s_rw.capture_ms_avg * 20.0);
    pthread_mutex_unlock(&s_mu);
    return ms;
}

/* ---- The service's thread -------------------------------------------------------------------------------- */

static void *service_main(void *arg)
{
    uint64_t last_cap = 0, last_step = 0;
    int was_held = 0;
    (void)arg;

    for (;;) {
        uint32_t pressed;
        int held;
        struct timespec ts;

        pthread_mutex_lock(&s_mu);
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 20 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        if (!s_pressed)
            pthread_cond_timedwait(&s_cv, &s_mu, &ts);
        pressed = s_pressed;
        s_pressed = 0;
        pthread_mutex_unlock(&s_mu);

        if (pressed)
            recomp_hotkeys_run(pressed);   /* quick save / load, slots: recomp_states, on this thread */

        if (!s_rw.on || !s_armed)
            continue;
        held = s_rw_held;
        if (held != was_held) {
            fprintf(stderr, "  [STATE] rewind %s\n", held ? "starts" : "ends");
            fflush(stderr);
            was_held = held;
            last_cap = now_ms();        /* playing on: the next capture a full interval after the last step */
        }
        if (held) {
            if (now_ms() - last_step >= RW_STEP_MS) {
                last_step = now_ms();
                rw_step();
            }
        } else if (now_ms() - last_cap >= (uint64_t)rw_interval_ms()) {
            last_cap = now_ms();
            rw_capture();
        }
    }
    return NULL;
}

void xbox_StateServiceHotkeys(uint32_t pressed, int rewind_held)
{
    s_armed = 1;
    s_rw_held = rewind_held;
    if (!pressed)
        return;
    pthread_mutex_lock(&s_mu);
    s_pressed |= pressed;
    pthread_cond_signal(&s_cv);
    pthread_mutex_unlock(&s_mu);
}

void xbox_StateRewindArm(void)
{
    s_armed = 1;
}

void xbox_StateRewindStats(XboxRewindStats *out)
{
    pthread_mutex_lock(&s_mu);
    *out = s_rw;
    pthread_mutex_unlock(&s_mu);
}

void xbox_StateServiceStart(void)
{
    static int started;
    recomp_rewind_cfg cfg;
    recomp_state_ops ops;
    const char *save;
    pthread_t t;

    /* Again once the paths are there (a host that sets them after the bridge): the attach only. */
    if (started && s_dir[0])
        return;
    save = xbox_path_is_initialized() ? xbox_path_save_dir_utf8() : NULL;
    if (!save || !*save || snprintf(s_dir, sizeof s_dir, "%s/xbox-%08X", save, xbox_path_title_id())
                                >= (int)sizeof s_dir)
        s_dir[0] = 0;
    memset(&ops, 0, sizeof ops);
    ops.console = "xbox";
    ops.save = op_save;
    ops.load = op_load;
    recomp_states_attach(&ops, s_dir);
    if (started)
        return;
    started = 1;

    recomp_rewind_config("xbox", &cfg);
    memset(&s_rw, 0, sizeof s_rw);
    if (cfg.on) {
        s_ring = recomp_rewind_create(cfg.budget);
        s_rw.on = s_ring != NULL;
    }
    fprintf(stderr, "  [STATE] save states %s%s; rewind %s (%zu MB ring, a capture every %d ms or more, from the"
                    " first pad report)\n", s_dir[0] ? "in " : "off (no save dir)", s_dir,
            s_rw.on ? "on" : "off", cfg.budget >> 20, rw_interval_ms());
    fflush(stderr);
    if (pthread_create(&t, NULL, service_main, NULL) == 0)
        pthread_detach(t);
}
