/* gc_exi.c -- the external interface and its devices (XBA-264): memory cards, the IPL chip's RTC and SRAM. See
 * gc_exi.h.
 *
 * Channels: three blocks of five 32-bit registers at 0x0C006800 (CSR, MAR, LENGTH, CR, DATA). CSR: EXIINT mask /
 * status (bits 0, 1), TCINT (2, 3), clock (4-6), chip selects CS0-CS2 (7-9), EXTINT (10, 11), EXT (12: a device in
 * the slot, channels 0 and 1), ROMDIS (13, channel 0); the status bits clear when written with 1. CR: TSTART (0),
 * DMA (1), read / write / both (2-3), immediate length - 1 (4-5). Transfers complete at once: a byte at a time
 * through the selected device, then TCINT. An immediate transfer shifts DATA out from its top byte and what comes
 * back in at the same place.
 *
 * Devices see the transfer as a byte stream between their chip select's rising and falling edges (select resets
 * their position; deselect is when a card programs or erases what the command asked for). A card raises its
 * interrupt (EXIINT) when an erase or a program is done, if the game enabled it (command 0x81 01); clear status
 * (0x89) drops it. The CARD libraries also arm a timeout, so the interrupt must come: it comes at the deselect.
 *
 * Facts from public documentation (YAGCD), the libogc sources (zlib licence) as the client side, and Dolphin's
 * behaviour where hardware details matter (its code is not used here). */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gc_exi.h"
#include "gc_internal.h"

enum {
    CSR_EXIINT_MASK = 1u << 0, CSR_EXIINT = 1u << 1, CSR_TCINT_MASK = 1u << 2, CSR_TCINT = 1u << 3,
    CSR_EXTINT_MASK = 1u << 10, CSR_EXTINT = 1u << 11, CSR_EXT = 1u << 12, CSR_ROMDIS = 1u << 13,
    CSR_INTS = CSR_EXIINT | CSR_TCINT | CSR_EXTINT,
};

/* Card status bits (command 0x83). */
enum {
    ST_READY = 0x01, ST_PROGRAM_ERR = 0x08, ST_ERASE_ERR = 0x10, ST_SLEEP = 0x20, ST_UNLOCKED = 0x40, ST_BUSY = 0x80,
};
#define CARD_LATENCY 4        /* dummy bytes between a read's address and its data (the ID's latency field 0) */
#define CARD_SECTOR 0x2000u   /* 8 KB (the ID's sector size field 0) */
#define CARD_PAGE 128u        /* page program's buffer */
#define CARD_VENDOR 0xC221u   /* command 0x85: a Nintendo card */

#define IPL_ROM_END 0x200000u /* 2 MB of mask ROM (the IPL, fonts): zeros here */
#define IPL_RTC 0x800000u     /* the RTC's 4 bytes, then the SRAM's 64 */
#define IPL_RTC_SRAM_LEN 0x44u
#define GC_EPOCH_UNIX 946684800LL /* 2000-01-01 00:00 in Unix time */
#define STATE_MAGIC 0x58494531u   /* "EXI1" */

/* The transfer state (a save state's blob, as is: no pointers). */
typedef struct card_dev {
    uint8_t present, mbit, cmd, status;
    uint8_t irq_on, irq_set, pad[2];
    uint32_t pos, addr;
    uint8_t buf[CARD_PAGE];
} card_dev;

typedef struct ipl_dev {
    uint32_t cmd, nbytes, cursor;
    uint32_t rtc_latched, rtc_written;
    uint32_t pad;
    int64_t rtc_offset; /* seconds the game moved the clock by */
    uint8_t sram[64];
} ipl_dev;

typedef struct exi_pod {
    uint32_t magic, size;
    uint32_t regs[15];
    uint32_t pad;
    card_dev card[2];
    ipl_dev ipl;
} exi_pod;

struct gc_exi {
    exi_pod s;
    uint8_t *img[2];
    uint64_t writes[2];
    int64_t rtc_fixed; /* < 0: the host's clock */
};

