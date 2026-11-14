/* gba_recomp.c -- see gba_recomp.h and gba_recomp_rt.h (XBA-151, XBA-152): attaching a pack's table, the run loop
 * that dispatches to lifted code, the record of ROM code the interpreter had to run (discovery loop).
 *
 * Dispatch: the table's entries are the starts of the contiguous runs ("islands") of each lifted function, sorted.
 * For a PC in the cartridge, the entry at or before it (same state) is the only function that can hold it; called
 * with that PC, the function resumes there, answers GBA_RC_EXIT_NOT_COVERED, or hands it to the interpreter with the
 * number of instructions to the end of its block (rc_hint: no resume point inside a block; that many run interpreted
 * without calling it again). A direct-mapped cache remembers the function per address, so calls and returns cost one
 * cache probe. The loop keeps the interpreter's order of checks (scheduler, HALT, IRQ) between every two steps,
 * lifted or interpreted, so both run cycle for cycle alike. */
#define _POSIX_C_SOURCE 200809L
#include "gba_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CACHE_BITS 14
#define CACHE_SIZE (1u << CACHE_BITS)
#define MISSED_MAX 65536u /* distinct addresses recorded at most */
#define KEY_NONE 0xFFFFFFFFu

typedef struct rc_island {
    uint32_t addr;
    gba_recomp_fn fn;
} rc_island;

typedef struct rc_cache_line {
    uint32_t key;      /* pc | thumb, KEY_NONE: empty */
    gba_recomp_fn fn;  /* NULL: no lifted code holds this address */
} rc_cache_line;

struct gba_rc_state {
    const gba_recomp_table *table; /* NULL: detached (the missed record stays) */
    rc_island *isl[2];             /* [0] ARM, [1] Thumb, sorted by addr */
    uint32_t nisl[2];
    uint32_t expect;               /* key of the instruction after the last interpreted ROM step */
    uint32_t interp_key;           /* lifted code handed this instruction to the interpreter (GBA_RC_EXIT_INTERP) */
    uint32_t interp_left;          /* and this many in sequence from there (rc_hint: the rest of a lifted block) */
    gba_recomp_stats stats;
    uint32_t *missed_set;          /* open addressing, 2 * MISSED_MAX slots, KEY_NONE = empty */
    uint32_t *missed;              /* insertion order */
    uint32_t nmissed;
    gba_machine *verify;           /* GBA_RC_VERIFY=1: an interpreter twin replays every lifted call (debugging) */
    rc_cache_line cache[CACHE_SIZE];
};

static struct gba_rc_state *state(gba_machine *m)
{
    if (!m->rc) {
        struct gba_rc_state *s = calloc(1, sizeof *s);
        if (!s)
            return NULL;
        s->missed_set = malloc(2 * MISSED_MAX * sizeof *s->missed_set);
        s->missed = malloc(MISSED_MAX * sizeof *s->missed);
        if (!s->missed_set || !s->missed) {
            free(s->missed_set);
            free(s->missed);
            free(s);
            return NULL;
        }
        memset(s->missed_set, 0xFF, 2 * MISSED_MAX * sizeof *s->missed_set);
        m->rc = s;
    }
    return m->rc;
}

static void flush_cache(struct gba_rc_state *s)
{
    for (uint32_t i = 0; i < CACHE_SIZE; i++)
        s->cache[i].key = KEY_NONE;
    s->expect = KEY_NONE;
    s->interp_key = KEY_NONE;
}

void gba_recomp_state_loaded(gba_machine *m)
{
    struct gba_rc_state *s = m->rc;
    if (!s)
        return;
    s->expect = KEY_NONE;
    s->interp_key = KEY_NONE;
    s->interp_left = 0;
}

static void drop_table(struct gba_rc_state *s)
{
    free(s->isl[0]);
    free(s->isl[1]);
    s->isl[0] = s->isl[1] = NULL;
    s->nisl[0] = s->nisl[1] = 0;
    s->table = NULL;
}

static int cmp_island(const void *a, const void *b)
{
    uint32_t x = ((const rc_island *)a)->addr, y = ((const rc_island *)b)->addr;
    return x < y ? -1 : x > y;
}

int gba_attach_recomp(gba_machine *m, const gba_recomp_table *t)
{
    if (t && (t->abi != GBA_RECOMP_ABI || (t->count && !t->entries)))
        return -1;
    if (!t && !m->rc)
        return 0;
    struct gba_rc_state *s = state(m);
    if (!s)
        return -1;
    drop_table(s);
    flush_cache(s);
    if (!t) {
        return 0;
    }
    for (int k = 0; k < 2; k++) {
        uint32_t n = 0;
        for (uint32_t i = 0; i < t->count; i++)
            n += ((t->entries[i].flags & GBA_RECOMP_THUMB) != 0) == k;
        s->isl[k] = malloc((n ? n : 1) * sizeof *s->isl[k]);
        if (!s->isl[k]) {
            drop_table(s);
            return -1;
        }
        n = 0;
        for (uint32_t i = 0; i < t->count; i++)
            if (((t->entries[i].flags & GBA_RECOMP_THUMB) != 0) == k && t->entries[i].fn)
                s->isl[k][n++] = (rc_island){t->entries[i].addr, t->entries[i].fn};
        qsort(s->isl[k], n, sizeof *s->isl[k], cmp_island);
        s->nisl[k] = n;
    }
    s->table = t;
    if (!s->verify && getenv("GBA_RC_VERIFY") && *getenv("GBA_RC_VERIFY") == '1')
        s->verify = malloc(sizeof *m);
    return 0;
}

