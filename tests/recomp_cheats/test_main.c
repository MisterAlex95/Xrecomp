/* recomp_cheats tests (XBA-197): see CMakeLists.txt. */
#define _POSIX_C_SOURCE 200809L
#include "recomp_cheats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int failures;

#define CHECK(c)                                                                                                     \
    do {                                                                                                             \
        if (!(c)) {                                                                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                      \
            failures++;                                                                                              \
        }                                                                                                            \
    } while (0)
#define CHECK_EQ(a, b)                                                                                               \
    do {                                                                                                             \
        unsigned long long a_ = (unsigned long long)(a), b_ = (unsigned long long)(b);                               \
        if (a_ != b_) {                                                                                              \
            printf("FAIL %s:%d: %s = 0x%llX, expected 0x%llX\n", __FILE__, __LINE__, #a, a_, b_);                    \
            failures++;                                                                                              \
        }                                                                                                            \
    } while (0)

/* ---- A fake GBA: sized little-endian accesses ---- */

static uint8_t ewram[0x40000], iwram[0x8000], io[0x400], rom[0x10000];
static unsigned writes, rom_writes;

static uint8_t *gptr(uint32_t a)
{
    switch (a >> 24) {
    case 2: return &ewram[a & 0x3FFFF];
    case 3: return &iwram[a & 0x7FFF];
    case 4: return (a & 0xFFFFFF) < 0x400 ? &io[a & 0x3FF] : NULL;
    case 8: return (a & 0xFFFFFF) < sizeof rom ? &rom[a & 0xFFFF] : NULL;
    default: return NULL;
    }
}

static uint32_t g_read(void *u, uint32_t a, int size)
{
    (void)u;
    uint32_t v = 0;
    for (int i = 0; i < size; i++) {
        uint8_t *p = gptr(a + (uint32_t)i);
        v |= (uint32_t)(p ? *p : 0) << (8 * i);
    }
    return v;
}

static void g_write(void *u, uint32_t a, uint32_t v, int size)
{
    (void)u;
    writes++;
    if ((a >> 24) == 8)
        return; /* the cartridge is only patched through rom_write16 */
    for (int i = 0; i < size; i++) {
        uint8_t *p = gptr(a + (uint32_t)i);
        if (p)
            *p = (uint8_t)(v >> (8 * i));
    }
}

static int g_rom(void *u, uint32_t a, uint16_t v)
{
    (void)u;
    rom_writes++;
    uint8_t *p = gptr(a & ~1u);
    if (!p)
        return -1;
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    return 0;
}

static uint32_t keys_held;
static recomp_cheat_bus gba_bus(void)
{
    recomp_cheat_bus b = {NULL, g_read, g_write, NULL, NULL, 0, g_rom, keys_held};
    return b;
}

static void gba_reset(void)
{
    memset(ewram, 0, sizeof ewram);
    memset(iwram, 0, sizeof iwram);
    memset(io, 0, sizeof io);
    for (size_t i = 0; i < sizeof rom; i++)
        rom[i] = (uint8_t)(i * 7);
    writes = rom_writes = 0;
    keys_held = 0;
}

/* ---- A fake PS1 (little-endian) / N64 (big-endian) RAM with word accessors only ---- */

static uint8_t ram[0x200000];
static int ram_be;

static uint32_t w_read32(void *u, uint32_t a)
{
    (void)u;
    uint32_t o = a & 0x1FFFFC;
    if (ram_be)
        return (uint32_t)ram[o] << 24 | (uint32_t)ram[o + 1] << 16 | (uint32_t)ram[o + 2] << 8 | ram[o + 3];
    return (uint32_t)ram[o] | (uint32_t)ram[o + 1] << 8 | (uint32_t)ram[o + 2] << 16 | (uint32_t)ram[o + 3] << 24;
}

static void w_write32(void *u, uint32_t a, uint32_t v)
{
    (void)u;
    writes++;
    uint32_t o = a & 0x1FFFFC;
    for (int i = 0; i < 4; i++)
        ram[o + i] = ram_be ? (uint8_t)(v >> (24 - 8 * i)) : (uint8_t)(v >> (8 * i));
}

static recomp_cheat_bus word_bus(int be)
{
    ram_be = be;
    recomp_cheat_bus b = {NULL, NULL, NULL, w_read32, w_write32, be, NULL, 0};
    return b;
}

static uint32_t r8(uint32_t a) { return ram[a & 0x1FFFFF]; }
static uint32_t r16(uint32_t a)
{
    a &= 0x1FFFFF;
    return ram_be ? (uint32_t)ram[a] << 8 | ram[a + 1] : ram[a] | (uint32_t)ram[a + 1] << 8;
}
static void w16(uint32_t a, uint32_t v)
{
    a &= 0x1FFFFF;
    ram[a] = (uint8_t)(ram_be ? v >> 8 : v);
    ram[a + 1] = (uint8_t)(ram_be ? v : v >> 8);
}

/* ---- Helpers ---- */

static recomp_cheats S;

/* One cheat, on, in a format; returns the cheat. */
static recomp_cheat *one(recomp_cheat_console con, const char *fmt, const char *code)
{
    char text[8192];
    snprintf(text, sizeof text, "# test\n[t]\nenabled=1\nformat=%s\n%s\n", fmt, code);
    recomp_cheats_free(&S);
    recomp_cheats_init(&S, con);
    recomp_cheats_parse_text(&S, text);
    if (S.n != 1)
        return NULL;
    if (S.list[0].error[0])
        printf("  (%s: %s)\n", fmt, S.list[0].error);
    return &S.list[0];
}

static void apply(const recomp_cheat_bus *b, int n)
{
    for (int i = 0; i < n; i++)
        recomp_cheats_apply(&S, b);
}

/* "XXXXXXXX YYYYYYYY" of the encrypted form of each (a, v) pair. */
static void enc(char *out, size_t cap, const uint32_t seeds[4], const uint32_t *pairs, int n)
{
    out[0] = 0;
    for (int i = 0; i < n; i++) {
        uint32_t a = pairs[2 * i], v = pairs[2 * i + 1];
        recomp_cheat_tea_encrypt(&a, &v, seeds);
        size_t l = strlen(out);
        snprintf(out + l, cap - l, "%08X %08X\n", a, v);
    }
}

/* ---- Encryption ---- */

static void test_tea(void)
{
    /* Round trips with both seed sets. */
    uint32_t x = 0x12345678u, y = 0x9ABCDEF0u;
    for (int s = 0; s < 2; s++) {
        const uint32_t *seeds = s ? recomp_cheat_seeds_arv3 : recomp_cheat_seeds_gsa;
        for (int i = 0; i < 64; i++) {
            uint32_t a = x ^ (uint32_t)i * 0x9E3779B9u, v = y + (uint32_t)i * 0x01000193u, a0 = a, v0 = v;
            recomp_cheat_tea_encrypt(&a, &v, seeds);
            CHECK(a != a0 || v != v0);
            recomp_cheat_tea_decrypt(&a, &v, seeds);
            CHECK_EQ(a, a0);
            CHECK_EQ(v, v0);
        }
    }
    /* Fixed vectors (our implementation of the published algorithm, kept so a change shows): the decrypted
     * "02000000 00000063" (32-bit write of 99 to EWRAM) under each seed set. */
    uint32_t a = 0x22000000u, v = 0x00000063u;
    recomp_cheat_tea_encrypt(&a, &v, recomp_cheat_seeds_gsa);
    uint32_t ga = a, gv = v;
    recomp_cheat_tea_decrypt(&a, &v, recomp_cheat_seeds_gsa);
    CHECK_EQ(a, 0x22000000u);
    CHECK_EQ(v, 0x63u);
    printf("  vector gsa  22000000 00000063 -> %08X %08X\n", ga, gv);
    /* The default seeds are the devices' (the DEADFACE tables with parameter 0), as the GSA / AR documentation's
     * implementations derive them. */
    CHECK_EQ(recomp_cheat_seeds_gsa[0], 0x09F4FBBDu);
    CHECK_EQ(recomp_cheat_seeds_gsa[3], 0xF3DEE5A7u);
    CHECK_EQ(recomp_cheat_seeds_arv3[0], 0x7AA9648Fu);
    CHECK_EQ(recomp_cheat_seeds_arv3[3], 0x42712C57u);
}

/* ---- GBA ---- */

static void test_gba_raw(void)
{
    gba_reset();
    recomp_cheat_bus b = gba_bus();
    recomp_cheat *c = one(RECOMP_CHEAT_GBA, "auto", "02000010:12\n02000020:3456\n0x03000004 89ABCDEF  # 32 bits");
    CHECK(c && !c->error[0]);
    CHECK_EQ(c->used, RECOMP_CF_RAW);
    apply(&b, 1);
    CHECK_EQ(g_read(NULL, 0x02000010, 4), 0x12);
    CHECK_EQ(g_read(NULL, 0x02000020, 4), 0x3456);
    CHECK_EQ(g_read(NULL, 0x03000004, 4), 0x89ABCDEFu);
    /* Only when different: a second frame writes nothing; a change by the game is undone. */
    unsigned w = writes;
    apply(&b, 1);
    CHECK_EQ(writes, w);
    ewram[0x10] = 5;
    apply(&b, 1);
    CHECK_EQ(writes, w + 1);
    CHECK_EQ(ewram[0x10], 0x12);
}

static void test_gba_gsa(void)
{
    gba_reset();
    recomp_cheat_bus b = gba_bus();
    /* Decrypted: writes, the ID code (ignored), a hook (ignored). */
    recomp_cheat *c = one(RECOMP_CHEAT_GBA, "gsa-raw",
                          "41424344 001DC0DE\n"
                          "02000001 000000AB\n"
                          "12000002 0000CDEF\n"
                          "23000000 01020304\n"
                          "F8000100 00000001\n");
    CHECK(c && !c->error[0]);
    CHECK(strstr(c->note, "ID") != NULL);
    apply(&b, 1);
    CHECK_EQ(ewram[1], 0xAB);
    CHECK_EQ(g_read(NULL, 0x02000002, 2), 0xCDEF);
    CHECK_EQ(g_read(NULL, 0x03000000, 4), 0x01020304u);

    /* D: 16-bit if equal for the next code; E: for the next zz codes. */
    gba_reset();
    c = one(RECOMP_CHEAT_GBA, "gsa-raw",
            "D2000100 00000007\n"
            "02000200 00000011\n"
            "02000201 00000022\n"
            "E0020009 02000102\n"
            "02000202 00000033\n"
            "02000203 00000044\n"
            "02000204 00000055\n");
    CHECK(c && !c->error[0]);
    apply(&b, 1);
    CHECK_EQ(ewram[0x200], 0);
    CHECK_EQ(ewram[0x201], 0x22);
    CHECK_EQ(ewram[0x202], 0);
    CHECK_EQ(ewram[0x203], 0);
    CHECK_EQ(ewram[0x204], 0x55);
    ewram[0x100] = 7;
    ewram[0x102] = 9;
    apply(&b, 1);
    CHECK_EQ(ewram[0x200], 0x11);
    CHECK_EQ(ewram[0x202], 0x33);
    CHECK_EQ(ewram[0x203], 0x44);

    /* Group write: 3000cccc value, then the addresses. */
    gba_reset();
    c = one(RECOMP_CHEAT_GBA, "gsa-raw", "30000003 CAFEBABE\n02000010 02000020\n03000040 00000000\n");
    CHECK(c && !c->error[0]);
    apply(&b, 1);
    CHECK_EQ(g_read(NULL, 0x02000010, 4), 0xCAFEBABEu);
    CHECK_EQ(g_read(NULL, 0x02000020, 4), 0xCAFEBABEu);
    CHECK_EQ(g_read(NULL, 0x03000040, 4), 0xCAFEBABEu);

    /* ROM patch: 6aaaaaaa 0000yyyy at 0x08000000 + 2 * a; put back when the cheat goes off. */
    gba_reset();
    uint16_t before = (uint16_t)g_read(NULL, 0x08000246, 2);
    c = one(RECOMP_CHEAT_GBA, "gsa-raw", "60000123 00004770\n");
    CHECK(c && !c->error[0]);
    CHECK_EQ(recomp_cheats_apply(&S, &b), 1);
    CHECK_EQ(g_read(NULL, 0x08000246, 2), 0x4770);
    CHECK_EQ(recomp_cheats_apply(&S, &b), 1);
    CHECK_EQ(rom_writes, 1);
    S.list[0].enabled = 0;
    CHECK_EQ(recomp_cheats_apply(&S, &b), 0);
    CHECK_EQ(g_read(NULL, 0x08000246, 2), before);
    S.list[0].enabled = 1;
    recomp_cheats_apply(&S, &b);
    recomp_cheats_restore(&S, &b);
    CHECK_EQ(g_read(NULL, 0x08000246, 2), before);

    /* Button writes: once when turned on. */
    gba_reset();
    c = one(RECOMP_CHEAT_GBA, "gsa-raw", "82100300 00000077\n");
    CHECK(c && !c->error[0]);
    apply(&b, 1);
    CHECK_EQ(ewram[0x300], 0x77);
    ewram[0x300] = 1;
    apply(&b, 3);
    CHECK_EQ(ewram[0x300], 1);

    /* Refused: DEADFACE. */
    c = one(RECOMP_CHEAT_GBA, "gsa-raw", "DEADFACE 00001234\n");
    CHECK(c && c->error[0]);

    /* Encrypted: the same codes through the v1 seeds, read as gsa and detected by auto. */
    uint32_t pairs[] = {0x02000001u, 0xABu, 0x12000002u, 0xCDEFu, 0xD2000100u, 0x7u, 0x02000200u, 0x11u};
    char code[512];
    enc(code, sizeof code, recomp_cheat_seeds_gsa, pairs, 4);
    for (int k = 0; k < 2; k++) {
        gba_reset();
        ewram[0x100] = 7;
        c = one(RECOMP_CHEAT_GBA, k ? "auto" : "gsa", code);
        CHECK(c && !c->error[0]);
        CHECK_EQ(c->used, RECOMP_CF_GSA);
        apply(&b, 1);
        CHECK_EQ(ewram[1], 0xAB);
        CHECK_EQ(g_read(NULL, 0x02000002, 2), 0xCDEF);
        CHECK_EQ(ewram[0x200], 0x11);
    }
}

static void test_gba_arv3(void)
{
    recomp_cheat_bus b = gba_bus();
    /* Writes and fills (00 / 02 / 04: 8 / 16 / 32-bit; the bytes above an 8 / 16-bit value repeat it). */
    gba_reset();
    recomp_cheat *c = one(RECOMP_CHEAT_GBA, "arv3-raw",
                          "00300000 00000078\n02300002 00005678\n04300004 12345678\n"
                          "00200010 00000311\n02200020 00022222\n");
    CHECK(c && !c->error[0]);
    apply(&b, 1);
    CHECK_EQ(iwram[0], 0x78);
    CHECK_EQ(g_read(NULL, 0x03000002, 2), 0x5678);
    CHECK_EQ(g_read(NULL, 0x03000004, 4), 0x12345678u);
    CHECK_EQ(g_read(NULL, 0x02000010, 4), 0x11111111u);
    CHECK_EQ(g_read(NULL, 0x02000014, 4), 0);
    CHECK_EQ(g_read(NULL, 0x02000020, 4), 0x22222222u);
    CHECK_EQ(g_read(NULL, 0x02000024, 2), 0x2222);
    CHECK_EQ(g_read(NULL, 0x02000026, 2), 0);

    /* Slides: 00000000 8x then value, vinc << 24 | count << 16 | step. */
    gba_reset();
    c = one(RECOMP_CHEAT_GBA, "arv3-raw", "00000000 80300000\n00000001 01020002\n"
                                          "00000000 82300100\n00000010 02030001\n");
    CHECK(c && !c->error[0]);
    apply(&b, 1);
    CHECK_EQ(iwram[0], 1);
    CHECK_EQ(iwram[1], 0);
    CHECK_EQ(iwram[2], 2);
    CHECK_EQ(iwram[3], 0);
    CHECK_EQ(g_read(NULL, 0x03000100, 2), 0x10);
    CHECK_EQ(g_read(NULL, 0x03000102, 2), 0x12);
    CHECK_EQ(g_read(NULL, 0x03000104, 2), 0x14);

    /* If for 1 code (08), 2 codes (48). */
    gba_reset();
    c = one(RECOMP_CHEAT_GBA, "arv3-raw",
            "08300000 00000001\n00300001 00000011\n00300002 00000021\n"
            "48300000 00000002\n00300003 00000031\n00300004 00000041\n00300005 00000051\n");
    CHECK(c && !c->error[0]);
    apply(&b, 1);
    CHECK_EQ(iwram[1], 0);
    CHECK_EQ(iwram[2], 0x21);
    CHECK_EQ(iwram[3], 0);
    CHECK_EQ(iwram[4], 0);
    CHECK_EQ(iwram[5], 0x51);
    iwram[0] = 1;
    apply(&b, 1);
    CHECK_EQ(iwram[1], 0x11);
    CHECK_EQ(iwram[3], 0);
    iwram[0] = 2;
    apply(&b, 1);
    CHECK_EQ(iwram[3], 0x31);
    CHECK_EQ(iwram[4], 0x41);

    /* Blocks: 88 if-block, else (00000000 60000000), endif (00000000 40000000), nested. */
    gba_reset();
    c = one(RECOMP_CHEAT_GBA, "arv3-raw",
            "88300000 00000001\n"
            "00300001 00000012\n"
            "88300010 00000005\n"
            "00300002 00000013\n"
            "00000000 40000000\n"
            "00000000 60000000\n"
            "00300001 00000022\n"
            "00000000 40000000\n"
            "00300003 00000099\n");
    CHECK(c && !c->error[0]);
    apply(&b, 1);
    CHECK_EQ(iwram[1], 0x22);
    CHECK_EQ(iwram[2], 0);
    CHECK_EQ(iwram[3], 0x99);
    iwram[0] = 1;
    apply(&b, 1);
    CHECK_EQ(iwram[1], 0x12);
    CHECK_EQ(iwram[2], 0);
    iwram[0x10] = 5;
    apply(&b, 1);
    CHECK_EQ(iwram[2], 0x13);

    /* Never (8E: always false) with an else; signed / unsigned compares; AND. */
    gba_reset();
    c = one(RECOMP_CHEAT_GBA, "arv3-raw",
            "8E300000 00000000\n00300001 00000012\n00000000 60000000\n00300002 00000022\n00000000 40000000\n"
            "18300010 00000005\n00300011 00000001\n"   /* 8-bit signed < 5 */
            "28300010 00000005\n00300012 00000001\n"   /* 8-bit unsigned < 5 */
            "38300010 00000080\n00300013 00000001\n"); /* AND 0x80 */
    CHECK(c && !c->error[0]);
    iwram[0x10] = 0xF0; /* -16 signed, 240 unsigned */
    apply(&b, 1);
    CHECK_EQ(iwram[1], 0);
    CHECK_EQ(iwram[2], 0x22);
    CHECK_EQ(iwram[0x11], 1);
    CHECK_EQ(iwram[0x12], 0);
    CHECK_EQ(iwram[0x13], 1);

    /* Pointer (40 / 42 / 44) and add (80 / 82 / 84); I/O register write C6. */
    gba_reset();
    g_write(NULL, 0x03000100, 0x02000400, 4);
    g_write(NULL, 0x03000104, 0x02000500, 4);
    c = one(RECOMP_CHEAT_GBA, "arv3-raw",
            "40300100 00000377\n44300104 AABBCCDD\n80300200 00000003\n82300202 00000100\nC6000050 00003F41\n"
            "C6000006 00000001\n");
    CHECK(c && !c->error[0]);
    apply(&b, 2);
    CHECK_EQ(ewram[0x403], 0x77);
    CHECK_EQ(g_read(NULL, 0x02000500, 4), 0xAABBCCDDu);
    CHECK_EQ(iwram[0x200], 6);
    CHECK_EQ(g_read(NULL, 0x03000202, 2), 0x200);
    CHECK_EQ(g_read(NULL, 0x04000050, 2), 0x3F41);
    CHECK_EQ(g_read(NULL, 0x04000006, 2), 0); /* VCOUNT: ignored */

    /* ROM patch (00000000 18...) and button write (00000000 10...): once. */
    gba_reset();
    c = one(RECOMP_CHEAT_GBA, "arv3-raw",
            "00000000 18000100\n00004770 00000000\n00000000 10300040\n00000055 00000000\n");
    CHECK(c && !c->error[0]);
    CHECK_EQ(recomp_cheats_apply(&S, &b), 1);
    CHECK_EQ(g_read(NULL, 0x08000200, 2), 0x4770);
    CHECK_EQ(iwram[0x40], 0x55);
    iwram[0x40] = 0;
    apply(&b, 1);
    CHECK_EQ(iwram[0x40], 0);

    /* Hook and ID codes ignored, end of list. */
    c = one(RECOMP_CHEAT_GBA, "arv3-raw",
            "C4123456 00000000\n00300000 00000001\n00000000 00000000\n00300001 00000001\n");
    CHECK(c && !c->error[0]);
    CHECK_EQ(c->nops, 1);
    CHECK(c->note[0] != 0);

    /* Encrypted, read as arv3 and by auto. */
    uint32_t pairs[] = {0x00300000u, 0x78u, 0x08300010u, 0x1u, 0x02300002u, 0x5678u};
    char code[512];
    enc(code, sizeof code, recomp_cheat_seeds_arv3, pairs, 3);
    for (int k = 0; k < 2; k++) {
        gba_reset();
        iwram[0x10] = 1;
        c = one(RECOMP_CHEAT_GBA, k ? "auto" : "arv3", code);
        CHECK(c && !c->error[0]);
        CHECK_EQ(c->used, RECOMP_CF_ARV3);
        apply(&b, 1);
        CHECK_EQ(iwram[0], 0x78);
        CHECK_EQ(g_read(NULL, 0x03000002, 2), 0x5678);
    }
    /* Decrypted AR v3 codes are found by auto too. */
    gba_reset();
    c = one(RECOMP_CHEAT_GBA, "auto", "00300000 00000078\n02300002 00005678\n");
    CHECK(c && !c->error[0]);
    CHECK(c->used == RECOMP_CF_ARV3_RAW || c->used == RECOMP_CF_GSA_RAW);
    apply(&b, 1);
    CHECK_EQ(iwram[0], 0x78);
}

static void test_gba_cb(void)
{
    recomp_cheat_bus b = gba_bus();
    gba_reset();
    recomp_cheat *c = one(RECOMP_CHEAT_GBA, "auto",
                          "0000A1B2 000A\n"           /* CRC master: ignored */
                          "32000010 0042\n"           /* 8-bit */
                          "82000020 BEEF\n"           /* 16-bit */
                          "22000030 00F0\n"           /* OR */
                          "62000032 0F0F\n"           /* AND */
                          "E2000034 FFFF\n"           /* add -1 */
                          "72000040 0001\n"           /* if == next */
                          "32000041 0011\n"
                          "A2000040 0001\n"           /* if != next */
                          "32000042 0022\n");
    CHECK(c && !c->error[0]);
    CHECK_EQ(c->used, RECOMP_CF_CB);
    ewram[0x30] = 0x0F;
    w16(0, 0);
    g_write(NULL, 0x02000032, 0xFFFF, 2);
    g_write(NULL, 0x02000034, 10, 2);
    apply(&b, 1);
    CHECK_EQ(ewram[0x10], 0x42);
    CHECK_EQ(g_read(NULL, 0x02000020, 2), 0xBEEF);
    CHECK_EQ(ewram[0x30], 0xFF);
    CHECK_EQ(g_read(NULL, 0x02000032, 2), 0x0F0F);
    CHECK_EQ(g_read(NULL, 0x02000034, 2), 9);
    CHECK_EQ(ewram[0x41], 0);
    CHECK_EQ(ewram[0x42], 0x22);

    /* Slide (4 + XXXXCCCC IIII), super code (5, bytes in the lines after), keys (D). */
    gba_reset();
    c = one(RECOMP_CHEAT_GBA, "cb",
            "42000100 0010\n00010003 0002\n"
            "52000200 0004\n11223344 5566\n77880000 0000\n"
            "D0000020 0001\n32000300 0001\n"
            "D0000010 0003\n32000301 0001\n");
    CHECK(c && !c->error[0]);
    apply(&b, 1);
    CHECK_EQ(g_read(NULL, 0x02000100, 2), 0x10);
    CHECK_EQ(g_read(NULL, 0x02000102, 2), 0x11);
    CHECK_EQ(g_read(NULL, 0x02000104, 2), 0x12);
    CHECK_EQ(g_read(NULL, 0x02000106, 2), 0);
    CHECK_EQ(g_read(NULL, 0x02000200, 4), 0x44332211u);
    CHECK_EQ(g_read(NULL, 0x02000204, 4), 0x88776655u);
    CHECK_EQ(ewram[0x300], 1); /* A not held */
    CHECK_EQ(ewram[0x301], 0);
    keys_held = 3;
    b = gba_bus();
    ewram[0x300] = 0;
    apply(&b, 1);
    CHECK_EQ(ewram[0x300], 0);
    CHECK_EQ(ewram[0x301], 1);

    /* Encrypted CodeBreaker: refused. */
    c = one(RECOMP_CHEAT_GBA, "cb", "9A1B2C3D 4E5F\n32000010 0042\n");
    CHECK(c && c->error[0]);
}

/* ---- PS1 ---- */

static void test_ps1(void)
{
    memset(ram, 0, sizeof ram);
    recomp_cheat_bus b = word_bus(0);
    recomp_cheat *c = one(RECOMP_CHEAT_PS1, "auto",
                          "80012344 03E7\n"
                          "30012347 0063\n"
                          "10012348 0002\n"
                          "21012350 0001\n"
                          "D0012360 0005\n"
                          "80012362 1111\n"
                          "E1012364 0000\n"
                          "30012366 0042\n");
    CHECK(c && !c->error[0]);
    CHECK_EQ(c->used, RECOMP_CF_GS);
    ram[0x12350] = 10;
    apply(&b, 2);
    CHECK_EQ(r16(0x12344), 999);
    CHECK_EQ(r8(0x12347), 0x63);
    CHECK_EQ(r8(0x12346), 0); /* the byte beside it untouched */
    CHECK_EQ(r16(0x12348), 4);
    CHECK_EQ(r8(0x12350), 8);
    CHECK_EQ(r16(0x12362), 0);
    CHECK_EQ(r8(0x12366), 0);
    w16(0x12360, 5);
    ram[0x12364] = 1;
    apply(&b, 1);
    CHECK_EQ(r16(0x12362), 0x1111);
    CHECK_EQ(r8(0x12366), 0x42);

    /* Serial 50, C0 activator, C2 copy, C1 delay. */
    memset(ram, 0, sizeof ram);
    c = one(RECOMP_CHEAT_PS1, "gs",
            "50000402 0010\n80020000 0100\n"
            "C2020000 0004\n80030000 0000\n"
            "C0040000 0001\n30040002 0077\n");
    CHECK(c && !c->error[0]);
    apply(&b, 1);
    CHECK_EQ(r16(0x20000), 0x100);
    CHECK_EQ(r16(0x20002), 0x110);
    CHECK_EQ(r16(0x20006), 0x130);
    CHECK_EQ(r16(0x20008), 0);
    CHECK_EQ(r16(0x30000), 0x100);
    CHECK_EQ(r16(0x30002), 0x110);
    CHECK_EQ(r8(0x40002), 0);
    w16(0x40000, 1);
    apply(&b, 1);
    CHECK_EQ(r8(0x40002), 0x77);

    memset(ram, 0, sizeof ram);
    c = one(RECOMP_CHEAT_PS1, "gs", "C1000000 0009\n30050000 0001\n");
    CHECK(c && !c->error[0]);
    apply(&b, 3);
    CHECK_EQ(r8(0x50000), 0);
    apply(&b, 1);
    CHECK_EQ(r8(0x50000), 1);

    c = one(RECOMP_CHEAT_PS1, "gs", "D4000000 0010\n");
    CHECK(c && c->error[0]);
    c = one(RECOMP_CHEAT_PS1, "gsa", "80012344 03E7\n");
    CHECK(c && c->error[0]); /* a GBA format */
}

/* ---- N64 ---- */

static void test_n64(void)
{
    memset(ram, 0, sizeof ram);
    recomp_cheat_bus b = word_bus(1);
    recomp_cheat *c = one(RECOMP_CHEAT_N64, "auto",
                          "8033B21D 0064\n"
                          "8133B220 0102\n"
                          "A033B223 0007\n"
                          "D033B230 0001\n"
                          "8133B232 BEEF\n"
                          "D333B234 0000\n"
                          "8033B236 0042\n"
                          "EE000000 0000\n");
    CHECK(c && !c->error[0]);
    CHECK(c->note[0] != 0);
    apply(&b, 1);
    CHECK_EQ(r8(0x33B21D), 0x64);
    CHECK_EQ(r8(0x33B21C), 0);
    CHECK_EQ(r16(0x33B220), 0x0102);
    CHECK_EQ(ram[0x33B220 & 0x1FFFFF], 0x01); /* big-endian */
    CHECK_EQ(r8(0x33B223), 7);
    CHECK_EQ(r16(0x33B232), 0);
    CHECK_EQ(r8(0x33B236), 0);
    ram[0x33B230 & 0x1FFFFF] = 1;
    w16(0x33B234, 3);
    apply(&b, 1);
    CHECK_EQ(r16(0x33B232), 0xBEEF);
    CHECK_EQ(r8(0x33B236), 0x42);

    /* Once: 88 / 89 (the button) and F0 / F1 (boot); serial 50 with a negative step. */
    memset(ram, 0, sizeof ram);
    c = one(RECOMP_CHEAT_N64, "gs", "88100000 0011\nF1100002 2222\n50000302 FFFF\n81100010 0005\n");
    CHECK(c && !c->error[0]);
    apply(&b, 1);
    CHECK_EQ(r8(0x100000), 0x11);
    CHECK_EQ(r16(0x100002), 0x2222);
    CHECK_EQ(r16(0x100010), 5);
    CHECK_EQ(r16(0x100012), 4);
    CHECK_EQ(r16(0x100014), 3);
    ram[0x100000 & 0x1FFFFF] = 0;
    apply(&b, 2);
    CHECK_EQ(r8(0x100000), 0);
    /* Turned off and on again: once more. */
    S.list[0].enabled = 0;
    apply(&b, 1);
    S.list[0].enabled = 1;
    apply(&b, 1);
    CHECK_EQ(r8(0x100000), 0x11);

    c = one(RECOMP_CHEAT_N64, "gs", "DE000400 0000\n");
    CHECK(c && c->error[0]);
}

/* ---- The file ---- */

static void test_file(const char *dir)
{
    const char *text = "# cheats\n"
                       "[Infinite lives]\nenabled=1\n8003A2C4 0063\n\n"
                       "[Off one]\nenabled=0\nformat=gs\n8103A2C6 0001\n"
                       "[Bad format]\nenabled=1\nformat=nope\n8003A2C4 0063\n"
                       "[Bad code]\nenabled=on\nnot a code\n"
                       "[Name [with] brackets]\nenabled=true\n8003A2C8 0001  # trailing comment\n";
    recomp_cheats_free(&S);
    recomp_cheats_init(&S, RECOMP_CHEAT_N64);
    CHECK_EQ(recomp_cheats_parse_text(&S, text), 5);
    CHECK(!strcmp(S.list[0].name, "Infinite lives"));
    CHECK(S.list[0].enabled && !S.list[0].error[0]);
    CHECK(!S.list[1].enabled && S.list[1].format == RECOMP_CF_GS);
    CHECK(S.list[2].error[0] != 0);
    CHECK(S.list[3].enabled && S.list[3].error[0] != 0);
    CHECK(!strcmp(S.list[4].name, "Name [with] brackets"));
    CHECK_EQ(recomp_cheats_active(&S), 2);

    /* The file and its reload: a cheat turned on is fresh, one that stayed on is not (once-codes not repeated). */
    char path[1024];
    snprintf(path, sizeof path, "%s/test.cheats", dir);
    unlink(path);
    recomp_cheats_free(&S);
    recomp_cheats_init(&S, RECOMP_CHEAT_N64);
    CHECK_EQ(recomp_cheats_load(&S, path), 0); /* missing: none */
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    if (!f)
        return;
    fputs("[A]\nenabled=1\n88100000 0011\n[B]\nenabled=0\n88100001 0022\n", f);
    fclose(f);
    CHECK_EQ(recomp_cheats_poll(&S), 1);
    CHECK_EQ(S.n, 2);
    memset(ram, 0, sizeof ram);
    recomp_cheat_bus b = word_bus(1);
    apply(&b, 1);
    CHECK_EQ(r8(0x100000), 0x11);
    CHECK_EQ(recomp_cheats_poll(&S), 0); /* unchanged */
    struct timespec ts = {0, 20 * 1000 * 1000};
    nanosleep(&ts, NULL);
    f = fopen(path, "w");
    fputs("[A]\nenabled=1\n88100000 0011\n[B]\nenabled=1\n88100001 0022\n", f);
    fclose(f);
    CHECK_EQ(recomp_cheats_poll(&S), 1);
    ram[0x100000] = 0;
    apply(&b, 1);
    CHECK_EQ(r8(0x100000), 0);    /* A stayed on: done */
    CHECK_EQ(r8(0x100001), 0x22); /* B turned on: once now */
    unlink(path);
    CHECK_EQ(recomp_cheats_poll(&S), 1); /* removed: none */
    CHECK_EQ(S.n, 0);

    /* Format names. */
    for (int i = 0; i < RECOMP_CF_COUNT; i++)
        CHECK_EQ(recomp_cheat_format_parse(recomp_cheat_format_name((recomp_cheat_format)i)), i);
    CHECK_EQ(recomp_cheat_format_parse("x"), -1);
}

int main(int argc, char **argv)
{
    recomp_cheats_init(&S, RECOMP_CHEAT_GBA);
    test_tea();
    test_gba_raw();
    test_gba_gsa();
    test_gba_arv3();
    test_gba_cb();
    test_ps1();
    test_n64();
    test_file(argc > 1 ? argv[1] : ".");
    recomp_cheats_free(&S);
    if (failures) {
        printf("recomp_cheats: %d failure(s)\n", failures);
        return 1;
    }
    printf("recomp_cheats: all tests passed\n");
    return 0;
}
