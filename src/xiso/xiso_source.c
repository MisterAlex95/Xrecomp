/* Disc image sources: one file, FAT32 split parts, compressed images (XBA-126). See xiso.h, « Sources ».
 *
 * Compressed formats, as far as this file needs them (all little-endian):
 *   CSO / ZSO header, 24 bytes: "CISO" or "ZISO", u32 header size (0 in some v1 files: 24), u64 image size,
 *     u32 block size, u8 version, u8 index alignment (shift), 2 reserved. The index follows the header:
 *     blocks + 1 u32 entries, (entry & 0x7FFFFFFF) << alignment = the block's offset in the file, the next entry
 *     its end. Bit 31:
 *       CSO v0/v1 (PSP tools): set = stored, clear = raw deflate.
 *       CSO v2 (maxcso): set = LZ4 if smaller than a block, else stored; clear = raw deflate.
 *       CSO v2 (the Xbox tools, MakeMHz ciso.py): set = LZ4, clear = stored (a full block); an LZ4 block may keep
 *       the 4-byte block header of an LZ4 frame. Both v2 readings are accepted: a block as long as the block is
 *       stored, a shorter one decodes as LZ4 when flagged and as deflate (then LZ4) when not.
 *       ZSO: set = stored, clear = LZ4.
 *   CCI header, 32 bytes: "CCIM", u32 header size, u64 image size, u64 index offset, u32 block size, u8 version,
 *     u8 index alignment, 2 reserved. The index (blocks + 1 u32) is at the index offset; bit 31 set = LZ4, whose
 *     first byte is the count of padding bytes at the block's end (tried without it too).
 * Every LZ4 block decodes to exactly the block's size and may be followed by alignment padding; every deflate block
 * is a complete raw deflate stream (final block set) of exactly the block's size. A block that does not decode is
 * XISO_ECORRUPT for the read that needed it, never wrong bytes.
 */
#define _FILE_OFFSET_BITS 64
#define _GNU_SOURCE 1

#include "xiso.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define BATCH_BYTES     (1u << 20)      /* compressed bytes read in one pread for a run of blocks */
#define CACHE_BYTES     (4u << 20)      /* decoded blocks kept */
#define MIN_BLOCK       512u
#define MAX_BLOCK       (1u << 20)

struct xiso_source {
    int nparts;
    int fds[XISO_SOURCE_MAX_PARTS];
    uint64_t starts[XISO_SOURCE_MAX_PARTS + 1];  /* starts[nparts]: total stored bytes (UINT64_MAX: unknown) */
    int own;
    int kind;                                    /* XISO_KIND_PLAIN, _CSO, _ZSO, _CCI */

    /* compressed */
    uint64_t size;                               /* decompressed */
    uint32_t block_size, nblocks;
    uint32_t *index;                             /* nblocks + 1 entries */
    int align, version;
    pthread_mutex_t lock;
    int lock_ok;
    uint32_t slots;                              /* direct-mapped cache, slot = block % slots */
    uint32_t *slot_block;                        /* UINT32_MAX: empty */
    uint8_t *cache;                              /* slots * block_size */
    uint8_t *batch;                              /* BATCH_BYTES + 2 * block_size */
    uint32_t raw_first, raw_end;                 /* the blocks whose stored bytes are in batch */
    uint32_t seq_next;                           /* the block after the last one decoded: a read there streams */
};

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }

/* ── Stored bytes (the parts, concatenated) ─────────────────────────────── */

/* Reads up to len stored bytes at off. Returns the bytes read (fewer only at the end), or XISO_EIO. */
static long raw_pread(const xiso_source *s, void *buf, size_t len, uint64_t off)
{
    uint8_t *p = buf;
    size_t done = 0;
    int i = 0;
    while (done < len) {
        while (i < s->nparts && off >= s->starts[i + 1])
            i++;
        if (i >= s->nparts)
            break;
        uint64_t room = s->starts[i + 1] - off;          /* UINT64_MAX - off for an unknown size */
        size_t want = len - done < room ? len - done : (size_t)room;
        ssize_t n = pread(s->fds[i], p + done, want, (off_t)(off - s->starts[i]));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return XISO_EIO;
        }
        if (n == 0) {
            if (s->starts[i + 1] == UINT64_MAX)
                break;                                   /* the end of a file of unknown size */
            return XISO_EIO;                             /* a part shorter than it said: it changed under us */
        }
        done += (size_t)n;
        off += (uint64_t)n;
    }
    return (long)done;
}

