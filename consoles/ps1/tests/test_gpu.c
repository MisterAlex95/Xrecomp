/* PS1 GPU tests (XBA-165): synthetic GP0 / GP1 streams, one per feature, with the expected pixels worked out by hand
 * from psx-spx; GPUSTAT / GPUREAD behaviour; transfers with odd sizes; a replica of the homebrew gfx.exe frame; golden
 * hashes of VRAM regions (regression guards: update them only for a deliberate change). */
#include "gpu_scene.h"

#include <stdio.h>
#include <string.h>

static int failures;
static const char *cur;
static ps1_gpu *G;

/* FNV-1a of busy scenes (320x240 at (0,0)); regression guards. */
#ifndef GOLDEN_BUSY
#define GOLDEN_BUSY 0xA2902495u
#endif
#ifndef GOLDEN_GFX
#define GOLDEN_GFX 0x42834BDEu
#endif

static int irq_count, irq_last;
static void on_irq(void *ctx, int irq)
{
    (void)ctx;
    irq_count++;
    irq_last = irq;
}

static void fresh(const char *name)
{
    cur = name;
    if (G)
        ps1_gpu_destroy(G);
    G = ps1_gpu_create((ps1_irq_sink){ on_irq, NULL });
    gpu_open(G);
}

static uint16_t px(int x, int y) { return ps1_gpu_vram(G)[(y & 511) * 1024 + (x & 1023)]; }

static void check(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s: %s\n", cur, what);
        failures++;
    }
}

static void expect_px(int x, int y, unsigned want)
{
    if (px(x, y) != want) {
        fprintf(stderr, "FAIL %s: pixel (%d,%d) = 0x%04X, want 0x%04X\n", cur, x, y, px(x, y), want);
        failures++;
    }
}

static void expect_u32(const char *what, uint32_t got, uint32_t want)
{
    if (got != want) {
        fprintf(stderr, "FAIL %s: %s = 0x%08X, want 0x%08X\n", cur, what, got, want);
        failures++;
    }
}

/* Number of pixels != 0 in a region. */
static int count_set(int x, int y, int w, int h)
{
    int n = 0;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            n += px(x + i, y + j) != 0;
    return n;
}

/* ---- GPUSTAT, GP1, GPUREAD --------------------------------------------------------------------------------------- */
static void t_status(void)
{
    fresh("reset status");
    ps1_gpu_gp1(G, 0);
    expect_u32("GPUSTAT", ps1_gpu_stat(G), 0x14802000);

    fresh("GP1 display bits");
    ps1_gpu_gp1(G, 0x03000000); /* display on */
    check(!(ps1_gpu_stat(G) & 1u << 23), "display enable clears bit 23");
    ps1_gpu_gp1(G, 0x08000000 | 0x7F); /* 368 wide, 480i, PAL, 24-bit, hres1 = 3 */
    uint32_t s = ps1_gpu_stat(G);
    expect_u32("mode bits 16-22", s & 0x7F0000, 1u << 16 | 3u << 17 | 1u << 19 | 1u << 20 | 1u << 21 | 1u << 22);
    ps1_gpu_timing t;
    ps1_gpu_get_timing(G, &t);
    check(t.pal == 1 && t.interlaced == 1 && t.dot_divider == 7 && t.lines == 314 && t.cycles_per_line == 3406,
          "timing PAL 368 interlaced");
    ps1_gpu_gp1(G, 0x08000001);
    ps1_gpu_get_timing(G, &t);
    check(t.pal == 0 && t.dot_divider == 8 && t.lines == 263 && t.cycles_per_line == 3413, "timing NTSC 320");

    fresh("GP0 E1 / E6 in GPUSTAT");
    GP0(G, 0xE1000000 | 0x7FF, 0xE6000003);
    expect_u32("bits 0-12", ps1_gpu_stat(G) & 0x9FFF, 0x1FFF);
    GP0(G, 0xE1000800); /* texture disable needs GP1(09h) */
    check(!(ps1_gpu_stat(G) & 1u << 15), "texture disable ignored without GP1(09h)");
    ps1_gpu_gp1(G, 0x09000001);
    GP0(G, 0xE1000800);
    check(ps1_gpu_stat(G) & 1u << 15, "texture disable with GP1(09h)");

    fresh("DMA direction / bit 25");
    for (int d = 0; d < 4; d++) {
        ps1_gpu_gp1(G, 0x04000000 | (uint32_t)d);
        s = ps1_gpu_stat(G);
        check((int)((s >> 29) & 3) == d, "direction bits");
        check(((s >> 25) & 1) == (d == 1 || d == 2), "bit 25 per direction");
    }
    GP0(G, 0xC0000000, XY(0, 0), XY(2, 1)); /* VRAM->CPU pending */
    s = ps1_gpu_stat(G);
    check((s >> 25 & 1) && (s >> 27 & 1), "dir 3: bit 25 = bit 27 while reading");
    ps1_gpu_read(G);
    check(!(ps1_gpu_stat(G) >> 27 & 1), "bit 27 drops when the read is done");

    fresh("command ready bit 26");
    check(ps1_gpu_stat(G) >> 26 & 1, "idle");
    GP0(G, 0x20FF0000, XY(0, 0));
    check(!(ps1_gpu_stat(G) >> 26 & 1), "mid-command");
    GP0(G, XY(10, 0), XY(0, 10));
    check(ps1_gpu_stat(G) >> 26 & 1, "after command");
    GP0(G, 0x20FF0000, XY(0, 0));
    ps1_gpu_gp1(G, 0x01000000); /* reset FIFO drops the half command */
    check(ps1_gpu_stat(G) >> 26 & 1, "after GP1(01h)");
    GP0(G, 0x02FFFFFF, XY(0, 100), XY(16, 1));
    expect_px(0, 100, 0x7FFF);

    fresh("IRQ 1Fh");
    irq_count = 0;
    GP0(G, 0x1F000000);
    check(irq_count == 1 && irq_last == PS1_IRQ_GPU, "raised once");
    check(ps1_gpu_stat(G) >> 24 & 1, "bit 24 set");
    GP0(G, 0x1F000000);
    check(irq_count == 1, "not raised again while pending");
    ps1_gpu_gp1(G, 0x02000000);
    check(!(ps1_gpu_stat(G) >> 24 & 1), "acknowledged");

    fresh("GP1(10h) info");
    GP0(G, 0xE2012345, 0xE3012345, 0xE4054321, 0xE5123456);
    ps1_gpu_gp1(G, 0x10000002);
    expect_u32("texture window", ps1_gpu_read(G), 0x12345);
    ps1_gpu_gp1(G, 0x10000003);
    expect_u32("area top-left", ps1_gpu_read(G), 0x12345);
    ps1_gpu_gp1(G, 0x10000004);
    expect_u32("area bottom-right", ps1_gpu_read(G), 0x54321);
    ps1_gpu_gp1(G, 0x10000005);
    expect_u32("offset", ps1_gpu_read(G), 0x123456);
    ps1_gpu_gp1(G, 0x10000007);
    expect_u32("version", ps1_gpu_read(G), 2);
    ps1_gpu_gp1(G, 0x10000000);
    expect_u32("index 0 keeps the latch", ps1_gpu_read(G), 2);

    fresh("scanline bits 13 / 31");
    ps1_gpu_gp1(G, 0x08000001); /* 240p */
    ps1_gpu_scanline(G, 0);     /* vblank (range 16..256) */
    check(!(ps1_gpu_stat(G) >> 31), "bit 31 = 0 in vblank");
    ps1_gpu_scanline(G, 17);
    check(ps1_gpu_stat(G) >> 31, "240p: odd line");
    ps1_gpu_scanline(G, 18);
    check(!(ps1_gpu_stat(G) >> 31), "240p: even line");
    check(ps1_gpu_stat(G) >> 13 & 1, "bit 13 = 1 when not interlaced");
    ps1_gpu_gp1(G, 0x08000025); /* 480i */
    int fields[4];
    for (int f = 0; f < 4; f++) {
        for (int l = 0; l < 263; l++) {
            ps1_gpu_scanline(G, l);
            if (l == 100) {
                fields[f] = (int)(ps1_gpu_stat(G) >> 31);
                check(((ps1_gpu_stat(G) >> 13) & 1) == (uint32_t)fields[f], "480i: bit 13 = field");
            }
        }
    }
    check(fields[0] != fields[1] && fields[1] != fields[2] && fields[2] != fields[3], "480i: field alternates");
}

