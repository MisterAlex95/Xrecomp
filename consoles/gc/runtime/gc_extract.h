/* gc_extract -- what Prepare needs from a GameCube disc, out of any image format the runtime reads (XBA-264).
 *
 * tools.prepare reads plain ISO / GCM, CISO and GCZ itself (consoles/gc/tools/gc_tools/gcm.py). An RVZ / WIA image, or
 * a disc the app sees only as a document, goes through the runtime's own readers instead: gc_extract_sparse writes a
 * GCM of the same size whose only written bytes are the ones Prepare reads (the header, bi2, the apploader, the FST,
 * the main DOL, every *.rel module and *.str name table), the rest left as holes (a few MB on disk for a 1.4 GB
 * disc). The app does it before a Prepare (PrepareService, through libxba_import), a desktop with
 * `gc_run --extract OUT.gcm IMAGE`. The same title key and main DOL come out, so the pack is the image's. */
#ifndef GC_EXTRACT_H
#define GC_EXTRACT_H

#include <stddef.h>
#include <stdint.h>

#include "gc_disc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gc_extract_info {
    uint32_t dol_size;
    uint32_t rels;
    uint64_t rel_bytes;
} gc_extract_info;

/* Writes the sparse GCM of d to out_path (through out_path.tmp and a rename). 0, or -1 with why. info may be NULL. */
int gc_extract_sparse(gc_disc *d, const char *out_path, gc_extract_info *info, char *why, size_t why_n);

/* The SHA-256 (lower-case hex) of the disc's main DOL, the pack's source identity. 0, or -1 (hex ""). */
int gc_extract_dol_sha256(gc_disc *d, char hex[65]);

#ifdef __cplusplus
}
#endif

#endif
