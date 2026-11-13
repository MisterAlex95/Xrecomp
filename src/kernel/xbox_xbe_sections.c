/* XBE sections loaded on demand (XBA-124): see xbox_xbe_sections.h.
 * No dependency on the rest of the runtime but guest memory, so a test
 * builds it with a stand-in for xbox_GetMemoryOffset. */
#include "xbox_xbe_sections.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static SRWLOCK s_lock = SRWLOCK_INIT;
#define LOCK()   AcquireSRWLockExclusive(&s_lock)
#define UNLOCK() ReleaseSRWLockExclusive(&s_lock)
#else
#include <pthread.h>
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK()   pthread_mutex_lock(&s_lock)
#define UNLOCK() pthread_mutex_unlock(&s_lock)
#endif

extern ptrdiff_t xbox_GetMemoryOffset(void);

/* XBE header and section header fields (docs/formats/xbe.md). */
#define XBE_BASE_ADDR        0x104u
#define XBE_NUM_SECTIONS     0x11Cu
#define XBE_SECTION_HEADERS  0x120u
#define SH_SIZE              56u
#define SH_FLAGS             0x00u
#define SH_VA                0x04u
#define SH_VSIZE             0x08u
#define SH_RAW               0x0Cu
#define SH_RAW_SIZE          0x10u
#define SH_NAME              0x14u
#define SH_REFCOUNT          0x18u
#define SH_HEAD_REF          0x1Cu
#define SH_TAIL_REF          0x20u

#define F_WRITABLE   0x1u
#define F_PRELOAD    0x2u
#define F_EXECUTABLE 0x4u

/* Guest addresses the module may write: the title's 64 MB, above the
 * header's base. A field pointing elsewhere is left alone. */
#define GUEST_LO 0x00010000u
#define GUEST_HI 0x04000000u

typedef struct {
    uint32_t hdr_va;        /* the section header, in the guest's copy of the XBE header */
    uint32_t va, vsize, flags;
    uint32_t head_ref, tail_ref;
    uint32_t copy_size;     /* bytes of image data (min(raw size, virtual size)) */
    uint8_t *image;         /* those bytes, kept when the section may need them again */
    int overlaps;           /* shares bytes with another section */
    int resident;           /* its bytes are the ones in memory (an overlapping one may have replaced them) */
    char name[12];
} XbeSection;

static XbeSection *s_secs;
static unsigned s_n, s_overlap_pairs;
static int s_on;
static unsigned s_said;

static uint8_t *guest(uint32_t va) { return (uint8_t *)xbox_GetMemoryOffset() + va; }
static int writable_range(uint32_t va, uint32_t n)
{
    return va >= GUEST_LO && va < GUEST_HI && n <= GUEST_HI - va;
}
static uint32_t rd32(uint32_t va) { uint32_t v; memcpy(&v, guest(va), 4); return v; }
static void wr32(uint32_t va, uint32_t v) { memcpy(guest(va), &v, 4); }

static void bump16(uint32_t va, int by)
{
    uint16_t v;
    if (!va || !writable_range(va, 2))
        return;
    memcpy(&v, guest(va), 2);
    if (by > 0)
        v++;
    else if (v)
        v--;
    memcpy(guest(va), &v, 2);
}

static int share_bytes(const XbeSection *a, const XbeSection *b)
{
    uint64_t lo = a->va > b->va ? a->va : b->va;
    uint64_t ahi = (uint64_t)a->va + a->vsize, bhi = (uint64_t)b->va + b->vsize;
    return a->vsize && b->vsize && lo < (ahi < bhi ? ahi : bhi);
}

/* Copy the section from the image: zero its virtual size, then the data. */
static void copy_in(XbeSection *s)
{
    if (!writable_range(s->va, s->vsize))
        return;
    memset(guest(s->va), 0, s->vsize);
    if (s->image && s->copy_size)
        memcpy(guest(s->va), s->image, s->copy_size);
    s->resident = 1;
    for (unsigned j = 0; j < s_n; j++)
        if (&s_secs[j] != s && s_secs[j].overlaps && share_bytes(s, &s_secs[j]))
            s_secs[j].resident = 0;
}

