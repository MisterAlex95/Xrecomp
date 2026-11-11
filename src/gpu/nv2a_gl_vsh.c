/**
 * NV2A vertex programs to GLSL. See nv2a_gl_vsh.h.
 *
 * Every rule is a transcription of nv2a_vsh_interp.c (vsh_run_decoded,
 * run_mac, run_ilu, vsh_finish, vsh_init_regs); the comments name what each
 * block mirrors.
 */
#include "nv2a_gl_vsh.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

const char *const nv2a_gl_vsh_capture_names[8] = {
    "x_pos", "x_d0", "x_d1", "x_fog", "x_t0", "x_t1", "x_t2", "x_t3"
};

void nv2a_gl_vsh_fog_uniform(uint32_t enable, uint32_t mode, float p0, float p1,
                             float out[4])
{
    out[0] = (mode == 0x800 || mode == 0x802) ? 1.0f
           : (mode == 0x801 || mode == 0x803) ? 2.0f : 0.0f;
    out[1] = p0;
    out[2] = p1;
    out[3] = !enable ? 0.0f
           : (mode == 0x802 || mode == 0x803 || mode == 0x804) ? 2.0f : 1.0f;
}

/* ---- a small string builder ------------------------------------------- */

typedef struct {
    char *buf;
    size_t cap, len;
    int overflow;
} Sb;

static void sb(Sb *b, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (b->overflow)
        return;
    va_start(ap, fmt);
    n = vsnprintf(b->buf + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= b->cap - b->len) {
        b->overflow = 1;
        return;
    }
    b->len += (size_t)n;
}

static uint32_t field(const uint32_t *ins, int dw, int lo, int width)
{
    return (ins[dw] >> lo) & ((1u << width) - 1u);
}

/* The sources an op pair reads (dec_reads in nv2a_vsh_interp.c). */
static uint32_t reads_of(uint32_t mac, uint32_t ilu)
{
    uint32_t r = 0;

    if (mac)
        r |= 1;
    if (mac == 2 || (mac >= 4 && mac <= 12))
        r |= 2;
    if (mac == 3 || mac == 4 || ilu)
        r |= 4;
    return r;
}

/* Source `which` (0 A, 1 B, 2 C) as a GLSL expression (dec_src/dec_read). */
static void src_expr(char *o, size_t n, const uint32_t *ins, int which,
                     Nv2aGlVshInfo *info)
{
    static const char sw[] = "xyzw";
    uint32_t neg, swz, mux, reg;
    char base[40];

    if (which == 0) {
        neg = field(ins, 1, 8, 1); swz = field(ins, 1, 0, 8);
        mux = field(ins, 2, 26, 2); reg = field(ins, 2, 28, 4);
    } else if (which == 1) {
        neg = field(ins, 2, 25, 1); swz = field(ins, 2, 17, 8);
        mux = field(ins, 2, 11, 2); reg = field(ins, 2, 13, 4);
    } else {
        neg = field(ins, 2, 10, 1); swz = field(ins, 2, 2, 8);
        mux = field(ins, 3, 28, 2);
        reg = (field(ins, 2, 0, 2) << 2) | field(ins, 3, 30, 2);
    }
    if (mux == 1) {
        /* R12 is oPos; R13..R15 read R0. */
        snprintf(base, sizeof base, "R%u", reg <= 12 ? reg : 0u);
    } else if (mux == 2) {
        uint32_t v = field(ins, 1, 9, 4);
        snprintf(base, sizeof base, "v%u", v);
        info->inputs |= 1u << v;
    } else {
        /* mux 0 reads c[] too. Out of range: c[0]. */
        uint32_t ci = field(ins, 1, 13, 8);
        if (field(ins, 3, 1, 1)) {
            snprintf(base, sizeof base, "c[vsh_ci(a0 + %u)]", ci);
            info->relative = 1;
        } else {
            snprintf(base, sizeof base, "c[%u]", ci < 192 ? ci : 0u);
        }
    }
    if (swz == 0x1B)
        snprintf(o, n, "%s%s", neg ? "-" : "", base);
    else
        snprintf(o, n, "%s%s.%c%c%c%c", neg ? "-" : "", base,
                 sw[(swz >> 6) & 3], sw[(swz >> 4) & 3],
                 sw[(swz >> 2) & 3], sw[swz & 3]);
}

