/*
 * xbox_mods.c - what the player's enabled mods ask of the Xbox runtime (xbox_mods.h, docs/technical/mods.md)
 */
#include "xbox_mods.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern void xbox_FramebufferWindowSetWide(int on);

#define XBE_BASE 0x00010000u
#define HOST(va) ((uint8_t *)(uintptr_t)(xbox_GetMemoryOffset() + (ptrdiff_t)(va)))

int xbox_ModsFramesWide(const char *frames, const char *aspect, int android)
{
    if (!frames || strcmp(frames, "anamorphic-16:9") != 0)
        return 0;
    /* RECOMP_ASPECT as src/video/aspect_policy.c aspect_parse_setting reads it. */
    if (aspect && (!strcmp(aspect, "4:3") || !strcmp(aspect, "4/3") || !strcmp(aspect, "43")))
        return 0;
    if (aspect && (!strcmp(aspect, "16:9") || !strcmp(aspect, "16/9") || !strcmp(aspect, "169")))
        return 1;
    return android != 0;
}

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10
         : -1;
}

int xbox_ModsParseWrites(const char *s, XboxModWrite *w, int max)
{
    int count = 0;

    if (!s || !*s)
        return 0;
    while (*s) {
        uint64_t va = 0;
        int digits = 0;
        XboxModWrite cur;

        if (count >= max)
            return -1;
        memset(&cur, 0, sizeof cur);
        while (hexval(*s) >= 0) {
            va = va << 4 | (uint64_t)hexval(*s++);
            if (++digits > 8)
                return -1;
        }
        if (!digits || *s++ != '=')
            return -1;
        while (hexval(s[0]) >= 0) {
            if (hexval(s[1]) < 0 || cur.n >= XBOX_MODS_WRITE_BYTES)
                return -1;
            cur.bytes[cur.n++] = (uint8_t)(hexval(s[0]) << 4 | hexval(s[1]));
            s += 2;
        }
        if (!cur.n || va + cur.n > 0x100000000ull)
            return -1;
        if (*s == ';')
            s++;
        else if (*s)
            return -1;
        cur.va = (uint32_t)va;
        w[count++] = cur;
    }
    return count;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* The certificate's title ID, or 0 when b is not an XBE. */
static uint32_t xbe_title(const uint8_t *b, size_t n)
{
    uint32_t base, cert;

    if (n < 0x120 || memcmp(b, "XBEH", 4) != 0)
        return 0;
    base = rd32(b + 0x104);
    cert = rd32(b + 0x118);
    if (cert < base || (size_t)(cert - base) + 12 > n)
        return 0;
    return rd32(b + (cert - base) + 8);
}

const uint8_t *xbox_ModsXbe(const uint8_t *xbe, size_t *n)
{
    static uint8_t *patched;
    static size_t patched_n;
    const char *path = getenv("RECOMP_MOD_XBE");
    FILE *f;
    long len;
    uint8_t *buf;

    if (!path || !*path || !xbe || !n)
        return xbe;
    if (patched) {
        *n = patched_n;
        return patched;
    }
    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[MODS] RECOMP_MOD_XBE %s cannot be opened: the disc's XBE is loaded, the mods' data "
                        "changes are missing (prepare again)\n", path);
        return xbe;
    }
    buf = NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (len = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0
        && (buf = (uint8_t *)malloc((size_t)len)) != NULL && fread(buf, 1, (size_t)len, f) == (size_t)len) {
        uint32_t want = xbe_title(xbe, *n), have = xbe_title(buf, (size_t)len);
        if (want && have == want) {
            fclose(f);
            patched = buf;
            patched_n = (size_t)len;
            fprintf(stderr, "[MODS] loading the mods' patched XBE %s (%zu bytes)\n", path, patched_n);
            *n = patched_n;
            return patched;
        }
        fprintf(stderr, "[MODS] RECOMP_MOD_XBE %s is not an XBE of title %08X (%08X): the disc's XBE is loaded\n",
                path, want, have);
    } else {
        fprintf(stderr, "[MODS] RECOMP_MOD_XBE %s cannot be read: the disc's XBE is loaded\n", path);
    }
    free(buf);
    fclose(f);
    return xbe;
}

int xbox_ModsAfterLoad(void)
{
    const char *frames = getenv("RECOMP_MOD_FRAMES");
    XboxModWrite w[XBOX_MODS_WRITES_MAX];
    int nw, i, wide;
    uint32_t image_end;
#ifdef __ANDROID__
    const int android = 1;
#else
    const int android = 0;
#endif

    if (!frames || !*frames)
        return 0;
    wide = xbox_ModsFramesWide(frames, getenv("RECOMP_ASPECT"), android);
    fprintf(stderr, "[MODS] frames %s: %s (RECOMP_ASPECT %s)\n", frames,
            wide ? "presented 16:9" : "presented 4:3", getenv("RECOMP_ASPECT") ? getenv("RECOMP_ASPECT") : "auto");
    if (!wide)
        return 0;
    image_end = XBE_BASE + rd32(HOST(XBE_BASE + 0x10C));         /* the loaded header's SizeOfImage */
    nw = xbox_ModsParseWrites(getenv("RECOMP_MOD_WIDE_WRITES"), w, XBOX_MODS_WRITES_MAX);
    if (nw < 0) {
        fprintf(stderr, "[MODS] RECOMP_MOD_WIDE_WRITES is malformed: no write made\n");
        nw = 0;
    }
    for (i = 0; i < nw; i++) {
        if (w[i].va < XBE_BASE || (uint64_t)w[i].va + w[i].n > image_end) {
            fprintf(stderr, "[MODS] wide write at %08X (%u bytes) is outside the image (%08X-%08X): skipped\n",
                    w[i].va, w[i].n, XBE_BASE, image_end);
            continue;
        }
        memcpy(HOST(w[i].va), w[i].bytes, w[i].n);
        fprintf(stderr, "[MODS] wide write: %u bytes at %08X\n", w[i].n, w[i].va);
    }
    xbox_FramebufferWindowSetWide(1);
    return 1;
}
