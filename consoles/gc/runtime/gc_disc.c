/* gc_disc.c -- GameCube disc images (XBA-261): open by content, ISO / GCM, CISO and GCZ readers, the disc header,
 * the FST and the title key. WIA / RVZ are in gc_disc_wia.c. See gc_disc.h. */
#define _FILE_OFFSET_BITS 64
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "gc_disc_internal.h"
#include "miniz.h"

int gc_disc_fail(gc_disc *d, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(d->error, sizeof d->error, fmt, ap);
    va_end(ap);
    return -1;
}

int gc_disc_file_read(gc_disc *d, uint64_t off, void *buf, size_t len)
{
    if (off > d->file_size || len > d->file_size - off)
        return gc_disc_fail(d, "read of %zu bytes at 0x%llX past the image's end", len, (unsigned long long)off);
    if (d->mem) {
        memcpy(buf, d->mem + off, len);
        return 0;
    }
    if (fseeko(d->f, (off_t)off, SEEK_SET) != 0 || fread(buf, 1, len, d->f) != len)
        return gc_disc_fail(d, "cannot read %zu bytes at 0x%llX of the image", len, (unsigned long long)off);
    return 0;
}

/* ---- ISO / GCM ---- */

static int iso_read(gc_disc *d, uint64_t off, uint8_t *buf, size_t len)
{
    return gc_disc_file_read(d, off, buf, len);
}

/* ---- CISO: "CISO", u32 LE block size, map[0x8000 - 8] of present blocks, then the present blocks in order ---- */

#define CISO_HEADER 0x8000u
#define CISO_MAP (CISO_HEADER - 8)

typedef struct {
    uint32_t block_size;
    int32_t index[CISO_MAP]; /* the block's place among the stored ones, -1 when absent */
} ciso_priv;

static int ciso_read(gc_disc *d, uint64_t off, uint8_t *buf, size_t len)
{
    ciso_priv *p = d->priv;
    while (len) {
        uint64_t blk = off / p->block_size;
        uint32_t in = (uint32_t)(off % p->block_size);
        size_t n = p->block_size - in < len ? p->block_size - in : len;
        if (blk >= CISO_MAP || p->index[blk] < 0) {
            memset(buf, 0, n);
        } else {
            uint64_t at = CISO_HEADER + (uint64_t)p->index[blk] * p->block_size + in;
            if (gc_disc_file_read(d, at, buf, n))
                return -1;
        }
        off += n;
        buf += n;
        len -= n;
    }
    return 0;
}

static int ciso_open(gc_disc *d, const uint8_t *head)
{
    uint8_t *map = malloc(CISO_MAP);
    ciso_priv *p = calloc(1, sizeof *p);
    if (!map || !p) {
        free(map);
        free(p);
        return gc_disc_fail(d, "out of memory");
    }
    p->block_size = gc_le32(head + 4);
    if (p->block_size < 0x800 || (p->block_size & (p->block_size - 1)) || gc_disc_file_read(d, 8, map, CISO_MAP)) {
        free(map);
        free(p);
        return gc_disc_fail(d, "CISO: bad block size or header");
    }
    int32_t stored = 0;
    uint64_t last = 0;
    for (uint32_t i = 0; i < CISO_MAP; i++) {
        p->index[i] = map[i] ? stored++ : -1;
        if (map[i])
            last = i + 1;
    }
    free(map);
    if (CISO_HEADER + (uint64_t)stored * p->block_size > d->file_size) {
        free(p);
        return gc_disc_fail(d, "CISO: the map lists more blocks than the file holds");
    }
    d->priv = p;
    d->free = NULL;
    d->read = ciso_read;
    d->format = "ciso";
    d->size = last * p->block_size;
    return 0;
}

/* ---- GCZ: header (LE) magic, sub type, compressed size, data size, block size, block count; then the block
 * pointers (u64, top bit = stored as is), the Adler-32 of each stored block, the blocks ---- */

