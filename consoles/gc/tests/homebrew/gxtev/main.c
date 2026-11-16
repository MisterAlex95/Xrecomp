/* gxtev: TEV combiners (XBA-262). Twelve tiles, each a TEV setup whose result follows from the hardware's arithmetic
 * (8-bit inputs, c = 255 counting as 256, d + bias, lerp, scale, rounding, clamp), checked by an EFB peek: konst
 * colours and fractions, bias, colour registers times the rasterised colour, scale and subtract, the R8 compare,
 * swap tables, the alpha test, destination registers over three stages, a texture modulated, sixteen chained
 * stages, an alpha result broadcast to colour. The rasterised colour is (255, 128, 0) unless said otherwise. */
#include "../common/gxdemo.h"

static u8 checker[8 * 8] __attribute__((aligned(32)));

static const GXColor RAS = {255, 128, 0, 255};

static void tile_quad(int slot, GXColor c0, GXColor c1)
{
    f32 x = 24 + (slot % 4) * 152, y = 24 + (slot / 4) * 136, w = 128, h = 112;
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    GX_Position3f32(x, y, -5);
    GX_Color4u8(c0.r, c0.g, c0.b, c0.a);
    GX_TexCoord2f32(0, 0);
    GX_Position3f32(x + w, y, -5);
    GX_Color4u8(c1.r, c1.g, c1.b, c1.a);
    GX_TexCoord2f32(1, 0);
    GX_Position3f32(x + w, y + h, -5);
    GX_Color4u8(c1.r, c1.g, c1.b, c1.a);
    GX_TexCoord2f32(1, 1);
    GX_Position3f32(x, y + h, -5);
    GX_Color4u8(c0.r, c0.g, c0.b, c0.a);
    GX_TexCoord2f32(0, 1);
    GX_End();
}

