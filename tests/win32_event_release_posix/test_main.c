/* win32_event_release_posix: SetEvent and PulseEvent release the threads that
 * wait at that moment. A reset that comes before a released thread is
 * scheduled must not send it back to sleep: XDK code resets a notification
 * event by hand and waits again (BlockUntilVerticalBlank), and two threads
 * doing so on one core starved each other.
 *
 * Cases 10-16: WaitForMultipleObjects blocks with the same rules (L-4); it
 * used to poll every millisecond, so a pulse released nobody and wait-all
 * took the objects that were ready one at a time. */
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

/* Start a waiter and return once it is blocked in the wait itself (counted
 * among the event's waiters), or already done. It used to sleep 20 ms for
 * that, which under qemu-user was sometimes not enough: in 9 the second set
 * then found nobody waiting, was rightly kept, and released the late waiter
 * too (3 runs in 10 on the arm64 job). */
static HANDLE start(Waiter *w, HANDLE ev, DWORD ms)
{
    HANDLE t;
    unsigned before = w32_event_waiters(ev);
    int k;
    w->ev = ev; w->woke = 0; w->waiting = 0; w->ms = ms;
    t = CreateThread(NULL, 0, waiter_main, w, 0, NULL);
    while (!w->waiting) Sleep(1);
    for (k = 0; k < 10000 && w32_event_waiters(ev) <= before
                && WaitForSingleObject(t, 0) == WAIT_TIMEOUT; k++)
        Sleep(1);
    return t;
}

/* Multiple-object waits (L-4): a thread in WaitForMultipleObjects. */
typedef struct { HANDLE h[4]; DWORD n, ms; BOOL all; volatile LONG ret, waiting; } MWaiter;

static DWORD WINAPI mwaiter_main(LPVOID p)
{
    MWaiter *w = (MWaiter *)p;
    InterlockedExchange(&w->waiting, 1);
    InterlockedExchange(&w->ret, (LONG)WaitForMultipleObjects(w->n, w->h, w->all, w->ms));
    return 0;
}

static HANDLE mstart(MWaiter *w, BOOL all, DWORD ms, DWORD n, HANDLE a, HANDLE b)
{
    HANDLE t;
    w->h[0] = a; w->h[1] = b; w->n = n; w->all = all; w->ms = ms;
    w->ret = -1; w->waiting = 0;
    t = CreateThread(NULL, 0, mwaiter_main, w, 0, NULL);
    while (!w->waiting) Sleep(1);
    Sleep(20);                              /* into the wait itself */
    return t;
}

static void join(HANDLE t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }

