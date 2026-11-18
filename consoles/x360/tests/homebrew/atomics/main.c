/* atomics -- a C test title for guest threads on host threads (XBA-283), built with devkitPPC like hello_c. Four
 * threads add to shared counters with GCC's __sync builtins (lwarx / stwcx. loops, an ldarx / stdcx. one for the
 * 64-bit counter) and take a spin lock (__sync_lock_test_and_set) around a plain increment, many times each; the
 * totals must be exact whether the threads run cooperatively (x360_run) or in parallel (x360_run --host-threads).
 * atomics.txt is what it prints through DbgPrint. */
#include "../kernel.h"

#define THREADS 4
#define ROUNDS 200000

static volatile u32 counter;
static volatile unsigned long long counter64;
static volatile int lock;
static volatile u32 guarded;

static u32 worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < ROUNDS; i++) {
        __sync_fetch_and_add(&counter, 1);
        __sync_fetch_and_add(&counter64, 3ull);
        while (__sync_lock_test_and_set(&lock, 1))
            ;
        guarded = guarded + 1;
        __sync_lock_release(&lock);
    }
    return 0;
}

int title_main(void)
{
    HANDLE th[THREADS];
    for (int k = 0; k < THREADS; k++)
        ExCreateThread(&th[k], 0, 0, 0, worker, 0, 0);
    NTSTATUS st = NtWaitForMultipleObjectsEx(THREADS, th, 0, 1, 0, 0);
    DbgPrint("atomics: wait %d, counter %u, counter64 %u, guarded %u\n", (int)st, counter, (u32)counter64, guarded);
    for (int k = 0; k < THREADS; k++)
        NtClose(th[k]);
    return 0;
}

__attribute__((section(".text.start"))) int _start(void)
{
    return title_main();
}