/* ── LZ4 block ───────────────────────────────────────────────────────────── */

/* Decodes one LZ4 block into exactly want bytes. Input after the block (alignment padding) is ignored.
 * Returns want, or -1. */
static long lz4_block(const uint8_t *in, size_t inlen, uint8_t *out, size_t want)
{
    const uint8_t *ip = in, *iend = in + inlen;
    uint8_t *op = out, *oend = out + want;
    if (want == 0)
        return 0;
    while (ip < iend) {
        unsigned token = *ip++;
        size_t lit = token >> 4, ml;
        unsigned b;
        if (lit == 15) {
            do {
                if (ip >= iend)
                    return -1;
                b = *ip++;
                lit += b;
            } while (b == 255);
        }
        if (lit > (size_t)(iend - ip) || lit > (size_t)(oend - op))
            return -1;
        memcpy(op, ip, lit);
        op += lit;
        ip += lit;
        if (op == oend)
            return (long)want;                           /* the last literals */
        if (iend - ip < 2)
            return -1;
        size_t off = (size_t)ip[0] | (size_t)ip[1] << 8;
        ip += 2;
        if (off == 0 || off > (size_t)(op - out))
            return -1;
        ml = token & 15;
        if (ml == 15) {
            do {
                if (ip >= iend)
                    return -1;
                b = *ip++;
                ml += b;
            } while (b == 255);
        }
        ml += 4;
        if (ml > (size_t)(oend - op))
            return -1;
        const uint8_t *m = op - off;
        while (ml--)
            *op++ = *m++;
        if (op == oend)
            return (long)want;
    }
    return -1;
}

/* ── Raw deflate (RFC 1951), after Mark Adler's puff.c ───────────────────── */

#define MAXBITS   15
#define MAXLCODES 286
#define MAXDCODES 30
#define MAXCODES  (MAXLCODES + MAXDCODES)
#define FIXLCODES 288

typedef struct {
    const uint8_t *in;
    size_t inlen, incnt;
    uint64_t bitbuf;
    int bitcnt;
    uint8_t *out;
    size_t outlen, outcnt;
    jmp_buf env;
} inf_t;

typedef struct {
    short *count;
    short *symbol;
} huff_t;

static int bits(inf_t *s, int need)
{
    uint64_t val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->incnt == s->inlen)
            longjmp(s->env, 1);                          /* out of input */
        val |= (uint64_t)s->in[s->incnt++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return (int)(val & ((1u << need) - 1));
}

static int stored(inf_t *s)
{
    unsigned len;
    s->bitbuf = 0;
    s->bitcnt = 0;
    if (s->inlen - s->incnt < 4)
        return 2;
    len = s->in[s->incnt] | (unsigned)s->in[s->incnt + 1] << 8;
    if (s->in[s->incnt + 2] != (~len & 0xFF) || s->in[s->incnt + 3] != ((~len >> 8) & 0xFF))
        return -2;
    s->incnt += 4;
    if (s->inlen - s->incnt < len)
        return 2;
    if (s->outlen - s->outcnt < len)
        return 1;
    memcpy(s->out + s->outcnt, s->in + s->incnt, len);
    s->outcnt += len;
    s->incnt += len;
    return 0;
}

static int decode(inf_t *s, const huff_t *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        code |= bits(s, 1);
        int count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -10;
}

static int construct(huff_t *h, const short *length, int n)
{
    short offs[MAXBITS + 1];
    int left = 1;
    for (int len = 0; len <= MAXBITS; len++)
        h->count[len] = 0;
    for (int sym = 0; sym < n; sym++)
        h->count[length[sym]]++;
    if (h->count[0] == n)
        return 0;
    for (int len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0)
            return left;
    }
    offs[1] = 0;
    for (int len = 1; len < MAXBITS; len++)
        offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (int sym = 0; sym < n; sym++)
        if (length[sym] != 0)
            h->symbol[offs[length[sym]]++] = (short)sym;
    return left;
}

