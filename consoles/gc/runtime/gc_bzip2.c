/* gc_bzip2.c -- a bzip2 decoder for WIA / RVZ groups (XBA-261), written from the format: stream header "BZh1".."9",
 * blocks (48-bit magic 0x314159265359, CRC, origPtr, symbol map, 2-6 Huffman tables picked per 50 symbols, MTF and
 * RUNA / RUNB zero runs, the Burrows-Wheeler transform undone, the initial run-length step undone), the end-of-stream
 * magic 0x177245385090 and the combined CRC. Every block's CRC is checked. Randomised blocks (an obsolete encoder
 * option) are refused. One stream, decoded whole into a buffer of known size. */
#include <stdlib.h>
#include <string.h>

#include "gc_disc_internal.h"

typedef struct {
    const uint8_t *p;
    size_t n, bit; /* bit position */
    int bad;
} bits;

static uint32_t get(bits *b, unsigned k)
{
    uint32_t v = 0;
    while (k--) {
        if (b->bit >= b->n * 8) {
            b->bad = 1;
            return 0;
        }
        v = v << 1 | ((b->p[b->bit >> 3] >> (7 - (b->bit & 7))) & 1);
        b->bit++;
    }
    return v;
}

static uint32_t crc_table[256];

static void crc_init(void)
{
    if (crc_table[1])
        return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i << 24;
        for (int k = 0; k < 8; k++)
            c = (c & 0x80000000u) ? c << 1 ^ 0x04C11DB7u : c << 1;
        crc_table[i] = c;
    }
}

#define MAX_ALPHA 258
#define MAX_LEN 20
#define MAX_GROUPS 6
#define MAX_SELECTORS 32768

typedef struct {
    int min, max;
    int count[MAX_LEN + 1];
    int first_code[MAX_LEN + 1], first_idx[MAX_LEN + 1];
    uint16_t perm[MAX_ALPHA];
} huff;

static void huff_build(huff *h, const uint8_t *len, int alpha)
{
    memset(h, 0, sizeof *h);
    h->min = MAX_LEN;
    h->max = 0;
    for (int i = 0; i < alpha; i++) {
        h->count[len[i]]++;
        if (len[i] < h->min)
            h->min = len[i];
        if (len[i] > h->max)
            h->max = len[i];
    }
    int code = 0, idx = 0;
    for (int l = 1; l <= MAX_LEN; l++) {
        h->first_code[l] = code;
        h->first_idx[l] = idx;
        for (int i = 0; i < alpha; i++)
            if (len[i] == l)
                h->perm[idx++] = (uint16_t)i;
        code = (code + h->count[l]) << 1;
    }
}

static int huff_sym(bits *b, const huff *h)
{
    int c = 0;
    for (int l = 1; l <= h->max; l++) {
        c = c << 1 | (int)get(b, 1);
        if (b->bad)
            return -1;
        int k = c - h->first_code[l];
        if (l >= h->min && k >= 0 && k < h->count[l])
            return h->perm[h->first_idx[l] + k];
    }
    return -1;
}

