/* win32_event_release_posix: SetEvent and PulseEvent release the threads that
 * wait at that moment. A reset that comes before a released thread is
 * scheduled must not send it back to sleep: XDK code resets a notification
 * event by hand and waits again (BlockUntilVerticalBlank), and two threads
 * doing so on one core starved each other. */
#define _GNU_SOURCE
#include "win32_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

typedef struct { HANDLE ev; volatile LONG woke, waiting; DWORD ms; } Waiter;

static DWORD WINAPI waiter_main(LPVOID p)
{
    Waiter *w = (Waiter *)p;
    InterlockedExchange(&w->waiting, 1);
    if (WaitForSingleObject(w->ev, w->ms) == WAIT_OBJECT_0)
        InterlockedExchange(&w->woke, 1);
    return 0;
}

static HANDLE start(Waiter *w, HANDLE ev, DWORD ms)
{
    HANDLE t;
    w->ev = ev; w->woke = 0; w->waiting = 0; w->ms = ms;
    t = CreateThread(NULL, 0, waiter_main, w, 0, NULL);
    while (!w->waiting) Sleep(1);
    Sleep(20);                              /* into the wait itself */
    return t;
}

/* 7. the title's pattern: two threads each clear a notification event and
 * wait for the next set (XDK BlockUntilVerticalBlank, inlined), a third
 * sets it on a clock. On Linux the two are pinned to one core, as the
 * guest threads are on arm64 (RECOMP_GUEST_CPUS=one). */
#if defined(__linux__)
#include <sched.h>
static void pin_cpu0(void)
{
    cpu_set_t set;
    CPU_ZERO(&set); CPU_SET(0, &set);
    sched_setaffinity(0, sizeof set, &set);
}
#else
static void pin_cpu0(void) {}
#endif

typedef struct { HANDLE ev; volatile LONG wakes, stop; int work_us; } Blocker;

static DWORD WINAPI blocker_main(LPVOID p)
{
    Blocker *b = (Blocker *)p;
    pin_cpu0();
    while (!b->stop) {
        ResetEvent(b->ev);
        if (WaitForSingleObject(b->ev, 200) == WAIT_OBJECT_0)
            InterlockedIncrement(&b->wakes);
        if (b->work_us) {                    /* a frame's worth of work */
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            do clock_gettime(CLOCK_MONOTONIC, &t1);
            while ((t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_nsec - t0.tv_nsec) / 1000 < b->work_us);
        }
    }
    return 0;
}

static void two_blockers(int sets)
{
    Blocker w = {0}, m = {0};
    HANDLE tw, tm;
    int i;

    w.ev = m.ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    w.work_us = 0;                           /* the stream thread: a short step */
    m.work_us = 2000;                        /* the main thread: a loading-screen frame */
    tw = CreateThread(NULL, 0, blocker_main, &w, 0, NULL);
    tm = CreateThread(NULL, 0, blocker_main, &m, 0, NULL);
    Sleep(50);
    for (i = 0; i < sets; i++) {
        SetEvent(w.ev);
        Sleep(8);
    }
    w.stop = m.stop = 1;
    SetEvent(w.ev);
    WaitForSingleObject(tw, INFINITE); WaitForSingleObject(tm, INFINITE);
    printf("two waiters, %d sets: short-step thread woke %ld times, frame thread %ld times\n",
           sets, (long)w.wakes, (long)m.wakes);
    CHECK(w.wakes >= sets * 9 / 10, "short-step thread saw %ld of %d sets", (long)w.wakes, sets);
    CHECK(m.wakes >= sets * 9 / 10, "frame thread saw %ld of %d sets", (long)m.wakes, sets);
}

