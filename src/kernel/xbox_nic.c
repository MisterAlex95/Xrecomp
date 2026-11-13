/*
 * xbox_nic.c -- the MCPX Ethernet controller with no cable (XBA-117).
 *
 * xbox_nic.h says what this is and where the register layout comes from.
 * Register semantics, by offset in the BAR:
 *
 *   0x000 IrqStatus          latched causes, write 1 to clear; never raised
 *                            as an interrupt (see "Interrupts" below)
 *   0x004 IrqMask            storage
 *   0x084 TransmitterControl storage; bit 0 (START) gates transmit
 *   0x088 TransmitterStatus  0: never busy
 *   0x094 ReceiverControl    storage
 *   0x098 ReceiverStatus     0: never busy
 *   0x100 TxRingPhysAddr     storage; a write rewinds the transmit cursor
 *   0x108 RingSizes          (rx - 1) << 16 | (tx - 1)
 *   0x130 UnknownSetupReg5   bit 31 reads set once TxRxControl was written
 *                            (drivers poll for it after a TxRxControl write)
 *   0x144 TxRxControl        KICK runs the transmit ring, RESET rewinds it;
 *                            both self-clear
 *   0x180 MIIStatus          write 1 to clear; LINKCHANGE is never set
 *   0x188 AdapterControl     storage, but LINKUP always reads clear
 *   0x190 MIIControl         a PHY read or write, completed at once (INUSE
 *                            reads clear); PHY address 1 answers, any other
 *                            address reads 0xFFFF (no PHY there)
 *   0x194 MIIData            the last PHY read, or the value to write
 *   everything else          storage
 *
 * The PHY (IEEE 802.3 clause 22, address 1):
 *   BMCR  0x00  autonegotiation enabled; RESET and RESTART_AN self-clear
 *   BMSR  0x01  10/100 half/full capable, autonegotiation capable, and
 *               neither LINK_STATUS nor AN_COMPLETE, ever
 *   ID    0x02/0x03  0 (the Xbox stack does not look at it)
 *   ANAR  0x04  10/100 half/full, IEEE 802.3
 *   ANLPAR 0x05 0: no partner
 *
 * Transmit: with the transmitter started, a KICK walks the ring from the
 * cursor and completes every descriptor the driver owns (flags bit 15,
 * VALID): VALID cleared, CARRIERLOST and ERROR set, the cursor advanced, and
 * TX_ERR latched in IrqStatus. That is what a MAC with no carrier reports,
 * and it gives the driver its buffers back instead of leaving the ring full.
 * With the transmitter stopped the descriptors stay the driver's, as on the
 * hardware. Nothing is ever received.
 *
 * Interrupts: none. KeConnectInterrupt accepts the NIC's vector and nothing
 * raises it, which for a controller with no cable is the link-change
 * interrupt never coming; a transmit completion the driver wants it can find
 * by polling the descriptor or IrqStatus.
 */
#include "xbox_nic.h"
#include "xbox_memory_layout.h"
#include "kernel.h"
#include "recomp_status.h"
#include "../platform/mmio_decode.h"
#include "../platform/mmio_decode_a64.h"
#include "../platform/mmio_decode_x86_64.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#if defined(_WIN32)
#  define NIC_TRAP_WIN32 1
#elif defined(__aarch64__) || (defined(__x86_64__) && defined(__linux__))
#  define NIC_TRAP_POSIX 1
#endif

/* ---- MAC registers (forcedeth names) ------------------------------------ */
#define NvRegIrqStatus            0x000
#define NvRegIrqMask              0x004
#define NvRegTransmitterControl   0x084
#define NvRegTransmitterStatus    0x088
#define NvRegReceiverControl      0x094
#define NvRegReceiverStatus       0x098
#define NvRegMacAddrA             0x0A8
#define NvRegMacAddrB             0x0AC
#define NvRegTxRingPhysAddr       0x100
#define NvRegRxRingPhysAddr       0x104
#define NvRegRingSizes            0x108
#define NvRegUnknownSetupReg5     0x130
#define NvRegTxRxControl          0x144
#define NvRegMIIStatus            0x180
#define NvRegAdapterControl       0x188
#define NvRegMIIControl           0x190
#define NvRegMIIData              0x194

