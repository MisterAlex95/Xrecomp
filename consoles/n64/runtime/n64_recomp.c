/* n64_recomp.c -- see n64_recomp.h and n64_recomp_rt.h (XBA-169 / XBA-170 / XBA-171): attaching a pack's table, the
 * run loop that dispatches to lifted code, the content check of RDRAM code (direct-mapped and TLB-mapped), the record
 * of code the interpreter had to run (discovery loop), and the state comparison of lockstep runs.
 *
 * Dispatch: entries are functions (virtual address, length, hash). A page index (4 KB virtual pages -> entries
 * crossing them) finds the entries that hold a PC; a direct-mapped cache remembers the answer per PC. An entry runs
 * only while its code is in memory: its "check sum" is the sum of the write counters of the RDRAM pages it covers
 * (physical: for a TLB-mapped entry, the page its virtual page maps to now); when that sum moved since the entry was
 * last hashed, or the entry now maps elsewhere, the words are hashed again and compared with the table's hash. The
 * loop keeps the interpreter's order of checks (scheduler, interrupt) between every two steps, lifted or interpreted,
 * so both run tick for tick alike. */
#define _POSIX_C_SOURCE 200809L
#include "n64_internal.h"
#include "n64_recomp_rt.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* n64_rc_head is the start of n64_machine, n64_rc_cpu the start of n64_cpu. */
#define HEAD_AT(f) _Static_assert(offsetof(n64_machine, f) == offsetof(n64_rc_head, f), "n64_rc_head." #f)
#define CPU_AT(f)                                                                                                    \
    _Static_assert(offsetof(n64_machine, cpu) + offsetof(n64_cpu, f) == offsetof(n64_rc_head, cpu.f),                \
                   "n64_rc_cpu." #f)
HEAD_AT(now);
HEAD_AT(stop_at);
HEAD_AT(instructions);
HEAD_AT(rdram);
HEAD_AT(rdram_gen);
HEAD_AT(rdram_size);
HEAD_AT(mi_mode);
HEAD_AT(rc_lo);
HEAD_AT(rc_len);
HEAD_AT(rc_hit);
HEAD_AT(rc_exit);
HEAD_AT(rc_native);
HEAD_AT(cpu);
CPU_AT(gpr);
CPU_AT(hi);
CPU_AT(lo);
CPU_AT(pc);
CPU_AT(npc);
CPU_AT(cur);
CPU_AT(ds);
CPU_AT(next_ds);
CPU_AT(llbit);
CPU_AT(irq);
CPU_AT(ksu);
CPU_AT(addr64);
CPU_AT(ops64);
CPU_AT(cop0);
CPU_AT(fpr);
CPU_AT(fcsr);
_Static_assert(N64_PAGE_SHIFT == 12, "lifted stores bump rdram_gen[offset >> 12]");
_Static_assert(N64_RC_CPI_CACHED == COST_CACHED && N64_RC_CPI_UNCACHED == COST_UNCACHED_RAM, "fetch costs");
_Static_assert(N64_RC_COST_MULT == COST_MULT && N64_RC_COST_DMULT == COST_DMULT && N64_RC_COST_DIV == COST_DIV &&
                   N64_RC_COST_DDIV == COST_DDIV,
               "multiply / divide costs");

#define CACHE_BITS 14
#define CACHE_SIZE (1u << CACHE_BITS)
#define MISSED_MAX 8192u /* distinct records at most */
#define KEY_NONE 0xFFFFFFFFu
#define PAGE 4096u

enum { V_UNKNOWN = 0, V_OK = 1, V_BAD = 2 };
enum { E_NONE = -1, E_STALE = -2 };

typedef struct rc_page_ref {
    uint32_t page; /* virtual address >> 12 */
    uint32_t e;
} rc_page_ref;

typedef struct rc_line {
    uint32_t key;  /* pc (32-bit virtual), KEY_NONE: empty */
    int32_t e;     /* entry index, E_NONE: nothing covers pc, E_STALE: candidates whose code is not in memory */
    uint32_t tg;   /* mapped pc: the TLB generation the translation was made under (and ERL in bit 31) */
    uint32_t pbase; /* the physical address the entry's start maps to (E_STALE: unused) */
    uint32_t p0, p1, sum; /* E_STALE: the RDRAM pages of the candidates and their counter sum then */
} rc_line;

typedef struct rc_missed {
    uint32_t addr;
    uint32_t n;      /* words */
    uint64_t hash;
    uint32_t *words; /* big-endian values */
    bool owner;      /* words is this record's copy (else a view into an earlier record's) */
} rc_missed;

struct n64_rc_state {
    const n64_recomp_table *table;
    uint32_t n;
    uint32_t *vsum;             /* per entry: page counter sum when last hashed */
    uint32_t *vpa;              /* per entry: the physical address it was hashed at */
    uint8_t *vstate;            /* V_* */
    uint8_t *mapped;            /* per entry: 1 for a TLB-mapped address */
    rc_page_ref *refs;
    uint32_t nrefs;
    bool booted;                /* the CPU reached the table's boot_pc (IPL3 is done) */
    uint64_t interp_pc;         /* lifted code handed this instruction to the interpreter */
    uint64_t expect;            /* the instruction after the last interpreted RDRAM step */
    n64_recomp_stats stats;
    rc_missed *missed;
    uint32_t nmissed;
    uint64_t *missed_set;       /* (addr, hash) keys, open addressing, 2 * MISSED_MAX slots, 0 = empty */
    uint32_t *var_addr;         /* address -> contents recorded there (N64_RECOMP_VARIANTS at most), same layout */
    uint8_t *var_cnt;
    rc_line cache[CACHE_SIZE];
};

uint64_t n64_recomp_hash(const uint32_t *words, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; i++) {
        h ^= words[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

static uint64_t hash_ram(const uint8_t *ram, uint32_t off, uint32_t len)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t i = 0; i < len; i += 4) {
        h ^= n64_be32(ram + off + i);
        h *= 0x100000001B3ull;
    }
    return h;
}

static void flush_cache(struct n64_rc_state *s)
{
    for (uint32_t i = 0; i < CACHE_SIZE; i++)
        s->cache[i].key = KEY_NONE;
    s->interp_pc = 1; /* never a PC */
    s->expect = 1;
}

static void drop_table(struct n64_rc_state *s)
{
    free(s->vsum);
    free(s->vpa);
    free(s->vstate);
    free(s->mapped);
    free(s->refs);
    s->vsum = s->vpa = NULL;
    s->vstate = s->mapped = NULL;
    s->refs = NULL;
    s->nrefs = s->n = 0;
    s->table = NULL;
}

static struct n64_rc_state *state(n64_machine *m)
{
    if (!m->rc) {
        struct n64_rc_state *s = calloc(1, sizeof *s);
        if (!s)
            return NULL;
        s->missed = calloc(MISSED_MAX, sizeof *s->missed);
        s->missed_set = calloc(2 * MISSED_MAX, sizeof *s->missed_set);
        s->var_addr = calloc(2 * MISSED_MAX, sizeof *s->var_addr);
        s->var_cnt = calloc(2 * MISSED_MAX, 1);
        if (!s->missed || !s->missed_set || !s->var_addr || !s->var_cnt) {
            free(s->missed);
            free(s->missed_set);
            free(s->var_addr);
            free(s->var_cnt);
            free(s);
            return NULL;
        }
        flush_cache(s);
        m->rc = s;
    }
    return m->rc;
}

static int cmp_ref(const void *a, const void *b)
{
    const rc_page_ref *x = a, *y = b;
    if (x->page != y->page)
        return x->page < y->page ? -1 : 1;
    return x->e < y->e ? -1 : x->e > y->e;
}

static inline bool is_direct(uint32_t va) { return va - 0x80000000u < 0x40000000u; } /* KSEG0 / KSEG1 */

static bool entry_ok(const n64_machine *m, const n64_recomp_entry *e)
{
    if (!e->fn || !e->len || (e->addr & 3) || (e->len & 3) || e->len > 0x100000u)
        return false;
    if ((uint64_t)e->addr + e->len > 0x100000000ull)
        return false;
    if (is_direct(e->addr)) {
        uint32_t pa = e->addr & 0x1FFFFFFFu;
        return pa < m->rdram_size && pa + e->len <= m->rdram_size && is_direct(e->addr + e->len - 1);
    }
    return (e->addr & ~(PAGE - 1)) == ((e->addr + e->len - 1) & ~(PAGE - 1)); /* mapped: within one page */
}

bool n64_recomp_active(const n64_machine *m) { return m->rc && m->rc->table; }

int n64_attach_recomp(n64_machine *m, const n64_recomp_table *t)
{
    if (t && (t->abi != N64_RECOMP_ABI || (t->count && !t->entries)))
        return -1;
    if (!t && !m->rc)
        return 0;
    struct n64_rc_state *s = state(m);
    if (!s)
        return -1;
    drop_table(s);
    flush_cache(s);
    if (!t)
        return 0; /* the record of missed code stays */
    uint32_t n = t->count, nrefs = 0;
    for (uint32_t i = 0; i < n; i++)
        if (entry_ok(m, &t->entries[i]))
            nrefs += ((t->entries[i].addr + t->entries[i].len - 1) >> 12) - (t->entries[i].addr >> 12) + 1;
    s->vsum = calloc(n ? n : 1, sizeof *s->vsum);
    s->vpa = calloc(n ? n : 1, sizeof *s->vpa);
    s->vstate = calloc(n ? n : 1, 1);
    s->mapped = calloc(n ? n : 1, 1);
    s->refs = malloc((nrefs ? nrefs : 1) * sizeof *s->refs);
    if (!s->vsum || !s->vpa || !s->vstate || !s->mapped || !s->refs) {
        drop_table(s);
        return -1;
    }
    /* Debugging: N64_RC_ENTRIES=LO-HI attaches only the entries LO..HI (indices), to bisect a divergence. */
    uint32_t only_lo = 0, only_hi = 0xFFFFFFFFu;
    const char *only = getenv("N64_RC_ENTRIES");
    if (only && *only) {
        char *end = NULL;
        only_lo = (uint32_t)strtoul(only, &end, 0);
        only_hi = (end && *end == '-') ? (uint32_t)strtoul(end + 1, NULL, 0) : only_lo;
    }
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        const n64_recomp_entry *e = &t->entries[i];
        if (!entry_ok(m, e) || i < only_lo || i > only_hi) {
            s->vstate[i] = V_BAD;
            continue;
        }
        s->mapped[i] = !is_direct(e->addr);
        for (uint32_t pg = e->addr >> 12; pg <= (e->addr + e->len - 1) >> 12; pg++)
            s->refs[k++] = (rc_page_ref){pg, i};
    }
    qsort(s->refs, k, sizeof *s->refs, cmp_ref);
    s->nrefs = k;
    s->n = n;
    s->table = t;
    s->booted = false;
    return 0;
}

