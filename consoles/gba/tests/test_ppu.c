/* GBA PPU tests (XBA-154): one synthetic scene per feature, built by filling the IO/palette/VRAM/OAM images, with
 * the expected pixels worked out from GBATEK; a hand-checked replica of the homebrew gfx.gba scenes; a golden hash
 * of a full busy frame (regression guard). */
#include "ppu_scene.h"

#include <stdio.h>

static int failures;
static const char *cur;

#define R 0x001Fu
#define G 0x03E0u
#define B 0x7C00u
#define WHITE 0x7FFFu
#define BD 0x1234u

/* FNV-1a of the 160 lines of scene_busy(); a regression guard, update it only for a deliberate change. */
#ifndef GOLDEN_BUSY
#define GOLDEN_BUSY 0xA7072882u
#endif

static uint16_t out[240];

static void render(scene *s, int line) { gba_ppu_render_line(&s->in, line, out); }

static void expect(int line, int x, unsigned want)
{
    if (out[x] != want) {
        fprintf(stderr, "FAIL %s: line %d x %d = 0x%04X, want 0x%04X\n", cur, line, x, out[x], want);
        failures++;
    }
}

static void expect_run(int line, int x0, int x1, unsigned want)
{
    for (int x = x0; x < x1; x++)
        if (out[x] != want) {
            expect(line, x, want);
            return;
        }
}

/* Mode 0 base: BD backdrop, palette 1 = R, 2 = G, 3 = B, tiles 1/2/3 solid of that index in char block 0. */
static void base(scene *s)
{
    scene_init(s);
    bg_color(s, 0, BD);
    bg_color(s, 1, R);
    bg_color(s, 2, G);
    bg_color(s, 3, B);
    for (unsigned t = 1; t <= 3; t++)
        tile4_solid(s, t * 32, t);
}

/* Fill a 32x32 screenblock with one entry. */
static void fill_map(scene *s, unsigned sb, unsigned entry)
{
    for (unsigned i = 0; i < 1024; i++)
        w16(s->vram, sb * 0x800 + i * 2, entry);
}

static void t_backdrop_blank(void)
{
    scene s;
    cur = "backdrop";
    base(&s);
    render(&s, 0);
    expect_run(0, 0, 240, BD);
    cur = "forced blank";
    w16(s.io, 0, 0x80 | 0x100);
    render(&s, 10);
    expect_run(10, 0, 240, WHITE);
}

static void t_text_scroll(void)
{
    scene s;
    cur = "text 4bpp scroll";
    base(&s);
    for (unsigned r = 0; r < 32; r++)
        for (unsigned c = 0; c < 32; c++)   /* odd columns tile 2, even tile 1; row 1 uses tile 3 */
            w16(s.vram, 8 * 0x800 + (r * 32 + c) * 2, r == 1 ? 3 : 1 + (c & 1));
    w16(s.io, 0x08, 8 << 8);
    w16(s.io, 0, 0x100);
    render(&s, 0);
    expect_run(0, 0, 8, R);
    expect_run(0, 8, 16, G);
    render(&s, 8);
    expect_run(8, 0, 240, B);
    w16(s.io, 0x10, 4);   /* HOFS 4 */
    render(&s, 0);
    expect_run(0, 0, 4, R);
    expect_run(0, 4, 12, G);
    w16(s.io, 0x10, 256 + 4);   /* 256x256 wraps at 256 */
    render(&s, 0);
    expect_run(0, 0, 4, R);
    w16(s.io, 0x12, 4);   /* VOFS 4: line 4 shows map row 1 */
    render(&s, 4);
    expect_run(4, 0, 240, B);
    render(&s, 3);
    expect(3, 0, R);

    cur = "text palette bank";
    bg_color(&s, 5 * 16 + 1, 0x4321);
    fill_map(&s, 8, 1 | (5 << 12));
    render(&s, 0);
    expect_run(0, 0, 240, 0x4321);
}

static void t_text_flip_8bpp(void)
{
    scene s;
    cur = "text flips";
    base(&s);
    tile4_solid(&s, 4 * 32, 2);
    tile4_px(&s, 4 * 32, 0, 0, 1);   /* tile 4: G with R top-left */
    fill_map(&s, 8, 4);
    w16(s.io, 0x08, 8 << 8);
    w16(s.io, 0, 0x100);
    render(&s, 0);
    expect(0, 0, R);
    expect(0, 1, G);
    expect(0, 7, G);
    fill_map(&s, 8, 4 | 0x400);   /* hflip */
    render(&s, 0);
    expect(0, 0, G);
    expect(0, 7, R);
    fill_map(&s, 8, 4 | 0x800);   /* vflip */
    render(&s, 0);
    expect(0, 0, G);
    render(&s, 7);
    expect(7, 0, R);

    cur = "text 8bpp";
    bg_color(&s, 200, 0x5555);
    tile8_solid(&s, 0x8000 + 3 * 64, 200);
    s.vram[0x8000 + 3 * 64 + 1] = 0;   /* transparent texel (1,0) */
    fill_map(&s, 8, 3);
    w16(s.io, 0x08, (8 << 8) | 0x80 | (2 << 2));
    render(&s, 0);
    expect(0, 0, 0x5555);
    expect(0, 1, BD);
    expect(0, 8, 0x5555);
    expect(0, 9, BD);
}

