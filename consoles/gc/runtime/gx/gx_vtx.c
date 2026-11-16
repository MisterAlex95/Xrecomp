/* gx_vtx.c -- GX's vertex loader and transform unit (XBA-262), run on the CPU.
 *
 * Vertex loader: the vertex descriptor (CP 0x50 / 0x60: per attribute none / direct / 8-bit index / 16-bit index)
 * and the attribute format of the draw's VAT (CP 0x70 / 0x80 / 0x90 + vat: elements, component format, fraction
 * bits) give each vertex's layout in the stream, in the hardware's order: position matrix index, eight texture
 * matrix indices, position, normal (or normal, binormal, tangent), colour 0, colour 1, texture coordinates 0..7.
 * Indexed attributes read from the arrays at CP 0xA0 + n (base) / 0xB0 + n (stride).
 *
 * Transform (XF): position by the 3x4 matrix the position matrix index picks, normal / binormal / tangent by the
 * normal matrix, projection (perspective or orthographic) and the viewport to EFB pixels (minus the scissor offset),
 * written as GL clip coordinates over the whole EFB. Lighting: two colour channels, colour and alpha controlled
 * apart, material / ambient from registers or the vertex, eight lights with diffuse none / sign / clamp and
 * attenuation none / specular / spot. Texgen: regular 2x4 / 3x4 matrices from position, normal, binormal, tangent
 * or a texture coordinate (AB11 / ABC1 forms), emboss (bump) offsets, colour sources, then the dual-texture post
 * matrix with optional normalisation. Results are 8-bit colours and (s, t, q) as the rasteriser interpolates them. */
#include <math.h>
#include <string.h>

#include "gx_internal.h"

enum { A_NONE = 0, A_DIRECT = 1, A_IDX8 = 2, A_IDX16 = 3 };

static const unsigned comp_size[8] = {1, 1, 2, 2, 4, 0, 0, 0};
static const unsigned color_size[8] = {2, 3, 4, 2, 3, 4, 0, 0};

typedef struct vfmt {
    uint32_t vcd_lo, vcd_hi, a, b, c;
    unsigned pos_type, pos_n, pos_fmt, pos_frac;
    unsigned nrm_type, nrm_nbt, nrm_fmt, nrm_idx3;
    unsigned col_type[2], col_fmt[2];
    unsigned tex_type[8], tex_n[8], tex_fmt[8], tex_frac[8];
} vfmt;

static void vfmt_get(gx_state *gx, unsigned vat, vfmt *f)
{
    f->vcd_lo = gx->cpr[0x50];
    f->vcd_hi = gx->cpr[0x60];
    uint32_t a = gx->cpr[0x70 + vat], b = gx->cpr[0x80 + vat], c = gx->cpr[0x90 + vat];
    f->a = a, f->b = b, f->c = c;
    f->pos_type = BITS(f->vcd_lo, 9, 2);
    f->pos_n = BITS(a, 0, 1) ? 3 : 2;
    f->pos_fmt = BITS(a, 1, 3);
    f->pos_frac = BITS(a, 4, 5);
    f->nrm_type = BITS(f->vcd_lo, 11, 2);
    f->nrm_nbt = BITS(a, 9, 1);
    f->nrm_fmt = BITS(a, 10, 3);
    f->nrm_idx3 = BITS(a, 31, 1);
    f->col_type[0] = BITS(f->vcd_lo, 13, 2);
    f->col_type[1] = BITS(f->vcd_lo, 15, 2);
    f->col_fmt[0] = BITS(a, 14, 3);
    f->col_fmt[1] = BITS(a, 18, 3);
    for (unsigned t = 0; t < 8; t++)
        f->tex_type[t] = BITS(f->vcd_hi, 2 * t, 2);
    f->tex_n[0] = BITS(a, 21, 1) ? 2 : 1, f->tex_fmt[0] = BITS(a, 22, 3), f->tex_frac[0] = BITS(a, 25, 5);
    f->tex_n[1] = BITS(b, 0, 1) ? 2 : 1, f->tex_fmt[1] = BITS(b, 1, 3), f->tex_frac[1] = BITS(b, 4, 5);
    f->tex_n[2] = BITS(b, 9, 1) ? 2 : 1, f->tex_fmt[2] = BITS(b, 10, 3), f->tex_frac[2] = BITS(b, 13, 5);
    f->tex_n[3] = BITS(b, 18, 1) ? 2 : 1, f->tex_fmt[3] = BITS(b, 19, 3), f->tex_frac[3] = BITS(b, 22, 5);
    f->tex_n[4] = BITS(b, 27, 1) ? 2 : 1, f->tex_fmt[4] = BITS(b, 28, 3), f->tex_frac[4] = BITS(c, 0, 5);
    f->tex_n[5] = BITS(c, 5, 1) ? 2 : 1, f->tex_fmt[5] = BITS(c, 6, 3), f->tex_frac[5] = BITS(c, 9, 5);
    f->tex_n[6] = BITS(c, 14, 1) ? 2 : 1, f->tex_fmt[6] = BITS(c, 15, 3), f->tex_frac[6] = BITS(c, 18, 5);
    f->tex_n[7] = BITS(c, 23, 1) ? 2 : 1, f->tex_fmt[7] = BITS(c, 24, 3), f->tex_frac[7] = BITS(c, 27, 5);
}

