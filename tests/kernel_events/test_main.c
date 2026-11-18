/*
 * Kernel events: KEVENTs a title builds by writing the header itself, with no
 * KeInitializeEvent call, checked through the thunk dispatcher like a title's
 * kernel calls.
 *
 * Runs the real memory layout on the small synthetic XBE from
 * tools/conformance, so no title and no game files are needed.
 *
 * One process per mode, because the switch is read once:
 *
 *   off            RECOMP_TITLE_KEVENTS=0 (title-built events are guest events
 *                  by default since K-5). Pins the opted-out behaviour: the guest
 *                  address goes to Win32 as a HANDLE, so a set is lost and a
 *                  wait fails instead of reporting the event's state.
 *   title-kevents  RECOMP_TITLE_KEVENTS=1. Each check fails without it:
 *
 *   1. A synchronization event that is not set times out.
 *   2. KeSetEvent sets it: the next wait succeeds, and SignalState in guest
 *      memory is cleared again (a synchronization wait consumes it).
 *   3. SignalState written to 1 by the title counts: XDK code sets and clears
 *      events by writing the header, not only through the kernel.
 *   4. A notification event stays set across two waits; KeResetEvent clears it.
 *
 *   timers         KeSetTimer / KeSetTimerEx, read back through the timer
 *                  thread's firing pass on a clock the test gives. Timers are
 *                  armed an hour ahead, so the real thread never fires them.
 */
#include "kernel.h"
#include "xbox_memory_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

/* Provided by the generated title; this harness has none. */
typedef void (*recomp_func_t)(void);
/* A guest routine for xbox_DpcQueueGuestCall (XBA-94): records its one
 * stack argument, read as lifted code reads it ([esp+4]). */
static uint32_t g_cb_arg, g_cb_calls;
static void fake_callback(void)
{
    g_cb_arg = *(uint32_t *)((uintptr_t)g_esp + 4 + xbox_GetMemoryOffset());
    g_cb_calls++;
}
/* XB-004: every call, in order, for the race below. */
#define GUEST_CALL_LOG 8
static uint32_t g_call_log[GUEST_CALL_LOG], g_call_logged;
static void logging_callback(void)
{
    if (g_call_logged < GUEST_CALL_LOG)
        g_call_log[g_call_logged] = *(uint32_t *)((uintptr_t)g_esp + 4 + xbox_GetMemoryOffset());
    g_call_logged++;
}
/* XB-004: how many times each context of the two-thread run was seen. */
#define STRESS_CALLS 20000u
static unsigned char g_stress_seen[STRESS_CALLS];
static void stress_callback(void)
{
    uint32_t ctx = *(uint32_t *)((uintptr_t)g_esp + 4 + xbox_GetMemoryOffset());
    if (ctx < STRESS_CALLS && g_stress_seen[ctx] < 255)
        g_stress_seen[ctx]++;
}
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va)
{
    if (xbox_va == 0x00123460u) return logging_callback;
    if (xbox_va == 0x00123470u) return stress_callback;
    return xbox_va == 0x00123450u ? fake_callback : NULL;
}
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);

enum { S_SET, S_WAIT, S_RESET, S_MULTI, S_INIT, S_SET_TIMER, S_SET_TIMER_EX,
       S_DOWNCASE, S_UPCASE, N_SLOTS };
static const uint32_t ORD[N_SLOTS] = {
    145,   /* KeSetEvent */
    159,   /* KeWaitForSingleObject */
    138,   /* KeResetEvent */
    158,   /* KeWaitForMultipleObjects */
    108,   /* KeInitializeEvent */
    149,   /* KeSetTimer */
    150,   /* KeSetTimerEx */
    276,   /* RtlDowncaseUnicodeString */
    314,   /* RtlUpcaseUnicodeString */
};

#define STATUS_TIMEOUT 0x00000102u

static uint32_t scratch;                 /* guest VA of a 64 KB block */
#define THUNK_VA (scratch)
#define STACK_VA (scratch + 0x1000)
#define ZERO_VA  (scratch + 0x2000)      /* a LARGE_INTEGER timeout of 0 */
#define SYNC_VA  (scratch + 0x3000)
#define NOTIF_VA (scratch + 0x3100)
#define SECOND_VA (scratch + 0x3200)
#define EXPLICIT_VA (scratch + 0x3300)
#define ARRAY_VA (scratch + 0x3400)
#define FRESH_A (scratch + 0x3500)
#define FRESH_B (scratch + 0x3600)
#define EXPLICIT_B (scratch + 0x3700)
#define TIMER_VA (scratch + 0x3B00)

