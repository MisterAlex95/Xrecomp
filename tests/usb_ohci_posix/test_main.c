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
 * The controller thread runs, as in the default configuration, and is held
 * still between two passes (xbox_OhciHoldControllerForTest, XBA-133): it is
 * then overdue by definition and cannot race the catch-up this checks. It
 * used to be kept from running with RECOMP_TIB_PER_THREAD=0, which tested a
 * configuration titles no longer run in; a third CTest entry still runs
 * that one.
 * Run a second time with RECOMP_OHCI_CATCHUP=0 (a second CTest entry), the
 * same calls must leave the list and the line alone.
 *
 * With the argument "state" (RECOMP_USB_PADS=1, another CTest entry), the
 * save state of the USB section (XBA-248): a pad enumerated through the
 * control list (SET_ADDRESS, SET_CONFIGURATION), the model captured, its
 * report read; the model reset to a capture taken before enumeration, where
 * the address no longer answers; the capture restored, and the same report
 * comes back from the same address, with the same registers.
 *
 * A host without a trapped-MMIO decoder (anything but POSIX arm64 or Linux
 * x86-64 here) keeps the model off by design; the test checks that it says
 * so and exits 77.
 */
#include "xbox_memory_layout.h"
#include "kernel.h"
#include "ohci.h"
#include "xbox_dev_state.h"
#include "platform/host_time.h"

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

    /* The controller thread stands still from here: overdue. With
     * RECOMP_TIB_PER_THREAD=0 it never ran, which is overdue too. */
    {
        int held = xbox_OhciHoldControllerForTest(1);
        const char *tib = getenv("RECOMP_TIB_PER_THREAD");
        if (!(tib && *tib == '0'))
            check(held, "the controller thread runs and is held between passes", NULL);
    }

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

/* ── Save state (XBA-248) ──────────────────────────────────────────── */

#define ST_ED     0x00402000u
#define ST_TD     0x00402100u        /* four TDs, 16 bytes apart; the last the tail */
#define ST_SETUP  0x00402200u
#define ST_DATA   0x00402300u

/* One transfer on endpoint `ep` of function `fa`: descriptors as the
 * driver queues them (dp: 0 SETUP, 1 OUT, 2 IN), walked once. Returns the
 * condition code of the last. */
static uint32_t st_transfer(uint32_t fa, uint32_t ep, int n, const uint32_t *dp,
                            const uint32_t *cbp, const uint32_t *len)
{
    uint32_t *mem = (uint32_t *)s_base, done = 0;
    int i;

    memset(mem + ST_TD / 4, 0, 0x60);
    for (i = 0; i < n; i++) {
        uint32_t td = ST_TD + (uint32_t)i * 16u;
        mem[td / 4 + 0] = (dp[i] << 19) | (0xFu << 28) | (1u << 18);
        mem[td / 4 + 1] = len[i] ? cbp[i] : 0;
        mem[td / 4 + 2] = td + 16u;
        mem[td / 4 + 3] = len[i] ? cbp[i] + len[i] - 1 : 0;
    }
    mem[ST_ED / 4 + 0] = fa | ep << 7 | 8u << 16;
    mem[ST_ED / 4 + 1] = ST_TD + (uint32_t)n * 16u;     /* TailP */
    mem[ST_ED / 4 + 2] = ST_TD;                          /* HeadP */
    mem[ST_ED / 4 + 3] = 0;
    xbox_OhciWalkListForTest(ST_ED, &done);
    return mem[(ST_TD + (uint32_t)(n - 1) * 16u) / 4] >> 28;
}

static void st_control(uint32_t fa, uint8_t req, uint16_t value)
{
    uint8_t *setup = (uint8_t *)(s_base + ST_SETUP);
    const uint32_t dp[2] = { 0, 2 }, cbp[2] = { ST_SETUP, 0 }, len[2] = { 8, 0 };

    memset(setup, 0, 8);
    setup[1] = req;
    setup[2] = (uint8_t)value;
    setup[3] = (uint8_t)(value >> 8);
    st_transfer(fa, 0, 2, dp, cbp, len);
}