static int codes(inf_t *s, const huff_t *lencode, const huff_t *distcode)
{
    static const short lens[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83,
                                    99, 115, 131, 163, 195, 227, 258 };
    static const short lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5,
                                    0 };
    static const short dists[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                     1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
    static const short dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11,
                                    12, 12, 13, 13 };
    int symbol;
    do {
        symbol = decode(s, lencode);
        if (symbol < 0)
            return symbol;
        if (symbol < 256) {
            if (s->outcnt == s->outlen)
                return 1;
            s->out[s->outcnt++] = (uint8_t)symbol;
        } else if (symbol > 256) {
            symbol -= 257;
            if (symbol >= 29)
                return -10;
            size_t len = (size_t)lens[symbol] + (size_t)bits(s, lext[symbol]);
            symbol = decode(s, distcode);
            if (symbol < 0)
                return symbol;
            if (symbol >= 30)
                return -10;
            size_t dist = (size_t)dists[symbol] + (size_t)bits(s, dext[symbol]);
            if (dist > s->outcnt)
                return -11;
            if (s->outlen - s->outcnt < len)
                return 1;
            while (len--) {
                s->out[s->outcnt] = s->out[s->outcnt - dist];
                s->outcnt++;
            }
            symbol = 0;
        }
    } while (symbol != 256);
    return 0;
}

static int fixed(inf_t *s)
{
    short lencnt[MAXBITS + 1], lensym[FIXLCODES], distcnt[MAXBITS + 1], distsym[MAXDCODES];
    short lengths[FIXLCODES];
    huff_t lencode = { lencnt, lensym }, distcode = { distcnt, distsym };
    int sym;
    for (sym = 0; sym < 144; sym++) lengths[sym] = 8;
    for (; sym < 256; sym++) lengths[sym] = 9;
    for (; sym < 280; sym++) lengths[sym] = 7;
    for (; sym < FIXLCODES; sym++) lengths[sym] = 8;
    construct(&lencode, lengths, FIXLCODES);
    for (sym = 0; sym < MAXDCODES; sym++) lengths[sym] = 5;
    construct(&distcode, lengths, MAXDCODES);
    return codes(s, &lencode, &distcode);
}

static int dynamic(inf_t *s)
{
    static const short order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    short lengths[MAXCODES];
    short lencnt[MAXBITS + 1], lensym[MAXLCODES], distcnt[MAXBITS + 1], distsym[MAXDCODES];
    huff_t lencode = { lencnt, lensym }, distcode = { distcnt, distsym };
    int nlen = bits(s, 5) + 257, ndist = bits(s, 5) + 1, ncode = bits(s, 4) + 4, index, err;
    if (nlen > MAXLCODES || ndist > MAXDCODES)
        return -3;
    for (index = 0; index < ncode; index++)
        lengths[order[index]] = (short)bits(s, 3);
    for (; index < 19; index++)
        lengths[order[index]] = 0;
    if (construct(&lencode, lengths, 19) != 0)
        return -4;
    index = 0;
    while (index < nlen + ndist) {
        int symbol = decode(s, &lencode), len = 0;
        if (symbol < 0)
            return symbol;
        if (symbol < 16) {
            lengths[index++] = (short)symbol;
            continue;
        }
        if (symbol == 16) {
            if (index == 0)
                return -5;
            len = lengths[index - 1];
            symbol = 3 + bits(s, 2);
        } else if (symbol == 17) {
            symbol = 3 + bits(s, 3);
        } else {
            symbol = 11 + bits(s, 7);
        }
        if (index + symbol > nlen + ndist)
            return -6;
        while (symbol--)
            lengths[index++] = (short)len;
    }
    if (lengths[256] == 0)
        return -9;
    err = construct(&lencode, lengths, nlen);
    if (err && (err < 0 || nlen != lencode.count[0] + lencode.count[1]))
        return -7;
    err = construct(&distcode, lengths + nlen, ndist);
    if (err && (err < 0 || ndist != distcode.count[0] + distcode.count[1]))
        return -8;
    return codes(s, &lencode, &distcode);
}

/* Inflates a complete raw deflate stream into exactly want bytes. Returns want, or -1. */
static long inflate_block(const uint8_t *in, size_t inlen, uint8_t *out, size_t want)
{
    inf_t s;
    volatile int err = 0;
    memset(&s, 0, sizeof s);
    s.in = in;
    s.inlen = inlen;
    s.out = out;
    s.outlen = want;
    if (setjmp(s.env) != 0) {
        err = 2;
    } else {
        int last;
        do {
            last = bits(&s, 1);
            int type = bits(&s, 2);
            err = type == 0 ? stored(&s) : type == 1 ? fixed(&s) : type == 2 ? dynamic(&s) : -1;
            if (err)
                break;
        } while (!last);
    }
    return err == 0 && s.outcnt == want ? (long)want : -1;
}