void n64_recomp_reset(n64_machine *m)
{
    struct n64_rc_state *s = m->rc;
    if (!s)
        return;
    flush_cache(s);
    s->booted = false;
    for (uint32_t i = 0; i < s->n; i++)
        if (s->vstate[i] == V_OK)
            s->vstate[i] = V_UNKNOWN;
}

void n64_recomp_free(n64_machine *m)
{
    struct n64_rc_state *s = m->rc;
    if (!s)
        return;
    drop_table(s);
    for (uint32_t i = 0; i < s->nmissed; i++)
        if (s->missed[i].owner)
            free(s->missed[i].words);
    free(s->missed);
    free(s->missed_set);
    free(s->var_addr);
    free(s->var_cnt);
    free(s);
    m->rc = NULL;
}

/* ---- translation and the content check ---- */

/* The translation key of a mapped address: the TLB generation and ERL (which unmaps KUSEG). */
static inline uint32_t tlb_key(const n64_machine *m)
{
    return (m->cpu.tlb_gen & 0x7FFFFFFFu) | ((m->cpu.cop0[C0_STATUS] & SR_ERL) ? 0x80000000u : 0);
}

/* The physical RDRAM address of virtual va (kernel mode), or false (unmapped, not RDRAM). */
static inline bool translate(n64_machine *m, uint32_t va, uint32_t *pa)
{
    if (is_direct(va)) {
        *pa = va & 0x1FFFFFFFu;
    } else if (n64_cpu_probe(m, (uint64_t)(int64_t)(int32_t)va, pa) != 0) {
        return false;
    }
    return *pa < m->rdram_size;
}

