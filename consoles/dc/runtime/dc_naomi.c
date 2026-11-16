/* dc_naomi.c -- the Naomi spike (XBA-235, epic XBA-221): the cartridge board, G1 DMA from it, the Naomi BIOS in C and
 * a JVS I/O board stub. Off unless the machine was created as DC_MODEL_NAOMI. See docs/technical/naomi.md.
 *
 * The cartridge is a ROM image the user supplies from a board he owns (XRecomp ships none), plain or already
 * decrypted: the M1 / M2 / M4 boards' decryption is not modelled. The SH-4 reaches it through 16-bit registers, one
 * per 32-bit word, in the G1 window (public register map: MAME src/mame/sega/naomibd.cpp):
 *
 *   0x005F7000 ROM_OFFSETH   bits 31-16 of the PIO offset (bit 31: advance by 2 after each ROM_DATA access;
 *                            bits 28-30: the board's mode bits, ignored for a plain image)
 *   0x005F7004 ROM_OFFSETL   bits 15-0
 *   0x005F7008 ROM_DATA      the 16-bit word at the offset (bytes)
 *   0x005F700C DMA_OFFSETH   the DMA source offset, same layout
 *   0x005F7010 DMA_OFFSETL
 *   0x005F7014 DMA_COUNT     stored (the transfer length comes from G1's GDLEN)
 *   0x005F7078 BOARDID write the board's serial EEPROM lines (stored), 0x005F707C BOARDID read (bit 15: SDA, 0 here)
 *
 * DMA is the G1 bus DMA the Dreamcast uses for the GD-ROM: GDSTAR (RAM destination) 0x005F7404, GDLEN 0x005F7408,
 * GDDIR 0x005F740C, GDEN 0x005F7414, GDST 0x005F7418 (write 1: start). The copy is done at once; GDST reads 0 and
 * Holly's normal interrupt bit 14 (G1 DMA end) is raised.
 *
 * The cartridge header (Flycast core/hw/naomi/naomi_cart.h, RomBootID): 0x000 board name ("NAOMI"), 0x010 maker,
 * 0x030 eight region titles, 0x130 date, 0x134 game ID, 0x138 ROM mode, 0x360 the game load list (eight entries of
 * ROM offset, RAM address, length; offset 0xFFFFFFFF ends it), 0x3C0 the test mode one, 0x420 the game entry point,
 * 0x424 the test mode entry point. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dc_internal.h"

#define G1_GDSTAR 0x005F7404u
#define G1_GDLEN 0x005F7408u
#define G1_GDDIR 0x005F740Cu
#define G1_GDEN 0x005F7414u
#define G1_GDST 0x005F7418u
#define G1REG(p) m->cart.g1[((p) - 0x005F7400u) >> 2]

#define NAOMI_HDR_LOAD 0x360u
#define NAOMI_HDR_ENTRY 0x420u
#define NAOMI_HDR_SIZE 0x500u
#define NAOMI_CART_MAX (1024u << 20)

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void dc_naomi_free(dc_machine *m)
{
    free(m->cart.rom);
    m->cart.rom = NULL;
    m->cart.size = 0;
}

static uint16_t cart_word(const dc_machine *m, uint32_t off)
{
    off &= 0x0FFFFFFEu; /* bits 28-31 are the mode / advance flags */
    if (!m->cart.rom || (uint64_t)off + 2 > m->cart.size)
        return 0xFFFF; /* no ROM there: open bus */
    return (uint16_t)(m->cart.rom[off] | (m->cart.rom[off + 1] << 8));
}