/* ---- Transfers ---------------------------------------------------------------------------------------------------- */
static void t_transfers(void)
{
    fresh("CPU->VRAM odd size, GPUREAD");
    uint16_t img[15];
    for (int i = 0; i < 15; i++)
        img[i] = (uint16_t)(0x100 + i);
    upload(G, 1021, 510, 5, 3, img); /* wraps right and bottom */
    check(ps1_gpu_stat(G) >> 26 & 1, "back to commands after ceil(15/2) = 8 words");
    expect_px(1021, 510, 0x100);
    expect_px(1023, 510, 0x102);
    expect_px(0, 510, 0x103);
    expect_px(1, 510, 0x104);
    expect_px(1021, 511, 0x105);
    expect_px(1, 0, 0x10E);
    GP0(G, 0x02000000 | 0x0000FF, XY(16, 16), XY(16, 1)); /* the next command runs: no word was swallowed */
    expect_px(16, 16, 0x001F);

    GP0(G, 0xC0000000, XY(1021, 510), XY(5, 3));
    uint32_t words[8];
    ps1_gpu_dma_read(G, words, 8);
    expect_u32("word 0", words[0], 0x01010100);
    expect_u32("word 2", words[2], 0x01050104);
    expect_u32("word 7 (half)", words[7], 0x0000010E);
    check(!(ps1_gpu_stat(G) >> 27 & 1), "read done");
    expect_u32("latch after the end", ps1_gpu_read(G), 0x0000010E);

    fresh("transfer size 0 = max, masks");
    GP0(G, 0xE6000001); /* set mask */
    GP0(G, 0xA0000000, XY(0, 0), XY(1, 1), 0x00001234);
    expect_px(0, 0, 0x9234);
    GP0(G, 0xE6000002); /* check mask */
    GP0(G, 0xA0000000, XY(0, 0), XY(2, 1), 0x56781111);
    expect_px(0, 0, 0x9234);
    expect_px(1, 0, 0x5678);

    fresh("fill rectangle");
    GP0(G, 0xE3000000 | 100, 0xE4000000 | 200 << 10 | 200, 0xE6000003); /* ignored by fill */
    GP0(G, 0x020000FF, XY(1015, 5), XY(17, 2)); /* x 1008, width 32, wraps */
    expect_px(1007, 5, 0);
    expect_px(1008, 5, 0x001F);
    expect_px(1023, 6, 0x001F);
    expect_px(15, 6, 0x001F);
    expect_px(16, 6, 0);
    expect_px(1008, 7, 0);
    GP0(G, 0x0200FF00, XY(0, 511), XY(16, 2)); /* y wraps */
    expect_px(3, 0, 0x03E0);

    fresh("VRAM->VRAM copy");
    uint16_t src[6] = { 1, 2, 3, 4, 5, 0x8006 };
    upload(G, 10, 10, 3, 2, src);
    GP0(G, 0x80000000, XY(10, 10), XY(1022, 20), XY(3, 2));
    expect_px(1022, 20, 1);
    expect_px(1023, 20, 2);
    expect_px(0, 20, 3);
    expect_px(0, 21, 0x8006);
    GP0(G, 0xE6000001);
    GP0(G, 0x80000000, XY(10, 10), XY(50, 50), XY(1, 1));
    expect_px(50, 50, 0x8001);
    GP0(G, 0xE6000002);
    GP0(G, 0x80000000, XY(11, 10), XY(50, 50), XY(1, 1));
    expect_px(50, 50, 0x8001);
    GP0(G, 0xE6000000);
    GP0(G, 0x80000000, XY(10, 10), XY(11, 10), XY(3, 1)); /* overlapping, row read before written */
    expect_px(11, 10, 1);
    expect_px(12, 10, 2);
    expect_px(13, 10, 3);
}

/* ---- Polygons ----------------------------------------------------------------------------------------------------- */
static void t_fill_rules(void)
{
    fresh("triangle top-left rule");
    GP0(G, 0x20FFFFFF, XY(0, 0), XY(4, 0), XY(0, 4));
    /* rows 0..3, row y covers x < 4 - y: 4 + 3 + 2 + 1 */
    check(count_set(0, 0, 8, 8) == 10, "10 pixels");
    expect_px(3, 0, 0x7FFF);
    expect_px(4, 0, 0);
    expect_px(0, 3, 0x7FFF);
    expect_px(0, 4, 0);
    expect_px(1, 3, 0);

    fresh("quad 10x10, no right column / bottom row, diagonal once");
    GP0(G, 0x2A00001F, XY(0, 0), XY(10, 0), XY(0, 10), XY(10, 10)); /* semi B+F/2 mode 0 would show doubles */
    GP0(G, 0xE1000020);                                               /* mode 1: B + F */
    GP0(G, 0x2A000008, XY(20, 0), XY(30, 0), XY(20, 10), XY(30, 10));
    check(count_set(0, 0, 16, 16) == 100, "100 pixels");
    int ok = 1;
    for (int y = 0; y < 10; y++)
        for (int x = 0; x < 10; x++)
            ok &= px(20 + x, y) == 1; /* 8 >> 3 = 1, drawn once on the shared diagonal */
    check(ok, "semi-transparent quad drawn once per pixel");

    fresh("clockwise and counter-clockwise");
    GP0(G, 0x20FFFFFF, XY(0, 0), XY(0, 4), XY(4, 0));
    check(count_set(0, 0, 8, 8) == 10, "same 10 pixels in the other winding");

    fresh("size limits");
    GP0(G, 0xE5000000 | (uint32_t)(-200 & 0x7FF));
    GP0(G, 0x20FFFFFF, XY(0, 10), XY(1023, 10), XY(0, 11));
    check(count_set(0, 10, 1024, 1) > 0, "dx 1023 drawn");
    GP0(G, 0x20FFFFFF, XY(0, 20), XY(1024, 20), XY(0, 21));
    check(count_set(0, 20, 1024, 1) == 0, "dx 1024 culled");
    GP0(G, 0xE5000000);
    GP0(G, 0x20FFFFFF, XY(100, 0), XY(101, 511), XY(99, 0));
    check(count_set(95, 100, 10, 400) > 0, "dy 511 drawn");
    GP0(G, 0x2000FF00, XY(300, 0), XY(301, 512), XY(299, 0));
    check(count_set(250, 100, 100, 400) == 0, "dy 512 culled");
    fresh("quad halves culled separately");
    GP0(G, 0x28FFFFFF, XY(0, 0), XY(10, 0), XY(0, 10), XY(1030, 10)); /* (v1 v2 v3) has dx 1030 */
    check(count_set(0, 0, 16, 16) == 55, "first triangle only (10+9+..+1)");

    fresh("draw offset sign extension and drawing area");
    GP0(G, 0xE5000000 | (uint32_t)(-5 & 0x7FF) | (uint32_t)(3 & 0x7FF) << 11); /* offset (-5, 3) */
    GP0(G, 0x60FFFFFF, XY(10, 0), XY(2, 2));
    expect_px(5, 3, 0x7FFF);
    expect_px(6, 4, 0x7FFF);
    expect_px(7, 5, 0);
    GP0(G, 0x60FFFFFF, XY(0x7FF, 0x7FF), XY(8, 8)); /* vertex (-1,-1) + offset: x -6..1, y 2..9 */
    expect_px(1, 2, 0x7FFF);
    expect_px(2, 2, 0);
    expect_px(0, 1, 0);
    GP0(G, 0xE5000000, 0xE3000000 | 20 << 10 | 20, 0xE4000000 | 29 << 10 | 29);
    GP0(G, 0x20FF0000, XY(0, 0), XY(100, 0), XY(0, 100));
    check(count_set(0, 0, 100, 100) == 120, "clipped to the inclusive 10x10 area (+ 20 earlier pixels)");
    expect_px(29, 29, 0x7C00);
    expect_px(30, 29, 0);
}

