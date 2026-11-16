/* gc_core.c -- the GameCube spike's machine (XBA-225): DOL loading, the state the IPL leaves for an application,
 * the run loop (events, interrupts, lifted code first and the interpreter behind it), the XFB scan-out. */
#include <stdlib.h>
#include <string.h>

#include "gc_internal.h"

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

uint64_t gc_tb(gc_machine *m)
{
    return m->tb_base + m->h.now / GC_TB_DIV;
}

/* When the decrementer next goes from 0 to -1 (it counts down at the time-base rate). */
static void dec_arm(gc_machine *m)
{
    if ((int32_t)m->dec < 0) {
        m->dec_fire = UINT64_MAX;
        return;
    }
    uint64_t tick = m->dec_written / GC_TB_DIV + (uint64_t)m->dec + 1;
    m->dec_fire = tick * GC_TB_DIV;
}

void gc_schedule(gc_machine *m)
{
    dec_arm(m);
    uint64_t t = m->field_end;
    if (m->next_line < t)
        t = m->next_line;
    if (m->dec_fire < t)
        t = m->dec_fire;
    if (((m->h.cpu.msr & PPC_MSR_EE) && gc_irq_pending(m)) || m->error[0])
        t = m->h.now;
    m->h.next_event = t;
}

/* Lines in field f (0: 263, 1: 262; 525 per frame). */
static uint32_t field_lines(uint64_t f)
{
    return (f & 1) ? GC_LINES / 2 : GC_LINES - GC_LINES / 2;
}

gc_machine *gc_create(const uint8_t *dol, size_t size, char *why, size_t why_len)
{
    if (size < 0x100) {
        snprintf(why, why_len, "too small for a DOL header (%zu bytes)", size);
        return NULL;
    }
    gc_machine *m = calloc(1, sizeof *m);
    if (!m) {
        snprintf(why, why_len, "out of memory");
        return NULL;
    }
    m->h.ram = calloc(1, GC_RAM_SIZE);
    m->aram = calloc(1, GC_ARAM_SIZE);
    if (!m->h.ram || !m->aram) {
        snprintf(why, why_len, "out of memory");
        gc_destroy(m);
        return NULL;
    }
    /* DOL: 7 text and 11 data sections (file offset, address, size), BSS, entry. */
    for (int i = 0; i < 18; i++) {
        uint32_t off = be32(dol + 4 * i), addr = be32(dol + 0x48 + 4 * i), len = be32(dol + 0x90 + 4 * i);
        if (!len)
            continue;
        uint32_t pa = addr & 0x3FFFFFFFu;
        if ((uint64_t)off + len > size || (uint64_t)pa + len > GC_RAM_SIZE) {
            snprintf(why, why_len, "section %d (%08X+%X at file %X) out of range", i, addr, len, off);
            gc_destroy(m);
            return NULL;
        }
        memcpy(m->h.ram + pa, dol + off, len);
        if (i < 7 && m->ntext < 8) {
            m->text_lo[m->ntext] = addr;
            m->text_hi[m->ntext] = addr + len;
            m->ntext++;
        }
    }
    uint32_t bss = be32(dol + 0xD8) & 0x3FFFFFFFu, bss_len = be32(dol + 0xDC), entry = be32(dol + 0xE0);
    if (bss_len && (uint64_t)bss + bss_len <= GC_RAM_SIZE) {
        /* elf2dol's BSS range can span data sections: clear only bytes no section filled */
        for (uint32_t a = bss; a < bss + bss_len; a++) {
            bool in_sec = false;
            for (int i = 0; i < 18 && !in_sec; i++) {
                uint32_t s = be32(dol + 0x48 + 4 * i) & 0x3FFFFFFFu, l = be32(dol + 0x90 + 4 * i);
                in_sec = l && a >= s && a < s + l;
            }
            if (!in_sec)
                m->h.ram[a] = 0;
        }
    }
    if (!m->ntext || (entry & 0x3FFFFFFFu) >= GC_RAM_SIZE) {
        snprintf(why, why_len, "no text section or entry %08X out of range", entry);
        gc_destroy(m);
        return NULL;
    }
    /* Low memory as the IPL leaves it for an application (boot info, clocks, TV mode). */
    uint8_t *lo = m->h.ram;
    rc_put32(lo + 0x20, 0x0D15EA5Eu);   /* boot magic */
    rc_put32(lo + 0x24, 1);             /* boot info version */
    rc_put32(lo + 0x28, GC_RAM_SIZE);   /* physical memory size */
    rc_put32(lo + 0x2C, 3);             /* console type: retail */
    rc_put32(lo + 0xCC, 0);             /* TV mode: NTSC */
    rc_put32(lo + 0xD0, GC_ARAM_SIZE);  /* ARAM size */
    rc_put32(lo + 0xF0, GC_RAM_SIZE);   /* simulated memory size */
    rc_put32(lo + 0xF8, 162000000u);    /* bus clock */
    rc_put32(lo + 0xFC, GC_CPU_HZ);     /* core clock */
    /* CPU: translation on (BATs as the IPL sets them: not modelled), FP available, entry. */
    gc_cpu *c = &m->h.cpu;
    c->msr = PPC_MSR_FP | PPC_MSR_IR | PPC_MSR_DR | PPC_MSR_RI | PPC_MSR_ME;
    c->pc = entry;
    c->spr[PPC_SPR_HID2] = 0xA0000000u; /* paired singles and write-gather pipe on */
    m->dec = 0xFFFFFFFFu;
    m->line = 0;
    m->next_line = GC_CYCLES_PER_LINE;
    m->field_end = (uint64_t)field_lines(0) * GC_CYCLES_PER_LINE;
    m->vi[0x01] = 0; /* display off until the program configures it */
    gc_schedule(m);
    return m;
}