static inline uint32_t page_sum(const n64_machine *m, uint32_t p0, uint32_t p1)
{
    uint32_t sum = 0;
    for (uint32_t p = p0; p <= p1; p++)
        sum += m->rdram_gen[p];
    return sum;
}

/* 1 when entry i's code is in RDRAM at pa (the physical address its start maps to now). */
static inline bool entry_valid(n64_machine *m, struct n64_rc_state *s, uint32_t i, uint32_t pa)
{
    const n64_recomp_entry *e = &s->table->entries[i];
    if (pa + e->len > m->rdram_size)
        return false;
    uint32_t sum = page_sum(m, pa >> 12, (pa + e->len - 1) >> 12);
    if (s->vstate[i] != V_UNKNOWN && s->vsum[i] == sum && s->vpa[i] == pa)
        return s->vstate[i] == V_OK;
    bool ok = hash_ram(m->rdram, pa, e->len) == e->hash;
    if (s->vstate[i] != V_UNKNOWN) {
        s->stats.rehashes++;
        if (!ok && s->vstate[i] == V_OK)
            s->stats.stale++;
    }
    s->vstate[i] = ok ? V_OK : V_BAD;
    s->vsum[i] = sum;
    s->vpa[i] = pa;
    return ok;
}

static inline rc_line *line_of(struct n64_rc_state *s, uint32_t pc)
{
    return &s->cache[((pc >> 2) ^ (pc >> (CACHE_BITS + 2))) & (CACHE_SIZE - 1)];
}

