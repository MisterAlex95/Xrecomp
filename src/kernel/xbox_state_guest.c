/*
 * xbox_state_guest.c - the built-in sections of an Xbox save state: CPU, MEM, RAM (XBA-245, xbox_state.h)
 *
 * CPU v2   u32 thread count, u32 sizeof(RecompCpu), then per thread a 68-byte head and the RecompCpu:
 *            slot, park state, ordinal, guest return address, returned (0 / 1), status, dispatch, host tid,
 *            u64 kernel calls, TIB, KTHREAD, guest stack base, guest stack limit, esp, guest thread id,
 *            park site (XBOX_PARK_SITE_*: 0 a kernel call; else a host wait inside lifted code, XBA-251,
 *            whose ordinal is 0 and guest return address the site's VA). v1 (64-byte head, no site) is
 *            refused: a state only loads into the build that wrote it anyway.
 * MEM v1   xbox_MemStateSave (xbox_memory_layout.c)
 * RAM v1   u32 range count, then per range u32 kind (XBOX_RANGE_*), u32 guest base, u32 pages, a bitmap of
 *          the pages stored (bit set: not all zero; (pages + 7) / 8 bytes), then those pages, 4 KB each
 *
 * Dirty pages: each capture keeps a 64-bit hash per page and counts the pages that changed since the
 * previous capture (logged; rewind, XBA-249, will store only those).
 */
#include "xbox_state.h"
#include "kernel_park.h"

#include <stdlib.h>
#include <string.h>

#define PAGE 4096u
#define CPU_HEAD 68u

/* ---- CPU ------------------------------------------------------------------------------------------------- */

int xbox_state_cpu_save(XboxStateWriter *w, const XboxStateContext *ctx, void *user)
{
    int i;
    (void)user;

    xbox_StatePut32(w, (uint32_t)ctx->threads);
    xbox_StatePut32(w, (uint32_t)sizeof(RecompCpu));
    for (i = 0; i < ctx->threads; i++) {
        XboxStateThread t;
        if (xbox_StateContextThread(ctx, i, &t) != 0)
            return XBOX_STATE_ECORRUPT;
        xbox_StatePut32(w, (uint32_t)t.slot);
        xbox_StatePut32(w, (uint32_t)t.state);
        xbox_StatePut32(w, t.ordinal);
        xbox_StatePut32(w, t.ret_va);
        xbox_StatePut32(w, (uint32_t)t.returned);
        xbox_StatePut32(w, t.status);
        xbox_StatePut32(w, (uint32_t)t.dispatch);
        xbox_StatePut32(w, t.tid);
        xbox_StatePut64(w, t.kernel_calls);
        xbox_StatePut32(w, t.tib);
        xbox_StatePut32(w, t.kthread);
        xbox_StatePut32(w, t.stack_base);
        xbox_StatePut32(w, t.stack_limit);
        xbox_StatePut32(w, t.cpu.r_esp);
        xbox_StatePut32(w, t.guest_tid);
        xbox_StatePut32(w, t.site);
        xbox_StatePut(w, &t.cpu, sizeof t.cpu);
    }
    return XBOX_STATE_OK;
}

