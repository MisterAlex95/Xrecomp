/* PIF (HLE boot, joybus) and the cartridge saves (N-1 XBA-169, N-4 XBA-172).
 *
 * Boot: there is no PIF ROM image. n64_pif_boot does what the PIF's boot code leaves behind when it hands over to
 * IPL3: the ROM header and IPL3 (0x000..0xFFF) copied to the RSP's DMEM, PI domain 1 timing from the header's first
 * word, the CPU registers IPL3 reads (s3 ROM type, s4 TV type, s5 reset type, s6 the CIC seed, s7 the version, sp
 * and ra in DMEM), the PIF RAM's CIC status byte, and the CPU at 0xA4000040. The RDRAM is reported configured
 * (RI_SELECT != 0, osMemSize at 0x80000318), so IPL3 skips the RDRAM init.
 *
 * Joybus (PIF RAM command block, channel by channel): on the four controller channels 0x00 / 0xFF info, 0x01 buttons
 * and stick, 0x02 / 0x03 pak read / write (32 bytes, address CRC ignored, data CRC returned); the controller pak is
 * 32 KB of memory, the rumble pak answers 0x80 at 0x8000 once probed and turns its motor with writes at 0xC000. On
 * channel 4, the cartridge: EEPROM 4K / 16K info, 0x04 read and 0x05 write of 8-byte blocks. No RTC (no reply).
 *
 * Saves: SRAM (32 KB, domain 2 at 0x08000000, CPU words and PI DMA), FlashRAM (128 KB, Macronix MX29L1100 ID
 * 0x1111800100C2001E: status / ID, read array, chip and sector erase, 128-byte page program through the PI), EEPROM
 * (joybus). The kind: forced by n64_options, else the ED64 / libdragon header byte (n64_rom_info.save_hint), else
 * from the first access: a write to the flash command register at 0x08010000 makes it FlashRAM, any other domain 2
 * access SRAM, a joybus EEPROM command EEPROM (4K unless the header asks for 16K or a block above 63 is used). The
 * classic per-game save database is not included: no per-title tables in the core. */
#include "n64_internal.h"

#include <stdlib.h>
#include <string.h>

/* ---- Boot ------------------------------------------------------------------------------------------------------ */

static uint8_t cic_seed(n64_cic cic)
{
    switch (cic) {
    case N64_CIC_6103: return 0x78;
    case N64_CIC_6105: return 0x91;
    case N64_CIC_6106: return 0x85;
    default: return 0x3F; /* 6101, 6102, 7101, 7102, libdragon */
    }
}

void n64_pif_boot(n64_machine *m)
{
    n64_cpu *c = &m->cpu;
    uint8_t *dmem = m->spmem;
    memcpy(dmem, m->rom, m->rom_size < 0x1000 ? m->rom_size : 0x1000);

    uint32_t h = m->info.pi_config;
    m->pi_dom[0][0] = h & 0xFF;
    m->pi_dom[0][1] = (h >> 8) & 0xFF;
    m->pi_dom[0][2] = (h >> 16) & 0x0F;
    m->pi_dom[0][3] = (h >> 20) & 0x03;
    /* Domain 2 (SRAM / FlashRAM): the values libultra programs. */
    m->pi_dom[1][0] = 0x05;
    m->pi_dom[1][1] = 0x0C;
    m->pi_dom[1][2] = 0x0D;
    m->pi_dom[1][3] = 0x02;

    uint64_t *g = c->gpr;
    memset(g, 0, sizeof c->gpr);
    g[19] = 0;                               /* s3: ROM type (cartridge) */
    g[20] = m->pal ? 0 : 1;                  /* s4: TV type: 0 PAL, 1 NTSC, 2 MPAL */
    g[21] = 0;                               /* s5: cold reset */
    g[22] = cic_seed(m->info.cic);           /* s6: the CIC seed */
    g[23] = 0;                               /* s7: version */
    g[29] = 0xFFFFFFFFA4001FF0ull;           /* sp */
    g[31] = 0xFFFFFFFFA4001550ull;           /* ra */
    g[11] = 0xFFFFFFFFA4000040ull;           /* t3: the entry */
    c->cop0[C0_STATUS] = 0x34000000u;
    c->cop0[C0_CONFIG] = 0x7006E463u;
    c->pc = 0xFFFFFFFFA4000040ull;
    c->npc = c->pc + 4;
    n64_cpu_status_changed(m);

    /* What the RDRAM init would have left: RI configured, the memory size where libultra and the warm-boot path of
     * the IPL3s read it. */
    m->ri[0] = 0x0E;
    m->ri[1] = 0x40;
    m->ri[3] = 0x14;
    m->ri[4] = 0x00063634;
    uint32_t osmem = m->rdram_size;
    n64_wbe32(m->rdram + 0x318, osmem);
    n64_wbe32(m->rdram + 0x3F0, osmem); /* 6105 */

    memset(m->pif_ram, 0, sizeof m->pif_ram);
    /* The CIC status the PIF leaves in its RAM (byte 0x24..0x27): seed and checksum flags. */
    m->pif_ram[0x26] = cic_seed(m->info.cic);
    m->pif_ram[0x27] = 0x3F;
}

