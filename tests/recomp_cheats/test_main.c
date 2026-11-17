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
    recomp_cheat_bus b = {NULL, g_read, g_write, NULL, NULL, 0, g_rom, keys_held, NULL};
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
    recomp_cheat_bus b = {NULL, NULL, NULL, w_read32, w_write32, be, NULL, 0, NULL};
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

/* ---- GameCube (XBA-264): MEM1 big-endian words (0x80000000 + the fake RAM's offset) ---- */

static uint32_t r32(uint32_t a)
{
    a &= 0x1FFFFC;
    return (uint32_t)ram[a] << 24 | (uint32_t)ram[a + 1] << 16 | (uint32_t)ram[a + 2] << 8 | ram[a + 3];
}
static void w32(uint32_t a, uint32_t v)
{
    a &= 0x1FFFFC;
    for (int i = 0; i < 4; i++)
        ram[a + i] = (uint8_t)(v >> (24 - 8 * i));
}

static void test_gc_ar(void)
{
    memset(ram, 0, sizeof ram);
    recomp_cheat_bus b = word_bus(1);
    /* writes: 32-bit, 8-bit fill (4 bytes), 16-bit fill (3 halfwords), pointer write (8-bit at [ptr] + 0x10), add,
     * float add */
    w32(0x3000, 0x80004000u);
    w32(0x3100, 10);
    w32(0x3200, 0x40000000u); /* 2.0f */
    recomp_cheat *c = one(RECOMP_CHEAT_GC, "ar",
                          "04001000 12345678\n"
                          "00002000 000003AB\n"
                          "02002010 0002BEEF\n"
                          "40003000 000010AA\n"
                          "84003100 00000005\n"
                          "86003200 3F800000\n");
    CHECK(c && !c->error[0] && c->used == RECOMP_CF_AR);
    apply(&b, 1);
    CHECK_EQ(r32(0x1000), 0x12345678u);
    CHECK_EQ(r32(0x2000), 0xABABABABu);
    CHECK_EQ(r8(0x2004), 0);
    CHECK_EQ(r16(0x2010), 0xBEEF);
    CHECK_EQ(r16(0x2014), 0xBEEF);
    CHECK_EQ(r16(0x2016), 0);
    CHECK_EQ(r8(0x4010), 0xAA);
    CHECK_EQ(r32(0x3100), 15);
    CHECK_EQ(r32(0x3200), 0x40400000u); /* 3.0f */

    /* conditionals: equal skip 1 / not equal skip 2 / greater signed until the end-if zero code / skip all */
    memset(ram, 0, sizeof ram);
    c = one(RECOMP_CHEAT_GC, "auto",
            "0C003300 0000BEEF\n" /* if [3300] == BEEF (32-bit) */
            "04003304 00000001\n"
            "52003310 00000001\n" /* if (16-bit at 3310) != 1: type 2, sub 1, skip 2 */
            "04003314 00000002\n"
            "04003318 00000003\n"
            "A4003320 00000000\n" /* if [3320] > 0 signed: type 4, sub 2, until 00000000 40000000 */
            "0400331C 00000004\n"
            "00000000 40000000\n"
            "04003324 00000005\n"
            "F4003330 00000000\n" /* if [3330] > 0 unsigned: type 6, sub 3: skip all */
            "04003334 00000006\n");
    CHECK(c && !c->error[0] && c->used == RECOMP_CF_AR); /* a zero code: AR */
    w16(0x3310, 1);
    apply(&b, 1);
    CHECK_EQ(r32(0x3304), 0);
    CHECK_EQ(r32(0x3314), 0);
    CHECK_EQ(r32(0x3318), 0);
    CHECK_EQ(r32(0x331C), 0);
    CHECK_EQ(r32(0x3324), 5);
    CHECK_EQ(r32(0x3334), 0);
    w32(0x3300, 0xBEEF);
    w16(0x3310, 2);
    w32(0x3320, 7);
    w32(0x3330, 1);
    apply(&b, 1);
    CHECK_EQ(r32(0x3304), 1);
    CHECK_EQ(r32(0x3314), 2);
    CHECK_EQ(r32(0x3318), 3);
    CHECK_EQ(r32(0x331C), 4);
    CHECK_EQ(r32(0x3334), 6);
    w32(0x3320, 0xFFFFFFFFu); /* -1: not greater than 0, signed */
    w32(0x331C, 0);
    apply(&b, 1);
    CHECK_EQ(r32(0x331C), 0);

    /* fill and slide: 32-bit 0x10, 0x11, 0x12 every 4 bytes; the end zero code stops the rest */
    memset(ram, 0, sizeof ram);
    c = one(RECOMP_CHEAT_GC, "ar", "00000000 84005000\n00000010 01030001\n00000000 00000000\n04005100 00000001\n");
    CHECK(c && !c->error[0]);
    apply(&b, 1);
    CHECK_EQ(r32(0x5000), 0x10);
    CHECK_EQ(r32(0x5004), 0x11);
    CHECK_EQ(r32(0x5008), 0x12);
    CHECK_EQ(r32(0x500C), 0);
    CHECK_EQ(r32(0x5100), 0);

    /* master codes are ignored with a note; encrypted codes refused with a reason */
    c = one(RECOMP_CHEAT_GC, "ar", "C4123456 0000FF00\n04001000 00000001\n");
    CHECK(c && !c->error[0] && c->note[0]);
    c = one(RECOMP_CHEAT_GC, "auto", "ABCD-EFGH-12345\nWXYZ-1234-ABCDE\n");
    CHECK(c && strstr(c->error, "encrypted"));
    c = one(RECOMP_CHEAT_GC, "gs", "80001000 0001\n");
    CHECK(c && c->error[0]); /* not a GameCube format */
}

