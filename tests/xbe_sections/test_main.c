/* XBE sections loaded on demand (XBA-124, src/kernel/xbox_xbe_sections.c).
 *
 * Built twice. xbe_sections_unit: the module alone over a 64 MB stand-in for
 * guest memory, which the test fills as xbox_MemoryLayoutInit does (header,
 * then every section in table order). xbe_sections_layout (TEST_LAYOUT):
 * the whole runtime, xbox_MemoryLayoutInit itself, on the same synthetic
 * image and on odd ones (no certificate, a certificate address that would
 * wrap, a section table cut by the end of the file).
 *
 * The image, our own: .text (preload, code), DATA (writable, loaded on
 * demand: 16 bytes of data then BSS), OVA (preload, writable) and OVB (on
 * demand) sharing 0x15400-0x15800, RO (preload, read-only) and PDATA
 * (preload, writable) sharing the page at 0x16000 and its reference count. */
#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xbox_xbe_sections.h"

#ifdef _WIN32
int main(void) { puts("skipped: POSIX mapping"); return 77; }
#else
#include <sys/mman.h>

#ifdef TEST_LAYOUT
#include "xbox_memory_layout.h"
#define MEM ((uint8_t *)xbox_GetMemoryOffset())
/* Supplied by the recompiled title in a real build; never called here. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(unsigned int va) { (void)va; return 0; }
recomp_func_t recomp_lookup_manual(unsigned int va) { (void)va; return 0; }
#else
static uint8_t *g_mem;
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)g_mem; }
#define MEM g_mem
#endif

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } \
                      else printf("ok   %s\n", #c); } while (0)

#define BASE      0x00010000u
#define HDR       0x1000u
#define CERT      0x180u
#define SECT      0x380u
#define NAMES     0x600u
#define REFS      0x700u
#define NSEC      6

enum { TEXT, DATA, OVA, OVB, RO, PDATA };
static const struct { const char *name; uint32_t va, vsize, raw_size, flags; uint8_t fill; } k_secs[NSEC] = {
    { ".text", 0x11000, 0x100,  0x100, 0x6, 0xC3 },   /* preload | executable */
    { "DATA",  0x12000, 0x2000, 0x10,  0x1, 0x00 },   /* writable, on demand; data below */
    { "OVA",   0x15000, 0x800,  0x800, 0x3, 0xAA },   /* preload | writable */
    { "OVB",   0x15400, 0x800,  0x800, 0x1, 0xBB },   /* writable, on demand: shares 0x15400-0x15800 */
    { "RO",    0x16000, 0x100,  0x100, 0x2, 0x11 },   /* preload, read-only */
    { "PDATA", 0x16100, 0x100,  0x10,  0x3, 0x50 },   /* preload | writable, same page as RO */
};
static const char k_data[16] = "DATAINIT-123456";

#define HDR_VA(i)   (BASE + SECT + (i) * 56u)
#define REF_VA(pg)  (BASE + REFS + 2u * (pg))     /* one u16 per page index below */

static uint32_t page_index(uint32_t va)
{
    static const uint32_t pages[] = { 0x11000, 0x12000, 0x13000, 0x15000, 0x16000 };
    for (unsigned i = 0; i < sizeof pages / sizeof pages[0]; i++)
        if ((va & ~0xFFFu) == pages[i])
            return i;
    return 7;
}

static void put32(uint8_t *b, uint32_t off, uint32_t v) { memcpy(b + off, &v, 4); }

/* The image: header page, then each section's raw data. cert_va 0: none. */
static uint8_t *build(size_t *len, uint32_t cert_va)
{
    size_t raw = HDR, total = HDR;
    for (int i = 0; i < NSEC; i++)
        total += (k_secs[i].raw_size + 15u) & ~15u;
    uint8_t *b = calloc(1, total);
    memcpy(b, "XBEH", 4);
    put32(b, 0x104, BASE);
    put32(b, 0x108, HDR);
    put32(b, 0x10C, 0x7000);                        /* image 0x10000-0x17000 */
    put32(b, 0x118, cert_va);
    put32(b, 0x11C, NSEC);
    put32(b, 0x120, BASE + SECT);
    put32(b, 0x128, 0x11000u ^ 0xA8FC57ABu);
    put32(b, 0x158, (BASE + 0x7F0u) ^ 0x5B6D40B6u); /* empty thunk table at 0x7F0 */
    if (cert_va == BASE + CERT) {
        put32(b, CERT + 8, 0x58424124u);
        memcpy(b + CERT + 12, "S\0e\0c\0", 6);
    }
    for (int i = 0; i < NSEC; i++) {
        uint32_t o = SECT + i * 56u;
        put32(b, o + 0x00, k_secs[i].flags);
        put32(b, o + 0x04, k_secs[i].va);
        put32(b, o + 0x08, k_secs[i].vsize);
        put32(b, o + 0x0C, (uint32_t)raw);
        put32(b, o + 0x10, k_secs[i].raw_size);
        put32(b, o + 0x14, BASE + NAMES + 16u * i);
        put32(b, o + 0x1C, REF_VA(page_index(k_secs[i].va)));
        put32(b, o + 0x20, REF_VA(page_index(k_secs[i].va + k_secs[i].vsize - 1)));
        strcpy((char *)b + NAMES + 16u * i, k_secs[i].name);
        if (i == DATA)
            memcpy(b + raw, k_data, 16);
        else
            memset(b + raw, k_secs[i].fill, k_secs[i].raw_size);
        raw += (k_secs[i].raw_size + 15u) & ~15u;
    }
    *len = total;
    return b;
}

