/* recomp_lifecycle (XBA-24): the flip parks while the app is in the
 * background, without burning CPU, and listeners (the audio device) see
 * every pause and resume once. */
#define _GNU_SOURCE
#include "recomp_lifecycle.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static uint64_t ns_of(clockid_t c)
{
    struct timespec ts;
    clock_gettime(c, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* The "flip" thread: counts frames, holding at each one. */
static volatile int s_stop, s_frames, s_held;
static volatile uint64_t s_cpu_ns;

static void *flipper(void *arg)
{
    (void)arg;
    while (!__atomic_load_n(&s_stop, __ATOMIC_ACQUIRE)) {
        uint64_t c0 = ns_of(CLOCK_THREAD_CPUTIME_ID);
        if (recomp_lifecycle_hold()) {
            __atomic_add_fetch(&s_held, 1, __ATOMIC_ACQ_REL);
            s_cpu_ns = ns_of(CLOCK_THREAD_CPUTIME_ID) - c0;
        }
        __atomic_add_fetch(&s_frames, 1, __ATOMIC_ACQ_REL);
        sleep_ms(2);
    }
    return NULL;
}

static char s_log[64];
static void listener_a(int paused, void *u) { (void)u; strcat(s_log, paused ? "A+" : "A-"); }
static void listener_b(int paused, void *u) { (void)u; strcat(s_log, paused ? "B+" : "B-"); }

int main(void)
{
    pthread_t th;

    /* Not paused: hold returns at once. */
    CHECK(recomp_lifecycle_paused() == 0);
    CHECK(recomp_lifecycle_hold() == 0);
    CHECK(recomp_lifecycle_listen(NULL, NULL) == -1);

    CHECK(recomp_lifecycle_listen(listener_a, NULL) == 0);
    CHECK(recomp_lifecycle_listen(listener_b, NULL) == 0);

    pthread_create(&th, NULL, flipper, NULL);
    sleep_ms(50);
    CHECK(s_frames > 0);

    /* Pause: listeners in order, the flip parks. */
    recomp_lifecycle_pause();
    recomp_lifecycle_pause();                        /* idempotent */
    CHECK(strcmp(s_log, "A+B+") == 0);
    CHECK(recomp_lifecycle_paused() == 1);
    CHECK(recomp_lifecycle_pauses() == 1);
    sleep_ms(30);                                    /* the frame in flight ends */
    int frames = s_frames;
    sleep_ms(300);
    CHECK(s_frames == frames);                       /* no frame while paused */

    /* A listener added while paused hears about it at once. */
    s_log[0] = 0;
    CHECK(recomp_lifecycle_listen(listener_a, NULL) == 0);
    CHECK(strcmp(s_log, "A+") == 0);

    /* Resume: listeners in reverse order, frames again. */
    s_log[0] = 0;
    recomp_lifecycle_resume();
    recomp_lifecycle_resume();                       /* idempotent */
    CHECK(strcmp(s_log, "A-B-A-") == 0);
    CHECK(recomp_lifecycle_paused() == 0);
    sleep_ms(50);
    CHECK(s_frames > frames);
    CHECK(s_held == 1);
    /* Parked for ~300 ms: well under 2 % of it on the CPU (it slept). */
    printf("held thread CPU while paused: %llu us\n", (unsigned long long)(s_cpu_ns / 1000));
    CHECK(s_cpu_ns < 6000000ull);

    /* Full listener table. */
    for (int i = 0; i < 5; i++)
        CHECK(recomp_lifecycle_listen(listener_b, NULL) == 0);
    CHECK(recomp_lifecycle_listen(listener_b, NULL) == -1);

    __atomic_store_n(&s_stop, 1, __ATOMIC_RELEASE);
    pthread_join(th, NULL);
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("recomp_lifecycle: all checks passed\n");
    return 0;
}