static void t_text_sizes(void)
{
    scene s;
    cur = "text 512x256";
    base(&s);
    fill_map(&s, 8, 1);
    fill_map(&s, 9, 2);
    fill_map(&s, 10, 3);
    fill_map(&s, 11, 0);
    w16(s.io, 0, 0x100);
    w16(s.io, 0x08, (8 << 8) | (1 << 14));
    w16(s.io, 0x10, 248);
    render(&s, 0);
    expect_run(0, 0, 8, R);
    expect_run(0, 8, 240, G);
    w16(s.io, 0x10, 504);   /* wraps back to block 8 */
    render(&s, 0);
    expect_run(0, 0, 8, G);
    expect_run(0, 8, 16, R);

    cur = "text 256x512";
    w16(s.io, 0x08, (8 << 8) | (2 << 14));
    w16(s.io, 0x10, 0);
    w16(s.io, 0x12, 256);
    render(&s, 0);
    expect_run(0, 0, 240, G);   /* second block = SB 9 */

    cur = "text 512x512";
    w16(s.io, 0x08, (8 << 8) | (3 << 14));
    render(&s, 0);
    expect_run(0, 0, 240, B);   /* bottom-left = SB 10 */
    w16(s.io, 0x10, 256);
    render(&s, 0);
    expect_run(0, 0, 240, BD);  /* bottom-right = SB 11, tile 0 transparent */
}

static void t_priorities(void)
{
    scene s;
    cur = "bg priority";
    base(&s);
    fill_map(&s, 8, 1);
    fill_map(&s, 9, 2);
    w16(s.io, 0x08, (8 << 8) | 1);   /* BG0 R prio 1 */
    w16(s.io, 0x0A, (9 << 8) | 0);   /* BG1 G prio 0 */
    w16(s.io, 0, 0x300);
    render(&s, 0);
    expect(0, 0, G);
    w16(s.io, 0x0A, (9 << 8) | 1);   /* same priority: lower BG number in front */
    render(&s, 0);
    expect(0, 0, R);
    w16(s.io, 0, 0x200);   /* BG0 off */
    render(&s, 0);
    expect(0, 0, G);
    cur = "bg transparency";
    w16(s.io, 0, 0x300);
    for (unsigned c = 0; c < 32; c += 2)
        w16(s.vram, 8 * 0x800 + c * 2, 0);   /* holes in BG0 */
    render(&s, 0);
    expect(0, 0, G);
    expect(0, 8, R);
    cur = "mode 2 hides text BGs";
    w16(s.io, 0, 0x302);
    render(&s, 0);
    expect(0, 0, BD);
}