#define M8(va)   (MEM[(uint32_t)(va)])
static uint32_t M32(uint32_t va) { uint32_t v; memcpy(&v, MEM + va, 4); return v; }
static uint16_t M16(uint32_t va) { uint16_t v; memcpy(&v, MEM + va, 2); return v; }
#define REFCOUNT(i) M32(HDR_VA(i) + 0x18)

#ifndef TEST_LAYOUT
/* What xbox_MemoryLayoutInit does with the image before it calls the module. */
static void layout(const uint8_t *b, size_t len)
{
    memset(MEM, 0, 0x100000);
    memcpy(MEM + BASE, b, HDR);
    for (int i = 0; i < NSEC; i++) {
        uint32_t raw;
        memcpy(&raw, b + SECT + i * 56u + 0x0C, 4);
        memset(MEM + k_secs[i].va, 0, k_secs[i].vsize);
        memcpy(MEM + k_secs[i].va, b + raw, k_secs[i].raw_size < k_secs[i].vsize ? k_secs[i].raw_size
                                                                                 : k_secs[i].vsize);
    }
    xbox_XbeSectionsInit(b, len);
}
#endif

static int all(uint32_t va, uint32_t n, uint8_t v)
{
    for (uint32_t i = 0; i < n; i++)
        if (M8(va + i) != v)
            return 0;
    return 1;
}

static void on_demand_checks(void)
{
    /* Boot: PRELOAD sections count 1, the others 0; their shared pages counted. */
    CHECK(xbox_XbeSectionCount() == NSEC && xbox_XbeSectionOverlaps() == 1);
    CHECK(REFCOUNT(TEXT) == 1 && REFCOUNT(DATA) == 0 && REFCOUNT(OVA) == 1 && REFCOUNT(OVB) == 0);
    CHECK(REFCOUNT(RO) == 1 && REFCOUNT(PDATA) == 1);
    CHECK(M16(REF_VA(page_index(0x11000))) == 2);           /* .text: head and tail page */
    CHECK(M16(REF_VA(page_index(0x16000))) == 4);           /* RO and PDATA share it */
    CHECK(M16(REF_VA(page_index(0x12000))) == 0);           /* DATA not loaded */
    /* The overlap: OVA (preload) is in memory, though OVB came later in the table. */
    CHECK(all(0x15400, 0x400, 0xAA) && all(0x15000, 0x400, 0xAA));
    CHECK(all(0x15800, 0x400, 0xBB));                       /* OVB's own tail, as the layout left it */

    /* DATA: 0 -> 1 copies it again; a reload after a full unload gives fresh
     * data and zeroed BSS; 1 -> 2 copies nothing. */
    M8(0x12000) = 'X';
    CHECK(xbox_XbeSectionLoad(HDR_VA(DATA)) == 0 && REFCOUNT(DATA) == 1);
    CHECK(memcmp(MEM + 0x12000, k_data, 16) == 0);
    CHECK(M16(REF_VA(page_index(0x12000))) == 1 && M16(REF_VA(page_index(0x13000))) == 1);
    M8(0x12001) = 'Y';
    M8(0x13000) = 0x77;                                     /* BSS */
    CHECK(xbox_XbeSectionLoad(HDR_VA(DATA)) == 0 && REFCOUNT(DATA) == 2 && M8(0x12001) == 'Y');
    CHECK(xbox_XbeSectionUnload(HDR_VA(DATA)) == 0 && REFCOUNT(DATA) == 1 && M8(0x12001) == 'Y');
    CHECK(xbox_XbeSectionUnload(HDR_VA(DATA)) == 0 && REFCOUNT(DATA) == 0);
    CHECK(M16(REF_VA(page_index(0x12000))) == 0);
    CHECK(xbox_XbeSectionUnload(HDR_VA(DATA)) == 0 && REFCOUNT(DATA) == 0);   /* never below 0 */
    CHECK(xbox_XbeSectionLoad(HDR_VA(DATA)) == 0);
    CHECK(memcmp(MEM + 0x12000, k_data, 16) == 0 && M8(0x13000) == 0);

    /* The alternatives: OVA out, OVB in, and back. */
    M8(0x15010) = 0x01;                                     /* OVA's data, changed by the title */
    CHECK(xbox_XbeSectionUnload(HDR_VA(OVA)) == 0 && REFCOUNT(OVA) == 0);
    CHECK(xbox_XbeSectionLoad(HDR_VA(OVB)) == 0 && REFCOUNT(OVB) == 1);
    CHECK(all(0x15400, 0x800, 0xBB));
    CHECK(M8(0x15010) == 0x01);                             /* outside OVB: untouched */
    CHECK(xbox_XbeSectionUnload(HDR_VA(OVB)) == 0);
    CHECK(xbox_XbeSectionLoad(HDR_VA(OVA)) == 0);
    CHECK(all(0x15000, 0x800, 0xAA));                       /* OVA again, as in the image */

    /* Read-only: not copied again (nothing writes it, its pages stay). */
    M8(0x16000) = 0x99;
    CHECK(xbox_XbeSectionUnload(HDR_VA(RO)) == 0 && xbox_XbeSectionLoad(HDR_VA(RO)) == 0);
    CHECK(M8(0x16000) == 0x99);
    M8(0x16000) = 0x11;

    /* A null header; a header that is not the image's: the count only. */
    CHECK(xbox_XbeSectionLoad(0) == 0xC000000Du);
    put32(MEM, 0x30000 + 0x18, 5);
    CHECK(xbox_XbeSectionLoad(0x30000) == 0 && M32(0x30018) == 6);
    CHECK(xbox_XbeSectionUnload(0x30000) == 0 && M32(0x30018) == 5);
}

