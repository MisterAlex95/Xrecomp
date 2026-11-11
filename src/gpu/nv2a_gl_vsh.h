/**
 * NV2A vertex programs to GLSL, for the GL back end (nv2a_gl.c, XBA-27 7c).
 *
 * The CPU runs a title's vertex programs in src/kernel/nv2a_vsh_interp.c.
 * This translates the same instruction words (the field table at the top of
 * nv2a_vsh_interp.c) into a GLSL vertex shader that computes the same
 * outputs, rule for rule: MAC/ILU pairs (the paired ILU writes R1), both
 * units reading before either writes, swizzles, negates, masks, a0-relative
 * constants (out of range reads c[0]), temps R13..R15 reading R0, oPos as
 * R12, the colour clamp of vsh_finish, and the ILU's special cases (RCP and
 * RSQ of 0 give +inf, RCC's clamps, LOG of 0, LIT's power clamp and pow(0,
 * p)). When the interpreter changes, change this; tests/nv2a_gl_vsh runs
 * random programs through both.
 *
 * Constants are `uniform vec4 c[192]`; inputs v0..v15 are vertex attributes
 * at locations 0..15 (only the ones the program reads are declared).
 *
 * Two endings:
 *   NV2A_GL_VSH_RENDER   what nv2a_gl.c draws with: oPos (surface pixels,
 *       window z, clip-space w, as on Xbox D3D) mapped to clip space the way
 *       nv2a_gl_vertex_body() maps a CPU-transformed vertex, the near plane at
 *       SET_CLIP_MIN, the fog factor from oFog.x (the executor's fog_factor),
 *       and the varyings the fragment shaders read (v_d0, v_d1, v_fog,
 *       v_t0..v_t3). Uniforms: u_vp = (2/width, 2/height, 2/(zmax - zmin),
 *       0), u_xf = (anti-aliasing x, y, zmin, 0), u_fog (see
 *       nv2a_gl_vsh_fog_uniform). With NV2A_GL_VSH_CLIP_DISTANCE the near
 *       plane is gl_ClipDistance[0] (desktop GL, depth clamp stays on);
 *       without it the caller turns depth clamp off and GL's own near plane
 *       is zmin (window depth comes out as z/zmax with glDepthRange(zmin/zmax,
 *       1)).
 *   NV2A_GL_VSH_CAPTURE  the raw outputs as the interpreter returns them
 *       (Nv2aVshOutput), for transform feedback: x_pos, x_d0, x_d1, x_fog,
 *       x_t0..x_t3, in that order (nv2a_gl_vsh_capture_names).
 *
 * The text is the common subset of "#version 330 core" and "#version 300
 * es"; the caller prepends nv2a_gl_glsl_header(). No GL calls here.
 */
#ifndef XBOXRECOMP_NV2A_GL_VSH_H
#define XBOXRECOMP_NV2A_GL_VSH_H

#include <stddef.h>
#include <stdint.h>

#define NV2A_GL_VSH_RENDER         0x0u
#define NV2A_GL_VSH_CAPTURE        0x1u
#define NV2A_GL_VSH_CLIP_DISTANCE  0x2u   /* RENDER only */

typedef struct {
    uint32_t inputs;        /* bit k: the program reads v[k] */
    uint32_t count;         /* instructions translated (through FINAL) */
    uint32_t relative;      /* reads c[a0.x + n] */
} Nv2aGlVshInfo;

/* Write the shader body for the `count` instructions at `ins` (start slot
 * first). Translation stops at the first FINAL. Returns the length, or 0 if
 * there is no FINAL within `count` or the text did not fit `cap`. */
size_t nv2a_gl_vsh_body(const uint32_t (*ins)[4], uint32_t count, uint32_t flags,
                        char *buf, size_t cap, Nv2aGlVshInfo *info);

/* Inputs as the title stored them (RECOMP_GPU_VBO, nv2a_gl.c): instead of
 * four floats per input gathered on the CPU, the attribute reads the guest's
 * own vertex bytes and the shader turns them into what fetch_attr in
 * nv2a_pb_exec.c returns. nv2a_gl_vsh_attr() says how to point a GL
 * attribute at an NV097 vertex array of `type` and `size`:
 *   kind   NV2A_GL_ATTR_FLOAT  glVertexAttribPointer(GL_FLOAT, comps, no norm)
 *          NV2A_GL_ATTR_UBYTE  glVertexAttribPointer(GL_UNSIGNED_BYTE, comps, normalised)
 *          NV2A_GL_ATTR_SHORT  glVertexAttribPointer(GL_SHORT, comps, not normalised)
 *          NV2A_GL_ATTR_INT    glVertexAttribIPointer(GL_INT, 1)
 *   fmt    the input's 4-bit format code for nv2a_gl_vsh_body_fmt (formats
 *          bits 4k..4k+3 for v[k]); 0 means "GL's conversion is already
 *          fetch_attr's", which is also what every gathered input is.
 * D3DCOLOR (0) reads 4 normalised bytes and swizzles B,G,R,A to R,G,B,A in
 * the shader; normalised shorts (1) arrive as integers and are divided by
 * 32767 in the shader (GL's own normalisation differs between versions);
 * 11:11:10 (6) arrives as an int and is unpacked in the shader. Returns 0
 * for a type or size fetch_attr does not read (the batch is then gathered). */
enum { NV2A_GL_ATTR_FLOAT, NV2A_GL_ATTR_UBYTE, NV2A_GL_ATTR_SHORT, NV2A_GL_ATTR_INT };
#define NV2A_GL_VSH_IN_AS_IS  0u
#define NV2A_GL_VSH_IN_BGRA   1u
#define NV2A_GL_VSH_IN_SNORM  2u    /* bits 2-3: components - 1 */
#define NV2A_GL_VSH_IN_PACKED 3u
typedef struct {
    uint32_t kind, comps, bytes, fmt;   /* bytes: one element in guest memory */
} Nv2aGlVshAttr;
int nv2a_gl_vsh_attr(uint32_t type, uint32_t size, Nv2aGlVshAttr *out);

/* nv2a_gl_vsh_body with input formats (see above); formats 0 is
 * nv2a_gl_vsh_body. */
size_t nv2a_gl_vsh_body_fmt(const uint32_t (*ins)[4], uint32_t count, uint32_t flags,
                            uint64_t formats, char *buf, size_t cap,
                            Nv2aGlVshInfo *info);

/* The transform feedback varyings of NV2A_GL_VSH_CAPTURE, 8 vec4s. */
extern const char *const nv2a_gl_vsh_capture_names[8];

/* u_fog from the executor's fog registers: x the curve (0 linear, 1 exp,
 * 2 exp2), y, z the two FOG_PARAMs, w 0 off, 1 on, 2 on with abs (the _ABS
 * modes and 0x804). Mirrors fog_factor() in nv2a_pb_exec.c. */
void nv2a_gl_vsh_fog_uniform(uint32_t enable, uint32_t mode, float p0, float p1,
                             float out[4]);

#endif
