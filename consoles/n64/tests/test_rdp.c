/* N64 RDP tests (XBA-173): synthetic command lists, one per feature, with the expected pixels worked out by hand from
 * the hardware rules n64_rdp.c documents (n64brew wiki RDP pages, libdragon rdpq docs); DPC register behaviour; a
 * replica of the homebrew gfx.z64 frame; golden hashes of busy scenes and of a matrix of random command lists over
 * every mode (regression guards: update them only for a deliberate change) and the same scenes drawn with 1 and 3
 * threads, which must match bit for bit. */
#include "rdp_cmd.h"

#include <stdio.h>

#ifndef GOLDEN_BUSY
#define GOLDEN_BUSY 0x1A70E70Fu
#endif
#ifndef GOLDEN_GFX
#define GOLDEN_GFX 0x370204FDu
#endif
#ifndef GOLDEN_MATRIX
#define GOLDEN_MATRIX 0x1DF30C72u
#endif

static int failures;
static const char *cur;
static rdp_rig G;
static rdp_list LS;

static void fresh(const char *name)
{
    cur = name;
    if (G.dp)
        rig_close(&G);
    rig_open(&G);
    LS.n = 0;
}
static void go(void)
{
    rig_run(&G, &LS);
    LS.n = 0;
}
static void check(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s: %s\n", cur, what);
        failures++;
    }
}
static void expect(const char *what, uint32_t got, uint32_t want)
{
    if (got != want) {
        fprintf(stderr, "FAIL %s: %s = 0x%08X (%u), want 0x%08X (%u)\n", cur, what, got, got, want, want);
        failures++;
    }
}
static void expect_px16(int w, int x, int y, unsigned want)
{
    char b[64];
    snprintf(b, sizeof b, "px16(%d,%d)", x, y);
    expect(b, fb16(&G, w, x, y), want);
}
/* RGB of a 32-bit pixel (alpha holds coverage). */
static void expect_rgb(int w, int x, int y, int r, int g, int b)
{
    char s[64];
    snprintf(s, sizeof s, "rgb(%d,%d)", x, y);
    expect(s, fb32(&G, w, x, y) >> 8, (uint32_t)(r << 16 | g << 8 | b));
}
static int count16(int w, int x0, int y0, int x1, int y1)
{
    int n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            n += fb16(&G, w, x, y) != 0;
    return n;
}

static void frame16(int w, int h)
{
    L(&LS, c_color_image(F_RGBA, SZ16, w, FB_ADDR));
    L(&LS, c_scissor(0, 0, w * 4, h * 4));
}
static void frame32(int w, int h)
{
    L(&LS, c_color_image(F_RGBA, SZ32, w, FB_ADDR));
    L(&LS, c_scissor(0, 0, w * 4, h * 4));
}

/* ---- DPC registers, fetch ------------------------------------------------------------------------------------------ */
static void t_regs(void)
{
    fresh("registers");
    expect("STATUS after reset", n64_rdp_read_reg(G.dp, 3), 0x80);
    n64_rdp_write_reg(G.dp, 0, 0x1003);
    expect("START", n64_rdp_read_reg(G.dp, 0), 0x1000);
    check(n64_rdp_read_reg(G.dp, 3) & 0x400, "start pending");
    n64_rdp_write_reg(G.dp, 0, 0x2000); /* ignored while pending */
    expect("START kept", n64_rdp_read_reg(G.dp, 0), 0x1000);
    n64_rdp_write_reg(G.dp, 1, 0x1000);
    expect("CURRENT", n64_rdp_read_reg(G.dp, 2), 0x1000);
    check(!(n64_rdp_read_reg(G.dp, 3) & 0x400), "start pending cleared");

    /* freeze holds the list back */
    frame16(64, 64);
    L(&LS, c_modes(OM_FILL));
    L(&LS, c_fill_color(0xFFFFFFFF));
    L(&LS, c_fill_rect(0, 0, 7 * 4, 7 * 4));
    L(&LS, c_sync_full());
    n64_rdp_write_reg(G.dp, 3, 0x8);
    check(n64_rdp_read_reg(G.dp, 3) & 0x2, "freeze set");
    go();
    expect("frozen: nothing drawn", count16(64, 0, 0, 8, 8), 0);
    check(n64_rdp_read_reg(G.dp, 3) & 0x100, "dma busy while frozen");
    expect("frozen: no interrupt", (uint32_t)G.irqs, 0);
    n64_rdp_write_reg(G.dp, 3, 0x4);
    n64_rdp_run(G.dp);
    expect("thawed: drawn", count16(64, 0, 0, 8, 8), 64);
    expect("SYNC_FULL interrupt", (uint32_t)G.irqs, 1);
    expect("CURRENT at END", n64_rdp_read_reg(G.dp, 2), n64_rdp_read_reg(G.dp, 1));
    check(!(n64_rdp_read_reg(G.dp, 3) & 0x160), "idle after SYNC_FULL (pipe, cmd, dma busy clear)");
    check(n64_rdp_read_reg(G.dp, 4) > 0, "CLOCK counts");
    check(n64_rdp_read_reg(G.dp, 5) > 0, "BUFBUSY counts");
    check(n64_rdp_read_reg(G.dp, 6) > 0, "PIPEBUSY counts");
    n64_rdp_write_reg(G.dp, 3, 0x200 | 0x100 | 0x080 | 0x040);
    expect("CLOCK cleared", n64_rdp_read_reg(G.dp, 4), 0);
    expect("BUFBUSY cleared", n64_rdp_read_reg(G.dp, 5), 0);
    expect("PIPEBUSY cleared", n64_rdp_read_reg(G.dp, 6), 0);
    /* the pipe and its clock run from an END write (even an empty buffer) until SYNC_FULL (n64-systemtest "RDP STATUS:
     * Flags during a run" compares the whole register) */
    go();
    expect("busy after an empty buffer", n64_rdp_read_reg(G.dp, 3), 0x080 | 0x020 | 0x008);
    L(&LS, c_color_image(F_RGBA, SZ16, 64, FB_ADDR));
    go();
    expect("busy after a non-drawing command", n64_rdp_read_reg(G.dp, 3), 0x080 | 0x020 | 0x008);
    L(&LS, c_sync_full());
    go();
    expect("idle after SYNC_FULL", n64_rdp_read_reg(G.dp, 3), 0x080);
    n64_rdp_write_reg(G.dp, 3, 0x20);
    check(n64_rdp_read_reg(G.dp, 3) & 0x4, "flush set");
    n64_rdp_write_reg(G.dp, 3, 0x10);
    check(!(n64_rdp_read_reg(G.dp, 3) & 0x4), "flush cleared");
}

static void t_xbus(void)
{
    fresh("XBUS from DMEM, command split over two buffers");
    uint8_t dmem[4096];
    memset(dmem, 0, sizeof dmem);
    frame16(64, 64);
    L(&LS, c_modes(OM_FILL));
    L(&LS, c_fill_color(0x12341234));
    L(&LS, c_fill_rect(4, 4, 8, 8)); /* pixels (1,1)-(2,2) */
    for (int i = 0; i < LS.n; i++) {
        n64_wbe32(dmem + 0x100 + i * 8, (uint32_t)(LS.w[i] >> 32));
        n64_wbe32(dmem + 0x100 + i * 8 + 4, (uint32_t)LS.w[i]);
    }
    n64_rdp_attach_dmem(G.dp, dmem);
    n64_rdp_write_reg(G.dp, 3, 0x2);
    check(n64_rdp_read_reg(G.dp, 3) & 1, "xbus set");
    n64_rdp_write_reg(G.dp, 0, 0x100);
    n64_rdp_write_reg(G.dp, 1, 0x100 + (uint32_t)LS.n * 8);
    n64_rdp_run(G.dp);
    LS.n = 0;
    expect_px16(64, 1, 1, 0x1234);
    expect_px16(64, 2, 2, 0x1234);
    expect_px16(64, 3, 3, 0);
    n64_rdp_write_reg(G.dp, 3, 0x1);
    check(!(n64_rdp_read_reg(G.dp, 3) & 1), "xbus cleared");

    /* a texture rectangle (2 words) whose second word comes with the next END */
    L(&LS, c_modes(OM_FILL));
    L(&LS, c_fill_color(0xFFFFFFFF));
    c_texrect(&LS, 0, 0, 40, 40, 44, 44, 0, 0, 0, 0); /* fill mode draws a rectangle: (10,10)-(11,11) */
    for (int i = 0; i < LS.n; i++) {
        n64_wbe32(G.ram + CMD_BASE + i * 8, (uint32_t)(LS.w[i] >> 32));
        n64_wbe32(G.ram + CMD_BASE + i * 8 + 4, (uint32_t)LS.w[i]);
    }
    n64_rdp_write_reg(G.dp, 0, CMD_BASE);
    n64_rdp_write_reg(G.dp, 1, CMD_BASE + 3 * 8);
    n64_rdp_run(G.dp);
    expect_px16(64, 10, 10, 0);
    check(n64_rdp_read_reg(G.dp, 3) & 0x40, "cmd busy with a partial command");
    n64_rdp_write_reg(G.dp, 1, CMD_BASE + 4 * 8); /* no START: continues */
    n64_rdp_run(G.dp);
    LS.n = 0;
    expect_px16(64, 10, 10, 0xFFFF);
    expect_px16(64, 11, 11, 0xFFFF);
}

