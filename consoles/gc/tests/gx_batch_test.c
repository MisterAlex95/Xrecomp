/* gx_batch_test -- the GLES renderer's draw batch (gx_gl.c) on a headless context: points and line strips become
 * quads (4 vertices, 6 indices each), and a draw that would not fit the batch's vertex buffer flushes it first.
 * gx_gl_draw used to plan 2 vertices per input vertex, so points drawn after a half-full batch of points wrote past
 * the end of the buffer (BATCH_V vertices). Exit 0, 1 on a failure, 77 without a GLES 3 context. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gx/gx.h"
#include "gx/gx_internal.h"

static int failures;

#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        if (!(cond)) {                                                                                                \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                               \
            printf(__VA_ARGS__);                                                                                      \
            printf("\n");                                                                                             \
            failures++;                                                                                               \
        }                                                                                                             \
    } while (0)

static void irq(void *user, uint32_t set, uint32_t clear)
{
    (void)user;
    (void)set;
    (void)clear;
}

static uint64_t batches(gx_state *gx)
{
    gx_stats s;
    gx_get_stats(gx, &s);
    return s.batches;
}

/* n vertices in gx->vout (as gx_load_vertices leaves them), then one draw of primitive prim (GX opcode bits 3-5). */
static void draw(gx_state *gx, unsigned prim, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        gx_vout *v = &gx->vout[i];
        memset(v, 0, sizeof *v);
        v->pos[0] = (float)(i % 200) / 100.0f - 1.0f;
        v->pos[1] = (float)(i / 200 % 200) / 100.0f - 1.0f;
        v->pos[3] = 1.0f;
        memset(v->c0, 0xFF, 4);
    }
    gx->vout_ntex = 0;
    gx_gl_draw(gx, prim, n);
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
    o.async_shaders = -1; /* the ubershader: nothing to compile per state */
    if (gx_gl_attach(gx, &o)) {
        printf("skipped: the renderer does not attach on this context\n");
        gx_destroy(gx);
        gx_egl_headless_destroy();
        return 77;
    }
    gx->bp[BP_LPSIZE] = 6 << 8 | 6; /* point size and line width (1 / 6 pixel units) */

    /* Points: 32750 points = 131000 vertices queued, then 40000 points (160000 more) must flush first. */
    uint64_t b0 = batches(gx);
    draw(gx, 7, 32750);
    CHECK(batches(gx) == b0, "32750 points flushed (%llu batches)", (unsigned long long)(batches(gx) - b0));
    draw(gx, 7, 40000);
    CHECK(batches(gx) == b0 + 1, "40000 more points: %llu flushes, want 1 (the batch would overflow)",
          (unsigned long long)(batches(gx) - b0));
    /* The largest draw fits an empty batch: 65535 points = 262140 vertices. */
    gx_gl_flush(gx);
    b0 = batches(gx);
    draw(gx, 7, 65535);
    CHECK(batches(gx) == b0, "65535 points into an empty batch flushed");
    draw(gx, 7, 2); /* 262140 + 8 > 262144 (one point would still fit) */
    CHECK(batches(gx) == b0 + 1, "two more points after a full batch: %llu flushes",
          (unsigned long long)(batches(gx) - b0));

    /* Line strips: n - 1 quads (4 (n - 1) vertices). */
    gx_gl_flush(gx);
    b0 = batches(gx);
    draw(gx, 6, 32751);
    draw(gx, 6, 40001);
    CHECK(batches(gx) == b0 + 1, "line strips: %llu flushes, want 1", (unsigned long long)(batches(gx) - b0));
    /* Line lists: n / 2 quads, 2n vertices: 65535 then 65535 more fit one batch (262140 vertices). */
    gx_gl_flush(gx);
    b0 = batches(gx);
    draw(gx, 5, 65534);
    draw(gx, 5, 65534);
    CHECK(batches(gx) == b0, "line lists flushed early: %llu", (unsigned long long)(batches(gx) - b0));
    gx_gl_flush(gx);

    gx_gl_detach(gx);
    gx_destroy(gx);
    gx_egl_headless_destroy();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("gx_batch_test: ok\n");
    return 0;
}
