/* Unit tests of the GBA save memory (G-7, XBA-156): kind detection from the ROM's ID strings, SRAM (8-bit bus,
 * mirrors), flash 64 / 128 KB (ID mode, program, sector / chip erase, bank switch), EEPROM 512 B / 8 KB driven by
 * DMA3 as games do (address width from the DMA length, read reply, ready bit), the dirty flag and
 * gba_save_load / gba_save_data round trips. Our own code. */
#include "gba_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...)                                                                                       \
    do {                                                                                                       \
        if (!(cond)) {                                                                                         \
            failures++;                                                                                        \
            printf("FAIL %s:%d: ", __func__, __LINE__);                                                        \
            printf(__VA_ARGS__);                                                                               \
            printf("\n");                                                                                      \
        }                                                                                                      \
    } while (0)

/* A ROM of `size` bytes with `id` (may be NULL) at offset 0x200. */
static gba_machine *machine_rom(const char *id, size_t size)
{
    uint8_t *rom = calloc(1, size);
    if (id)
        memcpy(rom + 0x200, id, strlen(id));
    gba_machine *m = gba_create(rom, size, NULL, 0);
    free(rom);
    if (!m) {
        printf("FAIL: gba_create\n");
        exit(1);
    }
    return m;
}

static gba_machine *machine(const char *id) { return machine_rom(id, 0x1000); }

static void w8(gba_machine *m, uint32_t a, uint8_t v) { gba_write8(m, a, v, false); }
static uint8_t r8(gba_machine *m, uint32_t a) { return gba_read8(m, a, false); }

static void test_detect(void)
{
    static const struct { const char *id; gba_save_kind kind; size_t size; } cases[] = {
        {NULL, GBA_SAVE_NONE, 0},
        {"SRAM_V113", GBA_SAVE_SRAM, 0x8000},
        {"SRAM_F_V100", GBA_SAVE_SRAM, 0x8000},
        {"FLASH_V126", GBA_SAVE_FLASH64, 0x10000},
        {"FLASH512_V131", GBA_SAVE_FLASH64, 0x10000},
        {"FLASH1M_V103", GBA_SAVE_FLASH128, 0x20000},
        {"EEPROM_V124", GBA_SAVE_EEPROM512, 512},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        gba_machine *m = machine(cases[i].id);
        size_t n = 99;
        const uint8_t *p = gba_save_data(m, &n);
        CHECK(gba_save_type(m) == cases[i].kind && n == cases[i].size && (p != NULL) == (n != 0),
              "%s: kind %d size %zu", cases[i].id ? cases[i].id : "(none)", (int)gba_save_type(m), n);
        CHECK(!gba_save_take_dirty(m), "%s: clean at start", cases[i].id ? cases[i].id : "(none)");
        gba_destroy(m);
    }
    /* An ID string off word alignment does not count (the libraries align them). */
    uint8_t rom[0x400] = {0};
    memcpy(rom + 0x101, "SRAM_V", 6);
    gba_machine *m = gba_create(rom, sizeof rom, NULL, 0);
    CHECK(gba_save_type(m) == GBA_SAVE_NONE, "unaligned ID ignored");
    gba_destroy(m);
}

static void test_none_fallback(void)
{
    gba_machine *m = machine(NULL);
    CHECK(r8(m, 0x0E000000) == 0xFF && r8(m, 0x0F000123) == 0xFF, "no chip reads FFh");
    CHECK(gba_save_load(m, (const uint8_t[16]){0}, 16) == -1, "NONE refuses a 16-byte image");
    w8(m, 0x0E000010, 0x42);
    CHECK(gba_save_type(m) == GBA_SAVE_SRAM && r8(m, 0x0E000010) == 0x42, "first write: SRAM");
    CHECK(gba_save_take_dirty(m) && !gba_save_take_dirty(m), "dirty once");
    gba_destroy(m);

    /* A 32 KB image put back on a game without ID makes it SRAM. */
    m = machine(NULL);
    static uint8_t img[0x8000];
    for (size_t i = 0; i < sizeof img; i++)
        img[i] = (uint8_t)(i * 7);
    CHECK(gba_save_load(m, img, sizeof img) == 0 && gba_save_type(m) == GBA_SAVE_SRAM, "NONE + 32 KB -> SRAM");
    CHECK(r8(m, 0x0E000100) == img[0x100], "loaded data visible");
    gba_destroy(m);
}

