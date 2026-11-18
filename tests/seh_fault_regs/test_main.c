/*
 * Guest registers in g_cpu at a host fault on a guest access (XBA-132,
 * XBA-138).
 *
 * Lifted code keeps the guest registers in g_cpu (thread-local memory), and a
 * host fault on a guest address is turned into a guest exception from what
 * g_cpu holds at that moment (xbox_seh_fault.c, xbox_SehRaiseHostFault). The
 * C compiler may keep register writes pending in host registers across a
 * memory access that, by the C aliasing rules, cannot touch g_cpu: a 16-bit,
 * 64-bit or floating-point access cannot alias a uint32_t, so
 * `esp -= 8; load16; esp -= 4` may be emitted as one store after the load.
 * A handler that faults there would then see esp 8 too high and esi stale.
 * Before XBA-138 every 16/64-bit and float case here failed (esp and esi
 * both stale, gcc -O2 x86-64); the access macros now use may_alias types.
 *
 * Each case below is the statement sequence lifted code has around such an
 * access, compiled at -O2 with the real access macros of
 * templates/runtime/recomp_types.h, faulting on a PROT_NONE page; the
 * SIGSEGV/SIGBUS handler reads g_cpu as the fault path does, then leaves by
 * siglongjmp.
 *
 * Loads: every register written before the access must be in g_cpu. Stores:
 * only esp is required (it is read after the store). esi, written before and
 * overwritten after without a read, is legitimately stale at a faulting
 * store of ANY width, MEM8/MEM32 included: C assumes a store cannot trap,
 * so the first write is dead. The output shows it; xbox_seh.c documents it.
 * POSIX only (exit 77 elsewhere); MSVC has no type-based aliasing to worry
 * about.
 *
 * SEH_TEST_CPU_SLOT (arm64 Linux): the same cases with g_cpu reached the way
 * Android's generated code reaches it, through bionic's TLS_SLOT_APP
 * (RECOMP_CPU_SLOT, XBA-273), while the handler reads g_cpu itself as the
 * runtime does. On glibc tp + 16 is the start of the executable's own TLS
 * block, so the first thread-local below stands in for the slot (checked).
 * Then the slot's own cases: bound lazily on the main thread and on new
 * threads, each to its own g_cpu, and the frame shadow macros through it.
 */
#include "recomp_types.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
int main(void)
{
    puts("POSIX only (sigaction + mmap); MSVC does not optimise on types");
    return 77;
}
#else
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#ifdef SEH_TEST_CPU_SLOT
#include <pthread.h>
/* Zero, but in .tdata, which comes before g_cpu's .tbss: the executable's first thread-local. */
__attribute__((section(".tdata"))) __thread void *seh_slot_standin = 0;
#endif
RECOMP_TLS RecompCpu g_cpu;
ptrdiff_t g_xbox_mem_offset;

#define ESP0   0x00100000u
#define ESI0   0x11111111u
#define ESI1   0x5E5E5E5Eu
#define BADVA  0x00000010u      /* inside the PROT_NONE page */

static sigjmp_buf s_env;
static volatile uint32_t s_esp, s_esi;
static volatile int s_faulted;
static int failures;

static void on_fault(int sig, siginfo_t *si, void *uc)
{
    (void)sig; (void)si; (void)uc;
    s_esp = g_cpu.r_esp;        /* what xbox_SehRaiseHostFault reads */
    s_esi = g_cpu.r_esi;
    s_faulted = 1;
    siglongjmp(s_env, 1);
}

/* Each: registers written, the faulting access, registers written again
 * (which is what lets the compiler merge or drop the first writes). */
#define CASE(name, access) \
    __attribute__((noinline)) static void name(uint32_t a) \
    { \
        g_esp -= 8; \
        g_esi = ESI1; \
        access; \
        g_esp -= 4; \
        g_esi = 0; \
    }

static volatile uint64_t sink;
CASE(load_mem16,   sink = MEM16(a))
CASE(load_smem16,  sink = (uint64_t)(int64_t)SMEM16(a))
CASE(load_smem64,  sink = (uint64_t)SMEM64(a))
CASE(load_memf,    sink = (uint64_t)MEMF(a))
CASE(load_memd,    sink = (uint64_t)MEMD(a))
CASE(store_mem16,  MEM16(a) = 0x1234)
CASE(store_smem64, SMEM64(a) = 0x123456789LL)
CASE(store_memf,   MEMF(a) = 1.5f)
CASE(store_memd,   MEMD(a) = 2.5)
CASE(load_mem32,   sink = MEM32(a))          /* control: always exact */
CASE(load_mem8,    sink = MEM8(a))           /* control: always exact */
CASE(store_mem32,  MEM32(a) = 0x1234)
CASE(store_mem8,   MEM8(a) = 0x12)

