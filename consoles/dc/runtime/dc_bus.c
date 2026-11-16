/* dc_bus.c -- the Dreamcast memory map as the SH-4 sees it with the MMU off (XBA-227).
 *
 * P0-P3 (0x00000000-0xDFFFFFFF) mirror the 29-bit physical space: area 0 (BIOS / flash: none, reads 0; Holly's
 * system bus block and PowerVR2 registers; AICA registers, RTC and sound RAM), area 1 (VRAM: 0x04 the 64-bit
 * interleaved view, 0x05 the 32-bit linear one, mirrored at 0x06 / 0x07), area 3 (main RAM, 16 MB, mirrored to
 * 0x0FFFFFFF), area 4 (the TA FIFO: writes dropped, no TA yet). P4 (0xE0000000-) holds the store queues, the cache
 * arrays (writes dropped) and the on-chip registers (CCN, BSC, INTC, TMU, SCIF, performance counters...).
 *
 * Naomi (XBA-235, m->model): main RAM is 32 MB (0x0C000000-0x0DFFFFFF, mirrored to 0x0FFFFFFF), sound RAM 8 MB,
 * battery SRAM (32 KB) at 0x00200000 where the Dreamcast has its flash, and the G1 window 0x005F7000-0x005F70FF
 * holds the cartridge board instead of the GD-ROM drive, G1 DMA (0x005F7400-) reading from it (dc_naomi.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dc_internal.h"

static dc_machine *M(sh4_cpu *c)
{
    return (dc_machine *)c;
}

void dc_hw_write_log(dc_machine *m, const char *what, uint32_t a, uint32_t v)
{
    if (m->verbose >= 2 || (m->verbose && m->unknown_logged < 64)) {
        m->unknown_logged++;
        fprintf(stderr, "dc: %s %08x = %08x (pc %08x)\n", what, a, v, m->cpu.pc);
    }
}

void dc_bus_reset(dc_machine *m)
{
    memset(m->sb, 0, sizeof m->sb);
    memset(m->pvr, 0, sizeof m->pvr);
    memset(m->aica, 0, sizeof m->aica);
    m->istnrm = m->istext = m->isterr = 0;
    m->ccr = m->mmucr = 0;
    m->qacr[0] = m->qacr[1] = 0;
    m->pctra = m->pdtra = m->pctrb = m->pdtrb = 0;
    m->scfsr = 0x0060;
}

/* 64-bit area offset -> linear (32-bit area) offset: the two 4 MB banks interleave by 32-bit word. */
static uint32_t vram64(uint32_t off)
{
    return ((off >> 1) & 0x3FFFFCu) | (off & 3u) | (((off >> 2) & 1u) << 22);
}

/* ---- Holly system bus block / PowerVR2 / AICA ---- */

#define SB_ISTNRM 0x005F6900u
#define SB_ISTEXT 0x005F6904u
#define SB_ISTERR 0x005F6908u

static uint32_t holly_read(dc_machine *m, uint32_t p)
{
    if (p >= 0x005F6800u && p < 0x005F8000u) {
        switch (p) {
        case SB_ISTNRM:
            return (m->istnrm & 0x3FFFFFu) | (m->isterr ? 0x80000000u : 0) | (m->istext ? 0x40000000u : 0);
        case SB_ISTEXT:
            return m->istext;
        case SB_ISTERR:
            return m->isterr;
        case 0x005F688Cu: /* SB_FFST: FIFO status, always empty */
            return 0;
        default:
            return m->sb[(p - 0x005F6800u) >> 2];
        }
    }
    if (p >= 0x005F8000u && p < 0x005FA000u) {
        switch (p) {
        case 0x005F8000u: /* ID */
            return 0x17FD11DBu;
        case 0x005F8004u: /* REVISION */
            return 0x00000011u;
        case 0x005F810Cu: /* SPG_STATUS */
            return dc_spg_status(m);
        default:
            return m->pvr[(p - 0x005F8000u) >> 2];
        }
    }
    return 0;
}

static void holly_write(dc_machine *m, uint32_t p, uint32_t v)
{
    if (p >= 0x005F6800u && p < 0x005F8000u) {
        switch (p) {
        case SB_ISTNRM:
            m->istnrm &= ~(v & 0x3FFFFFu);
            dc_hw_kick(m);
            return;
        case SB_ISTEXT:
            return; /* level-triggered from the devices: nothing raises them yet */
        case SB_ISTERR:
            m->isterr &= ~v;
            dc_hw_kick(m);
            return;
        default:
            m->sb[(p - 0x005F6800u) >> 2] = v;
            if (p >= 0x005F6910u && p <= 0x005F6938u)
                dc_hw_kick(m); /* IML2 / 4 / 6 masks */
            return;
        }
    }
    if (p >= 0x005F8000u && p < 0x005FA000u) {
        m->pvr[(p - 0x005F8000u) >> 2] = v;
        return;
    }
}

