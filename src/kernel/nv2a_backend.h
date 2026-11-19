/*
 * NV2A render back-end interface.
 *
 * The pushbuffer executor (nv2a_pb_exec.c) decodes what a title asks the GPU
 * to do. By default it carries that out itself, on the CPU, into guest memory.
 * A game project can instead register a back end -- a D3D11 renderer, say --
 * and the executor hands it the work in already-decoded form:
 *
 *   surface state + clears, triangles (transformed to surface pixels, with
 *   per-vertex colour and texel-space UVs), the texture each batch samples,
 *   and the flip that ends a frame.
 *
 * Everything is called on the NV2A poll thread, one call at a time, so a back
 * end may create its window and device lazily on the first call and pump its
 * window messages from flip().
 *
 * Fixed-function, pre-transformed and vertex-program batches all arrive the
 * same way: the executor transforms fixed-function vertices itself, runs
 * vertex programs on the CPU (nv2a_vsh_interp.c) and clips at the near plane,
 * so a back end only ever sees surface pixels. Blend, depth, alpha, cull and
 * stencil state arrive in Nv2aRenderState.
 *
 * A back end that sets NV2A_BACKEND_CAP_PIXEL in Nv2aBackend.caps also gets,
 * with every batch, the full vertex (both colours, fog, the four stages'
 * texture coordinates, clip-space w) and the pixel state: the register
 * combiners, all four texture stages, fog colour and clip-plane modes. That
 * is what a GPU renderer needs to reproduce the CPU rasteriser's colour
 * (src/gpu/nv2a_gl.c is one). Without the flag a back end sees stage 0 and
 * the diffuse colour only, as before.
 *
 * A back end that also sets NV2A_BACKEND_CAP_PROGRAM and draw_program runs
 * vertex programs itself: a batch drawn with a vertex program then arrives
 * untransformed (Nv2aProgramBatch: the program, the constants with change
 * stamps, the batch's attributes gathered once per unique vertex, and a
 * triangle index list), with the same surface, render and pixel state.
 * Fixed-function and screen-space batches still arrive as triangles.
 */
#ifndef NV2A_BACKEND_H
#define NV2A_BACKEND_H

#include <stdint.h>
#include "nv2a_combiner.h"   /* Nv2aCombiner, for Nv2aPixelState */

/* The colour surface being drawn into, in real (anti-aliased) pixels.
 * aa_sx/aa_sy give the anti-aliasing factor, so width/aa_sx is the logical
 * size the title thinks it renders at (e.g. 640x480). */
typedef struct {
    uint32_t color_va;          /* guest address of pixel (0,0) */
    uint32_t width, height;     /* clip rectangle size, real pixels */
    uint32_t pitch;             /* bytes per row */
    uint32_t bytes_per_pixel;   /* 2 or 4 */
    uint32_t aa_sx, aa_sy;      /* 1 or 2 each */
    /* SET_SURFACE_FORMAT as sent: colour format bits 0-3, zeta format bits
     * 4-7 (1 Z16, 2 Z24S8), type bits 8-11 (2 swizzled: log2 width/height in
     * bits 16-19/24-27), anti-aliasing bits 12-15. clip_x/clip_y: the clip
     * rectangle's origin in real pixels (width/height above are its size). */
    uint32_t format;
    uint32_t clip_x, clip_y;
    /* 0: there is no colour surface to write -- SET_SURFACE_COLOR_OFFSET is
     * 0 (a depth-only pass such as a shadow map) or the surface would land
     * on the title's image. A back end then draws into depth/stencil alone
     * (rs->zeta_va) and neither creates nor reads back a colour target at
     * color_va: Fable's 768x768 shadow pass at offset 0 resolved to
     * 0x80000000 and its read-back overwrote the title's textures there.
     * clear() then carries Z/stencil flags only. */
    uint32_t color_valid;
    /* draw() only: the colour surface is there (color_valid) but
     * SET_COLOR_MASK writes no channel -- a depth pre-pass or a visibility
     * proxy on the frame's own surface. A back end may draw it into the
     * colour target with every channel masked (it then writes nothing
     * there and need not read it back), or into depth/stencil alone. Fable
     * interleaves such batches with its normal ones on the back buffer; sent
     * as color_valid 0 they cost a framebuffer switch each way. */
    uint32_t color_masked;
} Nv2aSurface;

