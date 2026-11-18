/* gc_core.c -- the GameCube spike's machine (XBA-225): DOL loading, the state the IPL leaves for an application,
 * the run loop (events, interrupts, lifted code first and the interpreter behind it), video timing (NTSC / PAL,
 * XBA-259), the XFB scan-out. */
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

void gc_schedule(gc_machine *m)
{
    /* the decrementer's next 0 -> -1 transition (it counts down at the time-base rate) */
    m->dec_fire = m->dec_fire_tick * GC_TB_DIV;
    uint64_t t = m->field_end;
    if (m->next_line < t)
        t = m->next_line;
    if (m->dec_fire < t)
        t = m->dec_fire;
    if (m->di && gc_di_next_event(m) < t)
        t = gc_di_next_event(m);
    if (gc_audio_next_event(m) < t)
        t = gc_audio_next_event(m);
    if (((m->h.cpu.msr & PPC_MSR_EE) && gc_irq_pending(m)) || m->error[0])
        t = m->h.now;
    m->h.next_event = t;
}

/* Lines in field f: the first field of a frame has the odd line (NTSC 263 + 262, PAL 313 + 312). */
static uint32_t field_lines(const gc_machine *m, uint64_t f)
{
    return (f & 1) ? m->frame_lines / 2 : m->frame_lines - m->frame_lines / 2;
}

/* GX asks for PI interrupt causes (PE token / finish, CP). */
static void gx_irq(void *user, uint32_t set, uint32_t clear)
{
    gc_machine *m = user;
    m->pi_intsr = (m->pi_intsr | set) & ~clear;
    gc_check_interrupts(m);
}

gx_state *gc_gx(gc_machine *m)
{
    return m->gx;
}

/* The video standard of the fields to come, from the VI's DCR format (XBA-259): PAL (1) has 625 lines at 25 frames
 * per second, everything else (NTSC, MPAL, debug) 525 at 30. Taken at a field start, so a field keeps its length. */
static void video_timing(gc_machine *m)
{
    bool pal = ((m->vi[0x01] >> 8) & 3) == 1;
    m->frame_lines = pal ? GC_LINES_PAL : GC_LINES;
    m->line_cycles = pal ? GC_CYCLES_PER_LINE_PAL : GC_CYCLES_PER_LINE;
    if (m->line >= m->frame_lines)
        m->line = 0;
}

void gc_set_tv_mode(gc_machine *m, int mode)
{
    bool pal = mode == GC_TV_PAL;
    /* What the IPL leaves: the VI on in the TV's format (libogc's VIDEO_Init keeps an enabled VI's format and reads
     * it back as the current TV mode, and keeps its timing and display interrupts too), and low memory's TV mode
     * word. NTSC: the VI off, as before (VIDEO_Init sets it up itself). PAL: the minimal set libogc's own reset
     * writes, with DI0 at the end of a 313-line field and DI1 at its start, so retrace interrupts come at once;
     * VIDEO_Configure then programs the real mode. */
    if (pal) {
        static const struct { uint8_t idx; uint16_t v; } k_pal[] = {
            {0x00, 0x0006}, {0x02, 0x4769}, {0x03, 0x01AD}, {0x04, 0x02EA}, {0x05, 0x5140}, {0x06, 0x0005},
            {0x07, 0x01F6}, {0x08, 0x0004}, {0x09, 0x01F7}, {0x0A, 0x410C}, {0x0B, 0x410C}, {0x0C, 0x40ED},
            {0x0D, 0x40ED}, {0x18, 0x1000 | 313}, {0x19, 0x01AE}, {0x1A, 0x1001}, {0x1B, 0x0001}, {0x24, 0x2828},
        };
        for (size_t i = 0; i < sizeof k_pal / sizeof k_pal[0]; i++)
            m->vi[k_pal[i].idx] = k_pal[i].v;
    }
    m->vi[0x01] = pal ? 0x0101 : 0;
    rc_put32(m->h.ram + 0xCC, pal ? 1 : 0);
    gc_exi_tv_mode(m, pal);
    video_timing(m);
    m->next_line = m->h.now + m->line_cycles;
    m->field_end = m->h.now + (uint64_t)field_lines(m, m->frames) * m->line_cycles;
    gc_schedule(m);
}

uint64_t gc_field_ns(const gc_machine *m)
{
    /* cycles of the field / 486 MHz */
    return (uint64_t)field_lines(m, m->frames) * m->line_cycles * 1000000000ull / GC_CPU_HZ;
}

/* A machine with nothing loaded: MEM1 and ARAM cleared, low memory as the IPL leaves it for an application (boot
 * info, clocks, TV mode), the CPU with translation and FP on, video timing at the start of a field. */
gc_machine *gc_alloc_machine(char *why, size_t why_len)
{
    gc_machine *m = calloc(1, sizeof *m);
    if (!m) {
        snprintf(why, why_len, "out of memory");
        return NULL;
    }
    m->h.ram = calloc(1, GC_RAM_SIZE);
    m->aram = calloc(1, GC_ARAM_SIZE);
    m->gx = m->h.ram ? gx_create(m->h.ram, GC_RAM_SIZE, gx_irq, m) : NULL;
    m->exidev = gc_exi_create();
    if (!m->h.ram || !m->aram || !m->gx || !gc_audio_init(m) || !m->exidev) {
        snprintf(why, why_len, "out of memory");
        gc_destroy(m);
        return NULL;
    }
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
    /* CPU: translation on with the BATs the IPL sets (0x80000000 cached and 0xC0000000 uncached, both 256 MB onto
     * physical 0), FP available. */
    gc_cpu *c = &m->h.cpu;
    c->msr = PPC_MSR_FP | PPC_MSR_IR | PPC_MSR_DR | PPC_MSR_RI | PPC_MSR_ME;
    c->spr[528] = c->spr[536] = 0x80001FFFu; /* IBAT0U, DBAT0U */
    c->spr[529] = c->spr[537] = 0x00000002u; /* IBAT0L, DBAT0L */
    c->spr[530] = c->spr[538] = 0xC0001FFFu; /* IBAT1U, DBAT1U */
    c->spr[531] = c->spr[539] = 0x0000002Au; /* IBAT1L, DBAT1L: cache-inhibited, guarded */
    c->spr[PPC_SPR_HID2] = GC_HID2_LSQE | GC_HID2_PSE; /* quantised loads / stores and paired singles on */
    gc_mmu_bats(m);
    m->dec_zero = 0xFFFFFFFFu; /* DEC = -1 */
    m->dec_fire_tick = 1ull << 32;
    m->line = 0;
    m->vi[0x01] = 0; /* display off until the program configures it */
    video_timing(m);
    m->next_line = m->line_cycles;
    m->field_end = (uint64_t)field_lines(m, 0) * m->line_cycles;
    return m;
}

