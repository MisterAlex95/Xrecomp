/* gx_shader.c -- TEV state to GLSL ES 3.00 (XBA-262).
 *
 * One fragment shader text serves two ways, so that the ubershader and a specialised shader of the same state draw
 * the same pixels to the bit:
 * - the TEV arithmetic lives in GLSL functions (tev_stage, the indirect unit, alpha test, fog, EFB quantisation) that
 *   take the packed state words as int arguments;
 * - the ubershader's main() reads those words from its uniform block (u_st, u_si, u_misc) and loops over the stages:
 *   it draws any TEV state with no compile;
 * - a specialised shader's main() calls the same functions with the words as literals: the compiler inlines them and
 *   folds every branch away, which gives straight-line code for that state.
 * The renderer draws with the ubershader while the specialised program compiles on a worker thread (gx_gl.c).
 *
 * TEV arithmetic (facts, the hardware's): inputs a, b, c are 8 bits (c 255 counts as 256), d signed 11 bits;
 * result = (d + bias) +/- lerp(a, b, c), scaled by 1 / 2 / 4 / 0.5 (the scale applied inside the lerp, before its
 * rounding, which the colour combiner does when not halving and the alpha combiner only when halving), clamped to
 * 0..255 or -1024..1023; bias 3 is the compare mode (R8, GR16, BGR24, RGB8 / A8; greater or equal). The last stage's result is the output whatever its
 * destination. Konst selects, swap tables, colour channels, ALPHA_BUMP; indirect stages (texture components s = A,
 * t = B, u = G), indirect commands (format, bias, bump alpha, static and dynamic matrices, wrap, add previous). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gx_internal.h"
#include "gx_shader.h"

void gx_shader_key_make(gx_state *gx, gx_shader_key *k)
{
    memset(k, 0, sizeof *k);
    const uint32_t *bp = gx->bp;
    k->nst = BITS(bp[BP_GENMODE], 10, 4) + 1;
    k->nind = BITS(bp[BP_GENMODE], 16, 3);
    if (k->nind > 4)
        k->nind = 4;
    k->iref = k->nind ? bp[BP_RAS_IREF] & ((1u << (6 * k->nind)) - 1) : 0;
    k->alpha_cmp = bp[BP_ALPHA_COMPARE] & 0xFF0000u;
    k->fog = BITS(bp[BP_FOG_C], 21, 3) | BITS(bp[BP_FOG_C], 20, 1) << 3; /* fsel | proj << 3 */
    k->ztex = bp[BP_ZTEX1] & 0xF;
    if (BITS(k->ztex, 2, 2) == 0)
        k->ztex = 0;
    uint32_t pe = BITS(bp[BP_PE_CONTROL], 0, 3);
    uint32_t cm0 = bp[BP_CMODE0], cm1 = bp[BP_CMODE1];
    bool blend = cm0 & 1, logic = (cm0 & 2) && !blend;
    unsigned lop = logic ? BITS(cm0, 12, 4) : 3;
    bool uses_src_alpha = blend && !(cm0 & 0x800) &&
                          (BITS(cm0, 8, 3) == 4 || BITS(cm0, 8, 3) == 5 || BITS(cm0, 5, 3) == 4 || BITS(cm0, 5, 3) == 5);
    bool const_alpha = (cm1 & 0x100) && pe == 1 && !uses_src_alpha;
    k->pe = pe | (const_alpha ? 8u : 0u) | lop << 4;
    for (unsigned s = 0; s < k->nst; s++) {
        k->st_c[s] = bp[BP_TEV_COLOR_ENV + 2 * s] & 0xFFFFFFu;
        k->st_a[s] = bp[BP_TEV_COLOR_ENV + 2 * s + 1] & 0xFFFFFFu;
        uint32_t tref = bp[BP_RAS_TREF + s / 2], ksel = bp[BP_TEV_KSEL + s / 2];
        k->st_t[s] = (s & 1) ? BITS(tref, 12, 10) : BITS(tref, 0, 10);
        k->st_k[s] = (s & 1) ? BITS(ksel, 14, 10) : BITS(ksel, 4, 10);
        k->st_i[s] = bp[BP_IND_CMD + s] & 0x1FFFFFu;
    }
}

