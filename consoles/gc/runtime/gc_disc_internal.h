/* gc_disc_internal.h -- the disc readers' shared state (gc_disc.c, gc_disc_wia.c; XBA-261). */
#ifndef GC_DISC_INTERNAL_H
#define GC_DISC_INTERNAL_H

#include <stdio.h>

#include "gc_disc.h"

struct gc_disc {
    const char *format;
    FILE *f;                 /* the image file, or NULL for bytes in memory */
    const uint8_t *mem;
    uint64_t file_size;
    uint64_t size;           /* the disc's */
    /* the format's reader: off + len within the disc */
    int (*read)(gc_disc *d, uint64_t off, uint8_t *buf, size_t len);
    void (*free)(gc_disc *d);
    void *priv;
    char error[160];
};

/* len bytes of the image file at off: 0, or -1 (short read). */
int gc_disc_file_read(gc_disc *d, uint64_t off, void *buf, size_t len);
/* Sets d->error, returns -1. */
int gc_disc_fail(gc_disc *d, const char *fmt, ...);

/* gc_disc_wia.c: a WIA or RVZ image whose first 0x48 bytes are head. 0, or -1 with d->error set. */
int gc_disc_wia_open(gc_disc *d, const uint8_t *head);

/* gc_bzip2.c: one bzip2 stream in src into out (exactly out_len bytes expected, fewer is an error). 0 or -1. */
int gc_bzip2_decompress(const uint8_t *src, size_t src_len, uint8_t *out, size_t out_len);

static inline uint32_t gc_be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static inline uint64_t gc_be64(const uint8_t *p)
{
    return (uint64_t)gc_be32(p) << 32 | gc_be32(p + 4);
}

static inline uint32_t gc_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline uint64_t gc_le64(const uint8_t *p)
{
    return (uint64_t)gc_le32(p) | (uint64_t)gc_le32(p + 4) << 32;
}

#endif