/* ── Compressed blocks ───────────────────────────────────────────────────── */

static uint64_t block_pos(const xiso_source *s, uint32_t b)
{
    return (uint64_t)(s->index[b] & 0x7FFFFFFFu) << s->align;
}

/* LZ4, with or without the 4-byte block header an LZ4 frame puts in front (the Xbox tools keep it in some
 * versions; its value is the block's length, bit 31 = stored). */
static long lz4_maybe_framed(const uint8_t *d, size_t span, uint8_t *out, size_t want, int align)
{
    if (span >= 8) {
        uint32_t v = rd32(d), n = v & 0x7FFFFFFFu;
        if (n <= span - 4 && span - 4 - n <= (1u << align) + 3u) {
            if (v >> 31) {
                if (n >= want) {
                    memcpy(out, d + 4, want);
                    return (long)want;
                }
            } else if (lz4_block(d + 4, n, out, want) == (long)want) {
                return (long)want;
            }
        }
    }
    return lz4_block(d, span, out, want);
}

static long decode_block(const xiso_source *s, uint32_t b, const uint8_t *d, size_t span, uint8_t *out)
{
    size_t want = b + 1 < s->nblocks ? s->block_size : (size_t)(s->size - (uint64_t)b * s->block_size);
    int flag = (int)(s->index[b] >> 31);
    long r = -1;
    switch (s->kind) {
    case XISO_KIND_CSO:
        if (s->version < 2) {
            if (flag) {
                if (span < want)
                    return -1;
                memcpy(out, d, want);
                return (long)want;
            }
            return inflate_block(d, span, out, want);
        }
        if (span >= want) {                              /* stored, in both v2 readings */
            memcpy(out, d, want);
            return (long)want;
        }
        if (flag)
            return lz4_maybe_framed(d, span, out, want, s->align);
        r = inflate_block(d, span, out, want);
        return r == (long)want ? r : lz4_maybe_framed(d, span, out, want, s->align);
    case XISO_KIND_ZSO:
        if (flag) {
            if (span < want)
                return -1;
            memcpy(out, d, want);
            return (long)want;
        }
        return lz4_block(d, span, out, want);
    case XISO_KIND_CCI:
        if (!flag) {
            if (span < want)
                return -1;
            memcpy(out, d, want);
            return (long)want;
        }
        if (span >= 1 && d[0] < (1u << s->align) + 1u && span - 1 > d[0]
                && lz4_block(d + 1, span - 1 - d[0], out, want) == (long)want)
            return (long)want;
        return lz4_block(d, span, out, want);
    default:
        return -1;
    }
}

/* Reads the stored bytes of blocks [b, b + want) -- fewer when they exceed a batch, at least one -- into batch. */
static int load_raw(xiso_source *s, uint32_t b, uint32_t want)
{
    uint32_t end = b;
    uint64_t start = block_pos(s, b);
    while (end < s->nblocks && end - b < want) {
        uint64_t e = block_pos(s, end + 1);
        if (e < start || e - start > BATCH_BYTES)
            break;
        end++;
    }
    if (end == b)
        end = b + 1;                                     /* one block always goes, even an odd one */
    uint64_t stop = block_pos(s, end);
    if (stop < start || stop - start > BATCH_BYTES + 2ull * s->block_size)
        return XISO_ECORRUPT;
    s->raw_first = s->raw_end = 0;
    long n = raw_pread(s, s->batch, (size_t)(stop - start), start);
    if (n < 0)
        return (int)n;
    if ((uint64_t)n != stop - start)
        return XISO_ECORRUPT;                            /* the index points past the end of the file */
    s->raw_first = b;
    s->raw_end = end;
    return 0;
}

/* Decodes block b into its cache slot. A streaming read (b follows the last block decoded) loads a batch of stored
 * bytes ahead in one pread, decoded as the reads get there; a random one loads only the blocks the read covers
 * (need), so a 4 KB seek decodes one or two blocks, not a batch. Called with the lock held. */