#define ONE_HOUR_100NS 36000000000LL

static uint32_t slot_va[N_SLOTS];
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

static uint32_t call(int slot, int nargs, const uint32_t *args)
{
    uint32_t *sp = G(STACK_VA);
    int i;

    sp[0] = 0xBEEF0001u;
    for (i = 0; i < nargs; i++)
        sp[1 + i] = args[i];
    g_esp = STACK_VA;
    g_eax = 0xDEADBEEFu;
    recomp_lookup_kernel(slot_va[slot])();
    check(g_esp == STACK_VA + 4u + 4u * nargs, "stdcall stack cleanup", NULL);
    return g_eax;
}

/* Header of a KEVENT as XDK code writes it: Type, Absolute, Size (dwords),
 * Inserted, then SignalState and an empty wait list. */
static void build_event(uint32_t va, int type, int signalled)
{
    G(va)[0] = (uint32_t)type | (4u << 16);
    G(va)[1] = signalled ? 1u : 0u;
    G(va)[2] = va + 8;
    G(va)[3] = va + 8;
}

static uint32_t ke_wait(uint32_t va)
{
    uint32_t args[5] = { va, 0, 0, 0, ZERO_VA };
    return call(S_WAIT, 5, args);
}

static uint32_t ke_set(uint32_t va)
{
    uint32_t args[3] = { va, 0, 0 };
    return call(S_SET, 3, args);
}

static uint32_t ke_reset(uint32_t va)
{
    uint32_t args[1] = { va };
    return call(S_RESET, 1, args);
}

static uint32_t ke_multi(uint32_t a, uint32_t b, int wait_all)
{
    uint32_t args[8] = {2, ARRAY_VA, wait_all ? 0u : 1u, 0, 0, 0, ZERO_VA, 0};
    G(ARRAY_VA)[0] = a;
    G(ARRAY_VA)[1] = b;
    return call(S_MULTI, 8, args);
}

/* XB-004: the window between a DPC leaving the queue and its routine being
 * read. The hook runs in it, on this thread, and queues a second guest call,
 * as the thread executing the pushbuffer can at that moment. */
extern void (*xbox_DpcPoppedHookForTest)(void);

static void queue_second_call(void)
{
    xbox_DpcPoppedHookForTest = NULL;   /* once */
    check(xbox_DpcQueueGuestCall(0x00123460u, 0xB0B0B0B0u) == 1,
          "a guest call queued between the pop and the run", NULL);
}

static void guest_call_race(void)
{
    char d[120];
    int ran = 0;

    g_call_logged = 0;
    g_esp = STACK_VA + 0x800;
    check(xbox_DpcQueueGuestCall(0x00123460u, 0xA0A0A0A0u) == 1, "a first guest call is queued", NULL);
    xbox_DpcPoppedHookForTest = queue_second_call;
    xbox_DpcDrainTimer(&ran);
    xbox_DpcPoppedHookForTest = NULL;
    snprintf(d, sizeof d, "%u calls: 0x%08X, 0x%08X", g_call_logged, g_call_log[0], g_call_log[1]);
    check(g_call_logged == 2 && g_call_log[0] == 0xA0A0A0A0u && g_call_log[1] == 0xB0B0B0B0u,
          "each queued guest call runs once, with its own context", d);
}

static volatile LONG g_stress_done;

static DWORD WINAPI stress_producer(LPVOID unused)
{
    uint32_t i;

    (void)unused;
    for (i = 0; i < STRESS_CALLS; i++)
        while (!xbox_DpcQueueGuestCall(0x00123470u, i))
            Sleep(0);                   /* the pool is busy: the drain catches up */
    InterlockedExchange(&g_stress_done, 1);
    return 0;
}

