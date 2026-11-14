/* gba_recomp.c -- see gba_recomp.h. Placeholder until the recompiler lands (XBA-151): a table is accepted when its
 * ABI matches but not used yet, and nothing is recorded as missed. */
#include "gba_recomp.h"

int gba_attach_recomp(gba_machine *m, const gba_recomp_table *t)
{
    (void)m;
    if (t && t->abi != GBA_RECOMP_ABI)
        return -1;
    return 0;
}

size_t gba_recomp_missed(gba_machine *m, uint32_t *out, size_t max)
{
    (void)m;
    (void)out;
    (void)max;
    return 0;
}
