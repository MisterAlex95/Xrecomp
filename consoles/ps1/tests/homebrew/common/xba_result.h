/* XBA homebrew result block, PS1 flavour (XBA-149). Our own code, MIT like the rest of the repo.
 *
 * Every test program writes its verdict into a fixed 512-byte block, then
 * spins in a halt loop (a jump to itself). A headless runtime polls `status` at each VBlank:
 * once it is XBA_PASS or XBA_FAIL the run is over. Layout and values are shared by all consoles
 * (see consoles/<name>/tests/homebrew/README.md); only the address and the endianness differ.
 *
 *   0x801F0000  magic     XBA_MAGIC, written first: "the program reached main"
 *   0x801F0004  status    0 while running, then XBA_PASS or XBA_FAIL
 *   0x801F0008  test_id   fourcc of the program (XBA_ID_*)
 *   0x801F000C  detail    0 on pass; on fail, 1 + index of the first failing check
 *   0x801F0010  checksum  program-specific (FNV-1a of the table for `cpu`)
 *   0x801F0014  count     number of valid table entries
 *   0x801F0018  frames    VBlanks counted by the program (0 if it does not count)
 *   0x801F001C  stage     progress marker, see each program's README (gfx: capture points)
 *   0x801F0020  table[120] results (each program's README lists them)
 *
 * Location: main RAM, 64 KiB below the top (KSEG0 0x801F0000 = physical 0x001F0000). The stack
 * starts at 0x801FFFF0 and stays far above it; the programs never malloc. The R3000A has no data
 * cache, so a runtime reads physical 0x001F0000 directly.
 * Words are little-endian (the CPU's native order). xba_fnv1a() hashes each word's bytes low
 * byte first on every console, so the checksum does not depend on the endianness.
 */
#ifndef XBA_RESULT_H
#define XBA_RESULT_H

#include <stdint.h>

#define XBA_RESULT_ADDR 0x801F0000u
#define XBA_MAGIC 0x58424152u /* 'XBAR' */
#define XBA_PASS 0x50415353u  /* 'PASS' */
#define XBA_FAIL 0x4641494Cu  /* 'FAIL' */
#define XBA_TABLE_MAX 120

#define XBA_FOURCC(a, b, c, d) \
    ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#define XBA_ID_HELLO XBA_FOURCC('H', 'E', 'L', 'O')
#define XBA_ID_CPU XBA_FOURCC('C', 'P', 'U', ' ')
#define XBA_ID_TIMER XBA_FOURCC('T', 'I', 'R', 'Q')
#define XBA_ID_GFX XBA_FOURCC('G', 'F', 'X', ' ')
#define XBA_ID_OVL XBA_FOURCC('O', 'V', 'L', ' ')
#define XBA_ID_MCARD XBA_FOURCC('M', 'C', 'R', 'D')

typedef struct xba_result {
    uint32_t magic;
    uint32_t status;
    uint32_t test_id;
    uint32_t detail;
    uint32_t checksum;
    uint32_t count;
    uint32_t frames;
    uint32_t stage;
    uint32_t table[XBA_TABLE_MAX];
} xba_result;

#define XBA_RESULT ((volatile xba_result *)XBA_RESULT_ADDR)

static inline void xba_begin(uint32_t test_id)
{
    volatile xba_result *r = XBA_RESULT;
    r->status = 0;
    r->test_id = test_id;
    r->detail = 0;
    r->checksum = 0;
    r->count = 0;
    r->frames = 0;
    r->stage = 0;
    for (int i = 0; i < XBA_TABLE_MAX; i++)
        r->table[i] = 0;
    r->magic = XBA_MAGIC;
}

/* Records table[idx] = got and remembers the first mismatch. */
static inline void xba_check(uint32_t idx, uint32_t got, uint32_t expected)
{
    volatile xba_result *r = XBA_RESULT;
    if (idx < XBA_TABLE_MAX)
        r->table[idx] = got;
    if (idx + 1 > r->count)
        r->count = idx + 1;
    if (got != expected && r->detail == 0)
        r->detail = idx + 1;
}

static inline uint32_t xba_fnv1a(const volatile uint32_t *w, uint32_t n)
{
    uint32_t h = 0x811C9DC5u;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v = w[i];
        for (int b = 0; b < 4; b++) {
            h ^= (v >> (8 * b)) & 0xFF;
            h *= 0x01000193u;
        }
    }
    return h;
}

/* Writes the verdict and never returns. The halt loop is a plain branch to itself. */
static inline __attribute__((noreturn)) void xba_finish(void)
{
    volatile xba_result *r = XBA_RESULT;
    r->status = r->detail ? XBA_FAIL : XBA_PASS;
    for (;;) {
    }
}

#endif
