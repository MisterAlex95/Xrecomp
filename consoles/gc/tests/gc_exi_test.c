/* gc_exi_test -- the EXI devices (gc_exi.c, XBA-264) driven through their registers the way the CARD and system
 * libraries do, no homebrew: card ID, status, vendor ID, read array (latency, page wrap), sector / chip erase, page
 * program, the card's interrupt and its clear, EXT / EXTINT on insert and removal, the three card sizes; the IPL
 * chip's RTC (fixed, then set by the game), SRAM (checksum, language, video mode, writes), the flash ID a card's
 * header gives; a save state taken in the middle of a command. Exit 0, 1 on a mismatch. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gc_exi.h"
#include "gc_internal.h"

static int failures;

#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        if (!(cond)) {                                                                                                \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                               \
            printf(__VA_ARGS__);                                                                                      \
            printf("\n");                                                                                             \
            failures++;                                                                                               \
        }                                                                                                             \
    } while (0)

#define EXI(ch, reg) (0x0C006800u + 0x14u * (ch) + 4u * (reg))
enum { RD = 0, WR = 1 };

static uint32_t csr(gc_machine *m, unsigned ch)
{
    return gc_hw_read(m, EXI(ch, 0), 4);
}

/* EXI_Select: the interrupt masks kept, the device's chip select, 16 MHz. */
static void sel(gc_machine *m, unsigned ch, unsigned dev)
{
    gc_hw_write(m, EXI(ch, 0), (csr(m, ch) & 0x405) | (0x80u << dev) | (5u << 4), 4);
}

static void desel(gc_machine *m, unsigned ch)
{
    gc_hw_write(m, EXI(ch, 0), csr(m, ch) & 0x405, 4);
}

/* EXI_Imm: up to 4 bytes from the top of DATA. */
static uint32_t imm(gc_machine *m, unsigned ch, uint32_t data, unsigned len, unsigned mode)
{
    gc_hw_write(m, EXI(ch, 4), mode == RD ? 0xFFFFFFFFu : data, 4);
    gc_hw_write(m, EXI(ch, 3), ((len - 1) << 4) | (mode << 2) | 1, 4);
    CHECK(!(gc_hw_read(m, EXI(ch, 3), 4) & 1), "TSTART still set");
    return gc_hw_read(m, EXI(ch, 4), 4);
}

static void imm_bytes(gc_machine *m, unsigned ch, const uint8_t *b, unsigned n)
{
    while (n) {
        unsigned k = n > 4 ? 4 : n;
        uint32_t v = 0;
        for (unsigned i = 0; i < k; i++)
            v |= (uint32_t)b[i] << (24 - 8 * i);
        imm(m, ch, v, k, WR);
        b += k;
        n -= k;
    }
}

static void dma(gc_machine *m, unsigned ch, uint32_t pa, uint32_t len, unsigned mode)
{
    gc_hw_write(m, EXI(ch, 1), pa, 4);
    gc_hw_write(m, EXI(ch, 2), len, 4);
    gc_hw_write(m, EXI(ch, 3), (mode << 2) | 3, 4);
}

static uint32_t card_id(gc_machine *m, unsigned ch)
{
    sel(m, ch, 0);
    imm(m, ch, 0, 2, WR);
    uint32_t id = imm(m, ch, 0, 4, RD);
    desel(m, ch);
    return id;
}

static uint8_t card_status(gc_machine *m, unsigned ch)
{
    sel(m, ch, 0);
    imm(m, ch, 0x83000000u, 2, WR);
    uint8_t st = (uint8_t)(imm(m, ch, 0, 1, RD) >> 24);
    desel(m, ch);
    return st;
}

static void card_cmd1(gc_machine *m, unsigned ch, uint8_t c)
{
    sel(m, ch, 0);
    imm(m, ch, (uint32_t)c << 24, 1, WR);
    desel(m, ch);
}

static void addr_bytes(uint8_t *cmd, uint32_t a)
{
    cmd[1] = (uint8_t)((a & 0xFE0000) >> 17);
    cmd[2] = (uint8_t)((a & 0x01FE00) >> 9);
    cmd[3] = (uint8_t)((a & 0x000180) >> 7);
    cmd[4] = (uint8_t)(a & 0x7F);
}

/* __card_readsegment: 0x52, the address, 4 latency bytes, a DMA read into MEM1 at pa. */
static void card_read(gc_machine *m, unsigned ch, uint32_t a, uint32_t pa, uint32_t len)
{
    uint8_t cmd[9] = {0x52};
    addr_bytes(cmd, a);
    sel(m, ch, 0);
    imm_bytes(m, ch, cmd, 9);
    dma(m, ch, pa, len, RD);
    desel(m, ch);
}

