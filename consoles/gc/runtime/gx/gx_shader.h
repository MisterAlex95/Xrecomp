/* gx_shader.h -- TEV state to GLSL ES (gx_shader.c), XBA-262. */
#ifndef GX_SHADER_H
#define GX_SHADER_H

#include <stddef.h>
#include <stdint.h>

#include "gx_internal.h"

uint64_t gx_shader_key_hash(const gx_shader_key *k);
const char *gx_shader_vs_source(void);
/* The fragment shader of state k, or the ubershader for k = NULL. malloc'd. */
char *gx_shader_fs_source(const gx_shader_key *k);

/* The per-title list: T lines (shader keys) read back; written with the vertex formats met. */
int gx_shader_list_read(const char *path, gx_shader_key **keys, size_t *n);
int gx_shader_list_write(const char *path, const gx_shader_key *keys, size_t n, const uint32_t (*vfmts)[5], size_t nv);

#endif
