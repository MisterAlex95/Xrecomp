/* gxdemo.h -- what the GX test programs share (XBA-262): video and GX set up the libogc way (NTSC 480i, the
 * deflicker copy filter, an RGB8 / Z24 EFB), an orthographic projection in EFB pixels, a frame loop that draws the
 * same scene every frame, EFB peek checks and their status drawn as coloured squares.
 * Nothing depends on time, so each program's picture is the same on every run. */
#ifndef GXDEMO_H
#define GXDEMO_H

#include <gccore.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define DEMO_FIFO_SIZE (256 * 1024)

static void *demo_xfb;
static GXRModeObj *demo_rmode;
static unsigned demo_fail;

static void demo_init(GXColor clear)
{
    VIDEO_Init();
    demo_rmode = &TVNtsc480IntDf;
    demo_xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(demo_rmode));
    VIDEO_Configure(demo_rmode);
    VIDEO_SetNextFramebuffer(demo_xfb);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    VIDEO_WaitVSync();

    void *fifo = memalign(32, DEMO_FIFO_SIZE);
    memset(fifo, 0, DEMO_FIFO_SIZE);
    GX_Init(fifo, DEMO_FIFO_SIZE);
    GX_SetCopyClear(clear, GX_MAX_Z24);
    GX_SetViewport(0, 0, demo_rmode->fbWidth, demo_rmode->efbHeight, 0, 1);
    f32 yscale = GX_GetYScaleFactor(demo_rmode->efbHeight, demo_rmode->xfbHeight);
    u32 xfb_height = GX_SetDispCopyYScale(yscale);
    GX_SetScissor(0, 0, demo_rmode->fbWidth, demo_rmode->efbHeight);
    GX_SetDispCopySrc(0, 0, demo_rmode->fbWidth, demo_rmode->efbHeight);
    GX_SetDispCopyDst(demo_rmode->fbWidth, xfb_height);
    GX_SetCopyFilter(demo_rmode->aa, demo_rmode->sample_pattern, GX_TRUE, demo_rmode->vfilter);
    GX_SetFieldMode(demo_rmode->field_rendering,
                    (demo_rmode->viHeight == 2 * demo_rmode->xfbHeight) ? GX_ENABLE : GX_DISABLE);
    GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetDispCopyGamma(GX_GM_1_0);
    GX_CopyDisp(demo_xfb, GX_TRUE);
}

/* Orthographic projection: x 0..640 left to right, y 0..480 top to bottom (one unit = one EFB pixel), z -0.1
 * (near) .. -1000 (far). */
static void demo_ortho(void)
{
    Mtx44 p;
    Mtx mv;
    guOrtho(p, 0, 480, 0, 640, 0.1f, 1000.0f);
    GX_LoadProjectionMtx(p, GX_ORTHOGRAPHIC);
    guMtxIdentity(mv);
    GX_LoadPosMtxImm(mv, GX_PNMTX0);
    GX_SetCurrentMtx(GX_PNMTX0);
}

/* Vertex format 0: position xyz f32, colour RGBA8, one texture coordinate st f32 (direct). */
static void demo_vtx_pct(void)
{
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
}

static void demo_quad(f32 x, f32 y, f32 w, f32 h, f32 z, GXColor c)
{
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    GX_Position3f32(x, y, z);
    GX_Color4u8(c.r, c.g, c.b, c.a);
    GX_TexCoord2f32(0, 0);
    GX_Position3f32(x + w, y, z);
    GX_Color4u8(c.r, c.g, c.b, c.a);
    GX_TexCoord2f32(1, 0);
    GX_Position3f32(x + w, y + h, z);
    GX_Color4u8(c.r, c.g, c.b, c.a);
    GX_TexCoord2f32(1, 1);
    GX_Position3f32(x, y + h, z);
    GX_Color4u8(c.r, c.g, c.b, c.a);
    GX_TexCoord2f32(0, 1);
    GX_End();
}

/* One colour channel from the vertex colour, one TEV stage passing the rasterised colour, no texture. */
static void demo_flat_tev(void)
{
    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    GX_SetNumTexGens(0);
    GX_SetNumTevStages(1);
    GX_SetNumIndStages(0);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
}

/* Checks that the EFB pixel (x, y) is within tol of the expected colour; bit n of demo_fail on a mismatch. */
static void demo_expect(unsigned n, u16 x, u16 y, u8 r, u8 g, u8 b, int tol)
{
    GXColor c;
    GX_PeekARGB(x, y, &c);
    int dr = c.r - r, dg = c.g - g, db = c.b - b;
    if (dr < -tol || dr > tol || dg < -tol || dg > tol || db < -tol || db > tol)
        demo_fail |= 1u << n;
}

/* The status of the checks drawn into the EFB (bottom-left): a green square when every check passed, else a red
 * one followed by a row of small squares, white for each failed check (bit n = square n). Then the copy to the XFB
 * (clearing the EFB) and a wait for it. */
static void demo_end_frame(void)
{
    GX_DrawDone();
    demo_flat_tev();
    demo_vtx_pct();
    demo_ortho();
    GXColor ok = {0, 200, 0, 255}, bad = {220, 0, 0, 255}, white = {255, 255, 255, 255};
    demo_quad(8, 456, 16, 16, -1, demo_fail ? bad : ok);
    for (unsigned n = 0; n < 32; n++)
        if (demo_fail & (1u << n))
            demo_quad(32 + 8 * n, 462, 6, 6, -1, white);
    GX_DrawDone();
    GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GX_SetColorUpdate(GX_TRUE);
    GX_CopyDisp(demo_xfb, GX_TRUE);
    GX_DrawDone();
    VIDEO_WaitVSync();
}

#endif