static void card_erase(gc_machine *m, unsigned ch, uint32_t a)
{
    uint8_t cmd[3] = {0xF1, (uint8_t)((a >> 17) & 0x7F), (uint8_t)((a >> 9) & 0xFF)};
    sel(m, ch, 0);
    imm_bytes(m, ch, cmd, 3);
    desel(m, ch);
}

static void card_program(gc_machine *m, unsigned ch, uint32_t a, uint32_t pa)
{
    uint8_t cmd[5] = {0xF2};
    addr_bytes(cmd, a);
    sel(m, ch, 0);
    imm_bytes(m, ch, cmd, 5);
    dma(m, ch, pa, 128, WR);
    desel(m, ch);
}

/* The 0x40 bytes of RTC + SRAM through the IPL chip. */
static uint32_t ipl_rtc(gc_machine *m)
{
    sel(m, 0, 1);
    imm(m, 0, 0x20000000u, 4, WR);
    uint32_t t = imm(m, 0, 0, 4, RD);
    desel(m, 0);
    return t;
}

static void ipl_sram(gc_machine *m, uint8_t *out)
{
    sel(m, 0, 1);
    imm(m, 0, 0x20000100u, 4, WR);
    dma(m, 0, 0x1000, 64, RD);
    desel(m, 0);
    memcpy(out, m->h.ram + 0x1000, 64);
}

static int sram_sum_ok(const uint8_t *s)
{
    uint16_t c = 0, inv = 0;
    for (int i = 0; i < 4; i++) {
        uint16_t w = (uint16_t)(s[0x0C + 2 * i] << 8 | s[0x0D + 2 * i]);
        c = (uint16_t)(c + w);
        inv = (uint16_t)(inv + (uint16_t)~w);
    }
    return c == (s[0] << 8 | s[1]) && inv == (s[2] << 8 | s[3]);
}

