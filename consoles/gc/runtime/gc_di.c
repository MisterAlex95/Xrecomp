/* gc_di.c -- the DVD interface (XBA-261): registers at 0xCC006000, drive commands with DMA, a read-time estimate,
 * the transfer-complete / error / break / cover interrupts, audio streaming state, disc change. See gc_boot.h. */
#include <stdlib.h>
#include <string.h>

#include "gc_di.h"

/* A GameCube disc: 1,459,978,240 bytes. Reads past an image shorter than that (test images) give zeros. */
#define DI_DISC_MAX 0x57058000ull

/* Sense codes (status byte << 24 | code). */
#define DI_ERR_NONE 0x000000u
#define DI_ERR_NO_MEDIUM 0x023A00u     /* cover open / no disc */
#define DI_ERR_BAD_COMMAND 0x052000u
#define DI_ERR_LBA_RANGE 0x052100u
#define DI_STATUS_COVER_OPEN 1u
#define DI_STATUS_NO_DISC 3u

static uint64_t us(double v)
{
    return (uint64_t)(v * (GC_CPU_HZ / 1e6));
}

gc_di *gc_di_get(gc_machine *m)
{
    if (!m->di) {
        gc_di *d = calloc(1, sizeof *d);
        if (!d)
            return NULL;
        d->done_at = UINT64_MAX;
        d->cover_close_at = UINT64_MAX;
        d->cover_open = true; /* no disc yet */
        d->reg[DI_CVR] = DICVR_OPEN;
        m->di = d;
    }
    return m->di;
}

void gc_di_free(gc_machine *m)
{
    gc_di *d = m->di;
    if (!d)
        return;
    gc_disc_close(d->disc);
    gc_disc_close(d->next);
    free(d);
    m->di = NULL;
}

static void di_irq(gc_machine *m)
{
    gc_di *d = m->di;
    uint32_t sr = d->reg[DI_SR], cvr = d->reg[DI_CVR];
    bool on = ((sr & DISR_TCINT) && (sr & DISR_TCINTMASK)) || ((sr & DISR_DEINT) && (sr & DISR_DEINTMASK))
              || ((sr & DISR_BRKINT) && (sr & DISR_BRKINTMASK)) || ((cvr & DICVR_INT) && (cvr & DICVR_INTMASK));
    if (on)
        m->pi_intsr |= GC_INT_DI;
    else
        m->pi_intsr &= ~GC_INT_DI;
    gc_check_interrupts(m);
}

uint64_t gc_di_next_event(gc_machine *m)
{
    gc_di *d = m->di;
    return d->done_at < d->cover_close_at ? d->done_at : d->cover_close_at;
}

void gc_disc_insert(gc_machine *m, gc_disc *disc)
{
    gc_di *d = gc_di_get(m);
    if (!d) {
        gc_disc_close(disc);
        return;
    }
    gc_disc_close(d->next);
    d->next = disc;
    if (!d->cover_open) {
        d->cover_open = true;
        d->reg[DI_CVR] |= DICVR_OPEN | DICVR_INT;
        fprintf(stderr, "gc: dvd: cover opened\n");
    }
    gc_disc_close(d->disc);
    d->disc = NULL;
    d->cover_close_at = disc ? m->h.now + us(500000) : UINT64_MAX;
    di_irq(m);
    gc_schedule(m);
}

gc_disc *gc_current_disc(gc_machine *m)
{
    return m->di && !m->di->cover_open ? m->di->disc : NULL;
}

uint32_t gc_boot_entry(const gc_machine *m)
{
    return m->di ? m->di->boot_entry : 0;
}

unsigned gc_boot_reads(const gc_machine *m)
{
    return m->di ? m->di->boot_reads : 0;
}

/* The audio stream's play position (bytes from the disc start): 48 kHz, 28 stereo samples per 32-byte block. */
static uint64_t stream_pos(gc_machine *m)
{
    gc_di *d = m->di;
    if (!d->stream_on)
        return d->stream_start + d->stream_len;
    uint64_t samples = (m->h.now - d->stream_t0) * 48000u / GC_CPU_HZ;
    uint64_t pos = samples / 28 * 32;
    if (pos >= d->stream_len) {
        d->stream_on = false;
        return d->stream_start + d->stream_len;
    }
    return d->stream_start + pos;
}

/* How long the drive takes to read len bytes at off: command overhead, a seek over the distance from the head
 * (none when the data is just ahead), a constant-angular-velocity transfer (2 MB/s at the hub, 3.1 MB/s at the
 * rim), spin-up after a stop-motor. */
