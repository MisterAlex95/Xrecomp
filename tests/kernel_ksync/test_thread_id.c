/*
 * PsCreateSystemThreadEx (255) and PsCreateSystemThread (254) return the new
 * thread's id through ThreadId, and it is the ETHREAD.UniqueThread (+0x12C)
 * the thread itself reads from fs:[0x28] (GetCurrentThreadId).
 *
 * Since every guest thread has a KTHREAD of its own (XBA-113) the ids differ
 * between threads, but ThreadId was never written: a title that keeps
 * CreateThread's lpThreadId and compares it with GetCurrentThreadId() on the
 * thread (Black, to tell which allocator arena a thread owns) saw a mismatch
 * and sent the thread's allocations to the wrong heap.
 *
 * Driven through the thunk table against a real guest window
 * (xbox_MemoryLayoutInit on the synthetic XBE), as in tests/kernel_thread_stacks.
 *
 *   1. The first 255 call in INLINE mode runs the routine on the caller:
 *      ThreadId is the caller's own id.
 *   2. A spawned worker (255, then 254): ThreadId is non-zero, differs from
 *      the creator's, is already written when the worker starts, and equals
 *      what the worker reads from fs:[0x28]+0x12C and from
 *      KeGetCurrentThread()+0x12C.
 *   3. Two workers get two ids.
 */
#include "kernel.h"
#include "xbox_memory_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef void (*recomp_func_t)(void);
static void routine(void);
#define ROUTINE_VA 0x00010000u
recomp_func_t recomp_lookup(uint32_t va) { return va == ROUTINE_VA ? routine : NULL; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }

extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
extern ptrdiff_t g_xbox_mem_offset;

#define G32(va) (*(volatile uint32_t *)((uintptr_t)(va) + g_xbox_mem_offset))

enum { ORD_CURTHREAD = 104, ORD_NTCLOSE = 187, ORD_NTWAIT = 233,
       ORD_PSCREATE = 254, ORD_PSCREATEEX = 255 };
static const uint32_t ORDS[] = { ORD_CURTHREAD, ORD_NTCLOSE, ORD_NTWAIT,
                                 ORD_PSCREATE, ORD_PSCREATEEX };
#define NORDS (sizeof ORDS / sizeof ORDS[0])

static int failures;
static uint32_t g_thunks, g_out;
static RECOMP_TLS uint32_t t_stack;

/* What a routine saw, one record per run at g_out + 0x20 * ctx2:
 *   +0 *ThreadId when it started   +4 fs:[0x28]+0x12C
 *   +8 KeGetCurrentThread()+0x12C  +C fs:[0x28] == KeGetCurrentThread()
 *   +10 done */
#define REC(i) (g_out + 0x20u * (uint32_t)(i))

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-66s %s", what, ok ? "PASS" : "FAIL");
    if (!ok && detail) printf(" -- %s", detail);
    putchar('\n');
    if (!ok) failures++;
}

/* stdcall on this thread's own scratch stack. */
static uint32_t call(uint32_t ordinal, int argc, const uint32_t *argv)
{
    unsigned i;
    recomp_func_t fn = NULL;

    if (!t_stack)
        t_stack = xbox_HeapAlloc(0x1000, 4096) + 0x800;
    for (i = 0; i < NORDS; i++)
        if (ORDS[i] == ordinal)
            fn = recomp_lookup_kernel(G32(g_thunks + 4 * i));
    if (!fn) {
        printf("  ordinal %u did not resolve\n", ordinal);
        failures++;
        return 0xFFFFFFFFu;
    }
    G32(t_stack) = 0xBEEF0001u;
    for (i = 0; i < (unsigned)argc; i++)
        G32(t_stack + 4 + 4 * i) = argv[i];
    g_esp = t_stack;
    g_eax = 0xDEADBEEFu;
    fn();
    return g_eax;
}
#define CALL(ord, ...) call((ord), \
    (int)(sizeof((uint32_t[]){__VA_ARGS__}) / 4), (uint32_t[]){__VA_ARGS__})

/* The guest routine: VOID (PVOID ctx1 = &ThreadId, PVOID ctx2 = record). */
static void routine(void)
{
    uint32_t id_va = G32(g_esp + 4), rec = REC(G32(g_esp + 8));
    uint32_t esp = g_esp, kt_fs = g_fs_base ? G32(g_fs_base + 0x28) : 0, kt;

    G32(rec + 0) = G32(id_va);
    G32(rec + 4) = kt_fs ? G32(kt_fs + 0x12C) : 0;
    t_stack = 0;   /* a fresh scratch stack on a spawned thread */
    kt = CALL(ORD_CURTHREAD, 0);
    G32(rec + 8) = kt ? G32(kt + 0x12C) : 0;
    G32(rec + 12) = kt == kt_fs;
    __atomic_store_n((uint32_t *)((uintptr_t)(rec + 16) + g_xbox_mem_offset), 1,
                     __ATOMIC_SEQ_CST);
    g_esp = esp;
}

static int wait_done(int i)
{
    int t;
    for (t = 0; t < 10000; t++) {
        if (__atomic_load_n((uint32_t *)((uintptr_t)(REC(i) + 16) + g_xbox_mem_offset),
                            __ATOMIC_SEQ_CST))
            return 1;
        usleep(1000);
    }
    return 0;
}

