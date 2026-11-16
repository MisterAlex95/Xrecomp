/* gxtex: GX textures (XBA-262). Our pictures in every texture format, encoded by devkitPro's gxtexconv at build
 * time (mktex.py): I4 I8 IA4 IA8 RGB565 RGB5A3 RGBA8, CI4 and CI8 with RGB565 palettes (gxtexconv drops the alpha of the RGB5A3 ones), CMPR; CI14x2 with a
 * 512-entry RGB5A3 palette built here (opaque and translucent entries); an RGB565 mipmap chain drawn minified (trilinear); repeat, mirror and clamp. Alpha
 * blended over a grey checker so the alpha channels show. EFB peeks check texels whose colour follows from the
 * pictures. */
#include "../common/gxdemo.h"
#include "textures.h"

static u8 checker[8 * 8] __attribute__((aligned(32)));        /* I8, 8x8 = two 8x4 tiles */
static u16 c14[32 * 32] __attribute__((aligned(32)));         /* CI14x2, 4x4 tiles */
static u16 c14_pal[512] __attribute__((aligned(32)));         /* RGB5A3 */

static void make_textures(void)
{
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            int tile = y / 4, i = (y % 4) * 8 + x;
            checker[tile * 32 + i] = ((x / 4 + y / 4) & 1) ? 160 : 96;
        }
    DCFlushRange(checker, sizeof checker);
    for (int i = 0; i < 512; i++) { /* RGB5A3: even entries opaque RGB555, odd ones 3-bit alpha + RGB444 */
        int r = (i * 7) & 31, g = (i * 3) & 31, b = 31 - (i & 31);
        if (i & 1)
            c14_pal[i] = (u16)(((i >> 1) & 7) << 12 | (r >> 1) << 8 | (g >> 1) << 4 | (b >> 1));
        else
            c14_pal[i] = (u16)(0x8000 | r << 10 | g << 5 | b);
    }
    DCFlushRange(c14_pal, sizeof c14_pal);
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) {
            int tile = (y / 4) * 8 + x / 4, i = (y % 4) * 4 + x % 4;
            int idx = ((x / 4) * 8 + (y / 4) * 64 + (x % 4)) & 511;
            c14[tile * 16 + i] = (u16)(idx | 0xC000); /* the top two bits are ignored */
        }
    DCFlushRange(c14, sizeof c14);
}

static void quad_uv(f32 x, f32 y, f32 w, f32 h, f32 u0, f32 v0, f32 u1, f32 v1)
{
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    GX_Position3f32(x, y, -5);
    GX_Color4u8(255, 255, 255, 255);
    GX_TexCoord2f32(u0, v0);
    GX_Position3f32(x + w, y, -5);
    GX_Color4u8(255, 255, 255, 255);
    GX_TexCoord2f32(u1, v0);
    GX_Position3f32(x + w, y + h, -5);
    GX_Color4u8(255, 255, 255, 255);
    GX_TexCoord2f32(u1, v1);
    GX_Position3f32(x, y + h, -5);
    GX_Color4u8(255, 255, 255, 255);
    GX_TexCoord2f32(u0, v1);
    GX_End();
}

static void tex_tev(void)
{
    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    GX_SetNumTexGens(1);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GX_SetNumTevStages(1);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
}

/* The TPLs are opened once (opening one turns its offsets into pointers in place). */
enum { T_I4, T_I8, T_IA4, T_IA8, T_RGB565, T_RGB5A3, T_RGBA8, T_CI4, T_CI8, T_CMPR, T_MIP, T_N };
static GXTexObj tex[T_N];
static GXTlutObj tlut[T_N];

static void open_tpls(void)
{
    static u8 *const data[T_N] = {tpl_i4, tpl_i8, tpl_ia4, tpl_ia8, tpl_rgb565, tpl_rgb5a3, tpl_rgba8, tpl_ci4, tpl_ci8,
                                  tpl_cmpr, tpl_mip};
    const u32 size[T_N] = {tpl_i4_size, tpl_i8_size, tpl_ia4_size, tpl_ia8_size, tpl_rgb565_size, tpl_rgb5a3_size,
                           tpl_rgba8_size, tpl_ci4_size, tpl_ci8_size, tpl_cmpr_size, tpl_mip_size};
    for (int i = 0; i < T_N; i++) {
        TPLFile f;
        TPL_OpenTPLFromMemory(&f, data[i], size[i]);
        if (i == T_CI4 || i == T_CI8)
            TPL_GetTextureCI(&f, 0, &tex[i], &tlut[i], i == T_CI4 ? GX_TLUT0 : GX_TLUT1);
        else
            TPL_GetTexture(&f, 0, &tex[i]);
        if (i != T_MIP)
            GX_InitTexObjFilterMode(&tex[i], GX_NEAR, GX_NEAR);
    }
    GX_InitTexObjFilterMode(&tex[T_MIP], GX_LIN_MIP_LIN, GX_LINEAR);
}