uint64_t gx_shader_key_hash(const gx_shader_key *k)
{
    return gx_hash((const uint8_t *)k, sizeof *k, 0x6A09E667F3BCC909ull);
}

const char *gx_shader_vs_source(void)
{
    return "#version 300 es\n"
           "layout(location = 0) in vec4 a_pos;\n"
           "layout(location = 1) in vec4 a_c0;\n"
           "layout(location = 2) in vec4 a_c1;\n"
           "layout(location = 3) in vec3 a_t0;\n"
           "layout(location = 4) in vec3 a_t1;\n"
           "layout(location = 5) in vec3 a_t2;\n"
           "layout(location = 6) in vec3 a_t3;\n"
           "layout(location = 7) in vec3 a_t4;\n"
           "layout(location = 8) in vec3 a_t5;\n"
           "layout(location = 9) in vec3 a_t6;\n"
           "layout(location = 10) in vec3 a_t7;\n"
           "out vec4 v_c0;\n"
           "out vec4 v_c1;\n"
           "out vec3 v_t[8];\n"
           "void main() {\n"
           "  gl_Position = a_pos;\n"
           "  v_c0 = a_c0;\n"
           "  v_c1 = a_c1;\n"
           "  v_t[0] = a_t0; v_t[1] = a_t1; v_t[2] = a_t2; v_t[3] = a_t3;\n"
           "  v_t[4] = a_t4; v_t[5] = a_t5; v_t[6] = a_t6; v_t[7] = a_t7;\n"
           "}\n";
}