static void t_affine_bg(void)
{
    scene s;
    cur = "affine bg";
    base(&s);
    bg_color(&s, 10, 0x0101);
    bg_color(&s, 11, 0x0202);
    tile8_solid(&s, 0, 0);
    tile8_solid(&s, 64, 10);
    tile8_solid(&s, 128, 11);
    for (unsigned i = 0; i < 16 * 16; i++)   /* 128x128 map in SB 8: column parity picks tile 1 or 2 */
        s.vram[8 * 0x800 + i] = (uint8_t)(1 + ((i % 16) & 1));
    s.vram[8 * 0x800 + 16] = 0;              /* row 1 col 0 transparent */
    w16(s.io, 0x0C, 8 << 8);                 /* BG2 128x128, no wrap */
    w16(s.io, 0x20, 0x100);
    w16(s.io, 0x26, 0x100);
    w16(s.io, 0, 0x400 | 2);
    render(&s, 0);
    expect_run(0, 0, 8, 0x0101);
    expect_run(0, 8, 16, 0x0202);
    expect_run(0, 128, 240, BD);
    w16(s.io, 0x0C, (8 << 8) | 0x2000);       /* wrap */
    render(&s, 0);
    expect_run(0, 128, 136, 0x0101);
    s.in.bg2x = -8 * 256;                     /* negative x wraps to column 15 */
    render(&s, 0);
    expect_run(0, 0, 8, 0x0202);
    s.in.bg2x = 0;
    s.in.bg2y = 8 << 8;                       /* row 1 */
    render(&s, 0);
    expect_run(0, 0, 8, BD);
    expect_run(0, 8, 16, 0x0202);
    s.in.bg2y = 0;
    w16(s.io, 0x20, 0x200);                   /* 2x shrink: tile changes every 4 pixels */
    render(&s, 0);
    expect_run(0, 0, 4, 0x0101);
    expect_run(0, 4, 8, 0x0202);
    w16(s.io, 0x20, 0);                       /* PC steps y: x stays 0, y advances 1 per pixel */
    w16(s.io, 0x24, 0x100);
    render(&s, 0);
    expect_run(0, 0, 8, 0x0101);
    expect_run(0, 8, 16, BD);                 /* row 1 col 0 */
    cur = "affine bg3 mode 1/2";
    w16(s.io, 0x24, 0);
    w16(s.io, 0x20, 0x100);
    w16(s.io, 0x30, 0x100);
    w16(s.io, 0x0E, (8 << 8) | 0x2000);
    s.in.bg3x = 8 << 8;
    w16(s.io, 0, 0x800 | 2);
    render(&s, 0);
    expect_run(0, 0, 8, 0x0202);
    w16(s.io, 0, 0x800 | 1);                  /* no BG3 in mode 1 */
    render(&s, 0);
    expect(0, 0, BD);
}

static void t_bitmap(void)
{
    scene s;
    cur = "mode 3";
    base(&s);
    w16(s.io, 0x20, 0x100);
    w16(s.io, 0x26, 0x100);
    w16(s.vram, (5 * 240 + 10) * 2, 0x7ABC);
    w16(s.io, 0, 0x400 | 3);
    s.in.bg2y = 5 << 8;   /* the PPU samples where the reference point says, whatever the line */
    render(&s, 5);
    expect(5, 10, 0x7ABC & 0x7FFF);
    expect(5, 11, 0);   /* black, opaque */
    render(&s, 0);
    expect(0, 10, 0x7ABC & 0x7FFF);
    s.in.bg2x = -256;
    render(&s, 5);
    expect(5, 0, BD);
    expect(5, 11, 0x7ABC & 0x7FFF);
    s.in.bg2x = 0;
    s.in.bg2y = 0;
    render(&s, 5);
    expect(5, 10, 0);

    cur = "mode 4 frame select";
    bg_color(&s, 7, 0x0777);
    s.vram[0xA000 + 3 * 240 + 4] = 7;
    s.vram[3 * 240 + 5] = 1;
    w16(s.io, 0, 0x400 | 4);
    s.in.bg2y = 3 << 8;
    render(&s, 3);
    expect(3, 4, BD);
    expect(3, 5, R);
    w16(s.io, 0, 0x400 | 4 | 0x10);
    render(&s, 3);
    expect(3, 4, 0x0777);
    expect(3, 5, BD);

    cur = "mode 5";
    w16(s.vram, 0xA000 + (2 * 160 + 159) * 2, 0x0333);
    w16(s.io, 0, 0x400 | 5 | 0x10);
    s.in.bg2y = 2 << 8;
    render(&s, 2);
    expect(2, 159, 0x0333);
    expect(2, 160, BD);
    s.in.bg2y = 128 << 8;
    render(&s, 128);
    expect(128, 0, BD);
}

/* Sprite base: OBJ palette 1 = R, 2 = G, 3 = B; OBJ tile n (4bpp) solid colour n for n = 1..3. */
static void obj_base(scene *s)
{
    base(s);
    obj_color(s, 1, R);
    obj_color(s, 2, G);
    obj_color(s, 3, B);
    for (unsigned t = 1; t <= 3; t++)
        tile4_solid(s, 0x10000 + t * 32, t);
}

