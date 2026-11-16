/* gxtri: GX geometry (XBA-262). An orthographic Gouraud triangle (f32 positions, RGBA8 colours), a perspective cube
 * from indexed arrays (s16 positions with 8 fraction bits, 8-bit indices, RGB565 colours by 16-bit index) with back
 * faces culled and the Z buffer on, two quads crossing in depth, a fan and a strip from s8 positions with a matrix
 * index per vertex, wide lines and points. EFB peeks check a few pixels whose colour follows from the geometry. */
#include "../common/gxdemo.h"

/* the cube: 8 corners, s16 with 8 fraction bits (+-1.0 = +-256) */
static s16 cube_pos[8 * 3] __attribute__((aligned(32))) = {
    -256, -256, 256,  256, -256, 256,  256, 256, 256,  -256, 256, 256,
    -256, -256, -256, 256, -256, -256, 256, 256, -256, -256, 256, -256,
};
/* one RGB565 colour per face */
static u16 face_col[6] __attribute__((aligned(32))) = {0xF800, 0x07E0, 0x001F, 0xFFE0, 0xF81F, 0x07FF};
static const u8 faces[6][4] = {{0, 1, 2, 3}, {5, 4, 7, 6}, {4, 0, 3, 7}, {1, 5, 6, 2}, {3, 2, 6, 7}, {4, 5, 1, 0}};

static void draw_ortho(void)
{
    demo_flat_tev();
    demo_vtx_pct();
    demo_ortho();
    /* Gouraud triangle */
    GX_Begin(GX_TRIANGLES, GX_VTXFMT0, 3);
    GX_Position3f32(40, 40, -10);
    GX_Color4u8(255, 0, 0, 255);
    GX_TexCoord2f32(0, 0);
    GX_Position3f32(280, 40, -10);
    GX_Color4u8(0, 255, 0, 255);
    GX_TexCoord2f32(0, 0);
    GX_Position3f32(160, 240, -10);
    GX_Color4u8(0, 0, 255, 255);
    GX_TexCoord2f32(0, 0);
    GX_End();
    /* two quads crossing in depth: the Z test decides who wins on each half */
    GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    GX_Position3f32(40, 280, -20);
    GX_Color4u8(255, 255, 255, 255);
    GX_TexCoord2f32(0, 0);
    GX_Position3f32(280, 280, -80);
    GX_Color4u8(255, 255, 255, 255);
    GX_TexCoord2f32(0, 0);
    GX_Position3f32(280, 400, -80);
    GX_Color4u8(255, 255, 255, 255);
    GX_TexCoord2f32(0, 0);
    GX_Position3f32(40, 400, -20);
    GX_Color4u8(255, 255, 255, 255);
    GX_TexCoord2f32(0, 0);
    GX_End();
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    GX_Position3f32(40, 300, -80);
    GX_Color4u8(255, 128, 0, 255);
    GX_TexCoord2f32(0, 0);
    GX_Position3f32(280, 300, -20);
    GX_Color4u8(255, 128, 0, 255);
    GX_TexCoord2f32(0, 0);
    GX_Position3f32(280, 380, -20);
    GX_Color4u8(255, 128, 0, 255);
    GX_TexCoord2f32(0, 0);
    GX_Position3f32(40, 380, -80);
    GX_Color4u8(255, 128, 0, 255);
    GX_TexCoord2f32(0, 0);
    GX_End();
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
}

/* A fan and a strip of s8 positions (no fraction), placed by a per-vertex position matrix index. */
static void draw_matidx(void)
{
    Mtx m;
    guMtxIdentity(m);
    guMtxTransApply(m, m, 340, 300, -10);
    GX_LoadPosMtxImm(m, GX_PNMTX1);
    guMtxIdentity(m);
    guMtxTransApply(m, m, 480, 300, -10);
    GX_LoadPosMtxImm(m, GX_PNMTX2);
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_PTNMTXIDX, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS, GX_POS_XY, GX_S8, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_CLR0, GX_CLR_RGB, GX_RGB8, 0);
    /* fan: a hexagon-ish disc */
    static const s8 fan[7][2] = {{0, 0}, {60, 0}, {30, 52}, {-30, 52}, {-60, 0}, {-30, -52}, {30, -52}};
    GX_Begin(GX_TRIANGLEFAN, GX_VTXFMT1, 8);
    for (int i = 0; i < 8; i++) {
        int k = i < 7 ? i : 1;
        GX_MatrixIndex1x8(GX_PNMTX1);
        GX_Position2s8(fan[k][0], fan[k][1]);
        GX_Color3u8(k == 0 ? 255 : 0, 200, k == 0 ? 255 : 100);
    }
    GX_End();
    /* strip: a zigzag band */
    GX_Begin(GX_TRIANGLESTRIP, GX_VTXFMT1, 8);
    for (int i = 0; i < 8; i++) {
        GX_MatrixIndex1x8(GX_PNMTX2);
        GX_Position2s8((s8)(-60 + 17 * i), (s8)((i & 1) ? 40 : -40));
        GX_Color3u8(200, (u8)(30 * i), 50);
    }
    GX_End();
    GX_SetCurrentMtx(GX_PNMTX0);
}