#define NVREG_IRQ_TX_ERR          0x0008u
#define NVREG_XMITCTL_START       0x01u
#define NVREG_UNKSETUP5_BIT31     0x80000000u
#define NVREG_TXRXCTL_KICK        0x0001u
#define NVREG_TXRXCTL_RESET       0x0010u
#define NVREG_MIISTAT_MASK_ALL    0x000Fu
#define NVREG_ADAPTCTL_LINKUP     0x0004u
#define NVREG_MIICTL_INUSE        0x8000u
#define NVREG_MIICTL_WRITE        0x0400u
#define NVREG_MIICTL_ADDRSHIFT    5

/* Descriptor flags (the high half of forcedeth's v1 flaglen). */
#define NV_TX_LASTPACKET          0x0001u
#define NV_TX_CARRIERLOST         0x0800u
#define NV_TX_ERROR               0x4000u
#define NV_TX_VALID               0x8000u

/* ---- PHY (clause 22) ---------------------------------------------------- */
#define NIC_PHY_ADDR              1
#define MII_BMCR                  0x00
#define MII_BMSR                  0x01
#define MII_ANAR                  0x04
#define MII_ANLPAR                0x05
#define BMCR_RESET                0x8000u
#define BMCR_ANENABLE             0x1000u
#define BMCR_ANRESTART            0x0200u
#define BMCR_FULLDPLX             0x0100u
#define BMCR_SPEED100             0x2000u
#define BMSR_100FULL              0x4000u
#define BMSR_100HALF              0x2000u
#define BMSR_10FULL               0x1000u
#define BMSR_10HALF               0x0800u
#define BMSR_ANEGCOMPLETE         0x0020u
#define BMSR_ANEGCAPABLE          0x0008u
#define BMSR_LSTATUS              0x0004u
#define BMSR_ERCAP                0x0001u
#define ANAR_ALL_10_100           0x01E1u   /* 100FD 100HD 10FD 10HD, 802.3 */

/* The trapped span: the BAR, rounded up to whole host pages (16 KB pages
 * cover more than the BAR; the rest of the page is plain storage here). */
#define NIC_SPAN_MAX              0x10000u

static struct {
    int      enabled;
    uint32_t span;
    uint32_t reg[NIC_SPAN_MAX / 4];
    uint16_t phy[32];
    uint32_t tx_next;               /* transmit cursor, a descriptor index */
    uint32_t reads, writes, undecoded, tx_done, mii_ops;
} s_nic;

static volatile LONG s_lock;
static volatile LONG s_probed;      /* the title touched the NIC or the PHY */
static int s_trace;
static unsigned s_trace_lines;

static int nic_off_switch(void)
{
    const char *v = getenv("RECOMP_NIC");
    return v && v[0] == '0' && v[1] == '\0';
}

static void nic_lock(void)
{
    while (InterlockedExchange(&s_lock, 1) != 0)
        ;
}

static void nic_unlock(void)
{
    InterlockedExchange(&s_lock, 0);
}

/* Once per process: the log line and the status file's "net" key, so the
 * library report shows which titles have a network stack. stdio from a
 * signal handler, deliberately, as the other trapped models do. */
static void nic_note_probe(const char *how)
{
    if (InterlockedExchange(&s_probed, 1) != 0)
        return;
    if (s_nic.enabled || !nic_off_switch()) {
        fprintf(stderr, "[NET] nic probed (%s), link down: no cable"
                        " (RECOMP_NIC=0 for the old link-up answer)\n", how);
        recomp_status_net("nic probed, link down");
    } else {
        fprintf(stderr, "[NET] nic probed (%s); RECOMP_NIC=0: PHY answers"
                        " link up, registers are plain memory\n", how);
        recomp_status_net("nic probed, link up (RECOMP_NIC=0)");
    }
    fflush(stderr);
}

static void phy_reset(void)
{
    memset(s_nic.phy, 0, sizeof s_nic.phy);
    s_nic.phy[MII_BMCR] = BMCR_ANENABLE | BMCR_SPEED100 | BMCR_FULLDPLX;
    s_nic.phy[MII_BMSR] = BMSR_100FULL | BMSR_100HALF | BMSR_10FULL
                        | BMSR_10HALF | BMSR_ANEGCAPABLE | BMSR_ERCAP;
    s_nic.phy[MII_ANAR] = ANAR_ALL_10_100;
    s_nic.phy[MII_ANLPAR] = 0;
}

static uint16_t phy_read(int reg)
{
    reg &= 0x1F;
    if (reg == MII_BMSR)            /* no cable: whatever was written */
        return (uint16_t)(s_nic.phy[MII_BMSR] & ~(BMSR_LSTATUS | BMSR_ANEGCOMPLETE));
    if (reg == MII_ANLPAR)
        return 0;
    return s_nic.phy[reg];
}