static void off_checks(void)
{
    /* RECOMP_XBE_SECTIONS=0: the count goes up and down from the file's, and
     * nothing is copied; the layout's table order decides the overlap. */
    CHECK(REFCOUNT(TEXT) == 0 && REFCOUNT(OVA) == 0);
    CHECK(all(0x15400, 0x400, 0xBB));
    M8(0x12000) = 'X';
    CHECK(xbox_XbeSectionLoad(HDR_VA(DATA)) == 0 && REFCOUNT(DATA) == 1 && M8(0x12000) == 'X');
}

int main(void)
{
    size_t len;
    uint8_t *img = build(&len, BASE + CERT);
    setvbuf(stdout, NULL, _IONBF, 0);
#ifndef TEST_LAYOUT
    g_mem = mmap(NULL, 0x04000000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (g_mem == MAP_FAILED) {
        puts("skipped: cannot map 64 MB");
        return 77;
    }
    unsetenv("RECOMP_XBE_SECTIONS");
    layout(img, len);
    on_demand_checks();
    setenv("RECOMP_XBE_SECTIONS", "0", 1);
    layout(img, len);
    off_checks();
    /* Odd tables: none, cut by the end of the file. Nothing recorded, no crash. */
    unsetenv("RECOMP_XBE_SECTIONS");
    xbox_XbeSectionsInit(img, 0x100);
    CHECK(xbox_XbeSectionCount() == 0);
    put32(img, 0x11C, 100000);
    xbox_XbeSectionsInit(img, len);
    CHECK(xbox_XbeSectionCount() == 0);
    put32(img, 0x11C, NSEC);
    put32(img, 0x120, 0x1000);                              /* below the base */
    xbox_XbeSectionsInit(img, len);
    CHECK(xbox_XbeSectionCount() == 0);
    xbox_XbeSectionsShutdown();
#else
    unsetenv("RECOMP_XBE_SECTIONS");
    if (!xbox_MemoryLayoutInit(img, len)) {
        puts("FAIL xbox_MemoryLayoutInit");
        return 1;
    }
    free(img);              /* the module keeps its own copies */
    img = NULL;
    on_demand_checks();
    xbox_MemoryLayoutShutdown();
    setenv("RECOMP_XBE_SECTIONS", "0", 1);
    img = build(&len, BASE + CERT);
    CHECK(xbox_MemoryLayoutInit(img, len));
    off_checks();
    xbox_MemoryLayoutShutdown();
    unsetenv("RECOMP_XBE_SECTIONS");
    /* Odd headers the loader must survive: no certificate, a certificate
     * address whose offset wraps back into the buffer, a section table cut
     * by the end of the file. */
    free(img);
    img = build(&len, 0);
    CHECK(xbox_MemoryLayoutInit(img, len) && REFCOUNT(OVA) == 1);
    xbox_MemoryLayoutShutdown();
    free(img);
    img = build(&len, 0x0000FFF0u);
    CHECK(xbox_MemoryLayoutInit(img, len) && REFCOUNT(TEXT) == 1);
    xbox_MemoryLayoutShutdown();
    put32(img, 0x11C, 0x7FFFFFFFu);
    CHECK(xbox_MemoryLayoutInit(img, len) && xbox_XbeSectionCount() == 0);
    xbox_MemoryLayoutShutdown();
#endif
    free(img);
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    puts("xbe_sections: ok");
    return 0;
}
#endif