static void mask_str(char *o, uint32_t mask)
{
    int n = 0;

    if (mask & 8) o[n++] = 'x';
    if (mask & 4) o[n++] = 'y';
    if (mask & 2) o[n++] = 'z';
    if (mask & 1) o[n++] = 'w';
    o[n] = 0;
}

/* The ILU's special cases, as run_ilu computes them. */
static const char k_helpers[] =
    "float vsh_inf() { return uintBitsToFloat(0x7F800000u); }\n"
    "int vsh_ci(int i) { return (i < 0 || i >= 192) ? 0 : i; }\n"
    "float vsh_rcp(float x) { return x == 0.0 ? vsh_inf() : 1.0 / x; }\n"
    "float vsh_rcc(float x) {\n"
    "  float f = x == 0.0 ? ((floatBitsToUint(x) >> 31) != 0u ? -vsh_inf() : vsh_inf())\n"
    "                     : 1.0 / x;\n"
    "  if (abs(f) < 5.42101e-20) f = f < 0.0 ? -5.42101e-20 : 5.42101e-20;\n"
    "  if (abs(f) > 1.884467e19) f = f < 0.0 ? -1.884467e19 : 1.884467e19;\n"
    "  return f;\n"
    "}\n"
    "float vsh_rsq(float x) { float a = abs(x); return a == 0.0 ? vsh_inf() : 1.0 / sqrt(a); }\n"
    "vec4 vsh_exp(float x) { float fl = floor(x); return vec4(exp2(fl), x - fl, exp2(x), 1.0); }\n"
    /* floor(log2f(|x|)): the exponent bits, unless log2 rounded up to the
     * next integer (just below a power of two), as libm's does. */
    "vec4 vsh_log(float x) {\n"
    "  float ax = abs(x);\n"
    "  if (ax == 0.0) return vec4(-vsh_inf(), 1.0, -vsh_inf(), 1.0);\n"
    "  float l = log2(ax);\n"
    "  float e = max(float(int((floatBitsToUint(ax) >> 23) & 0xFFu) - 127), floor(l));\n"
    "  return vec4(e, ax / exp2(e), l, 1.0);\n"
    "}\n"
    "float vsh_pow(float b, float e) {\n"
    "  if (e == 0.0) return 1.0;\n"
    "  if (b == 0.0) return e > 0.0 ? 0.0 : vsh_inf();\n"
    "  return pow(b, e);\n"
    "}\n"
    "vec4 vsh_lit(vec4 s) {\n"
    "  float nl = s.x > 0.0 ? s.x : 0.0;\n"
    "  float nh = s.y > 0.0 ? s.y : 0.0;\n"
    "  float p = s.w < -127.9961 ? -127.9961 : (s.w > 127.9961 ? 127.9961 : s.w);\n"
    "  return vec4(1.0, nl, s.x > 0.0 ? vsh_pow(nh, p) : 0.0, 1.0);\n"
    "}\n"
    /* vsh_finish's colour clamp: NaN reads 0. */
    "float vsh_sat1(float c) { return c > 1.0 ? 1.0 : (c >= 0.0 ? c : 0.0); }\n"
    "vec4 vsh_sat(vec4 c) { return vec4(vsh_sat1(c.x), vsh_sat1(c.y), vsh_sat1(c.z), vsh_sat1(c.w)); }\n";

/* fog_factor() in nv2a_pb_exec.c. */
static const char k_fog[] =
    "uniform vec4 u_fog;\n"
    "float vsh_fog(float d) {\n"
    "  if (u_fog.w == 0.0) return 1.0;\n"
    "  float f;\n"
    "  if (u_fog.x == 1.0) f = u_fog.y + exp2(d * u_fog.z * 16.0) - 1.5;\n"
    "  else if (u_fog.x == 2.0) f = u_fog.y + exp2(-d * d * u_fog.z * u_fog.z * 32.0) - 1.5;\n"
    "  else f = u_fog.y + d * u_fog.z - 1.0;\n"
    "  if (u_fog.w == 2.0) f = abs(f);\n"
    "  if (isnan(f)) f = 1.0;\n"
    "  return clamp(f, 0.0, 1.0);\n"
    "}\n";

