/* x360_hle_test -- the kernel HLE called directly (XBA-270): registers set as a title's call would set them, the
 * export run through x360_hle_call, its results and guest memory checked. Covers what the test titles do not reach:
 * the formatters, reserve-then-commit memory, physical memory, mutants, semaphore limits, pulses, timers, waits on
 * several objects, duplicated handles, suspend / resume, absolute timeouts, files written in the data directory and
 * symbolic links. Argument orders and structure layouts follow Xenia's kernel (src/xenia/kernel, BSD). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "x360_internal.h"
#include "xenos_cp.h"

static int failures;

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            failures++;                                                                                                \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

static x360_machine *X;
static uint32_t scratch; /* guest memory for the tests' arguments */
static uint32_t stack_top;

static xe_cpu *cpu(void) { return &x360_cur(X)->cpu; }

/* call an export with up to 10 arguments (the ninth and tenth in the caller's stack slots) */
static uint32_t callv(int lib, unsigned ord, int n, va_list ap)
{
    xe_cpu *c = cpu();
    c->gpr[1] = stack_top;
    for (int k = 0; k < n; k++) {
        uint32_t v = va_arg(ap, uint32_t);
        if (k < 8)
            c->gpr[3 + k] = v;
        else
            x360_w64(X, stack_top + 0x50 + 8 * (uint32_t)(k - 8), v);
    }
    x360_thunk th = {0, (uint16_t)ord, (uint8_t)lib};
    X->m.slice = 1000;
    x360_hle_call(X, &th);
    return (uint32_t)c->gpr[3];
}

static uint32_t K(unsigned ord, int n, ...)
{
    va_list ap;
    va_start(ap, n);
    uint32_t r = callv(X360_LIB_XBOXKRNL, ord, n, ap);
    va_end(ap);
    return r;
}

static uint32_t gstr(const char *s)
{
    uint32_t a = x360_kalloc(X, (uint32_t)strlen(s) + 1);
    memcpy(X->base + a, s, strlen(s) + 1);
    return a;
}

/* OBJECT_ATTRIBUTES {root, ANSI_STRING*, attributes} for a path */
static uint32_t attrs(const char *path)
{
    uint32_t s = gstr(path), ansi = x360_kalloc(X, 8), oa = x360_kalloc(X, 12);
    x360_w16(X, ansi, (uint32_t)strlen(path));
    x360_w16(X, ansi + 2, (uint32_t)strlen(path) + 1);
    x360_w32(X, ansi + 4, s);
    x360_w32(X, oa + 4, ansi);
    return oa;
}

/* finish the waits that can finish now (what the scheduler does) */
static void settle(void)
{
    for (int k = 0; k < X->nthreads; k++)
        if (X->threads[k].state == TH_WAIT)
            x360_wait_check(X, &X->threads[k]);
}

static uint32_t wait_now(uint32_t handle) /* NtWaitForSingleObjectEx with a zero timeout, from the current thread */
{
    uint32_t zero = x360_kalloc(X, 8);
    K(X360_xboxkrnl_NtWaitForSingleObjectEx, 4, handle, 1, 0, zero);
    settle();
    return (uint32_t)cpu()->gpr[3];
}