/* AICA RTC: fixed at 2000-01-01 00:00:00 (seconds since 1950), so runs are reproducible. */
#define DC_RTC_FIXED 0x5E0D3B80u

static uint32_t aica_read(dc_machine *m, uint32_t p)
{
    if (p >= 0x00700000u && p < 0x00708000u)
        return m->aica[(p - 0x00700000u) >> 2];
    if (p == 0x00710000u)
        return DC_RTC_FIXED >> 16;
    if (p == 0x00710004u)
        return DC_RTC_FIXED & 0xFFFF;
    return 0;
}

/* ---- on-chip registers (P4) ---- */

static uint32_t onchip_read(dc_machine *m, uint32_t a, int size)
{
    sh4_cpu *c = &m->cpu;
    switch (a) {
    case 0xFF000010u: return m->mmucr;
    case 0xFF00001Cu: return m->ccr;
    case 0xFF000020u: return c->tra;
    case 0xFF000024u: return c->expevt;
    case 0xFF000028u: return c->intevt;
    case 0xFF000038u: return m->qacr[0];
    case 0xFF00003Cu: return m->qacr[1];
    case 0xFF000084u: return m->pmcr[0];
    case 0xFF000088u: return m->pmcr[1];
    case 0xFF100004u: case 0xFF10000Cu: return (uint32_t)(c->cycles >> 32) & 0xFFFF; /* PMCTRnH: CPU cycles */
    case 0xFF100008u: case 0xFF100010u: return (uint32_t)c->cycles;
    case 0xFF800028u: return (uint32_t)(c->cycles / 3200u) & 0x3FF; /* RFCR: refresh count */
    case 0xFF80002Cu: return m->pctra;
    case 0xFF800030u: return 0; /* PDTRA: bits 8-9 cable type 0 = VGA */
    case 0xFF800040u: return m->pctrb;
    case 0xFF800044u: return m->pdtrb;
    case 0xFFD00000u: return m->icr;
    case 0xFFD00004u: return m->ipr[0];
    case 0xFFD00008u: return m->ipr[1];
    case 0xFFD0000Cu: return m->ipr[2];
    case 0xFFD00010u: return m->ipr[3];
    case 0xFFE80000u: return m->scsmr;
    case 0xFFE80004u: return m->scbrr;
    case 0xFFE80008u: return m->scscr;
    case 0xFFE80010u: return m->scfsr | 0x0060; /* TEND | TDFE: the transmitter is always ready */
    case 0xFFE80014u: return 0;                 /* SCFRDR2: nothing received */
    case 0xFFE80018u: return m->scfcr;
    case 0xFFE8001Cu: return 0;                 /* SCFDR2: FIFOs empty */
    case 0xFFE80020u: return m->scsptr;
    case 0xFFE80024u: return m->sclsr;
    default:
        break;
    }
    if (a >= 0xFFD80000u && a < 0xFFD80030u)
        return dc_tmu_read(m, a - 0xFFD80000u);
    if (a >= 0xFFC00000u && a < 0xFFC00020u)
        return 0; /* CPG / watchdog: stopped */
    (void)size;
    dc_hw_write_log(m, "read unhandled on-chip", a, 0);
    return 0;
}

static void serial_put(dc_machine *m, char ch)
{
    if (m->serial_len + 2 > m->serial_cap) {
        size_t cap = m->serial_cap ? m->serial_cap * 2 : 4096;
        char *p = realloc(m->serial, cap);
        if (!p)
            return;
        m->serial = p;
        m->serial_cap = cap;
    }
    m->serial[m->serial_len++] = ch;
    m->serial[m->serial_len] = 0;
    if (m->verbose)
        fputc(ch, stderr);
}

