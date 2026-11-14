/* Placeholder GPU (ps1_devices.h), built when ps1_gpu.c (P-7, XBA-165) is absent, so the core builds and runs
 * without it: GP0 words are dropped (except GP0(1Fh), the IRQ request), GPUSTAT reports always ready, GP1(08h) sets
 * the video timing the core schedules scanlines from, the display is off. */
#include "ps1_devices.h"

#include <stdlib.h>

struct ps1_gpu {
    ps1_irq_sink irq;
    uint32_t stat;
    int line;
    uint16_t *vram;
};

ps1_gpu *ps1_gpu_create(ps1_irq_sink irq)
{
    ps1_gpu *g = calloc(1, sizeof *g);
    if (!g)
        return NULL;
    g->vram = calloc(1024 * 512, sizeof(uint16_t));
    if (!g->vram) {
        free(g);
        return NULL;
    }
    g->irq = irq;
    ps1_gpu_reset(g);
    return g;
}

void ps1_gpu_destroy(ps1_gpu *g)
{
    if (g) {
        free(g->vram);
        free(g);
    }
}

void ps1_gpu_reset(ps1_gpu *g)
{
    g->stat = 0x14802000u; /* display off, ready to receive commands, interlace field */
}

void ps1_gpu_gp0(ps1_gpu *g, uint32_t word)
{
    if ((word >> 24) == 0x1F) {
        g->stat |= 1u << 24;
        if (g->irq.raise)
            g->irq.raise(g->irq.ctx, PS1_IRQ_GPU);
    }
}

void ps1_gpu_gp1(ps1_gpu *g, uint32_t word)
{
    uint32_t v = word & 0xFFFFFFu;
    switch (word >> 24) {
    case 0x00: ps1_gpu_reset(g); break;
    case 0x02: g->stat &= ~(1u << 24); break;
    case 0x03: g->stat = (g->stat & ~(1u << 23)) | (v & 1) << 23; break;
    case 0x04: g->stat = (g->stat & ~(3u << 29)) | (v & 3) << 29; break;
    case 0x08: /* bits 0-1 -> 17-18, 2 -> 19, 3 -> 20, 4 -> 21, 5 -> 22, 6 -> 16 */
        g->stat = (g->stat & ~0x7F0000u) | (v & 0x3F) << 17 | (v & 0x40) << 10;
        break;
    default: break;
    }
}

uint32_t ps1_gpu_read(ps1_gpu *g)
{
    (void)g;
    return 0;
}

uint32_t ps1_gpu_stat(ps1_gpu *g)
{
    return g->stat | 0x1C000000u;
}

void ps1_gpu_dma_write(ps1_gpu *g, const uint32_t *words, size_t n)
{
    for (size_t i = 0; i < n; i++)
        ps1_gpu_gp0(g, words[i]);
}

void ps1_gpu_dma_read(ps1_gpu *g, uint32_t *words, size_t n)
{
    (void)g;
    for (size_t i = 0; i < n; i++)
        words[i] = 0;
}

void ps1_gpu_get_timing(ps1_gpu *g, ps1_gpu_timing *t)
{
    static const int div[4] = {10, 8, 5, 4};
    t->pal = (g->stat >> 20) & 1;
    t->interlaced = (g->stat >> 22) & 1;
    t->dot_divider = (g->stat & (1u << 16)) ? 7 : div[(g->stat >> 17) & 3];
    t->lines = t->pal ? 314 : 263;
    t->cycles_per_line = t->pal ? 3406 : 3413;
}

void ps1_gpu_scanline(ps1_gpu *g, int line)
{
    g->line = line;
    g->stat ^= 1u << 31; /* odd / even line (non-interlaced: toggles every line) */
}

int ps1_gpu_display(ps1_gpu *g, uint32_t *xrgb, int max_w, int max_h, int *w, int *h)
{
    (void)g;
    (void)xrgb;
    (void)max_w;
    (void)max_h;
    *w = 320;
    *h = 240;
    return -1;
}

const uint16_t *ps1_gpu_vram(ps1_gpu *g)
{
    return g->vram;
}
