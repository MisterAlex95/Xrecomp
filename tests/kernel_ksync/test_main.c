/*
 * SLists, KeGetCurrentThread, kernel queues, device queues, read-write locks,
 * APCs and thread priorities (XBA-113).
 *
 * Every export here used to do nothing: Pop returned NULL and Push lost the
 * node, KeRemoveQueue returned at once, the read-write lock let everyone in,
 * KeGetCurrentThread returned 0. Each is driven the way a title drives it,
 * through the thunk table, with a buffer standing in for guest memory (as in
 * tests/kernel_io_apc), and checked against the console's layout of the
 * structure it works on (nxdk xboxkrnl.h).
 *
 * Checked:
 *   1. SList, one thread: Push returns the old first entry, Pop is LIFO and
 *      keeps Depth/Sequence as the console does, Flush hands back the chain.
 *   2. SList, 4 threads pushing and popping 200 000 times each over a shared
 *      list: no node lost or duplicated, Depth right at the end.
 *   3. KeGetCurrentThread returns the KTHREAD fs:[0x28] names, a different
 *      one on each thread, with the fields XAPI reads.
 *   4. KQUEUE: insert/insert-head/remove/rundown, SignalState, a zero timeout
 *      on an empty queue, a remover on another thread woken by an insert, and
 *      esp exact after the 3-argument KeRemoveQueue.
 *   4b. XBA-133, the concurrency limit (MaximumCount 1): while the thread
 *      that took an entry runs guest code it counts in CurrentCount and a
 *      second remover gets nothing; once that thread blocks in a kernel call
 *      the second one gets the next entry. RECOMP_QUEUE_LIMIT=0 (second
 *      ctest entry): every remover gets one, as before.
 *   5. KDEVICE_QUEUE: busy/idle, by-key order, remove-entry.
 *   6. ERWLOCK: init values, readers share, a writer waits for them and
 *      readers wait for it; 4 threads mixing both keep exclusion.
 *   7. KAPC: KeInitializeApc pops 7 arguments; a kernel APC to the calling
 *      thread runs at once; a user APC runs at the next alertable wait,
 *      which returns STATUS_USER_APC; a second insert of a queued APC fails.
 *   8. KeSetPriorityThread / KeSetPriorityProcess / KeBoostPriorityThread.
 *   9. XBA-115: a call of a route still in the stub table is counted, and
 *      its first call lands in the discovery file as {"t":"stub","ord":N}.
 */
#include "kernel.h"
#include "xbox_memory_layout.h"
#include "kernel_stubs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*recomp_func_t)(void);

extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
extern ptrdiff_t g_xbox_mem_offset;

static uint8_t *mem;
#define M32(va) (*(volatile uint32_t *)(mem + (va)))
#define M16(va) (*(volatile uint16_t *)(mem + (va)))
#define M8(va)  (*(volatile uint8_t  *)(mem + (va)))

static int failures;
static void check(int ok, const char *what, const char *detail)
{
    printf("  %-66s %s", what, ok ? "PASS" : "FAIL");
    if (!ok && detail) printf(" -- %s", detail);
    putchar('\n');
    if (!ok) failures++;
}

/* ── Guest layout ── */
enum {
    THUNK_VA  = 0x10000,
    STACK_VA  = 0x20000,          /* per thread: STACK_VA + i * 0x2000 */
    TIB_VA    = 0x40000,          /* per thread: TIB_VA + i * 0x100     */
    KT_VA     = 0x50000,          /* per thread: KT_VA + i * 0x200      */
    SLIST_VA  = 0x60000,          /* 8-aligned header */
    NODES_VA  = 0x61000,          /* 64 nodes of 8 bytes */
    QUEUE_VA  = 0x62000,
    QENT_VA   = 0x62100,          /* 8 entries of 8 bytes */
    DQ_VA     = 0x63000,
    DQE_VA    = 0x63100,          /* 8 entries of 16 bytes */
    RW_VA     = 0x64000,
    APC_VA    = 0x65000,
    APC2_VA   = 0x65100,
    TIMEOUT_VA = 0x66000,
    KPROC_VA  = 0x67000,
    ROUTINE_KR = 0x00070000,      /* "guest" addresses of APC routines */
    ROUTINE_NR = 0x00070010,
};

/* Slots in the thunk table. */
enum {
    S_PUSH, S_POP, S_FLUSH, S_CURTHREAD,
    S_QINIT, S_QINS, S_QINSHEAD, S_QREM, S_QRUNDOWN,
    S_DQINIT, S_DQINS, S_DQINSKEY, S_DQREM, S_DQREMKEY, S_DQREMENT,
    S_RWINIT, S_RWEXCL, S_RWSHARED, S_RWREL,
    S_APCINIT, S_APCINS, S_DELAY,
    S_SETPRI, S_SETPRIPROC, S_BOOST, S_STUB,
    S_COUNT
};
static uint32_t slot_ord[S_COUNT] = {
    58, 57, 56, 104,
    111, 117, 116, 136, 141,
    106, 115, 114, 134, 133, 135,
    18, 12, 13, 28,
    105, 118, 99,
    148, 147, 94, 0 /* S_STUB: the first ordinal of the stub table */,
};

