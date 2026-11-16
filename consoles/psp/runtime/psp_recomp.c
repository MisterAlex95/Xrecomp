/* psp_recomp.c -- running lifted PSP code on the spike's core (XBA-224): the table, the content check, the
 * dispatcher, the discovery record.
 *
 * The spike loads one module and its code never changes, so each entry's bytes are checked once, at attach
 * (psp_recomp_hash over memory against the entry's hash): a pack lifted from another build of the module, or for
 * another load address, runs nowhere. Modules loaded or code written at run time would need the PS1's model
 * (consoles/ps1/tools/README.md « Code lives in RAM »: a write counter per RAM page, entries named by content). */
#include <stdlib.h>
#include <string.h>

#include "psp_internal.h"

uint64_t psp_recomp_hash(const uint8_t *p, uint32_t len)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t i = 0; i + 4 <= len; i += 4) {
        uint32_t w;
        memcpy(&w, p + i, 4);
        h = (h ^ w) * 0x100000001B3ull;
    }
    return h;
}

int psp_attach_recomp(psp_machine *m, const psp_recomp_table *t)
{
    free(m->rc_valid);
    m->rc_valid = NULL;
    m->rc = NULL;
    m->rc_stale = 0;
    if (!t)
        return 0;
    if (t->abi != PSP_RECOMP_ABI || t->base != PSP_LOAD_BASE)
        return -1;
    m->rc_valid = calloc(t->count ? t->count : 1, 1);
    if (!m->rc_valid)
        return -1;
    for (uint32_t i = 0; i < t->count; i++) {
        const psp_recomp_entry *e = &t->entries[i];
        uint8_t *p = psp_ptr(m, e->addr, e->len);
        m->rc_valid[i] = p && psp_recomp_hash(p, e->len) == e->hash;
        m->rc_stale += !m->rc_valid[i];
    }
    m->rc = t;
    return 0;
}

/* The entry holding pc (the last one starting at or before it, if pc is inside it), or -1. */
static int lookup(psp_machine *m, uint32_t pc)
{
    const psp_recomp_entry *e = m->rc->entries;
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
    if (best < 0 || pc - e[best].addr >= e[best].len || !m->rc_valid[best])
        return -1;
    return best;
}

static void record_miss(psp_machine *m, uint32_t pc)
{
    if (pc < m->code_lo || pc >= m->code_hi)
        return;
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

/* Runs lifted code at pc if there is some: true when it did (and the interpreter steps it asked for). */
bool psp_recomp_dispatch(psp_machine *m)
{
    psp_rc_head *h = &m->h;
    if (!m->rc)
        return false;
    uint32_t pc = h->pc;
    bool jumped = m->rc_after_jump || m->trace[(m->trace_pos + PSP_TRACE - 1) % PSP_TRACE] + 4 != pc;
    if (h->next_pc != pc + 4) /* a delay slot: the interpreter */
        return false;
    int idx = lookup(m, pc);
    if (idx >= 0) {
        h->rc_exit = PSP_RC_EXIT_NORMAL;
        h->rc_hint = 0;
        m->rc->entries[idx].fn(m);
        if (h->ld_reg) { /* a load in a JR's delay slot: no load delay on the Allegrex, it lands now */
            h->r[h->ld_reg] = h->ld_val;
            h->ld_reg = 0;
        }
        if (h->rc_exit != PSP_RC_EXIT_NOT_COVERED) {
            m->stats.calls++;
            if (h->rc_exit == PSP_RC_EXIT_INTERP) {
                unsigned n = h->rc_hint ? h->rc_hint : 1;
                for (unsigned i = 0; i < n && !m->error[0]; i++) {
                    uint32_t at = h->pc;
                    psp_cpu_step(m);
                    if (m->switched || m->exited || h->pc != at + 4 || h->cycles >= h->next_event)
                        break;
                }
            }
            m->rc_after_jump = true;
            return true;
        }
    }
    if (jumped)
        record_miss(m, pc);
    m->rc_after_jump = false;
    return false;
}

size_t psp_recomp_missed(psp_machine *m, uint32_t *out, size_t max)
{
    for (size_t i = 0; out && i < m->nmissed && i < max; i++)
        out[i] = m->missed[i];
    return m->nmissed;
}

void psp_recomp_get_stats(psp_machine *m, psp_recomp_stats *out)
{
    *out = m->stats;
    out->native = m->h.rc_native;
    out->entries = m->rc ? m->rc->count : 0;
    out->stale = m->rc_stale;
}
