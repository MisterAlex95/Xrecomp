/* gc_recomp.c -- running lifted GameCube code on the spike's core (XBA-225): the table, the span check, the
 * dispatcher, the discovery record.
 *
 * The table lists every resume point of every lifted function (gc_recomp.h), so dispatch is an exact lookup: an open
 * hash from address to entry. A span (a text section's lifted bytes) is trusted once its bytes in memory hash to what
 * was lifted; an icbi into it makes it checked again before its next use (code patched at run time then runs in the
 * interpreter, never stale native code). */
#include <stdlib.h>
#include <string.h>

#include "gc_internal.h"

/* The first slot of address a: Fibonacci hashing, the product's top bits. Its low bits depend only on a's low bits,
 * so addresses equal modulo the capacity (a DOL's text is larger than it) would pile up in one probe run. */
static inline uint32_t rc_slot(uint32_t a, uint32_t shift)
{
    return ((a >> 2) * 0x9E3779B1u) >> shift;
}

static const gc_recomp_span *span_table; /* for span_lo_cmp only */

static int span_lo_cmp(const void *a, const void *b)
{
    uint32_t x = span_table[*(const uint32_t *)a].lo, y = span_table[*(const uint32_t *)b].lo;
    return x < y ? -1 : x > y;
}

int gc_attach_recomp(gc_machine *m, const gc_recomp_table *t)
{
    free(m->span_state);
    free(m->span_order);
    free(m->span_maxhi);
    free(m->rc_hash_keys);
    free(m->rc_hash_idx);
    m->span_state = NULL;
    m->span_order = NULL;
    m->span_maxhi = NULL;
    m->rc_hash_keys = NULL;
    m->rc_hash_idx = NULL;
    m->rc = NULL;
    if (!t)
        return 0;
    if (t->abi != GC_RECOMP_ABI)
        return -1;
    uint32_t cap = 1024, shift = 22;
    while (cap < t->count * 2u) {
        cap *= 2;
        shift--;
    }
    m->rc_hash_keys = malloc(cap * sizeof *m->rc_hash_keys);
    m->rc_hash_idx = malloc(cap * sizeof *m->rc_hash_idx);
    m->span_state = calloc(t->nspans ? t->nspans : 1, 1);
    m->span_order = malloc((t->nspans ? t->nspans : 1) * sizeof *m->span_order);
    m->span_maxhi = malloc((t->nspans ? t->nspans : 1) * sizeof *m->span_maxhi);
    if (!m->rc_hash_keys || !m->rc_hash_idx || !m->span_state || !m->span_order || !m->span_maxhi)
        return -1;
    /* spans by lo (a pack lists one per lifted function, thousands: icbi finds the ones it hits by bisection) */
    span_table = t->spans;
    for (uint32_t i = 0; i < t->nspans; i++)
        m->span_order[i] = i;
    qsort(m->span_order, t->nspans, sizeof *m->span_order, span_lo_cmp);
    for (uint32_t k = 0, hi = 0; k < t->nspans; k++) {
        if (t->spans[m->span_order[k]].hi > hi)
            hi = t->spans[m->span_order[k]].hi;
        m->span_maxhi[k] = hi;
    }
    for (uint32_t i = 0; i < cap; i++)
        m->rc_hash_idx[i] = -1;
    m->rc_hash_mask = cap - 1;
    m->rc_hash_shift = shift;
    for (uint32_t i = 0; i < t->count; i++) {
        uint32_t a = t->entries[i].addr, s = rc_slot(a, shift);
        while (m->rc_hash_idx[s & m->rc_hash_mask] >= 0)
            s++;
        m->rc_hash_keys[s & m->rc_hash_mask] = a;
        m->rc_hash_idx[s & m->rc_hash_mask] = (int32_t)i;
    }
    m->rc = t;
    return 0;
}

static bool span_valid(gc_machine *m, uint32_t s)
{
    if (m->span_state[s])
        return m->span_state[s] == 1; /* known either way until an icbi in the span: a mismatch is not rehashed */
    const gc_recomp_span *sp = &m->rc->spans[s];
    uint32_t h = 0x811C9DC5u;
    for (uint32_t a = sp->lo; a < sp->hi; a++)
        h = (h ^ m->h.ram[a & 0x3FFFFFFFu]) * 0x01000193u;
    m->stats.span_checks++;
    m->span_state[s] = h == sp->fnv1a ? 1 : 2;
    return m->span_state[s] == 1;
}

