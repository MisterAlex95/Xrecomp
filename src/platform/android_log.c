/* stdout / stderr -> logcat on Android (android_log.h). The pump is built on
 * every POSIX host, so its drain can be tested on Linux. */
#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE   /* gettid / syscall */
#endif
#include "android_log.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <time.h>
#if defined(__linux__) && !defined(__ANDROID__)
#include <sys/syscall.h>
#endif
#include <unistd.h>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

typedef struct {
    int fd;          /* read end of the pipe */
    int target_fd;   /* STDOUT_FILENO / STDERR_FILENO: the write end */
    int is_stderr;
    /* Read by xbox_log_drain, possibly in a signal handler: plain ints
     * through __atomic builtins, lock-free on every host we build for. */
    int busy;        /* between poll() saying "data" and the sink calls done */
    int partial;     /* a line without its '\n' is held in the buffer */
    int tid;         /* the pump thread's kernel tid */
    int dead;        /* the thread has ended (pipe closed) */
} log_pump;

static log_pump s_pumps[2];
static xbox_log_sink s_sink;
static pthread_mutex_t s_start_lock = PTHREAD_MUTEX_INITIALIZER;
static int s_start_result = 1;          /* 1 = not started yet */
static int s_running;                   /* atomic: pumps are up */
static int s_draining;                  /* atomic: xbox_log_drain re-entry guard */

static int current_tid(void)
{
#if defined(__ANDROID__)
    return (int)gettid();
#elif defined(__linux__)
    return (int)syscall(SYS_gettid);
#else
    /* Only compared for equality with a pump thread's own value. */
    return (int)(uintptr_t)pthread_self();
#endif
}

#define LOAD(x)     __atomic_load_n(&(x), __ATOMIC_SEQ_CST)
#define STORE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_SEQ_CST)

/* One line per sink call. A line longer than the buffer is split; a partial
 * line is held until its newline arrives (or the buffer fills).
 *
 * The busy flag is raised as soon as poll() reports data, before read()
 * takes it out of the pipe, and lowered once the sink has it: at any moment
 * a byte that was written is either still in the pipe (FIONREAD > 0) or in
 * a pump that says busy. xbox_log_drain relies on exactly that. */
static void *pump_thread(void *arg)
{
    log_pump *p = (log_pump *)arg;
    char buf[1024];
    size_t used = 0;

    STORE(p->tid, current_tid());
    for (;;) {
        struct pollfd pfd = { p->fd, POLLIN, 0 };
        int r = poll(&pfd, 1, -1);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        STORE(p->busy, 1);
        ssize_t n = read(p->fd, buf + used, sizeof buf - 1 - used);
        if (n < 0 && errno == EINTR) {
            STORE(p->busy, 0);
            continue;
        }
        if (n <= 0)
            break;
        used += (size_t)n;
        char *start = buf, *nl;
        while ((nl = memchr(start, '\n', used - (size_t)(start - buf))) != NULL) {
            *nl = '\0';
            s_sink(p->is_stderr, start);
            start = nl + 1;
        }
        used -= (size_t)(start - buf);
        memmove(buf, start, used);
        if (used == sizeof buf - 1) {           /* no newline in a full buffer */
            buf[used] = '\0';
            s_sink(p->is_stderr, buf);
            used = 0;
        }
        STORE(p->partial, used != 0);
        STORE(p->busy, 0);
    }
    STORE(p->busy, 0);
    STORE(p->dead, 1);
    return NULL;
}

static int start_pump(int target_fd, log_pump *p, int is_stderr)
{
    int fds[2];
    pthread_t th;
    if (pipe(fds) != 0)
        return -1;
    if (dup2(fds[1], target_fd) < 0) {
        close(fds[0]); close(fds[1]);
        return -1;
    }
    close(fds[1]);
    p->fd = fds[0];
    p->target_fd = target_fd;
    p->is_stderr = is_stderr;
    p->tid = -1;
    if (pthread_create(&th, NULL, pump_thread, p) != 0)
        return -1;
    pthread_detach(th);
    return 0;
}

int xbox_log_pump_start(xbox_log_sink sink)
{
    pthread_mutex_lock(&s_start_lock);
    if (s_start_result == 1) {
        s_sink = sink;
        /* Line-buffered stdout and unbuffered stderr: a line reaches the
         * sink as soon as it is written, not when a 4 KB buffer fills. */
        setvbuf(stdout, NULL, _IOLBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
        s_start_result = -1;
        if (start_pump(STDOUT_FILENO, &s_pumps[0], 0) == 0 &&
            start_pump(STDERR_FILENO, &s_pumps[1], 1) == 0) {
            s_start_result = 0;
            STORE(s_running, 1);
        }
    }
    int rc = s_start_result;
    pthread_mutex_unlock(&s_start_lock);
    return rc;
}

static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Async-signal-safe: see android_log.h. */
int xbox_log_drain(int timeout_ms)
{
    int self, nudged[2] = { 0, 0 }, done = 0;
    int64_t deadline;

    if (!LOAD(s_running))
        return -1;
    if (__atomic_exchange_n(&s_draining, 1, __ATOMIC_SEQ_CST))
        return 0;
    self = current_tid();
    deadline = mono_ms() + (timeout_ms > 0 ? timeout_ms : 0);
    for (;;) {
        done = 1;
        for (int i = 0; i < 2; i++) {
            log_pump *p = &s_pumps[i];
            int pending = 0;
            if (LOAD(p->tid) == self || LOAD(p->dead))
                continue;                   /* cannot wait for ourselves */
            if (ioctl(p->fd, FIONREAD, &pending) != 0)
                pending = 0;
            /* Pipe first, then busy: see pump_thread. */
            if (pending > 0 || LOAD(p->busy)) {
                done = 0;
            } else if (LOAD(p->partial) && !nudged[i]) {
                /* The pipe is empty, so one byte cannot block. */
                ssize_t w = write(p->target_fd, "\n", 1);
                (void)w;
                nudged[i] = 1;
                done = 0;
            }
        }
        if (done || mono_ms() >= deadline)
            break;
        struct timespec nap = { 0, 1000000 };   /* 1 ms */
        nanosleep(&nap, NULL);
    }
    STORE(s_draining, 0);
    return done;
}

#if defined(__ANDROID__)

static void logcat_sink(int is_stderr, const char *line)
{
    __android_log_write(is_stderr ? ANDROID_LOG_WARN : ANDROID_LOG_INFO,
                        XBOX_ANDROID_LOG_TAG, line);
}

int xbox_android_log_init(void)
{
    return xbox_log_pump_start(logcat_sink);
}

#else

int xbox_android_log_init(void) { return 0; }

#endif
