/* dc_hw.c -- time on the Dreamcast spike (XBA-227): the SH-4's TMU channels, the PowerVR2 sync generator
 * (scanlines, VBlank in / out into Holly's ISTNRM), the interrupt controller (TMU priorities from IPRA, Holly's
 * three IRL levels 6 / 4 / 2 from SB_IML*), and the framebuffer read-out.
 *
 * Clock: 200 MHz SH-4, one instruction per cycle; peripheral clock 50 MHz, so a TMU tick at P/4 is 16 cycles. A
 * frame is 1/60 s whatever the mode; the lines of a frame come from SPG_LOAD. */
#include <stdio.h>
#include <string.h>

#include "dc_internal.h"

#define PVR(reg) m->pvr[((reg) - 0x005F8000u) >> 2]
#define SB(reg) m->sb[((reg) - 0x005F6800u) >> 2]

#define SPG_VBLANK_INT 0x005F80CCu
#define SPG_CONTROL 0x005F80D0u
#define SPG_LOAD 0x005F80D8u
#define SPG_VBLANK 0x005F80F0u
#define FB_R_CTRL 0x005F8044u
#define FB_R_SOF1 0x005F8050u
#define FB_R_SOF2 0x005F8054u
#define FB_R_SIZE 0x005F805Cu

void dc_hw_kick(dc_machine *m)
{
    m->cpu.next_event = m->cpu.cycles;
}

/* ---- TMU ---- */

static uint64_t tmu_cpt(const dc_tmu *t)
{
    unsigned tpsc = t->tcr & 7;
    if (tpsc > 4)
        tpsc = 4;
    return 16ull << (2 * tpsc);
}

static bool tmu_running(const dc_machine *m, int ch)
{
    return (m->tstr >> ch) & 1;
}

/* Brings base_cnt / base_cycle up to now, raising UNF on underflow. */
static void tmu_catch_up(dc_machine *m, int ch)
{
    dc_tmu *t = &m->tmu[ch];
    if (!tmu_running(m, ch))
        return;
    uint64_t cpt = tmu_cpt(t);
    uint64_t ticks = (m->cpu.cycles - t->base_cycle) / cpt;
    if (ticks == 0)
        return;
    if (ticks <= t->base_cnt) {
        t->base_cnt -= (uint32_t)ticks;
    } else {
        uint64_t after = ticks - ((uint64_t)t->base_cnt + 1);
        uint64_t period = (uint64_t)t->tcor + 1;
        t->base_cnt = t->tcor - (uint32_t)(after % period);
        t->tcr |= 0x100;
    }
    t->base_cycle += ticks * cpt;
}

static uint64_t tmu_next_underflow(const dc_machine *m, int ch)
{
    const dc_tmu *t = &m->tmu[ch];
    if (!tmu_running(m, ch))
        return UINT64_MAX;
    return t->base_cycle + ((uint64_t)t->base_cnt + 1) * tmu_cpt(t);
}

uint32_t dc_tmu_read(dc_machine *m, uint32_t reg)
{
    if (reg == 0x00)
        return m->tocr;
    if (reg == 0x04)
        return m->tstr;
    if (reg >= 0x08 && reg < 0x2C) {
        int ch = (int)(reg - 0x08) / 12;
        dc_tmu *t = &m->tmu[ch];
        switch ((reg - 0x08) % 12) {
        case 0:
            return t->tcor;
        case 4:
            tmu_catch_up(m, ch);
            return t->base_cnt;
        default:
            tmu_catch_up(m, ch);
            return t->tcr;
        }
    }
    return 0;
}