/* A texture as the title programmed it. uv in Nv2aVertex are in texels. */
typedef struct {
    uint32_t offset;            /* guest address of texel (0,0) */
    uint32_t width, height;
    uint32_t pitch;             /* linear formats only */
    uint32_t color;             /* NV097 colour-format code */
    uint32_t addr_u, addr_v;    /* NV097 wrap mode per axis (1 wrap, 3 clamp) */
    /* Mip levels stored after level 0 (the format word's MIPMAP_LEVELS; 1 for
     * a linear texture) and the raw SET_TEXTURE_FILTER word (MIN in bits
     * 16-23). A back end that ignores them samples level 0. */
    uint32_t levels, filter;
    /* Guest address of the 256-entry A8R8G8B8 palette (format 0x0B), the
     * border colour (0xAARRGGBB) and whether FORMAT selects it for address
     * mode 4, and whether the texture is a cube map (six faces, each
     * nv2a_backend_texture_bytes() / 6 apart). Zero in a struct a back end
     * builds itself means: no palette, no border, not a cube. */
    uint32_t palette, border, border_color, cube;
    /* SET_TEXTURE_CONTROL0 as sent: MIN_LOD_CLAMP bits 18-29 and
     * MAX_LOD_CLAMP bits 6-17, 4.8 fixed point (xemu). The LOD bias is in
     * `filter` bits 0-12, signed 5.8. 0 in a struct a back end builds
     * itself: level 0 only. */
    uint32_t control0;
    /* A volume (FORMAT DIMENSIONALITY 3): depth slices (BASE_SIZE_P), 0 or
     * 1 for a 2D texture; addr_p the third axis' wrap mode (ADDRESS bits
     * 16-19). Swizzled volumes are one 3D Morton order per level
     * (xbox_swizzle_offset_3d), each level halving all three sizes. */
    uint32_t depth, addr_p;
} Nv2aTexture;

typedef struct {
    float    x, y, z;           /* surface pixels; z as the title produced it */
    float    rhw;               /* 1/w, 1 for pre-transformed batches */
    uint32_t diffuse;           /* 0xAARRGGBB */
    float    u, v;              /* texels */
} Nv2aVertex;

/* Render state as the title set it, in NV2A's own (OpenGL) enum values:
 * blend factors GL_ZERO/GL_ONE/0x300..0x308/0x8001..0x8004, equations
 * GL_FUNC_ADD 0x8006 etc., compare functions GL_NEVER 0x200 .. GL_ALWAYS 0x207.
 * color_mask uses NV2A's bytes: A 0x01000000, R 0x00010000, G 0x100, B 0x1. */
typedef struct {
    uint32_t blend_enable, blend_src, blend_dst, blend_eq, blend_color;
    uint32_t alpha_test_enable, alpha_func, alpha_ref;   /* ref 0..255 */
    uint32_t depth_test_enable, depth_func, depth_write;
    uint32_t color_mask;
    uint32_t cull_enable, cull_face, front_face;
    uint32_t zeta_va;                                    /* 0: no depth surface */
    float    depth_min, depth_max;                       /* SET_CLIP_MIN/MAX */
    /* SET_STENCIL_*: compare function as above; ops GL_KEEP 0x1E00, GL_ZERO 0,
     * GL_REPLACE 0x1E01, GL_INCR 0x1E02, GL_DECR 0x1E03, GL_INVERT 0x150A,
     * GL_INCR_WRAP 0x8507, GL_DECR_WRAP 0x8508. Reset values GL_ALWAYS, masks
     * 0xFF, GL_KEEP. A title can confine a pass to the pixels an earlier pass
     * marked (a full-screen quad that only marked pixels pass); a back end
     * that ignores these draws that pass over the whole surface. */
    uint32_t stencil_enable, stencil_func, stencil_ref, stencil_func_mask;
    uint32_t stencil_write_mask, stencil_fail, stencil_zfail, stencil_zpass;
    /* SET_ZPASS_PIXEL_COUNT_ENABLE: a visibility test is counting the pixels
     * this batch passes the depth test with (see zpass_read). */
    uint32_t zpass_enable;
    /* SET_POLY_OFFSET_FILL_ENABLE, SET_POLYGON_OFFSET_SCALE_FACTOR / _BIAS:
     * glPolygonOffset's factor and units, the units counted in the zeta
     * format's own steps (1/0xFFFF of the range for Z16). Shadow-map
     * passes push their depth back with these. The CPU rasteriser ignores
     * them. */
    uint32_t poly_offset_fill;
    float    poly_offset_factor, poly_offset_units;
} Nv2aRenderState;

/* A vertex as the CPU transform left it, for back ends with
 * NV2A_BACKEND_CAP_PIXEL. pos: x, y in surface pixels (real, anti-aliased),
 * z the window z the title produced (0..0xFFFF for Z16, 0..0xFFFFFF for
 * Z24S8; not clamped), w the clip-space w (> 0 after near-plane clipping, 1
 * for screen-space batches). Interpolate with 1/w like the rasteriser.
 * tex: what the vertex program wrote to oT0..oT3 (s, t, r, q): normalised
 * for swizzled and DXT textures, in texels for linear ones. fog: the fog
 * factor after the fog unit (1 = no fog), what the combiners read as fog.a. */