static unsigned idx_size(unsigned type)
{
    return type == A_IDX8 ? 1 : type == A_IDX16 ? 2 : 0;
}

unsigned gx_vertex_size(gx_state *gx, unsigned vat)
{
    vfmt f;
    vfmt_get(gx, vat, &f);
    unsigned s = 0;
    s += BITS(f.vcd_lo, 0, 1);
    for (unsigned t = 0; t < 8; t++)
        s += BITS(f.vcd_lo, 1 + t, 1);
    if (f.pos_type == A_DIRECT)
        s += f.pos_n * comp_size[f.pos_fmt];
    else
        s += idx_size(f.pos_type);
    if (f.nrm_type == A_DIRECT)
        s += (f.nrm_nbt ? 9 : 3) * comp_size[f.nrm_fmt];
    else if (f.nrm_type)
        s += idx_size(f.nrm_type) * (f.nrm_nbt && f.nrm_idx3 ? 3 : 1);
    for (unsigned k = 0; k < 2; k++)
        s += f.col_type[k] == A_DIRECT ? color_size[f.col_fmt[k]] : idx_size(f.col_type[k]);
    for (unsigned t = 0; t < 8; t++)
        s += f.tex_type[t] == A_DIRECT ? f.tex_n[t] * comp_size[f.tex_fmt[t]] : idx_size(f.tex_type[t]);
    return s;
}

static float read_comp(const uint8_t *p, unsigned fmt, float scale)
{
    switch (fmt) {
    case 0: return p[0] * scale;
    case 1: return (int8_t)p[0] * scale;
    case 2: return gx_be16(p) * scale;
    case 3: return (int16_t)gx_be16(p) * scale;
    default: return gx_f32(gx_be32(p));
    }
}

static void read_color(const uint8_t *p, unsigned fmt, uint8_t out[4])
{
    switch (fmt) {
    case 0: { /* RGB565 */
        unsigned v = gx_be16(p);
        unsigned r = v >> 11, g = (v >> 5) & 63, b = v & 31;
        out[0] = (uint8_t)(r << 3 | r >> 2), out[1] = (uint8_t)(g << 2 | g >> 4), out[2] = (uint8_t)(b << 3 | b >> 2);
        out[3] = 255;
        break;
    }
    case 1: case 2: /* RGB888, RGB888x */
        out[0] = p[0], out[1] = p[1], out[2] = p[2], out[3] = 255;
        break;
    case 3: { /* RGBA4444 */
        unsigned v = gx_be16(p);
        for (int k = 0; k < 4; k++) {
            unsigned n = (v >> (12 - 4 * k)) & 15;
            out[k] = (uint8_t)(n << 4 | n);
        }
        break;
    }
    case 4: { /* RGBA6666 */
        uint32_t v = (uint32_t)p[0] << 16 | p[1] << 8 | p[2];
        for (int k = 0; k < 4; k++) {
            unsigned n = (v >> (18 - 6 * k)) & 63;
            out[k] = (uint8_t)(n << 2 | n >> 4);
        }
        break;
    }
    default: /* RGBA8888 */
        out[0] = p[0], out[1] = p[1], out[2] = p[2], out[3] = p[3];
        break;
    }
}

