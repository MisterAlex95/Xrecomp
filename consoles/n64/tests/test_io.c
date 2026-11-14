/* N64 core unit tests (XBA-169, XBA-172): ROM byte orders and header, the HLE PIF boot state, joybus (controllers,
 * controller pak with its CRC, rumble pak, EEPROM), SRAM and FlashRAM through the PI, PI DMA, the AI (queue,
 * interrupt, 48 kHz output) and the VI picture. Register sequences and data are made here; no ROM files. */
#include "n64_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;

#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        checks++;                                                                                                     \
        if (!(cond)) {                                                                                                \
            fails++;                                                                                                  \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                               \
            printf(__VA_ARGS__);                                                                                      \
            printf("\n");                                                                                             \
        }                                                                                                             \
    } while (0)
#define CHECK_EQ(got, want, what)                                                                                     \
    CHECK((uint64_t)(got) == (uint64_t)(want), "%s = 0x%llX, want 0x%llX", what, (unsigned long long)(got),          \
          (unsigned long long)(want))

#define ROM_SIZE 0x10000

static void make_rom(uint8_t *rom)
{
    memset(rom, 0, ROM_SIZE);
    static const uint8_t hdr[] = {0x80, 0x37, 0x12, 0x40, 0x00, 0x00, 0x00, 0x0F, 0x80, 0x00, 0x04, 0x00,
                                  0x00, 0x00, 0x14, 0x4C, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0};
    memcpy(rom, hdr, sizeof hdr);
    memcpy(rom + 0x20, "XBA TEST ROM        ", 20);
    memcpy(rom + 0x3B, "NXTP", 4); /* region 'P': PAL */
    rom[0x3F] = 2;
    memcpy(rom + 0x40 + 0x300, " Libdragon IPL3 ", 16);
    for (int i = 0; i < 16; i++)
        rom[0x2000 + i] = (uint8_t)(0xA0 + i);
}

static n64_machine *mk(const n64_options *o)
{
    static uint8_t rom[ROM_SIZE];
    make_rom(rom);
    char err[128];
    n64_machine *m = n64_create(rom, ROM_SIZE, o, err, sizeof err);
    if (!m) {
        printf("FAIL: n64_create: %s\n", err);
        exit(1);
    }
    m->cpu.cop0[C0_STATUS] = 0x34000000u; /* interrupts off: these tests drive the bus directly */
    n64_cpu_status_changed(m);
    n64_poke32(m, 0x80000800u, 0x08000200u); /* j 0x80000800 */
    n64_poke32(m, 0x80000804u, 0);
    m->cpu.pc = 0xFFFFFFFF80000800ull;
    m->cpu.npc = m->cpu.pc + 4;
    return m;
}

static void w32(n64_machine *m, uint32_t pa, uint32_t v) { n64_bus_write32(m, pa, v, 0xFFFFFFFFu); }
static uint32_t r32(n64_machine *m, uint32_t pa) { return n64_bus_read32(m, pa); }

/* ---- ROM ------------------------------------------------------------------------------------------------------- */