static void draw_cube(void)
{
    Mtx44 p;
    Mtx v, r, mv;
    guPerspective(p, 45, 4.0f / 3.0f, 1.0f, 100.0f);
    GX_LoadProjectionMtx(p, GX_PERSPECTIVE);
    guMtxIdentity(v);
    guMtxRotDeg(r, 'x', 25);
    guMtxConcat(r, v, v);
    guMtxRotDeg(r, 'y', 35);
    guMtxConcat(v, r, mv);
    guMtxTransApply(mv, mv, 1.6f, 0.9f, -6.0f);
    GX_LoadPosMtxImm(mv, GX_PNMTX0);
    GX_SetCurrentMtx(GX_PNMTX0);
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_INDEX8);
    GX_SetVtxDesc(GX_VA_CLR0, GX_INDEX16);
    GX_SetVtxAttrFmt(GX_VTXFMT2, GX_VA_POS, GX_POS_XYZ, GX_S16, 8);
    GX_SetVtxAttrFmt(GX_VTXFMT2, GX_VA_CLR0, GX_CLR_RGB, GX_RGB565, 0);
    GX_SetArray(GX_VA_POS, cube_pos, 3 * sizeof(s16));
    GX_SetArray(GX_VA_CLR0, face_col, sizeof(u16));
    DCFlushRange(cube_pos, sizeof cube_pos);
    DCFlushRange(face_col, sizeof face_col);
    GX_SetCullMode(GX_CULL_BACK);
    GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GX_Begin(GX_QUADS, GX_VTXFMT2, 24);
    for (int f = 0; f < 6; f++)
        for (int k = 0; k < 4; k++) {
            GX_Position1x8(faces[f][k]);
            GX_Color1x16((u16)f);
        }
    GX_End();
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
}

static void draw_lines(void)
{
    demo_flat_tev();
    demo_vtx_pct();
    demo_ortho();
    GX_SetLineWidth(24, GX_TO_ZERO); /* 4 pixels */
    GX_Begin(GX_LINESTRIP, GX_VTXFMT0, 4);
    GX_Position3f32(320, 420, -5);
    GX_Color4u8(255, 255, 0, 255);
    GX_TexCoord2f32(0, 0);
    GX_Position3f32(420, 420, -5);
    GX_Color4u8(255, 255, 0, 255);
    GX_TexCoord2f32(0, 0);
    GX_Position3f32(460, 380, -5);
    GX_Color4u8(0, 255, 255, 255);
    GX_TexCoord2f32(0, 0);
    GX_Position3f32(460, 440, -5);
    GX_Color4u8(0, 255, 255, 255);
    GX_TexCoord2f32(0, 0);
    GX_End();
    GX_SetPointSize(48, GX_TO_ZERO); /* 8 pixels */
    GX_Begin(GX_POINTS, GX_VTXFMT0, 3);
    for (int i = 0; i < 3; i++) {
        GX_Position3f32(520 + 30 * i, 420, -5);
        GX_Color4u8(255, (u8)(80 * i), 255, 255);
        GX_TexCoord2f32(0, 0);
    }
    GX_End();
}

int main(void)
{
    GXColor bg = {16, 24, 64, 255};
    demo_init(bg);
    for (;;) {
        demo_fail = 0;
        draw_ortho();
        draw_matidx();
        draw_cube();
        draw_lines();
        GX_DrawDone();
        demo_expect(0, 600, 20, 16, 24, 64, 0);       /* background: the clear colour */
        demo_expect(1, 160, 107, 85, 85, 85, 8);      /* triangle centroid: a third of each */
        demo_expect(2, 60, 340, 255, 255, 255, 0);    /* left: the white quad is nearer */
        demo_expect(3, 260, 340, 255, 128, 0, 0);     /* right: the orange one */
        demo_expect(4, 340, 300, 255, 200, 255, 4);   /* the fan's centre vertex */
        demo_expect(5, 380, 420, 255, 255, 0, 0);     /* a wide line */
        demo_expect(6, 580, 420, 255, 160, 255, 0);   /* a point */
        demo_end_frame();
    }
    return 0;
}