/* The entry to run at pc (index; *pbase the physical address of its start), or -1. pc_pa: pc's physical address. */
static int32_t lookup(n64_machine *m, struct n64_rc_state *s, uint32_t pc, uint32_t pc_pa, uint32_t *pbase)
{
    rc_line *ln = line_of(s, pc);
    bool direct = is_direct(pc);
    uint32_t tg = direct ? 0 : tlb_key(m);
    if (ln->key == pc && ln->tg == tg) {
        if (ln->e >= 0) {
            if (entry_valid(m, s, (uint32_t)ln->e, ln->pbase)) {
                *pbase = ln->pbase;
                return ln->e;
            }
        } else if (ln->e == E_NONE || page_sum(m, ln->p0, ln->p1) == ln->sum) {
            return -1;
        }
    }
    s->stats.slow_lookups++;
    uint32_t page = pc >> 12, lo = 0, hi = s->nrefs;
    while (lo < hi) { /* first ref with page >= pc's */
        uint32_t mid = lo + (hi - lo) / 2;
        if (s->refs[mid].page < page)
            lo = mid + 1;
        else
            hi = mid;
    }
    int32_t found = -1;
    bool any = false;
    uint32_t p0 = 0xFFFFFFFFu, p1 = 0, fbase = 0;
    for (uint32_t k = lo; k < s->nrefs && s->refs[k].page == page; k++) {
        uint32_t i = s->refs[k].e;
        const n64_recomp_entry *e = &s->table->entries[i];
        if (pc - e->addr >= e->len)
            continue;
        uint32_t base = pc_pa - (pc - e->addr); /* direct: contiguous; mapped: the same page */
        if (base + e->len > m->rdram_size || base > pc_pa)
            continue;
        any = true;
        if ((base >> 12) < p0)
            p0 = base >> 12;
        if (((base + e->len - 1) >> 12) > p1)
            p1 = (base + e->len - 1) >> 12;
        if (entry_valid(m, s, i, base)) {
            found = (int32_t)i;
            fbase = base;
            break;
        }
    }
    ln->key = pc;
    ln->tg = tg;
    if (found >= 0) {
        ln->e = found;
        ln->pbase = fbase;
        *pbase = fbase;
    } else if (!any) {
        ln->e = E_NONE;
    } else {
        ln->e = E_STALE;
        ln->p0 = p0;
        ln->p1 = p1;
        ln->sum = page_sum(m, p0, p1);
    }
    return found;
}