static int fill(xiso_source *s, uint32_t b, uint32_t need)
{
    if (!(s->raw_first <= b && b < s->raw_end)) {
        uint32_t max = s->slots < 64 ? s->slots : 64;
        int rc = load_raw(s, b, b == s->seq_next ? max : (need < max ? need : max));
        if (rc)
            return rc;
    }
    uint64_t base = block_pos(s, s->raw_first), p = block_pos(s, b), q = block_pos(s, b + 1);
    if (q < p || q - p > 2ull * s->block_size + 64)
        return XISO_ECORRUPT;
    uint32_t slot = b % s->slots;
    s->slot_block[slot] = UINT32_MAX;
    if (decode_block(s, b, s->batch + (p - base), (size_t)(q - p), s->cache + (size_t)slot * s->block_size) < 0)
        return XISO_ECORRUPT;
    s->slot_block[slot] = b;
    s->seq_next = b + 1;
    return 0;
}

static long compressed_pread(xiso_source *s, void *buf, size_t len, uint64_t off)
{
    uint8_t *p = buf;
    size_t done = 0;
    int rc = 0;
    if (off >= s->size)
        return 0;
    if (len > s->size - off)
        len = (size_t)(s->size - off);
    pthread_mutex_lock(&s->lock);
    while (done < len) {
        uint64_t at = off + done;
        uint32_t b = (uint32_t)(at / s->block_size), in = (uint32_t)(at % s->block_size);
        uint32_t slot = b % s->slots;
        if (s->slot_block[slot] != b) {
            uint64_t last = (off + len - 1) / s->block_size;   /* the blocks this read still covers */
            if ((rc = fill(s, b, (uint32_t)(last - b + 1))) != 0)
                break;
        }
        size_t avail = s->block_size - in;
        if (avail > len - done)
            avail = len - done;
        memcpy(p + done, s->cache + (size_t)slot * s->block_size + in, avail);
        done += avail;
    }
    pthread_mutex_unlock(&s->lock);
    return rc ? rc : (long)done;
}

/* ── Opening ─────────────────────────────────────────────────────────────── */

static int open_compressed(xiso_source *s)
{
    uint8_t h[32];
    long n = raw_pread(s, h, sizeof h, 0);
    if (n < 0)
        return (int)n;
    if (n < 24)
        return 0;                                        /* too short for any header: plain */
    uint64_t index_off;
    if (!memcmp(h, "CISO", 4) || !memcmp(h, "ZISO", 4)) {
        uint32_t hs = rd32(h + 4);
        s->kind = h[0] == 'C' ? XISO_KIND_CSO : XISO_KIND_ZSO;
        s->size = rd64(h + 8);
        s->block_size = rd32(h + 16);
        s->version = h[20];
        s->align = h[21];
        index_off = hs >= 24 && hs <= 4096 ? hs : 24;
    } else if (!memcmp(h, "CCIM", 4) && n >= 32) {
        s->kind = XISO_KIND_CCI;
        s->size = rd64(h + 8);
        index_off = rd64(h + 16);
        s->block_size = rd32(h + 24);
        s->version = h[28];
        s->align = h[29];
    } else {
        return 0;                                        /* plain */
    }
    if (s->block_size < MIN_BLOCK || s->block_size > MAX_BLOCK || s->size == 0 || s->align > 16)
        return XISO_ECORRUPT;
    uint64_t nb = (s->size + s->block_size - 1) / s->block_size;
    if (nb >= 0x7FFFFFFFull)
        return XISO_ECORRUPT;
    s->nblocks = (uint32_t)nb;
    size_t ibytes = ((size_t)nb + 1) * 4;
    uint8_t *raw = malloc(ibytes);
    s->index = malloc(ibytes);
    if (!raw || !s->index) {
        free(raw);
        return XISO_ENOMEM;
    }
    n = raw_pread(s, raw, ibytes, index_off);
    if (n < 0 || (size_t)n != ibytes) {
        free(raw);
        return n < 0 ? (int)n : XISO_ECORRUPT;
    }
    for (size_t i = 0; i <= nb; i++)
        s->index[i] = rd32(raw + 4 * i);
    free(raw);
    /* The positions only grow, and the last one is inside the stored bytes. */
    for (uint32_t i = 0; i < s->nblocks; i++)
        if (block_pos(s, i + 1) < block_pos(s, i))
            return XISO_ECORRUPT;
    if (s->starts[s->nparts] != UINT64_MAX && block_pos(s, s->nblocks) > s->starts[s->nparts])
        return XISO_ECORRUPT;

    s->slots = CACHE_BYTES / s->block_size;
    if (s->slots < 8)
        s->slots = 8;
    s->slot_block = malloc(s->slots * sizeof *s->slot_block);
    s->cache = malloc((size_t)s->slots * s->block_size);
    s->batch = malloc(BATCH_BYTES + 2u * s->block_size + 64);
    if (!s->slot_block || !s->cache || !s->batch)
        return XISO_ENOMEM;
    memset(s->slot_block, 0xFF, s->slots * sizeof *s->slot_block);
    if (pthread_mutex_init(&s->lock, NULL) != 0)
        return XISO_ENOMEM;
    s->lock_ok = 1;
    return 0;
}

