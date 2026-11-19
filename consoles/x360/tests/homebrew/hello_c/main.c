/* hello_c -- a C test title for the Xbox 360 runtime (XBA-270), built with devkitPPC (powerpc-eabi-gcc -mcpu=970
 * -mpowerpc64 -maltivec: the 64-bit PowerPC instructions and VMX Xenon has, in a 32-bit ABI close enough to the
 * console's for 32-bit arguments) into a synthetic XEX2 by elf2xex.py. Recompiled and run under x360_run, what it
 * prints through DbgPrint must equal hello_c.txt. It exercises what a compiler gives a title: recursion, a switch
 * GCC turns into a jump table, calls through function pointers, double-precision maths, AltiVec vectors, structs
 * copied with memcpy / memset, two worker threads sharing a critical section and signalling events, a wait on both,
 * delays, TLS, virtual and physical memory (and the physical mirrors), the kernel's sprintf. */
/* devkitPPC ships no altivec.h: the vector type and GCC's AltiVec builtin directly */
typedef __vector float vector_float;

#include "../kernel.h"

void *memset(void *d, int c, unsigned n)
{
    u8 *p = d;
    while (n--)
        *p++ = (u8)c;
    return d;
}

void *memcpy(void *d, const void *s, unsigned n)
{
    u8 *p = d;
    const u8 *q = s;
    while (n--)
        *p++ = *q++;
    return d;
}

static CRITICAL_SECTION cs;
static volatile int counter;

typedef struct {
    int id, iterations, result;
    HANDLE done;
} job;

static u32 worker(void *arg)
{
    job *j = arg;
    LARGE_TIME pause = {-1, (u32)-2000}; /* 200 us */
    int sum = 0;
    for (int i = 0; i < j->iterations; i++) {
        RtlEnterCriticalSection(&cs);
        counter++;
        RtlLeaveCriticalSection(&cs);
        sum += i;
        if (i % 50 == 49)
            KeDelayExecutionThread(1, 0, &pause);
    }
    j->result = sum;
    NtSetEvent(j->done, 0);
    return (u32)j->id;
}

static int fib(int n)
{
    return n < 2 ? n : fib(n - 1) + fib(n - 2);
}

static int __attribute__((noinline)) classify(int v, int w)
{
    switch (v) { /* different code per case: a jump table (bctr), not a lookup table */
    case 0: return w + 11;
    case 1: return w * 40 + 2;
    case 2: return w - 7;
    case 3: return -w;
    case 4: return w << 4;
    case 5: return w / 3;
    case 6: return w ^ 0x55;
    case 7: return w % 5;
    default: return 0;
    }
}

static int add(int a, int b) { return a + b; }
static int sub(int a, int b) { return a - b; }
static int mul(int a, int b) { return a * b; }
static int quo(int a, int b) { return a / b; }
static int (*const ops[4])(int, int) = {add, sub, mul, quo};

static int __attribute__((noinline)) apply(int k, int a, int b)
{
    return ops[k & 3](a, b);
}

int title_main(void)
{
    DbgPrint("C title: hello from main()\n");
    DbgPrint("fib(20) = %d\n", fib(20));
    DbgPrint("switch:");
    for (int v = 0; v < 9; v++)
        DbgPrint(" %d", classify(v, 3 * v + 1));
    DbgPrint("\n");
    DbgPrint("ops: %d %d %d %d\n", apply(0, 7, 5), apply(1, 7, 5), apply(2, 7, 5), apply(3, 7, 5));

    double x = 1.0;
    for (int i = 0; i < 6; i++)
        x = (x + 2.0 / x) / 2.0;
    DbgPrint("sqrt(2) ~ %d / 1000000\n", (int)(x * 1000000.0));

    vector_float a = {1.0f, 2.0f, 3.0f, 4.0f}, b = {0.5f, 0.25f, 2.0f, -1.0f}, c = {10.0f, 10.0f, 10.0f, 10.0f};
    union {
        vector_float v;
        float f[4];
    } r;
    r.v = __builtin_altivec_vmaddfp(a, b, c);
    DbgPrint("vec_madd: %d %d %d %d\n", (int)(r.f[0] * 100), (int)(r.f[1] * 100), (int)(r.f[2] * 100),
             (int)(r.f[3] * 100));

    /* virtual memory, 64 KB pages */
    u32 base = 0, size = 0x20000;
    NTSTATUS st = NtAllocateVirtualMemory(&base, &size, MEM_COMMIT | MEM_RESERVE | MEM_LARGE_PAGES, PAGE_READWRITE, 0);
    int *mem = (int *)base;
    int check = 0;
    for (int i = 0; i < 1000; i++)
        mem[i] = (i * i) % 97;
    for (int i = 0; i < 1000; i++)
        check += mem[i];
    DbgPrint("virtual: status %d, %s, checksum %d\n", st, (base >> 28) == 4 ? "64 KB pages" : "other", check);
    NtFreeVirtualMemory(&base, &size, MEM_RELEASE, 0);

    /* physical memory through two of its views */
    u8 *phys = MmAllocatePhysicalMemoryEx(0, 0x10000, PAGE_READWRITE | MEM_LARGE_PAGES, 0, 0xFFFFFFFF, 0);
    /* 0xA... -> 0xE...: the 4 KB view starts one page into physical memory, the 64 KB view at its start */
    volatile u8 *mirror = (u8 *)((u32)phys + 0x40000000u - 0x1000u);
    phys[0x123] = 0x5A;
    DbgPrint("physical: %s view, mirror %s\n", ((u32)phys >> 28) == 0xA ? "64 KB" : "other",
             mirror[0x123] == 0x5A ? "agrees" : "differs");

    /* structs */
    job jobs[2];
    memset(jobs, 0, sizeof jobs);
    job proto = {1, 100, 0, 0};
    memcpy(&jobs[0], &proto, sizeof proto);
    jobs[1] = jobs[0];
    jobs[1].id = 2;
    jobs[1].iterations = 200;

    /* two threads */
    RtlInitializeCriticalSection(&cs);
    HANDLE th[2], ev[2];
    for (int k = 0; k < 2; k++) {
        NtCreateEvent(&ev[k], 0, 0, 0);
        jobs[k].done = ev[k];
        ExCreateThread(&th[k], 0, 0, 0, worker, &jobs[k], 0);
    }
    st = NtWaitForMultipleObjectsEx(2, ev, 0, 1, 0, 0);
    DbgPrint("workers: wait %d, counter %d, sums %d %d\n", st, counter, jobs[0].result, jobs[1].result);
    for (int k = 0; k < 2; k++) {
        NtWaitForSingleObjectEx(th[k], 1, 0, 0);
        NtClose(th[k]);
        NtClose(ev[k]);
    }

    u32 slot = KeTlsAlloc();
    KeTlsSetValue(slot, 0xC0FFEE);
    DbgPrint("tls: %X\n", KeTlsGetValue(slot));

    char buf[64];
    sprintf(buf, "%s-%d-%x", "abc", -5, 255);
    DbgPrint("sprintf: %s\n", buf);
    DbgPrint("main returns 0\n");
    return 0;
}

/* the entry point: no C library, so no CRT; title_main's value ends the thread (not called main: GCC's EABI
 * main calls __eabi, which would set r2 / r13, and r13 is the PCR on the console) */
__attribute__((section(".text.start"))) int _start(void)
{
    return title_main();
}
