# Sega Chihiro

Chihiro is Sega's arcade board built on the original Xbox (2002 to about 2006: The House of the Dead III, Virtua Cop 3,
OutRun 2, Crazy Taxi High Roller, Ghost Squad...). The CPU, the GPU and most of the chipset are the Xbox's, so the
XRecomp stack (recompiler, kernel replacement, D3D / NV2A, APU) applies almost as it is. This page lists what differs,
what the runtime does about it today (`RECOMP_CHIHIRO=1`, XBA-234) and what epic XBA-232 still has to build.

Sources are public only: MAME's driver (`src/mame/sega/chihiro.cpp`, the most detailed hardware description),
Cxbx-Reloaded's kernel (`src/core/kernel/init/CxbxKrnl.cpp`, `src/common/xbe/Xbe.h`) and nxdk's `xboxkrnl.h`. We hold no
BIOS, no media board firmware, no game image, and none goes in the repository: the epic's games come from boards Alex
owns and stay outside it, like any Xbox disc.

Not verified yet: anything marked *to check*. The spike had no Chihiro software to look at, by design.

## The hardware

Two metal boxes stacked into one (MAME's description):

| Box | Board | Role |
|---|---|---|
| bottom | **Xbox board** (Microsoft) | Mostly a v1 retail motherboard: Pentium III 733 MHz, NV2A, **128 MB RAM**, **MCPX X2** (retail: X3), a 512 KB BIOS flash (Macronix 29F040), a PIC16LC63A |
| bottom | **Base board** 171-8204B (Sega) | Video output, audio, **JVS**; tied to the Xbox board by a 40-pin IDE cable (CN12) and a 16-pin LPC cable (CN11). EEPROMs: 24LC64 IC10 (firmware, serial, region byte at 0x1F10: 1 Japan, 2 USA, 3 export), 24LC64 IC32, 24LC024 IC11 |
| top | **Media board** 171-8234C (Sega) | Flash with the **SegaBoot** loader (two 1 MB images in 2 MB), two 72-pin **DIMM** sockets (the game runs from DIMM RAM), the **GD-ROM** connector, a Sega network connector |
| top | Network board | The Triforce v3 one |
| | Filter board | Power, DIP switches, the TEST / SERVICE buttons |

Games ship on a GD-ROM with a security PIC: at power on the media board copies the (encrypted) disc into the DIMMs,
the PIC holding the key; after that the GD-ROM is no longer read. Later games came on a DIMM / flash cartridge only.
For us the useful form is the decrypted DIMM image of a game Alex owns; how to obtain it from his own board is a step 3
question (*to check*: the MAME / Chihiro-Tools dump formats).

## Memory map

The CPU side is the Xbox's with the RAM doubled. Addresses are guest (title) addresses.

| Range | Xbox | Chihiro | Runtime today |
|---|---|---|---|
| RAM | `0x00000000-0x03FFFFFF` (64 MB) | `0x00000000-0x07FFFFFF` (**128 MB**) | 64 MB; **128 MB with `RECOMP_CHIHIRO=1`** |
| RAM wrap (mirrors) | every 64 MB (26-bit bus) | every 128 MB (27 bits) | follows the RAM size: mirrors at multiples of the map size |
| contiguous view (`MmAllocateContiguousMemory`) | `0x80000000` + 64 MB | `0x80000000` + 128 MB | the held window; contiguous arena unchanged |
| tiled aperture | `0xF0000000` | same | same |
| NV2A registers | `0xFD000000` (16 MB) | same | same |
| MCPX APU | `0xFE800000` | same | same |
| BIOS flash | `0xFF000000`, 1 MB window, mirrored up to 4 GB | `0xFF000000-0xFF07FFFF`, mirrored (MAME `mirror(0x00f80000)`) | the flash aperture, empty: titles do not read the BIOS |
| I/O ports | MCPX ACPI 0x8000, SMBus 0xC000, IDE 0x1F0 / 0x170 | the same, **plus the media board at 0x4000-0x40FF** (IRQ 10) | port reads are `kernel_hal.c`'s "x86 port I/O": 0x4000-0x40FF read 0 |
| USB | OHCI 0 (pads on 4 ports) | OHCI 0 port 1: **JVS** (AN2131QC), port 2: base board serial (AN2131SC); ports 3 and 4 unused | `src/usb` models pads only |
| IDE secondary, slave | the DVD drive (master) | **the media board** (`ide1:1`) | `kernel_disc` serves D: from the image; no media board device |

What the 128 MB changes in the runtime is what a devkit layout (`XBOX_DEVKIT_RAM`) already exercised:
`g_xbox_total_ram` sizes the heap top, the mirror stride and, since XBA-234, `MmQueryStatistics.TotalPhysicalPages`
(32768). Unlike `xbox_SetMapSize`, which grows only the mapping, a Chihiro title must be *told* 128 MB, because it sizes
its pools from it.

## Kernel differences

The Chihiro BIOS carries Sega's build of the Xbox kernel. What is known publicly:

- **XBE keys**. A Chihiro XBE encodes its entry point and kernel thunk address with a third key pair: entry
  `0x40B5C16E`, thunk `0x2290059D` (retail `0xA8FC57AB` / `0x5B6D40B6`, debug `0x94859D4B` / `0xEFB1F152`;
  Cxbx-Reloaded `Xbe.h`). Some Chihiro games still use the debug keys (Cxbx-Reloaded notes it and falls back on a
  `boot.id` next to the XBE to recognise a Chihiro title). `tools/xbe_parser` tries the three pairs (`entry_key` =
  `"chihiro"`); the runtime's loader tries the Chihiro thunk key in Chihiro mode.
- **`XboxHardwareInfo.Flags`** (export 322) carries `XBOX_HW_FLAG_ARCADE` = `0x00000008` (nxdk `xboxkrnl.h`; the other
  bits: internal USB hub 0x1, devkit kernel 0x2, 480p Macrovision 0x4). A title or its XDK libraries test it to take
  the arcade paths. The MCPX revision byte differs too (X2), *to check* which value the Chihiro kernel reports.
- **No hard disk**. The Chihiro kernel sets `bDontSetupHarddisk` in the XBE's init flags before starting the title,
  which skips XAPI's cache-partition setup (Cxbx-Reloaded does the same, "the Chihiro kernel does this"). There is no
  FATX disk: saves and settings go to the media board and the base board EEPROM. Our runtime already lifts XAPI and
  serves T: / U: / Z: from host folders, so nothing breaks; a title that probes the disk may need the flag honoured
  (*to check* with a real title).
- **Media board devices**. The kernel exposes the media board as object names a title opens like files:
  `\??\mbfs:` (media board file system), `\??\mbcom:` (the command channel), `\??\mbrom0:` and `\??\mbrom1:` (the two
  1 MB flash halves, at LBA `0x8000000` and `0x8000800` of the IDE device; MAME's partition notes). These are names
  under the object manager, not new exports.
- **No UEM**. The retail kernel shows the "Universal Error Message" screen on a fatal error; the Chihiro one does not
  (Cxbx-Reloaded exits instead). Our runtime has no UEM either.
- **SegaBoot**. The BIOS starts SegaBoot from the media board flash, which shows the boot / network / test screens,
  loads the game into the DIMMs and launches its XBE. A recompiled title does not need it: the runtime starts the
  game's XBE directly with the DIMM image already "loaded" (Cxbx-Reloaded extracts SegaBoot from a media board ROM
  dump instead; we will not).
- **Exports**. No public list of export differences was found. The arcade titles Cxbx-Reloaded runs import the
  retail ordinals through the same kernel replacement, which suggests the table is the retail one plus the arcade
  flag. *To check* in step 3: list the kernel imports of Alex's games (`python3 -m tools.xbe_parser` prints them) and
  diff them against `src/kernel/kernel_stub_table.c`; anything above ordinal 366 or unknown is the real difference.

## Media board protocol

From MAME's `ide_baseboard_device` and `mediaboard_r` / `mediaboard_w` (MAME's names; the board is the media board):