static uint64_t read_time(gc_di *d, uint64_t off, uint32_t len)
{
    double t = 300.0; /* microseconds */
    uint64_t dist = off > d->head ? off - d->head : d->head - off;
    if (!(off >= d->head && off - d->head < 0x40000))
        t += 35000.0 + 100000.0 * (double)dist / (double)DI_DISC_MAX;
    double rate = 2.0e6 + 1.125e6 * (double)(off < DI_DISC_MAX ? off : DI_DISC_MAX) / (double)DI_DISC_MAX;
    t += (double)len / rate * 1e6;
    if (d->motor_off)
        t += 700000.0;
    return us(t);
}

static void start_command(gc_machine *m)
{
    gc_di *d = m->di;
    d->cmd0 = d->reg[DI_CMD0];
    d->cmd1 = d->reg[DI_CMD1];
    d->cmd2 = d->reg[DI_CMD2];
    d->mar = d->reg[DI_MAR] & 0x03FFFFE0u;
    d->len = d->reg[DI_LEN] & ~31u;
    d->fail = false;
    uint8_t op = (uint8_t)(d->cmd0 >> 24);
    uint64_t t = us(20);
    bool needs_disc = op == 0xA8 || op == 0xAB || op == 0xE1;
    if (needs_disc && (d->cover_open || !d->disc)) {
        d->fail = true;
        d->error = (d->cover_open ? DI_STATUS_COVER_OPEN : DI_STATUS_NO_DISC) << 24 | DI_ERR_NO_MEDIUM;
    } else if (op == 0xA8) {
        uint64_t off = (d->cmd0 & 0xFF) == 0x40 ? 0 : (uint64_t)d->cmd1 << 2;
        uint32_t n = (d->cmd0 & 0xFF) == 0x40 ? 0x20 : d->cmd2;
        if (off + n > DI_DISC_MAX) {
            d->fail = true;
            d->error = DI_ERR_LBA_RANGE;
        } else {
            t = read_time(d, off, n);
        }
    } else if (op == 0xAB) {
        uint64_t off = (uint64_t)d->cmd1 << 2;
        t = read_time(d, off, 0);
    } else if (op != 0x12 && op != 0xE0 && op != 0xE1 && op != 0xE2 && op != 0xE3 && op != 0xE4 && op != 0x55
               && op != 0xFE && op != 0xFF) {
        d->fail = true;
        d->error = DI_ERR_BAD_COMMAND;
    }
    if (m->verbose)
        fprintf(stderr, "gc: dvd: command %08X %08X %08X dma %08X+%X%s\n", d->cmd0, d->cmd1, d->cmd2, d->mar, d->len,
                d->fail ? " (fails)" : "");
    d->done_at = m->h.now + t;
    if (d->done_at < m->h.next_event)
        m->h.next_event = d->done_at;
}

static void dma_to_ram(gc_machine *m, uint32_t pa, const uint8_t *src, uint32_t n)
{
    if (pa < GC_RAM_SIZE)
        memcpy(m->h.ram + pa, src, pa + n <= GC_RAM_SIZE ? n : GC_RAM_SIZE - pa);
}