void dc_tmu_write(dc_machine *m, uint32_t reg, uint32_t v)
{
    if (reg == 0x00) {
        m->tocr = (uint8_t)v;
        return;
    }
    if (reg == 0x04) {
        for (int ch = 0; ch < 3; ch++) {
            bool now_on = (v >> ch) & 1;
            if (now_on == tmu_running(m, ch))
                continue;
            if (!now_on)
                tmu_catch_up(m, ch);
            else
                m->tmu[ch].base_cycle = m->cpu.cycles;
            m->tstr = (uint8_t)((m->tstr & ~(1u << ch)) | ((unsigned)now_on << ch));
        }
        dc_hw_kick(m);
        return;
    }
    if (reg >= 0x08 && reg < 0x2C) {
        int ch = (int)(reg - 0x08) / 12;
        dc_tmu *t = &m->tmu[ch];
        tmu_catch_up(m, ch);
        switch ((reg - 0x08) % 12) {
        case 0:
            t->tcor = v;
            break;
        case 4:
            t->base_cnt = v;
            t->base_cycle = m->cpu.cycles;
            break;
        default: {
            uint32_t unf = t->tcr & v & 0x100; /* UNF: writing 0 clears it, 1 keeps it */
            t->tcr = (v & 0x2FF) | unf;
            t->base_cycle = m->cpu.cycles; /* prescaler may have changed: count from here */
            break;
        }
        }
        dc_hw_kick(m);
    }
}

/* ---- video timing ---- */

static uint32_t lines_per_frame(dc_machine *m)
{
    uint32_t l = ((PVR(SPG_LOAD) >> 16) & 0x3FF) + 1;
    return l < 100 ? 525 : l;
}

uint32_t dc_spg_status(dc_machine *m)
{
    uint32_t vb = PVR(SPG_VBLANK), vbstart = vb & 0x3FF, vbend = (vb >> 16) & 0x3FF;
    bool in_vblank = vbstart > vbend ? (m->line >= vbstart || m->line < vbend) : (m->line >= vbstart && m->line < vbend);
    return (m->line & 0x3FF) | (in_vblank ? 0x2000u : 0);
}

static void next_line(dc_machine *m)
{
    uint32_t lines = lines_per_frame(m);
    m->line++;
    if (m->line >= lines) {
        m->line = 0;
        m->frame++;
    }
    uint32_t vi = PVR(SPG_VBLANK_INT);
    if (m->line == (vi & 0x3FF))
        m->istnrm |= 1u << 3; /* VBlank in */
    if (m->line == ((vi >> 16) & 0x3FF))
        m->istnrm |= 1u << 4; /* VBlank out */
    m->next_line_at += DC_FRAME_CYCLES / lines;
}

void dc_hw_reset(dc_machine *m)
{
    memset(m->tmu, 0, sizeof m->tmu);
    for (int ch = 0; ch < 3; ch++) {
        m->tmu[ch].tcor = 0xFFFFFFFFu;
        m->tmu[ch].base_cnt = 0xFFFFFFFFu;
    }
    m->tocr = 0;
    m->tstr = 0;
    m->icr = 0;
    memset(m->ipr, 0, sizeof m->ipr);
    PVR(SPG_LOAD) = 0x01060359u;       /* reset values (NTSC) until the program sets a mode */
    PVR(SPG_VBLANK_INT) = 0x00150104u;
    PVR(SPG_VBLANK) = 0x00150104u;
    m->line = 0;
    m->frame = 0;
    m->next_line_at = DC_FRAME_CYCLES / lines_per_frame(m);
}

void dc_hw_events(dc_machine *m)
{
    sh4_cpu *c = &m->cpu;
    while (c->cycles >= m->next_line_at)
        next_line(m);
    uint64_t next = m->next_line_at;
    for (int ch = 0; ch < 3; ch++) {
        tmu_catch_up(m, ch);
        uint64_t u = tmu_next_underflow(m, ch);
        if (u < next)
            next = u;
    }
    if (m->run_end && m->run_end < next)
        next = m->run_end;
    if (next <= c->cycles)
        next = c->cycles + 1;
    c->next_event = next;
}

/* ---- interrupts ---- */

static int holly_level(dc_machine *m, uint32_t nrm_mask_reg)
{
    uint32_t nrm = m->istnrm & SB(nrm_mask_reg), ext = m->istext & SB(nrm_mask_reg + 4),
             err = m->isterr & SB(nrm_mask_reg + 8);
    return (nrm | ext | err) != 0;
}

