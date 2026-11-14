/* ps1_recomp.h -- static recompilation of PlayStation games (Plane P-2 XBA-160, P-3 XBA-161, P-4 XBA-162).
 *
 * A PS1 title pack (libtitle.so, pack.json with "console": "ps1") holds the game's MIPS code lifted to C by
 * consoles/ps1/tools (ps1_tools) and compiled for the host. The runtime attaches its table to the machine: when the
 * CPU reaches code the table covers, the recompiled function runs natively; anything else (code the discovery did
 * not find, overlays loaded at run time and not lifted yet, the BIOS) runs in the interpreter, which also records the
 * code it had to run, bytes included, so the next Prepare can lift it (the discovery loop). Graphics, sound, timers,
 * DMA, interrupts, the CD and the BIOS are the same code either way.
 *
 * The difference with the GBA: PS1 code runs from RAM. The main EXE is loaded from the disc, then overlays are read
 * over each other at run time, so an address alone does not say which code is there. Every table entry therefore
 * names its code by address AND content: a range of words and their hash. The core keeps one write counter per 1 KB
 * page of RAM (CPU stores, lifted stores, DMA, the EXE load bump it); an entry is re-hashed only when a page it covers
 * was written since it was last checked, and runs only while its hash matches. Several entries may share an address
 * (overlay A and overlay B both linked at 0x80100000): the one whose hash matches the RAM runs, never stale code.
 *
 * Contract (frozen between ps1_tools, Prepare, the loader and the core; change together and bump PS1_RECOMP_ABI):
 * - `python3 -m ps1_tools.recomp INPUT OUTDIR [--discovered FILE]` writes C sources into OUTDIR (nothing else needed
 *   to build): compile each of them with -I consoles/ps1/runtime into one shared library. INPUT is a disc image
 *   (.cue / .bin / .iso: the PS-X EXE named by SYSTEM.CNF BOOT, read from ISO 9660) or a PS-X EXE.
 * - That library exports `ps1_recomp_title_table` (pack.json "entry"), returning the table below.
 * - The library is linked against the core's symbols (ps1_recomp_rt.h; the shell / ps1_run provide them at load).
 * - The discovered file (--discovered; the runtime writes it, ps1_recomp_write_missed) is text, one record per line,
 *   `#` comments:  `ADDR HASH HEXBYTES`  -- ADDR the virtual address (hex) where the interpreter had to start, HASH
 *   ps1_recomp_hash of the words recorded (16 hex digits), HEXBYTES the code bytes from ADDR as they were in RAM then
 *   (little-endian, as in memory; a window of up to PS1_RECOMP_SNAPSHOT bytes). A line with ADDR alone names code of
 *   the input EXE. ps1_tools lifts from the recorded bytes, so code that is not in the EXE (overlays) gets lifted. An
 *   address recorded with PS1_RECOMP_VARIANTS different contents is code the program generates at run time (a
 *   stub rewritten before each call): the runtime records no more of it and ps1_tools does not lift it. */
#ifndef PS1_RECOMP_H
#define PS1_RECOMP_H

#include <stddef.h>
#include <stdint.h>

#include "ps1_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PS1_RECOMP_ABI 1
#define PS1_RECOMP_SNAPSHOT 2048u /* bytes of code recorded per missed address at most */
#define PS1_RECOMP_VARIANTS 8     /* contents recorded per address at most: more is code generated at run time */

/* Entered with the CPU at an instruction of the entry's range (no branch delay slot, no load in flight); returns
 * when control leaves the recompiled code, with the CPU state set to where execution continues. See ps1_recomp_rt.h
 * for the rest of the lifted code's ABI (machine layout, core symbols, exit codes). */
typedef void (*ps1_recomp_fn)(ps1_machine *m);

typedef struct ps1_recomp_entry {
    uint32_t addr;  /* virtual address of the first instruction (KUSEG / KSEG0 / KSEG1 main RAM, 4-aligned) */
    uint32_t len;   /* bytes of code the function covers, from addr (a multiple of 4, inside one 2 MB mirror) */
    uint64_t hash;  /* ps1_recomp_hash of those words: the function runs only while RAM holds them */
    ps1_recomp_fn fn;
} ps1_recomp_entry;

typedef struct ps1_recomp_table {
    uint32_t abi;               /* PS1_RECOMP_ABI */
    uint32_t count;             /* entries, sorted by addr (several may share an address: overlays) */
    const ps1_recomp_entry *entries;
    const char *source_sha256;  /* the EXE the table was built from, lower-case hex */
    uint32_t boot_pc;           /* that EXE's entry point: lifted code runs, and missed code is recorded, only once the
                                 * CPU got there (before, it is the BIOS: its shell runs from RAM too, interpreted) */
} ps1_recomp_table;

/* Exported by a PS1 pack library. */
typedef const ps1_recomp_table *(*ps1_recomp_title_table_fn)(void);
#define PS1_RECOMP_ENTRY_SYMBOL "ps1_recomp_title_table"

/* The content hash of n 32-bit words (FNV-1a 64 over the words: h ^= w; h *= 0x100000001B3, from
 * 0xCBF29CE484222325). ps1_tools computes the same. */
uint64_t ps1_recomp_hash(const uint32_t *words, size_t n);

/* Attaches a table (NULL detaches: pure interpreter). Returns 0, or -1 when its ABI differs. */
int ps1_attach_recomp(ps1_machine *m, const ps1_recomp_table *t);

/* Discovery: the distinct addresses where the interpreter had to start running main-RAM code (above the BIOS's
 * 64 KB, after the boot EXE's entry was reached) that no attached entry matched, since the table was attached. Copies at most max into out, returns the total count. */
size_t ps1_recomp_missed(ps1_machine *m, uint32_t *out, size_t max);

/* Appends the missed records (the format above: address, hash, the code bytes recorded when it was missed) to
 * path, skipping records the file already holds. Returns the records written, or -1 on an I/O error. */
int ps1_recomp_write_missed(ps1_machine *m, const char *path);

/* ---- Helpers for hosts and tests (not part of the pack ABI) ---- */

typedef struct ps1_recomp_stats {
    uint64_t native;       /* instructions lifted code ran (only counted by packs built with -DPS1_RC_COUNT) */
    uint64_t interp_ram;   /* main-RAM instructions (above 64 KB) the interpreter ran while a table was attached */
    uint64_t fallbacks;    /* of those, instructions lifted code handed to the interpreter (exceptions, COP0, ...) */
    uint64_t calls;        /* calls into lifted code */
    uint64_t slow_lookups; /* dispatch cache misses */
    uint64_t rehashes;     /* entries re-hashed because a page they cover was written */
    uint64_t stale;        /* of those, entries whose code was no longer in RAM (overlay swapped, code patched) */
    uint64_t interrupts;   /* interrupts taken (between lifted or interpreted instructions alike) */
} ps1_recomp_stats;
void ps1_recomp_get_stats(ps1_machine *m, ps1_recomp_stats *out);

/* Differential testing: 0 when two machines are in the same state (CPU registers with the load in flight applied,
 * HI / LO, PC, COP0, cycles, scheduler, IRQ registers, RAM, scratchpad, GTE, VRAM); else 1 and the first difference
 * in why. */
int ps1_recomp_compare(ps1_machine *a, ps1_machine *b, char *why, size_t why_n);

#ifdef __cplusplus
}
#endif

#endif
