/* gx_progcache.h -- the GX program binary cache file (XBA-264, layer 3 of the shader plan; no GL here).
 *
 * The TEV programs of a title, compiled by this device's GLES driver and kept as program binaries
 * (glGetProgramBinary), so a start links them with glProgramBinary instead of compiling GLSL. One file per title:
 * <files>/work/gc-<KEY>/gx_programs.bin next to gx_shaders.txt (gc_runtime; gc_run --gx-programs FILE).
 *
 * The file belongs to one driver and one shader generator: its key is a hash of GL_VENDOR, GL_RENDERER, GL_VERSION,
 * the GL flavour (ES or desktop core), our vertex shader and ubershader sources and GX_PROGCACHE_VERSION
 * (gx_progcache_key). Each entry also carries the hash of its own fragment shader source: a change of the generator
 * for that state drops the entry alone. A file of another key, truncated, of another format or whose checksum fails is
 * ignored (why in *why) and rebuilt from scratch at the next save. Written to <path>.tmp then renamed: a process killed
 * while writing leaves the previous file whole.
 *
 * Layout, host byte order (the file never leaves the device): "XRGXPB1\n" (8), key (u64), driver text length (u32),
 * entry count (u32), the driver text ("vendor | renderer | version", for the logs), then per entry: the shader key
 * (gx_shader_key, its words), the fragment source hash (u64), the binary format (u32), the binary length (u32), the
 * binary; then the FNV-1a 64 of everything before it (u64). */
#ifndef GX_PROGCACHE_H
#define GX_PROGCACHE_H

#include <stddef.h>
#include <stdint.h>

#include "gx_internal.h"

#define GX_PROGCACHE_VERSION 1u
#define GX_PROGCACHE_MAX_BINARY ((uint32_t)16 << 20)   /* a larger entry is not believed */

typedef struct gx_progbin {
    gx_shader_key key;
    uint64_t src_hash;       /* gx_progcache_src_hash of the key's fragment shader */
    uint32_t format, len;
    uint8_t *data;
} gx_progbin;

typedef struct gx_progcache {
    uint64_t key;
    char driver[512];
    gx_progbin *e;
    size_t n, cap;
} gx_progcache;

/* The cache key of a driver (any string may be NULL) with our current shader generator. desktop: a GL 3.3 core
 * context rather than GLES. $GC_GX_DRIVER_KEY, when set, is mixed in (tests: a pretend driver update). */
uint64_t gx_progcache_key(const char *vendor, const char *renderer, const char *version, int desktop);
/* The hash of state k's fragment shader source as gx_shader_fs_source generates it now (0 when it cannot). */
uint64_t gx_progcache_src_hash(const gx_shader_key *k);

/* An empty cache for key, driver text "vendor | renderer | version". */
void gx_progcache_init(gx_progcache *c, uint64_t key, const char *driver);
void gx_progcache_free(gx_progcache *c);
/* Adds (or replaces) k's binary; data is copied. 0, or -1 out of memory / too large. */
int gx_progcache_put(gx_progcache *c, const gx_shader_key *k, uint64_t src_hash, uint32_t format, const void *data,
                     uint32_t len);
/* The entry of k, or NULL. */
gx_progbin *gx_progcache_find(gx_progcache *c, const gx_shader_key *k);
/* Removes entry i (order not kept). */
void gx_progcache_drop(gx_progcache *c, size_t i);

/* Reads path into c (initialised by this call) when it is whole and of key. 0; or -1 with c empty for key and *why
 * one of "none" (no file), "truncated", "not a program cache", "another driver or shader generator", "checksum",
 * "out of memory". *driver_was (may be NULL) gets the file's driver text when it could be read. */
int gx_progcache_load(const char *path, uint64_t key, const char *driver, gx_progcache *c, const char **why,
                      char *driver_was, size_t driver_was_len);
/* Writes c to path (temporary file, rename). 0, or -1. */
int gx_progcache_save(const char *path, const gx_progcache *c);

#endif