/* ---- Saves ----------------------------------------------------------------------------------------------------- */

size_t n64_save_size(n64_save_kind kind)
{
    switch (kind) {
    case N64_SAVE_EEPROM4K: return 512;
    case N64_SAVE_EEPROM16K: return 2048;
    case N64_SAVE_SRAM: return N64_SRAM_SIZE;
    case N64_SAVE_FLASH: return N64_FLASH_SIZE;
    case N64_SAVE_PAK0: case N64_SAVE_PAK1: case N64_SAVE_PAK2: case N64_SAVE_PAK3: return N64_PAK_SIZE;
    default: return 0;
    }
}

void n64_save_set_kind(n64_machine *m, n64_save_kind kind)
{
    if (kind == N64_SAVE_FLASH && !m->flash) {
        m->flash = malloc(N64_FLASH_SIZE);
        if (!m->flash)
            return;
        memset(m->flash, 0xFF, N64_FLASH_SIZE);
    }
    m->save_kind = kind;
}

static bool eeprom_present(n64_machine *m)
{
    if (m->save_kind == N64_SAVE_NONE)
        return true; /* not known yet: answer, the first command decides */
    return m->save_kind == N64_SAVE_EEPROM4K || m->save_kind == N64_SAVE_EEPROM16K;
}

/* Domain 2: SRAM or FlashRAM. */
static void cart2_detect(n64_machine *m, uint32_t pa, bool write)
{
    if (m->save_kind != N64_SAVE_NONE)
        return;
    if (write && (pa & 0x1FFFF) == 0x10000)
        n64_save_set_kind(m, N64_SAVE_FLASH);
    else
        n64_save_set_kind(m, N64_SAVE_SRAM);
}

static void flash_command(n64_machine *m, uint32_t v)
{
    switch (v >> 24) {
    case 0xD2: /* execute */
        if (m->flash_mode == FLASH_ERASE) {
            if (m->flash_offset == 0xFFFFFFFFu)
                memset(m->flash, 0xFF, N64_FLASH_SIZE);
            else if (m->flash_offset < N64_FLASH_SIZE)
                memset(m->flash + (m->flash_offset & ~0x3FFFu), 0xFF, 0x4000);
            m->save_dirty = true;
            m->flash_status = (m->flash_status & ~0xFFu) | 0x08; /* erase done */
        } else if (m->flash_mode == FLASH_WRITE) {
            if (m->flash_offset + 128 <= N64_FLASH_SIZE) {
                memcpy(m->flash + m->flash_offset, m->flash_buf, 128);
                m->save_dirty = true;
            }
            m->flash_status = (m->flash_status & ~0xFFu) | 0x04; /* program done */
        }
        break;
    case 0xE1: /* status mode */
        m->flash_mode = FLASH_STATUS;
        m->flash_status = 0x11118001u;
        break;
    case 0xF0: /* read array */
        m->flash_mode = FLASH_READ;
        m->flash_status = 0x11118004u;
        break;
    case 0x3C: /* chip erase */
        m->flash_mode = FLASH_ERASE;
        m->flash_offset = 0xFFFFFFFFu;
        break;
    case 0x4B: /* sector erase: page number */
        m->flash_mode = FLASH_ERASE;
        m->flash_offset = (v & 0xFFFF) * 128;
        break;
    case 0x78: /* erase mode */
        m->flash_mode = FLASH_ERASE;
        m->flash_status = 0x11118008u;
        break;
    case 0xB4: /* write mode: the next PI DMA fills the page buffer */
        m->flash_mode = FLASH_WRITE;
        break;
    case 0xA5: /* program the buffer at a page */
        m->flash_mode = FLASH_WRITE;
        m->flash_offset = (v & 0xFFFF) * 128;
        m->flash_status = 0x11118004u;
        break;
    default:
        break;
    }
}

