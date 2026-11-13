/*
 * The Win32 thread shim on a POSIX host -- written to fail.
 *
 * pthreads cannot stop another thread mid-run, so the shim's SuspendThread
 * only counts. A thread that suspends *itself* is at a known point, though,
 * and Win32 parks it there until a ResumeThread. Worker pools idle that way
 * (NtSuspendThread on the current thread, resumed when work is queued); a
 * SuspendThread that returns at once turns the idle loop into a busy spin.
 *
 *   1. SuspendThread(own handle) does not return until ResumeThread
 *   2. the same through the current-thread pseudo-handle
 *   3. ResumeThread reports the count the park left (1)
 *   4. XBA-133: an APC queued from another thread ends an alertable
 *      WaitForSingleObjectEx and an alertable SleepEx of the main thread
 *      (which CreateThread did not start: w32_thread_apc_handle gives it a
 *      thread object) with WAIT_IO_COMPLETION, the APC run on the main
 *      thread; GetCurrentThread still answers the pseudo-handle there.
 */
#include "win32_compat.h"

#include <stdio.h>
#include <time.h>

static int failures;

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-58s %s", what, ok ? "PASS" : "FAIL");
    if (!ok && detail) printf(" -- %s", detail);
    putchar('\n');
    if (!ok) failures++;
}

static volatile LONG g_past_suspend;
static volatile LONG g_use_pseudo;

static DWORD WINAPI parks_itself(LPVOID unused)
{
    (void)unused;
    SuspendThread(g_use_pseudo ? (HANDLE)(LONG_PTR)-2 : GetCurrentThread());
    InterlockedExchange(&g_past_suspend, 1);
    return 0;
}

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void run(int pseudo, const char *what)
{
    DWORD tid, prev, w;
    HANDLE h;

    g_past_suspend = 0;
    g_use_pseudo = pseudo;
    h = CreateThread(NULL, 0, parks_itself, NULL, 0, &tid);
    check(h != NULL, "CreateThread", NULL);
    if (!h)
        return;
    sleep_ms(100);
    check(g_past_suspend == 0, what,
          "SuspendThread returned at once; an idle pool spins");
    prev = ResumeThread(h);
    check(prev == 1, "ResumeThread reports the parked count (1)", NULL);
    w = WaitForSingleObject(h, 5000);
    check(w == WAIT_OBJECT_0 && g_past_suspend == 1,
          "the thread runs on after ResumeThread", NULL);
    CloseHandle(h);
}

static HANDLE g_main_apc;
static volatile DWORD g_apc_tid;

static VOID CALLBACK note_apc(ULONG_PTR data)
{
    (void)data;
    g_apc_tid = GetCurrentThreadId();
}

static DWORD WINAPI queues_apc(LPVOID unused)
{
    (void)unused;
    sleep_ms(100);
    QueueUserAPC(note_apc, g_main_apc, 0);
    return 0;
}

static void apc_wake(int sleep)
{
    HANDLE ev = CreateEventA(NULL, TRUE, FALSE, NULL), th;
    struct timespec t0, t1;
    DWORD r;
    long ms;

    g_apc_tid = 0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    th = CreateThread(NULL, 0, queues_apc, NULL, 0, NULL);
    r = sleep ? SleepEx(3000, TRUE) : WaitForSingleObjectEx(ev, 3000, TRUE);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    CloseHandle(ev);
    check(r == WAIT_IO_COMPLETION && g_apc_tid == GetCurrentThreadId() && ms < 2000,
          sleep ? "an APC from another thread ends an alertable SleepEx"
                : "an APC from another thread ends WaitForSingleObjectEx", NULL);
}

int main(void)
{
    printf("win32 thread shim (POSIX)\n");
    run(0, "SuspendThread(own handle) parks the thread");
    run(1, "SuspendThread(pseudo-handle) parks the thread");
    g_main_apc = w32_thread_apc_handle();
    check(g_main_apc != NULL && GetCurrentThread() == (HANDLE)(LONG_PTR)-2,
          "the main thread gets an APC handle; GetCurrentThread unchanged", NULL);
    apc_wake(0);
    apc_wake(1);
    check(SleepEx(50, TRUE) == 0, "an alertable SleepEx with nothing queued runs out", NULL);
    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
