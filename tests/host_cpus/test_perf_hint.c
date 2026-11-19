/* host_perf_hint.c: the frame loop's performance hint session, over fake ADPF entry points (Android's are looked up
 * in libandroid.so; on this host there are none and every call is a no-op), and recomp_host_worker_hook: a worker
 * thread that went through it is put back on its cores after a resume (XBA-50) and is forgotten once it ended. */
#if !defined(_WIN32)
#define _GNU_SOURCE /* sched_getaffinity, CPU_* */
#endif
#include "host_cpus.h"
#include "host_perf_hint.h"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

/* ---- Fake ADPF ---------------------------------------------------------------------------------------------------- */
static int f_sessions_made, f_closed, f_set_threads, f_updates, f_reports;
static size_t f_ntids;
static int64_t f_target, f_last_work;
static int f_session_obj, f_manager_obj;

static void *fake_manager(void) { return &f_manager_obj; }
static void *fake_create(void *manager, const int32_t *tids, size_t n, int64_t target_ns)
{
    (void)tids;
    if (manager != &f_manager_obj)
        return NULL;
    f_sessions_made++;
    f_ntids = n;
    f_target = target_ns;
    return &f_session_obj;
}
static int fake_update(void *s, int64_t t) { (void)s; f_updates++; f_target = t; return 0; }
static int fake_report(void *s, int64_t w) { (void)s; f_reports++; f_last_work = w; return 0; }
static void fake_close(void *s) { (void)s; f_closed++; }
static int fake_set_threads(void *s, const int32_t *tids, size_t n) { (void)s; (void)tids; f_set_threads++; f_ntids = n; return 0; }

/* As ADPF does: no session for a thread that is not alive. */
static void *fake_create_live(void *manager, const int32_t *tids, size_t n, int64_t target_ns)
{
    for (size_t i = 0; i < n; i++)
        if (syscall(SYS_tgkill, getpid(), (pid_t)tids[i], 0) != 0)
            return NULL;
    return fake_create(manager, tids, n, target_ns);
}

static void fake_reset(void)
{
    f_sessions_made = f_closed = f_set_threads = f_updates = f_reports = 0;
    f_ntids = 0;
    f_target = f_last_work = 0;
}

/* ---- A worker thread through recomp_host_worker_hook -------------------------------------------------------------- */
static pthread_mutex_t w_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t w_cv = PTHREAD_COND_INITIALIZER;
static int w_stage;      /* 1: started (hooked), 2: told to end, 3: ended (unhooked) */
static pid_t w_tid;
static int w_cpu;

static void stage_set(int s)
{
    pthread_mutex_lock(&w_mu);
    w_stage = s;
    pthread_cond_broadcast(&w_cv);
    pthread_mutex_unlock(&w_mu);
}

static void stage_wait(int s)
{
    pthread_mutex_lock(&w_mu);
    while (w_stage < s)
        pthread_cond_wait(&w_cv, &w_mu);
    pthread_mutex_unlock(&w_mu);
}

static void *worker(void *arg)
{
    (void)arg;
    w_tid = (pid_t)syscall(SYS_gettid);
    recomp_host_worker_hook(1, NULL); /* as n64_rdp.c's band threads do: tracked with the mask it has (one core) */
    stage_set(1);
    stage_wait(2);
    recomp_host_worker_hook(0, NULL);
    stage_set(3);
    return NULL;
}

/* A thread that takes a role (recomp_perf_hint_thread_take) and ends at once, or stays until told to end. */
static pid_t t_tid;

static void *taker(void *slot)
{
    t_tid = (pid_t)syscall(SYS_gettid);
    recomp_perf_hint_thread_take((int32_t *)slot);
    return NULL;
}

static void *taker_stays(void *slot)
{
    t_tid = (pid_t)syscall(SYS_gettid);
    recomp_perf_hint_thread_take((int32_t *)slot);
    stage_set(1);
    stage_wait(2);
    return NULL;
}

