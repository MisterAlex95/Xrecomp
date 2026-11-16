/*
 * kernel_kobj_state.h - the kernel's guest-visible objects in a save state
 * (XBA-246, docs/technical/xbox-save-states.md, section KOBJ).
 *
 * Every object a title can hold a handle to, or that the kernel keeps for a
 * guest structure, read without changing it and made again with the same
 * handle values: events, semaphores, mutants, waitable timers, threads'
 * kernel-side records, files and directories (host path, offset, open mode),
 * I/O completion ports with their queued packets, the handle table (tokens,
 * thread objects behind them, reference counts), the KE shadows of guest
 * KEVENT / KMUTANT / KSEMAPHORE / KTIMER / thread objects, kernel timers
 * (due times relative to the guest clock), the DPC queue, the alert state of
 * threads, ObCreateObject objects, the critical-section semaphores, and the
 * small registration tables (interrupts, shutdown and thread notification
 * routines, the APC pool, asynchronous file handles).
 *
 * The guest structures themselves (KEVENT headers, KTHREAD, APC lists, KQUEUE)
 * live in guest RAM, which the RAM section (XBA-245) carries; this section is
 * the host side of them.
 *
 * Usage, as one section of the state container:
 *
 *   XboxKobjRefusals why;
 *   void *blob; size_t len;
 *   if (!xbox_KobjSave(&blob, &len, XBOX_KOBJ_IN_SESSION, &why)) refuse(why);
 *   ... container_put(tag XBOX_KOBJ_TAG, version XBOX_KOBJ_VERSION, blob, len); free(blob);
 *   ...
 *   if (!xbox_KobjLoad(blob, len, XBOX_KOBJ_IN_SESSION, err, sizeof err)) fail(err);
 *
 * Both run with every guest thread frozen (xbox_ParkFreeze) and nothing else
 * using the kernel. A load tears the current objects down first
 * (xbox_KobjReset) and recreates the saved ones under their saved handle
 * values, so no thread may be blocked in a host wait on one of them then: a
 * load refuses while a thread is blocked on an event (the one kind that
 * counts its waiters); bringing parked threads out of their waits and back
 * is the in-session restore's (XBA-249).
 *
 * Threads: a thread that has ended is made again (signalled, its exit
 * status). A running thread cannot be made here (its host stack is not
 * serialisable, XBA-247 / XBA-250): an in-session load binds its records to
 * the thread still running with the same KTHREAD, and fails when there is
 * none (threads are matched by host thread id, which is the process's
 * own); a save for another run (XBOX_KOBJ_ACROSS_RUNS) is refused while any
 * guest thread runs.
 *
 * The blob is self-describing: "XKOB", a version, then records (u16 type,
 * u16 version, u32 length, payload), little-endian. A reader skips record
 * types it does not know.
 *
 * POSIX hosts only for now: on Windows the objects are real Win32 objects
 * whose state cannot be read without changing it, and a save is refused.
 */
#ifndef KERNEL_KOBJ_STATE_H
#define KERNEL_KOBJ_STATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XBOX_KOBJ_TAG     0x4A424F4Bu   /* 'KOBJ' as a little-endian u32 */
#define XBOX_KOBJ_VERSION 1u

/* Flags. */
#define XBOX_KOBJ_IN_SESSION  0u   /* load into the process that saved it */
#define XBOX_KOBJ_ACROSS_RUNS 1u   /* load into a fresh process (refuses running threads) */
/* With ACROSS_RUNS, for a resume (XBA-250): running threads (and the mutants they own) are not refused, the
 * resume makes them again; xbox_KobjResumeThreads names the new threads before the load. */
#define XBOX_KOBJ_RESUME_THREADS 2u

/* Why a save cannot be taken: up to XBOX_KOBJ_REFUSALS_MAX, `count` is all of
 * them. `token` is the guest handle when the object has one, `guest_va` the
 * guest structure when it has one. */
#define XBOX_KOBJ_REFUSALS_MAX 16
typedef struct {
    uint32_t token;
    uint32_t guest_va;
    char     what[24];      /* "event", "thread", "file", ... */
    char     reason[104];
} XboxKobjRefusal;
typedef struct {
    int             count;
    XboxKobjRefusal r[XBOX_KOBJ_REFUSALS_MAX];
} XboxKobjRefusals;