static void test_sram(void)
{
    gba_machine *m = machine("SRAM_V113");
    w8(m, 0x0E001234, 0x5A);
    CHECK(r8(m, 0x0E009234) == 0x5A && r8(m, 0x0F001234) == 0x5A && r8(m, 0x0E011234) == 0x5A,
          "mirrors every 32 KB and at 0x0F000000");
    CHECK(gba_read16(m, 0x0E001234, false) == 0x5A5A, "16-bit read repeats the byte");
    CHECK(gba_read32(m, 0x0E001234, false) == 0x5A5A5A5Au, "32-bit read repeats the byte");
    gba_write32(m, 0x0E002002, 0xAABBCCDDu, false); /* the byte the address selects: bits 16-23 */
    CHECK(r8(m, 0x0E002002) == 0xBB && r8(m, 0x0E002000) == 0xFF && r8(m, 0x0E002003) == 0xFF,
          "32-bit write stores one byte: %02X", r8(m, 0x0E002002));
    gba_write16(m, 0x0E002011, 0x1122, false);
    CHECK(r8(m, 0x0E002011) == 0x11 && r8(m, 0x0E002010) == 0xFF, "16-bit write at an odd address: %02X",
          r8(m, 0x0E002011));
    CHECK(gba_save_take_dirty(m), "dirty after writes");

    size_t n;
    const uint8_t *p = gba_save_data(m, &n);
    CHECK(n == 0x8000 && p[0x1234] == 0x5A, "save data image");
    static uint8_t img[0x8000];
    memset(img, 0x33, sizeof img);
    CHECK(gba_save_load(m, img, sizeof img) == 0 && r8(m, 0x0E000000) == 0x33, "load round trip");
    CHECK(gba_save_load(m, img, 0x10000) == -1, "64 KB does not fit SRAM");
    CHECK(!gba_save_take_dirty(m), "load does not dirty");
    gba_destroy(m);
}

static void flash_cmd(gba_machine *m, uint8_t cmd)
{
    w8(m, 0x0E005555, 0xAA);
    w8(m, 0x0E002AAA, 0x55);
    w8(m, 0x0E005555, cmd);
}

static void flash_program(gba_machine *m, uint32_t addr, uint8_t v)
{
    flash_cmd(m, 0xA0);
    w8(m, addr, v);
}