void gc_recomp_icbi(gc_machine *m, uint32_t ea)
{
    if (!m->rc)
        return;
    const gc_recomp_span *sp = m->rc->spans;
    /* the last span starting at or below ea, then back while an earlier one may still reach past ea */
    uint32_t lo = 0, hi = m->rc->nspans;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (sp[m->span_order[mid]].lo <= ea)
            lo = mid + 1;
        else
            hi = mid;
    }
    while (lo > 0 && m->span_maxhi[lo - 1] > ea) {
        uint32_t i = m->span_order[--lo];
        if (ea < sp[i].hi)
            m->span_state[i] = 0;
    }
}

static bool in_text(gc_machine *m, uint32_t pc)
{
    for (int i = 0; i < m->ntext; i++)
        if (pc >= m->text_lo[i] && pc < m->text_hi[i])
            return true;
    return false;
}

#define GC_MISSED_MAX 65536u /* as the discovered file keeps (gc_rel.c) */

static bool missed_set_add(gc_machine *m, uint32_t pc)
{
    if ((m->nmissed + 1) * 2 > m->missed_set_cap) { /* grow and rehash (load factor 1/2) */
        size_t cap = m->missed_set_cap ? m->missed_set_cap * 2 : 1024;
        uint32_t *set = calloc(cap, sizeof *set);
        if (!set)
            return false;
        for (size_t i = 0; i < m->nmissed; i++) {
            size_t s = ((m->missed[i] >> 2) * 0x9E3779B1u) & (cap - 1);
            while (set[s])
                s = (s + 1) & (cap - 1);
            set[s] = m->missed[i];
        }
        free(m->missed_set);
        m->missed_set = set;
        m->missed_set_cap = cap;
    }
    size_t s = ((pc >> 2) * 0x9E3779B1u) & (m->missed_set_cap - 1);
    while (m->missed_set[s]) {
        if (m->missed_set[s] == pc)
            return false;
        s = (s + 1) & (m->missed_set_cap - 1);
    }
    m->missed_set[s] = pc;
    return true;
}

static void record_miss(gc_machine *m, uint32_t pc)
{
    if (!pc || m->nmissed >= GC_MISSED_MAX || !missed_set_add(m, pc))
        return;
    if (m->nmissed == m->cap_missed) {
        size_t cap = m->cap_missed ? m->cap_missed * 2 : 64;
        uint32_t *p = realloc(m->missed, cap * sizeof *p);
        if (!p)
            return;
        m->missed = p;
        m->cap_missed = cap;
    }
    m->missed[m->nmissed++] = pc;
}

bool gc_recomp_dispatch(gc_machine *m)
{
    if (!m->rc)
        return false;
    uint32_t pc = m->h.cpu.pc, s = rc_slot(pc, m->rc_hash_shift);
    int idx = -1;
    bool listed = false, direct = true;
    /* an address may be listed several times (REL modules loaded in turn at one place, XBA-264): the entry whose
     * span holds what memory holds runs */
    for (;; s++) {
        int32_t i = m->rc_hash_idx[s & m->rc_hash_mask];
        if (i < 0)
            break;
        if (m->rc_hash_keys[s & m->rc_hash_mask] != pc)
            continue;
        if (!listed) {
            listed = true;
            /* lifted code assumes the code at pc is MEM1[pc & 0x3FFFFFFF]: not so under non-default translation */
            direct = gc_icode_direct(m, pc);
        }
        if (!direct)
            break;
        if (span_valid(m, m->rc->entries[i].span)) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        if (m->branched && direct && (listed ? gc_rel_in_code(m, pc) : in_text(m, pc) || gc_rel_in_code(m, pc)))
            record_miss(m, pc);
        return false;
    }
    m->h.rc_exit = GC_RC_EXIT_NORMAL;
    m->rc->entries[idx].fn(m);
    if (m->h.rc_exit == GC_RC_EXIT_NOT_COVERED)
        return false;
    m->stats.calls++;
    m->branched = true; /* lifted code leaves at a jump, a call, a return or a resume point */
    if (m->h.rc_exit == GC_RC_EXIT_INTERP) {
        gc_cpu_step(m);
        m->stats.interp++;
    }
    return true;
}

size_t gc_recomp_missed(gc_machine *m, uint32_t *out, size_t max)
{
    for (size_t i = 0; i < m->nmissed && i < max; i++)
        out[i] = m->missed[i];
    return m->nmissed;
}

void gc_recomp_get_stats(gc_machine *m, gc_recomp_stats *out)
{
    *out = m->stats;
    out->native = m->h.rc_native;
}
