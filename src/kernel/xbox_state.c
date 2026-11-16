/*
 * xbox_state.c - Xbox save states: the XBXS container, its sections, the capture and the read side
 * (XBA-245; format and API in xbox_state.h, design in docs/technical/xbox-save-states.md)
 *
 * A capture, with every guest thread frozen at a kernel boundary (xbox_ParkFreeze):
 *   1. settle: a thread frozen inside a non-blocking call is still running that call's bridge; wait (a few
 *      ms at most) until it has returned and is held at the call's end
 *   2. refuse what a state cannot hold: a thread held at DISPATCH_LEVEL, threads the park detector could
 *      not track; take the dispatch gate (when on) so no ISR or DPC runs guest code meanwhile
 *   3. read the park records, write every section, and check the records again: a call that returned
 *      during the capture (a wait satisfied) rewrote its record and maybe guest memory, so the capture is
 *      taken again (that thread is held now; a few attempts at most)
 * The built-in sections' save and check callbacks are xbox_state_guest.c.
 */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE   /* dl_iterate_phdr, dladdr */
#endif
#include "xbox_state.h"
#include "kernel.h"
#include "kernel_park.h"
#include "platform/host_time.h"
#include "platform/recomp_states.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <dlfcn.h>
#include <mach-o/loader.h>
#elif !defined(_WIN32)
#include <dlfcn.h>
#include <link.h>
#endif

#ifndef XBOXRECOMP_TOOLKIT_VERSION
#define XBOXRECOMP_TOOLKIT_VERSION "unknown"
#endif

/* Built-in sections (xbox_state_guest.c). */
int xbox_state_cpu_save(XboxStateWriter *w, const XboxStateContext *ctx, void *user);
int xbox_state_cpu_check(const uint8_t *p, size_t n, uint32_t version, void *user);
int xbox_state_mem_save(XboxStateWriter *w, const XboxStateContext *ctx, void *user);
int xbox_state_mem_check(const uint8_t *p, size_t n, uint32_t version, void *user);
int xbox_state_ram_save(XboxStateWriter *w, const XboxStateContext *ctx, void *user);
int xbox_state_ram_check(const uint8_t *p, size_t n, uint32_t version, void *user);
void xbox_state_ram_begin(void);
void xbox_state_ram_commit(XboxStateStats *st);
void xbox_state_ram_reset(void);

/* Each frozen thread can change its record once (its call returns, then the freeze holds it), so
 * threads + 1 attempts always end with one that saw no change; a few more for threads that start. */
#define EXTRA_ATTEMPTS 2
#define SETTLE_MS 50u
#define GATE_MS 50u

/* ---- Writer / reader ------------------------------------------------------------------------------------- */

uint8_t *xbox_StateReserve(XboxStateWriter *w, size_t n)
{
    uint8_t *p;

    if (w->failed)
        return NULL;
    if (n > w->cap - w->n) {
        size_t cap = w->cap ? w->cap : 4096;
        uint8_t *b;
        while (n > cap - w->n) {
            if (cap > ((size_t)-1) / 2) {
                w->failed = 1;
                return NULL;
            }
            cap *= 2;
        }
        b = (uint8_t *)realloc(w->buf, cap);
        if (!b) {
            w->failed = 1;
            return NULL;
        }
        w->buf = b;
        w->cap = cap;
    }
    p = w->buf + w->n;
    w->n += n;
    return p;
}

void xbox_StatePut(XboxStateWriter *w, const void *p, size_t n)
{
    uint8_t *d = xbox_StateReserve(w, n);
    if (d && n)
        memcpy(d, p, n);
}