static void emit_mac(Sb *b, uint32_t op)
{
    switch (op) {
    case 0:  sb(b, "  t_m = vec4(0.0);\n"); break;          /* not run: 0 */
    case 2:  sb(b, "  t_m = A * B;\n"); break;
    case 3:  sb(b, "  t_m = A + C;\n"); break;
    case 4:  sb(b, "  t_m = A * B + C;\n"); break;
    case 5:  sb(b, "  t_m = vec4(dot(A.xyz, B.xyz));\n"); break;
    case 6:  sb(b, "  t_m = vec4(dot(A.xyz, B.xyz) + B.w);\n"); break;
    case 7:  sb(b, "  t_m = vec4(dot(A, B));\n"); break;
    case 8:  sb(b, "  t_m = vec4(1.0, A.y * B.y, A.z, B.w);\n"); break;
    case 9:  sb(b, "  t_m = mix(B, A, lessThan(A, B));\n"); break;
    case 10: sb(b, "  t_m = mix(B, A, greaterThanEqual(A, B));\n"); break;
    case 11: sb(b, "  t_m = vec4(lessThan(A, B));\n"); break;
    case 12: sb(b, "  t_m = vec4(greaterThanEqual(A, B));\n"); break;
    case 13: sb(b, "  t_m = A; a0 = int(floor(A.x + 0.001));\n"); break;
    default: sb(b, "  t_m = A;\n"); break;                /* mov, 14, 15 */
    }
}

static void emit_ilu(Sb *b, uint32_t op)
{
    switch (op) {
    case 0:  sb(b, "  t_i = vec4(0.0);\n"); break;
    case 2:  sb(b, "  t_i = vec4(vsh_rcp(C.x));\n"); break;
    case 3:  sb(b, "  t_i = vec4(vsh_rcc(C.x));\n"); break;
    case 4:  sb(b, "  t_i = vec4(vsh_rsq(C.x));\n"); break;
    case 5:  sb(b, "  t_i = vsh_exp(C.x);\n"); break;
    case 6:  sb(b, "  t_i = vsh_log(C.x);\n"); break;
    case 7:  sb(b, "  t_i = vsh_lit(C);\n"); break;
    default: sb(b, "  t_i = C;\n"); break;                /* mov */
    }
}

/* The output register o[addr] (outregs in the interpreter): only those the
 * rasteriser reads are kept; 1, 2, 6, 7, 8 (oPts, back colours...) are
 * written to nothing, as vsh_finish never reads them. */
static const char *out_name(uint32_t addr)
{
    switch (addr) {
    case 0:  return "R12";
    case 3:  return "o_d0";
    case 4:  return "o_d1";
    case 5:  return "o_fog";
    case 9:  return "o_t0";
    case 10: return "o_t1";
    case 11: return "o_t2";
    case 12: return "o_t3";
    default: return NULL;
    }
}

int nv2a_gl_vsh_attr(uint32_t type, uint32_t size, Nv2aGlVshAttr *out)
{
    if (!size || size > 4)
        return 0;
    switch (type) {
    case 0:                                  /* D3DCOLOR: B,G,R,A bytes */
        out->kind = NV2A_GL_ATTR_UBYTE; out->comps = 4; out->bytes = 4;
        out->fmt = NV2A_GL_VSH_IN_BGRA;
        return 1;
    case 1:                                  /* signed short, normalised */
        out->kind = NV2A_GL_ATTR_SHORT; out->comps = size; out->bytes = 2 * size;
        out->fmt = NV2A_GL_VSH_IN_SNORM | (size - 1) << 2;
        return 1;
    case 2:                                  /* float */
        out->kind = NV2A_GL_ATTR_FLOAT; out->comps = size; out->bytes = 4 * size;
        out->fmt = NV2A_GL_VSH_IN_AS_IS;
        return 1;
    case 4:                                  /* unsigned byte, normalised */
        out->kind = NV2A_GL_ATTR_UBYTE; out->comps = size; out->bytes = size;
        out->fmt = NV2A_GL_VSH_IN_AS_IS;
        return 1;
    case 5:                                  /* signed short, as is */
        out->kind = NV2A_GL_ATTR_SHORT; out->comps = size; out->bytes = 2 * size;
        out->fmt = NV2A_GL_VSH_IN_AS_IS;
        return 1;
    case 6:                                  /* packed 11:11:10 */
        out->kind = NV2A_GL_ATTR_INT; out->comps = 1; out->bytes = 4;
        out->fmt = NV2A_GL_VSH_IN_PACKED;
        return 1;
    default:
        return 0;
    }
}

size_t nv2a_gl_vsh_body(const uint32_t (*ins)[4], uint32_t count, uint32_t flags,
                        char *buf, size_t cap, Nv2aGlVshInfo *info_out)
{
    return nv2a_gl_vsh_body_fmt(ins, count, flags, 0, buf, cap, info_out);
}

