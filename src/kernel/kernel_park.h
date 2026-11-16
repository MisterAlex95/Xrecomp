/*
 * kernel_park.h - the safe-point park detector (XBA-244, docs/technical/xbox-save-states.md)
 *
 * A save state of a recompiled Xbox title can only be taken when no guest thread is in the middle of
 * lifted code: lifted code keeps its return path and its ebp on the host stack, so a thread caught
 * between two instructions has state nothing can serialise. A thread inside a kernel call is at a
 * known boundary: its registers are in its register set, its guest stack in guest memory, and the
 * call (ordinal, guest return address) names where it resumes. This tracks, per guest thread, whether
 * it is at that boundary ("parked") or running guest code ("busy"), and can freeze every guest thread
 * at its next boundary (xbox_ParkFreeze): the first step of a snapshot.
 *
 * Parked means: inside a top-level kernel call of the thread (not one made from a guest routine a bridge
 * runs inside another kernel call, an APC for instance: that thread has a bridge's host frame between
 * its guest frames, which a restore could not rebuild). A blocking call (a wait, a sleep, file I/O) is
 * parked for long; any other call only for its length, but a frozen thread is held at the call's end,
 * so with a freeze pending every kernel call is a place to stop. With the guest CPU lock on, a thread
 * queued for the guest CPU at the end of a call has run no guest code yet and is still parked; the
 * freeze holds it once it has the CPU (the other threads are parked or queued behind it).
 *
 * Hooks (each a few thread-local updates; one atomic add when a thread changes between parked and busy):
 *   xbox_ParkThreadStart / End      around a guest thread's routine (kernel_bridge.c)
 *   xbox_ParkKernelEnter / Leave    around every kernel call (kernel_thunk_dispatch)
 *   xbox_ParkCallbackEnter / Leave  around guest code a bridge runs (xbox_GuestCpuEnterCallback)
 * Leave and CallbackEnter block while a freeze holds the thread.
 *
 * Not covered: the timer thread's DPCs and the device models' ISRs (host threads that run guest
 * callbacks: quiesced by holding the one-CPU gate, not here), the thunk-table path of native-call
 * titles (kernel_thunks.c), and host waits made from inside lifted code (xbox_GuestPollWait, NV2A kick
 * waits): a thread blocked there counts as busy.
 *
 * Statistics: the share of time every registered guest thread was parked at once, and how many such
 * windows there were, from the busy count's transitions (approximate under races, never wrong by more
 * than one transition). Always on (it costs what the guest CPU lock's own bookkeeping costs).
 */
#ifndef KERNEL_PARK_H
#define KERNEL_PARK_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XBOX_PARK_MAX_THREADS 64

enum {
    XBOX_PARK_FREE = 0,      /* slot unused */
    XBOX_PARK_RUNNING,       /* running guest code */
    XBOX_PARK_KERNEL,        /* inside a top-level non-blocking kernel call */
    XBOX_PARK_WAITING,       /* inside a top-level blocking kernel call (wait, sleep, I/O) */
    XBOX_PARK_HELD           /* held by a freeze at the end of a kernel call, or at a callback's start */
};

typedef struct {
    int state;               /* XBOX_PARK_* */
    uint32_t ordinal;        /* the top-level kernel call it is in (or last made) */
    uint32_t ret_va;         /* that call's guest return address */
    int dispatch;            /* held at DISPATCH_LEVEL (it holds the one-CPU gate) */
    unsigned long tid;       /* host thread id */
    uint64_t kernel_calls;   /* top-level kernel calls so far */
} XboxParkThread;

typedef struct {
    int threads;             /* registered guest threads */
    int parked;              /* of those, at a kernel boundary (KERNEL, WAITING or HELD) */
    int waiting;             /* of those, in a blocking call */
    int frozen;              /* a freeze is in force */
    uint64_t all_parked_ns;  /* time every registered thread was parked at once (since the last reset) */
    uint64_t window_ns;      /* time covered (since the last reset) */
    uint64_t windows;        /* all-parked windows that ended */
    uint64_t longest_ns;     /* the longest such window */
    XboxParkThread t[XBOX_PARK_MAX_THREADS];   /* the registered ones first, `threads` of them */
} XboxParkStats;

/* Ordinals that can block the caller (waits, sleeps, file I/O, a suspend). */
int  xbox_ParkOrdinalBlocks(uint32_t ordinal);

/* Hooks. Start registers the calling thread as a guest thread whose routine runs now (a second Start on
 * a registered thread, a worker run inline on its creator, only nests). End unregisters it; so does a
 * kernel call to PsTerminateSystemThread. */
void xbox_ParkThreadStart(void);
void xbox_ParkThreadEnd(void);
void xbox_ParkKernelEnter(uint32_t ordinal, uint32_t ret_va);
void xbox_ParkKernelLeave(void);
void xbox_ParkCallbackEnter(void);
void xbox_ParkCallbackLeave(void);

/* The calling thread's kernel-call and callback depths, packed in 16 bits, and putting them back: an SEH
 * resume or a guest longjmp that jumps over bridge frames skips their Leave hooks (xbox_SehRuntimeSnapshot
 * carries these). Restore moves the thread between parked and busy as the depths say. */
uint32_t xbox_ParkDepthSnapshot(void);
void xbox_ParkDepthRestore(uint32_t snap);

/* 1 when the calling thread is a registered guest thread parked at a kernel boundary. */
int  xbox_ParkThisThreadParked(void);

/* Freezes every guest thread at its next kernel boundary: returns 1 once all registered threads are
 * parked and held (none can run guest code until xbox_ParkThaw), 0 when timeout_ms passed first (the
 * freeze is then lifted; *out, when given, shows who was still running). Not from a guest thread. One
 * freezer at a time: a second concurrent call returns 0 at once. */
int  xbox_ParkFreeze(uint32_t timeout_ms, XboxParkStats *out);
void xbox_ParkThaw(void);

/* The current picture; reset starts the all-parked statistics again. */
void xbox_ParkStatsGet(XboxParkStats *out, int reset);

/* One line for the periodic kernel summary, when guest threads are registered:
 *   [KERNEL] park: 3 guest threads, all parked 61.0% of 2000 ms (376 windows, longest so far 5.9 ms); now 1
 *            running (one last called 99 from 0x00011000), 2 waiting
 * "now" is the moment of the report; the share and the windows cover the time since the previous report
 * (the statistics xbox_ParkStatsGet reads are not reset).
 * Returns 1 when it printed. */
int  xbox_ParkReport(FILE *out);

/* Guest RAM a snapshot would carry: pages of [0, xbox_GetMappedSize()) the host holds (*resident,
 * POSIX mincore; every page elsewhere) and, of those, pages not all zero (*nonzero). Returns the pages
 * in the window. Reads guest memory: freeze first for a consistent count. */
uint32_t xbox_ParkGuestPages(uint32_t *resident, uint32_t *nonzero);

/* Tests: forget every thread and statistic. Not thread safe. */
void xbox_ParkReset(void);

#ifdef __cplusplus
}
#endif

#endif /* KERNEL_PARK_H */
