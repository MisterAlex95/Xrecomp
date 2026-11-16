/* ctr_recomp.c -- running lifted ARM11 code on the 3DS spike's core (XBA-228): the table, the dispatcher, the
 * discovery record. The DS spike's dispatcher (consoles/nds/runtime/nds_recomp.c) without its span checks: a 3DS
 * process's code is mapped read-only, so the table is checked once, when it is attached, against the loaded image
 * (FNV-1a of the relocated code and rodata). */
#include <stdlib.h>
#include <string.h>

#include "ctr_internal.h"

int ctr_attach_recomp(ctr_machine *m, const ctr_recomp_table *t)
{
    m->rc = NULL;
    for (int i = 0; i < 1024; i++)
        m->rc_cache_pc[i] = 0xFFFFFFFFu;
    if (!t)
        return 0;
    if (t->abi != CTR_RECOMP_ABI)
        return -1;
    if (t->image_hi < t->image_lo || !ctr_mem_mapped(m, t->image_lo, t->image_hi - t->image_lo))
        return -2;
    uint32_t h = 0x811C9DC5u;
    for (uint32_t a = t->image_lo; a < t->image_hi; a++)
        h = (h ^ m->h.mem[a]) * 0x01000193u;
    if (h != t->image_fnv1a)
        return -2;
    m->rc = t;
    return 0;
}

/* The entry whose run can hold pc (the last one at or before it, same state), or -1. */
static int lookup(ctr_machine *m, uint32_t pc, uint32_t thumb)
{
    unsigned slot = ((pc >> 2) ^ (pc >> 12) ^ thumb) & 1023;
    if (m->rc_cache_pc[slot] == (pc | thumb))
        return m->rc_cache_idx[slot];
    const ctr_recomp_entry *e = m->rc->entries;
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
    while (best >= 0 && (e[best].flags & CTR_RECOMP_THUMB) != thumb)
        best--;
    m->rc_cache_pc[slot] = pc | thumb;
    m->rc_cache_idx[slot] = best;
    return best;
}

static void record_miss(ctr_machine *m, uint32_t key)
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

bool ctr_recomp_dispatch(ctr_machine *m)
{
    if (!m->rc)
        return false;
    uint32_t thumb = (m->h.cpu.cpsr_ctl & 0x20u) ? 1u : 0u;
    uint32_t pc = m->h.cpu.r[15] - (thumb ? 4 : 8);
    if (pc < m->rc->image_lo || pc >= m->rc->image_hi) {
        m->rc_after_jump = m->branched;
        return false;
    }
    int idx = lookup(m, pc, thumb);
    if (idx >= 0) {
        m->h.rc_exit = CTR_RC_EXIT_NORMAL;
        m->h.rc_hint = 0;
        m->rc->entries[idx].fn(m);
        if (m->h.rc_exit != CTR_RC_EXIT_NOT_COVERED) {
            m->stats.calls++;
            if (m->h.rc_exit == CTR_RC_EXIT_INTERP) {
                unsigned n = m->h.rc_hint ? m->h.rc_hint : 1;
                int cur = m->cur;
                for (unsigned i = 0; i < n && !m->error[0]; i++) {
                    ctr_cpu_step(m);
                    m->stats.interp++;
                    if (m->branched || m->h.now >= m->h.next_event || m->cur != cur || m->resched)
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

size_t ctr_recomp_missed(ctr_machine *m, uint32_t *out, size_t max)
{
    for (size_t i = 0; i < m->nmissed && i < max; i++)
        out[i] = m->missed[i];
    return m->nmissed;
}

void ctr_recomp_get_stats(ctr_machine *m, ctr_recomp_stats *out)
{
    *out = m->stats;
    out->native = m->h.rc_native;
}
