/* dc_naomi_test -- the Naomi mode of the Dreamcast core (XBA-235 spike), no toolchain, no BIOS, no game data.
 *
 * Builds a synthetic cartridge image here (a "NAOMI" header whose load list copies a small SH-4 program to RAM, and
 * patterned data), writes it to a temporary file, inserts it as a user's cartridge file would be and boots it the
 * way the Naomi BIOS does. The program, assembled here, probes the Naomi memory map and the cartridge board and
 * stores what it read from 0x8C001000 on:
 *   - 32 MB of main RAM: 0x0D000000 is RAM of its own (a mirror of 0x0C000000 on the Dreamcast), 0x0E000000 mirrors
 *     0x0C000000;
 *   - the 32 KB battery SRAM at 0x00200000 (erased: 0xFF), read / write;
 *   - 8 MB of sound RAM: 0x00C00000 does not alias 0x00800000;
 *   - PIO reads through ROM_OFFSETH / L and ROM_DATA, with and without auto-advance, the header's first word;
 *   - a G1 DMA from the cartridge (DMA_OFFSET, GDSTAR / GDLEN / GDDIR / GDEN / GDST) and its end interrupt bit.
 * Then checks the words and the DMA'd RAM against the synthetic data, that a Dreamcast machine still mirrors its
 * 16 MB, that a bad header is refused, and the stubbed JVS I/O board's replies. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dc_core.h"

#define BASE 0x8C020000u /* where the load list puts the program */
#define RES 0x8C001000u
#define PROG_ROM 0x1000u
#define PIO_ROM 0x2000u
#define DMA_ROM 0x3000u
#define DMA_RAM 0x0C200000u
#define DMA_LEN 64u
#define CART_SIZE 0x10000u
#define MAXW 1024

static struct {
    uint16_t w[MAXW];
    int n;
    struct {
        int at;
        uint32_t val;
    } lit[128];
    int nlit;
    int nres;
} P;

static void e(uint16_t op)
{
    P.w[P.n++] = op;
}
static void li(int n, uint32_t v) /* mov.l @(pool),Rn */
{
    P.lit[P.nlit].at = P.n;
    P.lit[P.nlit].val = v;
    P.nlit++;
    e(0xD000 | (uint16_t)(n << 8));
}
static int st(int reg) /* stores reg as the next result word (r13 = its address); returns its index */
{
    int k = P.nres++;
    li(13, RES + (uint32_t)k * 4);
    e((uint16_t)(0x2D02 | (reg << 4))); /* mov.l Rreg,@r13 */
    return k;
}
static void wr(uint32_t addr, uint32_t v) /* *(u32 *)addr = v, via r1 / r2 */
{
    li(1, addr);
    li(2, v);
    e(0x2122); /* mov.l r2,@r1 */
}
static int rd(uint32_t addr) /* r0 = *(u32 *)addr, stored */
{
    li(1, addr);
    e(0x6012); /* mov.l @r1,r0 */
    return st(0);
}

static uint16_t pattern(uint32_t off)
{
    return (uint16_t)(off * 0x9E37u + 0x1234u);
}

/* Result word indices, filled while assembling. */
static struct {
    int ram_lo, ram_hi, ram_mirror, sram_rw, sram_fresh, aram_lo, aram_hi, pio[4], pio_after, pio_fixed[2], hdr, gdst,
        istnrm, dma_first;
} R;