**I/O ports `0x4000-0x40FF`** (32-bit registers, offset from 0x4000):

| Offset | Read | Meaning (MAME) |
|---|---|---|
| `0x1C` | high half `0x1000` | identification |
| `0x20` | `0x000000A0` low, `0x4258` high | identification |
| `0x24` | `0x4D41` low | identification ("MA") |
| `0xF0` | 0 | media board present |
| `0xF4` | 2 | DIMM size: 2 = 512 MB ("must be 2") |
| `0xE0` (write) | | acknowledges IRQ 10 |

**The IDE slave** (secondary channel) is a sector device with three regions:

- LBA `>= 0x08000000`: the media board flash (`mbrom0` / `mbrom1`), `(lba & 0x7FF) * 512` into it;
- the last `0x8000` sectors of the device (from `0xF8000` with the 512 MB setting) are the command window: a
  title (through `\??\mbcom:`) writes a request to relative LBA `0x4800`, then to `0x4801`, which triggers it; the
  board answers into the buffer read back at `0x4800` and raises IRQ 10. Commands seen in MAME: `0x0001`, `0x0100`,
  `0x0101` (firmware version, `0x1234` = "12.34"), `0x0102`, `0x0103` (serial number); the rest are unknown (*to check*);
- anything below: the DIMM contents, i.e. the game's own file system (`\??\mbfs:`).

