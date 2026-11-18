/* gx_shadow_test -- the renderer leaves out the GL state a batch does not change (gx_gl.c, gl_shadow), which is only
 * right while nothing else has changed that state. For each piece of shadowed state: a batch sets it, something else
 * disturbs GL (the window presenting between two fields, an XFB or texture copy, a clear, a poke, a peek), and a second
 * batch with the same GX state must still be drawn with it. Pixels are read back through EFB peeks. No homebrew; a
 * headless GLES 3 context (llvmpipe). Exit 0, 1 on a failure, 77 without a context. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gx/gx.h"
#include "gx/gx_glapi.h"
#include "gx/gx_internal.h"

enum { RED = 0x400000, GREEN = 0x004000, BLACK = 0 };
enum { X0 = 100, Y0 = 100, X1 = 200, Y1 = 200, IN_X = 150, IN_Y = 150, OUT_X = 300, OUT_Y = 300 };

static int failures;
static gx_glfn gl;

static void irq(void *user, uint32_t set, uint32_t clear)
{
    (void)user;
    (void)set;
    (void)clear;
}

/* One quad in EFB pixels, clockwise on screen (a GX front face), drawn at once. */
static void rect(gx_state *gx, int x0, int y0, int x1, int y1, float z, uint32_t rgb)
{
    const int xs[4] = {x0, x1, x1, x0}, ys[4] = {y0, y0, y1, y1};
    for (unsigned i = 0; i < 4; i++) {
        gx_vout *v = &gx->vout[i];
        memset(v, 0, sizeof *v);
        v->pos[0] = (float)xs[i] / GX_EFB_W * 2.0f - 1.0f;
        v->pos[1] = 1.0f - (float)ys[i] / GX_EFB_H * 2.0f;
        v->pos[2] = z;
        v->pos[3] = 1.0f;
        v->c0[0] = (uint8_t)(rgb >> 16), v->c0[1] = (uint8_t)(rgb >> 8), v->c0[2] = (uint8_t)rgb, v->c0[3] = 255;
    }
    gx->vout_ntex = 0;
    gx_gl_draw(gx, 0, 4);
    gx_gl_flush(gx);
}

static uint32_t peek(gx_state *gx, unsigned x, unsigned y)
{
    return gx_efb_peek(gx, x << 2 | y << 12) & 0xFFFFFFu;
}

/* The copy registers for a small rectangle in a corner the cases do not look at. */
static void copy_regs(gx_state *gx, unsigned w, unsigned h)
{
    gx->bp[BP_COPY_SRC] = 0;
    gx->bp[BP_COPY_SIZE] = (w - 1) | (h - 1) << 10;
    gx->bp[BP_COPY_DST] = 0x8000u >> 5;
    gx->bp[BP_COPY_STRIDE] = 8;
}

/* Everything off, the whole EFB cleared to black and far. */
static void reset(gx_state *gx)
{
    uint32_t *bp = gx->bp;
    gx_gl_flush(gx);
    bp[BP_GENMODE] = 0;
    bp[BP_SCISSOR_TL] = 0;
    bp[BP_SCISSOR_BR] = (GX_EFB_W - 1u) << 12 | (GX_EFB_H - 1u);
    bp[BP_SCISSOR_OFFSET] = 0;
    bp[BP_PE_CONTROL] = 0;
    bp[BP_ZMODE] = 0x10;
    bp[BP_CMODE0] = 0x18;
    bp[BP_CLEAR_AR] = bp[BP_CLEAR_GB] = 0;
    bp[BP_CLEAR_Z] = 0xFFFFFF;
    copy_regs(gx, GX_EFB_W, GX_EFB_H);
    gx_gl_copy(gx, 0x4000 | 0x800);
    bp[BP_ZMODE] = 0;
}

typedef enum { D_PRESENT, D_XFB_COPY, D_TEX_COPY, D_CLEAR, D_POKE, D_PEEK, D_COUNT } disturbance;
static const char *const disturbance_names[D_COUNT] = {"present", "XFB copy", "texture copy", "clear", "poke", "peek"};

