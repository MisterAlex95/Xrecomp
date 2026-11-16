/* psp_recomp.h -- static recompilation of PSP modules (XBA-224 spike, epic XBA-218).
 *
 * `python3 -m psp_tools.recomp MODULE OUTDIR` (consoles/psp/tools) lifts the code of a PRX / EBOOT.PBP / ELF, loaded
 * and relocated as psp_load.c loads it, to C; compiled with -I consoles/psp/runtime into one shared library, it
 * exports `psp_recomp_title_table`. The core attaches the table: an entry runs natively when the module's bytes in
 * memory hash to what was lifted (checked once, at attach: the spike loads one module and never changes its code);
 * anything else runs in the interpreter (psp_cpu.c), which records the addresses it had to start at inside the
 * module's code for the next lift (--discovered). Lifted-code ABI: psp_recomp_rt.h (the PS1 lifter's shape). */
#ifndef PSP_RECOMP_H
#define PSP_RECOMP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct psp_machine psp_machine;

#define PSP_RECOMP_ABI 1

typedef void (*psp_recomp_fn)(psp_machine *m);

/* A lifted function: its code from addr to addr + len, valid while those bytes hash to `hash` (FNV-1a 64 over the
 * 32-bit words, ps1_recomp_hash's). Sorted by addr. */
typedef struct psp_recomp_entry {
    uint32_t addr;
    uint32_t len;
    uint64_t hash;
    psp_recomp_fn fn;
} psp_recomp_entry;

typedef struct psp_recomp_table {
    uint32_t abi; /* PSP_RECOMP_ABI */
    uint32_t count;
    const psp_recomp_entry *entries;
    const char *module_sha256; /* of the file it was lifted from */
    uint32_t base;             /* load address it was lifted for */
} psp_recomp_table;

typedef const psp_recomp_table *(*psp_recomp_title_table_fn)(void);
#define PSP_RECOMP_ENTRY_SYMBOL "psp_recomp_title_table"

uint64_t psp_recomp_hash(const uint8_t *p, uint32_t len);

/* Attaches a table (NULL: interpreter only). Returns 0, -1 when its ABI or base differs. */
int psp_attach_recomp(psp_machine *m, const psp_recomp_table *t);

/* Addresses inside the module's code where the interpreter had to start: copies at most max, returns the total. */
size_t psp_recomp_missed(psp_machine *m, uint32_t *out, size_t max);

typedef struct psp_recomp_stats {
    uint64_t native;   /* instructions lifted code ran (packs built with -DPSP_RC_COUNT) */
    uint64_t interp;   /* instructions the interpreter ran */
    uint64_t calls;    /* calls into lifted code */
    uint32_t entries;  /* entries attached */
    uint32_t stale;    /* entries whose bytes did not match memory */
} psp_recomp_stats;
void psp_recomp_get_stats(psp_machine *m, psp_recomp_stats *out);

#ifdef __cplusplus
}
#endif

#endif