static void t_sprites(void)
{
    scene s;
    cur = "sprite 8x8";
    obj_base(&s);
    w16(s.io, 0, 0x1000 | 0x40);
    sprite(&s, 0, 10, 16, 1);
    render(&s, 10);
    expect(10, 15, BD);
    expect_run(10, 16, 24, R);
    expect(10, 24, BD);
    render(&s, 17);
    expect_run(17, 16, 24, R);
    render(&s, 18);
    expect(18, 16, BD);
    render(&s, 9);
    expect(9, 16, BD);

    cur = "sprite 16x16 1D vs 2D";
    sprite(&s, 0, 0 | (0 << 14), 0 | (1 << 14), 1);   /* tiles 1,2 / 3,4 in 1D; 1,2 / 33,34 in 2D */
    tile4_solid(&s, 0x10000 + 33 * 32, 3);
    tile4_solid(&s, 0x10000 + 4 * 32, 2);
    render(&s, 0);
    expect_run(0, 0, 8, R);
    expect_run(0, 8, 16, G);
    render(&s, 8);
    expect_run(8, 0, 8, B);    /* 1D: tile 3 */
    w16(s.io, 0, 0x1000);
    render(&s, 8);
    expect_run(8, 0, 8, B);    /* 2D: tile 33 */
    expect_run(8, 8, 16, BD);  /* 2D: tile 34 empty */
    w16(s.io, 0, 0x1040);
    render(&s, 8);
    expect_run(8, 8, 16, G);   /* 1D: tile 4 */

    cur = "sprite hflip/vflip";
    sprite(&s, 0, 0, (1 << 14) | 0x1000, 1);
    render(&s, 0);
    expect_run(0, 0, 8, G);
    expect_run(0, 8, 16, R);
    sprite(&s, 0, 0, (1 << 14) | 0x2000, 1);
    render(&s, 0);
    expect_run(0, 0, 8, B);

    cur = "sprite 8bpp";
    obj_color(&s, 99, 0x0999);
    memset(s.vram + 0x10000 + 8 * 32, 99, 64);
    sprite(&s, 0, 0x2000, 0, 8);
    render(&s, 0);
    expect_run(0, 0, 8, 0x0999);
    expect(0, 8, BD);

    cur = "sprite palette bank";
    obj_color(&s, 6 * 16 + 1, 0x0666);
    sprite(&s, 0, 0, 0, 1 | (6 << 12));
    render(&s, 0);
    expect(0, 0, 0x0666);

    cur = "sprite y wrap / negative x";
    sprite(&s, 0, 252, 512 - 4, 1);
    render(&s, 2);
    expect_run(2, 0, 4, R);
    expect(2, 4, BD);
    render(&s, 4);
    expect(4, 0, BD);

    cur = "sprite shapes";
    sprite(&s, 0, 0x4000, 3 << 14, 1);   /* horizontal size 3 = 64x32: 8 tiles per row in 1D */
    tile4_solid(&s, 0x10000 + 32 * 32, 1);   /* tile 1 + 3 * 8 + 7: bottom-right */
    render(&s, 0);
    expect(0, 0, R);
    expect(0, 8, G);
    expect(0, 48, BD);
    render(&s, 31);
    expect(31, 0, BD);
    expect_run(31, 56, 64, R);
    expect(31, 64, BD);
    render(&s, 32);
    expect(32, 63, BD);
    sprite(&s, 0, 0x8000, 0, 1);         /* vertical 8x16 */
    render(&s, 15);
    expect(15, 8, BD);
    render(&s, 16);
    expect(16, 0, BD);

    cur = "sprite disabled";
    sprite(&s, 0, 0x200, 0, 1);
    render(&s, 0);
    expect(0, 0, BD);
    cur = "OBJ layer off";
    sprite(&s, 0, 0, 0, 1);
    w16(s.io, 0, 0x40);
    render(&s, 0);
    expect(0, 0, BD);

    cur = "bitmap mode hides OBJ tiles < 512";
    w16(s.io, 0, 0x1040 | 3);
    render(&s, 0);
    expect(0, 0, BD);
    tile4_solid(&s, 0x10000 + 512 * 32, 2);
    sprite(&s, 0, 0, 0, 512);
    render(&s, 0);
    expect(0, 0, G);
}

static void t_sprite_priority(void)
{
    scene s;
    cur = "sprite vs sprite";
    obj_base(&s);
    w16(s.io, 0, 0x1000 | 0x40);
    sprite(&s, 0, 0, 0, 1);
    sprite(&s, 1, 0, 4, 2);
    render(&s, 0);
    expect(0, 4, R);   /* same priority: lower OAM index */
    sprite(&s, 1, 0, 4, 2 | (0 << 10));
    sprite(&s, 0, 0, 0, 1 | (1 << 10));
    render(&s, 0);
    expect(0, 4, G);   /* higher priority wins */
    expect(0, 0, R);

    cur = "sprite vs bg";
    fill_map(&s, 8, 3);
    w16(s.io, 0x08, (8 << 8) | 1);
    w16(s.io, 0, 0x1100 | 0x40);
    render(&s, 0);
    expect(0, 0, R);   /* OBJ prio 1 vs BG prio 1: OBJ in front */
    w16(s.io, 0x08, (8 << 8) | 0);
    render(&s, 0);
    expect(0, 0, B);
    expect(0, 4, G);   /* OBJ prio 0 in front of BG prio 0 */
}