static RECOMP_TLS int t_index;

/* Stack call: args on this thread's guest stack, after a return address. */
static uint32_t call(int slot, const uint32_t *args, int n)
{
    uint32_t sp = STACK_VA + (uint32_t)t_index * 0x2000 + 0x1000;
    int i;

    M32(sp) = 0xBEEF0000u | (uint32_t)slot;
    for (i = 0; i < n; i++)
        M32(sp + 4 + 4 * i) = args[i];
    g_esp = sp;
    g_eax = 0xDEADBEEFu;
    recomp_lookup_kernel(M32(THUNK_VA + 4 * slot))();
    return g_eax;
}

/* Fastcall: ecx, edx. */
static uint32_t fcall(int slot, uint32_t ecx, uint32_t edx)
{
    g_ecx = ecx; g_edx = edx;
    return call(slot, NULL, 0);
}

static uint32_t sp_after(int nargs)
{
    return STACK_VA + (uint32_t)t_index * 0x2000 + 0x1000 + 4 + 4 * nargs;
}

/* Give the calling host thread guest identity number i: its own TIB whose
 * fs:[0x28] names its own KTHREAD, as the loader / xbox_AllocThreadTib do. */
static void become_thread(int i)
{
    uint32_t tib = TIB_VA + (uint32_t)i * 0x100, kt = KT_VA + (uint32_t)i * 0x200;
    t_index = i;
    memset(mem + tib, 0, 0x40);
    memset(mem + kt, 0, 0x200);
    M32(tib + 0x00) = 0xFFFFFFFFu;
    M32(tib + 0x04) = STACK_VA + (uint32_t)i * 0x2000 + 0x2000;
    M32(tib + 0x08) = STACK_VA + (uint32_t)i * 0x2000;
    M32(tib + 0x28) = kt;
    xbox_InitGuestKthread(kt, 0x00ABC000u, M32(tib + 4), M32(tib + 8));
    g_fs_base = tib;
}

/* ── APC routines, as guest code ── */
static int kr_runs, nr_runs;
static uint32_t nr_ctx, nr_a1, nr_a2, kr_apc;

/* VOID KernelRoutine(PKAPC, PKNORMAL_ROUTINE*, PVOID*, PVOID*, PVOID*):
 * stdcall, 5 args. Changes SystemArgument2 to show the normal routine sees
 * what the kernel routine left. */
static void test_kernel_routine(void)
{
    kr_apc = M32(g_esp + 4);
    M32(M32(g_esp + 20)) = 0x5A5A5A5Au;
    kr_runs++;
    g_esp += 4 + 20;
}

/* VOID NormalRoutine(PVOID NormalContext, PVOID Arg1, PVOID Arg2). */
static void test_normal_routine(void)
{
    nr_ctx = M32(g_esp + 4);
    nr_a1 = M32(g_esp + 8);
    nr_a2 = M32(g_esp + 12);
    nr_runs++;
    g_esp += 4 + 12;
}

recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va)
{
    if (xbox_va == ROUTINE_KR) return test_kernel_routine;
    if (xbox_va == ROUTINE_NR) return test_normal_routine;
    return NULL;
}
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

/* ═══════════════════ 1-2. SList ═══════════════════ */

#define NODES 64
#define SLIST_ITERS 200000

static DWORD WINAPI slist_worker(LPVOID p)
{
    int i, idx = (int)(intptr_t)p;
    uint32_t held[4];
    int nheld = 0;

    become_thread(idx);
    for (i = 0; i < SLIST_ITERS; i++) {
        if (nheld < 4 && (nheld == 0 || ((i * 7 + idx) & 1))) {
            uint32_t e = fcall(S_POP, SLIST_VA, 0);
            if (e) {
                /* Scribble the node's Next, as a user of the node would. */
                M32(e) = 0xCCCCCCCCu;
                held[nheld++] = e;
            }
        } else {
            fcall(S_PUSH, SLIST_VA, held[--nheld]);
        }
    }
    while (nheld)
        fcall(S_PUSH, SLIST_VA, held[--nheld]);
    return 0;
}