typedef struct {
    float pos[4];
    float d0[4], d1[4];         /* diffuse, specular: r, g, b, a in 0..1 */
    float fog;
    float tex[4][4];
} Nv2aVertexFull;

/* How the CPU rasteriser decides a batch's colour, which a back end should
 * copy to draw the same picture. */
enum {
    NV2A_SHADE_COMBINERS = 0,   /* register combiners (Nv2aPixelState.rc) */
    NV2A_SHADE_MODULATE  = 1,   /* stage 0 texel * diffuse (no combiner program yet) */
    NV2A_SHADE_REPLACE   = 2    /* screen-space batch: stage 0 texel, else the
                                 * first vertex's colour, flat; no depth test, no
                                 * alpha test; tex[0] in texels for every format */
};

/* One texture stage. mode is the stage's SHADER_STAGE_PROGRAM field
 * (0 none, 1 2D, 2 3D, 3 cube, 4 pass-through, 5 clip plane, 6 bump-env,
 * 7 bump-env with luminance, 8 and up the dot-product and dependent-read
 * modes, Nv2aPixelState.dot_mapping); valid says the stage's texture
 * registers describe something samplable.
 *
 * Stages 1-3, modes 6 and 7 (xemu psh.c PS_TEXTUREMODES_BUMPENVMAP[_LUM]):
 * the stage samples its texture at its coordinates plus bump_mat times
 * (dU, dV), dU/dV the signed blue and green of the input stage's texel
 * (Nv2aPixelState.other_stage_input); mode 7 then multiplies the texel by
 * bump_scale * L + bump_offset, L the input texel's red.
 * SET_TEXTURE_SET_BUMP_ENV_MAT(st) words as matrix elements m00, m01, m10,
 * m11 (the method's own word order is 00, 01, 11, 10). */
typedef struct {
    uint32_t    mode;
    uint32_t    valid;
    Nv2aTexture tex;
    float       bump_mat[4];
    float       bump_scale, bump_offset;
} Nv2aTexStage;

typedef struct {
    uint32_t     shade;             /* NV2A_SHADE_* */
    Nv2aCombiner rc;                /* meaningful for NV2A_SHADE_COMBINERS */
    uint32_t     fog_color;         /* 0xAARRGGBB (converted from FOG_COLOR) */
    uint32_t     clip_plane_mode;   /* SET_SHADER_CLIP_PLANE_MODE */
    Nv2aTexStage stage[4];
    /* SET_SHADOW_DEPTH_FUNC (0x1E6C): 0 NEVER .. 7 ALWAYS, in GL's order
     * (GL_NEVER + value). A stage sampling a depth texture format
     * (0x2A-0x31, a shadow map) in PROJECT3D mode reads 1 where
     * "stored depth <func> r/q" holds, r/q clamped to the depth format's
     * range (xemu psh.c, psh_append_shadowmap), else 0; in PROJECT2D mode
     * "stored depth <func> 0". The CPU rasteriser reads such stages as
     * white. */
    uint32_t     shadow_func;
    /* SET_SHADER_OTHER_STAGE_INPUT (0x1E78): the stage a bump-env stage
     * reads dU/dV from -- stage 1 always stage 0, stage 2 bits 16-19,
     * stage 3 bits 20-23 (xemu psh.c input_tex). The dot-product and
     * dependent-read modes read their input stage the same way. */
    uint32_t     other_stage_input;
    /* XBA-311, the modes 8 and up (xemu psh.c, NV_texture_shader). A stage
     * in a dot mode computes dot_i = dot(its s, t, r; map(input texel)),
     * map from SET_DOT_RGBMAPPING (0x1E74; stage 1 bits 0-3, stage 2 4-7,
     * stage 3 8-11): 0 the texel's r, g, b as they are, 1 (b - 128) / 127,
     * 2 two's complement GL-style ((2b + 1) / 255), 3 two's complement over
     * 127, 4 HILO (g, b, 1), 5-7 signed HILO (g, b) as 1-3 with a third
     * component sqrt(1 - g^2 - b^2). Mappings 1-7 read the texel's bytes as
     * fetched; 0 after SET_TEXTURE_FILTER's signed channels. Then:
     *   0x11 DOTPRODUCT: dot_i, reads zero;
     *   0x09 DOT_ST: 2D at (dot_i-1, dot_i);
     *   0x0A DOT_ZW: reads zero; the fragment's depth is dot_i-1 / dot_i, the
     *        fragment dropped outside 0..1 (GPU only);
     *   0x0B DOT_RFLCT_DIFF (stage 2): cube at (dot_1, dot_2, dot_3), dot_3
     *        stage 3's own dot;
     *   0x0C DOT_RFLCT_SPEC (stage 3): cube at R = 2 N (N.E) / (N.N) - E,
     *        N = (dot_1, dot_2, dot_3), E the q of stages 1-3;
     *   0x12 DOT_RFLCT_SPEC_CONST: the same, E = eye (SET_EYE_VECTOR);
     *   0x0D DOT_STR_3D / 0x0E DOT_STR_CUBE (stage 3): volume / cube at
     *        (dot_1, dot_2, dot_3);
     *   0x0F DPNDNT_AR / 0x10 DPNDNT_GB: 2D at the input texel's (a, r) /
     *        (g, b);
     *   0x08 BRDF: not modelled, reads zero.
     * Results are texture coordinates as a PROJECT2D stage's are (texels
     * for a linear texture). */
    uint32_t     dot_mapping;
    float        eye[3];
} Nv2aPixelState;

