# Sega Naomi and Sammy Atomiswave

Naomi (1998 to about 2008: Crazy Taxi, The House of the Dead 2, Marvel vs. Capcom 2, Ikaruga, Guilty Gear XX...) is
Sega's arcade board built from the Dreamcast's chips; Atomiswave (Sammy, 2003 to 2009: Metal Slug 6, The King of
Fighters XI, Dolphin Blue...) is another arcade board from the same chips. The CPU, the GPU and the sound chip are the
Dreamcast's, so the Dreamcast frontend and runtime (`consoles/dc`, XBA-227) apply as they are. This page lists what
differs, what the core does about it today (the Naomi mode, XBA-235) and what epic XBA-221 still has to build.

Sources are public only: MAME's drivers (`src/mame/sega/naomi.cpp`, `naomibd.cpp`, `naomirom.cpp`, `naomim1.cpp`,
`naomim2.cpp`, `naomim4.cpp`, `naomigd.cpp`, `awboard.cpp`, `315-5881_crypt.cpp`, `mie.cpp`), Flycast's source used
as documentation (`core/hw/naomi/naomi_cart.h`: the cartridge header), and community wikis. We hold no BIOS, no
cartridge or DIMM dump, no key, and none goes in the repository: the epic's games come from boards Alex owns and stay
outside it. The spike had no Naomi software to look at, by design: anything marked *to check* is from the sources
above and was not tried against a board.

## Naomi against the Dreamcast

| | Dreamcast | Naomi | Core today (`DC_MODEL_NAOMI`) |
|---|---|---|---|
| CPU | SH-4 (SH7091) 200 MHz | same | same |
| GPU | PowerVR2 CLX2, **8 MB** VRAM | same chip, **16 MB** VRAM | 8 MB (the framebuffer read-out only; 16 MB with the TA work) |
| Sound | AICA (ARM7), **2 MB** sound RAM | same chip, **8 MB** | 8 MB, mirrored every 8 MB |
| Main RAM | **16 MB** at 0x0C000000, mirrored at 0x0D000000 | **32 MB**: 0x0C000000-0x0DFFFFFF (MAME: "Mirror on DC, Extra RAM on Naomi") | 32 MB, mirrored to 0x0FFFFFFF |
| Area 0 low | 2 MB BIOS ROM, 128 KB flash at 0x00200000 | 2 MB BIOS ROM, **32 KB battery SRAM** at 0x00200000 | no BIOS image (reads 0), SRAM (erased: 0xFF) |
| G1 window 0x005F7000-0x005F70FF | the GD-ROM drive (ATA registers) | **the cartridge board** (ROM / DIMM, below) | the board's PIO / DMA registers |
| G1 DMA 0x005F7400- | GD-ROM data to RAM | cartridge data to RAM | instant copy, ISTNRM bit 14 at the end |
| Media | GD-ROM | ROM cartridge, or GD-ROM + DIMM board | a ROM image file the user supplies |
| Controls | Maple pads | **JVS** I/O board through the **MIE** Maple-JVS bridge | a JVS packet stub (`dc_jvs_transfer`) |
| Settings | flash | SRAM + serial EEPROM (game settings, *to check* which side) | SRAM only, not saved |
| Boot | BIOS reads IP.BIN, runs 1ST_READ.BIN | BIOS checks the cartridge, copies its load list to RAM, runs it | the same in C (`dc_naomi_boot`) |

Naomi 2 (two CLX2 and an Elan T&L chip: Virtua Fighter 4 Evolution, Beach Spikers...) and Naomi multi-board setups
are out of the epic's first scope.

## The cartridge board and its ROM access protocol

Every Naomi game is reached through the same 16-bit registers, one per 32-bit word in the G1 window (MAME
`naomibd.cpp`, "each port is 16 bit wide"):

| Address | Register | Use |
|---|---|---|
| 0x005F7000 | ROM_OFFSETH | bits 31-16 of the PIO offset. Bit 31: advance by 2 after each ROM_DATA access ("auto-advance"); bits 28-30: the board's mode bits (for encrypted boards: which view, decrypted or not) |
| 0x005F7004 | ROM_OFFSETL | bits 15-0 |
| 0x005F7008 | ROM_DATA | the 16-bit word at the offset; writes go to the board (flash carts, the protection) |
| 0x005F700C / 0x005F7010 | DMA_OFFSETH / L | the source of the next G1 DMA, same layout |
| 0x005F7014 | DMA_COUNT | stored by the board |
| 0x005F7078 / 0x005F707C | BOARDID write / read | the bit-banged serial EEPROM on the cartridge (its ID: CS, RST, SCL, SDA; read bit 15) |

A transfer is the Dreamcast's G1 DMA (SB_GDSTAR 0x005F7404 the RAM destination, SB_GDLEN 0x005F7408, SB_GDDIR
0x005F740C read, SB_GDEN 0x005F7414, SB_GDST 0x005F7418 start), the data coming from the board at DMA_OFFSET.