/* The objects that keep a save from being taken (why may be NULL). Returns
 * how many: 0 means a save would succeed. */
int  xbox_KobjCheck(unsigned flags, XboxKobjRefusals *why);

/* Serialise. On success *out is a malloc'd blob (free() it) of *len bytes
 * and the result is 1. With refusals the result is 0, *out NULL, and why (if
 * not NULL) says which objects and why. */
int  xbox_KobjSave(void **out, size_t *len, unsigned flags, XboxKobjRefusals *why);

/* Tear the current objects down and make the blob's. Returns 1, or 0 with a
 * reason in err. A blob that is not whole fails before anything changes;
 * a failure while making objects leaves the kernel reset (empty). */
int  xbox_KobjLoad(const void *blob, size_t len, unsigned flags, char *err, size_t errlen);

/* For a load with XBOX_KOBJ_ACROSS_RUNS (XBA-250): the host threads made for the saved running threads, by the
 * host thread id each had at the save (n at most 64; n 0 forgets them). The load takes a reference of its own.
 * Before a check with XBOX_KOBJ_RESUME_THREADS: the threads the state holds (handles NULL); a running thread not
 * among them is refused. */
void xbox_KobjResumeThreads(const uint32_t *tids, void *const *handles, int n);

/* Close every guest-visible kernel object and empty the tables above, as a
 * fresh kernel has them. Running threads go on running; their records are
 * dropped. */
void xbox_KobjReset(void);

/* ── Internal: the pieces the owners of each table provide ───────────────── */

/* A writer and reader over a growing byte buffer (kernel_kobj_state.c). */
typedef struct {
    uint8_t *p;
    size_t   len, cap;
    int      oom;
} XboxKobjBuf;
void     xbox_kobj_put(XboxKobjBuf *b, const void *data, size_t n);
void     xbox_kobj_u8(XboxKobjBuf *b, uint8_t v);
void     xbox_kobj_u16(XboxKobjBuf *b, uint16_t v);
void     xbox_kobj_u32(XboxKobjBuf *b, uint32_t v);
void     xbox_kobj_u64(XboxKobjBuf *b, uint64_t v);
void     xbox_kobj_str(XboxKobjBuf *b, const char *s);   /* u16 length + bytes */

typedef struct {
    const uint8_t *p;
    size_t         len, pos;
    int            bad;     /* read past the end */
} XboxKobjRd;
uint8_t  xbox_kobj_get8(XboxKobjRd *r);
uint16_t xbox_kobj_get16(XboxKobjRd *r);
uint32_t xbox_kobj_get32(XboxKobjRd *r);
uint64_t xbox_kobj_get64(XboxKobjRd *r);
void     xbox_kobj_getstr(XboxKobjRd *r, char *out, size_t cap);

/* A save in progress: the host objects found so far (one id each, shared by
 * every table that names them) and the records. */
typedef struct XboxKobjSaveCtx XboxKobjSaveCtx;
/* The id of host object h in this save (registered on first sight, its state
 * read then); 0 for NULL. `token` / `guest_va` / `what` name it in a refusal. */
uint32_t xbox_kobj_ref(XboxKobjSaveCtx *c, void *h, uint32_t token, uint32_t guest_va);
/* Start a record of `type` (version 1) and end it. Not inside
 * xbox_kobj_ref's reach: records are written to their own buffer. */
XboxKobjBuf *xbox_kobj_rec_begin(XboxKobjSaveCtx *c, uint16_t type);
void     xbox_kobj_rec_end(XboxKobjSaveCtx *c);
void     xbox_kobj_refuse(XboxKobjSaveCtx *c, uint32_t token, uint32_t guest_va,
                          const char *what, const char *fmt, ...);
unsigned xbox_kobj_flags(const XboxKobjSaveCtx *c);

/* A load in progress: a reference to host object `id` (one more each call;
 * the caller owns it), NULL for 0 or an id that is not there. */
