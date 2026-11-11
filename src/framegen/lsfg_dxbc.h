/* DXBC -> SPIR-V for the LSFG shader cache, through DXVK's DXBC compiler (docs/technical/framegen.md, "DXBC
 * translation"). Built only when XBOXRECOMP_LSFG_DXVK_SRC / $DXVK_SRC points at a DXVK v2.6.2 checkout
 * (lsfg_dxbc.cmake); DXVK is zlib-licensed and not vendored. */
#ifndef LSFG_DXBC_H
#define LSFG_DXBC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* An lsfg_translate_fn (lsfg_cache.h): compiles one DXBC compute shader to SPIR-V with DXVK's compiler. The
 * bindings are DXVK's per-class ranges (cbuffers, samplers, SRVs, UAVs, each by register), which
 * lsfg_spv_renumber() maps to framegen's layout. *spv is malloc'd. Returns 0 or 1 (err gets DXVK's message). */
int lsfg_dxbc_translate(const uint8_t *dxbc, size_t len, uint32_t base_id, uint8_t **spv, size_t *spv_len,
                        char *err, size_t errlen, void *user);

/* "dxvk-<version>" for the manifest's translator line. */
const char *lsfg_dxbc_translator_name(void);

#ifdef __cplusplus
}
#endif

#endif /* LSFG_DXBC_H */
