/* Placeholder CD-ROM controller (ps1_devices.h), built when ps1_cdrom.c (P-9, XBA-167) is absent: the register
 * interface (index, parameter / response FIFOs, interrupt enable / flags) with an empty drive, so a BIOS or a game
 * probing the drive gets answers instead of hanging: every command acknowledges with INT3 (status: shell open),
 * Init / Pause / Stop / seeks / ReadTOC complete with INT2, GetID and reads fail with INT5 (no disc), Test(20h) gives
 * a controller version. No data, no audio. */
#include "ps1_devices.h"

#include <stdlib.h>
#include <string.h>

#define STAT 0x10u        /* shell open: no disc */
#define FIRST_DELAY 25000 /* CPU cycles to the first response */
#define SECOND_DELAY 50000

typedef struct cd_resp {
    uint8_t irq;      /* INT1..INT5, 0 = none */
    uint8_t len;
    uint8_t data[8];
} cd_resp;

struct ps1_cdrom {
    ps1_irq_sink irq;
    uint8_t index, ie, iflag;
    uint8_t param[16];
    int nparam;
    uint8_t resp[16];
    int rpos, rlen;
    int busy;          /* cycles to the first response, 0 = none in flight */
    int second_wait;   /* cycles to the second response */
    cd_resp first, second;
    int have_first, have_second;
};

ps1_cdrom *ps1_cdrom_create(ps1_irq_sink irq, ps1_disc *disc)
{
    (void)disc;
    ps1_cdrom *c = calloc(1, sizeof *c);
    if (c)
        c->irq = irq;
    return c;
}

void ps1_cdrom_destroy(ps1_cdrom *c)
{
    free(c);
}

void ps1_cdrom_set_disc(ps1_cdrom *c, ps1_disc *disc)
{
    (void)c;
    (void)disc;
}

void ps1_cdrom_set_lid(ps1_cdrom *c, int open)
{
    (void)c;
    (void)open;
}

static void deliver(ps1_cdrom *c, const cd_resp *r)
{
    c->iflag = r->irq;
    memcpy(c->resp, r->data, r->len);
    c->rpos = 0;
    c->rlen = r->len;
    if ((c->ie & c->iflag & 0x1F) && c->irq.raise)
        c->irq.raise(c->irq.ctx, PS1_IRQ_CDROM);
}

static void set_resp(cd_resp *r, uint8_t irq, const uint8_t *data, uint8_t len)
{
    r->irq = irq;
    r->len = len;
    memcpy(r->data, data, len);
}

static void command(ps1_cdrom *c, uint8_t cmd)
{
    static const uint8_t stat[1] = {STAT};
    static const uint8_t version[4] = {0x94, 0x09, 0x19, 0xC0};
    static const uint8_t nodisc[8] = {0x08, 0x40, 0, 0, 0, 0, 0, 0};
    static const uint8_t err[2] = {STAT | 1, 0x80};
    c->have_second = 0;
    set_resp(&c->first, 3, stat, 1);
    switch (cmd) {
    case 0x19: /* Test */
        if (c->nparam > 0 && c->param[0] == 0x20)
            set_resp(&c->first, 3, version, 4);
        break;
    case 0x1A: /* GetID */
        set_resp(&c->second, 5, nodisc, 8);
        c->have_second = 1;
        break;
    case 0x0A: case 0x09: case 0x08: case 0x15: case 0x16: case 0x1E: case 0x07:
        set_resp(&c->second, 2, stat, 1);
        c->have_second = 1;
        break;
    case 0x06: case 0x1B: /* ReadN / ReadS: no disc */
        set_resp(&c->first, 5, err, 2);
        break;
    default:
        break;
    }
    c->nparam = 0;
    c->have_first = 1;
    c->busy = FIRST_DELAY;
}

uint8_t ps1_cdrom_read(ps1_cdrom *c, uint32_t reg)
{
    switch (reg & 3) {
    case 0:
        return (uint8_t)(c->index | (c->nparam == 0 ? 0x08 : 0) | (c->nparam < 16 ? 0x10 : 0) |
                         (c->rpos < c->rlen ? 0x20 : 0) | (c->busy ? 0x80 : 0));
    case 1:
        return c->rpos < c->rlen ? c->resp[c->rpos++] : 0;
    case 2:
        return 0;
    default:
        return (uint8_t)(((c->index & 1) ? c->iflag : c->ie) | 0xE0);
    }
}

void ps1_cdrom_write(ps1_cdrom *c, uint32_t reg, uint8_t v)
{
    reg &= 3;
    if (reg == 0) {
        c->index = v & 3;
        return;
    }
    switch (reg << 2 | c->index) {
    case 1 << 2 | 0:
        command(c, v);
        break;
    case 2 << 2 | 0:
        if (c->nparam < 16)
            c->param[c->nparam++] = v;
        break;
    case 2 << 2 | 1:
        c->ie = v & 0x1F;
        break;
    case 3 << 2 | 1:
        c->iflag &= (uint8_t)~(v & 0x1F);
        if (v & 0x40)
            c->nparam = 0;
        break;
    default:
        break;
    }
}

void ps1_cdrom_tick(ps1_cdrom *c, uint32_t cpu_cycles)
{
    if (c->busy) {
        c->busy -= (int)(cpu_cycles > (uint32_t)c->busy ? (uint32_t)c->busy : cpu_cycles);
        if (!c->busy && c->have_first) {
            c->busy = 1; /* wait for the previous interrupt to be acknowledged */
            if (!c->iflag) {
                c->busy = 0;
                c->have_first = 0;
                deliver(c, &c->first);
                c->second_wait = SECOND_DELAY;
            }
        }
    } else if (c->have_second) {
        if (c->second_wait > 0)
            c->second_wait -= (int)(cpu_cycles > (uint32_t)c->second_wait ? (uint32_t)c->second_wait : cpu_cycles);
        if (c->second_wait <= 0 && !c->iflag) {
            c->have_second = 0;
            deliver(c, &c->second);
        }
    }
}

size_t ps1_cdrom_dma_read(ps1_cdrom *c, uint32_t *words, size_t n)
{
    (void)c;
    memset(words, 0, n * 4);
    return n;
}

size_t ps1_cdrom_audio_pull(ps1_cdrom *c, int16_t *stereo, size_t frames)
{
    (void)c;
    (void)stereo;
    (void)frames;
    return 0;
}

/* Save states (XBA-182): a placeholder has no state worth keeping. */
size_t ps1_cdrom_state_size(ps1_cdrom *x)
{
    (void)x;
    return 0;
}

void ps1_cdrom_state_save(ps1_cdrom *x, uint8_t *out)
{
    (void)x;
    (void)out;
}

int ps1_cdrom_state_valid(const uint8_t *in, size_t n)
{
    (void)in;
    return n == 0 ? 0 : -1;
}

int ps1_cdrom_state_load(ps1_cdrom *x, const uint8_t *in, size_t n)
{
    (void)x;
    (void)in;
    return n == 0 ? 0 : -1;
}
