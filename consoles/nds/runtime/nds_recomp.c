/* nds_recomp.c -- running lifted ARM9 code on the DS spike's core (XBA-223): the table, the span check, the
 * dispatcher, the discovery record.
 *
 * DS code lives in RAM, so unlike the GBA's cartridge an address does not name one piece of code for the whole run:
 * calico's boot stub runs at 0x02004800 from where it was loaded, and .main is copied over it a moment later. A span
 * (nds_recomp.h) is trusted once its bytes in memory hash to what was lifted, checked at most once per
 * NDS_SPAN_RECHECK interpreted instructions until then, and never again afterwards: the spike's limit (overlays,
 * code loaded or patched at run time would need the PS1's model: a write counter per RAM page and entries named by
 * content, consoles/ps1/tools/README.md « Code lives in RAM »). */
#include <stdlib.h>
#include <string.h>

#include "nds_internal.h"

#define NDS_SPAN_RECHECK 4096u

void nds_recomp_free(nds_arm *m)
{
    free(m->span_state);
    free(m->span_checked);
    free(m->missed);
    m->span_state = NULL;
    m->span_checked = NULL;
    m->missed = NULL;
}

int nds_attach_recomp(nds_machine *mm, const nds_recomp_table *t)
{
    return nds_attach_recomp_cpu(mm, 0, t);
}

int nds_attach_recomp_cpu(nds_machine *mm, int cpu, const nds_recomp_table *t)
{
    nds_arm *m = cpu ? &mm->a7 : &mm->a9;
    free(m->span_state);
    free(m->span_checked);
    m->span_state = NULL;
    m->span_checked = NULL;
    m->rc = NULL;
    for (int i = 0; i < 1024; i++)
        m->rc_cache_pc[i] = 0xFFFFFFFFu;
    if (!t)
        return 0;
    if (t->abi != NDS_RECOMP_ABI)
        return -1;
    m->span_state = calloc(t->nspans ? t->nspans : 1, 1);
    m->span_checked = calloc(t->nspans ? t->nspans : 1, sizeof *m->span_checked);
    if (!m->span_state || !m->span_checked)
        return -1;
    for (uint32_t i = 0; i < t->nspans; i++)
        m->span_checked[i] = UINT64_MAX;
    m->rc = t;
    return 0;
}

static int span_of(nds_arm *m, uint32_t pc)
{
    for (uint32_t i = 0; i < m->rc->nspans; i++)
        if (pc >= m->rc->spans[i].lo && pc < m->rc->spans[i].hi)
            return (int)i;
    return -1;
}

static bool span_valid(nds_arm *m, int s)
{
    if (m->span_state[s])
        return true;
    uint64_t last = m->span_checked[s];
    if (last != UINT64_MAX && m->stats.interp - last < NDS_SPAN_RECHECK)
        return false;
    const nds_recomp_span *sp = &m->rc->spans[s];
    uint32_t h = 0x811C9DC5u;
    for (uint32_t a = sp->lo; a < sp->hi; a++)
        h = (h ^ (m->id ? nds7_debug_read8(m->m, a) : nds9_debug_read8(m->m, a))) * 0x01000193u;
    m->stats.span_checks++;
    if (h == sp->fnv1a) {
        m->span_state[s] = 1;
        return true;
    }
    m->span_checked[s] = m->stats.interp;
    return false;
}

/* The entry whose run can hold pc (the last one at or before it, same state), or -1. */
static int lookup(nds_arm *m, uint32_t pc, uint32_t thumb)
{
    unsigned slot = ((pc >> 1) ^ (pc >> 11) ^ thumb) & 1023;
    if (m->rc_cache_pc[slot] == (pc | thumb))
        return m->rc_cache_idx[slot];
    const nds_recomp_entry *e = m->rc->entries;
    int lo = 0, hi = (int)m->rc->count - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (e[mid].addr <= pc) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    while (best >= 0 && (e[best].flags & NDS_RECOMP_THUMB) != thumb)
        best--;
    m->rc_cache_pc[slot] = pc | thumb;
    m->rc_cache_idx[slot] = best;
    return best;
}

static void record_miss(nds_arm *m, uint32_t key)
{
    for (size_t i = 0; i < m->nmissed; i++)
        if (m->missed[i] == key)
            return;
    if (m->nmissed == m->cap_missed) {
        size_t cap = m->cap_missed ? m->cap_missed * 2 : 64;
        uint32_t *p = realloc(m->missed, cap * sizeof *p);
        if (!p)
            return;
        m->missed = p;
        m->cap_missed = cap;
    }
    m->missed[m->nmissed++] = key;
}

bool nds_recomp_dispatch(nds_arm *m)
{
    if (!m->rc)
        return false;
    uint32_t thumb = (m->h.cpu.cpsr_ctl & 0x20u) ? 1u : 0u;
    uint32_t pc = m->h.cpu.r[15] - (thumb ? 4 : 8);
    int s = span_of(m, pc);
    if (s < 0 || !span_valid(m, s)) {
        m->rc_after_jump = m->branched;
        return false;
    }
    int idx = lookup(m, pc, thumb);
    if (idx >= 0) {
        m->h.rc_exit = NDS_RC_EXIT_NORMAL;
        m->h.rc_hint = 0;
        m->rc->entries[idx].fn(m);
        if (m->h.rc_exit != NDS_RC_EXIT_NOT_COVERED) {
            m->stats.calls++;
            if (m->h.rc_exit == NDS_RC_EXIT_INTERP) {
                unsigned n = m->h.rc_hint ? m->h.rc_hint : 1;
                for (unsigned i = 0; i < n && !m->m->error[0]; i++) {
                    nds_cpu_step(m);
                    m->stats.interp++;
                    if (m->branched || m->h.now >= m->h.next_event || m->h.cpu.halted)
                        break;
                }
            }
            m->rc_after_jump = true;
            return true;
        }
    }
    if (m->rc_after_jump || m->branched)
        record_miss(m, pc | thumb);
    m->rc_after_jump = false;
    return false;
}

void nds_recomp_note_interp(nds_arm *m)
{
    m->stats.interp++;
}

size_t nds_recomp_missed(nds_machine *mm, int cpu, uint32_t *out, size_t max)
{
    nds_arm *m = cpu ? &mm->a7 : &mm->a9;
    for (size_t i = 0; i < m->nmissed && i < max; i++)
        out[i] = m->missed[i];
    return m->nmissed;
}

void nds_recomp_get_stats(nds_machine *mm, int cpu, nds_recomp_stats *out)
{
    nds_arm *m = cpu ? &mm->a7 : &mm->a9;
    *out = m->stats;
    out->native = m->h.rc_native;
}