static void test_formatters(void)
{
    uint32_t buf = x360_kalloc(X, 256);
    uint32_t r = K(X360_xboxkrnl_sprintf, 10, buf, gstr("%s|%5d|%-4x|%c|%%|%08X|%.3s|%u"), gstr("ab"),
                   (uint32_t)-42, 0xAB, 'z', 0xBEEF, gstr("abcdef"), 4000000000u, 0);
    const char *want = "ab|  -42|ab  |z|%|0000BEEF|abc|4000000000";
    CHECK(!strcmp((char *)X->base + buf, want), "sprintf gave \"%s\"", (char *)X->base + buf);
    CHECK(r == strlen(want), "sprintf returned %u", r);
    memset(X->base + buf, 'q', 16);
    r = K(X360_xboxkrnl__snprintf, 4, buf, 5, gstr("%d"), 123456);
    CHECK(r == 0xFFFFFFFFu && !memcmp(X->base + buf, "12345q", 6), "_snprintf truncation: %d \"%.6s\"", (int)r,
          (char *)X->base + buf);
    uint32_t va = x360_kalloc(X, 32);
    x360_w64(X, va, 7);
    x360_w64(X, va + 8, gstr("seven"));
    r = K(X360_xboxkrnl_vsprintf, 3, buf, gstr("%d=%s"), va);
    CHECK(!strcmp((char *)X->base + buf, "7=seven"), "vsprintf gave \"%s\"", (char *)X->base + buf);
}

static void test_memory(void)
{
    uint32_t base_p = x360_kalloc(X, 8), size_p = base_p + 4;
    x360_w32(X, base_p, 0);
    x360_w32(X, size_p, 0x10000);
    CHECK(K(X360_xboxkrnl_NtAllocateVirtualMemory, 5, base_p, size_p, 0x2000, 4, 0) == 0, "reserve");
    uint32_t base = x360_r32(X, base_p);
    CHECK(base >= 0x10000 && base < 0x40000000, "reserve at %08X", base);
    x360_w32(X, base_p, base + 0x4321);
    x360_w32(X, size_p, 0x2000);
    uint32_t st = K(X360_xboxkrnl_NtAllocateVirtualMemory, 5, base_p, size_p, 0x1000, 4, 0);
    CHECK(st == 0 && x360_r32(X, base_p) == base + 0x4000, "commit inside the reservation: %08X at %08X", st,
          x360_r32(X, base_p));
    X->base[base + 0x5000] = 1; /* committed: writable */
    x360_w32(X, base_p, base);
    CHECK(K(X360_xboxkrnl_NtFreeVirtualMemory, 4, base_p, size_p, 0x8000, 0) == 0 &&
              x360_r32(X, size_p) == 0x10000, "release");
    uint32_t p = K(X360_xboxkrnl_MmAllocatePhysicalMemoryEx, 6, 0, 0x2000, 4, 0, 0xFFFFFFFFu, 0);
    CHECK(p >= 0xE0000000u, "physical 4 KB view: %08X", p);
    CHECK(K(X360_xboxkrnl_MmGetPhysicalAddress, 1, p) == (p & 0x1FFFFFFFu), "physical address");
    CHECK(K(X360_xboxkrnl_MmQueryAllocationSize, 1, p) == 0x2000, "allocation size");
    X->base[p + 5] = 0x77;
    CHECK(X->base[(p & 0x1FFFFFFFu) + 0xA0000000u + 5] == 0x77, "mirror");
    uint32_t q = K(X360_xboxkrnl_MmAllocatePhysicalMemoryEx, 6, 0, 0x10000, 0x20000004u, 0, 0xFFFFFFFFu, 0);
    CHECK(q >= 0xA0000000u && q < 0xC0000000u && !(q & 0xFFFF), "physical 64 KB view: %08X", q);
}