/* The pad's 20-byte report from function fa's interrupt IN endpoint into
 * out; returns the bytes moved (CBP left 0 when all of them moved). */
static int st_report(uint32_t fa, uint8_t out[20])
{
    uint8_t *data = (uint8_t *)(s_base + ST_DATA);
    const uint32_t dp[1] = { 2 }, cbp[1] = { ST_DATA }, len[1] = { 20 };
    uint32_t left;

    memset(data, 0xEE, 20);
    st_transfer(fa, 1, 1, dp, cbp, len);
    memcpy(out, data, 20);
    left = ((uint32_t *)s_base)[ST_TD / 4 + 1];
    return left ? (int)(left - ST_DATA) : 20;
}

static int state_round_trip(volatile uint32_t *hc0)
{
    XdsWriter fresh = { 0 }, s = { 0 };
    XdsReader r;
    uint8_t rep1[20], rep2[20], rep_reset[20];
    uint32_t regs1[0x60 / 4], regs2[0x60 / 4];
    int n1, n2, n_reset, pad = -1, ev, i;
    char detail[160];

    printf("\n  save state (XBA-248)\n");
    {
        /* The controller thread may still be starting: StateHold answers 0 at
         * once until it runs (no thread, nothing to hold), so give it the
         * same two seconds the hold itself waits for a pass. */
        int held = 0, t;
        for (t = 0; t < 2000 && !(held = xbox_OhciStateHold(1)); t++)
            Sleep(1);
        check(held, "the controller thread is held between passes", NULL);
    }
    /* A pad on the root hub, unaddressed. */
    ev = xbox_OhciHotplugStepForTest(1u, 1000, &pad);
    snprintf(detail, sizeof detail, "event %d pad %d", ev, pad);
    check(ev == 1 && pad == 0, "pad 1 plugged in", detail);
    hc0[0x10 / 4] = 0x80000002u;                /* MIE | WDH */
    hc0[0x04 / 4] = 0x00000080u;                /* operational */
    check(xbox_OhciStateCapture(&fresh) && !fresh.err, "capture before enumeration", NULL);

    st_control(0, 0x05, 5);                     /* SET_ADDRESS 5 */
    st_control(5, 0x09, 1);                     /* SET_CONFIGURATION 1 */
    for (i = 0; i < 0x60 / 4; i++)
        regs1[i] = hc0[i];
    check(xbox_OhciStateCapture(&s) && !s.err, "capture after enumeration", NULL);
    snprintf(detail, sizeof detail, "%zu bytes", s.len);
    check(s.len > 64 && s.len < 4096, "the USB section is small", detail);
    n1 = st_report(5, rep1);
    snprintf(detail, sizeof detail, "%d bytes, %02X %02X", n1, rep1[0], rep1[1]);
    check(n1 == 20 && rep1[0] == 0 && rep1[1] == 20, "address 5 answers with its report", detail);

    /* Reset: the model as before enumeration. */
    r = xds_reader(fresh.p, fresh.len);
    check(xbox_OhciStateRestore(&r, XDS_USB_VERSION) && !r.err, "restore the earlier capture", NULL);
    n_reset = st_report(5, rep_reset);
    snprintf(detail, sizeof detail, "%d bytes", n_reset);
    check(n_reset == 0, "reset: address 5 no longer answers", detail);

    /* Restore. */
    r = xds_reader(s.p, s.len);
    check(xbox_OhciStateRestore(&r, XDS_USB_VERSION) && !r.err, "restore the capture", NULL);
    for (i = 0; i < 0x60 / 4; i++)
        regs2[i] = hc0[i];
    /* HcFmNumber and HcFmRemaining move with every read. */
    regs1[0x3C / 4] = regs2[0x3C / 4] = 0;
    regs1[0x38 / 4] = regs2[0x38 / 4] = 0;
    check(!memcmp(regs1, regs2, sizeof regs1), "the registers read as they did at the capture", NULL);
    n2 = st_report(5, rep2);
    snprintf(detail, sizeof detail, "%d and %d bytes", n1, n2);
    check(n2 == n1 && !memcmp(rep1, rep2, sizeof rep1),
          "address 5 answers again with the same report", detail);

    /* Refusals: another version, a truncated payload. */
    r = xds_reader(s.p, s.len);
    check(!xbox_OhciStateRestore(&r, XDS_USB_VERSION + 1), "another version is refused", NULL);
    r = xds_reader(s.p, s.len / 2);
    check(!xbox_OhciStateRestore(&r, XDS_USB_VERSION), "a truncated section is refused", NULL);
    n2 = st_report(5, rep2);
    check(n2 == 20, "a refused state leaves the model as it was", NULL);

    xbox_OhciStateHold(0);
    xds_writer_free(&fresh);
    xds_writer_free(&s);
    return 0;
}