/* Stage modes that compute a dot (above), and the ones that read another
 * stage's texel (bump-env, dot, dependent reads). */
static inline int nv2a_stage_mode_dot(uint32_t mode)
{
    return (mode >= 0x09 && mode <= 0x0E) || mode == 0x11 || mode == 0x12;
}

static inline int nv2a_stage_mode_reads_stage(uint32_t mode)
{
    return mode == 6 || mode == 7 || nv2a_stage_mode_dot(mode) || mode == 0x0F || mode == 0x10;
}

/* The stage a stage-reading stage reads (xemu psh.c input_tex): stage 1
 * stage 0, stages 2 and 3 SET_SHADER_OTHER_STAGE_INPUT bits 16-19 / 20-23;
 * -1 for stage 0 or an input that is not fetched yet (not an earlier
 * stage), which reads zero. */
static inline int nv2a_stage_input(uint32_t other_stage_input, int st)
{
    uint32_t src = st == 1 ? 0u : (other_stage_input >> (st == 2 ? 16 : 20)) & 0xFu;
    return st >= 1 && (int)src < st ? (int)src : -1;
}

/* A vertex-program batch for a back end with NV2A_BACKEND_CAP_PROGRAM.
 *
 * Program: the instruction words (nv2a_vsh_interp.c encoding) from the start
 * slot through FINAL; `hash` is equal for equal words. Constants: c[0..191]
 * as the program reads them; const_stamp[i] is the serial of the last write
 * to c[i] and const_serial the newest, so a copy synced at serial S needs
 * exactly the c[i] with const_stamp[i] > S.
 *
 * Vertices: `attrs` holds vertex_count vertices, each the attributes in
 * `streams` (bit a = v[a]) as four floats, in increasing a: what the
 * executor's own fetch reads (D3DCOLOR as r, g, b, a; absent components
 * 0, 0, 0, 1). Every other input reads imm[a], its current value. Only
 * inputs the program reads are streamed. indices: a triangle list into
 * attrs, in the order and winding the CPU path draws the primitive.
 *
 * The program's oPos is in the title's logical pixels: x, y scale by aa_sx,
 * aa_sy to surface pixels; z is window z; w the clip-space w, +0 meaning 1
 * (transform_vertex). The executor clips at w > 0 and at the near plane
 * z >= state->depth_min (SET_CLIP_MIN). fog_*: the fog unit's registers;
 * the fog factor from oFog.x is fog_factor() in nv2a_pb_exec.c. */
/* A vertex array as the title programmed it (SET_VERTEX_DATA_ARRAY_*):
 * element i of the input is at guest address offset + i * stride, in NV097
 * data type `type` (0 D3DCOLOR, 1 normalised short, 2 float, 4 normalised
 * unsigned byte, 5 short, 6 packed 11:11:10) with `size` components. */
typedef struct {
    uint32_t offset, type, size, stride;
} Nv2aRawAttr;