static void test_card(void)
{
    char why[100];
    gc_machine *m = gc_alloc_machine(why, sizeof why);
    CHECK(!(csr(m, 0) & 0x1000) && !(csr(m, 1) & 0x1000), "EXT set with no card");
    CHECK(card_id(m, 0) == 0, "no card: ID %08X, want 0", card_id(m, 0));
    CHECK(gc_card_insert(m, 0, 16, NULL) == 0, "insert");
    CHECK(gc_card_insert(m, 0, 15, NULL) == -1 && gc_card_insert(m, 2, 16, NULL) == -1, "bad size / slot accepted");
    CHECK(csr(m, 0) & 0x1000, "EXT clear with a card in slot A");
    CHECK(!(csr(m, 0) & 0x800), "EXTINT at power-on");
    CHECK(!(csr(m, 1) & 0x1000), "EXT on slot B");
    CHECK(card_id(m, 0) == 0x10, "ID %08X, want 00000010 (16 Mbit)", card_id(m, 0));
    CHECK(csr(m, 0) & 0x8, "no TCINT after a transfer");
    CHECK(card_status(m, 0) == 0x41, "status %02X, want 41 (unlocked, ready)", card_status(m, 0));

    sel(m, 0, 0);
    imm(m, 0, 0x85000000u, 2, WR);
    uint32_t v = imm(m, 0, 0, 2, RD);
    desel(m, 0);
    CHECK(v >> 16 == 0xC221, "vendor ID %04X, want C221", v >> 16);

    /* a blank card reads 0xFF everywhere */
    card_read(m, 0, 0x2000, 0x2000, 512);
    int ff = 0;
    for (int i = 0; i < 512; i++)
        ff += m->h.ram[0x2000 + i] == 0xFF;
    CHECK(ff == 512, "blank card: %d of 512 bytes 0xFF", ff);

    /* interrupt on, erase a sector: the interrupt comes at the deselect */
    sel(m, 0, 0);
    imm(m, 0, 0x81010000u, 2, WR);
    desel(m, 0);
    gc_hw_write(m, EXI(0, 0), 0x1 | 0x8 | 0x800, 4); /* EXIINT mask on, TCINT / EXTINT cleared */
    CHECK(!(csr(m, 0) & 0x2), "EXIINT before any operation");
    size_t n = 0;
    uint8_t *img = (uint8_t *)gc_card_data(m, 0, &n);
    CHECK(img && n == 2u << 20, "card data %zu bytes, want 2 MB", n);
    memset(img + 0x4000, 0x11, 0x4000);
    card_erase(m, 0, 0x4000);
    CHECK(csr(m, 0) & 0x2, "no EXIINT after an erase");
    CHECK(m->pi_intsr & GC_INT_EXI, "PI EXI cause not raised");
    int erased = 0, kept = 0;
    for (int i = 0; i < 0x2000; i++) {
        erased += img[0x4000 + i] == 0xFF;
        kept += img[0x6000 + i] == 0x11;
    }
    CHECK(erased == 0x2000 && kept == 0x2000, "sector erase: %d erased, %d of the next sector kept", erased, kept);
    CHECK(gc_card_writes(m, 0) == 1, "writes %llu, want 1", (unsigned long long)gc_card_writes(m, 0));
    card_cmd1(m, 0, 0x89); /* clear status, then the CSR bit */
    gc_hw_write(m, EXI(0, 0), (csr(m, 0) & 0x405) | 0x2, 4);
    CHECK(!(csr(m, 0) & 0x2) && !(m->pi_intsr & GC_INT_EXI), "EXIINT not cleared");

    /* program a page */
    for (int i = 0; i < 128; i++)
        m->h.ram[0x3000 + i] = (uint8_t)(i * 7 + 1);
    card_program(m, 0, 0x4080, 0x3000);
    CHECK(csr(m, 0) & 0x2, "no EXIINT after a program");
    int same = 0;
    for (int i = 0; i < 128; i++)
        same += img[0x4080 + i] == (uint8_t)(i * 7 + 1);
    CHECK(same == 128 && img[0x407F] == 0xFF && img[0x4100] == 0xFF, "page program: %d of 128 bytes", same);
    card_read(m, 0, 0x4000, 0x2000, 512);
    CHECK(!memcmp(m->h.ram + 0x2080, m->h.ram + 0x3000, 128) && m->h.ram[0x2000] == 0xFF, "read back differs");
    /* a read wraps within its 512-byte page */
    img[0x4000] = 0x5A;
    card_read(m, 0, 0x41F0, 0x2000, 32);
    CHECK(m->h.ram[0x200F] == 0xFF && m->h.ram[0x2010] == 0x5A, "read did not wrap within the page");
    img[0x4000] = 0xFF;
    card_read(m, 0, 0x4000 + 0x7F, 0x2000, 32);
    CHECK(m->h.ram[0x2001] == img[0x4080] && m->h.ram[0x2001] == 1, "read at an odd address: %02X",
          m->h.ram[0x2001]);

    /* sleep / wake */
    card_cmd1(m, 0, 0x88);
    CHECK(card_status(m, 0) & 0x20, "sleep bit not set");
    card_cmd1(m, 0, 0x87);
    CHECK(!(card_status(m, 0) & 0x20), "sleep bit not cleared");

    /* a save state in the middle of a page program: a fresh machine with the same card finishes it the same way */
    uint8_t cmd[5] = {0xF2};
    addr_bytes(cmd, 0x8000);
    sel(m, 0, 0);
    imm_bytes(m, 0, cmd, 5);
    imm(m, 0, 0xA1B2C3D4u, 4, WR);
    size_t ss = gc_exi_state_size();
    uint8_t *st = malloc(ss);
    CHECK(gc_exi_state_save(m, st, ss) == ss && gc_exi_state_save(m, st, ss - 1) == 0, "state save size");
    gc_machine *m2 = gc_alloc_machine(why, sizeof why);
    gc_card_insert(m2, 0, 16, img);
    CHECK(gc_exi_state_load(m2, st, ss) == 0 && gc_exi_state_load(m2, st, ss - 1) == -1, "state load");
    desel(m, 0);
    desel(m2, 0);
    const uint8_t *img2 = gc_card_data(m2, 0, NULL);
    CHECK(img[0x8000] == 0xA1 && img[0x8003] == 0xD4 && img2[0x8000] == 0xA1 && img2[0x8003] == 0xD4,
          "programmed bytes after a state load %02X %02X / %02X %02X", img[0x8000], img[0x8003], img2[0x8000],
          img2[0x8003]);
    CHECK(img2[0x8004] == 0xFF, "a short program wrote past its bytes");
    free(st);
    gc_destroy(m2);

    /* chip erase */
    sel(m, 0, 0);
    imm(m, 0, 0xF4000000u, 3, WR);
    desel(m, 0);
    size_t notff = 0;
    for (size_t i = 0; i < n; i++)
        notff += img[i] != 0xFF;
    CHECK(notff == 0, "chip erase left %zu bytes", notff);

    /* removal: EXT goes, EXTINT comes */
    gc_card_remove(m, 0);
    CHECK(!(csr(m, 0) & 0x1000) && (csr(m, 0) & 0x800), "removal: CSR %08X", csr(m, 0));
    CHECK(!gc_card_data(m, 0, &n) && n == 0, "data of an empty slot");

    /* the sizes, slot B */
    CHECK(gc_card_insert(m, 1, 4, NULL) == 0 && card_id(m, 1) == 0x04, "slot B 4 Mbit ID %08X", card_id(m, 1));
    CHECK(gc_card_insert(m, 1, 64, NULL) == 0 && card_id(m, 1) == 0x40, "64 Mbit ID %08X", card_id(m, 1));
    CHECK(gc_card_bytes(4) == 512 * 1024 && gc_card_bytes(16) == 2 * 1024 * 1024 && gc_card_bytes(64) == 8 << 20 &&
              gc_card_bytes(8) == 0,
          "card sizes");
    CHECK(gc_card_mbit_of_blocks(59) == 4 && gc_card_mbit_of_blocks(251) == 16 && gc_card_mbit_of_blocks(1019) == 64 &&
              gc_card_mbit_of_blocks(100) == 0 && gc_card_mbit_of_size(8 << 20) == 64 && gc_card_mbit_of_size(5) == 0,
          "size helpers");
    gc_destroy(m);
}