static void t_gouraud_dither(void)
{
    fresh("gouraud vertices");
    GP0(G, 0x300000FF, XY(0, 0), 0x0000FF00, XY(64, 0), 0x00FF0000, XY(0, 64));
    expect_px(0, 0, 0x001F);
    uint16_t p = px(1, 62); /* near the blue corner */
    check((p >> 10 & 31) >= 29 && (p & 31) <= 1, "blue near v2");
    p = px(31, 0);
    check((p & 31) >= 15 && (p & 31) <= 16 && (p >> 5 & 31) >= 15 && (p >> 5 & 31) <= 16,
          "mid edge half red half green");

    fresh("dithering");
    GP0(G, 0xE1000200);
    GP0(G, 0x38808080, XY(0, 0), 0x808080, XY(8, 0), 0x808080, XY(0, 8), 0x808080, XY(8, 8));
    static const int8_t m[4][4] = { { -4, 0, -3, 1 }, { 2, -2, 3, -1 }, { -3, 1, -4, 0 }, { 3, -1, 2, -2 } };
    int ok = 1;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            int c = m[y & 3][x & 3] < 0 ? 15 : 16;
            ok &= px(x, y) == P15(c, c, c);
        }
    check(ok, "gouraud 808080 dithered 15/16 by the matrix");
    GP0(G, 0x28808080, XY(16, 0), XY(24, 0), XY(16, 8), XY(24, 8));
    check(px(16, 0) == P15(16, 16, 16) && px(18, 0) == P15(16, 16, 16), "flat untextured never dithered");
    GP0(G, 0x60808080, XY(32, 0), XY(8, 8));
    check(px(32, 0) == P15(16, 16, 16), "rectangle never dithered");
    GP0(G, 0xE1000000);
    GP0(G, 0x38808080, XY(40, 0), 0x808080, XY(48, 0), 0x808080, XY(40, 8), 0x808080, XY(48, 8));
    check(px(40, 0) == P15(16, 16, 16), "dither off");

    fresh("quad split (v0 v1 v2) (v1 v2 v3)");
    /* gfx.exe's POLY_G4: red, green, blue, white; (240,36) is in (v0 v1 v2) a quarter in from red. */
    GP0(G, 0x380000FF, XY(220, 16), 0x0000FF00, XY(300, 16), 0x00FF0000, XY(220, 96), 0x00FFFFFF, XY(300, 96));
    p = px(240, 36);
    check((p & 31) > 4 && (p >> 5 & 31) > 4 && (p >> 10 & 31) > 4, "(240,36) has all channels");
    p = px(259, 56); /* just left of the centre, on (v0 v1 v2)'s side of the v1-v2 diagonal: little red */
    check((p & 31) <= 1, "no red at the shared diagonal");
    p = px(298, 94); /* near white */
    check((p & 31) >= 28 && (p >> 5 & 31) >= 28 && (p >> 10 & 31) >= 28, "white corner");
}

