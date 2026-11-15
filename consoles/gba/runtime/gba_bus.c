/* GBA memory map (XBA-153): BIOS, EWRAM, IWRAM, IO, palette, VRAM, OAM, cartridge ROM and SRAM, with their
 * mirrors, bus widths and wait states (GBATEK « GBA Memory Map », « GBA System Control »).
 *
 * Accesses may get an unaligned address: the bus aligns it, except for the save chip (8-bit bus, gba_save.c); the
 * CPU rotates misaligned loads. The EEPROM answers in the 0x0D000000 area (gba_save.c). Each access adds its cycles to m->now from the
 * wait-state tables, which follow WAITCNT. Not modelled: the PPU's VRAM / palette / OAM contention, the exact
 * cartridge prefetch buffer (approximated: with prefetch on, sequential ROM opcode fetches cost 1 cycle per
 * halfword). */
#include "gba_internal.h"

#include <string.h>

static inline uint32_t vram_offset(uint32_t addr)
{
    uint32_t a = addr & 0x1FFFF;
    return a >= 0x18000 ? a - 0x8000 : a;
}

static inline bool bios_readable(const gba_machine *m)
{
    return m->cpu.r[15] < GBA_BIOS_SIZE + 8;
}

void gba_update_waitstates(gba_machine *m)
{
    static const uint8_t nwait[4] = {4, 3, 2, 8};
    uint16_t w = rd16(&m->io[0x204]);
    int sram = nwait[w & 3];
    int ws_n[3] = {nwait[(w >> 2) & 3], nwait[(w >> 5) & 3], nwait[(w >> 8) & 3]};
    int ws_s[3] = {(w & 0x10) ? 1 : 2, (w & 0x80) ? 1 : 4, (w & 0x400) ? 1 : 8};

    for (int r = 0; r < 16; r++) {
        int n16 = 1, s16 = 1, n32 = 1, s32 = 1;
        switch (r) {
        case 0x2: /* EWRAM: 16-bit bus, 2 wait states */
            n16 = s16 = 3;
            n32 = s32 = 6;
            break;
        case 0x5:
        case 0x6: /* palette, VRAM: 16-bit bus */
            n32 = s32 = 2;
            break;
        case 0x8: case 0x9: case 0xA: case 0xB: case 0xC: case 0xD: {
            int ws = (r - 8) >> 1;
            n16 = 1 + ws_n[ws];
            s16 = 1 + ws_s[ws];
            n32 = n16 + s16;
            s32 = 2 * s16;
            break;
        }
        case 0xE:
        case 0xF: /* SRAM: 8-bit bus */
            n16 = s16 = n32 = s32 = 1 + sram;
            break;
        default:
            break;
        }
        m->wait16[r][0] = (uint8_t)n16;
        m->wait16[r][1] = (uint8_t)s16;
        m->wait32[r][0] = (uint8_t)n32;
        m->wait32[r][1] = (uint8_t)s32;
    }
    m->prefetch = (w & 0x4000) != 0;
    /* Sequential opcode fetches as gba_fetch16 / gba_fetch32 charge them (lifted code adds these). */
    for (int r = 0; r < 16; r++) {
        if (r == 3) {
            m->fetch16[r] = m->fetch32[r] = 1;
        } else if (r >= 8 && r <= 0xD) {
            m->fetch16[r] = m->prefetch ? 1 : m->wait16[r][1];
            m->fetch32[r] = m->prefetch ? 2 : m->wait32[r][1];
        } else {
            m->fetch16[r] = m->wait16[r][1];
            m->fetch32[r] = m->wait32[r][1];
        }
    }
}

/* ---- mGBA's debug-print port (gba_set_debug_log, XBA-201) ----------------------------------------------- */

static inline bool dbg_port(const gba_machine *m, uint32_t addr)
{
    uint32_t a = addr & 0xFFFFFF;
    return m->dbg_cb && (addr >> 24) == 0x4 && a >= 0xFFF600 && a < 0xFFF782;
}