static void test_objects(void)
{
    uint32_t hp = x360_kalloc(X, 16);
    /* mutant: owned at creation, taken again by its owner, released twice, then not owned */
    CHECK(K(X360_xboxkrnl_NtCreateMutant, 3, hp, 0, 1) == 0, "NtCreateMutant");
    uint32_t mh = x360_r32(X, hp);
    CHECK(wait_now(mh) == 0, "owner waits on its mutant");
    CHECK(K(X360_xboxkrnl_NtReleaseMutant, 2, mh, 0) == 0, "release 1");
    CHECK(K(X360_xboxkrnl_NtReleaseMutant, 2, mh, 0) == 0, "release 2");
    CHECK(K(X360_xboxkrnl_NtReleaseMutant, 2, mh, 0) == X_STATUS_MUTANT_NOT_OWNED, "release 3: not owned");
    /* semaphore: count 1, limit 2 */
    CHECK(K(X360_xboxkrnl_NtCreateSemaphore, 4, hp, 0, 1, 2) == 0, "NtCreateSemaphore");
    uint32_t sh = x360_r32(X, hp), prev = x360_kalloc(X, 4);
    CHECK(K(X360_xboxkrnl_NtReleaseSemaphore, 3, sh, 1, prev) == 0 && x360_r32(X, prev) == 1, "release to 2");
    CHECK(K(X360_xboxkrnl_NtReleaseSemaphore, 3, sh, 1, prev) == X_STATUS_SEMAPHORE_LIMIT_EXCEEDED, "limit");
    CHECK(wait_now(sh) == 0 && wait_now(sh) == 0 && wait_now(sh) == X_STATUS_TIMEOUT, "two takes, then empty");
    /* duplicated handle */
    CHECK(K(X360_xboxkrnl_NtCreateEvent, 4, hp, 0, 0, 0) == 0, "NtCreateEvent");
    uint32_t eh = x360_r32(X, hp);
    CHECK(K(X360_xboxkrnl_NtDuplicateObject, 3, eh, hp + 4, 1) == 0, "NtDuplicateObject");
    uint32_t dh = x360_r32(X, hp + 4);
    CHECK(dh != eh && x360_handle_obj(X, dh) && x360_handle_obj(X, eh) == 0, "duplicate kept, source closed");
    K(X360_xboxkrnl_NtSetEvent, 2, dh, 0);
    CHECK(wait_now(dh) == 0 && wait_now(dh) == 0, "notification event stays set");
    /* wait on several objects (Ke*, objects by address) */
    uint32_t e1 = x360_kalloc(X, 16), e2 = x360_kalloc(X, 16), objs = x360_kalloc(X, 8), zero = x360_kalloc(X, 8);
    K(X360_xboxkrnl_KeInitializeEvent, 3, e1, 1, 0);
    K(X360_xboxkrnl_KeInitializeEvent, 3, e2, 1, 1);
    x360_w32(X, objs, e1);
    x360_w32(X, objs + 4, e2);
    K(X360_xboxkrnl_KeWaitForMultipleObjects, 8, 2, objs, 1, 0, 1, 0, zero, 0);
    settle();
    CHECK(cpu()->gpr[3] == 1 && x360_r32(X, e2 + 4) == 0, "wait any: the second, auto-reset");
    K(X360_xboxkrnl_KeSetEvent, 3, e2, 0, 0);
    K(X360_xboxkrnl_KeWaitForMultipleObjects, 8, 2, objs, 0, 0, 1, 0, zero, 0);
    settle();
    CHECK(cpu()->gpr[3] == X_STATUS_TIMEOUT && x360_r32(X, e2 + 4) == 1, "wait all times out, takes nothing");
}

