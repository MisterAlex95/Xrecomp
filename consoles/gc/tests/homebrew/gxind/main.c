/* gxind: indirect textures (XBA-262). A base texture whose texel columns run red to blue is looked up through an
 * indirect stage: a constant offset of +16 texels in s (8-bit format, bias on s t u, matrix 0.5 with scale 2^1), a
 * wave (a row-dependent offset), and a negative offset in t at half the indirect coordinate scale with a second
 * stage adding the previous coordinate (-4 twice). EFB peeks check the first: texel n shows texel n + 16. */
#include "../common/gxdemo.h"

static u16 base[64 * 64] __attribute__((aligned(32)));     /* RGB565, 4x4 tiles */
static u8 ind_const[4 * 4 * 4] __attribute__((aligned(32)));/* RGBA8 4x4: one 64-byte tile */
static u8 ind_wave[8 * 8 * 4] __attribute__((aligned(32))); /* RGBA8 8x8: four tiles */
static u8 ind_neg[4 * 4 * 4] __attribute__((aligned(32)));

/* RGBA8 texel (x, y) of a w-wide texture: AR pairs then GB pairs per 4x4 tile */
static void rgba8_put(u8 *t, int w, int x, int y, u8 r, u8 g, u8 b, u8 a)
{
    int tile = (y / 4) * (w / 4) + x / 4, i = (y % 4) * 4 + x % 4;
    u8 *p = t + tile * 64;
    p[2 * i] = a, p[2 * i + 1] = r, p[32 + 2 * i] = g, p[32 + 2 * i + 1] = b;
}

static void make_textures(void)
{
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            int r = x * 4, b = 255 - x * 4, g = (y & 8) ? 64 : 0;
            int tile = (y / 4) * 16 + x / 4, i = (y % 4) * 4 + x % 4;
            base[tile * 16 + i] = (u16)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
        }
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            rgba8_put(ind_const, 4, x, y, 128, 128, 128, 128 + 16); /* s = A - 128 = 16, t = B - 128 = 0, u = 0 */
            rgba8_put(ind_neg, 4, x, y, 128, 128, 128 - 4, 128);     /* t = -4 */
        }
    static const s8 wave[8] = {0, 6, 10, 12, 10, 6, 0, -6};
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            rgba8_put(ind_wave, 8, x, y, 128, 128, 128, (u8)(128 + wave[y]));
    DCFlushRange(base, sizeof base);
    DCFlushRange(ind_const, sizeof ind_const);
    DCFlushRange(ind_wave, sizeof ind_wave);
    DCFlushRange(ind_neg, sizeof ind_neg);
}

static void quad2(f32 x, f32 y, f32 s)
{
    GX_Begin(GX_QUADS, GX_VTXFMT3, 4);
    GX_Position3f32(x, y, -5);
    GX_TexCoord2f32(0, 0);
    GX_TexCoord2f32(0, 0);
    GX_Position3f32(x + s, y, -5);
    GX_TexCoord2f32(1, 0);
    GX_TexCoord2f32(1, 0);
    GX_Position3f32(x + s, y + s, -5);
    GX_TexCoord2f32(1, 1);
    GX_TexCoord2f32(1, 1);
    GX_Position3f32(x, y + s, -5);
    GX_TexCoord2f32(0, 1);
    GX_TexCoord2f32(0, 1);
    GX_End();
}