static void test_slist(void)
{
    char d[160];
    uint32_t r, a = NODES_VA, b = NODES_VA + 8, c = NODES_VA + 16;
    int i;

    become_thread(0);
    memset(mem + SLIST_VA, 0, 8);
    r = fcall(S_PUSH, SLIST_VA, a);
    check(r == 0 && M32(SLIST_VA) == a && M32(a) == 0,
          "SList: Push onto empty returns NULL, header names the entry", NULL);
    fcall(S_PUSH, SLIST_VA, b);
    r = fcall(S_PUSH, SLIST_VA, c);
    snprintf(d, sizeof d, "ret 0x%X depth %u seq %u", r, M16(SLIST_VA + 4), M16(SLIST_VA + 6));
    check(r == b && M16(SLIST_VA + 4) == 3 && M16(SLIST_VA + 6) == 3,
          "SList: Push returns the old first; Depth and Sequence count up", d);
    r = fcall(S_POP, SLIST_VA, 0);
    snprintf(d, sizeof d, "ret 0x%X depth %u seq %u", r, M16(SLIST_VA + 4), M16(SLIST_VA + 6));
    check(r == c && M16(SLIST_VA + 4) == 2 && M16(SLIST_VA + 6) == 3,
          "SList: Pop is LIFO, Depth down, Sequence unchanged", d);
    check(g_esp == sp_after(0), "SList: fastcall leaves esp past the return address", NULL);
    r = fcall(S_FLUSH, SLIST_VA, 0);
    check(r == b && M32(b) == a && M32(SLIST_VA) == 0 && M16(SLIST_VA + 4) == 0
          && M16(SLIST_VA + 6) == 3,
          "SList: Flush returns the chain, empties, keeps Sequence", NULL);
    r = fcall(S_POP, SLIST_VA, 0);
    check(r == 0, "SList: Pop of an empty list returns NULL", NULL);

    /* 4 threads. */
    memset(mem + SLIST_VA, 0, 8);
    for (i = 0; i < NODES; i++)
        fcall(S_PUSH, SLIST_VA, NODES_VA + 8u * i);
    {
        HANDLE th[4];
        uint8_t seen[NODES];
        int count = 0, dup = 0, bad = 0;
        uint32_t e;

        for (i = 0; i < 4; i++)
            th[i] = CreateThread(NULL, 0, slist_worker, (LPVOID)(intptr_t)(i + 1), 0, NULL);
        for (i = 0; i < 4; i++) {
            WaitForSingleObject(th[i], INFINITE);
            CloseHandle(th[i]);
        }
        memset(seen, 0, sizeof seen);
        snprintf(d, sizeof d, "depth %u", M16(SLIST_VA + 4));
        check(M16(SLIST_VA + 4) == NODES, "SList x4 threads: Depth is the node count", d);
        while ((e = fcall(S_POP, SLIST_VA, 0)) != 0 && count <= NODES) {
            uint32_t k = (e - NODES_VA) / 8;
            if (e < NODES_VA || k >= NODES || (e - NODES_VA) % 8) { bad++; break; }
            if (seen[k]++) dup++;
            count++;
        }
        snprintf(d, sizeof d, "%d nodes, %d duplicates, %d bad", count, dup, bad);
        check(count == NODES && !dup && !bad,
              "SList x4 threads x200000: every node back once, none lost", d);
    }
}

/* ═══════════════════ 3. KeGetCurrentThread ═══════════════════ */

static uint32_t worker_kt[4];

static DWORD WINAPI curthread_worker(LPVOID p)
{
    int idx = (int)(intptr_t)p;
    become_thread(idx);
    worker_kt[idx - 1] = call(S_CURTHREAD, NULL, 0);
    return 0;
}

static void test_current_thread(void)
{
    char d[200];
    uint32_t kt;
    int i, distinct = 1, match = 1;
    HANDLE th[4];

    become_thread(0);
    kt = call(S_CURTHREAD, NULL, 0);
    snprintf(d, sizeof d, "0x%08X, fs:[0x28] 0x%08X", kt, M32(g_fs_base + 0x28));
    check(kt && kt == M32(g_fs_base + 0x28),
          "KeGetCurrentThread returns the KTHREAD fs:[0x28] names", d);
    check(g_esp == sp_after(0), "KeGetCurrentThread: esp past the return address", NULL);
    snprintf(d, sizeof d, "type %u waitlist %d tls 0x%X state %u pri %d base %d"
             " apc %d/%d queueable %u exit 0x%X id 0x%X",
             M8(kt), M32(kt + 8) == kt + 8, M32(kt + 0x28), M8(kt + 0x2C),
             (int8_t)M8(kt + 0x32), (int8_t)M8(kt + 0x70),
             M32(kt + 0x34) == kt + 0x34, M32(kt + 0x3C) == kt + 0x3C,
             M8(kt + 0x4B), M32(kt + 0x120), M32(kt + 0x12C));
    check(M8(kt) == 6 && M32(kt + 8) == kt + 8 && M32(kt + 0x10) == kt + 0x10
          && M32(kt + 0x28) == 0x00ABC000u && M8(kt + 0x2C) == 2
          && M8(kt + 0x32) == 8 && M8(kt + 0x70) == 8
          && M32(kt + 0x34) == kt + 0x34 && M32(kt + 0x3C) == kt + 0x3C
          && M8(kt + 0x4B) == 1 && M32(kt + 0x120) == 0x103 && M32(kt + 0x12C) != 0
          && M32(kt + 0x1C) == M32(g_fs_base + 4),
          "KTHREAD: header, lists, TlsData, State, priorities, ApcState, ids", d);

    for (i = 0; i < 4; i++)
        th[i] = CreateThread(NULL, 0, curthread_worker, (LPVOID)(intptr_t)(i + 1), 0, NULL);
    for (i = 0; i < 4; i++) {
        WaitForSingleObject(th[i], INFINITE);
        CloseHandle(th[i]);
    }
    for (i = 0; i < 4; i++) {
        int j;
        if (worker_kt[i] != KT_VA + (uint32_t)(i + 1) * 0x200) match = 0;
        if (worker_kt[i] == kt) distinct = 0;
        for (j = 0; j < i; j++)
            if (worker_kt[i] == worker_kt[j]) distinct = 0;
    }
    for (i = 0; i < 4; i++)
        if (M32(worker_kt[i] + 0x12C) == M32(kt + 0x12C)) distinct = 0;
    snprintf(d, sizeof d, "0x%X 0x%X 0x%X 0x%X", worker_kt[0], worker_kt[1],
             worker_kt[2], worker_kt[3]);
    check(match && distinct,
          "KeGetCurrentThread: each thread its own KTHREAD and UniqueThread", d);
}