static uint16_t dbg_read16(const gba_machine *m, uint32_t addr)
{
    return ((addr & 0xFFFFFE) == 0xFFF780 && m->dbg_open) ? 0x1DEA : 0;
}

static void dbg_write8(gba_machine *m, uint32_t addr, uint8_t v)
{
    uint32_t a = addr & 0xFFFFFF;
    if (a < 0xFFF700) {
        if (m->dbg_open)
            m->dbg_str[a - 0xFFF600] = (char)v;
    }
}

static void dbg_write16(gba_machine *m, uint32_t addr, uint16_t v)
{
    uint32_t a = addr & 0xFFFFFE;
    if (a < 0xFFF700) {
        dbg_write8(m, a, (uint8_t)v);
        dbg_write8(m, a + 1, (uint8_t)(v >> 8));
    } else if (a == 0xFFF780) {
        m->dbg_open = v == 0xC0DE;
    } else if (a == 0xFFF700 && m->dbg_open && (v & 0x100)) {
        m->dbg_str[256] = 0;
        m->dbg_cb(m->dbg_ctx, v & 7, m->dbg_str);
        memset(m->dbg_str, 0, sizeof m->dbg_str);
    }
}

/* ---- reads ---------------------------------------------------------------------------------------------- */

static uint32_t rom_read32(const gba_machine *m, uint32_t addr)
{
    uint32_t a = addr & 0x1FFFFFC;
    if (a < m->rom_size)
        return rd32(&m->rom[a]);
    /* Past the end: the cartridge bus returns the address halfwords it latched. */
    return ((a >> 1) & 0xFFFF) | (((a >> 1) + 1) & 0xFFFF) << 16;
}

static uint16_t rom_read16(const gba_machine *m, uint32_t addr)
{
    uint32_t a = addr & 0x1FFFFFE;
    if (a < m->rom_size)
        return rd16(&m->rom[a]);
    return (uint16_t)(a >> 1);
}

static uint16_t io_half(gba_machine *m, uint32_t addr)
{
    if (gba_io_open_bus(addr))
        return (uint16_t)(m->open_bus >> ((addr & 2) * 8));
    return gba_io_read16(m, addr);
}

static uint32_t read32_raw(gba_machine *m, uint32_t addr)
{
    switch (addr >> 24) {
    case 0x0:
        if (addr < GBA_BIOS_SIZE)
            return bios_readable(m) ? rd32(&m->bios[addr]) : m->bios_latch;
        return m->open_bus;
    case 0x2:
        return rd32(&m->ewram[addr & (GBA_EWRAM_SIZE - 1)]);
    case 0x3:
        return rd32(&m->iwram[addr & (GBA_IWRAM_SIZE - 1)]);
    case 0x4:
        if ((addr & 0xFFFFFF) < GBA_IO_SIZE)
            return io_half(m, addr) | (uint32_t)io_half(m, addr + 2) << 16;
        if ((addr & 0xFFFC) == 0x800)
            return 0x0D000020;
        if (dbg_port(m, addr))
            return dbg_read16(m, addr) | (uint32_t)dbg_read16(m, addr + 2) << 16;
        return m->open_bus;
    case 0x5:
        return rd32(&m->pal[addr & (GBA_PAL_SIZE - 1)]);
    case 0x6:
        return rd32(&m->vram[vram_offset(addr)]);
    case 0x7:
        return rd32(&m->oam[addr & (GBA_OAM_SIZE - 1)]);
    case 0x8: case 0x9: case 0xA: case 0xB: case 0xC: case 0xD:
        return rom_read32(m, addr);
    case 0xE:
    case 0xF: /* save chip, 8-bit bus: the addressed byte, repeated */
        return gba_save_read8(m, addr) * 0x01010101u;
    default:
        return m->open_bus;
    }
}

