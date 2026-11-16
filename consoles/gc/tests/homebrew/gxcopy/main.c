/* gxcopy: EFB copies, peeks and pokes (XBA-262). A source picture (a Gouraud quad with an alpha gradient and a depth
 * ramp) drawn at the top left is copied to textures in ten formats (RGBA8, RGB565, RGB5A3, I8 and IA8 by
 * intensity, I4, A8, R8, the depth's high byte Z8, and a half-size RGBA8 through the box filter), each drawn back as a
 * tile; pokes write colours and depth that peeks read back; a peek reads the ramp's depth. The square at the bottom
 * left is the usual check status; the one beside it tells whether the RGBA8 copy's bytes in RAM hold the source's
 * colours (green: the runtime encoded the copy into RAM, gc_run --gx-copy-ram; grey: it kept it as a texture only). */
#include "../common/gxdemo.h"

#define SRC 128
enum { C_RGBA8, C_RGB565, C_RGB5A3, C_I8, C_IA8, C_I4, C_A8, C_R8, C_Z8, C_HALF, C_N };
static const u32 copy_fmt[C_N] = {GX_TF_RGBA8, GX_TF_RGB565, GX_TF_RGB5A3, GX_TF_I8, GX_TF_IA8, GX_TF_I4,
                                  GX_CTF_A8, GX_CTF_R8, GX_TF_Z8, GX_TF_RGBA8};
static const u8 sample_fmt[C_N] = {GX_TF_RGBA8, GX_TF_RGB565, GX_TF_RGB5A3, GX_TF_I8, GX_TF_IA8, GX_TF_I4,
                                   GX_TF_I8, GX_TF_I8, GX_TF_I8, GX_TF_RGBA8};
static void *buf[C_N];
static unsigned ram_ok;

static void source(void)
{
    demo_flat_tev();
    demo_vtx_pct();
    demo_ortho();
    GX_SetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    GX_Position3f32(16, 16, -10);
    GX_Color4u8(255, 0, 0, 255);
    GX_TexCoord2f32(0, 0);
    GX_Position3f32(16 + SRC, 16, -900);
    GX_Color4u8(0, 255, 0, 255);
    GX_TexCoord2f32(0, 0);
    GX_Position3f32(16 + SRC, 16 + SRC, -900);
    GX_Color4u8(255, 255, 255, 0);
    GX_TexCoord2f32(0, 0);
    GX_Position3f32(16, 16 + SRC, -10);
    GX_Color4u8(0, 0, 255, 0);
    GX_TexCoord2f32(0, 0);
    GX_End();
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
}

static void copies(void)
{
    GX_DrawDone();
    for (int i = 0; i < C_N; i++) {
        GX_SetTexCopySrc(16, 16, SRC, SRC);
        GX_SetTexCopyDst(i == C_HALF ? SRC / 2 : SRC, i == C_HALF ? SRC / 2 : SRC, copy_fmt[i], i == C_HALF);
        GX_CopyTex(buf[i], GX_FALSE);
    }
    GX_PixModeSync();
    GX_InvalidateTexAll();
}

static void tiles(void)
{
    GX_SetNumChans(1);
    GX_SetNumTexGens(1);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GX_SetNumTevStages(1);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
    GXColor white = {255, 255, 255, 255};
    for (int i = 0; i < C_N; i++) {
        unsigned w = i == C_HALF ? SRC / 2 : SRC;
        GXTexObj t;
        GX_InitTexObj(&t, buf[i], w, w, sample_fmt[i], GX_CLAMP, GX_CLAMP, GX_FALSE);
        GX_InitTexObjFilterMode(&t, GX_NEAR, GX_NEAR);
        GX_LoadTexObj(&t, GX_TEXMAP0);
        f32 x, y;
        if (i == C_HALF)
            x = 168, y = 16;
        else
            x = 16 + (i % 4) * 152, y = 168 + (i / 4) * 136;
        if (i >= 8 && i != C_HALF)
            x = 320 + (i - 8) * 152, y = 16;
        f32 s = i == C_HALF ? 64 : 128;
        demo_quad(x, y, s, s, -5, white);
    }
}

static void poke_checks(void)
{
    GXColor c = {10, 20, 30, 255};
    GX_PokeARGB(600, 20, c);
    GX_PokeZ(610, 20, 0x123456);
    GXColor got;
    GX_PeekARGB(600, 20, &got);
    if (got.r != 10 || got.g != 20 || got.b != 30)
        demo_fail |= 1u << 8;
    u32 z = 0;
    GX_PeekZ(610, 20, &z);
    if ((z & 0xFFFFFF) != 0x123456)
        demo_fail |= 1u << 9;
    /* the ramp's depth halfway: z -455 in a 0.1 .. 1000 orthographic range, viewport 0 .. 1 */
    GX_PeekZ(16 + SRC / 2, 16 + SRC / 2, &z);
    float zc = (455.0f - 1000.0f) / 999.9f;
    int expect = (int)(16777215.0f * (1.0f + zc));
    int d = (int)(z & 0xFFFFFF) - expect;
    if (d < -0x20000 || d > 0x20000)
        demo_fail |= 1u << 10;
}

/* Texel (0, 0) of the RGBA8 copy in RAM: AR then GB pairs; the source's top-left corner is red. */
static void ram_check(void)
{
    const u8 *b = MEM_K0_TO_K1(buf[C_RGBA8]);
    ram_ok = b[0] == 255 && b[1] >= 240 && b[32] <= 15 && b[33] <= 15;
}

int main(void)
{
    GXColor bg = {96, 96, 96, 255};
    demo_init(bg);
    for (int i = 0; i < C_N; i++) {
        unsigned w = i == C_HALF ? SRC / 2 : SRC;
        u32 n = GX_GetTexBufferSize(w, w, copy_fmt[i] == GX_TF_Z8 ? GX_TF_I8 : sample_fmt[i], GX_FALSE, 0);
        buf[i] = memalign(32, n);
        memset(buf[i], 0, n);
        DCFlushRange(buf[i], n);
    }
    for (;;) {
        demo_fail = 0;
        source();
        copies();
        tiles();
        GX_DrawDone();
        demo_expect(0, 16 + 2, 168 + 2, 255, 0, 0, 12);          /* RGBA8 copy: the red corner */
        demo_expect(1, 168 + 2, 168 + 2, 255, 0, 0, 12);         /* RGB565 */
        demo_expect(2, 168 + 125, 168 + 2, 0, 255, 0, 12);       /* RGB565: the green corner */
        demo_expect(4, 168 + 2, 16 + 2, 255, 0, 0, 12);          /* the half-size copy */
        demo_expect(5, 472 + 2, 168 + 2, 82, 82, 82, 6);         /* I8: intensity of red, 0.257 * 255 + 16 */
        demo_expect(6, 472 + 2, 304 + 2, 255, 255, 255, 12);     /* R8 of red: 255 */
        demo_expect(7, 320 + 2, 304 + 2, 255, 255, 255, 12);     /* A8 of the top edge: opaque */
        poke_checks();
        ram_check();
        demo_flat_tev();
        demo_vtx_pct();
        demo_ortho();
        GXColor green = {0, 200, 0, 255}, grey = {160, 160, 160, 255};
        demo_quad(48, 456, 16, 16, -1, ram_ok ? green : grey);
        demo_end_frame();
    }
    return 0;
}