int gc_bzip2_decompress(const uint8_t *src, size_t src_len, uint8_t *out, size_t out_len)
{
    bits b = {src, src_len, 0, 0};
    size_t o = 0;
    int rc = -1;
    uint32_t combined = 0;
    crc_init();
    if (src_len < 4 || memcmp(src, "BZh", 3) || src[3] < '1' || src[3] > '9')
        return -1;
    uint32_t max_block = (uint32_t)(src[3] - '0') * 100000u;
    b.bit = 32;
    uint8_t *ll = malloc(max_block);
    uint32_t *tt = malloc((size_t)max_block * 4);
    uint8_t *sel = malloc(MAX_SELECTORS);
    huff *hf = malloc(sizeof(huff) * MAX_GROUPS);
    if (!ll || !tt || !sel || !hf)
        goto done;
    for (;;) {
        uint32_t m_hi = get(&b, 24), m_lo = get(&b, 24);
        if (b.bad)
            goto done;
        if (m_hi == 0x177245 && m_lo == 0x385090) {
            uint32_t want = get(&b, 32);
            if (!b.bad && want == combined && o == out_len)
                rc = 0;
            goto done;
        }
        if (m_hi != 0x314159 || m_lo != 0x265359)
            goto done;
        uint32_t block_crc = get(&b, 32);
        if (get(&b, 1)) /* randomised */
            goto done;
        uint32_t orig = get(&b, 24);
        /* the symbols in use */
        uint8_t seq[256];
        int n_in_use = 0;
        uint32_t used16 = get(&b, 16);
        for (int i = 0; i < 16; i++)
            if (used16 & (0x8000u >> i)) {
                uint32_t u = get(&b, 16);
                for (int j = 0; j < 16; j++)
                    if (u & (0x8000u >> j))
                        seq[n_in_use++] = (uint8_t)(i * 16 + j);
            }
        if (b.bad || !n_in_use)
            goto done;
        int alpha = n_in_use + 2;
        int n_groups = (int)get(&b, 3);
        int n_sel = (int)get(&b, 15);
        if (b.bad || n_groups < 2 || n_groups > MAX_GROUPS || n_sel < 1)
            goto done;
        uint8_t mtf_g[MAX_GROUPS];
        for (int i = 0; i < n_groups; i++)
            mtf_g[i] = (uint8_t)i;
        for (int i = 0; i < n_sel; i++) {
            int j = 0;
            while (get(&b, 1)) {
                if (++j >= n_groups || b.bad)
                    goto done;
            }
            uint8_t v = mtf_g[j];
            memmove(mtf_g + 1, mtf_g, (size_t)j);
            mtf_g[0] = v;
            sel[i] = v;
        }
        for (int t = 0; t < n_groups; t++) {
            uint8_t len[MAX_ALPHA];
            int cur = (int)get(&b, 5);
            for (int i = 0; i < alpha; i++) {
                for (;;) {
                    if (cur < 1 || cur > MAX_LEN || b.bad)
                        goto done;
                    if (!get(&b, 1))
                        break;
                    cur += get(&b, 1) ? -1 : 1;
                }
                len[i] = (uint8_t)cur;
            }
            huff_build(&hf[t], len, alpha);
        }
        /* symbols -> the BWT's last column */
        uint8_t mtf[256];
        for (int i = 0; i < 256; i++)
            mtf[i] = (uint8_t)i;
        uint32_t n = 0, run = 0, run_bit = 1;
        int gsel = 0, left = 0;
        const huff *h = NULL;
        for (;;) {
            if (!left) {
                if (gsel >= n_sel)
                    goto done;
                h = &hf[sel[gsel++]];
                left = 50;
            }
            left--;
            int s = huff_sym(&b, h);
            if (s < 0)
                goto done;
            if (s <= 1) { /* RUNA / RUNB */
                run += run_bit << s;
                run_bit <<= 1;
                if (run > max_block)
                    goto done;
                continue;
            }
            if (run) {
                if (n + run > max_block)
                    goto done;
                memset(ll + n, seq[mtf[0]], run);
                n += run;
                run = 0;
            }
            run_bit = 1;
            if (s == alpha - 1)
                break;
            int k = s - 1;
            uint8_t v = mtf[k];
            memmove(mtf + 1, mtf, (size_t)k);
            mtf[0] = v;
            if (n >= max_block)
                goto done;
            ll[n++] = seq[v];
        }
        if (orig >= n)
            goto done;
        /* undo the BWT: tt = LF^-1, then walk from origPtr */
        uint32_t cum[256] = {0}, sum = 0;
        for (uint32_t i = 0; i < n; i++)
            cum[ll[i]]++;
        for (int i = 0; i < 256; i++) {
            uint32_t c = cum[i];
            cum[i] = sum;
            sum += c;
        }
        for (uint32_t i = 0; i < n; i++)
            tt[cum[ll[i]]++] = i;
        /* undo the initial run-length step (4 equal bytes, then a repeat count) while writing out */
        uint32_t crc = 0xFFFFFFFFu, pos = tt[orig];
        int last = -1, same = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint8_t c = ll[pos];
            pos = tt[pos];
            if (same == 4) {
                if (o + c > out_len)
                    goto done;
                for (unsigned r = 0; r < c; r++) {
                    out[o++] = (uint8_t)last;
                    crc = crc << 8 ^ crc_table[(crc >> 24) ^ (uint8_t)last];
                }
                same = 0;
                last = -1;
                continue;
            }
            if (c == last) {
                same++;
            } else {
                same = 1;
                last = c;
            }
            if (o >= out_len)
                goto done;
            out[o++] = c;
            crc = crc << 8 ^ crc_table[(crc >> 24) ^ c];
        }
        crc = ~crc;
        if (crc != block_crc)
            goto done;
        combined = (combined << 1 | combined >> 31) ^ crc;
    }
done:
    free(ll);
    free(tt);
    free(sel);
    free(hf);
    return rc;
}
