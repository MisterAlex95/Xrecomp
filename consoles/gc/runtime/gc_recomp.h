/* gc_recomp.h -- static recompilation of GameCube code (XBA-225 spike, epic XBA-219).
 *
 * `python3 -m gc_tools.recomp PROG.dol OUTDIR` (consoles/gc/tools, on the shared PowerPC frontend tools/ppc) lifts
 * the text sections of a DOL to C; compiled with -I consoles/gc/runtime into one shared library, it exports
 * `gc_recomp_title_table`. The core attaches the table: at an address it lists, inside a code span whose bytes in
 * memory match what was lifted, the native function runs; anything else runs in the interpreter (gc_cpu.c), which
 * records the text addresses it had to start at for the next lift (--discovered). Lifted-code ABI: gc_recomp_rt.h. */
#ifndef GC_RECOMP_H
#define GC_RECOMP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gc_machine gc_machine;

#define GC_RECOMP_ABI 1

typedef void (*gc_recomp_fn)(gc_machine *m);

/* A resume point: an address where a lifted function can start (its entry, a branch target, the return point after
 * a call, the instruction after one handed to the interpreter). */
typedef struct gc_recomp_entry {
    uint32_t addr;
    uint32_t span; /* index into spans */
    gc_recomp_fn fn;
} gc_recomp_entry;

/* A code span: a text section's lifted bytes, valid while memory holds them (FNV-1a 32 of lo..hi-1). Checked when
 * first used and again after an icbi into it (code loaded or patched at run time stops matching and is
 * interpreted). */
typedef struct gc_recomp_span {
    uint32_t lo, hi;
    uint32_t fnv1a;
} gc_recomp_span;

typedef struct gc_recomp_table {
    uint32_t abi;               /* GC_RECOMP_ABI */
    uint32_t count;             /* entries, sorted by addr */
    const gc_recomp_entry *entries;
    uint32_t nspans;
    const gc_recomp_span *spans;
    const char *dol_sha256;
} gc_recomp_table;

typedef const gc_recomp_table *(*gc_recomp_title_table_fn)(void);
#define GC_RECOMP_ENTRY_SYMBOL "gc_recomp_title_table"

/* Attaches a table (NULL: interpreter only). Returns 0, or -1 when its ABI differs. */
int gc_attach_recomp(gc_machine *m, const gc_recomp_table *t);

/* Text addresses where the interpreter had to start after a jump (not a resume point of the table): copies at
 * most max, returns the total. */
size_t gc_recomp_missed(gc_machine *m, uint32_t *out, size_t max);

typedef struct gc_recomp_stats {
    uint64_t native;      /* instructions lifted code ran (packs built with -DGC_RC_COUNT) */
    uint64_t interp;      /* instructions the interpreter ran */
    uint64_t calls;       /* calls into lifted code */
    uint64_t span_checks; /* span hashes computed */
} gc_recomp_stats;
void gc_recomp_get_stats(gc_machine *m, gc_recomp_stats *out);

#ifdef __cplusplus
}
#endif

#endif