/* ---- Fill mode ----------------------------------------------------------------------------------------------------- */
static void t_fill(void)
{
    fresh("fill 16-bit: inclusive bounds, two colours by address");
    frame16(64, 64);
    L(&LS, c_modes(OM_FILL));
    L(&LS, c_fill_color(0xF80107C1));
    L(&LS, c_fill_rect(2 * 4, 1 * 4, 9 * 4, 3 * 4));
    go();
    expect_px16(64, 2, 1, 0xF801);
    expect_px16(64, 3, 1, 0x07C1);
    expect_px16(64, 9, 3, 0x07C1);
    expect_px16(64, 10, 3, 0);
    expect_px16(64, 1, 1, 0);
    expect_px16(64, 2, 4, 0);
    expect("pixels", (uint32_t)count16(64, 0, 0, 64, 64), 24);

    fresh("fill 32-bit and 8-bit");
    frame32(64, 64);
    L(&LS, c_modes(OM_FILL));
    L(&LS, c_fill_color(0x11223344));
    L(&LS, c_fill_rect(0, 0, 3 * 4, 0));
    go();
    expect("32-bit (3,0)", fb32(&G, 64, 3, 0), 0x11223344);
    expect("32-bit (4,0)", fb32(&G, 64, 4, 0), 0);
    L(&LS, c_color_image(F_I, SZ8, 64, FB_ADDR + 0x10000));
    L(&LS, c_fill_color(0xAABBCCDD));
    L(&LS, c_fill_rect(0, 0, 5 * 4, 0));
    go();
    expect("8-bit bytes 0..3", n64_be32(G.ram + FB_ADDR + 0x10000), 0xAABBCCDD);
    expect("8-bit bytes 4..7", n64_be32(G.ram + FB_ADDR + 0x10004), 0xAABB0000);

    fresh("scissor and interlace field");
    frame16(64, 64);
    L(&LS, c_scissor(4 * 4, 4 * 4, 12 * 4, 12 * 4));
    L(&LS, c_modes(OM_FILL));
    L(&LS, c_fill_color(0xFFFFFFFF));
    L(&LS, c_fill_rect(0, 0, 20 * 4, 20 * 4));
    go();
    expect("scissored pixels", (uint32_t)count16(64, 0, 0, 64, 64), 64);
    expect_px16(64, 4, 4, 0xFFFF);
    expect_px16(64, 11, 11, 0xFFFF);
    expect_px16(64, 12, 11, 0);
    L(&LS, c_scissor_field(20 * 4, 0, 30 * 4, 10 * 4, 1));
    L(&LS, c_fill_rect(0, 0, 63 * 4, 63 * 4));
    go();
    expect("odd lines kept", (uint32_t)count16(64, 20, 0, 30, 10), 50);
    expect_px16(64, 20, 0, 0);
    expect_px16(64, 20, 1, 0xFFFF);
}

/* ---- Copy mode ----------------------------------------------------------------------------------------------------- */
static uint16_t ctex(int x, int y) { return (uint16_t)((x + 1) << 8 | y << 2 | (x & 1)); }

static void load_rgba16(int w, int h, uint32_t addr, int tile, int mask_s, int mask_t)
{
    int line = (w * 2 + 7) / 8;
    L(&LS, c_tex_image(F_RGBA, SZ16, w, addr));
    L(&LS, c_set_tile(F_RGBA, SZ16, line, 0, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0));
    L(&LS, c_load_tile(7, 0, 0, (w - 1) * 4, (h - 1) * 4));
    L(&LS, c_set_tile(F_RGBA, SZ16, line, 0, tile, 0, 0, 0, mask_t, 0, 0, 0, mask_s, 0));
    L(&LS, c_tile_size(tile, 0, 0, (w - 1) * 4, (h - 1) * 4));
}

static void t_copy(void)
{
    fresh("copy mode 1:1, alpha compare, flip");
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 16; x++)
            n64_wbe16(G.ram + TEX_ADDR + (y * 16 + x) * 2, ctex(x, y));
    frame16(64, 64);
    load_rgba16(16, 4, TEX_ADDR, 0, 0, 0);
    L(&LS, c_modes(OM_COPY));
    c_texrect(&LS, 0, 0, 10 * 4, 5 * 4, 25 * 4, 8 * 4, 0, 0, 4 << 10, 1 << 10);
    go();
    int bad = 0;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 16; x++)
            bad += fb16(&G, 64, 10 + x, 5 + y) != ctex(x, y);
    expect("copied texels wrong", (uint32_t)bad, 0);
    expect_px16(64, 26, 5, 0);
    expect_px16(64, 10, 9, 0);

    L(&LS, c_modes(OM_COPY | OM_ALPHA_COMPARE));
    c_texrect(&LS, 0, 0, 10 * 4, 20 * 4, 25 * 4, 20 * 4, 0, 0, 4 << 10, 1 << 10);
    go();
    expect_px16(64, 10, 20, 0);          /* alpha 0: skipped */
    expect_px16(64, 11, 20, ctex(1, 0)); /* alpha 1 */

    L(&LS, c_modes(OM_COPY));
    c_texrect(&LS, 1, 0, 40 * 4, 30 * 4, 43 * 4, 45 * 4, 0, 0, 1 << 10, 4 << 10);
    go();
    bad = 0;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 4; x++)
            bad += fb16(&G, 64, 40 + x, 30 + y) != ctex(y, x);
    expect("flipped texels wrong", (uint32_t)bad, 0);

    fresh("copy mode CI8 through the TLUT, 8-bit colour image");
    for (int i = 0; i < 8; i++) {
        G.ram[TEX_ADDR + i] = (uint8_t)i;
        n64_wbe16(G.ram + TEX_ADDR + 0x800 + i * 2, (uint16_t)((0x1000 * i + 0x42) | 1));
    }
    frame16(64, 64);
    L(&LS, c_tex_image(F_RGBA, SZ16, 1, TEX_ADDR + 0x800));
    L(&LS, c_set_tile(F_RGBA, SZ16, 0, 256, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0));
    L(&LS, c_load_tlut(6, 0, 0, 7 * 4, 0));
    L(&LS, c_tex_image(F_CI, SZ8, 8, TEX_ADDR));
    L(&LS, c_set_tile(F_CI, SZ8, 1, 0, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0));
    L(&LS, c_load_tile(7, 0, 0, 7 * 4, 0));
    L(&LS, c_set_tile(F_CI, SZ8, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0));
    L(&LS, c_tile_size(0, 0, 0, 7 * 4, 0));
    L(&LS, c_modes(OM_COPY | OM_TLUT));
    c_texrect(&LS, 0, 0, 0, 0, 7 * 4, 0, 0, 0, 4 << 10, 1 << 10);
    go();
    for (int i = 0; i < 8; i++)
        expect_px16(64, i, 0, (uint16_t)((0x1000 * i + 0x42) | 1));
    L(&LS, c_color_image(F_CI, SZ8, 64, FB_ADDR + 0x10000));
    L(&LS, c_modes(OM_COPY));
    c_texrect(&LS, 0, 0, 0, 0, 7 * 4, 0, 0, 0, 4 << 10, 1 << 10);
    go();
    expect("8-bit copy 0..3", n64_be32(G.ram + FB_ADDR + 0x10000), 0x00010203);
    expect("8-bit copy 4..7", n64_be32(G.ram + FB_ADDR + 0x10004), 0x04050607);
}

/* ---- Triangles ----------------------------------------------------------------------------------------------------- */
static void flat_tri(const vtx *a, const vtx *b, const vtx *c)
{
    L(&LS, c_modes(OM_STD));
    L(&LS, CC_PASS(CC_PRIM, AC_PRIM));
    c_triangle(&LS, 0, 0, 0, 0, 0, a, b, c);
}

