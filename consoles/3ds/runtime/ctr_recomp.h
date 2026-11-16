/* ctr_recomp.h -- static recompilation of 3DS (ARM11, ARMv6K + VFPv2) user code (XBA-228 spike, epic XBA-222).
 *
 * `python3 -m ctr_tools.recomp PROG.3dsx OUTDIR` (consoles/3ds/tools) lifts the code segment of a 3DSX, relocated at
 * 0x00100000 as the loader places it, to C; compiled with -I consoles/3ds/runtime into one shared library, it
 * exports `ctr_recomp_title_table`. The core attaches the table when the image it lifted (FNV-1a of the relocated
 * code and read-only data) is the one loaded: at an address the table covers, the native function runs; anything
 * else runs in the interpreter (ctr_cpu.c), which records where it had to start for the next lift (--discovered).
 *
 * Unlike the DS, a 3DS process cannot write its own code: the code segment is mapped read-only, so one image check
 * at attach replaces the DS spike's per-span hashes. Lifted-code ABI: ctr_recomp_rt.h. */
#ifndef CTR_RECOMP_H
#define CTR_RECOMP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ctr_machine ctr_machine;

#define CTR_RECOMP_ABI 1

typedef void (*ctr_recomp_fn)(ctr_machine *m);

enum { CTR_RECOMP_THUMB = 1u << 0 };

typedef struct ctr_recomp_entry {
    uint32_t addr;  /* first instruction of a contiguous run of a function */
    uint32_t flags; /* CTR_RECOMP_THUMB */
    ctr_recomp_fn fn;
} ctr_recomp_entry;

typedef struct ctr_recomp_table {
    uint32_t abi;            /* CTR_RECOMP_ABI */
    uint32_t count;          /* entries, sorted by addr */
    const ctr_recomp_entry *entries;
    uint32_t image_lo, image_hi; /* the read-only image the lifter saw (code + rodata) */
    uint32_t image_fnv1a;    /* FNV-1a 32 of image_lo..image_hi-1 after relocation */
    const char *sha256;      /* of the .3dsx */
} ctr_recomp_table;

typedef const ctr_recomp_table *(*ctr_recomp_title_table_fn)(void);
#define CTR_RECOMP_ENTRY_SYMBOL "ctr_recomp_title_table"

/* Attaches a table (NULL: interpreter only). Returns 0, -1 when its ABI differs, -2 when the image differs. */
int ctr_attach_recomp(ctr_machine *m, const ctr_recomp_table *t);

/* Addresses (bit 0 = Thumb) inside the lifted image where the interpreter had to start after a jump: copies at most
 * max, returns the total. */
size_t ctr_recomp_missed(ctr_machine *m, uint32_t *out, size_t max);

typedef struct ctr_recomp_stats {
    uint64_t native; /* instructions lifted code ran (packs built with -DCTR_RC_COUNT) */
    uint64_t interp; /* instructions the interpreter ran */
    uint64_t calls;  /* calls into lifted code */
    uint64_t svcs;   /* supervisor calls (HLE kernel) */
} ctr_recomp_stats;
void ctr_recomp_get_stats(ctr_machine *m, ctr_recomp_stats *out);

#ifdef __cplusplus
}
#endif

#endif