static void onchip_write(dc_machine *m, uint32_t a, uint32_t v, int size)
{
    (void)size;
    switch (a) {
    case 0xFF000010u:
        m->mmucr = v & ~4u; /* TI reads back 0 */
        if (v & 1u)
            fprintf(stderr, "dc: the program turned the MMU on (MMUCR %08x): not supported\n", v);
        return;
    case 0xFF00001Cu: m->ccr = v; return;
    case 0xFF000020u: m->cpu.tra = v; return;
    case 0xFF000024u: m->cpu.expevt = v; return;
    case 0xFF000028u: m->cpu.intevt = v; return;
    case 0xFF000038u: m->qacr[0] = v; return;
    case 0xFF00003Cu: m->qacr[1] = v; return;
    case 0xFF000084u: m->pmcr[0] = v; return;
    case 0xFF000088u: m->pmcr[1] = v; return;
    case 0xFF80002Cu: m->pctra = v; return;
    case 0xFF800030u: m->pdtra = v; return;
    case 0xFF800040u: m->pctrb = v; return;
    case 0xFF800044u: m->pdtrb = v; return;
    case 0xFFD00000u: m->icr = (uint16_t)v; return;
    case 0xFFD00004u: m->ipr[0] = (uint16_t)v; dc_hw_kick(m); return;
    case 0xFFD00008u: m->ipr[1] = (uint16_t)v; dc_hw_kick(m); return;
    case 0xFFD0000Cu: m->ipr[2] = (uint16_t)v; dc_hw_kick(m); return;
    case 0xFFD00010u: m->ipr[3] = (uint16_t)v; dc_hw_kick(m); return;
    case 0xFFE80000u: m->scsmr = (uint16_t)v; return;
    case 0xFFE80004u: m->scbrr = (uint8_t)v; return;
    case 0xFFE80008u: m->scscr = (uint16_t)v; return;
    case 0xFFE8000Cu: serial_put(m, (char)v); return;
    case 0xFFE80010u: m->scfsr = (uint16_t)(v & 0x00FF); return;
    case 0xFFE80018u: m->scfcr = (uint16_t)v; return;
    case 0xFFE80020u: m->scsptr = (uint16_t)v; return;
    case 0xFFE80024u: m->sclsr = (uint16_t)v; return;
    default:
        break;
    }
    if (a >= 0xFFD80000u && a < 0xFFD80030u) {
        dc_tmu_write(m, a - 0xFFD80000u, v);
        return;
    }
    /* UBC, BSC wait states and refresh, CPG, watchdog, DMAC, the SH-4 RTC, SCI: accepted, nothing modelled. */
    if ((a >= 0xFF200000u && a < 0xFF300000u) || (a >= 0xFF800000u && a < 0xFF900000u) ||
        (a >= 0xFFA00000u && a < 0xFFA00100u) || (a >= 0xFFC00000u && a < 0xFFC00020u) ||
        (a >= 0xFFC80000u && a < 0xFFC80100u) || (a >= 0xFFE00000u && a < 0xFFE00100u) ||
        (a >= 0xFF000000u && a < 0xFF000100u))
        return;
    dc_hw_write_log(m, "write unhandled on-chip", a, v);
}

/* ---- the generic paths ---- */

static uint32_t read_any(dc_machine *m, uint32_t a, int size)
{
    if ((a >> 29) == 7) {
        if (a < 0xE4000000u) {
            uint32_t v = m->sq[(a >> 5) & 1][(a >> 2) & 7];
            return size == 4 ? v : (v >> ((a & 3) * 8)) & (size == 2 ? 0xFFFF : 0xFF);
        }
        if (a < 0xF8000000u)
            return 0; /* cache address / data arrays */
        uint32_t v = onchip_read(m, a, size);
        return size == 4 ? v : v & (size == 2 ? 0xFFFF : 0xFF);
    }
    uint32_t p = a & 0x1FFFFFFFu;
    const uint8_t *mem = NULL;
    if (p >= 0x0C000000u && p < 0x10000000u) {
        mem = m->ram + (p & (m->ram_size - 1));
    } else if (p >= 0x04000000u && p < 0x08000000u) {
        uint32_t off = p & (DC_VRAM_SIZE - 1);
        mem = m->vram + ((p & 0x01000000u) ? off : vram64(off));
    } else if (p >= 0x00800000u && p < 0x01000000u) {
        mem = m->aram + (p & (m->aram_size - 1));
    } else if (m->sram && p >= 0x00200000u && p < 0x00200000u + NAOMI_SRAM_SIZE) {
        mem = m->sram + (p - 0x00200000u);
    }
    if (mem) {
        uint32_t v = 0;
        memcpy(&v, mem, (size_t)size);
        return v;
    }
    uint32_t v;
    if (m->model == DC_MODEL_NAOMI && ((p >= 0x005F7000u && p < 0x005F7100u) || (p >= 0x005F7400u && p < 0x005F7500u)))
        v = dc_naomi_g1_read(m, p & ~3u);
    else if (p >= 0x005F6800u && p < 0x005FA000u)
        v = holly_read(m, p & ~3u);
    else if (p >= 0x00700000u && p < 0x00710010u)
        v = aica_read(m, p & ~3u);
    else if (p < 0x00200000u)
        return 0; /* no BIOS image */
    else if (m->model == DC_MODEL_NAOMI && p < 0x00220000u)
        return 0; /* past the SRAM */
    else if (p < 0x00220000u)
        return size == 4 ? 0xFFFFFFFFu : (size == 2 ? 0xFFFF : 0xFF); /* flash: erased */
    else {
        dc_hw_write_log(m, "read unmapped", a, 0);
        return 0;
    }
    v >>= (p & 3) * 8;
    return size == 4 ? v : v & (size == 2 ? 0xFFFF : 0xFF);
}