/* The part every fragment shader shares. */
static const char fs_common[] =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "precision highp sampler2D;\n"
    "in vec4 v_c0;\n"
    "in vec4 v_c1;\n"
    "in vec3 v_t[8];\n"
    "out vec4 o_color;\n"
    "uniform sampler2D u_tex[8];\n"
    "layout(std140) uniform GXBlock {\n"
    "  ivec4 u_creg[4];\n"     /* PREV, C0, C1, C2 at the start of the TEV */
    "  ivec4 u_kreg[4];\n"
    "  ivec4 u_swap[4];\n"     /* swap tables: source component of r, g, b, a */
    "  vec4 u_tcscale[8];\n"   /* per texcoord: SU size (s, t) */
    "  vec4 u_texdim[8];\n"    /* per map: width, height, LOD bias */
    "  vec4 u_indm[6];\n"      /* indirect matrix i, row r: entries scaled, then the scale alone */
    "  vec4 u_indscale[4];\n"  /* per indirect stage: 2^-ss, 2^-ts */
    "  ivec4 u_alpharef;\n"    /* ref0, ref1, constant alpha */
    "  vec4 u_fog;\n"          /* a, b mantissa, 2^-b shift, c */
    "  ivec4 u_fogcol;\n"
    "  ivec4 u_zbias;\n"
    "  ivec4 u_st[16];\n"      /* ubershader: per stage colour env, alpha env, tref, ksel */
    "  ivec4 u_si[16];\n"      /* ubershader: per stage indirect command */
    "  ivec4 u_misc;\n"        /* ubershader: stages, indirect stages, iref, alpha compare */
    "  ivec4 u_misc2;\n"       /* ubershader: fog, ztex, pe */
    "};\n"
    "const int KFRAC[8] = int[8](255, 223, 191, 159, 128, 96, 64, 32);\n"
    "ivec4 to8(vec4 c) { return ivec4(round(clamp(c, 0.0, 1.0) * 255.0)); }\n"
    "ivec4 sample_map(int map, vec2 texel) {\n"
    "  vec2 uv = texel / u_texdim[map].xy;\n"
    "  float b = u_texdim[map].z;\n"
    "  vec4 c;\n"
    "  switch (map) {\n"
    "  case 0: c = texture(u_tex[0], uv, b); break;\n"
    "  case 1: c = texture(u_tex[1], uv, b); break;\n"
    "  case 2: c = texture(u_tex[2], uv, b); break;\n"
    "  case 3: c = texture(u_tex[3], uv, b); break;\n"
    "  case 4: c = texture(u_tex[4], uv, b); break;\n"
    "  case 5: c = texture(u_tex[5], uv, b); break;\n"
    "  case 6: c = texture(u_tex[6], uv, b); break;\n"
    "  default: c = texture(u_tex[7], uv, b); break;\n"
    "  }\n"
    "  return to8(c);\n"
    "}\n"
    "vec2 coord_st(vec3 t) { return t.z != 0.0 ? t.xy / t.z : t.xy; }\n"
    "ivec4 swapc(ivec4 c, int t) { ivec4 s = u_swap[t]; return ivec4(c[s.x], c[s.y], c[s.z], c[s.w]); }\n"
    "ivec3 konst_c(int sel) {\n"
    "  if (sel < 8) return ivec3(KFRAC[sel]);\n"
    "  if (sel < 12) return ivec3(0);\n"
    "  if (sel < 16) return u_kreg[sel - 12].rgb;\n"
    "  return ivec3(u_kreg[sel & 3][(sel - 16) >> 2]);\n"
    "}\n"
    "int konst_a(int sel) {\n"
    "  if (sel < 8) return KFRAC[sel];\n"
    "  if (sel < 16) return 0;\n"
    "  return u_kreg[sel & 3][(sel - 16) >> 2];\n"
    "}\n"
    "ivec3 cin(int s, ivec4 R[4], ivec4 tex, ivec4 ras, ivec3 kc) {\n"
    "  switch (s) {\n"
    "  case 0: return R[0].rgb; case 1: return ivec3(R[0].a);\n"
    "  case 2: return R[1].rgb; case 3: return ivec3(R[1].a);\n"
    "  case 4: return R[2].rgb; case 5: return ivec3(R[2].a);\n"
    "  case 6: return R[3].rgb; case 7: return ivec3(R[3].a);\n"
    "  case 8: return tex.rgb; case 9: return ivec3(tex.a);\n"
    "  case 10: return ras.rgb; case 11: return ivec3(ras.a);\n"
    "  case 12: return ivec3(255); case 13: return ivec3(128);\n"
    "  case 14: return kc;\n"
    "  default: return ivec3(0);\n"
    "  }\n"
    "}\n"
    "int ain(int s, ivec4 R[4], ivec4 tex, ivec4 ras, int ka) {\n"
    "  switch (s) {\n"
    "  case 0: return R[0].a; case 1: return R[1].a; case 2: return R[2].a; case 3: return R[3].a;\n"
    "  case 4: return tex.a; case 5: return ras.a; case 6: return ka;\n"
    "  default: return 0;\n"
    "  }\n"
    "}\n"
    /* lerp, bias, scale, rounding as the hardware does them (signed 11-bit d) */
    "ivec3 tev_lerp3(ivec3 a, ivec3 b, ivec3 c, ivec3 d, int bias, int op, int shift, bool alpha) {\n"
    "  c += c >> 7;\n"
    "  if (bias == 1) d += 128; else if (bias == 2) d -= 128;\n"
    "  ivec3 l = (a << 8) + (b - a) * c;\n"
    "  if (shift != 3) { l = l << shift; d = d << shift; }\n"
    "  if ((shift == 3) == alpha) l += op == 1 ? 127 : 128;\n"
    "  l = l >> 8;\n"
    "  ivec3 r = op == 1 ? d - l : d + l;\n"
    "  if (shift == 3) r = r >> 1;\n"
    "  return r;\n"
    "}\n"
    "int cmp_val(ivec3 v, int m) {\n"
    "  if (m < 2) return v.r;\n"
    "  if (m < 4) return (v.g << 8) | v.r;\n"
    "  return (v.b << 16) | (v.g << 8) | v.r;\n"
    "}\n"
    "ivec3 tev_c(ivec3 a, ivec3 b, ivec3 c, ivec3 d, int env) {\n"
    "  int bias = (env >> 16) & 3, op = (env >> 18) & 1, shift = (env >> 20) & 3;\n"
    "  ivec3 r;\n"
    "  if (bias == 3) {\n"
    "    int m = (shift << 1) | op;\n"
    "    if (m < 6) {\n"
    "      int va = cmp_val(a, m), vb = cmp_val(b, m);\n"
    "      bool t = (m & 1) == 1 ? va == vb : va > vb;\n"
    "      r = d + (t ? c : ivec3(0));\n"
    "    } else {\n"
    "      bvec3 t = m == 7 ? equal(a, b) : greaterThan(a, b);\n"
    "      r = d + ivec3(t) * c;\n"
    "    }\n"
    "  } else {\n"
    "    r = tev_lerp3(a, b, c, d, bias, op, shift, false);\n"
    "  }\n"
    "  return ((env >> 19) & 1) == 1 ? clamp(r, 0, 255) : clamp(r, -1024, 1023);\n"
    "}\n"
    "int tev_a(int a, int b, int c, int d, int env, ivec3 ca, ivec3 cb) {\n"
    "  int bias = (env >> 16) & 3, op = (env >> 18) & 1, shift = (env >> 20) & 3;\n"
    "  int r;\n"
    "  if (bias == 3) {\n"
    "    int m = (shift << 1) | op;\n"
    "    bool t;\n"
    "    if (m < 6) {\n"
    "      int va = cmp_val(ca, m), vb = cmp_val(cb, m);\n"
    "      t = (m & 1) == 1 ? va == vb : va > vb;\n"
    "    } else {\n"
    "      t = m == 7 ? a == b : a > b;\n"
    "    }\n"
    "    r = d + (t ? c : 0);\n"
    "  } else {\n"
    "    r = tev_lerp3(ivec3(a), ivec3(b), ivec3(c), ivec3(d), bias, op, shift, true).x;\n"
    "  }\n"
    "  return ((env >> 19) & 1) == 1 ? clamp(r, 0, 255) : clamp(r, -1024, 1023);\n"
    "}\n"
    "float wrapc(float v, int w) {\n"
    "  if (w == 0) return v;\n"
    "  if (w >= 6) return 0.0;\n"
    "  float m = float(256 >> (w - 1));\n"
    "  return v - m * floor(v / m);\n"
    "}\n"
    /* the indirect stages' texture values: s = A, t = B, u = G */
    "ivec4 ind_fetch(int j, int iref, vec3 tc[8]) {\n"
    "  int map = (iref >> (6 * j)) & 7, coord = (iref >> (6 * j + 3)) & 7;\n"
    "  vec2 t = coord_st(tc[coord]) * u_tcscale[coord].xy * u_indscale[j].xy;\n"
    "  ivec4 c = sample_map(map, t);\n"
    "  return ivec4(c.a, c.b, c.g, 0);\n"
    "}\n"
    /* one TEV stage: texture coordinate (with the indirect unit), texture, rasterised colour, konst, combiners */
    "void tev_stage(int cenv, int aenv, int tref, int ksel, int icmd, vec3 tc[8], ivec4 iv[4],\n"
    "               inout ivec4 R[4], inout ivec4 last, inout vec2 tcprev, inout int balpha, inout ivec4 ltex) {\n"
    "  int map = tref & 7, coord = (tref >> 3) & 7, chan = (tref >> 7) & 7;\n"
    "  bool ten = ((tref >> 6) & 1) == 1;\n"
    "  vec2 texel = coord_st(tc[coord]) * u_tcscale[coord].xy;\n"
    "  if (icmd != 0) {\n"
    "    int bt = icmd & 3, fmt = (icmd >> 2) & 3, bias = (icmd >> 4) & 7, bs = (icmd >> 7) & 3;\n"
    "    int mid = (icmd >> 9) & 15, sw = (icmd >> 13) & 7, tw = (icmd >> 16) & 7;\n"
    "    ivec3 v = iv[bt].xyz;\n"
    "    int cmask = fmt == 0 ? 255 : fmt == 1 ? 31 : fmt == 2 ? 15 : 7;\n"
    "    int amask = fmt == 0 ? 248 : fmt == 1 ? 224 : fmt == 2 ? 240 : 248;\n"
    "    if (bs != 0) balpha = v[bs - 1] & amask;\n"
    "    ivec3 ic = v & cmask;\n"
    "    int bv = fmt == 0 ? -128 : 1;\n"
    "    if ((bias & 1) != 0) ic.x += bv;\n"
    "    if ((bias & 2) != 0) ic.y += bv;\n"
    "    if ((bias & 4) != 0) ic.z += bv;\n"
    "    vec2 off = vec2(0.0);\n"
    "    if (mid >= 1 && mid <= 3) {\n"
    "      int m = (mid - 1) * 2;\n"
    "      off = vec2(dot(u_indm[m].xyz, vec3(ic)), dot(u_indm[m + 1].xyz, vec3(ic)));\n"
    "    } else if (mid >= 5 && mid <= 7) {\n"
    "      off = texel * (float(ic.x) / 256.0) * u_indm[(mid - 5) * 2].w;\n"
    "    } else if (mid >= 9 && mid <= 11) {\n"
    "      off = texel * (float(ic.y) / 256.0) * u_indm[(mid - 9) * 2].w;\n"
    "    }\n"
    "    vec2 w = vec2(wrapc(texel.x, sw), wrapc(texel.y, tw));\n"
    "    texel = w + off + (((icmd >> 20) & 1) == 1 ? tcprev : vec2(0.0));\n"
    "    tcprev = texel;\n"
    "  }\n"
    "  ivec4 tex = ten ? swapc(sample_map(map, texel), (aenv >> 2) & 3) : ivec4(255);\n"
    "  ivec4 ras;\n"
    "  if (chan == 0) ras = to8(v_c0);\n"
    "  else if (chan == 1) ras = to8(v_c1);\n"
    "  else if (chan == 5) ras = ivec4(balpha);\n"
    "  else if (chan == 6) ras = ivec4(balpha | (balpha >> 5));\n"
    "  else ras = ivec4(0);\n"
    "  ras = swapc(ras, aenv & 3);\n"
    "  ltex = tex;\n"
    "  ivec3 kc = konst_c(ksel & 31);\n"
    "  int ka = konst_a((ksel >> 5) & 31);\n"
    "  ivec3 a = cin((cenv >> 12) & 15, R, tex, ras, kc) & 255;\n"
    "  ivec3 b = cin((cenv >> 8) & 15, R, tex, ras, kc) & 255;\n"
    "  ivec3 c = cin((cenv >> 4) & 15, R, tex, ras, kc) & 255;\n"
    "  ivec3 d = cin(cenv & 15, R, tex, ras, kc);\n"
    "  int aa = ain((aenv >> 13) & 7, R, tex, ras, ka) & 255;\n"
    "  int ab = ain((aenv >> 10) & 7, R, tex, ras, ka) & 255;\n"
    "  int ac = ain((aenv >> 7) & 7, R, tex, ras, ka) & 255;\n"
    "  int ad = ain((aenv >> 4) & 7, R, tex, ras, ka);\n"
    "  ivec3 rc = tev_c(a, b, c, d, cenv);\n"
    "  int ra = tev_a(aa, ab, ac, ad, aenv, a, b);\n"
    "  int cd = (cenv >> 22) & 3, adst = (aenv >> 22) & 3;\n"
    "  R[cd].rgb = rc;\n"
    "  R[adst].a = ra;\n"
    "  last = ivec4(rc, ra);\n"
    "}\n"
    "bool alpha_cmp1(int a, int ref, int f) {\n"
    "  switch (f) {\n"
    "  case 0: return false; case 1: return a < ref; case 2: return a == ref; case 3: return a <= ref;\n"
    "  case 4: return a > ref; case 5: return a != ref; case 6: return a >= ref; default: return true;\n"
    "  }\n"
    "}\n"
    "bool alpha_test(int a, int cmp) {\n"
    "  bool t0 = alpha_cmp1(a, u_alpharef.x, (cmp >> 16) & 7), t1 = alpha_cmp1(a, u_alpharef.y, (cmp >> 19) & 7);\n"
    "  int lg = (cmp >> 22) & 3;\n"
    "  return lg == 0 ? (t0 && t1) : lg == 1 ? (t0 || t1) : lg == 2 ? (t0 != t1) : (t0 == t1);\n"
    "}\n"
    /* the pixel's way out: alpha test, Z texture, fog, the EFB's format, logic op; returns false to discard */
    "bool finish(ivec4 o, int cmp, int fogm, int ztex, int pe, ivec4 lasttex, out ivec4 res, out float depth) {\n"
    "  o = o & 255;\n"
    "  depth = gl_FragCoord.z;\n"
    "  if (cmp != 0x3F0000 && !alpha_test(o.a, cmp)) { res = o; return false; }\n"
    "  int z = int(gl_FragCoord.z * 16777215.0 + 0.5);\n"
    "  int zop = (ztex >> 2) & 3;\n"
    "  if (zop != 0) {\n"
    "    int ty = ztex & 3;\n"
    "    int zt = ty == 0 ? lasttex.r : ty == 1 ? ((lasttex.a << 8) | lasttex.r) : ((lasttex.r << 16) | (lasttex.g << 8) | lasttex.b);\n"
    "    z = clamp((zop == 1 ? z : 0) + zt + u_zbias.x, 0, 16777215);\n"
    "    depth = float(z) / 16777215.0;\n"
    "  }\n"
    "  int fsel = fogm & 7;\n"
    "  if (fsel >= 2) {\n"
    "    float d = float(z) / 16777215.0;\n"
    "    float ze = (fogm & 8) != 0 ? u_fog.x * d : u_fog.x / (u_fog.y - d * u_fog.z);\n"
    "    float f = clamp(ze - u_fog.w, 0.0, 1.0);\n"
    "    if (fsel == 4) f = 1.0 - exp2(-8.0 * f);\n"
    "    else if (fsel == 5) f = 1.0 - exp2(-8.0 * f * f);\n"
    "    else if (fsel == 6) f = exp2(-8.0 * (1.0 - f));\n"
    "    else if (fsel == 7) f = exp2(-8.0 * (1.0 - f) * (1.0 - f));\n"
    "    int fi = int(round(f * 256.0));\n"
    "    o.rgb = (o.rgb * (256 - fi) + u_fogcol.rgb * fi) >> 8;\n"
    "  }\n"
    "  int fmt = pe & 7;\n"
    "  if (fmt == 1) { ivec4 q = o >> 2; o = (q << 2) | (q >> 4); }\n"
    "  else if (fmt == 2) { ivec3 q = o.rgb >> ivec3(3, 2, 3); o.rgb = (q << ivec3(3, 2, 3)) | (q >> ivec3(2, 4, 2)); }\n"
    "  if ((pe & 8) != 0) o.a = u_alpharef.z;\n"
    "  int lop = (pe >> 4) & 15;\n"
    "  if (lop == 0) o = ivec4(0); else if (lop == 15) o = ivec4(255); else if (lop == 12) o = 255 - o;\n"
    "  res = o;\n"
    "  return true;\n"
    "}\n";

