/* Fast-forward for Xbox titles (XBA-240), through the runtime on the real
 * clock: the toggle (recomp_ff_set, what the in-game menu and the hotkey call)
 * picked up by xbox_FastForwardPoll sets the guest clock to RECOMP_FF_SPEED
 * (4 here) and mutes the output; then every guest time source --
 * KeQueryPerformanceCounter, KeQuerySystemTime, KeQueryInterruptTime, the
 * time stamp counter, the guest tick -- runs four times as fast as the host's
 * clock, a guest sleep (KeDelayExecutionThread) takes a quarter of its time,
 * and none of them steps back when fast-forward goes off again, after which
 * they run 1:1. Before any fast-forward each source reads what the host's own
 * call reads (the 1x path is the old one).
 *
 * Rates are checked against xbox_HostNowNs over ~120 ms with a generous
 * tolerance (a loaded CI host oversleeps); POSIX hosts (recomp_states). */
#include "kernel.h"
#include "guest_clock.h"
#include "apu/apu_volume.h"
#include "platform/host_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include "platform/recomp_states.h"
#include "input/keyboard.h"
#include "usb/usb_gamepad.h"
#endif

uint64_t xbox_ReadTimeStampCounter(void);

/* Provided by the generated title; this harness has none. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

typedef struct Sample {
    uint64_t host_ns, qpc, sys100, int100, tsc, tick;
} Sample;

static LONGLONG s_freq;

static void sample(Sample *s)
{
    LARGE_INTEGER q, t;
    s->host_ns = xbox_HostNowNs();
    q = xbox_KeQueryPerformanceCounter();
    s->qpc = (uint64_t)q.QuadPart;
    xbox_KeQuerySystemTime(&t);
    s->sys100 = (uint64_t)t.QuadPart;
    s->int100 = xbox_KeQueryInterruptTime();
    s->tsc = xbox_ReadTimeStampCounter();
    s->tick = xbox_GuestTickCount64();
}

/* Each source's guest seconds over the host's, between a and b. */
static void check_rate(const Sample *a, const Sample *b, double want, const char *what)
{
    double host = (double)(b->host_ns - a->host_ns) / 1e9;
    double r[5];
    const char *names[5] = {"QueryPerformanceCounter", "QuerySystemTime", "QueryInterruptTime", "TSC", "tick"};
    int i;

    r[0] = (double)(b->qpc - a->qpc) / (double)s_freq / host;
    r[1] = (double)(b->sys100 - a->sys100) / 1e7 / host;
    r[2] = (double)(b->int100 - a->int100) / 1e7 / host;
    r[3] = (double)(b->tsc - a->tsc) / 733333333.0 / host;
    r[4] = (double)(b->tick - a->tick) / 1e3 / host;
    for (i = 0; i < 5; i++) {
        /* Within 10 % (ms sources: 2 ms of slop over the span). */
        double slop = i >= 2 && i != 3 ? 0.1 + 0.002 / host : 0.1;
        if (r[i] < want * (1 - slop) || r[i] > want * (1 + slop)) {
            fprintf(stderr, "FAIL %s: %s ran x%.3f, want x%.1f (host %.1f ms)\n", what, names[i], r[i], want,
                    host * 1e3);
            failures++;
        }
    }
}

static void check_forward(const Sample *a, const Sample *b, const char *what)
{
    if (b->qpc < a->qpc || b->sys100 < a->sys100 || b->int100 < a->int100 || b->tsc < a->tsc || b->tick < a->tick) {
        fprintf(stderr, "FAIL %s: a guest clock stepped back\n", what);
        failures++;
    }
}

static void host_sleep_ms(unsigned ms)
{
    xbox_HostSleepNs((uint64_t)ms * 1000000u);
}

