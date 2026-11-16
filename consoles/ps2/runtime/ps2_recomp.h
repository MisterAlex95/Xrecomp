/* ps2_recomp.h -- static recompilation of PS2 EE executables (XBA-226 spike, epic XBA-220).
 *
 * `python3 -m ps2_tools.recomp ELF OUTDIR` (consoles/ps2/tools) lifts the EE code of an ELF, loaded as ps2_load.c
 * loads it, to C; compiled with -I consoles/ps2/runtime into one shared library, it exports `ps2_recomp_title_table`.
 * The core attaches the table: an entry runs natively when the ELF's bytes in memory hash to what was lifted
 * (checked once, at attach: the spike loads one executable and never changes its code); anything else runs in the
 * interpreter (ps2_cpu.c), which records the addresses it had to start at inside the executable's code for the next
 * lift (--discovered). Lifted-code ABI: ps2_recomp_rt.h (the PS1 lifter's shape). */
#ifndef PS2_RECOMP_H
#define PS2_RECOMP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ps2_machine ps2_machine;

#define PS2_RECOMP_ABI 1

typedef void (*ps2_recomp_fn)(ps2_machine *m);

/* A lifted function: its code from addr to addr + len, valid while those bytes hash to `hash` (FNV-1a 64 over the
 * 32-bit words, ps1_recomp_hash's). Sorted by addr. */
typedef struct ps2_recomp_entry {
    uint32_t addr;
    uint32_t len;
    uint64_t hash;
    ps2_recomp_fn fn;
} ps2_recomp_entry;

typedef struct ps2_recomp_table {
    uint32_t abi; /* PS2_RECOMP_ABI */
    uint32_t count;
    const ps2_recomp_entry *entries;
    const char *elf_sha256; /* of the file it was lifted from */
} ps2_recomp_table;

typedef const ps2_recomp_table *(*ps2_recomp_title_table_fn)(void);
#define PS2_RECOMP_ENTRY_SYMBOL "ps2_recomp_title_table"

uint64_t ps2_recomp_hash(const uint8_t *p, uint32_t len);

/* Attaches a table (NULL: interpreter only). Returns 0, -1 when its ABI differs. */
int ps2_attach_recomp(ps2_machine *m, const ps2_recomp_table *t);

/* Addresses inside the executable's code where the interpreter had to start: copies at most max, returns the
 * total. */
size_t ps2_recomp_missed(ps2_machine *m, uint32_t *out, size_t max);

typedef struct ps2_recomp_stats {
    uint64_t native;  /* instructions lifted code ran (packs built with -DPS2_RC_COUNT) */
    uint64_t interp;  /* instructions the interpreter ran */
    uint64_t calls;   /* calls into lifted code */
    uint32_t entries; /* entries attached */
    uint32_t stale;   /* entries whose bytes did not match memory */
} ps2_recomp_stats;
void ps2_recomp_get_stats(ps2_machine *m, ps2_recomp_stats *out);

#ifdef __cplusplus
}
#endif

#endif
