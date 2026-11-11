/**
 * Register combiners to GLSL. See nv2a_gl_shader.h.
 *
 * Every rule here is a transcription of the CPU code that draws the same
 * pixel; the comments name the function each block mirrors.
 */
#include "nv2a_gl_shader.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void nv2a_gl_shader_key(const Nv2aPixelState *ps, const Nv2aRenderState *rs,
                        const uint32_t tex_kind[4], Nv2aGlShaderKey *key)
{
    uint32_t s, n;

    memset(key, 0, sizeof *key);
    key->shade = ps->shade;
    for (s = 0; s < 4; s++)
        key->tex_kind[s] = tex_kind[s];
    if (ps->shade == NV2A_SHADE_COMBINERS) {
        n = ps->rc.control & 0xFF;
        if (n > 8)
            n = 8;
        key->stages = n;
        key->flags = ps->rc.control >> 8;
        for (s = 0; s < n; s++) {
            key->color_icw[s] = ps->rc.color_icw[s];
            key->alpha_icw[s] = ps->rc.alpha_icw[s];
            key->color_ocw[s] = ps->rc.color_ocw[s];
            key->alpha_ocw[s] = ps->rc.alpha_ocw[s];
        }
        key->final0 = ps->rc.final0;
        key->final1 = ps->rc.final1;
        key->stage_program = ps->rc.stage_program & 0xFFFFF;
        for (s = 0; s < 4; s++)
            if (((key->stage_program >> (s * 5)) & 0x1F) == 5)
                key->clip_plane_mode = ps->clip_plane_mode;
    }
    /* alpha_test_pass(): ALWAYS and anything unknown pass, so they are "off".
     * The screen-space path never alpha-tests. */
    if (rs->alpha_test_enable && ps->shade != NV2A_SHADE_REPLACE
        && rs->alpha_func >= 0x200 && rs->alpha_func <= 0x206)
        key->alpha_func = rs->alpha_func;
}