static void test_gc_gecko(void)
{
    memset(ram, 0, sizeof ram);
    recomp_cheat_bus b = word_bus(1);
    recomp_cheat *c = one(RECOMP_CHEAT_GC, "auto",
                          "00001000 000200AB\n"
                          "02001010 0001BEEF\n"
                          "04001020 DEADBEEF\n"
                          "06001030 00000005\n"
                          "48656C6C 6F000000\n"
                          "08001040 00000001\n"
                          "10020004 00000002\n");
    CHECK(c && !c->error[0] && c->used == RECOMP_CF_GECKO);
    apply(&b, 1);
    CHECK_EQ(r32(0x1000), 0xABABAB00u);
    CHECK_EQ(r32(0x1010), 0xBEEFBEEFu);
    CHECK_EQ(r32(0x1020), 0xDEADBEEFu);
    CHECK_EQ(r32(0x1030), 0x48656C6Cu);
    CHECK_EQ(r8(0x1034), 0x6F);
    CHECK_EQ(r16(0x1040), 1);
    CHECK_EQ(r16(0x1044), 3);
    CHECK_EQ(r16(0x1048), 5);
    CHECK_EQ(r16(0x104C), 0);

    /* ifs: 32-bit equal with an endif, 16-bit with a mask, an odd address applying an endif first, else */
    memset(ram, 0, sizeof ram);
    c = one(RECOMP_CHEAT_GC, "gecko",
            "20001100 00000007\n"
            "04001104 00000001\n"
            "E2000001 00000000\n"
            "04001108 00000002\n"
            "28001110 FF000012\n"
            "0400110C 00000003\n"
            "22001101 00000007\n" /* endif, then if [1100] != 7 */
            "04001114 00000004\n"
            "E2100000 00000000\n" /* else */
            "04001118 00000005\n"
            "E0000000 80008000\n"
            "2C001120 00000010\n" /* if 16-bit [1120] > 0x10 */
            "0400111C 00000006\n"
            "E0000000 80008000\n");
    CHECK(c && !c->error[0]);
    w16(0x1110, 0x3412); /* & 0x00FF = 0x12 */
    apply(&b, 1);
    CHECK_EQ(r32(0x1104), 0);
    CHECK_EQ(r32(0x1108), 2);
    CHECK_EQ(r32(0x110C), 3);
    CHECK_EQ(r32(0x1114), 4);
    CHECK_EQ(r32(0x1118), 0);
    CHECK_EQ(r32(0x111C), 0);
    memset(ram + 0x1104, 0, 0x20);
    w32(0x1100, 7);
    w16(0x1120, 0x11);
    w16(0x1110, 0x3413);
    apply(&b, 1);
    CHECK_EQ(r32(0x1104), 1);
    CHECK_EQ(r32(0x110C), 0);
    CHECK_EQ(r32(0x1114), 0);
    CHECK_EQ(r32(0x1118), 5);
    CHECK_EQ(r32(0x111C), 6);

    /* ba / po: set ba, write at ba + X; po loaded from a pointer, checked in range (DE), written at po + X */
    memset(ram, 0, sizeof ram);
    w32(0x3000, 0x80006000u);
    c = one(RECOMP_CHEAT_GC, "gecko",
            "42000000 80002000\n"
            "04000010 11111111\n"
            "E0000000 80008000\n"
            "48000000 80003000\n"
            "DE000000 80008180\n"
            "14000020 CAFEBABE\n"
            "E0000000 80008000\n"
            "48000000 80003004\n" /* po = 0: out of range, nothing written */
            "DE000000 80008180\n"
            "14000024 CAFEBABE\n"
            "E0000000 80008000\n"
            "44000000 80003008\n"); /* store ba */
    CHECK(c && !c->error[0]);
    apply(&b, 1);
    CHECK_EQ(r32(0x2010), 0x11111111u);
    CHECK_EQ(r32(0x6020), 0xCAFEBABEu);
    CHECK_EQ(r32(0x0024), 0);
    CHECK_EQ(r32(0x3008), 0x80000000u);

    /* Gecko registers, repeat, goto */
    memset(ram, 0, sizeof ram);
    c = one(RECOMP_CHEAT_GC, "gecko",
            "80000001 00000005\n" /* gr1 = 5 */
            "86000001 00000003\n" /* gr1 += 3 */
            "84200001 80001200\n" /* [1200] = gr1 */
            "82200002 80001200\n" /* gr2 = [1200] */
            "88000002 00000001\n" /* gr2 += gr1 */
            "84200002 80001204\n"
            "80000003 00000000\n"
            "60000003 00000000\n" /* repeat 3 times (block 0) */
            "86000003 00000001\n"
            "62000000 00000000\n"
            "84200003 80001208\n"
            "86500002 00000002\n" /* gr2 <<= 2 */
            "84200002 8000120C\n"
            "66000001 00000000\n" /* goto: skip one line */
            "04001300 11111111\n"
            "04001304 22222222\n"
            "80000004 80001200\n" /* gr4 = 0x80001200, gr5 = 0x80001400 */
            "80000005 80001400\n"
            "8A000845 00000000\n" /* copy 8 bytes from gr4's address to gr5's */
            "F0000000 00000000\n"
            "04001308 33333333\n");
    CHECK(c && !c->error[0]);
    apply(&b, 1);
    CHECK_EQ(r32(0x1200), 8);
    CHECK_EQ(r32(0x1204), 16);
    CHECK_EQ(r32(0x1208), 4);
    CHECK_EQ(r32(0x120C), 64);
    CHECK_EQ(r32(0x1300), 0);
    CHECK_EQ(r32(0x1304), 0x22222222u);
    CHECK_EQ(r32(0x1400), 8);
    CHECK_EQ(r32(0x1404), 16);
    CHECK_EQ(r32(0x1308), 0); /* after F0 */

    /* C2: the code in the hook area, a branch to it at the address, a branch back as its last word; turned off,
     * the instruction comes back */
    memset(ram, 0, sizeof ram);
    w32(0x4100, 0x7C0802A6u);
    c = one(RECOMP_CHEAT_GC, "gecko", "C2004100 00000002\n38600001 38800002\n38A00003 00000000\n");
    CHECK(c && !c->error[0]);
    apply(&b, 1);
    CHECK_EQ(r32(0x4100), 0x48000000u | ((0x80001800u - 0x80004100u) & 0x03FFFFFCu));
    CHECK_EQ(r32(0x1800), 0x38600001u);
    CHECK_EQ(r32(0x1804), 0x38800002u);
    CHECK_EQ(r32(0x1808), 0x38A00003u);
    CHECK_EQ(r32(0x180C), 0x48000000u | ((0x80004104u - 0x8000180Cu) & 0x03FFFFFCu));
    unsigned before = writes;
    apply(&b, 3); /* the same words: nothing written */
    CHECK_EQ(writes, before);
    S.list[0].enabled = 0;
    apply(&b, 1);
    CHECK_EQ(r32(0x4100), 0x7C0802A6u);
    /* C6 branch */
    c = one(RECOMP_CHEAT_GC, "gecko", "C6004200 80004300\n");
    CHECK(c && !c->error[0]);
    apply(&b, 1);
    CHECK_EQ(r32(0x4200), 0x48000100u);

    /* refused with a reason */
    c = one(RECOMP_CHEAT_GC, "gecko", "C0000000 00000001\n4E800020 00000000\n");
    CHECK(c && strstr(c->error, "C0"));
    c = one(RECOMP_CHEAT_GC, "gecko", "A8000000 00000001\n");
    CHECK(c && c->error[0]);
    c = one(RECOMP_CHEAT_GC, "gecko", "C2004100 00000003\n38600001 00000000\n");
    CHECK(c && c->error[0]); /* the code is missing lines */
    c = one(RECOMP_CHEAT_GC, "gecko", "04001000 0001\n");
    CHECK(c && c->error[0]); /* not 8 + 8 digits */
    /* raw lines */
    memset(ram, 0, sizeof ram);
    c = one(RECOMP_CHEAT_GC, "auto", "80001000:12\n80001002:3456\n");
    CHECK(c && !c->error[0] && c->used == RECOMP_CF_RAW);
    apply(&b, 1);
    CHECK_EQ(r32(0x1000), 0x12003456u);
}

