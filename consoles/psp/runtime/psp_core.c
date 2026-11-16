/* psp_core.c -- the PSP spike's machine and run loop (XBA-224). See psp_core.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psp_internal.h"

psp_machine *psp_create(const uint8_t *data, size_t size, char *why, size_t why_len)
{
    psp_machine *m = calloc(1, sizeof *m);
    if (!m) {
        snprintf(why, why_len, "out of memory");
        return NULL;
    }
    m->h.ram = calloc(1, PSP_RAM_SIZE);
    m->vram = calloc(1, PSP_VRAM_SIZE);
    if (!m->h.ram || !m->vram) {
        snprintf(why, why_len, "out of memory");
        psp_destroy(m);
        return NULL;
    }
    if (psp_load_module(m, data, size, why, why_len)) {
        psp_destroy(m);
        return NULL;
    }
    psp_hle_init(m);
    if (psp_hle_start_module(m)) {
        snprintf(why, why_len, "cannot start the module's thread");
        psp_destroy(m);
        return NULL;
    }
    m->frame_end = PSP_CYCLES_PER_FRAME;
    return m;
}

void psp_destroy(psp_machine *m)
{
    if (!m)
        return;
    free(m->h.ram);
    free(m->vram);
    free(m->imports);
    free(m->rc_valid);
    free(m->missed);
    free(m->out);
    free(m);
}

void psp_set_verbose(psp_machine *m, int level)
{
    m->verbose = level;
}

int psp_run_frame(psp_machine *m)
{
    psp_rc_head *h = &m->h;
    uint64_t end = m->frame_end;
    while (!m->error[0] && !m->exited) {
        uint64_t wake = psp_hle_next_wake(m);
        if (h->cycles >= wake) {
            psp_hle_timers(m);
            psp_hle_preempt(m);
            continue;
        }
        if (m->cur < 0 && !psp_hle_schedule(m)) {
            /* every thread waits: time jumps to the next wake-up or the VBlank */
            if (m->exited)
                break;
            h->cycles = wake < end ? wake : end;
        }
        if (h->cycles >= end)
            break;
        if (m->cur < 0)
            continue;
        h->next_event = wake < end ? wake : end;
        if (psp_recomp_dispatch(m))
            continue;
        psp_cpu_step(m);
    }
    if (m->error[0])
        return 1;
    if (m->exited)
        return 2;
    m->frames++;
    m->frame_end = end + PSP_CYCLES_PER_FRAME;
    psp_hle_vblank(m);
    psp_hle_preempt(m);
    return 0;
}

const char *psp_error(psp_machine *m)
{
    return m->error;
}

uint64_t psp_frames(psp_machine *m)
{
    return m->frames;
}

void psp_set_buttons(psp_machine *m, uint32_t buttons)
{
    m->buttons = buttons;
}

const char *psp_stdout(psp_machine *m)
{
    return m->out ? m->out : "";
}

/* Pixel formats of sceDisplaySetFrameBuf: 0 RGB565, 1 RGBA5551, 2 RGBA4444, 3 RGBA8888 (R in the low bits). */
int psp_screen_rgb(psp_machine *m, uint8_t *rgb)
{
    if (!m->fb_addr || m->fb_format > 3)
        return -1;
    uint32_t bpp = m->fb_format == 3 ? 4 : 2, stride = m->fb_width ? m->fb_width : 512;
    for (int y = 0; y < PSP_SCREEN_H; y++) {
        for (int x = 0; x < PSP_SCREEN_W; x++) {
            uint32_t a = m->fb_addr + ((uint32_t)y * stride + (uint32_t)x) * bpp, c = 0;
            (void)psp_read(m, a, (int)bpp, &c);
            uint8_t *o = rgb + ((size_t)y * PSP_SCREEN_W + (size_t)x) * 3;
            switch (m->fb_format) {
            case 0:
                o[0] = (uint8_t)((c & 31) << 3 | (c & 31) >> 2);
                o[1] = (uint8_t)(((c >> 5) & 63) << 2 | ((c >> 5) & 63) >> 4);
                o[2] = (uint8_t)(((c >> 11) & 31) << 3 | ((c >> 11) & 31) >> 2);
                break;
            case 1:
                o[0] = (uint8_t)((c & 31) << 3 | (c & 31) >> 2);
                o[1] = (uint8_t)(((c >> 5) & 31) << 3 | ((c >> 5) & 31) >> 2);
                o[2] = (uint8_t)(((c >> 10) & 31) << 3 | ((c >> 10) & 31) >> 2);
                break;
            case 2:
                o[0] = (uint8_t)((c & 15) * 17);
                o[1] = (uint8_t)(((c >> 4) & 15) * 17);
                o[2] = (uint8_t)(((c >> 8) & 15) * 17);
                break;
            default:
                o[0] = (uint8_t)c;
                o[1] = (uint8_t)(c >> 8);
                o[2] = (uint8_t)(c >> 16);
            }
        }
    }
    return 0;
}

void psp_dump_trace(psp_machine *m, FILE *f)
{
    fprintf(f, "last PCs:");
    for (unsigned i = 0; i < PSP_TRACE; i++) {
        unsigned k = (m->trace_pos + i) % PSP_TRACE;
        if (m->trace[k])
            fprintf(f, " %08X", m->trace[k]);
    }
    fprintf(f, "\nregs:");
    for (int r = 0; r < 32; r++)
        fprintf(f, "%s%08X", r % 8 ? " " : "\n  ", m->h.r[r]);
    fprintf(f, "\npc %08X next_pc %08X thread %d\n", m->h.pc, m->h.next_pc,
            m->cur >= 0 ? m->threads[m->cur].uid : -1);
    for (int i = 0; i < m->nunimpl; i++)
        fprintf(f, "NID 0x%08X called, not implemented\n", m->unimpl[i]);
}