static void t_textures(void)
{
    fresh("15 bpp texture, modulation, transparency");
    uint16_t tex[4] = { P15(10, 20, 31), 0, P15(31, 0, 0) | 0x8000, P15(1, 2, 3) };
    upload(G, 512, 0, 2, 2, tex); /* page x 8 */
    uint32_t tp = TPAGE(8, 0, 0, 2);
    GP0(G, 0xE1000000 | tp);
    GP0(G, 0x65000000, XY(0, 0), UV(0, 0, 0), XY(2, 2)); /* raw sprite */
    expect_px(0, 0, P15(10, 20, 31));
    expect_px(1, 0, 0);
    expect_px(0, 1, P15(31, 0, 0) | 0x8000);
    expect_px(1, 1, P15(1, 2, 3));
    GP0(G, 0x64404040, XY(10, 0), UV(0, 0, 0), XY(2, 2));
    expect_px(10, 0, P15(5, 10, 15));
    GP0(G, 0x64FFFFFF, XY(20, 0), UV(0, 0, 0), XY(1, 1));
    expect_px(20, 0, P15(19, 31, 31));
    GP0(G, 0x64808080, XY(30, 0), UV(0, 0, 0), XY(1, 1));
    expect_px(30, 0, P15(10, 20, 31));
    GP0(G, 0x2C404040, XY(40, 0), UV(0, 0, 0), XY(41, 0), UV(1, 0, (int)tp), XY(40, 1), UV(0, 1, 0), XY(41, 1),
        UV(1, 1, 0));
    expect_px(40, 0, P15(5, 10, 15));

    fresh("4 bpp CLUT");
    uint16_t t4[2] = { 0x3210, 0x7654 }; /* texels 0..7 in one row */
    upload(G, 576, 256, 2, 1, t4);         /* page x 9, y 256 */
    uint16_t clut[16];
    for (int i = 0; i < 16; i++)
        clut[i] = (uint16_t)(i ? 0x100 + i : 0);
    upload(G, 32, 500, 16, 1, clut);
    tp = TPAGE(9, 1, 0, 0);
    GP0(G, 0x2D000000, XY(0, 0), UV(0, 0, (int)CLUT(32, 500)), XY(8, 0), UV(8, 0, (int)tp), XY(0, 1), UV(0, 1, 0),
        XY(8, 1), UV(8, 1, 0));
    expect_px(0, 0, 0); /* index 0 -> 0000h: transparent */
    for (int i = 1; i < 8; i++)
        expect_px(i, 0, 0x100u + (unsigned)i);
    check((ps1_gpu_stat(G) & 0x1FF) == tp, "polygon texpage written to GPUSTAT");

    fresh("8 bpp CLUT and texture window");
    uint16_t t8[4] = { 0x0201, 0x0403, 0x0605, 0x0807 }; /* u 0..7 = indices 1..8 */
    upload(G, 640, 0, 4, 1, t8);
    uint16_t c8[256];
    for (int i = 0; i < 256; i++)
        c8[i] = (uint16_t)(0x200 + i);
    upload(G, 0, 300, 256, 1, c8);
    GP0(G, 0xE1000000 | TPAGE(10, 0, 0, 1));
    GP0(G, 0x75000000, XY(0, 0), UV(0, 0, (int)CLUT(0, 300))); /* 8x8 raw */
    for (int i = 0; i < 8; i++)
        expect_px(i, 0, 0x201u + (unsigned)i);
    /* window: mask x = 1 (8 texels), offset x = 0 -> u & ~8; u 8..15 read 0..7 */
    GP0(G, 0xE2000001);
    GP0(G, 0x75000000, XY(0, 10), UV(8, 0, (int)CLUT(0, 300)));
    expect_px(0, 10, 0x201);
    expect_px(7, 10, 0x208);
    /* mask x = 2 (16), offset x = 2 -> (u & ~16) | 16: u 0..7 read 16..23 = 0 = index 0 -> c8[0] */
    GP0(G, 0xE2000002 | 2 << 10);
    GP0(G, 0x75000000, XY(0, 20), UV(0, 0, (int)CLUT(0, 300)));
    expect_px(0, 20, 0x200);

    fresh("texture page wrap and rectangle flips");
    uint16_t row[4] = { 1, 2, 3, 4 };
    upload(G, 960, 0, 4, 1, row); /* page x 15 */
    upload(G, 0, 0, 4, 1, row); /* where u 64.. of page 15 wraps to */
    GP0(G, 0xE1000000 | TPAGE(15, 0, 0, 2));
    GP0(G, 0x65000000, XY(100, 100), UV(0, 0, 0), XY(4, 1));
    expect_px(100, 100, 1);
    expect_px(103, 100, 4);
    GP0(G, 0xE1000000 | TPAGE(15, 0, 0, 2) | 1 << 12); /* x flip: u0 + 1, u0, u0 - 1... (ps1-tests gpu/texture-flip) */
    GP0(G, 0x65000000, XY(100, 101), UV(2, 0, 0), XY(4, 1));
    expect_px(100, 101, 4);
    expect_px(103, 101, 1);
    GP0(G, 0xE1000000 | TPAGE(15, 0, 0, 2));
    GP0(G, 0x65000000, XY(100, 103), UV(64, 0, 0), XY(2, 1)); /* x 960 + 64 wraps to 0 */
    expect_px(100, 103, 1);
    expect_px(101, 103, 2);
    GP0(G, 0xE3000000 | 102, 0xE4000000 | 511 << 10 | 1023);
    GP0(G, 0x65000000, XY(100, 102), UV(0, 0, 0), XY(4, 1)); /* clipped left: u starts at 2 */
    expect_px(101, 102, 0);
    expect_px(102, 102, 3);

    fresh("texture disable");
    ps1_gpu_gp1(G, 0x09000001);
    upload(G, 512, 0, 1, 1, tex);
    GP0(G, 0xE1000800 | TPAGE(8, 0, 0, 2));
    GP0(G, 0x6400FF00, XY(1, 0), UV(0, 0, 0), XY(1, 1));
    expect_px(1, 0, 0x03E0);
}

/* ps1-tests uv-interpolate's idea: a quad w pixels wide with u 0..w maps pixel x to texel x exactly, for every w. */
static void t_uv_interpolation(void)
{
    fresh("uv interpolation, widths 1..255");
    static uint16_t row[256];
    for (int i = 0; i < 256; i++)
        row[i] = (uint16_t)(i + 1);
    upload(G, 0, 256, 256, 1, row); /* 15 bpp page (0, 256), row v = 0 */
    uint32_t tp = TPAGE(0, 1, 0, 2);
    int bad = 0;
    for (int w = 1; w < 256; w++) {
        int y = w & 127, x0 = 300;
        GP0(G, 0x2D000000, XY(x0, y), UV(0, 0, 0), XY(x0 + w, y), UV(w, 0, (int)tp), XY(x0, y + 1), UV(0, 0, 0),
            XY(x0 + w, y + 1), UV(w, 0, 0));
        for (int x = 0; x < w; x++)
            bad += px(x0 + x, y) != (uint16_t)(x + 1);
        bad += px(x0 + w, y) != 0 && w >= 128; /* nothing right of the quad (rows reused below 128) */
    }
    check(bad == 0, "texel x at pixel x");

    fresh("rectangle y flip");
    uint16_t col[3] = { 7, 8, 9 };
    upload(G, 960, 0, 1, 3, col);
    GP0(G, 0xE1000000 | TPAGE(15, 0, 0, 2) | 1 << 13);
    GP0(G, 0x65000000, XY(10, 10), UV(0, 2, 0), XY(1, 3));
    expect_px(10, 10, 9);
    expect_px(10, 12, 7);
}

static void t_semi_mask(void)
{
    static const uint16_t want[4] = { 15 | 15 << 5 | 17 << 10, 30 | 30 << 5 | 31 << 10, 10 | 0 << 5 | 0 << 10,
                                      22 | 15 << 5 | 11 << 10 };
    uint16_t bg = P15(20, 10, 4);
    for (int mode = 0; mode < 4; mode++) {
        fresh("semi-transparency untextured");
        GP0(G, 0xE1000000 | (uint32_t)mode << 5);
        GP0(G, 0xA0000000, XY(0, 0), XY(2, 1), bg | (uint32_t)bg << 16);
        GP0(G, 0x62000000 | RGB(80, 160, 248), XY(0, 0), XY(1, 1)); /* F = (10, 20, 31) */
        expect_px(0, 0, want[mode]);

        fresh("semi-transparency textured: bit 15 only");
        uint16_t t[2] = { P15(10, 20, 31) | 0x8000, P15(10, 20, 31) };
        upload(G, 512, 0, 2, 1, t);
        GP0(G, 0xA0000000, XY(0, 0), XY(2, 1), bg | (uint32_t)bg << 16);
        GP0(G, 0x2F000000, XY(0, 0), UV(0, 0, 0), XY(2, 0), UV(2, 0, (int)TPAGE(8, 0, mode, 2)), XY(0, 1),
            UV(0, 1, 0), XY(2, 1), UV(2, 1, 0));
        expect_px(0, 0, want[mode] | 0x8000);
        expect_px(1, 0, P15(10, 20, 31));
        check(((ps1_gpu_stat(G) >> 5) & 3) == (uint32_t)mode, "semi mode from the texpage");
    }

    fresh("mask set / check on polygons and lines");
    GP0(G, 0xE6000001);
    GP0(G, 0x60FF0000, XY(0, 0), XY(4, 1)); /* blue with bit 15 */
    expect_px(0, 0, 0xFC00);
    GP0(G, 0xE6000002);
    GP0(G, 0x6000FF00, XY(2, 0), XY(4, 1));
    expect_px(3, 0, 0xFC00);
    expect_px(4, 0, 0x03E0);
    GP0(G, 0x40FFFFFF, XY(0, 0), XY(5, 0));
    expect_px(1, 0, 0xFC00);
    expect_px(5, 0, 0x7FFF);
    GP0(G, 0xE6000000);
    GP0(G, 0x20FFFFFF, XY(0, 0), XY(4, 0), XY(0, 1));
    expect_px(0, 0, 0x7FFF);
}