size_t nv2a_gl_vsh_body_fmt(const uint32_t (*ins)[4], uint32_t count, uint32_t flags,
                            uint64_t formats, char *buf, size_t cap,
                            Nv2aGlVshInfo *info_out)
{
    Sb b = { buf, cap, 0, 0 };
    Sb body;
    Nv2aGlVshInfo info;
    static char prog[65536];        /* the program, written before the inputs are known */
    uint32_t n, k, final = 0;

    memset(&info, 0, sizeof info);
    if (!cap)
        return 0;
    buf[0] = 0;
    body.buf = prog; body.cap = sizeof prog; body.len = 0; body.overflow = 0;

    for (n = 0; n < count; n++) {
        const uint32_t *w = ins[n];
        uint32_t mac = field(w, 1, 21, 4), ilu = field(w, 1, 25, 3);
        uint32_t mac_mask = field(w, 3, 24, 4), ilu_mask = field(w, 3, 16, 4);
        uint32_t tdst = field(w, 3, 20, 4), it = mac ? 1u : tdst;
        uint32_t omask = field(w, 3, 12, 4), oaddr = field(w, 3, 3, 8);
        uint32_t reads = reads_of(mac, ilu);
        char s[64], m[8];

        sb(&body, "  // %u\n", n);
        /* Every source before any write (both units read first). */
        if (reads & 1) { src_expr(s, sizeof s, w, 0, &info); sb(&body, "  A = %s;\n", s); }
        if (reads & 2) { src_expr(s, sizeof s, w, 1, &info); sb(&body, "  B = %s;\n", s); }
        if (reads & 4) { src_expr(s, sizeof s, w, 2, &info); sb(&body, "  C = %s;\n", s); }
        emit_mac(&body, mac);
        emit_ilu(&body, ilu);
        /* MAC temp write (ARL writes only a0), then the ILU's: R1 when paired
         * with any MAC op. Temps above R12 are dropped. */
        if (mac && mac != 13 && mac_mask && tdst <= 12) {
            mask_str(m, mac_mask);
            sb(&body, "  R%u.%s = t_m.%s;\n", tdst, m, m);
        }
        if (ilu && ilu_mask && it <= 12) {
            mask_str(m, ilu_mask);
            sb(&body, "  R%u.%s = t_i.%s;\n", it, m, m);
        }
        /* Output write; to c[] is dropped (programs that write c[] with
         * _CXT_WRITE_EN on stay on the CPU). */
        if (omask && field(w, 3, 11, 1) && oaddr < 13 && out_name(oaddr)) {
            mask_str(m, omask);
            sb(&body, "  %s.%s = %s.%s;\n", out_name(oaddr), m,
               field(w, 3, 2, 1) ? "t_i" : "t_m", m);
        }
        if (field(w, 3, 0, 1)) {
            final = 1;
            n++;
            break;
        }
    }
    if (!final || body.overflow)
        return 0;
    info.count = n;

    sb(&b, "uniform vec4 c[192];\n");
    for (k = 0; k < 16; k++) {
        uint32_t f = (uint32_t)(formats >> (4 * k)) & 3u;
        if (!(info.inputs & (1u << k)))
            continue;
        if (f == NV2A_GL_VSH_IN_AS_IS)
            sb(&b, "layout(location = %u) in vec4 v%u;\n", k, k);
        else if (f == NV2A_GL_VSH_IN_PACKED)
            sb(&b, "layout(location = %u) in int i%u;\n", k, k);
        else
            sb(&b, "layout(location = %u) in vec4 i%u;\n", k, k);
    }
    if (flags & NV2A_GL_VSH_CAPTURE) {
        for (k = 0; k < 8; k++)
            sb(&b, "out vec4 %s;\n", nv2a_gl_vsh_capture_names[k]);
    } else {
        sb(&b, "uniform vec4 u_vp;\nuniform vec4 u_xf;\n%s", k_fog);
        sb(&b, "out vec4 v_d0;\nout vec4 v_d1;\nout float v_fog;\n"
               "out vec4 v_t0;\nout vec4 v_t1;\nout vec4 v_t2;\nout vec4 v_t3;\n");
    }
    sb(&b, "%s", k_helpers);
    sb(&b, "void main() {\n");
    /* vsh_init_regs: temps and oPos 0, colours opaque black, texture
     * coordinates (0,0,0,1), fog 0. */
    sb(&b, "  vec4 R0 = vec4(0.0), R1 = vec4(0.0), R2 = vec4(0.0), R3 = vec4(0.0);\n"
           "  vec4 R4 = vec4(0.0), R5 = vec4(0.0), R6 = vec4(0.0), R7 = vec4(0.0);\n"
           "  vec4 R8 = vec4(0.0), R9 = vec4(0.0), R10 = vec4(0.0), R11 = vec4(0.0);\n"
           "  vec4 R12 = vec4(0.0);\n"
           "  vec4 o_d0 = vec4(0.0, 0.0, 0.0, 1.0), o_d1 = vec4(0.0, 0.0, 0.0, 1.0);\n"
           "  vec4 o_fog = vec4(0.0);\n"
           "  vec4 o_t0 = vec4(0.0, 0.0, 0.0, 1.0), o_t1 = vec4(0.0, 0.0, 0.0, 1.0);\n"
           "  vec4 o_t2 = vec4(0.0, 0.0, 0.0, 1.0), o_t3 = vec4(0.0, 0.0, 0.0, 1.0);\n"
           "  vec4 A = vec4(0.0), B = vec4(0.0), C = vec4(0.0);\n"
           "  vec4 t_m = vec4(0.0), t_i = vec4(0.0);\n"
           "  int a0 = 0;\n");
    /* Raw inputs as fetch_attr reads them. */
    for (k = 0; k < 16; k++) {
        uint32_t f = (uint32_t)(formats >> (4 * k)) & 0xFu;
        if (!(info.inputs & (1u << k)) || !(f & 3u))
            continue;
        switch (f & 3u) {
        case NV2A_GL_VSH_IN_BGRA:
            sb(&b, "  vec4 v%u = i%u.zyxw;\n", k, k);
            break;
        case NV2A_GL_VSH_IN_SNORM: {
            static const char *const sw[4][2] = {
                { "x", "yzw" }, { "xy", "zw" }, { "xyz", "w" }, { "xyzw", NULL }
            };
            uint32_t n = f >> 2;
            if (sw[n][1])
                sb(&b, "  vec4 v%u = vec4(i%u.%s / 32767.0, i%u.%s);\n", k, k, sw[n][0],
                   k, sw[n][1]);
            else
                sb(&b, "  vec4 v%u = i%u / 32767.0;\n", k, k);
            break;
        }
        default:                            /* packed 11:11:10 */
            sb(&b, "  vec4 v%u = vec4(float((i%u << 21) >> 21) / 1023.0,"
                   " float((i%u << 10) >> 21) / 1023.0, float(i%u >> 22) / 511.0, 1.0);\n",
               k, k, k, k);
            break;
        }
    }
    sb(&b, "%s", prog);
    if (flags & NV2A_GL_VSH_CAPTURE) {
        sb(&b, "  x_pos = R12; x_d0 = vsh_sat(o_d0); x_d1 = vsh_sat(o_d1);\n"
               "  x_fog = o_fog; x_t0 = o_t0; x_t1 = o_t1; x_t2 = o_t2; x_t3 = o_t3;\n"
               "  gl_Position = vec4(0.0, 0.0, 0.0, 1.0);\n");
    } else {
        /* transform_vertex: w == +0 is a program that never divided (draw at
         * w = 1); the program's viewport is in logical pixels. Then the CPU
         * path's back end mapping (nv2a_gl_vertex_body), with z measured from
         * the near plane SET_CLIP_MIN. */
        sb(&b, "  vec4 p = R12;\n"
               "  if (floatBitsToUint(p.w) == 0u) p.w = 1.0;\n"
               "  p.xy *= u_xf.xy;\n"
               "  gl_Position = vec4((p.x * u_vp.x - 1.0) * p.w,\n"
               "                     (p.y * u_vp.y - 1.0) * p.w,\n"
               "                     ((p.z - u_xf.z) * u_vp.z - 1.0) * p.w, p.w);\n");
        if (flags & NV2A_GL_VSH_CLIP_DISTANCE)
            sb(&b, "  gl_ClipDistance[0] = (p.z - u_xf.z) * p.w;\n");
        sb(&b, "  v_d0 = vsh_sat(o_d0); v_d1 = vsh_sat(o_d1); v_fog = vsh_fog(o_fog.x);\n"
               "  v_t0 = o_t0; v_t1 = o_t1; v_t2 = o_t2; v_t3 = o_t3;\n");
    }
    sb(&b, "}\n");
    if (b.overflow)
        return 0;
    if (info_out)
        *info_out = info;
    return b.len;
}