/* ---- the missed record ---- */

static uint64_t missed_key(uint32_t addr, uint64_t hash)
{
    return (hash ^ ((uint64_t)addr * 0x9E3779B97F4A7C15ull)) | 1u; /* never 0 */
}

/* Inserts (addr, hash) into the set; false when it was there. */
static bool missed_insert(struct n64_rc_state *s, uint64_t key)
{
    uint32_t i = (uint32_t)(key * 0x9E3779B97F4A7C15ull >> 40) & (2 * MISSED_MAX - 1);
    while (s->missed_set[i]) {
        if (s->missed_set[i] == key)
            return false;
        i = (i + 1) & (2 * MISSED_MAX - 1);
    }
    s->missed_set[i] = key;
    return true;
}

/* The variant counter of an address (NULL when the map is full). */
static uint8_t *variants(struct n64_rc_state *s, uint32_t addr)
{
    uint32_t i = (addr * 2654435761u >> 8) & (2 * MISSED_MAX - 1);
    for (uint32_t probes = 0; probes < 2 * MISSED_MAX; probes++) {
        if (s->var_cnt[i] == 0 || s->var_addr[i] == addr) {
            s->var_addr[i] = addr;
            return &s->var_cnt[i];
        }
        i = (i + 1) & (2 * MISSED_MAX - 1);
    }
    return NULL;
}

/* pc: virtual, pa: where it is in RDRAM now. The window ends at the end of RDRAM, and of the page when mapped. */
static void record_missed(n64_machine *m, struct n64_rc_state *s, uint32_t pc, uint32_t pa)
{
    if (s->nmissed >= MISSED_MAX)
        return;
    uint8_t *var = variants(s, pc);
    if (!var || *var >= N64_RECOMP_VARIANTS)
        return; /* code rewritten over and over at this address: left to the interpreter */
    uint32_t n = N64_RECOMP_SNAPSHOT / 4;
    if (pa + n * 4 > m->rdram_size)
        n = (m->rdram_size - pa) / 4;
    if (!is_direct(pc) && (pc & (PAGE - 1)) + n * 4 > PAGE)
        n = (PAGE - (pc & (PAGE - 1))) / 4;
    if (!n)
        return;
    /* Inside a recent record whose bytes are still there: a view into its copy. */
    for (uint32_t k = s->nmissed, seen = 0; k > 0 && seen < 32; k--, seen++) {
        const rc_missed *r = &s->missed[k - 1];
        if (r->owner && pc - r->addr < r->n * 4u && (pc - r->addr) / 4 + 8 <= r->n) {
            uint32_t w = (pc - r->addr) / 4, same = 1;
            for (uint32_t j = w; j < r->n && same; j++)
                same = pa + (j - w) * 4 + 4 <= m->rdram_size && n64_be32(m->rdram + pa + (j - w) * 4) == r->words[j];
            if (same) {
                uint64_t hash = n64_recomp_hash(r->words + w, r->n - w);
                if (missed_insert(s, missed_key(pc, hash))) {
                    s->missed[s->nmissed++] = (rc_missed){pc, r->n - w, hash, r->words + w, false};
                    (*var)++;
                }
                return;
            }
        }
    }
    uint64_t hash = hash_ram(m->rdram, pa, n * 4);
    if (!missed_insert(s, missed_key(pc, hash)))
        return;
    uint32_t *w = malloc(n * 4);
    if (!w)
        return;
    for (uint32_t j = 0; j < n; j++)
        w[j] = n64_be32(m->rdram + pa + 4 * j);
    s->missed[s->nmissed++] = (rc_missed){pc, n, hash, w, true};
    (*var)++;
}

size_t n64_recomp_missed(n64_machine *m, uint32_t *out, size_t max)
{
    struct n64_rc_state *s = m->rc;
    if (!s)
        return 0;
    size_t n = 0;
    for (uint32_t i = 0; i < s->nmissed; i++) {
        bool dup = false;
        for (uint32_t j = 0; j < i && !dup; j++)
            dup = s->missed[j].addr == s->missed[i].addr;
        if (dup)
            continue;
        if (n < max)
            out[n] = s->missed[i].addr;
        n++;
    }
    return n;
}