/* ---- sizes ---- */

size_t gc_card_bytes(int mbit)
{
    return mbit == 4 || mbit == 16 || mbit == 64 ? (size_t)mbit << 17 : 0;
}

int gc_card_mbit_of_size(size_t n)
{
    return n == gc_card_bytes(4) ? 4 : n == gc_card_bytes(16) ? 16 : n == gc_card_bytes(64) ? 64 : 0;
}

int gc_card_mbit_of_blocks(long blocks)
{
    switch (blocks) {
    case 59: case 4: return 4;
    case 251: case 16: return 16;
    case 1019: case 64: return 64;
    default: return 0;
    }
}

/* ---- SRAM ---- */

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

/* The checksums at 0 / 2: the sum of the four 16-bit words at 0x0C-0x13 and the sum of their complements. */
static void sram_checksum(uint8_t *s)
{
    uint16_t c = 0, inv = 0;
    for (int i = 0; i < 4; i++) {
        uint16_t w = be16(s + 0x0C + 2 * i);
        c = (uint16_t)(c + w);
        inv = (uint16_t)(inv + (uint16_t)~w);
    }
    s[0] = (uint8_t)(c >> 8);
    s[1] = (uint8_t)c;
    s[2] = (uint8_t)(inv >> 8);
    s[3] = (uint8_t)inv;
}

/* What a console out of the box after its first setup holds: English, NTSC, stereo, the setup done (flags 0x2C, as
 * the IPL writes them: bit 5 always set, bit 3 setup done, bit 2 stereo, bits 0-1 video mode), no RTC bias, no
 * flash IDs (their checksums 0xFF: the complement of a zero sum). */
static void sram_defaults(uint8_t *s)
{
    memset(s, 0, 64);
    s[0x13] = 0x2C;
    s[0x3A] = s[0x3B] = 0xFF;
    sram_checksum(s);
}

/* A card's header (its first 32 bytes, the serial the format made from the console's flash ID and the format time)
 * gives the flash ID that slot must have in SRAM, and its checksum (the format's derivation run backwards). */
static void sram_flash_id(uint8_t *s, int slot, const uint8_t *hdr)
{
    uint64_t rnd = 0;
    for (int i = 0; i < 8; i++)
        rnd = rnd << 8 | hdr[12 + i];
    uint8_t sum = 0;
    for (int i = 0; i < 12; i++) {
        rnd = (rnd * 0x41C64E6Dull + 0x3039ull) >> 16;
        uint8_t id = (uint8_t)(hdr[i] - (uint8_t)rnd);
        s[0x14 + 12 * slot + i] = id;
        sum = (uint8_t)(sum + id);
        rnd = ((rnd * 0x41C64E6Dull + 0x3039ull) >> 16) & 0x7FFF;
    }
    s[0x3A + slot] = (uint8_t)~sum;
}

/* ---- the machine's devices ---- */

struct gc_exi *gc_exi_create(void)
{
    struct gc_exi *x = calloc(1, sizeof *x);
    if (!x)
        return NULL;
    x->s.magic = STATE_MAGIC;
    x->s.size = sizeof x->s;
    sram_defaults(x->s.ipl.sram);
    x->rtc_fixed = -1;
    return x;
}

void gc_exi_free(struct gc_exi *x)
{
    if (!x)
        return;
    free(x->img[0]);
    free(x->img[1]);
    free(x);
}

void gc_exi_tv_mode(gc_machine *m, int pal)
{
    uint8_t *s = m->exidev->s.ipl.sram;
    s[0x13] = (uint8_t)((s[0x13] & ~3u) | (pal ? 1u : 0u));
    sram_checksum(s);
}

void gc_exi_set_language(gc_machine *m, int lang)
{
    uint8_t *s = m->exidev->s.ipl.sram;
    s[0x12] = (uint8_t)(lang >= 0 && lang <= 5 ? lang : 0);
    sram_checksum(s);
}

