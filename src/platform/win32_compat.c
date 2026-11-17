/*
 * win32_compat.c - POSIX implementation of generic Win32 host primitives.
 *
 * See win32_compat.h. This is deliberately a *generic* OS-primitive layer
 * (threads/events/mutexes/atomics/heap/timers) -- it carries no Xbox
 * semantics. The Xbox kernel HLE in src/kernel builds on top of it.
 *
 * POSIX (Linux/MacOS) only.
 */

#if !defined(_WIN32)

/* Enable memfd_create, MAP_FIXED_NOREPLACE, timegm. Must precede all
#includes. */
#define _GNU_SOURCE
/* Darwin: exposes memset_s, its explicit_bzero equivalent. */
#define __STDC_WANT_LIB_EXT1__ 1

#include "win32_compat.h"
#include "host_cpus.h"   /* recomp_host_cpus_track (XBA-50) */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <sched.h>
#include <fenv.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <limits.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/stat.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#else
#include <sys/sysinfo.h>
#endif

#if defined(__ANDROID__)
/* bionic lacks the glibc thread-affinity calls; the kernel's per-task
 * affinity works on the thread's tid, which bionic hands out. */
static int xr_thread_affinity(pthread_t th, size_t size, cpu_set_t *set, int get)
{
    pid_t tid = pthread_gettid_np(th);
    int rc = get ? sched_getaffinity(tid, size, set)
                 : sched_setaffinity(tid, size, set);
    return rc == 0 ? 0 : errno;
}
#define pthread_getaffinity_np(th, size, set) xr_thread_affinity((th), (size), (set), 1)
#define pthread_setaffinity_np(th, size, set) xr_thread_affinity((th), (size), (cpu_set_t *)(set), 0)
#endif

/* ===================================================================== */
/* Last-error (thread-local)                                             */
/* ===================================================================== */

static __thread DWORD t_last_error = 0;

DWORD GetLastError(void)            { return t_last_error; }
VOID  SetLastError(DWORD code)      { t_last_error = code; }

/* ===================================================================== */
/* Interlocked atomics                                                   */
/* ===================================================================== */

