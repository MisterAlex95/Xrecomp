/*
 * The MCPX Ethernet controller with no cable (XBA-117), reached the way a
 * title's network stack reaches it: loads and stores to 0xFEF00000 fault,
 * the process's SIGBUS/SIGSEGV handler hands the fault to the toolkit's
 * trapped-MMIO dispatch, and the instruction completes with the model's
 * value.
 *
 * The sequence is the one a forcedeth-style driver performs at start-up:
 * reset (TxRxControl, then poll UnknownSetupReg5 bit 31), program the MAC
 * address the EEPROM gives, scan the MII bus for the PHY, reset it and
 * restart autonegotiation, check the link, start the transmitter and queue
 * one frame, waiting for it with a timeout. Every wait is bounded; the test
 * asserts each ends in time and the whole sequence ends well under a
 * second, with the link down, the frame refused with "carrier lost", and
 * no link-change ever latched. The kernel's PhyGetLinkState must agree,
 * the EEPROM MAC must be locally administered and per install, and the
 * status file must say "nic probed, link down".
 *
 * Run a second time with RECOMP_NIC=0 (a second CTest entry): no model, the
 * aperture is plain memory, PhyGetLinkState says link up as before.
 *
 * A host without a trapped-MMIO decoder (anything but POSIX arm64 or Linux
 * x86-64) keeps the model off by design: the test exits 77.
 */
#include "xbox_memory_layout.h"
#include "kernel.h"
#include "xbox_nic.h"
#include "recomp_status.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Supplied by recompiled game code in a real build; nothing here calls one. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(unsigned int va) { (void)va; return 0; }
recomp_func_t recomp_lookup_manual(unsigned int va) { (void)va; return 0; }

static int failures;
static uintptr_t s_base;
static volatile sig_atomic_t s_unhandled;

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-60s %s%s%s\n", what, ok ? "PASS" : "FAIL",
           detail && !ok ? " -- " : "", detail && !ok ? detail : "");
    if (!ok) failures++;
}

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

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* ---- The driver's side ------------------------------------------------- */

#define R(off) (*(volatile uint32_t *)(s_base + XBOX_NIC_BASE + (off)))

#define IrqStatus           0x000
#define IrqMask             0x004
#define TransmitterControl  0x084
#define TransmitterStatus   0x088
#define MacAddrA            0x0A8
#define MacAddrB            0x0AC
#define TxRingPhysAddr      0x100
#define RingSizes           0x108
#define UnknownSetupReg5    0x130
#define TxRxControl         0x144
#define MIIStatus           0x180
#define AdapterControl      0x188
#define MIIControl          0x190
#define MIIData             0x194

#define POLL_LIMIT          100000  /* iterations; each is a trapped read */

/* Poll until (reg & mask) == want; the iterations it took, or -1. */
static int poll_reg(uint32_t reg, uint32_t mask, uint32_t want)
{
    for (int i = 0; i < POLL_LIMIT; i++)
        if ((R(reg) & mask) == want)
            return i;
    return -1;
}

/* forcedeth's mii_rw: clear the status, issue, poll INUSE; -1 on timeout or
 * error. */
static int mii_rw(int addr, int reg, int value)
{
    R(MIIStatus) = 0x7;
    if (R(MIIControl) & 0x8000)
        R(MIIControl) = 0x8000;
    if (value >= 0)
        R(MIIData) = (uint32_t)value;
    R(MIIControl) = (uint32_t)((addr << 5) | reg | (value >= 0 ? 0x400 : 0));
    if (poll_reg(MIIControl, 0x8000, 0) < 0)
        return -1;
    if (value >= 0)
        return 0;
    if (R(MIIStatus) & 0x1)
        return -1;
    return (int)(R(MIIData) & 0xFFFF);
}

