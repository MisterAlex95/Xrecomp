/* nds_recomp.h -- static recompilation of the DS ARM9 (XBA-223 spike, epic XBA-217).
 *
 * `python3 -m nds_tools.recomp ROM.nds OUTDIR` (consoles/nds/tools) lifts the ARM9 code of a .nds, as it is once its
 * boot has moved it into place, to C; compiled with -I consoles/nds/runtime into one shared library, it exports
 * `nds_recomp_title_table`. The core attaches the table: at an address it covers, inside a code span whose bytes in
 * memory match what was lifted, the native function runs; anything else runs in the interpreter (nds_cpu.c), which
 * records the span addresses it had to run for the next lift (--discovered). Lifted-code ABI: nds_recomp_rt.h. */
#ifndef NDS_RECOMP_H
#define NDS_RECOMP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nds_machine nds_machine;
typedef struct nds_arm nds_arm; /* one CPU of the machine (lifted code gets it) */

#define NDS_RECOMP_ABI 2

typedef void (*nds_recomp_fn)(nds_arm *c);

enum { NDS_RECOMP_THUMB = 1u << 0 };

typedef struct nds_recomp_entry {
    uint32_t addr;  /* first instruction of a contiguous run of a function */
    uint32_t flags; /* NDS_RECOMP_THUMB */
    nds_recomp_fn fn;
} nds_recomp_entry;

/* A code span: lifted code from lo to hi, valid while memory holds these bytes (FNV-1a 32 of lo..hi-1). DS code
 * lives in RAM: the boot stub runs where .main is copied later, so a span is checked before its functions run. */
typedef struct nds_recomp_span {
    uint32_t lo, hi;
    uint32_t fnv1a;
} nds_recomp_span;

typedef struct nds_recomp_table {
    uint32_t abi;               /* NDS_RECOMP_ABI */
    uint32_t count;             /* entries, sorted by addr */
    const nds_recomp_entry *entries;
    uint32_t nspans;
    const nds_recomp_span *spans;
    const char *rom_sha256;
} nds_recomp_table;

typedef const nds_recomp_table *(*nds_recomp_title_table_fn)(void);
#define NDS_RECOMP_ENTRY_SYMBOL "nds_recomp_title_table"

/* Attaches a table to the ARM9 (cpu 0) or the ARM7 (cpu 1); NULL: interpreter only. Returns 0, or -1 when its ABI
 * differs. A pack exports nds_recomp_title_table (ARM9) and, when it lifted the ARM7 too, nds_recomp_title_table7. */
int nds_attach_recomp_cpu(nds_machine *m, int cpu, const nds_recomp_table *t);
int nds_attach_recomp(nds_machine *m, const nds_recomp_table *t); /* cpu 0 */
#define NDS_RECOMP_ENTRY_SYMBOL7 "nds_recomp_title_table7"

/* Addresses (bit 0 = Thumb) inside a valid span where the interpreter had to start: copies at most max, returns
 * the total. */
size_t nds_recomp_missed(nds_machine *m, int cpu, uint32_t *out, size_t max);

typedef struct nds_recomp_stats {
    uint64_t native;       /* instructions lifted code ran (packs built with -DNDS_RC_COUNT) */
    uint64_t interp;       /* instructions the interpreter ran */
    uint64_t calls;        /* calls into lifted code */
    uint64_t span_checks;  /* span hashes computed */
} nds_recomp_stats;
void nds_recomp_get_stats(nds_machine *m, int cpu, nds_recomp_stats *out);

#ifdef __cplusplus
}
#endif

#endif