So the media board is two things for us: a *file system* (the game's data, from its DIMM image) and a small *command
channel*. With the kernel replaced, both can be served at the object-name level (`mbfs:` → a folder or the DIMM
image, `mbcom:` → a command handler) without modelling IDE; the I/O registers at 0x4000 are a handful of constants.

## JVS I/O

JVS is Sega's arcade I/O bus (RS-485, 115200 baud, packets `E0 <node> <len> <data...> <sum>`). A JVS I/O board (837-13551
and similar) reports switches (players' buttons, START, TEST, SERVICE), coin counters, analog channels (wheels,
pedals), light-gun screen positions, and drives outputs (lamps, coin meters).

On Chihiro the base board bridges JVS to **USB**: a Cypress EZ-USB **AN2131QC** on OHCI port 1 (vendor `0x0CA3`,
product `0x0002`, class `0x60`). MAME's device: vendor command `0x20` sends a JVS packet (sync byte `0xE0` and checksum
checked), `0x19` fetches the reply, `0x16` / `0x17` read the base board EEPROM; the sense line tells the host whether a
node answered (50 none, 0 addressed, 25 otherwise). A second chip, the **AN2131SC** on port 2 (product `0x0003`,
subclass `0x01`), is a serial / MIDI port (commands `0x1A`, `0x1B`, `0x22`, `0x23`, mode `0x2E` / `0x2F`) used by some
cabinets (force feedback, card readers).

For the runtime that means a USB device model next to `src/usb/usb_gamepad.c` on our OHCI: the AN2131QC's vendor
requests, a JVS master, and a virtual I/O board answering the standard JVS commands (`0xF0` reset, `0xF1` set address,
`0x10` identify, `0x11`-`0x14` versions and features, `0x20` switches, `0x21` coins, `0x22` analog, `0x25` screen
position, `0x30` / `0x31` coin decrease / increase, `0x32` outputs), its inputs taken from the pad / keyboard / touch
(coin, START, TEST, SERVICE, and per game the wheel, pedals, gun).

## What the runtime has (XBA-234)

`RECOMP_CHIHIRO=1` (`src/kernel/xbox_chihiro.h`), off by default: an Xbox title is unchanged.

| Piece | Where | State |
|---|---|---|
| 128 MB RAM, the map, the mirrors at 128 MB, the heap top | `xbox_MemoryLayoutInit` | done |
| `MmQueryStatistics.TotalPhysicalPages` from the RAM size | `kernel_memory.c` | done (default still 16384) |
| `XboxHardwareInfo.Flags = XBOX_HW_FLAG_ARCADE` | `kernel_bridge.c` `kernel_data_init` | done |
| Chihiro XBE keys | `tools/xbe_parser` (three pairs), the runtime's thunk decoding (Chihiro mode) | done |
| Booted in CI | `tests/chihiro_mode` (the synthetic XBE: 128 MB map, no wrap at 64 MB, wrap at 128 MB, a 72 MB allocation, the arcade flag, the Chihiro thunk key; and the Xbox mode unchanged), and `tools/prepare/test_desktop_build.py`'s end-to-end test runs the lifted synthetic title a second time with `RECOMP_CHIHIRO=1` (`XBA_E2E=1`, the Linux CI image) | done |
| Media board: ports 0x4000-0x40FF, `mbfs:` / `mbcom:` / `mbrom:`, IRQ 10 | | step 2 |
| JVS (AN2131QC) and the serial chip (AN2131SC), the base board EEPROM (region, serial) | | step 2 |
| `bDontSetupHarddisk`, the MCPX X2 revision | | step 3, with a real title |
| A per-game setting and the arcade options (TEST menu, coin / free play, DIP switches) | profiles, `recomp.cfg` | step 3-4 |
| Prepare: recognise a Chihiro game (Chihiro key, `boot.id`), take a DIMM image as the source | `tools/prepare` | step 3 |

## Estimates (step 2)

Agent-days, from this spike; the comment on XBA-232 carries the same numbers.

| Work | Estimate | Notes |
|---|---|---|
| JVS: AN2131QC USB device on our OHCI, JVS master, a virtual 837-13551 I/O board (switches, coins, analog), mapping to pad / keyboard / touch, tests | 4-6 | The OHCI model and the pad device exist; JVS is a documented, small protocol. Light guns and wheels add 1-2 per kind |
| Base board: AN2131SC (stub that answers), EEPROM reads (region, serial) | 1-2 | |
| Media board, HLE: the 0x4000 registers and IRQ 10, `mbfs:` served from a DIMM image or folder, `mbcom:` commands (firmware, serial, then what titles send), `mbrom:` empty | 4-7 | The unknown commands are the risk: they show up only with a real title (step 3) |
| Prepare and settings: Chihiro detection, DIMM image as a source, arcade options | 2-3 | |
| **Total before a first real title** | **11-18** | Then step 3 (one of Alex's games on Linux arm64) is title debugging like any Xbox game |
