/* ps2_core.c -- the PS2 spike's machine and run loop (XBA-226). See ps2_core.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ps2_internal.h"

#define PS2_SLICE 100000u /* cycles lifted code runs before it comes back to the loop */

ps2_machine *ps2_create(const uint8_t *data, size_t size, char *why, size_t why_len)
{
    ps2_machine *m = calloc(1, sizeof *m);
    if (!m) {
        snprintf(why, why_len, "out of memory");
        return NULL;
    }
    m->h.ram = calloc(1, PS2_RAM_SIZE);
    if (!m->h.ram) {
        snprintf(why, why_len, "out of memory");
        ps2_destroy(m);
        return NULL;
    }
    if (ps2_load_elf(m, data, size, why, why_len)) {
        ps2_destroy(m);
        return NULL;
    }
    ps2_hle_init(m);
    return m;
}

void ps2_destroy(ps2_machine *m)
{
    if (!m)
        return;
    free(m->h.ram);
    free(m->rc_valid);
    free(m->missed);
    free(m->out);
    free(m);
}

void ps2_set_verbose(ps2_machine *m, int level)
{
    m->verbose = level;
}

int ps2_run(ps2_machine *m, uint64_t insns)
{
    ps2_rc_head *h = &m->h;
    uint64_t end = h->cycles + insns;
    while (!m->error[0] && !m->exited && h->cycles < end) {
        uint64_t slice = h->cycles + PS2_SLICE;
        h->next_event = slice < end ? slice : end;
        if (ps2_recomp_dispatch(m))
            continue;
        ps2_cpu_step(m);
    }
    if (m->error[0])
        return 1;
    return m->exited ? 2 : 0;
}

const char *ps2_error(ps2_machine *m)
{
    return m->error;
}

int ps2_exit_code(ps2_machine *m)
{
    return m->exit_code;
}

void ps2_dump_trace(ps2_machine *m, FILE *f)
{
    fprintf(f, "last PCs:");
    for (unsigned i = 0; i < PS2_TRACE; i++) {
        unsigned k = (m->trace_pos + i) % PS2_TRACE;
        if (m->trace[k])
            fprintf(f, " %08X", m->trace[k]);
    }
    fprintf(f, "\nregs (low 64 bits):");
    for (int r = 0; r < 32; r++)
        fprintf(f, "%s%016llX", r % 4 ? " " : "\n  ", (unsigned long long)m->h.r[r]);
    fprintf(f, "\npc %08X next_pc %08X thread %d\n", m->h.pc, m->h.next_pc, m->cur);
    for (int i = 0; i < m->nunimpl; i++)
        fprintf(f, "syscall %d called, not implemented\n", m->unimpl[i]);
}