uint32_t n64_cart2_read32(n64_machine *m, uint32_t pa)
{
    cart2_detect(m, pa, false);
    if (m->save_kind == N64_SAVE_SRAM) {
        uint32_t off = (pa - 0x08000000u) & (N64_SRAM_SIZE - 1);
        return n64_be32(m->sram + off);
    }
    if (m->save_kind == N64_SAVE_FLASH)
        return m->flash_status;
    return 0xFFFFFFFFu;
}

void n64_cart2_write32(n64_machine *m, uint32_t pa, uint32_t v)
{
    cart2_detect(m, pa, true);
    if (m->save_kind == N64_SAVE_SRAM) {
        uint32_t off = (pa - 0x08000000u) & (N64_SRAM_SIZE - 1);
        if (n64_be32(m->sram + off) != v) {
            n64_wbe32(m->sram + off, v);
            m->save_dirty = true;
        }
    } else if (m->save_kind == N64_SAVE_FLASH && (pa & 0x1FFFF) == 0x10000) {
        flash_command(m, v);
    }
}

void n64_cart2_dma_read(n64_machine *m, uint32_t cart, uint8_t *dst, uint32_t len)
{
    cart2_detect(m, cart, false);
    if (m->save_kind == N64_SAVE_SRAM) {
        for (uint32_t i = 0; i < len; i++)
            dst[i] = m->sram[(cart - 0x08000000u + i) & (N64_SRAM_SIZE - 1)];
    } else if (m->save_kind == N64_SAVE_FLASH) {
        if (m->flash_mode == FLASH_STATUS) {
            static const uint8_t id[8] = {0x11, 0x11, 0x80, 0x01, 0x00, 0xC2, 0x00, 0x1E};
            for (uint32_t i = 0; i < len; i++)
                dst[i] = id[i & 7];
        } else {
            /* Read array: the flash sits on a 16-bit bus, the cartridge address is half the byte offset. */
            uint32_t off = ((cart - 0x08000000u) & 0xFFFF) * 2;
            for (uint32_t i = 0; i < len; i++)
                dst[i] = m->flash[(off + i) & (N64_FLASH_SIZE - 1)];
        }
    } else {
        memset(dst, 0xFF, len);
    }
}

void n64_cart2_dma_write(n64_machine *m, uint32_t cart, const uint8_t *src, uint32_t len)
{
    cart2_detect(m, cart, true);
    if (m->save_kind == N64_SAVE_SRAM) {
        for (uint32_t i = 0; i < len; i++)
            m->sram[(cart - 0x08000000u + i) & (N64_SRAM_SIZE - 1)] = src[i];
        m->save_dirty = true;
    } else if (m->save_kind == N64_SAVE_FLASH) {
        for (uint32_t i = 0; i < len && i < 128; i++)
            m->flash_buf[i] = src[i];
    }
}

/* ---- Joybus ---------------------------------------------------------------------------------------------------- */

static uint8_t pak_crc(const uint8_t *data)
{
    uint8_t crc = 0;
    for (int i = 0; i <= 32; i++) {
        for (int b = 7; b >= 0; b--) {
            uint8_t x = (crc & 0x80) ? 0x85 : 0x00;
            crc = (uint8_t)(crc << 1);
            if (i < 32 && ((data[i] >> b) & 1))
                crc |= 1;
            crc ^= x;
        }
    }
    return crc;
}