/* The ubershader's main(): the state from the uniform block. */
static const char fs_uber_main[] =
    "void main() {\n"
    "  vec3 tc[8];\n"
    "  for (int i = 0; i < 8; i++) tc[i] = v_t[i];\n"
    "  ivec4 R[4] = ivec4[4](u_creg[0], u_creg[1], u_creg[2], u_creg[3]);\n"
    "  ivec4 iv[4] = ivec4[4](ivec4(0), ivec4(0), ivec4(0), ivec4(0));\n"
    "  int nst = u_misc.x, nind = u_misc.y, iref = u_misc.z;\n"
    "  for (int j = 0; j < 4; j++) if (j < nind) iv[j] = ind_fetch(j, iref, tc);\n"
    "  ivec4 last = ivec4(0), ltex = ivec4(0);\n"
    "  vec2 tcprev = vec2(0.0);\n"
    "  int balpha = 0;\n"
    "  for (int s = 0; s < 16; s++) {\n"
    "    if (s >= nst) break;\n"
    "    tev_stage(u_st[s].x, u_st[s].y, u_st[s].z, u_st[s].w, u_si[s].x, tc, iv, R, last, tcprev, balpha, ltex);\n"
    "  }\n"
    "  ivec4 res; float depth;\n"
    "  if (!finish(last, u_misc.w, u_misc2.x, u_misc2.y, u_misc2.z, ltex, res, depth)) discard;\n"
    "  gl_FragDepth = depth;\n"
    "  o_color = vec4(res) / 255.0;\n"
    "}\n";