static void test_flash(void)
{
    for (int big = 0; big < 2; big++) {
        gba_machine *m = machine(big ? "FLASH1M_V103" : "FLASH512_V131");
        uint32_t size = big ? 0x20000 : 0x10000;
        flash_cmd(m, 0x90);
        uint8_t man = r8(m, 0x0E000000), dev = r8(m, 0x0E000001);
        CHECK(big ? (man == 0x62 && dev == 0x13) : (man == 0x32 && dev == 0x1B), "ID %02X %02X", man, dev);
        flash_cmd(m, 0xF0);
        CHECK(r8(m, 0x0E000000) == 0xFF, "ID mode left");

        w8(m, 0x0E000100, 0x12); /* without the program command: ignored */
        CHECK(r8(m, 0x0E000100) == 0xFF && !gba_save_take_dirty(m), "plain write ignored");
        flash_program(m, 0x0E000100, 0x12);
        flash_program(m, 0x0E001100, 0x34);
        flash_program(m, 0x0E00F000, 0x56);
        CHECK(r8(m, 0x0E000100) == 0x12 && r8(m, 0x0F001100) == 0x34 && gba_save_take_dirty(m), "programmed");
        CHECK(gba_read32(m, 0x0E000100, false) == 0x12121212u, "32-bit read repeats");

        /* Sector erase (4 KB) of 0x1000-0x1FFF. */
        flash_cmd(m, 0x80);
        w8(m, 0x0E005555, 0xAA);
        w8(m, 0x0E002AAA, 0x55);
        w8(m, 0x0E001234, 0x30);
        CHECK(r8(m, 0x0E001100) == 0xFF && r8(m, 0x0E000100) == 0x12, "sector erase");

        if (big) {
            flash_cmd(m, 0xB0);
            w8(m, 0x0E000000, 1);
            CHECK(r8(m, 0x0E000100) == 0xFF, "bank 1 is erased");
            flash_program(m, 0x0E000100, 0x77);
            size_t n;
            const uint8_t *p = gba_save_data(m, &n);
            CHECK(n == size && p[0x10100] == 0x77 && p[0x100] == 0x12, "bank 1 at 0x10000 in the image");
            flash_cmd(m, 0xB0);
            w8(m, 0x0E000000, 0);
            CHECK(r8(m, 0x0E000100) == 0x12, "bank 0 back");
        } else {
            flash_cmd(m, 0xB0); /* no banks on 64 KB: the next write is a plain (ignored) write */
            w8(m, 0x0E000000, 1);
            CHECK(r8(m, 0x0E000100) == 0x12, "64 KB has no bank switch");
        }

        flash_cmd(m, 0x80);
        flash_cmd(m, 0x10);
        size_t n;
        const uint8_t *p = gba_save_data(m, &n);
        int erased = 1;
        for (uint32_t i = 0; i < size; i++)
            erased &= p[i] == 0xFF;
        CHECK(erased && gba_save_take_dirty(m), "chip erase");

        static uint8_t img[0x20000];
        for (uint32_t i = 0; i < size; i++)
            img[i] = (uint8_t)(i ^ (i >> 8));
        CHECK(gba_save_load(m, img, size) == 0, "load");
        CHECK(r8(m, 0x0E00ABCD) == img[0xABCD], "loaded data visible");
        p = gba_save_data(m, &n);
        CHECK(n == size && memcmp(p, img, size) == 0, "data round trip");
        CHECK(gba_save_load(m, img, size * 2) == -1, "oversize image refused");
        gba_destroy(m);
    }
}

/* EEPROM requests through DMA3, as the libraries send them: one bit per halfword. */
static void dma3(gba_machine *m, uint32_t src, uint32_t dst, uint16_t count)
{
    gba_io_write16(m, 0x040000D4, (uint16_t)src);
    gba_io_write16(m, 0x040000D6, (uint16_t)(src >> 16));
    gba_io_write16(m, 0x040000D8, (uint16_t)dst);
    gba_io_write16(m, 0x040000DA, (uint16_t)(dst >> 16));
    gba_io_write16(m, 0x040000DC, count);
    gba_io_write16(m, 0x040000DE, 0x8000); /* enable, now, 16-bit, increment */
}

#define BITS 0x02001000u
#define REPLY 0x02002000u

static int put_bits(gba_machine *m, int at, uint64_t v, int n)
{
    for (int i = n - 1; i >= 0; i--, at++)
        wr16(&m->ewram[(BITS & 0xFFFF) + 2 * (unsigned)at], (uint16_t)((v >> i) & 1));
    return at;
}

static void eeprom_write(gba_machine *m, uint32_t eaddr, int abits, uint64_t data, uint32_t ebase)
{
    int n = put_bits(m, 0, 2, 2);
    n = put_bits(m, n, eaddr, abits);
    n = put_bits(m, n, data, 64);
    n = put_bits(m, n, 0, 1);
    dma3(m, BITS, ebase, (uint16_t)n);
}

static uint64_t eeprom_read(gba_machine *m, uint32_t eaddr, int abits, uint32_t ebase)
{
    int n = put_bits(m, 0, 3, 2);
    n = put_bits(m, n, eaddr, abits);
    n = put_bits(m, n, 0, 1);
    dma3(m, BITS, ebase, (uint16_t)n);
    dma3(m, ebase, REPLY, 68);
    uint64_t v = 0;
    for (int i = 4; i < 68; i++)
        v = v << 1 | (rd16(&m->ewram[(REPLY & 0xFFFF) + 2 * (unsigned)i]) & 1);
    return v;
}