int main(int argc, char **argv)
{
    int i, lost = 0, rounds = argc > 1 ? atoi(argv[1]) : 50;
    Waiter a, b;
    HANDLE ta, tb, ev;

    /* 1. manual reset: set, reset at once; both waiters are released */
    ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    for (i = 0; i < rounds; i++) {
        ta = start(&a, ev, 1000);
        tb = start(&b, ev, 1000);
        SetEvent(ev);
        ResetEvent(ev);
        WaitForSingleObject(ta, INFINITE); WaitForSingleObject(tb, INFINITE);
        CloseHandle(ta); CloseHandle(tb);
        if (!a.woke) lost++;
        if (!b.woke) lost++;
    }
    CHECK(lost == 0, "manual reset: %d of %d waiters lost a set that was reset at once", lost, 2 * rounds);
    CHECK(WaitForSingleObject(ev, 0) == WAIT_TIMEOUT, "manual reset: still signalled after the reset");

    /* 2. manual reset pulse: releases the waiters, leaves it unsignalled */
    lost = 0;
    for (i = 0; i < rounds; i++) {
        ta = start(&a, ev, 1000);
        tb = start(&b, ev, 1000);
        PulseEvent(ev);
        WaitForSingleObject(ta, INFINITE); WaitForSingleObject(tb, INFINITE);
        CloseHandle(ta); CloseHandle(tb);
        if (!a.woke) lost++;
        if (!b.woke) lost++;
    }
    CHECK(lost == 0, "manual pulse: %d of %d waiters not released", lost, 2 * rounds);
    CHECK(WaitForSingleObject(ev, 0) == WAIT_TIMEOUT, "manual pulse: left signalled");
    PulseEvent(ev);                         /* nobody waits: nothing is kept */
    CHECK(WaitForSingleObject(ev, 50) == WAIT_TIMEOUT, "manual pulse with no waiter was remembered");

    /* 3. auto reset: one set releases one waiter, reset at once changes nothing */
    ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    lost = 0;
    for (i = 0; i < rounds && i < 20; i++) {  /* each round sits out one waiter's timeout */
        int n;
        ta = start(&a, ev, 250);
        tb = start(&b, ev, 250);
        SetEvent(ev);
        ResetEvent(ev);
        WaitForSingleObject(ta, INFINITE); WaitForSingleObject(tb, INFINITE);
        CloseHandle(ta); CloseHandle(tb);
        n = (a.woke != 0) + (b.woke != 0);
        if (n != 1) lost++;
    }
    CHECK(lost == 0, "auto reset: %d of %d rounds did not release exactly one waiter", lost, i);
    CHECK(WaitForSingleObject(ev, 0) == WAIT_TIMEOUT, "auto reset: signalled after a set a waiter took");

    /* 4. auto reset with nobody waiting: the set is kept for one wait */
    SetEvent(ev);
    CHECK(WaitForSingleObject(ev, 0) == WAIT_OBJECT_0, "auto reset: a set with no waiter was lost");
    CHECK(WaitForSingleObject(ev, 0) == WAIT_TIMEOUT, "auto reset: a set satisfied two waits");
    SetEvent(ev); ResetEvent(ev);
    CHECK(WaitForSingleObject(ev, 0) == WAIT_TIMEOUT, "auto reset: reset did not clear a set nobody took");

    /* 5. auto reset pulse: one waiter, nothing kept */
    ta = start(&a, ev, 1000);
    PulseEvent(ev);
    WaitForSingleObject(ta, INFINITE); CloseHandle(ta);
    CHECK(a.woke, "auto pulse: the waiter was not released");
    CHECK(WaitForSingleObject(ev, 0) == WAIT_TIMEOUT, "auto pulse: left signalled");

    /* 6. a waiter that timed out leaves nothing behind */
    ta = start(&a, ev, 30);
    WaitForSingleObject(ta, INFINITE); CloseHandle(ta);
    CHECK(!a.woke, "timed wait on an unset event succeeded");
    SetEvent(ev);
    CHECK(WaitForSingleObject(ev, 0) == WAIT_OBJECT_0, "a set after a timed-out waiter went to nobody");

    /* 8. zero-timeout polls are not waiters: after many of them a set with
     * nobody blocked is kept for the next wait (a poll counted and never
     * uncounted would take the set as a grant meant for nobody) */
    for (i = 0; i < 1000; i++)
        (void)WaitForSingleObject(ev, 0);
    SetEvent(ev);
    CHECK(WaitForSingleObject(ev, 0) == WAIT_OBJECT_0, "auto reset: a set after zero-timeout polls was lost");

    /* 9. auto reset: a set that comes while a release is still on its way to
     * a blocked thread changes nothing, as a set on a signalled event does.
     * The kernel mirrors a guest KEVENT's SignalState (still 1 until the
     * released thread returns) into the host event before every wait: one
     * KeSetEvent must not become two releases. */
    ta = start(&a, ev, 1000);
    tb = start(&b, ev, 300);
    SetEvent(ev);
    SetEvent(ev);
    WaitForSingleObject(ta, INFINITE); WaitForSingleObject(tb, INFINITE);
    CloseHandle(ta); CloseHandle(tb);
    CHECK((a.woke != 0) + (b.woke != 0) == 1, "auto reset: two sets in a row before the waiter ran released %d",
          (a.woke != 0) + (b.woke != 0));

    two_blockers(200);

    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
