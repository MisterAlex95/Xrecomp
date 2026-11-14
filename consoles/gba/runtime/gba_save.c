/* gba_save.c -- save memory of the GBA core (G-7, XBA-156): SRAM, flash 64 / 128 KB, EEPROM 512 B / 8 KB.
 * GBATEK « GBA Cart Backup IDs », « GBA Cart Backup SRAM/FRAM », « GBA Cart Backup Flash ROM », « GBA Cart Backup
 * EEPROM ».
 *
 * Kind: the first save library ID string found in the ROM (word aligned, as Nintendo's libraries place them):
 * EEPROM_V, SRAM_V / SRAM_F_V, FLASH_V / FLASH512_V (64 KB), FLASH1M_V (128 KB). Without one the kind is NONE:
 * the 0x0E000000 area reads 0xFF, and the first write there turns it into SRAM (homebrew often uses SRAM without an
 * ID string; an erased SRAM reads 0xFF too, so nothing a game could see changes before that write).
 *
 * SRAM: 32 KB on an 8-bit bus, mirrored over 0x0E000000-0x0FFFFFFF. 16/32-bit reads return the byte repeated,
 * 16/32-bit writes store the byte of the value the address selects (the bus calls us with the unaligned address).
 * Flash: the AA/55 command protocol with chip ID mode (Panasonic MN63F805MNP 32h/1Bh for 64 KB, Sanyo LE26FV10N1TS
 * 62h/13h for 128 KB: the IDs games check for those sizes), chip erase, 4 KB sector erase, byte program, bank switch
 * (128 KB). Erase and program complete at once (a game polling for 0xFF sees it on the first read).
 * EEPROM: the serial protocol, one bit per 16-bit access (normally through DMA3): 11 + address + 0 reads (4 dummy
 * bits, then 64 data bits), 10 + address + 64 bits + 0 writes; the ready bit reads 1 at once. The address width
 * (6 bits for 512 B, 14 for 8 KB) comes from the DMA3 length (gba_eeprom_dma_hint), or from the size of the image
 * gba_save_load puts back. Storage order: bit i of block b is bit 7 - (i & 7) of byte b * 8 + i / 8 (the layout
 * other emulators' .sav files use, so they load as is). */
#include "gba_internal.h"

#include <string.h>

static const struct {
    const char *id;
    gba_save_kind kind;
} save_ids[] = {
    {"EEPROM_V", GBA_SAVE_EEPROM512}, {"SRAM_V", GBA_SAVE_SRAM},        {"SRAM_F_V", GBA_SAVE_SRAM},
    {"FLASH_V", GBA_SAVE_FLASH64},    {"FLASH512_V", GBA_SAVE_FLASH64}, {"FLASH1M_V", GBA_SAVE_FLASH128},
};

static gba_save_kind detect(const uint8_t *rom, uint32_t size)
{
    for (uint32_t i = 0; i + 12 <= size; i += 4) {
        uint8_t c = rom[i];
        if (c != 'E' && c != 'S' && c != 'F')
            continue;
        for (size_t k = 0; k < sizeof save_ids / sizeof save_ids[0]; k++) {
            size_t n = strlen(save_ids[k].id);
            if (i + n <= size && memcmp(rom + i, save_ids[k].id, n) == 0)
                return save_ids[k].kind;
        }
    }
    return GBA_SAVE_NONE;
}

static size_t kind_size(gba_save_kind k)
{
    switch (k) {
    case GBA_SAVE_SRAM: return 0x8000;
    case GBA_SAVE_FLASH64: return 0x10000;
    case GBA_SAVE_FLASH128: return 0x20000;
    case GBA_SAVE_EEPROM512: return 512;
    case GBA_SAVE_EEPROM8K: return 8192;
    default: return 0;
    }
}

static void set_kind(gba_save *s, gba_save_kind k)
{
    s->kind = k;
    s->eeprom_abits = k == GBA_SAVE_EEPROM8K ? 14 : 6;
}