void gc_exi_sram(const gc_machine *m, uint8_t out[64])
{
    memcpy(out, m->exidev->s.ipl.sram, 64);
}

/* The clock without the game's changes: the host's local time, or the fixed start plus emulated seconds. */
static int64_t rtc_base(gc_machine *m)
{
    if (m->exidev->rtc_fixed >= 0)
        return m->exidev->rtc_fixed + (int64_t)(m->h.now / GC_CPU_HZ);
    time_t t = time(NULL);
    struct tm lt;
    long off = 0;
    if (localtime_r(&t, &lt))
        off = lt.tm_gmtoff;
    return (int64_t)t + off - GC_EPOCH_UNIX;
}

uint32_t gc_exi_rtc(gc_machine *m)
{
    return (uint32_t)(rtc_base(m) + m->exidev->s.ipl.rtc_offset);
}

void gc_exi_set_rtc(gc_machine *m, int64_t seconds)
{
    m->exidev->rtc_fixed = seconds < 0 ? -1 : seconds;
}

static void exi_int_update(gc_machine *m)
{
    bool on = false;
    for (int ch = 0; ch < 3; ch++) {
        uint32_t csr = m->exi[ch * 5];
        on |= ((csr & CSR_EXIINT) && (csr & CSR_EXIINT_MASK)) || ((csr & CSR_TCINT) && (csr & CSR_TCINT_MASK)) ||
              ((csr & CSR_EXTINT) && (csr & CSR_EXTINT_MASK));
    }
    if (on)
        m->pi_intsr |= GC_INT_EXI;
    else
        m->pi_intsr &= ~GC_INT_EXI;
}

/* ---- cards ---- */

int gc_card_insert(gc_machine *m, int slot, int mbit, const uint8_t *image)
{
    size_t n = gc_card_bytes(mbit);
    if (slot < 0 || slot > 1 || !n)
        return -1;
    uint8_t *img = malloc(n);
    if (!img)
        return -1;
    if (image)
        memcpy(img, image, n);
    else
        memset(img, 0xFF, n);
    struct gc_exi *x = m->exidev;
    free(x->img[slot]);
    x->img[slot] = img;
    card_dev *c = &x->s.card[slot];
    memset(c, 0, sizeof *c);
    c->present = 1;
    c->mbit = (uint8_t)mbit;
    c->status = ST_UNLOCKED | ST_READY; /* always unlocked: no DSP handshake */
    sram_flash_id(x->s.ipl.sram, slot, img);
    if (m->h.now) { /* a card that comes while the machine runs: the slot's EXTINT */
        m->exi[slot * 5] |= CSR_EXTINT;
        exi_int_update(m);
        gc_check_interrupts(m);
    }
    return 0;
}

void gc_card_remove(gc_machine *m, int slot)
{
    if (slot < 0 || slot > 1 || !m->exidev->img[slot])
        return;
    struct gc_exi *x = m->exidev;
    free(x->img[slot]);
    x->img[slot] = NULL;
    memset(&x->s.card[slot], 0, sizeof x->s.card[slot]);
    m->exi[slot * 5] |= CSR_EXTINT;
    exi_int_update(m);
    gc_check_interrupts(m);
}

const uint8_t *gc_card_data(const gc_machine *m, int slot, size_t *n)
{
    if (slot < 0 || slot > 1 || !m->exidev->img[slot]) {
        if (n)
            *n = 0;
        return NULL;
    }
    if (n)
        *n = gc_card_bytes(m->exidev->s.card[slot].mbit);
    return m->exidev->img[slot];
}

uint64_t gc_card_writes(const gc_machine *m, int slot)
{
    return slot >= 0 && slot <= 1 ? m->exidev->writes[slot] : 0;
}

/* An erase or a program is done: ready, and the card's interrupt if enabled. */
static void card_done(gc_machine *m, int slot)
{
    card_dev *c = &m->exidev->s.card[slot];
    c->status = (uint8_t)((c->status | ST_READY) & ~ST_BUSY);
    m->exidev->writes[slot]++;
    if (c->irq_on) {
        c->irq_set = 1;
        m->exi[slot * 5] |= CSR_EXIINT;
    }
}

