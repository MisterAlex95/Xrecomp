/* xenos_gl.h -- the first cut of a Xenos renderer on OpenGL ES 3.0 (XBA-270, epic XBA-229 step 3).
 *
 * Draws what the command processor records: at each draw initiator the active vertex and pixel shaders are looked up
 * by the CRC of their microcode in a shader cache of GLSL ES that x360_tools.xenos_glsl wrote ahead of time
 * (vs_XXXXXXXX.vert / ps_XXXXXXXX.frag, as bringup leaves them next to the dumped microcode: static translation,
 * like the code), the vertex fetch constants give the vertex buffers (converted to float on the CPU, the GPU's endian
 * swaps applied), the float / boolean / loop constants go to the shaders' uniform blocks, and the primitive is drawn
 * into an off-screen RGBA8 target. At each swap the target is read back (top row first), optionally written to a
 * frames directory as PPM, and cleared.
 *
 * EGL and GLES are loaded at run time (dlopen, as the GameCube's renderer does): no build dependency; without them
 * (or without a surfaceless EGL display) xenos_gl_create fails with a reason and the runtime runs without drawing.
 *
 * Render state: blending (factors, equations, constant colour), colour mask, depth test / write with a D24S8
 * buffer, stencil (front and back), culling and winding, the window scissor; quad lists become triangles. The vertex
 * shaders convert Direct3D's 0..w depth range to GL's (xenos_glsl).
 *
 * Not there yet: eDRAM and resolves (the guest's front buffer is not written; each swap clears), textures, rectangle
 * lists, alpha test, more vertex formats, multiple render targets, the viewport's exact conventions (the scale /
 * offset registers map to glViewport when enabled). A Vulkan back end (the epic's plan) would take the same state;
 * the GameCube chose GLES 3.0 for the Thor (Plane « Decisions taken »). */
#ifndef XENOS_GL_H
#define XENOS_GL_H

#include <stddef.h>
#include <stdint.h>

#include "xenos_cp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xenos_gl xenos_gl;

typedef struct xenos_gl_stats {
    uint64_t drawn;          /* draws that reached the GPU */
    uint64_t no_shader;      /* skipped: a shader missing from the cache (or not compiling) */
    uint64_t unsupported;    /* skipped: primitive type, vertex format or index source not handled yet */
    uint64_t frames;         /* swaps read back */
    uint32_t missing_shaders; /* distinct shaders missing from the cache */
} xenos_gl_stats;

/* NULL with a reason when EGL / GLES 3.0 cannot be had. width x height: the render target. */
xenos_gl *xenos_gl_create(const char *cache_dir, int width, int height, char *why, size_t why_n);
void xenos_gl_destroy(xenos_gl *g);
/* hooks for xenos_cp (draw / swap) */
void xenos_gl_draw(xenos_gl *g, const xenos_cp *cp, const xenos_draw *d);
void xenos_gl_swap(xenos_gl *g, const xenos_cp *cp);
/* frames: each swap written as DIR/frame_NNNNN.ppm (NULL: none) */
void xenos_gl_set_frames_dir(xenos_gl *g, const char *dir);
/* the last frame read back: RGBA, top row first */
const uint8_t *xenos_gl_frame(const xenos_gl *g, int *width, int *height);
void xenos_gl_get_stats(const xenos_gl *g, xenos_gl_stats *s);
/* where a missing shader is reported (one line per distinct shader: "vs_XXXXXXXX" / "ps_XXXXXXXX"); NULL: stderr */
void xenos_gl_set_log(xenos_gl *g, void *file);

#ifdef __cplusplus
}
#endif

#endif
