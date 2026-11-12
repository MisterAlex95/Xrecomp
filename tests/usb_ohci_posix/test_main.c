/*
 * The USB OHCI register model on a POSIX host, reached the way a title
 * reaches it: a load or store to the controller's aperture faults, the
 * process's SIGBUS/SIGSEGV handler hands the fault to xbox_OhciHandleMmio,
 * and the instruction completes with the model's value.
 *
 * Before this was ported the trap existed on Windows only: on POSIX the
 * aperture stayed plain zeroed memory and xbox_OhciHandleMmio returned 0, so
 * a title's USB driver found no controller and no pad.
 *
 * Then the guest's side of the lists (xbox_OhciCatchUp, K-8c): a driver
 * that has written ControlListFilled while the controller thread is overdue
 * has the control list walked and the USB line raised from its own thread.
 * The synthetic image has no TLS and the test sets RECOMP_TIB_PER_THREAD=0,
 * so the controller thread gets no TIB and stops at once; its tick never
 * runs and is therefore always overdue here. (Since XBA-113 a no-TLS image
 * gets a TIB per thread by default; the controller thread would then run
 * and race the catch-up this checks.)
 * Run a second time with RECOMP_OHCI_CATCHUP=0 (a second CTest entry), the
 * same calls must leave the list and the line alone.
 *
 * A host without a trapped-MMIO decoder (anything but POSIX arm64 or Linux
 * x86-64 here) keeps the model off by design; the test checks that it says
 * so and exits 77.
 */
#include "xbox_memory_layout.h"
#include "kernel.h"
#include "ohci.h"

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Supplied by recompiled game code in a real build; nothing here calls one. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(unsigned int va) { (void)va; return 0; }
recomp_func_t recomp_lookup_manual(unsigned int va) { (void)va; return 0; }

static int failures;
static uintptr_t s_base;
static volatile sig_atomic_t s_unhandled;

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-52s %s%s%s\n", what, ok ? "PASS" : "FAIL",
           detail && !ok ? " -- " : "", detail && !ok ? detail : "");
    if (!ok) failures++;
}

/* What a title's own fault handler does: hand the fault to the toolkit's
 * trapped-MMIO dispatch, which routes it to the OHCI model. */
static void on_fault(int sig, siginfo_t *si, void *ucv)
{
    if (xbox_PosixMmioFault(ucv, (uintptr_t)si->si_addr))
        return;
    s_unhandled = 1;
    signal(sig, SIG_DFL);           /* re-faults and dies: a FAIL by crash */
}

static unsigned char *load_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf = NULL;
    long n;

    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0) {
        rewind(f);
        buf = malloc((size_t)n);
        if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
            free(buf);
            buf = NULL;
        }
        *len = (size_t)n;
    }
    fclose(f);
    return buf;
}

/* ── The guest's side of the lists ─────────────────────────────────── */

#define CU_ED      0x00400000u
#define CU_TD0     0x00400100u
#define CU_TD1     0x00400110u
#define CU_HCCA    0x00401000u

static volatile long s_line_runs;

static void count_line(void)
{
    __atomic_add_fetch(&s_line_runs, 1, __ATOMIC_RELAXED);
}

/* A guest thread's raise and lower: the lower's release point runs what is
 * pending on the USB line. */
static void *release_point_thread(void *unused)
{
    int saved;

    (void)unused;
    xbox_DpcMarkGuestThread();
    saved = xbox_IrqlEnterInterrupt(DISPATCH_LEVEL);
    xbox_IrqlLeaveInterrupt(saved);
    return NULL;
}

static long run_release_point(void)
{
    pthread_t t;
    long before = s_line_runs;

    xbox_IrqSetHandler(XBOX_IRQ_OHCI, count_line);
    if (pthread_create(&t, NULL, release_point_thread, NULL) == 0)
        pthread_join(t, NULL);
    return s_line_runs - before;
}

static void catch_up(volatile uint32_t *hc0)
{
    uint32_t *mem = (uint32_t *)s_base;
    const char *v = getenv("RECOMP_OHCI_CATCHUP");
    int on = !(v && *v == '0');
    uint32_t head, done, status;
    long runs;
    char detail[128];

    printf("\n  catch-up %s (RECOMP_OHCI_CATCHUP=%s)\n", on ? "on" : "off",
           v ? v : "unset");

    /* One control ED for a function address no device has (so the transfer
     * completes without a pad behind it), one IN descriptor queued ahead of
     * the dummy tail. */
    memset(mem + CU_ED / 4, 0, 0x2000);
    mem[CU_ED / 4 + 0] = 0x7Fu;                 /* FA 127, endpoint 0     */
    mem[CU_ED / 4 + 1] = CU_TD1;                /* TailP                  */
    mem[CU_ED / 4 + 2] = CU_TD0;                /* HeadP                  */
    mem[CU_ED / 4 + 3] = 0;                     /* NextED                 */
    mem[CU_TD0 / 4 + 0] = (2u << 19) | (0xFu << 28);   /* IN, not accessed */
    mem[CU_TD0 / 4 + 2] = CU_TD1;
    mem[CU_TD1 / 4 + 2] = 0;

    hc0[0x18 / 4] = CU_HCCA;                    /* HcHCCA                 */
    hc0[0x20 / 4] = CU_ED;                      /* HcControlHeadED        */
    hc0[0x10 / 4] = 0x80000002u;                /* enable MIE | WDH       */
    hc0[0x04 / 4] = 0x00000090u;                /* operational, CLE       */

    /* Nothing filled yet: nothing to catch up on. */
    xbox_OhciCatchUp();
    head = mem[CU_ED / 4 + 2] & ~0xFu;
    snprintf(detail, sizeof detail, "HeadP 0x%08X", head);
    check(head == CU_TD0, "no list filled: the guest side walks nothing",
          detail);

    hc0[0x08 / 4] = 0x00000002u;                /* ControlListFilled      */
    xbox_OhciCatchUp();
    head   = mem[CU_ED / 4 + 2] & ~0xFu;
    done   = mem[(CU_HCCA + 0x84) / 4];
    status = hc0[0x0C / 4];
    runs   = run_release_point();
    snprintf(detail, sizeof detail,
             "HeadP 0x%08X done 0x%08X status 0x%08X line runs %ld",
             head, done, status, runs);
    if (on) {
        check(head == CU_TD1 && done == CU_TD0,
              "CLF with the controller overdue: the list is walked", detail);
        check((status & 0x02u) && !(hc0[0x08 / 4] & 0x02u),
              "the done queue is written back, CLF consumed", detail);
        check(runs == 1,
              "the USB line was raised and runs at the release point",
              detail);
    } else {
        check(head == CU_TD0 && done == 0 && !(status & 0x02u),
              "RECOMP_OHCI_CATCHUP=0: the list is left to the controller",
              detail);
        check(runs == 0, "RECOMP_OHCI_CATCHUP=0: no line raised", detail);
    }
}