/* A fixed-function batch for a back end's generated vertex shader
 * (Nv2aProgramBatch.ffp; on by default, RECOMP_GPU_FFP=0 off; XBA-134,
 * XBA-306). What ffp_batch_vertex and ffp_batch_lit in nv2a_pb_exec.c
 * compute, as state the shader is built from; the numbers are in
 * Nv2aProgramBatch.constants:
 *   c[0..3]   the composite matrix's rows (clip = c[i] . v0; with vertex
 *             blending clip = c[i] . e, e the blended eye position)
 *   c[4]      the viewport offset c[59] (xyz), added after the divide
 *   c[5..7]   the model-view matrix's rows (eye = c[i].xyz . v0.xyz + c[i].w;
 *             normals by c[i].xyz); c[41] its fourth row (eye w)
 *   c[8]      scene ambient (rgb), material alpha (w)
 *   c[9 + 4 i .. 12 + 4 i], light i of nlights: direction or eye-space
 *             position (xyz) and range (w); attenuation (xyz); ambient;
 *             diffuse.
 *   c[42..53] model-view matrices 1..3, four rows each (vertex blending)
 *   c[54], c[55] SET_SPECULAR_PARAMS front, back (L, M, N)
 *   c[56]     SET_EYE_POSITION
 *   c[57]     back scene ambient (rgb), back material alpha (w)
 *   c[58 + 7 i .. 64 + 7 i], light i: specular colour; infinite half
 *             vector; spot falloff (L, M, N); spot direction (xyzw); back
 *             ambient, diffuse, specular colours
 *   c[114 + 4 s + k] stage s's texgen plane k (S, T, R, Q)
 *   c[130 + 4 s + r] stage s's texture matrix, row r
 * Inputs: v0 the position; v[nrm] the normal, v[col] the colour (D3DCOLOR
 * as r, g, b, a), v[spc] the specular colour, v[wgt] the blend weights,
 * v[tex[s]] stage s's coordinates, each only when its bit is in
 * Nv2aProgramBatch.streams (else: no normal -- unlit --, white, 0,0,0,1).
 * The position comes out as a vertex program's oPos would: (clip.xyz /
 * clip.w + c[4].xyz, clip.w), logical pixels. */
typedef struct {
    uint32_t streams;               /* = Nv2aProgramBatch.streams: inputs present */
    uint32_t nrm, col, tex[4];      /* input slots */
    uint32_t lit;                   /* SET_LIGHTING_ENABLE with a normal array */
    uint32_t flip;                  /* RECOMP_FFP_FLIP_NORMALS */
    uint32_t amb_vc, dif_vc;        /* SET_COLOR_MATERIAL: ambient / diffuse from v[col] */
    uint32_t nlights;
    uint32_t light_kind[8];         /* 1 infinite, 2 local, 3 spot */
    /* XBA-306 */
    uint32_t spc, wgt;              /* input slots: specular colour, blend weights */
    uint32_t skin, skin_mix;        /* matrices blended (0 off); last weight 1 - the others */
    uint32_t specular;              /* SET_SPECULAR_ENABLE */
    uint32_t separate, local_eye;   /* SET_LIGHT_CONTROL */
    uint32_t spc_src;               /* SET_COLOR_MATERIAL specular: 1 v[col], 2 v[spc] */
    uint32_t two_side;              /* SET_TWO_SIDE_LIGHT_EN */
    uint32_t texgen[4][4];          /* stage, channel: 0 off, 1 eye linear, 2 object
                                     * linear, 3 sphere map, 4 reflection, 5 normal */
    uint32_t texgen_inf;            /* SET_TEXGEN_VIEW_MODEL: infinite viewer */
    uint32_t texmat[4];             /* SET_TEXTURE_MATRIX_ENABLE */
} Nv2aFfpKey;

typedef struct {
    const uint32_t (*ins)[4];
    uint32_t        ins_count;
    uint64_t        hash;
    const float    (*constants)[4];
    const uint64_t *const_stamp;
    uint64_t        const_serial;
    const float    *attrs;
    uint32_t        vertex_count;
    uint32_t        streams;
    const float    (*imm)[4];       /* [16] */
    const uint16_t *indices;
    uint32_t        index_count;
    float           aa_sx, aa_sy;
    uint32_t        fog_enable, fog_mode;
    float           fog_param[2];
    /* NV2A_BACKEND_CAP_RAW_VERTEX only. raw non-NULL: attrs is NULL and the
     * inputs in `streams` are to be read from guest memory, raw[a] for v[a]
     * (16 entries); `indices` count from element raw_first (vertex i of the
     * batch is element raw_first + i of every stream) and vertex_count
     * elements from raw_first on are inside guest memory. A back end that
     * cannot read some format returns 0 from draw_program; the executor then
     * sends the same batch gathered (raw NULL). */
    const Nv2aRawAttr *raw;
    uint32_t        raw_first;
    /* Non-NULL: no program (ins NULL, ins_count 0) but the fixed-function
     * unit, as Nv2aFfpKey says; `hash` is equal for equal keys and differs
     * from every program's. A back end that does not build such shaders
     * returns 0 (the executor then transforms on the CPU). */
    const Nv2aFfpKey *ffp;
} Nv2aProgramBatch;

/* Nv2aBatch.prim: what vertices[] / Nv2aProgramBatch.indices list. */
enum {
    NV2A_PRIM_TRIANGLES = 0,        /* a triangle list (count a multiple of 3) */
    NV2A_PRIM_POINTS    = 1,        /* one vertex a point */
    NV2A_PRIM_LINES     = 2         /* a line list: two vertices a segment */
};