/* ---- The file ---- */

/* ---- Xbox (XBA-241): a fake guest window, [0x10000, 0x200000) mapped, its first page a "code section" ---- */

static unsigned refused;

static int x_ok(void *u, uint32_t a, int size, int write)
{
    (void)u;
    int ok = a >= 0x10000u && (uint64_t)a + (uint64_t)size <= 0x200000u && !(write && a < 0x11000u);
    refused += !ok;
    return ok;
}

static uint32_t x_read(void *u, uint32_t a, int size)
{
    if (!x_ok(u, a, size, 0))
        return 0;
    uint32_t v = 0;
    for (int i = 0; i < size; i++)
        v |= (uint32_t)ram[a + (uint32_t)i] << (8 * i);
    return v;
}

static void x_write(void *u, uint32_t a, uint32_t v, int size)
{
    writes++;
    if (!x_ok(u, a, size, 1))
        return;
    for (int i = 0; i < size; i++)
        ram[a + (uint32_t)i] = (uint8_t)(v >> (8 * i));
}

static uint32_t x32(uint32_t a)
{
    return ram[a] | (uint32_t)ram[a + 1] << 8 | (uint32_t)ram[a + 2] << 16 | (uint32_t)ram[a + 3] << 24;
}

static void test_xbox(void)
{
    memset(ram, 0, sizeof ram);
    recomp_cheat_bus b = {NULL, x_read, x_write, NULL, NULL, 0, NULL, 0, x_ok};

    /* Typed writes of each size, little-endian; auto is the xbox format. */
    recomp_cheat *c = one(RECOMP_CHEAT_XBOX, "auto",
                          "00020000 00000063\n"
                          "10020010 000003E7\n"
                          "20020020 DEADBEEF\n");
    CHECK(c && !c->error[0]);
    CHECK_EQ(c->used, RECOMP_CF_XBOX);
    CHECK_EQ(c->nops, 3);
    apply(&b, 1);
    CHECK_EQ(x32(0x20000), 0x63);
    CHECK_EQ(x32(0x20010), 0x3E7);
    CHECK_EQ(x32(0x20020), 0xDEADBEEFu);
    unsigned w = writes;
    apply(&b, 1);
    CHECK_EQ(writes, w); /* unchanged: no write */
    ram[0x20000] = 1;
    apply(&b, 1);
    CHECK_EQ(ram[0x20000], 0x63); /* the game's change is undone at the next vblank */

    /* Raw lines: ADDRESS:VALUE (any value width) and ADDRESS VALUE with a short value; a full 32-bit address. */
    memset(ram, 0, sizeof ram);
    c = one(RECOMP_CHEAT_XBOX, "xbox", "00030000:12345678\n0x30010 7F\n00030020 1234\n00030030:00000001");
    CHECK(c && !c->error[0]);
    apply(&b, 1);
    CHECK_EQ(x32(0x30000), 0x12345678u);
    CHECK_EQ(x32(0x30010), 0x7F);
    CHECK_EQ(x32(0x30020), 0x1234);
    CHECK_EQ(x32(0x30030), 1); /* a 32-bit write: 00000001 at 0x30030 (not a typed byte write) */
    c = one(RECOMP_CHEAT_XBOX, "auto", "7FFD0000:01");
    CHECK(c && !c->error[0]); /* high addresses are raw lines; the bus decides what is mapped */

    /* Conditionals: the next line only, chains guard together, the line after the guarded one always runs. */
    memset(ram, 0, sizeof ram);
    c = one(RECOMP_CHEAT_XBOX, "auto",
            "40040000 00000005\n"   /* if byte [0x40000] == 5 */
            "00040010 00000063\n"   /*   byte [0x40010] = 0x63 */
            "00040011 00000001\n"   /* always */
            "50040002 00001234\n"   /* if 16-bit [0x40002] == 0x1234 */
            "60040004 CAFEF00D\n"   /* and 32-bit [0x40004] == 0xCAFEF00D */
            "20040020 11111111\n"   /*   32-bit [0x40020] = 0x11111111 */
            "00040030 00000002\n"); /* always */
    CHECK(c && !c->error[0]);
    CHECK_EQ(c->nops, 7);
    apply(&b, 1);
    CHECK_EQ(ram[0x40010], 0);
    CHECK_EQ(ram[0x40011], 1);
    CHECK_EQ(x32(0x40020), 0);
    CHECK_EQ(ram[0x40030], 2);
    ram[0x40000] = 5;
    ram[0x40002] = 0x34;
    ram[0x40003] = 0x12;
    apply(&b, 1);
    CHECK_EQ(ram[0x40010], 0x63);
    CHECK_EQ(x32(0x40020), 0); /* the second condition still fails */
    ram[0x40004] = 0x0D;
    ram[0x40005] = 0xF0;
    ram[0x40006] = 0xFE;
    ram[0x40007] = 0xCA;
    apply(&b, 1);
    CHECK_EQ(x32(0x40020), 0x11111111u);
    ram[0x40000] = 4;
    ram[0x40010] = 0;
    apply(&b, 1);
    CHECK_EQ(ram[0x40010], 0); /* condition false again: not written */

    /* Unmapped and code addresses: writes dropped, conditionals on them fail, nothing crashes. */
    memset(ram, 0, sizeof ram);
    refused = 0;
    c = one(RECOMP_CHEAT_XBOX, "auto",
            "00000100 00000001\n"   /* the low 64 KB */
            "00010800 00000001\n"   /* the "code section" */
            "0FFFFFF0 00000001\n"   /* past the mapped window */
            "40000100 00000000\n"   /* if byte [0x100] == 0: unreadable, fails */
            "00050000 00000001\n"
            "40010800 00000000\n"   /* code may be read: [0x10800] == 0 holds */
            "00050001 00000001\n"
            "FFFFFFF0:01020304\n"); /* the end of the 32-bit space */
    CHECK(c && !c->error[0]);
    apply(&b, 1);
    CHECK(refused >= 5);
    CHECK_EQ(ram[0x100], 0);
    CHECK_EQ(ram[0x10800], 0);
    CHECK_EQ(ram[0x50000], 0);
    CHECK_EQ(ram[0x50001], 1);

    /* Refused codes, with a reason. */
    c = one(RECOMP_CHEAT_XBOX, "auto", "30020000 00000001");
    CHECK(c && strstr(c->error, "type 3"));
    c = one(RECOMP_CHEAT_XBOX, "auto", "80020000 00000001");
    CHECK(c && strstr(c->error, "type 8"));
    c = one(RECOMP_CHEAT_XBOX, "auto", "00020000 00000100");
    CHECK(c && strstr(c->error, "does not fit"));
    c = one(RECOMP_CHEAT_XBOX, "auto", "10020000 00010000");
    CHECK(c && strstr(c->error, "does not fit"));
    c = one(RECOMP_CHEAT_XBOX, "auto", "40020000 00000001");
    CHECK(c && strstr(c->error, "missing a line"));
    c = one(RECOMP_CHEAT_XBOX, "auto", "00020000 00000001\n40020000 00000001\n50020004 00000001");
    CHECK(c && strstr(c->error, "missing a line"));
    c = one(RECOMP_CHEAT_XBOX, "gs", "80012344 03E7");
    CHECK(c && strstr(c->error, "not for this console"));
    c = one(RECOMP_CHEAT_XBOX, "auto", "hello");
    CHECK(c && c->error[0]);
    c = one(RECOMP_CHEAT_PS1, "xbox", "00020000 00000063");
    CHECK(c && strstr(c->error, "not for this console"));
    CHECK_EQ(recomp_cheat_format_parse("xbox"), RECOMP_CF_XBOX);
    CHECK(!strcmp(recomp_cheat_format_name(RECOMP_CF_XBOX), "xbox"));
}

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
    test_xbox();
    test_gc_ar();
    test_gc_gecko();
    test_file(argc > 1 ? argv[1] : ".");
    recomp_cheats_free(&S);
    if (failures) {
        printf("recomp_cheats: %d failure(s)\n", failures);
        return 1;
    }
    printf("recomp_cheats: all tests passed\n");
    return 0;
}