int main(void)
{
    /* No ADPF here: a no-op. */
    CHECK(recomp_perf_hint_begin(16683333) == 0, "no hint on this host");
    recomp_perf_hint_frame(5000000, 16683333);
    recomp_perf_hint_end();

    /* Fake ADPF without setThreads (API 33): the session opens at the first frame with the frame loop's thread, the
     * work time reported as given, the target following the period, and opens again when a worker joins. */
    static const recomp_perf_hint_api api33 = {fake_manager, fake_create, fake_update, fake_report, fake_close, NULL};
    fake_reset();
    recomp_perf_hint_set_api(&api33);
    CHECK(recomp_perf_hint_begin(16683333) == 1, "hint available");
    CHECK(f_sessions_made == 0, "no session before the first frame");
    recomp_perf_hint_frame(4000000, 16683333);
    CHECK(f_sessions_made == 1 && f_ntids == 1 && f_target == 16683333, "session for 1 thread at 16.68 ms (%d, %zu)",
          f_sessions_made, f_ntids);
    CHECK(f_reports == 1 && f_last_work == 4000000, "4 ms reported");
    recomp_perf_hint_frame(4100000, 16683400); /* a VI period wobble: not a new target */
    CHECK(f_updates == 0, "target kept within 1%%");
    recomp_perf_hint_frame(4100000, 20000000); /* PAL */
    CHECK(f_updates == 1 && f_target == 20000000, "target 20 ms");
    recomp_perf_hint_frame(0, 20000000);
    CHECK(f_last_work == 1, "a zero work time is reported as 1 ns (ADPF refuses 0)");

    /* A worker joins (and is tracked by host_cpus). */
    cpu_set_t all, one;
    int n = 0;
    sched_getaffinity(0, sizeof all, &all);
    w_cpu = -1;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++)
        if (CPU_ISSET(cpu, &all)) {
            if (w_cpu < 0)
                w_cpu = cpu;
            n++;
        }
    CPU_ZERO(&one);
    CPU_SET(w_cpu, &one);
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setaffinity_np(&at, sizeof one, &one); /* the "fast cores" it inherits */
    pthread_t th;
    CHECK(pthread_create(&th, &at, worker, NULL) == 0, "worker started");
    stage_wait(1);
    recomp_perf_hint_frame(3000000, 20000000);
    CHECK(f_closed == 1 && f_sessions_made == 2 && f_ntids == 2, "API 33: session opened again with 2 threads");

    if (n >= 2) {
        /* The cpuset move Android makes on going to the background: the worker's mask widened, put back. */
        CHECK(recomp_host_cpus_restore() == 0, "nothing drifted yet");
        sched_setaffinity(w_tid, sizeof all, &all);
        CHECK(recomp_host_cpus_restore() == 1, "the worker put back");
        cpu_set_t now;
        sched_getaffinity(w_tid, sizeof now, &now);
        CHECK(CPU_COUNT(&now) == 1 && CPU_ISSET(w_cpu, &now), "worker back on cpu %d", w_cpu);
    } else {
        puts("one core only: the worker restore check is skipped");
    }

    /* It ends: forgotten by host_cpus (its tid may be reused) and out of the session at the next frame. */
    stage_set(2);
    stage_wait(3);
    if (n >= 2) {
        sched_setaffinity(w_tid, sizeof all, &all); /* still alive until joined below: must not be moved */
        CHECK(recomp_host_cpus_restore() == 0, "an ended worker is left alone");
    }
    pthread_join(th, NULL);
    recomp_perf_hint_frame(3000000, 20000000);
    CHECK(f_sessions_made == 3 && f_ntids == 1, "back to 1 thread");
    recomp_perf_hint_end();
    CHECK(f_closed == 3, "closed at the end");
    recomp_perf_hint_frame(3000000, 20000000);
    CHECK(f_sessions_made == 3 && f_reports == 6, "nothing after the end (%d sessions, %d reports)", f_sessions_made,
          f_reports);

    /* API 34: setThreads instead of a new session. */
    static const recomp_perf_hint_api api34 = {fake_manager, fake_create, fake_update, fake_report, fake_close,
                                               fake_set_threads};
    fake_reset();
    recomp_perf_hint_set_api(&api34);
    recomp_perf_hint_begin(16683333);
    recomp_perf_hint_frame(2000000, 16683333);
    recomp_perf_hint_thread_join(); /* this thread is the frame loop's already: no change */
    recomp_perf_hint_frame(2000000, 16683333);
    CHECK(f_sessions_made == 1 && f_set_threads == 0, "the same thread twice: nothing to tell");
    w_stage = 0;
    pthread_create(&th, NULL, worker, NULL);
    stage_wait(1);
    recomp_perf_hint_frame(2000000, 16683333);
    CHECK(f_sessions_made == 1 && f_set_threads == 1 && f_ntids == 2, "API 34: setThreads with 2 threads");
    stage_set(2);
    pthread_join(th, NULL);
    recomp_perf_hint_end();
    recomp_perf_hint_set_api(NULL);

    /* XBA-305, the Xbox's session: a role one thread holds at a time (the guest main thread, started again on a save
     * state's load) and a joined thread that ends without leaving. ADPF refuses a session for a thread that is gone
     * (fake_create_live below); the session opens without it rather than giving up for the run. */
    static const recomp_perf_hint_api api_live = {fake_manager, fake_create_live, fake_update, fake_report,
                                                  fake_close, NULL};
    fake_reset();
    recomp_perf_hint_set_api(&api_live);
    int32_t main_slot = 0;
    w_stage = 0;
    pthread_create(&th, NULL, taker, &main_slot); /* the first guest main thread: takes the role and ends */
    pthread_join(th, NULL);
    CHECK(main_slot == t_tid && main_slot != 0, "the role names the thread that took it");
    /* pthread_join returns once the kernel cleared the thread's tid word, a moment before the task is gone. */
    for (int i = 0; i < 1000 && syscall(SYS_tgkill, getpid(), t_tid, 0) == 0; i++)
        usleep(1000);
    recomp_perf_hint_begin(33366667);
    recomp_perf_hint_frame(5000000, 33366667);
    CHECK(f_sessions_made == 1 && f_ntids == 1, "the ended thread pruned, the session opened (%d, %zu)",
          f_sessions_made, f_ntids);
    CHECK(f_reports == 1 && f_target == 33366667, "reported against 33.4 ms");
    pthread_create(&th, NULL, taker_stays, &main_slot); /* the next one: in the old one's place */
    stage_wait(1);
    recomp_perf_hint_frame(5000000, 33366667);
    CHECK(f_sessions_made == 2 && f_ntids == 2, "the new holder joined (%d, %zu)", f_sessions_made, f_ntids);
    int32_t first_holder = main_slot;
    stage_set(2);
    pthread_join(th, NULL);
    CHECK(first_holder == t_tid, "the second holder named");
    recomp_perf_hint_thread_take(&main_slot); /* this thread takes it: the frame loop's, already in */
    recomp_perf_hint_frame(5000000, 33366667);
    CHECK(f_ntids == 1, "the holder that left is out, the frame loop stays (%zu)", f_ntids);
    recomp_perf_hint_end();
    recomp_perf_hint_set_api(NULL);

    /* The work of a frame from flip to flip, less the sleep at the cap. */
    recomp_perf_hint_work work = {0};
    CHECK(recomp_perf_hint_work_at(&work, 1000000000ull, 0, 250000000ull) == 0, "first flip: nothing yet");
    CHECK(recomp_perf_hint_work_at(&work, 1016666667ull, 6000000ull, 250000000ull) == 10666667,
          "16.7 ms flip to flip, 6 ms asleep: 10.7 ms of work");
    CHECK(recomp_perf_hint_work_at(&work, 1033333334ull, 6000000ull, 250000000ull) == 16666667,
          "no sleep: the whole period");
    CHECK(recomp_perf_hint_work_at(&work, 1533333334ull, 6000000ull, 250000000ull) == 0, "a half second gap: none");
    CHECK(recomp_perf_hint_work_at(&work, 1550000000ull, 30000000ull, 250000000ull) == 1,
          "slept longer than the frame (clock skew): 1 ns, never 0");

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    puts("PASS: host perf hint");
    return 0;
}