typedef struct {
    const Nv2aVertex  *vertices;    /* triangle list: count is a multiple of 3 */
    uint32_t           count;
    const Nv2aTexture *texture;     /* NULL: untextured */
    const Nv2aRenderState *state;
    /* NV2A_BACKEND_CAP_PIXEL only (NULL otherwise): count entries matching
     * vertices[], and the pixel state. */
    const Nv2aVertexFull  *full;
    const Nv2aPixelState  *pixel;
    /* draw_program only (NULL in draw()): vertices and full are NULL then,
     * count is 0, and pixel is always set. */
    const Nv2aProgramBatch *program;
    /* NV2A_BACKEND_CAP_POINTS_LINES only (0 otherwise): NV2A_PRIM_POINTS or
     * _LINES, the vertex / index list then being points or segment pairs
     * (strips and loops arrive as pairs). point_size: SET_POINT_SIZE in
     * surface pixels (logical size times the anti-aliasing factor);
     * point_sprite: SET_POINT_SMOOTH_ENABLE, which the NV2A uses for point
     * sprites (xemu): every stage samples at the position inside the point,
     * (0,0) at its top left, instead of the vertex's coordinates. */
    uint32_t prim;
    float    point_size;
    uint32_t point_sprite;
} Nv2aBatch;

#define NV2A_BACKEND_CAP_PIXEL 0x1u
/* The back end culls from the winding (Nv2aRenderState cull_*): the executor
 * then hands over every triangle strip with one winding (odd triangles with
 * their first two vertices swapped, as RECOMP_STRIP_ALT_WINDING does), as the
 * hardware assembles strips. Without it strips arrive as (i, i+1, i+2). */
#define NV2A_BACKEND_CAP_CULL  0x2u
#define NV2A_BACKEND_CAP_PROGRAM 0x4u   /* with CAP_PIXEL and draw_program */
/* With CAP_PROGRAM: vertex-program batches drawn from vertex arrays (not
 * INLINE_ARRAY) may arrive with their arrays as the title stored them
 * (Nv2aProgramBatch.raw) instead of gathered. */
#define NV2A_BACKEND_CAP_RAW_VERTEX 0x8u
/* With CAP_PIXEL: points and lines arrive as such (Nv2aBatch.prim) from
 * every path (screen space, fixed function, vertex programs); without it
 * they are not drawn (the CPU rasteriser draws none either) and counted as
 * unsupported (nv2a_coverage.h). */
#define NV2A_BACKEND_CAP_POINTS_LINES 0x10u
/* With CAP_PIXEL: the back end samples volume textures (Nv2aTexture.depth);
 * without it a stage holding one is counted as unsupported. */
#define NV2A_BACKEND_CAP_VOLUME 0x20u

typedef struct {
    /* flags: CLEAR_SURFACE bits (Z 0x1, stencil 0x2, colour 0xF0).
     * zstencil: SET_ZSTENCIL_CLEAR_VALUE (depth in the top 24 bits for Z24S8). */
    void (*clear)(const Nv2aSurface *s, const Nv2aRenderState *rs,
                  uint32_t flags, uint32_t argb, uint32_t zstencil);
    void (*draw)(const Nv2aSurface *s, const Nv2aBatch *b);
    void (*flip)(void);
    uint32_t caps;              /* NV2A_BACKEND_CAP_*; 0 in older initialisers */
    /* Visibility tests. The pixels the back end's batches passed the depth
     * test with while zpass_enable was set, since the previous call (the
     * call resets the count). The executor calls it at GET_REPORT, adding to
     * what it reports, and at CLEAR_REPORT_VALUE, discarding. NULL: reports
     * carry 0 for a back end's batches. A count may be a lower bound
     * (GLES 3.0 queries say only "any passed": report 1). */
    uint32_t (*zpass_read)(void);
    /* Deferred visibility tests (both or neither; NULL: the executor reads
     * zpass_read synchronously). zpass_end closes the count of the batches
     * drawn with zpass_enable since the previous zpass_end and returns a
     * ticket for it, without waiting for the GPU. zpass_resolve(ticket,
     * block, &count) returns 1 with that count once it is in, 0 while it is
     * not (only when block is 0; with block set it waits). The executor
     * resolves tickets in the order zpass_end issued them, and writes the
     * report slot only then (see nv2a_pb_exec.c, "Visibility reports").
     * RECOMP_GPU_ZPASS=sync uses zpass_read instead. */
    uint32_t (*zpass_end)(void);
    int (*zpass_resolve)(uint32_t ticket, int block, uint32_t *count);
    /* NV2A_BACKEND_CAP_PROGRAM: draw a vertex-program batch (b->program).
     * Return 0 to decline it (the executor then transforms on the CPU and
     * calls draw() with triangles); RECOMP_GPU_VSH=0 never calls it. */
    int (*draw_program)(const Nv2aSurface *s, const Nv2aBatch *b);
    /* Optional, with zpass_resolve: submit the GPU work queued so far
     * without waiting for it (glFlush), so the counts zpass_resolve polls
     * for come in. The executor calls it at most once per report that a
     * title may be spinning on (unresolved at the head of the queue for
     * 300 us across idle calls), never with RECOMP_GPU_ZPASS=lazy. NULL:
     * the executor waits for that report after 2 ms instead. */
    void (*zpass_flush)(void);
    /* Optional, save states (XBA-248, xbox_dev_state.h), on the executor
     * thread with the title frozen. NV2A_BACKEND_STATE_FLUSH, before a
     * capture: everything only the GPU holds goes to guest memory (every
     * render target read back, a present read-back in flight completed), so
     * the state's RAM has the picture. NV2A_BACKEND_STATE_DROP, after a load:
     * forget every cache (targets, depth, textures, programs, vertex
     * buffers); they rebuild from the restored guest memory. Depth held only
     * by the GPU is not read back: a frame's depth starts cleared after a
     * load. NULL: the back end keeps nothing guest memory does not have. */
    void (*state)(int op);
} Nv2aBackend;