int main(void)
{
#ifdef _WIN32
    printf("xbox_fast_forward kernel: skipped (POSIX hosts: recomp_states)\n");
    return 77;
#else
    LARGE_INTEGER f, q0, q1;
    Sample a, b, c, d;

    QueryPerformanceFrequency(&f);
    s_freq = f.QuadPart;
    setenv("RECOMP_FF_SPEED", "4", 1);
    recomp_ff_reset();

    /* Never fast-forwarded: the sources are the host's own calls. */
    xbox_FastForwardPoll();
    CHECK(xbox_GuestClockSpeed() == 1);
    CHECK(!xbox_GuestClockActive());
    CHECK(!xbox_ApuFastForwardMuted());
    QueryPerformanceCounter(&q0);
    q1 = xbox_KeQueryPerformanceCounter();
    CHECK(q1.QuadPart >= q0.QuadPart && q1.QuadPart - q0.QuadPart < s_freq / 100);
    CHECK(xbox_GuestTickCount64() - GetTickCount64() <= 1);
    sample(&a);
    host_sleep_ms(120);
    sample(&b);
    check_rate(&a, &b, 1.0, "1x");

    /* The toggle on: x4, muted. */
    recomp_ff_set(1);
    xbox_FastForwardPoll();
    CHECK(xbox_GuestClockSpeed() == 4);
    CHECK(xbox_GuestClockActive());
    CHECK(xbox_ApuFastForwardMuted());
    sample(&b);
    check_forward(&a, &b, "on");
    host_sleep_ms(120);
    sample(&c);
    check_rate(&b, &c, 4.0, "x4");

    /* A guest sleep of 200 ms takes ~50 ms of the host's. */
    {
        LARGE_INTEGER iv;
        uint64_t t0 = xbox_HostNowNs(), took;
        iv.QuadPart = -200 * 10000LL;
        xbox_KeDelayExecutionThread(0, FALSE, &iv);
        took = xbox_HostNowNs() - t0;
        if (took < 45000000ull || took > 150000000ull) {
            fprintf(stderr, "FAIL KeDelayExecutionThread 200 ms at x4 took %.1f ms\n", took / 1e6);
            failures++;
        }
    }

    /* The hold alone keeps it on; off when neither holds. */
    recomp_ff_set(0);
    recomp_ff_hold(1);
    xbox_FastForwardPoll();
    CHECK(xbox_GuestClockSpeed() == 4);
    recomp_ff_hold(0);
    xbox_FastForwardPoll();
    CHECK(xbox_GuestClockSpeed() == 1);
    CHECK(!xbox_ApuFastForwardMuted());
    sample(&d);
    check_forward(&c, &d, "off");
    /* The gain stays: KeQueryPerformanceCounter is ahead of the host's. */
    QueryPerformanceCounter(&q0);
    q1 = xbox_KeQueryPerformanceCounter();
    CHECK(q1.QuadPart - q0.QuadPart > s_freq / 4);   /* > 250 ms gained over the 120 + ~50 ms at x4 */
    sample(&a);
    host_sleep_ms(120);
    sample(&b);
    check_rate(&a, &b, 1.0, "off again");

    /* "max" is x16. */
    setenv("RECOMP_FF_SPEED", "max", 1);
    recomp_ff_reset();
    recomp_ff_set(1);
    xbox_FastForwardPoll();
    CHECK(xbox_GuestClockSpeed() == XBOX_GUEST_CLOCK_MAX_SPEED);
    recomp_ff_set(0);
    xbox_FastForwardPoll();
    CHECK(xbox_GuestClockSpeed() == 1);

    /* XBA-274: the desktop keys reach an Xbox title as they reach the console runtimes: Tab held (HID 43, the
     * default fast-forward hold) through the pad's report (usb_gamepad_report, port 0) fast-forwards, let go it
     * stops; F1 (save slot 1) is ignored without save states (none attached here). */
    {
        uint8_t rep[20];
        setenv("RECOMP_FF_SPEED", "4", 1);
        recomp_ff_reset();
        xbox_InputHidKeySet(43, 1);
        usb_gamepad_report(0, rep, sizeof rep);
        CHECK(recomp_ff_active());
        CHECK(xbox_GuestClockSpeed() == 4);
        xbox_InputHidKeySet(43, 0);
        usb_gamepad_report(0, rep, sizeof rep);
        CHECK(!recomp_ff_active());
        CHECK(xbox_GuestClockSpeed() == 1);
        xbox_InputHidKeySet(43, 1);
        xbox_InputKeysClear();   /* focus lost: every key lets go */
        usb_gamepad_report(0, rep, sizeof rep);
        CHECK(!recomp_ff_active());
    }

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("xbox_fast_forward kernel: all checks passed\n");
    return 0;
#endif
}