LONG InterlockedIncrement(volatile LONG *p)        { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
LONG InterlockedDecrement(volatile LONG *p)        { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
LONG InterlockedExchange(volatile LONG *p, LONG v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
LONG InterlockedExchangeAdd(volatile LONG *p, LONG v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }

LONG InterlockedAnd(volatile LONG *p, LONG v)      { return __atomic_fetch_and(p, v, __ATOMIC_SEQ_CST); }
LONG InterlockedOr(volatile LONG *p, LONG v)       { return __atomic_fetch_or(p, v, __ATOMIC_SEQ_CST); }

LONG InterlockedCompareExchange(volatile LONG *p, LONG xchg, LONG cmp)
{
    __atomic_compare_exchange_n(p, &cmp, xchg, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return cmp;
}

LONGLONG InterlockedCompareExchange64(volatile LONGLONG *p, LONGLONG xchg, LONGLONG cmp)
{
    __atomic_compare_exchange_n(p, &cmp, xchg, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return cmp;
}

PVOID InterlockedCompareExchangePointer(PVOID volatile *p, PVOID xchg, PVOID cmp)
{
    __atomic_compare_exchange_n(p, &cmp, xchg, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return cmp;
}

/* ===================================================================== */
/* Critical sections (recursive pthread mutex)                           */
/* ===================================================================== */

static pthread_mutex_t *cs_new_mutex(void)
{
    pthread_mutex_t *m = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &attr);
    pthread_mutexattr_destroy(&attr);
    return m;
}

VOID InitializeCriticalSection(LPCRITICAL_SECTION cs)
{
    memset(cs, 0, sizeof(*cs));
    cs->LockSemaphore = cs_new_mutex();
}

/* A zeroed (never initialised) section gets its mutex on first use. Two
 * threads can reach that first use together: as for SRW locks below, the
 * creation is serialised and the pointer published atomically, so both end
 * up on the same mutex (each used to install its own and both "held" it). */
static pthread_mutex_t *cs_mutex(LPCRITICAL_SECTION cs)
{
    pthread_mutex_t *m = __atomic_load_n((pthread_mutex_t **)&cs->LockSemaphore, __ATOMIC_ACQUIRE);
    if (!m) {
        static pthread_mutex_t init_lock = PTHREAD_MUTEX_INITIALIZER;
        pthread_mutex_lock(&init_lock);
        m = (pthread_mutex_t *)cs->LockSemaphore;
        if (!m) {
            m = cs_new_mutex();
            __atomic_store_n((pthread_mutex_t **)&cs->LockSemaphore, m, __ATOMIC_RELEASE);
        }
        pthread_mutex_unlock(&init_lock);
    }
    return m;
}

VOID InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION cs, DWORD spin)
{
    InitializeCriticalSection(cs);
    cs->SpinCount = spin;
}

VOID EnterCriticalSection(LPCRITICAL_SECTION cs)
{
    pthread_mutex_lock(cs_mutex(cs));
    cs->RecursionCount++;
}

VOID LeaveCriticalSection(LPCRITICAL_SECTION cs)
{
    if (!cs->LockSemaphore) return;
    cs->RecursionCount--;
    pthread_mutex_unlock((pthread_mutex_t *)cs->LockSemaphore);
}

BOOL TryEnterCriticalSection(LPCRITICAL_SECTION cs)
{
    if (pthread_mutex_trylock(cs_mutex(cs)) == 0) {
        cs->RecursionCount++;
        return TRUE;
    }
    return FALSE;
}

VOID DeleteCriticalSection(LPCRITICAL_SECTION cs)
{
    if (cs->LockSemaphore) {
        pthread_mutex_destroy((pthread_mutex_t *)cs->LockSemaphore);
        free(cs->LockSemaphore);
        cs->LockSemaphore = NULL;
    }
}

/* ===================================================================== */
/* Slim reader/writer locks                                              */
/* ===================================================================== */

/* An SRWLOCK is usable straight from SRWLOCK_INIT, so the pthread_rwlock_t
 * behind it has to appear on first use. Unlike the condition variables below
 * -- whose lazy init is covered by the caller holding the paired CRITICAL
 * SECTION -- an SRWLOCK is by definition taken from several threads at once
 * with nothing else held, so first use genuinely races. Serialise just that:
 * once Ptr is published, every acquire is a plain atomic load. */
static pthread_rwlock_t *srw_lazy_init(PSRWLOCK lock)
{
    pthread_rwlock_t *rw = __atomic_load_n((pthread_rwlock_t **)&lock->Ptr,
                                           __ATOMIC_ACQUIRE);
    if (!rw) {
        static pthread_mutex_t init_lock = PTHREAD_MUTEX_INITIALIZER;
        pthread_mutex_lock(&init_lock);
        rw = (pthread_rwlock_t *)lock->Ptr;
        if (!rw) {
            rw = (pthread_rwlock_t *)malloc(sizeof(*rw));
            pthread_rwlock_init(rw, NULL);
            __atomic_store_n((pthread_rwlock_t **)&lock->Ptr, rw, __ATOMIC_RELEASE);
        }
        pthread_mutex_unlock(&init_lock);
    }
    return rw;
}

VOID InitializeSRWLock(PSRWLOCK lock)
{
    lock->Ptr = NULL;
    srw_lazy_init(lock);
}

VOID AcquireSRWLockShared(PSRWLOCK lock)     { pthread_rwlock_rdlock(srw_lazy_init(lock)); }
VOID ReleaseSRWLockShared(PSRWLOCK lock)     { pthread_rwlock_unlock(srw_lazy_init(lock)); }
VOID AcquireSRWLockExclusive(PSRWLOCK lock)  { pthread_rwlock_wrlock(srw_lazy_init(lock)); }
VOID ReleaseSRWLockExclusive(PSRWLOCK lock)  { pthread_rwlock_unlock(srw_lazy_init(lock)); }

/* ===================================================================== */
/* One-time initialisation                                               */
/* ===================================================================== */

/* Win32 semantics: the callback runs at most once for a given INIT_ONCE, and
 * a callback returning FALSE leaves it un-run so a later call retries. Ptr
 * doubles as the "done" flag. One global mutex covers every INIT_ONCE --
 * initialisation is rare, and the fast path never touches it. */
BOOL InitOnceExecuteOnce(PINIT_ONCE once, PINIT_ONCE_FN fn, PVOID param, PVOID *context)
{
    static pthread_mutex_t once_lock = PTHREAD_MUTEX_INITIALIZER;

    if (__atomic_load_n(&once->Ptr, __ATOMIC_ACQUIRE))
        return TRUE;

    pthread_mutex_lock(&once_lock);
    BOOL ok = TRUE;
    if (!once->Ptr) {
        ok = fn(once, param, context);
        if (ok)
            __atomic_store_n(&once->Ptr, (PVOID)1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&once_lock);
    return ok;
}

/* ===================================================================== */
/* Condition variables (paired with a CRITICAL_SECTION)                  */
/* ===================================================================== */

/* The clock every timed wait counts on. A phone steps its wall clock (NTP,
 * carrier time) far more often than a desktop, and a CLOCK_REALTIME deadline
 * then stretches or cuts the wait by the step. Darwin has no
 * pthread_condattr_setclock, so it stays on the wall clock there. */
#if defined(__APPLE__)
#define W32_WAIT_CLOCK CLOCK_REALTIME
#else
#define W32_WAIT_CLOCK CLOCK_MONOTONIC
#endif

/* A condition variable whose timed waits count on W32_WAIT_CLOCK. */
static void cond_init_wait_clock(pthread_cond_t *c)
{
#if defined(__APPLE__)
    pthread_cond_init(c, NULL);
#else
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, W32_WAIT_CLOCK);
    pthread_cond_init(c, &a);
    pthread_condattr_destroy(&a);
#endif
}

/* Forward decl; the definition lives further down with the wait helpers. */
static void deadline_from_ms(DWORD ms, struct timespec *ts);

/* As cs_mutex: a Wake can come with no lock held, at the same moment as a
 * first Sleep, so the creation is serialised too. */
static void cv_lazy_init(PCONDITION_VARIABLE cv)
{
    if (!__atomic_load_n(&cv->Ptr, __ATOMIC_ACQUIRE)) {
        static pthread_mutex_t init_lock = PTHREAD_MUTEX_INITIALIZER;
        pthread_mutex_lock(&init_lock);
        if (!cv->Ptr) {
            pthread_cond_t *c = (pthread_cond_t *)malloc(sizeof(pthread_cond_t));
            cond_init_wait_clock(c);
            __atomic_store_n(&cv->Ptr, (PVOID)c, __ATOMIC_RELEASE);
        }
        pthread_mutex_unlock(&init_lock);
    }
}

VOID InitializeConditionVariable(PCONDITION_VARIABLE cv)
{
    cv->Ptr = NULL;
    cv_lazy_init(cv);
}

BOOL SleepConditionVariableCS(PCONDITION_VARIABLE cv, PCRITICAL_SECTION cs, DWORD ms)
{
    cv_lazy_init(cv);
    pthread_cond_t  *c = (pthread_cond_t  *)cv->Ptr;
    pthread_mutex_t *m = cs_mutex(cs);
    if (ms == INFINITE) {
        pthread_cond_wait(c, m);
        return TRUE;
    }
    struct timespec ts;
    deadline_from_ms(ms, &ts);
    int rc = pthread_cond_timedwait(c, m, &ts);
    if (rc == ETIMEDOUT) { SetLastError(WAIT_TIMEOUT); return FALSE; }
    return TRUE;
}

VOID WakeConditionVariable(PCONDITION_VARIABLE cv)
{
    cv_lazy_init(cv);
    pthread_cond_signal((pthread_cond_t *)cv->Ptr);
}

VOID WakeAllConditionVariable(PCONDITION_VARIABLE cv)
{
    cv_lazy_init(cv);
    pthread_cond_broadcast((pthread_cond_t *)cv->Ptr);
}

/* ===================================================================== */
/* Waitable kernel objects                                               */
/* ===================================================================== */

typedef enum { K_EVENT, K_SEM, K_MUTEX, K_THREAD, K_TIMER, K_HEAP,
               K_FILEMAP, K_FILE, K_WAITABLE_TIMER } w32_kind;

#define W32_MAX_APC 16

/* A thread blocked in WaitForMultipleObjectsEx: one wait block of its own,
 * linked into every object it waits on (w32_waitlink). Whatever changes an
 * object's state wakes the object's own waiters and bumps each linked
 * block's seq (obj_wake). Lock order: object lock, then the block's. */
typedef struct w32_waitblock {
    pthread_mutex_t m;
    pthread_cond_t  c;
    unsigned        seq;
} w32_waitblock;

typedef struct w32_waitlink {
    struct w32_waitlink *next;
    w32_waitblock       *wb;
} w32_waitlink;

typedef struct w32_object {
    w32_kind        kind;
    LONG            refcount;
    uint32_t        handle_id;   /* its slot in the handle table, (slot+1)*4 */
    pthread_mutex_t lock;
    pthread_cond_t  cond;

    /* event */
    int             signaled;
    int             manual_reset;
    /* A set or a pulse releases the threads blocked on the event at that
     * moment, whatever happens to its state afterwards (see wait_single). */
    unsigned        ev_waiters;  /* threads blocked in wait_single on it */
    unsigned        ev_grants;   /* auto-reset: releases handed to blocked waiters, not taken yet */
    unsigned        ev_epoch;    /* manual-reset: one more per SetEvent or PulseEvent that released */
    w32_waitlink   *mw_list;     /* multiple-object waiters linked to it, under lock */

    /* semaphore */
    long            sem_count;
    long            sem_max;

    /* mutex */
    DWORD           mtx_owner;
    int             mtx_recursion;

    /* thread */
    pthread_t       thread;      /* detached: valid only while !exited, read under lock */
    DWORD           tid;
    int             exited;
    DWORD           exit_code;
    int             suspend_count;
    pthread_cond_t  gate;
    LPTHREAD_START_ROUTINE start;
    LPVOID          start_param;
    int             priority;
    PAPCFUNC        apc_func[W32_MAX_APC];
    ULONG_PTR       apc_data[W32_MAX_APC];
    int             apc_count;

    /* timer-queue timer: the fields are read and written under lock */
    int             timer_cancel;
    int             timer_running;   /* the thread is inside the callback */
    unsigned        timer_seq;       /* one more per ChangeTimerQueueTimer */
    DWORD           timer_due;
    DWORD           timer_period;
    WAITORTIMERCALLBACK timer_cb;
    PVOID           timer_param;

    /* waitable timer */
    int             waitable_manual_reset;
    struct timespec waitable_due_time;
    int             waitable_triggered;
    int             waitable_armed;
    LONG            waitable_period_ms;   /* > 0: re-arms itself after each expiry */

    /* file mapping / fd-backed file handle */
    int             fd;
    SIZE_T          map_size;
    char           *file_path;
    /* A file handle with no host fd (a file of a disc image, XBA-126):
     * the file layer's own state, freed with the handle. */
    void           *file_ext;
    void          (*file_ext_free)(void *);
} w32_object;

/* pseudo handles for "current thread"/"current process" */
#define PSEUDO_CURRENT_PROCESS ((HANDLE)(LONG_PTR)-1)
#define PSEUDO_CURRENT_THREAD  ((HANDLE)(LONG_PTR)-2)

/*
 * A HANDLE is an index into this table, not the object's address.
 *
 * The guest is 32-bit code: every handle it receives is stored in a 32-bit
 * slot and handed back later, which the kernel bridge does literally --
 * `sp[1] = (uint32_t)(uintptr_t)h`. Returning the w32_object pointer means
 * handing out values like 0x1038dffd0, because macOS heap allocations sit
 * above 4 GB. The guest keeps the low half, gives it back, and the shim
 * dereferences an address that was never an object: a SIGSEGV with nothing
 * to say it came from a truncated handle.
 *
 * Windows has no such problem -- a Win32 HANDLE is an index into the process
 * handle table, small even in a 64-bit process, which is why handles are
 * documented as 32-bit-significant for interop. This is the same trick.
 *
 * Values are HTAB_TAG | (slot + 1) * 4, so no live handle is NULL, -1, -2 or
 * INVALID_HANDLE_VALUE, and all of them fit in 32 bits.
 *
 * The tag is not decoration. A bare (slot + 1) * 4 makes the first handles 4,
 * 8, 12 -- and the kernel bridge turns any guest word it does not recognise
 * into a HANDLE by cast (bridge_resolve_handle's "untagged: synthetic/dummy
 * handle" path). A title passing a small bogus value would then land on a live
 * object and quietly operate on someone else's event, where the old pointer
 * handles made it a fault at address 4. The tag keeps the two apart, and is
 * distinct from the bridge's own 0x48000000 token tag so neither is mistaken
 * for the other. */
#define HTAB_TAG    0x5A000000u
#define HTAB_MASK   0x00FFFFFFu
#define HTAB_STRIDE 4u

static w32_object    **s_htab;
static uint32_t        s_htab_len;
/* No free slot below this one: obj_handle scans from here, not from 0, so
 * creating a handle with N live ones is not O(N) under the lock. */
static uint32_t        s_htab_free_hint;
static pthread_mutex_t s_htab_lock = PTHREAD_MUTEX_INITIALIZER;

/* Register o and return its handle. Idempotent: an object keeps one id for
 * its lifetime, so DuplicateHandle and repeated GetCurrentThread() agree. */
static HANDLE obj_handle(w32_object *o)
{
    uint32_t slot;
    HANDLE h;

    if (!o) return NULL;
    pthread_mutex_lock(&s_htab_lock);
    if (o->handle_id) {
        HANDLE existing = (HANDLE)(uintptr_t)o->handle_id;
        pthread_mutex_unlock(&s_htab_lock);
        return existing;
    }
    for (slot = s_htab_free_hint; slot < s_htab_len; slot++)
        if (!s_htab[slot]) break;
    if (slot == s_htab_len) {
        uint32_t grown = s_htab_len ? s_htab_len * 2 : 256;
        w32_object **t = (w32_object **)realloc(s_htab, grown * sizeof *t);
        if (!t) { pthread_mutex_unlock(&s_htab_lock); return NULL; }
        memset(t + s_htab_len, 0, (grown - s_htab_len) * sizeof *t);
        s_htab = t;
        s_htab_len = grown;
    }
    s_htab[slot] = o;
    s_htab_free_hint = slot + 1;
    o->handle_id = HTAB_TAG | ((slot + 1) * HTAB_STRIDE);
    h = (HANDLE)(uintptr_t)o->handle_id;
    pthread_mutex_unlock(&s_htab_lock);
    return h;
}

/* The inverse. NULL for anything that is not a live handle, which is what
 * callers already expect from a bad one. */
static w32_object *obj_from_handle(HANDLE h)
{
    uintptr_t v = (uintptr_t)h;
    uint32_t slot;
    w32_object *o;

    if ((v & ~(uintptr_t)0xFFFFFFFFu) != 0) return NULL;   /* not a handle id */
    if (((uint32_t)v & ~HTAB_MASK) != HTAB_TAG) return NULL;
    v &= HTAB_MASK;
    if (!v || v % HTAB_STRIDE) return NULL;
    slot = (uint32_t)(v / HTAB_STRIDE) - 1;
    pthread_mutex_lock(&s_htab_lock);
    o = (slot < s_htab_len) ? s_htab[slot] : NULL;
    pthread_mutex_unlock(&s_htab_lock);
    return o;
}

static void obj_unregister(w32_object *o)
{
    uint32_t slot;

    if (!o || !o->handle_id) return;
    pthread_mutex_lock(&s_htab_lock);
    slot = (o->handle_id & HTAB_MASK) / HTAB_STRIDE - 1;
    if (slot < s_htab_len && s_htab[slot] == o) {
        s_htab[slot] = NULL;
        if (slot < s_htab_free_hint) s_htab_free_hint = slot;
    }
    o->handle_id = 0;
    pthread_mutex_unlock(&s_htab_lock);
}
#define STILL_ACTIVE 259u

static __thread w32_object *t_self_obj = NULL;
static __thread DWORD       t_tid      = 0;
/* The thread object of a thread CreateThread did not start, made by
 * w32_thread_apc_handle so that its alertable waits can be woken by a
 * QueueUserAPC from another thread (XBA-133). Never released. */
static __thread w32_object *t_adopted  = NULL;

/* The object APCs for this thread are queued on. */
static w32_object *self_apc_obj(void)
{
    return t_self_obj ? t_self_obj : t_adopted;
}
static volatile LONG        s_next_tid = 1000;

DWORD GetCurrentThreadId(void)
{
    if (t_tid == 0)
        t_tid = (DWORD)InterlockedIncrement(&s_next_tid);
    return t_tid;
}

DWORD GetCurrentProcessId(void) { return (DWORD)getpid(); }

/* errno -> the Win32 code a caller would test for. Only the handful that
 * callers actually branch on; everything else is a generic failure. */
static DWORD w32_error_from_errno(int e)
{
    switch (e) {
    case ENOENT:  return ERROR_FILE_NOT_FOUND;
    case EEXIST:  return ERROR_ALREADY_EXISTS;
    case EACCES:
    case EPERM:   return ERROR_ACCESS_DENIED;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case ENOTEMPTY: return ERROR_DIR_NOT_EMPTY;
    default:      return ERROR_GEN_FAILURE;
    }
}

/* ── Directories and the temp path ──────────────────────────────────────
 *
 * Thin enough to be obviously right. A test or a title that wants a
 * scratch directory should not have to bracket itself in #ifdef to get one.
 */
/* Win32 accepts either separator, and callers mix them freely -- the kernel
 * tests build paths as "<posix dir>\\Child". Without this the backslash is
 * just another filename character on POSIX, so the call quietly operates on a
 * sibling with a backslash in its name instead of the child it named. */
static const char *norm_path(const char *in, char *buf, size_t n)
{
    snprintf(buf, n, "%s", in);
    xbox_path_normalize(buf);
    return buf;
}

BOOL CreateDirectoryA(LPCSTR path, LPSECURITY_ATTRIBUTES sa)
{
    (void)sa;
    if (!path) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    char norm[1024];
    if (mkdir(norm_path(path, norm, sizeof norm), 0777) == 0)
        return TRUE;
    /* Win32 reports an existing directory as a failure with this code, and
     * callers test for it rather than treating every failure as fatal. */
    SetLastError(errno == EEXIST ? ERROR_ALREADY_EXISTS
                                 : w32_error_from_errno(errno));
    return FALSE;
}

BOOL RemoveDirectoryA(LPCSTR path)
{
    if (!path) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    char norm[1024];
    if (rmdir(norm_path(path, norm, sizeof norm)) == 0) return TRUE;
    SetLastError(w32_error_from_errno(errno));
    return FALSE;
}

/* Win32 returns the length written and guarantees a trailing separator. */
DWORD GetTempPathA(DWORD buflen, LPSTR buf)
{
    const char *tmp = getenv("TMPDIR");
    size_t n;

    if (!tmp || !*tmp) tmp = "/tmp";
    n = strlen(tmp);
    if (!buf || buflen < n + 2)
        return (DWORD)(n + 2);          /* needed size, including the '/' */
    memcpy(buf, tmp, n);
    if (n && buf[n - 1] != '/') buf[n++] = '/';
    buf[n] = 0;
    return (DWORD)n;
}

DWORD GetFileAttributesA(LPCSTR path)
{
    struct stat st;
    char norm[1024];

    if (!path || stat(norm_path(path, norm, sizeof norm), &st) != 0) {
        SetLastError(w32_error_from_errno(path ? errno : EINVAL));
        return INVALID_FILE_ATTRIBUTES;
    }
    return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY
                               : FILE_ATTRIBUTE_NORMAL;
}

BOOL DeleteFileA(LPCSTR path)
{
    if (!path) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    char norm[1024];
    if (unlink(norm_path(path, norm, sizeof norm)) == 0) return TRUE;
    SetLastError(w32_error_from_errno(errno));
    return FALSE;
}
HANDLE GetCurrentThread(void)   { return t_self_obj ? obj_handle(t_self_obj) : PSEUDO_CURRENT_THREAD; }
HANDLE GetCurrentProcess(void)  { return PSEUDO_CURRENT_PROCESS; }

static w32_object *obj_alloc(w32_kind kind)
{
    w32_object *o = (w32_object *)calloc(1, sizeof(w32_object));
    o->kind     = kind;
    o->refcount = 1;
    pthread_mutex_init(&o->lock, NULL);
    cond_init_wait_clock(&o->cond);
    cond_init_wait_clock(&o->gate);
    return o;
}

static void obj_release(w32_object *o)
{
    /* obj_from_handle() returns NULL for a handle the table never issued --
     * a stale or invented guest value. Closing one is a no-op, not a fault. */
    if (!o) return;
    if (InterlockedDecrement(&o->refcount) > 0)
        return;
    if (o->kind == K_FILE) {
        if (o->fd >= 0) close(o->fd);
        free(o->file_path);
        if (o->file_ext && o->file_ext_free)
            o->file_ext_free(o->file_ext);
    } else if (o->kind == K_FILEMAP) {
        if (o->fd >= 0) close(o->fd);
    } else if (o->kind == K_WAITABLE_TIMER) {
        /* Waitable timers: no special cleanup needed */
    }
    /* Before the memory goes: a stale slot would hand a later caller a
     * dangling pointer under a handle that looks live. */
    obj_unregister(o);
    pthread_mutex_destroy(&o->lock);
    pthread_cond_destroy(&o->cond);
    pthread_cond_destroy(&o->gate);
    free(o);
}

/* ---- fd-backed file handle (for the file-I/O HLE) -------------------- */
HANDLE w32_open_handle(int fd, const char *host_path)
{
    w32_object *o = obj_alloc(K_FILE);
    o->fd        = fd;
    o->file_path = host_path ? strdup(host_path) : NULL;
    return obj_handle(o);
}

HANDLE w32_open_handle_ext(int fd, const char *host_path, void *ext, void (*ext_free)(void *))
{
    HANDLE h = w32_open_handle(fd, host_path);
    w32_object *o = obj_from_handle(h);
    if (o) {
        o->file_ext = ext;
        o->file_ext_free = ext_free;
    }
    return h;
}

void *w32_handle_ext(HANDLE h)
{
    w32_object *o = obj_from_handle(h);
    return (o && o->kind == K_FILE) ? o->file_ext : NULL;
}

int w32_handle_fd(HANDLE h)
{
    w32_object *o = obj_from_handle(h);
    return (o && o->kind == K_FILE) ? o->fd : -1;
}

const char *w32_handle_path(HANDLE h)
{
    w32_object *o = obj_from_handle(h);
    return (o && o->kind == K_FILE) ? o->file_path : NULL;
}

void w32_handle_set_path(HANDLE h, const char *host_path)
{
    w32_object *o = obj_from_handle(h);
    char *old;

    if (!o || o->kind != K_FILE)
        return;
    old = o->file_path;
    o->file_path = host_path ? strdup(host_path) : NULL;
    free(old);
}

#define W32_FD_LIMIT_CAP 65536

/* Raise the soft open-file limit to the hard one, at most W32_FD_LIMIT_CAP.
 *
 * A title may keep a file open for every entry of its Z: cache, as the
 * console's kernel has no per-process limit: one title keeps 363 open for
 * a full cache. macOS gives an app launched from Finder a soft limit of
 * 256, so that title's first boot failed its 247th open (EMFILE). There the
 * hard limit is usually RLIM_INFINITY, which setrlimit may refuse for
 * RLIMIT_NOFILE and which the kernel caps at kern.maxfilesperproc anyway, so
 * the target is that, then OPEN_MAX if even that is refused. Linux hard
 * limits from systemd run from 524288 to about 2^30 in containers, and the
 * raised limit reaches every child too, where select() (FD_SETSIZE) and fd
 * sweeps up to the limit break or crawl. A title needs a few hundred, so the
 * target is capped. Returns 1 when the limit changed, 0 when it was already
 * there, -1 on failure; *after is the limit in force, read back. */
int w32_raise_fd_limit(unsigned long long *before, unsigned long long *after)
{
    struct rlimit rl;
    rlim_t was, want;

    /* 0 until read, so a failed getrlimit is told apart from a limit. */
    if (before) *before = 0;
    if (after)  *after  = 0;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0)
        return -1;
    was = rl.rlim_cur;
    if (before) *before = (unsigned long long)was;
    if (after)  *after  = (unsigned long long)was;
    want = rl.rlim_max;
    if (want == RLIM_INFINITY || want > W32_FD_LIMIT_CAP)
        want = W32_FD_LIMIT_CAP;
#if defined(__APPLE__)
    {
        int per_proc = 0;
        size_t len = sizeof per_proc;
        if (sysctlbyname("kern.maxfilesperproc", &per_proc, &len, NULL, 0) == 0
                && per_proc > 0 && (rlim_t)per_proc < want)
            want = (rlim_t)per_proc;
    }
#endif
    if (want <= was)
        return 0;
    rl.rlim_cur = want;
    if (setrlimit(RLIMIT_NOFILE, &rl) != 0) {
#if defined(__APPLE__)
        rl.rlim_cur = OPEN_MAX < rl.rlim_max ? OPEN_MAX : rl.rlim_max;
        if (rl.rlim_cur <= was || setrlimit(RLIMIT_NOFILE, &rl) != 0)
            return -1;
#else
        return -1;
#endif
    }
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0)
        return -1;
    if (after) *after = (unsigned long long)rl.rlim_cur;
    return rl.rlim_cur > was ? 1 : -1;
}

BOOL CloseHandle(HANDLE h)
{
    if (!h || h == PSEUDO_CURRENT_THREAD || h == PSEUDO_CURRENT_PROCESS ||
        h == INVALID_HANDLE_VALUE)
        return TRUE;
    obj_release(obj_from_handle(h));
    return TRUE;
}

BOOL DuplicateHandle(HANDLE srcProc, HANDLE src, HANDLE dstProc, PHANDLE dst,
                     DWORD access, BOOL inherit, DWORD options)
{
    (void)srcProc; (void)dstProc; (void)access; (void)inherit;
    if (!dst) return FALSE;
    if (src == PSEUDO_CURRENT_THREAD)  src = GetCurrentThread();
    if (src == PSEUDO_CURRENT_PROCESS) { *dst = src; return TRUE; }
    if (src == PSEUDO_CURRENT_THREAD || !src) { *dst = src; return TRUE; }
    w32_object *o = obj_from_handle(src);
    if (!o) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    InterlockedIncrement(&o->refcount);
    *dst = src;
    if (options & DUPLICATE_CLOSE_SOURCE)
        obj_release(o);
    return TRUE;
}

/* The object's state changed: wake the threads waiting on it alone and the
 * multiple-object waiters linked to it. Caller holds o->lock. */
static void obj_wake(w32_object *o)
{
    w32_waitlink *l;
    pthread_cond_broadcast(&o->cond);
    for (l = o->mw_list; l; l = l->next) {
        pthread_mutex_lock(&l->wb->m);
        l->wb->seq++;
        pthread_cond_signal(&l->wb->c);
        pthread_mutex_unlock(&l->wb->m);
    }
}

/* ---- deadline helper -------------------------------------------------- */
static void deadline_from_ms(DWORD ms, struct timespec *ts)
{
    clock_gettime(W32_WAIT_CLOCK, ts);
    ts->tv_sec  += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}

/* Run any pending user-APCs for the calling thread. Returns count run. */
static int drain_apcs(void)
{
    w32_object *o = self_apc_obj();
    int run = 0;
    if (!o) return 0;
    pthread_mutex_lock(&o->lock);
    while (o->apc_count > 0) {
        PAPCFUNC  f = o->apc_func[0];
        ULONG_PTR d = o->apc_data[0];
        memmove(o->apc_func, o->apc_func + 1, sizeof(PAPCFUNC) * (o->apc_count - 1));
        memmove(o->apc_data, o->apc_data + 1, sizeof(ULONG_PTR) * (o->apc_count - 1));
        o->apc_count--;
        pthread_mutex_unlock(&o->lock);
        f(d);
        run++;
        pthread_mutex_lock(&o->lock);
    }
    pthread_mutex_unlock(&o->lock);
    return run;
}

static int timespec_before(const struct timespec *a, const struct timespec *b)
{
    return a->tv_sec != b->tv_sec ? a->tv_sec < b->tv_sec : a->tv_nsec < b->tv_nsec;
}

/* Signalled once the due time passes; latched, so a manual-reset timer stays
 * signalled until it is set or cancelled again. Caller holds o->lock. */
static int waitable_due(w32_object *o)
{
    struct timespec now;
    if (o->waitable_triggered) return 1;
    if (!o->waitable_armed) return 0;
    clock_gettime(W32_WAIT_CLOCK, &now);
    if (timespec_before(&now, &o->waitable_due_time)) return 0;
    o->waitable_triggered = 1;
    return 1;
}

/*
 * Wait on a single object. The object lock must NOT be held.
 * Returns WAIT_OBJECT_0 / WAIT_TIMEOUT.
 *
 * Events: NT satisfies a wait at the moment the event is set. A thread
 * blocked here when SetEvent or PulseEvent runs is released by that call,
 * even if the event is reset before the thread is scheduled again. Looking at
 * the signaled flag alone, a second waiter that ran first and reset the event
 * sent this one back to sleep: XDK code clears a notification event by hand
 * and waits again (BlockUntilVerticalBlank, inlined), and a title whose main
 * thread and stream thread both did so for the vblank event lost a wake-up at
 * almost every vblank on one core (Black: 2-3 frames a second while loading).
 * So a blocked waiter is counted in ev_waiters; a manual-reset set bumps
 * ev_epoch, which every counted waiter sees; an auto-reset set with counted
 * waiters hands one grant (ev_grants) to one of them and leaves the event
 * unsignalled. A grant is taken before the signaled flag, and a waiter whose
 * time ran out looks once more, so no grant is left behind with no waiter
 * for it. A zero timeout is a poll: it never counts as a waiter.
 */
/* Whether a waiter may take o now. counted: the waiter is one of
 * ev_waiters and saw ev_epoch == epoch when it started; a release meant for
 * the blocked waiters (epoch moved, grant pending) sets *granted. Caller
 * holds o->lock. */
static int obj_ready_locked(w32_object *o, int counted, unsigned epoch, int *granted)
{
    *granted = 0;
    switch (o->kind) {
    case K_EVENT:
        if (counted && (o->manual_reset ? o->ev_epoch != epoch : o->ev_grants > 0))
            return *granted = 1;
        return o->signaled;
    case K_THREAD: return o->exited;
    case K_SEM:    return o->sem_count > 0;
    case K_MUTEX:  return o->mtx_owner == 0 || o->mtx_owner == GetCurrentThreadId();
    case K_WAITABLE_TIMER: return waitable_due(o);
    default:       return 1;
    }
}

/* A satisfied wait takes what it consumes. Caller holds o->lock. */
static void obj_consume_locked(w32_object *o, int granted)
{
    switch (o->kind) {
    case K_EVENT:
        if (granted) { if (!o->manual_reset) o->ev_grants--; }
        else if (!o->manual_reset) o->signaled = 0;
        break;
    case K_WAITABLE_TIMER:
        if (!o->waitable_manual_reset && o->waitable_period_ms > 0) {
            /* Periodic synchronization timer: the next expiry is one
             * period after the last due time (on the grid, catching up
             * if the waiter was late), and it stays armed. */
            struct timespec now;
            clock_gettime(W32_WAIT_CLOCK, &now);
            do {
                long long ns = o->waitable_due_time.tv_nsec + (long long)o->waitable_period_ms * 1000000LL;
                o->waitable_due_time.tv_sec += (time_t)(ns / 1000000000LL);
                o->waitable_due_time.tv_nsec = (long)(ns % 1000000000LL);
            } while (!timespec_before(&now, &o->waitable_due_time));
            o->waitable_triggered = 0;
        } else if (!o->waitable_manual_reset) {
            o->waitable_triggered = 0; o->waitable_armed = 0;
        }
        break;
    case K_SEM:   o->sem_count--; break;
    case K_MUTEX: o->mtx_owner = GetCurrentThreadId(); o->mtx_recursion++; break;
    default: break;
    }
}

static DWORD wait_single(w32_object *o, DWORD ms)
{
    struct timespec ts;
    int timed = (ms != INFINITE);
    if (timed) deadline_from_ms(ms, &ts);

    pthread_mutex_lock(&o->lock);
    DWORD result = WAIT_OBJECT_0;
    unsigned epoch = o->ev_epoch;
    int counted = 0, granted = 0, expired = 0;

    for (;;) {
        if (obj_ready_locked(o, counted, epoch, &granted)) break;
        if (expired || (timed && ms == 0)) { result = WAIT_TIMEOUT; break; }
        if (o->kind == K_EVENT && !counted) { counted = 1; o->ev_waiters++; }

        /* An armed timer has its own deadline. Waiting on the caller's alone
         * would sleep straight past the due time, so take whichever comes
         * first and re-test. */
        struct timespec until = ts;
        int bounded = timed;
        if (o->kind == K_WAITABLE_TIMER && o->waitable_armed &&
            (!timed || timespec_before(&o->waitable_due_time, &ts))) {
            until = o->waitable_due_time;
            bounded = 1;
        }
        int rc = bounded ? pthread_cond_timedwait(&o->cond, &o->lock, &until)
                         : pthread_cond_wait(&o->cond, &o->lock);
        if (rc == ETIMEDOUT && timed && !timespec_before(&until, &ts))
            expired = 1;            /* one more look: a release may have come meanwhile */
    }
    if (counted) o->ev_waiters--;

    if (result == WAIT_OBJECT_0)
        obj_consume_locked(o, granted);
    pthread_mutex_unlock(&o->lock);
    return result;
}

DWORD WaitForSingleObject(HANDLE h, DWORD ms)
{
    if (!h || h == PSEUDO_CURRENT_THREAD || h == PSEUDO_CURRENT_PROCESS)
        return WAIT_OBJECT_0;
    w32_object *o = obj_from_handle(h);
    if (!o) { SetLastError(ERROR_INVALID_HANDLE); return WAIT_FAILED; }
    return wait_single(o, ms);
}

/* Alertable: an APC queued while the thread is blocked ends the wait
 * (WAIT_IO_COMPLETION), as on Win32 -- the multiple-object wait links the
 * thread's own object for that. It used to look for APCs only before
 * blocking (XBA-133). */
DWORD WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable)
{
    if (alertable && drain_apcs() > 0)
        return WAIT_IO_COMPLETION;
    if (alertable && self_apc_obj() && h && obj_from_handle(h))
        return WaitForMultipleObjectsEx(1, &h, FALSE, ms, TRUE);
    return WaitForSingleObject(h, ms);
}

DWORD WaitForMultipleObjects(DWORD count, const HANDLE *handles, BOOL waitAll, DWORD ms)
{
    return WaitForMultipleObjectsEx(count, handles, waitAll, ms, FALSE);
}

/* A handle the table knows, or one of the pseudo-handles. NULL is not one:
 * the single-object wait answers it as signalled, a convenience for callers
 * that pass an optional event, but Win32 rejects the whole multiple wait for
 * it -- and the kernel's own regression pins that, because a NULL slot means
 * the title named an object the runtime did not resolve. */
static int wait_handle_ok(HANDLE h)
{
    if (h == PSEUDO_CURRENT_THREAD || h == PSEUDO_CURRENT_PROCESS)
        return 1;
    return obj_from_handle(h) != NULL;
}

/* One multiple-object wait in progress. obj[i] is NULL for a pseudo-handle,
 * which is always signalled; uniq[] is each object once, by address, for
 * wait-all's locking. */
typedef struct {
    DWORD        count, nuniq;
    BOOL         wait_all;
    w32_object  *obj[MAXIMUM_WAIT_OBJECTS];
    w32_object  *uniq[MAXIMUM_WAIT_OBJECTS];
    unsigned     epoch[MAXIMUM_WAIT_OBJECTS];
    int          counted[MAXIMUM_WAIT_OBJECTS];
    int          has_due;           /* an armed waitable timer: its due time */
    struct timespec due;
} w32_multiwait;

static void multi_note_due(w32_multiwait *w, w32_object *o)
{
    if (o->kind == K_WAITABLE_TIMER && o->waitable_armed && !o->waitable_triggered &&
        (!w->has_due || timespec_before(&o->waitable_due_time, &w->due))) {
        w->due = o->waitable_due_time;
        w->has_due = 1;
    }
}

/* Wait-any: the lowest index that may be taken now, taken; or -1. */
static int multi_take_any(w32_multiwait *w)
{
    DWORD i;
    w->has_due = 0;
    for (i = 0; i < w->count; i++) {
        w32_object *o = w->obj[i];
        int granted;
        if (!o) return (int)i;
        pthread_mutex_lock(&o->lock);
        if (obj_ready_locked(o, w->counted[i], w->epoch[i], &granted)) {
            obj_consume_locked(o, granted);
            pthread_mutex_unlock(&o->lock);
            return (int)i;
        }
        multi_note_due(w, o);
        pthread_mutex_unlock(&o->lock);
    }
    return -1;
}

/* Wait-all: every object at once, under all their locks, or none. A wait-all
 * waiter is never counted on an event (a set while it waits for the others
 * stays a set others can see); a manual-reset event set or pulsed since the
 * last look counts as signalled for this look. */
static int multi_take_all(w32_multiwait *w)
{
    DWORD i, j;
    int all = 1;
    w->has_due = 0;
    for (j = 0; j < w->nuniq; j++)
        pthread_mutex_lock(&w->uniq[j]->lock);
    for (i = 0; i < w->count && all; i++) {
        w32_object *o = w->obj[i];
        int granted;
        if (o && !obj_ready_locked(o, o->kind == K_EVENT && o->manual_reset, w->epoch[i], &granted))
            all = 0;
    }
    for (j = 0; j < w->nuniq; j++) {
        w32_object *o = w->uniq[j];
        if (all)
            obj_consume_locked(o, 0);
        else
            multi_note_due(w, o);
    }
    for (i = 0; i < w->count; i++)
        if (w->obj[i])
            w->epoch[i] = w->obj[i]->ev_epoch;
    for (j = w->nuniq; j-- > 0; )
        pthread_mutex_unlock(&w->uniq[j]->lock);
    return all ? 0 : -1;
}

static int multi_take(w32_multiwait *w)
{
    return w->wait_all ? multi_take_all(w) : multi_take_any(w);
}

static void link_add(w32_object *o, w32_waitlink *l, w32_waitblock *wb)
{
    l->wb = wb;
    l->next = o->mw_list;
    o->mw_list = l;
}

static void link_remove(w32_object *o, w32_waitlink *l)
{
    w32_waitlink **pp;
    for (pp = &o->mw_list; *pp; pp = &(*pp)->next)
        if (*pp == l) { *pp = l->next; break; }
}

static int ptr_cmp(const void *a, const void *b)
{
    uintptr_t x = (uintptr_t)*(w32_object *const *)a, y = (uintptr_t)*(w32_object *const *)b;
    return x < y ? -1 : x > y;
}

/*
 * Blocks until one object (wait-any) or every object at once (wait-all) can
 * be taken. It used to poll every object with a zero timeout and sleep 1 ms
 * between rounds: a pulse, or a set another thread reset within the
 * millisecond, released nobody, and every wait cost a wake-up per
 * millisecond. Now the waiting thread links one wait block of its own into
 * each object; whatever changes an object's state (set, pulse, release,
 * thread exit, timer arm, an APC for an alertable wait) bumps the block, and
 * the thread looks again.
 *
 * Wait-any takes the semantics of wait_single: it counts as a waiter on each
 * event, so a manual-reset set or pulse while it is blocked releases it (the
 * epoch moved) and an auto-reset one may hand it the grant -- which it takes,
 * consuming that one object only. Grants left on other events when it
 * returns, more than their remaining waiters, go back to the signalled flag:
 * on NT those events would simply have stayed signalled. Wait-all takes
 * every object under all their locks (address order), so it is atomic; it is
 * not counted on events, so an auto-reset pulse while it waits for the other
 * objects is not seen, and a manual-reset one counts as signalled at its next
 * look. Ties go to the lowest index, as on Win32.
 */
DWORD WaitForMultipleObjectsEx(DWORD count, const HANDLE *handles, BOOL waitAll,
                               DWORD ms, BOOL alertable)
{
    struct timespec ts;
    int timed = (ms != INFINITE), r, expired = 0;
    DWORD i, j, result;
    w32_multiwait w;
    w32_waitblock wb;
    w32_waitlink links[MAXIMUM_WAIT_OBJECTS + 1];
    w32_object *self = alertable ? self_apc_obj() : NULL;

    /* Win32 rejects the whole call when any handle is not one, before waiting
     * on any of them. Polling each with a zero timeout instead never reported
     * a bad handle as anything but "not signalled", so the call used to sit
     * out its timeout and answer STATUS_TIMEOUT where a title expects
     * STATUS_UNSUCCESSFUL. A title that reads a timeout as "not yet" retries
     * forever. */
    if (count == 0 || count > MAXIMUM_WAIT_OBJECTS)
        { SetLastError(ERROR_INVALID_PARAMETER); return WAIT_FAILED; }
    if (!handles)
        { SetLastError(ERROR_INVALID_HANDLE); return WAIT_FAILED; }
    for (i = 0; i < count; i++)
        if (!wait_handle_ok(handles[i]))
            { SetLastError(ERROR_INVALID_HANDLE); return WAIT_FAILED; }

    if (alertable && drain_apcs() > 0)
        return WAIT_IO_COMPLETION;
    if (timed) deadline_from_ms(ms, &ts);

    memset(&w, 0, sizeof w);
    w.count = count;
    w.wait_all = waitAll;
    for (i = 0; i < count; i++) {
        HANDLE h = handles[i];
        w32_object *o = (h == PSEUDO_CURRENT_THREAD || h == PSEUDO_CURRENT_PROCESS)
                        ? NULL : obj_from_handle(h);
        if (o) InterlockedIncrement(&o->refcount);   /* stays alive while we wait */
        w.obj[i] = o;
        if (o) w.uniq[w.nuniq++] = o;
    }
    qsort(w.uniq, w.nuniq, sizeof w.uniq[0], ptr_cmp);
    for (i = j = 0; i < w.nuniq; i++)
        if (!j || w.uniq[j - 1] != w.uniq[i])
            w.uniq[j++] = w.uniq[i];
    w.nuniq = j;
    for (i = 0; i < count; i++)
        if (w.obj[i]) {
            pthread_mutex_lock(&w.obj[i]->lock);
            w.epoch[i] = w.obj[i]->ev_epoch;   /* wait-all: a pulse from here on counts */
            pthread_mutex_unlock(&w.obj[i]->lock);
        }

    r = multi_take(&w);
    if (r >= 0 || (timed && ms == 0)) {
        result = r >= 0 ? WAIT_OBJECT_0 + (DWORD)r : WAIT_TIMEOUT;
        goto done;
    }

    /* Block: link the wait block into every object (and our own thread, for
     * an APC), counting as a waiter on each event for wait-any. */
    pthread_mutex_init(&wb.m, NULL);
    cond_init_wait_clock(&wb.c);
    wb.seq = 0;
    for (i = 0; i < count; i++) {
        w32_object *o = w.obj[i];
        if (!o) continue;
        pthread_mutex_lock(&o->lock);
        link_add(o, &links[i], &wb);
        if (!waitAll && o->kind == K_EVENT) {
            w.counted[i] = 1;
            w.epoch[i] = o->ev_epoch;
            o->ev_waiters++;
        }
        pthread_mutex_unlock(&o->lock);
    }
    if (self) {
        pthread_mutex_lock(&self->lock);
        link_add(self, &links[count], &wb);
        pthread_mutex_unlock(&self->lock);
    }

    for (;;) {
        unsigned seq;
        struct timespec until = ts;
        int bounded = timed, rc = 0;

        pthread_mutex_lock(&wb.m);
        seq = wb.seq;                /* before looking: a change after it wakes us */
        pthread_mutex_unlock(&wb.m);
        if (self && drain_apcs() > 0) { result = WAIT_IO_COMPLETION; break; }
        r = multi_take(&w);
        if (r >= 0) { result = WAIT_OBJECT_0 + (DWORD)r; break; }
        if (expired) { result = WAIT_TIMEOUT; break; }
        if (w.has_due && (!timed || timespec_before(&w.due, &ts))) {
            until = w.due;           /* an armed timer comes due on its own */
            bounded = 1;
        }
        pthread_mutex_lock(&wb.m);
        while (wb.seq == seq && rc != ETIMEDOUT)
            rc = bounded ? pthread_cond_timedwait(&wb.c, &wb.m, &until)
                         : pthread_cond_wait(&wb.c, &wb.m);
        pthread_mutex_unlock(&wb.m);
        if (rc == ETIMEDOUT && timed && !timespec_before(&until, &ts))
            expired = 1;             /* one more look: a release may have come meanwhile */
    }

    for (i = 0; i < count; i++) {
        w32_object *o = w.obj[i];
        if (!o) continue;
        pthread_mutex_lock(&o->lock);
        link_remove(o, &links[i]);
        if (w.counted[i]) {
            o->ev_waiters--;
            if (!o->manual_reset && o->ev_grants > o->ev_waiters) {
                /* A grant handed to us that we did not take: the set stays. */
                o->ev_grants = o->ev_waiters;
                o->signaled = 1;
                obj_wake(o);
            }
        }
        pthread_mutex_unlock(&o->lock);
    }
    if (self) {
        pthread_mutex_lock(&self->lock);
        link_remove(self, &links[count]);
        pthread_mutex_unlock(&self->lock);
    }
    pthread_cond_destroy(&wb.c);
    pthread_mutex_destroy(&wb.m);

done:
    for (i = 0; i < count; i++)
        if (w.obj[i]) obj_release(w.obj[i]);
    return result;
}

/* ===================================================================== */
/* Events                                                                */
/* ===================================================================== */

HANDLE CreateEventA(LPSECURITY_ATTRIBUTES sa, BOOL manualReset, BOOL initialState, LPCSTR name)
{
    (void)sa; (void)name;
    w32_object *o = obj_alloc(K_EVENT);
    o->manual_reset = manualReset ? 1 : 0;
    o->signaled     = initialState ? 1 : 0;
    return obj_handle(o);
}
HANDLE CreateEventW(LPSECURITY_ATTRIBUTES sa, BOOL manualReset, BOOL initialState, LPCWSTR name)
{
    (void)name;
    return CreateEventA(sa, manualReset, initialState, NULL);
}

unsigned w32_event_waiters(HANDLE h)
{
    w32_object *o = obj_from_handle(h);
    unsigned n = 0;
    if (o && o->kind == K_EVENT) {
        pthread_mutex_lock(&o->lock);
        n = o->ev_waiters;
        pthread_mutex_unlock(&o->lock);
    }
    return n;
}

static void event_set_locked(w32_object *o)
{
    if (o->manual_reset) {
        if (!o->signaled)
            o->ev_epoch++;          /* releases every thread blocked now */
        o->signaled = 1;
    } else if (!o->signaled && !o->ev_grants && o->ev_waiters) {
        o->ev_grants = 1;           /* straight to one blocked thread; stays unsignalled */
    } else if (!o->ev_grants) {
        o->signaled = 1;
    }
    /* With a grant not taken yet the set changes nothing, as setting an event
     * that is already signalled changes nothing. The kernel mirrors a guest
     * KEVENT's SignalState into its host event before each wait (SetEvent when
     * it reads 1): a KeSetEvent that went to a blocked thread as a grant would
     * otherwise release a second thread when the next one prepares its wait. */
    obj_wake(o);
}

BOOL SetEvent(HANDLE h)
{
    w32_object *o = obj_from_handle(h);
    if (!o || o->kind != K_EVENT) return FALSE;
    pthread_mutex_lock(&o->lock);
    event_set_locked(o);
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

BOOL w32_event_set_n(HANDLE h, int n)
{
    w32_object *o = obj_from_handle(h);
    if (!o || o->kind != K_EVENT) return FALSE;
    pthread_mutex_lock(&o->lock);
    while (n-- > 0)
        event_set_locked(o);
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

BOOL ResetEvent(HANDLE h)
{
    w32_object *o = obj_from_handle(h);
    if (!o || o->kind != K_EVENT) return FALSE;
    pthread_mutex_lock(&o->lock);
    o->signaled = 0;
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

BOOL PulseEvent(HANDLE h)
{
    w32_object *o = obj_from_handle(h);
    if (!o || o->kind != K_EVENT) return FALSE;
    pthread_mutex_lock(&o->lock);
    /* Releases the threads blocked now (manual reset), or one of them (auto
     * reset), and leaves the event unsignalled. Setting the flag and clearing
     * it under the lock released nobody: the waiters woke to a clear flag. */
    if (o->manual_reset)
        o->ev_epoch++;
    else if (!o->signaled && o->ev_waiters > o->ev_grants)
        o->ev_grants++;
    o->signaled = 0;
    obj_wake(o);
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

/* ===================================================================== */
/* Semaphores                                                            */
/* ===================================================================== */

HANDLE CreateSemaphoreA(LPSECURITY_ATTRIBUTES sa, LONG initial, LONG maximum, LPCSTR name)
{
    (void)sa; (void)name;
    w32_object *o = obj_alloc(K_SEM);
    o->sem_count = initial;
    o->sem_max   = maximum;
    return obj_handle(o);
}
HANDLE CreateSemaphoreW(LPSECURITY_ATTRIBUTES sa, LONG initial, LONG maximum, LPCWSTR name)
{
    (void)name;
    return CreateSemaphoreA(sa, initial, maximum, NULL);
}

BOOL ReleaseSemaphore(HANDLE h, LONG releaseCount, PLONG previousCount)
{
    w32_object *o = obj_from_handle(h);
    if (!o || o->kind != K_SEM) return FALSE;
    pthread_mutex_lock(&o->lock);
    if (previousCount) *previousCount = (LONG)o->sem_count;
    o->sem_count += releaseCount;
    if (o->sem_max && o->sem_count > o->sem_max) o->sem_count = o->sem_max;
    obj_wake(o);
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

/* ===================================================================== */
/* Mutexes                                                               */
/* ===================================================================== */

HANDLE CreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL initialOwner, LPCSTR name)
{
    (void)sa; (void)name;
    w32_object *o = obj_alloc(K_MUTEX);
    if (initialOwner) { o->mtx_owner = GetCurrentThreadId(); o->mtx_recursion = 1; }
    return obj_handle(o);
}
HANDLE CreateMutexW(LPSECURITY_ATTRIBUTES sa, BOOL initialOwner, LPCWSTR name)
{
    (void)name;
    return CreateMutexA(sa, initialOwner, NULL);
}

BOOL ReleaseMutex(HANDLE h)
{
    w32_object *o = obj_from_handle(h);
    if (!o || o->kind != K_MUTEX) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    pthread_mutex_lock(&o->lock);
    if (o->mtx_owner != GetCurrentThreadId()) {
        pthread_mutex_unlock(&o->lock);
        SetLastError(ERROR_NOT_OWNER);
        return FALSE;
    }
    if (--o->mtx_recursion <= 0) {
        o->mtx_owner = 0;
        o->mtx_recursion = 0;
        obj_wake(o);
    }
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

int w32_query_object(HANDLE h, LONG info[3])
{
    w32_object *o = obj_from_handle(h);
    int kind = 0;
    if (!o || !info) return 0;
    pthread_mutex_lock(&o->lock);
    switch (o->kind) {
    case K_EVENT:
        kind = W32_QUERY_EVENT;
        info[0] = o->manual_reset ? 0 : 1;
        info[1] = o->signaled ? 1 : 0;
        break;
    case K_SEM:
        kind = W32_QUERY_SEMAPHORE;
        info[0] = (LONG)o->sem_count;
        info[1] = (LONG)o->sem_max;
        break;
    case K_MUTEX:
        kind = W32_QUERY_MUTEX;
        info[0] = o->mtx_owner ? 1 - o->mtx_recursion : 1;
        info[1] = o->mtx_owner && o->mtx_owner == GetCurrentThreadId();
        break;
    case K_WAITABLE_TIMER: {
        long long left = 0;
        kind = W32_QUERY_TIMER;
        info[2] = waitable_due(o);
        if (o->waitable_armed && !info[2]) {
            struct timespec now;
            clock_gettime(W32_WAIT_CLOCK, &now);
            left = (long long)(o->waitable_due_time.tv_sec - now.tv_sec) * 10000000LL
                 + (o->waitable_due_time.tv_nsec - now.tv_nsec) / 100;
            if (left < 0) left = 0;
        }
        info[0] = (LONG)(uint32_t)left;
        info[1] = (LONG)(left >> 32);
        break;
    }
    default:
        break;
    }
    pthread_mutex_unlock(&o->lock);
    return kind;
}

int w32_state_read(HANDLE h, w32_state *out)
{
    w32_object *o = obj_from_handle(h);
    if (!o || !out) return 0;
    memset(out, 0, sizeof *out);
    out->fd = -1;
    pthread_mutex_lock(&o->lock);
    out->refcount = o->refcount;
    switch (o->kind) {
    case K_EVENT:
        out->kind = W32_STATE_EVENT;
        out->manual_reset = o->manual_reset;
        out->signaled = o->signaled;
        out->grants = o->ev_grants;
        out->waiters = o->ev_waiters;
        break;
    case K_SEM:
        out->kind = W32_STATE_SEMAPHORE;
        out->count = (LONG)o->sem_count;
        out->maximum = (LONG)o->sem_max;
        break;
    case K_MUTEX:
        out->kind = W32_STATE_MUTEX;
        out->owner_tid = o->mtx_owner;
        out->recursion = o->mtx_recursion;
        break;
    case K_THREAD:
        out->kind = W32_STATE_THREAD;
        out->tid = o->tid;
        out->exited = o->exited;
        out->exit_code = o->exit_code;
        out->suspend_count = o->suspend_count;
        out->priority = o->priority;
        out->apc_count = o->apc_count;
        break;
    case K_WAITABLE_TIMER:
        out->kind = W32_STATE_TIMER;
        out->manual_reset = o->waitable_manual_reset;
        out->signaled = o->waitable_triggered;
        out->armed = o->waitable_armed;
        out->period_ms = o->waitable_period_ms;
        if (o->waitable_armed && !o->waitable_triggered) {
            struct timespec now;
            long long left;
            clock_gettime(W32_WAIT_CLOCK, &now);
            left = (long long)(o->waitable_due_time.tv_sec - now.tv_sec) * 1000000000LL
                 + (o->waitable_due_time.tv_nsec - now.tv_nsec);
            out->due_in_ns = left > 0 ? left : 0;
        }
        break;
    case K_FILE:
        out->kind = W32_STATE_FILE;
        out->fd = o->fd;
        out->has_ext = o->file_ext != NULL;
        out->path = o->file_path;
        break;
    default:
        out->kind = W32_STATE_OTHER;
        break;
    }
    pthread_mutex_unlock(&o->lock);
    return out->kind;
}

HANDLE w32_state_make(const w32_state *s)
{
    w32_object *o;
    if (!s) return NULL;
    switch (s->kind) {
    case W32_STATE_EVENT:
        return CreateEventA(NULL, s->manual_reset, s->signaled, NULL);
    case W32_STATE_SEMAPHORE:
        return CreateSemaphoreA(NULL, s->count, s->maximum, NULL);
    case W32_STATE_MUTEX:
        o = obj_alloc(K_MUTEX);
        if (s->owner_tid && s->recursion > 0) {
            o->mtx_owner = s->owner_tid;
            o->mtx_recursion = s->recursion;
        }
        return obj_handle(o);
    case W32_STATE_TIMER:
        o = obj_alloc(K_WAITABLE_TIMER);
        o->waitable_manual_reset = s->manual_reset;
        o->waitable_armed = s->armed;
        o->waitable_triggered = s->signaled;
        o->waitable_period_ms = s->period_ms;
        clock_gettime(W32_WAIT_CLOCK, &o->waitable_due_time);
        if (s->armed && !s->signaled && s->due_in_ns > 0) {
            long long ns = o->waitable_due_time.tv_nsec + s->due_in_ns;
            o->waitable_due_time.tv_sec += (time_t)(ns / 1000000000LL);
            o->waitable_due_time.tv_nsec = (long)(ns % 1000000000LL);
        }
        return obj_handle(o);
    case W32_STATE_THREAD:
        if (!s->exited) return NULL;      /* a running thread is not made here */
        o = obj_alloc(K_THREAD);
        o->tid = s->tid;
        o->exited = 1;
        o->signaled = 1;
        o->exit_code = s->exit_code;
        o->priority = s->priority;
        return obj_handle(o);
    default:
        return NULL;
    }
}

/* ===================================================================== */
/* Threads                                                               */
/* ===================================================================== */

/* Win32 gives a new thread the process's CPU mask; pthread_create copies
 * the creator's. A guest thread pinned to one core (xbox_GuestThreadPin)
 * would otherwise hand that core to every host thread it creates, the
 * kernel timer thread from KeSetTimer among them. The mask is taken once,
 * from the first thread to ask, before any pin. */
static DWORD_PTR s_process_mask;

static DWORD_PTR process_mask_capture(void)
{
    DWORD_PTR mask = 0;
    if (s_process_mask)
        return s_process_mask;
#if defined(__linux__)
    {
        cpu_set_t set;
        int i;
        if (sched_getaffinity(0, sizeof set, &set) == 0)
            for (i = 0; i < (int)(8 * sizeof mask) && i < CPU_SETSIZE; i++)
                if (CPU_ISSET(i, &set))
                    mask |= (DWORD_PTR)1 << i;
    }
#endif
    if (!mask) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        if (n < 1) n = 1;
        if (n >= (long)(8 * sizeof mask)) n = (long)(8 * sizeof mask) - 1;
        mask = ((DWORD_PTR)1 << n) - 1;
    }
    s_process_mask = mask;
    return mask;
}

/* Host stack for threads that may run lifted code. Lifted code recurses on
 * the native stack, so the libc default is not enough: glibc gives 8 MB,
 * bionic about 1 MB, and a Win32 stackSize (64 KB for an Xbox kernel
 * thread) is only a commit size on Windows but the whole stack here. Every
 * thread the shim creates for a caller's routine gets at least this floor;
 * a larger request is kept. Stacks are committed lazily, so the floor costs
 * address space, not memory. RECOMP_THREAD_STACK_MB replaces the floor. */
#define W32_THREAD_STACK_FLOOR_MB 8

static size_t         s_stack_floor;
static pthread_once_t s_stack_floor_once = PTHREAD_ONCE_INIT;

static void stack_floor_init(void)
{
    unsigned long mb = W32_THREAD_STACK_FLOOR_MB;
    const char   *env = getenv("RECOMP_THREAD_STACK_MB");
    const char   *from = "default";

    if (env && *env) {
        char *end = NULL;
        unsigned long v = strtoul(env, &end, 10);
        if (end != env && *end == '\0' && v >= 1 && v <= 1024) {
            mb = v;
            from = "RECOMP_THREAD_STACK_MB";
        } else {
            fprintf(stderr, "[win32_compat] RECOMP_THREAD_STACK_MB=%s ignored "
                    "(want 1..1024)\n", env);
        }
    }
    s_stack_floor = (size_t)mb << 20;
    fprintf(stderr, "[win32_compat] thread stack floor: %lu MB (%s)\n", mb, from);
}

/* The stack size a new thread gets for a request (0 = no preference). */
static size_t thread_stack_size(size_t requested)
{
    size_t size, page;
    long   pg;

    pthread_once(&s_stack_floor_once, stack_floor_init);
    size = requested > s_stack_floor ? requested : s_stack_floor;
    pg   = sysconf(_SC_PAGESIZE);
    page = pg > 0 ? (size_t)pg : 4096;
    return (size + page - 1) / page * page;
}

/* pthread_attr_init plus the stack size; the caller destroys the attr. */
static void thread_attr_init(pthread_attr_t *attr, size_t requested)
{
    pthread_attr_init(attr);
    pthread_attr_setstacksize(attr, thread_stack_size(requested));
}

static void *thread_trampoline(void *arg)
{
    w32_object *o = (w32_object *)arg;
    t_self_obj = o;
    t_tid      = o->tid;
    recomp_host_cpus_track(s_process_mask);   /* restored after a resume (XBA-50) */

#if defined(__linux__)
    /* The process mask, not the creator's: see process_mask_capture. */
    if (s_process_mask) {
        cpu_set_t set;
        int i;
        CPU_ZERO(&set);
        for (i = 0; i < (int)(8 * sizeof s_process_mask) && i < CPU_SETSIZE; i++)
            if (s_process_mask & ((DWORD_PTR)1 << i))
                CPU_SET(i, &set);
        pthread_setaffinity_np(pthread_self(), sizeof set, &set);
    }
#endif

    /* CREATE_SUSPENDED gate */
    pthread_mutex_lock(&o->lock);
    while (o->suspend_count > 0)
        pthread_cond_wait(&o->gate, &o->lock);
    pthread_mutex_unlock(&o->lock);

    DWORD rc = o->start ? o->start(o->start_param) : 0;

    recomp_host_cpus_untrack();   /* its tid may go to a thread that is not ours */
    pthread_mutex_lock(&o->lock);
    o->exit_code = rc;
    o->exited    = 1;
    o->signaled  = 1;
    obj_wake(o);
    pthread_mutex_unlock(&o->lock);

    obj_release(o);   /* drop the trampoline's reference */
    return NULL;
}

HANDLE CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stackSize,
                    LPTHREAD_START_ROUTINE start, LPVOID param,
                    DWORD flags, LPDWORD threadId)
{
    (void)sa;
    w32_object *o = obj_alloc(K_THREAD);
    o->start         = start;
    o->start_param   = param;
    o->tid           = (DWORD)InterlockedIncrement(&s_next_tid);
    o->suspend_count = (flags & CREATE_SUSPENDED) ? 1 : 0;
    o->refcount      = 2;   /* one for caller, one for the trampoline */

    pthread_attr_t attr;
    thread_attr_init(&attr, stackSize);

    if (pthread_create(&o->thread, &attr, thread_trampoline, o) != 0) {
        pthread_attr_destroy(&attr);
        o->refcount = 1;
        obj_release(o);
        SetLastError(8 /* ERROR_NOT_ENOUGH_MEMORY */);
        return NULL;
    }
    pthread_attr_destroy(&attr);
    /* Detached: nothing joins a guest thread, and a joinable thread that
     * ended keeps its stack mapped (the 8 MB floor) until it is joined. The
     * handle keeps the exit code and the signalled state (XBA-56). Detached,
     * o->thread names a live thread only until the thread marks itself
     * exited under o->lock: SetThreadAffinityMask and TerminateThread use it
     * under that lock and only while !exited. */
    pthread_detach(o->thread);

    if (threadId) *threadId = o->tid;
    return obj_handle(o);
}

VOID ExitThread(DWORD exitCode)
{
    w32_object *o = t_self_obj;
    if (o) {
        pthread_mutex_lock(&o->lock);
        o->exit_code = exitCode;
        o->exited    = 1;
        o->signaled  = 1;
        obj_wake(o);
        pthread_mutex_unlock(&o->lock);
        obj_release(o);
    }
    recomp_host_cpus_untrack();
    pthread_exit(NULL);
}

BOOL GetExitCodeThread(HANDLE h, LPDWORD exitCode)
{
    w32_object *o = obj_from_handle(h);
    if (!o || o->kind != K_THREAD || !exitCode) return FALSE;
    pthread_mutex_lock(&o->lock);
    *exitCode = o->exited ? o->exit_code : STILL_ACTIVE;
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

DWORD ResumeThread(HANDLE h)
{
    w32_object *o = obj_from_handle(h);
    if (!o || o->kind != K_THREAD) return (DWORD)-1;
    pthread_mutex_lock(&o->lock);
    DWORD prev = (DWORD)o->suspend_count;
    if (o->suspend_count > 0 && --o->suspend_count == 0)
        pthread_cond_broadcast(&o->gate);
    pthread_mutex_unlock(&o->lock);
    return prev;
}

DWORD SuspendThread(HANDLE h)
{
    /* POSIX cannot stop another thread at an arbitrary point, so suspending
     * one only counts, for ResumeThread. A thread suspending *itself* is a
     * different matter: it is at a known point, and it parks on the same gate
     * CREATE_SUSPENDED uses until a ResumeThread brings the count to zero.
     * Worker pools idle that way; returning at once turned one title's pool
     * into a suspend/check spin of millions of calls a second. */
    if (h == PSEUDO_CURRENT_THREAD) h = GetCurrentThread();
    w32_object *o = obj_from_handle(h);
    if (!o || o->kind != K_THREAD)
        return (DWORD)-1;
    pthread_mutex_lock(&o->lock);
    DWORD prev = (DWORD)o->suspend_count;
    o->suspend_count++;
    if (o == t_self_obj)
        while (o->suspend_count > 0)
            pthread_cond_wait(&o->gate, &o->lock);
    pthread_mutex_unlock(&o->lock);
    return prev;
}

BOOL TerminateThread(HANDLE h, DWORD exitCode)
{
    w32_object *o = obj_from_handle(h);
    if (!o || o->kind != K_THREAD) return FALSE;
#if defined(__ANDROID__)
    /* bionic has no pthread_cancel. Nothing in the runtime calls this: an
     * Xbox thread only ends itself (PsTerminateSystemThread -> ExitThread,
     * which bionic's pthread_exit handles). So fail plainly rather than
     * report a thread as exited while it keeps running (XBA-25). */
    {
        static volatile LONG warned;
        if (InterlockedExchange(&warned, 1) == 0)
            fprintf(stderr, "TerminateThread: not supported on Android (no pthread_cancel); "
                            "the thread keeps running\n");
    }
    SetLastError(50 /* ERROR_NOT_SUPPORTED */);
    return FALSE;
#else
    pthread_mutex_lock(&o->lock);
    if (o->exited) {               /* gone: its pthread_t may be reused */
        pthread_mutex_unlock(&o->lock);
        return TRUE;
    }
    pthread_cancel(o->thread);
#endif
    o->exit_code = exitCode;
    o->exited    = 1;
    o->signaled  = 1;
    obj_wake(o);
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

BOOL SetThreadPriority(HANDLE h, int priority)
{
    w32_object *o = (h == PSEUDO_CURRENT_THREAD) ? t_self_obj : obj_from_handle(h);
    if (o && o->kind == K_THREAD) o->priority = priority;
    return TRUE;   /* real RT priorities need privileges; tracked only */
}

int GetThreadPriority(HANDLE h)
{
    w32_object *o = (h == PSEUDO_CURRENT_THREAD) ? t_self_obj : obj_from_handle(h);
    return (o && o->kind == K_THREAD) ? o->priority : THREAD_PRIORITY_NORMAL;
}

#if defined(__linux__)
static DWORD_PTR thread_affinity_set(pthread_t th, DWORD_PTR mask)
{
    DWORD_PTR prev = 0;
    cpu_set_t set;
    int i;
    if (pthread_getaffinity_np(th, sizeof set, &set) == 0)
        for (i = 0; i < (int)(8 * sizeof prev) && i < CPU_SETSIZE; i++)
            if (CPU_ISSET(i, &set))
                prev |= (DWORD_PTR)1 << i;
    CPU_ZERO(&set);
    for (i = 0; i < (int)(8 * sizeof mask) && i < CPU_SETSIZE; i++)
        if (mask & ((DWORD_PTR)1 << i))
            CPU_SET(i, &set);
    if (pthread_setaffinity_np(th, sizeof set, &set) != 0)
        return 0;
    return prev ? prev : mask;
}
#endif

DWORD_PTR SetThreadAffinityMask(HANDLE h, DWORD_PTR mask)
{
    w32_object *o = (h == PSEUDO_CURRENT_THREAD) ? t_self_obj : obj_from_handle(h);
    if (o && o->kind != K_THREAD)
        o = NULL;
    if (!mask)
        return 0;
    process_mask_capture();   /* before this thread's mask shrinks */
#if defined(__linux__)
    {
        DWORD_PTR prev;
        if (!o || o == t_self_obj)
            return thread_affinity_set(pthread_self(), mask);
        /* Another shim thread, detached (XBA-56): its pthread_t is valid
         * only until it marks itself exited, which it does under o->lock. */
        pthread_mutex_lock(&o->lock);
        prev = o->exited ? 0 : thread_affinity_set(o->thread, mask);
        pthread_mutex_unlock(&o->lock);
        return prev;
    }
#else
    /* No thread affinity on this host: macOS has only affinity tags, a
     * hint, so the guest threads' one-core pin (RECOMP_GUEST_CPUS) does
     * nothing here and returns 0 as a failed call would. */
    (void)o;
    return 0;
#endif
}

BOOL GetProcessAffinityMask(HANDLE process, DWORD_PTR *process_mask,
                            DWORD_PTR *system_mask)
{
    /* The process's mask, captured before any thread shrank its own:
     * sched_getaffinity(0) would answer for the calling thread. */
    DWORD_PTR mask = process_mask_capture();
    (void)process;
    if (process_mask) *process_mask = mask;
    if (system_mask)  *system_mask  = mask;
    return TRUE;
}

VOID SwitchToThread(void) { sched_yield(); }

DWORD QueueUserAPC(PAPCFUNC func, HANDLE thread, ULONG_PTR data)
{
    w32_object *o = (thread == PSEUDO_CURRENT_THREAD) ? self_apc_obj() : obj_from_handle(thread);
    if (!o || o->kind != K_THREAD) return 0;
    pthread_mutex_lock(&o->lock);
    DWORD ok = 0;
    if (o->apc_count < W32_MAX_APC) {
        o->apc_func[o->apc_count] = func;
        o->apc_data[o->apc_count] = data;
        o->apc_count++;
        ok = 1;
        obj_wake(o);               /* an alertable WaitForMultipleObjectsEx runs it */
    }
    pthread_mutex_unlock(&o->lock);
    return ok;
}

/* ===================================================================== */
/* Sleep                                                                 */
/* ===================================================================== */

VOID Sleep(DWORD ms)
{
    if (ms == 0) { sched_yield(); return; }
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) { }
}

/* Alertable: an APC queued meanwhile ends the sleep (WAIT_IO_COMPLETION)
 * rather than waiting for its end; the thread's own object carries a wait
 * block for it, as in WaitForMultipleObjectsEx (XBA-133). */
DWORD SleepEx(DWORD ms, BOOL alertable)
{
    w32_object *self = alertable ? self_apc_obj() : NULL;
    struct timespec ts;
    w32_waitblock wb;
    w32_waitlink link;
    int rc = 0;

    if (alertable && drain_apcs() > 0)
        return WAIT_IO_COMPLETION;
    if (!self || ms == 0) {
        Sleep(ms);
        if (alertable && drain_apcs() > 0)
            return WAIT_IO_COMPLETION;
        return 0;
    }
    if (ms != INFINITE) deadline_from_ms(ms, &ts);
    pthread_mutex_init(&wb.m, NULL);
    cond_init_wait_clock(&wb.c);
    wb.seq = 0;
    pthread_mutex_lock(&self->lock);
    link_add(self, &link, &wb);
    pthread_mutex_unlock(&self->lock);
    for (;;) {
        unsigned seq;
        int queued;
        pthread_mutex_lock(&wb.m);
        seq = wb.seq;
        pthread_mutex_unlock(&wb.m);
        pthread_mutex_lock(&self->lock);
        queued = self->apc_count > 0;
        pthread_mutex_unlock(&self->lock);
        if (queued || rc == ETIMEDOUT)
            break;
        pthread_mutex_lock(&wb.m);
        while (wb.seq == seq && rc != ETIMEDOUT)
            rc = ms == INFINITE ? pthread_cond_wait(&wb.c, &wb.m)
                                : pthread_cond_timedwait(&wb.c, &wb.m, &ts);
        pthread_mutex_unlock(&wb.m);
    }
    pthread_mutex_lock(&self->lock);
    link_remove(self, &link);
    pthread_mutex_unlock(&self->lock);
    pthread_cond_destroy(&wb.c);
    pthread_mutex_destroy(&wb.m);
    return drain_apcs() > 0 ? WAIT_IO_COMPLETION : 0;
}

HANDLE w32_thread_apc_handle(void)
{
    w32_object *o = self_apc_obj();
    if (!o) {
        o = obj_alloc(K_THREAD);
        o->thread = pthread_self();
        o->tid = GetCurrentThreadId();
        t_adopted = o;
    }
    InterlockedIncrement(&o->refcount);
    return obj_handle(o);
}

/* ===================================================================== */
/* Timer queues (one helper thread per timer)                            */
/* ===================================================================== */

/* The timer whose callback this thread is running: DeleteTimerQueueTimer
 * called from it does not wait for itself. */
static __thread w32_object *t_timer_obj = NULL;

static void timespec_add_ms(struct timespec *t, DWORD ms)
{
    long long ns = t->tv_nsec + (long long)(ms % 1000) * 1000000LL;
    t->tv_sec += (time_t)(ms / 1000) + (time_t)(ns / 1000000000LL);
    t->tv_nsec = (long)(ns % 1000000000LL);
}

/* The thread waits on o->cond under o->lock for an absolute deadline, so a
 * DeleteTimerQueueTimer wakes it at once and it exits, its stack with it,
 * instead of sleeping out the due time (a timer re-armed every frame used to
 * leave one sleeping thread per arm). A periodic timer's next deadline is one
 * period after the previous one, not after the callback's end, so it does not
 * drift; when it falls a whole period behind it fires once and goes on from
 * now (no burst of late callbacks). */
static void *timer_thread(void *arg)
{
    w32_object *o = (w32_object *)arg;
    struct timespec due, now;
    unsigned seq;

    t_timer_obj = o;
    pthread_mutex_lock(&o->lock);
    seq = o->timer_seq;
    deadline_from_ms(o->timer_due, &due);
    while (!o->timer_cancel) {
        if (o->timer_seq != seq) {               /* ChangeTimerQueueTimer */
            seq = o->timer_seq;
            deadline_from_ms(o->timer_due, &due);
        }
        clock_gettime(W32_WAIT_CLOCK, &now);
        if (timespec_before(&now, &due)) {
            pthread_cond_timedwait(&o->cond, &o->lock, &due);
            continue;                            /* re-check cancel, change, time */
        }
        WAITORTIMERCALLBACK cb = o->timer_cb;
        PVOID param = o->timer_param;
        DWORD period = o->timer_period;
        o->timer_running = 1;
        pthread_mutex_unlock(&o->lock);
        if (cb) cb(param, TRUE);
        pthread_mutex_lock(&o->lock);
        o->timer_running = 0;
        pthread_cond_broadcast(&o->cond);        /* a DeleteTimerQueueTimer waiting */
        if (!period || o->timer_seq != seq) {
            if (o->timer_seq != seq) continue;   /* changed from the callback */
            break;
        }
        timespec_add_ms(&due, period);
        clock_gettime(W32_WAIT_CLOCK, &now);
        if (!timespec_before(&now, &due)) {      /* a period or more late */
            due = now;
            timespec_add_ms(&due, period);
        }
    }
    pthread_mutex_unlock(&o->lock);
    t_timer_obj = NULL;
    obj_release(o);
    return NULL;
}

HANDLE CreateTimerQueue(void)
{
    /* A timer queue is just a grouping token here. */
    return obj_handle(obj_alloc(K_TIMER));
}

BOOL DeleteTimerQueue(HANDLE timerQueue)
{
    return CloseHandle(timerQueue);
}

BOOL CreateTimerQueueTimer(PHANDLE newTimer, HANDLE timerQueue,
                           WAITORTIMERCALLBACK callback, PVOID param,
                           DWORD dueTime, DWORD period, ULONG flags)
{
    (void)timerQueue;
    if (flags & WT_EXECUTEONLYONCE) period = 0;
    w32_object *o = obj_alloc(K_TIMER);
    o->timer_cb     = callback;
    o->timer_param  = param;
    o->timer_due    = dueTime;
    o->timer_period = period;
    o->refcount     = 2;   /* caller + timer thread */

    /* The callback can be a guest DPC (KeSetTimerEx): lifted code. */
    pthread_attr_t attr;
    pthread_t th;
    int rc;
    thread_attr_init(&attr, 0);
    rc = pthread_create(&th, &attr, timer_thread, o);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        o->refcount = 1;
        obj_release(o);
        return FALSE;
    }
    pthread_detach(th);
    if (newTimer) *newTimer = obj_handle(o);
    return TRUE;
}

/* Win32: the timer is started again, dueTime from now. */
BOOL ChangeTimerQueueTimer(HANDLE timerQueue, HANDLE timer, ULONG dueTime, ULONG period)
{
    (void)timerQueue;
    w32_object *o = obj_from_handle(timer);
    if (!o || o->kind != K_TIMER) return FALSE;
    pthread_mutex_lock(&o->lock);
    o->timer_due    = dueTime;
    o->timer_period = period;
    o->timer_seq++;
    pthread_cond_broadcast(&o->cond);
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

/* The timer thread is woken and exits without another callback. With
 * INVALID_HANDLE_VALUE, as on Win32, the call returns once a callback
 * running at that moment has returned (unless called from that callback). */
BOOL DeleteTimerQueueTimer(HANDLE timerQueue, HANDLE timer, HANDLE completionEvent)
{
    (void)timerQueue;
    w32_object *o = obj_from_handle(timer);
    if (!o || o->kind != K_TIMER) return FALSE;
    pthread_mutex_lock(&o->lock);
    o->timer_cancel = 1;
    pthread_cond_broadcast(&o->cond);
    if (completionEvent == INVALID_HANDLE_VALUE && t_timer_obj != o)
        while (o->timer_running)
            pthread_cond_wait(&o->cond, &o->lock);
    pthread_mutex_unlock(&o->lock);
    if (completionEvent && completionEvent != INVALID_HANDLE_VALUE) SetEvent(completionEvent);
    obj_release(o);
    return TRUE;
}

struct w32_tp_args { PTP_SIMPLE_CALLBACK cb; PVOID ctx; };

static void *w32_tp_trampoline(void *arg)
{
    struct w32_tp_args *a = (struct w32_tp_args *)arg;
    a->cb(NULL, a->ctx);
    free(a);
    return NULL;
}

BOOL TrySubmitThreadpoolCallback(PTP_SIMPLE_CALLBACK callback,
                                 PVOID context, PVOID env)
{
    (void)env;
    /* Run on a throwaway detached thread. */
    struct w32_tp_args *a = (struct w32_tp_args *)malloc(sizeof(*a));
    a->cb = callback; a->ctx = context;
    /* The callback can be a guest DPC (KeInsertQueueDpc): lifted code. */
    pthread_t th;
    pthread_attr_t attr;
    int rc;
    thread_attr_init(&attr, 0);
    rc = pthread_create(&th, &attr, w32_tp_trampoline, a);
    pthread_attr_destroy(&attr);
    if (rc != 0) { free(a); return FALSE; }
    pthread_detach(th);
    return TRUE;
}

/* ===================================================================== */
/* Waitable timers                                                       */
/* ===================================================================== */

HANDLE CreateWaitableTimerW(LPSECURITY_ATTRIBUTES sa, BOOL manualReset, LPCWSTR name)
{
    (void)sa;
    (void)name;
    w32_object *o = obj_alloc(K_WAITABLE_TIMER);
    o->waitable_manual_reset = manualReset;
    return obj_handle(o);
}

BOOL SetWaitableTimer(HANDLE h, const LARGE_INTEGER *dueTime, LONG period,
                      PTIMERAPCROUTINE completion, PVOID arg, BOOL resume)
{
    w32_object *o = obj_from_handle(h);
    (void)completion; (void)arg; (void)resume;
    if (!o || o->kind != K_WAITABLE_TIMER || !dueTime) return FALSE;
    pthread_mutex_lock(&o->lock);
    /* Win32 100ns units: negative is relative to now, positive is an absolute
     * FILETIME. A period re-arms a synchronization timer as its waiter takes
     * each expiry (wait_single); a periodic manual-reset timer stays
     * signalled after the first expiry, which is the same to a waiter. */
    if (dueTime->QuadPart <= 0) {
        clock_gettime(W32_WAIT_CLOCK, &o->waitable_due_time);
        LONGLONG ns = -dueTime->QuadPart * 100LL;
        o->waitable_due_time.tv_sec  += (time_t)(ns / 1000000000LL);
        o->waitable_due_time.tv_nsec += (long)(ns % 1000000000LL);
        if (o->waitable_due_time.tv_nsec >= 1000000000L) {
            o->waitable_due_time.tv_sec++;
            o->waitable_due_time.tv_nsec -= 1000000000L;
        }
    } else {
        /* An absolute FILETIME (epoch 1601-01-01; Unix is 1970-01-01) is a
         * wall-clock instant: turn it into a deadline on the wait clock once,
         * now, as "that far from the wall clock's present". */
        LONGLONG unix100ns = dueTime->QuadPart - 116444736000000000LL;
        struct timespec wall, base;
        clock_gettime(CLOCK_REALTIME, &wall);
        clock_gettime(W32_WAIT_CLOCK, &base);
        long long delta_ns = (long long)unix100ns * 100LL
                           - ((long long)wall.tv_sec * 1000000000LL + wall.tv_nsec);
        long long due_ns = (long long)base.tv_sec * 1000000000LL + base.tv_nsec + delta_ns;
        if (due_ns < 0) due_ns = 0;          /* long past: due at once */
        o->waitable_due_time.tv_sec  = (time_t)(due_ns / 1000000000LL);
        o->waitable_due_time.tv_nsec = (long)(due_ns % 1000000000LL);
    }
    o->waitable_period_ms = period > 0 ? period : 0;
    o->waitable_armed = 1;
    o->waitable_triggered = 0;
    obj_wake(o);
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

BOOL CancelWaitableTimer(HANDLE h)
{
    w32_object *o = obj_from_handle(h);
    if (!o || o->kind != K_WAITABLE_TIMER) return FALSE;
    pthread_mutex_lock(&o->lock);
    o->waitable_triggered = 0;
    o->waitable_armed = 0;
    o->waitable_period_ms = 0;
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

/* ===================================================================== */
/* Heap (thin wrapper over malloc; the single process heap)              */
/* ===================================================================== */

static w32_object s_process_heap = { .kind = K_HEAP };

HANDLE GetProcessHeap(void)                       { return (HANDLE)&s_process_heap; }
HANDLE HeapCreate(DWORD o, SIZE_T i, SIZE_T m)    { (void)o;(void)i;(void)m; return (HANDLE)&s_process_heap; }
BOOL   HeapDestroy(HANDLE h)                      { (void)h; return TRUE; }

LPVOID HeapAlloc(HANDLE heap, DWORD flags, SIZE_T bytes)
{
    (void)heap;
    return (flags & HEAP_ZERO_MEMORY) ? calloc(1, bytes ? bytes : 1)
                                      : malloc(bytes ? bytes : 1);
}
LPVOID HeapReAlloc(HANDLE heap, DWORD flags, LPVOID mem, SIZE_T bytes)
{
    (void)heap; (void)flags;
    return realloc(mem, bytes ? bytes : 1);
}
BOOL HeapFree(HANDLE heap, DWORD flags, LPVOID mem)
{
    (void)heap; (void)flags;
    free(mem);
    return TRUE;
}
SIZE_T HeapSize(HANDLE heap, DWORD flags, LPCVOID mem)
{
    (void)heap; (void)flags; (void)mem;
    return 0;   /* glibc malloc_usable_size could be used; not needed yet */
}

/* ===================================================================== */
/* Virtual memory                                                        */
/* ===================================================================== */

/* Never PROT_EXEC (XBA-51). Nothing executes memory allocated through this
 * layer: guest code is recompiled ahead of time into the host binary, so a
 * guest's execute request (its code pages, NtAllocateVirtualMemory or
 * NtProtectVirtualMemory with PAGE_EXECUTE_*, MmSetAddressProtect) only ever
 * needs the data access that comes with it, and the runtime has no JIT. An
 * executable mapping costs something, though: in an Android app (SELinux
 * untrusted_app) an executable anonymous or memfd mapping can be refused
 * (execmem), and then the guest's request fails for a permission it never
 * uses. So the execute bit is dropped and read / write kept; PAGE_EXECUTE
 * alone, which on x86 reads as well, becomes PROT_READ. VirtualQuery already
 * reports every mapping as PAGE_READWRITE, so no caller sees the difference. */
static int prot_from_page(DWORD protect)
{
    switch (protect & 0xFF) {
    case PAGE_NOACCESS:          return PROT_NONE;
    case PAGE_READONLY:          return PROT_READ;
    case PAGE_READWRITE:         return PROT_READ | PROT_WRITE;
    case PAGE_EXECUTE:           return PROT_READ;
    case PAGE_EXECUTE_READ:      return PROT_READ;
    case PAGE_EXECUTE_READWRITE: return PROT_READ | PROT_WRITE;
    default:                     return PROT_READ | PROT_WRITE;   /* WRITECOPY, EXECUTE_WRITECOPY */
    }
}

#if defined(__APPLE__)
/* Darwin has no MAP_FIXED_NOREPLACE, and the two mmap options are both wrong
 * for VirtualAlloc: MAP_FIXED silently unmaps whatever already occupies the
 * range, and a bare address hint can be relocated by the kernel for reasons
 * other than the range being taken -- so "we got a different address" is only
 * an approximation of "it was occupied", and a racy one.
 *
 * mach_vm_map with VM_FLAGS_FIXED is the exact primitive: it maps at the
 * address given, and returns KERN_NO_SPACE rather than displacing an existing
 * mapping. That is what Win32 promises, and this layer exists to keep the Xbox
 * HLE above it honest -- a VirtualAlloc that quietly replaced a live mapping
 * would corrupt whatever held it, far from the call that did it.
 *
 * Memory from mach_vm_map is released by munmap like any other, because the
 * BSD and Mach halves of Darwin share one VM map, so VirtualFree is unchanged.
 */
static void *mach_map_fixed(void *address, size_t size, int prot)
{
    mach_vm_address_t addr = (mach_vm_address_t)(uintptr_t)address;
    mach_vm_size_t len = (size + vm_page_size - 1) & ~((mach_vm_size_t)vm_page_size - 1);
    vm_prot_t vmprot = VM_PROT_NONE;

    if (prot & PROT_READ)  vmprot |= VM_PROT_READ;
    if (prot & PROT_WRITE) vmprot |= VM_PROT_WRITE;
    if (prot & PROT_EXEC)  vmprot |= VM_PROT_EXECUTE;

    kern_return_t kr = mach_vm_map(
        mach_task_self(),
        &addr,
        len,
        0,
        VM_FLAGS_FIXED,
        MEMORY_OBJECT_NULL,
        0,
        FALSE,
        vmprot,
        VM_PROT_ALL,
        VM_INHERIT_DEFAULT
    );
    if (kr != KERN_SUCCESS) {
        return MAP_FAILED;
    }
    return (void *)(uintptr_t)addr;
}
#endif

/* Length registry, defined with the view helpers below. Win32 frees by address
 * alone -- UnmapViewOfFile takes no length and VirtualFree(MEM_RELEASE) is
 * documented to take size 0 -- so the length has to be recoverable here or
 * munmap cannot be called at all. */
void view_register(void *addr, size_t len);
size_t view_take(const void *addr);

LPVOID VirtualAlloc(LPVOID address, SIZE_T size, DWORD allocationType, DWORD protect)
{
    int prot  = prot_from_page(protect);
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;

    /* MEM_COMMIT on a region already reserved by a prior VirtualAlloc:
     * just adjust protection. */
    if ((allocationType & MEM_COMMIT) && !(allocationType & MEM_RESERVE) && address) {
        if (mprotect(address, size, prot) == 0)
            return address;
        /* fall through to a fresh mapping */
    }

#if defined(MAP_FIXED_NOREPLACE)
    if (address) flags |= MAP_FIXED_NOREPLACE;
#endif

    void *p;
#if defined(__APPLE__)
    if (address) {
        p = mach_map_fixed(address, size, prot ? prot : PROT_READ | PROT_WRITE);
    } else
#endif
    p = mmap(address, size, prot ? prot : PROT_READ | PROT_WRITE, flags, -1, 0);
    if (p == MAP_FAILED) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    /* Remember the length: VirtualFree(MEM_RELEASE) is passed size 0 by every
     * Win32 caller, and munmap cannot be called without one. */
    view_register(p, size);
#if !defined(MAP_FIXED_NOREPLACE) && !defined(__APPLE__)
    /* Older kernels without MAP_FIXED_NOREPLACE: plain MAP_FIXED would silently
     * unmap whatever already lives there, so we pass the address as a hint and
     * treat a different result as "taken". Apple goes through mach_map_fixed
     * above, which reports that properly instead of inferring it. */
    if (address && p != address) {
        munmap(p, size);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
#endif
    return p;
}

BOOL VirtualFree(LPVOID address, SIZE_T size, DWORD freeType)
{
    if (freeType & MEM_RELEASE) {
        /* Win32 MEM_RELEASE passes size 0 and frees the whole allocation, so
         * the length comes from the registry VirtualAlloc filled in. Returning
         * TRUE without unmapping -- as this used to -- made every release a
         * silent no-op: the caller believed the address was free, the next
         * allocation there failed, and nothing connected the two. */
        size_t len = view_take(address);
        if (size == 0) size = len;
        if (size == 0) return FALSE;
        return munmap(address, size) == 0;
    }
    if (freeType & MEM_DECOMMIT)
        return mprotect(address, size, PROT_NONE) == 0;
    return TRUE;
}

BOOL VirtualProtect(LPVOID address, SIZE_T size, DWORD newProtect, PDWORD oldProtect)
{
    if (oldProtect) *oldProtect = PAGE_READWRITE;
    return mprotect(address, size, prot_from_page(newProtect)) == 0;
}

/* ===================================================================== */
/* Time                                                                  */
/* ===================================================================== */

/* 100-ns intervals between 1601-01-01 and 1970-01-01 */
#define FILETIME_EPOCH_DIFF 116444736000000000ULL

VOID GetSystemTimeAsFileTime(LPFILETIME ft)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ULONGLONG t = FILETIME_EPOCH_DIFF
                + (ULONGLONG)ts.tv_sec * 10000000ULL
                + (ULONGLONG)ts.tv_nsec / 100ULL;
    ft->dwLowDateTime  = (DWORD)(t & 0xFFFFFFFFULL);
    ft->dwHighDateTime = (DWORD)(t >> 32);
}

static void fill_systemtime(LPSYSTEMTIME st, const struct tm *tm, long nsec)
{
    st->wYear         = (WORD)(tm->tm_year + 1900);
    st->wMonth        = (WORD)(tm->tm_mon + 1);
    st->wDayOfWeek    = (WORD)tm->tm_wday;
    st->wDay          = (WORD)tm->tm_mday;
    st->wHour         = (WORD)tm->tm_hour;
    st->wMinute       = (WORD)tm->tm_min;
    st->wSecond       = (WORD)tm->tm_sec;
    st->wMilliseconds = (WORD)(nsec / 1000000L);
}

VOID GetSystemTime(LPSYSTEMTIME st)
{
    struct timespec ts; struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    gmtime_r(&ts.tv_sec, &tm);
    fill_systemtime(st, &tm, ts.tv_nsec);
}

VOID GetLocalTime(LPSYSTEMTIME st)
{
    struct timespec ts; struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    fill_systemtime(st, &tm, ts.tv_nsec);
}

ULONGLONG GetTickCount64(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ULONGLONG)ts.tv_sec * 1000ULL + (ULONGLONG)ts.tv_nsec / 1000000ULL;
}

DWORD GetTickCount(void) { return (DWORD)GetTickCount64(); }

BOOL QueryPerformanceCounter(PLARGE_INTEGER count)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    count->QuadPart = (LONGLONG)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    return TRUE;
}

BOOL QueryPerformanceFrequency(PLARGE_INTEGER freq)
{
    freq->QuadPart = 1000000000LL;   /* QPC is in nanoseconds */
    return TRUE;
}

/* ===================================================================== */
/* Misc                                                                  */
/* ===================================================================== */

VOID OutputDebugStringA(LPCSTR str)
{
    if (str) fputs(str, stderr);
}

VOID ExitProcess(UINT exitCode) { exit((int)exitCode); }

BOOL IsDebuggerPresent(void)
{
#if defined(__APPLE__)
    /* Darwin: KERN_PROC_PID reports P_TRACED when a debugger is attached. */
    struct kinfo_proc info;
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() };
    size_t size = sizeof(info);
    memset(&info, 0, size);
    if (sysctl(mib, sizeof(mib), &info, &size, NULL, 0) != 0) return FALSE;
    return (info.kp_proc.p_flag & P_TRACED) != 0;
#else
    /* Linux: a non-zero TracerPid in /proc/self/status means ptrace is attached. */
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return FALSE;
    char line[256];
    BOOL traced = FALSE;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "TracerPid:", 10) == 0) {
            traced = strtol(line + 10, NULL, 10) != 0;
            break;
        }
    }
    fclose(f);
    return traced;
#endif
}

VOID DebugBreak(void) {
    /* Not __debugbreak(): that is an MSVC intrinsic, and this file is the
     * half that MSVC never compiles. SIGTRAP is the POSIX equivalent --
     * continuable under a debugger, fatal without one, as on Windows. */
    raise(SIGTRAP);
}

VOID SecureZeroMemory(PVOID ptr, SIZE_T cnt)
{
#if defined(__APPLE__)
    memset_s(ptr, cnt, 0, cnt);
#elif defined(__ANDROID__)
    /* bionic has no explicit_bzero: memset, then a barrier that tells the
     * compiler the bytes are read, so the store is not dropped. */
    memset(ptr, 0, cnt);
    __asm__ __volatile__("" : : "r"(ptr) : "memory");
#else
    explicit_bzero(ptr, cnt);
#endif
}

unsigned int _clearfp(void)
{
    feclearexcept(FE_ALL_EXCEPT);
    return 0;
}

/* ===================================================================== */
/* Win32 file API on POSIX (open/read/write/fstat-backed)                */
/* ===================================================================== */

#include <fcntl.h>
#include <sys/stat.h>

HANDLE CreateFileA(LPCSTR name, DWORD access, DWORD share,
                   LPSECURITY_ATTRIBUTES sa, DWORD disp,
                   DWORD flags, HANDLE templ)
{
    (void)share; (void)sa; (void)flags; (void)templ;
    if (!name) { SetLastError(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }

    int rw = O_RDONLY;
    int wantW = (access & (GENERIC_WRITE | GENERIC_ALL)) != 0;
    int wantR = (access & (GENERIC_READ  | GENERIC_ALL)) != 0;
    if (wantW && wantR) rw = O_RDWR;
    else if (wantW)     rw = O_WRONLY;

    int extra = 0;
    switch (disp) {
    case CREATE_NEW:        extra = O_CREAT | O_EXCL;  break;
    case CREATE_ALWAYS:     extra = O_CREAT | O_TRUNC; break;
    case OPEN_EXISTING:     extra = 0;                 break;
    case OPEN_ALWAYS:       extra = O_CREAT;           break;
    case TRUNCATE_EXISTING: extra = O_TRUNC;           break;
    default:                extra = 0;                 break;
    }
    if ((extra & (O_CREAT | O_TRUNC)) && rw == O_RDONLY) rw = O_RDWR;

    /* Normalise embedded Windows-style backslashes before open(). */
    char norm[1024];
    snprintf(norm, sizeof(norm), "%s", name);
    xbox_path_normalize(norm);
    int fd = open(norm, rw | extra, 0644);
    if (fd < 0) { SetLastError(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    return w32_open_handle(fd, name);
}

HANDLE CreateFileW(LPCWSTR name, DWORD access, DWORD share,
                   LPSECURITY_ATTRIBUTES sa, DWORD disp,
                   DWORD flags, HANDLE templ)
{
    /* Basic UTF-16 -> UTF-8 (ASCII path of WideCharToMultiByte). */
    char buf[1024];
    int len = WideCharToMultiByte(CP_UTF8, 0, name, -1, buf, sizeof(buf), NULL, NULL);
    if (len <= 0) buf[0] = '\0';
    return CreateFileA(buf, access, share, sa, disp, flags, templ);
}

BOOL ReadFile(HANDLE h, LPVOID buf, DWORD len, LPDWORD nread, void *overlapped)
{
    (void)overlapped;
    int fd = w32_handle_fd(h);
    if (fd < 0) { if (nread) *nread = 0; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    ssize_t n = read(fd, buf, len);
    if (n < 0)  { if (nread) *nread = 0; SetLastError(ERROR_GEN_FAILURE);    return FALSE; }
    if (nread)  *nread = (DWORD)n;
    return TRUE;
}

BOOL WriteFile(HANDLE h, LPCVOID buf, DWORD len, LPDWORD nwritten, void *overlapped)
{
    (void)overlapped;
    int fd = w32_handle_fd(h);
    if (fd < 0) { if (nwritten) *nwritten = 0; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    ssize_t n = write(fd, buf, len);
    if (n < 0)  { if (nwritten) *nwritten = 0; SetLastError(ERROR_GEN_FAILURE);    return FALSE; }
    if (nwritten) *nwritten = (DWORD)n;
    return TRUE;
}

DWORD GetFileSize(HANDLE h, LPDWORD high)
{
    int fd = w32_handle_fd(h);
    if (fd < 0) return INVALID_FILE_SIZE;
    struct stat st;
    if (fstat(fd, &st) != 0) return INVALID_FILE_SIZE;
    if (high) *high = (DWORD)(((uint64_t)st.st_size >> 32) & 0xFFFFFFFFu);
    return (DWORD)(st.st_size & 0xFFFFFFFFu);
}

BOOL GetFileSizeEx(HANDLE h, PLARGE_INTEGER size)
{
    int fd = w32_handle_fd(h);
    if (fd < 0 || !size) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    struct stat st;
    if (fstat(fd, &st) != 0) { SetLastError(ERROR_GEN_FAILURE); return FALSE; }
    size->QuadPart = (LONGLONG)st.st_size;
    return TRUE;
}

BOOL FlushFileBuffers(HANDLE h)
{
    int fd = w32_handle_fd(h);
    if (fd < 0) return FALSE;
    return fsync(fd) == 0;
}

/* ===================================================================== */
/* Keyboard + window helpers -- stubs. Real keyboard polling will come   */
/* via SDL_GetKeyboardState when main.c gets its SDL2 port.              */
/* ===================================================================== */

SHORT GetAsyncKeyState(int vKey)          { (void)vKey; return 0; }
HWND  FindWindowA(LPCSTR c, LPCSTR w)     { (void)c; (void)w; return NULL; }
HWND  GetActiveWindow(void)               { return NULL; }
BOOL  SetWindowTextA(HWND h, LPCSTR t)    { (void)h; (void)t; return TRUE; }
int   GetWindowTextA(HWND h, LPSTR t, int n) { (void)h; (void)t; (void)n; return 0; }
BOOL  EnumWindows(WNDENUMPROC p, LPARAM l) { (void)p; (void)l; return FALSE; }

int MessageBoxA(HWND h, LPCSTR text, LPCSTR caption, UINT type)
{
    (void)h; (void)type;
    fprintf(stderr, "[%s] %s\n", caption ? caption : "MessageBox",
                                   text    ? text    : "");
    return 1;   /* IDOK */
}

/* Message-loop stubs: no Win32 messages on POSIX (SDL events drive the
 * d3d8_gl backend; this layer is just for the game's Win32 message pump). */
BOOL    PeekMessageA(LPMSG m, HWND w, UINT a, UINT b, UINT f)
{ (void)m; (void)w; (void)a; (void)b; (void)f; return FALSE; }
BOOL    TranslateMessage(const MSG *m) { (void)m; return TRUE; }
LRESULT DispatchMessageA(const MSG *m) { (void)m; return 0; }

/* XInput stub: real gamepad is wired through input_compat (SDL2). */
DWORD XInputGetState(DWORD idx, XINPUT_STATE *state)
{ (void)idx; if (state) memset(state, 0, sizeof(*state)); return ERROR_DEVICE_NOT_CONNECTED; }

BOOL TerminateProcess(HANDLE process, UINT exitCode)
{
    (void)process;
    exit((int)exitCode);
}

VOID OutputDebugStringW(LPCWSTR str)
{
    if (!str) return;
    for (const WCHAR *p = str; *p; p++)
        fputc((*p < 128) ? (int)*p : '?', stderr);
}

/*
 * Minimal MultiByteToWideChar / WideCharToMultiByte. Handles UTF-8 and a
 * latin-1 interpretation of CP_ACP -- enough for path/name strings.
 */
int MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR mb, int mbCount,
                        LPWSTR wide, int wideCount)
{
    (void)flags;
    if (!mb) return 0;
    int srcLen = (mbCount < 0) ? (int)strlen(mb) + 1 : mbCount;
    int out = 0;

    for (int i = 0; i < srcLen; ) {
        unsigned int cpval;
        unsigned char c = (unsigned char)mb[i];

        if (cp == CP_UTF8 && c >= 0x80) {
            if ((c & 0xE0) == 0xC0 && i + 1 < srcLen) {
                cpval = ((c & 0x1F) << 6) | (mb[i+1] & 0x3F); i += 2;
            } else if ((c & 0xF0) == 0xE0 && i + 2 < srcLen) {
                cpval = ((c & 0x0F) << 12) | ((mb[i+1] & 0x3F) << 6) |
                        (mb[i+2] & 0x3F); i += 3;
            } else if ((c & 0xF8) == 0xF0 && i + 3 < srcLen) {
                cpval = ((c & 0x07) << 18) | ((mb[i+1] & 0x3F) << 12) |
                        ((mb[i+2] & 0x3F) << 6) | (mb[i+3] & 0x3F); i += 4;
            } else { cpval = c; i += 1; }
        } else {
            cpval = c; i += 1;   /* ASCII / latin-1 */
        }

        if (cpval > 0xFFFF) cpval = '?';   /* no surrogate pairs */
        if (wideCount > 0) {
            if (out >= wideCount) return 0;
            wide[out] = (WCHAR)cpval;
        }
        out++;
    }
    return out;
}

int WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR wide, int wideCount,
                        LPSTR mb, int mbCount, LPCSTR defChar, PBOOL usedDef)
{
    (void)flags; (void)defChar; (void)usedDef;
    if (!wide) return 0;
    int srcLen = wideCount;
    if (srcLen < 0) { srcLen = 0; while (wide[srcLen]) srcLen++; srcLen++; }
    int out = 0;

    for (int i = 0; i < srcLen; i++) {
        unsigned int cpval = wide[i];
        char buf[4]; int n;
        if (cp == CP_UTF8 && cpval >= 0x80) {
            if (cpval < 0x800) {
                buf[0] = (char)(0xC0 | (cpval >> 6));
                buf[1] = (char)(0x80 | (cpval & 0x3F)); n = 2;
            } else {
                buf[0] = (char)(0xE0 | (cpval >> 12));
                buf[1] = (char)(0x80 | ((cpval >> 6) & 0x3F));
                buf[2] = (char)(0x80 | (cpval & 0x3F)); n = 3;
            }
        } else {
            buf[0] = (char)(cpval > 0xFF ? '?' : cpval); n = 1;
        }
        if (mbCount > 0) {
            if (out + n > mbCount) return 0;
            for (int k = 0; k < n; k++) mb[out + k] = buf[k];
        }
        out += n;
    }
    return out;
}

/* ===================================================================== */
/* File mapping (memfd-backed) -- true aliased mirror views              */
/* ===================================================================== */

/* Registry of active views: UnmapViewOfFile takes no length, so we must
 * recover the mapping length here for munmap. */
typedef struct { void *addr; size_t len; } w32_view;
static w32_view        s_views[512];
static pthread_mutex_t s_views_lock = PTHREAD_MUTEX_INITIALIZER;

void view_register(void *addr, size_t len)
{
    pthread_mutex_lock(&s_views_lock);
    for (int i = 0; i < 512; i++)
        if (!s_views[i].addr) { s_views[i].addr = addr; s_views[i].len = len; break; }
    pthread_mutex_unlock(&s_views_lock);
}

/* Non-destructive counterpart to view_take, and interior-aware: VirtualQuery
 * is asked about addresses *within* a region at least as often as about its
 * base -- a translated guest VA lands in the middle of the 64 MB window.
 * Picks the containing region and reports where it starts. */
int view_lookup(const void *addr, void **base_out, size_t *len_out)
{
    int found = 0;
    pthread_mutex_lock(&s_views_lock);
    for (int i = 0; i < 512; i++) {
        if (!s_views[i].addr)
            continue;
        uintptr_t lo = (uintptr_t)s_views[i].addr;
        uintptr_t hi = lo + s_views[i].len;
        if ((uintptr_t)addr >= lo && (uintptr_t)addr < hi) {
            if (base_out) *base_out = s_views[i].addr;
            if (len_out)  *len_out  = s_views[i].len;
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&s_views_lock);
    return found;
}

/* Start of the lowest registered view above addr, or 0 when there is none.
 * VirtualQuery's answer for memory the shim does not own: the free run it
 * reports ends where the next mapping it knows of begins. */
static uintptr_t view_next_above(const void *addr)
{
    uintptr_t next = 0;
    pthread_mutex_lock(&s_views_lock);
    for (int i = 0; i < 512; i++) {
        uintptr_t lo = (uintptr_t)s_views[i].addr;
        if (lo > (uintptr_t)addr && (!next || lo < next))
            next = lo;
    }
    pthread_mutex_unlock(&s_views_lock);
    return next;
}

size_t view_take(const void *addr)
{
    size_t len = 0;
    pthread_mutex_lock(&s_views_lock);
    for (int i = 0; i < 512; i++)
        if (s_views[i].addr == addr) { len = s_views[i].len; s_views[i].addr = NULL; break; }
    pthread_mutex_unlock(&s_views_lock);
    return len;
}

/* An unnamed file descriptor that ftruncate and mmap both accept. Linux has
 * memfd_create for this; elsewhere an immediately-unlinked temp file does. */
static int anon_map_fd(const char *name)
{
#if defined(__APPLE__)
    static volatile LONG map_counter = 0;
    /* The pid keeps two instances apart: with a bare counter, both open
     * "/xbox_map_1" and the second O_EXCL loses. macOS caps the name at 31. */
    char shm_name[32];
    LONG seq = InterlockedIncrement(&map_counter);
    const char *base = name ? name : "xbox_map";
    snprintf(shm_name, sizeof(shm_name), "/%.12s_%d_%ld", base, (int)getpid(), (long)seq);
    int fd = shm_open(shm_name, O_CREAT | O_RDWR | O_EXCL, 0600);
    if (fd >= 0) shm_unlink(shm_name);
    return fd;
#else
    return memfd_create(name ? name : "xbox_map", 0);
#endif
}

HANDLE CreateFileMappingA(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD protect,
                          DWORD maxSizeHigh, DWORD maxSizeLow, LPCSTR name)
{
    (void)file; (void)sa; (void)protect;
    SIZE_T size = ((SIZE_T)maxSizeHigh << 32) | maxSizeLow;
    if (size == 0) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }

    int fd = anon_map_fd(name);
    if (fd < 0) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }

    w32_object *o = obj_alloc(K_FILEMAP);
    o->fd       = fd;
    o->map_size = size;
    return obj_handle(o);
}

HANDLE CreateFileMappingW(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD protect,
                          DWORD maxSizeHigh, DWORD maxSizeLow, LPCWSTR name)
{
    (void)name;
    return CreateFileMappingA(file, sa, protect, maxSizeHigh, maxSizeLow, NULL);
}

LPVOID MapViewOfFileEx(HANDLE mapping, DWORD access, DWORD offHigh, DWORD offLow,
                       SIZE_T count, LPVOID baseAddr)
{
    w32_object *o = obj_from_handle(mapping);
    if (!o || o->kind != K_FILEMAP) { SetLastError(ERROR_INVALID_HANDLE); return NULL; }

    off_t  off = ((off_t)offHigh << 32) | offLow;
    SIZE_T len = count ? count : (o->map_size - (SIZE_T)off);
    int prot   = PROT_READ | ((access != FILE_MAP_READ) ? PROT_WRITE : 0);
    int flags  = MAP_SHARED;
    int placeholder = 0;

    /* Win32 MapViewOfFileEx *fails* when the requested address is unavailable.
     * Plain MAP_FIXED does the opposite: it silently unmaps whatever is there
     * and succeeds. The Xbox memory model asks for 28 mirror views at computed
     * addresses, so with a base the OS chose rather than one we picked, that
     * difference is the process quietly destroying its own libraries and heap
     * and dying somewhere unrelated a moment later. */
    if (baseAddr) {
#if defined(MAP_FIXED_NOREPLACE)
        flags |= MAP_FIXED_NOREPLACE;
#elif defined(__APPLE__)
        /* Darwin has no MAP_FIXED_NOREPLACE. Claim the range first with
         * mach_vm_map(VM_FLAGS_FIXED), which refuses rather than displaces;
         * MAP_FIXED below can then only replace the placeholder we now own. */
        if (mach_map_fixed(baseAddr, len, PROT_READ | PROT_WRITE) == MAP_FAILED) {
            SetLastError(ERROR_INVALID_ADDRESS);
            return NULL;
        }
        placeholder = 1;
        flags |= MAP_FIXED;
#else
        flags |= MAP_FIXED;
#endif
    }

    void *p = mmap(baseAddr, len, prot, flags, o->fd, off);
    if (p == MAP_FAILED) {
        /* A failed MAP_FIXED leaves the placeholder where it was. Nothing has
         * registered it, so UnmapViewOfFile could never free it, and every
         * later attempt at this address would be refused as occupied. */
        if (placeholder) munmap(baseAddr, len);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    if (baseAddr && p != baseAddr) {
        /* MAP_FIXED_NOREPLACE hands back a different address instead of
         * failing on some kernels; treat that as the refusal it means. */
        munmap(p, len);
        SetLastError(ERROR_INVALID_ADDRESS);
        return NULL;
    }
    view_register(p, len);
    return p;
}

LPVOID MapViewOfFile(HANDLE mapping, DWORD access, DWORD offHigh, DWORD offLow, SIZE_T count)
{
    return MapViewOfFileEx(mapping, access, offHigh, offLow, count, NULL);
}

BOOL UnmapViewOfFile(LPCVOID baseAddr)
{
    size_t len = view_take(baseAddr);
    if (len == 0) return FALSE;
    return munmap((void *)baseAddr, len) == 0;
}

/* Replacing inside our own reservation (see the header). MAP_FIXED is the
 * point here, not the hazard it is elsewhere in this file: the range is the
 * caller's, so what it unmaps is the caller's reservation and nothing else,
 * and it does so atomically. Linux may still have unmapped the old range when
 * a MAP_FIXED mmap fails (ENOMEM on the map count, say), so a failure puts the
 * reservation back where it can; MAP_FIXED_NOREPLACE leaves an intact one
 * alone. */
static void *replace_in_reservation(void *addr, size_t len, int prot, int flags,
                                    int fd)
{
    void *p = mmap(addr, len, prot, flags | MAP_FIXED, fd, 0);
    if (p == MAP_FAILED) {
#if defined(MAP_FIXED_NOREPLACE)
        void *r = mmap(addr, len, PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE,
                       -1, 0);
        if (r != MAP_FAILED && r != addr)
            munmap(r, len);
#endif
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    view_register(p, len);
    return p;
}

LPVOID w32_map_view_replace(HANDLE mapping, SIZE_T len, LPVOID addr)
{
    w32_object *o = obj_from_handle(mapping);
    if (!o || o->kind != K_FILEMAP || !addr) {
        SetLastError(ERROR_INVALID_HANDLE);
        return NULL;
    }
    return replace_in_reservation(addr, len, PROT_READ | PROT_WRITE, MAP_SHARED,
                                  o->fd);
}

LPVOID w32_commit_replace(LPVOID addr, SIZE_T len)
{
    if (!addr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    return replace_in_reservation(addr, len, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1);
}

void w32_view_forget(LPCVOID addr)
{
    (void)view_take(addr);
}

int w32_anon_map_fd(const char *name)
{
    return anon_map_fd(name);
}

/* ===================================================================== */
/* VirtualQuery                                                           */
/* ===================================================================== */

/*
 * Answers from the view registry rather than from a constant.
 *
 * This used to report RegionSize 0x1000, MEM_COMMIT, PAGE_READWRITE and
 * AllocationBase NULL for every address it was handed, mapped or not. Four
 * kernel entry points are built on it, and one of them chooses a deallocator
 * with it: MmFreeContiguousMemory frees via VirtualFree only when
 * AllocationBase equals the pointer, so a hardcoded NULL sent every
 * contiguous buffer -- all of which come from VirtualAlloc, i.e. mmap -- to
 * _aligned_free, which is free(). The allocator aborts on the foreign
 * pointer. MmQueryAllocationSize answered 0x1000 for everything and
 * NtQueryVirtualMemory called unmapped addresses committed and readable.
 *
 * Everything the shim maps -- VirtualAlloc and MapViewOfFileEx alike -- is in
 * the registry, so it can answer for exactly the memory it owns and say
 * MEM_FREE for the rest. Saying MEM_FREE for an address it did not map is the
 * honest answer: a host heap pointer is not a Win32 reservation, and the
 * callers that branch on this want to know which allocator owns the pointer.
 */
SIZE_T VirtualQuery(LPCVOID address, PMEMORY_BASIC_INFORMATION buffer, SIZE_T length)
{
    if (!buffer || length < sizeof(*buffer)) return 0;
    memset(buffer, 0, sizeof(*buffer));

    void  *base = NULL;
    size_t len  = 0;

    if (!view_lookup(address, &base, &len)) {
        /* Not ours. Report it free rather than inventing a committed page,
         * up to the next mapping the shim knows of (or the end of the address
         * space). Win32 callers walk free memory region by region; a size of
         * 0 never moved them on (guest_vmem's free-run scan spun forever).
         * Mappings made outside the shim are invisible here, so a caller that
         * then reserves at a fixed address can still be refused. */
        uintptr_t next = view_next_above(address);
        uintptr_t a    = (uintptr_t)address;
        buffer->BaseAddress    = (PVOID)address;
        buffer->AllocationBase = NULL;
        /* At address 0 the span to the top is UINTPTR_MAX + 1, which wraps
         * to 0; report UINTPTR_MAX so the walk still moves on. */
        buffer->RegionSize     = next ? next - a
                               : a    ? UINTPTR_MAX - a + 1
                                      : UINTPTR_MAX;
        buffer->State          = MEM_FREE;
        buffer->Protect        = PAGE_NOACCESS;
        buffer->Type           = 0;
        return sizeof(*buffer);
    }

    buffer->BaseAddress      = (PVOID)address;
    buffer->AllocationBase   = base;
    buffer->AllocationProtect = PAGE_READWRITE;
    /* From the queried address to the end of the region, which is what Win32
     * reports and what callers sizing a copy out of it depend on. */
    buffer->RegionSize       = len - (size_t)((uintptr_t)address - (uintptr_t)base);
    buffer->State            = MEM_COMMIT;
    buffer->Protect          = PAGE_READWRITE;
    buffer->Type             = MEM_PRIVATE;
    return sizeof(*buffer);
}

BOOL GlobalMemoryStatusEx(LPMEMORYSTATUSEX b)
{
    if (!b) return FALSE;
#if defined(__APPLE__)
    /* Darwin has no sysinfo(2): physical memory comes from sysctl hw.memsize,
     * swap from vm.swapusage, the free page count from the Mach VM statistics. */
    uint64_t memsize = 0;
    size_t   len     = sizeof(memsize);
    int oid_memsize[] = { CTL_HW, HW_MEMSIZE };
    if (sysctl(oid_memsize, 2, &memsize, &len, NULL, 0) != 0) return FALSE;

    vm_size_t page = 0;
    if (host_page_size(mach_host_self(), &page) != KERN_SUCCESS) page = 4096;

    vm_statistics64_data_t vm;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    ULONGLONG avail = 0;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                          (host_info64_t)&vm, &count) == KERN_SUCCESS)
        avail = ((ULONGLONG)vm.free_count + vm.inactive_count) * (ULONGLONG)page;

    struct xsw_usage swap;
    len = sizeof(swap);
    int oid_swapusage[] = { CTL_VM, VM_SWAPUSAGE };
    if (sysctl(oid_swapusage, 2, &swap, &len, NULL, 0) != 0)
        memset(&swap, 0, sizeof(swap));

    b->ullTotalPhys     = (ULONGLONG)memsize;
    b->ullAvailPhys     = avail;
    b->ullTotalPageFile = b->ullTotalPhys + (ULONGLONG)swap.xsu_total;
    b->ullAvailPageFile = b->ullAvailPhys + (ULONGLONG)swap.xsu_avail;
#else
    struct sysinfo si;
    if (sysinfo(&si) != 0) return FALSE;

    ULONGLONG unit = si.mem_unit ? si.mem_unit : 1;
    b->ullTotalPhys     = (ULONGLONG)si.totalram  * unit;
    b->ullAvailPhys     = (ULONGLONG)si.freeram   * unit;
    b->ullTotalPageFile = b->ullTotalPhys + (ULONGLONG)si.totalswap * unit;
    b->ullAvailPageFile = b->ullAvailPhys + (ULONGLONG)si.freeswap  * unit;
#endif
    b->ullTotalVirtual  = b->ullTotalPhys;
    b->ullAvailVirtual  = b->ullAvailPhys;
    b->ullAvailExtendedVirtual = 0;
    b->dwMemoryLoad = b->ullTotalPhys
        ? (DWORD)(100 - (b->ullAvailPhys * 100 / b->ullTotalPhys)) : 0;
    return TRUE;
}

/* ===================================================================== */
/* Aligned allocation                                                     */
/* ===================================================================== */

void *_aligned_malloc(SIZE_T size, SIZE_T alignment)
{
    if (alignment < sizeof(void *)) alignment = sizeof(void *);
    /* round alignment up to a power of two */
    SIZE_T a = sizeof(void *);
    while (a < alignment) a <<= 1;
    void *p = NULL;
    if (posix_memalign(&p, a, size ? size : 1) != 0) return NULL;
    return p;
}

void _aligned_free(void *ptr) { free(ptr); }

/* ===================================================================== */
/* Case-insensitive string compare                                        */
/* ===================================================================== */

int _stricmp(const char *a, const char *b)            { return strcasecmp(a, b); }
int _strnicmp(const char *a, const char *b, SIZE_T n) { return strncasecmp(a, b, n); }

/* ===================================================================== */
/* Wide-string helpers (16-bit Xbox WCHAR)                                */
/* ===================================================================== */

SIZE_T xbox_wcslen(const WCHAR *s)
{
    SIZE_T n = 0;
    if (s) while (s[n]) n++;
    return n;
}

int xbox_wcsncmp(const WCHAR *a, const WCHAR *b, SIZE_T n)
{
    for (SIZE_T i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)a[i] - (int)b[i];
        if (a[i] == 0)    return 0;
    }
    return 0;
}

WCHAR *xbox_wcscat(WCHAR *dst, const WCHAR *src)
{
    SIZE_T d = xbox_wcslen(dst), i = 0;
    while (src[i]) { dst[d + i] = src[i]; i++; }
    dst[d + i] = 0;
    return dst;
}

WCHAR *xbox_wcscpy(WCHAR *dst, const WCHAR *src)
{
    SIZE_T i = 0;
    while (src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
    return dst;
}

/* ===================================================================== */
/* Time conversion                                                        */
/* ===================================================================== */

BOOL SystemTimeToFileTime(const SYSTEMTIME *st, LPFILETIME ft)
{
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = st->wYear - 1900;
    tm.tm_mon  = st->wMonth - 1;
    tm.tm_mday = st->wDay;
    tm.tm_hour = st->wHour;
    tm.tm_min  = st->wMinute;
    tm.tm_sec  = st->wSecond;
    time_t t = timegm(&tm);
    ULONGLONG ticks = FILETIME_EPOCH_DIFF
                    + (ULONGLONG)t * 10000000ULL
                    + (ULONGLONG)st->wMilliseconds * 10000ULL;
    ft->dwLowDateTime  = (DWORD)(ticks & 0xFFFFFFFFULL);
    ft->dwHighDateTime = (DWORD)(ticks >> 32);
    return TRUE;
}

BOOL FileTimeToSystemTime(const FILETIME *ft, LPSYSTEMTIME st)
{
    ULONGLONG ticks = ((ULONGLONG)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
    if (ticks < FILETIME_EPOCH_DIFF) { memset(st, 0, sizeof(*st)); return FALSE; }
    ULONGLONG since = ticks - FILETIME_EPOCH_DIFF;
    time_t t = (time_t)(since / 10000000ULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    fill_systemtime(st, &tm, (long)((since % 10000000ULL) * 100ULL));
    return TRUE;
}

/* ===================================================================== */
/* Exception handling (compile-shim -- SEH not yet emulated on Linux)     */
/* ===================================================================== */

VOID RtlUnwind(PVOID TargetFrame, PVOID TargetIp,
               PEXCEPTION_RECORD ExceptionRecord, PVOID ReturnValue)
{
    (void)TargetFrame; (void)TargetIp; (void)ExceptionRecord; (void)ReturnValue;
    /* TODO: Windows SEH unwinding is not yet emulated on Linux. */
}

VOID RaiseException(DWORD code, DWORD flags, DWORD nargs, const ULONG_PTR *args)
{
    (void)flags; (void)nargs; (void)args;
    fprintf(stderr, "[win32_compat] RaiseException(0x%08X): SEH not emulated\n", code);
    /* TODO: on Windows this does not return; SEH dispatch unimplemented. */
}

PVOID AddVectoredExceptionHandler(ULONG First, PVECTORED_EXCEPTION_HANDLER Handler)
{ (void)First; (void)Handler; return NULL; }   /* TODO: wire to sigaction */
ULONG RemoveVectoredExceptionHandler(PVOID h) { (void)h; return 1; }

#endif /* !_WIN32 */