typedef struct XboxKobjLoadCtx XboxKobjLoadCtx;
void    *xbox_kobj_obj(XboxKobjLoadCtx *c, uint32_t id);
void     xbox_kobj_fail(XboxKobjLoadCtx *c, const char *fmt, ...);
int      xbox_kobj_failed(const XboxKobjLoadCtx *c);
unsigned xbox_kobj_load_flags(const XboxKobjLoadCtx *c);
/* The running thread with this host thread id, for an in-session load (one
 * more reference), or NULL. Thread ids are the process's own, so an
 * in-session load finds each running thread under the id it was saved with. */
void    *xbox_kobj_live_thread(XboxKobjLoadCtx *c, uint32_t tid);

/* Record types. */
enum {
    XKOB_R_OBJECT   = 1,    /* a host object: id, kind, state */
    XKOB_R_CLOCK    = 2,    /* guest clock at the save */
    XKOB_R_HANDLE   = 3,    /* handle table slot: index, object id, thread object */
    XKOB_R_THPOOL   = 4,    /* guest thread object pool */
    XKOB_R_SHADOW   = 5,    /* KE shadow: guest VA, object id, kind */
    XKOB_R_TIMER    = 6,    /* kernel timer */
    XKOB_R_DPC      = 7,    /* queued DPC */
    XKOB_R_THALERT  = 8,    /* a thread's alert state */
    XKOB_R_U32TAB   = 9,    /* a table of guest addresses / tokens */
    XKOB_R_OB       = 10,   /* an ObCreateObject object */
    XKOB_R_PORT     = 12,   /* I/O completion port and its packets */
    XKOB_R_PASSOC   = 13,   /* file joined to a port */
    XKOB_R_APCPEND  = 14,   /* inserted KAPCs */
    XKOB_R_CSSEM    = 15,   /* critical-section semaphore */
    XKOB_R_CSHELD   = 16,   /* a held host critical section (checked, not made) */
    XKOB_R_END      = 0xFFFF
};

/* Host object kinds in XKOB_R_OBJECT. */
enum {
    XKOB_K_EVENT = 1, XKOB_K_SEMAPHORE, XKOB_K_MUTANT, XKOB_K_THREAD,
    XKOB_K_TIMER, XKOB_K_FILE
};

/* The tables' owners (kernel_bridge.c, kernel_rtl.c): add their records,
 * check their own state, drop everything, and take their records back. */
void xbox_bridge_kobj_save(XboxKobjSaveCtx *c);
void xbox_bridge_kobj_reset(void);
int  xbox_bridge_kobj_load(XboxKobjLoadCtx *c, uint16_t type, XboxKobjRd *r);
/* Every host handle the bridge's tables hold, with the guest structure it
 * stands for (a KE shadow's guest VA, the thread object behind a token, a
 * thread's KTHREAD in the alert table; 0 for none). */
void xbox_bridge_kobj_handles(void (*fn)(void *ctx, uint32_t guest_va, void *h), void *ctx);
void xbox_rtl_kobj_save(XboxKobjSaveCtx *c);
void xbox_rtl_kobj_reset(void);
int  xbox_rtl_kobj_load(XboxKobjLoadCtx *c, uint16_t type, XboxKobjRd *r);

/* Files (kernel_file.c, POSIX): what reopens one. */
typedef struct {
    char     path[1024];    /* host path */
    int      flags;         /* open(2) flags it has (O_ACCMODE, O_APPEND) */
    int      is_dir;
    int      disc;          /* a file of the mounted disc image */
    int      overwrite_pending;
    int64_t  offset;        /* the file position */
    uint32_t disc_next;     /* disc image: directory search state */
    int      disc_scanning;
    char     disc_pattern[64];
} XboxFileState;
/* 1 and *out for a file handle; 0 for no file; -1 when it cannot be saved
 * (why says so). */
int   xbox_FileStateRead(void *h, XboxFileState *out, char *why, size_t n);
/* A handle that is the file again, or NULL (why says so). */
void *xbox_FileStateReopen(const XboxFileState *s, char *why, size_t n);
/* Before a teardown: forget the handle's pending overwrite and directory
 * search, so closing it changes nothing on disk. */
void  xbox_FileStateForget(void *h);

#ifdef __cplusplus
}
#endif

#endif /* KERNEL_KOBJ_STATE_H */