void gba_save_init(gba_machine *m)
{
    gba_save *s = &m->save;
    memset(s, 0, sizeof *s);
    memset(s->data, 0xFF, sizeof s->data);
    set_kind(s, detect(m->rom, m->rom_size));
}

/* ---- 0x0E000000: SRAM / flash ------------------------------------------------------------------------------ */

uint8_t gba_save_read8(gba_machine *m, uint32_t addr)
{
    gba_save *s = &m->save;
    uint32_t off = addr & 0xFFFF;
    switch (s->kind) {
    case GBA_SAVE_SRAM:
        return s->data[off & 0x7FFF];
    case GBA_SAVE_FLASH64:
    case GBA_SAVE_FLASH128:
        if (s->flash_id && off < 2) {
            static const uint8_t id64[2] = {0x32, 0x1B}, id128[2] = {0x62, 0x13};
            return (s->kind == GBA_SAVE_FLASH64 ? id64 : id128)[off];
        }
        return s->data[s->flash_bank + off];
    default:
        return 0xFF; /* no chip (EEPROM carts too): the bus floats high */
    }
}

static void flash_write(gba_save *s, uint32_t off, uint8_t v)
{
    if (s->flash_program) {
        s->flash_program = false;
        s->data[s->flash_bank + off] = v;
        s->dirty = true;
        return;
    }
    if (s->flash_bank_sel) {
        s->flash_bank_sel = false;
        if (off == 0 && s->kind == GBA_SAVE_FLASH128) {
            s->flash_bank = (v & 1) ? 0x10000 : 0;
            return;
        }
    }
    switch (s->flash_seq) {
    case 0:
        if (off == 0x5555 && v == 0xAA)
            s->flash_seq = 1;
        else if (v == 0xF0) /* reset / leave ID mode without the unlock sequence (Atmel, Macronix) */
            s->flash_id = false;
        return;
    case 1:
        s->flash_seq = (off == 0x2AAA && v == 0x55) ? 2 : 0;
        return;
    default: {
        bool erase = s->flash_erase; /* the previous command was 80h: this one may be an erase */
        s->flash_seq = 0;
        s->flash_erase = false;
        if (erase && v == 0x30) { /* sector erase: the 4 KB sector holding the address */
            memset(&s->data[s->flash_bank + (off & 0xF000)], 0xFF, 0x1000);
            s->dirty = true;
            return;
        }
        if (off != 0x5555)
            return;
        switch (v) {
        case 0x90: s->flash_id = true; break;
        case 0xF0: s->flash_id = false; break;
        case 0x80: s->flash_erase = true; break;
        case 0x10:
            if (erase) {
                memset(s->data, 0xFF, kind_size(s->kind));
                s->dirty = true;
            }
            break;
        case 0xA0: s->flash_program = true; break;
        case 0xB0: s->flash_bank_sel = s->kind == GBA_SAVE_FLASH128; break;
        default: break;
        }
        return;
    }
    }
}

void gba_save_write8(gba_machine *m, uint32_t addr, uint8_t v)
{
    gba_save *s = &m->save;
    switch (s->kind) {
    case GBA_SAVE_NONE:
        set_kind(s, GBA_SAVE_SRAM); /* no ID string: a game writing here has SRAM */
        /* fall through */
    case GBA_SAVE_SRAM:
        s->data[addr & 0x7FFF] = v;
        s->dirty = true;
        return;
    case GBA_SAVE_FLASH64:
    case GBA_SAVE_FLASH128:
        flash_write(s, addr & 0xFFFF, v);
        return;
    default:
        return; /* EEPROM carts have nothing at 0x0E000000 */
    }
}

/* ---- EEPROM -------------------------------------------------------------------------------------------- */

void gba_eeprom_dma_hint(gba_machine *m, uint32_t count)
{
    gba_save *s = &m->save;
    if (count == 9 || count == 73) {
        if (s->kind == GBA_SAVE_EEPROM512)
            s->eeprom_abits = 6; /* once 8 KB (a 14-bit request or an 8 KB image put back), it stays so */
    } else if (count == 17 || count == 81) {
        s->eeprom_abits = 14;
        s->kind = GBA_SAVE_EEPROM8K;
    }
}