static void card_deselect(gc_machine *m, int slot)
{
    struct gc_exi *x = m->exidev;
    card_dev *c = &x->s.card[slot];
    uint8_t *img = x->img[slot];
    uint32_t size = (uint32_t)gc_card_bytes(c->mbit);
    if (!img)
        return;
    switch (c->cmd) {
    case 0xF1: /* sector erase */
        if (c->pos >= 3) {
            memset(img + (c->addr & (size - 1) & ~(CARD_SECTOR - 1)), 0xFF, CARD_SECTOR);
            card_done(m, slot);
        }
        break;
    case 0xF4: /* chip erase */
        if (c->pos >= 3) {
            memset(img, 0xFF, size);
            card_done(m, slot);
        }
        break;
    case 0xF2: /* page program: the bytes sent after the address, within the 128-byte page */
        if (c->pos >= 5) {
            uint32_t n = c->pos - 5 < CARD_PAGE ? c->pos - 5 : CARD_PAGE;
            uint32_t page = c->addr & (size - 1) & ~(CARD_PAGE - 1);
            for (uint32_t i = 0; i < n; i++)
                img[page | ((c->addr + i) & (CARD_PAGE - 1))] = c->buf[i];
            card_done(m, slot);
        }
        break;
    default:
        break;
    }
    c->cmd = 0xFF;
}

/* A card's address bytes (read array, page program): AD1 bits 17-23, AD2 9-16, AD3 7-8, BA 0-6. */
static void card_addr_byte(card_dev *c, uint32_t pos, uint8_t in)
{
    switch (pos) {
    case 1: c->addr = (uint32_t)(in & 0x7F) << 17; break;
    case 2: c->addr |= (uint32_t)in << 9; break;
    case 3: c->addr |= (uint32_t)(in & 3) << 7; break;
    case 4: c->addr |= in & 0x7Fu; break;
    }
}

static uint8_t card_xfer(gc_machine *m, int slot, uint8_t in)
{
    struct gc_exi *x = m->exidev;
    card_dev *c = &x->s.card[slot];
    uint32_t pos = c->pos++;
    uint32_t size = (uint32_t)gc_card_bytes(c->mbit);
    if (pos == 0) {
        c->cmd = in;
        switch (in) {
        case 0x89: /* clear status: the error bits, the interrupt */
            c->status = (uint8_t)((c->status & ~(ST_PROGRAM_ERR | ST_ERASE_ERR)) | ST_READY);
            c->irq_set = 0;
            break;
        case 0x88: c->status |= ST_SLEEP; break;
        case 0x87: c->status &= (uint8_t)~ST_SLEEP; break;
        default: break;
        }
        return 0xFF;
    }
    switch (c->cmd) {
    case 0x00: /* ID: a dummy byte, then the 32-bit ID (the size in Mbit; latency and sector size fields 0) */
        return pos == 1 ? 0x80 : (uint8_t)((uint32_t)c->mbit >> (24 - 8 * ((pos - 2) & 3)));
    case 0x52: /* read array: address, latency, then bytes, wrapping within the 512-byte page */
        if (pos <= 4) {
            card_addr_byte(c, pos, in);
            return 0xFF;
        }
        if (pos < 5 + CARD_LATENCY)
            return 0xFF;
        {
            uint8_t out = x->img[slot][c->addr & (size - 1)];
            c->addr = (c->addr & ~0x1FFu) | ((c->addr + 1) & 0x1FFu);
            return out;
        }
    case 0x83: /* read status */
        return c->status;
    case 0x85: /* read vendor ID: a dummy byte, then the 16-bit ID over and over */
        return (uint8_t)(pos == 1 || !(pos & 1) ? CARD_VENDOR >> 8 : CARD_VENDOR);
    case 0x81: /* interrupt enable */
        if (pos == 1)
            c->irq_on = in & 1;
        return 0xFF;
    case 0xF1: /* sector erase: AD1 (bits 17-23), AD2 (9-16) */
        if (pos == 1)
            c->addr = (uint32_t)(in & 0x7F) << 17;
        else if (pos == 2)
            c->addr |= (uint32_t)in << 9;
        return 0xFF;
    case 0xF2: /* page program: address, then up to 128 bytes into the page buffer */
        if (pos <= 4)
            card_addr_byte(c, pos, in);
        else
            c->buf[(pos - 5) & (CARD_PAGE - 1)] = in;
        return 0xFF;
    default: /* 0xF4 chip erase, and what we do not model (0x53, 0x82, 0x86, 0xF3) */
        return 0xFF;
    }
}