gc_machine *gc_create(const uint8_t *dol, size_t size, char *why, size_t why_len)
{
    if (size < 0x100) {
        snprintf(why, why_len, "too small for a DOL header (%zu bytes)", size);
        return NULL;
    }
    gc_machine *m = gc_alloc_machine(why, why_len);
    if (!m)
        return NULL;
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
    m->h.cpu.pc = entry;
    gc_schedule(m);
    return m;
}

void gc_destroy(gc_machine *m)
{
    if (!m)
        return;
    gx_destroy(m->gx);
    gc_exi_free(m->exidev);
    free(m->h.ram);
    free(m->aram);
    free(m->span_state);
    free(m->span_order);
    free(m->span_maxhi);
    free(m->rc_hash_keys);
    free(m->rc_hash_idx);
    free(m->missed);
    free(m->missed_set);
    gc_rel_free(m);
    gc_di_free(m);
    gc_audio_free(m);
    free(m);
}

void gc_set_verbose(gc_machine *m, int level)
{
    m->verbose = level;
    gx_set_verbose(m->gx, level);
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

/* The CPU until the next event. A DSI raised in the middle of an instruction (lifted or interpreted) longjmps back
 * here with the exception set up: the instruction had no effect, execution goes on at the handler. */
static void run_cpu(gc_machine *m)
{
    if (setjmp(m->fault_jb))
        m->stats.faults++; /* resumed after a fault */
    m->fault_armed = true;
    while (m->h.now < m->h.next_event && !m->error[0]) {
        if (!gc_recomp_dispatch(m)) {
            gc_cpu_step(m);
            m->stats.interp++;
        }
    }
    m->fault_armed = false;
}

int gc_run_frame(gc_machine *m)
{
    if (m->error[0])
        return -1;
    gc_fp_enter(m);
    while (!m->error[0]) {
        while (m->h.now >= m->next_line) {
            gc_vi_line(m);
            m->next_line += m->line_cycles;
            gc_si_line(m, ++m->field_line);
        }
        while (m->h.now >= m->dec_fire) { /* the next transition is 2^32 ticks later */
            m->dec_pending = true;
            m->dec_fire_tick += 1ull << 32;
            m->dec_fire = m->dec_fire_tick * GC_TB_DIV;
        }
        if (m->di && m->h.now >= gc_di_next_event(m))
            gc_di_event(m);
        if (m->h.now >= gc_audio_next_event(m))
            gc_audio_event(m);
        if (m->h.now >= m->field_end)
            break;
        take_interrupts(m);
        gc_schedule(m);
        run_cpu(m);
    }
    gc_fp_leave(m);
    if (m->error[0])
        return -1;
    m->frames++;
    gx_field(m->gx);
    video_timing(m);
    m->field_end += (uint64_t)field_lines(m, m->frames) * m->line_cycles;
    m->field_line = 0;
    gc_si_line(m, 0); /* a poll starts each field */
    gc_rel_field(m);  /* REL modules loaded or unloaded (XBA-264) */
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

/* Where each line of the picture the VI scans out starts in MEM1, and its size: 0, or -1 without a framebuffer. */
static int xfb_lines(gc_machine *m, uint32_t *base, int *w, int *h)
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
    for (int y = 0; y < height; y++)
        base[y] = interlaced ? ((y & 1) ? bot : top) + (uint32_t)(y >> 1) * stride : top + (uint32_t)y * stride;
    *w = width;
    *h = height;
    return 0;
}

uint32_t gc_xfb_addr(gc_machine *m)
{
    uint32_t base[GC_XFB_MAX_H];
    int w, h;
    return xfb_lines(m, base, &w, &h) ? 0 : base[0];
}

int gc_xfb_yuv(gc_machine *m, uint8_t *yuv, int *w, int *h)
{
    uint32_t base[GC_XFB_MAX_H];
    int width, height;
    if (xfb_lines(m, base, &width, &height))
        return -1;
    size_t row = (size_t)width * 2;
    for (int y = 0; y < height; y++) {
        uint8_t *o = yuv + (size_t)y * row;
        if (base[y] + row <= GC_RAM_SIZE) {
            memcpy(o, m->h.ram + base[y], row);
        } else {
            for (size_t i = 0; i < row; i += 2) {
                o[i] = 16; /* black */
                o[i + 1] = 128;
            }
        }
    }
    *w = width;
    *h = height;
    return 0;
}

int gc_xfb_rgb(gc_machine *m, uint8_t *rgb, int *w, int *h)
{
    uint32_t lines[GC_XFB_MAX_H];
    int width, height;
    if (xfb_lines(m, lines, &width, &height))
        return -1;
    for (int y = 0; y < height; y++) {
        uint32_t base = lines[y];
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
