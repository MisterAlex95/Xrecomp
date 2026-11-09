/* stdout / stderr -> logcat on Android (android_log.h). */
#include "android_log.h"

#if defined(__ANDROID__)

#include <android/log.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    int fd;          /* read end of the pipe */
    int priority;    /* ANDROID_LOG_INFO for stdout, ANDROID_LOG_WARN for stderr */
} log_pump;

static log_pump s_pumps[2];
static pthread_once_t s_once = PTHREAD_ONCE_INIT;
static int s_result = -1;

/* One line per logcat entry. A line longer than the buffer is split; a
 * partial line is held until its newline arrives (or the buffer fills). */
static void *pump_thread(void *arg)
{
    log_pump *p = (log_pump *)arg;
    char buf[1024];
    size_t used = 0;
    for (;;) {
        ssize_t n = read(p->fd, buf + used, sizeof buf - 1 - used);
        if (n <= 0)
            break;
        used += (size_t)n;
        char *start = buf, *nl;
        while ((nl = memchr(start, '\n', used - (size_t)(start - buf))) != NULL) {
            *nl = '\0';
            __android_log_write(p->priority, XBOX_ANDROID_LOG_TAG, start);
            start = nl + 1;
        }
        used -= (size_t)(start - buf);
        memmove(buf, start, used);
        if (used == sizeof buf - 1) {           /* no newline in a full buffer */
            buf[used] = '\0';
            __android_log_write(p->priority, XBOX_ANDROID_LOG_TAG, buf);
            used = 0;
        }
    }
    return NULL;
}

static int start_pump(int target_fd, log_pump *p, int priority)
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
    p->priority = priority;
    if (pthread_create(&th, NULL, pump_thread, p) != 0)
        return -1;
    pthread_detach(th);
    return 0;
}

static void init_once(void)
{
    /* Line-buffered stdout and unbuffered stderr: a line reaches logcat as
     * soon as it is written, not when a 4 KB buffer fills. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (start_pump(STDOUT_FILENO, &s_pumps[0], ANDROID_LOG_INFO) == 0 &&
        start_pump(STDERR_FILENO, &s_pumps[1], ANDROID_LOG_WARN) == 0)
        s_result = 0;
}

int xbox_android_log_init(void)
{
    pthread_once(&s_once, init_once);
    return s_result;
}

#else

int xbox_android_log_init(void) { return 0; }

#endif
