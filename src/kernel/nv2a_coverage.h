/* What the NV2A executor and its back ends do not draw, by name (XBA-120).
 *
 * A title that uses a texture format the sampler cannot read, a primitive a
 * path does not rasterise, a texture dimensionality or a piece of state
 * neither path models gets a picture with something missing and no word
 * about it. Each such use is counted here under a fixed name, once per
 * batch that needed it:
 *
 *   texture format 0x26 LU_IMAGE_A8CR8CB8Y8
 *   points | lines | line strip | line loop      (a path that drew none)
 *   volume texture | texture stage mode 8 DOT_PRODUCT ...
 *   logic op | polygon mode line ...             (state, see nv2a_pb_exec.c)
 *
 * The counts go to the executor's [GPU] report (the `unsupported:` line)
 * and to the status file (RECOMP_STATUS, recomp_status.h: "unsupported"),
 * where the compat runner and Check my library name them with the run's
 * blocker. Unmodelled methods are counted apart (by method, named from
 * xemu's nv2a_regs.h): "unmodelled" in the status file.
 *
 * Executor thread only (the report and the status writer read the counts;
 * a torn read is one stale number). */
#ifndef NV2A_COVERAGE_H
#define NV2A_COVERAGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A texture colour format (SET_TEXTURE_FORMAT bits 8-15, xemu's
 * NV097_SET_TEXTURE_FORMAT_COLOR_* list). */
typedef struct {
    const char *name;       /* "LU_IMAGE_A8R8G8B8"; NULL: no NV2A format has the code */
    const char *gap;        /* the name a use the sampler cannot read is counted under */
    uint8_t     bytes;      /* bytes per texel; 0 for DXT (blocks) */
    uint8_t     swizzled;   /* SZ_*: Morton order, size from the format word */
    uint8_t     decoded;    /* the executor's sampler reads it (sample_tex) */
} Nv2aTexFormat;

/* Never NULL: an unknown code returns an entry with name NULL and the gap
 * name "texture format (not an NV2A format)". */
const Nv2aTexFormat *nv2a_tex_format(uint32_t fmt);

/* "SET_POINT_SIZE" for 0x043C, the array's name for any element of a
 * method array (SET_LIGHT_LOCAL_POSITION for every light and component);
 * NULL when xemu's nv2a_regs.h names nothing there. */
const char *nv2a_method_name(uint32_t method);

/* Texture stage mode names (SET_SHADER_STAGE_PROGRAM fields, xemu psh.c
 * PS_TEXTUREMODES_*): "PROJECT2D" ... "DOT_ZW". NULL past the list. */
const char *nv2a_stage_mode_name(uint32_t mode);

/* Count n uses of `name`, which must be a string that lives for ever (a
 * literal or one of the tables' entries): the registry keeps the pointer.
 * Also forwarded to the status file. */
void nv2a_gap_note(const char *name, uint32_t n);

/* Uses of `name` since the start (compared as a string). */
uint64_t nv2a_gap_count(const char *name);

/* Up to max names and counts, most used first. Returns how many. */
int nv2a_gap_list(const char **names, uint64_t *counts, int max);

/* Tests: forget everything. */
void nv2a_gap_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* NV2A_COVERAGE_H */