/* A controller command: tx bytes in, rx bytes out. Returns 0 or -1 (no device). */
static int controller_cmd(n64_machine *m, int port, const uint8_t *tx, int txn, uint8_t *rx, int rxn)
{
    n64_pad *p = &m->pad[port];
    if (!p->connected || txn < 1)
        return -1;
    switch (tx[0]) {
    case 0x00:
    case 0xFF:
        if (rxn >= 3) {
            rx[0] = 0x05;
            rx[1] = 0x00;
            rx[2] = p->acc != N64_ACC_NONE ? 0x01 : 0x02;
        }
        return 0;
    case 0x01:
        if (rxn >= 4) {
            rx[0] = (uint8_t)(p->buttons >> 8);
            rx[1] = (uint8_t)p->buttons;
            rx[2] = (uint8_t)p->x;
            rx[3] = (uint8_t)p->y;
        }
        return 0;
    case 0x02: { /* read pak: address (2 bytes) -> 32 bytes + CRC */
        if (txn < 3 || rxn < 33)
            return 0;
        uint16_t addr = (uint16_t)((tx[1] << 8 | tx[2]) & 0xFFE0);
        if (p->acc == N64_ACC_CONTROLLER_PAK && addr < 0x8000) {
            memcpy(rx, m->pak[port] + addr, 32);
        } else if (p->acc == N64_ACC_RUMBLE_PAK) {
            memset(rx, (addr >= 0x8000 && addr < 0x9000) ? (p->rumble_probe ? 0x80 : 0x00) : 0x00, 32);
        } else {
            memset(rx, 0, 32);
        }
        rx[32] = pak_crc(rx);
        if (p->acc == N64_ACC_NONE)
            rx[32] ^= 0xFF;
        return 0;
    }
    case 0x03: { /* write pak: address + 32 bytes -> CRC */
        if (txn < 35 || rxn < 1)
            return 0;
        uint16_t addr = (uint16_t)((tx[1] << 8 | tx[2]) & 0xFFE0);
        const uint8_t *d = tx + 3;
        if (p->acc == N64_ACC_CONTROLLER_PAK && addr < 0x8000) {
            if (memcmp(m->pak[port] + addr, d, 32) != 0) {
                memcpy(m->pak[port] + addr, d, 32);
                m->pak_dirty[port] = true;
            }
        } else if (p->acc == N64_ACC_RUMBLE_PAK) {
            if (addr >= 0x8000 && addr < 0x9000)
                p->rumble_probe = d[0] == 0x80;
            else if (addr >= 0xC000)
                p->rumble = d[0] != 0;
        }
        rx[0] = pak_crc(d);
        if (p->acc == N64_ACC_NONE)
            rx[0] ^= 0xFF;
        return 0;
    }
    default:
        return -1;
    }
}

static int eeprom_cmd(n64_machine *m, const uint8_t *tx, int txn, uint8_t *rx, int rxn)
{
    if (!eeprom_present(m) || txn < 1)
        return -1;
    bool big = m->save_kind == N64_SAVE_EEPROM16K;
    switch (tx[0]) {
    case 0x00:
    case 0xFF:
        if (rxn >= 3) {
            rx[0] = 0x00;
            rx[1] = big ? 0xC0 : 0x80;
            rx[2] = 0x00;
        }
        return 0;
    case 0x04: /* read block */
    case 0x05: { /* write block */
        if (txn < 2)
            return 0;
        if (m->save_kind == N64_SAVE_NONE)
            n64_save_set_kind(m, N64_SAVE_EEPROM4K);
        if (tx[1] >= 64 && m->save_kind == N64_SAVE_EEPROM4K)
            n64_save_set_kind(m, N64_SAVE_EEPROM16K);
        uint32_t off = (uint32_t)tx[1] * 8 % N64_EEPROM_MAX;
        if (tx[0] == 0x04) {
            if (rxn >= 8)
                memcpy(rx, m->eeprom + off, 8);
        } else if (txn >= 10) {
            if (memcmp(m->eeprom + off, tx + 2, 8) != 0) {
                memcpy(m->eeprom + off, tx + 2, 8);
                m->save_dirty = true;
            }
            if (rxn >= 1)
                rx[0] = 0x00;
        }
        return 0;
    }
    default:
        return -1;
    }
}

void n64_pif_write_ram(n64_machine *m, uint32_t off, uint32_t v)
{
    n64_wbe32(m->pif_ram + (off & 0x3C), v);
}

