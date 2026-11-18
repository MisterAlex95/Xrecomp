/* xenos_gl.h -- the first cut of a Xenos renderer on OpenGL ES 3.0 (XBA-270, epic XBA-229 step 3).
 *
 * Draws what the command processor records: at each draw initiator the active vertex and pixel shaders are looked up
 * by the CRC of their microcode in a shader cache of GLSL ES that x360_tools.xenos_glsl wrote ahead of time
 * (vs_XXXXXXXX.vert / ps_XXXXXXXX.frag, as bringup leaves them next to the dumped microcode: static translation,
 * like the code), the vertex fetch constants give the vertex buffers (converted to float on the CPU, the GPU's endian
 * swaps applied), the float / boolean / loop constants go to the shaders' uniform blocks, and the primitive is drawn
 * into an off-screen RGBA8 target. At each swap the frame is the front buffer VdSwap names when a resolve wrote it
 * since the last swap (decoded from guest memory), else render target 0 as it stands (top row first); it can be
 * written to a frames directory as PPM.
 *
 * EGL and GLES are loaded at run time (dlopen, as the GameCube's renderer does): no build dependency; without them
 * (or without a surfaceless EGL display) xenos_gl_create fails with a reason and the runtime runs without drawing.
 *
 * Render state: blending (factors, equations, constant colour), colour mask, depth test / write with a D24S8
 * buffer, stencil (front and back), culling and winding, the window scissor. Quad lists become triangles; rectangle
 * lists run the vertex shader with transform feedback, get their fourth corners on the CPU and are drawn as
 * triangles through a pass-through vertex shader (15 interpolators: the 16th is zero). The vertex shaders convert
 * Direct3D's 0..w depth range to GL's (xenos_glsl).
 *
 * Textures: 1D, 2D, 3D and cube from the texture fetch constants, linear or tiled, 8 / 8_8 / 8_8_8_8 / 1_5_5_5 /
 * 5_6_5 / 4_4_4_4 / DXT1 / DXT2_3 / DXT4_5 decoded to RGBA8 (their memory compared once per frame and after each
 * resolve, decoded again when it changed), clamp, filter and swizzle from the fetch constant; base level only.
 *
 * Resolves (eDRAM mode "copy"): the colour copied to guest memory as a tiled 8_8_8_8 or 5_6_5 texture at the
 * resolve rectangle, colour / depth / stencil cleared there; once a title clears through resolves, swaps stop
 * clearing. Four host colour targets (oC0-oC3; frames come from target 0, resolves from copy_src_select's) and one
 * depth / stencil target stand for the eDRAM (no eDRAM model: surface pitch, base, formats, MSAA ignored;
 * RB_COLOR_MASK's target 0 bits apply to all four: GLES 3.0 has no per-target mask).
 *
 * Not there yet: the eDRAM model (render target bases, formats, MSAA), the other resolve formats, mips, the other
 * texture formats, vertex fetches at a computed index (instancing), memory export, the
 * viewport's exact conventions (the scale / offset registers map to glViewport when enabled). A Vulkan back end (the
 * epic's plan) would take the same state; the GameCube chose GLES 3.0 for the Thor (Plane « Decisions taken »). */
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
    uint64_t unsupported_textures; /* texture bindings left empty: format / dimension not handled yet */
    uint64_t resolves;             /* render target copies written to guest memory */
    uint64_t front_buffer_frames;  /* frames taken from the front buffer VdSwap named (a resolve wrote it) */
    uint64_t surfaces;             /* eDRAM surfaces made (host render targets) */
    uint64_t copy_textures;        /* textures sampled from a resolve's host picture (no trip through memory) */
    uint64_t mipmapped;            /* textures uploaded with mip levels */
    uint64_t computed_fetches;     /* vertex buffers fetched at a computed index (instancing) */
} xenos_gl_stats;

/* NULL with a reason when EGL / GLES 3.0 cannot be had. width x height: the render target. */
xenos_gl *xenos_gl_create(const char *cache_dir, int width, int height, char *why, size_t why_n);
/* the same at an internal resolution: every eDRAM surface scale (1-8) x its guest size, resolves and frames read
 * back at the guest size */
xenos_gl *xenos_gl_create_scaled(const char *cache_dir, int width, int height, int scale, char *why, size_t why_n);
/* on the caller's current GLES 3 context (a window's: host_window), no EGL of our own; readback off (the frame is
 * xenos_gl_frame_texture's). The caller keeps the context current on the thread that runs the machine and restores
 * what its own drawing needs after the guest's frames (framebuffer 0, viewport, blend / depth / scissor off). */
xenos_gl *xenos_gl_attach(const char *cache_dir, int width, int height, int scale, char *why, size_t why_n);
/* the last swap's picture as a GL texture of *width x *height host pixels, top row first, with the front buffer's
 * swizzle as its texture swizzle (0 before the first frame) */
unsigned xenos_gl_frame_texture(const xenos_gl *g, int *width, int *height);
/* read each swap's frame back into memory (xenos_gl_frame, PPM frames): on by default with xenos_gl_create */
void xenos_gl_set_readback(xenos_gl *g, int on);
int xenos_gl_scale(const xenos_gl *g);
/* after a save state loads: the eDRAM is not in states (as the GameCube's EFB), surfaces start clear and the resolve
 * copies go (frames come from memory again until the title resolves) */
void xenos_gl_reset(xenos_gl *g);
/* hand an attached context back to its owner's drawing: framebuffer 0, blend / depth / stencil / scissor / cull off */
void xenos_gl_end_frame(xenos_gl *g);
void xenos_gl_destroy(xenos_gl *g);
/* hooks for xenos_cp (draw / swap) */
void xenos_gl_draw(xenos_gl *g, const xenos_cp *cp, const xenos_draw *d);
void xenos_gl_swap(xenos_gl *g, const xenos_cp *cp);
/* frames: the first max_frames swaps written as DIR/frame_NNNNN.ppm (NULL: none); later ones are only counted */
void xenos_gl_set_frames_dir(xenos_gl *g, const char *dir, unsigned max_frames);
/* the last frame read back: RGBA, top row first */
const uint8_t *xenos_gl_frame(const xenos_gl *g, int *width, int *height);
void xenos_gl_get_stats(const xenos_gl *g, xenos_gl_stats *s);
/* where a missing shader is reported (one line per distinct shader: "vs_XXXXXXXX" / "ps_XXXXXXXX"); NULL: stderr */
void xenos_gl_set_log(xenos_gl *g, void *file);

#ifdef __cplusplus
}
#endif

#endif