static void assemble(void)
{
    memset(&P, 0, sizeof P);
    e(0x4F22); /* sts.l pr,@-r15 */
    /* main RAM: 32 MB */
    wr(0x8C100000u, 0x11111111u);
    wr(0x8D100000u, 0x22222222u);
    R.ram_lo = rd(0x8C100000u);
    R.ram_hi = rd(0x8D100000u);
    R.ram_mirror = rd(0x8E100000u);
    /* SRAM */
    wr(0xA0200010u, 0x5A5AA5A5u);
    R.sram_rw = rd(0xA0200010u);
    R.sram_fresh = rd(0xA0200100u);
    /* sound RAM: 8 MB */
    wr(0xA0C00000u, 0x0BADF00Du);
    R.aram_lo = rd(0xA0800000u);
    R.aram_hi = rd(0xA0C00000u);
    /* PIO, auto-advance (bit 31 of the offset) */
    wr(0xA05F7000u, 0x8000u | (PIO_ROM >> 16));
    wr(0xA05F7004u, PIO_ROM & 0xFFFF);
    for (int i = 0; i < 4; i++)
        R.pio[i] = rd(0xA05F7008u);
    R.pio_after = rd(0xA05F7004u);
    /* PIO without auto-advance: the same word twice */
    wr(0xA05F7000u, 0);
    wr(0xA05F7004u, PIO_ROM + 0x10);
    R.pio_fixed[0] = rd(0xA05F7008u);
    R.pio_fixed[1] = rd(0xA05F7008u);
    /* the header's first word ("NA") */
    wr(0xA05F7004u, 0);
    R.hdr = rd(0xA05F7008u);
    /* G1 DMA from the cartridge */
    wr(0xA05F700Cu, 0x8000u | (DMA_ROM >> 16));
    wr(0xA05F7010u, DMA_ROM & 0xFFFF);
    wr(0xA05F7014u, 1);
    wr(0xA05F7404u, DMA_RAM);
    wr(0xA05F7408u, DMA_LEN);
    wr(0xA05F740Cu, 1);
    wr(0xA05F7414u, 1);
    wr(0xA05F7418u, 1);
    R.gdst = rd(0xA05F7418u);
    R.istnrm = rd(0xA05F6900u);
    wr(0xA05F6900u, 1u << 14); /* acknowledge */
    R.dma_first = rd(0x80000000u | DMA_RAM);
    /* back to the BIOS */
    e(0x4F26); /* lds.l @r15+,pr */
    e(0x000B); /* rts */
    e(0x0009);
    if (P.n & 1)
        e(0x0009);
    int pool = P.n;
    for (int i = 0; i < P.nlit; i++) {
        uint32_t lit_addr = BASE + (uint32_t)(pool + 2 * i) * 2, pc = BASE + (uint32_t)P.lit[i].at * 2;
        uint32_t disp = (lit_addr - ((pc & ~3u) + 4)) / 4;
        if (disp > 255) {
            fprintf(stderr, "literal out of reach\n");
            exit(2);
        }
        P.w[P.lit[i].at] |= (uint16_t)disp;
        P.w[pool + 2 * i] = (uint16_t)P.lit[i].val;
        P.w[pool + 2 * i + 1] = (uint16_t)(P.lit[i].val >> 16);
    }
    P.n = pool + 2 * P.nlit;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint8_t cart[CART_SIZE];

static void build_cart(void)
{
    memset(cart, 0, sizeof cart);
    memcpy(cart, "NAOMI           ", 16);
    memcpy(cart + 0x10, "XRECOMP SYNTHETIC TEST CART     ", 32);
    for (int r = 0; r < 8; r++)
        memcpy(cart + 0x30 + 32 * r, "NAOMI MODE PROBE                ", 32);
    memcpy(cart + 0x134, "XRT0", 4);
    put32(cart + 0x360, PROG_ROM);
    put32(cart + 0x364, BASE);
    put32(cart + 0x368, (uint32_t)P.n * 2);
    put32(cart + 0x36C, 0xFFFFFFFFu);
    put32(cart + 0x3C0, 0xFFFFFFFFu);
    put32(cart + 0x420, BASE);
    put32(cart + 0x424, BASE);
    for (int i = 0; i < P.n; i++) {
        cart[PROG_ROM + 2 * i] = (uint8_t)P.w[i];
        cart[PROG_ROM + 2 * i + 1] = (uint8_t)(P.w[i] >> 8);
    }
    for (uint32_t off = PIO_ROM; off < CART_SIZE; off += 2) {
        uint16_t v = pattern(off);
        cart[off] = (uint8_t)v;
        cart[off + 1] = (uint8_t)(v >> 8);
    }
}

static int bad;
static void check(const char *what, uint32_t got, uint32_t want)
{
    if (got != want) {
        printf("FAIL %s: %08X, want %08X\n", what, got, want);
        bad = 1;
    } else {
        printf("ok   %s: %08X\n", what, got);
    }
}
static uint32_t res(dc_machine *m, int k)
{
    return dc_peek32(m, RES + 4u * (uint32_t)k);
}

static int test_boot(const char *path)
{
    dc_machine *m = dc_create_model(DC_MODEL_NAOMI);
    char why[200];
    if (!m || dc_naomi_insert_cart(m, path, why, sizeof why) || dc_naomi_boot(m, why, sizeof why)) {
        printf("FAIL boot: %s\n", m ? why : "out of memory");
        dc_destroy(m);
        return 1;
    }
    check("model", (uint32_t)dc_get_model(m), DC_MODEL_NAOMI);
    check("entry", dc_cpu(m)->pc, BASE);
    dc_run_frames(m, 2);
    int code = -1;
    check("returned to the BIOS", dc_exited(m, &code), 1);
    check("exit code", (uint32_t)code, 0);
    check("RAM 0x0C100000", res(m, R.ram_lo), 0x11111111u);
    check("RAM 0x0D100000 (32 MB)", res(m, R.ram_hi), 0x22222222u);
    check("RAM 0x0E100000 mirrors 0x0C100000", res(m, R.ram_mirror), 0x11111111u);
    check("SRAM write / read", res(m, R.sram_rw), 0x5A5AA5A5u);
    check("SRAM erased", res(m, R.sram_fresh), 0xFFFFFFFFu);
    check("sound RAM 0x00800000 (8 MB: no alias)", res(m, R.aram_lo), 0);
    check("sound RAM 0x00C00000", res(m, R.aram_hi), 0x0BADF00Du);
    for (int i = 0; i < 4; i++) {
        char what[64];
        snprintf(what, sizeof what, "PIO ROM_DATA %d (auto-advance)", i);
        check(what, res(m, R.pio[i]), pattern(PIO_ROM + 2u * (uint32_t)i));
    }
    check("ROM_OFFSETL advanced", res(m, R.pio_after), PIO_ROM + 8);
    check("PIO without advance, 1", res(m, R.pio_fixed[0]), pattern(PIO_ROM + 0x10));
    check("PIO without advance, 2", res(m, R.pio_fixed[1]), pattern(PIO_ROM + 0x10));
    check("PIO header word", res(m, R.hdr), 'N' | ('A' << 8));
    check("GDST after DMA", res(m, R.gdst), 0);
    check("ISTNRM G1 DMA end", res(m, R.istnrm) & (1u << 14), 1u << 14);
    check("DMA first word", res(m, R.dma_first), pattern(DMA_ROM) | ((uint32_t)pattern(DMA_ROM + 2) << 16));
    int dma_bad = 0;
    for (uint32_t i = 0; i < DMA_LEN; i += 4) {
        uint32_t want = pattern(DMA_ROM + i) | ((uint32_t)pattern(DMA_ROM + i + 2) << 16);
        if (dc_peek32(m, 0x80000000u | (DMA_RAM + i)) != want)
            dma_bad = 1;
    }
    check("DMA'd block equals the cartridge's", (uint32_t)dma_bad, 0);
    check("RAM past the DMA untouched", dc_peek32(m, 0x80000000u | (DMA_RAM + DMA_LEN)), 0);
    dc_destroy(m);
    return 0;
}

static void test_dreamcast_unchanged(void)
{
    dc_machine *m = dc_create();
    if (!m) {
        bad = 1;
        return;
    }
    sh4_cpu *c = dc_cpu(m);
    check("Dreamcast model", (uint32_t)dc_get_model(m), DC_MODEL_DREAMCAST);
    sh4_w32(c, 0x8C100000u, 0x11111111u);
    sh4_w32(c, 0x8D100000u, 0x22222222u);
    check("Dreamcast: 0x0D100000 mirrors 0x0C100000", sh4_r32(c, 0x8C100000u), 0x22222222u);
    sh4_w32(c, 0xA0C00000u, 0x0BADF00Du);
    check("Dreamcast: sound RAM mirrors every 2 MB", sh4_r32(c, 0xA0800000u), 0x0BADF00Du);
    check("Dreamcast: flash at 0x00200000 reads erased", sh4_r32(c, 0xA0200010u), 0xFFFFFFFFu);
    char why[100];
    check("Dreamcast refuses a cartridge", (uint32_t)dc_naomi_insert_cart_mem(m, cart, sizeof cart, why, sizeof why),
          (uint32_t)-1);
    dc_destroy(m);
}

static void test_bad_header(void)
{
    static uint8_t junk[CART_SIZE];
    memcpy(junk, cart, sizeof junk);
    memcpy(junk, "NOT A NAOMI CART", 16);
    dc_machine *m = dc_create_model(DC_MODEL_NAOMI);
    char why[200];
    int r = m ? dc_naomi_insert_cart_mem(m, junk, sizeof junk, why, sizeof why) : -1;
    check("bad header inserted", (uint32_t)r, 0);
    check("bad header refused at boot", (uint32_t)dc_naomi_boot(m, why, sizeof why), (uint32_t)-1);
    memcpy(junk, cart, 16);
    put32(junk + 0x364, 0x80000000u); /* load into the BIOS area */
    dc_naomi_insert_cart_mem(m, junk, sizeof junk, why, sizeof why);
    check("load entry outside RAM refused", (uint32_t)dc_naomi_boot(m, why, sizeof why), (uint32_t)-1);
    dc_destroy(m);
}

static size_t jvs_packet(uint8_t *out, uint8_t node, const uint8_t *d, size_t n)
{
    out[0] = 0xE0;
    out[1] = node;
    out[2] = (uint8_t)(n + 1);
    uint8_t sum = (uint8_t)(node + n + 1);
    for (size_t i = 0; i < n; i++) {
        out[3 + i] = d[i];
        sum = (uint8_t)(sum + d[i]);
    }
    out[3 + n] = sum; /* the test's packets need no escapes */
    return n + 4;
}

static void test_jvs(void)
{
    dc_machine *m = dc_create_model(DC_MODEL_NAOMI);
    if (!m) {
        bad = 1;
        return;
    }
    dc_jvs_state *s = dc_jvs(m);
    s->test = true;
    s->coins[0] = 3;
    s->players[0] = 0x8000u | 0x0200u; /* start, push 1 */
    uint8_t req[64], rep[300];
    static const uint8_t reset[] = {0xF0, 0xD9};
    size_t n = jvs_packet(req, 0xFF, reset, sizeof reset);
    check("JVS reset: no reply", (uint32_t)dc_jvs_transfer(m, req, n, rep, sizeof rep), 0);
    static const uint8_t addr[] = {0xF1, 0x01};
    n = jvs_packet(req, 0xFF, addr, sizeof addr);
    size_t r = dc_jvs_transfer(m, req, n, rep, sizeof rep);
    check("JVS set address reply", r == 6 && rep[0] == 0xE0 && rep[1] == 0 && rep[2] == 3 && rep[3] == 1 && rep[4] == 1,
          1);
    static const uint8_t sw[] = {0x20, 0x02, 0x02, 0x21, 0x02};
    n = jvs_packet(req, 0x01, sw, sizeof sw);
    r = dc_jvs_transfer(m, req, n, rep, sizeof rep);
    /* E0 00 len 01 | 01 sys p1h p1l p2h p2l | 01 c1h c1l c2h c2l | sum */
    static const uint8_t want[] = {0xE0, 0x00, 0x0D, 0x01, 0x01, 0x80, 0x82, 0x00, 0x00, 0x00, 0x01, 0x00, 0x03, 0x00, 0x00};
    int ok = r == sizeof want + 1 && memcmp(rep, want, sizeof want) == 0;
    uint8_t sum = 0;
    for (size_t i = 1; i + 1 < r; i++)
        sum = (uint8_t)(sum + rep[i]);
    check("JVS switches + coins reply", (uint32_t)(ok && r && rep[r - 1] == sum), 1);
    req[n - 1] ^= 0x55;
    check("JVS bad checksum: no reply", (uint32_t)dc_jvs_transfer(m, req, n, rep, sizeof rep), 0);
    n = jvs_packet(req, 0x02, sw, sizeof sw);
    check("JVS other node: no reply", (uint32_t)dc_jvs_transfer(m, req, n, rep, sizeof rep), 0);
    dc_destroy(m);
}

int main(void)
{
    assemble();
    build_cart();
    const char *tmp = getenv("TMPDIR");
    char path[512];
    snprintf(path, sizeof path, "%s/dc_naomi_cart_XXXXXX", tmp && *tmp ? tmp : "/tmp");
    int fd = mkstemp(path);
    if (fd < 0 || write(fd, cart, sizeof cart) != (ssize_t)sizeof cart) {
        perror("dc_naomi_test: temporary cartridge");
        return 2;
    }
    close(fd);
    bad |= test_boot(path);
    unlink(path);
    test_dreamcast_unchanged();
    test_bad_header();
    test_jvs();
    printf("%s\n", bad ? "dc_naomi: FAIL" : "dc_naomi: ok");
    return bad ? 1 : 0;
}