/* Where an attribute's data is: the stream for direct ones, the array for indexed ones. */
static const uint8_t *attr_src(gx_state *gx, unsigned type, unsigned arr, const uint8_t **sp, unsigned direct_size)
{
    const uint8_t *s = *sp;
    if (type == A_DIRECT) {
        *sp += direct_size;
        return s;
    }
    uint32_t idx = type == A_IDX8 ? s[0] : gx_be16(s);
    *sp += idx_size(type);
    uint32_t pa = (gx->cpr[0xA0 + arr] & 0x3FFFFFFFu) + idx * (gx->cpr[0xB0 + arr] & 0xFF);
    static const uint8_t zero[64];
    if (pa + 64 > gx->ram_size)
        return zero;
    return gx->ram + pa;
}

/* ---- transform ---- */

static inline const float *xfm(gx_state *gx, uint32_t word)
{
    return &gx->xf.f[word & 0x7FF];
}

static void mul34(const float *m, const float v[4], float out[3])
{
    for (int r = 0; r < 3; r++)
        out[r] = m[4 * r] * v[0] + m[4 * r + 1] * v[1] + m[4 * r + 2] * v[2] + m[4 * r + 3] * v[3];
}

static void mul33n(gx_state *gx, unsigned pidx, const float v[3], float out[3])
{
    uint32_t base = 0x400 + (pidx & 31) * 3;
    for (int r = 0; r < 3; r++) {
        const float *m = xfm(gx, base + 3 * r);
        out[r] = m[0] * v[0] + m[1] * v[1] + m[2] * v[2];
    }
}

static void normalize3(float v[3])
{
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0) {
        v[0] /= l, v[1] /= l, v[2] /= l;
    }
}