enum { NV2A_BACKEND_STATE_FLUSH = 1, NV2A_BACKEND_STATE_DROP = 2 };

/* The NV2A poll loop calls this when a tick found nothing new to walk
 * (the walker has caught up with PUT): pending visibility reports the GPU
 * has finished are written then, without waiting; one the title may be
 * spinning on is flushed for, then waited for (see nv2a_pb_exec.c). */
void nv2a_pb_exec_idle(void);

/* Register (or, with NULL, remove) the back end. Call before the title starts
 * submitting work; the executor must also be enabled (RECOMP_PB_EXEC). */
void nv2a_backend_register(const Nv2aBackend *backend);

/* Register a back end that must synchronize a surface into guest memory
 * before the flip's window snapshot or framebuffer dump. present_sync runs
 * synchronously on the executor thread and must complete pending GPU work and
 * readback before returning; it must not present a window. flip still runs
 * after the snapshot and dump. A NULL present_sync disables synchronization;
 * a NULL backend removes both registrations. A later nv2a_backend_register
 * call replaces both registrations. */
void nv2a_backend_register_with_present_sync(const Nv2aBackend *backend,
                                             void (*present_sync)(void));

/* The surface a flip presents (FLIP_STALL): the biggest colour surface
 * drawn since the previous flip, as the CPU path presents it. */
typedef struct {
    uint32_t va;                /* guest address of pixel (0,0), resolved */
    uint32_t pitch;             /* bytes per row */
    uint32_t bpp;               /* 2 or 4 */
    uint32_t format;            /* SET_SURFACE_FORMAT (colour format in bits 0-3) */
    uint32_t x, y, w, h;        /* the clip rectangle drawn, real pixels */
    /* A frame the back end hands the window directly, not through guest
     * memory (present hook only, `ready` only; zero in `now`). host_px
     * non-NULL: FLIP_STALL shows these host_w x host_h pixels, rows
     * host_pitch bytes apart, top row first, in NV2A_HOST_FORMAT_*
     * host_format, through xbox_FramebufferWindowPresentHost instead of
     * reading guest memory at va. The size is the back end's own (an
     * internal resolution above the guest's, RECOMP_GPU_SCALE); the window
     * scales it to its logical 640x480. The bytes must stay valid until the
     * back end's flip() (the window copies them before present returns);
     * guest memory at va need not hold the frame then. A back end only
     * sets it while a window shows frames (sync == 0, never for a dump). */
    const void *host_px;
    uint32_t host_w, host_h, host_pitch, host_format;
} Nv2aPresentSurface;

/* Nv2aPresentSurface.host_format; the same values as the window's
 * XBOX_FB_HOST_* (src/video/video_player.h). Alpha bytes are ignored. */
#define NV2A_HOST_FORMAT_BGRX8888 1u    /* bytes B, G, R, X: 0xXXRRGGBB words
                                         * (desktop GL GL_BGRA read-back) */
#define NV2A_HOST_FORMAT_RGBX8888 2u    /* bytes R, G, B, X (GLES GL_RGBA) */
#define NV2A_HOST_FORMAT_GPU      3u    /* host_px is an XboxFbGpuFrame (src/video/fb_gpu_frame.h),
                                         * host_pitch 0: the frame as a texture the window's GL
                                         * context shares (zero-copy present, XBA-308) */

