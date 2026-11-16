/*
 * xbox_state_guest.c - the built-in sections of an Xbox save state: CPU, MEM, RAM (XBA-245), and the wrappers
 * that make the kernel objects (XBA-246) and the devices (XBA-248) sections of the container (XBA-249).
 * xbox_state.h has the API.
 *
 * CPU v4   u32 thread count, u32 sizeof(RecompCpuRegs), u64 run id (xbox_StateRunId, XBA-249), then per thread
 *          a 68-byte head, the RecompCpuRegs and the call chain:
 *            head: slot, park state, ordinal, guest return address, returned (0 / 1), status, dispatch, host
 *            tid, u64 kernel calls, TIB, KTHREAD, guest stack base, guest stack limit, esp at the park point
 *            (the call's return address slot, XBA-249), guest thread id, park site (XBOX_PARK_SITE_*: 0 a
 *            kernel call; else a host wait inside lifted code, XBA-251, whose ordinal is 0 and guest return
 *            address the site's VA)
 *            chain (XBA-247, xbox_StateChain): u32 frames, u32 base, u32 flags (XBOX_CHAIN_*), u32 0,
 *            u64 signature, then per frame u32 return VA, esp, ebp, function, call-site flags
 *          Older versions are refused (v3 had a RecompCpu without the frame count and no chain): a state only
 *          loads into the build that wrote it anyway. Its load is xbox_state.c's (threads matched).
 * MEM v1   xbox_MemStateSave / xbox_MemStateLoad (xbox_memory_layout.c)
 * RAM v1   u32 range count, then per range u32 kind (XBOX_RANGE_*), u32 guest base, u32 pages, a bitmap of
 *          the pages stored (bit set: not all zero; (pages + 7) / 8 bytes), then those pages, 4 KB each
 * KOBJ v1  xbox_KobjSave's blob ("XKOB"), taken for this run (XBOX_KOBJ_IN_SESSION)
 * NV2A, APU, USB v1   the device's own payload (xbox_DevStateSections)
 *
 * Dirty pages: each capture keeps a 64-bit hash per page and counts the pages that changed since the
 * previous capture (logged).
 */
#include "xbox_state.h"
#include "kernel_park.h"
#include "kernel_kobj_state.h"
#include "xbox_dev_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PAGE 4096u
#define CPU_HEAD 68u

uint64_t xbox_StateRunId(void)
{
    static uint64_t id;
    if (!id) {             /* the same answer every time after the first: a benign race at most */
        struct timespec ts;
        uint64_t v;
        clock_gettime(CLOCK_REALTIME, &ts);
        v = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
        v ^= (uint64_t)getpid() << 40;
        v ^= (uint64_t)(uintptr_t)&id;
        v *= 0x9E3779B97F4A7C15ull;
        v ^= v >> 31;
        id = v ? v : 1;
    }
    return id;
}
#define CHAIN_HEAD 24u
#define CHAIN_FRAME 20u

/* ---- CPU ------------------------------------------------------------------------------------------------- */

int xbox_state_cpu_save(XboxStateWriter *w, const XboxStateContext *ctx, void *user)
{
    XboxStateFrame *fr = malloc(RECOMP_FRAME_SLOTS * sizeof *fr);
    int i, rc = XBOX_STATE_OK;
    (void)user;

    if (!fr)
        return XBOX_STATE_ENOMEM;
    xbox_StatePut32(w, (uint32_t)ctx->threads);
    xbox_StatePut32(w, (uint32_t)sizeof(RecompCpuRegs));
    xbox_StatePut64(w, xbox_StateRunId());
    for (i = 0; i < ctx->threads; i++) {
        XboxStateThread t;
        XboxStateChain c;
        uint32_t k;
        int n;
        if (xbox_StateContextThread(ctx, i, &t) != 0
            || (n = xbox_StateChain(&t, fr, RECOMP_FRAME_SLOTS, &c)) < 0 || (uint32_t)n != c.frames) {
            rc = XBOX_STATE_ECORRUPT;
            break;
        }
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
        xbox_StatePut32(w, t.call_esp);
        xbox_StatePut32(w, t.guest_tid);
        xbox_StatePut32(w, t.site);
        xbox_StatePut(w, &t.cpu, sizeof t.cpu);
        xbox_StatePut32(w, c.frames);
        xbox_StatePut32(w, c.base);
        xbox_StatePut32(w, c.flags);
        xbox_StatePut32(w, 0);
        xbox_StatePut64(w, c.signature);
        for (k = 0; k < c.frames; k++) {
            xbox_StatePut32(w, fr[k].ret_va);
            xbox_StatePut32(w, fr[k].esp);
            xbox_StatePut32(w, fr[k].ebp);
            xbox_StatePut32(w, fr[k].func);
            xbox_StatePut32(w, fr[k].flags);
        }
    }
    free(fr);
    return rc;
}