static void app(char **buf, size_t *len, size_t *cap, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

#include <stdarg.h>
static void app(char **buf, size_t *len, size_t *cap, const char *fmt, ...)
{
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int n = vsnprintf(*buf + *len, *cap - *len, fmt, ap);
        va_end(ap);
        if (n < 0)
            return;
        if (*len + (size_t)n < *cap) {
            *len += (size_t)n;
            return;
        }
        size_t nc = *cap * 2 + (size_t)n;
        char *b = realloc(*buf, nc);
        if (!b)
            return;
        *buf = b;
        *cap = nc;
    }
}

char *gx_shader_fs_source(const gx_shader_key *k)
{
    size_t cap = sizeof fs_common + 8192, len = 0;
    char *b = malloc(cap);
    if (!b)
        return NULL;
    b[0] = 0;
    app(&b, &len, &cap, "%s", fs_common);
    if (!k) {
        app(&b, &len, &cap, "%s", fs_uber_main);
        return b;
    }
    app(&b, &len, &cap,
        "void main() {\n"
        "  vec3 tc[8];\n"
        "  for (int i = 0; i < 8; i++) tc[i] = v_t[i];\n"
        "  ivec4 R[4] = ivec4[4](u_creg[0], u_creg[1], u_creg[2], u_creg[3]);\n"
        "  ivec4 iv[4] = ivec4[4](ivec4(0), ivec4(0), ivec4(0), ivec4(0));\n");
    for (unsigned j = 0; j < k->nind; j++)
        app(&b, &len, &cap, "  iv[%u] = ind_fetch(%u, %u, tc);\n", j, j, k->iref);
    app(&b, &len, &cap, "  ivec4 last = ivec4(0), ltex = ivec4(0);\n  vec2 tcprev = vec2(0.0);\n  int balpha = 0;\n");
    for (unsigned s = 0; s < k->nst; s++)
        app(&b, &len, &cap, "  tev_stage(%u, %u, %u, %u, %u, tc, iv, R, last, tcprev, balpha, ltex);\n", k->st_c[s],
            k->st_a[s], k->st_t[s], k->st_k[s], k->st_i[s]);
    app(&b, &len, &cap, "  ivec4 res; float depth;\n");
    app(&b, &len, &cap, "  if (!finish(last, %u, %u, %u, %u, ltex, res, depth)) discard;\n", k->alpha_cmp, k->fog,
        k->ztex, k->pe);
    if (k->ztex)
        app(&b, &len, &cap, "  gl_FragDepth = depth;\n");
    app(&b, &len, &cap, "  o_color = vec4(res) / 255.0;\n}\n");
    return b;
}