static void t_rects_lines(void)
{
    fresh("rectangle sizes");
    GP0(G, 0x680000FF, XY(0, 0));
    GP0(G, 0x700000FF, XY(10, 0));
    GP0(G, 0x780000FF, XY(30, 0));
    GP0(G, 0x600000FF, XY(50, 0), XY(3, 5));
    check(count_set(0, 0, 10, 20) == 1, "1x1");
    check(count_set(10, 0, 20, 20) == 64, "8x8");
    check(count_set(30, 0, 20, 20) == 256, "16x16");
    check(count_set(50, 0, 10, 20) == 15, "3x5");
    GP0(G, 0x600000FF, XY(100, 100), XY(0, 5));
    check(count_set(100, 100, 10, 10) == 0, "zero width draws nothing");

    fresh("lines");
    GP0(G, 0x40FFFFFF, XY(0, 0), XY(5, 0));
    check(count_set(0, 0, 10, 1) == 6, "both ends");
    GP0(G, 0x40FFFFFF, XY(0, 10), XY(3, 13));
    check(px(0, 10) && px(1, 11) && px(2, 12) && px(3, 13) && count_set(0, 10, 5, 5) == 4, "diagonal");
    GP0(G, 0x40FFFFFF, XY(10, 20), XY(10, 20));
    check(count_set(8, 18, 5, 5) == 1, "single point");
    GP0(G, 0x40FFFFFF, XY(0, 30), XY(1024, 30));
    check(count_set(0, 30, 1024, 1) == 0, "dx 1024 culled");
    GP0(G, 0x40FFFFFF, XY(9, 40), XY(5, 42));
    check(count_set(0, 40, 16, 3) == 5, "right to left, 5 pixels");
    GP0(G, 0x500000FF, XY(0, 50), 0x00FF0000, XY(10, 50));
    expect_px(0, 50, 0x001F);
    expect_px(10, 50, 0x7C00);

    fresh("polyline");
    GP0(G, 0x48FFFFFF, XY(0, 0), XY(4, 0), XY(4, 4), 0x55555555);
    check(count_set(0, 0, 8, 8) == 9, "two segments, shared corner");
    GP0(G, 0x020000FF, XY(32, 0), XY(16, 1)); /* next command decoded normally */
    expect_px(32, 0, 0x001F);
    GP0(G, 0x58FF0000, XY(0, 10), 0x0000FF00, XY(10, 10), 0x000000FF, XY(10, 20), 0x50005000);
    expect_px(0, 10, 0x7C00);
    expect_px(10, 10, 0x03E0);
    expect_px(10, 20, 0x001F);
    check(ps1_gpu_stat(G) >> 26 & 1, "gouraud polyline ended by the terminator in the colour slot");
    GP0(G, 0x4AFFFFFF, XY(0, 30), XY(3, 30), 0x5000500F); /* terminator variant */
    check(count_set(0, 30, 8, 1) == 4 && (ps1_gpu_stat(G) >> 26 & 1), "flat polyline");

    fresh("interlaced drawing skips the displayed field");
    ps1_gpu_gp1(G, 0x08000025); /* 480i */
    int field = 0;
    for (int l = 0; l < 263; l++)
        ps1_gpu_scanline(G, l);
    ps1_gpu_scanline(G, 100);
    field = (int)(ps1_gpu_stat(G) >> 31);
    GP0(G, 0x60FFFFFF, XY(0, 0), XY(4, 4));
    check(px(0, field) == 0 && px(0, field ^ 1) == 0x7FFF, "displayed field's rows left alone");
    GP0(G, 0xE1000400);
    GP0(G, 0x60FFFFFF, XY(10, 0), XY(4, 4));
    check(px(10, 0) && px(10, 1), "bit 10 allows both");
}

static void t_display(void)
{
    uint32_t out[640 * 512];
    int w, h;
    fresh("display off");
    check(ps1_gpu_display(G, out, 640, 512, &w, &h) == -1, "-1 when off");

    fresh("display 320x240 15-bit");
    ps1_gpu_gp1(G, 0x03000000);
    ps1_gpu_gp1(G, 0x08000001);
    ps1_gpu_gp1(G, 0x06000000 | 0x260 | (0x260 + 320 * 8) << 12);
    ps1_gpu_gp1(G, 0x07000000 | 0x10 | (0x10 + 240) << 10);
    ps1_gpu_gp1(G, 0x05000000 | 8 | 4 << 10);
    GP0(G, 0xA0000000, XY(8, 4), XY(2, 1), 0x7FFF001F);
    check(ps1_gpu_display(G, out, 640, 512, &w, &h) == 0 && w == 320 && h == 240, "320x240");
    expect_u32("red", out[0], 0xFF0000);
    expect_u32("white", out[1], 0xFFFFFF);
    ps1_gpu_gp1(G, 0x06000000 | 0x260 | (0x260 + 256 * 8) << 12);
    ps1_gpu_display(G, out, 640, 512, &w, &h);
    check(w == 256, "narrower horizontal range crops");
    ps1_gpu_gp1(G, 0x08000027); /* 640x480i */
    ps1_gpu_gp1(G, 0x06000000 | 0x260 | (0x260 + 2560) << 12);
    ps1_gpu_display(G, out, 640, 512, &w, &h);
    check(w == 640 && h == 480, "640x480");
    ps1_gpu_gp1(G, 0x08000008 | 0x40); /* PAL 368 */
    ps1_gpu_gp1(G, 0x07000000 | 0x23 | (0x23 + 288) << 10);
    ps1_gpu_display(G, out, 640, 512, &w, &h);
    check(w == 364 && h == 288, "PAL 368 (364 visible) x 288");

    fresh("display 24-bit");
    ps1_gpu_gp1(G, 0x03000000);
    ps1_gpu_gp1(G, 0x08000011); /* 320, 24-bit */
    ps1_gpu_gp1(G, 0x05000000 | 100);
    /* bytes R0 G0 B0 R1 G1 B1 = 11 22 33 44 55 66 */
    GP0(G, 0xA0000000, XY(100, 0), XY(4, 1), 0x44332211, 0x00006655);
    ps1_gpu_display(G, out, 640, 512, &w, &h);
    expect_u32("24-bit pixel 0", out[0], 0x112233);
    expect_u32("24-bit pixel 1", out[1], 0x445566);
}