static void test_rom(void)
{
    static uint8_t z[ROM_SIZE], v[ROM_SIZE], n[ROM_SIZE];
    make_rom(z);
    for (int i = 0; i < ROM_SIZE; i += 2) {
        v[i] = z[i + 1];
        v[i + 1] = z[i];
    }
    for (int i = 0; i < ROM_SIZE; i += 4) {
        n[i] = z[i + 3];
        n[i + 1] = z[i + 2];
        n[i + 2] = z[i + 1];
        n[i + 3] = z[i];
    }
    CHECK_EQ(n64_rom_detect(z, ROM_SIZE), N64_ROM_Z64, "z64 detected");
    CHECK_EQ(n64_rom_detect(v, ROM_SIZE), N64_ROM_V64, "v64 detected");
    CHECK_EQ(n64_rom_detect(n, ROM_SIZE), N64_ROM_N64, "n64 detected");
    CHECK(n64_rom_normalize(v, ROM_SIZE) == 0 && memcmp(v, z, ROM_SIZE) == 0, "v64 normalised");
    CHECK(n64_rom_normalize(n, ROM_SIZE) == 0 && memcmp(n, z, ROM_SIZE) == 0, "n64 normalised");
    uint8_t junk[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK_EQ(n64_rom_detect(junk, 8), N64_ROM_UNKNOWN, "junk not a ROM");

    n64_rom_info info;
    CHECK(n64_rom_parse(z, ROM_SIZE, &info) == 0, "parse");
    CHECK(strcmp(info.title, "XBA TEST ROM") == 0, "title \"%s\"", info.title);
    CHECK(strcmp(info.game_code, "NXTP") == 0, "game code %s", info.game_code);
    CHECK_EQ(info.crc1, 0x12345678, "crc1");
    CHECK_EQ(info.crc2, 0x9ABCDEF0, "crc2");
    CHECK_EQ(info.boot_address, 0x80000400, "boot address");
    CHECK_EQ(info.region, 'P', "region");
    CHECK_EQ(info.pal, 1, "PAL");
    CHECK_EQ(info.version, 2, "version");
    CHECK_EQ(info.cic, N64_CIC_LIBDRAGON, "libdragon IPL3 recognised");
    CHECK_EQ(info.save_hint, N64_SAVE_NONE, "no save hint");
    z[0x3C] = 'E';
    z[0x3D] = 'D';
    z[0x3F] = 0x51; /* FlashRAM + RTC */
    n64_rom_parse(z, ROM_SIZE, &info);
    CHECK_EQ(info.save_hint, N64_SAVE_FLASH, "ED64 save byte: FlashRAM");
    CHECK_EQ(info.save_hint_rtc, 1, "ED64 save byte: RTC");
    CHECK_EQ(n64_crc32((const uint8_t *)"123456789", 9), 0xCBF43926u, "CRC-32 check value");

    /* A machine from a v64 file: the ROM is normalised, the order reported. */
    make_rom(z);
    for (int i = 0; i < ROM_SIZE; i += 2) {
        v[i] = z[i + 1];
        v[i + 1] = z[i];
    }
    char err[64];
    n64_machine *m = n64_create(v, ROM_SIZE, NULL, err, sizeof err);
    CHECK(m != NULL, "create from v64");
    if (m) {
        CHECK_EQ(n64_info(m)->order, N64_ROM_V64, "order reported");
        CHECK_EQ(n64_peek32(m, 0xB0002000u), 0xA0A1A2A3u, "ROM readable big-endian");
        n64_destroy(m);
    }
    CHECK(n64_create(junk, 8, NULL, err, sizeof err) == NULL, "tiny file refused");
}

/* ---- Boot ------------------------------------------------------------------------------------------------------ */

static void test_boot(void)
{
    n64_machine *m = mk(NULL);
    n64_reset(m);
    CHECK_EQ(n64_pc(m), 0xFFFFFFFFA4000040ull, "boot PC in DMEM");
    CHECK_EQ(n64_peek32(m, 0xA4000000u), 0x80371240u, "header copied to DMEM");
    CHECK_EQ(n64_peek32(m, 0xA4000340u), 0x204C6962u, "IPL3 copied to DMEM");
    CHECK_EQ(n64_reg(m, 20), 0, "s4: PAL from the region");
    CHECK_EQ(n64_reg(m, 22), 0x3F, "s6: the 6102 seed");
    CHECK_EQ(n64_reg(m, 29), 0xFFFFFFFFA4001FF0ull, "sp");
    CHECK(r32(m, 0x0470000C) != 0, "RI_SELECT reports the RDRAM configured");
    CHECK_EQ(n64_peek32(m, 0x80000318u), N64_RDRAM_SIZE, "osMemSize");
    CHECK_EQ(m->pi_dom[0][0], 0x40, "PI domain 1 latency from the header");
    CHECK_EQ(m->pi_dom[0][1], 0x12, "PI domain 1 pulse width");
    CHECK_EQ(m->pi_dom[0][2], 0x07, "PI domain 1 page size");
    CHECK_EQ(m->pi_dom[0][3], 0x03, "PI domain 1 release");
    CHECK_EQ(r32(m, 0x04300004), 0x02020102u, "MI_VERSION");
    n64_destroy(m);
    n64_options o = {.force_pal = 0};
    m = mk(&o);
    CHECK_EQ(n64_reg(m, 20), 1, "s4: NTSC forced");
    n64_destroy(m);
}

/* ---- Joybus ---------------------------------------------------------------------------------------------------- */

/* Sends a PIF RAM command block through the SI and reads the answer back. */
static void pif_run(n64_machine *m, uint8_t block[64])
{
    memcpy(m->rdram + 0x1000, block, 64);
    w32(m, 0x04800000, 0x1000);
    w32(m, 0x04800010, 0x1FC007C0); /* PIF_AD_WR64B */
    n64_run_cycles(m, 5000);
    CHECK(r32(m, 0x04300008) & (1u << N64_MI_SI), "SI interrupt after the write");
    w32(m, 0x04800018, 0); /* clear it */
    w32(m, 0x04800000, 0x2000);
    w32(m, 0x04800004, 0x1FC007C0); /* PIF_AD_RD64B */
    n64_run_cycles(m, 5000);
    memcpy(block, m->rdram + 0x2000, 64);
}

static void test_joybus(void)
{
    n64_machine *m = mk(NULL);
    uint8_t b[64];

    /* Info and buttons on port 0; port 1 empty. */
    n64_set_controller(m, 0, 1, N64_BTN_A | N64_BTN_START | N64_BTN_CLEFT, 42, -17);
    memset(b, 0, 64);
    uint8_t q[] = {0x01, 0x03, 0x00, 0xFF, 0xFF, 0xFF, 0x01, 0x03, 0x00, 0xFF, 0xFF, 0xFF, 0xFE};
    memcpy(b, q, sizeof q);
    b[63] = 0x01;
    pif_run(m, b);
    CHECK(b[3] == 0x05 && b[4] == 0x00 && b[5] == 0x01, "port 0 info: controller, pak in (%02X %02X %02X)", b[3], b[4],
          b[5]);
    CHECK(b[7] & 0x80, "port 1 empty: no response flag");
    memset(b, 0, 64);
    uint8_t q2[] = {0x01, 0x04, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE};
    memcpy(b, q2, sizeof q2);
    b[63] = 0x01;
    pif_run(m, b);
    CHECK(b[3] == 0x90 && b[4] == 0x02, "buttons A START C-left (%02X %02X)", b[3], b[4]);
    CHECK(b[5] == 42 && (int8_t)b[6] == -17, "stick");
    CHECK_EQ(b[63] & 1, 0, "command bit cleared");

    /* Controller pak: write 32 bytes at 0x0100, read them back, both CRCs. */
    uint8_t data[32];
    for (int i = 0; i < 32; i++)
        data[i] = (uint8_t)(i * 7 + 1);
    memset(b, 0, 64);
    b[0] = 35;
    b[1] = 1;
    b[2] = 0x03;
    b[3] = 0x01;
    b[4] = 0x00;
    memcpy(b + 5, data, 32);
    b[38] = 0xFE;
    b[63] = 1;
    pif_run(m, b);
    uint8_t wcrc = b[37];
    memset(b, 0, 64);
    b[0] = 3;
    b[1] = 33;
    b[2] = 0x02;
    b[3] = 0x01;
    b[4] = 0x00;
    b[38] = 0xFE;
    b[63] = 1;
    pif_run(m, b);
    CHECK(memcmp(b + 5, data, 32) == 0, "pak read back");
    CHECK_EQ(b[37], wcrc, "read CRC = write CRC");
    size_t sz;
    const uint8_t *pak = n64_save_data(m, N64_SAVE_PAK0, &sz);
    CHECK(pak && sz == N64_PAK_SIZE && memcmp(pak + 0x100, data, 32) == 0, "pak image");
    CHECK_EQ(n64_save_take_dirty(m, N64_SAVE_PAK0), 1, "pak dirty");
    CHECK_EQ(n64_save_take_dirty(m, N64_SAVE_PAK0), 0, "pak dirty cleared");
    /* The pak data CRC (CRC-8, polynomial 0x85, over the 32 bytes and a zero byte): 0x0A for 32 x 0xFF. */
    memset(b, 0, 64);
    b[0] = 35;
    b[1] = 1;
    b[2] = 0x03;
    b[3] = 0x02;
    b[4] = 0x00;
    memset(b + 5, 0xFF, 32);
    b[38] = 0xFE;
    b[63] = 1;
    pif_run(m, b);
    CHECK_EQ(b[37], 0x0A, "CRC of 32 x 0xFF");

    /* Rumble pak: probe at 0x8000, motor at 0xC000. */
    n64_set_accessory(m, 0, N64_ACC_RUMBLE_PAK);
    memset(b, 0, 64);
    b[0] = 35;
    b[1] = 1;
    b[2] = 0x03;
    b[3] = 0x80;
    b[4] = 0x00;
    memset(b + 5, 0x80, 32);
    b[38] = 0xFE;
    b[63] = 1;
    pif_run(m, b);
    memset(b, 0, 64);
    b[0] = 3;
    b[1] = 33;
    b[2] = 0x02;
    b[3] = 0x80;
    b[4] = 0x00;
    b[38] = 0xFE;
    b[63] = 1;
    pif_run(m, b);
    CHECK(b[5] == 0x80 && b[36] == 0x80, "rumble pak answers 0x80 once probed");
    memset(b, 0, 64);
    b[0] = 35;
    b[1] = 1;
    b[2] = 0x03;
    b[3] = 0xC0;
    b[4] = 0x00;
    memset(b + 5, 0x01, 32);
    b[38] = 0xFE;
    b[63] = 1;
    pif_run(m, b);
    CHECK_EQ(n64_rumble(m, 0), 1, "motor on");

    /* EEPROM on channel 4: info, write block 3, read it back; detected as 4K. */
    memset(b, 0, 64);
    uint8_t e1[] = {0, 0, 0, 0, 0x01, 0x03, 0x00, 0xFF, 0xFF, 0xFF, 0xFE};
    memcpy(b, e1, sizeof e1);
    b[63] = 1;
    pif_run(m, b);
    CHECK(b[7] == 0x00 && b[8] == 0x80, "EEPROM 4K info (%02X %02X)", b[7], b[8]);
    memset(b, 0, 64);
    uint8_t e2[] = {0, 0, 0, 0, 0x0A, 0x01, 0x05, 0x03, 1, 2, 3, 4, 5, 6, 7, 8, 0xFF, 0xFE};
    memcpy(b, e2, sizeof e2);
    b[63] = 1;
    pif_run(m, b);
    memset(b, 0, 64);
    uint8_t e3[] = {0, 0, 0, 0, 0x02, 0x08, 0x04, 0x03, 0, 0, 0, 0, 0, 0, 0, 0, 0xFE};
    memcpy(b, e3, sizeof e3);
    b[63] = 1;
    pif_run(m, b);
    CHECK(b[8] == 1 && b[15] == 8, "EEPROM block read back");
    CHECK_EQ(n64_save_kind_detected(m), N64_SAVE_EEPROM4K, "EEPROM detected at its first command");
    const uint8_t *ee = n64_save_data(m, N64_SAVE_EEPROM4K, &sz);
    CHECK(ee && sz == 512 && ee[24] == 1, "EEPROM image");
    CHECK_EQ(n64_save_take_dirty(m, N64_SAVE_EEPROM4K), 1, "EEPROM dirty");
    n64_destroy(m);
}

/* ---- PI, SRAM, FlashRAM ---------------------------------------------------------------------------------------- */

static void pi_dma(n64_machine *m, uint32_t dram, uint32_t cart, uint32_t len, bool to_rdram)
{
    w32(m, 0x04600000, dram);
    w32(m, 0x04600004, cart);
    w32(m, to_rdram ? 0x0460000C : 0x04600008, len - 1);
    CHECK(r32(m, 0x04600010) & 1, "PI busy during the DMA");
    n64_run_cycles(m, 200000);
    CHECK(!(r32(m, 0x04600010) & 1), "PI idle after the DMA");
    CHECK(r32(m, 0x04300008) & (1u << N64_MI_PI), "PI interrupt");
    w32(m, 0x04600010, 2); /* clear */
}

static void test_pi_saves(void)
{
    n64_machine *m = mk(NULL);
    /* ROM -> RDRAM, registers advanced. */
    pi_dma(m, 0x3000, 0x10002000, 16, true);
    CHECK_EQ(n64_peek32(m, 0x80003000u), 0xA0A1A2A3u, "ROM DMA data");
    CHECK_EQ(r32(m, 0x04600000), 0x3010, "PI_DRAM_ADDR advanced");
    CHECK_EQ(r32(m, 0x04600004), 0x10002010, "PI_CART_ADDR advanced");

    /* SRAM, detected at the first access: RDRAM -> SRAM -> RDRAM. */
    CHECK_EQ(n64_save_kind_detected(m), N64_SAVE_NONE, "save kind unknown at first");
    for (int i = 0; i < 256; i++)
        m->rdram[0x4000 + i] = (uint8_t)(255 - i);
    pi_dma(m, 0x4000, 0x08000100, 256, false);
    CHECK_EQ(n64_save_kind_detected(m), N64_SAVE_SRAM, "SRAM detected");
    memset(m->rdram + 0x5000, 0, 256);
    pi_dma(m, 0x5000, 0x08000100, 256, true);
    CHECK(memcmp(m->rdram + 0x5000, m->rdram + 0x4000, 256) == 0, "SRAM read back");
    size_t sz;
    const uint8_t *sram = n64_save_data(m, N64_SAVE_SRAM, &sz);
    CHECK(sram && sz == N64_SRAM_SIZE && sram[0x100] == 255, "SRAM image");
    CHECK_EQ(n64_save_take_dirty(m, N64_SAVE_SRAM), 1, "SRAM dirty");
    CHECK(n64_save_data(m, N64_SAVE_FLASH, &sz) == NULL, "no flash image");
    n64_destroy(m);

    /* FlashRAM, detected from its command register; ID, erase, program, read. */
    m = mk(NULL);
    w32(m, 0x08010000, 0xE1000000); /* status mode */
    n64_run_cycles(m, 1000);
    CHECK_EQ(n64_save_kind_detected(m), N64_SAVE_FLASH, "FlashRAM detected");
    pi_dma(m, 0x3000, 0x08000000, 8, true);
    CHECK_EQ(n64_peek32(m, 0x80003000u), 0x11118001u, "flash ID high");
    CHECK_EQ(n64_peek32(m, 0x80003004u), 0x00C2001Eu, "flash ID low (MX29L1100)");
    for (int i = 0; i < 128; i++)
        m->rdram[0x4000 + i] = (uint8_t)i;
    w32(m, 0x08010000, 0xB4000000); /* write buffer */
    n64_run_cycles(m, 1000);
    pi_dma(m, 0x4000, 0x08000000, 128, false);
    w32(m, 0x08010000, 0xA5000000 | 130); /* page 130 */
    n64_run_cycles(m, 1000);
    w32(m, 0x08010000, 0xD2000000); /* execute */
    n64_run_cycles(m, 1000);
    w32(m, 0x08010000, 0xF0000000); /* read array */
    n64_run_cycles(m, 1000);
    memset(m->rdram + 0x5000, 0, 128);
    pi_dma(m, 0x5000, 0x08000000 + 130 * 128 / 2, 128, true); /* 16-bit bus: half the offset */
    CHECK(memcmp(m->rdram + 0x5000, m->rdram + 0x4000, 128) == 0, "flash page read back");
    const uint8_t *fl = n64_save_data(m, N64_SAVE_FLASH, &sz);
    CHECK(fl && sz == N64_FLASH_SIZE && fl[130 * 128 + 5] == 5 && fl[0] == 0xFF, "flash image");
    w32(m, 0x08010000, 0x4B000000 | 128); /* erase the sector of page 128 */
    n64_run_cycles(m, 1000);
    w32(m, 0x08010000, 0x78000000);
    n64_run_cycles(m, 1000);
    w32(m, 0x08010000, 0xD2000000);
    n64_run_cycles(m, 1000);
    CHECK_EQ(fl[130 * 128 + 5], 0xFF, "sector erased");

    /* Save load: the right size only, and the kind must agree. */
    static uint8_t img[N64_FLASH_SIZE];
    memset(img, 0x5A, sizeof img);
    CHECK(n64_save_load(m, N64_SAVE_FLASH, img, sizeof img) == 0, "flash load");
    CHECK_EQ(n64_save_data(m, N64_SAVE_FLASH, &sz)[77], 0x5A, "flash loaded");
    CHECK(n64_save_load(m, N64_SAVE_SRAM, img, N64_SRAM_SIZE) != 0, "a different kind is refused");
    CHECK(n64_save_load(m, N64_SAVE_FLASH, img, 100) != 0, "a wrong size is refused");
    n64_destroy(m);

    /* The header hint decides before any access. */
    n64_options o = {.force_pal = -1, .save_kind = N64_SAVE_EEPROM16K};
    m = mk(&o);
    CHECK_EQ(n64_save_kind_detected(m), N64_SAVE_EEPROM16K, "forced save kind");
    n64_destroy(m);
}

/* ---- AI -------------------------------------------------------------------------------------------------------- */

static void test_ai(void)
{
    n64_machine *m = mk(NULL);
    int16_t buf[4096 * 2];
    n64_audio_pull(m, buf, 4096); /* drop what boot made */
    uint32_t vi_clock = 48681812u;
    for (uint32_t i = 0; i < 0x1000; i += 4) {
        n64_wbe16(m->rdram + 0x10000 + i, 0x1000);
        n64_wbe16(m->rdram + 0x10000 + i + 2, (uint16_t)-0x1000);
    }
    m->pal = false;
    w32(m, 0x04500010, vi_clock / 32000 - 1); /* DACRATE: 32 kHz */
    w32(m, 0x04500014, 15);
    w32(m, 0x04500008, 1); /* DMA on */
    w32(m, 0x04500000, 0x10000);
    w32(m, 0x04500004, 0x1000); /* 1024 frames: 32 ms */
    CHECK(r32(m, 0x04300008) & (1u << N64_MI_AI), "AI interrupt when the buffer starts");
    CHECK(r32(m, 0x0450000C) & 0x40000000u, "AI busy");
    CHECK(!(r32(m, 0x0450000C) & 0x80000000u), "AI not full with one buffer");
    w32(m, 0x0450000C, 0); /* clear the interrupt */
    w32(m, 0x04500004, 0x1000); /* queue a second */
    CHECK(r32(m, 0x0450000C) & 0x80000000u, "AI full with two buffers");
    CHECK(!(r32(m, 0x04300008) & (1u << N64_MI_AI)), "no interrupt for a queued buffer");
    CHECK_EQ(n64_audio_source_rate(m), vi_clock / (vi_clock / 32000), "source rate");
    n64_run_cycles(m, N64_CPU_HZ / 50); /* 20 ms */
    size_t n = n64_audio_pull(m, buf, 4096);
    CHECK(n >= 950 && n <= 970, "20 ms of output at 48 kHz: %zu frames", n);
    CHECK(buf[2 * 100] == 0x1000 && buf[2 * 100 + 1] == -0x1000, "samples (%d %d)", buf[200], buf[201]);
    n64_run_cycles(m, N64_CPU_HZ / 50); /* 40 ms: the first buffer ended, the second started */
    CHECK(r32(m, 0x04300008) & (1u << N64_MI_AI), "AI interrupt when the second buffer starts");
    CHECK(!(r32(m, 0x0450000C) & 0x80000000u), "not full any more");
    uint32_t left = r32(m, 0x04500004);
    CHECK(left > 0 && left < 0x1000, "AI_LEN counts down (%u)", left);
    n64_run_cycles(m, N64_CPU_HZ / 20);
    CHECK(!(r32(m, 0x0450000C) & 0x40000000u), "idle after both");
    n = n64_audio_pull(m, buf, 4096);
    CHECK(n > 3000, "output keeps flowing while idle (%zu)", n);
    n64_destroy(m);
}

/* ---- VI -------------------------------------------------------------------------------------------------------- */

static void test_vi(void)
{
    n64_options ntsc = {.force_pal = 0};
    n64_machine *m = mk(&ntsc);
    static uint32_t px[640 * 480];
    int w, h;
    CHECK(n64_display(m, px, 640, 480, &w, &h) != 0, "blank before the VI is set up");
    w32(m, 0x04400004, 0x100000); /* origin */
    w32(m, 0x04400008, 320);      /* width */
    w32(m, 0x04400024, 0x006C02EC); /* H_VIDEO 108..748 */
    w32(m, 0x04400028, 0x002501FF); /* V_VIDEO 37..511 */
    w32(m, 0x04400030, 0x200);      /* X_SCALE 0.5 */
    w32(m, 0x04400034, 0x400);      /* Y_SCALE 1.0 */
    w32(m, 0x04400000, 0x3202);     /* 16-bit */
    n64_wbe16(m->rdram + 0x100000, 0xF801);           /* red */
    n64_wbe16(m->rdram + 0x100000 + 2 * 321, 0x07C1); /* green at (1, 1) */
    CHECK(n64_display(m, px, 640, 480, &w, &h) == 0, "16-bit picture");
    CHECK(w == 320 && h == 237, "visible area %dx%d", w, h);
    CHECK_EQ(px[0], 0xFF0000, "red pixel");
    CHECK_EQ(px[321], 0x00FF00, "green pixel");
    w32(m, 0x04400000, 0x3203); /* 32-bit */
    n64_wbe32(m->rdram + 0x100000, 0x123456FF);
    n64_display(m, px, 640, 480, &w, &h);
    CHECK_EQ(px[0], 0x123456, "32-bit pixel");
    CHECK(n64_display(m, px, 100, 50, &w, &h) == 0 && w == 100 && h == 50, "clipped to the buffer");

    /* Timing: V_CURRENT counts lines; the VI interrupt at V_INTR ends n64_run_frame; 60 fields a second (NTSC). */
    w32(m, 0x0440000C, 0x200); /* V_INTR: line 256 */
    n64_run_frame(m);
    CHECK_EQ(r32(m, 0x04400010) >> 1, 256, "frame ends at V_INTR");
    CHECK(r32(m, 0x04300008) & (1u << N64_MI_VI), "VI interrupt");
    uint64_t c0 = n64_cycles(m);
    for (int i = 0; i < 60; i++)
        n64_run_frame(m);
    double secs = (double)(n64_cycles(m) - c0) / N64_CPU_HZ;
    CHECK(secs > 0.99 && secs < 1.01, "60 NTSC fields in %.4f s", secs);
    n64_destroy(m);

    n64_options o = {.force_pal = 1};
    m = mk(&o);
    c0 = n64_cycles(m);
    for (int i = 0; i < 50; i++)
        n64_run_frame(m);
    secs = (double)(n64_cycles(m) - c0) / N64_CPU_HZ;
    CHECK(secs > 0.99 && secs < 1.01, "50 PAL fields in %.4f s", secs);
    n64_destroy(m);
}

int main(void)
{
    test_rom();
    test_boot();
    test_joybus();
    test_pi_saves();
    test_ai();
    test_vi();
    printf("n64_io: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