/* ---- the IPL chip: a 4-byte command (bit 31 write, address in bits 6-30), then data at that address ---- */

static uint8_t ipl_xfer(gc_machine *m, uint8_t in)
{
    ipl_dev *d = &m->exidev->s.ipl;
    if (d->nbytes < 4) {
        d->cmd = d->cmd << 8 | in;
        if (++d->nbytes == 4)
            d->rtc_latched = gc_exi_rtc(m); /* the clock as the command arrives */
        return 0;
    }
    uint32_t a = (d->cmd & 0x7FFFFFFFu) >> 6;
    bool wr = d->cmd >> 31;
    if (a < IPL_ROM_END)
        return 0; /* no IPL ROM, no font */
    if (a >= IPL_RTC && a < IPL_RTC + IPL_RTC_SRAM_LEN) {
        uint32_t off = (a - IPL_RTC + d->cursor++) % IPL_RTC_SRAM_LEN;
        if (off < 4) {
            unsigned sh = 24 - 8 * off;
            if (!wr)
                return (uint8_t)(d->rtc_latched >> sh);
            d->rtc_written = (d->rtc_written & ~(0xFFu << sh)) | (uint32_t)in << sh;
            if (off == 3) /* the game set the clock */
                d->rtc_offset = (int64_t)d->rtc_written - rtc_base(m);
            return 0;
        }
        if (wr)
            d->sram[off - 4] = in;
        return wr ? 0 : d->sram[off - 4];
    }
    return 0; /* UART (OSReport output), the rest: dropped */
}

/* ---- channels ---- */

/* The device on ch's chip select d: 0 / 1 card A / B, 2 the IPL chip, -1 nothing. */
static int device_of(gc_machine *m, unsigned ch, unsigned d)
{
    if (d == 0 && ch < 2)
        return m->exidev->img[ch] ? (int)ch : -1;
    if (ch == 0 && d == 1)
        return 2;
    return -1;
}

static int selected(gc_machine *m, unsigned ch)
{
    uint32_t cs = (m->exi[ch * 5] >> 7) & 7;
    return cs & 1 ? device_of(m, ch, 0) : cs & 2 ? device_of(m, ch, 1) : cs & 4 ? device_of(m, ch, 2) : -1;
}

static void dev_select(gc_machine *m, int dev, bool on)
{
    if (dev == 0 || dev == 1) {
        if (on)
            m->exidev->s.card[dev].pos = 0;
        else
            card_deselect(m, dev);
    } else if (dev == 2 && on) {
        ipl_dev *d = &m->exidev->s.ipl;
        d->nbytes = d->cursor = 0;
        d->cmd = 0;
    }
}

/* One byte out to the selected device and its byte back; nothing there reads 0. */
static uint8_t xfer(gc_machine *m, int dev, uint8_t in)
{
    if (dev == 0 || dev == 1)
        return card_xfer(m, dev, in);
    if (dev == 2)
        return ipl_xfer(m, in);
    return 0;
}