The header, at ROM offset 0 (Flycast `RomBootID`; offsets worked out from the field sizes):

| Offset | Field |
|---|---|
| 0x000 | board name, 16 bytes: "NAOMI" (Naomi 2: "NAOMI2") |
| 0x010 | maker, 32 bytes |
| 0x030 | eight game titles (Japan, USA, export, Korea, Australia...), 32 bytes each |
| 0x130 | date (year u16, month, day), 0x134 game ID (4 bytes), 0x138 ROM mode (non-zero: the BIOS reads with offset bit 29), 0x13A ROM wait flag, 0x13C ROM wait values |
| 0x15C | ROM IDs, 0x1E0 coin settings per region, 0x260 credit sequences |
| 0x360 | the game load list: eight (ROM offset, RAM address, length) entries; offset 0xFFFFFFFF ends it |
| 0x3C0 | the test mode load list, same form |
| 0x420 / 0x424 | the game / test mode entry points |
| 0x428 | country, cabinet, resolution, vertical (monitor rotation), serial ID, service mode flags |

### The boards

- **Plain ROM boards**: mask ROMs / flash behind the registers, nothing between. What the core reads today; also what
  any decrypted dump looks like.
- **M2** (most of the classic library): the **315-5881** chip decrypts and decompresses on the way out when the
  offset's mode bits select it. MAME describes it as a block cipher on 16-bit words (a Feistel-type network with fixed
  S-boxes, keyed per game) followed by a line-based decompression; the game programs the "protection" start
  address through ROM_DATA writes. **The per-game key is not in the cartridge's readable ROM**; MAME carries it in its
  game tables. We do not copy any key.
- **M1**: an Actel FPGA that decompresses a dictionary-coded stream with a per-game key and checksum (`naomim1.cpp`).
- **M4** (later games): a 16-bit block cipher with fixed S-boxes and a per-game key, plus a key EEPROM (`naomim4.cpp`).
- **GD-ROM + DIMM board** (`naomigd.cpp`): at power on the DIMM board copies the encrypted GD-ROM image into its
  DIMMs, decrypting it with DES and a key held in the security PIC on the board; the game then reads the DIMM through
  the same registers as a plain board. The useful form for us is the decrypted DIMM image.

What this means for XRecomp: the runtime reads a **plain or decrypted image** of a cartridge or DIMM Alex owns. How
he obtains it from his own board (a dumper running on his Naomi reads through the board, which decrypts itself; or a
key the user supplies, like the Xbox 360's `--key-file`) is a step for the epic (*to check*: the dump formats MAME
and Flycast accept, and whether encrypted views beyond the program, read later by the game, must be pre-decrypted
too).

## JVS through the Maple-JVS bridge

Naomi has no Maple pads. Maple port 0 goes to the **MIE** (315-6146, a Z80-based controller on the main board,
MAME `mie.cpp`), which turns Maple frames into **JVS** (RS-485) packets for the I/O board (837-13551 and others): coin
counters, TEST / SERVICE, two players' switches, analog inputs, outputs (lamps, coin meters). A JVS packet is
`E0 node length data... checksum`, with D0 escaping E0 / D0 in the stream; commands include reset (F0 D9), set
address (F1), ID (10), versions (11-13), features (14), switches (20), coins (21), analog (22), outputs (32).

Naomi keeps its own JVS layer. Today the core has the JVS side as a stub: `dc_jvs_transfer` answers reset, set
address, ID ("XRECOMP;JVS I/O STUB"), versions, features (2 players x 13 switches, 2 coin slots), switches and coins from `dc_jvs_state`. The MIE
side (Maple DMA frames, the MIE's own commands and its EEPROM access) is not modelled: the Dreamcast core has no
Maple yet either.

## The BIOS and its role

The Naomi BIOS (2 MB, region-specific: Japan, USA, export, Korea...) tests the hardware, shows the boot screens,
reads and checks the cartridge (header, region, the board's ID EEPROM, ROM checksums), runs the operator **test
menu** (coin settings, game settings in SRAM / EEPROM, input tests) and finally copies the load list to RAM and jumps
to the game. Games use few BIOS services afterwards (*to check* which: the Dreamcast-style syscall vectors, the
EEPROM and the system flags).

The core does it in C, like the Dreamcast BIOS (`dc_bios.c`): `dc_naomi_boot` checks the board name, copies the game
load list, sets the CPU up as for a disc's program (privileged, bank 1, interrupts blocked, the stack below
0x8C00F400, PR at the "exit" stub) and starts at the entry point. Not there: the test menu (the game's own test mode
entry 0x424 is the better path), region selection, the checks, ROM wait settings, the EEPROM.

## Atomiswave

Sammy's board is Dreamcast-sized (MAME `naomi.cpp`'s Atomiswave machine: *to check* the RAM sizes; it is closer to
a Dreamcast than to a Naomi) with its own media and I/O:

- **BIOS**: 128 KB flash at 0x00000000 (writable: the games' settings live there), no SRAM.
- **Cartridge** (`awboard.cpp`): different registers in the same G1 window: AW_EPR_OFFSETL / H (0x005F7000 /
  0x005F7004) for the program area, AW_MPR_RECORD_INDEX (0x005F700C), AW_MPR_FIRST_FILE_INDEX (0x005F7010),
  AW_MPR_FILE_OFFSETL / H (0x005F7014 / 0x005F7018) for a small file system in the mask ROMs, AW_PIO_DATA
  (0x005F7080, undecrypted reads). Encryption: an 8-bit per-game key choosing an XOR constant, an S-box set and a bit
  permutation, address-dependent; MAME notes the key equals the checksum of the decrypted code, so the board checks the
  program before opening the data area.
- **Controls**: not JVS: the inputs and coin lines are read in area 0's "modem" range (0x00600000-, MAME aw_modem);
  some cabinets have light guns or trackballs there.

In the core, Atomiswave would be a third `dc_model`: the flash BIOS area, the AW board registers and its file
system, the inputs in the modem range. Not started.

## What the runtime has today (XBA-235)

`consoles/dc/runtime`, Linux / macOS headless, nothing in the app, off unless asked for:

- `dc_create_model(DC_MODEL_NAOMI)`: 32 MB main RAM (`sh4_cpu.ram_mask` sizes the fast RAM path of the interpreter and
  lifted code: `DC_RECOMP_ABI` 2, the Dreamcast keeps 16 MB and its mirrors), 8 MB sound RAM, the SRAM, the board's
  G1 window and G1 DMA (`dc_naomi.c`).
- `dc_naomi_insert_cart(m, path)`: the cartridge, a plain / decrypted ROM image file the user supplies (read whole, up
  to 1 GB); `dc_naomi_boot`: the BIOS in C. `dc_run --naomi CART.bin` runs one headless, with the usual options
  (`--png`, `--serial`, `--pack` for lifted code: the program is in RAM after boot, so a pack is made from the
  program's bytes as the load list places them).
- Tests (`consoles/dc/tests`): `dc_naomi` builds a synthetic cartridge in the test (a header whose load list copies a
  small SH-4 program assembled there, and patterned data), writes it to a temporary file and boots it; the program
  checks the 32 MB RAM and its mirror, the SRAM, the 8 MB sound RAM, PIO reads with and without auto-advance, the
  header through PIO, a G1 DMA and its interrupt bit; the test also checks that a Dreamcast machine still mirrors its
  16 MB and 2 MB, that bad headers are refused, and the JVS stub's replies. `dc_naomi_hello` wraps our KallistiOS
  hello world in a synthetic cartridge and runs it under `dc_run --naomi`: the same serial lines and framebuffer as on
  the Dreamcast (skipped without the homebrew corpus).

## What the epic still needs, and estimates

Agent-days, after the Dreamcast work the Naomi shares (the PowerVR2 TA / ISP / TSP, AICA, Maple, G2 / PVR DMA: on
XBA-221's other tickets, not counted here):

| Piece | Estimate | Needs Alex's board? |
|---|---|---|
| A decrypted dump of one game Alex owns, and its format (load it as it is, or a small converter) | 1-2 | **yes**: the cartridge (or GD-ROM + DIMM) and a way to read it through his Naomi |
| BIOS in C completed for that game: what it calls after boot, ROM wait / G1 setup, system flags, region | 2-4 | a running board helps to compare boot behaviour |
| 16 MB VRAM (the 64-bit view's banks), Naomi's video modes (31 kHz, vertical monitors: rotation in the host) | 1-2 | no |
| MIE: Maple frames to JVS, its EEPROM; the JVS layer completed (analog, outputs); coin / start / service / test mapped to the host's input | 3-4 | the I/O board's ID string and features from his board (*to check*) |
| SRAM and EEPROM saved per game (host saves directory) | 1 | no |
| On-the-fly decryption, only if a game reads encrypted views late (M2 315-5881, M1, M4), keys supplied by the user, never shipped | 3-5 per board type | the board, to verify |
| Recompilation of the game program (the dc_tools discovery loop on the loaded image) and Linux arm64, then the Thor | 3-5 | the Thor for the device pass |
| Atomiswave (third model: flash BIOS, AW board + file system, inputs; its decryption likewise user-keyed) | 4-6 | an Atomiswave cart Alex owns |

About 3 to 4 weeks of agent time to one Naomi game playing on Linux arm64 and the Thor, once the Dreamcast renders 3D.
Biggest unknowns: how the dump is made (it decides whether decryption is needed at all) and what the games ask of the
BIOS after boot.
