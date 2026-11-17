/* Primitive helpers shared by the D3D8 back ends (d3d8_gl.c, d3d8_device.c).
 *
 * Neither D3D11 nor GL draws an Xbox quad list, and D3D11 draws no triangle
 * fan: those become triangle lists. For a draw from a vertex buffer the
 * vertices cannot be rewritten, so the triangles are made of indices:
 *
 *   fan   primitive k: (v0, v[k+1], v[k+2])
 *   quads quad q:      (v[4q], v[4q+1], v[4q+2]), (v[4q], v[4q+2], v[4q+3])
 *
 * where v[k] is the draw's k-th index (16- or 32-bit), or k itself for a
 * draw without indices. The windings are the ones convert_fan_or_quad
 * (d3d8_device.c) gives the vertex copies of DrawPrimitiveUP.
 *
 * Pure C, no D3D type: tests/d3d8_prim checks it on every host. */
#ifndef XBOX_D3D8_PRIM_H
#define XBOX_D3D8_PRIM_H

#include <stdint.h>
#include <string.h>

/* The draw's k-th vertex: src[k] (src_bytes 2 or 4), or k without indices. */
static inline uint32_t d3d8_prim_vertex(const void *src, uint32_t src_bytes, uint32_t k)
{
    if (!src)
        return k;
    if (src_bytes == 4) {
        uint32_t v;
        memcpy(&v, (const uint8_t *)src + (size_t)k * 4u, 4);
        return v;
    } else {
        uint16_t v;
        memcpy(&v, (const uint8_t *)src + (size_t)k * 2u, 2);
        return v;
    }
}

/* Indices the draw reads: prims + 2 for a fan, 4 a quad. */
static inline uint32_t d3d8_fan_quad_source_count(int quads, uint32_t prims)
{
    return quads ? prims * 4u : prims + 2u;
}

/* Triangle-list indices for `prims` primitives of a triangle fan (quads 0)
 * or a quad list (quads 1) into out (3 or 6 a primitive). Returns the
 * number written. */
static inline uint32_t d3d8_fan_quad_indices(int quads, uint32_t prims, const void *src,
                                             uint32_t src_bytes, uint32_t *out)
{
    uint32_t i, n = 0;

    if (quads) {
        for (i = 0; i < prims; i++) {
            uint32_t a = d3d8_prim_vertex(src, src_bytes, i * 4u),
                     b = d3d8_prim_vertex(src, src_bytes, i * 4u + 1u),
                     c = d3d8_prim_vertex(src, src_bytes, i * 4u + 2u),
                     d = d3d8_prim_vertex(src, src_bytes, i * 4u + 3u);
            out[n++] = a; out[n++] = b; out[n++] = c;
            out[n++] = a; out[n++] = c; out[n++] = d;
        }
    } else {
        uint32_t hub = d3d8_prim_vertex(src, src_bytes, 0);
        for (i = 0; i < prims; i++) {
            out[n++] = hub;
            out[n++] = d3d8_prim_vertex(src, src_bytes, i + 1u);
            out[n++] = d3d8_prim_vertex(src, src_bytes, i + 2u);
        }
    }
    return n;
}

#endif /* XBOX_D3D8_PRIM_H */
