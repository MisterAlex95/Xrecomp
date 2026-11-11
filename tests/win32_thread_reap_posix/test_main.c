/*
 * Ended CreateThread threads give their stack back (XBA-56).
 *
 * POSIX keeps the stack of a joinable thread that ended until someone joins
 * it, and nothing joins a guest thread. With the 8 MB host stack floor
 * (XBA-49) every ended thread kept 8 MB of address space. The shim now
 * detaches its threads; the handle keeps the exit code and the signalled
 * state. Checks:
 *
 *   1. 2000 threads (stackSize 0, every fourth ends with ExitThread) are
 *      started, waited for and closed: VmSize afterwards is within 64 MB of
 *      before, plus 80 KB a thread for qemu-user's own per-thread leak
 *      (Linux and Android, from /proc/self/status). "Before" is taken after
 *      a warm-up of the same loop, so libc's one-time growth is not counted.
 *      Kept stacks would be 16 GB.
 *   2. 64 more threads end while their handles stay open: the stacks are
 *      still given back, and each handle still reads signalled with its exit
 *      code, before and after CloseHandle of the other handles.
 *   3. SetThreadAffinityMask and (not on Android) TerminateThread on an ended
 *      thread's handle return without touching the gone pthread.
 */
#if defined(__linux__)
#define _GNU_SOURCE
#endif
#include "win32_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOOP_THREADS 2000u
#define KEPT_THREADS 64u
#define SLACK_KB     (64u * 1024u)
/* Per ended thread, on top: qemu-user keeps about 40 KB of its own for every
 * guest thread that ever ran (measured with bare detached pthreads, no shim:
 * 338 -> 462 MB over 2000 threads). A kept stack is 8 MB, 100 times this. */
#define PER_THREAD_KB 80u
#define EXIT_BASE    0x1000u
#define STILL_ACTIVE_CODE 259u

static int failures = 0;

static void check(int ok, const char *what)
{
    printf("  %-64s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

/* VmSize in KB, 0 when the host has no /proc. */
static unsigned long vm_size_kb(void)
{
    unsigned long kb = 0;
#if defined(__linux__)
    char line[256];
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f))
        if (strncmp(line, "VmSize:", 7) == 0) {
            kb = strtoul(line + 7, NULL, 10);
            break;
        }
    fclose(f);
#endif
    return kb;
}

static DWORD WINAPI worker(LPVOID arg)
{
    DWORD code = (DWORD)(uintptr_t)arg;
    volatile char touch[16 * 1024];          /* use some of the stack */
    touch[0] = (char)code;
    touch[sizeof touch - 1] = (char)(code >> 8);
    if (code % 4u == 0)
        ExitThread(code);
    return code + touch[0] - (char)code;
}

/* Detached threads unmap their stack a moment after they signal. */
static void settle(void) { Sleep(100); }

int main(void)
{
    unsigned i, bad;
    unsigned long before, after;
    HANDLE kept[KEPT_THREADS];

    printf("win32_thread_reap_posix (XBA-56)\n");

    /* Warm up: the first threads set the stack floor and libc's one-time
     * pieces (glibc's thread malloc arena, the unwinder pthread_exit loads
     * for ExitThread), which stay whatever the shim does. Codes 0..7: two of
     * them end with ExitThread. */
    {
        HANDLE h[8];
        bad = 0;
        for (i = 0; i < 8; i++)
            if (!(h[i] = CreateThread(NULL, 0, worker, (LPVOID)(uintptr_t)i, 0, NULL)))
                bad++;
        for (i = 0; i < 8; i++)
            if (h[i]) { WaitForSingleObject(h[i], INFINITE); CloseHandle(h[i]); }
        /* Then the measured loop's own pattern, 200 times, so its first-run
         * costs (libc, qemu-user's translation cache) are paid before. */
        for (i = 0; i < 200; i++) {
            HANDLE w = CreateThread(NULL, 0, worker, (LPVOID)(uintptr_t)i, 0, NULL);
            if (!w) { bad++; break; }
            WaitForSingleObject(w, INFINITE);
            CloseHandle(w);
        }
        check(bad == 0, "warm-up threads start");
        settle();
    }

    before = vm_size_kb();
    bad = 0;
    for (i = 0; i < LOOP_THREADS; i++) {
        DWORD code = EXIT_BASE + i, got = 0;
        HANDLE h = CreateThread(NULL, 0, worker, (LPVOID)(uintptr_t)code, 0, NULL);
        if (!h) { bad++; break; }
        if (WaitForSingleObject(h, INFINITE) != WAIT_OBJECT_0) bad++;
        if (!GetExitCodeThread(h, &got) || got != code) bad++;
        CloseHandle(h);
    }
    check(bad == 0, "2000 threads: started, signalled, exit code kept");
    settle();
    after = vm_size_kb();
    if (before) {
        printf("    VmSize %lu KB -> %lu KB\n", before, after);
        check(after <= before + SLACK_KB + LOOP_THREADS * PER_THREAD_KB,
              "2000 ended threads: VmSize within 64 MB (+80 KB a thread)");
    } else {
        printf("    no /proc/self/status: VmSize not checked\n");
    }

    /* Threads that end while their handle stays open. */
    before = vm_size_kb();
    bad = 0;
    for (i = 0; i < KEPT_THREADS; i++) {
        kept[i] = CreateThread(NULL, 0, worker,
                               (LPVOID)(uintptr_t)(EXIT_BASE + i), 0, NULL);
        if (!kept[i] || WaitForSingleObject(kept[i], INFINITE) != WAIT_OBJECT_0)
            bad++;
    }
    check(bad == 0, "64 threads end, handles kept open");
    settle();
    after = vm_size_kb();
    if (before) {
        printf("    VmSize %lu KB -> %lu KB (64 open handles)\n", before, after);
        check(after <= before + 32u * 1024u + KEPT_THREADS * PER_THREAD_KB,
              "ended threads with open handles: stacks given back");
    }

    /* Read the even handles, close the odd ones, read the even ones again. */
    for (int pass = 0; pass < 2; pass++) {
        bad = 0;
        for (i = 0; i < KEPT_THREADS; i += 2) {
            DWORD got = STILL_ACTIVE_CODE;
            if (WaitForSingleObject(kept[i], 0) != WAIT_OBJECT_0) bad++;
            if (!GetExitCodeThread(kept[i], &got) || got != EXIT_BASE + i) bad++;
        }
        if (pass == 0) {
            check(bad == 0, "ended thread handles: signalled, exit code");
            for (i = 1; i < KEPT_THREADS; i += 2)
                CloseHandle(kept[i]);
        } else {
            check(bad == 0, "the same after CloseHandle of the others");
        }
    }

    /* Calls that used the thread's pthread_t. */
    {
        DWORD got = 0;
        SetThreadAffinityMask(kept[0], 1);
        check(1, "SetThreadAffinityMask on an ended thread returns");
#if !defined(__ANDROID__)
        check(TerminateThread(kept[2], 0xDEAD) != 0,
              "TerminateThread on an ended thread succeeds");
        check(GetExitCodeThread(kept[2], &got) && got == EXIT_BASE + 2,
              "  and keeps the exit code");
#else
        (void)got;
#endif
    }

#if defined(__linux__)
    /* The calling thread's affinity still works (the self path). */
    {
        DWORD_PTR proc = 0, sys = 0;
        GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
        check(SetThreadAffinityMask(GetCurrentThread(), proc) != 0,
              "SetThreadAffinityMask on the current thread");
    }
#endif

    for (i = 0; i < KEPT_THREADS; i += 2)
        CloseHandle(kept[i]);

    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
