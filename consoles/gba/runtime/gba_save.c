/* gba_save.c -- save memory of the GBA core (G-7, XBA-156). Placeholder until save detection lands: the 32 KB SRAM
 * area, never reported dirty (the host still writes it at exit). */
#include "gba_core.h"

#include <string.h>

gba_save_kind gba_save_type(gba_machine *m)
{
    (void)m;
    return GBA_SAVE_SRAM;
}

const uint8_t *gba_save_data(gba_machine *m, size_t *size)
{
    size_t n = 0;
    uint8_t *p = gba_sram(m, &n);
    if (size)
        *size = n < 32768 ? n : 32768;
    return p;
}

int gba_save_take_dirty(gba_machine *m)
{
    (void)m;
    return 0;
}

int gba_save_load(gba_machine *m, const uint8_t *data, size_t size)
{
    size_t n = 0;
    uint8_t *p = gba_sram(m, &n);
    if (!p || !data || size > n)
        return -1;
    memcpy(p, data, size);
    return 0;
}
