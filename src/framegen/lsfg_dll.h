/* Reading the RT_RCDATA resources of a PE file (Lossless.dll) for frame generation (docs/technical/framegen.md).
 *
 * Lossless Scaling's DLL carries the LSFG compute shaders as raw resources (RT_RCDATA, numeric IDs). This walks the
 * PE resource tree (type -> name -> language) of a PE32 or PE32+ image held in memory and hands each RCDATA entry
 * with a numeric ID to a callback. libc only, no global state: the Android app links it into libxba_import.so to
 * read a DLL the player picked (an fd from the ContentResolver), the desktop runtime and tools use it on a path.
 *
 * The DLL is the user's own file. Nothing here keeps, copies or logs its contents; callers write the extracted
 * shaders to the user's private cache only (lsfg_cache.h), never into this repository, an APK or CI.
 *
 * Every offset and size read from the file is checked against the buffer, the resource tree depth is fixed (3) and
 * the entry counts are bounded, so a truncated or hostile file fails with LSFG_DLL_ECORRUPT instead of reading out
 * of bounds or looping.
 */
#ifndef LSFG_DLL_H
#define LSFG_DLL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes: negative, 0 is success. */
#define LSFG_DLL_EIO       (-1)   /* reading the fd failed */
#define LSFG_DLL_ENOTPE    (-2)   /* no MZ / PE signature, or an optional header that is neither PE32 nor PE32+ */
#define LSFG_DLL_ECORRUPT  (-3)   /* truncated file, out-of-range offsets, oversized resource tree */
#define LSFG_DLL_ENORSRC   (-4)   /* no resource directory, or no RT_RCDATA resource */
#define LSFG_DLL_ENOMEM    (-5)
#define LSFG_DLL_EINVAL    (-6)   /* bad argument */
#define LSFG_DLL_ETOOBIG   (-7)   /* the file is larger than LSFG_DLL_MAX_SIZE */

#define LSFG_DLL_MAX_SIZE  (512u << 20)   /* lsfg_dll_read_fd refuses larger files */
#define LSFG_DLL_RT_RCDATA 10u

typedef struct LsfgDllResource {
    uint32_t id;          /* numeric resource ID (named entries are skipped) */
    uint32_t lang;        /* language ID of the leaf (the first one is reported when there are several) */
    uint32_t size;        /* data size in bytes */
    uint32_t file_offset; /* offset of the data in the file */
} LsfgDllResource;

/* Called for each RT_RCDATA resource with a numeric ID, in the order of the resource directory (ascending IDs in a
 * linker-made file). data points into the caller's buffer (size bytes, already bounds-checked). Return nonzero to
 * stop the walk (lsfg_dll_walk then returns that value). */
typedef int (*lsfg_dll_visit_fn)(const LsfgDllResource *res, const uint8_t *data, void *user);

/* Walks the RT_RCDATA resources of the PE image in buf[0..len). Returns 0 when the walk finished (at least one
 * RCDATA resource seen), the callback's nonzero value when it stopped the walk, or a negative LSFG_DLL_* code. */
int lsfg_dll_walk(const uint8_t *buf, size_t len, lsfg_dll_visit_fn fn, void *user);

/* Finds one RT_RCDATA resource by ID. Returns 0 and fills *res / *data, LSFG_DLL_ENORSRC when the ID is absent, or
 * another negative code. Either output may be NULL. */
int lsfg_dll_find(const uint8_t *buf, size_t len, uint32_t id, LsfgDllResource *res, const uint8_t **data);

/* Reads a whole file from an open fd (pread from offset 0, the fd position is not used) into a malloc'd buffer.
 * The caller frees *buf. Returns 0 or LSFG_DLL_EIO / ENOMEM / ETOOBIG / EINVAL. */
int lsfg_dll_read_fd(int fd, uint8_t **buf, size_t *len);

/* Same, from a path. */
int lsfg_dll_read_path(const char *path, uint8_t **buf, size_t *len);

const char *lsfg_dll_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif /* LSFG_DLL_H */