static void t_tri_cover(void)
{
    fresh("triangle coverage: left major");
    frame16(64, 64);
    L(&LS, c_prim(0, 0, 0xFFFFFFFF));
    vtx a = V(10, 10), b = V(30, 10), c = V(10, 30);
    flat_tri(&a, &b, &c);
    go();
    expect("pixels", (uint32_t)count16(64, 0, 0, 64, 64), 210);
    check(fb16(&G, 64, 10, 10) && fb16(&G, 64, 29, 10) && !fb16(&G, 64, 30, 10), "row 10 = x 10..29");
    check(fb16(&G, 64, 10, 29) && !fb16(&G, 64, 11, 29) && !fb16(&G, 64, 10, 30), "row 29 = x 10");
    check(!fb16(&G, 64, 9, 15), "left edge exclusive of x 9");

    fresh("triangle coverage: right major, same area");
    frame16(64, 64);
    L(&LS, c_prim(0, 0, 0xFFFFFFFF));
    vtx d = V(30, 10), e = V(10, 10), f = V(30, 30);
    flat_tri(&d, &e, &f);
    go();
    expect("pixels", (uint32_t)count16(64, 0, 0, 64, 64), 210);
    check(fb16(&G, 64, 10, 10) && fb16(&G, 64, 29, 10) && !fb16(&G, 64, 30, 10), "row 10 = x 10..29");
    check(fb16(&G, 64, 29, 29) && !fb16(&G, 64, 28, 29), "row 29 = x 29");

    fresh("shared edge: every coverage sample drawn once (wrapped coverage sums to 8)");
    frame32(64, 64);
    vtx p = V(3.3f, 2.7f), q = V(50.6f, 17.2f), r = V(20.1f, 60.9f), s = V(55.0f, 58.0f);
    L(&LS, c_prim(0, 0, 0x404040FF));
    L(&LS, c_modes(OM_STD | OM_IMAGE_READ | OM_AA | OM_CVG_DEST(1)));
    L(&LS, CC_PASS(CC_PRIM, AC_PRIM));
    c_triangle(&LS, 0, 0, 0, 0, 0, &p, &q, &r);
    c_triangle(&LS, 0, 0, 0, 0, 0, &q, &s, &r);
    go();
    int over = 0, blank = 0;
    for (int y = 25; y < 45; y++)
        for (int x = 25; x < 40; x++) {
            over += (fb32(&G, 64, x, y) & 0xE0) != 0;
            blank += (fb32(&G, 64, x, y) >> 8) == 0;
        }
    expect("pixels with coverage != 8 inside the quad", (uint32_t)over, 0);
    expect("pixels not drawn inside the quad", (uint32_t)blank, 0);

    fresh("triangle vertex order does not matter");
    frame16(64, 64);
    const u64 additive = OM_STD | OM_IMAGE_READ | OM_FORCE_BLEND | OM_BLEND0(0, 0, 1, 2); /* P * 31/32 + M */
    L(&LS, c_prim(0, 0, 0x404040FF));
    L(&LS, c_modes(additive));
    L(&LS, CC_PASS(CC_PRIM, AC_PRIM));
    c_triangle(&LS, 0, 0, 0, 0, 0, &p, &q, &r);
    c_triangle(&LS, 0, 0, 0, 0, 0, &q, &s, &r);
    go();
    uint32_t h1 = fnv1a(G.ram + FB_ADDR, 64 * 64 * 2);
    fresh("triangle vertex order does not matter (reversed)");
    frame16(64, 64);
    L(&LS, c_prim(0, 0, 0x404040FF));
    L(&LS, c_modes(additive));
    L(&LS, CC_PASS(CC_PRIM, AC_PRIM));
    c_triangle(&LS, 0, 0, 0, 0, 0, &r, &p, &q);
    c_triangle(&LS, 0, 0, 0, 0, 0, &s, &r, &q);
    go();
    expect("same image", fnv1a(G.ram + FB_ADDR, 64 * 64 * 2), h1);

    fresh("coverage of edge pixels (32-bit alpha holds it)");
    frame32(64, 64);
    L(&LS, c_prim(0, 0, 0xFFFFFFFF));
    L(&LS, c_modes(OM_STD | OM_IMAGE_READ | OM_AA | OM_CVG_DEST(0)));
    L(&LS, CC_PASS(CC_PRIM, AC_PRIM));
    c_triangle(&LS, 0, 0, 0, 0, 0, &a, &b, &c);
    go();
    expect("diagonal pixel coverage 4/8", fb32(&G, 64, 29, 10) & 0xFF, 4 << 5);
    expect("inner pixel coverage full (clamped 7)", fb32(&G, 64, 15, 15) & 0xFF, 7 << 5);
    expect("left edge pixel full", fb32(&G, 64, 10, 20) & 0xFF, 7 << 5);
}

static void t_shade(void)
{
    fresh("gouraud: planes sampled at pixel corners");
    frame32(128, 128);
    L(&LS, c_modes(OM_STD));
    L(&LS, CC_PASS(CC_SHADE, AC_SHADE));
    vtx a = VC(0, 0, 0, 0, 50, 255), b = VC(100, 0, 200, 0, 50, 255), c = VC(0, 100, 0, 200, 50, 255);
    c_triangle(&LS, 1, 0, 0, 0, 0, &a, &b, &c);
    go();
    expect_rgb(128, 10, 5, 20, 10, 50);
    expect_rgb(128, 40, 30, 80, 60, 50);
    expect_rgb(128, 0, 0, 0, 0, 50);
}

/* ---- Texture formats ----------------------------------------------------------------------------------------------- */
/* Draws a w x h texture at TEX_ADDR (fmt / size; loaded as bytes for 4-bit sizes) 1:1, point sampled, into a 32-bit
 * frame: TEX0 RGB at row t, TEX0 alpha (as grey) at row 32 + t. */
static void draw_tex(int fmt, int size, int w, int h, u64 modes, int pal)
{
    int lsize = size == SZ4 ? SZ8 : size, lw = size == SZ4 ? w / 2 : w;
    int line = size == SZ32 ? (w * 2 + 7) / 8 : fmt == F_YUV ? (w + 7) / 8 : (((w << size) >> 1) + 7) / 8;
    frame32(64, 64);
    L(&LS, c_tex_image(fmt, lsize, lw, TEX_ADDR));
    L(&LS, c_set_tile(fmt, lsize, line, 0, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0));
    L(&LS, c_load_tile(7, 0, 0, (lw - 1) * 4, (h - 1) * 4));
    L(&LS, c_set_tile(fmt, size, line, 0, 0, pal, 0, 0, 0, 0, 0, 0, 0, 0));
    L(&LS, c_tile_size(0, 0, 0, (w - 1) * 4, (h - 1) * 4));
    L(&LS, c_modes(modes));
    L(&LS, CC_PASS(CC_TEX0, AC_TEX0));
    c_texrect(&LS, 0, 0, 0, 0, w * 4, h * 4, 0, 0, 1 << 10, 1 << 10);
    L(&LS, c_combine1(CC_ONE, CC_ZERO_A, CC_TEX0_A, CC_ZERO_ADD, AC_ZERO, AC_ZERO, AC_ZERO, AC_ZERO));
    c_texrect(&LS, 0, 0, 0, 32 * 4, w * 4, (32 + h) * 4, 0, 0, 1 << 10, 1 << 10);
    go();
}
static void expect_texel(int x, int y, int r, int g, int b, int a)
{
    expect_rgb(64, x, y, r, g, b);
    char s[64];
    snprintf(s, sizeof s, "alpha(%d,%d)", x, y);
    expect(s, fb32(&G, 64, x, 32 + y) >> 24, (uint32_t)a);
}

static void t_formats(void)
{
    fresh("RGBA16");
    const uint16_t c16[4] = { 0xF801, 0x07C0, 0x003F, 0x8421 };
    for (int i = 0; i < 4; i++)
        n64_wbe16(G.ram + TEX_ADDR + i * 2, c16[i]);
    draw_tex(F_RGBA, SZ16, 4, 1, OM_STD, 0);
    expect_texel(0, 0, 255, 0, 0, 255);
    expect_texel(1, 0, 0, 255, 0, 0);
    expect_texel(2, 0, 0, 0, 255, 255);
    expect_texel(3, 0, 132, 132, 132, 255);

    fresh("RGBA32 (split halves)");
    n64_wbe32(G.ram + TEX_ADDR, 0x11223344);
    n64_wbe32(G.ram + TEX_ADDR + 4, 0xAABBCCDD);
    n64_wbe32(G.ram + TEX_ADDR + 8, 0x01020304);
    n64_wbe32(G.ram + TEX_ADDR + 12, 0xF0E0D0C0);
    n64_wbe32(G.ram + TEX_ADDR + 16, 0x55667788); /* row 1 (texture 4 wide) */
    draw_tex(F_RGBA, SZ32, 4, 2, OM_STD, 0);
    expect_texel(0, 0, 0x11, 0x22, 0x33, 0x44);
    expect_texel(1, 0, 0xAA, 0xBB, 0xCC, 0xDD);
    expect_texel(3, 0, 0xF0, 0xE0, 0xD0, 0xC0);
    expect_texel(0, 1, 0x55, 0x66, 0x77, 0x88);

    fresh("IA4 / IA8 / IA16");
    G.ram[TEX_ADDR] = 0xB3;
    G.ram[TEX_ADDR + 1] = 0x5C;
    draw_tex(F_IA, SZ4, 4, 1, OM_STD, 0);
    expect_texel(0, 0, 182, 182, 182, 255);
    expect_texel(1, 0, 36, 36, 36, 255);
    expect_texel(2, 0, 73, 73, 73, 255);
    expect_texel(3, 0, 219, 219, 219, 0);
    G.ram[TEX_ADDR] = 0x9C;
    G.ram[TEX_ADDR + 1] = 0x0F;
    draw_tex(F_IA, SZ8, 2, 1, OM_STD, 0);
    expect_texel(0, 0, 153, 153, 153, 204);
    expect_texel(1, 0, 0, 0, 0, 255);
    n64_wbe16(G.ram + TEX_ADDR, 0x80FF);
    n64_wbe16(G.ram + TEX_ADDR + 2, 0x1234);
    draw_tex(F_IA, SZ16, 2, 1, OM_STD, 0);
    expect_texel(0, 0, 128, 128, 128, 255);
    expect_texel(1, 0, 0x12, 0x12, 0x12, 0x34);

    fresh("I4 / I8");
    G.ram[TEX_ADDR] = 0x7F;
    draw_tex(F_I, SZ4, 2, 1, OM_STD, 0);
    expect_texel(0, 0, 119, 119, 119, 119);
    expect_texel(1, 0, 255, 255, 255, 255);
    G.ram[TEX_ADDR] = 0x5A;
    draw_tex(F_I, SZ8, 1, 1, OM_STD, 0);
    expect_texel(0, 0, 90, 90, 90, 90);

    fresh("CI4 (palette 2) and CI8 through the TLUT, RGBA16 and IA16 entries");
    for (int i = 0; i < 48; i++)
        n64_wbe16(G.ram + TEX_ADDR + 0x800 + i * 2, (uint16_t)(i == 35 ? 0xF801 : i == 32 ? 0x07C1 : i == 5 ? 0x7F80 : 0));
    G.ram[TEX_ADDR] = 0x30;
    L(&LS, c_tex_image(F_RGBA, SZ16, 1, TEX_ADDR + 0x800));
    L(&LS, c_set_tile(F_RGBA, SZ16, 0, 256, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0));
    L(&LS, c_load_tlut(6, 0, 0, 47 * 4, 0));
    draw_tex(F_CI, SZ4, 2, 1, OM_STD | OM_TLUT, 2);
    expect_texel(0, 0, 255, 0, 0, 255);
    expect_texel(1, 0, 0, 255, 0, 255);
    G.ram[TEX_ADDR] = 5;
    draw_tex(F_CI, SZ8, 1, 1, OM_STD | OM_TLUT | OM_TLUT_IA, 0);
    expect_texel(0, 0, 0x7F, 0x7F, 0x7F, 0x80);

    fresh("YUV through the convert step");
    G.ram[TEX_ADDR] = 128; /* U Y0 V Y1 */
    G.ram[TEX_ADDR + 1] = 100;
    G.ram[TEX_ADDR + 2] = 228;
    G.ram[TEX_ADDR + 3] = 200;
    L(&LS, c_convert(175, -43, -89, 222, 0, 0));
    draw_tex(F_YUV, SZ16, 2, 1, OM_1CYC | OM_RGB_DITHER(3) | OM_ALPHA_DITHER(3), 0); /* bi_lerp_0 = 0: convert */
    expect_rgb(64, 0, 0, 168, 65, 100);
    expect_rgb(64, 1, 0, 255, 165, 200);
}

