/* win32_wait_clock_posix (XBA-46): the shim's timed waits count on the
 * monotonic clock and keep their length; an absolute waitable-timer due time
 * (a wall-clock FILETIME) is converted once and fires on time.
 *
 * What this cannot do in CI: step the wall clock in the middle of a wait (the
 * clock is not per container, and the CI host is shared). The change that
 * makes such a step harmless is that every deadline and condition variable
 * uses CLOCK_MONOTONIC; the checks below make sure the waits still last what
 * they should after that change. */
#define _GNU_SOURCE
#include "win32_compat.h"

#include <stdio.h>
#include <time.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Generous upper slack: a loaded CI host oversleeps, it never undersleeps. */
#define NEAR(got, want) ((got) >= (want) - 5 && (got) <= (want) + 150)

int main(void)
{
    long long t;
    DWORD r;

    /* 1. a timed wait on an object that never signals */
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    t = now_ms();
    r = WaitForSingleObject(ev, 300);
    t = now_ms() - t;
    CHECK(r == WAIT_TIMEOUT && NEAR(t, 300), "event wait: r=%lu after %lld ms (want timeout after 300)", (unsigned long)r, t);

    /* 2. a condition variable with its critical section */
    CRITICAL_SECTION cs;
    CONDITION_VARIABLE cv;
    InitializeCriticalSection(&cs);
    InitializeConditionVariable(&cv);
    EnterCriticalSection(&cs);
    t = now_ms();
    BOOL ok = SleepConditionVariableCS(&cv, &cs, 300);
    t = now_ms() - t;
    LeaveCriticalSection(&cs);
    CHECK(!ok && NEAR(t, 300), "condition variable: ok=%d after %lld ms (want FALSE after 300)", ok, t);

    /* 3. WaitForMultipleObjects' own deadline */
    HANDLE two[2] = { ev, CreateEventA(NULL, FALSE, FALSE, NULL) };
    t = now_ms();
    r = WaitForMultipleObjects(2, two, FALSE, 300);
    t = now_ms() - t;
    CHECK(r == WAIT_TIMEOUT && NEAR(t, 300), "multiple wait: r=%lu after %lld ms", (unsigned long)r, t);

    /* 4. an absolute waitable timer 200 ms from the wall clock's now */
    HANDLE wt = CreateWaitableTimerW(NULL, FALSE, NULL);
    FILETIME ft;
    LARGE_INTEGER due;
    GetSystemTimeAsFileTime(&ft);
    due.QuadPart = (((LONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) + 200LL * 10000;
    t = now_ms();
    SetWaitableTimer(wt, &due, 0, NULL, NULL, FALSE);
    r = WaitForSingleObject(wt, 2000);
    t = now_ms() - t;
    CHECK(r == WAIT_OBJECT_0 && NEAR(t, 200), "absolute timer: r=%lu after %lld ms (want fired after 200)", (unsigned long)r, t);

    /* 5. an absolute due time already past fires at once */
    due.QuadPart = (((LONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - 3600LL * 10000000;
    SetWaitableTimer(wt, &due, 0, NULL, NULL, FALSE);
    r = WaitForSingleObject(wt, 0);
    CHECK(r == WAIT_OBJECT_0, "past absolute timer: r=%lu (want signalled at once)", (unsigned long)r);

    /* 6. a relative one still works */
    due.QuadPart = -100LL * 10000;     /* 100 ms */
    t = now_ms();
    SetWaitableTimer(wt, &due, 0, NULL, NULL, FALSE);
    r = WaitForSingleObject(wt, 2000);
    t = now_ms() - t;
    CHECK(r == WAIT_OBJECT_0 && NEAR(t, 100), "relative timer: r=%lu after %lld ms", (unsigned long)r, t);

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    puts("PASS: timed waits on the monotonic clock");
    return 0;
}
