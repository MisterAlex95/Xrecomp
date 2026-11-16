/* dc_recomp.h -- static recompilation of Dreamcast SH-4 code (XBA-227 spike, epic XBA-221).
 *
 * `python3 -m dc_tools.recomp PROGRAM.elf|1ST_READ.BIN OUTDIR` (consoles/dc/tools) lifts the program's code to C;
 * compiled with -I consoles/dc/runtime into one shared library, it exports `dc_recomp_title_table`. The core
 * attaches the table: at an address it lists, the native function runs; anything else runs in the interpreter
 * (sh4_interp.c), which records the addresses inside the lifted image it had to run for the next lift
 * (--discovered). Lifted code's ABI: sh4_ops.h (state, instruction semantics, bus) plus the two functions below. */
#ifndef DC_RECOMP_H
#define DC_RECOMP_H

#include <stddef.h>
#include <stdint.h>

#include "sh4_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DC_RECOMP_ABI 1

typedef void (*dc_recomp_fn)(sh4_cpu *c);

typedef struct dc_recomp_entry {
    uint32_t addr; /* an address the function can start at (its entry, a return site, a resume point) */
    dc_recomp_fn fn;
} dc_recomp_entry;

/* The bytes the code was lifted from (FNV-1a 32 of lo..hi-1): the table is refused when memory differs. */
typedef struct dc_recomp_span {
    uint32_t lo, hi;
    uint32_t fnv1a;
} dc_recomp_span;

typedef struct dc_recomp_table {
    uint32_t abi;   /* DC_RECOMP_ABI */
    uint32_t count; /* entries, sorted by addr */
    const dc_recomp_entry *entries;
    uint32_t nspans;
    const dc_recomp_span *spans;
    const char *image_sha256;
} dc_recomp_table;

typedef const dc_recomp_table *(*dc_recomp_title_table_fn)(void);
#define DC_RECOMP_ENTRY_SYMBOL "dc_recomp_title_table"

/* Exported by the host for lifted code: the lifted function for an address (NULL: not lifted), so a call through a
 * register can nest instead of going back to the dispatcher. */
dc_recomp_fn dc_rc_find(sh4_cpu *c, uint32_t addr);

/* Calls nest on the host stack up to this depth; deeper calls return to the dispatcher. */
#define DC_RC_MAX_DEPTH 192

#ifdef __cplusplus
}
#endif

#endif