/* ═══════════════════ 4. KQUEUE ═══════════════════ */

static volatile uint32_t remover_got;

static DWORD WINAPI queue_remover(LPVOID p)
{
    uint32_t a[3] = { QUEUE_VA, 0, 0 };   /* infinite */
    (void)p;
    become_thread(5);
    remover_got = call(S_QREM, a, 3);
    return 0;
}

static void test_queue(void)
{
    char d[160];
    uint32_t a[3], r, e0 = QENT_VA, e1 = QENT_VA + 8, e2 = QENT_VA + 16, e3 = QENT_VA + 24;

    become_thread(0);
    memset(mem + QUEUE_VA, 0xEE, 0x28);
    a[0] = QUEUE_VA; a[1] = 0;
    call(S_QINIT, a, 2);
    snprintf(d, sizeof d, "type %u size %u signal %u max %u", M8(QUEUE_VA),
             M8(QUEUE_VA + 2), M32(QUEUE_VA + 4), M32(QUEUE_VA + 0x1C));
    check(M8(QUEUE_VA) == 4 && M8(QUEUE_VA + 2) == 10 && M32(QUEUE_VA + 4) == 0
          && M32(QUEUE_VA + 0x10) == QUEUE_VA + 0x10 && M32(QUEUE_VA + 0x18) == 0
          && M32(QUEUE_VA + 0x1C) == 1 && M32(QUEUE_VA + 0x20) == QUEUE_VA + 0x20,
          "KeInitializeQueue: QueueObject header, empty lists, MaximumCount 1", d);

    a[1] = e0; r = call(S_QINS, a, 2);
    check(r == 0, "KeInsertQueue returns the previous SignalState (0)", NULL);
    a[1] = e1; call(S_QINS, a, 2);
    a[1] = e2; r = call(S_QINSHEAD, a, 2);
    check(r == 2 && M32(QUEUE_VA + 4) == 3,
          "KeInsertHeadQueue returns 2; SignalState is 3", NULL);
    check(g_esp == sp_after(2), "KeInsertHeadQueue: esp past 2 arguments", NULL);

    a[1] = 0; a[2] = 0;
    r = call(S_QREM, a, 3);
    snprintf(d, sizeof d, "esp 0x%X expected 0x%X", g_esp, sp_after(3));
    check(g_esp == sp_after(3), "KeRemoveQueue: esp past 3 arguments", d);
    check(r == e2 && M32(e2) == 0, "KeRemoveQueue: head-inserted entry first, Flink cleared", NULL);
    r = call(S_QREM, a, 3);
    check(r == e0 && M32(QUEUE_VA + 4) == 1, "KeRemoveQueue: then FIFO, SignalState counts down", NULL);

    a[1] = e3; call(S_QINS, a, 2);
    r = call(S_QRUNDOWN, a, 1);
    check(r == e1 && M32(e1) == e3 && M32(e3) == e1
          && M32(QUEUE_VA + 0x10) == QUEUE_VA + 0x10 && M32(QUEUE_VA + 4) == 0,
          "KeRundownQueue: returns the chain without its head, queue empty", NULL);

    memset(mem + TIMEOUT_VA, 0, 8);
    a[1] = 0; a[2] = TIMEOUT_VA;
    r = call(S_QREM, a, 3);
    check(r == 0x102, "KeRemoveQueue: zero timeout on an empty queue is STATUS_TIMEOUT", NULL);
    *(int64_t *)(mem + TIMEOUT_VA) = -200000;   /* 20 ms */
    r = call(S_QREM, a, 3);
    check(r == 0x102, "KeRemoveQueue: a 20 ms timeout runs out", NULL);

    {
        HANDLE th;
        /* This thread took entries above, so it is one of the queue's
         * running threads; with MaximumCount 1 its insert would not let
         * the remover go until it blocks (XBA-133). A rundown ends that. */
        a[0] = QUEUE_VA;
        call(S_QRUNDOWN, a, 1);
        remover_got = 0;
        th = CreateThread(NULL, 0, queue_remover, NULL, 0, NULL);
        Sleep(50);
        check(remover_got == 0, "KeRemoveQueue on another thread blocks while empty", NULL);
        a[1] = e0; call(S_QINS, a, 2);
        r = WaitForSingleObject(th, 5000);
        CloseHandle(th);
        snprintf(d, sizeof d, "wait %u got 0x%X current %d", r, remover_got,
                 (int32_t)M32(QUEUE_VA + 0x18));
        check(r == 0 && remover_got == e0, "KeInsertQueue wakes the blocked remover with the entry", d);
    }
}