static void finish_command(gc_machine *m)
{
    gc_di *d = m->di;
    uint8_t op = (uint8_t)(d->cmd0 >> 24);
    d->done_at = UINT64_MAX;
    if (!d->fail) {
        switch (op) {
        case 0xA8: { /* read, or the disc ID (0xA8000040) */
            bool id = (d->cmd0 & 0xFF) == 0x40;
            uint64_t off = id ? 0 : (uint64_t)d->cmd1 << 2;
            uint32_t n = id ? 0x20 : d->cmd2;
            if (n > d->len)
                n = d->len;
            uint8_t *buf = malloc(n ? n : 1);
            if (!buf || gc_disc_read(d->disc, off, buf, n)) {
                d->fail = true;
                d->error = 0x030200u; /* unrecovered read error */
                if (gc_disc_error(d->disc))
                    fprintf(stderr, "gc: dvd: %s\n", gc_disc_error(d->disc));
            } else {
                dma_to_ram(m, d->mar, buf, n);
                d->head = off + n;
                d->motor_off = false;
            }
            free(buf);
            break;
        }
        case 0xAB:
            d->head = (uint64_t)d->cmd1 << 2;
            d->motor_off = false;
            break;
        case 0x12: { /* inquiry: revision, device code, release date */
            uint8_t info[0x20] = {0x00, 0x02, 0x00, 0x20, 0x20, 0x02, 0x04, 0x02};
            dma_to_ram(m, d->mar, info, d->len < 0x20 ? d->len : 0x20);
            break;
        }
        case 0xE0:
            d->reg[DI_IMM] = d->error;
            d->error = DI_ERR_NONE;
            break;
        case 0xE1: {
            unsigned sub = (d->cmd0 >> 16) & 0xFF;
            if (sub == 0 && d->cmd2) {
                d->stream_on = true;
                d->stream_start = (uint64_t)d->cmd1 << 2;
                d->stream_len = d->cmd2;
                d->stream_t0 = m->h.now;
            } else {
                d->stream_on = false;
            }
            break;
        }
        case 0xE2: {
            unsigned sub = (d->cmd0 >> 16) & 0xFF;
            uint64_t pos = stream_pos(m);
            d->reg[DI_IMM] = sub == 0   ? (uint32_t)d->stream_on
                             : sub == 1 ? (uint32_t)(pos >> 2)
                             : sub == 2 ? (uint32_t)(d->stream_start >> 2)
                                        : (uint32_t)d->stream_len;
            break;
        }
        case 0xE3:
            d->motor_off = true;
            d->stream_on = false;
            break;
        case 0xE4:
            d->stream_cfg = d->cmd0 & 0x00FFFFFFu;
            break;
        default: /* drive unlock / debug commands: accepted */
            break;
        }
    }
    if (d->fail) {
        d->reg[DI_SR] |= DISR_DEINT;
        if (m->verbose)
            fprintf(stderr, "gc: dvd: command %08X failed, error %08X\n", d->cmd0, d->error);
    } else {
        d->reg[DI_SR] |= DISR_TCINT;
        if (d->reg[DI_CR] & DICR_DMA) {
            d->reg[DI_MAR] = (d->reg[DI_MAR] + d->len) & 0x03FFFFE0u;
            d->reg[DI_LEN] = 0;
        }
    }
    d->reg[DI_CR] &= ~DICR_TSTART;
    di_irq(m);
}

void gc_di_event(gc_machine *m)
{
    gc_di *d = m->di;
    if (m->h.now >= d->done_at)
        finish_command(m);
    if (m->h.now >= d->cover_close_at) {
        d->cover_close_at = UINT64_MAX;
        d->disc = d->next;
        d->next = NULL;
        d->cover_open = false;
        d->head = 0;
        d->reg[DI_CVR] = (d->reg[DI_CVR] & ~DICVR_OPEN) | DICVR_INT;
        char key[48] = "?";
        if (d->disc)
            gc_disc_title_key(d->disc, key, sizeof key);
        fprintf(stderr, "gc: dvd: cover closed on disc %s\n", key);
        di_irq(m);
    }
}

uint32_t gc_di_read32(gc_machine *m, unsigned idx)
{
    gc_di *d = m->di;
    if (idx >= DI_NREG)
        return 0;
    if (idx == DI_CVR)
        return (d->reg[DI_CVR] & ~DICVR_OPEN) | (d->cover_open ? DICVR_OPEN : 0);
    return d->reg[idx];
}

void gc_di_write32(gc_machine *m, unsigned idx, uint32_t v)
{
    gc_di *d = m->di;
    if (!d || idx >= DI_NREG)
        return;
    switch (idx) {
    case DI_SR: {
        uint32_t ints = (DISR_DEINT | DISR_TCINT | DISR_BRKINT) & d->reg[DI_SR] & ~v; /* write 1 to clear */
        d->reg[DI_SR] = (v & (DISR_DEINTMASK | DISR_TCINTMASK | DISR_BRKINTMASK)) | ints;
        if ((v & DISR_BRK) && d->done_at != UINT64_MAX) { /* break: the command stops now */
            d->done_at = UINT64_MAX;
            d->reg[DI_CR] &= ~DICR_TSTART;
            d->reg[DI_SR] |= DISR_BRKINT;
        }
        break;
    }
    case DI_CVR: {
        uint32_t ints = DICVR_INT & d->reg[DI_CVR] & ~v;
        d->reg[DI_CVR] = (v & DICVR_INTMASK) | ints;
        break;
    }
    case DI_CR:
        d->reg[DI_CR] = v & 7;
        if ((v & DICR_TSTART) && d->done_at == UINT64_MAX)
            start_command(m);
        break;
    case DI_CMD0: case DI_CMD1: case DI_CMD2: case DI_MAR: case DI_LEN: case DI_IMM: case DI_CFG:
        d->reg[idx] = v;
        break;
    default:
        break;
    }
    di_irq(m);
}