static uint16_t read16_raw(gba_machine *m, uint32_t addr)
{
    switch (addr >> 24) {
    case 0x0:
        if (addr < GBA_BIOS_SIZE)
            return bios_readable(m) ? rd16(&m->bios[addr]) : (uint16_t)(m->bios_latch >> ((addr & 2) * 8));
        return (uint16_t)(m->open_bus >> ((addr & 2) * 8));
    case 0x2:
        return rd16(&m->ewram[addr & (GBA_EWRAM_SIZE - 1)]);
    case 0x3:
        return rd16(&m->iwram[addr & (GBA_IWRAM_SIZE - 1)]);
    case 0x4:
        if ((addr & 0xFFFFFF) < GBA_IO_SIZE)
            return io_half(m, addr);
        if ((addr & 0xFFFC) == 0x800)
            return (addr & 2) ? 0x0D00 : 0x0020;
        if (dbg_port(m, addr))
            return dbg_read16(m, addr);
        return (uint16_t)(m->open_bus >> ((addr & 2) * 8));
    case 0x5:
        return rd16(&m->pal[addr & (GBA_PAL_SIZE - 1)]);
    case 0x6:
        return rd16(&m->vram[vram_offset(addr)]);
    case 0x7:
        return rd16(&m->oam[addr & (GBA_OAM_SIZE - 1)]);
    case 0xD:
        if (gba_eeprom_area(m, addr))
            return gba_eeprom_read(m);
        return rom_read16(m, addr);
    case 0x8: case 0x9: case 0xA: case 0xB: case 0xC:
        return rom_read16(m, addr);
    case 0xE:
    case 0xF:
        return (uint16_t)(gba_save_read8(m, addr) * 0x0101u);
    default:
        return (uint16_t)(m->open_bus >> ((addr & 2) * 8));
    }
}

static uint8_t read8_raw(gba_machine *m, uint32_t addr)
{
    switch (addr >> 24) {
    case 0x2:
        return m->ewram[addr & (GBA_EWRAM_SIZE - 1)];
    case 0x3:
        return m->iwram[addr & (GBA_IWRAM_SIZE - 1)];
    case 0xE:
    case 0xF:
        return gba_save_read8(m, addr);
    default:
        return (uint8_t)(read16_raw(m, addr & ~1u) >> ((addr & 1) * 8));
    }
}

/* The CPU passes single-transfer addresses unaligned: the save chip's 8-bit bus sees every address line, the
 * other regions ignore the low bits. */
static inline bool save_region(uint32_t addr)
{
    return (addr >> 25) == 7; /* 0x0E000000-0x0FFFFFFF */
}

uint32_t gba_read32(gba_machine *m, uint32_t addr, bool seq)
{
    m->now += m->wait32[(addr >> 24) & 15][seq];
    if (!save_region(addr))
        addr &= ~3u;
    return read32_raw(m, addr);
}

uint16_t gba_read16(gba_machine *m, uint32_t addr, bool seq)
{
    m->now += m->wait16[(addr >> 24) & 15][seq];
    if (!save_region(addr))
        addr &= ~1u;
    return read16_raw(m, addr);
}

uint8_t gba_read8(gba_machine *m, uint32_t addr, bool seq)
{
    m->now += m->wait16[(addr >> 24) & 15][seq];
    return read8_raw(m, addr);
}

/* A halfword of code memory, without side effects (the regions code can run from). */
static uint16_t code_half(gba_machine *m, uint32_t addr)
{
    switch (addr >> 24) {
    case 0x0:
        return addr < GBA_BIOS_SIZE ? rd16(&m->bios[addr & ~1u]) : 0;
    case 0x2:
        return rd16(&m->ewram[addr & (GBA_EWRAM_SIZE - 2)]);
    case 0x3:
        return rd16(&m->iwram[addr & (GBA_IWRAM_SIZE - 2)]);
    case 0x5:
        return rd16(&m->pal[addr & (GBA_PAL_SIZE - 2)]);
    case 0x6:
        return rd16(&m->vram[vram_offset(addr) & ~1u]);
    case 0x7:
        return rd16(&m->oam[addr & (GBA_OAM_SIZE - 2)]);
    case 0x8: case 0x9: case 0xA: case 0xB: case 0xC: case 0xD:
        return gba_rom_half(m, addr);
    default:
        return 0;
    }
}