static void t_affine_sprites(void)
{
    scene s;
    cur = "affine sprite identity";
    obj_base(&s);
    tile4_px(&s, 0x10000 + 32, 0, 0, 2);
    w16(s.io, 0, 0x1000 | 0x40);
    obj_affine(&s, 3, 0x100, 0, 0, 0x100);
    sprite(&s, 0, 0x100 | 20, 30 | (3 << 9), 1);
    render(&s, 20);
    expect(20, 30, G);
    expect_run(20, 31, 38, R);
    expect(20, 38, BD);

    cur = "affine sprite hflip matrix";   /* texel = 8 - i: the hardware mirror is one pixel off */
    obj_affine(&s, 3, -0x100, 0, 0, 0x100);
    render(&s, 20);
    expect(20, 30, BD);
    expect_run(20, 31, 38, R);

    cur = "affine sprite double size";
    obj_affine(&s, 3, 0x100, 0, 0, 0x100);
    sprite(&s, 0, 0x300 | 20, 30 | (3 << 9), 1);   /* 16x16 box, sprite centred at +4 */
    render(&s, 20);
    expect(20, 30, BD);
    render(&s, 24);
    expect(24, 33, BD);
    expect(24, 34, G);
    expect_run(24, 35, 42, R);
    expect(24, 42, BD);
    render(&s, 32);
    expect(32, 34, BD);

    cur = "affine sprite zoom";
    obj_affine(&s, 3, 0x80, 0, 0, 0x80);   /* 2x: fills the 16x16 box */
    render(&s, 20);
    expect_run(20, 30, 32, G);
    expect_run(20, 32, 46, R);
    expect(20, 46, BD);
    render(&s, 22);
    expect_run(22, 30, 46, R);
}

static void t_windows(void)
{
    scene s;
    cur = "win0";
    base(&s);
    fill_map(&s, 8, 1);
    fill_map(&s, 9, 2);
    w16(s.io, 0x08, (8 << 8) | 0);
    w16(s.io, 0x0A, (9 << 8) | 1);
    w16(s.io, 0, 0x300 | 0x2000);
    w16(s.io, 0x40, (10 << 8) | 20);
    w16(s.io, 0x44, (5 << 8) | 50);
    w16(s.io, 0x48, 0x01);   /* inside WIN0: BG0 */
    w16(s.io, 0x4A, 0x02);   /* outside: BG1 */
    render(&s, 5);
    expect(5, 9, G);
    expect_run(5, 10, 20, R);
    expect(5, 20, G);
    render(&s, 4);
    expect(4, 10, G);
    render(&s, 50);
    expect(50, 10, G);

    cur = "win0 wraparound";
    w16(s.io, 0x40, (230 << 8) | 5);
    render(&s, 5);
    expect_run(5, 0, 5, R);
    expect_run(5, 5, 230, G);
    expect_run(5, 230, 240, R);
    w16(s.io, 0x44, (100 << 8) | 10);
    render(&s, 120);
    expect(120, 0, R);
    render(&s, 50);
    expect(50, 0, G);

    cur = "win1 under win0";
    w16(s.io, 0x40, (10 << 8) | 20);
    w16(s.io, 0x44, (0 << 8) | 160);
    w16(s.io, 0x42, (15 << 8) | 30);
    w16(s.io, 0x46, (0 << 8) | 160);
    w16(s.io, 0x48, 0x01 | (0x00 << 8));   /* WIN1 shows nothing: backdrop */
    w16(s.io, 0, 0x300 | 0x6000);
    render(&s, 0);
    expect(0, 15, R);     /* WIN0 has priority */
    expect(0, 20, BD);
    expect(0, 30, G);

    cur = "obj window";
    obj_color(&s, 1, R);
    tile4_solid(&s, 0x10000 + 32, 1);
    sprite(&s, 0, 0x800, 100, 1);           /* OBJ window sprite at x 100..107 */
    w16(s.io, 0x4A, 0x02 | (0x01 << 8));    /* outside BG1, OBJ window BG0 */
    w16(s.io, 0, 0x300 | 0x1000 | 0x8000);
    render(&s, 0);
    expect(0, 99, G);
    expect_run(0, 100, 108, R);
    expect(0, 108, G);
    w16(s.io, 0, 0x300 | 0x1000 | 0x8000 | 0x2000);
    w16(s.io, 0x40, (104 << 8) | 120);
    w16(s.io, 0x48, 0x02);
    render(&s, 0);
    expect_run(0, 100, 104, R);
    expect_run(0, 104, 108, G);   /* WIN0 over OBJ window */
}

