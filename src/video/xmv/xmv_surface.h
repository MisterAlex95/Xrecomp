/* Writing a decoded movie picture into a title's D3D surface (XBA-70):
 * the part of the runtime's XMV hook (xmv_hle.c) that needs no guest. */
#ifndef XMV_SURFACE_H
#define XMV_SURFACE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Writes a YUV 4:2:0 picture (w x h) into a D3D surface's pixels: format is
 * the D3DFORMAT code (bits 8..15 of the resource's Format), sw x sh the
 * surface size, pitch its row pitch (linear formats). Supported: YUY2, UYVY,
 * (LIN_)A8R8G8B8, (LIN_)X8R8G8B8, (LIN_)R5G6B5; swizzled ones need power-of-
 * two sizes. The picture lands at the top left, cropped to the surface.
 * Returns 0, or -1 for a format it does not write. */
int xmv_surface_write(const uint8_t *const plane[3], const int stride[3], int w, int h,
                      uint32_t format, uint32_t sw, uint32_t sh, uint32_t pitch,
                      uint8_t *dst, size_t dst_bytes);

/* Decodes a resource's Format and Size dwords: the D3DFORMAT code, width,
 * height and pitch (Size 0: sizes from the log2 fields, pitch width * bpp).
 * Returns the bytes the pixels span, 0 for a format xmv_surface_write does
 * not know. */
size_t xmv_surface_layout(uint32_t format_dword, uint32_t size_dword,
                          uint32_t *format, uint32_t *w, uint32_t *h, uint32_t *pitch);

#ifdef __cplusplus
}
#endif

#endif