int xbox_StateCpuThreads(const uint8_t *p, size_t n, XboxStateThread *out, int max)
{
    XboxStateReader r = { p, n, 0, 0 };
    uint32_t count = xbox_StateGet32(&r), cpu_bytes = xbox_StateGet32(&r), i;
    uint64_t used = 0;

    if (r.bad || count > XBOX_PARK_MAX_THREADS || cpu_bytes != sizeof(RecompCpu)
        || n != 8 + (size_t)count * (CPU_HEAD + cpu_bytes))
        return -1;
    for (i = 0; i < count; i++) {
        XboxStateThread t;
        const uint8_t *cpu;
        memset(&t, 0, sizeof t);
        t.slot = (int)xbox_StateGet32(&r);
        t.state = (int)xbox_StateGet32(&r);
        t.ordinal = xbox_StateGet32(&r);
        t.ret_va = xbox_StateGet32(&r);
        t.returned = (int)xbox_StateGet32(&r);
        t.status = xbox_StateGet32(&r);
        t.dispatch = (int)xbox_StateGet32(&r);
        t.tid = xbox_StateGet32(&r);
        t.kernel_calls = xbox_StateGet64(&r);
        t.tib = xbox_StateGet32(&r);
        t.kthread = xbox_StateGet32(&r);
        t.stack_base = xbox_StateGet32(&r);
        t.stack_limit = xbox_StateGet32(&r);
        (void)xbox_StateGet32(&r);   /* esp, also in the RecompCpu */
        t.guest_tid = xbox_StateGet32(&r);
        t.site = xbox_StateGet32(&r);
        cpu = xbox_StateGet(&r, cpu_bytes);
        if (r.bad || t.slot < 0 || t.slot >= XBOX_PARK_MAX_THREADS || (used >> t.slot) & 1u
            || (t.state != XBOX_PARK_KERNEL && t.state != XBOX_PARK_WAITING && t.state != XBOX_PARK_HELD)
            || (t.returned != 0 && t.returned != 1) || t.dispatch
            || t.site >= XBOX_PARK_SITE_COUNT || (t.site != XBOX_PARK_SITE_KERNEL && t.ordinal != 0))
            return -1;
        used |= 1ull << t.slot;
        memcpy(&t.cpu, cpu, sizeof t.cpu);
        if ((int)i < max && out)
            out[i] = t;
    }
    return (int)count;
}

int xbox_state_cpu_check(const uint8_t *p, size_t n, uint32_t version, void *user)
{
    (void)user;
    if (version != XBOX_STATE_CPU_VERSION)
        return XBOX_STATE_EVERSION;
    if (n >= 8 && (p[4] | p[5] << 8 | p[6] << 16 | (uint32_t)p[7] << 24) != sizeof(RecompCpu))
        return XBOX_STATE_EVERSION;
    return xbox_StateCpuThreads(p, n, NULL, 0) >= 0 ? XBOX_STATE_OK : XBOX_STATE_ECORRUPT;
}

/* ---- MEM ------------------------------------------------------------------------------------------------- */

int xbox_state_mem_save(XboxStateWriter *w, const XboxStateContext *ctx, void *user)
{
    size_t start = w->n, room = xbox_MemStateSave(NULL, 0) + 4096, got;
    int tries;
    (void)ctx; (void)user;

    for (tries = 0; tries < 4; tries++) {
        uint8_t *p = xbox_StateReserve(w, room);
        if (!p)
            return XBOX_STATE_ENOMEM;
        got = xbox_MemStateSave(p, room);
        if (got <= room) {
            w->n = start + got;
            return XBOX_STATE_OK;
        }
        w->n = start;             /* a host thread allocated meanwhile: once more, larger */
        room = got + 65536;
    }
    return XBOX_STATE_ESECTION;
}

int xbox_state_mem_check(const uint8_t *p, size_t n, uint32_t version, void *user)
{
    (void)user;
    if (version != XBOX_MEM_STATE_VERSION)
        return XBOX_STATE_EVERSION;
    return xbox_MemStateCheck(p, n, NULL) == 0 ? XBOX_STATE_OK : XBOX_STATE_ECORRUPT;
}

/* ---- RAM ------------------------------------------------------------------------------------------------- */

typedef struct {
    uint32_t kind, va, pages;
    uint64_t *hash;          /* per page, 0 for an all-zero page */
} PageHashes;

#define MAX_HASHED 64
static PageHashes s_prev[MAX_HASHED], s_cur[MAX_HASHED];
static int s_nprev, s_ncur;
static int s_have_prev;
static uint32_t s_ranges, s_pages, s_stored, s_changed;

static void hashes_free(PageHashes *h, int *n)
{
    int i;
    for (i = 0; i < *n; i++)
        free(h[i].hash);
    *n = 0;
}

void xbox_state_ram_begin(void)
{
    hashes_free(s_cur, &s_ncur);
    s_ranges = s_pages = s_stored = s_changed = 0;
}

void xbox_state_ram_commit(XboxStateStats *st)
{
    hashes_free(s_prev, &s_nprev);
    memcpy(s_prev, s_cur, (size_t)s_ncur * sizeof s_cur[0]);
    s_nprev = s_ncur;
    s_ncur = 0;
    s_have_prev = 1;
    st->ram_ranges = s_ranges;
    st->ram_pages = s_pages;
    st->ram_stored = s_stored;
    st->ram_changed = s_changed;
}