static void tile(int slot, int t)
{
    if (t == T_CI4 || t == T_CI8)
        GX_LoadTlut(&tlut[t], t == T_CI4 ? GX_TLUT0 : GX_TLUT1);
    GX_LoadTexObj(&tex[t], GX_TEXMAP0);
    int c = slot % 5, r = slot / 5;
    quad_uv(24 + c * 122, 24 + r * 140, 96, 96, 0, 0, 1, 1);
}

static void draw(void)
{
    demo_vtx_pct();
    demo_ortho();
    tex_tev();
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    /* the checker behind everything, repeated */
    GXTexObj ck;
    GX_InitTexObj(&ck, checker, 8, 8, GX_TF_I8, GX_REPEAT, GX_REPEAT, GX_FALSE);
    GX_InitTexObjFilterMode(&ck, GX_NEAR, GX_NEAR);
    GX_LoadTexObj(&ck, GX_TEXMAP0);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    quad_uv(0, 0, 640, 448, 0, 0, 40, 28);
    /* every format, alpha blended */
    GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    GX_InvalidateTexAll();
    for (int t = T_I4; t <= T_CMPR; t++)
        tile(t, t);
    /* CI14x2 */
    GXTlutObj lut;
    GXTexObj t;
    GX_InitTlutObj(&lut, c14_pal, GX_TL_RGB5A3, 512);
    GX_LoadTlut(&lut, GX_BIGTLUT0);
    GX_InitTexObjCI(&t, c14, 32, 32, GX_TF_CI14, GX_CLAMP, GX_CLAMP, GX_FALSE, GX_BIGTLUT0);
    GX_InitTexObjFilterMode(&t, GX_NEAR, GX_NEAR);
    GX_LoadTexObj(&t, GX_TEXMAP0);
    quad_uv(24, 24 + 2 * 140, 96, 96, 0, 0, 1, 1);
    /* mipmaps, trilinear, minified to 48, 24 and 12 pixels */
    GX_LoadTexObj(&tex[T_MIP], GX_TEXMAP0);
    quad_uv(146, 304, 48, 48, 0, 0, 1, 1);
    quad_uv(146 + 52, 304, 24, 24, 0, 0, 1, 1);
    quad_uv(146 + 52, 304 + 28, 12, 12, 0, 0, 1, 1);
    /* wrap modes on the RGBA8 picture: repeat, mirror, clamp */
    static const u8 wraps[3] = {GX_REPEAT, GX_MIRROR, GX_CLAMP};
    for (int k = 0; k < 3; k++) {
        t = tex[T_RGBA8];
        GX_InitTexObjWrapMode(&t, wraps[k], wraps[k]);
        GX_InitTexObjFilterMode(&t, GX_NEAR, GX_NEAR);
        GX_LoadTexObj(&t, GX_TEXMAP0);
        if (k < 2)
            quad_uv(24 + (2 + k) * 122, 304, 96, 96, 0, 0, 2, 2);
        else
            quad_uv(24 + 4 * 122, 304, 96, 96, -0.5f, -0.5f, 1.5f, 1.5f);
    }
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
}

int main(void)
{
    GXColor bg = {0, 0, 0, 255};
    demo_init(bg);
    make_textures();
    open_tpls();
    for (;;) {
        demo_fail = 0;
        draw();
        GX_DrawDone();
        demo_expect(0, 24 + 4 * 122, 24, 255, 0, 0, 10);   /* RGB565: hue 0 at texel (0, 0) */
        demo_expect(1, 24 + 1 * 122, 164, 255, 0, 0, 10);  /* RGBA8 */
        demo_expect(2, 24 + 2 * 122, 164, 255, 24, 24, 10); /* CI4: hue 0 through the RGB565 palette */
        demo_expect(3, 2, 2, 96, 96, 96, 0);               /* the checker */
        demo_expect(4, 24, 304, 0, 0, 255, 8);             /* CI14x2 index 0 (0xC000): r 0, g 0, b 31 */
        demo_expect(5, 24 + 3 * 122, 24, 0, 0, 0, 255);    /* (IA8: anything, a peek of a blended pixel) */
        demo_end_frame();
    }
    return 0;
}
