/* gc_si.c -- the serial interface and the standard controllers on its four ports (XBA-259).
 *
 * Registers (0x0C006400, 32-bit, index = offset / 4):
 *   0..11   per channel n: SICnOUTBUF (3n: the command the channel polls with: 0x40 = read the pad, its analog mode,
 *           the rumble motor), SICnINBUFH / SICnINBUFL (3n+1, 3n+2: the last poll's answer; reading H clears RDST)
 *   12      SIPOLL: lines between polls (X), polls per field (Y), the channels that poll (EN, bit 7 - n)
 *   13      SICOMCSR: a transfer through the I/O buffer (TSTART, channel, out / in lengths, 0 = 128 bytes), its
 *           interrupt (TCINT, write 1 to clear) and the poll's (RDSTINT: some channel has a fresh answer)
 *   14      SISR: per channel (byte 3 - n) RDST / WRST and the error bits (NOREP: nothing answered; write 1 to
 *           clear); WR (bit 31) sends the OUTBUFs
 *   0x20..  the 128-byte I/O buffer (0x80..0xFF), big-endian bytes
 * Transfers complete at once. A port with nothing plugged in answers nothing (NOREP, COMERR); a standard controller
 * answers what libogc's PAD library sends: 0x00 / 0xFF (type: 09 00 00, a standard controller with a motor), 0x41 /
 * 0x42 (origin / recalibrate: sticks centred, triggers released), 0x40 (the pad in the analog mode asked). The
 * channels that poll take the pad's state at the field start and every X lines after (gc_si_line): libogc asks
 * X = 246 lines, two polls per NTSC field, so a PAD_Read once per field always finds a fresh answer. */
#include <string.h>

#include "gc_internal.h"

enum {
    CSR_TSTART = 1u << 0, CSR_RDSTINTMSK = 1u << 27, CSR_RDSTINT = 1u << 28, CSR_COMERR = 1u << 29,
    CSR_TCINTMSK = 1u << 30, CSR_TCINT = 1u << 31,
};
#define SR_RDST(n) (1u << (29 - 8 * (n)))
#define SR_WRST(n) (1u << (28 - 8 * (n)))
#define SR_NOREP(n) (1u << (27 - 8 * (n)))
#define SR_ERRORS 0x0F0F0F0Fu /* UNRUN OVRUN COLL NOREP of each channel */
#define SR_WR (1u << 31)

static void si_int_update(gc_machine *m)
{
    uint32_t cs = m->si[13];
    bool on = ((cs & CSR_TCINT) && (cs & CSR_TCINTMSK)) || ((cs & CSR_RDSTINT) && (cs & CSR_RDSTINTMSK));
    if (on)
        m->pi_intsr |= GC_INT_SI;
    else
        m->pi_intsr &= ~GC_INT_SI;
}

static void rdst_update(gc_machine *m)
{
    bool any = false;
    for (int n = 0; n < 4; n++)
        any |= (m->si[14] & SR_RDST(n)) != 0;
    if (any)
        m->si[13] |= CSR_RDSTINT;
    else
        m->si[13] &= ~CSR_RDSTINT;
}

/* The answer to a 0x40 poll in analog mode `mode`: the high word (buttons, main stick) and the low word (C stick,
 * triggers, analog A / B at 4 or 8 bits depending on the mode; libogc asks mode 3). */
static void pad_answer(const gc_pad_state *p, unsigned mode, uint32_t *hi, uint32_t *lo)
{
    /* 0x0080: "use origin", always set by a standard controller; 0x2000 (asks the host to read the origin again)
     * and the error bits stay clear */
    uint32_t buttons = (p->buttons & 0x1F7Fu) | 0x0080u;
    *hi = buttons << 16 | (uint32_t)p->stick_x << 8 | p->stick_y;
    uint32_t cx = p->cstick_x, cy = p->cstick_y, l = p->trigger_l, r = p->trigger_r;
    uint32_t aa = (p->buttons & GC_PAD_A) ? 0xFF : 0, ab = (p->buttons & GC_PAD_B) ? 0xFF : 0;
    switch (mode & 7) {
    case 1:
        *lo = (cx >> 4) << 28 | (cy >> 4) << 24 | l << 16 | r << 8 | (aa >> 4) << 4 | ab >> 4;
        break;
    case 2:
        *lo = (cx >> 4) << 28 | (cy >> 4) << 24 | (l >> 4) << 20 | (r >> 4) << 16 | aa << 8 | ab;
        break;
    case 3:
        *lo = cx << 24 | cy << 16 | l << 8 | r;
        break;
    case 4:
        *lo = cx << 24 | cy << 16 | aa << 8 | ab;
        break;
    default: /* 0, 5, 6, 7 */
        *lo = cx << 24 | cy << 16 | (l >> 4) << 12 | (r >> 4) << 8 | (aa >> 4) << 4 | ab >> 4;
        break;
    }
}

static uint8_t buf_get(const gc_machine *m, unsigned i)
{
    return (uint8_t)(m->si[0x20 + (i >> 2 & 31)] >> (24 - 8 * (i & 3)));
}

static void buf_put(gc_machine *m, unsigned i, uint8_t v)
{
    uint32_t *w = &m->si[0x20 + (i >> 2 & 31)];
    unsigned sh = 24 - 8 * (i & 3);
    *w = (*w & ~(0xFFu << sh)) | (uint32_t)v << sh;
}