bool dc_hw_take_interrupt(dc_machine *m)
{
    sh4_cpu *c = &m->cpu;
    if (c->sr & SH4_SR_BL)
        return false;
    int best = (int)((c->sr >> 4) & 15);
    uint32_t code = 0;
    static const struct {
        uint32_t reg;
        int pri;
        uint32_t code;
    } irl[3] = {{0x005F6930u, 6, 0x320}, {0x005F6920u, 4, 0x360}, {0x005F6910u, 2, 0x3A0}};
    for (int i = 0; i < 3; i++) {
        if (irl[i].pri > best && holly_level(m, irl[i].reg)) {
            best = irl[i].pri;
            code = irl[i].code;
        }
    }
    for (int ch = 0; ch < 3; ch++) {
        const dc_tmu *t = &m->tmu[ch];
        int pri = (m->ipr[0] >> (12 - 4 * ch)) & 15;
        if ((t->tcr & 0x120) == 0x120 && pri > best) {
            best = pri;
            code = 0x400 + 0x20u * (uint32_t)ch;
        }
    }
    if (!code)
        return false;
    c->intevt = code;
    sh4_enter_exception(c, 0x600, c->pc);
    c->sleeping = 0;
    m->stats.irqs++;
    return true;
}

/* ---- framebuffer ---- */

int dc_framebuffer_rgb(dc_machine *m, uint8_t *out, int *w, int *h)
{
    uint32_t ctrl = PVR(FB_R_CTRL), size = PVR(FB_R_SIZE);
    if (!(ctrl & 1))
        return -1;
    static const int bpp_tab[4] = {2, 2, 3, 4};
    int depth = (ctrl >> 2) & 3, bpp = bpp_tab[depth];
    uint32_t xwords = (size & 0x3FF) + 1, ylines = ((size >> 10) & 0x3FF) + 1, modulus = (size >> 20) & 0x3FF;
    uint32_t stride = (xwords + modulus - 1) * 4;
    bool interlace = (PVR(SPG_CONTROL) >> 4) & 1;
    int width = (int)(xwords * 4 / (uint32_t)bpp), height = (int)ylines * (interlace ? 2 : 1);
    if (width > DC_FB_MAX_W)
        width = DC_FB_MAX_W;
    if (height > DC_FB_MAX_H)
        height = DC_FB_MAX_H;
    for (int y = 0; y < height; y++) {
        uint32_t base = interlace ? ((y & 1) ? PVR(FB_R_SOF2) : PVR(FB_R_SOF1)) + (uint32_t)(y >> 1) * stride
                                  : PVR(FB_R_SOF1) + (uint32_t)y * stride;
        for (int x = 0; x < width; x++) {
            uint32_t a = base + (uint32_t)(x * bpp);
            /* each byte wrapped: FB_R_SOF is the guest's, unaligned, and a 24 bpp pixel can start at VRAM's last byte */
            const uint8_t p[3] = {m->vram[a & (DC_VRAM_SIZE - 1)], m->vram[(a + 1) & (DC_VRAM_SIZE - 1)],
                                  m->vram[(a + 2) & (DC_VRAM_SIZE - 1)]};
            uint8_t *o = out + ((size_t)y * (size_t)width + (size_t)x) * 3;
            uint32_t px;
            switch (depth) {
            case 0:
                px = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
                o[0] = (uint8_t)(((px >> 10) & 31) << 3);
                o[1] = (uint8_t)(((px >> 5) & 31) << 3);
                o[2] = (uint8_t)((px & 31) << 3);
                break;
            case 1:
                px = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
                o[0] = (uint8_t)(((px >> 11) & 31) << 3);
                o[1] = (uint8_t)(((px >> 5) & 63) << 2);
                o[2] = (uint8_t)((px & 31) << 3);
                break;
            default: /* 888 packed / 0888: B, G, R in memory */
                o[0] = p[2];
                o[1] = p[1];
                o[2] = p[0];
                break;
            }
        }
    }
    *w = width;
    *h = height;
    return 0;
}