static void test_eeprom(void)
{
    /* 512 B: 6-bit addresses. */
    gba_machine *m = machine("EEPROM_V124");
    CHECK(gba_read16(m, 0x0D000000, false) == 1, "ready");
    CHECK(r8(m, 0x0E000000) == 0xFF, "nothing at 0x0E000000");
    eeprom_write(m, 5, 6, 0x0123456789ABCDEFull, 0x0D000000);
    CHECK(gba_save_type(m) == GBA_SAVE_EEPROM512 && gba_save_take_dirty(m), "512 B write: kind %d",
          (int)gba_save_type(m));
    CHECK(gba_read16(m, 0x0D000000, false) == 1, "ready after the write");
    size_t n;
    const uint8_t *p = gba_save_data(m, &n);
    CHECK(n == 512 && p[40] == 0x01 && p[47] == 0xEF, "block 5 at bytes 40-47, first bit = bit 7 of byte 0");
    uint64_t v = eeprom_read(m, 5, 6, 0x0D000000);
    CHECK(v == 0x0123456789ABCDEFull, "read back %016llX", (unsigned long long)v);
    v = eeprom_read(m, 6, 6, 0x0DFFFF00);
    CHECK(v == ~0ull, "erased block reads all ones (and the area spans 0x0D000000-0x0DFFFFFF)");
    gba_destroy(m);

    /* 8 KB: 14-bit addresses, detected from the DMA length (17 / 81). */
    m = machine("EEPROM_V124");
    eeprom_write(m, 0x3FF, 14, 0xFEDCBA9876543210ull, 0x0D000000);
    p = gba_save_data(m, &n);
    CHECK(gba_save_type(m) == GBA_SAVE_EEPROM8K && n == 8192 && p[0x3FF * 8] == 0xFE, "8 KB: kind %d size %zu",
          (int)gba_save_type(m), n);
    v = eeprom_read(m, 0x3FF, 14, 0x0D000000);
    CHECK(v == 0xFEDCBA9876543210ull, "8 KB read back %016llX", (unsigned long long)v);
    /* Round trip through the API. */
    static uint8_t img[8192];
    for (size_t i = 0; i < sizeof img; i++)
        img[i] = (uint8_t)(i * 13);
    CHECK(gba_save_load(m, img, 1000) == -1, "odd size refused");
    CHECK(gba_save_load(m, img, sizeof img) == 0, "load 8 KB");
    v = eeprom_read(m, 2, 14, 0x0D000000);
    uint64_t want = 0;
    for (int i = 0; i < 8; i++)
        want = want << 8 | img[16 + i];
    CHECK(v == want, "loaded block 2: %016llX want %016llX", (unsigned long long)v, (unsigned long long)want);
    gba_destroy(m);

    /* A stored 8 KB image decides the width before any DMA. */
    m = machine("EEPROM_V124");
    CHECK(gba_save_load(m, img, sizeof img) == 0 && gba_save_type(m) == GBA_SAVE_EEPROM8K, "load sets 8 KB");
    CHECK(gba_save_load(m, img, 512) == 0 && gba_save_type(m) == GBA_SAVE_EEPROM512, "load sets 512 B");
    gba_destroy(m);

    /* ROM over 16 MB: the EEPROM is only at 0x0DFFFF00-0x0DFFFFFF, the rest of 0x0D is ROM. */
    m = machine_rom("EEPROM_V124", 0x1800000);
    m->rom[0x1000000] = 0x34;
    m->rom[0x1000001] = 0x12;
    CHECK(gba_read16(m, 0x0D000000, false) == 0x1234, "32 MB cart: 0x0D000000 is ROM");
    eeprom_write(m, 9, 6, 0x1122334455667788ull, 0x0DFFFF00);
    v = eeprom_read(m, 9, 6, 0x0DFFFF00);
    CHECK(v == 0x1122334455667788ull, "32 MB cart: EEPROM at 0x0DFFFF00");
    gba_destroy(m);
}

int main(void)
{
    test_detect();
    test_none_fallback();
    test_sram();
    test_flash();
    test_eeprom();
    if (failures) {
        printf("gba_save_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("gba_save_test: ok\n");
    return 0;
}
