/* gc_disc_wia.c -- WIA and RVZ disc images, GameCube discs only (XBA-261). Written from the formats' public
 * description (Dolphin's docs/WiaAndRvz.md: facts only, no code taken):
 *
 *   0x00  wia_file_head_t (0x48): "WIA\1" / "RVZ\1", versions, size of wia_disc_t, SHA-1, ISO size, file size
 *   0x48  wia_disc_t: disc type, compression (0 none, 1 purge [WIA], 2 bzip2, 3 LZMA, 4 LZMA2, 5 Zstandard [RVZ]),
 *         level, chunk size, the disc's first 0x80 bytes, then partitions (Wii: refused), raw data entries
 *         (compressed table of {u64 disc offset, u64 size, u32 first group, u32 groups}), group entries (compressed
 *         table of {u32 file offset / 4, u32 size} for WIA, plus {u32 packed size} for RVZ), compressor data (LZMA:
 *         7-Zip's 5 property bytes; LZMA2: its dictionary byte)
 *   group data: chunk_size bytes of disc (the last group of an entry less); size 0 means zeros; RVZ: the size's top
 *         bit says compressed (else stored), and a non-zero packed size says the decompressed bytes are RVZ-packed:
 *         {u32 BE size; top bit set: a 68-byte seed and that many bytes of generated padding; else the bytes}
 *   padding generator: lagged Fibonacci, XOR, j = 32, k = 521, seeded with 17 big-endian words, advanced 4 times
 *         before output and once per 521 words output; each word gives bytes >> 24, >> 18, >> 8, >> 0; output at disc
 *         offset o first skips o % 0x8000 bytes
 * The first raw data entry starts at 0x80 (the header's copy holds the rest): entries start rounded down to 0x8000. */
#include <stdlib.h>
#include <string.h>

#include "gc_disc_internal.h"
#include "LzmaDec.h"
#include "zstd.h"

/* Exported by the LZMA SDK's LzmaDec.c for its LZMA2 decoder, not declared in LzmaDec.h. */
void LzmaDec_InitDicAndState(CLzmaDec *p, BoolInt initDic, BoolInt initState);

enum { WIA_NONE = 0, WIA_PURGE = 1, WIA_BZIP2 = 2, WIA_LZMA = 3, WIA_LZMA2 = 4, WIA_ZSTD = 5 };

typedef struct {
    uint64_t start, end;     /* disc range, start rounded down to 0x8000 */
    uint32_t group, ngroups;
} wia_raw;

typedef struct {
    uint64_t off;            /* file offset */
    uint32_t size;           /* stored bytes (without the RVZ flag) */
    bool compressed;
    uint32_t packed;         /* RVZ packed size, 0 */
} wia_group;

typedef struct {
    bool rvz;
    uint32_t method, chunk;
    uint8_t dhead[0x80];
    uint8_t props[7];
    wia_raw *raw;
    uint32_t nraw;
    wia_group *groups;
    uint32_t ngroups;
    uint8_t *cache;          /* one group's disc bytes */
    int64_t cached;          /* its group index */
    uint64_t cached_base;    /* and the disc range it was decoded for (two raw entries may name one group) */
    size_t cached_len;
    uint8_t *stored, *unpack;
    size_t stored_cap, unpack_cap;
    ZSTD_DCtx *zstd;
} wia_priv;

/* ---- the padding generator ---- */

#define LFG_K 521
#define LFG_J 32
#define LFG_BYTES (LFG_K * 4)

typedef struct {
    uint32_t b[LFG_K];
    uint32_t pos; /* bytes into the buffer */
} lfg;

static void lfg_forward(lfg *g)
{
    for (int i = 0; i < LFG_J; i++)
        g->b[i] ^= g->b[i + LFG_K - LFG_J];
    for (int i = LFG_J; i < LFG_K; i++)
        g->b[i] ^= g->b[i - LFG_J];
}

static void lfg_seed(lfg *g, const uint8_t seed[68])
{
    for (int i = 0; i < 17; i++)
        g->b[i] = gc_be32(seed + 4 * i);
    for (int i = 17; i < LFG_K; i++)
        g->b[i] = (g->b[i - 17] << 23) ^ (g->b[i - 16] >> 9) ^ g->b[i - 1];
    for (int i = 0; i < 4; i++)
        lfg_forward(g);
    g->pos = 0;
}