void xbox_state_ram_reset(void)
{
    hashes_free(s_prev, &s_nprev);
    hashes_free(s_cur, &s_ncur);
    s_have_prev = 0;
}

/* Not all zero? and its hash (0 when all zero), in one pass. */
static int page_scan(const uint8_t *p, uint64_t *hash)
{
    const uint64_t K = 0x9E3779B97F4A7C15ull;
    uint64_t any = 0, a = 1, b = 2, c = 3, d = 4, w0, w1, w2, w3;
    unsigned i;

    for (i = 0; i < PAGE; i += 32) {
        memcpy(&w0, p + i, 8);
        memcpy(&w1, p + i + 8, 8);
        memcpy(&w2, p + i + 16, 8);
        memcpy(&w3, p + i + 24, 8);
        any |= w0 | w1 | w2 | w3;
        a = (a ^ w0) * K; a ^= a >> 32;
        b = (b ^ w1) * K; b ^= b >> 32;
        c = (c ^ w2) * K; c ^= c >> 32;
        d = (d ^ w3) * K; d ^= d >> 32;
    }
    if (!any) {
        *hash = 0;
        return 0;
    }
    *hash = (a ^ (b * 3) ^ (c * 5) ^ (d * 7)) | 1u;
    return 1;
}

static int page_nonzero(const uint8_t *p)
{
    uint64_t any = 0, w[8];
    unsigned i, k;

    for (i = 0; i < PAGE; i += 64) {
        memcpy(w, p + i, 64);
        for (k = 0; k < 8; k++)
            any |= w[k];
        if (any)
            return 1;
    }
    return 0;
}

static const PageHashes *prev_range(uint32_t kind, uint32_t va)
{
    int i;
    for (i = 0; i < s_nprev; i++)
        if (s_prev[i].kind == kind && s_prev[i].va == va)
            return &s_prev[i];
    return NULL;
}

int xbox_state_ram_save(XboxStateWriter *w, const XboxStateContext *ctx, void *user)
{
    XboxGuestRange stack_ranges[16], *ranges = stack_ranges;
    int nr, i;
    (void)ctx; (void)user;

    nr = xbox_StateGuestRanges(ranges, 16);
    if (nr > 16) {
        ranges = (XboxGuestRange *)malloc((size_t)nr * sizeof *ranges);
        if (!ranges)
            return XBOX_STATE_ENOMEM;
        nr = xbox_StateGuestRanges(ranges, nr);
    }
    xbox_StatePut32(w, (uint32_t)nr);
    for (i = 0; i < nr; i++) {
        const XboxGuestRange *g = &ranges[i];
        uint32_t pages = g->size / PAGE, pg;
        size_t bits_at;
        uint8_t *bits;
        const PageHashes *prev = prev_range(g->kind, g->va);
        PageHashes *cur = NULL;

        if (s_ncur < MAX_HASHED) {
            cur = &s_cur[s_ncur];
            cur->hash = (uint64_t *)calloc(pages ? pages : 1, sizeof(uint64_t));
            if (cur->hash) {
                cur->kind = g->kind;
                cur->va = g->va;
                cur->pages = pages;
                s_ncur++;
            } else {
                cur = NULL;
            }
        }
        xbox_StatePut32(w, g->kind);
        xbox_StatePut32(w, g->va);
        xbox_StatePut32(w, pages);
        bits_at = w->n;
        bits = xbox_StateReserve(w, (pages + 7) / 8);
        if (!bits)
            break;
        memset(bits, 0, (pages + 7) / 8);
        s_ranges++;
        s_pages += pages;
        for (pg = 0; pg < pages; pg++) {
            const uint8_t *src = g->host + (size_t)pg * PAGE;
            uint64_t h = 0;
            uint8_t *dst;
            int nz = page_nonzero(src);
            /* A page that is not all zero is copied and then hashed from the copy: what is stored and what
             * is hashed are the same bytes even if a host thread writes the page meanwhile. */
            if (nz) {
                dst = xbox_StateReserve(w, PAGE);
                if (!dst)
                    break;
                memcpy(dst, src, PAGE);
                nz = page_scan(dst, &h);
                if (!nz)
                    w->n -= PAGE;
            }
            if (cur)
                cur->hash[pg] = h;
            if (!s_have_prev || !prev || pg >= prev->pages ? nz : prev->hash[pg] != h)
                s_changed++;
            if (!nz)
                continue;
            w->buf[bits_at + (pg >> 3)] |= (uint8_t)(1u << (pg & 7));
            s_stored++;
        }
        if (w->failed)
            break;
    }
    if (ranges != stack_ranges)
        free(ranges);
    return w->failed ? XBOX_STATE_ENOMEM : XBOX_STATE_OK;
}

