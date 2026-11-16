/* gxfog: fog, blending, Z compare and logic ops (XBA-262). Top row: five squares at eye depths 10, 30, 50, 70 and 90
 * under perspective linear fog from 20 to 80 (none, 1/6, 1/2, 5/6, all fog). Second row: the same depths under
 * exponential fog. Third row: alpha blending, additive, subtractive and multiplicative blending, a Z GREATER test and
 * the INVCOPY and CLEAR logic ops. EFB peeks check the values the formulas give. */
#include "../common/gxdemo.h"
#include <math.h>

static const GXColor SQ = {255, 200, 0, 255}, FOG = {40, 80, 160, 255};

static void persp_square(int col, int row, f32 z, GXColor c)
{
    /* a square centred on column col of row row whatever its depth: the view-space size grows with z */
    f32 sx = (64 + col * 128 - 320) / 320.0f, sy = (240 - (56 + row * 112)) / 240.0f;
    f32 t = tanf(30.0f * (f32)M_PI / 180.0f);
    f32 cx = sx * z * t * (4.0f / 3.0f), cy = sy * z * t, h = 0.16f * z * t;
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    GX_Position3f32(cx - h, cy + h, -z);
    GX_Color4u8(c.r, c.g, c.b, c.a);
    GX_TexCoord2f32(0, 0);
    GX_Position3f32(cx + h, cy + h, -z);
    GX_Color4u8(c.r, c.g, c.b, c.a);
    GX_TexCoord2f32(0, 0);
    GX_Position3f32(cx + h, cy - h, -z);
    GX_Color4u8(c.r, c.g, c.b, c.a);
    GX_TexCoord2f32(0, 0);
    GX_Position3f32(cx - h, cy - h, -z);
    GX_Color4u8(c.r, c.g, c.b, c.a);
    GX_TexCoord2f32(0, 0);
    GX_End();
}

static void fog_rows(void)
{
    Mtx44 p;
    Mtx mv;
    guPerspective(p, 60, 4.0f / 3.0f, 1.0f, 100.0f);
    GX_LoadProjectionMtx(p, GX_PERSPECTIVE);
    guMtxIdentity(mv);
    GX_LoadPosMtxImm(mv, GX_PNMTX0);
    GX_SetCurrentMtx(GX_PNMTX0);
    static const f32 depth[5] = {10, 30, 50, 70, 90};
    GX_SetFog(GX_FOG_PERSP_LIN, 20, 80, 1, 100, FOG);
    for (int i = 0; i < 5; i++)
        persp_square(i, 0, depth[i], SQ);
    GX_SetFog(GX_FOG_PERSP_EXP, 20, 80, 1, 100, FOG);
    for (int i = 0; i < 5; i++)
        persp_square(i, 1, depth[i], SQ);
    GX_SetFog(GX_FOG_NONE, 0, 1, 0.1f, 1, FOG);
}

static void blend_row(void)
{
    demo_ortho();
    f32 y = 248;
    GXColor blue = {0, 0, 255, 255}, red_half = {255, 0, 0, 128};
    /* alpha blend: red at alpha 128 over blue */
    demo_quad(24, y, 64, 64, -5, blue);
    GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    demo_quad(24, y, 64, 64, -5, red_half);
    /* additive */
    GXColor a = {50, 100, 0, 255}, b = {100, 50, 0, 255};
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    demo_quad(104, y, 64, 64, -5, a);
    GX_SetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_CLEAR);
    demo_quad(104, y, 64, 64, -5, b);
    /* subtract: destination minus source */
    GXColor g = {200, 200, 200, 255}, s = {50, 100, 150, 255};
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    demo_quad(184, y, 64, 64, -5, g);
    GX_SetBlendMode(GX_BM_SUBTRACT, GX_BL_ONE, GX_BL_ONE, GX_LO_CLEAR);
    demo_quad(184, y, 64, 64, -5, s);
    /* multiply: source times destination colour */
    GXColor d = {200, 100, 50, 255}, m = {128, 255, 64, 255};
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    demo_quad(264, y, 64, 64, -5, d);
    GX_SetBlendMode(GX_BM_BLEND, GX_BL_DSTCLR, GX_BL_ZERO, GX_LO_CLEAR);
    demo_quad(264, y, 64, 64, -5, m);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    /* Z GREATER: a near white quad, then a far green one that passes because it is farther */
    GXColor white = {255, 255, 255, 255}, green = {0, 255, 0, 255};
    GX_SetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
    demo_quad(344, y, 64, 64, -10, white);
    GX_SetZMode(GX_TRUE, GX_GREATER, GX_TRUE);
    demo_quad(344, y + 32, 64, 32, -500, green);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    /* logic ops: INVCOPY of (40, 100, 200), CLEAR */
    GXColor l = {40, 100, 200, 255};
    GX_SetBlendMode(GX_BM_LOGIC, GX_BL_ONE, GX_BL_ZERO, GX_LO_INVCOPY);
    demo_quad(424, y, 64, 64, -5, l);
    GX_SetBlendMode(GX_BM_LOGIC, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    demo_quad(504, y, 64, 64, -5, l);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
}

/* linear fog factor f at eye depth z, then the colour the fog blend gives (8-bit fraction) */
static u8 fogged(u8 c, u8 f8, float f)
{
    int fi = (int)(f * 256.0f + 0.5f);
    return (u8)((c * (256 - fi) + f8 * fi) >> 8);
}

int main(void)
{
    GXColor bg = {16, 24, 64, 255};
    demo_init(bg);
    for (;;) {
        demo_fail = 0;
        demo_flat_tev();
        demo_vtx_pct();
        fog_rows();
        blend_row();
        GX_DrawDone();
        static const float lin[5] = {0, 1.0f / 6, 0.5f, 5.0f / 6, 1};
        for (int i = 0; i < 5; i++)
            demo_expect(i, (u16)(64 + i * 128), 56, fogged(SQ.r, FOG.r, lin[i]), fogged(SQ.g, FOG.g, lin[i]),
                        fogged(SQ.b, FOG.b, lin[i]), 8);
        float e = 1.0f - exp2f(-8.0f * 0.5f); /* exponential fog halfway */
        demo_expect(5, 64 + 2 * 128, 168, fogged(SQ.r, FOG.r, e), fogged(SQ.g, FOG.g, e), fogged(SQ.b, FOG.b, e), 8);
        demo_expect(6, 56, 280, 128, 0, 127, 2);     /* alpha blend */
        demo_expect(7, 136, 280, 150, 150, 0, 1);    /* additive */
        demo_expect(8, 216, 280, 150, 100, 50, 1);   /* subtract */
        demo_expect(9, 296, 280, 100, 100, 13, 2);   /* multiply */
        demo_expect(10, 376, 260, 255, 255, 255, 0); /* Z GREATER: the near white quad's top half */
        demo_expect(11, 376, 300, 0, 255, 0, 0);     /* the far green quad passed over it */
        demo_expect(12, 456, 280, 215, 155, 55, 0);  /* INVCOPY */
        demo_expect(13, 536, 280, 0, 0, 0, 0);       /* CLEAR */
        demo_end_frame();
    }
    return 0;
}