static void t_tmem(void)
{
    /* 8x4 RGBA16, 2 TMEM words per row: LOAD_BLOCK with dxt = 2048 / 2 must match LOAD_TILE. */
    uint16_t v[4][8];
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 8; x++) {
            v[y][x] = rgba16(x * 32, y * 64, 0, 1);
            n64_wbe16(G.ram + TEX_ADDR + (y * 8 + x) * 2, v[y][x]);
        }
    for (int mode = 0; mode < 3; mode++) {
        if (mode) {
            uint8_t keep[64];
            memcpy(keep, G.ram + TEX_ADDR, 64);
            fresh(mode == 1 ? "LOAD_BLOCK with dxt" : "LOAD_BLOCK without dxt: odd rows read swapped");
            memcpy(G.ram + TEX_ADDR, keep, 64);
        } else {
            fresh("LOAD_TILE 8x4 RGBA16");
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 8; x++)
                    n64_wbe16(G.ram + TEX_ADDR + (y * 8 + x) * 2, v[y][x]);
        }
        frame32(64, 64);
        L(&LS, c_tex_image(F_RGBA, SZ16, 8, TEX_ADDR));
        if (mode == 0) {
            L(&LS, c_set_tile(F_RGBA, SZ16, 2, 0, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0));
            L(&LS, c_load_tile(7, 0, 0, 7 * 4, 3 * 4));
        } else {
            L(&LS, c_set_tile(F_RGBA, SZ16, 0, 0, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0));
            L(&LS, c_load_block(7, 0, 0, 31, mode == 1 ? 1024 : 0));
        }
        L(&LS, c_set_tile(F_RGBA, SZ16, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0));
        L(&LS, c_tile_size(0, 0, 0, 7 * 4, 3 * 4));
        L(&LS, c_modes(OM_STD));
        L(&LS, CC_PASS(CC_TEX0, AC_TEX0));
        c_texrect(&LS, 0, 0, 0, 0, 8 * 4, 4 * 4, 0, 0, 1 << 10, 1 << 10);
        go();
        int bad = 0;
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 8; x++) {
                int sx = (mode == 2 && (y & 1)) ? x ^ 2 : x;
                uint16_t t = v[y][sx];
                uint32_t want = (uint32_t)((t >> 11) << 3 | (t >> 11) >> 2) << 16 |
                                (uint32_t)(((t >> 6) & 31) << 3 | ((t >> 6) & 31) >> 2) << 8;
                bad += (fb32(&G, 64, x, y) >> 8) != want;
            }
        expect("texels wrong", (uint32_t)bad, 0);
    }
}

/* I8 texture 4x1 = 10 20 30 40, drawn 16 pixels with s = x through tile settings. */
static void wrap_case(const char *name, int cs, int ms, int mask, int shift, int sl, const int *want)
{
    fresh(name);
    const uint8_t t[4] = { 10, 20, 30, 40 };
    memcpy(G.ram + TEX_ADDR, t, 4);
    frame32(64, 64);
    L(&LS, c_tex_image(F_I, SZ8, 4, TEX_ADDR));
    L(&LS, c_set_tile(F_I, SZ8, 1, 0, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0));
    L(&LS, c_load_tile(7, 0, 0, 3 * 4, 0));
    L(&LS, c_set_tile(F_I, SZ8, 1, 0, 0, 0, 1, 0, 0, 0, cs, ms, mask, shift));
    L(&LS, c_tile_size(0, sl * 4, 0, (sl + 3) * 4, 0));
    L(&LS, c_modes(OM_STD));
    L(&LS, CC_PASS(CC_TEX0, AC_TEX0));
    c_texrect(&LS, 0, 0, 0, 0, 16 * 4, 4, 0, 0, 1 << 10, 1 << 10);
    go();
    for (int x = 0; x < 16; x++) {
        char s[32];
        snprintf(s, sizeof s, "x %d", x);
        expect(s, fb32(&G, 64, x, 0) >> 24, (uint32_t)want[x]);
    }
}

static void t_wrap(void)
{
    static const int wrap[16] = { 10, 20, 30, 40, 10, 20, 30, 40, 10, 20, 30, 40, 10, 20, 30, 40 };
    static const int mirror[16] = { 10, 20, 30, 40, 40, 30, 20, 10, 10, 20, 30, 40, 40, 30, 20, 10 };
    static const int clamp[16] = { 10, 20, 30, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40 };
    static const int shr1[16] = { 10, 10, 20, 20, 30, 30, 40, 40, 10, 10, 20, 20, 30, 30, 40, 40 };
    static const int shl1[16] = { 10, 30, 10, 30, 10, 30, 10, 30, 10, 30, 10, 30, 10, 30, 10, 30 };
    static const int off1[16] = { 40, 10, 20, 30, 40, 10, 20, 30, 40, 10, 20, 30, 40, 10, 20, 30 };
    wrap_case("wrap (mask 2)", 0, 0, 2, 0, 0, wrap);
    wrap_case("mirror", 0, 1, 2, 0, 0, mirror);
    wrap_case("clamp", 1, 0, 0, 0, 0, clamp);
    wrap_case("shift right 1", 0, 0, 2, 1, 0, shr1);
    wrap_case("shift left 1 (15)", 0, 0, 2, 15, 0, shl1);
    wrap_case("SL offset 1", 0, 0, 2, 0, 1, off1);
}

static void t_bilinear(void)
{
    fresh("bilinear (3-point) along s");
    G.ram[TEX_ADDR] = 0;
    G.ram[TEX_ADDR + 1] = 255;
    frame32(64, 64);
    L(&LS, c_tex_image(F_I, SZ8, 2, TEX_ADDR));
    L(&LS, c_set_tile(F_I, SZ8, 1, 0, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0));
    L(&LS, c_load_tile(7, 0, 0, 1 * 4, 0));
    L(&LS, c_set_tile(F_I, SZ8, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0));
    L(&LS, c_tile_size(0, 0, 0, 1 * 4, 0));
    L(&LS, c_modes(OM_STD | OM_BILERP));
    L(&LS, CC_PASS(CC_TEX0, AC_TEX0));
    c_texrect(&LS, 0, 0, 0, 0, 6 * 4, 4, 0, 0, 1 << 8, 1 << 10);
    go();
    const int want[6] = { 0, 64, 128, 191, 255, 255 };
    for (int x = 0; x < 6; x++) {
        char s[32];
        snprintf(s, sizeof s, "x %d", x);
        expect(s, fb32(&G, 64, x, 0) >> 24, (uint32_t)want[x]);
    }
}

/* ---- Combiner, blender, dither ------------------------------------------------------------------------------------- */
static void const_rect(u64 modes, u64 combine, int x, int y)
{
    L(&LS, c_modes(modes));
    L(&LS, combine);
    L(&LS, c_fill_rect(x * 4, y * 4, (x + 4) * 4, (y + 4) * 4));
}

