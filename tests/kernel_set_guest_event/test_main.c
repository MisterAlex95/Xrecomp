/*
 * xbox_KeSetGuestEvent: a host thread (a title profile's device model) sets a
 * KEVENT the title built by writing its header, and a guest thread blocked in
 * KeWaitForSingleObject on it with no timeout returns -- written to fail.
 *
 * Burnout 3's D3D sleeps that way on the fence event at device + 0x1DCC after
 * patching software method 5 into the pushbuffer; on the console the PGRAPH
 * interrupt sets it. The profile's software-method hook calls this.
 *
 * Runs the real memory layout on the small synthetic XBE from
 * tools/conformance, with the waits going through the thunk dispatcher.
 *
 *   default        No switch: returns 0 and leaves SignalState alone.
 *   title-kevents  RECOMP_TITLE_KEVENTS=1:
 *     1. an object whose header is not an event: 0, memory untouched
 *     2. a synchronization event a guest thread waits on for ever: returns 1,
 *        the waiter wakes with STATUS_SUCCESS and the wait consumes SignalState
 *     3. a notification event: SignalState stays 1, a zero-timeout wait succeeds
 */
#include "kernel.h"
#include "xbox_memory_layout.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Provided by the generated title; this harness has none. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);

#define ORD_KE_WAIT     159u          /* KeWaitForSingleObject */
#define STATUS_TIMEOUT  0x00000102u

static uint32_t scratch;              /* guest VA of a 64 KB block */
#define THUNK_VA   (scratch)
#define STACK_MAIN (scratch + 0x1000)
#define STACK_WAIT (scratch + 0x2000)
#define ZERO_VA    (scratch + 0x2800) /* a LARGE_INTEGER timeout of 0 */
#define SYNC_VA    (scratch + 0x3000)
#define NOTIF_VA   (scratch + 0x3100)
#define OTHER_VA   (scratch + 0x3200)

static uint32_t wait_va;
static int failures;

static uint32_t *G(uint32_t va)
{
    return (uint32_t *)((uintptr_t)va + xbox_GetMemoryOffset());
}

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-66s %s", what, ok ? "PASS" : "FAIL");
    if (!ok && detail) printf(" -- %s", detail);
    putchar('\n');
    if (!ok) failures++;
}

static void build_event(uint32_t va, int type, int signalled)
{
    G(va)[0] = (uint32_t)type | (4u << 16);
    G(va)[1] = signalled ? 1u : 0u;
    G(va)[2] = va + 8;
    G(va)[3] = va + 8;
}

/* KeWaitForSingleObject through the dispatcher, on this thread's registers. */
static uint32_t ke_wait(uint32_t stack, uint32_t va, uint32_t timeout_va)
{
    uint32_t *sp = G(stack);
    sp[0] = 0xBEEF0001u;
    sp[1] = va; sp[2] = 0; sp[3] = 0; sp[4] = 0; sp[5] = timeout_va;
    g_esp = stack;
    g_eax = 0xDEADBEEFu;
    recomp_lookup_kernel(wait_va)();
    return g_eax;
}

static volatile int waiter_done;
static volatile uint32_t waiter_status;

static void *waiter(void *arg)
{
    (void)arg;
    waiter_status = ke_wait(STACK_WAIT, SYNC_VA, 0);   /* no timeout */
    waiter_done = 1;
    return NULL;
}

static int wait_done(int ms)
{
    while (!waiter_done && ms-- > 0)
        usleep(1000);
    return waiter_done;
}

static unsigned char *load_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long n;

    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    rewind(f);
    buf = malloc((size_t)n);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "default";
    const char *path = argc > 2 ? argv[2] : "tools/conformance/test.xbe";
    int on = !strcmp(mode, "title-kevents");
    size_t xbe_len = 0;
    unsigned char *xbe;
    char d[160];
    pthread_t t;
    int r;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (!on && strcmp(mode, "default")) {
        printf("unknown mode '%s'\n", mode);
        return 2;
    }
    if (on) setenv("RECOMP_TITLE_KEVENTS", "1", 1);
    else    unsetenv("RECOMP_TITLE_KEVENTS");

    xbe = load_file(path, &xbe_len);
    if (!xbe) {
        printf("cannot read %s -- run: python tools/conformance/mkxbe.py\n", path);
        return 2;
    }
    if (!xbox_MemoryLayoutInit(xbe, xbe_len)) {
        puts("xbox_MemoryLayoutInit failed");
        return 2;
    }
    scratch = xbox_HeapAlloc(0x10000, 4096);
    *G(THUNK_VA) = 0x80000000u | ORD_KE_WAIT;
    xbox_kernel_set_thunk_address(THUNK_VA, 1);
    xbox_kernel_bridge_init();
    wait_va = *G(THUNK_VA);
    if (!recomp_lookup_kernel(wait_va)) {
        puts("KeWaitForSingleObject did not resolve to a bridge entry point");
        return 2;
    }
    G(ZERO_VA)[0] = G(ZERO_VA)[1] = 0;
    printf("mode: %s\n", mode);

    build_event(SYNC_VA, 1, 0);
    if (!on) {
        r = xbox_KeSetGuestEvent(SYNC_VA);
        check(r == 0 && G(SYNC_VA)[1] == 0,
              "default: not a host event -- returns 0, SignalState untouched", NULL);
    } else {
        /* 1. Not an event header (type 5 = semaphore-like, size 4). */
        G(OTHER_VA)[0] = 5u | (4u << 16);
        G(OTHER_VA)[1] = 0;
        r = xbox_KeSetGuestEvent(OTHER_VA);
        check(r == 0 && G(OTHER_VA)[1] == 0, "1. not an event header: returns 0, memory untouched", NULL);

        /* 2. A guest thread blocked for ever, released from this host thread. */
        pthread_create(&t, NULL, waiter, NULL);
        usleep(100 * 1000);
        check(!waiter_done, "2a. the guest wait without a timeout blocks", NULL);
        r = xbox_KeSetGuestEvent(SYNC_VA);
        check(r == 1, "2b. xbox_KeSetGuestEvent sets it (returns 1)", NULL);
        if (!wait_done(5000)) {
            check(0, "2c. the blocked guest thread wakes", "still waiting after 5 s");
            printf("\n%d failure(s)\n", failures);
            return 1;                  /* the waiter is stuck: do not join */
        }
        pthread_join(t, NULL);
        snprintf(d, sizeof d, "status 0x%08X, SignalState %u", waiter_status, G(SYNC_VA)[1]);
        check(waiter_status == 0 && G(SYNC_VA)[1] == 0,
              "2c. the blocked guest thread wakes; the wait consumes SignalState", d);
        r = (int)ke_wait(STACK_MAIN, SYNC_VA, ZERO_VA);
        snprintf(d, sizeof d, "status 0x%08X", (uint32_t)r);
        check((uint32_t)r == STATUS_TIMEOUT, "2d. a second wait times out", d);

        /* 3. Notification: stays set. */
        build_event(NOTIF_VA, 0, 0);
        r = xbox_KeSetGuestEvent(NOTIF_VA);
        check(r == 1 && G(NOTIF_VA)[1] == 1, "3a. notification event: SignalState 1", NULL);
        r = (int)ke_wait(STACK_MAIN, NOTIF_VA, ZERO_VA);
        check(r == 0 && G(NOTIF_VA)[1] == 1, "3b. a zero-timeout wait succeeds and leaves it set", NULL);
    }

    xbox_MemoryLayoutShutdown();
    free(xbe);
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
