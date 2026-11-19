/* A console runtime's frame loop and its worker threads as one performance hint session (host_perf_hint.h). */
#if defined(__linux__)
#define _GNU_SOURCE
#endif
#include "host_perf_hint.h"
#include "host_cpus.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#if defined(__linux__)
#include <errno.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
#if defined(__ANDROID__)
#include <dlfcn.h>
#endif

#define MAX_TIDS 16
#define URGENT_DISPLAY_NICE (-8) /* android.os.Process.THREAD_PRIORITY_URGENT_DISPLAY */

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static recomp_perf_hint_api s_api;  /* the entry points in use; get_manager NULL: none */
static int s_api_state;             /* 0: not looked up yet, 1: looked up (s_api says what was found) */
static const recomp_perf_hint_api *s_test_api;
static void *s_manager;
static void *s_session;
static int32_t s_tids[MAX_TIDS];
static int s_ntids;
static int s_dirty;                 /* the threads changed since the session opened */
static int s_begun;                 /* between recomp_perf_hint_begin and recomp_perf_hint_end */
static int32_t s_main_tid;          /* the frame loop's thread */
static int s_main_nice, s_main_niced;
static int64_t s_target;
static int s_logged;

static int32_t self_tid(void)
{
#if defined(__linux__)
    return (int32_t)syscall(SYS_gettid);
#else
    return 0;
#endif
}

/* Android: the thread at a display thread's priority (the frame loop and the threads it waits on). Returns 1 when
 * it was raised, *was receiving the nice value it had. */
static int raise_priority(int32_t tid, int *was)
{
#if defined(__ANDROID__)
    int old = getpriority(PRIO_PROCESS, (id_t)tid);
    if (old > URGENT_DISPLAY_NICE && setpriority(PRIO_PROCESS, (id_t)tid, URGENT_DISPLAY_NICE) == 0) {
        if (was)
            *was = old;
        return 1;
    }
#else
    (void)tid;
    (void)was;
#endif
    return 0;
}

#if defined(__ANDROID__)
/* libandroid's APerformanceHint_* (API 33; setThreads 34), cast to the table's types: same arguments, size_t and
 * pid_t being the table's size_t and int32_t on Android. */
static void lookup_android(recomp_perf_hint_api *a)
{
    void *lib = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib)
        return;
    a->get_manager = (void *(*)(void))dlsym(lib, "APerformanceHint_getManager");
    a->create_session = (void *(*)(void *, const int32_t *, size_t, int64_t))dlsym(lib, "APerformanceHint_createSession");
    a->update_target = (int (*)(void *, int64_t))dlsym(lib, "APerformanceHint_updateTargetWorkDuration");
    a->report_actual = (int (*)(void *, int64_t))dlsym(lib, "APerformanceHint_reportActualWorkDuration");
    a->close_session = (void (*)(void *))dlsym(lib, "APerformanceHint_closeSession");
    a->set_threads = (int (*)(void *, const int32_t *, size_t))dlsym(lib, "APerformanceHint_setThreads");
    if (!a->get_manager || !a->create_session || !a->update_target || !a->report_actual || !a->close_session)
        memset(a, 0, sizeof *a); /* libandroid stays loaded: it always is in an app */
}
#endif

/* Under s_lock: the entry points, looked up once. 1 when there are some. */
static int api_ready(void)
{
    if (!s_api_state) {
        memset(&s_api, 0, sizeof s_api);
        if (s_test_api)
            s_api = *s_test_api;
#if defined(__ANDROID__)
        else
            lookup_android(&s_api);
#endif
        if (s_api.get_manager)
            s_manager = s_api.get_manager();
        if (!s_manager)
            memset(&s_api, 0, sizeof s_api);
        s_api_state = 1;
    }
    return s_api.get_manager != NULL;
}

static void close_locked(void)
{
    if (s_session)
        s_api.close_session(s_session);
    s_session = NULL;
}

void recomp_perf_hint_set_api(const recomp_perf_hint_api *api)
{
    pthread_mutex_lock(&s_lock);
    if (s_api_state)
        close_locked();
    s_test_api = api;
    s_api_state = 0;
    s_manager = NULL;
    s_logged = 0;
    pthread_mutex_unlock(&s_lock);
}

static void add_tid_locked(int32_t tid)
{
    for (int i = 0; i < s_ntids; i++)
        if (s_tids[i] == tid)
            return;
    if (s_ntids == MAX_TIDS)
        return;
    s_tids[s_ntids++] = tid;
    s_dirty = 1;
}

static void remove_tid_locked(int32_t tid)
{
    for (int i = 0; i < s_ntids; i++)
        if (s_tids[i] == tid) {
            s_tids[i] = s_tids[--s_ntids];
            s_dirty = 1;
            return;
        }
}

/* Threads that ended without leaving (a guest thread a save state's load abandoned, a worker that exited past its
 * hook): out of the list, so the session ADPF refuses for a thread that is gone opens without them. Returns how many
 * went. Linux: a tid of this process that no longer exists answers ESRCH. */
static int prune_dead_locked(void)
{
    int gone = 0;
#if defined(__linux__)
    pid_t pid = getpid();
    for (int i = 0; i < s_ntids;)
        if (syscall(SYS_tgkill, pid, (pid_t)s_tids[i], 0) != 0 && errno == ESRCH) {
            if (s_tids[i] == s_main_tid)
                s_main_tid = 0;
            s_tids[i] = s_tids[--s_ntids];
            s_dirty = 1;
            gone++;
        } else {
            i++;
        }
#endif
    return gone;
}

