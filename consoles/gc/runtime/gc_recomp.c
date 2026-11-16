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

int gc_attach_recomp(gc_machine *m, const gc_recomp_table *t)
{
    free(m->span_state);
    free(m->rc_hash_keys);
    free(m->rc_hash_idx);
    m->span_state = NULL;
    m->rc_hash_keys = NULL;
    m->rc_hash_idx = NULL;
    m->rc = NULL;
    if (!t)
        return 0;
    if (t->abi != GC_RECOMP_ABI)
        return -1;
    uint32_t cap = 1024;
    while (cap < t->count * 2u)
        cap *= 2;
    m->rc_hash_keys = malloc(cap * sizeof *m->rc_hash_keys);
    m->rc_hash_idx = malloc(cap * sizeof *m->rc_hash_idx);
    m->span_state = calloc(t->nspans ? t->nspans : 1, 1);
    if (!m->rc_hash_keys || !m->rc_hash_idx || !m->span_state)
        return -1;
    for (uint32_t i = 0; i < cap; i++)
        m->rc_hash_idx[i] = -1;
    m->rc_hash_mask = cap - 1;
    for (uint32_t i = 0; i < t->count; i++) {
        uint32_t a = t->entries[i].addr, s = (a >> 2) * 0x9E3779B1u;
        while (m->rc_hash_idx[s & m->rc_hash_mask] >= 0)
            s++;
        m->rc_hash_keys[s & m->rc_hash_mask] = a;
        m->rc_hash_idx[s & m->rc_hash_mask] = (int32_t)i;
    }
    m->rc = t;
    return 0;
}

static int lookup(gc_machine *m, uint32_t pc)
{
    uint32_t s = (pc >> 2) * 0x9E3779B1u;
    for (;;) {
        int32_t i = m->rc_hash_idx[s & m->rc_hash_mask];
        if (i < 0 || m->rc_hash_keys[s & m->rc_hash_mask] == pc)
            return i;
        s++;
    }
}

static bool span_valid(gc_machine *m, uint32_t s)
{
    if (m->span_state[s] == 1)
        return true;
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
    for (uint32_t i = 0; i < m->rc->nspans; i++)
        if (ea >= m->rc->spans[i].lo && ea < m->rc->spans[i].hi)
            m->span_state[i] = 0;
}

static bool in_text(gc_machine *m, uint32_t pc)
{
    for (int i = 0; i < m->ntext; i++)
        if (pc >= m->text_lo[i] && pc < m->text_hi[i])
            return true;
    return false;
}

static void record_miss(gc_machine *m, uint32_t pc)
{
    for (size_t i = 0; i < m->nmissed; i++)
        if (m->missed[i] == pc)
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
    uint32_t pc = m->h.cpu.pc;
    int idx = lookup(m, pc);
    if (idx < 0) {
        if (m->branched && in_text(m, pc))
            record_miss(m, pc);
        return false;
    }
    if (!span_valid(m, m->rc->entries[idx].span))
        return false;
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