static void t_blending(void)
{
    scene s;
    cur = "alpha";
    base(&s);
    bg_color(&s, 4, 0x0010);   /* r = 16 */
    bg_color(&s, 5, 0x0008);   /* r = 8 */
    tile4_solid(&s, 4 * 32, 4);
    tile4_solid(&s, 5 * 32, 5);
    fill_map(&s, 8, 4);
    fill_map(&s, 9, 5);
    w16(s.io, 0x08, (8 << 8) | 0);
    w16(s.io, 0x0A, (9 << 8) | 1);
    w16(s.io, 0, 0x300);
    w16(s.io, 0x50, 0x01 | 0x40 | (0x02 << 8));
    w16(s.io, 0x52, 8 | (8 << 8));
    render(&s, 0);
    expect(0, 0, 12);
    w16(s.io, 0x52, 16 | (16 << 8));
    render(&s, 0);
    expect(0, 0, 24);
    bg_color(&s, 4, 0x7FFF);
    render(&s, 0);
    expect(0, 0, 0x7C00 | 0x03E0 | 31);   /* saturates */
    bg_color(&s, 4, 0x0010);
    w16(s.io, 0x50, 0x01 | 0x40 | (0x20 << 8));   /* 2nd target backdrop only: BG1 is not */
    render(&s, 0);
    expect(0, 0, 0x0010);
    w16(s.io, 0x50, 0x02 | 0x40 | (0x01 << 8));   /* BG1 first target but not on top */
    render(&s, 0);
    expect(0, 0, 0x0010);

    cur = "brighten/darken";
    w16(s.io, 0x50, 0x01 | 0x80);
    w16(s.io, 0x54, 16);
    render(&s, 0);
    expect(0, 0, 0x7FFF);
    w16(s.io, 0x54, 8);
    render(&s, 0);
    expect(0, 0, (16 + 7) | (15 << 5) | (15 << 10));
    w16(s.io, 0x50, 0x01 | 0xC0);
    render(&s, 0);
    expect(0, 0, 8);
    w16(s.io, 0x54, 31);   /* clamps to 16 */
    render(&s, 0);
    expect(0, 0, 0);
    w16(s.io, 0x50, 0x20 | 0xC0);   /* backdrop target */
    w16(s.io, 0, 0);
    render(&s, 0);
    expect(0, 0, 0);

    cur = "window effect bit";
    w16(s.io, 0, 0x300 | 0x2000);
    w16(s.io, 0x50, 0x01 | 0xC0);
    w16(s.io, 0x40, (0 << 8) | 10);
    w16(s.io, 0x44, (0 << 8) | 160);
    w16(s.io, 0x48, 0x03);          /* WIN0: no effects */
    w16(s.io, 0x4A, 0x23);          /* outside: effects */
    render(&s, 0);
    expect(0, 0, 0x0010);
    expect(0, 10, 0);

    cur = "semi-transparent OBJ";
    w16(s.io, 0, 0x1300 | 0x40);
    obj_color(&s, 1, 0x0010);
    tile4_solid(&s, 0x10000 + 32, 1);
    sprite(&s, 0, 0x400, 0, 1);      /* prio 0, in front of BG0 */
    w16(s.io, 0x50, 0x01 << 8);     /* effects off, BG0 second target */
    w16(s.io, 0x52, 8 | (8 << 8));
    render(&s, 0);
    expect(0, 0, 16);   /* (16 + 16) / 2 */
    w16(s.io, 0x50, 0x02 << 8);     /* BG0 not a 2nd target: plain */
    render(&s, 0);
    expect(0, 0, 0x0010);
    w16(s.io, 0x50, 0x10 | 0xC0 | (0x02 << 8));   /* falls back to darken when OBJ is a 1st target */
    w16(s.io, 0x54, 16);
    render(&s, 0);
    expect(0, 0, 0);
    sprite(&s, 0, 0x000, 0, 1);     /* normal OBJ, alpha mode, OBJ not 1st target */
    w16(s.io, 0x50, 0x01 | 0x40 | (0x01 << 8));
    render(&s, 0);
    expect(0, 0, 0x0010);
}

