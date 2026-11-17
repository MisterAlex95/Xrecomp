/* Placeholder SPU (ps1_devices.h), built when ps1_spu.c (P-8, XBA-166) is absent: registers read back (SPUSTAT
 * follows SPUCNT's mode bits, as libraries wait for it), 512 KB of sound RAM for transfers, silent output at
 * 44100 Hz (one sample per 768 CPU cycles). */
#include "ps1_devices.h"

#include <stdlib.h>
#include <string.h>

#define SPU_RAM 0x80000u
#define RING 8192u

struct ps1_spu {
    ps1_irq_sink irq;
    uint16_t regs[0x200];
    uint8_t *ram;
    uint32_t addr;   /* transfer address, bytes */
    uint32_t cycles; /* towards the next sample */
    size_t queued;   /* silent frames waiting to be pulled */
};

ps1_spu *ps1_spu_create(ps1_irq_sink irq)
{
    ps1_spu *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->ram = calloc(1, SPU_RAM);
    if (!s->ram) {
        free(s);
        return NULL;
    }
    s->irq = irq;
    return s;
}

void ps1_spu_destroy(ps1_spu *s)
{
    if (s) {
        free(s->ram);
        free(s);
    }
}

uint16_t ps1_spu_read16(ps1_spu *s, uint32_t off)
{
    off &= 0x3FE;
    if (off == 0x1AE) /* SPUSTAT: mode bits of SPUCNT, never busy */
        return s->regs[0x1AA / 2] & 0x3F;
    return s->regs[off / 2];
}

void ps1_spu_write16(ps1_spu *s, uint32_t off, uint16_t v)
{
    off &= 0x3FE;
    s->regs[off / 2] = v;
    if (off == 0x1A6) {
        s->addr = (uint32_t)v * 8u;
    } else if (off == 0x1A8) {
        s->ram[s->addr & (SPU_RAM - 1)] = (uint8_t)v;
        s->ram[(s->addr + 1) & (SPU_RAM - 1)] = (uint8_t)(v >> 8);
        s->addr += 2;
    }
}

void ps1_spu_dma_write(ps1_spu *s, const uint32_t *words, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        for (int b = 0; b < 4; b++)
            s->ram[(s->addr + (uint32_t)b) & (SPU_RAM - 1)] = (uint8_t)(words[i] >> (8 * b));
        s->addr += 4;
    }
}

void ps1_spu_dma_read(ps1_spu *s, uint32_t *words, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint32_t w = 0;
        for (int b = 0; b < 4; b++)
            w |= (uint32_t)s->ram[(s->addr + (uint32_t)b) & (SPU_RAM - 1)] << (8 * b);
        words[i] = w;
        s->addr += 4;
    }
}

void ps1_spu_tick(ps1_spu *s, uint32_t cpu_cycles, ps1_cdrom *cd)
{
    int16_t cdbuf[64];
    s->cycles += cpu_cycles;
    size_t n = s->cycles / 768u;
    s->cycles %= 768u;
    s->queued += n;
    if (s->queued > RING)
        s->queued = RING;
    while (cd && n) { /* drain the CD audio so the controller's buffer does not fill up */
        size_t k = n > 32 ? 32 : n;
        ps1_cdrom_audio_pull(cd, cdbuf, k);
        n -= k;
    }
}

size_t ps1_spu_audio_pull(ps1_spu *s, int16_t *stereo, size_t frames)
{
    size_t n = frames < s->queued ? frames : s->queued;
    memset(stereo, 0, n * 2 * sizeof(int16_t));
    s->queued -= n;
    return n;
}

/* Save states (XBA-182): a placeholder has no state worth keeping. */
size_t ps1_spu_state_size(ps1_spu *x)
{
    (void)x;
    return 0;
}

void ps1_spu_state_save(ps1_spu *x, uint8_t *out)
{
    (void)x;
    (void)out;
}

int ps1_spu_state_valid(const uint8_t *in, size_t n)
{
    (void)in;
    return n == 0 ? 0 : -1;
}

int ps1_spu_state_load(ps1_spu *x, const uint8_t *in, size_t n)
{
    (void)x;
    (void)in;
    return n == 0 ? 0 : -1;
}
