/*
 * xbox_nic.h -- the MCPX Ethernet controller, with no cable plugged in
 * (XBA-117).
 *
 * A title with Xbox Live or system link links XNet (and XOnline, WinSock on
 * Xbox) into its image. That code drives the nForce MAC directly, through
 * 1 KB of registers at 0xFEF00000, and asks the kernel about the PHY through
 * PhyInitialize / PhyGetLinkState. Before this model the registers were plain
 * zeroed RAM and PhyGetLinkState said the link was up, a combination no
 * console ever presents: a driver that believed the link and waited for the
 * MAC to say anything could wait forever.
 *
 * This is a console with no Ethernet cable: the MAC resets and answers, the
 * PHY behind the MII management registers answers with link down and
 * autonegotiation never completing, nothing is ever received, the link-change
 * interrupt never comes, and a frame queued for transmit completes at once
 * with "carrier lost". PhyGetLinkState reads the same PHY and says "no link".
 * Live and system link stay out of scope: the point is that the network stack
 * fails the way it does on a console without a cable, quickly, so the title
 * reaches its menus.
 *
 * Written from the register layout of the Linux forcedeth driver (offsets
 * and bit names only, which are the hardware's interface), IEEE 802.3
 * clause 22 for the PHY registers, and facts about the Xbox variant (PHY at
 * MII address 1, a 0x400-byte register BAR, 8-byte descriptors of a 32-bit
 * buffer address, a 16-bit length - 1 and 16-bit flags) read from xemu's
 * nvnet.c. No code is taken from either; this file is MIT like the rest of
 * the runtime. docs/technical/network.md has the behaviour and the check
 * list.
 *
 * On by default. RECOMP_NIC=0 puts back the previous behaviour: the aperture
 * is plain memory and PhyGetLinkState answers "link up".
 */
#ifndef XBOX_NIC_H
#define XBOX_NIC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XBOX_NIC_BASE   0xFEF00000u
#define XBOX_NIC_REGS   0x00000400u     /* the register BAR */

/* PhyGetLinkState's flags (XNET_ETHERNET_LINK_*). */
#define XBOX_NIC_LINK_ACTIVE      0x01u
#define XBOX_NIC_LINK_100MBPS     0x02u
#define XBOX_NIC_LINK_10MBPS      0x04u
#define XBOX_NIC_LINK_FULL_DUPLEX 0x08u
#define XBOX_NIC_LINK_HALF_DUPLEX 0x10u

/* Arms the model once guest memory is mapped (xbox_MemoryLayoutInit calls
 * it). Resets the state, so a second layout gets a fresh controller. Does
 * nothing with RECOMP_NIC=0, or on a host with no trapped-MMIO decoder (it
 * says so once; the aperture is then plain memory, as before). */
void xbox_NicInit(void);

/* Before the aperture goes away (xbox_MemoryLayoutShutdown). */
void xbox_NicShutdown(void);

/* 1 while the model is armed (the registers fault and are answered here). */
int  xbox_NicEnabled(void);

/* 1 if the address is in the trapped span this model answers. */
int  xbox_NicOwnsAddress(uint32_t xbox_va);

/* Service a trapped access: ctx is the PCONTEXT of a Win32 VEH or the
 * ucontext_t * of a POSIX SA_SIGINFO handler. 1 when the instruction was
 * decoded and stepped over. xbox_VehMmioFault / xbox_PosixMmioFault route
 * here. */
int  xbox_NicHandleMmio(void *ctx, uint32_t xbox_va);

/* The kernel's PHY routes (kernel_xbox.c): the same PHY the MII registers
 * reach. Link state is XBOX_NIC_LINK_* flags; 0 is "no cable". With the model
 * off they keep the old answers (link up, success). */
uint32_t xbox_NicPhyLinkState(void);
void     xbox_NicPhyInitialize(int force_reset);

/* The factory Ethernet address (EEPROM XC_FACTORY_ETHERNET_ADDR): a locally
 * administered unicast address derived from this install's save directory,
 * so it is stable for an install and never a real console's. */
void xbox_NicMacAddress(uint8_t mac[6]);

/* The register file and PHY, as the model sees them, for tests and the exit
 * report. reg is a byte offset in the BAR; phy a clause-22 register. */
uint32_t xbox_NicPeekReg(uint32_t reg);
uint16_t xbox_NicPeekPhy(uint32_t reg);

/* Exit report: counts, and whether the title ever looked. */
void xbox_NicReport(void);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_NIC_H */
