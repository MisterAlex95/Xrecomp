/*
 * Fixed-address mappings in the POSIX Win32 shim.
 *
 * Win32 refuses a VirtualAlloc or MapViewOfFileEx at an address that is
 * already in use. mmap(MAP_FIXED) does the opposite: it unmaps whatever is
 * there and succeeds. Linux has MAP_FIXED_NOREPLACE for this; Darwin has
 * nothing, so the shim claims the range with mach_vm_map(VM_FLAGS_FIXED) and
 * only then maps over its own placeholder. These checks pin both halves:
 *
 *   1. refusal      a fixed request over a live mapping fails, and the bytes
 *                   that were there survive it
 *   2. aliasing     two views of one CreateFileMapping are the same memory,
 *                   which is what the 28 RAM mirror views are built on
 *   3. no residue   a fixed view that fails after the range was claimed gives
 *                   the range back, so the same address can be mapped again
 *   4. release      UnmapViewOfFile and VirtualFree(MEM_RELEASE) free the
 *                   address for reuse
 *   5. no exec      a PAGE_EXECUTE_* request (VirtualAlloc, VirtualProtect,
 *                   also on a view of a file mapping) succeeds with its read /
 *                   write access and without PROT_EXEC (XBA-51): nothing runs
 *                   guest memory, and an Android app may not get an
 *                   executable memfd or anonymous mapping
 *   6. lazy CS      8 threads released together enter one zeroed (never
 *                   initialised) CRITICAL_SECTION and increment a plain
 *                   counter: the total is exact, round after round (the
 *                   first-use creation of the mutex used to race, each thread
 *                   installing its own)
 *   7. dup thread   DuplicateHandle(GetCurrentThread()) on a raw pthread is a
 *                   real handle to an adopted thread object, shared with
 *                   w32_thread_apc_handle, signalled by ExitThread; never the
 *                   pseudo handle, which a save state cannot bind
 *
 * The addresses come from the OS: map anonymously, note where it landed,
 * unmap. Nothing here assumes a fixed host layout.
 */
#include "win32_compat.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

static int failures = 0;

#include <pthread.h>
#include <sched.h>

#define CS_THREADS 8
#define CS_ITERS   2000
static CRITICAL_SECTION g_lazy_cs;
static volatile int     g_go;
static volatile long    g_counter;

static void *cs_worker(void *arg)
{
    (void)arg;
    while (!__atomic_load_n(&g_go, __ATOMIC_ACQUIRE)) { }
    for (int i = 0; i < CS_ITERS; i++) {
        EnterCriticalSection(&g_lazy_cs);
        long v = g_counter;
        if ((i & 63) == 0) sched_yield();
        g_counter = v + 1;
        LeaveCriticalSection(&g_lazy_cs);
    }
    return NULL;
}

static void check(int ok, const char *what)
{
    printf("  %-60s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

/* 7. DuplicateHandle of the pseudo current-thread handle. */
#define TEST_STILL_ACTIVE 259u
typedef struct {
    HANDLE h;              /* the main thread's duplicate */
    DWORD  exit_code;
    DWORD  wait;
    BOOL   got_code;
} dup_probe;

static void *dup_probe_thread(void *arg)
{
    dup_probe *p = (dup_probe *)arg;
    p->got_code  = GetExitCodeThread(p->h, &p->exit_code);
    p->wait      = WaitForSingleObject(p->h, 0);
    SetThreadPriority(p->h, THREAD_PRIORITY_HIGHEST);
    return NULL;
}

/* A raw pthread that duplicates itself, hands the handle over and exits. */
static HANDLE volatile g_raw_dup;
static volatile int    g_raw_go;
static void *dup_raw_exit_thread(void *arg)
{
    HANDLE h = NULL;
    (void)arg;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                    &h, 0, FALSE, 0);
    __atomic_store_n(&g_raw_dup, h, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&g_raw_go, __ATOMIC_ACQUIRE)) sched_yield();
    usleep(20000);   /* let the waiter block first */
    ExitThread(0x1234);
    return NULL;
}

