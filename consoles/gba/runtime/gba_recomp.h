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

#define GBA_RECOMP_ABI 1

/* Entered with the CPU state at the entry's address (ARM or Thumb as flagged); returns when control leaves the
 * recompiled code, with the CPU's PC (and mode) set to where execution continues. */
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

#ifdef __cplusplus
}
#endif

#endif
