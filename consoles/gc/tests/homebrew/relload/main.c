/* relload: REL modules loaded at run time, the way a commercial game's SDK does it (XBA-264). Our own code only: a
 * small OSLink, not Nintendo's.
 *
 *   1. finds rel/a.rel and rel/b.rel in the disc's FST (the apploader left it at *(u32 *)0x80000038) and reads each
 *      through the DVD interface's DMA read command (no libogc DVD layer)
 *   2. links a module the way the SDK's OSLink does: its sections stay where they are in the buffer, the bss is
 *      allocated apart, the relocations against the DOL (module 0) and against itself are applied, the section table
 *      becomes addresses, the module goes on the list at 0x800030C8 (head) / 0x800030CC (tail), the data cache is
 *      flushed and the instruction cache invalidated over it; then its prolog runs
 *   3. module A runs (it calls back into the DOL, reads its own data and a pointer to its own function), is unlinked,
 *      then module B is read into the same buffer and linked at the same address (stage modules loaded in turn),
 *      runs, and stays loaded; every field after that calls B's function again (so a recompiled pack runs it often)
 *
 * The modules (relload/mod/a.c, b.c) are built as relocatable objects and turned into REL files by
 * consoles/gc/tools/gc_tools/elf2rel.py against this DOL's symbols; consoles/gc/tests/run_relload.py builds the disc.
 * Results go into `report` (magic "XBARELD1") and on the screen. */
#include <gccore.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* What the modules call and touch in the DOL (module 0): their imports name these. */
int rel_svc_mul3(int x);
int rel_shared_counter;

__attribute__((noinline)) int rel_svc_mul3(int x)
{
    return x * 3 + 1;
}

/* Referenced, so the linker's section garbage collection keeps them for the modules. */
void *volatile rel_exports[2] = {(void *)rel_svc_mul3, &rel_shared_counter};

struct report {
    char magic[8] __attribute__((nonstring));
    uint32_t done;
    uint32_t base;          /* where both modules were linked */
    int32_t a_result, b_result;
    uint32_t counter;       /* rel_shared_counter after A and B */
    uint32_t b_calls;       /* B's function, called every field */
    int32_t b_sum;          /* the sum of its results */
    int32_t err;            /* 0, else a step number that failed */
};
volatile struct report report __attribute__((aligned(32))) = {.magic = "XBARELD1"};

#define DI ((volatile uint32_t *)0xCC006000)

/* A DVD read of len bytes at disc offset off into dst (32-byte aligned, len a multiple of 32). */
static int dvd_read(void *dst, uint32_t off, uint32_t len)
{
    DCInvalidateRange(dst, len);
    DI[0] = 0x2E;                      /* clear TC / DE / BRK interrupts, masks off */
    DI[2] = 0xA8000000;
    DI[3] = off >> 2;
    DI[4] = len;
    DI[5] = (uint32_t)MEM_VIRTUAL_TO_PHYSICAL(dst);
    DI[6] = len;
    DI[7] = 3;                         /* DMA, start */
    for (int spins = 0; DI[7] & 1; spins++)
        if (spins > 50000000)
            return -1;
    DCInvalidateRange(dst, len);
    return (DI[0] & 0x04) ? -1 : 0;    /* DEINT: an error */
}

/* The FST: entry i at fst + 12 * i; names after the last entry. */
static int fst_find(const char *path, uint32_t *off, uint32_t *size)
{
    const uint8_t *fst = *(const uint8_t *const *)0x80000038;
    if (!fst)
        return -1;
    uint32_t n = ((const uint32_t *)fst)[2];
    const char *names = (const char *)fst + 12 * n;
    uint32_t parent_end[16], depth = 0;
    char cur[16][64];
    cur[0][0] = 0;
    parent_end[0] = n;
    for (uint32_t i = 1; i < n; i++) {
        while (depth && i >= parent_end[depth])
            depth--;
        const uint32_t *e = (const uint32_t *)(fst + 12 * i);
        const char *nm = names + (e[0] & 0xFFFFFF);
        char full[128];
        snprintf(full, sizeof full, "%.63s%.63s", cur[depth], nm);
        if (e[0] >> 24) {
            if (depth + 1 < 16) {
                depth++;
                snprintf(cur[depth], sizeof cur[depth], "%.62s/", full);
                parent_end[depth] = e[2];
            }
        } else if (!strcmp(full, path)) {
            *off = e[1];
            *size = e[2];
            return 0;
        }
    }
    return -1;
}

/* ---- A small OSLink ---- */

typedef struct {
    uint32_t id, next, prev, nsec, secinfo, name_off, name_size, version;
    uint32_t bss_size, rel_off, imp_off, imp_size;
    uint8_t prolog_sec, epilog_sec, unres_sec, bss_sec;
    uint32_t prolog, epilog, unresolved;
} rel_header;

typedef struct {
    uint32_t offset, size;
} rel_section;

typedef struct {
    uint32_t id, offset;
} rel_import;

typedef struct {
    uint16_t offset;
    uint8_t type, section;
    uint32_t addend;
} rel_reloc;

#define LIST_HEAD (*(volatile uint32_t *)0x800030C8)
#define LIST_TAIL (*(volatile uint32_t *)0x800030CC)