int recomp_perf_hint_begin(int64_t target_ns)
{
    int32_t tid = self_tid();
    int ok;
    if (!tid)
        return 0; /* no thread ids here (macOS) */
    pthread_mutex_lock(&s_lock);
    close_locked();
    ok = api_ready();
    s_main_tid = tid;
    add_tid_locked(tid);
    s_target = target_ns > 0 ? target_ns : 16666667;
    s_begun = 1;
    s_main_niced = raise_priority(tid, &s_main_nice);
#if defined(__ANDROID__)
    if (!ok && !s_logged) {
        printf("perf hint: no ADPF on this device (Android 13 or later), clocks left to the governor\n");
        fflush(stdout);
        s_logged = 1;
    }
#endif
    pthread_mutex_unlock(&s_lock);
    return ok;
}

void recomp_perf_hint_frame(int64_t work_ns, int64_t target_ns)
{
    pthread_mutex_lock(&s_lock);
    if (!s_begun || !api_ready()) {
        pthread_mutex_unlock(&s_lock);
        return;
    }
    if (target_ns > 0 && (target_ns > s_target + s_target / 100 || target_ns < s_target - s_target / 100)) {
        s_target = target_ns;
        if (s_session)
            s_api.update_target(s_session, s_target);
    }
    if (s_session && s_dirty) {
        /* Threads joined or left: told on API 34, the session opened again with them before. */
        if (!s_api.set_threads || s_api.set_threads(s_session, s_tids, (size_t)s_ntids) != 0) {
            if (!s_api.set_threads || !prune_dead_locked()
                    || s_api.set_threads(s_session, s_tids, (size_t)s_ntids) != 0)
                close_locked();
        }
        s_dirty = 0;
    }
    if (!s_session && s_ntids) {
        s_session = s_api.create_session(s_manager, s_tids, (size_t)s_ntids, s_target);
        if (!s_session && prune_dead_locked() && s_ntids) /* a thread that is gone: once more without it */
            s_session = s_api.create_session(s_manager, s_tids, (size_t)s_ntids, s_target);
        s_dirty = 0;
        if (!s_logged) {
            printf("perf hint: ADPF session %s for %d thread(s), target %.2f ms per frame\n",
                   s_session ? "open" : "refused", s_ntids, (double)s_target / 1e6);
            fflush(stdout);
            s_logged = 1;
        }
        if (!s_session) {
            s_begun = 0; /* refused: not asked again every frame */
            pthread_mutex_unlock(&s_lock);
            return;
        }
    }
    if (s_session)
        s_api.report_actual(s_session, work_ns > 0 ? work_ns : 1);
    pthread_mutex_unlock(&s_lock);
}

void recomp_perf_hint_end(void)
{
    pthread_mutex_lock(&s_lock);
    close_locked();
    if (s_main_tid) {
        remove_tid_locked(s_main_tid);
#if defined(__ANDROID__)
        if (s_main_niced)
            setpriority(PRIO_PROCESS, (id_t)s_main_tid, s_main_nice);
#endif
    }
    s_main_tid = 0;
    s_main_niced = 0;
    s_begun = 0;
    s_logged = 0;
    pthread_mutex_unlock(&s_lock);
}

void recomp_perf_hint_thread_join(void)
{
    int32_t tid = self_tid();
    if (!tid)
        return;
    pthread_mutex_lock(&s_lock);
    add_tid_locked(tid);
    pthread_mutex_unlock(&s_lock);
}

void recomp_perf_hint_thread_leave(void)
{
    int32_t tid = self_tid();
    if (!tid)
        return;
    pthread_mutex_lock(&s_lock);
    remove_tid_locked(tid);
    pthread_mutex_unlock(&s_lock);
}

void recomp_perf_hint_thread_take(int32_t *slot)
{
    int32_t tid = self_tid();
    if (!tid || !slot)
        return;
    pthread_mutex_lock(&s_lock);
    if (*slot && *slot != tid && *slot != s_main_tid)
        remove_tid_locked(*slot);
    add_tid_locked(tid);
    *slot = tid;
    pthread_mutex_unlock(&s_lock);
}

int64_t recomp_perf_hint_work_at(recomp_perf_hint_work *w, uint64_t now_ns, uint64_t slept_total_ns,
                                 uint64_t gap_ns)
{
    uint64_t span, slept;
    int first = !w->last_ns;
    if (!first && now_ns >= w->last_ns)
        span = now_ns - w->last_ns;
    else
        span = 0;
    slept = slept_total_ns >= w->last_slept_ns ? slept_total_ns - w->last_slept_ns : 0;
    w->last_ns = now_ns;
    w->last_slept_ns = slept_total_ns;
    if (first || !span || (gap_ns && span > gap_ns))
        return 0;
    return span > slept ? (int64_t)(span - slept) : 1; /* a sleep longer than the frame: ADPF refuses 0 */
}

void recomp_host_worker_hook(int starting, void *user)
{
    (void)user;
    if (starting) {
        recomp_host_cpus_track(0); /* the mask it inherited from the frame loop's thread: the fast cores */
        recomp_perf_hint_thread_join();
        raise_priority(self_tid(), NULL);
    } else {
        recomp_perf_hint_thread_leave();
        recomp_host_cpus_untrack(); /* its tid may go to a thread that is not ours */
    }
}