void xbox_XbeSectionsShutdown(void)
{
    LOCK();
    for (unsigned i = 0; i < s_n; i++)
        free(s_secs[i].image);
    free(s_secs);
    s_secs = NULL;
    s_n = s_overlap_pairs = 0;
    UNLOCK();
}

void xbox_XbeSectionsInit(const uint8_t *xbe, size_t xbe_size)
{
    const char *e = getenv("RECOMP_XBE_SECTIONS");
    uint32_t base, n, hdrs_va;
    uint64_t hdrs_off;

    xbox_XbeSectionsShutdown();
    s_on = !(e && e[0] == '0');
    s_said = 0;
    if (!xbe || xbe_size < 0x178)
        return;
    memcpy(&base, xbe + XBE_BASE_ADDR, 4);
    memcpy(&n, xbe + XBE_NUM_SECTIONS, 4);
    memcpy(&hdrs_va, xbe + XBE_SECTION_HEADERS, 4);
    if (hdrs_va < base)
        return;
    hdrs_off = (uint64_t)hdrs_va - base;
    if (!n || hdrs_off + (uint64_t)n * SH_SIZE > xbe_size || n > 4096)
        return;

    LOCK();
    s_secs = (XbeSection *)calloc(n, sizeof *s_secs);
    if (!s_secs) {
        UNLOCK();
        return;
    }
    s_n = n;
    for (unsigned i = 0; i < n; i++) {
        const uint8_t *sh = xbe + hdrs_off + (uint64_t)i * SH_SIZE;
        XbeSection *s = &s_secs[i];
        uint32_t raw, raw_size, name_va;
        memcpy(&s->flags, sh + SH_FLAGS, 4);
        memcpy(&s->va, sh + SH_VA, 4);
        memcpy(&s->vsize, sh + SH_VSIZE, 4);
        memcpy(&raw, sh + SH_RAW, 4);
        memcpy(&raw_size, sh + SH_RAW_SIZE, 4);
        memcpy(&name_va, sh + SH_NAME, 4);
        memcpy(&s->head_ref, sh + SH_HEAD_REF, 4);
        memcpy(&s->tail_ref, sh + SH_TAIL_REF, 4);
        s->hdr_va = hdrs_va + i * SH_SIZE;
        s->copy_size = raw_size < s->vsize ? raw_size : s->vsize;
        if ((uint64_t)raw + s->copy_size > xbe_size)
            s->copy_size = 0;               /* truncated: the layout left it zero too */
        if (name_va >= base && (uint64_t)name_va - base < xbe_size) {
            size_t off = name_va - base, k;
            for (k = 0; k + 1 < sizeof s->name && off + k < xbe_size && xbe[off + k]; k++)
                s->name[k] = (char)xbe[off + k];
        }
        if (!s->name[0])
            snprintf(s->name, sizeof s->name, "#%u", i);
        s->resident = 1;
        /* Borrowed until the copies below. */
        s->image = s->copy_size ? (uint8_t *)(uintptr_t)(xbe + raw) : NULL;
    }

    /* Overlaps, in table order: the layout copied every section in that
     * order, so the later of two holds the shared bytes now. */
    for (unsigned i = 0; i < n; i++)
        for (unsigned j = i + 1; j < n; j++)
            if (share_bytes(&s_secs[i], &s_secs[j])) {
                int code = (s_secs[i].flags & F_EXECUTABLE) && (s_secs[j].flags & F_EXECUTABLE);
                s_secs[i].overlaps = s_secs[j].overlaps = 1;
                s_secs[i].resident = 0;
                s_overlap_pairs++;
                fprintf(stderr, "  [XBE] sections %s and %s share 0x%08X..: loaded one at a time "
                        "(XeLoadSection)%s\n", s_secs[i].name, s_secs[j].name,
                        s_secs[i].va > s_secs[j].va ? s_secs[i].va : s_secs[j].va,
                        code ? "; BLOCKER: both hold code, and the lifted C has only one function per "
                               "address (XBA-124)" : "");
            }

    /* Keep a copy of what may have to be copied in again; the caller's
     * buffer is not ours to keep. */
    for (unsigned i = 0; i < n; i++) {
        XbeSection *s = &s_secs[i];
        const uint8_t *src = s->image;
        s->image = NULL;
        if (!s_on || !s->copy_size || !((s->flags & F_WRITABLE) || s->overlaps))
            continue;
        s->image = (uint8_t *)malloc(s->copy_size);
        if (s->image)
            memcpy(s->image, src, s->copy_size);
        else
            fprintf(stderr, "  [XBE] section %s: no memory for its copy (%u bytes); not reloaded\n",
                    s->name, s->copy_size);
    }

    if (s_on) {
        /* The console's boot state: PRELOAD sections loaded once, with their
         * shared pages counted; and an overlap's PRELOAD section in memory. */
        for (unsigned i = 0; i < n; i++) {
            XbeSection *s = &s_secs[i];
            if (!(s->flags & F_PRELOAD) || !writable_range(s->hdr_va, SH_SIZE))
                continue;
            if (rd32(s->hdr_va + SH_REFCOUNT) == 0) {
                wr32(s->hdr_va + SH_REFCOUNT, 1);
                bump16(s->head_ref, +1);
                bump16(s->tail_ref, +1);
            }
            if (s->overlaps && !s->resident) {
                copy_in(s);
                fprintf(stderr, "  [XBE] section %s (preload) is the one in memory at 0x%08X\n",
                        s->name, s->va);
            }
        }
    }
    UNLOCK();
}