static void t_mosaic(void)
{
    scene s;
    cur = "bg mosaic h";
    base(&s);
    for (unsigned c = 0; c < 32; c++)
        w16(s.vram, 8 * 0x800 + c * 2, 1 + (c % 3));
    for (unsigned c = 0; c < 32; c++)
        w16(s.vram, 8 * 0x800 + 64 + c * 2, 3);
    w16(s.io, 0x08, (8 << 8) | 0x40);
    w16(s.io, 0, 0x100);
    w16(s.io, 0x4C, 15);   /* h 16 */
    render(&s, 0);
    expect_run(0, 0, 16, R);
    expect_run(0, 16, 32, B);   /* x 16: column 2 */
    w16(s.io, 0x4C, 0);
    render(&s, 0);
    expect_run(0, 8, 16, G);

    cur = "bg mosaic v";
    w16(s.io, 0x4C, 15 << 4);   /* v 16 */
    render(&s, 9);
    expect(9, 0, R);            /* line 9 shows line 0 (row 0) */
    w16(s.io, 0x4C, 0);
    render(&s, 9);
    expect(9, 0, B);

    cur = "obj mosaic";
    obj_color(&s, 1, R);
    obj_color(&s, 2, G);
    for (unsigned x = 0; x < 8; x++)
        for (unsigned y = 0; y < 8; y++)
            tile4_px(&s, 0x10000 + 32, x, y, x < 4 ? 1 : 2);
    sprite(&s, 0, 0x1000, 0, 1);
    w16(s.io, 0, 0x1000 | 0x40);
    w16(s.io, 0x4C, 7 << 8);    /* OBJ h 8 */
    render(&s, 0);
    expect_run(0, 0, 8, R);
    w16(s.io, 0x4C, 0);
    render(&s, 0);
    expect(0, 4, G);
}

static void t_greenswap(void)
{
    scene s;
    cur = "green swap";
    base(&s);
    for (unsigned c = 0; c < 32; c++)
        w16(s.vram, 8 * 0x800 + c * 2, 1 + (c & 1));
    w16(s.io, 0x08, 8 << 8);
    w16(s.io, 0, 0x100);
    w16(s.io, 0x02, 1);
    render(&s, 0);
    expect(0, 6, R);   /* pair (6,7) both from tile 0 */
    expect(0, 7, R);
    w16(s.io, 0x10, 1);   /* pair (6,7): x6 = R, x7 = G: greens swap */
    render(&s, 0);
    expect(0, 6, R | G);
    expect(0, 7, 0);
}

static void t_cycle_limit(void)
{
    scene s;
    cur = "sprite cycle limit";
    obj_base(&s);
    w16(s.io, 0, 0x1000 | 0x40);
    for (int n = 0; n < 18; n++)   /* 18 x 64 = 1152 cycles, off-screen */
        sprite(&s, n, 0 | (0 << 14), 300 | (3 << 14), 1);
    sprite(&s, 18, 0, 0, 1);   /* 8x8 */
    render(&s, 0);
    expect(0, 0, R);
    sprite(&s, 18, 0, 300 | (3 << 14), 1);   /* 1216 > 1210 */
    sprite(&s, 19, 0, 0, 2);
    render(&s, 0);
    expect(0, 0, BD);
    w16(s.io, 0, 0x1000 | 0x40 | 0x20);   /* HBlank free: 954 */
    sprite(&s, 18, 0, 0, 1);
    for (int n = 15; n < 18; n++)
        sprite(&s, n, 0x200, 0, 0);
    render(&s, 0);
    expect(0, 0, BD);   /* 15 x 64 = 960 > 954 */
}

/* A replica of homebrew gfx.gba scene 1 (consoles/gba/tests/homebrew/gfx/main.c) built directly in memory, with
 * pixels worked out by hand, and scene 2 (mode 3). The ROM itself needs the CPU core. */