static void write_any(dc_machine *m, uint32_t a, uint32_t v, int size)
{
    if ((a >> 29) == 7) {
        if (a < 0xE4000000u) {
            uint32_t *w = &m->sq[(a >> 5) & 1][(a >> 2) & 7];
            if (size == 4)
                *w = v;
            else {
                unsigned sh = (a & 3) * 8;
                uint32_t mask = (size == 2 ? 0xFFFFu : 0xFFu) << sh;
                *w = (*w & ~mask) | ((v << sh) & mask);
            }
            return;
        }
        if (a < 0xF8000000u)
            return;
        onchip_write(m, a, v, size);
        return;
    }
    uint32_t p = a & 0x1FFFFFFFu;
    uint8_t *mem = NULL;
    if (p >= 0x0C000000u && p < 0x10000000u) {
        mem = m->ram + (p & (m->ram_size - 1));
    } else if (p >= 0x04000000u && p < 0x08000000u) {
        uint32_t off = p & (DC_VRAM_SIZE - 1);
        mem = m->vram + ((p & 0x01000000u) ? off : vram64(off));
    } else if (p >= 0x00800000u && p < 0x01000000u) {
        mem = m->aram + (p & (m->aram_size - 1));
    } else if (m->sram && p >= 0x00200000u && p < 0x00200000u + NAOMI_SRAM_SIZE) {
        mem = m->sram + (p - 0x00200000u);
    }
    if (mem) {
        memcpy(mem, &v, (size_t)size);
        return;
    }
    if (m->model == DC_MODEL_NAOMI && ((p >= 0x005F7000u && p < 0x005F7100u) || (p >= 0x005F7400u && p < 0x005F7500u))) {
        dc_naomi_g1_write(m, p & ~3u, size == 4 ? v : v & (size == 2 ? 0xFFFFu : 0xFFu));
        return;
    }
    if (p >= 0x005F6800u && p < 0x005FA000u) {
        holly_write(m, p & ~3u, v);
        return;
    }
    if (p >= 0x00700000u && p < 0x00708000u) {
        m->aica[(p - 0x00700000u) >> 2] = v;
        return;
    }
    if (p >= 0x00710000u && p < 0x00710010u)
        return; /* RTC writes ignored */
    if (p >= 0x10000000u && p < 0x14000000u)
        return; /* TA FIFO / YUV / direct texture path: no TA in the spike */
    if (p >= 0x00600000u && p < 0x00700000u)
        return; /* modem */
    dc_hw_write_log(m, "write unmapped", a, v);
}

uint8_t dc_read8(sh4_cpu *c, uint32_t a)
{
    return (uint8_t)read_any(M(c), a, 1);
}
uint16_t dc_read16(sh4_cpu *c, uint32_t a)
{
    return (uint16_t)read_any(M(c), a, 2);
}
uint32_t dc_read32(sh4_cpu *c, uint32_t a)
{
    return read_any(M(c), a, 4);
}
void dc_write8(sh4_cpu *c, uint32_t a, uint8_t v)
{
    write_any(M(c), a, v, 1);
}
void dc_write16(sh4_cpu *c, uint32_t a, uint16_t v)
{
    write_any(M(c), a, v, 2);
}
void dc_write32(sh4_cpu *c, uint32_t a, uint32_t v)
{
    write_any(M(c), a, v, 4);
}

void dc_pref(sh4_cpu *c, uint32_t a)
{
    if ((a >> 26) != (0xE0000000u >> 26))
        return;
    dc_machine *m = M(c);
    unsigned q = (a >> 5) & 1;
    uint32_t dest = ((m->qacr[q] & 0x1Cu) << 24) | (a & 0x03FFFFE0u);
    for (int i = 0; i < 8; i++)
        write_any(m, dest + (uint32_t)i * 4, m->sq[q][i], 4);
}