static void g1_dma(dc_machine *m)
{
    uint32_t dst = G1REG(G1_GDSTAR) & 0x1FFFFFE0u, len = G1REG(G1_GDLEN) & 0x01FFFFFEu;
    if (!(G1REG(G1_GDEN) & 1) || !(G1REG(G1_GDDIR) & 1)) {
        dc_hw_write_log(m, "naomi: G1 DMA start without enable / read direction", G1_GDST, G1REG(G1_GDDIR));
        return;
    }
    uint32_t src = m->cart.dma_offset;
    for (uint32_t i = 0; i < len; i += 2) {
        uint16_t w = cart_word(m, src + i);
        sh4_w16(&m->cpu, 0xA0000000u | (dst + i), w); /* P2: the bus, never the cache */
    }
    if (m->cart.dma_offset & 0x80000000u)
        m->cart.dma_offset = (m->cart.dma_offset & 0xF0000000u) | ((src + len) & 0x0FFFFFFFu);
    m->cart.dma_bytes += len;
    G1REG(G1_GDSTAR) = dst + len;
    G1REG(G1_GDLEN) = 0;
    G1REG(G1_GDST) = 0;
    m->istnrm |= 1u << 14; /* G1 (GD-ROM) DMA end */
    dc_hw_kick(m);
}

uint32_t dc_naomi_g1_read(dc_machine *m, uint32_t p)
{
    dc_naomi_cart *c = &m->cart;
    switch (p) {
    case 0x005F7000u: return c->rom_offset >> 16;
    case 0x005F7004u: return c->rom_offset & 0xFFFF;
    case 0x005F7008u: {
        uint16_t w = cart_word(m, c->rom_offset);
        c->pio_reads++;
        if (c->rom_offset & 0x80000000u)
            c->rom_offset = (c->rom_offset & 0xF0000000u) | ((c->rom_offset + 2) & 0x0FFFFFFFu);
        return w;
    }
    case 0x005F700Cu: return c->dma_offset >> 16;
    case 0x005F7010u: return c->dma_offset & 0xFFFF;
    case 0x005F7014u: return c->dma_count;
    case 0x005F707Cu: return 0; /* EEPROM SDA low: no board serial EEPROM modelled */
    default:
        break;
    }
    if (p >= 0x005F7400u && p < 0x005F7500u)
        return G1REG(p);
    return 0;
}

void dc_naomi_g1_write(dc_machine *m, uint32_t p, uint32_t v)
{
    dc_naomi_cart *c = &m->cart;
    switch (p) {
    case 0x005F7000u: c->rom_offset = (c->rom_offset & 0x0000FFFFu) | ((v & 0xFFFF) << 16); return;
    case 0x005F7004u: c->rom_offset = (c->rom_offset & 0xFFFF0000u) | (v & 0xFFFF); return;
    case 0x005F7008u: /* writes to ROM: dropped, the offset still advances */
        if (c->rom_offset & 0x80000000u)
            c->rom_offset = (c->rom_offset & 0xF0000000u) | ((c->rom_offset + 2) & 0x0FFFFFFFu);
        return;
    case 0x005F700Cu: c->dma_offset = (c->dma_offset & 0x0000FFFFu) | ((v & 0xFFFF) << 16); return;
    case 0x005F7010u: c->dma_offset = (c->dma_offset & 0xFFFF0000u) | (v & 0xFFFF); return;
    case 0x005F7014u: c->dma_count = v & 0xFFFF; return;
    case 0x005F7078u: c->boardid = (uint16_t)v; return;
    default:
        break;
    }
    if (p >= 0x005F7400u && p < 0x005F7500u) {
        G1REG(p) = v;
        if (p == G1_GDST && (v & 1))
            g1_dma(m);
        return;
    }
    dc_hw_write_log(m, "naomi: write unhandled board register", p, v);
}

/* ---- the cartridge and the BIOS in C ---- */

int dc_naomi_insert_cart_mem(dc_machine *m, const uint8_t *data, size_t size, char *err, size_t errlen)
{
    if (m->model != DC_MODEL_NAOMI) {
        snprintf(err, errlen, "not a Naomi machine");
        return -1;
    }
    if (size < NAOMI_HDR_SIZE || size > NAOMI_CART_MAX) {
        snprintf(err, errlen, "cartridge image of %zu bytes: want a header (0x500 bytes) and at most 1 GB", size);
        return -1;
    }
    uint8_t *rom = malloc(size);
    if (!rom) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    memcpy(rom, data, size);
    dc_naomi_free(m);
    m->cart.rom = rom;
    m->cart.size = size;
    return 0;
}