/* stage s: colour d + lerp(a, b, c) with the given op, alpha passing the rasterised alpha */
static void stage(int s, u8 a, u8 b, u8 c, u8 d, u8 op, u8 bias, u8 scale, u8 dest)
{
    GX_SetTevOrder(s, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GX_SetTevColorIn(s, a, b, c, d);
    GX_SetTevColorOp(s, op, bias, scale, GX_TRUE, dest);
    GX_SetTevAlphaIn(s, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
    GX_SetTevAlphaOp(s, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GX_SetTevSwapMode(s, GX_TEV_SWAP0, GX_TEV_SWAP0);
}

static void reset(int nstages)
{
    GX_SetNumTevStages(nstages);
    GX_SetNumTexGens(0);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GXColor black = {0, 0, 0, 0};
    GX_SetTevColor(GX_TEVPREV, black);
}

static void draw(void)
{
    demo_flat_tev();
    demo_vtx_pct();
    demo_ortho();
    GXColor k0 = {200, 100, 50, 255}, k1 = {16, 8, 4, 255}, k2 = {50, 100, 90, 100}, k3 = {0, 255, 0, 255};
    GX_SetTevKColor(GX_KCOLOR0, k0);
    GX_SetTevKColor(GX_KCOLOR1, k1);
    GX_SetTevKColor(GX_KCOLOR2, k2);
    GX_SetTevKColor(GX_KCOLOR3, k3);
    GXColor c0 = {128, 255, 64, 255}, c1 = {100, 0, 0, 255}, c2 = {200, 150, 100, 255};
    GX_SetTevColor(GX_TEVREG0, c0);
    GX_SetTevColor(GX_TEVREG1, c1);
    GX_SetTevColor(GX_TEVREG2, c2);
    GX_SetTevSwapModeTable(GX_TEV_SWAP0, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_ALPHA);
    GX_SetTevSwapModeTable(GX_TEV_SWAP1, GX_CH_BLUE, GX_CH_GREEN, GX_CH_RED, GX_CH_ALPHA);

    /* 0: konst colour K0 */
    reset(1);
    stage(0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_KONST, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TEVPREV);
    GX_SetTevKColorSel(0, GX_TEV_KCSEL_K0);
    tile_quad(0, RAS, RAS);
    /* 1: the 3/4 fraction, minus a half: 191 - 128 */
    stage(0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_KONST, GX_TEV_ADD, GX_TB_SUBHALF, GX_CS_SCALE_1, GX_TEVPREV);
    GX_SetTevKColorSel(0, GX_TEV_KCSEL_3_4);
    tile_quad(1, RAS, RAS);
    /* 2: C0 x rasterised colour */
    stage(0, GX_CC_ZERO, GX_CC_C0, GX_CC_RASC, GX_CC_ZERO, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TEVPREV);
    tile_quad(2, RAS, RAS);
    /* 3: (C2 - K2) x 2, clamped: C2 is d, K2 the lerp's a with c = 0 */
    stage(0, GX_CC_KONST, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C2, GX_TEV_SUB, GX_TB_ZERO, GX_CS_SCALE_2, GX_TEVPREV);
    GX_SetTevKColorSel(0, GX_TEV_KCSEL_K2);
    tile_quad(3, RAS, RAS);
    /* 4: compare R8 greater: C0.r 128 > C1.r 100, so d + c = 0 + K3 */
    stage(0, GX_CC_C0, GX_CC_C1, GX_CC_KONST, GX_CC_ZERO, GX_TEV_COMP_R8_GT, GX_TB_ZERO, GX_CS_SCALE_1, GX_TEVPREV);
    GX_SetTevKColorSel(0, GX_TEV_KCSEL_K3);
    tile_quad(4, RAS, RAS);
    /* 5: the rasterised colour through swap table 1 (blue, green, red) */
    stage(0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TEVPREV);
    GX_SetTevSwapMode(0, GX_TEV_SWAP1, GX_TEV_SWAP0);
    tile_quad(5, RAS, RAS);
    /* 6: alpha test: alpha 0 on the left to 255 on the right, kept where greater than 128 */
    stage(0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TEVPREV);
    GX_SetAlphaCompare(GX_GREATER, 128, GX_AOP_AND, GX_ALWAYS, 0);
    GXColor left = {255, 128, 0, 0};
    tile_quad(6, left, RAS);
    /* 7: three stages: C1 = K0; PREV = C1 / 2; PREV += C1 / 4 */
    reset(3);
    stage(0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_KONST, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TEVREG1);
    GX_SetTevKColorSel(0, GX_TEV_KCSEL_K0);
    stage(1, GX_CC_ZERO, GX_CC_C1, GX_CC_HALF, GX_CC_ZERO, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TEVPREV);
    stage(2, GX_CC_ZERO, GX_CC_C1, GX_CC_KONST, GX_CC_CPREV, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TEVPREV);
    GX_SetTevKColorSel(2, GX_TEV_KCSEL_1_4);
    tile_quad(7, RAS, RAS);
    /* 8: an I8 checker (96 / 160) modulated by the rasterised colour */
    reset(1);
    GX_SetNumTexGens(1);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GXTexObj ck;
    GX_InitTexObj(&ck, checker, 8, 8, GX_TF_I8, GX_REPEAT, GX_REPEAT, GX_FALSE);
    GX_InitTexObjFilterMode(&ck, GX_NEAR, GX_NEAR);
    GX_LoadTexObj(&ck, GX_TEXMAP0);
    GX_SetTevOrder(0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GX_SetTevOp(0, GX_MODULATE);
    tile_quad(8, RAS, RAS);
    /* 9: sixteen stages, each adding K1 (16, 8, 4) to PREV (from black): 256 clamps to 255 */
    reset(16);
    GX_SetNumTexGens(0);
    for (int s = 0; s < 16; s++) {
        stage(s, GX_CC_KONST, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TEVPREV);
        GX_SetTevKColorSel(s, GX_TEV_KCSEL_K1);
    }
    tile_quad(9, RAS, RAS);
    /* 10: K2's alpha (100) as the alpha result, then broadcast to colour */
    reset(2);
    stage(0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TEVPREV);
    GX_SetTevAlphaIn(0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
    GX_SetTevKAlphaSel(0, GX_TEV_KASEL_K2_A);
    stage(1, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_APREV, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TEVPREV);
    GX_SetTevAlphaIn(1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
    tile_quad(10, RAS, RAS);
    /* 11: konst components: K0's green as colour (100), divided by 2 with bias +0.5: (100 + 128) / 2 */
    reset(1);
    stage(0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_KONST, GX_TEV_ADD, GX_TB_ADDHALF, GX_CS_DIVIDE_2, GX_TEVPREV);
    GX_SetTevKColorSel(0, GX_TEV_KCSEL_K0_G);
    tile_quad(11, RAS, RAS);
    demo_flat_tev();
}

static u16 tx(int slot, int dx) { return (u16)(24 + (slot % 4) * 152 + dx); }
static u16 ty(int slot, int dy) { return (u16)(24 + (slot / 4) * 136 + dy); }

int main(void)
{
    GXColor bg = {16, 24, 64, 255};
    demo_init(bg);
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            checker[(y / 4) * 32 + (y % 4) * 8 + x] = ((x / 4 + y / 4) & 1) ? 160 : 96;
    DCFlushRange(checker, sizeof checker);
    for (;;) {
        demo_fail = 0;
        draw();
        GX_DrawDone();
        demo_expect(0, tx(0, 64), ty(0, 56), 200, 100, 50, 0);
        demo_expect(1, tx(1, 64), ty(1, 56), 63, 63, 63, 0);
        demo_expect(2, tx(2, 64), ty(2, 56), 128, 128, 0, 0);
        demo_expect(3, tx(3, 64), ty(3, 56), 255, 100, 20, 0);
        demo_expect(4, tx(4, 64), ty(4, 56), 0, 255, 0, 0);
        demo_expect(5, tx(5, 64), ty(5, 56), 0, 128, 255, 0);
        demo_expect(6, tx(6, 8), ty(6, 56), 16, 24, 64, 0);   /* discarded: the clear colour */
        demo_expect(7, tx(6, 120), ty(6, 56), 255, 128, 0, 0);
        demo_expect(8, tx(7, 64), ty(7, 56), 151, 75, 38, 0);
        demo_expect(9, tx(8, 3), ty(8, 3), 96, 48, 0, 0);     /* texel (0, 0) = 96: 96 x (255, 128, 0) */
        demo_expect(10, tx(9, 64), ty(9, 56), 255, 128, 64, 0);
        demo_expect(11, tx(10, 64), ty(10, 56), 100, 100, 100, 0);
        demo_expect(12, tx(11, 64), ty(11, 56), 114, 114, 114, 0);
        demo_end_frame();
    }
    return 0;
}
