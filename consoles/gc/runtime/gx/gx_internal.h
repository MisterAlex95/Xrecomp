/* gx_internal.h -- GX state shared by the core (gx_fifo.c, gx_vtx.c, gx_tex.c) and the renderer (gx_gl.c,
 * gx_shader.c). Register layouts are the hardware's (facts from libogc's gx.c / YAGCD / Dolphin's documentation;
 * no code copied). XBA-262. */
#ifndef GX_INTERNAL_H
#define GX_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "gx.h"

#define GX_TMEM_SIZE 0x100000u  /* 1 MB: TLUTs live in its upper half */
#define GX_EFB_W 640
#define GX_EFB_H 528
#define GX_MAX_VERTS 65536

/* Bit field of a register. */
#define BITS(v, lo, n) (((v) >> (lo)) & ((1u << (n)) - 1u))

/* BP register addresses used by name. */
enum {
    BP_GENMODE = 0x00, BP_IND_MTXA = 0x06, BP_IND_IMASK = 0x0F, BP_IND_CMD = 0x10, BP_SCISSOR_TL = 0x20,
    BP_SCISSOR_BR = 0x21, BP_LPSIZE = 0x22, BP_RAS_SS0 = 0x25, BP_RAS_SS1 = 0x26, BP_RAS_IREF = 0x27,
    BP_RAS_TREF = 0x28, BP_SU_SSIZE = 0x30, BP_ZMODE = 0x40, BP_CMODE0 = 0x41, BP_CMODE1 = 0x42,
    BP_PE_CONTROL = 0x43, BP_FIELD_MASK = 0x44, BP_PE_DONE = 0x45, BP_PE_TOKEN = 0x47, BP_PE_TOKEN_INT = 0x48,
    BP_COPY_SRC = 0x49, BP_COPY_SIZE = 0x4A, BP_COPY_DST = 0x4B, BP_COPY_STRIDE = 0x4D, BP_COPY_YSCALE = 0x4E,
    BP_CLEAR_AR = 0x4F, BP_CLEAR_GB = 0x50, BP_CLEAR_Z = 0x51, BP_COPY_EXEC = 0x52, BP_COPY_FILTER0 = 0x53,
    BP_COPY_FILTER1 = 0x54, BP_SCISSOR_OFFSET = 0x59, BP_LOADTLUT0 = 0x64, BP_LOADTLUT1 = 0x65,
    BP_TEX_INVALIDATE = 0x66, BP_TEV_COLOR_ENV = 0xC0, BP_TEV_REGS = 0xE0, BP_FOG_RANGE = 0xE8, BP_FOG_A = 0xEE,
    BP_FOG_B_MAG = 0xEF, BP_FOG_B_SHIFT = 0xF0, BP_FOG_C = 0xF1, BP_FOG_COLOR = 0xF2, BP_ALPHA_COMPARE = 0xF3,
    BP_ZTEX0 = 0xF4, BP_ZTEX1 = 0xF5, BP_TEV_KSEL = 0xF6, BP_MASK = 0xFE,
};

/* Texture map n's registers: mode0, mode1, image0 (size, format), image1/2 (TMEM), image3 (address), TLUT. */
static inline unsigned bp_tex_reg(unsigned base_lo, unsigned n)
{
    return n < 4 ? base_lo + n : base_lo + 0x20 + (n - 4);
}
enum { BP_TX_MODE0 = 0x80, BP_TX_MODE1 = 0x84, BP_TX_IMAGE0 = 0x88, BP_TX_IMAGE1 = 0x8C, BP_TX_IMAGE3 = 0x94,
       BP_TX_TLUT = 0x98 };

/* Texture formats (GX_TF_*). */
enum { TF_I4 = 0, TF_I8 = 1, TF_IA4 = 2, TF_IA8 = 3, TF_RGB565 = 4, TF_RGB5A3 = 5, TF_RGBA8 = 6, TF_C4 = 8, TF_C8 = 9,
       TF_C14X2 = 10, TF_CMPR = 14 };

/* A transformed vertex as the renderer draws it: clip-space position already mapped to the EFB (GL clip space of
 * the whole EFB), the two colour channels after lighting, texture coordinates (s, t, q) after texgen and post
 * matrix, scaled to texels by the renderer's shader. */
typedef struct gx_vout {
    float pos[4];
    uint8_t c0[4], c1[4];
    float tex[8][3];
} gx_vout;

/* The pixel state a shader is built from (gx_shader.c): only what changes the generated code, per TEV stage in the
 * packed layout the ubershader reads from its uniform block (the same words). Unused stages are zero. */