int dc_naomi_insert_cart(dc_machine *m, const char *path, char *err, size_t errlen)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "cannot open %s", path);
        return -1;
    }
    uint8_t *buf = NULL;
    size_t size = 0, cap = 0;
    for (;;) {
        if (size == cap) {
            size_t ncap = cap ? cap * 2 : (1u << 20);
            if (ncap > (size_t)NAOMI_CART_MAX + 1)
                ncap = (size_t)NAOMI_CART_MAX + 1;
            if (ncap == cap)
                break; /* too large: the size check below refuses it */
            uint8_t *p = realloc(buf, ncap);
            if (!p) {
                free(buf);
                fclose(f);
                snprintf(err, errlen, "out of memory");
                return -1;
            }
            buf = p;
            cap = ncap;
        }
        size_t got = fread(buf + size, 1, cap - size, f);
        size += got;
        if (got == 0)
            break;
    }
    fclose(f);
    int r = dc_naomi_insert_cart_mem(m, buf, size, err, errlen);
    free(buf);
    return r;
}

int dc_naomi_boot(dc_machine *m, char *err, size_t errlen)
{
    const uint8_t *rom = m->cart.rom;
    if (!rom) {
        snprintf(err, errlen, "no cartridge");
        return -1;
    }
    if (memcmp(rom, "NAOMI", 5) != 0) {
        snprintf(err, errlen, "not a Naomi cartridge header (board name '%.16s')", (const char *)rom);
        return -1;
    }
    m->img_lo = UINT32_MAX;
    m->img_hi = 0;
    for (int i = 0; i < 8; i++) {
        const uint8_t *e = rom + NAOMI_HDR_LOAD + 12u * (uint32_t)i;
        uint32_t off = rd32(e), addr = rd32(e + 4), len = rd32(e + 8);
        if (off == 0xFFFFFFFFu)
            break;
        off &= 0x0FFFFFFFu; /* ROM mode: the BIOS sets 0x20000000 for encrypted boards */
        if ((uint64_t)off + len > m->cart.size || !sh4_is_ram(addr) ||
            (addr & (m->ram_size - 1)) + (uint64_t)len > m->ram_size) {
            snprintf(err, errlen, "load entry %d (ROM %08x -> %08x, %u bytes) out of range", i, off, addr, len);
            return -1;
        }
        memcpy(m->ram + (addr & (m->ram_size - 1)), rom + off, len);
        uint32_t lo = (addr & 0x1FFFFFFFu) | 0x80000000u;
        if (lo < m->img_lo)
            m->img_lo = lo;
        if (lo + len > m->img_hi)
            m->img_hi = lo + len;
    }
    uint32_t entry = rd32(rom + NAOMI_HDR_ENTRY);
    if (!sh4_is_ram(entry)) {
        snprintf(err, errlen, "game entry %08x is not in main RAM", entry);
        return -1;
    }
    dc_bios_setup(m, entry);
    dc_hw_events(m);
    return 0;
}

/* ---- the JVS I/O board (stub) ---- */

dc_jvs_state *dc_jvs(dc_machine *m)
{
    return &m->jvs;
}

/* Unescapes a packet (D0 xx = xx + 1) after the E0 sync. */
static size_t jvs_unescape(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
    size_t k = 0;
    for (size_t i = 1; i < n && k < cap; i++) {
        uint8_t b = in[i];
        if (b == 0xD0 && i + 1 < n)
            b = (uint8_t)(in[++i] + 1);
        out[k++] = b;
    }
    return k;
}