int main(int argc, char **argv)
{
    size_t xbe_len = 0;
    unsigned char *xbe;
    struct sigaction sa;
    char detail[96];

    /* Before the layout reads it: keep the controller thread from running
     * (see the comment at the top). */
    setenv("RECOMP_TIB_PER_THREAD", "0", 1);

    setvbuf(stdout, NULL, _IONBF, 0);
    xbe = argc > 1 ? load_file(argv[1], &xbe_len) : NULL;
    if (!xbe) {
        fprintf(stderr, "usage: %s test.xbe (tools/conformance/mkxbe.py)\n",
                argv[0]);
        return 2;
    }
    /* The model is opt-in. The gate puts the safe points on, which the
     * catch-up needs to raise its line. */
    setenv("RECOMP_USB", "1", 1);
    setenv("RECOMP_DISPATCH_GATE", "1", 1);
    if (!xbox_MemoryLayoutInit(xbe, xbe_len)) {
        fprintf(stderr, "xbox_MemoryLayoutInit failed\n");
        return 2;
    }
    s_base = (uintptr_t)xbox_GetMemoryOffset();

    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);

    xbox_OhciInit();

#if !defined(__aarch64__) && !(defined(__x86_64__) && defined(__linux__))
    check(!xbox_OhciOwnsAddress(XBOX_OHCI0_BASE),
          "no decoder on this host: the model stays off", NULL);
    printf("\n%d failure(s); trap path not available here (skipped)\n",
           failures);
    return failures ? 1 : 77;
#else
    {
        volatile uint32_t *hc0 = (volatile uint32_t *)(s_base + XBOX_OHCI0_BASE);
        volatile uint32_t *hc1 = (volatile uint32_t *)(s_base + XBOX_OHCI1_BASE);
        uint32_t v;
        uint64_t q;

        check(xbox_OhciOwnsAddress(XBOX_OHCI0_BASE)
              && xbox_OhciOwnsAddress(XBOX_OHCI1_BASE + 0x54),
              "both controllers claim their apertures", NULL);
        check(!xbox_OhciOwnsAddress(XBOX_OHCI0_BASE + XBOX_OHCI_SIZE),
              "the byte past a controller is not claimed", NULL);

        v = hc0[0];                               /* HcRevision */
        snprintf(detail, sizeof detail, "read 0x%08X", v);
        check(v == 0x10u, "HC0 HcRevision reads OHCI 1.0 through the trap",
              detail);
        v = hc1[0];
        snprintf(detail, sizeof detail, "read 0x%08X", v);
        check(v == 0x10u, "HC1 HcRevision reads OHCI 1.0", detail);

        v = hc0[0x48 / 4] & 0xFFu;                /* HcRhDescriptorA.NDP */
        snprintf(detail, sizeof detail, "NDP %u", v);
        check(v >= 1 && v <= 4, "root hub reports its downstream ports",
              detail);

        v = *(volatile uint8_t *)(s_base + XBOX_OHCI0_BASE);
        snprintf(detail, sizeof detail, "read 0x%02X", v);
        check(v == 0x10u, "a byte read takes its slice of the register",
              detail);

        hc0[0x10 / 4] = 0x00000004u;              /* HcInterruptEnable: SF */
        v = hc0[0x10 / 4];
        snprintf(detail, sizeof detail, "enable mask 0x%08X", v);
        check((v & 0x4u) != 0, "a store reaches the model (enable SF)",
              detail);
        hc0[0x14 / 4] = 0x00000004u;              /* HcInterruptDisable */
        v = hc0[0x10 / 4];
        snprintf(detail, sizeof detail, "enable mask 0x%08X", v);
        check((v & 0x4u) == 0, "HcInterruptDisable clears it again", detail);

        q = *(volatile uint64_t *)(s_base + XBOX_OHCI0_BASE);
        snprintf(detail, sizeof detail, "read 0x%016llX",
                 (unsigned long long)q);
        check((uint32_t)q == 0x10u,
              "an 8-byte read is two register reads", detail);

        catch_up(hc0);
    }
    check(!s_unhandled, "no access fell through to the crash path", NULL);
    xbox_OhciReport();
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
#endif
}