uint32_t gba_prefetch_latch(gba_machine *m, uint32_t pc, bool thumb)
{
    if (!thumb)
        return code_half(m, pc + 8) | (uint32_t)code_half(m, pc + 10) << 16;
    uint32_t r = pc >> 24;
    bool aligned = !(pc & 2);
    if (r == 0x0 || r == 0x7) /* BIOS, OAM: the word holding [pc+4] when aligned, [pc+2] and [pc+4] when not */
        return aligned ? code_half(m, pc + 4) | (uint32_t)code_half(m, pc + 6) << 16
                       : code_half(m, pc + 2) | (uint32_t)code_half(m, pc + 4) << 16;
    if (r == 0x3) /* IWRAM: [pc+4] in the half it lands in, [pc+2] left in the other */
        return aligned ? code_half(m, pc + 4) | (uint32_t)code_half(m, pc + 2) << 16
                       : code_half(m, pc + 2) | (uint32_t)code_half(m, pc + 4) << 16;
    return code_half(m, pc + 4) * 0x00010001u; /* EWRAM, palette, VRAM, cartridge: [pc+4] twice */
}

/* Opcode fetches: like reads, plus the open-bus / BIOS latches and the prefetch approximation. */
uint32_t gba_fetch32(gba_machine *m, uint32_t addr, bool seq)
{
    uint32_t r = (addr >> 24) & 15;
    uint32_t v;
    if (r == 3) {
        m->now += 1;
        v = rd32(&m->iwram[addr & (GBA_IWRAM_SIZE - 1)]);
    } else if (r >= 8 && r <= 0xD) {
        m->now += (seq && m->prefetch) ? 2 : m->wait32[r][seq];
        v = rom_read32(m, addr);
    } else {
        m->now += m->wait32[r][seq];
        if (addr < GBA_BIOS_SIZE) {
            v = rd32(&m->bios[addr]);
            m->bios_latch = rd32(&m->bios[(addr + 8) & (GBA_BIOS_SIZE - 4)]); /* the prefetched opcode */
        } else {
            v = read32_raw(m, addr);
        }
    }
    m->open_bus = gba_prefetch_latch(m, addr, false);
    return v;
}

uint16_t gba_fetch16(gba_machine *m, uint32_t addr, bool seq)
{
    uint32_t r = (addr >> 24) & 15;
    uint16_t v;
    if (r >= 8 && r <= 0xD) {
        m->now += (seq && m->prefetch) ? 1 : m->wait16[r][seq];
        v = rom_read16(m, addr);
    } else if (r == 3) {
        m->now += 1;
        v = rd16(&m->iwram[addr & (GBA_IWRAM_SIZE - 1)]);
    } else {
        m->now += m->wait16[r][seq];
        if (addr < GBA_BIOS_SIZE) {
            v = rd16(&m->bios[addr]);
            /* The BIOS latch holds the prefetched word: approximate with this halfword pair. */
            m->bios_latch = rd32(&m->bios[(addr + 4) & (GBA_BIOS_SIZE - 4)]);
        } else {
            v = read16_raw(m, addr);
        }
    }
    m->open_bus = gba_prefetch_latch(m, addr, true);
    return v;
}

/* ---- writes --------------------------------------------------------------------------------------------- */

static bool bitmap_mode(const gba_machine *m)
{
    return (m->io[0] & 7) >= 3;
}