size_t dc_jvs_transfer(dc_machine *m, const uint8_t *req, size_t n, uint8_t *out, size_t cap)
{
    uint8_t pk[256], rep[256];
    if (n < 4 || req[0] != 0xE0)
        return 0;
    size_t k = jvs_unescape(req, n, pk, sizeof pk);
    if (k < 3 || (size_t)pk[1] + 2 != k)
        return 0;
    uint8_t sum = 0;
    for (size_t i = 0; i + 1 < k; i++)
        sum = (uint8_t)(sum + pk[i]);
    if (sum != pk[k - 1])
        return 0;
    uint8_t node = pk[0];
    const uint8_t *d = pk + 2;
    size_t dn = k - 3, r = 0;
    if (node != 0xFF && node != 1)
        return 0; /* our board is node 1 */
    rep[r++] = 1; /* status: normal */
    const dc_jvs_state *s = &m->jvs;
    for (size_t i = 0; i < dn && r < sizeof rep - 64;) {
        uint8_t cmd = d[i++];
        switch (cmd) {
        case 0xF0: /* reset (F0 D9): no reply */
            return 0;
        case 0xF1: /* set address */
            i++;
            rep[r++] = 1;
            break;
        case 0x10: { /* ID */
            static const char id[] = "XRECOMP;JVS I/O STUB;Ver0.1";
            rep[r++] = 1;
            memcpy(rep + r, id, sizeof id);
            r += sizeof id;
            break;
        }
        case 0x11: rep[r++] = 1; rep[r++] = 0x13; break; /* command format revision 1.3 */
        case 0x12: rep[r++] = 1; rep[r++] = 0x30; break; /* JVS revision 3.0 */
        case 0x13: rep[r++] = 1; rep[r++] = 0x10; break; /* communication version 1.0 */
        case 0x14: { /* features: 2 players x 13 switches, 2 coin slots */
            static const uint8_t f[] = {1, 0x01, 2, 13, 0, 0x02, 2, 0, 0, 0x00};
            memcpy(rep + r, f, sizeof f);
            r += sizeof f;
            break;
        }
        case 0x20: { /* switch inputs: players, bytes per player */
            uint8_t np = i < dn ? d[i++] : 0, nb = i < dn ? d[i++] : 0;
            rep[r++] = 1;
            rep[r++] = s->test ? 0x80 : 0;
            for (uint8_t p = 0; p < np && r < sizeof rep - 8; p++)
                for (uint8_t b = 0; b < nb; b++)
                    rep[r++] = p < 2 && b < 2 ? (uint8_t)(s->players[p] >> (8 * (1 - b))) : 0;
            break;
        }
        case 0x21: { /* coins: slots */
            uint8_t ns = i < dn ? d[i++] : 0;
            rep[r++] = 1;
            for (uint8_t c = 0; c < ns && r < sizeof rep - 8; c++) {
                uint16_t v = c < 2 ? (uint16_t)(s->coins[c] & 0x3FFF) : 0;
                rep[r++] = (uint8_t)(v >> 8);
                rep[r++] = (uint8_t)v;
            }
            break;
        }
        default:
            rep[r++] = 2; /* report: unknown command; the rest is not parsed */
            i = dn;
            break;
        }
    }
    /* E0 00 len rep.. sum, escaped */
    uint8_t raw[260];
    size_t rn = 0;
    raw[rn++] = 0x00;
    raw[rn++] = (uint8_t)(r + 1);
    memcpy(raw + rn, rep, r);
    rn += r;
    sum = 0;
    for (size_t i = 0; i < rn; i++)
        sum = (uint8_t)(sum + raw[i]);
    raw[rn++] = sum;
    size_t o = 0;
    if (cap < 1)
        return 0;
    out[o++] = 0xE0;
    for (size_t i = 0; i < rn; i++) {
        if (raw[i] == 0xE0 || raw[i] == 0xD0) {
            if (o + 2 > cap)
                return 0;
            out[o++] = 0xD0;
            out[o++] = (uint8_t)(raw[i] - 1);
        } else {
            if (o + 1 > cap)
                return 0;
            out[o++] = raw[i];
        }
    }
    return o;
}
