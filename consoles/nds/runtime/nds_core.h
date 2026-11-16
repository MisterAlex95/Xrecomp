/* nds_core.h -- the Nintendo DS spike's machine (XBA-223, epic XBA-217): the ARM9 (ARMv5TE interpreter + lifted
 * code), its memory map, CP15, IRQs, timers, DMA, the IPC registers with a stub ARM7 in C, a BIOS in C (HLE: no
 * BIOS image), the 2D engines' text backgrounds. Linux only, headless, enough to run a libnds (calico) hello world;
 * not a DS emulator yet. C11 + libc, no host layer. */
#ifndef NDS_CORE_H
#define NDS_CORE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "nds_recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nds_info {
    char title[13];
    char game_code[5];
    uint32_t arm9_rom_offset, arm9_entry, arm9_ram, arm9_size;
    uint32_t arm7_rom_offset, arm7_entry, arm7_ram, arm7_size;
} nds_info;

/* Parses a .nds header: 0, or -1 with why. */
int nds_parse_header(const uint8_t *rom, size_t size, nds_info *out, char *why, size_t why_n);

/* A machine with the ROM's ARM9 binary loaded at its RAM address (what the firmware does), direct boot at its
 * entry. NULL with why on a bad ROM. The ARM7 binary is not run (stub ARM7). */
nds_machine *nds_create(const uint8_t *rom, size_t size, char *why, size_t why_n);
void nds_destroy(nds_machine *m);
void nds_set_verbose(nds_machine *m, int level);

/* Runs until the next frame starts (line 0 after VBlank). Returns 0, or -1 when the machine stopped (see
 * nds_error). */
int nds_run_frame(nds_machine *m);
const char *nds_error(const nds_machine *m);
/* The last instructions the interpreter ran and the registers (debugging). */
void nds_dump_trace(const nds_machine *m, FILE *f);
uint64_t nds_frames(const nds_machine *m);

/* The two screens' last frames, 256x192 BGR555 (DS order: bit 0-4 red). top: the engine POWCNT1 bit 15 routes to the
 * top screen. */
const uint16_t *nds_screen(const nds_machine *m, int top);

/* Debug reads of the ARM9's view of memory (no side effects). */
uint32_t nds_peek32(nds_machine *m, uint32_t addr);

#ifdef __cplusplus
}
#endif

#endif
