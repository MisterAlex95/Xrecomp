/* gc_png.h -- a W x H RGB PNG writer for the GameCube tools (gc_run, gc_runtime's screenshot; XBA-225 / XBA-259):
 * zlib with stored blocks, no dependency, deterministic bytes. Static functions: include it in one file. */
#ifndef GC_PNG_H
#define GC_PNG_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t crc_table[256];

static void crc_init(void)
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static uint32_t crc32_of(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--)
        crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static void put32(FILE *f, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    fwrite(b, 1, 4, f);
}

static void chunk(FILE *f, const char *type, const uint8_t *data, size_t n)
{
    put32(f, (uint32_t)n);
    fwrite(type, 1, 4, f);
    if (n)
        fwrite(data, 1, n, f);
    uint32_t c = crc32_of(0, (const uint8_t *)type, 4);
    c = crc32_of(c, data, n);
    put32(f, c);
}

/* A W x H RGB PNG, zlib with stored blocks (no compression: no dependency, deterministic bytes). */
static int write_png(const char *path, const uint8_t *rgb, int w, int h)
{
    crc_init();
    size_t row = 1 + (size_t)w * 3, n = row * (size_t)h;
    uint8_t *raw = malloc(n);
    size_t nblk = (n + 65534) / 65535;
    size_t zn = 2 + n + nblk * 5 + 4;
    uint8_t *z = malloc(zn);
    if (!raw || !z) {
        free(raw);
        free(z);
        return -1;
    }
    for (int y = 0; y < h; y++) {
        raw[y * row] = 0;
        memcpy(raw + y * row + 1, rgb + (size_t)y * w * 3, (size_t)w * 3);
    }
    size_t o = 0;
    z[o++] = 0x78;
    z[o++] = 0x01;
    uint32_t s1 = 1, s2 = 0;
    for (size_t i = 0; i < n; i++) {
        s1 = (s1 + raw[i]) % 65521;
        s2 = (s2 + s1) % 65521;
    }
    for (size_t at = 0; at < n; at += 65535) {
        size_t len = n - at < 65535 ? n - at : 65535;
        z[o++] = at + len == n ? 1 : 0;
        z[o++] = (uint8_t)len;
        z[o++] = (uint8_t)(len >> 8);
        z[o++] = (uint8_t)~len;
        z[o++] = (uint8_t)(~len >> 8);
        memcpy(z + o, raw + at, len);
        o += len;
    }
    uint32_t ad = s2 << 16 | s1;
    z[o++] = (uint8_t)(ad >> 24);
    z[o++] = (uint8_t)(ad >> 16);
    z[o++] = (uint8_t)(ad >> 8);
    z[o++] = (uint8_t)ad;
    free(raw);
    FILE *f = fopen(path, "wb");
    if (!f) {
        free(z);
        return -1;
    }
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13] = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                        (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 2, 0, 0, 0};
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, o);
    chunk(f, "IEND", NULL, 0);
    free(z);
    return fclose(f) ? -1 : 0;
}

#endif