void gba_write32(gba_machine *m, uint32_t addr, uint32_t v, bool seq)
{
    m->now += m->wait32[(addr >> 24) & 15][seq];
    if (save_region(addr)) { /* 8-bit bus: the byte of the value the address selects */
        gba_save_write8(m, addr, (uint8_t)(v >> ((addr & 3) * 8)));
        return;
    }
    addr &= ~3u;
    switch (addr >> 24) {
    case 0x2:
        wr32(&m->ewram[addr & (GBA_EWRAM_SIZE - 1)], v);
        break;
    case 0x3:
        wr32(&m->iwram[addr & (GBA_IWRAM_SIZE - 1)], v);
        break;
    case 0x4:
        if ((addr & 0xFFFFFF) < GBA_IO_SIZE) {
            gba_io_write16(m, addr, (uint16_t)v);
            gba_io_write16(m, addr + 2, (uint16_t)(v >> 16));
        } else if (dbg_port(m, addr)) {
            dbg_write16(m, addr, (uint16_t)v);
            dbg_write16(m, addr + 2, (uint16_t)(v >> 16));
        }
        break;
    case 0x5:
        wr32(&m->pal[addr & (GBA_PAL_SIZE - 1)], v);
        break;
    case 0x6:
        wr32(&m->vram[vram_offset(addr)], v);
        break;
    case 0x7:
        wr32(&m->oam[addr & (GBA_OAM_SIZE - 1)], v);
        break;
    default:
        break; /* BIOS, ROM, unmapped: ignored */
    }
}

void gba_write16(gba_machine *m, uint32_t addr, uint16_t v, bool seq)
{
    m->now += m->wait16[(addr >> 24) & 15][seq];
    if (save_region(addr)) {
        gba_save_write8(m, addr, (uint8_t)(v >> ((addr & 1) * 8)));
        return;
    }
    addr &= ~1u;
    switch (addr >> 24) {
    case 0x2:
        wr16(&m->ewram[addr & (GBA_EWRAM_SIZE - 1)], v);
        break;
    case 0x3:
        wr16(&m->iwram[addr & (GBA_IWRAM_SIZE - 1)], v);
        break;
    case 0x4:
        if ((addr & 0xFFFFFF) < GBA_IO_SIZE)
            gba_io_write16(m, addr, v);
        else if (dbg_port(m, addr))
            dbg_write16(m, addr, v);
        break;
    case 0x5:
        wr16(&m->pal[addr & (GBA_PAL_SIZE - 1)], v);
        break;
    case 0x6:
        wr16(&m->vram[vram_offset(addr)], v);
        break;
    case 0x7:
        wr16(&m->oam[addr & (GBA_OAM_SIZE - 1)], v);
        break;
    case 0xD:
        if (gba_eeprom_area(m, addr))
            gba_eeprom_write(m, v);
        break;
    default:
        break;
    }
}

void gba_write8(gba_machine *m, uint32_t addr, uint8_t v, bool seq)
{
    m->now += m->wait16[(addr >> 24) & 15][seq];
    switch (addr >> 24) {
    case 0x2:
        m->ewram[addr & (GBA_EWRAM_SIZE - 1)] = v;
        break;
    case 0x3:
        m->iwram[addr & (GBA_IWRAM_SIZE - 1)] = v;
        break;
    case 0x4:
        if ((addr & 0xFFFFFF) < GBA_IO_SIZE)
            gba_io_write8(m, addr, v);
        else if (dbg_port(m, addr))
            dbg_write8(m, addr, v);
        break;
    case 0x5: /* 8-bit writes to palette fill the halfword */
        wr16(&m->pal[addr & (GBA_PAL_SIZE - 2)], (uint16_t)(v * 0x0101u));
        break;
    case 0x6: { /* BG VRAM: the halfword; OBJ VRAM: ignored */
        uint32_t a = vram_offset(addr);
        if (a < (bitmap_mode(m) ? 0x14000u : 0x10000u))
            wr16(&m->vram[a & ~1u], (uint16_t)(v * 0x0101u));
        break;
    }
    case 0xE:
    case 0xF:
        gba_save_write8(m, addr, v);
        break;
    default:
        break; /* OAM ignores 8-bit writes; BIOS, ROM, unmapped too */
    }
}

