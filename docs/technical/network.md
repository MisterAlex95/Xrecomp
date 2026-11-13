# Network: a Console with No Cable

> Part of the [Documentation Index](../INDEX.md)

A title with Xbox Live or system link starts its network stack at boot, and that stack is XDK code lifted with the title
(XNet, XOnline, WinSock on Xbox): it drives the MCPX Ethernet controller's registers itself and asks the kernel about
the PHY. This runtime does not do networking. What it does (XBA-117) is present the one network every console can
have, **no cable plugged in**, so that the stack fails the way it does on a console without a cable — quickly — and
the title reaches its menus and plays single-player. Live and system link stay out of scope.

Source: [`src/kernel/xbox_nic.c`](../../src/kernel/xbox_nic.c), [`xbox_nic.h`](../../src/kernel/xbox_nic.h).
Switch: `RECOMP_NIC=0` gives the behaviour before (see [environment variables](../runtime/environment-variables.md#network)).

## Before

The 8 MB MCPX aperture was plain zeroed memory, the NIC's registers at 0xFEF00000 included, and `PhyGetLinkState`
answered `XNET_ETHERNET_LINK_ACTIVE`. No console ever presents that: the PHY says there is a link while the MAC
never answers anything. Whether a given XDK's stack then booted, spun on a register or waited forever for DHCP was
unknown.

## The model

The registers fault (the page at 0xFEF00000 is `PAGE_NOACCESS`) and the trapped-MMIO dispatch
(`xbox_PosixMmioFault` on Linux and Android, `xbox_VehMmioFault` on Windows) routes them to the model, after the APU,
AC'97 and OHCI. `xbox_MemoryLayoutInit` arms it, so no title project needs a change. A host with no trapped-MMIO
decoder keeps the registers as plain memory and says so; `PhyGetLinkState` still answers "no link" there.

| Part | What it answers |
|---|---|
| MAC reset | `TxRxControl` writes complete at once; `UnknownSetupReg5` bit 31, which a driver polls after one, reads set. Transmitter and receiver are never busy. |
| MII management | `MIIControl` operations complete at once (`INUSE` reads clear). The PHY answers at address 1; every other address reads 0xFFFF (no PHY), so a bus scan finds exactly one. |
| PHY (clause 22) | BMCR: autonegotiation enabled, reset and restart self-clear. BMSR: 10/100 half/full and autonegotiation capable, and **neither link status nor autonegotiation complete, ever**. ANLPAR 0: no partner. |
| Adapter | `AdapterControl` keeps what is written except `LINKUP`, which always reads clear. `MIIStatus` never latches a link change. |
| Transmit | With the transmitter started, a `KICK` completes every descriptor the driver owns with `ERROR \| CARRIERLOST` and latches `TX_ERR`: the driver gets its buffers back. With it stopped they stay queued, as on hardware. |
| Receive | Nothing, ever. |
| Interrupts | None. `KeConnectInterrupt` accepts the NIC's vector and nothing raises it: the link-change interrupt never comes. |
| `PhyGetLinkState` | Reads the same PHY: 0 (no `XNET_ETHERNET_LINK_*` flag). |
| `PhyInitialize` | Resets the PHY (with `ForceReset`), restarts autonegotiation, returns success. |
| EEPROM | `XC_FACTORY_ETHERNET_ADDR` is a locally administered unicast address hashed from this install's save directory: stable for the install, never a real console's. `XC_ONLINE_*` (static IP, DNS, gateway, mask) are 0: DHCP, a console never configured. |

`HalRegisterShutdownNotification` stays a no-op by design (`tools/kernel_audit/stubs.py`): a reboot here ends or
replaces the process, and the NIC has no DMA in flight for XNet's notification to stop. The network configuration
sectors XNet reads from the hard disk's config area are zero (the partition images start empty), which is a console
whose network was never set up.

### Where the facts come from

Register offsets and bit names: the Linux forcedeth driver (`drivers/net/ethernet/nvidia/forcedeth.c`), the public
description of the nForce MAC. The Xbox variant's specifics — PHY at MII address 1, a 0x400-byte BAR, 8-byte
descriptors (32-bit buffer address, 16-bit length − 1, 16-bit flags whose bits are forcedeth's v1 flags shifted down
by 16) — from xemu's `hw/xbox/mcpx/nvnet/nvnet.c`. Only facts about the hardware interface were taken; no code is
copied from either (forcedeth is GPL-2.0, xemu's file LGPL-2.0+, and the files listed in `NOTICE` are the only
xemu-derived code in this tree). `xbox_nic.c` is MIT like the rest of the runtime.

## What a title should then do

Decided by the lifted XDK code, not by us; this is what a console without a cable does, and what the check list
below confirms:

- `XNetStartup` / `WSAStartup` succeed: the stack initialises the controller and has no link.
- `XNetGetEthernetLinkStatus` returns 0 (no `XNET_ETHERNET_LINK_ACTIVE`).
- `XNetGetTitleXnAddr` returns `XNET_GET_XNADDR_NONE` or stays `PENDING`: no address without a link.
- `XOnlineStartup` succeeds; a sign-in (`XOnlineLogon`) fails at once with the XDK's "no network connection"
  error, which titles show as "check your network cable".
- System link menus list no games and the title says the cable is unplugged.

## Status and discovery

The first time a title touches the NIC registers or calls `PhyGetLinkState` / `PhyInitialize`, the log says so once:

```
[NET] nic probed (registers), link down: no cable (RECOMP_NIC=0 for the old link-up answer)
```

and the status file (`RECOMP_STATUS`) gains `"net":"nic probed, link down"` (`"nic probed, link up (RECOMP_NIC=0)"`
with the model off). `tools.compat` keeps it in the run summary and the result (`"net"`), and its report marks the
title "(network stack)", so the library report shows which titles have one. `RECOMP_NIC_TRACE=1` prints the first
400 register accesses.

## Tests

- `tests/nic_posix`: the model behind a real fault on Linux x86-64 and arm64. A forcedeth-style start-up — reset,
  EEPROM MAC programmed, MII scan, PHY reset and autonegotiation, link polled for 50 ms, a frame queued with the
  transmitter stopped then started — every wait bounded, the whole sequence asserted under a second with link down,
  frame refused with carrier lost, no link change, `PhyGetLinkState` 0, and the status line. A second CTest entry
  runs it with `RECOMP_NIC=0`.
- `tools/compat/test_e2e.py::test_a_title_with_a_network_stack_finds_no_cable` (`XBA_E2E=1`): a synthetic title of
  ours does the same sequence in x86 code, lifted and run through the compat runner; it prints `XBA117 NIC OK` only if
  every bounded wait ended and the link was down, and the result carries `"net"`. With `RECOMP_NIC=0` the same title
  takes its failure path (the reset bit never comes up out of plain memory).

## Check list (device or Mac, with titles from the library)

Pick two titles with Xbox Live sign-in at boot or on the main menu, and one with a system link menu (the library
report's "(network stack)" mark finds them).

1. Boot each title. It reaches its main menu with no multi-second stall after the logo compared to `RECOMP_NIC=0`
   (time it from launch both ways).
2. The log has `[NET] nic probed (...), link down` once, and the library check / status shows the title's network stack.
3. Open the Xbox Live menu (Live titles): the title reports no network connection / cable rather than hanging on
   "signing in".
4. Open the system link menu: it reports the cable unplugged, or lists no games; backing out returns to the menu.
5. Start a single-player game from each and play a minute: no hang, no crash.
6. If a title stalls or hangs at boot, run it again with `RECOMP_NIC_TRACE=1` and attach the `[NIC]` lines; then try
   `RECOMP_NIC=0` and say whether that boots.