static XbeSection *find(uint32_t hdr_va)
{
    for (unsigned i = 0; i < s_n; i++)
        if (s_secs[i].hdr_va == hdr_va)
            return &s_secs[i];
    return NULL;
}

static uint32_t section_op(uint32_t hdr_va, int load)
{
    XbeSection *s;
    uint32_t count;

    if (!hdr_va)
        return 0xC000000Du;                         /* STATUS_INVALID_PARAMETER */
    if (!writable_range(hdr_va, SH_SIZE))
        return 0xC000000Du;
    LOCK();
    s = s_on ? find(hdr_va) : NULL;
    count = rd32(hdr_va + SH_REFCOUNT);
    if (load) {
        if (s && count == 0) {
            const char *why = NULL;
            for (unsigned j = 0; j < s_n; j++) {
                XbeSection *o = &s_secs[j];
                if (o != s && o->overlaps && share_bytes(s, o) && writable_range(o->hdr_va, SH_SIZE)
                    && rd32(o->hdr_va + SH_REFCOUNT))
                    fprintf(stderr, "  [XBE] XeLoadSection(%s) while %s, at the same addresses, is "
                            "loaded: %s's bytes are replaced\n", s->name, o->name, o->name);
            }
            if (s->overlaps && !s->resident)
                why = "another section held its addresses";
            else if (s->flags & F_WRITABLE)
                why = "writable: fresh data, zeroed BSS";
            if (why) {
                copy_in(s);
                if (s_said++ < 16)
                    fprintf(stderr, "  [XBE] XeLoadSection(%s): copied again from the image at 0x%08X "
                            "(+0x%X; %s)\n", s->name, s->va, s->vsize, why);
                if ((s->flags & F_EXECUTABLE) && s->overlaps)
                    fprintf(stderr, "  [XBE] section %s holds code at the same addresses as another: "
                            "its calls still run the other's lifted code (XBA-124)\n", s->name);
            }
            bump16(s->head_ref, +1);
            bump16(s->tail_ref, +1);
        }
        count++;
    } else if (count) {
        count--;
        if (s && count == 0) {
            bump16(s->head_ref, -1);
            bump16(s->tail_ref, -1);
        }
    }
    wr32(hdr_va + SH_REFCOUNT, count);
    UNLOCK();
    return 0;
}

uint32_t xbox_XbeSectionLoad(uint32_t hdr_va)   { return section_op(hdr_va, 1); }
uint32_t xbox_XbeSectionUnload(uint32_t hdr_va) { return section_op(hdr_va, 0); }
unsigned xbox_XbeSectionCount(void)             { return s_n; }
unsigned xbox_XbeSectionOverlaps(void)          { return s_overlap_pairs; }