/* ---- Scenes ------------------------------------------------------------------------------------------------------- */
/* The homebrew gfx.exe's frame, drawn into g; its 16 bpp texture and CLUT returned. */
static void gfx_scene(ps1_gpu *g, uint16_t tex16[256], uint16_t clut[16])
{
    uint16_t tex4[64];
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            tex16[y * 16 + x] = ((x >> 2) ^ (y >> 2)) & 1 ? P15(31, 31, 31) : P15(31, 0, 16);
    for (int y = 0; y < 16; y++)
        for (int wd = 0; wd < 4; wd++) {
            uint16_t v = 0;
            for (int n = 0; n < 4; n++)
                v |= (uint16_t)(((wd * 4 + n + y) & 15) << (4 * n));
            tex4[y * 4 + wd] = v;
        }
    for (int i = 0; i < 16; i++)
        clut[i] = (uint16_t)(P15(i * 2, 31 - i * 2, 8) | (i ? 0 : 0x8000));
    upload(g, 640, 0, 16, 16, tex16);
    upload(g, 640, 256, 4, 16, tex4);
    upload(g, 640, 480, 16, 1, clut);
    GP0(g, 0xE3000000, 0xE4000000 | 239 << 10 | 319, 0xE5000000, 0xE1000000);
    GP0(g, 0x02202020, XY(0, 0), XY(320, 240));
    GP0(g, 0xE1000000 | TPAGE(10, 1, 0, 0));
    GP0(g, 0x64808080, XY(160, 140), UV(0, 0, (int)CLUT(640, 480)), XY(16, 16));
    GP0(g, 0x2C808080, XY(16, 120), UV(0, 0, 0), XY(112, 120), UV(15, 0, (int)TPAGE(10, 0, 0, 2)), XY(16, 216),
        UV(0, 15, 0), XY(112, 216), UV(15, 15, 0));
    GP0(g, 0x380000FF, XY(220, 16), 0x0000FF00, XY(300, 16), 0x00FF0000, XY(220, 96), 0x00FFFFFF, XY(300, 96));
    GP0(g, 0x2000FF00, XY(120, 16), XY(200, 100), XY(100, 90));
    GP0(g, 0x600000FF, XY(16, 16), XY(64, 48));
}

static void t_gfx_replica(void)
{
    fresh("gfx.exe replica");
    uint16_t tex16[256], clut[16];
    gfx_scene(G, tex16, clut);
    expect_px(20, 20, 0x001F);
    expect_px(150, 60, 0x03E0);
    expect_px(5, 5, 0x1084);
    expect_px(17, 121, tex16[0]);
    expect_px(160, 140, clut[0]); /* texel index 0 = 8000h: drawn (only 0000h is transparent) */
    expect_px(161, 140, clut[1]);
    uint16_t p = px(240, 36);
    check((p & 31) > 4 && (p >> 5 & 31) > 4 && (p >> 10 & 31) > 4, "gouraud (240,36)");
    uint32_t hsh = fnv1a16(ps1_gpu_vram(G), 0, 0, 320, 240);
    if (GOLDEN_GFX && hsh != GOLDEN_GFX) {
        fprintf(stderr, "FAIL gfx replica: hash %08X, want %08X\n", hsh, GOLDEN_GFX);
        failures++;
    }
    printf("gfx replica hash %08X\n", hsh);
}

static void t_busy(void)
{
    fresh("busy scene");
    scene_textures(G);
    scene_busy(G, 400, 12345);
    uint32_t hsh = fnv1a16(ps1_gpu_vram(G), 0, 0, 320, 240);
    if (GOLDEN_BUSY && hsh != GOLDEN_BUSY) {
        fprintf(stderr, "FAIL busy scene: hash %08X, want %08X\n", hsh, GOLDEN_BUSY);
        failures++;
    }
    printf("busy scene hash %08X\n", hsh);
    /* DMA delivers the same words as GP0 writes. */
    ps1_gpu *a = G;
    G = NULL;
    fresh("busy scene via DMA");
    uint32_t words[] = { 0x02202020, XY(0, 0), XY(320, 240) };
    ps1_gpu_dma_write(G, words, 3);
    check(px(5, 5) == 0x1084, "DMA write");
    ps1_gpu_destroy(a);
}

/* ---- Internal resolution upscale (XBA-184) ----------------------------------------------------------------------- */
#ifndef GOLDEN_GFX_2X
#define GOLDEN_GFX_2X 0xB7AB1136u
#endif
#ifndef GOLDEN_GFX_4X
#define GOLDEN_GFX_4X 0xAEAC961Au
#endif
#ifndef GOLDEN_BUSY_2X
#define GOLDEN_BUSY_2X 0xCFF53DF7u
#endif
#ifndef GOLDEN_BUSY_4X
#define GOLDEN_BUSY_4X 0xEAA96334u
#endif

/* FNV-1a of the shadow's (x, y, w, h) native rectangle, at factor n. */
static uint32_t hash_hi(ps1_gpu *g, int n, int x, int y, int w, int h)
{
    const uint16_t *hi = ps1_gpu_vram_hi(g);
    uint32_t hsh = 2166136261u;
    for (int j = 0; j < h * n; j++)
        for (int i = 0; i < w * n; i++) {
            uint16_t p = hi[(size_t)(y * n + j) * (size_t)(1024 * n) + (size_t)(x * n + i)];
            hsh = (hsh ^ (p & 0xFF)) * 16777619u;
            hsh = (hsh ^ (p >> 8)) * 16777619u;
        }
    return hsh;
}

/* Is the shadow's (x, y, w, h) native rectangle the native VRAM's pixels, each an n x n block? (The first difference
 * logged unless quiet.) */
static int hi_blocks(ps1_gpu *g, int n, int x, int y, int w, int h, int quiet)
{
    const uint16_t *v = ps1_gpu_vram(g), *hi = ps1_gpu_vram_hi(g);
    for (int j = 0; j < h * n; j++)
        for (int i = 0; i < w * n; i++) {
            int sx = x * n + i, sy = y * n + j;
            if (hi[(size_t)sy * (size_t)(1024 * n) + (size_t)sx] != v[(sy / n) * 1024 + sx / n]) {
                if (!quiet)
                    fprintf(stderr, "  shadow (%d,%d) = %04X, native (%d,%d) = %04X\n", sx, sy,
                        hi[(size_t)sy * (size_t)(1024 * n) + (size_t)sx], sx / n, sy / n, v[(sy / n) * 1024 + sx / n]);
                return 0;
            }
        }
    return 1;
}
#define hi_is_blocks(g, n, x, y, w, h) hi_blocks(g, n, x, y, w, h, 0)

/* Reads (x, y, w, h) back through GPUREAD (GP0(C0h)). */
static void gpuread(ps1_gpu *g, int x, int y, int w, int h, uint32_t *out)
{
    GP0(g, 0xC0000000, XY(x, y), XY(w, h));
    for (int i = 0; i < (w * h + 1) / 2; i++)
        out[i] = ps1_gpu_read(g);
}