/* ═══════════════════ 4b. Concurrency limit ═══════════════════ */

static volatile int worker_took, worker_go_block, worker_blocked;
static volatile uint32_t worker_entry;

static DWORD WINAPI queue_worker(LPVOID p)
{
    uint32_t a[3] = { QUEUE_VA, 0, 0 };
    (void)p;
    become_thread(6);
    worker_entry = call(S_QREM, a, 3);
    worker_took = 1;
    while (!worker_go_block)            /* "guest code": no kernel call */
        Sleep(1);
    worker_blocked = 1;
    *(int64_t *)(mem + TIMEOUT_VA + 8) = -3000000;   /* 300 ms */
    a[0] = 0 /* KernelMode */; a[1] = 0; a[2] = TIMEOUT_VA + 8;
    call(S_DELAY, a, 3);
    return 0;
}

static void test_queue_limit(void)
{
    char d[160];
    uint32_t a[3], r, e0 = QENT_VA, e1 = QENT_VA + 8;
    int limit = getenv("RECOMP_QUEUE_LIMIT") == NULL;
    HANDLE th;
    DWORD t0;

    become_thread(0);
    a[0] = QUEUE_VA; a[1] = 1;
    call(S_QINIT, a, 2);
    a[1] = e0; call(S_QINS, a, 2);
    a[1] = e1; call(S_QINS, a, 2);
    worker_took = worker_go_block = worker_blocked = 0;
    th = CreateThread(NULL, 0, queue_worker, NULL, 0, NULL);
    for (t0 = 0; !worker_took && t0 < 5000; t0++)
        Sleep(1);
    snprintf(d, sizeof d, "entry 0x%X current %d", worker_entry, (int32_t)M32(QUEUE_VA + 0x18));
    check(worker_entry == e0 && M32(QUEUE_VA + 0x18) == (limit ? 1u : 1u),
          "a worker takes an entry and counts in CurrentCount", d);

    *(int64_t *)(mem + TIMEOUT_VA) = -1000000;       /* 100 ms */
    a[0] = QUEUE_VA; a[1] = 0; a[2] = TIMEOUT_VA;
    r = call(S_QREM, a, 3);
    snprintf(d, sizeof d, "got 0x%X", r);
    if (limit) {
        check(r == 0x102u, "while it runs, a second remover gets nothing (MaximumCount 1)", d);
        worker_go_block = 1;
        for (t0 = 0; !worker_blocked && t0 < 5000; t0++)
            Sleep(1);
        t0 = GetTickCount();
        a[2] = 0;                                     /* forever */
        r = call(S_QREM, a, 3);
        t0 = GetTickCount() - t0;
        snprintf(d, sizeof d, "got 0x%X after %lu ms", r, (unsigned long)t0);
        check(r == e1 && t0 < 250, "once it blocks in a kernel call, the next entry goes", d);
    } else {
        check(r == e1, "RECOMP_QUEUE_LIMIT=0: a second remover gets one at once", d);
        worker_go_block = 1;
    }
    WaitForSingleObject(th, 5000);
    CloseHandle(th);
    a[0] = QUEUE_VA;
    call(S_QRUNDOWN, a, 1);
}

/* ═══════════════════ 5. KDEVICE_QUEUE ═══════════════════ */

static void test_device_queue(void)
{
    uint32_t a[3], r, e[5];
    int i;

    become_thread(0);
    for (i = 0; i < 5; i++) e[i] = DQE_VA + 16u * i;
    a[0] = DQ_VA;
    call(S_DQINIT, a, 1);
    check(M16(DQ_VA) == 0x14 && M8(DQ_VA + 3) == 0 && M32(DQ_VA + 4) == DQ_VA + 4,
          "KeInitializeDeviceQueue: type, idle, empty list", NULL);
    a[1] = e[0]; r = call(S_DQINS, a, 2);
    check(r == 0 && M8(DQ_VA + 3) == 1 && M8(e[0] + 12) == 0,
          "KeInsertDeviceQueue on idle: FALSE, queue now busy, entry not inserted", NULL);
    a[1] = e[1]; a[2] = 30; call(S_DQINSKEY, a, 3);
    a[1] = e[2]; a[2] = 10; call(S_DQINSKEY, a, 3);
    a[1] = e[3]; a[2] = 20; r = call(S_DQINSKEY, a, 3);
    check(r == 1 && M8(e[3] + 12) == 1 && g_esp == sp_after(3),
          "KeInsertByKeyDeviceQueue on busy: TRUE, inserted, esp past 3 args", NULL);
    check(M32(DQ_VA + 4) == e[2] && M32(e[2]) == e[3] && M32(e[3]) == e[1],
          "KeInsertByKeyDeviceQueue keeps the list in key order", NULL);
    a[1] = 15; r = call(S_DQREMKEY, a, 2);
    check(r == e[3], "KeRemoveByKeyDeviceQueue: first entry with key >= 15", NULL);
    a[1] = e[1]; r = call(S_DQREMENT, a, 2);
    check(r == 1 && M8(e[1] + 12) == 0, "KeRemoveEntryDeviceQueue removes a queued entry", NULL);
    r = call(S_DQREMENT, a, 2);
    check(r == 0, "KeRemoveEntryDeviceQueue: FALSE for an entry not queued", NULL);
    r = call(S_DQREM, a, 1);
    check(r == e[2], "KeRemoveDeviceQueue: the remaining entry", NULL);
    r = call(S_DQREM, a, 1);
    check(r == 0 && M8(DQ_VA + 3) == 0, "KeRemoveDeviceQueue on empty: NULL, queue idle", NULL);
}