static float dot3(const float *a, const float *b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void unpack_rgba(uint32_t v, float out[4])
{
    out[0] = (float)(v >> 24) / 255.0f;
    out[1] = (float)((v >> 16) & 255) / 255.0f;
    out[2] = (float)((v >> 8) & 255) / 255.0f;
    out[3] = (float)(v & 255) / 255.0f;
}

static float clamp01(float v)
{
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

/* One component group (rgb: comps 0..2, alpha: comp 3) of one channel. */
static void light_chan(gx_state *gx, uint32_t ctrl, const float vcol[4], uint32_t matreg, uint32_t ambreg,
                       const float pos[3], const float nrm[3], int c0, int c1, float out[4])
{
    float mat[4], amb[4];
    unpack_rgba(matreg, mat);
    if (ctrl & 1)
        memcpy(mat, vcol, sizeof mat);
    if (!(ctrl & 2)) {
        for (int c = c0; c < c1; c++)
            out[c] = mat[c];
        return;
    }
    unpack_rgba(ambreg, amb);
    if (ctrl & 0x40)
        memcpy(amb, vcol, sizeof amb);
    float acc[4] = {amb[0], amb[1], amb[2], amb[3]};
    unsigned mask = BITS(ctrl, 2, 4) | BITS(ctrl, 11, 4) << 4;
    unsigned dfn = BITS(ctrl, 7, 2);
    bool atten = ctrl & 0x200, spot = ctrl & 0x400;
    for (unsigned l = 0; l < 8; l++) {
        if (!(mask & (1u << l)))
            continue;
        uint32_t base = 0x600 + 16 * l;
        float lcol[4];
        unpack_rgba(gx->xf.u[base + 3], lcol);
        const float *cosatt = xfm(gx, base + 4), *distatt = xfm(gx, base + 7);
        const float *lpos = xfm(gx, base + 10), *ldirv = xfm(gx, base + 13);
        float ldir[3] = {lpos[0] - pos[0], lpos[1] - pos[1], lpos[2] - pos[2]};
        float att = 1.0f;
        if (atten && spot) {
            float d2 = dot3(ldir, ldir), d = sqrtf(d2);
            if (d > 0)
                ldir[0] /= d, ldir[1] /= d, ldir[2] /= d;
            float cs = fmaxf(0.0f, dot3(ldir, ldirv));
            float num = fmaxf(0.0f, cosatt[0] + cosatt[1] * cs + cosatt[2] * cs * cs);
            float den = distatt[0] + distatt[1] * d + distatt[2] * d2;
            att = den != 0 ? num / den : 0;
        } else if (atten) { /* specular: the direction slot holds the half angle */
            normalize3(ldir);
            float cs = dot3(nrm, ldir) >= 0 ? fmaxf(0.0f, dot3(nrm, ldirv)) : 0.0f;
            float num = fmaxf(0.0f, cosatt[0] + cosatt[1] * cs + cosatt[2] * cs * cs);
            float den = distatt[0] + distatt[1] * cs + distatt[2] * cs * cs;
            att = den != 0 ? num / den : 0;
        } else {
            normalize3(ldir);
        }
        float diff = 1.0f;
        if (dfn == 1)
            diff = dot3(ldir, nrm);
        else if (dfn == 2)
            diff = fmaxf(0.0f, dot3(ldir, nrm));
        for (int c = c0; c < c1; c++)
            acc[c] += att * diff * lcol[c];
    }
    for (int c = c0; c < c1; c++)
        out[c] = mat[c] * clamp01(acc[c]);
}

static uint8_t to8(float v)
{
    v = clamp01(v) * 255.0f + 0.5f;
    return (uint8_t)v;
}

void gx_load_vertices(gx_state *gx, unsigned vat, const uint8_t *src, unsigned n)
{
    vfmt f;
    vfmt_get(gx, vat, &f);
    const uint32_t *xr = gx->xf_reg;
    uint32_t mi_a = xr[0x18], mi_b = xr[0x19];
    float pos_scale = 1.0f / (float)(1u << f.pos_frac);
    float nrm_scale = (f.nrm_fmt == 0 || f.nrm_fmt == 1) ? 1.0f / 64.0f : 1.0f / 16384.0f;
    float tex_scale[8];
    for (unsigned t = 0; t < 8; t++)
        tex_scale[t] = 1.0f / (float)(1u << f.tex_frac[t]);
    /* viewport, projection, scissor offset: EFB pixels -> GL clip space of the whole EFB */
    float sx = gx_f32(xr[0x1A]), sy = gx_f32(xr[0x1B]), sz = gx_f32(xr[0x1C]);
    float ox = gx_f32(xr[0x1D]), oy = gx_f32(xr[0x1E]), oz = gx_f32(xr[0x1F]);
    float p[6];
    for (int k = 0; k < 6; k++)
        p[k] = gx_f32(xr[0x20 + k]);
    bool ortho = xr[0x26] & 1;
    float offx = (float)(BITS(gx->bp[BP_SCISSOR_OFFSET], 0, 10) * 2), offy = (float)(BITS(gx->bp[BP_SCISSOR_OFFSET], 10, 10) * 2);
    unsigned ntex = xr[0x3F] & 15, ncol = xr[0x09] & 3;
    if (ntex > 8)
        ntex = 8;
    bool dual = xr[0x12] & 1;

    for (unsigned v = 0; v < n; v++) {
        gx_vout *o = &gx->vout[v];
        const uint8_t *s = src;
        unsigned pidx = BITS(mi_a, 0, 6);
        unsigned tidx[8] = {BITS(mi_a, 6, 6), BITS(mi_a, 12, 6), BITS(mi_a, 18, 6), BITS(mi_a, 24, 6),
                            BITS(mi_b, 0, 6), BITS(mi_b, 6, 6), BITS(mi_b, 12, 6), BITS(mi_b, 18, 6)};
        if (f.vcd_lo & 1)
            pidx = *s++ & 63;
        for (unsigned t = 0; t < 8; t++)
            if (f.vcd_lo & (2u << t))
                tidx[t] = *s++ & 63;
        float pos[4] = {0, 0, 0, 1};
        if (f.pos_type) {
            unsigned cs = comp_size[f.pos_fmt];
            const uint8_t *d = attr_src(gx, f.pos_type, 0, &s, f.pos_n * cs);
            for (unsigned k = 0; k < f.pos_n; k++)
                pos[k] = read_comp(d + k * cs, f.pos_fmt, pos_scale);
        }
        float nbt[3][3] = {{0, 0, 1}, {0, 1, 0}, {1, 0, 0}};
        bool has_nrm = f.nrm_type != A_NONE;
        if (has_nrm) {
            unsigned cs = comp_size[f.nrm_fmt], nv = f.nrm_nbt ? 3 : 1;
            if (f.nrm_type != A_DIRECT && f.nrm_nbt && f.nrm_idx3) {
                for (unsigned k = 0; k < 3; k++) {
                    const uint8_t *d = attr_src(gx, f.nrm_type, 1, &s, 0);
                    for (unsigned c = 0; c < 3; c++)
                        nbt[k][c] = read_comp(d + c * cs, f.nrm_fmt, nrm_scale);
                }
            } else {
                const uint8_t *d = attr_src(gx, f.nrm_type, 1, &s, 3 * nv * cs);
                for (unsigned k = 0; k < nv; k++)
                    for (unsigned c = 0; c < 3; c++)
                        nbt[k][c] = read_comp(d + (3 * k + c) * cs, f.nrm_fmt, nrm_scale);
            }
        }
        uint8_t vc[2][4] = {{255, 255, 255, 255}, {255, 255, 255, 255}};
        for (unsigned k = 0; k < 2; k++) {
            if (!f.col_type[k])
                continue;
            const uint8_t *d = attr_src(gx, f.col_type[k], 2 + k, &s, color_size[f.col_fmt[k]]);
            read_color(d, f.col_fmt[k], vc[k]);
        }
        float vt[8][2] = {{0}};
        for (unsigned t = 0; t < 8; t++) {
            if (!f.tex_type[t])
                continue;
            unsigned cs = comp_size[f.tex_fmt[t]];
            const uint8_t *d = attr_src(gx, f.tex_type[t], 4 + t, &s, f.tex_n[t] * cs);
            for (unsigned k = 0; k < f.tex_n[t]; k++)
                vt[t][k] = read_comp(d + k * cs, f.tex_fmt[t], tex_scale[t]);
        }
        src += (size_t)(s - src);

        /* position and normals to view space */
        float vp[3];
        mul34(xfm(gx, pidx * 4), pos, vp);
        float vn[3][3];
        for (int k = 0; k < 3; k++) {
            mul33n(gx, pidx, nbt[k], vn[k]);
            if (k == 0)
                normalize3(vn[k]);
        }
        /* projection, viewport */
        float cx, cy, cz, cw;
        if (ortho) {
            cx = p[0] * vp[0] + p[1];
            cy = p[2] * vp[1] + p[3];
            cz = p[4] * vp[2] + p[5];
            cw = 1.0f;
        } else {
            cx = p[0] * vp[0] + p[1] * vp[2];
            cy = p[2] * vp[1] + p[3] * vp[2];
            cz = p[4] * vp[2] + p[5];
            cw = -vp[2];
        }
        o->pos[0] = 2.0f * (sx * cx + (ox - offx) * cw) / (float)GX_EFB_W - cw;
        o->pos[1] = cw - 2.0f * (sy * cy + (oy - offy) * cw) / (float)GX_EFB_H;
        o->pos[2] = 2.0f * (sz * cz + oz * cw) / 16777215.0f - cw;
        o->pos[3] = cw;

        /* lighting */
        float lit[2][4] = {{1, 1, 1, 1}, {1, 1, 1, 1}};
        for (unsigned c = 0; c < 2; c++) {
            float vcol[4] = {vc[c][0] / 255.0f, vc[c][1] / 255.0f, vc[c][2] / 255.0f, vc[c][3] / 255.0f};
            if (c >= ncol && ncol) {
                memcpy(lit[c], lit[0], sizeof lit[c]);
                continue;
            }
            light_chan(gx, xr[0x0E + c], vcol, xr[0x0C + c], xr[0x0A + c], vp, vn[0], 0, 3, lit[c]);
            light_chan(gx, xr[0x10 + c], vcol, xr[0x0C + c], xr[0x0A + c], vp, vn[0], 3, 4, lit[c]);
        }
        for (int k = 0; k < 4; k++) {
            o->c0[k] = to8(lit[0][k]);
            o->c1[k] = to8(lit[1][k]);
        }

        /* texgen */
        for (unsigned t = 0; t < ntex; t++) {
            uint32_t info = xr[0x40 + t];
            unsigned type = BITS(info, 4, 3), row = BITS(info, 7, 5);
            bool proj = info & 2, abc1 = info & 4;
            float stq[3] = {0, 0, 1};
            if (type == 0) {
                float in[4] = {0, 0, 1, 1};
                if (row == 0)
                    in[0] = pos[0], in[1] = pos[1], in[2] = pos[2];
                else if (row == 1)
                    in[0] = nbt[0][0], in[1] = nbt[0][1], in[2] = nbt[0][2];
                else if (row == 2)
                    in[0] = lit[0][0], in[1] = lit[0][1], in[2] = lit[0][2];
                else if (row == 3 || row == 4)
                    in[0] = nbt[row - 2][0], in[1] = nbt[row - 2][1], in[2] = nbt[row - 2][2];
                else if (row >= 5 && row <= 12)
                    in[0] = vt[row - 5][0], in[1] = vt[row - 5][1];
                if (!abc1)
                    in[2] = 1.0f;
                const float *m = xfm(gx, tidx[t] * 4);
                stq[0] = m[0] * in[0] + m[1] * in[1] + m[2] * in[2] + m[3] * in[3];
                stq[1] = m[4] * in[0] + m[5] * in[1] + m[6] * in[2] + m[7] * in[3];
                stq[2] = proj ? m[8] * in[0] + m[9] * in[1] + m[10] * in[2] + m[11] * in[3] : 1.0f;
                if (dual) {
                    uint32_t post = xr[0x50 + t];
                    if (post & 0x100) {
                        float l = sqrtf(stq[0] * stq[0] + stq[1] * stq[1] + stq[2] * stq[2]);
                        if (l > 0)
                            stq[0] /= l, stq[1] /= l, stq[2] /= l;
                    }
                    float pin[4] = {stq[0], stq[1], stq[2], 1.0f};
                    mul34(xfm(gx, 0x500 + BITS(post, 0, 6) * 4), pin, stq);
                }
            } else if (type == 1) { /* emboss: an earlier coordinate shifted along the light */
                unsigned srcc = BITS(info, 12, 3), light = BITS(info, 15, 3);
                const float *lpos = xfm(gx, 0x600 + 16 * light + 10);
                float ld[3] = {lpos[0] - vp[0], lpos[1] - vp[1], lpos[2] - vp[2]};
                normalize3(ld);
                float q = o->tex[srcc][2] != 0 ? o->tex[srcc][2] : 1.0f;
                stq[0] = o->tex[srcc][0] / q + dot3(ld, vn[2]);
                stq[1] = o->tex[srcc][1] / q + dot3(ld, vn[1]);
                stq[2] = 1.0f;
            } else { /* colour channel 0 / 1 */
                const float *lc = lit[type == 3 ? 1 : 0];
                stq[0] = lc[0], stq[1] = lc[1], stq[2] = 1.0f;
            }
            o->tex[t][0] = stq[0], o->tex[t][1] = stq[1], o->tex[t][2] = stq[2];
        }
        for (unsigned t = ntex; t < 8; t++)
            o->tex[t][0] = o->tex[t][1] = 0, o->tex[t][2] = 1;
    }
}