void gc_exi_write32(gc_machine *m, unsigned idx, uint32_t v)
{
    unsigned ch = idx / 5, reg = idx % 5;
    uint32_t *r = &m->exi[ch * 5];
    if (reg == 0) {
        uint32_t old = r[0];
        uint32_t ints = old & CSR_INTS & ~v; /* write 1 to clear */
        r[0] = (v & ~(CSR_INTS | CSR_EXT)) | ints | (ch == 0 ? old & CSR_ROMDIS : 0);
        uint32_t ocs = (old >> 7) & 7, ncs = (r[0] >> 7) & 7;
        for (unsigned d = 0; d < 3; d++)
            if (((ocs ^ ncs) >> d) & 1)
                dev_select(m, device_of(m, ch, d), (ncs >> d) & 1);
    } else if (reg == 3) {
        r[3] = v;
        if (v & 1) { /* TSTART: done at once */
            int dev = selected(m, ch);
            unsigned rw = (v >> 2) & 3;
            if (v & 2) { /* DMA, in 32-byte units */
                uint32_t mar = r[1] & 0x03FFFFE0u, len = r[2] & 0x03FFFFE0u;
                for (uint32_t i = 0; i < len; i++) {
                    bool in_ram = mar + i < GC_RAM_SIZE;
                    if (rw == 0) {
                        uint8_t b = xfer(m, dev, 0);
                        if (in_ram)
                            m->h.ram[mar + i] = b;
                    } else if (rw == 1) {
                        xfer(m, dev, in_ram ? m->h.ram[mar + i] : 0);
                    }
                }
            } else { /* immediate: up to 4 bytes of DATA, from its top */
                unsigned len = ((v >> 4) & 3) + 1;
                uint32_t out = r[4], back = 0;
                for (unsigned i = 0; i < len; i++)
                    back |= (uint32_t)xfer(m, dev, (uint8_t)(out >> (24 - 8 * i))) << (24 - 8 * i);
                if (rw != 1)
                    r[4] = back;
            }
            r[3] &= ~1u;
            r[0] |= CSR_TCINT;
        }
    } else {
        r[reg] = v;
    }
    exi_int_update(m);
    gc_check_interrupts(m);
}

uint32_t gc_exi_read32(gc_machine *m, unsigned idx)
{
    uint32_t v = m->exi[idx];
    if (idx % 5 == 0) {
        unsigned ch = idx / 5;
        v &= ~CSR_EXT;
        if (ch < 2 && m->exidev->img[ch])
            v |= CSR_EXT; /* a card in the slot */
    }
    return v;
}

/* ---- save states ---- */

size_t gc_exi_state_size(void)
{
    return sizeof(exi_pod);
}

size_t gc_exi_state_save(const gc_machine *m, uint8_t *out, size_t cap)
{
    if (cap < sizeof(exi_pod))
        return 0;
    exi_pod p = m->exidev->s;
    memcpy(p.regs, m->exi, sizeof p.regs);
    memcpy(out, &p, sizeof p);
    return sizeof p;
}

int gc_exi_state_check(const uint8_t *in, size_t n)
{
    exi_pod p;
    if (n != sizeof p)
        return -1;
    memcpy(&p, in, sizeof p);
    return p.magic == STATE_MAGIC && p.size == sizeof p ? 0 : -1;
}

int gc_exi_state_load(gc_machine *m, const uint8_t *in, size_t n)
{
    exi_pod p;
    if (gc_exi_state_check(in, n))
        return -1;
    memcpy(&p, in, sizeof p);
    struct gc_exi *x = m->exidev;
    memcpy(m->exi, p.regs, sizeof p.regs);
    for (int s = 0; s < 2; s++) {
        card_dev *c = &x->s.card[s];
        if (c->present && p.card[s].present && p.card[s].mbit == c->mbit)
            *c = p.card[s]; /* the command under way, status, interrupt */
    }
    x->s.ipl = p.ipl;
    /* the cards in the machine decide EXT and the flash IDs */
    for (int s = 0; s < 2; s++)
        if (x->img[s])
            sram_flash_id(x->s.ipl.sram, s, x->img[s]);
    exi_int_update(m);
    gc_check_interrupts(m);
    return 0;
}