void gc_destroy(gc_machine *m)
{
    if (!m)
        return;
    free(m->h.ram);
    free(m->aram);
    free(m->span_state);
    free(m->rc_hash_keys);
    free(m->rc_hash_idx);
    free(m->missed);
    free(m);
}

void gc_set_verbose(gc_machine *m, int level)
{
    m->verbose = level;
}

const char *gc_error(const gc_machine *m)
{
    return m->error[0] ? m->error : NULL;
}

uint64_t gc_frames(const gc_machine *m)
{
    return m->frames;
}

uint32_t gc_peek32(gc_machine *m, uint32_t ea)
{
    uint32_t pa = ea & 0x3FFFFFFFu;
    return pa <= GC_RAM_SIZE - 4 ? rc_be32(m->h.ram + pa) : 0;
}

static void take_interrupts(gc_machine *m)
{
    gc_cpu *c = &m->h.cpu;
    if (!(c->msr & PPC_MSR_EE))
        return;
    if (m->pi_intsr & m->pi_intmr) {
        gc_exception(m, 0x500, 0);
    } else if (m->dec_pending) {
        m->dec_pending = false;
        gc_exception(m, 0x900, 0);
    }
}

int gc_run_frame(gc_machine *m)
{
    if (m->error[0])
        return -1;
    while (!m->error[0]) {
        while (m->h.now >= m->next_line) {
            gc_vi_line(m);
            m->next_line += GC_CYCLES_PER_LINE;
        }
        if (m->h.now >= m->dec_fire) {
            m->dec_pending = true;
            m->dec = 0xFFFFFFFFu; /* keeps counting down from -1: no second edge soon */
            m->dec_written = m->h.now;
        }
        if (m->h.now >= m->field_end)
            break;
        take_interrupts(m);
        gc_schedule(m);
        while (m->h.now < m->h.next_event && !m->error[0]) {
            if (!gc_recomp_dispatch(m)) {
                gc_cpu_step(m);
                m->stats.interp++;
            }
        }
    }
    if (m->error[0])
        return -1;
    m->frames++;
    m->field_end += (uint64_t)field_lines(m->frames) * GC_CYCLES_PER_LINE;
    return 0;
}