/* Walks a RAM payload: fn for each range (its stored bitmap and pages); -1 when malformed. */
typedef int (*range_fn)(uint32_t kind, uint32_t va, uint32_t pages, const uint8_t *bits, const uint8_t *data,
                        void *user);

static int ram_walk(const uint8_t *p, size_t n, range_fn fn, void *user, XboxStateRamInfo *info)
{
    XboxStateReader r = { p, n, 0, 0 };
    uint32_t count = xbox_StateGet32(&r), i;
    XboxStateRamInfo d;

    memset(&d, 0, sizeof d);
    if (r.bad || count > 1u << 20)
        return -1;
    for (i = 0; i < count; i++) {
        uint32_t kind = xbox_StateGet32(&r), va = xbox_StateGet32(&r), pages = xbox_StateGet32(&r), pg, stored = 0;
        const uint8_t *bits, *data;
        if (r.bad || kind > XBOX_RANGE_VMEM || (va & (PAGE - 1)) || (uint64_t)va + (uint64_t)pages * PAGE > 1ull << 32)
            return -1;
        bits = xbox_StateGet(&r, (pages + 7) / 8);
        if (!bits)
            return -1;
        for (pg = 0; pg < pages; pg++)
            stored += (bits[pg >> 3] >> (pg & 7)) & 1u;
        if (pages & 7u && bits[pages >> 3] >> (pages & 7))
            return -1;            /* bits past the last page */
        data = xbox_StateGet(&r, (size_t)stored * PAGE);
        if (!data)
            return -1;
        d.ranges++;
        d.pages += pages;
        d.stored += stored;
        if (fn) {
            int rc = fn(kind, va, pages, bits, data, user);
            if (rc)
                return rc;
        }
    }
    if (r.at != n)
        return -1;
    if (info)
        *info = d;
    return 0;
}

int xbox_StateRamInfo(const uint8_t *p, size_t n, XboxStateRamInfo *info)
{
    return ram_walk(p, n, NULL, NULL, info);
}

struct page_find { uint32_t va; const uint8_t *page; int found; };

static int find_cb(uint32_t kind, uint32_t va, uint32_t pages, const uint8_t *bits, const uint8_t *data, void *user)
{
    struct page_find *f = (struct page_find *)user;
    uint32_t idx, pg, before = 0;
    (void)kind;

    if (f->va < va || (uint64_t)f->va >= (uint64_t)va + (uint64_t)pages * PAGE)
        return 0;
    idx = (f->va - va) / PAGE;
    f->found = 1;
    if (!((bits[idx >> 3] >> (idx & 7)) & 1u)) {
        f->page = NULL;
        return 1;
    }
    for (pg = 0; pg < idx; pg++)
        before += (bits[pg >> 3] >> (pg & 7)) & 1u;
    f->page = data + (size_t)before * PAGE;
    return 1;
}

int xbox_StateRamPage(const uint8_t *p, size_t n, uint32_t va, const uint8_t **page)
{
    struct page_find f = { va & ~(PAGE - 1), NULL, 0 };
    int rc = ram_walk(p, n, find_cb, &f, NULL);

    if (rc < 0)
        return -1;
    if (page)
        *page = f.page;
    return f.found;
}

int xbox_state_ram_check(const uint8_t *p, size_t n, uint32_t version, void *user)
{
    (void)user;
    if (version != XBOX_STATE_RAM_VERSION)
        return XBOX_STATE_EVERSION;
    return ram_walk(p, n, NULL, NULL, NULL) == 0 ? XBOX_STATE_OK : XBOX_STATE_ECORRUPT;
}
