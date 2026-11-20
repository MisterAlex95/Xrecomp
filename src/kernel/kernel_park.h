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
 *   xbox_ParkHostWaitEnter / Leave  around the runtime helpers lifted code calls that wait on the host
 *                                   (XBA-251, below)
 * Leave and CallbackEnter block while a freeze holds the thread.
 *
 * Host waits inside lifted code (XBA-251): a guest thread can also wait in the middle of a lifted
 * function, in a runtime helper the lifter emits a call to, with no kernel call: xbox_GuestPollWait (the
 * back edge of a loop that only polls one memory word, RECOMP_POLL_WAIT), xbox_Nv2aKickWait (D3D's PFB
 * flush kick, from xbox_GuestPollWait or a title override), xbox_Halt (hlt) and xbox_GuestCpuPreempt
 * (a back edge handing the guest CPU over, RECOMP_BACK_EDGE). Each is a park point like a kernel call:
 * the thread is between two guest instructions at a known guest VA (the site), its registers are in
 * g_cpu, and the helper changes no guest state a restore would have to replay (calling it again is
 * harmless: the poll loop tests its word again, a kick sets a bit already set). Its record says
 * « in host wait <site kind> at <va> » (ordinal 0, ret_va the site's VA); returned means the helper
 * returned and the thread resumes in the lifted code right after the call. xbox_GuestPollWait parks
 * even when it returns at once, so a thread spinning in a marked poll loop is held at its next turn.
 *
 * Not covered: the timer thread's DPCs and the device models' ISRs (host threads that run guest
 * callbacks: quiesced by holding the one-CPU gate, not here), the thunk-table path of native-call
 * titles (kernel_thunks.c), and a spin loop the lifter did not mark (anything but `test [mem], imm;
 * jcc itself`, without --back-edge-yield): a thread there counts as busy.
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

/* Where a parked thread is (XboxParkRecord.site): a kernel call, or a host wait inside lifted code. */
enum {
    XBOX_PARK_SITE_KERNEL = 0,   /* a top-level kernel call (ordinal, guest return address) */
    XBOX_PARK_SITE_POLL,         /* xbox_GuestPollWait returning at once: a poll loop's back edge */
    XBOX_PARK_SITE_KICK,         /* xbox_Nv2aKickWait: waits for the NV2A ack thread (blocks) */
    XBOX_PARK_SITE_HALT,         /* xbox_Halt: hlt, the guest CPU let go for a millisecond (blocks) */
    XBOX_PARK_SITE_PREEMPT,      /* xbox_GuestCpuPreempt: queued for the guest CPU at a back edge (blocks) */
    XBOX_PARK_SITE_COUNT
};

typedef struct {
    int state;               /* XBOX_PARK_* */
    uint32_t ordinal;        /* the top-level kernel call it is in (or last made); 0 at a host wait */
    uint32_t ret_va;         /* that call's guest return address, or the host wait's site */
    int dispatch;            /* held at DISPATCH_LEVEL (it holds the one-CPU gate) */
    unsigned long tid;       /* host thread id */
    uint64_t kernel_calls;   /* top-level kernel calls so far */
    uint32_t site;           /* XBOX_PARK_SITE_* of that park point (XBA-251) */
} XboxParkThread;

typedef struct {
    int threads;             /* registered guest threads */
    int parked;              /* of those, at a kernel boundary (KERNEL, WAITING or HELD) */
    int waiting;             /* of those, in a blocking call or a blocking host wait */
    int host_waits;          /* of the parked ones, at a host wait inside lifted code (XBA-251) */
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
/* Between the bridge's return and the guest CPU taken back (XBA-249): the record says « returned » already, so a
 * thread queued for the guest CPU behind a frozen one counts as back from its call for a same-run load. */
void xbox_ParkKernelReturned(void);
void xbox_ParkCallbackEnter(void);
void xbox_ParkCallbackLeave(void);
/* A host wait inside lifted code (XBOX_PARK_SITE_POLL .. PREEMPT, va the guest VA of the call site; 0 when
 * the caller has none, a title override's call). Nested ones (xbox_GuestPollWait's kick wait) only count
 * once: the outermost site is recorded. Leave blocks while a freeze holds the thread. */
void xbox_ParkHostWaitEnter(uint32_t site, uint32_t va);
/* Returns 1 when a same-run state load (XBA-249) put the thread back « inside » this wait: its registers are
 * the ones it entered with, and the helper waits again (call Enter / wait / Leave once more). */
int  xbox_ParkHostWaitLeave(void);
/* "kernel call", "poll loop", "NV2A kick wait", "hlt", "guest CPU preempt"; "?" otherwise. */
const char *xbox_ParkSiteName(uint32_t site);

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

/* Frozen time (KOTOR's gamepad lost to the first rewind capture): a guest timeout must not run while the game
 * stands still. A capture holds the device threads too (xbox_DevStateFreeze): a wait for a USB transfer
 * that expires during the hold expired against hardware that was stopped.
 *   xbox_ParkFrozenNs       host ns spent with a freeze in force (or being reached), since the start; monotonic,
 *                           and counting the freeze in force now. The difference of two readings is the frozen
 *                           time between them.
 *   xbox_ParkFreezeInForce  1 while a freeze is in force or being reached.
 *   xbox_ParkWaitThaw       a parked guest thread whose timed wait expired: blocks while a freeze is in force
 *                           (max_ms at most), so the expiry neither rewrites its record (the capture needs no
 *                           second attempt) nor returns before the time it lost is given back. Returns at once
 *                           when the thread is not parked (it would keep the freeze from being reached) or the
 *                           freezer lets waits expire. 1 when it waited.
 *   xbox_ParkFreezeLetWaitsExpire  for the freezer: until the thaw, timed waits end at their host timeout again
 *                           (a same-run load settles on every wait having returned, xbox_StateLoadFrozen), and
 *                           threads blocked in xbox_ParkWaitThaw return now. */
uint64_t xbox_ParkFrozenNs(void);
int  xbox_ParkFreezeInForce(void);
int  xbox_ParkWaitThaw(uint32_t max_ms);
void xbox_ParkFreezeLetWaitsExpire(void);
/* 1 while timed waits may be held for the freeze in force (not let expire). */
int  xbox_ParkFreezeHoldsWaits(void);

/* The current picture; reset starts the all-parked statistics again. */
void xbox_ParkStatsGet(XboxParkStats *out, int reset);

/* One line for the periodic kernel summary, when guest threads are registered:
 *   [KERNEL] park: 3 guest threads, all parked 61.0% of 2000 ms (376 windows, longest so far 5.9 ms); now 1
 *            running (one last called 99 from 0x00011000), 2 waiting
 * and, when parked threads are at host waits inside lifted code (XBA-251), the count and one of them:
 *            ..., 2 waiting, 1 at a host wait (NV2A kick wait at 0x0001A2B4)
 * A running thread whose last park point was a host wait reads « (one last in poll loop at 0x...) ».
 * "now" is the moment of the report; the share and the windows cover the time since the previous report
 * (the statistics xbox_ParkStatsGet reads are not reset).
 * Returns 1 when it printed. */
int  xbox_ParkReport(FILE *out);

/* Guest RAM a snapshot would carry: pages of [0, xbox_GetMappedSize()) the host holds (*resident,
 * POSIX mincore; every page elsewhere) and, of those, pages not all zero (*nonzero). Returns the pages
 * in the window. Reads guest memory: freeze first for a consistent count. */
uint32_t xbox_ParkGuestPages(uint32_t *resident, uint32_t *nonzero);

/* ---- Park records: what a save state stores per thread (XBA-245, xbox_state.h) ----------------------------
 *
 * Each registered thread keeps a record of its current top-level kernel call: the ordinal, the guest return
 * address, and a copy of its register set (RecompCpu, the calling thread's g_cpu). Enter writes the
 * registers as the guest made the call (esp at the return address, the arguments above it); Leave rewrites
 * them as the call left them (eax the status, esp past the return address and the stdcall arguments) and
 * sets `returned`. So a frozen thread's record says either « in call N from R, registers at the call » (the
 * call is re-issued on restore, its partial effects on its outputs overwritten) or « returned from N with
 * these registers » (held at the call's end: it resumes at R). Two copies of about 330 bytes per top-level
 * kernel call.
 *
 * Each record has a sequence number, odd while its owner rewrites it. xbox_ParkRecords reads them while a
 * freeze holds the threads; a frozen thread can still change its record once (its call returns, then the
 * freeze holds it), so a capture reads the records, captures, and calls xbox_ParkRecordsChanged: a change
 * means a call returned meanwhile and the capture must be taken again (that thread is held now).
 *
 * A host wait inside lifted code (XBA-251) keeps the same record: site XBOX_PARK_SITE_POLL .. PREEMPT,
 * ordinal 0, ret_va the site's guest VA, the registers at the call (Enter) or after it (Leave, returned;
 * status is eax then, meaningless). A reader that only knows kernel calls tells them apart by `site`
 * (or by ordinal 0: no kernel export has ordinal 0). The field is new at the end of the record: code
 * written against the XBA-245 record compiles unchanged. */
#define XBOX_PARK_CPU_BYTES 512      /* room for a RecompCpuRegs (sizeof checked in kernel_park.c) */

typedef struct {
    int slot;                /* 0..XBOX_PARK_MAX_THREADS-1, stable for the thread's life */
    int state;               /* XBOX_PARK_* */
    uint32_t ordinal;        /* the top-level kernel call */
    uint32_t ret_va;         /* its guest return address */
    int returned;            /* the call has returned (the thread is held at its end) */
    uint32_t status;         /* eax as the call left it, when returned */
    uint32_t call_esp;       /* esp when the call was made (the return address's slot), returned or not */
    int dispatch;            /* held at DISPATCH_LEVEL */
    unsigned long tid;       /* host thread id */
    uint64_t kernel_calls;
    uint32_t seq;            /* even: the record is complete */
    uint32_t cpu_bytes;      /* sizeof(RecompCpuRegs): the registers, not the frames */
    uint64_t cpu[XBOX_PARK_CPU_BYTES / 8];   /* the RecompCpuRegs, cpu_bytes of it */
    uint32_t site;           /* XBOX_PARK_SITE_*: a kernel call, or a host wait inside lifted code */
    int reissue;             /* a same-run load set its registers to make its call again at the thaw (XBA-249) */
    int interp;              /* interpreter activations the thread is inside at the park point (XBA-303):
                              * no lifted frame to resume it at, so a state refuses it */
} XboxParkRecord;

/* The registered threads' records (at most max), each read consistently: a record being rewritten is waited
 * for (its owner finishes within the Leave hook). Meant for a frozen set of threads. Returns the count. */
int  xbox_ParkRecords(XboxParkRecord *out, int max);
/* 1 when a record of r[0..n) changed since it was read, or a thread registered or ended. */
int  xbox_ParkRecordsChanged(const XboxParkRecord *r, int n);
/* Same-run restore (XBA-249, xbox_state.h xbox_StateLoad). With a freeze in force and the thread in `slot` held
 * at the end of its call (state HELD, returned): its register set becomes cpu, which it sees when the call
 * returns at the thaw (reissue 0: the record was « returned », cpu is as the call left it), or the call is
 * made again from cpu (reissue 1: the record was « in the call », cpu as the guest made it, esp at the return
 * address): kernel_thunk_dispatch asks xbox_ParkTakeReissue after its Leave hook and dispatches the same
 * slot again. Its park record is rewritten to match. A thread at a host wait inside lifted code (XBA-251) may
 * also be still waiting: its registers are set at once, and with reissue the helper waits once more when its
 * xbox_ParkHostWaitLeave returns 1. 0, or -1 (not frozen, not held at a call's end nor at a host wait). */
int  xbox_ParkRestoreThread(int slot, const void *cpu, int reissue);   /* cpu: a RecompCpuRegs */
/* On the calling guest thread, after xbox_ParkKernelLeave: 1 (once) when its call must be issued again. */
int  xbox_ParkTakeReissue(void);

/* ---- Re-entry in the same run (XBA-294) ---------------------------------------------------------------------
 *
 * A same-run load whose state has a thread parked at another call chain than the one it stands at now cannot
 * write that thread's registers back: the host frames below it are not the state's. Such a thread is
 * re-entered instead: it leaves its host frames (a longjmp to where its routine was started, on the same host
 * thread, so its kernel objects stay bound to it) and calls its routine again in resume mode (xbox_resume.h,
 * the plan the XBA-250 resume entries follow), as a thread of a fresh process does.
 *
 *   xbox_ParkRunRoutine      xbox_ParkThreadStart; fn(); xbox_ParkThreadEnd, with the re-entry point taken
 *                            first (the SEH dispatcher's state, the frame count). The runtime starts every guest
 *                            routine through it; a thread registered with xbox_ParkThreadStart alone cannot be
 *                            re-entered. A routine run nested on a registered thread (a worker run inline) only
 *                            nests. resume: a plan the routine re-enters at once (a thread of a fresh process,
 *                            xbox_StateResume; the caller keeps it alive), or NULL.
 *   xbox_ParkReenterCheck    whether the frozen thread in `slot` can be re-entered: started through
 *                            xbox_ParkRunRoutine with routine `fn`, not nested, not inside the interpreter, held
 *                            at the end of a top-level park point. 1, or 0 with the reason.
 *   xbox_ParkReenterThread   with a freeze in force: at the thaw, the thread in `slot` leaves its frames at the
 *                            end of its park point and re-enters `plan` (an XboxResumePlan, owned by the thread
 *                            from then on; one set before and not taken yet is freed). 0, or -1.
 *   xbox_ParkReenterPending  1 while a re-entry set on `slot` has not been taken by its thread: its record and
 *                            frames still show the chain it leaves.
 *   xbox_ParkReenterPoint    the jump, on the guest thread, at the end of a park point (after the guest CPU is
 *                            taken back): kernel_thunk_dispatch after xbox_ParkKernelLeave, and the outermost
 *                            xbox_ParkHostWaitLeave. Returns when nothing is asked. */
typedef void (*xbox_park_routine_t)(void);
void xbox_ParkRunRoutine(xbox_park_routine_t fn, void *resume);
int  xbox_ParkReenterCheck(int slot, xbox_park_routine_t fn, char *why, size_t why_n);
int  xbox_ParkReenterThread(int slot, void *plan);
int  xbox_ParkReenterPending(int slot);
void xbox_ParkReenterPoint(void);
/* The routine the thread in `slot` was started with through xbox_ParkRunRoutine (NULL: none, or nested). */
xbox_park_routine_t xbox_ParkRoutine(int slot);
/* Threads re-entered since the start (tests). */
uint32_t xbox_ParkReentered(void);

/* Threads that started while every slot was taken (untracked: a state cannot cover them). */
int  xbox_ParkDropped(void);
/* 1 when the calling thread is a registered guest thread. */
int  xbox_ParkThisThreadIsGuest(void);

/* Tests: forget every thread and statistic. Not thread safe. */
void xbox_ParkReset(void);

/* ---- Guest frames and call sites (XBA-247) ------------------------------------------------------------------
 *
 * Every lifted function that may park pushes a RecompFrame on its thread's frame shadow stack on entry (its
 * entry esp, its caller's published ebp) and drops it at its returns (recomp_types.h, RECOMP_FRAME_ENTRY).
 * xbox_ParkFrames copies a
 * registered thread's frames: at most max of them, from the bottom of its host thread's stack (*base of
 * them were there when its routine started: the INLINE main thread runs inside its creator's kernel call);
 * *depth is the count pushed, which may exceed RECOMP_FRAME_SLOTS (the frames past it were not kept).
 * Meant for a frozen thread (its frames do not move while it is parked). Returns the frames copied, or -1
 * (no such thread).
 *
 * The call-site table comes with the generated code (tools/recomp: recomp_dispatch.c,
 * g_recomp_call_sites): per guest call site its return VA, the function it is in and flags, sorted by
 * return VA. recomp_dispatch_init registers it (count 0 or a NULL table: none). xbox_CallSiteLookup
 * finds a return VA: 1 with its function and flags, 0 when no lifted call returns there. The chain
 * reader and its signature are xbox_StateChain (xbox_state.h). */
enum {
    XBOX_CALL_SITE_INDIRECT = 0x01,      /* through a register or memory */
    XBOX_CALL_SITE_NO_EBP = 0x02,        /* the caller has no ebp local: the callee's frame's ebp means nothing */
    XBOX_CALL_SITE_STACK_SWITCH = 0x04,  /* the caller switches guest stacks (XBA-101) */
    XBOX_CALL_SITE_JMPBUF = 0x08,        /* setjmp / longjmp */
    XBOX_CALL_SITE_SEH_HELPER = 0x10,    /* __SEH_prolog / epilog */
    XBOX_CALL_SITE_MANUAL = 0x20,        /* a hand-written replacement */
    XBOX_CALL_SITE_SEH_ANCHOR = 0x40,    /* the caller takes an SEH resume anchor a resume would skip (XBA-250) */
    XBOX_CALL_SITE_NO_RESUME = 0x80,     /* no resume entry at this site (XBA-250) */
    XBOX_CALL_SITE_HOST_WAIT = 0x100     /* a host wait inside lifted code, keyed 0x80000000 | its VA (XBA-250) */
};
/* What keeps a chain through a site from being re-entered in another run (XBA-250, xbox_resume.h). */
#define XBOX_CALL_SITE_UNRESUMABLE (XBOX_CALL_SITE_STACK_SWITCH | XBOX_CALL_SITE_JMPBUF | XBOX_CALL_SITE_MANUAL \
                                    | XBOX_CALL_SITE_SEH_ANCHOR | XBOX_CALL_SITE_NO_RESUME)
struct RecompFrame;
int  xbox_ParkFrames(int slot, struct RecompFrame *out, uint32_t max, uint32_t *depth, uint32_t *base);
void recomp_call_sites_register(const uint32_t *sites, uint32_t count);
uint32_t xbox_CallSiteCount(void);
int  xbox_CallSiteLookup(uint32_t ret_va, uint32_t *func, uint32_t *flags);

#ifdef __cplusplus
}
#endif

#endif /* KERNEL_PARK_H */