static void t_combiner(void)
{
    fresh("combiner: constant, generic, K4 / K5, prim LOD fraction, noise");
    frame32(64, 64);
    L(&LS, c_prim(0, 77, 0xC8643280)); /* 200 100 50 128 */
    L(&LS, c_env(0x00326440));         /* 0 50 100 64 */
    L(&LS, c_convert(0, 0, 0, 0, 16, 128));
    const_rect(OM_STD, c_combine1(CC_PRIM, CC_ENV, CC_PRIM_A, CC_ENV, AC_PRIM, AC_ENV, AC_PRIM, AC_ENV), 0, 0);
    const_rect(OM_STD, c_combine1(CC_PRIM, CC_K4, CC_K5, CC_ENV, AC_ZERO, AC_ZERO, AC_ZERO, AC_ONE), 8, 0);
    const_rect(OM_STD, c_combine1(CC_ONE, CC_ZERO_A, CC_PRIMLODFRAC, CC_ZERO_ADD, AC_ZERO, AC_ZERO, AC_ZERO, AC_ONE), 16, 0);
    go();
    expect_rgb(64, 1, 1, 100, 75, 75);
    expect_rgb(64, 9, 1, 92, 92, 117);
    expect_rgb(64, 17, 1, 77, 77, 77);
    /* the generic path (shade is per pixel) gives the constant path's result */
    L(&LS, c_modes(OM_STD));
    L(&LS, c_combine1(CC_SHADE, CC_ENV, CC_PRIM_A, CC_ENV, AC_ZERO, AC_ZERO, AC_ZERO, AC_ONE));
    vtx a = VC(0, 20, 200, 100, 50, 128), b = VC(40, 20, 200, 100, 50, 128), c = VC(0, 60, 200, 100, 50, 128);
    c_triangle(&LS, 1, 0, 0, 0, 0, &a, &b, &c);
    go();
    expect_rgb(64, 5, 25, 100, 75, 75);
    /* noise: several values, all from (n << 6 | 0x20) * 255 / 256 through the 9-bit clamp */
    L(&LS, c_modes(OM_STD));
    L(&LS, c_prim(0, 0, 0xC86432FF)); /* (noise - 0) * 255 / 256 */
    L(&LS, c_combine1(CC_NOISE, CC_ZERO_A, CC_PRIM_A, CC_ZERO_ADD, AC_ZERO, AC_ZERO, AC_ZERO, AC_ONE));
    L(&LS, c_fill_rect(32 * 4, 32 * 4, 48 * 4, 48 * 4));
    go();
    int seen[256] = { 0 }, bad = 0, kinds = 0;
    for (int y = 32; y < 48; y++)
        for (int x = 32; x < 48; x++) {
            int v = (int)(fb32(&G, 64, x, y) >> 24);
            bad += !(v == 32 || v == 96 || v == 159 || v == 223 || v == 255 || v == 0);
            kinds += !seen[v]++;
        }
    expect("noise values out of the set", (uint32_t)bad, 0);
    check(kinds >= 4, "noise varies");
    uint32_t h = fnv1a(G.ram + FB_ADDR + 32 * 64 * 4, 16 * 64 * 4);
    L(&LS, c_fill_rect(32 * 4, 32 * 4, 48 * 4, 48 * 4));
    go();
    check(fnv1a(G.ram + FB_ADDR + 32 * 64 * 4, 16 * 64 * 4) != h, "noise differs per primitive");

    fresh("2-cycle combiner");
    frame32(64, 64);
    L(&LS, c_prim(0, 0, 0xC86432FF));
    L(&LS, c_env(0x80FF40FF));
    const_rect(OM_2CYC | OM_TF0_RGB | OM_TF1_RGB | OM_RGB_DITHER(3) | OM_ALPHA_DITHER(3) | OM_BLEND1(0, 0, 0, 0),
               c_combine(CC_PRIM, CC_ZERO_A, CC_ENV, CC_ZERO_ADD, AC_ZERO, AC_ZERO, AC_ZERO, AC_ONE, CC_ONE,
                         CC_COMBINED, CC_PRIM, CC_COMBINED, AC_ZERO, AC_ZERO, AC_ZERO, AC_ONE),
               0, 0);
    go(); /* cycle 0: prim * env = (100, 100, 13); cycle 1: (1 - c) * prim + c */
    expect_rgb(64, 1, 1, 222, 161, 60);
}

static void t_blender(void)
{
    fresh("blender: alpha blend over memory (force blend and divided)");
    frame32(64, 64);
    L(&LS, c_modes(OM_FILL));
    L(&LS, c_fill_color(0x0000FFFF));
    L(&LS, c_fill_rect(0, 0, 63 * 4, 63 * 4));
    L(&LS, c_prim(0, 0, 0xFF000080));
    const_rect(OM_STD | OM_IMAGE_READ | OM_FORCE_BLEND | OM_BLEND0(0, 0, 1, 0), CC_PASS(CC_PRIM, AC_PRIM), 0, 0);
    const_rect(OM_STD | OM_IMAGE_READ | OM_BLEND0(0, 0, 1, 0), CC_PASS(CC_PRIM, AC_PRIM), 8, 0);
    go();
    expect_rgb(64, 1, 1, 127, 0, 127);
    expect_rgb(64, 9, 1, 127, 0, 127);

    fresh("blender 16-bit with dither off");
    frame16(64, 64);
    L(&LS, c_modes(OM_FILL));
    L(&LS, c_fill_color(0x003F003F));
    L(&LS, c_fill_rect(0, 0, 63 * 4, 63 * 4));
    L(&LS, c_prim(0, 0, 0xFF000080));
    const_rect(OM_STD | OM_IMAGE_READ | OM_FORCE_BLEND | OM_BLEND0(0, 0, 1, 0), CC_PASS(CC_PRIM, AC_PRIM), 0, 0);
    go();
    expect_px16(64, 1, 1, rgba16(127, 0, 127, 1));

    fresh("fog in 2-cycle mode (shade alpha)");
    frame32(64, 64);
    L(&LS, c_fog(0x282828FF));
    L(&LS, c_modes(OM_2CYC | OM_TF0_RGB | OM_TF1_RGB | OM_RGB_DITHER(3) | OM_ALPHA_DITHER(3) | OM_BLEND0(0, 2, 3, 0) |
                   OM_BLEND1(0, 0, 0, 0)));
    L(&LS, c_combine(CC_ZERO_A, CC_ZERO_A, CC_ZERO_MUL, CC_SHADE, AC_ZERO, AC_ZERO, AC_ZERO, AC_SHADE, CC_ZERO_A,
                     CC_ZERO_A, CC_ZERO_MUL, CC_COMBINED, AC_ZERO, AC_ZERO, AC_ZERO, AC_COMBINED));
    vtx a = VC(0, 0, 200, 200, 200, 64), b = VC(40, 0, 200, 200, 200, 64), c = VC(0, 40, 200, 200, 200, 64);
    c_triangle(&LS, 1, 0, 0, 0, 0, &a, &b, &c);
    go();
    expect_rgb(64, 5, 5, 80, 80, 80);

    fresh("alpha compare (blend alpha threshold) and chroma key");
    frame32(64, 64);
    L(&LS, c_prim(0, 0, 0xFFFFFF64)); /* alpha 100 */
    L(&LS, c_blend(0x00000080));
    const_rect(OM_STD | OM_ALPHA_COMPARE, CC_PASS(CC_PRIM, AC_PRIM), 0, 0);
    L(&LS, c_blend(0x00000064));
    const_rect(OM_STD | OM_ALPHA_COMPARE, CC_PASS(CC_PRIM, AC_PRIM), 8, 0);
    go();
    expect_rgb(64, 1, 1, 0, 0, 0);
    expect_rgb(64, 9, 1, 255, 255, 255);
    L(&LS, c_key_r(16, 0x40, 255));
    L(&LS, c_key_gb(16, 16, 0x80, 255, 0xC0, 255));
    L(&LS, c_blend(0x00000001));
    L(&LS, c_prim(0, 0, 0x4080C0FF));
    const_rect(OM_STD | OM_KEY | OM_ALPHA_COMPARE, CC_PASS(CC_PRIM, AC_PRIM), 16, 0);
    L(&LS, c_prim(0, 0, 0x4090C0FF));
    const_rect(OM_STD | OM_KEY | OM_ALPHA_COMPARE, CC_PASS(CC_PRIM, AC_PRIM), 24, 0);
    go();
    expect_rgb(64, 17, 1, 0, 0, 0);
    expect_rgb(64, 25, 1, 0x40, 0x90, 0xC0);
}

static void t_dither(void)
{
    static const int bayer01[8] = { 13, 12, 13, 12, 12, 13, 12, 13 };
    static const int magic01[8] = { 13, 12, 13, 12, 12, 12, 12, 12 };
    for (int m = 0; m < 3; m++) {
        fresh(m == 0 ? "dither: magic square" : m == 1 ? "dither: Bayer" : "dither: off");
        frame16(64, 64);
        L(&LS, c_prim(0, 0, 0x626262FF)); /* 98: low bits 2 */
        const_rect(OM_1CYC | OM_RGB_DITHER(m == 2 ? 3 : m) | OM_ALPHA_DITHER(3), CC_PASS(CC_PRIM, AC_PRIM), 0, 0);
        go();
        for (int i = 0; i < 8; i++) {
            int want = m == 0 ? magic01[i] : m == 1 ? bayer01[i] : 12;
            char s[32];
            snprintf(s, sizeof s, "(%d,%d) red", i & 3, i >> 2);
            expect(s, (uint32_t)(fb16(&G, 64, i & 3, i >> 2) >> 11), (uint32_t)want);
        }
    }
}

/* ---- Z buffer ------------------------------------------------------------------------------------------------------ */
static void z_tri(uint32_t color, float z, u64 zmodes, float x0, float y0, float sz)
{
    L(&LS, c_prim(0, 0, color));
    L(&LS, c_modes(OM_STD | zmodes));
    L(&LS, CC_PASS(CC_PRIM, AC_PRIM));
    vtx a = VZ(x0, y0, z), b = VZ(x0 + sz * 2, y0, z), c = VZ(x0, y0 + sz * 2, z);
    c_triangle(&LS, 0, 0, 1, 0, 0, &a, &b, &c);
}

static void t_zbuf(void)
{
    fresh("Z buffer: opaque, decal, transparent, stored format");
    L(&LS, c_color_image(F_RGBA, SZ16, 64, ZB_ADDR));
    L(&LS, c_scissor(0, 0, 64 * 4, 64 * 4));
    L(&LS, c_modes(OM_FILL));
    L(&LS, c_fill_color(0xFFFCFFFC));
    L(&LS, c_fill_rect(0, 0, 63 * 4, 63 * 4));
    frame16(64, 64);
    L(&LS, c_z_image(ZB_ADDR));
    const u64 zon = OM_Z_COMPARE | OM_Z_UPDATE;
    z_tri(0xF80000FF, 0x4000, zon, 0, 0, 30);  /* red, far */
    z_tri(0x0000F8FF, 0x1000, zon, 0, 0, 10);  /* blue, near, top-left corner */
    z_tri(0x00F800FF, 0x4000, zon, 0, 0, 30);  /* green at red's depth: not in front */
    go();
    expect_px16(64, 2, 2, rgba16(0, 0, 248, 1));
    expect_px16(64, 25, 5, rgba16(248, 0, 0, 1));
    expect("Z word (z 0x1000)", n64_be16(G.ram + ZB_ADDR + (2 * 64 + 2) * 2), 0x0800);
    expect("Z word (z 0x4000)", n64_be16(G.ram + ZB_ADDR + (5 * 64 + 25) * 2), 0x2000);
    z_tri(0xF8F800FF, 0x4000, OM_Z_COMPARE | OM_ZMODE(3), 0, 0, 30); /* decal: only where the depth matches */
    go();
    expect_px16(64, 25, 5, rgba16(248, 248, 0, 1));
    expect_px16(64, 2, 2, rgba16(0, 0, 248, 1));
    z_tri(0xFFFFFFFF, 0x2000, OM_Z_COMPARE | OM_ZMODE(2), 0, 0, 30); /* transparent: in front of red, not blue */
    go();
    expect_px16(64, 25, 5, 0xFFFF);
    expect_px16(64, 2, 2, rgba16(0, 0, 248, 1));
    expect("no Z update", n64_be16(G.ram + ZB_ADDR + (5 * 64 + 25) * 2), 0x2000);
    /* primitive depth on a rectangle */
    L(&LS, c_prim_depth(0x2000, 0x10));
    L(&LS, c_prim(0, 0, 0xFFFFFFFF));
    L(&LS, c_modes(OM_STD | zon | OM_Z_PRIM));
    L(&LS, c_fill_rect(40 * 4, 40 * 4, 44 * 4, 44 * 4));
    go();
    expect("Z word (prim z 0x2000, dz 16)", n64_be16(G.ram + ZB_ADDR + (41 * 64 + 41) * 2), 0x1001);
    expect_px16(64, 41, 41, 0xFFFF);
}