/* ---- side-effect-free peeks (public, for tests) --------------------------------------------------------- */

static const uint8_t *peek_ptr(gba_machine *m, uint32_t addr)
{
    switch (addr >> 24) {
    case 0x0:
        return addr < GBA_BIOS_SIZE ? &m->bios[addr] : NULL;
    case 0x2:
        return &m->ewram[addr & (GBA_EWRAM_SIZE - 1)];
    case 0x3:
        return &m->iwram[addr & (GBA_IWRAM_SIZE - 1)];
    case 0x4:
        return (addr & 0xFFFFFF) < GBA_IO_SIZE ? &m->io[addr & 0x3FF] : NULL;
    case 0x5:
        return &m->pal[addr & (GBA_PAL_SIZE - 1)];
    case 0x6:
        return &m->vram[vram_offset(addr)];
    case 0x7:
        return &m->oam[addr & (GBA_OAM_SIZE - 1)];
    case 0x8: case 0x9: case 0xA: case 0xB: case 0xC: case 0xD:
        return (addr & 0x1FFFFFF) < m->rom_size ? &m->rom[addr & 0x1FFFFFF] : NULL;
    case 0xE:
    case 0xF:
        if (m->save.kind == GBA_SAVE_FLASH64 || m->save.kind == GBA_SAVE_FLASH128)
            return &m->save.data[m->save.flash_bank + (addr & 0xFFFF)];
        return &m->save.data[addr & 0x7FFF];
    default:
        return NULL;
    }
}

uint8_t gba_peek8(gba_machine *m, uint32_t addr)
{
    const uint8_t *p = peek_ptr(m, addr);
    return p ? *p : 0;
}

uint16_t gba_peek16(gba_machine *m, uint32_t addr)
{
    addr &= ~1u;
    const uint8_t *p = peek_ptr(m, addr);
    return p ? rd16(p) : 0;
}

uint32_t gba_peek32(gba_machine *m, uint32_t addr)
{
    addr &= ~3u;
    const uint8_t *p = peek_ptr(m, addr);
    return p ? rd32(p) : 0;
}

/* ---- cheats (XBA-197) ------------------------------------------------------------------------------------- */

void gba_cheat_poke(gba_machine *m, uint32_t addr, uint32_t v, int size)
{
    uint32_t region = addr >> 24;
    if (region == 0x4) {
        if (size == 1)
            gba_io_write8(m, addr, (uint8_t)v);
        else if (size == 2)
            gba_io_write16(m, addr & ~1u, (uint16_t)v);
        else {
            gba_io_write16(m, addr & ~3u, (uint16_t)v);
            gba_io_write16(m, (addr & ~3u) + 2, (uint16_t)(v >> 16));
        }
        return;
    }
    if (region < 0x2 || region > 0x7)
        return;
    for (int i = 0; i < size; i++) {
        uint8_t *p = (uint8_t *)peek_ptr(m, addr + (uint32_t)i);
        if (p)
            *p = (uint8_t)(v >> (8 * i));
    }
}

int gba_rom_patch16(gba_machine *m, uint32_t addr, uint16_t v)
{
    uint32_t off = (addr & 0x1FFFFFFu) & ~1u;
    if ((addr >> 24) < 0x8 || (addr >> 24) > 0x9 || off + 2 > m->rom_size)
        return -1;
    gba_state_pin_rom_hash(m);
    m->rom[off] = (uint8_t)v;
    m->rom[off + 1] = (uint8_t)(v >> 8);
    return 0;
}