/* One thread queues distinct contexts while this one drains. */
static void guest_call_stress(void)
{
    HANDLE t;
    char d[120];
    uint32_t i, lost = 0, repeated = 0;
    int ran;

    g_esp = STACK_VA + 0x800;
    t = CreateThread(NULL, 0, stress_producer, NULL, 0, NULL);
    while (!InterlockedCompareExchange(&g_stress_done, 0, 0) || xbox_DpcPending())
        xbox_DpcDrainTimer(&ran);
    WaitForSingleObject(t, 30000);
    CloseHandle(t);
    for (i = 0; i < STRESS_CALLS; i++) {
        lost += g_stress_seen[i] == 0;
        repeated += g_stress_seen[i] > 1;
    }
    snprintf(d, sizeof d, "%u lost, %u repeated of %u", lost, repeated, STRESS_CALLS);
    check(lost == 0 && repeated == 0, "two threads: every guest call runs exactly once", d);
}

static void ke_init(uint32_t va)
{
    uint32_t args[3] = {va, 0, 1}; /* notification, initially signalled */
    call(S_INIT, 3, args);
}

/* XB-005: RtlDowncaseUnicodeString / RtlUpcaseUnicodeString. The console's
 * functions write no terminator, so one goes in only where the caller's buffer
 * has room for it. */
#define RTL_VA      (scratch + 0x4000)
#define RTL_SRC     (RTL_VA + 0x00)      /* UNICODE_STRING */
#define RTL_DST     (RTL_VA + 0x10)      /* UNICODE_STRING */
#define RTL_SRC_BUF (RTL_VA + 0x100)
#define RTL_DST_BUF (RTL_VA + 0x200)
#define RTL_CANARY  0xCAFEu

static uint16_t *G16(uint32_t va)
{
    return (uint16_t *)((uintptr_t)va + xbox_GetMemoryOffset());
}

static void rtl_string(uint32_t va, uint16_t len, uint16_t max, uint32_t buf)
{
    *G16(va) = len;
    *G16(va + 2) = max;
    G(va)[1] = buf;
}

/* "aBcD" at va, four characters, the canary just after them. */
static void rtl_text(uint32_t va)
{
    static const uint16_t text[4] = { 'a', 'B', 'c', 'D' };
    memcpy(G16(va), text, sizeof text);
    *G16(va + 8) = RTL_CANARY;
}

static uint32_t rtl_call(int slot, uint32_t dst, uint32_t src, uint32_t alloc)
{
    uint32_t args[3] = { dst, src, alloc };
    return call(slot, 3, args);
}

static int rtl_is(uint32_t va, int upper)
{
    const uint16_t *t = G16(va);
    return upper ? t[0] == 'A' && t[1] == 'B' && t[2] == 'C' && t[3] == 'D'
                 : t[0] == 'a' && t[1] == 'b' && t[2] == 'c' && t[3] == 'd';
}

static void rtl_strings(int slot, int upper)
{
    char d[120];
    uint32_t st, buf;

    /* A destination of exactly Length bytes. */
    rtl_text(RTL_SRC_BUF);
    rtl_text(RTL_DST_BUF);
    rtl_string(RTL_SRC, 8, 8, RTL_SRC_BUF);
    rtl_string(RTL_DST, 0, 8, RTL_DST_BUF);
    st = rtl_call(slot, RTL_DST, RTL_SRC, 0);
    snprintf(d, sizeof d, "status 0x%08X, word after the buffer 0x%04X", st, *G16(RTL_DST_BUF + 8));
    check(st == 0 && rtl_is(RTL_DST_BUF, upper) && *G16(RTL_DST_BUF + 8) == RTL_CANARY,
          "a buffer of exactly Length: converted, the next word untouched", d);

    /* In place: Dest and Src are the same string. */
    rtl_text(RTL_SRC_BUF);
    rtl_string(RTL_SRC, 8, 8, RTL_SRC_BUF);
    st = rtl_call(slot, RTL_SRC, RTL_SRC, 0);
    snprintf(d, sizeof d, "status 0x%08X, word after the buffer 0x%04X", st, *G16(RTL_SRC_BUF + 8));
    check(st == 0 && rtl_is(RTL_SRC_BUF, upper) && *G16(RTL_SRC_BUF + 8) == RTL_CANARY,
          "in place: converted, the next word untouched", d);

    /* Room for the terminator: it is still written. */
    rtl_text(RTL_SRC_BUF);
    rtl_text(RTL_DST_BUF);
    rtl_string(RTL_SRC, 8, 8, RTL_SRC_BUF);
    rtl_string(RTL_DST, 0, 10, RTL_DST_BUF);
    st = rtl_call(slot, RTL_DST, RTL_SRC, 0);
    check(st == 0 && rtl_is(RTL_DST_BUF, upper) && *G16(RTL_DST_BUF + 8) == 0,
          "a buffer with room: terminated", NULL);

    /* AllocateDestinationString: the new buffer is terminated. */
    rtl_text(RTL_SRC_BUF);
    rtl_string(RTL_SRC, 8, 8, RTL_SRC_BUF);
    rtl_string(RTL_DST, 0, 0, 0);
    st = rtl_call(slot, RTL_DST, RTL_SRC, 1);
    buf = G(RTL_DST)[1];
    snprintf(d, sizeof d, "status 0x%08X, buffer 0x%08X", st, buf);
    check(st == 0 && buf && *G16(RTL_DST) == 8 && *G16(RTL_DST + 2) == 10
          && rtl_is(buf, upper) && *G16(buf + 8) == 0,
          "allocated: converted and terminated", d);

#ifndef _WIN32
    /* Length 0 and no buffer: nothing to write, and guest 0 is not memory. */
    {
        pid_t pid = fork();
        int status = -1;

        if (pid == 0) {
            /* If this regresses it faults, with the whole guest address space
             * mapped: no core file of several GB on the test host. */
            struct rlimit no_core = { 0, 0 };
            setrlimit(RLIMIT_CORE, &no_core);
            alarm(10);
            rtl_string(RTL_SRC, 0, 0, 0);
            rtl_string(RTL_DST, 0, 0, 0);
            _exit(rtl_call(slot, RTL_DST, RTL_SRC, 0) == 0 ? 0 : 1);
        }
        waitpid(pid, &status, 0);
        snprintf(d, sizeof d, "child status 0x%X", (unsigned)status);
        check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "an empty string with no buffer writes nothing", d);
    }