/* What the window does to the context between two fields, and everything else the renderer does between two batches. */
static void disturb(gx_state *gx, disturbance d)
{
    switch (d) {
    case D_PRESENT:
        gx_field(gx);
        gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
        gl.glViewport(0, 0, 1, 1);
        gl.glDisable(GL_SCISSOR_TEST);
        gl.glScissor(0, 0, 1, 1);
        gl.glDisable(GL_DEPTH_TEST);
        gl.glDepthFunc(GL_NEVER + 7);
        gl.glDepthMask(GL_FALSE);
        gl.glDisable(GL_BLEND);
        gl.glBlendFuncSeparate(GL_ZERO, GL_ZERO, GL_ZERO, GL_ZERO);
        gl.glBlendEquationSeparate(GL_FUNC_REVERSE_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT);
        gl.glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        gl.glDisable(GL_CULL_FACE);
        gl.glCullFace(GL_FRONT_AND_BACK);
        break;
    case D_XFB_COPY:
        copy_regs(gx, 16, 16);
        gx_gl_copy(gx, 0x4000);
        break;
    case D_TEX_COPY:
        copy_regs(gx, 16, 16);
        gx_gl_copy(gx, 0);
        break;
    case D_CLEAR: {
        /* clears colour and depth of the corner whatever the GX state: it needs both updates on */
        uint32_t cm0 = gx->bp[BP_CMODE0], zm = gx->bp[BP_ZMODE];
        gx->bp[BP_CMODE0] = cm0 | 0x18;
        gx->bp[BP_ZMODE] = zm | 0x10;
        copy_regs(gx, 16, 16);
        gx_gl_copy(gx, 0x4000 | 0x800);
        gx->bp[BP_CMODE0] = cm0;
        gx->bp[BP_ZMODE] = zm;
        break;
    }
    case D_POKE:
        gx_efb_poke(gx, 8u << 2 | 8u << 12, 0, 4);
        gx_efb_poke(gx, 0x400000u | 8u << 2 | 8u << 12, 0xFFFFFF, 4);
        break;
    default:
        (void)peek(gx, 8, 8);
        break;
    }
}

static void expect(gx_state *gx, const char *what, disturbance d, unsigned x, unsigned y, uint32_t want)
{
    uint32_t got = peek(gx, x, y);
    if (got != want) {
        printf("FAIL %s after a %s: pixel (%u, %u) is %06X, want %06X\n", what, disturbance_names[d], x, y, got, want);
        failures++;
    }
}

