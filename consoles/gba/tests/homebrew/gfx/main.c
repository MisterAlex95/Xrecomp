/* GBA gfx (XBA-149): two deterministic scenes for renderer comparison.
 *   stage 1: mode 0, BG0 (4bpp tiles, procedural map) + BG1 (scrolled) + 4 sprites (8x8, 16x16, hflip)
 *   stage 2: mode 3 bitmap (gradient, rectangles, diagonal)
 * The host captures the frame at the first VBlank where `stage` holds a new value (see README.md).
 * The program also reads VRAM/OAM/palette back and checks them against what it wrote. Our own code. */
#include <tonc.h>

#include "../common/xba_result.h"

#define C15(r, g, b) ((u16)((r) | (g) << 5 | (b) << 10)) /* constant-expression RGB15 */

static volatile u32 vblanks;
static void on_vblank(void)
{
    vblanks++;
    XBA_RESULT->frames = vblanks;
}

static void wait_frames(u32 n)
{
    u32 end = vblanks + n;
    while (vblanks < end)
        VBlankIntrWait();
}

/* 4 tiles, 4bpp: solid, checker, horizontal stripes, diagonal. */
static void make_tiles(u32 *dst)
{
    for (int t = 0; t < 4; t++) {
        for (int y = 0; y < 8; y++) {
            u32 row = 0;
            for (int x = 0; x < 8; x++) {
                u32 c;
                switch (t) {
                case 0: c = 1; break;
                case 1: c = ((x ^ y) & 1) ? 2 : 3; break;
                case 2: c = (y & 2) ? 4 : 5; break;
                default: c = (x == y) ? 6 : 7; break;
                }
                row |= c << (4 * x);
            }
            dst[t * 8 + y] = row;
        }
    }
}

static const u16 palette[16] = {
    C15(0, 0, 0),   C15(31, 0, 0),  C15(0, 31, 0),  C15(0, 0, 31),
    C15(31, 31, 0), C15(0, 31, 31), C15(31, 0, 31), C15(16, 16, 16),
    C15(31, 16, 0), C15(8, 8, 24),  C15(24, 8, 8),  C15(8, 24, 8),
    C15(31, 31, 31), C15(20, 20, 4), C15(4, 20, 20), C15(20, 4, 20),
};

static u32 check_idx;
static void check_mem(const volatile void *got, const void *want, u32 bytes)
{
    const volatile u16 *g = got;
    const u16 *w = want;
    u32 bad = 0;
    for (u32 i = 0; i < bytes / 2; i++)
        bad += g[i] != w[i];
    xba_check(check_idx++, bad, 0);
}

int main(void)
{
    xba_begin(XBA_ID_GFX);
    irq_init(NULL);
    irq_add(II_VBLANK, on_vblank);

    /* ---- scene 1: mode 0 tiles + sprites ---- */
    REG_DISPCNT = DCNT_MODE0 | DCNT_BLANK;
    static u32 tiles[4 * 8];
    make_tiles(tiles);

    for (int i = 0; i < 16; i++) {
        pal_bg_mem[i] = palette[i];
        pal_obj_mem[i] = palette[15 - i];
    }
    memcpy32(&tile_mem[0][0], tiles, sizeof(tiles) / 4);
    memcpy32(&tile_mem[4][0], tiles, sizeof(tiles) / 4); /* sprite tiles, 1D mapping */

    /* BG0: screenblock 30, tile (x + 2y) & 3; BG1: screenblock 31, sparse diagonal of tile 3. */
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) {
            se_mem[30][y * 32 + x] = (x + 2 * y) & 3;
            se_mem[31][y * 32 + x] = (x == y) ? (3 | SE_HFLIP) : 0;
        }
    REG_BG0CNT = BG_CBB(0) | BG_SBB(30) | BG_4BPP | BG_REG_32x32 | BG_PRIO(1);
    REG_BG1CNT = BG_CBB(0) | BG_SBB(31) | BG_4BPP | BG_REG_32x32 | BG_PRIO(0);
    REG_BG0HOFS = 0;
    REG_BG0VOFS = 0;
    REG_BG1HOFS = 3;
    REG_BG1VOFS = 5;

    OBJ_ATTR objs[128];
    oam_init(objs, 128);
    obj_set_attr(&objs[0], ATTR0_SQUARE | ATTR0_Y(40), ATTR1_SIZE_8 | ATTR1_X(40), ATTR2_ID(1));
    obj_set_attr(&objs[1], ATTR0_SQUARE | ATTR0_Y(60), ATTR1_SIZE_16 | ATTR1_X(100), ATTR2_ID(0));
    obj_set_attr(&objs[2], ATTR0_SQUARE | ATTR0_Y(100), ATTR1_SIZE_16 | ATTR1_X(180) | ATTR1_HFLIP,
                 ATTR2_ID(0));
    obj_set_attr(&objs[3], ATTR0_SQUARE | ATTR0_Y(140), ATTR1_SIZE_8 | ATTR1_X(220), ATTR2_ID(3));
    oam_copy(oam_mem, objs, 128);

    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_BG1 | DCNT_OBJ | DCNT_OBJ_1D;

    check_mem(&tile_mem[0][0], tiles, sizeof(tiles));
    check_mem(pal_bg_mem, palette, sizeof(palette));
    check_mem(oam_mem, objs, 4 * sizeof(OBJ_ATTR));

    wait_frames(30);
    XBA_RESULT->stage = 1; /* capture point 1 */
    wait_frames(30);

    /* ---- scene 2: mode 3 bitmap ---- */
    REG_DISPCNT = DCNT_MODE3 | DCNT_BG2;
    for (int y = 0; y < M3_HEIGHT; y++)
        for (int x = 0; x < M3_WIDTH; x++)
            m3_mem[y][x] = RGB15(x * 31 / (M3_WIDTH - 1), y * 31 / (M3_HEIGHT - 1), 12);
    m3_rect(20, 20, 100, 60, RGB15(31, 31, 31));
    m3_rect(140, 90, 220, 150, RGB15(0, 0, 0));
    for (int i = 0; i < M3_HEIGHT; i++)
        m3_plot(i + 40, i, RGB15(31, 0, 31));

    u32 bad = 0;
    bad += m3_mem[0][0] != RGB15(0, 0, 12);
    bad += m3_mem[30][30] != RGB15(31, 31, 31);
    bad += m3_mem[100][150] != RGB15(0, 0, 0);
    bad += m3_mem[10][50] != RGB15(31, 0, 31);
    xba_check(check_idx++, bad, 0);
    XBA_RESULT->checksum = xba_fnv1a((const volatile u32 *)m3_mem, M3_WIDTH * M3_HEIGHT / 2);

    wait_frames(30);
    XBA_RESULT->stage = 2; /* capture point 2 */
    wait_frames(30);
    xba_finish();
}