/* One thread's chain at r (after its registers): its head into *c, the frames (at most max) into out.
 * -1 when it is malformed: more frames than a thread can have, unknown flags, EMPTY not matching the
 * count, a signature that is not the frames'. */
static int chain_read(XboxStateReader *r, XboxStateChain *c, XboxStateFrame *out, uint32_t max)
{
    uint64_t h = 0xCBF29CE484222325ull;
    uint32_t k, pad;

    c->frames = xbox_StateGet32(r);
    c->base = xbox_StateGet32(r);
    c->flags = xbox_StateGet32(r);
    pad = xbox_StateGet32(r);
    c->signature = xbox_StateGet64(r);
    if (r->bad || pad || c->frames > RECOMP_FRAME_SLOTS || c->base > RECOMP_FRAME_SLOTS
        || (c->flags & ~(uint32_t)XBOX_CHAIN_ALL) || !(c->flags & XBOX_CHAIN_EMPTY) != !!c->frames)
        return -1;
#define CHAIN_MIX(w) (h = (h ^ (uint32_t)(w)) * 0x100000001B3ull)
    CHAIN_MIX(c->frames);
    for (k = 0; k < c->frames; k++) {
        XboxStateFrame f;
        f.ret_va = xbox_StateGet32(r);
        f.esp = xbox_StateGet32(r);
        f.ebp = xbox_StateGet32(r);
        f.func = xbox_StateGet32(r);
        f.flags = xbox_StateGet32(r);
        if (r->bad || f.flags >= 0x40u)
            return -1;
        CHAIN_MIX(f.ret_va);
        CHAIN_MIX(f.esp);
        if (out && k < max)
            out[k] = f;
    }
#undef CHAIN_MIX
    return h == c->signature ? 0 : -1;
}

int xbox_StateCpuThreads(const uint8_t *p, size_t n, XboxStateThread *out, int max)
{
    XboxStateReader r = { p, n, 0, 0 };
    uint32_t count = xbox_StateGet32(&r), cpu_bytes = xbox_StateGet32(&r), i;
    uint64_t used = 0;

    (void)xbox_StateGet64(&r);       /* the run id */
    if (r.bad || count > XBOX_PARK_MAX_THREADS || cpu_bytes != sizeof(RecompCpuRegs))
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
        t.call_esp = xbox_StateGet32(&r);
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
        if (chain_read(&r, &t.chain, NULL, 0) != 0)
            return -1;
        if ((int)i < max && out)
            out[i] = t;
    }
    return r.at == n ? (int)count : -1;     /* nothing after the last thread */
}

int xbox_StateCpuChain(const uint8_t *p, size_t n, int i, XboxStateFrame *out, uint32_t max)
{
    XboxStateReader r = { p, n, 0, 0 };
    uint32_t count, k;
    XboxStateChain c;

    if (xbox_StateCpuThreads(p, n, NULL, 0) < 0)
        return -1;
    count = xbox_StateGet32(&r);
    (void)xbox_StateGet32(&r);
    (void)xbox_StateGet64(&r);       /* the run id */
    if (i < 0 || (uint32_t)i >= count)
        return -1;
    for (k = 0; k <= (uint32_t)i; k++) {
        (void)xbox_StateGet(&r, CPU_HEAD + sizeof(RecompCpuRegs));
        if (chain_read(&r, &c, k == (uint32_t)i ? out : NULL, max) != 0)
            return -1;
    }
    return (int)c.frames;
}