static void check_all(gx_state *gx, disturbance d)
{
    uint32_t *bp = gx->bp;

    /* framebuffer and viewport: only the rectangle of the EFB is drawn */
    reset(gx);
    rect(gx, X0, Y0, X1, Y1, 0, RED);
    disturb(gx, d);
    rect(gx, X0, Y0, X1, Y1, 0, GREEN);
    expect(gx, "framebuffer", d, IN_X, IN_Y, GREEN);
    expect(gx, "viewport", d, OUT_X, OUT_Y, BLACK);

    /* scissor: both draws cover the whole EFB, the box keeps them in the rectangle */
    reset(gx);
    bp[BP_SCISSOR_TL] = (uint32_t)X0 << 12 | Y0;
    bp[BP_SCISSOR_BR] = (uint32_t)(X1 - 1) << 12 | (Y1 - 1);
    rect(gx, 0, 0, GX_EFB_W, GX_EFB_H, 0, RED);
    disturb(gx, d);
    rect(gx, 0, 0, GX_EFB_W, GX_EFB_H, 0, GREEN);
    expect(gx, "scissor box", d, IN_X, IN_Y, GREEN);
    expect(gx, "scissor test", d, OUT_X, OUT_Y, BLACK);

    /* depth: the second draw is farther, so it fails the test and writes nothing */
    reset(gx);
    bp[BP_ZMODE] = 1 | 1u << 1 | 0x10; /* test on, LESS, update on */
    rect(gx, X0, Y0, X1, Y1, 0, RED);
    disturb(gx, d);
    rect(gx, X0, Y0, X1, Y1, 0.5f, GREEN);
    expect(gx, "depth test", d, IN_X, IN_Y, RED);

    /* depth writes off: a near draw leaves the depth alone, so a farther one still passes */
    reset(gx);
    bp[BP_ZMODE] = 1 | 1u << 1; /* test on, LESS, update off */
    rect(gx, X0, Y0, X1, Y1, 0, RED);
    disturb(gx, d);
    rect(gx, X0, Y0, X1, Y1, -0.5f, RED);
    rect(gx, X0, Y0, X1, Y1, 0.5f, GREEN);
    expect(gx, "depth mask", d, IN_X, IN_Y, GREEN);

    /* colour mask: neither draw may write */
    reset(gx);
    bp[BP_CMODE0] = 0;
    rect(gx, X0, Y0, X1, Y1, 0, RED);
    disturb(gx, d);
    rect(gx, X0, Y0, X1, Y1, 0, GREEN);
    expect(gx, "colour mask", d, IN_X, IN_Y, BLACK);

    /* blending: ONE + ONE adds the second draw to the first */
    reset(gx);
    bp[BP_CMODE0] = 1 | 0x18 | 1u << 8 | 1u << 5;
    rect(gx, X0, Y0, X1, Y1, 0, RED);
    disturb(gx, d);
    rect(gx, X0, Y0, X1, Y1, 0, GREEN);
    expect(gx, "blending", d, IN_X, IN_Y, RED | GREEN);

    /* culling: the clockwise quad is a front face, and front faces are culled */
    reset(gx);
    bp[BP_GENMODE] = 2u << 14;
    rect(gx, X0, Y0, X1, Y1, 0, RED);
    disturb(gx, d);
    rect(gx, X0, Y0, X1, Y1, 0, GREEN);
    expect(gx, "culling", d, IN_X, IN_Y, BLACK);
}

int main(void)
{
    static uint8_t ram[1 << 20];
    gx_state *gx = gx_create(ram, sizeof ram, irq, NULL);
    if (!gx) {
        printf("FAIL gx_create\n");
        return 1;
    }
    char why[160] = "";
    if (gx_egl_headless_create(true, why, sizeof why)) {
        printf("skipped: no headless GLES 3 context (%s)\n", why);
        gx_destroy(gx);
        return 77;
    }
    gx_gl_options o;
    memset(&o, 0, sizeof o);
    o.scale = 1;
    o.async_shaders = 0; /* the first draw of a state is the specialised shader's: no ubershader on a software GL */
    gx_eglfn egl;
    if (gx_gl_attach(gx, &o) || gx_egl_load(&egl) || gx_gl_load(&gl, &egl)) {
        printf("skipped: the renderer does not attach on this context\n");
        gx_destroy(gx);
        gx_egl_headless_destroy();
        return 77;
    }

    /* One TEV stage passing the vertex colour through, every pixel kept. */
    uint32_t *bp = gx->bp;
    bp[BP_TEV_COLOR_ENV] = 1u << 19 | 0xFFFAu;     /* clamp, a = b = c = 0, d = rasterised colour */
    bp[BP_TEV_COLOR_ENV + 1] = 1u << 19 | 0xFFD0u; /* clamp, a = b = c = 0, d = rasterised alpha */
    bp[BP_ALPHA_COMPARE] = 7u << 16 | 7u << 19;    /* always, and always */
    for (unsigned t = 0; t < 4; t++) {             /* the swap tables: r g b a as they are */
        bp[BP_TEV_KSEL + 2 * t] = 0u | 1u << 2;
        bp[BP_TEV_KSEL + 2 * t + 1] = 2u | 3u << 2;
    }

    for (int d = 0; d < D_COUNT; d++)
        check_all(gx, (disturbance)d);

    gx_gl_detach(gx);
    gx_destroy(gx);
    gx_egl_headless_destroy();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("gx_shadow_test: ok\n");
    return 0;
}
