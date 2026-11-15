/* XMV movies decoded by the runtime (XBA-70). See xmv_surface.h. */
#include "xmv_surface.h"
#include "xmv_player.h"
#include "../../d3d/d3d8_swizzle.h"

#include <stdlib.h>
#include <string.h>

#define FMT_R5G6B5       0x05
#define FMT_A8R8G8B8     0x06
#define FMT_X8R8G8B8     0x07
#define FMT_LIN_R5G6B5   0x11
#define FMT_LIN_A8R8G8B8 0x12
#define FMT_LIN_X8R8G8B8 0x1E
#define FMT_YUY2         0x24
#define FMT_UYVY         0x25

static int fmt_bpp(uint32_t f)
{
    switch (f) {
    case FMT_A8R8G8B8: case FMT_X8R8G8B8: case FMT_LIN_A8R8G8B8: case FMT_LIN_X8R8G8B8:
        return 4;
    case FMT_R5G6B5: case FMT_LIN_R5G6B5: case FMT_YUY2: case FMT_UYVY:
        return 2;
    default:
        return 0;
    }
}

static int fmt_swizzled(uint32_t f)
{
    return f == FMT_R5G6B5 || f == FMT_A8R8G8B8 || f == FMT_X8R8G8B8;
}

size_t xmv_surface_layout(uint32_t fd, uint32_t sd, uint32_t *format, uint32_t *w, uint32_t *h,
                          uint32_t *pitch)
{
    uint32_t f = (fd >> 8) & 0xFF;
    int bpp = fmt_bpp(f);
    if (!bpp)
        return 0;
    if (sd && !fmt_swizzled(f)) {
        *w = (sd & 0xFFF) + 1;
        *h = ((sd >> 12) & 0xFFF) + 1;
        *pitch = (((sd >> 24) & 0xFF) + 1) * 64;
    } else {
        *w = 1u << ((fd >> 20) & 0xF);
        *h = 1u << ((fd >> 24) & 0xF);
        *pitch = *w * (uint32_t)bpp;
    }
    *format = f;
    return (size_t)*pitch * *h;
}

static void bgra_to_565(uint8_t *dst, const uint8_t *src, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        uint16_t v = (uint16_t)(((src[4 * i + 2] >> 3) << 11) | ((src[4 * i + 1] >> 2) << 5) | (src[4 * i] >> 3));
        dst[2 * i] = (uint8_t)v;
        dst[2 * i + 1] = (uint8_t)(v >> 8);
    }
}

int xmv_surface_write(const uint8_t *const plane[3], const int stride[3], int w, int h, uint32_t f,
                      uint32_t sw, uint32_t sh, uint32_t pitch, uint8_t *dst, size_t dst_bytes)
{
    int bpp = fmt_bpp(f), cw = w < (int)sw ? w : (int)sw, ch = h < (int)sh ? h : (int)sh, y;
    uint8_t *lin, *row;
    uint32_t lpitch;
    if (!bpp || cw <= 0 || ch <= 0)
        return -1;
    if (fmt_swizzled(f)) {
        if ((sw & (sw - 1)) || (sh & (sh - 1)) || (size_t)sw * sh * (size_t)bpp > dst_bytes)
            return -1;
        lpitch = sw * (uint32_t)bpp;
        lin = (uint8_t *)calloc((size_t)lpitch * sh, 1);
        if (!lin)
            return -1;
    } else {
        if ((size_t)pitch * (size_t)(ch - 1) + (size_t)cw * (size_t)bpp > dst_bytes || pitch < (uint32_t)(cw * bpp))
            return -1;
        lpitch = pitch;
        lin = dst;
    }
    if (f == FMT_YUY2 || f == FMT_UYVY) {
        xmv_yuv420_to_yuy2(plane, stride, cw & ~1, ch, lin, (int)lpitch);
        if (f == FMT_UYVY) {
            for (y = 0; y < ch; y++) {
                int x;
                row = lin + (size_t)y * lpitch;
                for (x = 0; x + 1 < (cw & ~1) * 2; x += 2) {
                    uint8_t t = row[x];
                    row[x] = row[x + 1];
                    row[x + 1] = t;
                }
            }
        }
    } else if (bpp == 4) {
        xmv_yuv420_to_bgra(plane, stride, cw, ch, lin, (int)lpitch);
    } else {
        uint8_t *tmp = (uint8_t *)malloc((size_t)cw * 4);
        if (!tmp) {
            if (lin != dst) free(lin);
            return -1;
        }
        for (y = 0; y < ch; y++) {
            const uint8_t *pl[3] = { plane[0] + (size_t)y * stride[0], plane[1] + (size_t)(y >> 1) * stride[1],
                                     plane[2] + (size_t)(y >> 1) * stride[2] };
            int st[3] = { 0, 0, 0 };
            /* One row at a time: the row pointers above, stride 0. Chroma
             * rows are shared by line pairs, which y >> 1 already picked. */
            xmv_yuv420_to_bgra(pl, st, cw, 1, tmp, cw * 4);
            bgra_to_565(lin + (size_t)y * lpitch, tmp, (uint32_t)cw);
        }
        free(tmp);
    }
    if (lin != dst) {
        xbox_swizzle_rect(dst, lin, sw, sh, (uint32_t)bpp);
        free(lin);
    }
    return 0;
}