/* ---- the per-title list of states met (layer 2 of the shader plan) ---- */

#define LIST_MAGIC "# xrecomp gx shader list v1"

int gx_shader_list_read(const char *path, gx_shader_key **keys, size_t *n)
{
    *keys = NULL;
    *n = 0;
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char line[2048];
    size_t cap = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] != 'T' || line[1] != ' ')
            continue;
        gx_shader_key k;
        uint32_t *w = (uint32_t *)&k;
        size_t nw = sizeof k / 4, got = 0;
        char *p = line + 2;
        while (got < nw) {
            char *e;
            unsigned long v = strtoul(p, &e, 16);
            if (e == p)
                break;
            w[got++] = (uint32_t)v;
            p = e;
        }
        if (got != nw || k.nst < 1 || k.nst > 16 || k.nind > 4)
            continue;
        if (*n == cap) {
            cap = cap ? cap * 2 : 64;
            gx_shader_key *nk = realloc(*keys, cap * sizeof *nk);
            if (!nk)
                break;
            *keys = nk;
        }
        (*keys)[(*n)++] = k;
    }
    fclose(f);
    return 0;
}

int gx_shader_list_write(const char *path, const gx_shader_key *keys, size_t n, const uint32_t (*vfmts)[5], size_t nv)
{
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return -1;
    fprintf(f, "%s\n# T: a TEV state (gx_shader_key words, hex); V: a vertex format (VCD lo, hi, VAT A, B, C).\n"
               "# No game data: GX state descriptors only. Prepare puts it in the title pack (GC-6).\n", LIST_MAGIC);
    for (size_t i = 0; i < n; i++) {
        const uint32_t *w = (const uint32_t *)&keys[i];
        fputs("T", f);
        for (size_t j = 0; j < sizeof keys[i] / 4; j++)
            fprintf(f, " %x", w[j]);
        fputs("\n", f);
    }
    for (size_t i = 0; i < nv; i++)
        fprintf(f, "V %x %x %x %x %x\n", vfmts[i][0], vfmts[i][1], vfmts[i][2], vfmts[i][3], vfmts[i][4]);
    if (fclose(f))
        return -1;
    return rename(tmp, path);
}