static void t_gfx_replica(void)
{
    static const uint16_t palette[16] = {
        0x0000, 0x001F, 0x03E0, 0x7C00, 0x03FF, 0x7FE0, 0x7C1F, 0x4210,
        0x021F, 0x6108, 0x2118, 0x2308, 0x7FFF, 0x1294, 0x5284, 0x5094,
    };
    scene s;
    cur = "gfx scene 1";
    scene_init(&s);
    for (unsigned i = 0; i < 16; i++) {
        bg_color(&s, i, palette[i]);
        obj_color(&s, i, palette[15 - i]);
    }
    for (unsigned t = 0; t < 4; t++)
        for (unsigned y = 0; y < 8; y++)
            for (unsigned x = 0; x < 8; x++) {
                unsigned c = t == 0 ? 1 : t == 1 ? (((x ^ y) & 1) ? 2 : 3) : t == 2 ? ((y & 2) ? 4 : 5)
                                                                         : (x == y ? 6 : 7);
                tile4_px(&s, t * 32, x, y, c);
                tile4_px(&s, 0x10000 + t * 32, x, y, c);
            }
    for (unsigned y = 0; y < 32; y++)
        for (unsigned x = 0; x < 32; x++) {
            w16(s.vram, 30 * 0x800 + (y * 32 + x) * 2, (x + 2 * y) & 3);
            w16(s.vram, 31 * 0x800 + (y * 32 + x) * 2, x == y ? (3 | 0x400) : 0);
        }
    w16(s.io, 0x08, (30 << 8) | 1);
    w16(s.io, 0x0A, (31 << 8) | 0);
    w16(s.io, 0x14, 3);
    w16(s.io, 0x16, 5);
    sprite(&s, 0, 40, 40, 1);
    sprite(&s, 1, 60, 100 | (1 << 14), 0);
    sprite(&s, 2, 100, 180 | (1 << 14) | 0x1000, 0);
    sprite(&s, 3, 140, 220, 3);
    w16(s.io, 0, 0x0300 | 0x1000 | 0x40);
    render(&s, 0);
    expect(0, 0, 0x4210);   /* BG1 (3,5): h-flipped diagonal tile, texel (4,5) */
    expect(0, 5, 0x001F);   /* BG1 tile 0 is solid colour 1 */
    render(&s, 10);
    expect(10, 5, 0x7C1F);  /* BG1 (8,15): diagonal tile (1,1), texel (7,7) */
    render(&s, 40);
    expect(40, 40, 0x7FFF); /* sprite 0 checker: index 3 -> OBJ palette 3 = colour 12 */
    expect(40, 41, 0x1294);
    render(&s, 60);
    expect(60, 100, 0x5284);
    expect(60, 108, 0x7FFF);
    render(&s, 68);
    expect(68, 100, 0x2118);
    render(&s, 100);
    expect(100, 180, 0x1294);
    expect(100, 188, 0x5284);
    render(&s, 140);
    expect(140, 220, 0x6108);
    expect(140, 221, 0x021F);
    w16(s.io, 0x0A, (31 << 8) | 2);   /* BG1 behind BG0: BG0 tile (x + 2y) & 3 shows */
    render(&s, 0);
    expect(0, 0, 0x001F);
    expect(0, 8, 0x7C00);   /* tile 1 checker texel (0,0) -> 3 */
    expect(0, 9, 0x03E0);

    cur = "gfx scene 2";
    for (unsigned y = 0; y < 160; y++)
        for (unsigned x = 0; x < 240; x++) {
            unsigned c = (x * 31 / 239) | ((y * 31 / 159) << 5) | (12u << 10);
            if (x >= 20 && x < 100 && y >= 20 && y < 60)
                c = 0x7FFF;
            if (x >= 140 && x < 220 && y >= 90 && y < 150)
                c = 0;
            if (x == y + 40)
                c = 0x7C1F;
            w16(s.vram, (y * 240 + x) * 2, c);
        }
    w16(s.io, 0x20, 0x100);   /* what the BIOS leaves in BG2PA/PD */
    w16(s.io, 0x26, 0x100);
    w16(s.io, 0, 0x0403);
    s.in.bg2x = 0;
    uint32_t h = 2166136261u;
    for (int line = 0; line < 160; line++) {
        s.in.bg2y = line << 8;
        render(&s, line);
        h = fnv1a(out, sizeof out, h);
        if (line == 0)
            expect(0, 0, 12u << 10);
        if (line == 30)
            expect(30, 30, 0x7FFF);
        if (line == 100)
            expect(100, 150, 0);
        if (line == 10)
            expect(10, 50, 0x7C1F);
        if (line == 159)
            expect(159, 239, 0x7FFF & (31 | (31 << 5) | (12u << 10)));
    }
    /* The frame is the VRAM image itself: same bytes, same hash. */
    if (h != fnv1a(s.vram, 240 * 160 * 2, 2166136261u)) {
        fprintf(stderr, "FAIL %s: frame differs from VRAM\n", cur);
        failures++;
    }
}

static void t_golden(void)
{
    scene s;
    cur = "golden busy frame";
    scene_busy(&s);
    uint32_t h = 2166136261u;
    for (int line = 0; line < 160; line++) {
        render(&s, line);
        h = fnv1a(out, sizeof out, h);
    }
    if (h != GOLDEN_BUSY) {
        fprintf(stderr, "FAIL %s: hash 0x%08X, want 0x%08X\n", cur, h, GOLDEN_BUSY);
        failures++;
    }
}

int main(void)
{
    t_backdrop_blank();
    t_text_scroll();
    t_text_flip_8bpp();
    t_text_sizes();
    t_priorities();
    t_affine_bg();
    t_bitmap();
    t_sprites();
    t_sprite_priority();
    t_affine_sprites();
    t_windows();
    t_blending();
    t_mosaic();
    t_greenswap();
    t_cycle_limit();
    t_gfx_replica();
    t_golden();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("gba_ppu: ok\n");
    return 0;
}
