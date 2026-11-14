/* Placeholder MDEC (ps1_devices.h), built when ps1_mdec.c (P-9, XBA-167) is absent: idle, decodes nothing. */
#include "ps1_devices.h"

#include <stdlib.h>

struct ps1_mdec {
    uint32_t ctrl;
};

ps1_mdec *ps1_mdec_create(void)
{
    return calloc(1, sizeof(ps1_mdec));
}

void ps1_mdec_destroy(ps1_mdec *d)
{
    free(d);
}

uint32_t ps1_mdec_read(ps1_mdec *d, uint32_t reg)
{
    (void)d;
    return reg == 1 ? 0x80040000u : 0; /* status: data-out FIFO empty, idle, current block 4 */
}

void ps1_mdec_write(ps1_mdec *d, uint32_t reg, uint32_t v)
{
    if (reg == 1)
        d->ctrl = v;
}

void ps1_mdec_dma_write(ps1_mdec *d, const uint32_t *words, size_t n)
{
    (void)d;
    (void)words;
    (void)n;
}

size_t ps1_mdec_dma_read(ps1_mdec *d, uint32_t *words, size_t n)
{
    (void)d;
    for (size_t i = 0; i < n; i++)
        words[i] = 0;
    return n;
}

/* Save states (XBA-182): a placeholder has no state worth keeping. */
size_t ps1_mdec_state_size(ps1_mdec *x)
{
    (void)x;
    return 0;
}

void ps1_mdec_state_save(ps1_mdec *x, uint8_t *out)
{
    (void)x;
    (void)out;
}

int ps1_mdec_state_load(ps1_mdec *x, const uint8_t *in, size_t n)
{
    (void)x;
    (void)in;
    return n == 0 ? 0 : -1;
}