/* Every device section through the kernel's entry points: the NV2A poll
 * thread held (it runs in this process: xbox_MemoryLayoutInit started it),
 * the NV2A aperture and the OHCI registers captured, both overwritten,
 * restored, the threads let go. */
static void dev_state_all(volatile uint32_t *hc0)
{
    volatile uint32_t *nv = (volatile uint32_t *)(s_base + 0xFD000000u);
    XdsWriter w = { 0 };
    XdsReader r;
    uint64_t t0, t1;
    int n = 0;
    char detail[160];

    printf("\n  every device section (xbox_DevState*)\n");
    xbox_DevStateSections(&n);
    check(n == 3, "three device sections: NV2A, APU, USB", NULL);
    nv[0x700010 / 4] = 0x13572468u;             /* RAMIN: a DMA object word */
    nv[0x400820 / 4] = 0x00ABCDEFu;             /* PGRAPH */
    hc0[0x18 / 4] = 0x00403000u;                /* HcHCCA */
    check(xbox_DevStateFreeze(), "freeze: the device threads stand still", NULL);
    t0 = xbox_HostNowNs();
    check(xbox_DevStateCaptureAll(&w) && !w.err, "capture every section", NULL);
    t1 = xbox_HostNowNs();
    snprintf(detail, sizeof detail, "%zu bytes", w.len);
    printf("  (sections: %zu bytes in %.2f ms)\n", w.len, (double)(t1 - t0) / 1e6);
    check(w.len < 256u * 1024u, "the sections are small (the aperture is run-length coded)", detail);

    nv[0x700010 / 4] = 0;
    nv[0x400820 / 4] = 0x11111111u;
    hc0[0x18 / 4] = 0x00500000u;
    r = xds_reader(w.p, w.len);
    check(xbox_DevStateRestoreAll(&r) && !r.err, "restore every section", NULL);
    xbox_DevStateThaw(1);
    snprintf(detail, sizeof detail, "RAMIN 0x%08X PGRAPH 0x%08X HCCA 0x%08X",
             nv[0x700010 / 4], nv[0x400820 / 4], hc0[0x18 / 4]);
    check(nv[0x700010 / 4] == 0x13572468u && nv[0x400820 / 4] == 0x00ABCDEFu
          && hc0[0x18 / 4] == 0x00403000u, "the registers are back", detail);

    /* A foreign section is skipped, a broken one refused. */
    {
        XdsWriter x = { 0 };
        size_t at = xds_section_begin(&x, XDS_TAG('R', 'A', 'M', ' '), 1);
        xds_put_u32(&x, 42);
        xds_section_end(&x, at);
        r = xds_reader(x.p, x.len);
        check(xbox_DevStateRestoreAll(&r), "another component's section is skipped", NULL);
        xds_writer_free(&x);
        r = xds_reader(w.p, w.len - 5);
        check(!xbox_DevStateRestoreAll(&r), "a truncated state is refused", NULL);
    }
    xds_writer_free(&w);
}

int main(int argc, char **argv)
{
    size_t xbe_len = 0;
    unsigned char *xbe;
    struct sigaction sa;
    char detail[96];

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

        if (argc > 2 && !strcmp(argv[2], "state")) {
            state_round_trip(hc0);
            dev_state_all(hc0);
        }
        else
            catch_up(hc0);
    }
    check(!s_unhandled, "no access fell through to the crash path", NULL);
    xbox_OhciReport();
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
#endif
}