/* ═══════════════════ 6. ERWLOCK ═══════════════════ */

static volatile LONG rw_stage;

static DWORD WINAPI rw_writer(LPVOID p)
{
    uint32_t a[1] = { RW_VA };
    (void)p;
    become_thread(6);
    call(S_RWEXCL, a, 1);
    InterlockedExchange(&rw_stage, 1);
    Sleep(50);
    call(S_RWREL, a, 1);
    return 0;
}

static DWORD WINAPI rw_reader(LPVOID p)
{
    uint32_t a[1] = { RW_VA };
    (void)p;
    become_thread(7);
    call(S_RWSHARED, a, 1);
    InterlockedExchange(&rw_stage, 2);
    call(S_RWREL, a, 1);
    return 0;
}

static volatile LONG rw_inside_ex, rw_inside_sh, rw_violations;

static DWORD WINAPI rw_mixer(LPVOID p)
{
    int idx = (int)(intptr_t)p, i;
    uint32_t a[1] = { RW_VA };
    become_thread(idx);
    for (i = 0; i < 20000; i++) {
        if ((i + idx) % 4 == 0) {
            call(S_RWEXCL, a, 1);
            if (InterlockedIncrement(&rw_inside_ex) != 1 || rw_inside_sh != 0)
                InterlockedIncrement(&rw_violations);
            InterlockedDecrement(&rw_inside_ex);
            call(S_RWREL, a, 1);
        } else {
            call(S_RWSHARED, a, 1);
            InterlockedIncrement(&rw_inside_sh);
            if (rw_inside_ex != 0)
                InterlockedIncrement(&rw_violations);
            InterlockedDecrement(&rw_inside_sh);
            call(S_RWREL, a, 1);
        }
    }
    return 0;
}

static void test_rwlock(void)
{
    char d[160];
    uint32_t a[1] = { RW_VA };
    HANDLE th, th2, mx[4];
    int i;

    become_thread(0);
    memset(mem + RW_VA, 0xEE, 0x34);
    call(S_RWINIT, a, 1);
    check(M32(RW_VA) == 0xFFFFFFFFu && M32(RW_VA + 4) == 0 && M32(RW_VA + 0xC) == 0
          && M8(RW_VA + 0x10) == 1 && M8(RW_VA + 0x20) == 5
          && M32(RW_VA + 0x30) == 0x7FFFFFFFu,
          "ExInitializeReadWriteLock: LockCount -1, event and semaphore headers", NULL);
    call(S_RWSHARED, a, 1);
    call(S_RWSHARED, a, 1);
    check(M32(RW_VA) == 1 && M32(RW_VA + 0xC) == 2,
          "two shared acquires: LockCount 1, ReadersEntryCount 2", NULL);

    rw_stage = 0;
    th = CreateThread(NULL, 0, rw_writer, NULL, 0, NULL);
    Sleep(50);
    snprintf(d, sizeof d, "stage %ld writers waiting %u", rw_stage, M32(RW_VA + 4));
    check(rw_stage == 0 && M32(RW_VA + 4) == 1,
          "a writer waits while readers hold the lock", d);
    call(S_RWREL, a, 1);
    Sleep(20);
    check(rw_stage == 0, "the writer still waits after one of two readers leaves", NULL);
    call(S_RWREL, a, 1);
    Sleep(20);
    check(rw_stage == 1 && M32(RW_VA + 4) == 0,
          "the last reader leaving hands the lock to the writer", NULL);
    th2 = CreateThread(NULL, 0, rw_reader, NULL, 0, NULL);
    Sleep(20);
    check(rw_stage == 1 && M32(RW_VA + 8) == 1,
          "a reader waits while the writer holds it", NULL);
    WaitForSingleObject(th, 5000);
    WaitForSingleObject(th2, 5000);
    CloseHandle(th); CloseHandle(th2);
    snprintf(d, sizeof d, "stage %ld LockCount 0x%X", rw_stage, M32(RW_VA));
    check(rw_stage == 2 && M32(RW_VA) == 0xFFFFFFFFu,
          "the writer's release lets the reader in; the lock ends free", d);

    for (i = 0; i < 4; i++)
        mx[i] = CreateThread(NULL, 0, rw_mixer, (LPVOID)(intptr_t)(i + 1), 0, NULL);
    for (i = 0; i < 4; i++) {
        WaitForSingleObject(mx[i], INFINITE);
        CloseHandle(mx[i]);
    }
    snprintf(d, sizeof d, "%ld violations, LockCount 0x%X", rw_violations, M32(RW_VA));
    check(rw_violations == 0 && M32(RW_VA) == 0xFFFFFFFFu,
          "ERWLOCK x4 threads x20000: exclusion held, lock ends free", d);
}