/* Register a back end with a present hook instead of a present sync. At
 * every flip, before the window snapshot and the frame dump, the executor
 * calls present(now, sync, ready) on its thread. The back end makes the
 * pixels of one finished frame available in guest memory and describes
 * that surface in *ready, returning 1 (0: nothing to show this flip):
 *
 *   sync != 0 (a frame dump will read `now`): ready must be `now`, its
 *     pixels in guest memory on return;
 *   sync == 0: ready may be an earlier frame's surface whose read-back
 *     has completed since (an asynchronous read-back is one frame behind);
 *     its bytes must stay valid until the window has copied them, which
 *     happens before present returns control to the title.
 *   sync == 0, alternatively: ready->host_px set, the frame's pixels in
 *     host memory at the back end's own size (see Nv2aPresentSurface);
 *     guest memory then keeps whatever it held. A frame the guest CPU
 *     wrote (no GPU target at va) still goes through guest memory, so the
 *     window may switch between the two from one flip to the next.
 *
 * The back end stores surfaces in their own colour format (X1R5G5B5 for
 * colour formats 1 and 2, R5G6B5 for 3), and the window and frame dumps
 * read them so; the CPU rasteriser writes R5G6B5 for every 16-bit surface.
 *
 * Only the presented surface needs to be in guest memory: other surfaces
 * are the back end's business (it reads them back when the title samples
 * them in a form it cannot use, or when the guest CPU needs them). flip()
 * still runs after the snapshot and dump. A later register call replaces
 * this registration. */
void nv2a_backend_register_with_present(const Nv2aBackend *backend,
                                        int (*present)(const Nv2aPresentSurface *now,
                                                       int sync,
                                                       Nv2aPresentSurface *ready));

/* RECOMP_FB_DUMP's pictures normally show the surface; while something
 * else is on screen -- the video overlay's picture, which the GPU never
 * draws -- `dump` writes that to `path` instead and returns 1 (0: nothing
 * of its own, the surface is written as before). Called from the executor's
 * thread. The POSIX overlay scan-out registers it (src/video/pvideo_posix.c);
 * NULL removes it. */
void nv2a_pb_exec_set_screen_dump(int (*dump)(const char *path));

/* Decode a whole texture to 0xAARRGGBB, row-major, width*height entries
 * (width*height*depth, slice after slice, for a volume: Nv2aTexture.depth >
 * 1, swizzled formats only). Handles every format the executor can sample
 * (swizzled, linear, DXT). Returns 0 if the format is not supported. */
int nv2a_backend_decode_texture(const Nv2aTexture *tex, uint32_t *argb_out);

/* Mip level `level` of a swizzled or DXT texture as a texture of its own:
 * *out is *tex with the level's offset, width and height (halved per level,
 * at least 1) and one level. NV2A levels follow one another, each in the
 * base level's layout (its own Morton order or blocks). Returns 0 past the
 * texture's levels or for a linear texture's levels past 0. */
int nv2a_backend_texture_level(const Nv2aTexture *tex, uint32_t level, Nv2aTexture *out);

/* Bytes of guest memory a texture occupies: every mip level it declares
 * (rounded to the NV2A's 128-byte face alignment) times six for a cube map,
 * or pitch * height for a linear one. What a back end hashes to notice that
 * a texture changed. 0 if the format is unknown. */
uint32_t nv2a_backend_texture_bytes(const Nv2aTexture *tex);

/* Where BACK_END_WRITE_SEMAPHORE_RELEASE (0x1D70) values land: the guest VA
 * of the semaphore the title reads GPU progress from (for XDK D3D, the
 * pointer at device+0x30). The executor writes each release there once it
 * has executed everything before it -- which is
 * what lets D3D's fence waits and ring-space checks see real progress.
 *
 * ponytail: the title supplies the address instead of the executor resolving
 * the semaphore context DMA object through RAMIN. */
void nv2a_pb_set_semaphore_target(uint32_t guest_va);

/* D3D's software methods: NV097_NO_OPERATION (0x0100) with a non-zero value,
 * which the GPU traps to D3D's interrupt handler on the console. The low five
 * bits are the method, the rest its payload. hook(value) is called for every
 * one except the swap (1), on the executor thread, before the walk reads the
 * next pushbuffer word -- the GPU waits for the handler on the console, and a
 * handler may rewrite the pushbuffer ahead of it. Return non-zero when it was
 * serviced. NULL removes the hook. The guest's main thread runs meanwhile:
 * guest words it also writes need atomic updates.
 *
 * ponytail: a title hook re-implements what its D3D's handler does, instead
 * of the executor delivering the interrupt to it (see pushbuffer-executor.md,
 * "D3D Software Methods"). */
void nv2a_pb_set_software_method_hook(int (*hook)(uint32_t param));

/* The hook's refusals: describe(buf, size) writes what the hook's guards
 * refused so far, as a suffix of the report line "[GPU] software methods:"
 * ("; fixup lists refused 3; slots refused 1", "" when nothing), and returns
 * its length. NULL removes it. */
void nv2a_pb_set_software_method_refusals(int (*describe)(char *buf, int size));

#endif /* NV2A_BACKEND_H */