#endif
}

/* Arm TIMER_VA; a negative due time is relative, in 100 ns units. */
static void ke_set_timer_ex(long long due_100ns, uint32_t period_ms)
{
    uint32_t args[5] = { TIMER_VA, (uint32_t)due_100ns,
                         (uint32_t)((uint64_t)due_100ns >> 32), period_ms, 0 };
    call(S_SET_TIMER_EX, 5, args);
}

/* When the armed timer is due, on the kernel's timer clock. */
static long long timer_due(void)
{
    long long next = -1;
    xbox_TimerFireForTest(0, &next);
    return next;
}

/* XB-001: a periodic timer keeps its own schedule, however late the pass that
 * fires it, and a stall restarts it instead of firing a burst. */
static void timers_periodic(void)
{
    long long due, next = -1, t;
    char d[120];
    int fired, n = 0;

    ke_set_timer_ex(-ONE_HOUR_100NS, 16);
    due = timer_due();
    fired = xbox_TimerFireForTest(due + 3, &next);
    snprintf(d, sizeof d, "fired %d, next due +%lld", fired, next - due);
    check(fired == 1 && next == due + 16, "fired 3 ms late: the next is one period after the due time", d);
    fired = xbox_TimerFireForTest(due + 19, &next);
    snprintf(d, sizeof d, "fired %d, next due +%lld", fired, next - due);
    check(fired == 1 && next == due + 32, "late again: still on the 16 ms schedule", d);
    fired = xbox_TimerFireForTest(due + 500, &next);
    snprintf(d, sizeof d, "fired %d, next due +%lld", fired, next - due);
    check(fired == 1 && next > due + 500 && next <= due + 516,
          "a stall fires once and restarts one period on", d);
    check(xbox_TimerFireForTest(due + 500, &next) == 0, "no burst after the stall", NULL);

    /* The legacy loop's 10 ms tick, for 1.6 s: 16 ms is 62.5 Hz, not 50. */
    ke_set_timer_ex(-ONE_HOUR_100NS, 16);
    due = timer_due();
    for (t = 0; t < 1600; t += 10)
        n += xbox_TimerFireForTest(due + t, &next);
    snprintf(d, sizeof d, "%d fires", n);
    check(n == 100, "a 16 ms timer on the 10 ms tick fires 100 times in 1.6 s", d);
}

static void timers_seam(void)
{
    long long due, next = -1;

    ke_set_timer_ex(-ONE_HOUR_100NS, 0);
    due = timer_due();
    check(due > 0, "an armed timer is reported by the firing pass", NULL);
    check(xbox_TimerFireForTest(due - 1, &next) == 0 && next == due,
          "it does not fire before its due time", NULL);
}