static void test_threads_and_time(void)
{
    uint32_t hp = x360_kalloc(X, 16);
    CHECK(K(X360_xboxkrnl_ExCreateThread, 7, hp, 0, hp + 4, 0, 0x82001000u, 0x1234, 0) == 0, "ExCreateThread");
    x360_thread *t1 = x360_thread_by_kthread(X, x360_handle_obj(X, x360_r32(X, hp)));
    CHECK(t1 && t1->cpu.gpr[3] == 0x1234 && t1->cpu.pc == 0x82001000u && t1->state == TH_READY, "the new thread");
    if (!t1)
        return;
    CHECK(t1->cpu.gpr[13] == t1->pcr && x360_r32(X, t1->pcr + 0x100) == t1->kthread, "r13 is its PCR");
    uint32_t th = x360_r32(X, hp), prev = x360_kalloc(X, 4);
    CHECK(K(X360_xboxkrnl_NtSuspendThread, 2, th, prev) == 0 && t1->state == TH_SUSPENDED, "suspend");
    CHECK(K(X360_xboxkrnl_NtResumeThread, 2, th, prev) == 0 && x360_r32(X, prev) == 1 && t1->state == TH_READY,
          "resume");
    /* a pulse wakes the thread waiting now and leaves the event clear */
    K(X360_xboxkrnl_NtCreateEvent, 4, hp + 8, 0, 0, 0);
    uint32_t eh = x360_r32(X, hp + 8);
    int me = X->cur;
    X->cur = (int)(t1 - X->threads);
    K(X360_xboxkrnl_NtWaitForSingleObjectEx, 4, eh, 1, 0, 0);
    CHECK(t1->state == TH_WAIT, "the second thread waits");
    X->cur = me;
    K(X360_xboxkrnl_NtPulseEvent, 2, eh, 0);
    CHECK(t1->state == TH_READY && t1->cpu.gpr[3] == 0, "pulsed awake");
    CHECK(wait_now(eh) == X_STATUS_TIMEOUT, "and the event is clear");
    /* timer: due in 1 ms */
    uint32_t due = x360_kalloc(X, 8);
    x360_w64(X, due, (uint64_t)-10000);
    CHECK(K(X360_xboxkrnl_NtCreateTimer, 3, hp + 12, 0, 1) == 0, "NtCreateTimer");
    uint32_t tm = x360_r32(X, hp + 12);
    CHECK(K(X360_xboxkrnl_NtSetTimerEx, 8, tm, due, 0, 1, 0, 0, 0, 0) == 0, "NtSetTimerEx");
    CHECK(wait_now(tm) == X_STATUS_TIMEOUT, "not due yet");
    X->time_skip += 20000;
    x360_timers_fire(X);
    CHECK(wait_now(tm) == 0, "due");
    /* an absolute timeout, against KeQuerySystemTime */
    uint32_t now = x360_kalloc(X, 8), when = x360_kalloc(X, 8);
    K(X360_xboxkrnl_KeQuerySystemTime, 1, now);
    x360_w64(X, when, x360_r64(X, now) + 5000);
    K(X360_xboxkrnl_NtWaitForSingleObjectEx, 4, eh, 1, 0, when);
    settle();
    CHECK(x360_cur(X)->state == TH_WAIT, "absolute timeout not reached");
    X->time_skip += 6000;
    settle();
    CHECK(x360_cur(X)->state == TH_READY && cpu()->gpr[3] == X_STATUS_TIMEOUT, "absolute timeout reached");
    /* TLS */
    uint32_t slot = K(X360_xboxkrnl_KeTlsAlloc, 0);
    K(X360_xboxkrnl_KeTlsSetValue, 2, slot, 0xABCD);
    CHECK(K(X360_xboxkrnl_KeTlsGetValue, 1, slot) == 0xABCD, "TLS");
}

static uint32_t XM(unsigned ord, int n, ...)
{
    va_list ap;
    va_start(ap, n);
    uint32_t r = callv(X360_LIB_XAM, ord, n, ap);
    va_end(ap);
    return r;
}