static void phy_write(int reg, uint16_t v)
{
    reg &= 0x1F;
    if (reg == MII_BMSR || reg == MII_ANLPAR || reg == 2 || reg == 3)
        return;                     /* read-only */
    if (reg == MII_BMCR) {
        if (v & BMCR_RESET) {
            phy_reset();
            return;                 /* reset completes at once */
        }
        v &= (uint16_t)~BMCR_ANRESTART;   /* restarted, and never completes */
    }
    s_nic.phy[reg] = v;
}

static void nic_reset(void)
{
    memset(s_nic.reg, 0, sizeof s_nic.reg);
    s_nic.tx_next = 0;
    s_nic.reads = s_nic.writes = s_nic.undecoded = 0;
    s_nic.tx_done = s_nic.mii_ops = 0;
    phy_reset();
}

/* A ring's guest address. The driver writes physical addresses
 * (MmGetPhysicalAddress of contiguous memory), which this runtime shows at
 * XBOX_CONTIG_BASE + phys. Anything else is not RAM the controller could
 * reach: 0, and the kick does nothing (a fault here would be inside the
 * signal handler). */
static uint32_t ring_va(uint32_t phys, uint32_t bytes)
{
    if (phys >= XBOX_CONTIG_SIZE || bytes > XBOX_CONTIG_SIZE - phys)
        return 0;
    return XBOX_CONTIG_BASE + phys;
}

static void tx_kick(void)
{
    uint32_t n = (s_nic.reg[NvRegRingSizes / 4] & 0xFFFFu) + 1;
    uint32_t va = ring_va(s_nic.reg[NvRegTxRingPhysAddr / 4], n * 8u);
    ptrdiff_t off = xbox_GetMemoryOffset();
    uint32_t i;

    if (!va || !off || !(s_nic.reg[NvRegTransmitterControl / 4] & NVREG_XMITCTL_START))
        return;
    if (s_nic.tx_next >= n)
        s_nic.tx_next = 0;
    for (i = 0; i < n; i++) {       /* at most one lap */
        volatile uint16_t *flags = (volatile uint16_t *)
            ((uintptr_t)off + va + s_nic.tx_next * 8u + 6u);
        uint16_t f = *flags;

        if (!(f & NV_TX_VALID))
            break;
        *flags = (uint16_t)((f & ~NV_TX_VALID) | NV_TX_CARRIERLOST | NV_TX_ERROR);
        s_nic.reg[NvRegIrqStatus / 4] |= NVREG_IRQ_TX_ERR;
        s_nic.tx_done++;
        s_nic.tx_next = (s_nic.tx_next + 1) % n;
    }
}

static uint32_t nic_read32(uint32_t off)
{
    switch (off) {
    case NvRegTransmitterStatus:
    case NvRegReceiverStatus:
        return 0;
    case NvRegAdapterControl:
        return s_nic.reg[off / 4] & ~NVREG_ADAPTCTL_LINKUP;
    case NvRegMIIControl:
        return s_nic.reg[off / 4] & ~NVREG_MIICTL_INUSE;
    default:
        return s_nic.reg[off / 4];
    }
}

static void nic_write32(uint32_t off, uint32_t v)
{
    switch (off) {
    case NvRegIrqStatus:
    case NvRegMIIStatus:
        s_nic.reg[off / 4] &= ~v;   /* write 1 to clear */
        return;
    case NvRegTransmitterStatus:
    case NvRegReceiverStatus:
        return;
    case NvRegTxRingPhysAddr:
        s_nic.reg[off / 4] = v;
        s_nic.tx_next = 0;
        return;
    case NvRegTxRxControl:
        s_nic.reg[NvRegUnknownSetupReg5 / 4] |= NVREG_UNKSETUP5_BIT31;
        if (v & NVREG_TXRXCTL_RESET)
            s_nic.tx_next = 0;
        if (v & NVREG_TXRXCTL_KICK)
            tx_kick();
        s_nic.reg[off / 4] = v & ~(NVREG_TXRXCTL_KICK | NVREG_TXRXCTL_RESET);
        return;
    case NvRegMIIControl: {
        int addr = (int)((v >> NVREG_MIICTL_ADDRSHIFT) & 0x1F);
        int reg = (int)(v & 0x1F);

        s_nic.mii_ops++;
        if (v & NVREG_MIICTL_WRITE) {
            if (addr == NIC_PHY_ADDR)
                phy_write(reg, (uint16_t)s_nic.reg[NvRegMIIData / 4]);
        } else {
            s_nic.reg[NvRegMIIData / 4] = addr == NIC_PHY_ADDR ? phy_read(reg) : 0xFFFFu;
        }
        s_nic.reg[off / 4] = v & ~NVREG_MIICTL_INUSE;   /* done at once */
        return;
    }
    default:
        s_nic.reg[off / 4] = v;
        return;
    }
}