void xiso_source_close(xiso_source *s)
{
    if (!s)
        return;
    if (s->own)
        for (int i = 0; i < s->nparts; i++)
            if (s->fds[i] >= 0)
                close(s->fds[i]);
    if (s->lock_ok)
        pthread_mutex_destroy(&s->lock);
    free(s->index);
    free(s->slot_block);
    free(s->cache);
    free(s->batch);
    free(s);
}

int xiso_source_open_fds(const int *fds, int n, int own, xiso_source **out)
{
    if (!out)
        return XISO_EINVAL;
    *out = NULL;
    if (!fds || n < 1 || n > XISO_SOURCE_MAX_PARTS)
        return XISO_EINVAL;
    for (int i = 0; i < n; i++)
        if (fds[i] < 0)
            return XISO_EINVAL;
    xiso_source *s = calloc(1, sizeof *s);
    if (!s)
        return XISO_ENOMEM;
    s->nparts = n;
    s->own = 0;                                          /* until success: a failure leaves the fds to the caller */
    s->kind = XISO_KIND_PLAIN;
    for (int i = 0; i < n; i++) {
        struct stat st;
        s->fds[i] = fds[i];
        uint64_t size = UINT64_MAX;
        if (fstat(fds[i], &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0) {
            size = (uint64_t)st.st_size;
        } else {
            off_t end = lseek(fds[i], 0, SEEK_END);      /* a pipe says ESPIPE: size unknown */
            if (end > 0)
                size = (uint64_t)end;
        }
        if (size == UINT64_MAX && i + 1 < n) {
            xiso_source_close(s);
            return XISO_EINVAL;                          /* only the last part may be of unknown size */
        }
        s->starts[i + 1] = size == UINT64_MAX ? UINT64_MAX : s->starts[i] + size;
    }
    int rc = open_compressed(s);
    if (rc) {
        xiso_source_close(s);
        return rc;
    }
    if (s->kind == XISO_KIND_PLAIN)
        s->size = s->starts[n];
    s->own = own;
    *out = s;
    return 0;
}

/* ── Split names ─────────────────────────────────────────────────────────── */

static const char *const EXTS[] = { "iso", "xiso", "cso", "ciso", "zso", "cci" };

static int is_ext(const char *s, size_t len)
{
    for (size_t i = 0; i < sizeof EXTS / sizeof EXTS[0]; i++)
        if (strlen(EXTS[i]) == len && strncasecmp(s, EXTS[i], len) == 0)
            return 1;
    return 0;
}

static int all_digits(const char *s, size_t len)
{
    if (len == 0 || len > 4)
        return 0;
    for (size_t i = 0; i < len; i++)
        if (s[i] < '0' || s[i] > '9')
            return 0;
    return 1;
}

/* Finds the part number in path's last component: the digits of NAME.<n>.EXT, NAME.part<n>.EXT or NAME.EXT.<n>.
 * Sets *at and *len (the digits) and returns 1, or returns 0. */
static int split_digits(const char *path, size_t *at, size_t *len)
{
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    size_t nl = strlen(name);
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name)
        return 0;
    const char *last = dot + 1;
    size_t ll = nl - (size_t)(last - name);
    if (all_digits(last, ll)) {                          /* NAME.EXT.<n> */
        const char *d2 = dot - 1;
        while (d2 > name && *d2 != '.')
            d2--;
        if (*d2 == '.' && is_ext(d2 + 1, (size_t)(dot - d2 - 1))) {
            *at = (size_t)(last - path);
            *len = ll;
            return 1;
        }
        return 0;
    }
    if (!is_ext(last, ll))
        return 0;
    const char *d2 = dot - 1;                            /* the token before .EXT */
    while (d2 > name && *d2 != '.')
        d2--;
    if (*d2 != '.' || d2 == name)
        return 0;
    const char *tok = d2 + 1;
    size_t tl = (size_t)(dot - tok);
    if (tl > 4 && strncasecmp(tok, "part", 4) == 0 && all_digits(tok + 4, tl - 4)) {
        *at = (size_t)(tok + 4 - path);
        *len = tl - 4;
        return 1;
    }
    if (all_digits(tok, tl)) {
        *at = (size_t)(tok - path);
        *len = tl;
        return 1;
    }
    return 0;
}