static void t_upscale(void)
{
    static const int factors[2] = { 2, 4 };
    static const uint32_t golden_gfx[2] = { GOLDEN_GFX_2X, GOLDEN_GFX_4X };
    static const uint32_t golden_busy[2] = { GOLDEN_BUSY_2X, GOLDEN_BUSY_4X };
    static uint32_t out[640 * 4 * 512 * 4];
    static uint32_t rd1[320 * 240 / 2], rdn[320 * 240 / 2];
    uint16_t tex16[256], clut[16];

    fresh("upscale factor");
    check(ps1_gpu_upscale(G) == 1, "1x by default");
    check(ps1_gpu_vram_hi(G) == ps1_gpu_vram(G), "no shadow at 1x");
    check(ps1_gpu_set_upscale(G, 2) == 2 && ps1_gpu_upscale(G) == 2, "2x");
    check(ps1_gpu_set_upscale(G, 8) == 4, "8x caps at 4x");
    check(ps1_gpu_set_upscale(G, 3) == 2, "3x is 2x");
    check(ps1_gpu_set_upscale(G, 1) == 1 && ps1_gpu_vram_hi(G) == ps1_gpu_vram(G), "back to 1x");

    for (int f = 0; f < 2; f++) {
        int n = factors[f];
        char name[64];
        /* The native VRAM is what a game reads back: the same bytes as at 1x, through GPUREAD too. */
        snprintf(name, sizeof name, "upscale %dx: native VRAM coherent", n);
        fresh(name);
        ps1_gpu *one = ps1_gpu_create((ps1_irq_sink){ 0 });
        gpu_open(one);
        check(ps1_gpu_set_upscale(G, n) == n, "factor set");
        scene_textures(one);
        scene_textures(G);
        scene_busy(one, 400, 12345);
        scene_busy(G, 400, 12345);
        GP0(one, 0x80000000, XY(0, 0), XY(400, 0), XY(64, 32)); /* VRAM->VRAM copy */
        GP0(G, 0x80000000, XY(0, 0), XY(400, 0), XY(64, 32));
        check(!memcmp(ps1_gpu_vram(one), ps1_gpu_vram(G), 1024 * 512 * 2), "native VRAM = 1x VRAM");
        gpuread(one, 0, 0, 320, 240, rd1);
        gpuread(G, 0, 0, 320, 240, rdn);
        check(!memcmp(rd1, rdn, sizeof rd1), "GPUREAD = 1x GPUREAD");
        check(fnv1a16(ps1_gpu_vram(G), 0, 0, 320, 240) == GOLDEN_BUSY, "native busy scene = 1x golden");
        uint32_t hb = hash_hi(G, n, 0, 0, 320, 240);
        printf("busy scene %dx shadow hash %08X\n", n, hb);
        /* the shadow pass's workers change nothing: none, then three, the same picture */
        for (int th = 0; th <= 3; th += 3) {
            ps1_gpu *t = ps1_gpu_create((ps1_irq_sink){ 0 });
            gpu_open(t);
            ps1_gpu_set_upscale(t, n);
            check(ps1_gpu_set_threads(t, th) == th, "workers started");
            scene_textures(t);
            scene_busy(t, 400, 12345);
            GP0(t, 0x80000000, XY(0, 0), XY(400, 0), XY(64, 32));
            check(hash_hi(t, n, 0, 0, 1024, 512) == hash_hi(G, n, 0, 0, 1024, 512), "same shadow whatever the workers");
            gfx_scene(t, tex16, clut);
            ps1_gpu *u = ps1_gpu_create((ps1_irq_sink){ 0 });
            gpu_open(u);
            ps1_gpu_set_upscale(u, n);
            ps1_gpu_set_threads(u, 3 - th);
            scene_textures(u);
            scene_busy(u, 400, 12345);
            GP0(u, 0x80000000, XY(0, 0), XY(400, 0), XY(64, 32));
            gfx_scene(u, tex16, clut);
            check(hash_hi(t, n, 0, 0, 1024, 512) == hash_hi(u, n, 0, 0, 1024, 512), "gfx scene: same shadow");
            ps1_gpu_destroy(u);
            ps1_gpu_destroy(t);
        }
        if (golden_busy[f] && hb != golden_busy[f]) {
            fprintf(stderr, "FAIL busy scene %dx: hash %08X, want %08X\n", n, hb, golden_busy[f]);
            failures++;
        }
        check(!hi_blocks(G, n, 0, 0, 320, 240, 1), "the shadow has more detail than the native picture");
        /* the copy moved the shadow's blocks: its destination is the source's detail */
        check(hash_hi(G, n, 400, 0, 64, 32) == hash_hi(G, n, 0, 0, 64, 32), "VRAM copy copies the shadow");
        ps1_gpu_destroy(one);

        snprintf(name, sizeof name, "upscale %dx: gfx.exe replica", n);
        fresh(name);
        ps1_gpu_set_upscale(G, n);
        gfx_scene(G, tex16, clut);
        check(fnv1a16(ps1_gpu_vram(G), 0, 0, 320, 240) == GOLDEN_GFX, "native gfx replica = 1x golden");
        uint32_t hg = hash_hi(G, n, 0, 0, 320, 240);
        printf("gfx replica %dx shadow hash %08X\n", n, hg);
        if (golden_gfx[f] && hg != golden_gfx[f]) {
            fprintf(stderr, "FAIL gfx replica %dx: hash %08X, want %08X\n", n, hg, golden_gfx[f]);
            failures++;
        }
        /* axis-aligned flat rectangles, fills and untextured sprites are exact blocks */
        check(hi_is_blocks(G, n, 16, 16, 64, 48), "flat rectangle = blocks");
        check(hi_is_blocks(G, n, 0, 0, 12, 12), "fill = blocks");
        check(hi_is_blocks(G, n, 160, 140, 16, 16), "4 bpp sprite = blocks");

        /* The display reads the shadow: n times the size. */
        ps1_gpu_gp1(G, 0x03000000);
        ps1_gpu_gp1(G, 0x08000001);
        ps1_gpu_gp1(G, 0x06000000 | 0x260 | (0x260 + 320 * 8) << 12);
        ps1_gpu_gp1(G, 0x07000000 | 0x10 | (0x10 + 240) << 10);
        ps1_gpu_gp1(G, 0x05000000);
        int w = 0, h = 0;
        check(ps1_gpu_display(G, out, 640 * n, 512 * n, &w, &h) == 0 && w == 320 * n && h == 240 * n,
              "display n times the size");
        const uint16_t *hi = ps1_gpu_vram_hi(G);
        int same = 1;
        for (int j = 0; j < h && same; j += 7)
            for (int i = 0; i < w; i += 5) {
                uint16_t p = hi[(size_t)j * (size_t)(1024 * n) + (size_t)i];
                uint32_t r = p & 31, gg = (p >> 5) & 31, b = (p >> 10) & 31;
                uint32_t want = (r << 3 | r >> 2) << 16 | (gg << 3 | gg >> 2) << 8 | (b << 3 | b >> 2);
                if (out[(size_t)j * (size_t)w + (size_t)i] != want) {
                    same = 0;
                    break;
                }
            }
        check(same, "display = the shadow");
        check(ps1_gpu_display(G, out, 320 * n - 1, 512, &w, &h) == 0 && w == 320 && h == 240,
              "native picture when the buffer is too small (a thumbnail)");
        ps1_gpu_gp1(G, 0x08000011); /* 24-bit: native pixels repeated */
        GP0(G, 0xA0000000, XY(0, 0), XY(4, 1), 0x44332211, 0x00006655);
        check(ps1_gpu_display(G, out, 640 * n, 512 * n, &w, &h) == 0 && w == 320 * n, "24-bit size");
        expect_u32("24-bit block corner", out[(size_t)(n - 1) * (size_t)w + (size_t)(n - 1)], 0x112233);
        expect_u32("24-bit next pixel", out[n], 0x445566);

        /* CPU->VRAM writes and fills set blocks; sprites flipped and lines are blocks too. */
        snprintf(name, sizeof name, "upscale %dx: writes, flips, lines", n);
        fresh(name);
        ps1_gpu_set_upscale(G, n);
        static uint16_t px16[64 * 16];
        for (int i = 0; i < 64 * 16; i++)
            px16[i] = (uint16_t)(i * 0x1357u + 1);
        upload(G, 640, 0, 16, 16, px16);
        upload(G, 3, 5, 7, 3, px16);
        check(hi_is_blocks(G, n, 0, 0, 16, 16), "CPU->VRAM writes = blocks");
        GP0(G, 0xE1000000 | TPAGE(10, 0, 0, 2) | 1 << 12 | 1 << 13); /* 15 bpp, flipped both ways */
        GP0(G, 0x65808080, XY(100, 50), UV(2, 3, 0), XY(12, 9));
        GP0(G, 0xE1000000 | TPAGE(10, 0, 0, 2));
        GP0(G, 0x65808080, XY(130, 50), UV(1, 1, 0), XY(12, 9));
        check(hi_is_blocks(G, n, 96, 46, 60, 20), "textured sprites (and flipped) = blocks");
        GP0(G, 0x40FFFFFF, XY(10, 100), XY(40, 100));  /* horizontal, vertical, single point */
        GP0(G, 0x4000FF00, XY(60, 120), XY(60, 90));
        GP0(G, 0x40FF0000, XY(70, 70), XY(70, 70));
        check(hi_is_blocks(G, n, 0, 60, 100, 70), "straight lines = blocks");
        GP0(G, 0x40FFFFFF, XY(200, 100), XY(260, 130)); /* a diagonal: 61 native pixels' worth, no gap or overlap */
        const uint16_t *hl = ps1_gpu_vram_hi(G);
        int lit = 0;
        for (int j = 95 * n; j < 135 * n; j++)
            for (int i = 195 * n; i < 265 * n; i++)
                lit += hl[(size_t)j * (size_t)(1024 * n) + (size_t)i] != 0;
        check(lit == 61 * n * n, "diagonal line: 61 x n x n shadow pixels");

        /* A state is the native GPU (the size it had at 1x); loaded, the shadow is made again from its VRAM. */
        snprintf(name, sizeof name, "upscale %dx: save state", n);
        fresh(name);
        ps1_gpu *ref = ps1_gpu_create((ps1_irq_sink){ 0 });
        size_t sz1 = ps1_gpu_state_size(ref);
        ps1_gpu_set_upscale(G, n);
        check(ps1_gpu_state_size(G) == sz1, "state size as at 1x");
        gfx_scene(G, tex16, clut);
        static uint8_t blob[1200000];
        if (sz1 <= sizeof blob) {
            ps1_gpu_state_save(G, blob);
            check(ps1_gpu_state_load(ref, blob, sz1) == 0, "a 1x GPU loads it");
            check(!memcmp(ps1_gpu_vram(ref), ps1_gpu_vram(G), 1024 * 512 * 2), "same VRAM at 1x");
            ps1_gpu *b = ps1_gpu_create((ps1_irq_sink){ 0 });
            ps1_gpu_set_upscale(b, n);
            check(ps1_gpu_state_load(b, blob, sz1) == 0 && ps1_gpu_upscale(b) == n, "an upscaled GPU loads it");
            check(hi_is_blocks(b, n, 0, 0, 1024, 512), "its shadow rebuilt from the VRAM");
            GP0(b, 0x02000000, XY(0, 0), XY(16, 16));
            check(hi_is_blocks(b, n, 0, 0, 16, 16), "and drawn into after the load");
            ps1_gpu_destroy(b);
        } else {
            check(0, "state blob buffer too small");
        }
        ps1_gpu_destroy(ref);
    }

    /* Random streams at 4x: nothing reads or writes outside the shadow (ASan / UBSan), the native VRAM still that of a
     * 1x GPU. */
    fresh("upscale 4x fuzz");
    ps1_gpu *one = ps1_gpu_create((ps1_irq_sink){ 0 });
    gpu_open(one);
    ps1_gpu_set_upscale(G, 4);
    uint32_t sd = 4242;
    for (int i = 0; i < 60000; i++) {
        uint32_t r = scene_rand(&sd) ^ scene_rand(&sd) << 24;
        uint32_t w = i % 5 == 0 ? (0x20u + (r >> 24) % 0xC0u) << 24 | (r & 0xFFFFFF) : r;
        ps1_gpu_gp0(one, w);
        ps1_gpu_gp0(G, w);
        if (i % 997 == 0) {
            uint32_t g1 = (r & 0x0F00FFFF) | (r % 12) << 24;
            ps1_gpu_gp1(one, g1);
            ps1_gpu_gp1(G, g1);
        }
    }
    check(!memcmp(ps1_gpu_vram(one), ps1_gpu_vram(G), 1024 * 512 * 2), "native VRAM = 1x after fuzz");
    ps1_gpu_destroy(one);
}