#define GCZ_MAGIC 0xB10BC001u

typedef struct {
    uint32_t block_size, nblocks;
    uint64_t comp_size, data_off;
    uint64_t *ptr;
    uint32_t *hash;
    uint8_t *comp;     /* one block's stored bytes */
    uint8_t *cache;    /* one block decompressed */
    int64_t cached;    /* its index, -1 */
} gcz_priv;

static void gcz_free(gc_disc *d)
{
    gcz_priv *p = d->priv;
    if (p) {
        free(p->ptr);
        free(p->hash);
        free(p->comp);
        free(p->cache);
    }
}

static int gcz_block(gc_disc *d, uint32_t i)
{
    gcz_priv *p = d->priv;
    if (p->cached == i)
        return 0;
    p->cached = -1;
    uint64_t at = p->ptr[i] & ~(1ull << 63);
    uint64_t end = i + 1 < p->nblocks ? p->ptr[i + 1] & ~(1ull << 63) : p->comp_size;
    if (end < at || end - at > p->block_size + 0x1000ull)
        return gc_disc_fail(d, "GCZ: block %u has a bad size", i);
    size_t n = (size_t)(end - at);
    if (gc_disc_file_read(d, p->data_off + at, p->comp, n))
        return -1;
    if (mz_adler32(MZ_ADLER32_INIT, p->comp, n) != p->hash[i])
        return gc_disc_fail(d, "GCZ: block %u fails its Adler-32", i);
    if (p->ptr[i] >> 63) {
        if (n > p->block_size)
            return gc_disc_fail(d, "GCZ: stored block %u is too long", i);
        memcpy(p->cache, p->comp, n);
        memset(p->cache + n, 0, p->block_size - n);
    } else {
        size_t got = tinfl_decompress_mem_to_mem(p->cache, p->block_size, p->comp, n,
                                                 TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
        if (got == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED)
            return gc_disc_fail(d, "GCZ: block %u does not inflate", i);
        memset(p->cache + got, 0, p->block_size - got);
    }
    p->cached = i;
    return 0;
}

static int gcz_read(gc_disc *d, uint64_t off, uint8_t *buf, size_t len)
{
    gcz_priv *p = d->priv;
    while (len) {
        uint64_t blk = off / p->block_size;
        uint32_t in = (uint32_t)(off % p->block_size);
        size_t n = p->block_size - in < len ? p->block_size - in : len;
        if (blk >= p->nblocks) {
            memset(buf, 0, n);
        } else {
            if (gcz_block(d, (uint32_t)blk))
                return -1;
            memcpy(buf, p->cache + in, n);
        }
        off += n;
        buf += n;
        len -= n;
    }
    return 0;
}

static int gcz_open(gc_disc *d, const uint8_t *head)
{
    gcz_priv *p = calloc(1, sizeof *p);
    if (!p)
        return gc_disc_fail(d, "out of memory");
    d->priv = p;
    d->free = gcz_free;
    p->comp_size = gc_le64(head + 8);
    d->size = gc_le64(head + 16);
    p->block_size = gc_le32(head + 24);
    p->nblocks = gc_le32(head + 28);
    p->cached = -1;
    if (p->block_size < 0x200 || p->block_size > (64u << 20) || !p->nblocks
        || (uint64_t)p->nblocks * p->block_size < d->size || p->nblocks > (1u << 24))
        return gc_disc_fail(d, "GCZ: bad block size or count");
    p->data_off = 32 + (uint64_t)p->nblocks * 12;
    p->ptr = malloc((size_t)p->nblocks * 8);
    p->hash = malloc((size_t)p->nblocks * 4);
    p->comp = malloc(p->block_size + 0x1000);
    p->cache = malloc(p->block_size);
    uint8_t *raw = malloc((size_t)p->nblocks * 12);
    if (!p->ptr || !p->hash || !p->comp || !p->cache || !raw) {
        free(raw);
        return gc_disc_fail(d, "out of memory");
    }
    if (gc_disc_file_read(d, 32, raw, (size_t)p->nblocks * 12)) {
        free(raw);
        return -1;
    }
    for (uint32_t i = 0; i < p->nblocks; i++) {
        p->ptr[i] = gc_le64(raw + 8 * i);
        p->hash[i] = gc_le32(raw + (size_t)p->nblocks * 8 + 4 * i);
    }
    free(raw);
    if (p->data_off + p->comp_size > d->file_size)
        return gc_disc_fail(d, "GCZ: the file is shorter than its header says");
    d->read = gcz_read;
    d->format = "gcz";
    return 0;
}

/* ---- open ---- */

static gc_disc *open_common(gc_disc *d, char *why, size_t why_len)
{
    uint8_t head[0x48];
    int rc = -1;
    memset(head, 0, sizeof head);
    if (d->file_size < sizeof head) {
        gc_disc_fail(d, "too small for a disc image (%llu bytes)", (unsigned long long)d->file_size);
    } else if (gc_disc_file_read(d, 0, head, sizeof head) == 0) {
        if (!memcmp(head, "CISO", 4)) {
            rc = ciso_open(d, head);
        } else if (gc_le32(head) == GCZ_MAGIC) {
            rc = gcz_open(d, head);
        } else if ((!memcmp(head, "RVZ\x01", 4) || !memcmp(head, "WIA\x01", 4))) {
            rc = gc_disc_wia_open(d, head);
        } else if (gc_be32(head + GC_DISC_MAGIC_OFF) == GC_DISC_MAGIC) {
            d->read = iso_read;
            d->format = "iso";
            d->size = d->file_size;
            rc = 0;
        } else {
            gc_disc_fail(d, "no GameCube disc (no magic 0xC2339F3D at 0x1C, not CISO / GCZ / RVZ / WIA)");
        }
    }
    if (rc == 0) { /* every format: the disc itself must carry the magic */
        uint8_t h[0x20];
        if (gc_disc_read(d, 0, h, sizeof h) || gc_be32(h + GC_DISC_MAGIC_OFF) != GC_DISC_MAGIC)
            rc = d->error[0] ? -1 : gc_disc_fail(d, "%s image of no GameCube disc", d->format);
    }
    if (rc) {
        snprintf(why, why_len, "%s", d->error[0] ? d->error : "unreadable");
        gc_disc_close(d);
        return NULL;
    }
    return d;
}

gc_disc *gc_disc_open(const char *path, char *why, size_t why_len)
{
    FILE *f = NULL;
    if (!path) {
        snprintf(why, why_len, "no path");
        return NULL;
    }
    if (!strncmp(path, "fd:", 3)) {
        char *end;
        long v = strtol(path + 3, &end, 10);
        int fd = end != path + 3 && !*end && v >= 0 ? dup((int)v) : -1;
        if (fd >= 0 && !(f = fdopen(fd, "rb")))
            close(fd);
    } else {
        f = fopen(path, "rb");
    }
    if (!f) {
        snprintf(why, why_len, "cannot open %s", path);
        return NULL;
    }
    gc_disc *d = calloc(1, sizeof *d);
    if (!d) {
        fclose(f);
        snprintf(why, why_len, "out of memory");
        return NULL;
    }
    d->f = f;
    if (fseeko(f, 0, SEEK_END) == 0) {
        off_t n = ftello(f);
        d->file_size = n > 0 ? (uint64_t)n : 0;
    }
    return open_common(d, why, why_len);
}

gc_disc *gc_disc_open_mem(const uint8_t *data, size_t size, char *why, size_t why_len)
{
    gc_disc *d = calloc(1, sizeof *d);
    if (!d) {
        snprintf(why, why_len, "out of memory");
        return NULL;
    }
    d->mem = data;
    d->file_size = size;
    return open_common(d, why, why_len);
}

void gc_disc_close(gc_disc *d)
{
    if (!d)
        return;
    if (d->free)
        d->free(d);
    free(d->priv);
    if (d->f)
        fclose(d->f);
    free(d);
}

const char *gc_disc_format(const gc_disc *d)
{
    return d->format;
}

uint64_t gc_disc_size(const gc_disc *d)
{
    return d->size;
}

const char *gc_disc_error(const gc_disc *d)
{
    return d->error[0] ? d->error : NULL;
}

int gc_disc_read(gc_disc *d, uint64_t off, void *buf, size_t len)
{
    uint8_t *b = buf;
    if (off >= d->size) {
        memset(b, 0, len);
        return 0;
    }
    if (len > d->size - off) {
        size_t in = (size_t)(d->size - off);
        memset(b + in, 0, len - in);
        len = in;
    }
    return len ? d->read(d, off, b, len) : 0;
}

/* ---- header, title key, FST ---- */

static char code_char(uint8_t c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ? (char)c : '_';
}

int gc_disc_key_from_header(const uint8_t hdr[0x20], char *key, size_t key_n)
{
    if (!key || key_n < 24 || gc_be32(hdr + GC_DISC_MAGIC_OFF) != GC_DISC_MAGIC)
        return -1;
    char id[7];
    for (int i = 0; i < 6; i++)
        id[i] = code_char(hdr[i]);
    id[6] = 0;
    if (hdr[6])
        snprintf(key, key_n, "%s-%u-D%u", id, hdr[7], hdr[6] + 1u);
    else
        snprintf(key, key_n, "%s-%u", id, hdr[7]);
    return 0;
}

int gc_disc_title_key(gc_disc *d, char *key, size_t key_n)
{
    uint8_t h[0x20];
    if (gc_disc_read(d, 0, h, sizeof h))
        return -1;
    return gc_disc_key_from_header(h, key, key_n);
}

int gc_disc_header_read(gc_disc *d, gc_disc_header *h)
{
    uint8_t b[0x440], a[0x20];
    memset(h, 0, sizeof *h);
    if (gc_disc_read(d, 0, b, sizeof b) || gc_disc_read(d, GC_APPLOADER_OFF, a, sizeof a))
        return -1;
    if (gc_be32(b + GC_DISC_MAGIC_OFF) != GC_DISC_MAGIC)
        return gc_disc_fail(d, "no GameCube magic");
    memcpy(h->game_id, b, 6);
    for (int i = 0; i < 6; i++)
        if (h->game_id[i] < 0x20 || h->game_id[i] > 0x7E)
            h->game_id[i] = '_';
    h->disc_number = b[6];
    h->revision = b[7];
    h->audio_streaming = b[8];
    h->stream_buffer_size = b[9];
    memcpy(h->name, b + 0x20, 0x40);
    h->dol_off = gc_be32(b + 0x420);
    h->fst_off = gc_be32(b + 0x424);
    h->fst_size = gc_be32(b + 0x428);
    h->fst_max = gc_be32(b + 0x42C);
    memcpy(h->apploader_date, a, 16);
    for (int i = 0; i < 16; i++)
        if (h->apploader_date[i] && (h->apploader_date[i] < 0x20 || h->apploader_date[i] > 0x7E))
            h->apploader_date[i] = '?';
    h->apploader_entry = gc_be32(a + 0x10);
    h->apploader_size = gc_be32(a + 0x14);
    h->apploader_trailer = gc_be32(a + 0x18);
    return 0;
}

void gc_fst_free(gc_fst *f)
{
    if (f) {
        free(f->e);
        f->e = NULL;
        f->n = 0;
    }
}

int gc_disc_fst(gc_disc *d, gc_fst *out)
{
    gc_disc_header h;
    memset(out, 0, sizeof *out);
    if (gc_disc_header_read(d, &h))
        return -1;
    if (h.fst_size < 12 || h.fst_size > (16u << 20) || h.fst_off >= d->size)
        return gc_disc_fail(d, "FST: bad offset or size (0x%X+0x%X)", h.fst_off, h.fst_size);
    uint8_t *t = malloc(h.fst_size + 1);
    if (!t)
        return gc_disc_fail(d, "out of memory");
    if (gc_disc_read(d, h.fst_off, t, h.fst_size)) {
        free(t);
        return -1;
    }
    t[h.fst_size] = 0;
    uint32_t n = gc_be32(t + 8);
    if (!(t[0] & 1) || n < 1 || (uint64_t)n * 12 > h.fst_size) {
        free(t);
        return gc_disc_fail(d, "FST: bad root entry (%u entries)", n);
    }
    const char *names = (const char *)t + (size_t)n * 12;
    uint32_t names_len = h.fst_size - n * 12;
    gc_fst_entry *e = calloc(n, sizeof *e);
    /* directory stack: the end index of each open directory and its path length */
    uint32_t stack_end[64];
    size_t stack_len[64];
    int depth = 0;
    char path[256] = "";
    size_t plen = 0;
    if (!e) {
        free(t);
        return gc_disc_fail(d, "out of memory");
    }
    stack_end[0] = n;
    stack_len[0] = 0;
    e[0].dir = true;
    for (uint32_t i = 1; i < n; i++) {
        while (depth > 0 && i >= stack_end[depth]) {
            depth--;
            plen = stack_len[depth + 1];
            path[plen] = 0;
        }
        const uint8_t *r = t + (size_t)i * 12;
        uint32_t name_off = gc_be32(r) & 0xFFFFFF, a = gc_be32(r + 4), b = gc_be32(r + 8);
        bool dir = r[0] & 1;
        if (name_off >= names_len) {
            free(e);
            free(t);
            return gc_disc_fail(d, "FST: entry %u's name is outside the string table", i);
        }
        const char *nm = names + name_off;
        size_t nl = strnlen(nm, names_len - name_off);
        size_t at = plen;
        if (at + (at ? 1 : 0) + nl >= sizeof path) {
            free(e);
            free(t);
            return gc_disc_fail(d, "FST: path too long at entry %u", i);
        }
        if (at)
            path[at++] = '/';
        memcpy(path + at, nm, nl);
        path[at + nl] = 0;
        memcpy(e[i].path, path, at + nl + 1);
        e[i].dir = dir;
        if (dir) {
            /* a: parent index, b: the index after its last entry */
            if (b <= i || b > stack_end[depth] || depth + 1 >= 64) {
                free(e);
                free(t);
                return gc_disc_fail(d, "FST: directory %u ends at %u, outside its parent", i, b);
            }
            depth++;
            stack_end[depth] = b;
            stack_len[depth] = plen;
            plen = at + nl;
        } else {
            e[i].offset = a;
            e[i].size = b;
            path[plen] = 0;
        }
    }
    free(t);
    out->e = e;
    out->n = n;
    return 0;
}

const gc_fst_entry *gc_fst_find(const gc_fst *f, const char *path)
{
    while (*path == '/')
        path++;
    for (size_t i = 1; i < f->n; i++)
        if (!strcasecmp(f->e[i].path, path))
            return &f->e[i];
    return NULL;
}

uint32_t gc_disc_dol_size(gc_disc *d)
{
    gc_disc_header h;
    uint8_t b[0x100];
    if (gc_disc_header_read(d, &h) || gc_disc_read(d, h.dol_off, b, sizeof b))
        return 0;
    uint32_t end = 0x100;
    for (int i = 0; i < 18; i++) {
        uint32_t off = gc_be32(b + 4 * i), len = gc_be32(b + 0x90 + 4 * i);
        if (len && off + len > end)
            end = off + len;
    }
    return end;
}