static void lfg_skip(lfg *g, uint64_t n)
{
    n += g->pos;
    while (n >= LFG_BYTES) {
        lfg_forward(g);
        n -= LFG_BYTES;
    }
    g->pos = (uint32_t)n;
}

static void lfg_out(lfg *g, uint8_t *out, size_t n)
{
    static const uint8_t shift[4] = {24, 18, 8, 0};
    while (n--) {
        *out++ = (uint8_t)(g->b[g->pos >> 2] >> shift[g->pos & 3]);
        if (++g->pos == LFG_BYTES) {
            lfg_forward(g);
            g->pos = 0;
        }
    }
}

/* ---- codecs ---- */

static void *sz_alloc(ISzAllocPtr p, size_t n)
{
    (void)p;
    return malloc(n);
}

static void sz_free(ISzAllocPtr p, void *a)
{
    (void)p;
    free(a);
}

static const ISzAlloc lzma_alloc = {sz_alloc, sz_free};

/* LZMA2 chunks on the LZMA decoder: the whole output is the dictionary (it never wraps). */
static int lzma2_decode(const uint8_t *src, size_t n, uint8_t *out, size_t out_len, uint8_t dict_prop)
{
    if (dict_prop > 40)
        return -1;
    uint32_t dict = dict_prop == 40 ? 0xFFFFFFFFu : (2u | (dict_prop & 1u)) << (dict_prop / 2 + 11);
    CLzmaDec dec;
    LzmaDec_Construct(&dec);
    /* Construct sets the pointers only. A stream whose first chunk does not reset the dictionary (not LZMA2,
     * but a damaged file can say so) would otherwise decode with whatever the stack held here, and a match
     * distance checked against it can reach outside the output. */
    dec.processedPos = 0;
    dec.checkDicSize = 0;
    bool have_props = false, dict_reset = false;
    size_t i = 0;
    int rc = -1;
    dec.dicPos = 0;
    for (;;) {
        if (i >= n)
            goto done;
        uint8_t c = src[i++];
        if (c == 0) {
            rc = dec.dicPos == out_len ? 0 : -1;
            goto done;
        }
        if (c == 1 || c == 2) { /* stored chunk (1: dictionary reset) */
            if (i + 2 > n)
                goto done;
            size_t len = ((size_t)src[i] << 8 | src[i + 1]) + 1;
            i += 2;
            if (i + len > n || dec.dicPos + len > out_len || (c == 2 && !have_props && !dec.probs && dec.dicPos == 0))
                goto done;
            if (c == 1)
                dict_reset = true;
            else if (!dict_reset)
                goto done;
            if (!dec.probs) { /* stored data before any LZMA chunk: a decoder to keep the dictionary state */
                uint8_t p5[5] = {0, (uint8_t)dict, (uint8_t)(dict >> 8), (uint8_t)(dict >> 16), (uint8_t)(dict >> 24)};
                if (LzmaDec_AllocateProbs(&dec, p5, 5, &lzma_alloc) != SZ_OK)
                    goto done;
                dec.dic = out;
                dec.dicBufSize = out_len;
            }
            LzmaDec_InitDicAndState(&dec, c == 1, False);
            memcpy(out + dec.dicPos, src + i, len);
            if (dec.checkDicSize == 0 && dec.prop.dicSize - dec.processedPos <= len)
                dec.checkDicSize = dec.prop.dicSize;
            dec.processedPos += (UInt32)len;
            dec.dicPos += len;
            i += len;
            continue;
        }
        if (!(c & 0x80) || i + 4 > n)
            goto done;
        size_t unpack = (((size_t)c & 0x1F) << 16 | (size_t)src[i] << 8 | src[i + 1]) + 1;
        size_t pack = ((size_t)src[i + 2] << 8 | src[i + 3]) + 1;
        i += 4;
        unsigned mode = (c >> 5) & 3;
        if (mode >= 2) {
            if (i >= n)
                goto done;
            uint8_t p5[5] = {src[i++], (uint8_t)dict, (uint8_t)(dict >> 8), (uint8_t)(dict >> 16), (uint8_t)(dict >> 24)};
            if (p5[0] >= 9 * 5 * 5 || LzmaDec_AllocateProbs(&dec, p5, 5, &lzma_alloc) != SZ_OK)
                goto done;
            dec.dic = out;
            dec.dicBufSize = out_len;
            have_props = true;
        } else if (!have_props) {
            goto done;
        }
        if (i + pack > n || dec.dicPos + unpack > out_len)
            goto done;
        if (mode == 3)
            dict_reset = true;
        else if (!dict_reset)
            goto done; /* the first chunk of an LZMA2 stream resets the dictionary */
        LzmaDec_InitDicAndState(&dec, mode == 3, mode >= 1);
        SizeT src_len = pack;
        ELzmaStatus st;
        size_t want = dec.dicPos + unpack;
        if (LzmaDec_DecodeToDic(&dec, want, src + i, &src_len, LZMA_FINISH_END, &st) != SZ_OK || src_len != pack
            || dec.dicPos != want)
            goto done;
        i += pack;
    }
done:
    dec.dic = NULL; /* ours */
    LzmaDec_FreeProbs(&dec, &lzma_alloc);
    return rc;
}

