/* gba_recomp.h -- static recompilation of GBA games (Plane G-1 XBA-150, G-2 XBA-151, G-3 XBA-152).
 *
 * A GBA title pack (libtitle.so, pack.json with "console": "gba") holds the game's ARM / Thumb code lifted to C by
 * consoles/gba/tools (gba_tools) and compiled for the host. The runtime attaches its table to the machine: when the
 * CPU reaches an address the table covers, the recompiled function runs natively; anything else (code the discovery
 * did not find, code copied to IWRAM / EWRAM, computed jumps out of known code) runs in the interpreter, which also
 * records the ROM addresses it had to run so the next Prepare can lift them (the discovery loop). Graphics, sound,
 * timers, DMA, interrupts and saves are the same code either way.
 *
 * Contract (frozen between gba_tools, Prepare, the loader and the core; change together and bump GBA_RECOMP_ABI):
 * - `python3 -m gba_tools.recomp ROM OUTDIR [--discovered FILE]` writes C sources into OUTDIR (nothing else needed to
 *   build): compile each of them with -I consoles/gba/runtime into one shared library.
 * - That library exports `gba_recomp_title_table` (pack.json "entry"), returning the table below.
 * - The library is linked against the core's symbols (the shell / gba_play provide them at load time). */
#ifndef GBA_RECOMP_H
#define GBA_RECOMP_H

#include <stddef.h>
#include <stdint.h>

#include "gba_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 2 (XBA-201): lifted code stores the prefetched opcode as the open bus, adds RC_NEXTRA after instructions with data
 * or internal cycles and sequences LDM / STM per region, matching the core's timing: packs built for 1 are refused
 * (Prepare builds them again). */
#define GBA_RECOMP_ABI 2

/* Entered with the CPU state at the entry's address (ARM or Thumb as flagged); returns when control leaves the
 * recompiled code, with the CPU's PC (and mode) set to where execution continues. The core may also enter it at a
 * later address of the same function (resuming after a scheduler event or an IRQ): see gba_recomp_rt.h, which
 * holds the rest of the lifted code's ABI (machine layout, bus symbols, exit codes). Entries: the start of each
 * contiguous run of a function's instructions. */
typedef void (*gba_recomp_fn)(gba_machine *m);

enum { GBA_RECOMP_THUMB = 1u << 0 };

typedef struct gba_recomp_entry {
    uint32_t addr;  /* GBA address of the first instruction */
    uint32_t flags; /* GBA_RECOMP_THUMB */
    gba_recomp_fn fn;
} gba_recomp_entry;

typedef struct gba_recomp_table {
    uint32_t abi;               /* GBA_RECOMP_ABI */
    uint32_t count;             /* entries, sorted by addr */
    const gba_recomp_entry *entries;
    const char *rom_sha256;     /* the ROM the table was built from, lower-case hex */
} gba_recomp_table;

/* Exported by a GBA pack library. */
typedef const gba_recomp_table *(*gba_recomp_title_table_fn)(void);
#define GBA_RECOMP_ENTRY_SYMBOL "gba_recomp_title_table"

/* Attaches a table (NULL detaches: pure interpreter). Returns 0, or -1 when its ABI differs. */
int gba_attach_recomp(gba_machine *m, const gba_recomp_table *t);

/* Discovery: the distinct ROM addresses (bit 0 set = Thumb) where the interpreter had to start running code that
 * the attached table does not cover, since creation. Copies at most max into out, returns the total count. */
size_t gba_recomp_missed(gba_machine *m, uint32_t *out, size_t max);

/* ---- Helpers for hosts and tests (not part of the pack ABI; loading a pack: gba_pack.h) ---- */

typedef struct gba_recomp_stats {
    uint64_t native;       /* instructions lifted code ran (only counted by packs built with -DGBA_RC_COUNT) */
    uint64_t interp_rom;   /* cartridge instructions the interpreter ran while a table was attached */
    uint64_t fallbacks;    /* of those, instructions lifted code handed to the interpreter (SWI, MSR, ...) */
    uint64_t calls;        /* calls into lifted code */
    uint64_t slow_lookups; /* dispatch cache misses */
} gba_recomp_stats;
void gba_recomp_get_stats(gba_machine *m, gba_recomp_stats *out);

/* Differential testing: 0 when two machines are in the same state (CPU, banks, cycle count, scheduler, IRQ
 * registers, every memory but the cartridge, the save chip); else 1 and the first difference in why. */
int gba_recomp_compare(gba_machine *a, gba_machine *b, char *why, size_t why_n);

#ifdef __cplusplus
}
#endif

#endif