static void test_system(void)
{
    uint32_t p = x360_kalloc(X, 64);
    /* 2000-01-01 00:00:00.000, a Saturday: FILETIME 125911584000000000 */
    x360_w64(X, p, 125911584000000000ull);
    K(X360_xboxkrnl_RtlTimeToTimeFields, 2, p, p + 16);
    CHECK(x360_r16(X, p + 16) == 2000 && x360_r16(X, p + 18) == 1 && x360_r16(X, p + 20) == 1 &&
              x360_r16(X, p + 22) == 0 && x360_r16(X, p + 30) == 6, "time fields %u-%u-%u weekday %u",
          x360_r16(X, p + 16), x360_r16(X, p + 18), x360_r16(X, p + 20), x360_r16(X, p + 30));
    x360_w16(X, p + 16, 2026);
    x360_w16(X, p + 18, 11);
    x360_w16(X, p + 20, 18);
    x360_w16(X, p + 22, 7);
    x360_w16(X, p + 24, 30);
    x360_w16(X, p + 26, 15);
    x360_w16(X, p + 28, 250);
    CHECK(K(X360_xboxkrnl_RtlTimeFieldsToTime, 2, p + 16, p + 32) == 1, "time fields to time");
    K(X360_xboxkrnl_RtlTimeToTimeFields, 2, p + 32, p + 48);
    CHECK(!memcmp(X->base + p + 16, X->base + p + 48, 14), "round trip");
    K(X360_xboxkrnl_VdQueryVideoMode, 1, p);
    CHECK(x360_r32(X, p) == 1280 && x360_r32(X, p + 4) == 720 && x360_r32(X, p + 20) == 0x42700000u, "video mode");
    CHECK(XM(X360_xam_XamUserGetSigninState, 1, 0) == 1 && XM(X360_xam_XamUserGetSigninState, 1, 1) == 0, "sign-in");
    CHECK(XM(X360_xam_XamUserGetXUID, 3, 0, 1, p) == 0 && x360_r64(X, p) == 0xB13EBABEBABEBABEull, "xuid");
    CHECK(XM(X360_xam_XamUserGetName, 3, 0, p, 16) == 0 && !strcmp((char *)X->base + p, "Player"), "user name");
    CHECK(XM(X360_xam_XamInputGetState, 3, 0, 0, p) == 0x48F, "no controller yet");
    CHECK(XM(X360_xam_XamAlloc, 3, 0, 100, p) == 0 && x360_r32(X, p) >= 0x10000, "XamAlloc");
    CHECK(XM(X360_xam_XamFree, 1, x360_r32(X, p)) == 0, "XamFree");
}