uint64_t nv2a_gl_shader_key_hash(const Nv2aGlShaderKey *key)
{
    const unsigned char *p = (const unsigned char *)key;
    uint64_t h = 1469598103934665603ull;
    size_t i;

    for (i = 0; i < sizeof *key; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

const char *nv2a_gl_glsl_header(int gles)
{
    return gles ? "#version 300 es\nprecision highp float;\nprecision highp int;\n"
                : "#version 330 core\n";
}

const char *nv2a_gl_vertex_body(void)
{
    return
        "layout(location = 0) in vec4 a_pos;\n"
        "layout(location = 1) in vec4 a_d0;\n"
        "layout(location = 2) in vec4 a_d1;\n"
        "layout(location = 3) in float a_fog;\n"
        "layout(location = 4) in vec4 a_t0;\n"
        "layout(location = 5) in vec4 a_t1;\n"
        "layout(location = 6) in vec4 a_t2;\n"
        "layout(location = 7) in vec4 a_t3;\n"
        "uniform vec4 u_vp;\n"
        "out vec4 v_d0;\nout vec4 v_d1;\nout float v_fog;\n"
        "out vec4 v_t0;\nout vec4 v_t1;\nout vec4 v_t2;\nout vec4 v_t3;\n"
        "void main() {\n"
        /* Surface pixels and window z back to clip space, times w so the
         * divide gives them back and every varying is interpolated with 1/w,
         * as the CPU rasteriser interpolates texture coordinates. Row y of
         * the surface is row y of the render target: no flip anywhere. */
        "  float w = a_pos.w;\n"
        "  gl_Position = vec4((a_pos.x * u_vp.x - 1.0) * w,\n"
        "                     (a_pos.y * u_vp.y - 1.0) * w,\n"
        "                     (a_pos.z * u_vp.z - 1.0) * w, w);\n"
        "  v_d0 = a_d0; v_d1 = a_d1; v_fog = a_fog;\n"
        "  v_t0 = a_t0; v_t1 = a_t1; v_t2 = a_t2; v_t3 = a_t3;\n"
        "}\n";
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

/* ---- combiner operands (nv2a_combiner.c read_rgb / read_alpha / map_in) */

static const char *map_fmt(uint32_t byte)
{
    switch ((byte >> 5) & 7) {
    case 0:  return "max(%s, 0.0)";                       /* unsigned identity */
    case 1:  return "(1.0 - clamp(%s, 0.0, 1.0))";        /* unsigned invert   */
    case 2:  return "(2.0 * max(%s, 0.0) - 1.0)";         /* expand normal     */
    case 3:  return "(-2.0 * max(%s, 0.0) + 1.0)";        /* expand negate     */
    case 4:  return "(max(%s, 0.0) - 0.5)";               /* half-bias normal  */
    case 5:  return "(-max(%s, 0.0) + 0.5)";              /* half-bias negate  */
    case 6:  return "(%s)";                               /* signed identity   */
    default: return "(-(%s))";                            /* signed negate     */
    }
}

static void emit_rgb(Sb *b, const char *name, uint32_t byte)
{
    char src[32];

    byte &= 0xFF;
    if (byte & 0x10)
        snprintf(src, sizeof src, "vec3(R%u.a)", byte & 0xF);
    else
        snprintf(src, sizeof src, "R%u.rgb", byte & 0xF);
    sb(b, "    vec3 %s = ", name);
    sb(b, map_fmt(byte), src);
    sb(b, ";\n");
}

static void emit_alpha(Sb *b, const char *name, uint32_t byte)
{
    char src[16];

    byte &= 0xFF;
    snprintf(src, sizeof src, "R%u.%c", byte & 0xF, (byte & 0x10) ? 'a' : 'b');
    sb(b, "    float %s = ", name);
    sb(b, map_fmt(byte), src);
    sb(b, ";\n");
}

/* map_out(): output scale and bias. */
static const char *out_fmt(uint32_t mapping)
{
    switch (mapping) {
    case 0x08: return "(%s - 0.5)";
    case 0x10: return "(%s * 2.0)";
    case 0x18: return "((%s - 0.5) * 2.0)";
    case 0x20: return "(%s * 4.0)";
    case 0x30: return "(%s * 0.5)";
    default:   return "%s";
    }
}

static void emit_clamped(Sb *b, const char *type, const char *name,
                         uint32_t mapping, const char *src)
{
    sb(b, "    %s %s = clamp(", type, name);
    sb(b, out_fmt(mapping), src);
    sb(b, ", -1.0, 1.0);\n");
}

/* One general combiner stage (nv2a_rc_eval, the loop body). */
static void emit_stage(Sb *b, const Nv2aGlShaderKey *k, uint32_t s)
{
    uint32_t icw = k->color_icw[s], ocw = k->color_ocw[s];
    uint32_t aicw = k->alpha_icw[s], aocw = k->alpha_ocw[s];
    uint32_t fl = ocw >> 12, afl = aocw >> 12, d;

    sb(b, "  {\n");
    /* One c0/c1 for every stage, or one per stage. */
    sb(b, "    R1 = u_c0[%u]; R2 = u_c1[%u];\n",
       (k->flags & 0x010) ? s : 0u, (k->flags & 0x100) ? s : 0u);
    if ((fl & 4) || (afl & 4)) {
        if (k->flags & 1)
            sb(b, "    bool mux = R12.a >= 0.5;\n");
        else
            sb(b, "    bool mux = (int(R12.a * 255.0) & 1) != 0;\n");
    }
    /* Both portions read before either writes. */
    emit_rgb(b, "A", icw >> 24); emit_rgb(b, "B", icw >> 16);
    emit_rgb(b, "C", icw >> 8);  emit_rgb(b, "D", icw);
    emit_alpha(b, "aA", aicw >> 24); emit_alpha(b, "aB", aicw >> 16);
    emit_alpha(b, "aC", aicw >> 8);  emit_alpha(b, "aD", aicw);

    sb(b, "    vec3 pab = %s;\n", (fl & 2) ? "vec3(dot(A, B))" : "A * B");
    sb(b, "    vec3 pcd = %s;\n", (fl & 1) ? "vec3(dot(C, D))" : "C * D");
    sb(b, "    vec3 psum = %s;\n", (fl & 4) ? "(mux ? pcd : pab)" : "pab + pcd");
    emit_clamped(b, "vec3", "ab", fl & 0x38, "pab");
    emit_clamped(b, "vec3", "cd", fl & 0x38, "pcd");
    emit_clamped(b, "vec3", "ms", fl & 0x38, "psum");
    sb(b, "    float qab = aA * aB, qcd = aC * aD;\n");
    sb(b, "    float qsum = %s;\n", (afl & 4) ? "(mux ? qcd : qab)" : "qab + qcd");
    emit_clamped(b, "float", "aab", afl & 0x38, "qab");
    emit_clamped(b, "float", "acd", afl & 0x38, "qcd");
    emit_clamped(b, "float", "ams", afl & 0x38, "qsum");

    /* RGB destinations; blue-to-alpha also writes the destination's .a. */
    if ((d = (ocw >> 4) & 0xF) != 0) {
        sb(b, "    R%u.rgb = ab;\n", d);
        if (fl & 0x80) sb(b, "    R%u.a = ab.b;\n", d);
    }
    if ((d = ocw & 0xF) != 0) {
        sb(b, "    R%u.rgb = cd;\n", d);
        if (fl & 0x40) sb(b, "    R%u.a = cd.b;\n", d);
    }
    if ((d = (ocw >> 8) & 0xF) != 0)
        sb(b, "    R%u.rgb = ms;\n", d);
    if ((d = (aocw >> 4) & 0xF) != 0) sb(b, "    R%u.a = aab;\n", d);
    if ((d = aocw & 0xF) != 0)        sb(b, "    R%u.a = acd;\n", d);
    if ((d = (aocw >> 8) & 0xF) != 0) sb(b, "    R%u.a = ams;\n", d);
    /* Register 0 is zero whatever was "written" to it. */
    sb(b, "    R0 = vec4(0.0);\n  }\n");
}

/* The final combiner (nv2a_rc_eval, after the loop). */
static void emit_final(Sb *b, const Nv2aGlShaderKey *k)
{
    uint32_t f0 = k->final0, f1 = k->final1, ff = f1 & 0xFF;

    sb(b, "  R1 = u_fc0; R2 = u_fc1;\n");
    sb(b, "  {\n    vec3 fa = %s;\n    vec3 fb = %s;\n",
       (ff & 0x40) ? "1.0 - R5.rgb" : "R5.rgb",
       (ff & 0x20) ? "1.0 - R12.rgb" : "R12.rgb");
    sb(b, "    R14 = vec4(%s, 0.0);\n  }\n",
       (ff & 0x80) ? "clamp(fa + fb, 0.0, 1.0)" : "fa + fb");
    sb(b, "  {\n");
    emit_rgb(b, "E", f1 >> 24);
    emit_rgb(b, "F", f1 >> 16);
    sb(b, "    R15 = vec4(E * F, 0.0);\n  }\n");
    sb(b, "  {\n");
    emit_rgb(b, "A", f0 >> 24); emit_rgb(b, "B", f0 >> 16);
    emit_rgb(b, "C", f0 >> 8);  emit_rgb(b, "D", f0);
    emit_alpha(b, "G", f1 >> 8);
    sb(b, "    o = vec4(D + C * (1.0 - A) + B * A, G);\n  }\n");
}

/* One texture stage's contribution (nv2a_pb_exec.c rc_stage_fetch). */
static void emit_fetch(Sb *b, const Nv2aGlShaderKey *k, uint32_t st)
{
    uint32_t mode = (k->stage_program >> (st * 5)) & 0x1F, kind = k->tex_kind[st], j;

    switch (mode) {
    case 0:                                       /* NONE */
        sb(b, "  vec4 T%u = vec4(0.0, 0.0, 0.0, 1.0);\n", st);
        return;
    case 1: case 2:                               /* PROJECT2D / 3D */
        if (kind == NV2A_GL_TEX_NONE) {
            sb(b, "  vec4 T%u = vec4(1.0);\n", st);  /* unusable: white */
            return;
        }
        sb(b, "  float q%u = v_t%u.w != 0.0 ? v_t%u.w : 1.0;\n", st, st, st);
        sb(b, "  vec4 T%u = texture(u_t%u, v_t%u.xy / q%u * u_tscale[%u]);\n",
           st, st, st, st, st);
        return;
    case 3:                                       /* CUBEMAP */
        if (kind == NV2A_GL_TEX_CUBE)
            sb(b, "  vec4 T%u = texture(u_t%u, v_t%u.xyz);\n", st, st, st);
        else if (kind == NV2A_GL_TEX_2D)          /* not a cube: plain 2D, no divide */
            sb(b, "  vec4 T%u = texture(u_t%u, v_t%u.xy * u_tscale[%u]);\n",
               st, st, st, st);
        else
            sb(b, "  vec4 T%u = vec4(1.0);\n", st);
        return;
    case 4:                                       /* PASSTHRU */
        sb(b, "  vec4 T%u = clamp(v_t%u, 0.0, 1.0);\n", st, st);
        return;
    case 5:                                       /* CLIPPLANE */
        for (j = 0; j < 4; j++) {
            int ge = (k->clip_plane_mode >> (st * 4 + j)) & 1;
            sb(b, "  if (v_t%u[%u] %s 0.0) discard;\n", st, j, ge ? ">=" : "<");
        }
        sb(b, "  vec4 T%u = vec4(0.0);\n", st);
        return;
    default:
        /* Bump-env and dot-product modes read zero on the CPU path too. */
        sb(b, "  vec4 T%u = vec4(0.0);\n", st);
        return;
    }
}

static void emit_alpha_test(Sb *b, uint32_t func)
{
    const char *op;

    switch (func) {
    case 0x200: sb(b, "  discard;\n"); return;    /* NEVER */
    case 0x201: op = "<";  break;
    case 0x202: op = "=="; break;
    case 0x203: op = "<="; break;
    case 0x204: op = ">";  break;
    case 0x205: op = "!="; break;
    case 0x206: op = ">="; break;
    default: return;
    }
    /* alpha_test_pass(): the alpha in 0..255, rounded, against the ref. */
    sb(b, "  if (!(int(o.a * 255.0 + 0.5) %s int(u_aref))) discard;\n", op);
}

size_t nv2a_gl_fragment_body(const Nv2aGlShaderKey *k, char *buf, size_t cap)
{
    Sb b;
    uint32_t st, s;

    b.buf = buf; b.cap = cap; b.len = 0; b.overflow = 0;
    if (!cap)
        return 0;
    buf[0] = 0;
    sb(&b, "in vec4 v_d0;\nin vec4 v_d1;\nin float v_fog;\n"
           "in vec4 v_t0;\nin vec4 v_t1;\nin vec4 v_t2;\nin vec4 v_t3;\n");
    for (st = 0; st < 4; st++)
        sb(&b, "uniform %s u_t%u;\n",
           k->tex_kind[st] == NV2A_GL_TEX_CUBE ? "samplerCube" : "sampler2D", st);
    sb(&b, "uniform vec2 u_tscale[4];\nuniform vec4 u_c0[8];\nuniform vec4 u_c1[8];\n"
           "uniform vec4 u_fc0;\nuniform vec4 u_fc1;\nuniform vec4 u_fogc;\n"
           "uniform float u_aref;\n"
           "layout(location = 0) out vec4 o_color;\n"
           "void main() {\n  vec4 o;\n");

    if (k->shade == NV2A_SHADE_COMBINERS) {
        for (st = 0; st < 4; st++)
            emit_fetch(&b, k, st);
        /* Only the registers a program can read before writing need a value:
         * zero, r0, r1 and the final-combiner specials. */
        sb(&b, "  vec4 R0 = vec4(0.0), R1 = vec4(0.0), R2 = vec4(0.0);\n"
               "  vec4 R3 = vec4(u_fogc.rgb, v_fog);\n"
               "  vec4 R4 = v_d0, R5 = v_d1, R6 = vec4(0.0), R7 = vec4(0.0);\n"
               "  vec4 R8 = T0, R9 = T1, R10 = T2, R11 = T3;\n"
               "  vec4 R12 = vec4(0.0, 0.0, 0.0, %s), R13 = vec4(0.0);\n"
               "  vec4 R14 = vec4(0.0), R15 = vec4(0.0);\n",
           /* r0.a starts as texture 0's alpha, or 1 with stage 0 off. */
           (k->stage_program & 0x1F) ? "T0.a" : "1.0");
        for (s = 0; s < k->stages; s++)
            emit_stage(&b, k, s);
        if (k->final0 || k->final1)
            emit_final(&b, k);
        else
            sb(&b, "  o = R12;\n");
        sb(&b, "  o = clamp(o, 0.0, 1.0);\n");
    } else if (k->shade == NV2A_SHADE_MODULATE) {
        /* xf_rows without combiners: stage 0 texel * diffuse. */
        if (k->tex_kind[0] != NV2A_GL_TEX_NONE)
            sb(&b, "  o = texture(u_t0, v_t0.xy * u_tscale[0]) * clamp(v_d0, 0.0, 1.0);\n");
        else
            sb(&b, "  o = clamp(v_d0, 0.0, 1.0);\n");
    } else {
        /* raster_triangle: the texel as it is, else the flat colour. */
        if (k->tex_kind[0] != NV2A_GL_TEX_NONE)
            sb(&b, "  o = texture(u_t0, v_t0.xy * u_tscale[0]);\n");
        else
            sb(&b, "  o = clamp(v_d0, 0.0, 1.0);\n");
    }
    if (k->alpha_func)
        emit_alpha_test(&b, k->alpha_func);
    sb(&b, "  o_color = o;\n}\n");
    return b.overflow ? 0 : b.len;
}