/* ---- Perspective, LOD ---------------------------------------------------------------------------------------------- */
static void t_persp(void)
{
    fresh("perspective-correct texture coordinates");
    for (int i = 0; i < 64; i++)
        G.ram[TEX_ADDR + i] = (uint8_t)(i * 3);
    frame32(128, 64);
    L(&LS, c_tex_image(F_I, SZ8, 64, TEX_ADDR));
    L(&LS, c_set_tile(F_I, SZ8, 8, 0, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0));
    L(&LS, c_load_tile(7, 0, 0, 63 * 4, 0));
    L(&LS, c_set_tile(F_I, SZ8, 8, 0, 0, 0, 1, 0, 0, 0, 0, 0, 6, 0));
    L(&LS, c_tile_size(0, 0, 0, 63 * 4, 0));
    L(&LS, c_modes(OM_STD | OM_PERSP));
    L(&LS, CC_PASS(CC_TEX0, AC_TEX0));
    vtx a = VT(0, 0, 0, 0, 1.0f), b = VT(64, 0, 64, 0, 0.25f), c = VT(0, 64, 0, 0, 1.0f);
    c_triangle(&LS, 0, 1, 0, 0, 0, &a, &b, &c);
    go();
    expect("s at x 32 = 12.8", fb32(&G, 128, 32, 2) >> 24, 36);
    expect("s at x 16 = 4.9", fb32(&G, 128, 16, 2) >> 24, 12);
}

static void t_lod(void)
{
    /* tile 0: 8x8 I8 of 50 at TMEM 0; tile 1: 4x4 of 150 at TMEM 64. TEX1 - TEX0 lerp by the LOD fraction. */
    const float scale[3] = { 1.0f, 1.5f, 2.0f };
    const int want[3] = { 50, 100, 150 };
    for (int k = 0; k < 3; k++) {
        fresh(k == 0 ? "LOD 1:1 -> tile 0" : k == 1 ? "LOD 1.5 -> half way" : "LOD 2 -> tile 1 (max level)");
        memset(G.ram + TEX_ADDR, 50, 64);
        memset(G.ram + TEX_ADDR + 64, 150, 16);
        frame32(64, 64);
        L(&LS, c_tex_image(F_I, SZ8, 8, TEX_ADDR));
        L(&LS, c_set_tile(F_I, SZ8, 1, 0, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0));
        L(&LS, c_load_tile(7, 0, 0, 7 * 4, 7 * 4));
        L(&LS, c_tex_image(F_I, SZ8, 4, TEX_ADDR + 64));
        L(&LS, c_set_tile(F_I, SZ8, 1, 8, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0));
        L(&LS, c_load_tile(6, 0, 0, 3 * 4, 3 * 4));
        L(&LS, c_set_tile(F_I, SZ8, 1, 0, 0, 0, 0, 0, 3, 0, 0, 0, 3, 0));
        L(&LS, c_tile_size(0, 0, 0, 7 * 4, 7 * 4));
        L(&LS, c_set_tile(F_I, SZ8, 1, 8, 1, 0, 0, 0, 2, 1, 0, 0, 2, 1));
        L(&LS, c_tile_size(1, 0, 0, 3 * 4, 3 * 4));
        L(&LS, c_modes(OM_2CYC | OM_LOD | OM_TF0_RGB | OM_TF1_RGB | OM_RGB_DITHER(3) | OM_ALPHA_DITHER(3) |
                       OM_BLEND1(0, 0, 0, 0)));
        L(&LS, c_combine(CC_TEX1, CC_TEX0, CC_LODFRAC, CC_TEX0, AC_ZERO, AC_ZERO, AC_ZERO, AC_ONE, CC_ZERO_A,
                         CC_ZERO_A, CC_ZERO_MUL, CC_COMBINED, AC_ZERO, AC_ZERO, AC_ZERO, AC_ONE));
        float sc = scale[k];
        vtx a = VT(0, 0, 0, 0, 0), b = VT(40, 0, 40 * sc, 0, 0), c = VT(0, 40, 0, 40 * sc, 0);
        c_triangle(&LS, 0, 1, 0, 0, 1, &a, &b, &c);
        go();
        expect("lerped texel", fb32(&G, 64, 5, 5) >> 24, (uint32_t)want[k]);
    }
}

/* ---- Scenes -------------------------------------------------------------------------------------------------------- */
static rdp_list scene;

static uint32_t busy_hash(int threads)
{
    fresh(threads > 1 ? "busy scene, threads" : "busy scene");
    if (threads > 1)
        check(n64_rdp_set_threads(G.dp, threads) == threads, "threads started");
    scene_textures(G.ram);
    scene.n = 0;
    scene_busy(&scene, 1500, 12345);
    rig_run(&G, &scene);
    expect("SYNC_FULL interrupt", (uint32_t)G.irqs, 1);
    return fnv1a(G.ram + FB_ADDR, 320 * 240 * 2) ^ fnv1a(G.ram + ZB_ADDR, 320 * 240 * 2) * 3;
}

static void t_scenes(void)
{
    uint32_t h1 = busy_hash(1);
    uint32_t h3 = busy_hash(3);
    expect("1 thread vs 3 threads", h3, h1);
    cur = "busy scene golden";
    if (GOLDEN_BUSY)
        expect("hash", h1, GOLDEN_BUSY);
    else
        printf("busy scene hash 0x%08X\n", h1);
}

/* ---- Mode matrix --------------------------------------------------------------------------------------------------- */
/* Random command lists over every mode, texel format, filter, blender, coverage and Z setting (integers only, so the
 * lists are the same on every host): a regression guard for the specialised paths (span_fast, the samplers, the packed
 * blender, decoded TMEM, copy mode), whose golden hash comes from the generic implementation they replaced. */
static uint32_t mx_s;
static uint32_t mx_rnd(void)
{
    mx_s ^= mx_s << 13;
    mx_s ^= mx_s >> 17;
    mx_s ^= mx_s << 5;
    return mx_s;
}
static int mx(int n) { return (int)(mx_rnd() % (uint32_t)n); }
static int32_t mx_range(int32_t lo, int32_t hi) { return lo + (int32_t)(mx_rnd() % (uint32_t)(hi - lo)); }
static int mx_max(int a, int b) { return a > b ? a : b; }

static const u64 mx_modes[] = {
    OM_STD,
    OM_STD | OM_BILERP,
    OM_STD | OM_BILERP | OM_Z_COMPARE | OM_Z_UPDATE | OM_PERSP,
    OM_1CYC | OM_TF0_RGB | OM_TF1_RGB | OM_RGB_DITHER(0) | OM_ALPHA_DITHER(3) | OM_BILERP | OM_Z_COMPARE | OM_Z_UPDATE,
    OM_2CYC | OM_TF0_RGB | OM_TF1_RGB | OM_RGB_DITHER(0) | OM_ALPHA_DITHER(3) | OM_BILERP | OM_IMAGE_READ |
        OM_FORCE_BLEND | OM_Z_COMPARE | OM_Z_UPDATE | OM_BLEND0(0, 2, 3, 0) | OM_BLEND1(0, 0, 1, 0),
    OM_2CYC | OM_TF0_RGB | OM_TF1_RGB | OM_RGB_DITHER(1) | OM_ALPHA_DITHER(3) | OM_IMAGE_READ | OM_FORCE_BLEND |
        OM_Z_COMPARE | OM_BLEND0(0, 2, 3, 0) | OM_BLEND1(0, 0, 1, 0),
    OM_STD | OM_BILERP | OM_IMAGE_READ | OM_FORCE_BLEND | OM_BLEND0(0, 0, 1, 0) | OM_BLEND1(0, 0, 1, 0),
    OM_STD | OM_BILERP | OM_IMAGE_READ | OM_BLEND0(0, 0, 1, 0) | OM_BLEND1(0, 0, 1, 0) | OM_CVG_X_ALPHA | OM_AA |
        OM_Z_COMPARE | OM_Z_UPDATE | OM_PERSP,
    OM_STD | OM_BILERP | OM_ALPHA_COMPARE | OM_Z_COMPARE | OM_Z_UPDATE | OM_PERSP | OM_TLUT,
    OM_STD | OM_BILERP | OM_MID | OM_TLUT | OM_TLUT_IA,
    OM_2CYC | OM_TF0_RGB | OM_TF1_RGB | OM_BILERP | OM_LOD | OM_PERSP | OM_Z_COMPARE | OM_Z_UPDATE,
    OM_STD | OM_RGB_DITHER(2) | OM_ALPHA_DITHER(2) | OM_ALPHA_COMPARE | OM_DITHER_ALPHA,
};
static const int mx_cc[][8] = {
    { CC_TEX0, CC_ZERO_A, CC_SHADE, CC_ZERO_ADD, AC_ZERO, AC_ZERO, AC_ZERO, AC_SHADE },
    { CC_TEX0, CC_ZERO_A, CC_SHADE, CC_ZERO_ADD, AC_TEX0, AC_ZERO, AC_SHADE, AC_ZERO },
    { CC_TEX0, CC_ZERO_A, CC_PRIM, CC_ZERO_ADD, AC_ZERO, AC_ZERO, AC_ZERO, AC_PRIM },
    { CC_ZERO_A, CC_ZERO_A, CC_ZERO_MUL, CC_TEX0, AC_ZERO, AC_ZERO, AC_ZERO, AC_TEX0 },
    { CC_ZERO_A, CC_ZERO_A, CC_ZERO_MUL, CC_SHADE, AC_ZERO, AC_ZERO, AC_ZERO, AC_SHADE },
    { CC_PRIM, CC_ENV, CC_TEX0, CC_ENV, AC_PRIM, AC_ZERO, AC_TEX0, AC_ZERO },
    { CC_TEX1, CC_TEX0, CC_LODFRAC, CC_TEX0, AC_TEX1, AC_TEX0, AC_LODFRAC, AC_TEX0 },
    { CC_TEX0, CC_K4, CC_K5, CC_TEX0, AC_ZERO, AC_ZERO, AC_ZERO, AC_ONE },
};