uint16_t gba_eeprom_read(gba_machine *m)
{
    gba_save *s = &m->save;
    if (s->eeprom_read_left <= 0)
        return 1; /* ready */
    int left = s->eeprom_read_left--;
    if (left > 64)
        return 0; /* the 4 dummy bits */
    uint32_t bit = (uint32_t)(64 - left); /* 0 = first (most significant) data bit */
    uint32_t pos = s->eeprom_addr * 64 + bit;
    return (s->data[pos >> 3] >> (7 - (pos & 7))) & 1;
}

void gba_eeprom_write(gba_machine *m, uint16_t v)
{
    gba_save *s = &m->save;
    int bit = v & 1;
    int n = ++s->eeprom_nbits;
    int ab = s->eeprom_abits;
    if (n == 1) {
        s->eeprom_cmd = bit;
        s->eeprom_addr = 0;
        s->eeprom_data = 0;
        return;
    }
    if (n == 2) {
        s->eeprom_cmd = s->eeprom_cmd << 1 | bit;
        if (s->eeprom_cmd < 2) /* 00 / 01: not a request */
            s->eeprom_nbits = 0;
        s->eeprom_read_left = 0;
        return;
    }
    if (n <= 2 + ab) {
        s->eeprom_addr = s->eeprom_addr << 1 | (uint32_t)bit;
        return;
    }
    uint32_t blocks = ab == 6 ? 64 : 1024; /* 8 KB parts decode 10 of their 14 address bits */
    if (s->eeprom_cmd == 3) { /* read: the stop bit, then the reply */
        s->eeprom_addr &= blocks - 1;
        s->eeprom_read_left = 68;
        s->eeprom_nbits = 0;
        return;
    }
    if (n <= 2 + ab + 64) {
        s->eeprom_data = s->eeprom_data << 1 | (uint64_t)bit;
        return;
    }
    /* write: stop bit received, program the block */
    if (ab == 14)
        s->kind = GBA_SAVE_EEPROM8K;
    uint32_t a = (s->eeprom_addr & (blocks - 1)) * 8;
    for (int i = 0; i < 8; i++)
        s->data[a + (uint32_t)i] = (uint8_t)(s->eeprom_data >> (56 - 8 * i));
    s->dirty = true;
    s->eeprom_nbits = 0;
}

/* ---- public API (gba_core.h) ------------------------------------------------------------------------------- */

gba_save_kind gba_save_type(gba_machine *m)
{
    return m->save.kind;
}

const uint8_t *gba_save_data(gba_machine *m, size_t *size)
{
    size_t n = kind_size(m->save.kind);
    if (size)
        *size = n;
    return n ? m->save.data : NULL;
}

uint8_t *gba_sram(gba_machine *m, size_t *size)
{
    size_t n = kind_size(m->save.kind);
    if (size)
        *size = n ? n : 0x8000;
    return m->save.data;
}

int gba_save_take_dirty(gba_machine *m)
{
    int d = m->save.dirty;
    m->save.dirty = false;
    return d;
}

int gba_save_load(gba_machine *m, const uint8_t *data, size_t size)
{
    gba_save *s = &m->save;
    if (!data || size == 0)
        return -1;
    switch (s->kind) {
    case GBA_SAVE_NONE: /* a game that fell back to SRAM last time */
        if (size != 0x8000)
            return -1;
        set_kind(s, GBA_SAVE_SRAM);
        break;
    case GBA_SAVE_EEPROM512:
    case GBA_SAVE_EEPROM8K:
        if (size != 512 && size != 8192)
            return -1;
        set_kind(s, size == 512 ? GBA_SAVE_EEPROM512 : GBA_SAVE_EEPROM8K);
        break;
    default:
        if (size > kind_size(s->kind))
            return -1;
        break;
    }
    memset(s->data, 0xFF, sizeof s->data);
    memcpy(s->data, data, size);
    return 0;
}