static void run_model(void)
{
    char detail[160];
    double t0 = now_ms(), t;
    int n, phy = -1, v;
    uint8_t mac[6], mac2[6];
    ULONG type = 0, len = 0;
    NTSTATUS st;

    check(xbox_NicEnabled() && xbox_NicOwnsAddress(XBOX_NIC_BASE)
          && xbox_NicOwnsAddress(XBOX_NIC_BASE + XBOX_NIC_REGS - 4),
          "the model claims the register BAR", NULL);

    /* Reset. */
    R(TxRxControl) = 0x0002 | 0x0010;           /* BIT1 | RESET */
    n = poll_reg(UnknownSetupReg5, 0x80000000u, 0x80000000u);
    snprintf(detail, sizeof detail, "polls %d", n);
    check(n >= 0 && n < 10, "reset: UnknownSetupReg5 bit 31 comes up at once", detail);
    R(TxRxControl) = 0;
    check(R(TransmitterStatus) == 0, "the transmitter is not busy", NULL);

    /* MAC address from the EEPROM, programmed as the stack does. */
    memset(mac, 0, sizeof mac);
    st = xbox_ExQueryNonVolatileSetting(XC_FACTORY_ETHERNET_ADDR, &type, mac,
                                        sizeof mac, &len);
    snprintf(detail, sizeof detail, "st 0x%08X type %u len %u mac %02X:%02X:%02X:%02X:%02X:%02X",
             (unsigned)st, (unsigned)type, (unsigned)len,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    check(st == 0 && type == 3 && len == 6, "EEPROM XC_FACTORY_ETHERNET_ADDR answers 6 bytes", detail);
    check((mac[0] & 0x03) == 0x02, "the MAC is unicast and locally administered", detail);
    xbox_NicMacAddress(mac2);
    check(!memcmp(mac, mac2, 6), "the MAC is stable for an install", detail);
    R(MacAddrA) = (uint32_t)mac[0] | mac[1] << 8 | mac[2] << 16 | (uint32_t)mac[3] << 24;
    R(MacAddrB) = (uint32_t)mac[4] | mac[5] << 8;
    check(R(MacAddrA) == ((uint32_t)mac[0] | mac[1] << 8 | mac[2] << 16 | (uint32_t)mac[3] << 24),
          "the MAC address registers keep what was programmed", NULL);

    /* MII scan, as forcedeth's probe: the first address whose ID is not
     * 0xFFFF holds the PHY. */
    for (int a = 1; a <= 32; a++) {
        int id = mii_rw(a & 31, 2, -1);
        if (id < 0) {
            snprintf(detail, sizeof detail, "address %d", a & 31);
            check(0, "an MII read completes", detail);
            break;
        }
        if (id != 0xFFFF && phy < 0)
            phy = a & 31;
    }
    snprintf(detail, sizeof detail, "phy at %d", phy);
    check(phy == 1, "the MII scan finds one PHY, at address 1", detail);

    /* PHY reset and autonegotiation. */
    check(mii_rw(1, 0, 0x8000) == 0, "PHY reset written", NULL);
    v = mii_rw(1, 0, -1);
    snprintf(detail, sizeof detail, "BMCR 0x%04X", v);
    check(v >= 0 && !(v & 0x8000) && (v & 0x1000), "BMCR: reset self-clears, autoneg enabled", detail);
    mii_rw(1, 0, v | 0x0200);                   /* restart autonegotiation */
    v = mii_rw(1, 1, -1);
    snprintf(detail, sizeof detail, "BMSR 0x%04X", v);
    check(v >= 0 && (v & 0x7800) && (v & 0x0008),
          "BMSR: 10/100 capable, autoneg capable", detail);
    check(v >= 0 && !(v & 0x0004), "BMSR: link down", detail);
    {
        /* The link check a driver repeats for a while: never comes up. */
        double until = now_ms() + 50;
        int up = 0;
        while (now_ms() < until)
            if ((v = mii_rw(1, 1, -1)) < 0 || (v & 0x0024))
                up = 1;
        check(!up, "BMSR over 50 ms: link and autoneg-complete stay clear", NULL);
    }
    v = mii_rw(1, 5, -1);
    snprintf(detail, sizeof detail, "ANLPAR 0x%04X", v);
    check(v == 0, "no link partner", detail);

    /* The adapter cannot be told the link is up. */
    R(AdapterControl) = 0x02 | 0x04 | 0x40000;  /* START | LINKUP | PHYVALID */
    snprintf(detail, sizeof detail, "AdapterControl 0x%08X", R(AdapterControl));
    check(!(R(AdapterControl) & 0x04) && (R(AdapterControl) & 0x40000),
          "AdapterControl: LINKUP reads clear, the rest is kept", detail);

    /* The kernel's view agrees. */
    check(xbox_PhyInitialize(1, NULL) == 0, "PhyInitialize succeeds", NULL);
    v = (int)xbox_PhyGetLinkState(1);
    snprintf(detail, sizeof detail, "flags 0x%X", v);
    check(v == 0, "PhyGetLinkState: no link (0)", detail);

    /* Transmit: one frame on a 4-entry ring in contiguous memory. */
    {
        const uint32_t ring_phys = 0x00200000u, buf_phys = 0x00201000u;
        volatile uint32_t *ring = (volatile uint32_t *)(s_base + XBOX_CONTIG_BASE + ring_phys);
        volatile uint16_t *flags = (volatile uint16_t *)((uintptr_t)ring + 6);
        int polls = -1;

        memset((void *)ring, 0, 32);
        R(TxRingPhysAddr) = ring_phys;
        R(RingSizes) = (15u << 16) | 3u;        /* 16 rx, 4 tx */
        R(IrqMask) = 0x00FF;
        ring[0] = buf_phys;
        ring[1] = (uint32_t)(0x8000u | 0x0001u) << 16 | 59u;   /* VALID | LAST, 60 bytes */

        /* Stopped transmitter: the descriptor stays the driver's. */
        R(TxRxControl) = 0x0001;                /* KICK */
        check((*flags & 0x8000) != 0, "transmitter stopped: the frame stays queued", NULL);

        R(TransmitterControl) = 0x01;           /* START */
        R(TxRxControl) = 0x0001;                /* KICK */
        for (int i = 0; i < POLL_LIMIT; i++)
            if (!(*flags & 0x8000)) {
                polls = i;
                break;
            }
        snprintf(detail, sizeof detail, "polls %d flags 0x%04X IrqStatus 0x%08X",
                 polls, *flags, R(IrqStatus));
        check(polls >= 0, "the queued frame completes within the timeout", detail);
        check((*flags & 0x4800) == 0x4800, "it completes with ERROR | CARRIERLOST", detail);
        check((R(IrqStatus) & 0x0008) != 0, "IrqStatus latches TX_ERR", detail);
        R(IrqStatus) = 0x0008;
        check((R(IrqStatus) & 0x0008) == 0, "IrqStatus is write-1-to-clear", NULL);
        check(xbox_NicPeekReg(TxRxControl) == 0, "KICK self-clears", NULL);
    }

    check(!(R(IrqStatus) & 0x0040) && !(R(MIIStatus) & 0x0008),
          "no link change was ever latched", NULL);
    t = now_ms() - t0;
    snprintf(detail, sizeof detail, "%.1f ms", t);
    check(t < 1000.0, "the whole start-up sequence ends in bounded time", detail);

    /* Another install, another address. */
    {
        char d2[] = "/tmp/nic_posix_save2XXXXXX";
        if (mkdtemp(d2)) {
            xbox_path_init(".", d2);
            xbox_NicMacAddress(mac2);
            check(memcmp(mac, mac2, 6) != 0 && (mac2[0] & 3) == 2,
                  "another save directory derives another MAC", NULL);
        }
    }
}

static void run_off(void)
{
    char detail[96];
    uint32_t v;

    check(!xbox_NicEnabled() && !xbox_NicOwnsAddress(XBOX_NIC_BASE),
          "RECOMP_NIC=0: no model", NULL);
    R(MIIControl) = 0x0022;                     /* a PHY read: plain memory */
    v = R(MIIControl);
    snprintf(detail, sizeof detail, "read 0x%08X", v);
    check(v == 0x0022, "RECOMP_NIC=0: the registers are plain memory", detail);
    R(MIIControl) = 0;
    v = xbox_PhyGetLinkState(1);
    snprintf(detail, sizeof detail, "flags 0x%X", v);
    check(v == XBOX_NIC_LINK_ACTIVE, "RECOMP_NIC=0: PhyGetLinkState says link up, as before", detail);
}

int main(int argc, char **argv)
{
    size_t xbe_len = 0;
    unsigned char *xbe;
    struct sigaction sa;
    char save[] = "/tmp/nic_posix_saveXXXXXX";
    char status[sizeof save + 16];
    const char *off = getenv("RECOMP_NIC");
    int model = !(off && !strcmp(off, "0"));

    setvbuf(stdout, NULL, _IONBF, 0);
    xbe = argc > 1 ? load_file(argv[1], &xbe_len) : NULL;
    if (!xbe) {
        fprintf(stderr, "usage: %s test.xbe (tools/conformance/mkxbe.py)\n", argv[0]);
        return 2;
    }
    if (!mkdtemp(save)) {
        perror("mkdtemp");
        return 2;
    }
    snprintf(status, sizeof status, "%s/status.jsonl", save);
    setenv("RECOMP_STATUS", status, 1);
    recomp_status_reset();                      /* opens it, no thread */

    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);

    if (!xbox_MemoryLayoutInit(xbe, xbe_len)) {
        fprintf(stderr, "xbox_MemoryLayoutInit failed\n");
        return 2;
    }
    s_base = (uintptr_t)xbox_GetMemoryOffset();
    xbox_path_init(".", save);

#if !defined(__aarch64__) && !(defined(__x86_64__) && defined(__linux__))
    check(!xbox_NicEnabled(), "no decoder on this host: the model stays off", NULL);
    printf("\n%d failure(s); trap path not available here (skipped)\n", failures);
    return failures ? 1 : 77;
#else
    if (model)
        run_model();
    else
        run_off();
    check(!s_unhandled, "no access fell through to the crash path", NULL);

    /* Said once, in the status file, for the library report. */
    {
        char line[512] = "";
        FILE *f;
        recomp_status_write_now();
        f = fopen(status, "r");
        if (f) {
            if (!fgets(line, sizeof line, f))
                line[0] = 0;
            fclose(f);
        }
        check(strstr(line, model ? "\"net\":\"nic probed, link down\""
                                 : "\"net\":\"nic probed, link up (RECOMP_NIC=0)\"") != NULL,
              "the status file names the network stack", line);
    }
    xbox_NicReport();
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
#endif
}