static void patch(uint8_t *p, int type, uint32_t x)
{
    uint32_t *w = (uint32_t *)p;
    uint16_t *h = (uint16_t *)p;
    switch (type) {
    case 1: *w = x; break;                                                   /* ADDR32 */
    case 2: *w = (*w & ~0x03FFFFFCu) | (x & 0x03FFFFFCu); break;             /* ADDR24 */
    case 3: case 4: *h = (uint16_t)x; break;                                 /* ADDR16, ADDR16_LO */
    case 5: *h = (uint16_t)(x >> 16); break;                                 /* ADDR16_HI */
    case 6: *h = (uint16_t)((x >> 16) + ((x & 0x8000) ? 1 : 0)); break;     /* ADDR16_HA */
    case 10: *w = (*w & ~0x03FFFFFCu) | ((x - (uint32_t)p) & 0x03FFFFFCu); break;  /* REL24 */
    case 11: case 12: case 13: *w = (*w & ~0xFFFCu) | ((x - (uint32_t)p) & 0xFFFCu); break; /* REL14 */
    default: break;
    }
}

static int link_module(rel_header *m, void *bss)
{
    rel_section *si = (rel_section *)((uint8_t *)m + m->secinfo);
    m->secinfo += (uint32_t)m;
    for (uint32_t k = 0; k < m->nsec; k++) {
        if (si[k].offset)
            si[k].offset += (uint32_t)m;
        else if (si[k].size && k == m->bss_sec)
            si[k].offset = (uint32_t)bss;
    }
    rel_import *imp = (rel_import *)((uint8_t *)m + m->imp_off);
    for (uint32_t i = 0; i < m->imp_size / 8; i++) {
        if (imp[i].id != 0 && imp[i].id != m->id)
            continue; /* a module that is not loaded: left as the file has it */
        uint8_t *p = NULL;
        for (rel_reloc *r = (rel_reloc *)((uint8_t *)m + imp[i].offset); r->type != 203; r++) {
            p += r->offset;
            if (r->type == 202) {                /* DOLPHIN_SECTION */
                p = (uint8_t *)(si[r->section].offset & ~1u);
                continue;
            }
            if (r->type == 0 || r->type == 201)  /* NONE, DOLPHIN_NOP */
                continue;
            uint32_t s = imp[i].id ? si[r->section].offset & ~1u : 0;
            patch(p, r->type, s + r->addend);
        }
    }
    if (m->prolog_sec)
        m->prolog += si[m->prolog_sec].offset & ~1u;
    for (uint32_t k = 0; k < m->nsec; k++)
        if (si[k].offset & 1) {
            DCFlushRange((void *)(si[k].offset & ~1u), si[k].size);
            ICInvalidateRange((void *)(si[k].offset & ~1u), si[k].size);
        }
    /* on the list */
    m->next = 0;
    m->prev = LIST_TAIL;
    if (LIST_TAIL)
        ((rel_header *)LIST_TAIL)->next = (uint32_t)m;
    else
        LIST_HEAD = (uint32_t)m;
    LIST_TAIL = (uint32_t)m;
    return 0;
}

static void unlink_module(rel_header *m)
{
    if (m->prev)
        ((rel_header *)m->prev)->next = m->next;
    else
        LIST_HEAD = m->next;
    if (m->next)
        ((rel_header *)m->next)->prev = m->prev;
    else
        LIST_TAIL = m->prev;
}

typedef int (*prolog_fn)(int);

static rel_header *load(const char *path, uint8_t *buf, uint32_t cap, void *bss, int step)
{
    uint32_t off, size;
    if (fst_find(path, &off, &size) || ((size + 31) & ~31u) > cap) {
        report.err = step;
        return NULL;
    }
    if (dvd_read(buf, off, (size + 31) & ~31u)) {
        report.err = step + 1;
        return NULL;
    }
    rel_header *m = (rel_header *)buf;
    memset(bss, 0, m->bss_size);
    link_module(m, bss);
    return m;
}

int main(void)
{
    VIDEO_Init();
    GXRModeObj *rmode = VIDEO_GetPreferredMode(NULL);
    void *xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    console_init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight, rmode->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    printf("\x1b[2;0HXRecomp GameCube REL modules (XBA-264)\n\n");

    static uint8_t buf[64 * 1024] __attribute__((aligned(32)));
    static uint8_t bss[4096] __attribute__((aligned(32)));
    report.base = (uint32_t)buf;
    if (!rel_exports[0] || !rel_exports[1])
        report.err = 1;
    rel_header *a = load("rel/a.rel", buf, sizeof buf, bss, 10);
    if (a) {
        for (int i = 0; i < 3; i++)
            VIDEO_WaitVSync(); /* loaded for a few fields: the runtime records a module once its list settles */
        report.a_result = ((prolog_fn)a->prolog)(5);
        unlink_module(a);
    }
    rel_header *b = report.err ? NULL : load("rel/b.rel", buf, sizeof buf, bss, 20);
    prolog_fn bf = b ? (prolog_fn)b->prolog : NULL;
    if (bf)
        report.b_result = bf(7);
    report.counter = (uint32_t)rel_shared_counter;
    report.done = 1;
    printf("modules at %08lX: A %ld, B %ld, counter %lu, error %ld\n", (unsigned long)report.base,
           (long)report.a_result, (long)report.b_result, (unsigned long)report.counter, (long)report.err);
    for (;;) {
        VIDEO_WaitVSync();
        if (bf) {
            report.b_sum += bf((int)report.b_calls & 7);
            report.b_calls++;
        }
    }
    return 0;
}
