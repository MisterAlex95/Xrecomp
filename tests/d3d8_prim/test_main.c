/*
 * d3d8_fan_quad_indices: triangle fans and Xbox quad lists as triangle-list
 * indices, the form the D3D8 back ends draw them in from vertex and index
 * buffers (D3D11 has neither primitive, GL no quads).
 *
 *   1. without indices: a two-triangle fan and two quads, by hand;
 *   2. through 16- and 32-bit indices (unaligned source too): each output
 *      index is the source index the hand-made vertex list names;
 *   3. the same triangles as the vertex copies DrawPrimitiveUP makes
 *      (convert_fan_or_quad's windings), for random counts.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "d3d8_prim.h"

static int s_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); s_fail = 1; } } while (0)

static uint32_t s_rng = 0x2468ACE1u;
static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return s_rng;
}

/* convert_fan_or_quad's vertex order, as vertex numbers. */
static uint32_t reference(int quads, uint32_t prims, uint32_t *out)
{
    uint32_t i, n = 0;
    for (i = 0; i < prims; i++) {
        if (quads) {
            out[n++] = i * 4; out[n++] = i * 4 + 1; out[n++] = i * 4 + 2;
            out[n++] = i * 4; out[n++] = i * 4 + 2; out[n++] = i * 4 + 3;
        } else {
            out[n++] = 0; out[n++] = i + 1; out[n++] = i + 2;
        }
    }
    return n;
}

int main(void)
{
    uint32_t out[6 * 64], ref[6 * 64], n, i, t;

    /* 1. Without indices. */
    {
        static const uint32_t fan[6] = { 0, 1, 2, 0, 2, 3 };
        static const uint32_t quads[12] = { 0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7 };
        n = d3d8_fan_quad_indices(0, 2, NULL, 0, out);
        CHECK(n == 6 && memcmp(out, fan, sizeof fan) == 0);
        n = d3d8_fan_quad_indices(1, 2, NULL, 0, out);
        CHECK(n == 12 && memcmp(out, quads, sizeof quads) == 0);
        CHECK(d3d8_fan_quad_source_count(0, 2) == 4 && d3d8_fan_quad_source_count(1, 2) == 8);
        CHECK(d3d8_fan_quad_indices(1, 0, NULL, 0, out) == 0);
    }

    /* 2 and 3. Through indices, against the vertex copies' order. */
    for (t = 0; t < 200; t++) {
        int quads = (int)(t & 1);
        uint32_t prims = 1 + rnd() % 64, src32[4 * 64 + 1];
        uint16_t src16[4 * 64 + 1];
        uint8_t raw[4 * (4 * 64) + 1];
        uint32_t ns = d3d8_fan_quad_source_count(quads, prims), nr;
        for (i = 0; i < ns; i++) {
            src32[i] = rnd();
            src16[i] = (uint16_t)rnd();
        }
        nr = reference(quads, prims, ref);
        n = d3d8_fan_quad_indices(quads, prims, NULL, 0, out);
        CHECK(n == nr && memcmp(out, ref, n * 4) == 0);
        n = d3d8_fan_quad_indices(quads, prims, src16, 2, out);
        for (i = 0; i < n; i++)
            CHECK(out[i] == src16[ref[i]]);
        n = d3d8_fan_quad_indices(quads, prims, src32, 4, out);
        for (i = 0; i < n; i++)
            CHECK(out[i] == src32[ref[i]]);
        memcpy(raw + 1, src32, ns * 4);         /* an unaligned index buffer */
        n = d3d8_fan_quad_indices(quads, prims, raw + 1, 4, out);
        CHECK(n == nr);
        for (i = 0; i < n; i++)
            CHECK(out[i] == src32[ref[i]]);
    }

    if (!s_fail)
        printf("d3d8_prim: fans and quad lists as triangle-list indices: ok\n");
    return s_fail;
}
