/*
 * Host stack floor of the POSIX CreateThread (XBA-49).
 *
 * Lifted code recurses on the native stack, so a guest thread needs a host
 * stack as deep as the main thread's. Without a floor, CreateThread with
 * stackSize 0 gets the libc default (about 1 MB on bionic) and an Xbox
 * kernel thread's 64 KB becomes the whole host stack. Checks:
 *
 *   1. stackSize 0       the thread recurses through 2+ MB of frames
 *   2. stackSize 64 KB   the same (crashed before the floor)
 *   3. a larger request  is kept, not lowered to the floor
 *
 * On Linux and Android, pthread_getattr_np also confirms each thread's size.
 *
 * Usage: win32_thread_stack_posix_test [floor_mb]   (default 8; pass the
 * value of RECOMP_THREAD_STACK_MB when the test sets it).
 */
#if defined(__linux__)
#define _GNU_SOURCE
#endif
#include "win32_compat.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define FRAME_BYTES 4096u
#define DEPTH       600u              /* 600 x 4 KB = 2.4 MB of frames */
#define MIN_SPAN    (2u << 20)

static int failures = 0;

static void check(int ok, const char *what)
{
    printf("  %-60s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

struct probe {
    size_t    request;     /* CreateThread stackSize */
    uintptr_t top, bottom; /* frame addresses at the first and last level */
    unsigned  sum;         /* what the recursion returned */
    size_t    stack_size;  /* pthread_getattr_np, 0 if unknown */
};

/* Called through a volatile pointer, the frame read back after the call: the
 * compiler can neither inline the recursion, turn it into a loop (it is not a
 * tail call) nor drop the frame (volatile). */
static unsigned recurse(struct probe *p, unsigned depth);
static unsigned (*volatile s_recurse)(struct probe *, unsigned) = recurse;

__attribute__((noinline))
static unsigned recurse(struct probe *p, unsigned depth)
{
    volatile unsigned char frame[FRAME_BYTES];
    unsigned r;

    frame[0] = (unsigned char)depth;
    frame[FRAME_BYTES - 1] = (unsigned char)(depth >> 8);
    if (depth == DEPTH)
        p->top = (uintptr_t)&frame[0];
    if (depth == 0) {
        p->bottom = (uintptr_t)&frame[0];
        r = 0;
    } else {
        r = s_recurse(p, depth - 1);
    }
    return r + frame[0] + frame[FRAME_BYTES - 1];
}

static unsigned expected_sum(void)
{
    unsigned d, s = 0;
    for (d = 0; d <= DEPTH; d++)
        s += (d & 0xffu) + ((d >> 8) & 0xffu);
    return s;
}

static DWORD WINAPI probe_thread(LPVOID arg)
{
    struct probe *p = (struct probe *)arg;
#if defined(__linux__)
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        size_t sz = 0;
        if (pthread_attr_getstacksize(&attr, &sz) == 0)
            p->stack_size = sz;
        pthread_attr_destroy(&attr);
    }
#endif
    p->sum = s_recurse(p, DEPTH);
    return 1;
}

static void run(size_t request, size_t want_at_least, const char *name)
{
    struct probe p = { request, 0, 0, 0, 0 };
    DWORD code = 0;
    HANDLE h;
    char what[128];

    h = CreateThread(NULL, request, probe_thread, &p, 0, NULL);
    snprintf(what, sizeof what, "%s: CreateThread succeeds", name);
    check(h != NULL, what);
    if (!h) return;
    WaitForSingleObject(h, INFINITE);
    GetExitCodeThread(h, &code);
    CloseHandle(h);

    snprintf(what, sizeof what, "%s: thread returned", name);
    check(code == 1, what);
    snprintf(what, sizeof what, "%s: recursion result intact", name);
    check(p.sum == expected_sum(), what);
    {
        uintptr_t span = p.top > p.bottom ? p.top - p.bottom : p.bottom - p.top;
        printf("    frames spanned %lu KB\n", (unsigned long)(span >> 10));
        snprintf(what, sizeof what, "%s: frames span at least 2 MB", name);
        check(span >= MIN_SPAN, what);
    }
#if defined(__linux__)
    /* A guard page or two may be counted out of the reported size. */
    printf("    host stack %lu KB\n", (unsigned long)(p.stack_size >> 10));
    snprintf(what, sizeof what, "%s: host stack at least %lu MB", name,
             (unsigned long)(want_at_least >> 20));
    check(p.stack_size + (64u << 10) >= want_at_least, what);
#else
    (void)want_at_least;
#endif
}

int main(int argc, char **argv)
{
    size_t floor_mb = argc > 1 ? (size_t)strtoul(argv[1], NULL, 10) : 8;
    size_t floor = floor_mb << 20;
    size_t big = floor + (16u << 20);

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("win32_thread_stack_posix: floor %lu MB\n", (unsigned long)floor_mb);

    run(0, floor, "stackSize 0");
    run(64u << 10, floor, "stackSize 64 KB");
    run(big, big, "stackSize floor + 16 MB");

    printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