void n64_pif_process(n64_machine *m)
{
    uint8_t *r = m->pif_ram;
    uint8_t cmd = r[63];
    if (cmd & 0x01) { /* run the joybus commands */
        int ch = 0;
        int i = 0;
        while (i < 63 && ch < 6) {
            uint8_t t = r[i];
            if (t == 0xFE) /* end */
                break;
            if (t == 0xFF || t == 0xFD) { /* padding / channel reset */
                i++;
                continue;
            }
            if (t == 0x00) { /* skip this channel */
                ch++;
                i++;
                continue;
            }
            if (i + 1 >= 63)
                break;
            int txn = t & 0x3F, rxn = r[i + 1] & 0x3F;
            int rxi = i + 2 + txn;
            if (rxi + rxn > 63)
                break;
            uint8_t *tx = r + i + 2, *rx = r + rxi;
            int res;
            if (ch < 4)
                res = controller_cmd(m, ch, tx, txn, rx, rxn);
            else if (ch == 4)
                res = eeprom_cmd(m, tx, txn, rx, rxn);
            else
                res = -1;
            if (res < 0)
                r[i + 1] |= 0x80; /* no response */
            i = rxi + rxn;
            ch++;
        }
        r[63] &= (uint8_t)~0x01;
    }
    if (cmd & 0x02) /* CIC challenge: not emulated, acknowledged */
        r[63] &= (uint8_t)~0x02;
    if (cmd & 0x08) /* boot done: the PIF stops waiting */
        r[63] &= (uint8_t)~0x08;
    if (cmd & 0x10) { /* lock out the PIF ROM */
        m->pif_rom_lock = 1;
        r[63] &= (uint8_t)~0x10;
    }
    if (cmd & 0x20) /* acknowledge */
        r[63] |= 0x80;
    if (cmd & 0x40) /* clear PIF RAM */
        memset(r, 0, 64);
}

/* ---- Public save / controller API ------------------------------------------------------------------------------ */

n64_save_kind n64_save_kind_detected(n64_machine *m)
{
    return m->save_kind;
}

static uint8_t *save_ptr(n64_machine *m, n64_save_kind kind, bool **dirty)
{
    switch (kind) {
    case N64_SAVE_EEPROM4K:
    case N64_SAVE_EEPROM16K:
        *dirty = &m->save_dirty;
        return m->save_kind == kind ? m->eeprom : NULL;
    case N64_SAVE_SRAM:
        *dirty = &m->save_dirty;
        return m->save_kind == kind ? m->sram : NULL;
    case N64_SAVE_FLASH:
        *dirty = &m->save_dirty;
        return m->save_kind == kind ? m->flash : NULL;
    case N64_SAVE_PAK0: case N64_SAVE_PAK1: case N64_SAVE_PAK2: case N64_SAVE_PAK3: {
        int p = kind - N64_SAVE_PAK0;
        *dirty = &m->pak_dirty[p];
        return m->pak[p];
    }
    default:
        return NULL;
    }
}

const uint8_t *n64_save_data(n64_machine *m, n64_save_kind kind, size_t *size)
{
    bool *dirty;
    const uint8_t *p = save_ptr(m, kind, &dirty);
    if (size)
        *size = p ? n64_save_size(kind) : 0;
    return p;
}

int n64_save_take_dirty(n64_machine *m, n64_save_kind kind)
{
    bool *dirty;
    if (!save_ptr(m, kind, &dirty))
        return 0;
    int d = *dirty;
    *dirty = false;
    return d;
}

int n64_save_load(n64_machine *m, n64_save_kind kind, const uint8_t *data, size_t size)
{
    if (!data || size != n64_save_size(kind) || size == 0)
        return -1;
    if (kind < N64_SAVE_PAK0) {
        if (m->save_kind != N64_SAVE_NONE && m->save_kind != kind)
            return -1;
        n64_save_set_kind(m, kind);
    }
    bool *dirty;
    uint8_t *p = save_ptr(m, kind, &dirty);
    if (!p)
        return -1;
    memcpy(p, data, size);
    *dirty = false;
    return 0;
}

void n64_set_controller(n64_machine *m, int port, int connected, uint16_t buttons, int8_t x, int8_t y)
{
    if (port < 0 || port > 3)
        return;
    m->pad[port].connected = connected != 0;
    m->pad[port].buttons = buttons;
    m->pad[port].x = x;
    m->pad[port].y = y;
}

void n64_set_accessory(n64_machine *m, int port, n64_accessory acc)
{
    if (port < 0 || port > 3)
        return;
    m->pad[port].acc = acc;
    m->pad[port].rumble = false;
    m->pad[port].rumble_probe = false;
}

int n64_rumble(n64_machine *m, int port)
{
    return port >= 0 && port < 4 && m->pad[port].rumble;
}
