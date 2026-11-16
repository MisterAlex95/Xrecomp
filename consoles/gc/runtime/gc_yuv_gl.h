/* gc_yuv_gl -- the GameCube XFB to the host window on the GPU (XBA-259): YUV 4:2:2 lines (Y0 U Y1 V) uploaded as an
 * RGBA8 texture of half their width, converted to RGB (BT.601, studio range: what gc_xfb_rgb computes) by a fragment
 * shader into a texture of the window's GL context, which host_window_present_texture then shows. GL entry points
 * come from host_window_gl_proc; GLSL ES 3.00 on a GLES context (Android), 1.50 on a desktop core one. Use from the
 * thread that opened the window. */
#ifndef GC_YUV_GL_H
#define GC_YUV_GL_H

#include <stdint.h>

typedef struct gc_yuv_gl gc_yuv_gl;

/* After host_window_open. NULL (the reason on stderr) when the shader does not build or GL is missing: the caller
 * converts on the CPU instead. filter: HOST_FILTER_* for the converted picture's sampling. */
gc_yuv_gl *gc_yuv_gl_create(int filter);
/* Uploads yuv (w x h pixels, w * 2 bytes per line), converts it and presents it. 1, or 0 on a GL error. */
int gc_yuv_gl_present(gc_yuv_gl *g, const uint8_t *yuv, int w, int h);
void gc_yuv_gl_destroy(gc_yuv_gl *g);

#endif