static void test_ipl(void)
{
    char why[100];
    gc_machine *m = gc_alloc_machine(why, sizeof why);
    gc_exi_set_rtc(m, 123456789);
    CHECK(ipl_rtc(m) == 123456789, "RTC %u, want 123456789", ipl_rtc(m));
    m->h.now = 3ull * GC_CPU_HZ; /* three emulated seconds later */
    CHECK(ipl_rtc(m) == 123456792, "RTC %u after 3 s", ipl_rtc(m));
    /* the game sets the clock (0xA0000000 + 4 bytes) */
    sel(m, 0, 1);
    imm(m, 0, 0xA0000000u, 4, WR);
    imm(m, 0, 5000, 4, WR);
    desel(m, 0);
    CHECK(ipl_rtc(m) == 5000 && gc_exi_rtc(m) == 5000, "RTC %u after a set, want 5000", ipl_rtc(m));
    /* the host clock: some time after 2024 */
    gc_exi_set_rtc(m, -1);
    uint32_t host = ipl_rtc(m) - 5000 + 123456792;
    CHECK(host > 24u * 365 * 86400, "host RTC %u looks wrong", host);

    uint8_t s[64];
    ipl_sram(m, s);
    CHECK(sram_sum_ok(s), "SRAM checksum");
    CHECK(s[0x12] == 0 && s[0x13] == 0x2C && s[0x3A] == 0xFF, "SRAM defaults lang %u flags %02X", s[0x12], s[0x13]);
    gc_exi_set_language(m, 2);
    gc_set_tv_mode(m, GC_TV_PAL);
    ipl_sram(m, s);
    CHECK(sram_sum_ok(s) && s[0x12] == 2 && (s[0x13] & 3) == 1, "SRAM lang %u flags %02X", s[0x12], s[0x13]);
    /* a write at an offset (libogc's __sram_write: 0xA0000100 + (offset << 6)) */
    sel(m, 0, 1);
    imm(m, 0, 0xA0000100u + (0x12u << 6), 4, WR);
    imm(m, 0, 0x05000000u, 1, WR);
    desel(m, 0);
    ipl_sram(m, s);
    CHECK(s[0x12] == 5, "SRAM write: lang %u, want 5", s[0x12]);
    /* the IPL ROM reads zeros */
    sel(m, 0, 1);
    imm(m, 0, 0x1AFF00u << 6, 4, WR);
    CHECK(imm(m, 0, 0, 4, RD) == 0, "ROM not zero");
    desel(m, 0);

    /* a formatted card's header gives the slot's flash ID (the format's serial run backwards) */
    static uint8_t img[512 * 1024];
    memset(img, 0xFF, sizeof img);
    const uint8_t flash[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 200};
    uint64_t t = 0x0123456789ABCDEFull, rnd = t;
    for (int i = 0; i < 8; i++)
        img[12 + i] = (uint8_t)(t >> (56 - 8 * i));
    for (int i = 0; i < 12; i++) { /* libogc's __card_formatregion */
        rnd = (rnd * 0x41C64E6Dull + 0x3039) >> 16;
        img[i] = (uint8_t)(flash[i] + (uint32_t)rnd);
        rnd = ((rnd * 0x41C64E6Dull + 0x3039) >> 16) & 0x7FFF;
    }
    gc_card_insert(m, 1, 4, img);
    ipl_sram(m, s);
    unsigned sum = 0;
    for (int i = 0; i < 12; i++)
        sum += s[0x14 + 12 + i];
    CHECK(!memcmp(s + 0x14 + 12, flash, 12), "slot B flash ID not derived from the header");
    CHECK(s[0x3B] == (uint8_t)~sum, "flash ID checksum %02X, want %02X", s[0x3B], (uint8_t)~sum);
    gc_destroy(m);
}

int main(void)
{
    test_card();
    test_ipl();
    if (failures) {
        printf("gc_exi_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("gc_exi_test: all passed\n");
    return 0;
}