/* One stored block (a table or a group) into exactly out_len bytes. */
static int decompress(gc_disc *d, uint32_t method, const uint8_t *src, size_t n, uint8_t *out, size_t out_len)
{
    wia_priv *p = d->priv;
    switch (method) {
    case WIA_NONE:
        if (n < out_len)
            return gc_disc_fail(d, "%s: stored block shorter than its data", d->format);
        memcpy(out, src, out_len);
        return 0;
    case WIA_PURGE: { /* segments {u32 offset, u32 size, data}, then a SHA-1; the rest is zeros */
        if (n < 20)
            return gc_disc_fail(d, "WIA: purge block too short");
        memset(out, 0, out_len);
        size_t i = 0, end = n - 20;
        while (i < end) {
            if (i + 8 > end)
                return gc_disc_fail(d, "WIA: bad purge segment");
            uint32_t off = gc_be32(src + i), len = gc_be32(src + i + 4);
            i += 8;
            if (len > end - i || off > out_len || len > out_len - off)
                return gc_disc_fail(d, "WIA: purge segment out of range");
            memcpy(out + off, src + i, len);
            i += len;
        }
        return 0;
    }
    case WIA_BZIP2:
        if (gc_bzip2_decompress(src, n, out, out_len))
            return gc_disc_fail(d, "%s: bad bzip2 data", d->format);
        return 0;
    case WIA_LZMA: {
        SizeT dl = out_len, sl = n;
        ELzmaStatus st;
        if (LzmaDecode(out, &dl, src, &sl, p->props, 5, LZMA_FINISH_ANY, &st, &lzma_alloc) != SZ_OK || dl != out_len)
            return gc_disc_fail(d, "%s: bad LZMA data", d->format);
        return 0;
    }
    case WIA_LZMA2:
        if (lzma2_decode(src, n, out, out_len, p->props[0]))
            return gc_disc_fail(d, "%s: bad LZMA2 data", d->format);
        return 0;
    case WIA_ZSTD: {
        if (!p->zstd && !(p->zstd = ZSTD_createDCtx()))
            return gc_disc_fail(d, "out of memory");
        size_t got = ZSTD_decompressDCtx(p->zstd, out, out_len, src, n);
        if (ZSTD_isError(got) || got != out_len)
            return gc_disc_fail(d, "RVZ: bad Zstandard data");
        return 0;
    }
    default:
        return gc_disc_fail(d, "%s: unknown compression %u", d->format, method);
    }
}

static int grow(gc_disc *d, uint8_t **buf, size_t *cap, size_t n)
{
    if (*cap >= n)
        return 0;
    uint8_t *b = realloc(*buf, n);
    if (!b)
        return gc_disc_fail(d, "out of memory");
    *buf = b;
    *cap = n;
    return 0;
}

/* A compressed table at off (n stored bytes) into out (out_len bytes). */
static int read_table(gc_disc *d, uint64_t off, uint32_t n, uint8_t *out, size_t out_len)
{
    wia_priv *p = d->priv;
    /* the header's size is checked against the file first: 4 GB asked by a damaged header is never allocated */
    if (off > d->file_size || n > d->file_size - off)
        return gc_disc_fail(d, "%s: table of %u bytes at 0x%llX past the file's end", d->format, n,
                            (unsigned long long)off);
    if (grow(d, &p->stored, &p->stored_cap, n ? n : 1) || gc_disc_file_read(d, off, p->stored, n))
        return -1;
    return decompress(d, p->method, p->stored, n, out, out_len);
}