typedef struct gx_shader_key {
    uint32_t nst, nind;        /* TEV stages 1..16, indirect stages 0..4 */
    uint32_t iref;             /* RAS1_IREF: indirect stage maps / coordinates */
    uint32_t alpha_cmp;        /* alpha test comparisons and logic (refs are uniforms) */
    uint32_t fog;              /* proj << 3 | fsel */
    uint32_t ztex;             /* ZTEX_ENV1: op << 2 | type */
    uint32_t pe;               /* EFB format | const alpha out << 3 | logic op << 4 */
    uint32_t pad;
    uint32_t st_c[16];         /* colour combiner */
    uint32_t st_a[16];         /* alpha combiner (with the swap table selects) */
    uint32_t st_t[16];         /* texmap | texcoord << 3 | tex enable << 6 | colour channel << 7 */
    uint32_t st_k[16];         /* konst colour select | konst alpha select << 5 */
    uint32_t st_i[16];         /* indirect command */
} gx_shader_key;

typedef struct gx_gl gx_gl;

struct gx_state {
    uint8_t *ram;
    uint32_t ram_size;
    gx_irq_fn irq;
    void *irq_user;
    int verbose;
    int trace;                 /* GC_GX_TRACE: texture loads, copies, peeks */

    /* processor interface FIFO (the CPU's side) */
    uint32_t pi_base, pi_end, pi_wp;
    /* command processor */
    uint16_t cp_sr, cp_cr, cp_clear;
    uint16_t cp_raw[0x40];     /* the 16-bit halves as written */
    uint32_t cp_base, cp_end, cp_hiwm, cp_lowm, cp_dist, cp_wp, cp_rp, cp_bp;
    uint16_t cp_token;
    /* pixel engine */
    uint16_t pe_reg[0x20];
    uint16_t pe_ctrl, pe_token;

    /* command staging: bytes read from the FIFO not yet parsed */
    uint8_t *cmd;
    size_t cmd_len, cmd_cap;
    int dl_depth;
    unsigned last_vat;         /* VAT of the draw being decoded (the renderer records vertex formats) */

    /* registers */
    uint32_t bp[256];
    uint32_t bp_mask;
    uint32_t cpr[256];         /* CP registers by address (VCD, VAT, array bases / strides, matrix indices) */
    union { uint32_t u[0x800]; float f[0x800]; } xf; /* matrices (0x000), normal (0x400), post (0x500), lights (0x600) */
    uint32_t xf_reg[0x100];    /* 0x1000 + n */
    uint8_t *tmem;
    int16_t creg[4][4];        /* TEV colour registers PREV, C0..C2 (r, g, b, a; signed 11 bits) */
    uint8_t kreg[4][4];        /* konst colours K0..K3 */

    /* transformed vertices of the current draw */
    gx_vout *vout;
    uint32_t tex_epoch;        /* bumped by TX_INVALIDATE and each field: texture contents are hashed again */
    uint64_t field_count;

    gx_gl *gl;
    gx_stats stats;
    bool warned[64];
};

/* ---- gx_fifo.c ---- */
void gx_log(gx_state *gx, const char *fmt, ...);
static inline float gx_f32(uint32_t v)
{
    union { uint32_t u; float f; } c = {v};
    return c.f;
}
static inline uint32_t gx_be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static inline uint16_t gx_be16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

/* ---- gx_vtx.c ---- */
/* Size in bytes of one vertex of VAT vat with the current VCD. */
unsigned gx_vertex_size(gx_state *gx, unsigned vat);
/* Decodes and transforms n vertices from src into gx->vout. */
void gx_load_vertices(gx_state *gx, unsigned vat, const uint8_t *src, unsigned n);

/* ---- gx_tex.c ---- */
typedef struct gx_texinfo {
    uint32_t addr;             /* physical */
    unsigned w, h, fmt, levels;
    uint32_t tlut_addr;        /* TMEM byte offset of the palette */
    unsigned tlut_fmt;
    uint32_t size;             /* bytes of every level */
} gx_texinfo;
void gx_tex_info(gx_state *gx, unsigned map, gx_texinfo *ti);
uint32_t gx_tex_level_size(unsigned fmt, unsigned w, unsigned h);
/* Decodes one level (w x h) at src into RGBA8 (rows top to bottom). */
void gx_tex_decode(gx_state *gx, const gx_texinfo *ti, const uint8_t *src, unsigned w, unsigned h, uint8_t *rgba);
uint64_t gx_hash(const uint8_t *p, size_t n, uint64_t seed);
/* Encodes an RGBA8 picture (rows top to bottom) into texture format fmt at dst (EFB copy to RAM). */
void gx_tex_encode(unsigned fmt, const uint8_t *rgba, unsigned w, unsigned h, uint8_t *dst, uint32_t stride);

/* ---- gx_gl.c (no-ops without a renderer) ---- */
/* State about to change: draws queued so far must be drawn with the old state. */
void gx_gl_flush(gx_state *gx);
/* Queue the gx->vout vertices of one primitive (GX opcode prim >> 3 & 7). */
void gx_gl_draw(gx_state *gx, unsigned prim, unsigned n);
void gx_gl_copy(gx_state *gx, uint32_t exec);
void gx_gl_tex_invalidate(gx_state *gx);
void gx_gl_frame(gx_state *gx);

/* ---- gx_shader.c ---- */
void gx_shader_key_make(gx_state *gx, gx_shader_key *k);

#endif