int n64_recomp_write_missed(n64_machine *m, const char *path)
{
    struct n64_rc_state *s = m->rc;
    if (!s || !s->nmissed)
        return 0;
    uint64_t *have = NULL; /* (addr, hash) pairs the file holds already */
    size_t nhave = 0, cap = 0;
    FILE *f = fopen(path, "r");
    if (f) {
        char head[64];
        int c;
        for (;;) {
            size_t len = 0;
            while ((c = fgetc(f)) != EOF && c != '\n') {
                if (len < sizeof head - 1)
                    head[len++] = (char)c;
            }
            head[len] = 0;
            unsigned long a = 0;
            unsigned long long hsh = 0;
            if (head[0] != '#' && sscanf(head, "%lx %llx", &a, &hsh) == 2) {
                if (nhave == cap) {
                    cap = cap ? cap * 2 : 256;
                    uint64_t *nh = realloc(have, cap * sizeof *have);
                    if (!nh)
                        break;
                    have = nh;
                }
                have[nhave++] = missed_key((uint32_t)a, (uint64_t)hsh);
            }
            if (c == EOF)
                break;
        }
        fclose(f);
    }
    f = fopen(path, "a");
    if (!f) {
        free(have);
        return -1;
    }
    int written = 0;
    for (uint32_t i = 0; i < s->nmissed; i++) {
        const rc_missed *r = &s->missed[i];
        uint64_t key = missed_key(r->addr, r->hash);
        bool dup = false;
        for (size_t j = 0; j < nhave && !dup; j++)
            dup = have[j] == key;
        if (dup)
            continue;
        fprintf(f, "%08X %016llX ", r->addr, (unsigned long long)r->hash);
        for (uint32_t k = 0; k < r->n; k++)
            fprintf(f, "%08X", r->words[k]);
        fputc('\n', f);
        written++;
    }
    free(have);
    return fclose(f) == 0 ? written : -1;
}

void n64_recomp_get_stats(n64_machine *m, n64_recomp_stats *out)
{
    memset(out, 0, sizeof *out);
    if (m->rc)
        *out = m->rc->stats;
    out->native = m->rc_native;
}

/* ---- the run loop ---- */

void n64_recomp_run(n64_machine *m)
{
    struct n64_rc_state *s = m->rc;
    n64_cpu *c = &m->cpu;
    const n64_recomp_table *t = s->table;
    while (m->now < m->stop_at) {
        if (c->irq) {
            s->stats.interrupts++;
            n64_cpu_take_interrupt(m);
            continue;
        }
        uint64_t pc = c->pc;
        uint32_t va = (uint32_t)pc, pa;
        if (N64_UNLIKELY(!s->booted)) {
            if (pc != (uint64_t)(int64_t)(int32_t)t->boot_pc) {
                n64_cpu_step(m);
                continue;
            }
            s->booted = true;
        }
        if ((pc & 3) || pc != (uint64_t)(int64_t)(int32_t)va || c->ksu != 0 || !translate(m, va, &pa)) {
            n64_cpu_step(m); /* not RDRAM code in kernel mode: IPL3 in DMEM, the ROM, user mode, a fault to come */
            continue;
        }
        if (pc == s->interp_pc || c->next_ds) {
            /* the instruction lifted code left to the interpreter (after the loop's checks), or a delay slot */
            if (pc == s->interp_pc)
                s->stats.fallbacks++;
            s->interp_pc = 1;
            s->stats.interp_ram++;
            n64_cpu_step(m);
            s->expect = 1;
            continue;
        }
        uint32_t base = 0;
        int32_t e = lookup(m, s, va, pa, &base);
        if (e >= 0) {
            const n64_recomp_entry *en = &t->entries[e];
            m->rc_lo = base;
            m->rc_len = en->len;
            m->rc_hit = 0;
            m->rc_exit = N64_RC_EXIT_NORMAL;
            en->fn(m);
            m->rc_len = 0;
            c->gpr[0] = 0;
            uint8_t why = m->rc_exit;
            m->rc_exit = N64_RC_EXIT_NORMAL;
            if (why != N64_RC_EXIT_NOT_COVERED) {
                s->stats.calls++;
                if (why == N64_RC_EXIT_INTERP)
                    s->interp_pc = c->pc;
                s->expect = 1;
                continue;
            }
        } else if (pc != s->expect) {
            record_missed(m, s, va, pa);
        }
        s->stats.interp_ram++;
        n64_cpu_step(m);
        s->expect = pc + 4;
    }
}