/* A triangle from random fixed-point coefficients (as the RSP would send them). */
static void mx_triangle(rdp_list *l, int fbw, int fbh)
{
    const int shade = mx(4) != 0, tex = mx(4) != 0, zb = mx(2);
    const int yh = mx(fbh * 4 + 40), ym = yh + mx(200), yl = ym + mx(200);
    /* one random draw per declarator: the order of calls inside an expression is unspecified */
    const int32_t xhi = mx_range(-20, fbw + 20), xhf = mx(65536), xmo = mx_range(-8, 8);
    const int32_t xli = mx_range(-20, fbw + 20), xlf = mx(65536);
    const int32_t xh = xhi * 65536 + xhf, xm = xh + xmo * 65536, xl = xli * 65536 + xlf;
    const int32_t dxh = mx_range(-3 * 65536, 3 * 65536), dxm = mx_range(-3 * 65536, 3 * 65536);
    const int32_t dxl = mx_range(-3 * 65536, 3 * 65536);
    const int lft = mx(2), level = mx(8), tile = mx(8) < 6 ? mx(2) : mx(8);
    L(l, OP(0x08 | shade << 2 | tex << 1 | zb) | (u64)lft << 55 | (u64)level << 51 | (u64)tile << 48 |
             (u64)(yl & 0x3FFF) << 32 | (u64)(ym & 0x3FFF) << 16 | (u64)(yh & 0x3FFF));
    L(l, (u64)(uint32_t)xl << 32 | (uint32_t)dxl);
    L(l, (u64)(uint32_t)xh << 32 | (uint32_t)dxh);
    L(l, (u64)(uint32_t)xm << 32 | (uint32_t)dxm);
    int32_t a[8], dx[8], de[8], dy[8];
    for (int k = 0; k < 4; k++) { /* shade: 0..255 with some overshoot */
        const int32_t ai = mx_range(-16, 272), af = mx(65536);
        a[k] = ai * 65536 + af;
        dx[k] = mx_range(-8 * 65536, 8 * 65536);
        de[k] = mx_range(-8 * 65536, 8 * 65536);
        dy[k] = mx_range(-8 * 65536, 8 * 65536);
    }
    const int big = mx(3) == 0;
    for (int k = 4; k < 6; k++) { /* S, T (s10.5 in the top half) */
        a[k] = mx_range(-2048, big ? 16384 : 4096) * 65536;
        dx[k] = mx_range(-64, 64) * 65536 / (big ? 1 : 4);
        de[k] = mx_range(-64, 64) * 65536 / (big ? 1 : 4);
        dy[k] = mx_range(-64, 64) * 65536 / (big ? 1 : 4);
    }
    a[6] = mx(10) == 0 ? mx_range(-4096, 4096) * 65536 : mx_range(1024, 32767) * 65536; /* W */
    dx[6] = mx_range(-256, 256) * 65536;
    de[6] = mx_range(-256, 256) * 65536;
    dy[6] = mx_range(-256, 256) * 65536;
    const int32_t zi = mx_range(0, 0x7FFF), zf = mx(65536);
    a[7] = zi * 65536 + zf; /* Z */
    dx[7] = mx_range(-512, 512) * 65536;
    de[7] = mx_range(-512, 512) * 65536;
    dy[7] = mx_range(-512, 512) * 65536;
    if (shade) {
        L(l, pack4(a[0], a[1], a[2], a[3], 1));
        L(l, pack4(dx[0], dx[1], dx[2], dx[3], 1));
        L(l, pack4(a[0], a[1], a[2], a[3], 0));
        L(l, pack4(dx[0], dx[1], dx[2], dx[3], 0));
        L(l, pack4(de[0], de[1], de[2], de[3], 1));
        L(l, pack4(dy[0], dy[1], dy[2], dy[3], 1));
        L(l, pack4(de[0], de[1], de[2], de[3], 0));
        L(l, pack4(dy[0], dy[1], dy[2], dy[3], 0));
    }
    if (tex) {
        L(l, pack4(a[4], a[5], a[6], 0, 1));
        L(l, pack4(dx[4], dx[5], dx[6], 0, 1));
        L(l, pack4(a[4], a[5], a[6], 0, 0));
        L(l, pack4(dx[4], dx[5], dx[6], 0, 0));
        L(l, pack4(de[4], de[5], de[6], 0, 1));
        L(l, pack4(dy[4], dy[5], dy[6], 0, 1));
        L(l, pack4(de[4], de[5], de[6], 0, 0));
        L(l, pack4(dy[4], dy[5], dy[6], 0, 0));
    }
    if (zb) {
        L(l, (u64)(uint32_t)a[7] << 32 | (uint32_t)dx[7]);
        L(l, (u64)(uint32_t)de[7] << 32 | (uint32_t)dy[7]);
    }
}

