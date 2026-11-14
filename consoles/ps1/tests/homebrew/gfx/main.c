/* PS1 gfx (XBA-149): one deterministic 320x240 frame for renderer comparison: flat TILE, flat
 * triangle (POLY_F3), gouraud quad (POLY_G4), textured quad (POLY_FT4, 16-bit texture uploaded
 * with LoadImage) and a sprite (SPRT, 4-bit CLUT texture). The program reads a few pixels back
 * with StoreImage and checks them. Capture point: stage == 1 (README.md). Our own code. */
#include <stdint.h>
#include <psxgpu.h>

#include "../common/xba_result.h"
#include "../common/screen.h"

#define OT_LEN 8
static uint32_t ot[OT_LEN];
static uint8_t prims[1024];

#define TEX_X 640 /* 16-bit 16x16 texture */
#define TEX_Y 0
#define TEX4_X 640 /* 4-bit 16x16 texture = 4x16 VRAM words */
#define TEX4_Y 256
#define CLUT_X 640
#define CLUT_Y 480

static uint16_t tex16[16 * 16];
static uint16_t tex4[4 * 16];
static uint16_t clut[16];

static uint16_t rgb555(int r, int g, int b) { return (uint16_t)((r >> 3) | (g >> 3) << 5 | (b >> 3) << 10); }

static void make_textures(void)
{
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            tex16[y * 16 + x] = ((x >> 2) ^ (y >> 2)) & 1 ? rgb555(255, 255, 255) : rgb555(255, 0, 128);
    for (int y = 0; y < 16; y++)
        for (int w = 0; w < 4; w++) {
            uint16_t v = 0;
            for (int n = 0; n < 4; n++)
                v |= (uint16_t)(((w * 4 + n + y) & 15) << (4 * n));
            tex4[y * 4 + w] = v;
        }
    for (int i = 0; i < 16; i++)
        clut[i] = rgb555(i * 16, 255 - i * 16, 64) | (i ? 0 : 0x8000); /* index 0 opaque black-ish */
}

static uint32_t read_pixel(int x, int y)
{
    uint32_t px[1];
    RECT r = { x, y, 2, 1 }; /* StoreImage moves whole 32-bit words: 2 pixels */
    DrawSync(0);
    StoreImage(&r, px);
    DrawSync(0);
    return px[0] & 0xFFFF;
}

int main(void)
{
    xba_begin(XBA_ID_GFX);
    screen_init(32, 32, 32);

    make_textures();
    RECT r16 = { TEX_X, TEX_Y, 16, 16 };
    LoadImage(&r16, (const uint32_t *)tex16);
    RECT r4 = { TEX4_X, TEX4_Y, 4, 16 };
    LoadImage(&r4, (const uint32_t *)tex4);
    RECT rc = { CLUT_X, CLUT_Y, 16, 1 };
    LoadImage(&rc, (const uint32_t *)clut);
    DrawSync(0);

    ClearOTagR(ot, OT_LEN);
    uint8_t *p = prims;

    TILE *tile = (TILE *)p;
    setTile(tile);
    setXY0(tile, 16, 16);
    setWH(tile, 64, 48);
    setRGB0(tile, 255, 0, 0);
    addPrim(&ot[OT_LEN - 1], tile);
    p += sizeof(TILE);

    POLY_F3 *f3 = (POLY_F3 *)p;
    setPolyF3(f3);
    setXY3(f3, 120, 16, 200, 100, 100, 90);
    setRGB0(f3, 0, 255, 0);
    addPrim(&ot[OT_LEN - 1], f3);
    p += sizeof(POLY_F3);

    POLY_G4 *g4 = (POLY_G4 *)p;
    setPolyG4(g4);
    setXY4(g4, 220, 16, 300, 16, 220, 96, 300, 96);
    setRGB0(g4, 255, 0, 0);
    setRGB1(g4, 0, 255, 0);
    setRGB2(g4, 0, 0, 255);
    setRGB3(g4, 255, 255, 255);
    addPrim(&ot[OT_LEN - 1], g4);
    p += sizeof(POLY_G4);

    POLY_FT4 *ft4 = (POLY_FT4 *)p;
    setPolyFT4(ft4);
    setXY4(ft4, 16, 120, 112, 120, 16, 216, 112, 216);
    setUV4(ft4, 0, 0, 15, 0, 0, 15, 15, 15);
    setRGB0(ft4, 128, 128, 128); /* neutral modulation */
    setTPage(ft4, 2, 0, TEX_X, TEX_Y);
    addPrim(&ot[OT_LEN - 1], ft4);
    p += sizeof(POLY_FT4);

    SPRT *spr = (SPRT *)p;
    setSprt(spr);
    setXY0(spr, 160, 140);
    setWH(spr, 16, 16);
    setUV0(spr, 0, 0);
    setClut(spr, CLUT_X, CLUT_Y);
    setRGB0(spr, 128, 128, 128);
    addPrim(&ot[OT_LEN - 1], spr);
    p += sizeof(SPRT);

    /* SPRT has no texture page field: set it with a DR_TPAGE drawn before it (OT is reversed). */
    DR_TPAGE *tp = (DR_TPAGE *)p;
    setDrawTPage(tp, 0, 0, getTPage(0, 0, TEX4_X, TEX4_Y));
    addPrim(&ot[OT_LEN - 1], tp);
    p += sizeof(DR_TPAGE);

    DrawOTag(&ot[OT_LEN - 1]);
    DrawSync(0);
    VSync(0);

    uint32_t i = 0;
    xba_check(i++, read_pixel(20, 20), rgb555(255, 0, 0));    /* inside TILE */
    xba_check(i++, read_pixel(150, 60), rgb555(0, 255, 0));   /* inside POLY_F3 */
    xba_check(i++, read_pixel(5, 5), rgb555(32, 32, 32));     /* background */
    xba_check(i++, read_pixel(17, 121), tex16[0]);            /* textured quad, top-left texel */
    uint32_t g = read_pixel(260, 56);                         /* gouraud middle: all channels mixed */
    xba_check(i++, (g & 0x1F) > 4 && ((g >> 5) & 0x1F) > 4 && ((g >> 10) & 0x1F) > 4, 1);
    xba_check(i++, g, g);                                     /* raw, informative */

    VSync(0);
    XBA_RESULT->stage = 1; /* capture point */
    for (int f = 0; f < 30; f++)
        VSync(0);
    xba_finish();
}