/* XB-002: a positive due time is an absolute system time, and a relative one
 * is never cut short by its fraction of a millisecond. */
static void timers_due_time(void)
{
    LARGE_INTEGER now;
    long long rel, abs_due, past, next = -1, gap;
    char d[120];
    int i, later = 0;
    enum { ROUNDS = 20 };

    ke_set_timer_ex(-ONE_HOUR_100NS, 0);
    rel = timer_due();
    xbox_KeQuerySystemTime(&now);
    ke_set_timer_ex(now.QuadPart + ONE_HOUR_100NS, 0);
    abs_due = timer_due();
    gap = abs_due > rel ? abs_due - rel : rel - abs_due;
    snprintf(d, sizeof d, "%lld ms from the relative timer", gap);
    check(gap <= 2000, "an absolute due time an hour ahead is due in an hour", d);
    check(xbox_TimerFireForTest(abs_due - 1, &next) == 0, "not due 1 ms before it", NULL);
    check(xbox_TimerFireForTest(abs_due, &next) == 1 && next == -1,
          "due at it, once (a one-shot)", NULL);

    /* Long past: at once. The real timer thread may take it first. */
    ke_set_timer_ex(1, 0);
    past = timer_due();
    check(past == -1 || past < rel - 3000000, "an absolute time in the past is due now", NULL);

    /* Same millisecond or not, the longer delay must end later. */
    for (i = 0; i < ROUNDS; i++) {
        long long whole, frac;
        ke_set_timer_ex(-ONE_HOUR_100NS, 0);
        whole = timer_due();
        ke_set_timer_ex(-(ONE_HOUR_100NS + 5000), 0);
        frac = timer_due();
        later += frac > whole;
    }
    snprintf(d, sizeof d, "%d of %d rounds", later, ROUNDS);
    check(later == ROUNDS, "half a millisecond more is rounded up, not dropped", d);
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
    const char *mode = argc > 1 ? argv[1] : "off";
    const char *path = argc > 2 ? argv[2] : "tools/conformance/test.xbe";
    int on = !strcmp(mode, "title-kevents");
    int timers = !strcmp(mode, "timers");
    size_t xbe_len = 0;
    unsigned char *xbe;
    char d[160];
    uint32_t st;
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (!on && !timers && strcmp(mode, "off")) {
        printf("unknown mode '%s'\n", mode);
        return 2;
    }

#if defined(_WIN32)
    _putenv_s("RECOMP_TITLE_KEVENTS", on ? "1" : "0");
#else
    setenv("RECOMP_TITLE_KEVENTS", on ? "1" : "0", 1);
#endif

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
    for (i = 0; i < N_SLOTS; i++)
        *G(THUNK_VA + 4 * i) = 0x80000000u | ORD[i];
    xbox_kernel_set_thunk_address(THUNK_VA, N_SLOTS);
    xbox_kernel_bridge_init();
    for (i = 0; i < N_SLOTS; i++) {
        slot_va[i] = *G(THUNK_VA + 4 * i);
        if (!recomp_lookup_kernel(slot_va[i])) {
            printf("ordinal %u did not resolve to a bridge entry point\n", ORD[i]);
            return 2;
        }
    }
    G(ZERO_VA)[0] = G(ZERO_VA)[1] = 0;
    printf("mode: %s\n", mode);

    if (timers) {
        timers_periodic();
        timers_seam();
        timers_due_time();
        /* The timer thread is still running: no layout shutdown under it. */
        printf("\n%d failure(s)\n", failures);
        return failures ? 1 : 0;
    }

    rtl_strings(S_DOWNCASE, 0);
    rtl_strings(S_UPCASE, 1);

    ke_init(EXPLICIT_VA);
    ke_init(EXPLICIT_B);
    check(ke_multi(EXPLICIT_VA, EXPLICIT_B, 1) == 0,
          "explicit events resolve through shadows in either mode", NULL);

    build_event(FRESH_A, 1, 0);
    build_event(FRESH_B, 1, 1);
    st = ke_multi(FRESH_A, FRESH_B, 0);
    if (on)
        check(st == 1 && G(FRESH_A)[1] == 0 && G(FRESH_B)[1] == 0,
              "first access via WaitAny resolves nonzero index and consumes it", NULL);
    else
        check(st != 0 && st != 1 && st != STATUS_TIMEOUT && G(FRESH_B)[1] == 1,
              "default multiple wait rejects header events without consumption", NULL);

    build_event(SYNC_VA, 1, 0);
    build_event(NOTIF_VA, 0, 0);

    if (!on) {
        ke_set(SYNC_VA);
        st = ke_wait(SYNC_VA);
        snprintf(d, sizeof d, "status 0x%08X", st);
        check(st != 0 && st != STATUS_TIMEOUT,
              "default: a title-built event is not a host event (the wait fails)", d);
    } else {
        /* 1. Not set: times out. */
        st = ke_wait(SYNC_VA);
        snprintf(d, sizeof d, "status 0x%08X", st);
        check(st == STATUS_TIMEOUT, "an unset synchronization event times out", d);

        /* 2. KeSetEvent, then a wait consumes it. */
        ke_set(SYNC_VA);
        st = ke_wait(SYNC_VA);
        snprintf(d, sizeof d, "status 0x%08X, SignalState %u", st, G(SYNC_VA)[1]);
        check(st == 0 && G(SYNC_VA)[1] == 0,
              "KeSetEvent sets it; the wait succeeds and clears SignalState", d);
        st = ke_wait(SYNC_VA);
        snprintf(d, sizeof d, "status 0x%08X", st);
        check(st == STATUS_TIMEOUT, "a second wait times out again", d);

        /* 3. The title sets SignalState itself. */
        G(SYNC_VA)[1] = 1;
        st = ke_wait(SYNC_VA);
        snprintf(d, sizeof d, "status 0x%08X", st);
        check(st == 0, "SignalState written by the title counts", d);

        /* 4. Notification: stays set until reset. */
        ke_set(NOTIF_VA);
        st = ke_wait(NOTIF_VA);
        i = ke_wait(NOTIF_VA) == 0;
        snprintf(d, sizeof d, "status 0x%08X, second %s", st, i ? "ok" : "failed");
        check(st == 0 && i, "a notification event stays set across two waits", d);
        ke_reset(NOTIF_VA);
        st = ke_wait(NOTIF_VA);
        snprintf(d, sizeof d, "status 0x%08X, SignalState %u", st, G(NOTIF_VA)[1]);
        check(st == STATUS_TIMEOUT && G(NOTIF_VA)[1] == 0,
              "KeResetEvent clears it", d);
    }

    if (on) {
        build_event(SECOND_VA, 1, 1);
        G(SYNC_VA)[1] = 1;
        st = ke_multi(SYNC_VA, SECOND_VA, 0);
        check(st == 0 && G(SYNC_VA)[1] == 0 && G(SECOND_VA)[1] == 1,
              "WaitAny consumes only its selected synchronization event", NULL);
        G(SECOND_VA)[1] = 0;
        check(ke_multi(SYNC_VA, SECOND_VA, 0) == STATUS_TIMEOUT,
              "header reset before multiple wait clears stale host signal", NULL);
        G(SYNC_VA)[1] = 1;
        st = ke_multi(SYNC_VA, SECOND_VA, 1);
        check(st == STATUS_TIMEOUT && G(SYNC_VA)[1] == 1,
              "WaitAll timeout preserves signalled guest state", NULL);
        G(SECOND_VA)[1] = 1;
        st = ke_multi(SYNC_VA, SECOND_VA, 1);
        check(st == 0 && G(SYNC_VA)[1] == 0 && G(SECOND_VA)[1] == 0,
              "WaitAll success consumes both synchronization events", NULL);
        G(NOTIF_VA)[1] = G(SYNC_VA)[1] = 1;
        st = ke_multi(NOTIF_VA, SYNC_VA, 1);
        check(st == 0 && G(NOTIF_VA)[1] == 1 && G(SYNC_VA)[1] == 0,
              "mixed notification and synchronization WaitAll", NULL);
        st = ke_multi(NOTIF_VA, SYNC_VA, 0);
        check(st == 0 && ke_multi(NOTIF_VA, SYNC_VA, 0) == 0 && G(NOTIF_VA)[1] == 1,
              "notification persists across multiple waits", NULL);
        G(SYNC_VA)[1] = 1;
        st = ke_multi(SYNC_VA, 0, 1);
        check(st != 0 && st != STATUS_TIMEOUT && G(SYNC_VA)[1] == 1,
              "failed multiple wait does not consume header state", NULL);
        st = ke_multi(SYNC_VA, EXPLICIT_VA, 1);
        check(st == 0 && G(SYNC_VA)[1] == 0,
              "mixed header and explicitly initialized event resolves", NULL);

        /* KRN-10: KeInitializeEvent writes the console's header and keeps
         * SignalState as the truth, as for an event XDK code builds. */
        {
            uint32_t va = scratch + 0x3A00, args[3] = { va, 1, 0 };  /* sync, unset */
            call(S_INIT, 3, args);
            snprintf(d, sizeof d, "header 0x%08X list 0x%08X/0x%08X", G(va)[0], G(va)[2], G(va)[3]);
            check(G(va)[0] == (1u | (4u << 16)) && G(va)[2] == va + 8 && G(va)[3] == va + 8,
                  "KeInitializeEvent: Type 1, Size 4, Inserted 0, self-linked list", d);
            ke_set(va);
            check(G(va)[1] == 1, "KeSetEvent on it writes SignalState 1 (KeReadStateEvent)", NULL);
            G(va)[1] = 0;                                        /* XDK clears it inline */
            st = ke_wait(va);
            snprintf(d, sizeof d, "status 0x%08X", st);
            check(st == STATUS_TIMEOUT, "SignalState cleared by the title: the wait times out", d);
            G(va)[1] = 1;
            st = ke_wait(va);
            check(st == 0 && G(va)[1] == 0, "set by the title: the wait succeeds and consumes it", NULL);
        }
    }

    if (!on) {
        /* XBA-93: a thread failing to wait on a title-built event thousands of
         * times a second is a spin. With nothing ever trying to set the event
         * it is only reported (promoted, the wait would block for ever); once
         * a KeSetEvent has been seen on it, the next spin window promotes it,
         * and from then on it behaves as with RECOMP_TITLE_KEVENTS. */
        uint32_t spin_va = scratch + 0x3800, n, fails = 0;
        build_event(spin_va, 1, 0);
        for (n = 0; n < 2500; n++)
            fails += ke_wait(spin_va) != 0 && g_eax != STATUS_TIMEOUT;
        snprintf(d, sizeof d, "%u immediate failures of 2500", fails);
        check(fails == 2500, "no setter seen: reported, not promoted (the waits still fail)", d);
        ke_set(spin_va);                       /* fails (no host event) but counts as a setter */
        for (n = 0, fails = 0; n < 2500; n++)
            fails += ke_wait(spin_va) != 0 && g_eax != STATUS_TIMEOUT;
        snprintf(d, sizeof d, "%u immediate failures of 2500", fails);
        check(fails < 2500, "a setter seen: the next spin window promotes it", d);
        st = ke_wait(spin_va);
        snprintf(d, sizeof d, "status 0x%08X", st);
        check(st == STATUS_TIMEOUT, "promoted: an unset event now times out instead of failing", d);
        ke_set(spin_va);
        st = ke_wait(spin_va);
        snprintf(d, sizeof d, "status 0x%08X, SignalState %u", st, G(spin_va)[1]);
        check(st == 0 && G(spin_va)[1] == 0, "promoted: KeSetEvent wakes the wait", d);
        /* Another title-built event is left alone: the promotion is per object. */
        build_event(scratch + 0x3900, 1, 0);
        ke_set(scratch + 0x3900);
        st = ke_wait(scratch + 0x3900);
        check(st != 0 && st != STATUS_TIMEOUT, "the promotion is for that object only", NULL);
    }

    {
        /* XBA-94: routine(context) on the DPC path, one argument, registers back after. */
        uint32_t esp_before;
        int ran = 0;
        g_esp = STACK_VA + 0x800;
        esp_before = g_esp;
        check(xbox_DpcQueueGuestCall(0x00123450u, 0xC0FFEEu) == 1, "a guest call is queued", NULL);
        check(xbox_DpcQueueGuestCall(0x00999990u, 1) == 0, "a routine that is not code is refused", NULL);
        xbox_DpcDrainTimer(&ran);
        snprintf(d, sizeof d, "calls %u, arg 0x%08X, esp %s", g_cb_calls, g_cb_arg,
                 g_esp == esp_before ? "kept" : "moved");
        check(g_cb_calls == 1 && g_cb_arg == 0xC0FFEEu && g_esp == esp_before,
              "the DPC path calls routine(context) once, esp kept", d);
    }

    guest_call_race();
    guest_call_stress();

    xbox_MemoryLayoutShutdown();
    free(xbe);
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