/* One create; returns the ThreadId the caller got. */
static uint32_t create(int ex, int i, uint32_t *status)
{
    uint32_t hva = g_out + 0x800 + 4u * (uint32_t)i;
    uint32_t idva = g_out + 0x900 + 4u * (uint32_t)i;

    G32(hva) = 0; G32(idva) = 0;
    if (ex)
        /* ThreadHandle, ExtraSize, KStack, Tls, ThreadId, ctx1, ctx2,
         * CreateSuspended, DebugStack, StartRoutine */
        *status = CALL(ORD_PSCREATEEX, hva, 0, 0, 0, idva, idva, (uint32_t)i,
                       0, 0, ROUTINE_VA);
    else
        /* ThreadHandle, ThreadExtraSize, KernelStackSize, ThreadId, ctx1,
         * ctx2, StartRoutine */
        *status = CALL(ORD_PSCREATE, hva, 0, 0, idva, idva, (uint32_t)i,
                       ROUTINE_VA);
    return G32(idva);
}

static void join(int i)
{
    uint32_t h = G32(g_out + 0x800 + 4u * (uint32_t)i);
    if (h && h != 0xBEEF0001u && h != 0xBEEF0002u) {
        CALL(ORD_NTWAIT, h, 0, 0);
        CALL(ORD_NTCLOSE, h);
    }
}

static unsigned char *load(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    unsigned char *b;
    long len;

    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    rewind(f);
    b = malloc((size_t)len);
    if (b && fread(b, 1, (size_t)len, f) != (size_t)len) { free(b); b = NULL; }
    fclose(f);
    *n = (size_t)len;
    return b;
}

static void spawned(int ex, int i, uint32_t creator_id, const char *name)
{
    char what[100], d[200];
    uint32_t st, id = create(ex, i, &st);
    int done = wait_done(i);

    join(i);
    snprintf(d, sizeof d, "status 0x%X done %d ThreadId 0x%X creator 0x%X at"
             " start 0x%X fs 0x%X KeGetCurrentThread 0x%X same-kt %u",
             st, done, id, creator_id, G32(REC(i)), G32(REC(i) + 4),
             G32(REC(i) + 8), G32(REC(i) + 12));
    snprintf(what, sizeof what, "%s worker: ThreadId == its UniqueThread, non-zero", name);
    check(st == 0 && done && id && id != creator_id && G32(REC(i)) == id
          && G32(REC(i) + 4) == id && G32(REC(i) + 8) == id && G32(REC(i) + 12),
          what, d);
}

int main(int argc, char **argv)
{
    char tmpl[] = "/tmp/kthreadidXXXXXX", cmd[300], d[200];
    uint32_t st, id, main_id, ids[2];
    size_t len = 0;
    unsigned char *xbe;
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (!mkdtemp(tmpl)) return 2;
    setenv("XDG_DATA_HOME", tmpl, 1);

    xbe = load(argc > 1 ? argv[1] : "tools/conformance/test.xbe", &len);
    if (!xbe || !xbox_MemoryLayoutInit(xbe, len)) {
        printf("cannot map guest memory (run tools/conformance/mkxbe.py)\n");
        return 2;
    }
    g_thunks = xbox_HeapAlloc(0x1000, 4096);
    g_out    = xbox_HeapAlloc(0x1000, 4096);
    memset((void *)((uintptr_t)g_out + g_xbox_mem_offset), 0, 0x1000);
    for (i = 0; i < (int)NORDS; i++)
        G32(g_thunks + 4 * i) = 0x80000000u | ORDS[i];
    G32(g_thunks + 4 * NORDS) = 0;
    xbox_kernel_set_thunk_address(g_thunks, NORDS + 1);
    xbox_kernel_bridge_init();

    main_id = G32(CALL(ORD_CURTHREAD, 0) + 0x12C);
    snprintf(d, sizeof d, "0x%X", main_id);
    check(main_id != 0, "the creating thread has a UniqueThread", d);

    /* 1. INLINE mode (the default): the first create runs on the caller. */
    id = create(1, 0, &st);
    snprintf(d, sizeof d, "status 0x%X ThreadId 0x%X caller 0x%X routine saw 0x%X/0x%X",
             st, id, main_id, G32(REC(0) + 4), G32(REC(0) + 8));
    check(st == 0 && G32(REC(0) + 16) && id == main_id && G32(REC(0) + 4) == main_id
          && G32(REC(0) + 8) == main_id,
          "255 first call, run inline: ThreadId is the caller's id", d);

    /* 2. Spawned workers. */
    spawned(1, 1, main_id, "255");
    spawned(0, 2, main_id, "254");

    /* 3. Two workers, two ids. */
    ids[0] = G32(g_out + 0x904);
    ids[1] = G32(g_out + 0x908);
    snprintf(d, sizeof d, "0x%X 0x%X", ids[0], ids[1]);
    check(ids[0] && ids[1] && ids[0] != ids[1], "two workers, two ids", d);

    snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmpl);
    (void)system(cmd);
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
