/*
 * RtlTryEnterCriticalSection takes the lock RtlEnterCriticalSection and
 * RtlLeaveCriticalSection take -- written to fail.
 *
 * TryEnter used to keep a host-lock table of its own in kernel_bridge.c, so
 * it excluded nobody who used Enter, and the Leave after it released the
 * other lock. Fable: The Lost Chapters' loader thread TryEnters a load
 * request while the main thread Enters it; both allocated the request's
 * buffer, and the main thread parsed the one nothing was read into.
 *
 *   1. held by Enter on one thread, TryEnter on another fails
 *   2. after Leave, that TryEnter succeeds
 *   3. held by TryEnter, Enter on another thread waits until the Leave
 *   4. TryEnter recurses on the owning thread, and two Leaves free it
 */
#include "kernel.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Provided by the generated title; this harness has none. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

static int failures;
static RTL_CRITICAL_SECTION g_cs;
static volatile int g_try_result = -1, g_entered;

static void check(int ok, const char *what)
{
    printf("  %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static void *try_once(void *arg)
{
    (void)arg;
    g_try_result = xbox_RtlTryEnterCriticalSection(&g_cs) ? 1 : 0;
    if (g_try_result)
        xbox_RtlLeaveCriticalSection(&g_cs);
    return NULL;
}

static void *enter_once(void *arg)
{
    (void)arg;
    xbox_RtlEnterCriticalSection(&g_cs);
    g_entered = 1;
    xbox_RtlLeaveCriticalSection(&g_cs);
    return NULL;
}

static RTL_CRITICAL_SECTION *g_target;

static void *try_target(void *arg)
{
    (void)arg;
    g_try_result = xbox_RtlTryEnterCriticalSection(g_target) ? 1 : 0;
    if (g_try_result)
        xbox_RtlLeaveCriticalSection(g_target);
    return NULL;
}

static int run(void *(*fn)(void *))
{
    pthread_t t;
    if (pthread_create(&t, NULL, fn, NULL) != 0)
        return 0;
    pthread_join(t, NULL);
    return 1;
}

int main(void)
{
    pthread_t t;

    memset(&g_cs, 0, sizeof g_cs);
    xbox_RtlInitializeCriticalSection(&g_cs);
    printf("RtlTryEnterCriticalSection vs Enter/Leave\n");

    xbox_RtlEnterCriticalSection(&g_cs);
    run(try_once);
    check(g_try_result == 0, "1. TryEnter from another thread fails while Enter holds it");
    xbox_RtlLeaveCriticalSection(&g_cs);

    run(try_once);
    check(g_try_result == 1, "2. after Leave, TryEnter from another thread succeeds");

    check(xbox_RtlTryEnterCriticalSection(&g_cs), "3a. TryEnter on a free lock succeeds");
    g_entered = 0;
    pthread_create(&t, NULL, enter_once, NULL);
    usleep(100 * 1000);
    check(!g_entered, "3b. Enter from another thread waits while TryEnter holds it");
    xbox_RtlLeaveCriticalSection(&g_cs);
    pthread_join(t, NULL);
    check(g_entered, "3c. and gets it after the Leave");

    xbox_RtlEnterCriticalSection(&g_cs);
    check(xbox_RtlTryEnterCriticalSection(&g_cs), "4a. TryEnter recurses on the owning thread");
    xbox_RtlLeaveCriticalSection(&g_cs);
    run(try_once);
    check(g_try_result == 0, "4b. one Leave of two keeps it held");
    xbox_RtlLeaveCriticalSection(&g_cs);
    run(try_once);
    check(g_try_result == 1, "4c. the second Leave frees it");

    /* KRN-6: a lock initialised again while held starts free, and the
     * shadow table does not run out. */
    printf("re-initialised and many locks\n");
    {
        static RTL_CRITICAL_SECTION reused;
        memset(&reused, 0, sizeof reused);
        xbox_RtlInitializeCriticalSection(&reused);
        xbox_RtlEnterCriticalSection(&reused);           /* held, never left */
        xbox_RtlInitializeCriticalSection(&reused);      /* a new object there */
        g_target = &reused;
        run(try_target);
        check(g_try_result == 1, "5a. initialised again while held: another thread gets it");
        xbox_RtlLeaveCriticalSection(&reused);           /* the old holder's Leave */
        run(try_target);
        check(g_try_result == 1, "5b. the old holder's Leave does not break the new lock");
        check(xbox_RtlTryEnterCriticalSection(&reused), "5c. and this thread can take it");
        g_target = &reused;
        run(try_target);
        check(g_try_result == 0, "5d. which then excludes the other thread");
        xbox_RtlLeaveCriticalSection(&reused);
    }
    {
        enum { N = 6000 };
        static RTL_CRITICAL_SECTION many[N];
        int i;
        for (i = 0; i < N; i++) {
            xbox_RtlInitializeCriticalSection(&many[i]);
            xbox_RtlEnterCriticalSection(&many[i]);
            xbox_RtlLeaveCriticalSection(&many[i]);
        }
        xbox_RtlEnterCriticalSection(&many[N - 1]);
        g_target = &many[N - 1];
        run(try_target);
        check(g_try_result == 0, "6a. after 6000 distinct locks the last one still excludes");
        xbox_RtlLeaveCriticalSection(&many[N - 1]);
        run(try_target);
        check(g_try_result == 1, "6b. and is free after its Leave");
        xbox_RtlEnterCriticalSection(&many[0]);
        g_target = &many[0];
        run(try_target);
        check(g_try_result == 0, "6c. the first one still excludes too");
        xbox_RtlLeaveCriticalSection(&many[0]);
    }

    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