/* RVZ packing: src (n bytes) into out (out_len bytes), the group starting at disc offset base. */
static int rvz_unpack(gc_disc *d, const uint8_t *src, size_t n, uint8_t *out, size_t out_len, uint64_t base)
{
    size_t i = 0, o = 0;
    lfg g;
    while (o < out_len) {
        if (i + 4 > n)
            return gc_disc_fail(d, "RVZ: packed data ends early");
        uint32_t size = gc_be32(src + i);
        i += 4;
        bool junk = size >> 31;
        size &= 0x7FFFFFFFu;
        if (size > out_len - o)
            return gc_disc_fail(d, "RVZ: packed run past the group's end");
        if (junk) {
            if (i + 68 > n)
                return gc_disc_fail(d, "RVZ: packed seed cut");
            lfg_seed(&g, src + i);
            i += 68;
            lfg_skip(&g, (base + o) % 0x8000);
            lfg_out(&g, out + o, size);
        } else {
            if (size > n - i)
                return gc_disc_fail(d, "RVZ: packed data cut");
            memcpy(out + o, src + i, size);
            i += size;
        }
        o += size;
    }
    return 0;
}

/* Group gi, the disc bytes [base, base + len), into p->cache. */
static int load_group(gc_disc *d, uint32_t gi, uint64_t base, size_t len)
{
    wia_priv *p = d->priv;
    if (p->cached == gi && p->cached_base == base && p->cached_len == len)
        return 0;
    p->cached = -1;
    const wia_group *g = &p->groups[gi];
    if (!g->size) {
        memset(p->cache, 0, len);
    } else {
        if (grow(d, &p->stored, &p->stored_cap, g->size) || gc_disc_file_read(d, g->off, p->stored, g->size))
            return -1;
        uint32_t method = g->compressed ? p->method : WIA_NONE;
        if (g->packed) {
            if (grow(d, &p->unpack, &p->unpack_cap, g->packed)
                || decompress(d, method, p->stored, g->size, p->unpack, g->packed)
                || rvz_unpack(d, p->unpack, g->packed, p->cache, len, base))
                return -1;
        } else if (decompress(d, method, p->stored, g->size, p->cache, len)) {
            return -1;
        }
    }
    p->cached = gi;
    p->cached_base = base;
    p->cached_len = len;
    return 0;
}

static int wia_read(gc_disc *d, uint64_t off, uint8_t *buf, size_t len)
{
    wia_priv *p = d->priv;
    while (len) {
        size_t n;
        if (off < 0x80) {
            n = 0x80 - off < len ? (size_t)(0x80 - off) : len;
            memcpy(buf, p->dhead + off, n);
        } else {
            const wia_raw *r = NULL;
            uint64_t next = UINT64_MAX;
            for (uint32_t i = 0; i < p->nraw; i++) {
                if (off >= p->raw[i].start && off < p->raw[i].end) {
                    r = &p->raw[i];
                    break;
                }
                if (p->raw[i].start > off && p->raw[i].start < next)
                    next = p->raw[i].start;
            }
            if (!r) { /* not covered: zeros up to the next entry */
                n = next - off < len ? (size_t)(next - off) : len;
                memset(buf, 0, n);
            } else {
                uint64_t k = (off - r->start) / p->chunk;
                uint64_t base = r->start + k * p->chunk;
                size_t glen = (size_t)(r->end - base < p->chunk ? r->end - base : p->chunk);
                if (k >= r->ngroups)
                    return gc_disc_fail(d, "%s: offset 0x%llX has no group", d->format, (unsigned long long)off);
                if (load_group(d, r->group + (uint32_t)k, base, glen))
                    return -1;
                size_t in = (size_t)(off - base);
                n = glen - in < len ? glen - in : len;
                memcpy(buf, p->cache + in, n);
            }
        }
        off += n;
        buf += n;
        len -= n;
    }
    return 0;
}

static void wia_free(gc_disc *d)
{
    wia_priv *p = d->priv;
    if (!p)
        return;
    free(p->raw);
    free(p->groups);
    free(p->cache);
    free(p->stored);
    free(p->unpack);
    if (p->zstd)
        ZSTD_freeDCtx(p->zstd);
}