void gba_recomp_free(gba_machine *m)
{
    struct gba_rc_state *s = m->rc;
    if (!s)
        return;
    drop_table(s);
    free(s->verify);
    free(s->missed_set);
    free(s->missed);
    free(s);
    m->rc = NULL;
}

/* ---- lookup and the missed record ---- */

static gba_recomp_fn find(const struct gba_rc_state *s, uint32_t pc, int thumb)
{
    const rc_island *v = s->isl[thumb];
    uint32_t lo = 0, hi = s->nisl[thumb]; /* first island with addr > pc */
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (v[mid].addr <= pc)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo ? v[lo - 1].fn : NULL;
}

static inline rc_cache_line *line_of(struct gba_rc_state *s, uint32_t key)
{
    return &s->cache[((key >> 1) ^ (key >> (CACHE_BITS + 1))) & (CACHE_SIZE - 1)];
}

static void record_missed(struct gba_rc_state *s, uint32_t key)
{
    uint32_t h = (key * 2654435761u) & (2 * MISSED_MAX - 1);
    while (s->missed_set[h] != KEY_NONE) {
        if (s->missed_set[h] == key)
            return;
        h = (h + 1) & (2 * MISSED_MAX - 1);
    }
    if (s->nmissed >= MISSED_MAX)
        return;
    s->missed_set[h] = key;
    s->missed[s->nmissed++] = key;
}

static inline bool in_rom(uint32_t pc)
{
    return pc - 0x08000000u < 0x06000000u;
}

/* GBA_RC_VERIFY=1: ref is the machine as it was before the lifted call; the interpreter runs it up to the same
 * cycle and both must agree on everything. */
static void verify_call(gba_machine *ref, gba_machine *m, uint32_t key)
{
    char why[160];
    ref->rc = NULL;
    ref->rc_ewram = ref->ewram;
    ref->rc_iwram = ref->iwram;
    uint64_t steps = 0;
    while (ref->now < m->now && !ref->cpu.halted) {
        gba_cpu_step(ref);
        steps++;
    }
    if (gba_recomp_compare(ref, m, why, sizeof why) != 0) {
        fprintf(stderr,
                "GBA_RC_VERIFY: lifted call at 0x%08X (%s) differs after %llu interpreter steps: %s (interpreter vs "
                "lifted); interpreter pc 0x%08X, lifted pc 0x%08X, exit %d\n",
                key & ~1u, (key & 1) ? "Thumb" : "ARM", (unsigned long long)steps, why, gba_cpu_pc(ref),
                gba_cpu_pc(m), m->rc_exit);
        abort();
    }
}

/* ---- the run loop ---- */

