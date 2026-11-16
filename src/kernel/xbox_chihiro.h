/**
 * Sega Chihiro mode (XBA-234, epic XBA-232).
 *
 * Chihiro is Sega's arcade board built on the original Xbox: the same Pentium III,
 * NV2A and MCPX (an X2 revision), 128 MB of RAM instead of 64, a media board
 * (DIMM / GD-ROM), a JVS I/O interface on USB and Sega's own BIOS and kernel build.
 * docs/technical/chihiro.md has the differences and what the runtime still lacks.
 *
 * The mode is a runtime setting, off by default, so an Xbox title behaves exactly
 * as before:
 *
 *   RECOMP_CHIHIRO=1   (any value but empty or "0")
 *
 * What it changes today:
 *   - RAM: xbox_MemoryLayoutInit maps 128 MB (g_xbox_total_ram = XBOX_CHIHIRO_RAM),
 *     so the heap top, the mirror stride and MmQueryStatistics follow;
 *   - kernel variant: XboxHardwareInfo.Flags carries XBOX_HW_FLAG_ARCADE, the bit
 *     a title tests to know it runs on the arcade kernel;
 *   - the XBE loader also decodes the kernel thunk address with the Chihiro key.
 *
 * Not modelled yet (step 2 of the epic): the media board (I/O ports 0x4000-0x40FF,
 * the IDE slave on the secondary channel, mbfs/mbcom/mbrom), the JVS USB devices,
 * the base board EEPROM and SegaBoot.
 */
#ifndef XBOX_CHIHIRO_H
#define XBOX_CHIHIRO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XBOX_CHIHIRO_RAM        (128u * 1024u * 1024u)  /* 128 MB, as the Chihiro Xbox board */

/* XBOX_HARDWARE_INFO.Flags bits (nxdk lib/xboxkrnl/xboxkrnl.h). */
#define XBOX_HW_FLAG_INTERNAL_USB_HUB   0x00000001u
#define XBOX_HW_FLAG_DEVKIT_KERNEL      0x00000002u
#define XBOX_HW_FLAG_ARCADE             0x00000008u

/* The XOR keys a Chihiro XBE header uses for its entry point and kernel thunk
 * address (retail 0xA8FC57AB / 0x5B6D40B6, debug 0x94859D4B / 0xEFB1F152). */
#define XBE_ENTRY_CHIHIRO_XOR   0x40B5C16Eu
#define XBE_THUNK_CHIHIRO_XOR   0x2290059Du

/* 1 when RECOMP_CHIHIRO is set to anything but "" or "0", else 0. Read from the
 * environment on each call (it is asked a handful of times at boot). */
int xbox_ChihiroMode(void);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_CHIHIRO_H */