/* A CreateThread thread: its duplicate is its own handle, as before. */
static HANDLE volatile g_ct_dup;
static HANDLE volatile g_ct_cur;
static DWORD WINAPI dup_ct_thread(LPVOID arg)
{
    HANDLE h = NULL;
    (void)arg;
    g_ct_cur = GetCurrentThread();
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                    &h, 0, FALSE, 0);
    g_ct_dup = h;
    return 7;
}

#if defined(__linux__)
/* The permission letters /proc/self/maps shows for addr ("rw-p"), or "" when
 * no mapping covers it. */
static void maps_perms(const void *addr, char out[5])
{
    char line[512];
    FILE *f = fopen("/proc/self/maps", "r");
    out[0] = '\0';
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        char perms[5];
        if (sscanf(line, "%lx-%lx %4s", &lo, &hi, perms) == 3
                && (uintptr_t)addr >= lo && (uintptr_t)addr < hi) {
            memcpy(out, perms, 5);
            break;
        }
    }
    fclose(f);
}

static int not_exec(const void *addr)
{
    char perms[5];
    maps_perms(addr, perms);
    return perms[0] && perms[2] != 'x';
}
#else
static int not_exec(const void *addr) { (void)addr; return 1; }   /* no /proc: access checks only */
#endif

/* An address range nothing currently occupies. */
static void *free_range(size_t len)
{
    void *p = mmap(NULL, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return NULL;
    munmap(p, len);
    return p;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    const size_t len = 4 * (size_t)sysconf(_SC_PAGESIZE);

    HANDLE map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                    0, (DWORD)len, NULL);
    check(map != NULL, "CreateFileMapping returns a mapping");
    if (!map) return 1;

    /* 1. Refusal. A live anonymous mapping stands in for whatever else the
     *    process might have at the address a guest asks for. */
    unsigned char *live = mmap(NULL, len, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (live == MAP_FAILED) { perror("mmap"); return 1; }
    memset(live, 0xC3, len);

    void *v = MapViewOfFileEx(map, FILE_MAP_ALL_ACCESS, 0, 0, len, live);
    check(v == NULL, "MapViewOfFileEx over a live mapping fails");
    check(live[0] == 0xC3 && live[len - 1] == 0xC3,
          "...and the live mapping keeps its contents");

    void *a = VirtualAlloc(live, len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    check(a == NULL, "VirtualAlloc over a live mapping fails");
    check(live[0] == 0xC3 && live[len - 1] == 0xC3,
          "...and the live mapping keeps its contents");
    munmap(live, len);

    /* 2. Aliasing: a fixed view and a floating view of the same mapping. */
    unsigned char *at = free_range(len);
    unsigned char *v1 = MapViewOfFileEx(map, FILE_MAP_ALL_ACCESS, 0, 0, len, at);
    unsigned char *v2 = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, len);
    check(at && v1 == at, "MapViewOfFileEx maps at a free address");
    check(v2 != NULL, "MapViewOfFile maps a second view");
    if (v1 == at && v2) {
        v1[0x10] = 0x5A;
        v2[len - 1] = 0xA5;
        check(v2[0x10] == 0x5A && v1[len - 1] == 0xA5,
              "the two views alias the same memory");
    }

    /* 4. Release. Done before 3 so 3 starts from a known free address. */
    check(v1 && UnmapViewOfFile(v1), "UnmapViewOfFile releases the fixed view");
    unsigned char *again = MapViewOfFileEx(map, FILE_MAP_ALL_ACCESS, 0, 0, len, at);
    check(again == at, "the address can be mapped again after UnmapViewOfFile");
    if (again) UnmapViewOfFile(again);
    if (v2) UnmapViewOfFile(v2);

    /* 3. No residue. An offset that is not page aligned makes the mmap itself
     *    fail, after the range has been checked and, on Darwin, claimed. */
    void *bad = MapViewOfFileEx(map, FILE_MAP_ALL_ACCESS, 0, 1, len / 2, at);
    check(bad == NULL, "MapViewOfFileEx with an unaligned offset fails");
    unsigned char *after = MapViewOfFileEx(map, FILE_MAP_ALL_ACCESS, 0, 0, len, at);
    check(after == at, "...and leaves the address free for the next view");
    if (after) UnmapViewOfFile(after);

    a = VirtualAlloc(at, len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    check(a == at, "VirtualAlloc maps at a free address");
    check(a && VirtualFree(a, 0, MEM_RELEASE), "VirtualFree(MEM_RELEASE) releases it");
    a = VirtualAlloc(at, len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    check(a == at, "the address can be allocated again after VirtualFree");
    if (a) VirtualFree(a, 0, MEM_RELEASE);

    /* 5. No exec. The guest's execute protections keep their data access and
     *    lose the execute bit. */
    {
        DWORD old = 0;
        volatile unsigned char *x = VirtualAlloc(NULL, len, MEM_RESERVE | MEM_COMMIT,
                                                 PAGE_EXECUTE_READWRITE);
        check(x != NULL, "VirtualAlloc(PAGE_EXECUTE_READWRITE) succeeds");
        if (x) {
            x[0] = 0x5A;
            check(x[0] == 0x5A, "...and the memory is readable and writable");
            check(not_exec((const void *)x), "...without PROT_EXEC");
            check(VirtualProtect((LPVOID)x, len, PAGE_EXECUTE_READ, &old),
                  "VirtualProtect(PAGE_EXECUTE_READ) succeeds");
            check(x[0] == 0x5A && not_exec((const void *)x), "...readable, without PROT_EXEC");
            check(VirtualProtect((LPVOID)x, len, PAGE_EXECUTE, &old) && x[0] == 0x5A,
                  "VirtualProtect(PAGE_EXECUTE) leaves it readable");
            VirtualFree((LPVOID)x, 0, MEM_RELEASE);
        }
        unsigned char *v = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, len);
        check(v && VirtualProtect(v, len, PAGE_EXECUTE_READWRITE, &old),
              "VirtualProtect(PAGE_EXECUTE_READWRITE) on a mapping view succeeds");
        if (v) {
            v[1] = 0xA5;
            check(v[1] == 0xA5 && not_exec(v), "...read / write, without PROT_EXEC");
            UnmapViewOfFile(v);
        }
    }

    CloseHandle(map);

    /* 6. A zeroed CRITICAL_SECTION entered by several threads at once. */
    {
        int bad = 0;
        for (int round = 0; round < 50; round++) {
            pthread_t t[CS_THREADS];
            memset(&g_lazy_cs, 0, sizeof g_lazy_cs);
            g_counter = 0;
            g_go = 0;
            for (int i = 0; i < CS_THREADS; i++)
                pthread_create(&t[i], NULL, cs_worker, NULL);
            __atomic_store_n(&g_go, 1, __ATOMIC_RELEASE);
            for (int i = 0; i < CS_THREADS; i++)
                pthread_join(t[i], NULL);
            if (g_counter != (long)CS_THREADS * CS_ITERS) bad++;
            DeleteCriticalSection(&g_lazy_cs);
        }
        check(bad == 0, "zeroed CRITICAL_SECTION: first use from 8 threads at once excludes");
    }

    /* 7. DuplicateHandle(GetCurrentThread()) on a thread CreateThread did not
     *    start (this main thread, like the main guest thread) is a real
     *    handle to an adopted thread object, never the pseudo handle: a save
     *    state cannot bind the pseudo handle, and it names whichever thread
     *    uses it. */
    {
        HANDLE cur = GetCurrentThread();
        HANDLE d1 = NULL, d2 = NULL, apc;
        w32_state st;
        int kind;

        check(cur == (HANDLE)(LONG_PTR)-2, "main thread: GetCurrentThread is the pseudo handle");
        check(DuplicateHandle(GetCurrentProcess(), cur, GetCurrentProcess(), &d1, 0, FALSE, 0),
              "main thread: DuplicateHandle(GetCurrentThread()) succeeds");
        check(d1 && d1 != cur, "...and does not hand back the pseudo handle");
        memset(&st, 0, sizeof st);
        kind = w32_state_read(d1, &st);
        check(kind == W32_STATE_THREAD && st.tid == GetCurrentThreadId() && !st.exited,
              "...a live thread object with this thread's tid");
        check(DuplicateHandle(GetCurrentProcess(), cur, GetCurrentProcess(), &d2, 0, FALSE,
                              DUPLICATE_CLOSE_SOURCE) && d2 == d1,
              "a second duplicate (CLOSE_SOURCE) names the same object");
        apc = w32_thread_apc_handle();
        check(apc == d1, "...the one w32_thread_apc_handle returns");
        check(GetCurrentThread() == cur, "GetCurrentThread still answers the pseudo handle");
        CloseHandle(d2);
        CloseHandle(apc);
        memset(&st, 0, sizeof st);
        check(w32_state_read(d1, &st) == W32_STATE_THREAD,
              "closing duplicates leaves the object live");

        /* From another thread: running, unsignalled, and a priority set
         * through the duplicate is the one the pseudo handle reads. */
        {
            dup_probe p;
            pthread_t t;
            memset(&p, 0, sizeof p);
            p.h = d1;
            pthread_create(&t, NULL, dup_probe_thread, &p);
            pthread_join(t, NULL);
            check(p.got_code && p.exit_code == TEST_STILL_ACTIVE,
                  "another thread: GetExitCodeThread == STILL_ACTIVE");
            check(p.wait == WAIT_TIMEOUT, "another thread: WaitForSingleObject(h, 0) times out");
            check(GetThreadPriority(GetCurrentThread()) == THREAD_PRIORITY_HIGHEST,
                  "SetThreadPriority(dup) reads back through the pseudo handle");
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
            check(GetThreadPriority(d1) == THREAD_PRIORITY_LOWEST,
                  "SetThreadPriority(pseudo) reads back through the duplicate");
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
        }
        CloseHandle(d1);
    }

    /* 7b. A raw pthread duplicates itself and calls ExitThread: a waiter on
     *     the duplicate wakes, with the exit code. */
    {
        pthread_t t;
        HANDLE h;
        DWORD code = 0, w;
        g_raw_dup = NULL;
        g_raw_go  = 0;
        pthread_create(&t, NULL, dup_raw_exit_thread, NULL);
        while (!(h = __atomic_load_n(&g_raw_dup, __ATOMIC_ACQUIRE))) sched_yield();
        __atomic_store_n(&g_raw_go, 1, __ATOMIC_RELEASE);
        w = WaitForSingleObject(h, 5000);
        check(w == WAIT_OBJECT_0, "raw pthread: ExitThread wakes a waiter on its duplicate");
        check(GetExitCodeThread(h, &code) && code == 0x1234, "...with its exit code");
        pthread_join(t, NULL);
        CloseHandle(h);
    }

    /* 7c. A CreateThread thread: unchanged, its duplicate is its own handle. */
    {
        DWORD tid = 0, code = 0;
        HANDLE th = CreateThread(NULL, 0, dup_ct_thread, NULL, 0, &tid);
        check(th && WaitForSingleObject(th, 5000) == WAIT_OBJECT_0, "CreateThread thread ran");
        check(g_ct_cur == th && g_ct_dup == th,
              "CreateThread thread: GetCurrentThread and its duplicate are its handle");
        check(GetExitCodeThread(th, &code) && code == 7, "...exit code 7");
        CloseHandle(g_ct_dup);
        CloseHandle(th);
    }

    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