static void draw(void)
{
    demo_ortho();
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX1, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT3, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT3, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT3, GX_VA_TEX1, GX_TEX_ST, GX_F32, 0);
    GX_SetNumChans(0);
    GX_SetNumTexGens(2);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);
    GX_SetNumTevStages(1);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
    GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);

    GXTexObj b, ic, iw, in;
    GX_InitTexObj(&b, base, 64, 64, GX_TF_RGB565, GX_REPEAT, GX_REPEAT, GX_FALSE);
    GX_InitTexObjFilterMode(&b, GX_NEAR, GX_NEAR);
    GX_LoadTexObj(&b, GX_TEXMAP0);
    GX_InitTexObj(&ic, ind_const, 4, 4, GX_TF_RGBA8, GX_REPEAT, GX_REPEAT, GX_FALSE);
    GX_InitTexObjFilterMode(&ic, GX_NEAR, GX_NEAR);
    GX_InitTexObj(&iw, ind_wave, 8, 8, GX_TF_RGBA8, GX_REPEAT, GX_REPEAT, GX_FALSE);
    GX_InitTexObjFilterMode(&iw, GX_NEAR, GX_NEAR);
    GX_InitTexObj(&in, ind_neg, 4, 4, GX_TF_RGBA8, GX_REPEAT, GX_REPEAT, GX_FALSE);
    GX_InitTexObjFilterMode(&in, GX_NEAR, GX_NEAR);

    /* the base alone, for comparison */
    GX_SetNumIndStages(0);
    GX_SetTevDirect(GX_TEVSTAGE0);
    quad2(24, 24, 128);
    /* 1: constant +16 texels in s */
    f32 m[2][3] = {{0.5f, 0, 0}, {0, 0.5f, 0}};
    GX_SetIndTexMatrix(GX_ITM_0, m, 1);
    GX_SetNumIndStages(1);
    GX_SetIndTexOrder(GX_INDTEXSTAGE0, GX_TEXCOORD1, GX_TEXMAP1);
    GX_SetIndTexCoordScale(GX_INDTEXSTAGE0, GX_ITS_1, GX_ITS_1);
    GX_SetTevIndirect(GX_TEVSTAGE0, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_STU, GX_ITM_0, GX_ITW_OFF, GX_ITW_OFF,
                      GX_FALSE, GX_FALSE, GX_ITBA_OFF);
    GX_LoadTexObj(&ic, GX_TEXMAP1);
    quad2(176, 24, 128);
    /* 2: a wave along t (the offset in s changes with the row of the 8x8 indirect texture) */
    GX_LoadTexObj(&iw, GX_TEXMAP1);
    quad2(328, 24, 128);
    /* 3: -4 texels in t through matrix 1 (identity at 2^0), the indirect coordinate at half scale; a second stage
     * adds its offset to the first's coordinate (add previous) and keeps the result */
    f32 m1[2][3] = {{1.0f, 0, 0}, {0, 1.0f, 0}};
    GX_SetIndTexMatrix(GX_ITM_1, m1, 0);
    GX_SetIndTexCoordScale(GX_INDTEXSTAGE0, GX_ITS_2, GX_ITS_2);
    GX_LoadTexObj(&in, GX_TEXMAP1);
    GX_SetNumTevStages(2);
    GX_SetTevIndirect(GX_TEVSTAGE0, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_STU, GX_ITM_1, GX_ITW_OFF, GX_ITW_OFF,
                      GX_FALSE, GX_FALSE, GX_ITBA_OFF);
    GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
    GX_SetTevIndirect(GX_TEVSTAGE1, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_STU, GX_ITM_1, GX_ITW_0, GX_ITW_0, GX_TRUE,
                      GX_FALSE, GX_ITBA_OFF);
    GX_SetTevOp(GX_TEVSTAGE1, GX_REPLACE);
    quad2(480, 24, 128);
    GX_SetNumTevStages(1);
    GX_SetTevDirect(GX_TEVSTAGE0);
    GX_SetTevDirect(GX_TEVSTAGE1);
    GX_SetNumIndStages(0);
}

int main(void)
{
    GXColor bg = {16, 24, 64, 255};
    demo_init(bg);
    make_textures();
    for (;;) {
        demo_fail = 0;
        draw();
        GX_DrawDone();
        demo_expect(0, 24 + 1, 24 + 1, 0, 0, 255, 2);       /* base texel 0: blue */
        demo_expect(1, 176 + 1, 24 + 1, 66, 0, 189, 4);     /* texel 16 in RGB565: r 64, b 191 */
        demo_expect(2, 176 + 33, 24 + 1, 132, 0, 123, 4);   /* texel 16 + 16 = 32: r 128, b 127 */
        demo_expect(3, 480 + 1, 24 + 33, 0, 65, 255, 2);    /* row 16 shows row 8 (two -4 stages): green band */
        demo_end_frame();
    }
    return 0;
}
