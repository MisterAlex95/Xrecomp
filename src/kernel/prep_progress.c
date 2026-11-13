/* "Preparing data" (prep_progress.h): the writes to Z:, the clock and the log.
 *
 * kernel_file.c's NtWriteFile reports every successful write with the host
 * path of its file; the ones under the utility drive's folder feed the burst.
 * The window loop (fb_present_sdl.c) asks for the status text once a second;
 * a run with no window has a watcher thread instead, started when a burst
 * shows and gone when it ends, so the end line is logged there too. */
#include "prep_progress.h"

#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <pthread.h>
#include <time.h>
#include <unistd.h>

extern const char *xbox_path_utility_dir_utf8(void);

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static PrepProgress s_prep;
static int s_off = -1;                  /* RECOMP_PREP_INDICATOR=0; -1 not read yet */
static int s_watching;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int indicator_off(void)
{
    if (s_off < 0) {
        const char *e = getenv("RECOMP_PREP_INDICATOR");
        s_off = e && e[0] == '0' && !e[1];
    }
    return s_off;
}

/* Under s_lock. */
static void log_event(int ev, uint64_t bytes, uint64_t ms)
{
    switch (ev) {
    case PREP_EV_START:
        fprintf(stderr, "[PREP] preparing data: the title is writing to its utility drive (Z:)\n");
        break;
    case PREP_EV_STEP:
        fprintf(stderr, "[PREP] preparing data: %llu MB written to Z:\n",
                (unsigned long long)(bytes >> 20));
        break;
    case PREP_EV_END:
        fprintf(stderr, "[PREP] data preparation done: %llu MB written to Z: in %llu s\n",
                (unsigned long long)(bytes >> 20), (unsigned long long)((ms + 500) / 1000));
        break;
    default:
        return;
    }
    fflush(stderr);
}

static void *watch(void *unused)
{
    char buf[64];
    (void)unused;
    for (;;) {
        sleep(1);
        if (!recomp_prep_status(buf, sizeof buf))
            break;
    }
    pthread_mutex_lock(&s_lock);
    s_watching = 0;
    pthread_mutex_unlock(&s_lock);
    return NULL;
}

void xbox_UtilityWriteNote(const char *host_path, size_t n)
{
    const char *dir;
    size_t len;
    uint64_t bytes = 0, ms = 0;
    int ev, start_watch = 0;

    if (!host_path || !n || indicator_off())
        return;
    pthread_mutex_lock(&s_lock);
    dir = xbox_path_utility_dir_utf8();
    len = dir ? strlen(dir) : 0;
    if (!len || strncmp(host_path, dir, len) != 0 || (host_path[len] != '/' && host_path[len])) {
        pthread_mutex_unlock(&s_lock);
        return;
    }
    ev = prep_progress_write(&s_prep, now_ms(), n, &bytes, &ms);
    if (ev == PREP_EV_STEP)
        bytes = s_prep.logged;
    log_event(ev, bytes, ms);
    if (ev == PREP_EV_START && !s_watching)
        start_watch = s_watching = 1;
    pthread_mutex_unlock(&s_lock);
    if (start_watch) {
        pthread_t t;
        if (pthread_create(&t, NULL, watch, NULL) == 0) {
            pthread_detach(t);
        } else {
            pthread_mutex_lock(&s_lock);
            s_watching = 0;
            pthread_mutex_unlock(&s_lock);
        }
    }
}

int recomp_prep_status(char *buf, size_t n)
{
    uint64_t bytes = 0, ms = 0;
    int on;

    if (buf && n)
        buf[0] = 0;
    if (indicator_off())
        return 0;
    pthread_mutex_lock(&s_lock);
    log_event(prep_progress_expire(&s_prep, now_ms(), &bytes, &ms), bytes, ms);
    on = buf ? prep_progress_text(&s_prep, buf, n) : s_prep.shown;
    pthread_mutex_unlock(&s_lock);
    return on;
}

#else /* _WIN32: the Win32 file backend does not report its writes. */

void xbox_UtilityWriteNote(const char *host_path, size_t n)
{
    (void)host_path;
    (void)n;
}

int recomp_prep_status(char *buf, size_t n)
{
    if (buf && n)
        buf[0] = 0;
    return 0;
}

#endif
