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
    s_esp = g_esp;              /* what xbox_SehRaiseHostFault would read */
    s_esi = g_esi;
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

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
#endif