/* Sub-word and 8-byte accesses: the registers are 32-bit, so a narrower
 * access takes its slice (a narrow write merges, as forcedeth-style drivers
 * never do it to a register with side effects), a wider one is two. */
static uint64_t nic_read(void *dev, uint32_t off, int size)
{
    uint64_t v;
    (void)dev;

    nic_lock();
    s_nic.reads++;
    if (size == 8) {
        v = nic_read32(off & ~3u) | ((uint64_t)nic_read32((off & ~3u) + 4) << 32);
    } else {
        uint32_t w = nic_read32(off & ~3u) >> ((off & 3u) * 8);
        v = size == 4 ? w : size == 2 ? (w & 0xFFFFu) : (w & 0xFFu);
    }
    nic_unlock();
    if (s_trace && s_trace_lines < 400) {
        s_trace_lines++;
        fprintf(stderr, "  [NIC] read  +0x%03X (%d) = 0x%llX\n", off, size,
                (unsigned long long)v);
    }
    return v;
}

static void nic_write(void *dev, uint32_t off, uint64_t val, int size)
{
    (void)dev;

    if (s_trace && s_trace_lines < 400) {
        s_trace_lines++;
        fprintf(stderr, "  [NIC] write +0x%03X (%d) = 0x%llX\n", off, size,
                (unsigned long long)val);
    }
    nic_lock();
    s_nic.writes++;
    if (size == 8) {
        nic_write32(off & ~3u, (uint32_t)val);
        nic_write32((off & ~3u) + 4, (uint32_t)(val >> 32));
    } else if (size == 4 && !(off & 3u)) {
        nic_write32(off, (uint32_t)val);
    } else {
        uint32_t sh = (off & 3u) * 8, mask = size == 2 ? 0xFFFFu : 0xFFu;
        uint32_t w = s_nic.reg[(off & ~3u) / 4];
        w = (w & ~(mask << sh)) | (((uint32_t)val & mask) << sh);
        nic_write32(off & ~3u, w);
    }
    nic_unlock();
}

/* ---- Public ------------------------------------------------------------- */

void xbox_NicInit(void)
{
    ptrdiff_t base;
    uint32_t span;
    DWORD old;

    s_nic.enabled = 0;
    s_trace = getenv("RECOMP_NIC_TRACE") && getenv("RECOMP_NIC_TRACE")[0] == '1';
    s_trace_lines = 0;
    nic_reset();
    if (nic_off_switch()) {
        fprintf(stderr, "  NIC: RECOMP_NIC=0: no model; registers are plain"
                        " memory, PhyGetLinkState says link up\n");
        return;
    }
#if !defined(NIC_TRAP_WIN32) && !defined(NIC_TRAP_POSIX)
    fprintf(stderr, "  NIC: no trapped-MMIO decoder for this host; registers"
                    " are plain memory (PhyGetLinkState still says no cable)\n");
    return;
#endif
    base = xbox_GetMemoryOffset();
    if (!base || !xbox_GetMcpxAperture()) {
        fprintf(stderr, "  NIC: MCPX aperture not mapped; no model\n");
        return;
    }
#if defined(_WIN32)
    span = 0x1000u;
#else
    {
        long pg = sysconf(_SC_PAGESIZE);
        span = pg > 0 ? (uint32_t)pg : 0x1000u;
    }
#endif
    if (span < XBOX_NIC_REGS)
        span = XBOX_NIC_REGS;
    if (span > NIC_SPAN_MAX) {
        fprintf(stderr, "  NIC: host page of %u bytes is larger than the"
                        " model covers; no model\n", span);
        return;
    }
    if (!VirtualProtect((LPVOID)((uintptr_t)base + XBOX_NIC_BASE), span,
                        PAGE_NOACCESS, &old)) {
        fprintf(stderr, "  NIC: cannot trap 0x%08X; no model\n", XBOX_NIC_BASE);
        return;
    }
    s_nic.span = span;
    s_nic.enabled = 1;
    fprintf(stderr, "  NIC: Ethernet controller at 0x%08X, no cable (link"
                    " down; RECOMP_NIC=0 for plain memory)\n", XBOX_NIC_BASE);
}