/* A command through the I/O buffer: the answer's length, or -1 when nothing answers. */
static int device_transfer(gc_machine *m, int ch, const uint8_t *out, unsigned out_len, uint8_t *in)
{
    const gc_pad_state *p = &m->pad[ch];
    if (!p->connected || !out_len)
        return -1;
    switch (out[0]) {
    case 0x00: /* type and status */
    case 0xFF: /* reset */
        in[0] = 0x09; /* SI_TYPE_GC | SI_GC_STANDARD: a standard controller (with its motor) */
        in[1] = 0x00;
        in[2] = 0x00;
        return 3;
    case 0x41: /* origin */
    case 0x42: /* recalibrate */
        memset(in, 0, 10);
        in[1] = 0x80;
        in[2] = in[3] = in[4] = in[5] = 0x80; /* sticks at rest */
        return 10;
    case 0x40: { /* direct read */
        uint32_t hi, lo;
        pad_answer(p, out_len > 1 ? out[1] : 3, &hi, &lo);
        for (int i = 0; i < 4; i++) {
            in[i] = (uint8_t)(hi >> (24 - 8 * i));
            in[4 + i] = (uint8_t)(lo >> (24 - 8 * i));
        }
        return 8;
    }
    default:
        return -1;
    }
}

static void transfer(gc_machine *m, uint32_t csr)
{
    int ch = (int)((csr >> 1) & 3);
    unsigned out_len = (csr >> 16) & 0x7F, in_len = (csr >> 8) & 0x7F;
    if (!out_len)
        out_len = 128;
    if (!in_len)
        in_len = 128;
    uint8_t out[128], in[128];
    for (unsigned i = 0; i < out_len; i++)
        out[i] = buf_get(m, i);
    int n = device_transfer(m, ch, out, out_len, in);
    if (n < 0) {
        m->si[14] |= SR_NOREP(ch);
        m->si[13] |= CSR_COMERR;
    } else {
        for (unsigned i = 0; i < in_len && i < (unsigned)n; i++)
            buf_put(m, i, in[i]);
    }
    if (m->verbose >= 2)
        fprintf(stderr, "gc: SI transfer ch %d cmd %02X out %u in %u -> %d\n", ch, out[0], out_len, in_len, n);
    m->si[13] |= CSR_TCINT;
}

uint32_t gc_si_read(gc_machine *m, unsigned idx)
{
    idx &= 0x3F;
    if (idx < 12 && idx % 3 == 1) { /* INBUFH: the answer is taken */
        int ch = (int)(idx / 3);
        uint32_t v = m->si[idx];
        if (m->si[14] & SR_RDST(ch)) {
            m->si[14] &= ~SR_RDST(ch);
            rdst_update(m);
            si_int_update(m);
            gc_check_interrupts(m);
        }
        return v;
    }
    return m->si[idx];
}

void gc_si_write(gc_machine *m, unsigned idx, uint32_t v)
{
    idx &= 0x3F;
    if (idx == 13) { /* COMCSR */
        /* TCINT: write 1 to clear. COMERR: the last transfer's result (libogc reads it after writing it back), read
         * only, cleared when the next one starts. RDSTINT: read only. */
        uint32_t keep = m->si[13] & CSR_TCINT & ~v;
        uint32_t status = m->si[13] & (CSR_RDSTINT | CSR_COMERR);
        if (v & CSR_TSTART)
            status &= ~CSR_COMERR;
        m->si[13] = (v & ~(CSR_TCINT | CSR_COMERR | CSR_RDSTINT | CSR_TSTART)) | keep | status;
        if (v & CSR_TSTART)
            transfer(m, v);
    } else if (idx == 14) { /* SISR */
        m->si[14] &= ~(v & SR_ERRORS);
        if (v & SR_WR) /* the OUTBUFs go to the devices at once */
            for (int n = 0; n < 4; n++)
                m->si[14] &= ~SR_WRST(n);
    } else if (idx < 12 && idx % 3 != 0) {
        /* INBUFs: read only */
    } else {
        m->si[idx] = v;
    }
    si_int_update(m);
    gc_check_interrupts(m);
}

static void si_poll(gc_machine *m)
{
    uint32_t poll = m->si[12];
    bool any = false;
    for (int n = 0; n < 4; n++) {
        if (!(poll & (0x80u >> n)))
            continue;
        uint32_t cmd = m->si[3 * n];
        const gc_pad_state *p = &m->pad[n];
        if (p->connected && ((cmd >> 16) & 0xFF) == 0x40) {
            pad_answer(p, (cmd >> 8) & 0xFF, &m->si[3 * n + 1], &m->si[3 * n + 2]);
            m->si[14] |= SR_RDST(n);
            m->si[14] &= ~SR_NOREP(n);
        } else {
            m->si[3 * n + 1] = 0x80000000u; /* ERRSTAT */
            m->si[14] |= SR_NOREP(n);
        }
        any = true;
    }
    if (!any)
        return;
    rdst_update(m);
    si_int_update(m);
    gc_check_interrupts(m);
}

void gc_si_line(gc_machine *m, uint32_t field_line)
{
    uint32_t x = (m->si[12] >> 16) & 0x3FF;
    if (field_line == 0 || (x && field_line % x == 0))
        si_poll(m);
}

void gc_set_pad(gc_machine *m, int port, const gc_pad_state *st)
{
    if (port < 0 || port > 3)
        return;
    if (st)
        m->pad[port] = *st;
    else
        memset(&m->pad[port], 0, sizeof m->pad[port]);
}

int gc_pad_motor(const gc_machine *m, int port)
{
    if (port < 0 || port > 3 || !m->pad[port].connected || ((m->si[3 * port] >> 16) & 0xFF) != 0x40)
        return 0;
    return (int)(m->si[3 * port] & 3);
}