/* ═══════════════════ 7. KAPC ═══════════════════ */

static void test_apc(void)
{
    char d[160];
    uint32_t kt, a[7], r;

    become_thread(0);
    kt = M32(g_fs_base + 0x28);

    /* Kernel-mode APC with a normal routine, to the calling thread. */
    a[0] = APC_VA; a[1] = kt; a[2] = ROUTINE_KR; a[3] = 0; a[4] = ROUTINE_NR;
    a[5] = 0 /* KernelMode */; a[6] = 0xC0C0u;
    call(S_APCINIT, a, 7);
    snprintf(d, sizeof d, "esp 0x%X expected 0x%X", g_esp, sp_after(7));
    check(g_esp == sp_after(7), "KeInitializeApc pops 7 arguments", d);
    check(M16(APC_VA) == 0x12 && M32(APC_VA + 4) == kt && M32(APC_VA + 0x10) == ROUTINE_KR
          && M32(APC_VA + 0x18) == ROUTINE_NR && M32(APC_VA + 0x1C) == 0xC0C0u
          && M8(APC_VA + 3) == 0,
          "KeInitializeApc fills the KAPC", NULL);
    a[1] = 0x1111u; a[2] = 0x2222u; a[3] = 0;
    r = call(S_APCINS, a, 4);
    snprintf(d, sizeof d, "ret %u kr %d nr %d ctx 0x%X a1 0x%X a2 0x%X", r, kr_runs,
             nr_runs, nr_ctx, nr_a1, nr_a2);
    check(r == 1 && kr_runs == 1 && kr_apc == APC_VA && nr_runs == 1 && nr_ctx == 0xC0C0u
          && nr_a1 == 0x1111u && nr_a2 == 0x5A5A5A5Au,
          "KeInsertQueueApc: a kernel APC to this thread runs at once", d);
    check(g_esp == sp_after(4) && M8(APC_VA + 3) == 0,
          "KeInsertQueueApc: esp past 4 arguments, APC no longer inserted", NULL);

    /* User-mode APC: waits for an alertable wait. */
    kr_runs = nr_runs = 0;
    a[0] = APC2_VA; a[1] = kt; a[2] = ROUTINE_KR; a[3] = 0; a[4] = ROUTINE_NR;
    a[5] = 1 /* UserMode */; a[6] = 0xD0D0u;
    call(S_APCINIT, a, 7);
    a[1] = 3; a[2] = 4; a[3] = 0;
    r = call(S_APCINS, a, 4);
    check(r == 1 && kr_runs == 0 && M8(APC2_VA + 3) == 1 && M8(kt + 0x4A) == 1
          && M32(kt + 0x3C) == APC2_VA + 8,
          "KeInsertQueueApc: a user APC is queued on ApcListHead[UserMode]", NULL);
    r = call(S_APCINS, a, 4);
    check(r == 0, "KeInsertQueueApc: inserting a queued APC again is FALSE", NULL);
    memset(mem + TIMEOUT_VA, 0, 8);
    {
        uint32_t w[3] = { 0 /* KernelMode */, 1, TIMEOUT_VA };
        call(S_DELAY, w, 3);
        check(kr_runs == 0, "a kernel-mode wait does not deliver it", NULL);
        w[0] = 1; w[1] = 0;
        call(S_DELAY, w, 3);
        check(kr_runs == 0, "a non-alertable wait does not deliver it", NULL);
        w[1] = 1;
        r = call(S_DELAY, w, 3);
        snprintf(d, sizeof d, "status 0x%X kr %d nr %d ctx 0x%X", r, kr_runs, nr_runs, nr_ctx);
        check(r == 0xC0 && kr_runs == 1 && nr_runs == 1 && nr_ctx == 0xD0D0u,
              "an alertable user-mode wait runs it and returns STATUS_USER_APC", d);
        check(g_esp == sp_after(3) && M8(kt + 0x4A) == 0
              && M32(kt + 0x3C) == kt + 0x3C,
              "...esp past the wait's 3 arguments, the user APC list empty", NULL);
    }
}

/* ═══════════════════ 8. Priorities ═══════════════════ */