void xbox_NicShutdown(void)
{
    DWORD old;
    ptrdiff_t base = xbox_GetMemoryOffset();

    if (!s_nic.enabled)
        return;
    s_nic.enabled = 0;
    if (base)
        VirtualProtect((LPVOID)((uintptr_t)base + XBOX_NIC_BASE), s_nic.span,
                       PAGE_READWRITE, &old);
}

int xbox_NicEnabled(void)
{
    return s_nic.enabled;
}

int xbox_NicOwnsAddress(uint32_t xbox_va)
{
    return s_nic.enabled && xbox_va - XBOX_NIC_BASE < s_nic.span;
}

int xbox_NicHandleMmio(void *ctx, uint32_t xbox_va)
{
    int ok = 0;

    if (!xbox_NicOwnsAddress(xbox_va))
        return 0;
    nic_note_probe("registers");
#if defined(NIC_TRAP_POSIX)
#  if defined(__aarch64__)
    ok = mmio_emulate_a64((ucontext_t *)ctx, xbox_va - XBOX_NIC_BASE, &s_nic,
                          nic_read, nic_write);
#  else
    ok = mmio_emulate_x86_64((ucontext_t *)ctx, xbox_va - XBOX_NIC_BASE, &s_nic,
                             nic_read, nic_write);
#  endif
#elif defined(NIC_TRAP_WIN32)
    ok = mmio_emulate((PCONTEXT)ctx, xbox_va - XBOX_NIC_BASE, &s_nic,
                      nic_read, nic_write);
#else
    (void)ctx;
#endif
    if (!ok && s_nic.undecoded++ < 20) {
        fprintf(stderr, "  [NIC] undecoded access at +0x%03X\n",
                xbox_va - XBOX_NIC_BASE);
        fflush(stderr);
    }
    return ok;
}

uint32_t xbox_NicPhyLinkState(void)
{
    uint16_t bmsr;

    nic_note_probe("PhyGetLinkState");
    if (nic_off_switch())
        return XBOX_NIC_LINK_ACTIVE;            /* the old answer */
    nic_lock();
    bmsr = phy_read(MII_BMSR);
    nic_unlock();
    if (!(bmsr & BMSR_LSTATUS))
        return 0;                               /* no cable */
    return XBOX_NIC_LINK_ACTIVE | XBOX_NIC_LINK_100MBPS | XBOX_NIC_LINK_FULL_DUPLEX;
}

void xbox_NicPhyInitialize(int force_reset)
{
    nic_note_probe("PhyInitialize");
    if (nic_off_switch())
        return;
    nic_lock();
    if (force_reset)
        phy_reset();
    s_nic.phy[MII_BMCR] &= (uint16_t)~BMCR_ANRESTART;
    nic_unlock();
}

void xbox_NicMacAddress(uint8_t mac[6])
{
    /* FNV-1a 64 over the save directory: one address per install, the same
     * on every boot of it. */
    const char *p = xbox_path_save_dir_utf8();
    uint64_t h = 1469598103934665603ull;
    int i;

    for (; p && *p; p++) {
        h ^= (uint8_t)*p;
        h *= 1099511628211ull;
    }
    for (i = 0; i < 6; i++)
        mac[i] = (uint8_t)(h >> (i * 8));
    mac[0] = (uint8_t)((mac[0] & 0xFC) | 0x02);   /* unicast, locally administered */
}

uint32_t xbox_NicPeekReg(uint32_t reg)
{
    uint32_t v;

    if (reg >= NIC_SPAN_MAX)
        return 0;
    nic_lock();
    v = nic_read32(reg & ~3u);
    nic_unlock();
    return v;
}

uint16_t xbox_NicPeekPhy(uint32_t reg)
{
    uint16_t v;

    nic_lock();
    v = phy_read((int)reg);
    nic_unlock();
    return v;
}

void xbox_NicReport(void)
{
    if (!s_probed)
        return;
    fprintf(stderr, "  [NIC] %u reads, %u writes, %u MII operations, %u frames"
                    " refused (no carrier), %u undecoded\n",
            s_nic.reads, s_nic.writes, s_nic.mii_ops, s_nic.tx_done,
            s_nic.undecoded);
    fflush(stderr);
}