uint64_t xbox_StateCpuRun(const uint8_t *p, size_t n)
{
    XboxStateReader r = { p, n, 8, 0 };
    uint64_t v = xbox_StateGet64(&r);
    return r.bad ? 0 : v;
}

int xbox_state_cpu_check(const uint8_t *p, size_t n, uint32_t version, void *user)
{
    (void)user;
    if (version != XBOX_STATE_CPU_VERSION)
        return XBOX_STATE_EVERSION;
    if (n >= 8 && (p[4] | p[5] << 8 | p[6] << 16 | (uint32_t)p[7] << 24) != sizeof(RecompCpuRegs))
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

int xbox_state_mem_load(const uint8_t *p, size_t n, uint32_t version, void *user)
{
    char why[160];
    (void)version; (void)user;
    if (xbox_MemStateLoad(p, n, why, sizeof why) != 0) {
        xbox_StateSectionReason("memory manager: %s", why);
        return XBOX_STATE_ECHAIN;
    }
    return XBOX_STATE_OK;
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

/* ---- RAM back into guest memory (XBA-249) ------------------------------------------------------------------ */

/* Every page of every saved range, the stored ones copied and the others zeroed, into wherever that guest page
 * lives now (xbox_StateGuestRanges: the ranges may have grown since; a saved page no range holds now refuses
 * the state). check_only: only that check. */
struct ram_put {
    XboxGuestRange *cur;
    int ncur;
    int check_only;
    uint32_t missing_va;
    uint32_t written;
};

static uint8_t *cur_host(const struct ram_put *rp, uint32_t va)
{
    int i;
    for (i = 0; i < rp->ncur; i++) {
        const XboxGuestRange *g = &rp->cur[i];
        if (va >= g->va && (uint64_t)va + PAGE <= (uint64_t)g->va + g->size)
            return g->host + (va - g->va);
    }
    return NULL;
}

static int put_cb(uint32_t kind, uint32_t va, uint32_t pages, const uint8_t *bits, const uint8_t *data, void *user)
{
    struct ram_put *rp = (struct ram_put *)user;
    uint32_t pg;
    (void)kind;

    for (pg = 0; pg < pages; pg++) {
        uint32_t at = va + pg * PAGE;
        uint8_t *dst = cur_host(rp, at);
        int stored = (bits[pg >> 3] >> (pg & 7)) & 1u;
        if (!dst) {
            rp->missing_va = at;
            return 1;
        }
        if (rp->check_only)
            continue;
        if (stored) {
            if (memcmp(dst, data, PAGE) != 0) {
                memcpy(dst, data, PAGE);
                rp->written++;
            }
            data += PAGE;
        } else if (page_nonzero(dst)) {
            memset(dst, 0, PAGE);
            rp->written++;
        }
    }
    return 0;
}

static int ram_put(const uint8_t *p, size_t n, int check_only, uint32_t *missing, uint32_t *written)
{
    XboxGuestRange stack_ranges[16], *ranges = stack_ranges;
    struct ram_put rp;
    int nr, rc;

    nr = xbox_StateGuestRanges(ranges, 16);
    if (nr > 16) {
        ranges = (XboxGuestRange *)malloc((size_t)nr * sizeof *ranges);
        if (!ranges)
            return XBOX_STATE_ENOMEM;
        nr = xbox_StateGuestRanges(ranges, nr);
    }
    memset(&rp, 0, sizeof rp);
    rp.cur = ranges;
    rp.ncur = nr;
    rp.check_only = check_only;
    rc = ram_walk(p, n, put_cb, &rp, NULL);
    if (ranges != stack_ranges)
        free(ranges);
    if (missing)
        *missing = rp.missing_va;
    if (written)
        *written = rp.written;
    if (rc < 0)
        return XBOX_STATE_ECORRUPT;
    return rc ? XBOX_STATE_ECHAIN : XBOX_STATE_OK;
}

int xbox_state_ram_precheck(const uint8_t *p, size_t n, char *why, size_t why_n)
{
    uint32_t missing = 0;
    int rc = ram_put(p, n, 1, &missing, NULL);
    if (rc == XBOX_STATE_ECHAIN && why && why_n)
        snprintf(why, why_n, "guest page 0x%08X of the state is not mapped now", missing);
    return rc;
}

static uint32_t s_ram_written;

int xbox_state_ram_load(const uint8_t *p, size_t n, uint32_t version, void *user)
{
    uint32_t missing = 0;
    int rc;
    (void)version; (void)user;

    rc = ram_put(p, n, 0, &missing, &s_ram_written);
    if (rc == XBOX_STATE_ECHAIN)
        xbox_StateSectionReason("guest page 0x%08X of the state is not mapped now", missing);
    /* The page hashes describe the memory before the load: the next capture compares against nothing. */
    xbox_state_ram_reset();
    return rc;
}

uint32_t xbox_state_ram_written(void)
{
    return s_ram_written;
}

/* ---- KOBJ: the kernel objects (XBA-246) --------------------------------------------------------------------- */

int xbox_state_kobj_save(XboxStateWriter *w, const XboxStateContext *ctx, void *user)
{
    XboxKobjRefusals why;
    void *blob = NULL;
    size_t len = 0;
    (void)ctx; (void)user;

    memset(&why, 0, sizeof why);
    if (!xbox_KobjSave(&blob, &len, XBOX_KOBJ_IN_SESSION, &why)) {
        if (why.count > 0)
            xbox_StateSectionReason("%d kernel object(s) cannot be saved now; first: %s 0x%08X: %s", why.count,
                                    why.r[0].what, why.r[0].token ? why.r[0].token : why.r[0].guest_va,
                                    why.r[0].reason);
        else
            xbox_StateSectionReason("kernel objects: out of memory");
        return XBOX_STATE_EBUSY;
    }
    xbox_StatePut(w, blob, len);
    free(blob);
    return XBOX_STATE_OK;
}

int xbox_state_kobj_check(const uint8_t *p, size_t n, uint32_t version, void *user)
{
    (void)user;
    if (version != XBOX_KOBJ_VERSION)
        return XBOX_STATE_EVERSION;
    if (n < 8 || memcmp(p, "XKOB", 4) != 0)
        return XBOX_STATE_ECORRUPT;
    if ((p[4] | p[5] << 8 | p[6] << 16 | (uint32_t)p[7] << 24) != XBOX_KOBJ_VERSION)
        return XBOX_STATE_EVERSION;
    return XBOX_STATE_OK;
}

int xbox_state_kobj_load(const uint8_t *p, size_t n, uint32_t version, void *user)
{
    char err[200];
    (void)version; (void)user;
    if (!xbox_KobjLoad(p, n, XBOX_KOBJ_IN_SESSION, err, sizeof err)) {
        xbox_StateSectionReason("kernel objects: %s", err[0] ? err : "refused");
        return XBOX_STATE_ESECTION;
    }
    return XBOX_STATE_OK;
}

/* ---- NV2A, APU, USB (XBA-248): user is the device's XdsSection ---------------------------------------------- */

int xbox_state_dev_save(XboxStateWriter *w, const XboxStateContext *ctx, void *user)
{
    const XdsSection *sec = (const XdsSection *)user;
    XdsWriter dw;
    int ok;
    (void)ctx;

    memset(&dw, 0, sizeof dw);
    ok = sec->capture(&dw) && !dw.err;
    if (ok)
        xbox_StatePut(w, dw.p, dw.len);
    else
        xbox_StateSectionReason("%s: the device could not be captured", sec->name);
    xds_writer_free(&dw);
    return ok ? XBOX_STATE_OK : XBOX_STATE_ESECTION;
}

int xbox_state_dev_load(const uint8_t *p, size_t n, uint32_t version, void *user)
{
    const XdsSection *sec = (const XdsSection *)user;
    XdsReader r = xds_reader(p, n);

    if (!sec->restore(&r, version) || r.err) {
        xbox_StateSectionReason("%s: the device refused its state (version %u, %zu bytes)", sec->name, version, n);
        return XBOX_STATE_ESECTION;
    }
    return XBOX_STATE_OK;
}