/* ---- differential testing ---- */

#define CMP_FIELD(name, x, y)                                                                                        \
    do {                                                                                                             \
        if ((x) != (y)) {                                                                                            \
            snprintf(why, why_n, "%s: 0x%llX vs 0x%llX", name, (unsigned long long)(x), (unsigned long long)(y));    \
            return 1;                                                                                                \
        }                                                                                                            \
    } while (0)

static int cmp_mem(const char *name, const uint8_t *x, const uint8_t *y, size_t n, char *why, size_t why_n)
{
    if (!memcmp(x, y, n))
        return 0;
    size_t i = 0;
    while (x[i] == y[i])
        i++;
    snprintf(why, why_n, "%s[0x%zX]: 0x%02X vs 0x%02X", name, i, x[i], y[i]);
    return 1;
}

int n64_recomp_compare(n64_machine *a, n64_machine *b, char *why, size_t why_n)
{
    char name[32];
    const n64_cpu *x = &a->cpu, *y = &b->cpu;
    CMP_FIELD("instructions", a->instructions, b->instructions);
    CMP_FIELD("now", a->now, b->now);
    for (int i = 0; i < 32; i++) {
        snprintf(name, sizeof name, "r%d", i);
        CMP_FIELD(name, x->gpr[i], y->gpr[i]);
    }
    CMP_FIELD("pc", x->pc, y->pc);
    CMP_FIELD("npc", x->npc, y->npc);
    CMP_FIELD("delay slot", x->next_ds, y->next_ds);
    CMP_FIELD("hi", x->hi, y->hi);
    CMP_FIELD("lo", x->lo, y->lo);
    CMP_FIELD("llbit", x->llbit, y->llbit);
    for (int i = 0; i < 32; i++) {
        if (i == C0_COUNT || i == C0_RANDOM)
            continue;
        snprintf(name, sizeof name, "cop0 %d", i);
        CMP_FIELD(name, x->cop0[i], y->cop0[i]);
    }
    CMP_FIELD("COUNT", n64_cpu_count(a), n64_cpu_count(b));
    for (int i = 0; i < 32; i++) {
        snprintf(name, sizeof name, "tlb %d", i);
        CMP_FIELD(name, x->tlb[i].hi, y->tlb[i].hi);
        CMP_FIELD(name, x->tlb[i].lo[0] ^ ((uint64_t)x->tlb[i].lo[1] << 32),
                  y->tlb[i].lo[0] ^ ((uint64_t)y->tlb[i].lo[1] << 32));
        CMP_FIELD(name, x->tlb[i].mask, y->tlb[i].mask);
    }
    for (int i = 0; i < 32; i++) {
        snprintf(name, sizeof name, "f%d", i);
        CMP_FIELD(name, x->fpr[i], y->fpr[i]);
    }
    CMP_FIELD("fcsr", x->fcsr, y->fcsr);
    for (int i = 0; i < EV_COUNT; i++) {
        snprintf(name, sizeof name, "event %d", i);
        CMP_FIELD(name, a->ev[i], b->ev[i]);
    }
    CMP_FIELD("frames", a->frames, b->frames);
    CMP_FIELD("MI_INTR", a->mi_intr, b->mi_intr);
    CMP_FIELD("MI_MASK", a->mi_mask, b->mi_mask);
    CMP_FIELD("MI_MODE", a->mi_mode, b->mi_mode);
    CMP_FIELD("rdram size", a->rdram_size, b->rdram_size);
    if (cmp_mem("rdram", a->rdram, b->rdram, a->rdram_size, why, why_n) ||
        cmp_mem("rsp mem", a->spmem, b->spmem, 0x2000, why, why_n))
        return 1;
    return 0;
}