void gba_recomp_run(gba_machine *m)
{
    struct gba_rc_state *s = m->rc;
    if (!s->table) {
        /* Detached: the interpreter's loop. */
        while (m->now < m->next_event) {
            if (m->cpu.halted || m->cpu.stopped) {
                m->now = m->next_event;
                return;
            }
            gba_cpu_step(m);
        }
        return;
    }
    while (m->now < m->next_event) {
        if (m->cpu.halted || m->cpu.stopped) {
            m->now = m->next_event;
            return;
        }
        uint32_t thumb = (m->cpu.cpsr_ctl & CPSR_T) ? 1 : 0;
        uint32_t pc = m->cpu.r[15] - (thumb ? 4 : 8);
        if (!in_rom(pc) || (m->irq_line && !(m->cpu.cpsr_ctl & CPSR_I))) {
            gba_cpu_step(m); /* BIOS, RAM, or the IRQ to take */
            continue;
        }
        uint32_t key = pc | thumb;
        if (key == s->interp_key) {
            /* The instruction lifted code left to the interpreter: run it now, after the loop's checks (the
             * scheduler may have been due, or an IRQ pending, when the lifted code stopped in front of it). Then
             * the rest of its block, while execution stays in sequence: inside a block nothing is a resume point. */
            s->interp_key = KEY_NONE;
            s->stats.fallbacks++;
            s->stats.interp_rom++;
            gba_cpu_step(m);
            s->expect = KEY_NONE;
            if (s->interp_left > 1) {
                uint32_t next = key + (thumb ? 2 : 4);
                uint32_t now_key = gba_cpu_pc(m) | ((m->cpu.cpsr_ctl & CPSR_T) ? 1u : 0u);
                if (now_key == next) {
                    s->interp_key = next;
                    s->interp_left--;
                }
            }
            continue;
        }
        s->interp_key = KEY_NONE;
        rc_cache_line *ln = line_of(s, key);
        if (ln->key != key) {
            ln->key = key;
            ln->fn = find(s, pc, (int)thumb);
            s->stats.slow_lookups++;
        }
        if (ln->fn) {
            m->rc_exit = GBA_RC_EXIT_NORMAL;
            m->rc_hint = 0;
            if (s->verify)
                memcpy(s->verify, m, sizeof *m);
            uint64_t before = m->now;
            ln->fn(m);
            if (s->verify && m->rc_exit != GBA_RC_EXIT_NOT_COVERED)
                verify_call(s->verify, m, pc | thumb);
            if (m->rc_exit == GBA_RC_EXIT_NORMAL && m->now == before && m->cpu.r[15] - (thumb ? 4 : 8) == pc &&
                ((m->cpu.cpsr_ctl & CPSR_T) ? 1u : 0u) == thumb)
                m->rc_exit = GBA_RC_EXIT_NOT_COVERED; /* ran nothing (not lifted code of ours): interpret */
            if (m->rc_exit == GBA_RC_EXIT_NORMAL) {
                s->stats.calls++;
                continue;
            }
            if (m->rc_exit == GBA_RC_EXIT_INTERP) {
                s->stats.calls++;
                m->rc_exit = GBA_RC_EXIT_NORMAL;
                s->interp_key = (gba_cpu_pc(m) | ((m->cpu.cpsr_ctl & CPSR_T) ? 1u : 0u));
                s->interp_left = m->rc_hint ? m->rc_hint : 1;
                continue;
            }
            m->rc_exit = GBA_RC_EXIT_NORMAL;
            ln->fn = NULL; /* not covered after all */
        }
        /* ROM code no lifted function holds: the interpreter runs it; the start of each such run is missed. */
        if (key != s->expect)
            record_missed(s, key);
        s->stats.interp_rom++;
        gba_cpu_step(m);
        s->expect = key + (thumb ? 2 : 4);
    }
}

size_t gba_recomp_missed(gba_machine *m, uint32_t *out, size_t max)
{
    struct gba_rc_state *s = m->rc;
    if (!s)
        return 0;
    for (size_t i = 0; i < max && i < s->nmissed; i++)
        out[i] = s->missed[i];
    return s->nmissed;
}

void gba_recomp_get_stats(gba_machine *m, gba_recomp_stats *out)
{
    memset(out, 0, sizeof *out);
    if (m->rc)
        *out = m->rc->stats;
    out->native = m->rc_native;
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

int gba_recomp_compare(gba_machine *a, gba_machine *b, char *why, size_t why_n)
{
    char name[32];
    for (int i = 0; i < 16; i++) {
        snprintf(name, sizeof name, "r%d", i);
        CMP_FIELD(name, a->cpu.r[i], b->cpu.r[i]);
    }
    CMP_FIELD("cpsr", gba_get_cpsr(a), gba_get_cpsr(b));
    for (int i = 0; i < BANK_COUNT; i++) {
        CMP_FIELD("bank r13", a->cpu.bank_r13[i], b->cpu.bank_r13[i]);
        CMP_FIELD("bank r14", a->cpu.bank_r14[i], b->cpu.bank_r14[i]);
        CMP_FIELD("spsr", a->cpu.spsr[i], b->cpu.spsr[i]);
    }
    for (int i = 0; i < 5; i++) {
        CMP_FIELD("fiq r8-12", a->cpu.fiq_r8_12[i], b->cpu.fiq_r8_12[i]);
        CMP_FIELD("usr r8-12", a->cpu.usr_r8_12[i], b->cpu.usr_r8_12[i]);
    }
    CMP_FIELD("halted", a->cpu.halted, b->cpu.halted);
    CMP_FIELD("cycles", a->now, b->now);
    CMP_FIELD("next_event", a->next_event, b->next_event);
    CMP_FIELD("vcount", a->vcount, b->vcount);
    CMP_FIELD("IE", a->ie, b->ie);
    CMP_FIELD("IF", a->if_, b->if_);
    CMP_FIELD("IME", a->ime, b->ime);
    if (cmp_mem("ewram", a->ewram, b->ewram, GBA_EWRAM_SIZE, why, why_n) ||
        cmp_mem("iwram", a->iwram, b->iwram, GBA_IWRAM_SIZE, why, why_n) ||
        cmp_mem("io", a->io, b->io, GBA_IO_SIZE, why, why_n) ||
        cmp_mem("palette", a->pal, b->pal, GBA_PAL_SIZE, why, why_n) ||
        cmp_mem("vram", a->vram, b->vram, GBA_VRAM_SIZE, why, why_n) ||
        cmp_mem("oam", a->oam, b->oam, GBA_OAM_SIZE, why, why_n) ||
        cmp_mem("save", a->save.data, b->save.data, GBA_SAVE_MAX, why, why_n))
        return 1;
    return 0;
}
