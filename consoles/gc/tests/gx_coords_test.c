/* gx_coords_test -- the renderer keeps one vertex array per number of texture coordinates in a batch, and a batch with
 * all eight uploads its vertices without packing them (gx_gl.c, gx_gl_flush). Quads of every count, in an order that
 * changes the vertex array and the stride each time, must land where they were drawn in their colour. Pixels are read
 * back through EFB peeks. No homebrew; a headless GLES 3 context (llvmpipe). Exit 0, 1 on a failure, 77 without one. */
#include <stdio.h>
#include <string.h>

#include "gx/gx.h"
#include "gx/gx_internal.h"

enum { RED = 0x400000, GREEN = 0x004000, BLACK = 0 };
enum { X0 = 100, Y0 = 100, X1 = 200, Y1 = 200, IN_X = 150, IN_Y = 150, OUT_X = 300, OUT_Y = 300 };

static void irq(void *user, uint32_t set, uint32_t clear)
{
    (void)user;
    (void)set;
    (void)clear;
}

static void quad(gx_state *gx, unsigned ntex, uint32_t rgb)
{
    const int xs[4] = {X0, X1, X1, X0}, ys[4] = {Y0, Y0, Y1, Y1};
    for (unsigned i = 0; i < 4; i++) {
        gx_vout *v = &gx->vout[i];
        memset(v, 0, sizeof *v);
        v->pos[0] = (float)xs[i] / GX_EFB_W * 2.0f - 1.0f;
        v->pos[1] = 1.0f - (float)ys[i] / GX_EFB_H * 2.0f;
        v->pos[3] = 1.0f;
        v->c0[0] = (uint8_t)(rgb >> 16), v->c0[1] = (uint8_t)(rgb >> 8), v->c0[2] = (uint8_t)rgb, v->c0[3] = 255;
        for (unsigned t = 0; t < ntex; t++)
            v->tex[t][0] = v->tex[t][1] = v->tex[t][2] = 0.5f;
    }
    gx->vout_ntex = ntex;
    gx_gl_draw(gx, 0, 4);
    gx_gl_flush(gx);
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
    o.async_shaders = 0;
    if (gx_gl_attach(gx, &o)) {
        printf("skipped: the renderer does not attach on this context\n");
        gx_destroy(gx);
        gx_egl_headless_destroy();
        return 77;
    }

    /* One TEV stage passing the vertex colour through, every pixel kept, the whole EFB writable. */
    uint32_t *bp = gx->bp;
    bp[BP_TEV_COLOR_ENV] = 1u << 19 | 0xFFFAu;
    bp[BP_TEV_COLOR_ENV + 1] = 1u << 19 | 0xFFD0u;
    bp[BP_ALPHA_COMPARE] = 7u << 16 | 7u << 19;
    for (unsigned t = 0; t < 4; t++) {
        bp[BP_TEV_KSEL + 2 * t] = 0u | 1u << 2;
        bp[BP_TEV_KSEL + 2 * t + 1] = 2u | 3u << 2;
    }
    bp[BP_SCISSOR_BR] = (GX_EFB_W - 1u) << 12 | (GX_EFB_H - 1u);
    bp[BP_CMODE0] = 0x18;

    int failures = 0;
    static const unsigned counts[] = {8, 1, 0, 3, 8, 7, 2, 8, 0, 4, 5, 6};
    for (unsigned i = 0; i < sizeof counts / sizeof counts[0]; i++) {
        uint32_t want = i & 1 ? RED : GREEN;
        quad(gx, counts[i], want);
        uint32_t in = gx_efb_peek(gx, IN_X << 2 | IN_Y << 12) & 0xFFFFFFu;
        uint32_t out = gx_efb_peek(gx, OUT_X << 2 | OUT_Y << 12) & 0xFFFFFFu;
        if (in != want || out != BLACK) {
            printf("FAIL %u texture coordinates: inside %06X (want %06X), outside %06X\n", counts[i], in, want, out);
            failures++;
        }
    }

    gx_gl_detach(gx);
    gx_destroy(gx);
    gx_egl_headless_destroy();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("gx_coords_test: ok\n");
    return 0;
}
