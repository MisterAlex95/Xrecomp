/* recomp_lifecycle.h (XBA-24). */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "recomp_lifecycle.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define MAX_LISTENERS 8

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_cond = PTHREAD_COND_INITIALIZER;
/* Held across a whole pause or resume, listeners included, so a pause that
 * arrives while resume's listeners run waits its turn instead of being lost
 * (s_lock alone is released around the listeners). */
static pthread_mutex_t s_transition = PTHREAD_MUTEX_INITIALIZER;
static int      s_paused;
static unsigned s_pauses;
static unsigned s_reasons;   /* RECOMP_HOLD_* set; s_paused while any */
static struct { recomp_lifecycle_fn fn; void *user; } s_listeners[MAX_LISTENERS];
static int      s_nlisteners;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint64_t s_paused_at;

/* Called with s_lock held; copies the listeners so they run unlocked. */
static int snapshot(recomp_lifecycle_fn *fns, void **users)
{
    for (int i = 0; i < s_nlisteners; i++) {
        fns[i] = s_listeners[i].fn;
        users[i] = s_listeners[i].user;
    }
    return s_nlisteners;
}

static const char *reason_name(unsigned reasons)
{
    if (reasons == RECOMP_HOLD_BACKGROUND)
        return "background";
    if (reasons == RECOMP_HOLD_MENU)
        return "menu";
    return "background, menu";
}

void recomp_lifecycle_pause_for(unsigned reasons)
{
    recomp_lifecycle_fn fns[MAX_LISTENERS];
    void *users[MAX_LISTENERS];
    unsigned before;
    int n;

    if (!reasons)
        return;
    pthread_mutex_lock(&s_transition);
    pthread_mutex_lock(&s_lock);
    before = s_reasons;
    s_reasons |= reasons;
    if (before) {
        /* Already held (s_transition orders this after any resume): one
         * more reason, no listener. */
        unsigned now = s_reasons;
        pthread_mutex_unlock(&s_lock);
        if (now != before) {
            printf("lifecycle: still paused (%s)\n", reason_name(now));
            fflush(stdout);
        }
        pthread_mutex_unlock(&s_transition);
        return;
    }
    s_paused = 1;
    s_pauses++;
    s_paused_at = now_ms();
    n = snapshot(fns, users);
    pthread_mutex_unlock(&s_lock);

    printf("lifecycle: paused (%s)\n", reason_name(reasons));
    fflush(stdout);
    for (int i = 0; i < n; i++)
        fns[i](1, users[i]);
    pthread_mutex_unlock(&s_transition);
}

void recomp_lifecycle_resume_for(unsigned reasons)
{
    recomp_lifecycle_fn fns[MAX_LISTENERS];
    void *users[MAX_LISTENERS];
    uint64_t away;
    int n;

    pthread_mutex_lock(&s_transition);
    pthread_mutex_lock(&s_lock);
    if (!s_paused || !(s_reasons & reasons)) {
        pthread_mutex_unlock(&s_lock);
        pthread_mutex_unlock(&s_transition);
        return;
    }
    s_reasons &= ~reasons;
    if (s_reasons) {
        /* Another reason still holds the guest. */
        unsigned left = s_reasons;
        pthread_mutex_unlock(&s_lock);
        printf("lifecycle: still paused (%s)\n", reason_name(left));
        fflush(stdout);
        pthread_mutex_unlock(&s_transition);
        return;
    }
    away = now_ms() - s_paused_at;
    n = snapshot(fns, users);
    pthread_mutex_unlock(&s_lock);

    /* Listeners first (the audio device is running again when the guest
     * submits its next samples), then the guest. */
    for (int i = n - 1; i >= 0; i--)
        fns[i](0, users[i]);

    pthread_mutex_lock(&s_lock);
    s_paused = 0;
    pthread_cond_broadcast(&s_cond);
    pthread_mutex_unlock(&s_lock);
    printf("lifecycle: resumed after %llu ms\n", (unsigned long long)away);
    fflush(stdout);
    pthread_mutex_unlock(&s_transition);
}

void recomp_lifecycle_pause(void)
{
    recomp_lifecycle_pause_for(RECOMP_HOLD_BACKGROUND);
}

void recomp_lifecycle_resume(void)
{
    recomp_lifecycle_resume_for(RECOMP_HOLD_BACKGROUND);
}

unsigned recomp_lifecycle_reasons(void)
{
    return __atomic_load_n(&s_reasons, __ATOMIC_ACQUIRE);
}

int recomp_lifecycle_paused(void)
{
    return __atomic_load_n(&s_paused, __ATOMIC_ACQUIRE);
}

int recomp_lifecycle_hold(void)
{
    int waited = 0;

    if (!__atomic_load_n(&s_paused, __ATOMIC_ACQUIRE))
        return 0;
    pthread_mutex_lock(&s_lock);
    while (s_paused) {
        waited = 1;
        pthread_cond_wait(&s_cond, &s_lock);
    }
    pthread_mutex_unlock(&s_lock);
    return waited;
}

int recomp_lifecycle_listen(recomp_lifecycle_fn fn, void *user)
{
    int paused;

    if (!fn)
        return -1;
    pthread_mutex_lock(&s_lock);
    if (s_nlisteners == MAX_LISTENERS) {
        pthread_mutex_unlock(&s_lock);
        return -1;
    }
    s_listeners[s_nlisteners].fn = fn;
    s_listeners[s_nlisteners].user = user;
    s_nlisteners++;
    paused = s_paused;
    pthread_mutex_unlock(&s_lock);
    if (paused)
        fn(1, user);
    return 0;
}

unsigned recomp_lifecycle_pauses(void)
{
    return __atomic_load_n(&s_pauses, __ATOMIC_RELAXED);
}