static void multi_waits(int rounds)
{
    MWaiter a, b;
    HANDLE ta, tb, m1, m2, a1, a2, sem;
    int i, bad;
    struct timespec t0, t1;
    long ms;

    m1 = CreateEventA(NULL, TRUE, FALSE, NULL);
    m2 = CreateEventA(NULL, TRUE, FALSE, NULL);
    a1 = CreateEventA(NULL, FALSE, FALSE, NULL);
    a2 = CreateEventA(NULL, FALSE, FALSE, NULL);

    /* 10. wait-any released by a pulse of its second object, manual and auto */
    for (i = bad = 0; i < rounds; i++) {
        ta = mstart(&a, FALSE, 1000, 2, m1, m2);
        PulseEvent(m2);
        join(ta);
        if (a.ret != (LONG)(WAIT_OBJECT_0 + 1)) bad++;
    }
    CHECK(bad == 0, "wait-any: %d of %d manual pulses did not release it", bad, rounds);
    CHECK(WaitForSingleObject(m2, 0) == WAIT_TIMEOUT, "wait-any: manual pulse left signalled");
    for (i = bad = 0; i < rounds; i++) {
        ta = mstart(&a, FALSE, 1000, 2, a1, a2);
        PulseEvent(a2);
        join(ta);
        if (a.ret != (LONG)(WAIT_OBJECT_0 + 1)) bad++;
    }
    CHECK(bad == 0, "wait-any: %d of %d auto pulses did not release it", bad, rounds);
    CHECK(WaitForSingleObject(a2, 0) == WAIT_TIMEOUT, "wait-any: auto pulse left signalled");

    /* 11. wait-any: a manual set reset at once still releases both waiters */
    for (i = bad = 0; i < rounds; i++) {
        ta = mstart(&a, FALSE, 1000, 2, m1, m2);
        tb = mstart(&b, FALSE, 1000, 2, m2, m1);
        SetEvent(m1);
        ResetEvent(m1);
        join(ta); join(tb);
        if (a.ret != (LONG)WAIT_OBJECT_0 || b.ret != (LONG)(WAIT_OBJECT_0 + 1)) bad++;
    }
    CHECK(bad == 0, "wait-any: %d of %d rounds lost a set reset at once", bad, rounds);

    /* 12. auto reset: one set releases one of two wait-any waiters, and is consumed */
    for (i = bad = 0; i < rounds && i < 10; i++) {   /* each round sits out one timeout */
        ta = mstart(&a, FALSE, 200, 2, a1, m1);
        tb = mstart(&b, FALSE, 200, 2, m1, a1);
        SetEvent(a1);
        join(ta); join(tb);
        if ((a.ret == (LONG)WAIT_OBJECT_0) + (b.ret == (LONG)(WAIT_OBJECT_0 + 1)) != 1) bad++;
        if (WaitForSingleObject(a1, 0) != WAIT_TIMEOUT) bad++;
    }
    CHECK(bad == 0, "wait-any, auto reset: %d of %d rounds did not release exactly one waiter", bad, i);

    /* 13. wait-any: only the object it returns is consumed; a grant it did
     * not take stays a set */
    ta = mstart(&a, FALSE, 1000, 2, a1, a2);
    SetEvent(a2);
    SetEvent(a1);
    join(ta);
    CHECK(a.ret == (LONG)WAIT_OBJECT_0 || a.ret == (LONG)(WAIT_OBJECT_0 + 1), "wait-any: returned %ld", (long)a.ret);
    bad = (WaitForSingleObject(a1, 0) == WAIT_OBJECT_0) + (WaitForSingleObject(a2, 0) == WAIT_OBJECT_0);
    CHECK(bad == 1, "wait-any on two set auto events: %d left signalled, want 1", bad);

    /* 14. wait-all is atomic: one of two auto events set takes nothing, a
     * single waiter can still have it; with both set it takes both */
    ta = mstart(&a, TRUE, 1000, 2, a1, a2);
    SetEvent(a1);
    Sleep(30);
    CHECK(a.ret == -1, "wait-all returned with one of two objects set");
    CHECK(WaitForSingleObject(a1, 0) == WAIT_OBJECT_0, "wait-all consumed a1 without a2");
    SetEvent(a2);
    Sleep(30);
    CHECK(a.ret == -1, "wait-all returned with a1 taken by another waiter");
    SetEvent(a1);
    join(ta);
    CHECK(a.ret == (LONG)WAIT_OBJECT_0, "wait-all: returned %ld once both were set", (long)a.ret);
    CHECK(WaitForSingleObject(a1, 0) == WAIT_TIMEOUT && WaitForSingleObject(a2, 0) == WAIT_TIMEOUT,
          "wait-all: did not consume both auto events");

    /* 15. wait-any on a semaphore and an event: a release wakes it, takes one count */
    sem = CreateSemaphoreA(NULL, 0, 4, NULL);
    ta = mstart(&a, FALSE, 1000, 2, m1, sem);
    ReleaseSemaphore(sem, 2, NULL);
    join(ta);
    CHECK(a.ret == (LONG)(WAIT_OBJECT_0 + 1), "wait-any: semaphore release gave %ld", (long)a.ret);
    CHECK(WaitForSingleObject(sem, 0) == WAIT_OBJECT_0 && WaitForSingleObject(sem, 0) == WAIT_TIMEOUT,
          "wait-any: took other than one semaphore count");

    /* 16. timeout: no set, WAIT_TIMEOUT after the time; nothing left behind */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    CHECK(WaitForMultipleObjects(2, (HANDLE[]){ a1, a2 }, FALSE, 60) == WAIT_TIMEOUT, "wait-any: no timeout");
    CHECK(WaitForMultipleObjects(2, (HANDLE[]){ a1, m1 }, TRUE, 60) == WAIT_TIMEOUT, "wait-all: no timeout");
    clock_gettime(CLOCK_MONOTONIC, &t1);
    ms = (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;
    CHECK(ms >= 115 && ms < 1000, "two 60 ms timeouts took %ld ms", ms);
    SetEvent(a1);
    CHECK(WaitForSingleObject(a1, 0) == WAIT_OBJECT_0, "a set after timed-out multiple waits went to nobody");
    CHECK(WaitForMultipleObjects(2, (HANDLE[]){ a1, a2 }, FALSE, 0) == WAIT_TIMEOUT, "zero-timeout poll");
    SetEvent(a2);
    CHECK(WaitForMultipleObjects(2, (HANDLE[]){ a1, a2 }, FALSE, 0) == WAIT_OBJECT_0 + 1, "poll finds a2");

    CloseHandle(m1); CloseHandle(m2); CloseHandle(a1); CloseHandle(a2); CloseHandle(sem);
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
    /* The two sets go in under one lock hold (w32_event_set_n): with two
     * SetEvent calls, waiter a could run and take the first release in
     * between, and the second then rightly released b too, as Win32 does
     * (3 runs in 10 under qemu-user). The property is the shim's, so it is
     * tested without the scheduler in it. */
    ta = start(&a, ev, 1000);
    tb = start(&b, ev, 300);
    w32_event_set_n(ev, 2);
    WaitForSingleObject(ta, INFINITE); WaitForSingleObject(tb, INFINITE);
    CloseHandle(ta); CloseHandle(tb);
    CHECK((a.woke != 0) + (b.woke != 0) == 1, "auto reset: two sets in a row before the waiter ran released %d",
          (a.woke != 0) + (b.woke != 0));

    two_blockers(200);
    multi_waits(rounds < 20 ? rounds : 20);

    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