static void test_priority(void)
{
    uint32_t kt, a[2], r;

    become_thread(0);
    kt = M32(g_fs_base + 0x28);
    a[0] = kt; a[1] = 10;
    r = call(S_SETPRI, a, 2);
    check(r == 8 && M8(kt + 0x32) == 10 && g_esp == sp_after(2),
          "KeSetPriorityThread: old priority back, KTHREAD.Priority set", NULL);
    a[1] = 4;
    call(S_BOOST, a, 2);
    check(M8(kt + 0x32) == 12, "KeBoostPriorityThread: Priority = Base + Increment", NULL);
    a[1] = 20;
    call(S_BOOST, a, 2);
    check(M8(kt + 0x32) == 15, "KeBoostPriorityThread stops below the real-time range", NULL);
    memset(mem + KPROC_VA, 0, 0x20);
    M8(KPROC_VA + 0x18) = 8;
    a[0] = KPROC_VA; a[1] = 9;
    r = call(S_SETPRIPROC, a, 2);
    check(r == 8 && M8(KPROC_VA + 0x18) == 9,
          "KeSetPriorityProcess: old base priority back, field set", NULL);
}

/* ═══════════════════ 9. Stub report (XBA-115) ═══════════════════ */

static void test_stub_report(const char *disc_path)
{
    char d[200], buf[4096];
    uint32_t ord = slot_ord[S_STUB];
    FILE *f;
    size_t n = 0;
    int i;

    become_thread(0);
    if (g_kernel_stub_count == 0) {
        /* XBA-138 implemented the last one: the table is a placeholder
         * entry counted out. No route is a stub, and a call of a routed
         * ordinal is neither counted nor reported. */
        for (i = 0; i < 3; i++)
            call(S_STUB, NULL, 0);
        check(!xbox_kernel_is_stub(ord) && !xbox_kernel_is_stub(0) && !xbox_kernel_is_stub(57)
              && xbox_kernel_stub_calls(ord) == 0,
              "stub table empty (0 stubs): nothing is a stub, nothing counted", NULL);
        f = fopen(disc_path, "rb");
        if (f) {
            n = fread(buf, 1, sizeof buf - 1, f);
            fclose(f);
        }
        buf[n] = 0;
        check(!strstr(buf, "\"t\":\"stub\""), "...and nothing in the discovery file", buf);
        return;
    }
    check(g_kernel_stub_count > 0 && xbox_kernel_is_stub(ord) && !xbox_kernel_is_stub(57),
          "stub table: generated, a stub is in it, InterlockedPopEntrySList is not", NULL);
    for (i = 0; i < 10; i++)
        call(S_STUB, NULL, 0);
    snprintf(d, sizeof d, "ordinal %u: %u calls", ord, xbox_kernel_stub_calls(ord));
    check(xbox_kernel_stub_calls(ord) == 10, "every call of a stub route is counted", d);
    check(xbox_kernel_stub_calls(57) == 0, "an implemented route is not", NULL);
    f = fopen(disc_path, "rb");
    if (f) {
        n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
    }
    buf[n] = 0;
    snprintf(d, sizeof d, "{\"t\":\"stub\",\"ord\":%u,\"name\":\"%s\",\"ret\":\"0xBEEF00%02X\"}\n",
             ord, xbox_kernel_stub_name(ord), S_STUB);
    check(strstr(buf, d) != NULL, "its first call is in the discovery file", buf);
    {
        const char *p = strstr(buf, "\"t\":\"stub\"");
        check(p && !strstr(p + 1, "\"t\":\"stub\""), "...once", NULL);
    }
}

int main(void)
{
    int i;
    char disc_path[256];

    setvbuf(stdout, NULL, _IONBF, 0);
    mem = VirtualAlloc(NULL, 16 * 1024 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!mem) {
        printf("guest memory stand-in not allocated\n");
        return 1;
    }
    g_xbox_mem_offset = (ptrdiff_t)mem;

    /* Discovery to a fresh file, for check 9. */
    {
        const char *tmp = getenv("TMPDIR");
        snprintf(disc_path, sizeof disc_path, "%s/xba_ksync_%u.jsonl",
                 tmp && *tmp ? tmp : "/tmp", (unsigned)GetCurrentProcessId());
        remove(disc_path);
#ifdef _WIN32
        _putenv_s("RECOMP_DISCOVERY", disc_path);
#else
        setenv("RECOMP_DISCOVERY", disc_path, 1);
#endif
    }
    slot_ord[S_STUB] = g_kernel_stub_count ? g_kernel_stub_table[0].ordinal : 1;

    for (i = 0; i < S_COUNT; i++)
        M32(THUNK_VA + 4 * i) = 0x80000000u | slot_ord[i];
    xbox_kernel_set_thunk_address(THUNK_VA, S_COUNT);
    xbox_kernel_init();
    xbox_kernel_bridge_init();

    test_slist();
    test_current_thread();
    test_queue();
    test_queue_limit();
    test_device_queue();
    test_rwlock();
    test_apc();
    test_priority();
    test_stub_report(disc_path);
    remove(disc_path);

    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