static void test_gpu(void)
{
    uint32_t ring = K(X360_xboxkrnl_MmAllocatePhysicalMemoryEx, 6, 0, 0x2000, 4, 0, 0xFFFFFFFFu, 0);
    uint32_t misc = K(X360_xboxkrnl_MmAllocatePhysicalMemoryEx, 6, 0, 0x1000, 4, 0, 0xFFFFFFFFu, 0);
    uint32_t ring_phys = K(X360_xboxkrnl_MmGetPhysicalAddress, 1, ring);
    uint32_t misc_phys = misc & 0x1FFFFFFFu, fence = misc + 0x100, wb = misc + 0x104;
    K(X360_xboxkrnl_VdInitializeRingBuffer, 2, ring_phys, 10);
    K(X360_xboxkrnl_VdEnableRingBufferRPtrWriteBack, 2, misc_phys + 0x104, 6);
    /* a fence through EVENT_WRITE_SHD (8in32 endian mode), then the 64 words VdSwap fills */
    x360_w32(X, ring, 3u << 30 | 2u << 16 | (uint32_t)PM4_EVENT_WRITE_SHD << 8);
    x360_w32(X, ring + 4, 5);
    x360_w32(X, ring + 8, (misc_phys + 0x100) | 2);
    x360_w32(X, ring + 12, 0x1234);
    uint32_t args = x360_kalloc(X, 32);
    x360_w32(X, args, 0xA1000000u); /* front buffer */
    x360_w32(X, args + 4, 1280);
    x360_w32(X, args + 8, 720);
    K(X360_xboxkrnl_VdSwap, 10, ring + 16, args + 16, 0, 0, 0, args, args + 20, args + 24, args + 4, args + 8);
    /* a pixel shader's microcode, dumped once to a directory */
    char sdir[] = "/tmp/x360_shaders_XXXXXX";
    int have_dir = mkdtemp(sdir) != NULL;
    if (have_dir)
        x360_set_shader_dump(X, sdir);
    uint32_t sp = ring + 4 * (4 + 64);
    x360_w32(X, sp, 3u << 30 | (5u - 1) << 16 | (uint32_t)PM4_IM_LOAD_IMMEDIATE << 8);
    x360_w32(X, sp + 4, 1);
    x360_w32(X, sp + 8, 3);
    x360_w32(X, sp + 12, 0x11111111u);
    x360_w32(X, sp + 16, 0x22222222u);
    x360_w32(X, sp + 20, 0x33333333u);
    x360_w32(X, 0x7FC80000u + 4 * XENOS_REG_CP_RB_WPTR, 4 + 64 + 6);
    x360_gpu_poll(X);
    x360_stats st;
    x360_get_stats(X, &st);
    CHECK(x360_r32(X, fence) == 0x1234, "fence %08X", x360_r32(X, fence));
    CHECK(x360_r32(X, wb) == 74, "read pointer written back: %u", x360_r32(X, wb));
    if (have_dir) {
        uint8_t want[12] = {0x11, 0x11, 0x11, 0x11, 0x22, 0x22, 0x22, 0x22, 0x33, 0x33, 0x33, 0x33};
        char path[128];
        snprintf(path, sizeof path, "%s/ps_%08X.bin", sdir, x360_crc32(want, 12));
        FILE *f = fopen(path, "rb");
        uint8_t got[16];
        size_t n = f ? fread(got, 1, sizeof got, f) : 0;
        CHECK(n == 12 && !memcmp(got, want, 12), "shader dumped to %s (%zu bytes)", path, n);
        if (f)
            fclose(f);
        unlink(path);
        rmdir(sdir);
        x360_set_shader_dump(X, NULL);
    }
    CHECK(st.gpu_swaps == 1 && st.gpu_packets >= 3, "swap seen: %llu swaps, %llu packets",
          (unsigned long long)st.gpu_swaps, (unsigned long long)st.gpu_packets);
    /* the vertical blank interrupt runs the callback on its own thread */
    K(X360_xboxkrnl_VdSetGraphicsInterruptCallback, 2, 0x82001000u, 0x55);
    int before = X->nthreads;
    X->time_skip += 200000;
    x360_gpu_poll(X);
    x360_thread *irq = NULL;
    for (int k = 0; k < X->nthreads; k++)
        if (X->threads[k].interrupt && X->threads[k].state == TH_READY)
            irq = &X->threads[k];
    CHECK(irq && irq->cpu.pc == 0x82001000u && irq->cpu.gpr[3] == 0 && irq->cpu.gpr[4] == 0x55,
          "vblank interrupt thread (threads %d -> %d)", before, X->nthreads);
    /* ten thousand vertical blanks (about three minutes of virtual time) reuse that one context */
    int fired = 0, nthreads = X->nthreads;
    uint32_t kfree = X->kalloc_cur;
    for (int k = 0; k < 10000 && irq; k++) {
        if (irq->state == TH_READY) {
            fired++;
            x360_thread_exit(X, irq, 0); /* the callback returned */
        }
        X->time_skip += 166667;
        x360_gpu_poll(X);
    }
    CHECK(fired >= 9999 && X->nthreads == nthreads && X->kalloc_cur == kfree && X->status == X360_RUNNING,
          "vblanks: %d callbacks, threads %d -> %d", fired, nthreads, X->nthreads);
}

static void test_strict_imports(void)
{
    X->strict_imports = 1;
    K(X360_xboxkrnl_VdGetCurrentDisplayInformation, 1, 0);
    CHECK(X->status == X360_ERROR && strstr(X->error, "VdGetCurrentDisplayInformation"), "strict imports: %s",
          X->error);
    X->status = X360_RUNNING;
    X->strict_imports = 0;
}