static int is_file(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

int xiso_split_parts(const char *path, char (*parts)[1024], int max)
{
    size_t at, len, pl;
    if (!path || !parts || max < 1)
        return XISO_EINVAL;
    pl = strlen(path);
    if (pl >= 1024)
        return XISO_EINVAL;
    if (!is_file(path))
        return 0;
    memcpy(parts[0], path, pl + 1);
    if (!split_digits(path, &at, &len))
        return 1;
    int width = path[at] == '0' && len > 1 ? (int)len : 1;
    long given = strtol(path + at, NULL, 10);
    long first = given == 0 ? 0 : 1;
    int count = 0;
    char cand[1024];
    for (long k = first; count < max && k < first + XISO_SOURCE_MAX_PARTS; k++) {
        int w = snprintf(cand, sizeof cand, "%.*s%0*ld%s", (int)at, path, width, k, path + at + len);
        if (w < 0 || (size_t)w >= sizeof cand)
            return XISO_EINVAL;
        if (!is_file(cand))
            break;
        memcpy(parts[count++], cand, (size_t)w + 1);
    }
    /* The named part must be one of them, and one part alone is no split. */
    int has = 0;
    for (int i = 0; i < count; i++) {
        struct stat a, b;
        if (stat(parts[i], &a) == 0 && stat(path, &b) == 0 && a.st_dev == b.st_dev && a.st_ino == b.st_ino)
            has = 1;
    }
    if (count < 2 || !has) {
        memcpy(parts[0], path, pl + 1);
        return 1;
    }
    return count;
}

int xiso_source_open_path(const char *path, xiso_source **out)
{
    char (*parts)[1024];
    int fds[XISO_SOURCE_MAX_PARTS];
    if (!out)
        return XISO_EINVAL;
    *out = NULL;
    if (!path)
        return XISO_EINVAL;
    parts = malloc(sizeof(*parts) * XISO_SOURCE_MAX_PARTS);
    if (!parts)
        return XISO_ENOMEM;
    int n = xiso_split_parts(path, parts, XISO_SOURCE_MAX_PARTS);
    if (n <= 0) {
        free(parts);
        return n == 0 ? XISO_EIO : n;
    }
    for (int i = 0; i < n; i++) {
        fds[i] = open(parts[i], O_RDONLY | O_CLOEXEC);
        if (fds[i] < 0) {
            while (i-- > 0)
                close(fds[i]);
            free(parts);
            return XISO_EIO;
        }
    }
    free(parts);
    int rc = xiso_source_open_fds(fds, n, 1, out);
    if (rc)
        for (int i = 0; i < n; i++)
            close(fds[i]);
    return rc;
}

/* ── Reading ─────────────────────────────────────────────────────────────── */

long xiso_source_pread(xiso_source *s, void *buf, size_t len, uint64_t off)
{
    if (!s || (!buf && len))
        return XISO_EINVAL;
    if (len > 0x7FFFFFFFu)
        len = 0x7FFFFFFFu;
    if (s->kind == XISO_KIND_PLAIN)
        return raw_pread(s, buf, len, off);
    return compressed_pread(s, buf, len, off);
}

uint64_t xiso_source_size(const xiso_source *s) { return s ? s->size : 0; }
uint64_t xiso_source_stored_size(const xiso_source *s) { return s ? s->starts[s->nparts] : 0; }
int xiso_source_parts(const xiso_source *s) { return s ? s->nparts : 0; }

int xiso_source_kind(const xiso_source *s)
{
    if (!s)
        return 0;
    return s->kind | (s->nparts > 1 ? XISO_KIND_SPLIT : 0);
}

const char *xiso_source_kind_name(int kind)
{
    int split = (kind & XISO_KIND_SPLIT) != 0;
    switch (kind & 0xFF) {
    case XISO_KIND_PLAIN: return split ? "iso split" : "iso";
    case XISO_KIND_CSO:   return split ? "cso split" : "cso";
    case XISO_KIND_ZSO:   return split ? "zso split" : "zso";
    case XISO_KIND_CCI:   return split ? "cci split" : "cci";
    default:              return "?";
    }
}