/* Random command streams (random words after random command bytes, random GP1 writes): nothing may crash or read
 * outside VRAM (run under ASan / UBSan to check), and the GPU still answers afterwards. */
static void t_fuzz(void)
{
    fresh("fuzz");
    uint32_t s = 99;
    GP0(G, 0xE1000200);
    for (int i = 0; i < 200000; i++) {
        uint32_t r = scene_rand(&s) ^ scene_rand(&s) << 24;
        if (i % 97 == 0)
            ps1_gpu_gp1(G, (r & 0x0F00FFFF) | (r % 12) << 24);
        else if (i % 5 == 0)
            ps1_gpu_gp0(G, (0x20u + (r >> 24) % 0xC0u) << 24 | (r & 0xFFFFFF));
        else if (i % 7 == 0) /* a sliver: tiny area, big colour change */
            GP0(G, 0x300000FF, XY(10, 10), 0x00FF00, XY(500, 11), 0xFF0000, XY(990, 12));
        else
            ps1_gpu_gp0(G, r);
        if (i % 1000 == 0) {
            ps1_gpu_read(G);
            ps1_gpu_scanline(G, (int)(r % 263));
        }
    }
    uint32_t out[640 * 512];
    int w, h;
    ps1_gpu_gp1(G, 0x01000000);
    ps1_gpu_gp1(G, 0x03000000);
    ps1_gpu_gp1(G, 0x08000017);
    check(ps1_gpu_display(G, out, 640, 512, &w, &h) == 0, "display after fuzz");
    gpu_open(G);
    GP0(G, 0x02FFFFFF, XY(0, 0), XY(16, 1));
    expect_px(0, 0, 0x7FFF);
}

int main(void)
{
    t_status();
    t_transfers();
    t_fill_rules();
    t_gouraud_dither();
    t_textures();
    t_uv_interpolation();
    t_semi_mask();
    t_rects_lines();
    t_display();
    t_gfx_replica();
    t_busy();
    t_upscale();
    t_fuzz();
    ps1_gpu_destroy(G);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("ps1 gpu: ok\n");
    return 0;
}