/* is_store: only esp is required exact (see the header comment). */
static void run(const char *what, void (*fn)(uint32_t), int is_store)
{
    g_esp = ESP0;
    g_esi = ESI0;
    s_faulted = 0;
    if (sigsetjmp(s_env, 1) == 0)
        fn(BADVA);
    if (!s_faulted) {
        printf("  %-14s no fault?                                    FAIL\n", what);
        failures++;
        return;
    }
    {
        int ok = s_esp == ESP0 - 8 && (is_store || s_esi == ESI1);
        printf("  %-14s esp at the fault 0x%08X (want 0x%08X), esi 0x%08X (%s 0x%08X)  %s\n",
               what, s_esp, ESP0 - 8, s_esi, is_store ? "x86 has" : "want", ESI1,
               ok ? "PASS" : "FAIL");
        if (!ok)
            failures++;
    }
}

#ifdef SEH_TEST_CPU_SLOT
/* A lifted function's shape: registers and a frame record through the slot. */
__attribute__((noinline)) static void slot_body(uint32_t v)
{
    RECOMP_FRAME_ENTRY();
    g_esp -= 4;
    g_eax = v;
    g_ebx += v;
    RECOMP_FRAME_LEAVE();
}

__attribute__((noinline)) static void slot_work(uint32_t v)
{
    g_esp = 0x1000u;
    slot_body(v);
}

/* Reads only g_cpu itself: the slot's asm is free to move within a function that uses it, so the slot is
 * looked at before and after a call into one. */
static void *slot_thread(void *arg)
{
    uintptr_t ok = 1;
    if (seh_slot_standin != NULL)
        ok = 0;                         /* a new thread's slot starts at zero */
    slot_work((uint32_t)(uintptr_t)arg);
    if (seh_slot_standin != (void *)&g_cpu || g_cpu.r_eax != (uint32_t)(uintptr_t)arg ||
        g_cpu.r_esp != 0x1000u - 4 || g_cpu.r_ebx != (uint32_t)(uintptr_t)arg ||
        !g_cpu.r_fbase || g_cpu.r_ftop != g_cpu.r_fbase)
        ok = 0;
    return (void *)ok;
}

static void slot_cases(void)
{
    pthread_t t[4];
    void *main_cpu = &g_cpu;
    int i;
    if (seh_slot_standin != main_cpu) {
        printf("FAIL slot: not bound to the main thread's g_cpu after the cases (%p, %p)\n",
               seh_slot_standin, main_cpu);
        failures++;
    }
    for (i = 0; i < 4; i++)
        pthread_create(&t[i], NULL, slot_thread, (void *)(uintptr_t)(0x100 + i));
    for (i = 0; i < 4; i++) {
        void *r = NULL;
        pthread_join(t[i], &r);
        if (!r) {
            printf("FAIL slot: thread %d did not see its own g_cpu through the slot\n", i);
            failures++;
        }
    }
    if (seh_slot_standin != main_cpu || g_cpu.r_eax == 0x100u) {
        printf("FAIL slot: the main thread's slot or g_cpu changed\n");
        failures++;
    }
    printf("slot cases done\n");
}
#endif

int main(void)
{
    struct sigaction sa;
    long page = sysconf(_SC_PAGESIZE);
    void *p = mmap(NULL, (size_t)page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    setvbuf(stdout, NULL, _IONBF, 0);
    if (p == MAP_FAILED) {
        puts("mmap failed");
        return 2;
    }
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)p;
#ifdef SEH_TEST_CPU_SLOT
    if ((void *)&seh_slot_standin != (void *)((void **)__builtin_thread_pointer() + RECOMP_CPU_SLOT_INDEX)) {
        puts("the slot's stand-in is not at tp + 16 (TLS layout): cannot test");
        return 2;
    }
    if (seh_slot_standin != NULL) {
        puts("FAIL slot: bound before any lifted code ran");
        return 1;
    }
#endif
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    run("MEM8 load", load_mem8, 0);
    run("MEM32 load", load_mem32, 0);
    run("MEM16 load", load_mem16, 0);
    run("SMEM16 load", load_smem16, 0);
    run("SMEM64 load", load_smem64, 0);
    run("MEMF load", load_memf, 0);
    run("MEMD load", load_memd, 0);
    run("MEM16 store", store_mem16, 1);
    run("SMEM64 store", store_smem64, 1);
    run("MEMF store", store_memf, 1);
    run("MEMD store", store_memd, 1);
    run("MEM32 store", store_mem32, 1);
    run("MEM8 store", store_mem8, 1);
#ifdef SEH_TEST_CPU_SLOT
    slot_cases();
#endif

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
#endif