static void le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void le64(uint8_t *p, uint64_t v)
{
    le32(p, (uint32_t)v);
    le32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t rd64(const uint8_t *p)
{
    return rd32(p) | (uint64_t)rd32(p + 4) << 32;
}

void xbox_StatePut32(XboxStateWriter *w, uint32_t v)
{
    uint8_t *d = xbox_StateReserve(w, 4);
    if (d)
        le32(d, v);
}

void xbox_StatePut64(XboxStateWriter *w, uint64_t v)
{
    uint8_t *d = xbox_StateReserve(w, 8);
    if (d)
        le64(d, v);
}

const uint8_t *xbox_StateGet(XboxStateReader *r, size_t n)
{
    const uint8_t *p;
    if (r->bad || r->at > r->n || n > r->n - r->at) {
        r->bad = 1;
        return NULL;
    }
    p = r->p + r->at;
    r->at += n;
    return p;
}

uint32_t xbox_StateGet32(XboxStateReader *r)
{
    const uint8_t *p = xbox_StateGet(r, 4);
    return p ? rd32(p) : 0;
}

uint64_t xbox_StateGet64(XboxStateReader *r)
{
    const uint8_t *p = xbox_StateGet(r, 8);
    return p ? rd64(p) : 0;
}

/* Four independent multiply-xorshift lanes over 32-byte blocks (memory bound on every host we run on),
 * then the tail and the length. */
uint64_t xbox_StateHash(const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    const uint64_t K = 0x9E3779B97F4A7C15ull;
    uint64_t l0 = 0x243F6A8885A308D3ull, l1 = 0x13198A2E03707344ull, l2 = 0xA4093822299F31D0ull,
             l3 = 0x082EFA98EC4E6C89ull, h, w;
    size_t i = 0;

    for (; i + 32 <= n; i += 32) {
        uint64_t a, b, c, d;
        memcpy(&a, p + i, 8);
        memcpy(&b, p + i + 8, 8);
        memcpy(&c, p + i + 16, 8);
        memcpy(&d, p + i + 24, 8);
        l0 = (l0 ^ a) * K; l0 ^= l0 >> 32;
        l1 = (l1 ^ b) * K; l1 ^= l1 >> 32;
        l2 = (l2 ^ c) * K; l2 ^= l2 >> 32;
        l3 = (l3 ^ d) * K; l3 ^= l3 >> 32;
    }
    h = l0 ^ (l1 * 3) ^ (l2 * 5) ^ (l3 * 7) ^ (uint64_t)n;
    for (; i < n; i += 8) {
        size_t k = n - i < 8 ? n - i : 8;
        w = 0;
        memcpy(&w, p + i, k);
        h = (h ^ w) * K;
        h ^= h >> 29;
    }
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 33;
    return h;
}

/* ---- Errors ---------------------------------------------------------------------------------------------- */

const char *xbox_StateError(int code)
{
    switch (code) {
    case XBOX_STATE_OK: return "ok";
    case XBOX_STATE_EEMPTY: return "empty slot";
    case XBOX_STATE_EGAME: return "another title's state";
    case XBOX_STATE_EVERSION: return "saved by another build";
    case XBOX_STATE_EIO: return "cannot read or write the file";
    case XBOX_STATE_ECORRUPT: return "not a state, or damaged";
    case XBOX_STATE_EFREEZE: return "a guest thread did not stop at a kernel call";
    case XBOX_STATE_EBUSY: return "not possible right now";
    case XBOX_STATE_ENOMEM: return "out of memory";
    case XBOX_STATE_ESECTION: return "a component could not save its state";
    case XBOX_STATE_EUNSTABLE: return "guest threads kept moving during the capture";
    default: return "error";
    }
}

#define REASON(buf, ...) snprintf((buf), sizeof(buf), __VA_ARGS__)

static void tag_str(uint32_t tag, char out[5])
{
    int i;
    for (i = 0; i < 4; i++) {
        char c = (char)(tag >> (8 * i));
        out[i] = c >= 0x20 && c < 0x7F ? c : '?';
    }
    out[4] = 0;
}

/* ---- Registry -------------------------------------------------------------------------------------------- */

static XboxStateSection s_sections[XBOX_STATE_MAX_SECTIONS];
static int s_nsections;
static int s_builtin_done;
static SRWLOCK s_reg_lock = SRWLOCK_INIT;

static int register_locked(const XboxStateSection *s)
{
    int i;
    for (i = 0; i < s_nsections; i++)
        if (s_sections[i].tag == s->tag) {
            s_sections[i] = *s;
            return 0;
        }
    if (s_nsections >= XBOX_STATE_MAX_SECTIONS)
        return -1;
    s_sections[s_nsections++] = *s;
    return 0;
}

static void builtin_locked(void)
{
    static const XboxStateSection builtin[] = {
        { XBOX_STATE_TAG_CPU, XBOX_STATE_CPU_VERSION, "cpu", xbox_state_cpu_save, xbox_state_cpu_check, NULL },
        { XBOX_STATE_TAG_MEM, XBOX_MEM_STATE_VERSION, "mem", xbox_state_mem_save, xbox_state_mem_check, NULL },
        { XBOX_STATE_TAG_RAM, XBOX_STATE_RAM_VERSION, "ram", xbox_state_ram_save, xbox_state_ram_check, NULL },
    };
    size_t i;
    if (s_builtin_done)
        return;
    s_builtin_done = 1;
    for (i = 0; i < sizeof builtin / sizeof builtin[0]; i++)
        register_locked(&builtin[i]);
}

static void ensure_builtin(void)
{
    AcquireSRWLockExclusive(&s_reg_lock);
    builtin_locked();
    ReleaseSRWLockExclusive(&s_reg_lock);
}

int xbox_StateRegister(const XboxStateSection *s)
{
    int r;
    if (!s || !s->tag || !s->save)
        return -1;
    AcquireSRWLockExclusive(&s_reg_lock);
    builtin_locked();
    r = register_locked(s);
    ReleaseSRWLockExclusive(&s_reg_lock);
    return r;
}

void xbox_StateUnregister(uint32_t tag)
{
    int i;
    AcquireSRWLockExclusive(&s_reg_lock);
    for (i = 0; i < s_nsections; i++)
        if (s_sections[i].tag == tag) {
            memmove(&s_sections[i], &s_sections[i + 1], (size_t)(s_nsections - i - 1) * sizeof s_sections[0]);
            s_nsections--;
            break;
        }
    ReleaseSRWLockExclusive(&s_reg_lock);
}

const XboxStateSection *xbox_StateFindSection(uint32_t tag)
{
    const XboxStateSection *r = NULL;
    int i;
    ensure_builtin();
    AcquireSRWLockShared(&s_reg_lock);
    for (i = 0; i < s_nsections; i++)
        if (s_sections[i].tag == tag)
            r = &s_sections[i];
    ReleaseSRWLockShared(&s_reg_lock);
    return r;
}

/* ---- Identity -------------------------------------------------------------------------------------------- */

static uint8_t s_build_override[32];
static size_t s_build_override_n;

void xbox_StateSetBuildId(const uint8_t *id, size_t n)
{
    if (n > sizeof s_build_override)
        n = sizeof s_build_override;
    if (id && n)
        memcpy(s_build_override, id, n);
    s_build_override_n = id ? n : 0;
}

#if defined(__APPLE__)
static size_t module_build_id(uint8_t out[32])
{
    Dl_info info;
    const struct mach_header_64 *mh;
    const uint8_t *cmd;
    uint32_t i;

    if (!dladdr((const void *)&module_build_id, &info) || !info.dli_fbase)
        return 0;
    mh = (const struct mach_header_64 *)info.dli_fbase;
    if (mh->magic != MH_MAGIC_64)
        return 0;
    cmd = (const uint8_t *)(mh + 1);
    for (i = 0; i < mh->ncmds; i++) {
        const struct load_command *lc = (const struct load_command *)cmd;
        if (lc->cmd == LC_UUID) {
            memcpy(out, ((const struct uuid_command *)lc)->uuid, 16);
            return 16;
        }
        cmd += lc->cmdsize;
    }
    return 0;
}
#elif !defined(_WIN32)
struct build_id_find {
    uintptr_t addr;
    uint8_t *out;
    size_t n;
};

static int build_id_cb(struct dl_phdr_info *info, size_t size, void *data)
{
    struct build_id_find *f = (struct build_id_find *)data;
    int i, mine = 0;
    (void)size;

    for (i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        uintptr_t lo = (uintptr_t)info->dlpi_addr + ph->p_vaddr;
        if (ph->p_type == PT_LOAD && f->addr >= lo && f->addr < lo + ph->p_memsz)
            mine = 1;
    }
    if (!mine)
        return 0;
    for (i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        const uint8_t *p, *end;
        if (ph->p_type != PT_NOTE)
            continue;
        p = (const uint8_t *)((uintptr_t)info->dlpi_addr + ph->p_vaddr);
        end = p + ph->p_memsz;
        while (p + 12 <= end) {
            uint32_t namesz = rd32(p), descsz = rd32(p + 4), type = rd32(p + 8);
            const uint8_t *name = p + 12, *desc = name + ((namesz + 3u) & ~3u);
            if (desc + descsz > end)
                break;
            if (type == 3 /* NT_GNU_BUILD_ID */ && namesz == 4 && memcmp(name, "GNU", 4) == 0 && descsz) {
                f->n = descsz > 32 ? 32 : descsz;
                memcpy(f->out, desc, f->n);
                return 1;
            }
            p = desc + ((descsz + 3u) & ~3u);
        }
    }
    return 1;   /* our module, no build ID */
}

static size_t module_build_id(uint8_t out[32])
{
    struct build_id_find f;
    f.addr = (uintptr_t)&module_build_id;
    f.out = out;
    f.n = 0;
    dl_iterate_phdr(build_id_cb, &f);
    return f.n;
}
#else
static size_t module_build_id(uint8_t out[32])
{
    (void)out;
    return 0;
}
#endif

void xbox_StateIdentityGet(XboxStateIdentity *out)
{
    static uint8_t mod_id[32];
    static size_t mod_n;
    static int mod_done;

    memset(out, 0, sizeof *out);
    out->title_id = xbox_path_title_id();
    memcpy(out->xbe_sha1, xbox_XbeHeaderSha1(), 20);
    if (s_build_override_n) {
        out->build_id_len = (uint8_t)s_build_override_n;
        memcpy(out->build_id, s_build_override, s_build_override_n);
    } else {
        if (!mod_done) {        /* the same answer every time: a benign race */
            mod_n = module_build_id(mod_id);
            mod_done = 1;
        }
        out->build_id_len = (uint8_t)mod_n;
        memcpy(out->build_id, mod_id, mod_n);
    }
    snprintf(out->toolkit, sizeof out->toolkit, "%s", XBOXRECOMP_TOOLKIT_VERSION);
    out->ram_size = (uint32_t)g_xbox_total_ram;
    out->mapped_size = (uint32_t)xbox_GetMappedSize();
    out->contig_size = XBOX_CONTIG_SIZE;
    out->cpu_bytes = (uint32_t)sizeof(RecompCpu);
}

/* Header layout (XBOX_STATE_HEADER_BYTES). */
enum {
    H_MAGIC = 0, H_VERSION = 4, H_HEADER = 8, H_COUNT = 12, H_TITLE = 16, H_SHA1 = 20, H_BUILD_N = 40,
    H_BUILD = 44, H_TOOLKIT = 76, H_RAM = 92, H_MAPPED = 96, H_CONTIG = 100, H_CPU = 104, H_SAVED = 108,
    H_HASH = 120
};

static void header_write(uint8_t *h, const XboxStateIdentity *id, uint32_t count, int64_t saved_ms)
{
    memset(h, 0, XBOX_STATE_HEADER_BYTES);
    memcpy(h + H_MAGIC, "XBXS", 4);
    le32(h + H_VERSION, XBOX_STATE_VERSION);
    le32(h + H_HEADER, XBOX_STATE_HEADER_BYTES);
    le32(h + H_COUNT, count);
    le32(h + H_TITLE, id->title_id);
    memcpy(h + H_SHA1, id->xbe_sha1, 20);
    h[H_BUILD_N] = id->build_id_len;
    memcpy(h + H_BUILD, id->build_id, 32);
    memcpy(h + H_TOOLKIT, id->toolkit, 16);
    le32(h + H_RAM, id->ram_size);
    le32(h + H_MAPPED, id->mapped_size);
    le32(h + H_CONTIG, id->contig_size);
    le32(h + H_CPU, id->cpu_bytes);
    le64(h + H_SAVED, (uint64_t)saved_ms);
    le64(h + H_HASH, xbox_StateHash(h, H_HASH));
}

/* ---- Capture --------------------------------------------------------------------------------------------- */

static volatile int32_t s_capturing;

static double ms_since(uint64_t t0)
{
    return (double)(xbox_HostNowNs() - t0) / 1e6;
}

static int64_t unix_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* A guest word from any range a state carries (the KTHREAD of a spawned thread is a pool object in the
 * contiguous window, outside the RAM view). 0 when no range holds it. */
static uint32_t guest_read32(uint32_t va)
{
    XboxGuestRange r[8];
    int n = xbox_StateGuestRanges(r, 8), i;
    uint32_t v = 0;

    if (n > 8)
        n = 8;
    for (i = 0; i < n; i++)
        if (va >= r[i].va && (uint64_t)va + 4 <= (uint64_t)r[i].va + r[i].size) {
            memcpy(&v, r[i].host + (va - r[i].va), 4);
            break;
        }
    return v;
}

int xbox_StateContextThread(const XboxStateContext *ctx, int i, XboxStateThread *out)
{
    const XboxParkRecord *r;

    if (!ctx || i < 0 || i >= ctx->threads)
        return -1;
    r = &ctx->records[i];
    memset(out, 0, sizeof *out);
    out->slot = r->slot;
    out->state = r->state;
    out->ordinal = r->ordinal;
    out->ret_va = r->ret_va;
    out->returned = r->returned;
    out->status = r->status;
    out->dispatch = r->dispatch;
    out->tid = (uint32_t)r->tid;
    out->kernel_calls = r->kernel_calls;
    memcpy(&out->cpu, r->cpu, sizeof(RecompCpu));
    out->tib = out->cpu.r_fs_base;
    /* fs:[0x28] names the KTHREAD (XBA-113); the runtime fills its StackBase (+0x1C), StackLimit (+0x20)
     * and UniqueThread (+0x12C), xbox_InitGuestKthread. fs:[4] is the TLS pointer here, not the stack. */
    if (out->tib)
        out->kthread = guest_read32(out->tib + 0x28);
    if (out->kthread) {
        out->stack_base = guest_read32(out->kthread + 0x1C);
        out->stack_limit = guest_read32(out->kthread + 0x20);
        out->guest_tid = guest_read32(out->kthread + 0x12C);
    }
    return 0;
}

/* One attempt: every section into w (the header first). */
static int capture_once(XboxStateWriter *w, const XboxStateContext *ctx, XboxStateStats *st)
{
    XboxStateIdentity id;
    XboxStateSection secs[XBOX_STATE_MAX_SECTIONS];
    int n, i;

    AcquireSRWLockShared(&s_reg_lock);
    n = s_nsections;
    memcpy(secs, s_sections, (size_t)n * sizeof secs[0]);
    ReleaseSRWLockShared(&s_reg_lock);

    xbox_StateIdentityGet(&id);
    w->n = 0;
    w->failed = 0;
    if (!xbox_StateReserve(w, XBOX_STATE_HEADER_BYTES))
        return XBOX_STATE_ENOMEM;
    xbox_state_ram_begin();
    for (i = 0; i < n; i++) {
        size_t head = w->n, len;
        int rc;
        char t[5];

        if (!xbox_StateReserve(w, XBOX_STATE_SECTION_HEAD))
            return XBOX_STATE_ENOMEM;
        rc = secs[i].save(w, ctx, secs[i].user);
        if (w->failed)
            return XBOX_STATE_ENOMEM;
        if (rc != XBOX_STATE_OK) {
            tag_str(secs[i].tag, t);
            REASON(st->reason, "section %s (%s) failed: %s", t, secs[i].name ? secs[i].name : "?",
                   xbox_StateError(rc));
            return XBOX_STATE_ESECTION;
        }
        len = w->n - head - XBOX_STATE_SECTION_HEAD;
        le32(w->buf + head, secs[i].tag);
        le32(w->buf + head + 4, secs[i].version);
        le64(w->buf + head + 8, (uint64_t)len);
        le64(w->buf + head + 16, xbox_StateHash(w->buf + head + XBOX_STATE_SECTION_HEAD, len));
        if (secs[i].tag == XBOX_STATE_TAG_RAM) st->ram_bytes = len;
        else if (secs[i].tag == XBOX_STATE_TAG_MEM) st->mem_bytes = len;
        else if (secs[i].tag == XBOX_STATE_TAG_CPU) st->cpu_bytes = len;
    }
    header_write(w->buf, &id, (uint32_t)n, unix_ms());
    return XBOX_STATE_OK;
}

static size_t s_last_blob;   /* the previous capture's size: the next one's first allocation */

int xbox_StateCaptureFrozen(uint8_t **blob, size_t *blob_n, XboxStateStats *st)
{
    static XboxParkRecord recs[XBOX_PARK_MAX_THREADS];   /* one capture at a time (s_capturing) */
    XboxStateStats local;
    XboxStateContext ctx;
    XboxStateWriter w;
    uint64_t t0;
    int n = 0, i, rc = XBOX_STATE_OK, attempt, gate;

    if (!st)
        st = &local;
    {   /* keep what xbox_StateCapture measured before */
        double fz = st->freeze_ms;
        memset(st, 0, sizeof *st);
        st->freeze_ms = fz;
    }
    *blob = NULL;
    *blob_n = 0;
    ensure_builtin();

    /* 1. Settle: calls in progress in a bridge that does not block return and are held at their end. */
    t0 = xbox_HostNowNs();
    for (;;) {
        int busy = 0;
        n = xbox_ParkRecords(recs, XBOX_PARK_MAX_THREADS);
        for (i = 0; i < n; i++)
            if (recs[i].state == XBOX_PARK_RUNNING
                || (recs[i].state == XBOX_PARK_KERNEL && !recs[i].returned))
                busy++;
        if (!busy || ms_since(t0) >= (double)SETTLE_MS)
            break;
        SwitchToThread();
    }
    st->settle_ms = ms_since(t0);

    /* 2. What a state cannot hold. */
    for (i = 0; i < n; i++) {
        if (recs[i].state == XBOX_PARK_RUNNING) {
            REASON(st->reason, "guest thread %lu is running (not frozen)", recs[i].tid);
            return XBOX_STATE_EFREEZE;
        }
        if (recs[i].dispatch) {
            REASON(st->reason, "guest thread %lu is held at DISPATCH_LEVEL (in ordinal %u from 0x%08X)",
                   recs[i].tid, recs[i].ordinal, recs[i].ret_va);
            return XBOX_STATE_EBUSY;
        }
    }
    if (xbox_ParkDropped() > 0) {
        REASON(st->reason, "%d guest thread(s) started untracked (more than %d at once)", xbox_ParkDropped(),
               XBOX_PARK_MAX_THREADS);
        return XBOX_STATE_EBUSY;
    }
    t0 = xbox_HostNowNs();
    while (!(gate = xbox_DispatchGateTryEnter()) && ms_since(t0) < (double)GATE_MS)
        SwitchToThread();
    if (!gate) {
        REASON(st->reason, "the dispatch gate is held (by host thread %lu)", xbox_DispatchGateOwner());
        return XBOX_STATE_EBUSY;
    }

    /* 3. Capture until no record changed meanwhile. */
    memset(&w, 0, sizeof w);
    if (s_last_blob)
        xbox_StateReserve(&w, s_last_blob + (s_last_blob >> 4)), w.n = 0;
    t0 = xbox_HostNowNs();
    for (attempt = 1; attempt <= n + 1 + EXTRA_ATTEMPTS; attempt++) {
        n = xbox_ParkRecords(recs, XBOX_PARK_MAX_THREADS);
        ctx.records = recs;
        ctx.threads = n;
        st->attempts = attempt;
        st->threads = n;
        rc = capture_once(&w, &ctx, st);
        if (rc != XBOX_STATE_OK)
            break;
        if (!xbox_ParkRecordsChanged(recs, n))
            break;
        rc = XBOX_STATE_EUNSTABLE;
    }
    st->capture_ms = ms_since(t0);
    xbox_DispatchGateLeave();

    if (rc == XBOX_STATE_EUNSTABLE)
        REASON(st->reason, "a kernel call returned during each of %d attempts", st->attempts);
    else if (rc == XBOX_STATE_ENOMEM)
        REASON(st->reason, "out of memory at %zu bytes", w.n);
    if (rc != XBOX_STATE_OK) {
        free(w.buf);
        return rc;
    }
    xbox_state_ram_commit(st);
    st->blob_bytes = w.n;
    s_last_blob = w.n;
    *blob = w.buf;
    *blob_n = w.n;
    return XBOX_STATE_OK;
}

static void log_capture(int rc, const XboxStateStats *st)
{
    if (rc == XBOX_STATE_OK)
        fprintf(stderr, "  [STATE] capture: %d guest thread%s, RAM %u of %u pages stored (%.1f MB, %u changed"
                        " since the last capture) in %u range%s, blob %.2f MB; %.2f ms (freeze %.2f, settle %.2f,"
                        " sections %.2f, %d attempt%s)\n",
                st->threads, st->threads == 1 ? "" : "s", st->ram_stored, st->ram_pages,
                st->ram_stored / 256.0, st->ram_changed, st->ram_ranges, st->ram_ranges == 1 ? "" : "s",
                st->blob_bytes / 1048576.0, st->freeze_ms + st->settle_ms + st->capture_ms, st->freeze_ms,
                st->settle_ms, st->capture_ms, st->attempts, st->attempts == 1 ? "" : "s");
    else
        fprintf(stderr, "  [STATE] capture refused: %s (%s)\n", xbox_StateError(rc),
                st->reason[0] ? st->reason : "-");
    fflush(stderr);
}

int xbox_StateCapture(uint32_t timeout_ms, uint8_t **blob, size_t *blob_n, XboxStateStats *st)
{
    XboxStateStats local;
    XboxParkStats ps;
    uint64_t t0;
    int rc, k;

    if (!st)
        st = &local;
    memset(st, 0, sizeof *st);
    *blob = NULL;
    *blob_n = 0;
    if (xbox_ParkThisThreadIsGuest()) {
        REASON(st->reason, "called from a guest thread");
        rc = XBOX_STATE_EBUSY;
        goto out;
    }
    if (InterlockedCompareExchange((volatile LONG *)&s_capturing, 1, 0) != 0) {
        REASON(st->reason, "a capture is already running");
        rc = XBOX_STATE_EBUSY;
        goto out;
    }
    t0 = xbox_HostNowNs();
    if (!xbox_ParkFreeze(timeout_ms, &ps)) {
        st->freeze_ms = ms_since(t0);
        REASON(st->reason, "no freeze within %u ms", timeout_ms);
        for (k = 0; k < ps.threads; k++)
            if (ps.t[k].state == XBOX_PARK_RUNNING) {
                REASON(st->reason, "guest thread %lu did not reach a kernel call within %u ms (it last called"
                       " ordinal %u from 0x%08X)", ps.t[k].tid, timeout_ms, ps.t[k].ordinal, ps.t[k].ret_va);
                break;
            }
        InterlockedExchange((volatile LONG *)&s_capturing, 0);
        rc = XBOX_STATE_EFREEZE;
        goto out;
    }
    st->freeze_ms = ms_since(t0);
    rc = xbox_StateCaptureFrozen(blob, blob_n, st);
    xbox_ParkThaw();
    InterlockedExchange((volatile LONG *)&s_capturing, 0);
out:
    log_capture(rc, st);
    return rc;
}

/* ---- Reading --------------------------------------------------------------------------------------------- */

int xbox_StateParseHeader(const uint8_t *b, size_t n, XboxStateInfo *info)
{
    XboxStateInfo local;
    uint32_t hb;

    if (!info)
        info = &local;
    memset(info, 0, sizeof *info);
    if (!b || n < XBOX_STATE_HEADER_BYTES || memcmp(b, "XBXS", 4) != 0) {
        REASON(info->reason, "not an Xbox state");
        return XBOX_STATE_ECORRUPT;
    }
    info->version = rd32(b + H_VERSION);
    if (info->version != XBOX_STATE_VERSION) {
        REASON(info->reason, "format version %u (this build reads %u)", info->version, XBOX_STATE_VERSION);
        return XBOX_STATE_EVERSION;
    }
    hb = rd32(b + H_HEADER);
    if (hb != XBOX_STATE_HEADER_BYTES || rd64(b + H_HASH) != xbox_StateHash(b, H_HASH)) {
        REASON(info->reason, "header damaged");
        return XBOX_STATE_ECORRUPT;
    }
    info->id.title_id = rd32(b + H_TITLE);
    memcpy(info->id.xbe_sha1, b + H_SHA1, 20);
    info->id.build_id_len = b[H_BUILD_N] > 32 ? 32 : b[H_BUILD_N];
    memcpy(info->id.build_id, b + H_BUILD, 32);
    memcpy(info->id.toolkit, b + H_TOOLKIT, 16);
    info->id.toolkit[15] = 0;
    info->id.ram_size = rd32(b + H_RAM);
    info->id.mapped_size = rd32(b + H_MAPPED);
    info->id.contig_size = rd32(b + H_CONTIG);
    info->id.cpu_bytes = rd32(b + H_CPU);
    info->saved_ms = (int64_t)rd64(b + H_SAVED);
    info->sections = (int)rd32(b + H_COUNT);
    if (info->sections < 0 || info->sections > XBOX_STATE_MAX_SECTIONS) {
        REASON(info->reason, "%u sections", rd32(b + H_COUNT));
        info->sections = 0;
        return XBOX_STATE_ECORRUPT;
    }
    return XBOX_STATE_OK;
}

static int identity_check(const XboxStateIdentity *s, XboxStateInfo *info)
{
    XboxStateIdentity cur;

    xbox_StateIdentityGet(&cur);
    if (s->title_id != cur.title_id) {
        REASON(info->reason, "title %08X (running %08X)", s->title_id, cur.title_id);
        return XBOX_STATE_EGAME;
    }
    if (memcmp(s->xbe_sha1, cur.xbe_sha1, 20) != 0) {
        REASON(info->reason, "another XBE of title %08X", s->title_id);
        return XBOX_STATE_EGAME;
    }
    if (s->build_id_len != cur.build_id_len || memcmp(s->build_id, cur.build_id, cur.build_id_len) != 0) {
        REASON(info->reason, "another build of the generated code");
        return XBOX_STATE_EVERSION;
    }
    if (strncmp(s->toolkit, cur.toolkit, sizeof cur.toolkit) != 0) {
        REASON(info->reason, "toolkit %s (running %s)", s->toolkit, cur.toolkit);
        return XBOX_STATE_EVERSION;
    }
    if (s->ram_size != cur.ram_size || s->mapped_size != cur.mapped_size || s->contig_size != cur.contig_size) {
        REASON(info->reason, "memory %u / %u / %u MB (running %u / %u / %u MB)", s->ram_size >> 20,
               s->mapped_size >> 20, s->contig_size >> 20, cur.ram_size >> 20, cur.mapped_size >> 20,
               cur.contig_size >> 20);
        return XBOX_STATE_EVERSION;
    }
    if (s->cpu_bytes != cur.cpu_bytes) {
        REASON(info->reason, "register set of %u bytes (this build: %u)", s->cpu_bytes, cur.cpu_bytes);
        return XBOX_STATE_EVERSION;
    }
    return XBOX_STATE_OK;
}

int xbox_StateCheck(const uint8_t *b, size_t n, XboxStateInfo *info)
{
    XboxStateInfo local;
    XboxStateSection secs[XBOX_STATE_MAX_SECTIONS];
    int seen[XBOX_STATE_MAX_SECTIONS];
    int nsec, rc, i, k;
    size_t at;
    char t[5];

    if (!info)
        info = &local;
    rc = xbox_StateParseHeader(b, n, info);
    if (rc != XBOX_STATE_OK)
        return rc;
    rc = identity_check(&info->id, info);
    if (rc != XBOX_STATE_OK)
        return rc;

    ensure_builtin();
    AcquireSRWLockShared(&s_reg_lock);
    nsec = s_nsections;
    memcpy(secs, s_sections, (size_t)nsec * sizeof secs[0]);
    ReleaseSRWLockShared(&s_reg_lock);
    memset(seen, 0, sizeof seen);

    at = XBOX_STATE_HEADER_BYTES;
    for (i = 0; i < info->sections; i++) {
        uint32_t tag, ver;
        uint64_t len;
        const uint8_t *p;

        if (n - at < XBOX_STATE_SECTION_HEAD) {
            REASON(info->reason, "truncated at section %d", i);
            return XBOX_STATE_ECORRUPT;
        }
        tag = rd32(b + at);
        ver = rd32(b + at + 4);
        len = rd64(b + at + 8);
        tag_str(tag, t);
        info->section[i].tag = tag;
        info->section[i].version = ver;
        info->section[i].length = len;
        if (len > n - at - XBOX_STATE_SECTION_HEAD) {
            REASON(info->reason, "section %s truncated", t);
            return XBOX_STATE_ECORRUPT;
        }
        p = b + at + XBOX_STATE_SECTION_HEAD;
        if (rd64(b + at + 16) != xbox_StateHash(p, (size_t)len)) {
            REASON(info->reason, "section %s damaged (hash)", t);
            return XBOX_STATE_ECORRUPT;
        }
        for (k = 0; k < nsec && secs[k].tag != tag; k++)
            ;
        if (k == nsec) {
            REASON(info->reason, "section %s unknown to this build", t);
            return XBOX_STATE_EVERSION;
        }
        if (seen[k]++) {
            REASON(info->reason, "section %s twice", t);
            return XBOX_STATE_ECORRUPT;
        }
        if (secs[k].version != ver) {
            REASON(info->reason, "section %s version %u (this build: %u)", t, ver, secs[k].version);
            return XBOX_STATE_EVERSION;
        }
        if (secs[k].check) {
            rc = secs[k].check(p, (size_t)len, ver, secs[k].user);
            if (rc != XBOX_STATE_OK) {
                REASON(info->reason, "section %s refused: %s", t, xbox_StateError(rc));
                return rc == XBOX_STATE_EVERSION ? rc : XBOX_STATE_ECORRUPT;
            }
        }
        at += XBOX_STATE_SECTION_HEAD + (size_t)len;
    }
    if (at != n) {
        REASON(info->reason, "%zu bytes after the last section", n - at);
        return XBOX_STATE_ECORRUPT;
    }
    for (k = 0; k < nsec; k++)
        if (!seen[k]) {
            tag_str(secs[k].tag, t);
            REASON(info->reason, "section %s missing (saved by another build)", t);
            return XBOX_STATE_EVERSION;
        }
    info->reason[0] = 0;
    return XBOX_STATE_OK;
}

int xbox_StateSection(const uint8_t *b, size_t n, uint32_t tag, const uint8_t **p, size_t *len,
                      uint32_t *version)
{
    uint32_t count, i;
    size_t at = XBOX_STATE_HEADER_BYTES;

    if (!b || n < XBOX_STATE_HEADER_BYTES || memcmp(b, "XBXS", 4) != 0)
        return XBOX_STATE_ECORRUPT;
    count = rd32(b + H_COUNT);
    for (i = 0; i < count; i++) {
        uint64_t l;
        if (n - at < XBOX_STATE_SECTION_HEAD)
            return XBOX_STATE_ECORRUPT;
        l = rd64(b + at + 8);
        if (l > n - at - XBOX_STATE_SECTION_HEAD)
            return XBOX_STATE_ECORRUPT;
        if (rd32(b + at) == tag) {
            if (p) *p = b + at + XBOX_STATE_SECTION_HEAD;
            if (len) *len = (size_t)l;
            if (version) *version = rd32(b + at + 4);
            return XBOX_STATE_OK;
        }
        at += XBOX_STATE_SECTION_HEAD + (size_t)l;
    }
    return XBOX_STATE_EEMPTY;
}

/* ---- Slot files ------------------------------------------------------------------------------------------ */

int xbox_StateWriteSlot(const char *path, const uint8_t *blob, size_t n, const uint16_t *thumb, int tw, int th)
{
    recomp_state_meta meta;

    memset(&meta, 0, sizeof meta);
    snprintf(meta.console, sizeof meta.console, "xbox");
    snprintf(meta.key, sizeof meta.key, "%08X", xbox_path_title_id());
    meta.thumb_w = thumb ? tw : 0;
    meta.thumb_h = thumb ? th : 0;
    return recomp_states_write(path, &meta, thumb, blob, n) == RECOMP_STATE_OK ? XBOX_STATE_OK : XBOX_STATE_EIO;
}

int xbox_StateSaveSlot(const char *path, uint32_t timeout_ms, const uint16_t *thumb, int tw, int th,
                       XboxStateStats *st)
{
    uint8_t *blob;
    size_t n;
    int rc = xbox_StateCapture(timeout_ms, &blob, &n, st);

    if (rc != XBOX_STATE_OK)
        return rc;
    rc = xbox_StateWriteSlot(path, blob, n, thumb, tw, th);
    free(blob);
    return rc;
}

int xbox_StateReadSlot(const char *path, uint8_t **blob, size_t *n, XboxStateInfo *info)
{
    XboxStateInfo local;
    recomp_state_meta meta;
    uint8_t *b = NULL;
    size_t bn = 0;
    int rc;

    if (!info)
        info = &local;
    memset(info, 0, sizeof *info);
    if (blob)
        *blob = NULL;
    if (n)
        *n = 0;
    rc = recomp_states_read(path, &meta, NULL, &b, &bn);
    if (rc != RECOMP_STATE_OK) {
        REASON(info->reason, "slot file: %s", recomp_states_error(rc));
        return rc;
    }
    if (strcmp(meta.console, "xbox") != 0) {
        REASON(info->reason, "a %s state", meta.console);
        free(b);
        return XBOX_STATE_EGAME;
    }
    rc = xbox_StateCheck(b, bn, info);
    if (rc != XBOX_STATE_OK || !blob) {
        free(b);
        return rc;
    }
    *blob = b;
    if (n)
        *n = bn;
    return XBOX_STATE_OK;
}

/* ---- The measurement probe ------------------------------------------------------------------------------- */

static void *probe_main(void *arg)
{
    unsigned period = (unsigned)(uintptr_t)arg;
    const char *dir = getenv("RECOMP_STATE_PROBE_DIR");
    char path[1024];

    if (dir && *dir)
        snprintf(path, sizeof path, "%s/probe.st", dir);
    for (;;) {
        uint8_t *blob = NULL;
        size_t n = 0;
        XboxStateStats st;
        sleep(period);
        if (xbox_StateCapture(200, &blob, &n, &st) != XBOX_STATE_OK)
            continue;
        if (dir && *dir) {
            XboxStateInfo info;
            uint64_t t0 = xbox_HostNowNs();
            int wr = xbox_StateWriteSlot(path, blob, n, NULL, 0, 0), rd;
            double wms = ms_since(t0);
            t0 = xbox_HostNowNs();
            rd = wr == XBOX_STATE_OK ? xbox_StateReadSlot(path, NULL, NULL, &info) : wr;
            fprintf(stderr, "  [STATE] probe: %s written in %.1f ms (%s), read back and checked in %.1f ms: %s%s%s\n",
                    path, wms, xbox_StateError(wr), ms_since(t0), xbox_StateError(rd),
                    rd != XBOX_STATE_OK ? " -- " : "", rd != XBOX_STATE_OK ? info.reason : "");
            fflush(stderr);
        }
        free(blob);
    }
    return NULL;
}

void xbox_StateProbeStart(void)
{
    static int started;
    const char *e = getenv("RECOMP_STATE_PROBE");
    int period = e ? atoi(e) : 0;
    pthread_t t;

    if (period <= 0 || started)
        return;
    started = 1;
    if (pthread_create(&t, NULL, probe_main, (void *)(uintptr_t)(unsigned)period) == 0) {
        pthread_detach(t);
        fprintf(stderr, "  [STATE] probe: a capture every %d s (RECOMP_STATE_PROBE)\n", period);
    }
}

void xbox_StateReset(void)
{
    AcquireSRWLockExclusive(&s_reg_lock);
    s_nsections = 0;
    s_builtin_done = 0;
    builtin_locked();
    ReleaseSRWLockExclusive(&s_reg_lock);
    xbox_state_ram_reset();
    s_last_blob = 0;
}