void gc_dump_state(gc_machine *m, FILE *f)
{
    gc_cpu *c = &m->h.cpu;
    fprintf(f, "pc %08X lr %08X ctr %08X cr %08X msr %08X srr0 %08X srr1 %08X cycles %llu\n", c->pc, c->lr,
            c->ctr, c->cr, c->msr, c->spr[PPC_SPR_SRR0], c->spr[PPC_SPR_SRR1], (unsigned long long)m->h.now);
    for (int i = 0; i < 32; i++)
        fprintf(f, "r%-2d %08X%s", i, c->gpr[i], i % 8 == 7 ? "\n" : "  ");
    fprintf(f, "PI intsr %08X intmr %08X  VI dcr %04X tfbl %04X%04X picconf %04X di0 %04X%04X di1 %04X%04X\n",
            m->pi_intsr, m->pi_intmr, m->vi[1], m->vi[0x0E], m->vi[0x0F], m->vi[0x24], m->vi[0x18], m->vi[0x19],
            m->vi[0x1A], m->vi[0x1B]);
    fprintf(f, "last interpreted PCs:");
    for (unsigned i = 0; i < 64; i++) {
        if (i % 8 == 0)
            fprintf(f, "\n ");
        fprintf(f, " %08X", m->trace[(m->trace_at + i) & 63]);
    }
    fprintf(f, "\n");
}

/* ---- XFB scan-out ---- */

static uint32_t fb_addr(uint16_t hi, uint16_t lo)
{
    uint32_t v = (uint32_t)hi << 16 | lo;
    uint32_t a = v & 0x00FFFFFFu;
    return (v & 0x10000000u) ? a << 5 : a;
}

static uint8_t clamp8(int v)
{
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

int gc_xfb_rgb(gc_machine *m, uint8_t *rgb, int *w, int *h)
{
    uint32_t top = fb_addr(m->vi[0x0E], m->vi[0x0F]), bot = fb_addr(m->vi[0x12], m->vi[0x13]);
    unsigned wpl = (m->vi[0x24] >> 8) & 0x7F, std = m->vi[0x24] & 0xFF, acv = (m->vi[0x00] >> 4) & 0x3FF;
    bool interlaced = !(m->vi[0x01] & 4);
    if (!top || !wpl || !acv)
        return -1;
    int width = (int)wpl * 16, height = (int)acv * (interlaced ? 2 : 1);
    uint32_t stride = std * 32; /* bytes from one line of a field to the next */
    if (width > GC_XFB_MAX_W || height > GC_XFB_MAX_H)
        return -1;
    for (int y = 0; y < height; y++) {
        uint32_t base = interlaced ? ((y & 1) ? bot : top) + (uint32_t)(y >> 1) * stride : top + (uint32_t)y * stride;
        for (int x = 0; x < width; x += 2) {
            uint32_t pa = base + (uint32_t)x * 2;
            uint8_t y0 = 0, u = 128, y1 = 0, v = 128;
            if (pa + 4 <= GC_RAM_SIZE) {
                y0 = m->h.ram[pa];
                u = m->h.ram[pa + 1];
                y1 = m->h.ram[pa + 2];
                v = m->h.ram[pa + 3];
            }
            /* BT.601 studio range, 16.16 fixed point */
            int cu = u - 128, cv = v - 128;
            int rr = 104597 * cv, gg = -25675 * cu - 53279 * cv, bb = 132201 * cu;
            for (int k = 0; k < 2; k++) {
                int yy = 76309 * ((k ? y1 : y0) - 16) + 32768;
                uint8_t *o = rgb + ((size_t)y * width + x + k) * 3;
                o[0] = clamp8((yy + rr) >> 16);
                o[1] = clamp8((yy + gg) >> 16);
                o[2] = clamp8((yy + bb) >> 16);
            }
        }
    }
    *w = width;
    *h = height;
    return 0;
}