static void test_files(const char *dir)
{
    x360_set_data_dir(X, dir);
    uint32_t hp = x360_kalloc(X, 8), iosb = x360_kalloc(X, 8), info = x360_kalloc(X, 64), buf = x360_kalloc(X, 64);
    uint32_t link = x360_r32(X, attrs("\\??\\save:") + 4), target = x360_r32(X, attrs("\\Device\\Harddisk0\\Partition1") + 4);
    CHECK(K(X360_xboxkrnl_ObCreateSymbolicLink, 2, link, target) == 0, "ObCreateSymbolicLink");
    /* NtCreateFile: create options are the ninth argument (stack) */
    uint32_t st = K(X360_xboxkrnl_NtCreateFile, 9, hp, 0xC0000000u, attrs("save:\\dir\\..\\x"), iosb, 0, 0, 0, 5,
                    0x40);
    CHECK(st == 0xC000003Au, "a path with .. is refused: %08X", st);
    st = K(X360_xboxkrnl_NtCreateFile, 9, hp, 0xC0000000u, attrs("save:\\test.bin"), iosb, 0, 0, 0, 5, 0x40);
    CHECK(st == 0 && x360_r32(X, iosb + 4) == 2, "created: %08X, information %u", st, x360_r32(X, iosb + 4));
    uint32_t h = x360_r32(X, hp);
    memcpy(X->base + buf, "hello world", 11);
    CHECK(K(X360_xboxkrnl_NtWriteFile, 8, h, 0, 0, 0, iosb, buf, 11, 0) == 0 && x360_r32(X, iosb + 4) == 11,
          "NtWriteFile");
    CHECK(K(X360_xboxkrnl_NtQueryInformationFile, 5, h, iosb, info, 8, 14) == 0 && x360_r64(X, info) == 11,
          "position after the write");
    x360_w64(X, info, 6);
    CHECK(K(X360_xboxkrnl_NtSetInformationFile, 5, h, iosb, info, 8, 14) == 0, "seek");
    memset(X->base + buf, 0, 64);
    CHECK(K(X360_xboxkrnl_NtReadFile, 8, h, 0, 0, 0, iosb, buf, 64, 0) == 0 && x360_r32(X, iosb + 4) == 5 &&
              !memcmp(X->base + buf, "world", 5), "read from the position");
    CHECK(K(X360_xboxkrnl_NtReadFile, 8, h, 0, 0, 0, iosb, buf, 64, 0) == 0xC0000011u, "end of file");
    CHECK(K(X360_xboxkrnl_NtClose, 1, h) == 0, "close");
    CHECK(K(X360_xboxkrnl_NtQueryFullAttributesFile, 2, attrs("\\Device\\Harddisk0\\Partition1\\test.bin"), info) ==
              0 && x360_r64(X, info + 40) == 11 && x360_r32(X, info + 48) == 0x80, "full attributes");
    CHECK(K(X360_xboxkrnl_NtOpenFile, 5, hp, 1, attrs("game:\\nothing"), iosb, 0x40) == 0xC000003Au,
          "no disc: path not found");
    char host[600];
    snprintf(host, sizeof host, "%s/test.bin", dir);
    FILE *f = fopen(host, "rb");
    char got[16] = "";
    CHECK(f && fread(got, 1, 11, f) == 11 && !memcmp(got, "hello world", 11), "the host file");
    if (f)
        fclose(f);
    unlink(host);
}

int main(void)
{
    char why[256] = "";
    X = x360_create_bare(why, sizeof why);
    if (!X) {
        printf("cannot create the machine: %s\n", why);
        return 1;
    }
    X->dbg = stdout;
    X->log = stdout;
    scratch = x360_heap_alloc(X, &X->heap_v4k, 0x10000, 0);
    stack_top = scratch + 0x8000;
    test_formatters();
    test_memory();
    test_objects();
    test_threads_and_time();
    test_system();
    test_gpu();
    test_strict_imports();
    char dir[] = "/tmp/x360_hle_XXXXXX";
    if (mkdtemp(dir)) {
        test_files(dir);
        rmdir(dir);
    } else {
        CHECK(0, "mkdtemp");
    }
    x360_destroy(X);
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "all HLE checks passed", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