int gc_disc_wia_open(gc_disc *d, const uint8_t *head)
{
    wia_priv *p = calloc(1, sizeof *p);
    uint8_t disc[0xDC];
    if (!p)
        return gc_disc_fail(d, "out of memory");
    d->priv = p;
    d->free = wia_free;
    p->rvz = head[0] == 'R';
    p->cached = -1;
    d->format = p->rvz ? "rvz" : "wia";
    uint32_t disc_size = gc_be32(head + 0x0C);
    memset(disc, 0, sizeof disc);
    if (disc_size < 0xD5 || gc_disc_file_read(d, 0x48, disc, disc_size < sizeof disc ? disc_size : sizeof disc))
        return gc_disc_fail(d, "%s: bad disc struct", d->format);
    d->size = gc_be64(head + 0x24);
    uint32_t type = gc_be32(disc + 0x00);
    p->method = gc_be32(disc + 0x04);
    p->chunk = gc_be32(disc + 0x0C);
    memcpy(p->dhead, disc + 0x10, 0x80);
    uint32_t n_part = gc_be32(disc + 0x90);
    p->nraw = gc_be32(disc + 0xB4);
    uint64_t raw_off = gc_be64(disc + 0xB8);
    uint32_t raw_size = gc_be32(disc + 0xC0);
    p->ngroups = gc_be32(disc + 0xC4);
    uint64_t group_off = gc_be64(disc + 0xC8);
    uint32_t group_size = gc_be32(disc + 0xD0);
    memcpy(p->props, disc + 0xD5, 7);
    if (type == 2 || n_part)
        return gc_disc_fail(d, "%s: a Wii disc (not supported yet)", d->format);
    if (p->method > WIA_ZSTD || (p->rvz && p->method == WIA_PURGE) || (!p->rvz && p->method == WIA_ZSTD))
        return gc_disc_fail(d, "%s: compression %u not valid here", d->format, p->method);
    if (p->chunk < 0x8000 || p->chunk > (64u << 20) || (p->chunk < 0x200000 && (p->chunk & (p->chunk - 1)))
        || (p->chunk >= 0x200000 && p->chunk % 0x200000))
        return gc_disc_fail(d, "%s: bad chunk size 0x%X", d->format, p->chunk);
    if (!p->nraw || p->nraw > 4096 || p->ngroups > (1u << 22) || d->size > (16ull << 30))
        return gc_disc_fail(d, "%s: bad table sizes", d->format);
    size_t gsz = p->rvz ? 12 : 8;
    uint8_t *rt = malloc((size_t)p->nraw * 0x18), *gt = malloc((size_t)p->ngroups * gsz + 1);
    p->raw = calloc(p->nraw, sizeof *p->raw);
    p->groups = calloc(p->ngroups + 1, sizeof *p->groups);
    p->cache = malloc(p->chunk);
    if (!rt || !gt || !p->raw || !p->groups || !p->cache) {
        free(rt);
        free(gt);
        return gc_disc_fail(d, "out of memory");
    }
    int rc = read_table(d, raw_off, raw_size, rt, (size_t)p->nraw * 0x18);
    if (!rc)
        rc = read_table(d, group_off, group_size, gt, (size_t)p->ngroups * gsz);
    for (uint32_t i = 0; !rc && i < p->nraw; i++) {
        const uint8_t *e = rt + (size_t)i * 0x18;
        uint64_t off = gc_be64(e), size = gc_be64(e + 8);
        wia_raw *r = &p->raw[i];
        r->start = off & ~0x7FFFull;
        r->end = off + size;
        r->group = gc_be32(e + 16);
        r->ngroups = gc_be32(e + 20);
        if (r->end < off || (uint64_t)r->group + r->ngroups > p->ngroups
            || (r->end - r->start + p->chunk - 1) / p->chunk > r->ngroups)
            rc = gc_disc_fail(d, "%s: raw data entry %u is inconsistent", d->format, i);
    }
    for (uint32_t i = 0; !rc && i < p->ngroups; i++) {
        const uint8_t *e = gt + (size_t)i * gsz;
        wia_group *g = &p->groups[i];
        uint32_t size = gc_be32(e + 4);
        g->off = (uint64_t)gc_be32(e) * 4;
        if (p->rvz) {
            g->compressed = size >> 31;
            g->size = size & 0x7FFFFFFFu;
            g->packed = gc_be32(e + 8);
        } else {
            g->compressed = true;
            g->size = size;
        }
        if (g->off + g->size > d->file_size)
            rc = gc_disc_fail(d, "%s: group %u runs past the file's end", d->format, i);
    }
    free(rt);
    free(gt);
    if (rc)
        return -1;
    d->read = wia_read;
    return 0;
}