static void mx_list(rdp_list *l)
{
    const int fbw = 32 + mx(130), fbh = 16 + mx(100);
    int fbsize = mx(8) == 0 ? SZ32 : mx(10) == 0 ? SZ8 : SZ16;
    l->n = 0;
    L(l, c_color_image(F_RGBA, fbsize, fbw, FB_ADDR));
    L(l, c_z_image(ZB_ADDR));
    if (mx(6) == 0) {
        const int x0 = mx(40), y0 = mx(40), x1 = mx_max(fbw * 4 - mx(40), 0), y1 = mx_max(fbh * 4 - mx(40), 0);
        L(l, c_scissor_field(x0, y0, x1, y1, mx(2)));
    } else if (mx(3) == 0) {
        const int x0 = mx(80), y0 = mx(80), x1 = mx_max(fbw * 4 - mx(80), 0), y1 = mx_max(fbh * 4 - mx(80), 0);
        L(l, c_scissor(x0, y0, x1, y1));
    } else {
        L(l, c_scissor(0, 0, fbw * 4, fbh * 4));
    }
    const int nc = 20 + mx(120);
    for (int i = 0; i < nc; i++) {
        const int k = mx(24);
        if (k == 0) { /* texture load through tile 7 */
            const int fmt = mx(5), sz = mx(4), w = 8 << mx(4);
            const uint32_t addr = TEX_ADDR + (uint32_t)mx(4096);
            L(l, c_tex_image(fmt, sz, w, addr));
            const int line = 1 + mx(16), tm = mx(512);
            L(l, c_set_tile(fmt, sz, line, tm, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0));
            const int op = mx(4);
            if (op == 0) {
                const int sh = mx(2048), dxt = mx(4096);
                L(l, c_load_block(7, 0, 0, sh, dxt));
            } else if (op == 1) {
                const int sl = mx(64) * 4, sh = mx(256) * 4;
                L(l, c_load_tlut(7, sl, 0, sh, 0));
            } else {
                const int sl = mx(16) * 4, tl = mx(16) * 4, sh = (16 + mx(48)) * 4, th = (16 + mx(48)) * 4;
                L(l, c_load_tile(7, sl, tl, sh, th));
            }
        } else if (k <= 2) {
            int f[14];
            const int t = mx(8) < 6 ? mx(2) : mx(8);
            if (mx(2)) {
                const int lim[14] = { 5, 4, 16, 512, 1, 16, 2, 2, 11, 16, 2, 2, 11, 16 };
                for (int j = 0; j < 14; j++)
                    f[j] = j == 4 ? t : mx(lim[j]) + (j == 2);
            } else {
                f[0] = mx(5), f[1] = mx(4), f[2] = 1 + mx(16), f[3] = mx(512), f[4] = t, f[5] = mx(16);
                f[6] = mx(4) == 0, f[7] = mx(4) == 0, f[8] = mx(6), f[9] = mx(4) ? 0 : mx(16);
                f[10] = mx(4) == 0, f[11] = mx(4) == 0, f[12] = mx(6), f[13] = mx(4) ? 0 : mx(16);
            }
            L(l, c_set_tile(f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13]));
        } else if (k == 3) {
            const int t = mx(8) < 6 ? mx(2) : mx(8);
            int c[4];
            const int wide = mx(2);
            for (int j = 0; j < 4; j++)
                c[j] = wide ? mx(4096) : j < 2 ? mx(64) : mx(256) + 16;
            L(l, c_tile_size(t, c[0], c[1], c[2], c[3]));
        } else if (k <= 5) {
            u64 m;
            if (mx(4) == 0) {
                const u64 hi = mx_rnd();
                m = (hi << 32 | mx_rnd()) & 0x00FFFFFFFFFFFFFFull;
            } else {
                m = mx_modes[mx((int)(sizeof mx_modes / sizeof mx_modes[0]))];
                if (mx(2))
                    m ^= 1ull << mx(56);
                if (mx(3) == 0)
                    m ^= 1ull << mx(56);
            }
            if (mx(12) == 0)
                m = (m & ~(3ull << 52)) | OM_COPY;
            else if (mx(16) == 0)
                m = (m & ~(3ull << 52)) | OM_FILL;
            else if (((m >> 52) & 3) >= 2)
                m &= ~(2ull << 52);
            L(l, c_modes(m));
        } else if (k == 6) {
            if (mx(3) == 0) {
                const u64 hi = mx_rnd();
                L(l, OP(0x3C) | ((hi << 32 | mx_rnd()) & 0x00FFFFFFFFFFFFFFull));
            } else {
                const int *a = mx_cc[mx(8)], *b = mx_cc[mx(8)];
                if (mx(2))
                    L(l, c_combine1(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]));
                else
                    L(l, c_combine(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], b[0], b[1], b[2], b[3], b[4], b[5],
                                   b[6], b[7]));
            }
        } else if (k == 7) {
            const int w = mx(6);
            const uint32_t c = mx_rnd();
            if (w == 0) {
                const int ml = mx(32), lf = mx(256);
                L(l, c_prim(ml, lf, c));
            } else if (w == 1) {
                L(l, c_env(c));
            } else if (w == 2) {
                L(l, c_blend(c));
            } else if (w == 3) {
                L(l, c_fog(c));
            } else if (w == 4) {
                L(l, c_fill_color(c));
            } else {
                const int z = mx(0x8000), dz = mx(0x10000);
                L(l, c_prim_depth(z, dz));
            }
        } else if (k == 8) {
            int v[6];
            const int kind = mx(2) ? 0 : mx(2) ? 1 : 2;
            if (kind == 0) {
                for (int j = 0; j < 6; j++)
                    v[j] = mx(512);
                L(l, c_convert(v[0], v[1], v[2], v[3], v[4], v[5]));
            } else if (kind == 1) {
                v[0] = mx(4096), v[1] = mx(256), v[2] = mx(256);
                L(l, c_key_r(v[0], v[1], v[2]));
            } else {
                v[0] = mx(4096), v[1] = mx(4096), v[2] = mx(256), v[3] = mx(256), v[4] = mx(256), v[5] = mx(256);
                L(l, c_key_gb(v[0], v[1], v[2], v[3], v[4], v[5]));
            }
        } else if (k <= 10) {
            const int xh = mx_max((mx(fbw + 20) - 10) * 4 + mx(4), 0);
            const int yh = mx_max((mx(fbh + 20) - 10) * 4 + mx(4), 0);
            const int w = mx(60), h = mx(40);
            int dsdx = mx(4) == 0 ? (int16_t)mx_rnd() : (mx(2) ? 1 << 10 : 1 << (6 + mx(6)));
            const int dtdy = mx(4) == 0 ? (int16_t)mx_rnd() : (mx(2) ? 1 << 10 : 1 << (6 + mx(6)));
            if (mx(3) == 0)
                dsdx = -dsdx;
            const int flip = mx(4) == 0, tile = mx(8) < 6 ? mx(2) : mx(8);
            const int xl = xh + w * 4 + mx(4), yl = yh + h * 4 + mx(4);
            const int s0 = (int16_t)(mx(4) ? mx(64 * 32) : (int)mx_rnd());
            const int t0 = (int16_t)(mx(4) ? mx(64 * 32) : (int)mx_rnd());
            c_texrect(l, flip, tile, xh, yh, xl, yl, s0, t0, dsdx, dtdy);
        } else if (k == 11) {
            const int x = mx(fbw), y = mx(fbh);
            const int x0 = x * 4 + mx(4), y0 = y * 4 + mx(4), x1 = (x + mx(50)) * 4 + mx(4), y1 = (y + mx(30)) * 4 + mx(4);
            L(l, c_fill_rect(x0, y0, x1, y1));
        } else {
            mx_triangle(l, fbw, fbh);
        }
        if (mx(30) == 0) {
            fbsize = mx(8) == 0 ? SZ32 : mx(10) == 0 ? SZ8 : SZ16;
            L(l, c_color_image(F_RGBA, fbsize, fbw, FB_ADDR));
        }
    }
    L(l, c_sync_full());
}

static uint32_t mx_run(int threads, int seed)
{
    fresh("mode matrix");
    if (threads > 1)
        n64_rdp_set_threads(G.dp, threads);
    mx_s = 0x9E3779B9u * (uint32_t)(seed + 1);
    for (uint32_t a = 0; a < 0x30000; a++) {
        G.ram[FB_ADDR + a] = (uint8_t)mx_rnd();
        G.ram[ZB_ADDR + a] = mx(2) ? (uint8_t)mx_rnd() : 0xFF;
    }
    for (uint32_t a = 0; a < 0x6000; a++)
        G.ram[TEX_ADDR + a] = (uint8_t)mx_rnd();
    mx_list(&scene);
    rig_run(&G, &scene);
    return fnv1a(G.ram + FB_ADDR, 0x40000) ^ fnv1a(G.ram + ZB_ADDR, 0x40000) * 3;
}

static void t_matrix(void)
{
    uint32_t h = 0;
    int same = 1;
    for (int i = 0; i < 64; i++) {
        const uint32_t h1 = mx_run(1, i);
        if (i % 4 == 0)
            same &= mx_run(3, i) == h1;
        h = (h ^ h1) * 0x01000193u;
    }
    cur = "mode matrix";
    check(same, "1 thread vs 3 threads");
    if (GOLDEN_MATRIX)
        expect("hash", h, GOLDEN_MATRIX);
    else
        printf("mode matrix hash 0x%08X\n", h);
}

/* The homebrew gfx.z64 frame as libdragon's rdpq emits it (fill and copy mode fix-ups included), with its checks. */
static void t_gfx(void)
{
    fresh("gfx.z64 replica");
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            n64_wbe16(G.ram + TEX_ADDR + (y * 16 + x) * 2,
                      ((x >> 2) ^ (y >> 2)) & 1 ? rgba16(255, 255, 255, 1) : rgba16(255, 0, 128, 1));
    frame16(320, 240);
    L(&LS, c_modes(OM_FILL));
    L(&LS, c_fill_color(0x00010001));
    L(&LS, c_fill_rect(0, 0, 319 * 4, 239 * 4));
    L(&LS, c_fill_color(0xF801F801));
    L(&LS, c_fill_rect(16 * 4, 16 * 4, 79 * 4, 63 * 4));
    L(&LS, c_fill_color(0x003F003F));
    L(&LS, c_fill_rect(40 * 4, 80 * 4, 139 * 4, 119 * 4));
    load_rgba16(16, 16, TEX_ADDR, 0, 0, 0);
    L(&LS, c_modes(OM_COPY));
    c_texrect(&LS, 0, 0, 120 * 4, 16 * 4, 135 * 4, 31 * 4, 0, 0, 4 << 10, 1 << 10);
    L(&LS, c_modes(OM_STD));
    L(&LS, CC_PASS(CC_TEX0, AC_TEX0));
    c_texrect(&LS, 0, 0, 200 * 4, 16 * 4, 264 * 4, 80 * 4, 0, 0, 1 << 8, 1 << 8);
    L(&LS, c_modes(OM_STD));
    L(&LS, CC_PASS(CC_SHADE, AC_SHADE));
    vtx a = VC(160, 130, 255, 0, 0, 255), b = VC(300, 220, 0, 255, 0, 255), c = VC(40, 220, 0, 0, 255, 255);
    c_triangle(&LS, 1, 0, 0, 0, 0, &a, &b, &c);
    L(&LS, c_sync_full());
    go();
    expect_px16(320, 20, 20, rgba16(255, 0, 0, 1));
    expect_px16(320, 100, 100, rgba16(0, 0, 255, 1));
    expect("clear colour", (uint32_t)(fb16(&G, 320, 5, 5) >> 1), 0);
    expect_px16(320, 121, 17, rgba16(255, 0, 128, 1));
    expect_px16(320, 124, 17, rgba16(255, 255, 255, 1));
    expect_px16(320, 200 + 17, 16 + 1, rgba16(255, 255, 255, 1)); /* scaled 4x: texel (4,0) */
    uint16_t g = fb16(&G, 320, 160, 200);
    check((g >> 11) > 2 && ((g >> 6) & 31) > 2 && ((g >> 1) & 31) > 2, "gouraud: all channels present");
    expect("interrupt", (uint32_t)G.irqs, 1);
    uint32_t h = fnv1a(G.ram + FB_ADDR, 320 * 240 * 2);
    if (GOLDEN_GFX)
        expect("frame hash", h, GOLDEN_GFX);
    else
        printf("gfx frame hash 0x%08X\n", h);
}

int main(void)
{
    t_regs();
    t_xbus();
    t_fill();
    t_copy();
    t_tri_cover();
    t_shade();
    t_formats();
    t_tmem();
    t_wrap();
    t_bilinear();
    t_combiner();
    t_blender();
    t_dither();
    t_zbuf();
    t_persp();
    t_lod();
    t_scenes();
    t_matrix();
    t_gfx();
    if (G.dp)
        rig_close(&G);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("n64_rdp: all tests passed\n");
    return 0;
}
