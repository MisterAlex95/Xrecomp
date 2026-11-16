/*
 * guest_clock.h - the guest's sense of time, scaled for fast-forward (XBA-240).
 *
 * The console runtimes fast-forward by running N console frames per shown
 * frame (recomp_states.h). An Xbox title has no frame loop of ours to repeat:
 * it paces itself on the vblank count, KeQueryPerformanceCounter, the time
 * stamp counter, KeQuerySystemTime / KeQueryInterruptTime / KeTickCount, its
 * kernel timers and the timeouts of its waits. So fast-forward is one speed
 * factor on all of them: every guest time source adds the time this clock has
 * gained on the host's, and every guest span the host sleeps or waits for is
 * divided by the speed. The vblank schedule runs on the same clock, so a title
 * that counts vblanks speeds up too.
 *
 * The guest clock is the host's monotonic clock plus an "extra" that grows by
 * (speed - 1) ns per host ns while the speed is above 1 and stays put when it
 * goes back to 1: continuous and monotonic across every change, never back in
 * time. Until the speed first leaves 1, the extra is 0 and no clock is read
 * for it, so a run that never fast-forwards sees exactly the time it saw
 * before.
 *
 * The pure part (XboxGuestClock and the helpers that take one) has no state
 * and no clock: the unit tests drive it with their own times. The process-wide
 * part reads xbox_HostNowNs and may be called from any thread: the timer
 * thread sets the speed (xbox_FastForwardPoll, kernel_hal.c), every guest
 * thread reads it.
 */
#ifndef XBOX_GUEST_CLOCK_H
#define XBOX_GUEST_CLOCK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The fastest the guest clock runs: the consoles' RECOMP_FF_SPEED range, and
 * what "max" means here (an Xbox title has no unpaced mode). */
#define XBOX_GUEST_CLOCK_MAX_SPEED 16u

typedef struct XboxGuestClock {
    uint64_t host0;    /* host ns at the last speed change */
    uint64_t extra0;   /* ns the guest clock was ahead of the host's at host0 */
    uint32_t speed;    /* 1..XBOX_GUEST_CLOCK_MAX_SPEED */
} XboxGuestClock;

/* ---- Pure ---------------------------------------------------------------- */

/* Speed 1, nothing gained. */
void xbox_GuestClockInit(XboxGuestClock *c);
/* The ns the guest clock is ahead of the host's at host_ns (a host_ns before
 * the last change counts as that change). */
uint64_t xbox_GuestClockExtraAt(const XboxGuestClock *c, uint64_t host_ns);
/* A new speed from host_ns on (clamped to 1..MAX); what was gained is kept. */
void xbox_GuestClockRetime(XboxGuestClock *c, uint64_t host_ns, uint32_t speed);
/* ns in units of per_second (a counter frequency, 1000 for ms, 10^7 for NT's
 * 100 ns), rounded down; no overflow for any ns. */
uint64_t xbox_GuestClockUnits(uint64_t ns, uint64_t per_second);
/* A guest span on the host's clock at this speed: span / speed, rounded up so
 * a non-zero span stays non-zero. Speed 0 or 1: span. */
uint64_t xbox_GuestClockHostSpan(uint64_t span, uint32_t speed);
/* A guest wait in ms (0xFFFFFFFF = INFINITE, kept) on the host's clock. */
uint32_t xbox_GuestClockHostMsAt(uint32_t ms, uint32_t speed);
/* Presentation decimation: 1 when this flip is shown. One flip in `speed`
 * is shown (the first after a change of speed too); speed 1 shows them all.
 * *n is the caller's flip counter, *last_speed the speed it last saw. */
int xbox_GuestClockShowNth(uint32_t *n, uint32_t *last_speed, uint32_t speed);

/* ---- The process's guest clock -------------------------------------------- */

/* Sets the speed (1..MAX) from now on. */
void xbox_GuestClockSetSpeed(uint32_t speed);
/* The current speed, 1 when normal. */
uint32_t xbox_GuestClockSpeed(void);
/* 1 once the speed has ever left 1 (the extra may be non-zero). */
int xbox_GuestClockActive(void);
/* The ns gained now; 0 without reading any clock while never active. */
uint64_t xbox_GuestClockExtraNs(void);
/* The same in units of per_second (see xbox_GuestClockUnits). */
uint64_t xbox_GuestClockExtraUnits(uint64_t per_second);
/* xbox_HostNowNs() plus the ns gained: the guest's monotonic clock. */
uint64_t xbox_GuestNowNs(void);
/* A guest span (ns) or wait (ms, INFINITE kept) on the host's clock now. */
uint64_t xbox_GuestClockHostNs(uint64_t guest_ns);
uint32_t xbox_GuestClockHostMs(uint32_t guest_ms);
/* At a flip: 1 when it is presented (always at speed 1). */
int xbox_GuestClockShowFrame(void);

/* Tests: speed 1, nothing gained, never active. */
void xbox_GuestClockReset(void);

/* ---- kernel_hal.c ---------------------------------------------------------- */

/* GetTickCount64 plus the ms gained: the guest's millisecond tick. */
uint64_t xbox_GuestTickCount64(void);
/* Fast-forward's toggle and hold (recomp_states.h) -> the guest clock's speed
 * (RECOMP_FF_SPEED, "max" = XBOX_GUEST_CLOCK_MAX_SPEED) and the output's mute.
 * Any thread; cheap when nothing changed. A no-op on Windows hosts. */
void xbox_FastForwardPoll(void);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_GUEST_CLOCK_H */
