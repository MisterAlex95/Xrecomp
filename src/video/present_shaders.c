/* present_shaders.c -- see present_shaders.h (XBA-195, shared with the Xbox presenter in XBA-203). */
#include "present_shaders.h"

#include <SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ── Presets and keys ──────────────────────────────────────────────────── */

static const char *const k_names[HOST_SHADER_COUNT] = { "none", "sharp", "scanlines", "crt", "lcd" };
static const int k_strength[HOST_SHADER_COUNT] = { 0, 0, 50, 60, 50 };

static int clamp100(int v)
{
    return v < 0 ? 0 : v > 100 ? 100 : v;
}

int present_shader_parse(const char *v)
{
    if (!v)
        return -1;
    for (int i = 0; i < HOST_SHADER_COUNT; i++) {
        if (!strcasecmp(v, k_names[i]))
            return i;
    }
    return -1;
}

const char *present_shader_name(int preset)
{
    return preset >= 0 && preset < HOST_SHADER_COUNT ? k_names[preset] : k_names[0];
}

int present_shader_strength(const HostShader *s)
{
    if (!s || s->preset <= HOST_SHADER_NONE || s->preset >= HOST_SHADER_COUNT)
        return 0;
    return s->strength >= 0 ? clamp100(s->strength) : k_strength[s->preset];
}

HostShader present_shader_clamp(const HostShader *s)
{
    HostShader r = { HOST_SHADER_NONE, -1, 0, 0 };

    if (!s || s->preset <= HOST_SHADER_NONE || s->preset >= HOST_SHADER_COUNT)
        return r;
    r.preset = s->preset;
    r.strength = s->strength < 0 ? -1 : clamp100(s->strength);
    r.curvature = clamp100(s->curvature);
    r.ghosting = clamp100(s->ghosting);
    return r;
}

/* An integer 0..100 from $name, else def (unset, empty or not a number). */
static int env_percent(const char *name, int def)
{
    const char *v = getenv(name);
    char *end;
    long n;

    if (!v || !v[0])
        return def;
    n = strtol(v, &end, 10);
    if (end == v)
        return def;
    return clamp100(n > 1000 ? 1000 : n < -1000 ? -1000 : (int)n);
}

void present_shader_env(const char *console_var, int def, HostShader *out)
{
    int p = console_var ? present_shader_parse(getenv(console_var)) : -1;

    if (!out)
        return;
    if (p < 0)
        p = present_shader_parse(getenv("RECOMP_SHADER"));
    out->preset = p < 0 ? def : p;
    out->strength = env_percent("RECOMP_SHADER_STRENGTH", -1);
    out->curvature = env_percent("RECOMP_SHADER_CURVATURE", 0);
    out->ghosting = env_percent("RECOMP_SHADER_GHOSTING", 0);
}

int present_shader_console_cells(int h)
{
    int k = (h + 120) / 240;

    if (h < 1)
        return 1;
    return h / (k < 1 ? 1 : k);
}

int present_shader_xbox_cells(int h)
{
    static const int k_lines[] = { 480, 576, 720 };
    int lines = h;

    if (h < 1)
        return 1;
    for (size_t i = 0; i < sizeof k_lines / sizeof k_lines[0]; i++) {
        if (h % k_lines[i] == 0) {
            lines = k_lines[i];
            break;
        }
    }
    return lines <= 576 && lines >= 2 ? lines / 2 : lines;
}

/* ── GLSL ──────────────────────────────────────────────────────────────── */

/* One triangle over the viewport. A context of our own: no vertex buffer (gl_VertexID 0..2). */
static const char k_vs_modern[] =
    "out vec2 v_uv;\n"
    "uniform float u_flip;\n"
    "void main() {\n"
    "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "    v_uv = vec2(p.x, mix(p.y, 1.0 - p.y, u_flip));\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";
/* SDL_Renderer's context: the corners from attribute 0; u_rect the part of the texture shown (a ZOOM crop). */
static const char k_vs_legacy[] =
    "attribute vec2 a_pos;\n"
    "varying vec2 v_uv;\n"
    "uniform float u_flip;\n"
    "uniform vec4 u_rect;\n"
    "void main() {\n"
    "    vec2 t = vec2(a_pos.x, mix(a_pos.y, 1.0 - a_pos.y, u_flip));\n"
    "    v_uv = u_rect.xy + t * u_rect.zw;\n"
    "    gl_Position = vec4(a_pos * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

/* The fragment headers: the varying, the output o_color, and texture() for the legacy dialects (which also swap B
 * and R when u_swap is set: SDL's opengles2 renderer keeps ARGB textures as RGBA bytes and swizzles in its own
 * shaders). */
static const char k_fs_head_es300[] =
    "#version 300 es\n"
    /* highp: texel coordinates of an upscaled picture need more than mediump's 11 bits (GLES 3.0 guarantees it). */
    "precision highp float;\n"
    "in vec2 v_uv;\n"
    "out vec4 o_color;\n";
static const char k_fs_head_150[] =
    "#version 150\n"
    "in vec2 v_uv;\n"
    "out vec4 o_color;\n";
static const char k_fs_legacy_tail[] =
    "varying vec2 v_uv;\n"
    "uniform float u_swap;\n"
    "vec4 xr_tex(sampler2D s, vec2 c) {\n"
    "    vec4 t = texture2D(s, c);\n"
    "    return u_swap > 0.5 ? t.bgra : t;\n"
    "}\n"
    "#define texture(s, c) xr_tex(s, c)\n"
    "#define o_color gl_FragColor\n";
static const char k_fs_head_es100[] =
    "#version 100\n"
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
    "precision highp float;\n"
    "#else\n"
    "precision mediump float;\n"
    "#endif\n";
static const char k_fs_head_120[] = "#version 120\n";

/* Uniforms: u_src the texture's size in texels, u_out the viewport's in pixels, u_cells the console pixels the
 * texture holds, u_strength / u_curve / u_ghost 0..1, u_prev the previous picture (LCD ghosting). */
static const char k_fs_common[] =
    "uniform sampler2D u_tex;\n"
    "uniform sampler2D u_prev;\n"
    "uniform vec2 u_src, u_out, u_cells;\n"
    "uniform float u_strength, u_curve, u_ghost;\n"
    /* 1 in the lower half of a console line, 0 in the upper, antialiased over one output pixel; faded out when a
     * line is less than two output pixels tall (no room for a dark half). */
    "float scan_band(float y) {\n"
    "    float px = u_out.y / u_cells.y;\n"
    "    float e = 0.5 / max(px, 1.0);\n"
    "    float f = fract(y);\n"
    "    float b = smoothstep(0.5 - e, 0.5 + e, f) - smoothstep(1.0 - e, 1.0 + e, f);\n"
    "    return b * clamp(px - 1.0, 0.0, 1.0);\n"
    "}\n";

/* Sharp bilinear: as if the picture were first enlarged by the largest whole factor s with nearest, then linearly
 * to the window. Between two texel centres the blend happens only over the 1/s texel around their shared edge. */
static const char k_fs_sharp[] =
    "void main() {\n"
    "    vec2 t = v_uv * u_src - 0.5;\n"
    "    vec2 i = floor(t);\n"
    "    vec2 s = max(floor(u_out / u_src), 1.0);\n"
    "    vec2 w = clamp((t - i - 0.5) * s + 0.5, 0.0, 1.0);\n"
    "    o_color = vec4(texture(u_tex, (i + 0.5 + w) / u_src).rgb, 1.0);\n"
    "}\n";

/* Scanlines: the upper half of each line brightened a little, the lower half darkened by the strength. */
static const char k_fs_scanlines[] =
    "void main() {\n"
    "    vec3 c = texture(u_tex, v_uv).rgb;\n"
    "    float st = u_strength;\n"
    "    float m = mix(1.0 + 0.2 * st, 1.0 - 0.8 * st, scan_band(v_uv.y * u_cells.y));\n"
    "    o_color = vec4(min(c * m, 1.0), 1.0);\n"
    "}\n";

/* Simple CRT: optional barrel curvature (black outside, a soft vignette), scanlines, and an aperture grille (RGB
 * stripes one output pixel wide), faded out when a console pixel is barely one output pixel wide. */
static const char k_fs_crt[] =
    "void main() {\n"
    "    vec2 uv = v_uv;\n"
    "    if (u_curve > 0.0) {\n"
    "        vec2 p = uv * 2.0 - 1.0;\n"
    "        p *= 1.0 + u_curve * 0.15 * (p.yx * p.yx);\n"
    "        uv = p * 0.5 + 0.5;\n"
    "        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {\n"
    "            o_color = vec4(0.0, 0.0, 0.0, 1.0);\n"
    "            return;\n"
    "        }\n"
    "    }\n"
    "    vec3 c = texture(u_tex, uv).rgb;\n"
    "    float st = u_strength;\n"
    "    c *= mix(1.0 + 0.25 * st, 1.0 - 0.7 * st, scan_band(uv.y * u_cells.y));\n"
    "    float col = mod(floor(gl_FragCoord.x), 3.0);\n"
    "    vec3 mask = col < 0.5 ? vec3(1.0, 0.65, 0.65) : col < 1.5 ? vec3(0.65, 1.0, 0.65) : vec3(0.65, 0.65, 1.0);\n"
    "    c *= mix(vec3(1.0), mask * 1.2, st * clamp(u_out.x / u_cells.x - 1.0, 0.0, 1.0));\n"
    "    if (u_curve > 0.0) {\n"
    "        vec2 d = uv * 2.0 - 1.0;\n"
    "        c *= 1.0 - 0.3 * u_curve * dot(d * d, d * d);\n"
    "    }\n"
    "    o_color = vec4(min(c, 1.0), 1.0);\n"
    "}\n";

/* LCD grid: each console pixel sharp, its right and bottom edge (one output pixel, antialiased) darkened by the
 * strength, faded out below 2.5 output pixels per cell; ghosting blends in the previous picture. */
static const char k_fs_lcd[] =
    "void main() {\n"
    "    vec2 tc = (floor(v_uv * u_src) + 0.5) / u_src;\n"
    "    vec3 c = texture(u_tex, tc).rgb;\n"
    "    if (u_ghost > 0.0)\n"
    "        c = mix(c, texture(u_prev, tc).rgb, 0.5 * u_ghost);\n"
    "    vec2 px = u_out / u_cells;\n"
    "    vec2 f = fract(v_uv * u_cells);\n"
    "    vec2 g = clamp((f + 0.5 / px - 1.0 + 1.0 / px) * px, 0.0, 1.0) * clamp(px - 1.5, 0.0, 1.0);\n"
    "    c *= 1.0 - 0.7 * u_strength * max(g.x, g.y);\n"
    "    o_color = vec4(c, 1.0);\n"
    "}\n";

static const char *const k_fs_body[HOST_SHADER_COUNT] = { NULL, k_fs_sharp, k_fs_scanlines, k_fs_crt, k_fs_lcd };

static int legacy(PresentGlsl d)
{
    return d == PRESENT_GLSL_ES100 || d == PRESENT_GLSL_120;
}

void present_shader_vs_src(PresentGlsl d, const char **src, int *n)
{
    static const char *const heads[] = { "#version 300 es\n", "#version 150\n", "#version 100\n", "#version 120\n" };

    src[0] = heads[(unsigned)d < 4 ? d : 0];
    src[1] = legacy(d) ? k_vs_legacy : k_vs_modern;
    *n = 2;
}

void present_shader_fs_src(PresentGlsl d, int preset, const char *fs_body, const char **src, int *n)
{
    const char *body = fs_body;

    *n = 0;
    if (preset <= HOST_SHADER_NONE || preset >= HOST_SHADER_COUNT)
        return;
    if (!body)
        body = k_fs_body[preset];
    switch (d) {
    case PRESENT_GLSL_150:
        src[(*n)++] = k_fs_head_150;
        break;
    case PRESENT_GLSL_ES100:
        src[(*n)++] = k_fs_head_es100;
        src[(*n)++] = k_fs_legacy_tail;
        break;
    case PRESENT_GLSL_120:
        src[(*n)++] = k_fs_head_120;
        src[(*n)++] = k_fs_legacy_tail;
        break;
    default:
        src[(*n)++] = k_fs_head_es300;
        break;
    }
    src[(*n)++] = k_fs_common;
    src[(*n)++] = body;
}

/* ── GL ────────────────────────────────────────────────────────────────── */

#ifndef APIENTRY
#define APIENTRY
#endif

#define PGL_FLOAT                     0x1406
#define PGL_UNSIGNED_BYTE             0x1401
#define PGL_RGBA                      0x1908
#define PGL_TRIANGLES                 0x0004
#define PGL_TEXTURE0                  0x84C0
#define PGL_ACTIVE_TEXTURE            0x84E0
#define PGL_CURRENT_PROGRAM           0x8B8D
#define PGL_ARRAY_BUFFER              0x8892
#define PGL_ARRAY_BUFFER_BINDING      0x8894
#define PGL_VIEWPORT                  0x0BA2
#define PGL_SCISSOR_TEST              0x0C11
#define PGL_BLEND                     0x0BE2
#define PGL_PACK_ALIGNMENT            0x0D05
#define PGL_FRAGMENT_SHADER           0x8B30
#define PGL_VERTEX_SHADER             0x8B31
#define PGL_COMPILE_STATUS            0x8B81
#define PGL_LINK_STATUS               0x8B82
#define PGL_ATTRIB_ENABLED            0x8622
#define PGL_ATTRIB_SIZE               0x8623
#define PGL_ATTRIB_STRIDE             0x8624
#define PGL_ATTRIB_TYPE               0x8625
#define PGL_ATTRIB_NORMALIZED         0x886A
#define PGL_ATTRIB_BUFFER_BINDING     0x889F
#define PGL_ATTRIB_POINTER            0x8645

static struct {
    int loaded;   /* 1 loaded, -1 an entry point is missing */
    unsigned (APIENTRY *CreateShader)(unsigned);
    void (APIENTRY *ShaderSource)(unsigned, int, const char *const *, const int *);
    void (APIENTRY *CompileShader)(unsigned);
    void (APIENTRY *GetShaderiv)(unsigned, unsigned, int *);
    void (APIENTRY *GetShaderInfoLog)(unsigned, int, int *, char *);
    void (APIENTRY *DeleteShader)(unsigned);
    unsigned (APIENTRY *CreateProgram)(void);
    void (APIENTRY *AttachShader)(unsigned, unsigned);
    void (APIENTRY *BindAttribLocation)(unsigned, unsigned, const char *);
    void (APIENTRY *LinkProgram)(unsigned);
    void (APIENTRY *GetProgramiv)(unsigned, unsigned, int *);
    void (APIENTRY *GetProgramInfoLog)(unsigned, int, int *, char *);
    void (APIENTRY *DeleteProgram)(unsigned);
    void (APIENTRY *UseProgram)(unsigned);
    int (APIENTRY *GetUniformLocation)(unsigned, const char *);
    void (APIENTRY *Uniform1i)(int, int);
    void (APIENTRY *Uniform1f)(int, float);
    void (APIENTRY *Uniform2f)(int, float, float);
    void (APIENTRY *Uniform4f)(int, float, float, float, float);
    void (APIENTRY *GetIntegerv)(unsigned, int *);
    unsigned char (APIENTRY *IsEnabled)(unsigned);
    void (APIENTRY *Enable)(unsigned);
    void (APIENTRY *Disable)(unsigned);
    void (APIENTRY *Viewport)(int, int, int, int);
    void (APIENTRY *ActiveTexture)(unsigned);
    void (APIENTRY *BindBuffer)(unsigned, unsigned);
    void (APIENTRY *GetVertexAttribiv)(unsigned, unsigned, int *);
    void (APIENTRY *GetVertexAttribPointerv)(unsigned, unsigned, void **);
    void (APIENTRY *EnableVertexAttribArray)(unsigned);
    void (APIENTRY *DisableVertexAttribArray)(unsigned);
    void (APIENTRY *VertexAttribPointer)(unsigned, int, unsigned, unsigned char, int, const void *);
    void (APIENTRY *DrawArrays)(unsigned, int, int);
    void (APIENTRY *PixelStorei)(unsigned, int);
    void (APIENTRY *ReadPixels)(int, int, int, int, unsigned, unsigned, void *);
    unsigned (APIENTRY *GetError)(void);
} g;

static int gl_load(void)
{
    struct { void **slot; const char *name; } fns[] = {
#define F(n) { (void **)&g.n, "gl" #n }
        F(CreateShader), F(ShaderSource), F(CompileShader), F(GetShaderiv), F(GetShaderInfoLog), F(DeleteShader),
        F(CreateProgram), F(AttachShader), F(BindAttribLocation), F(LinkProgram), F(GetProgramiv),
        F(GetProgramInfoLog), F(DeleteProgram), F(UseProgram), F(GetUniformLocation), F(Uniform1i), F(Uniform1f),
        F(Uniform2f), F(Uniform4f), F(GetIntegerv), F(IsEnabled), F(Enable), F(Disable), F(Viewport),
        F(ActiveTexture), F(BindBuffer), F(GetVertexAttribiv), F(GetVertexAttribPointerv),
        F(EnableVertexAttribArray), F(DisableVertexAttribArray), F(VertexAttribPointer), F(DrawArrays),
        F(PixelStorei), F(ReadPixels), F(GetError),
#undef F
    };
    if (g.loaded)
        return g.loaded > 0;
    for (size_t i = 0; i < sizeof fns / sizeof fns[0]; i++) {
        *fns[i].slot = SDL_GL_GetProcAddress(fns[i].name);
        if (!*fns[i].slot) {
            fprintf(stderr, "  [HOST] present shaders: no %s in the GL context\n", fns[i].name);
            g.loaded = -1;
            return 0;
        }
    }
    g.loaded = 1;
    return 1;
}

static unsigned compile(unsigned type, const char **src, int n, const char *tag)
{
    unsigned sh = g.CreateShader(type);
    int ok = 0;

    g.ShaderSource(sh, n, src, NULL);
    g.CompileShader(sh);
    g.GetShaderiv(sh, PGL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        g.GetShaderInfoLog(sh, (int)sizeof log, NULL, log);
        fprintf(stderr, "  %s shader: %s\n", tag, log);
        g.DeleteShader(sh);
        return 0;
    }
    return sh;
}

static unsigned link_program(PresentGlsl d, int preset, const char *fs_body, const char *tag)
{
    const char *vsrc[4], *fsrc[4];
    int vn = 0, fn = 0, ok = 0;
    unsigned vs, fs, prog;

    present_shader_vs_src(d, vsrc, &vn);
    present_shader_fs_src(d, preset, fs_body, fsrc, &fn);
    if (!fn)
        return 0;
    vs = compile(PGL_VERTEX_SHADER, vsrc, vn, tag);
    fs = vs ? compile(PGL_FRAGMENT_SHADER, fsrc, fn, tag) : 0;
    if (!vs || !fs) {
        if (vs)
            g.DeleteShader(vs);
        return 0;
    }
    prog = g.CreateProgram();
    g.AttachShader(prog, vs);
    g.AttachShader(prog, fs);
    if (legacy(d))
        g.BindAttribLocation(prog, 0, "a_pos");
    g.LinkProgram(prog);
    g.DeleteShader(vs);
    g.DeleteShader(fs);
    g.GetProgramiv(prog, PGL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        g.GetProgramInfoLog(prog, (int)sizeof log, NULL, log);
        fprintf(stderr, "  %s program: %s\n", tag, log);
        g.DeleteProgram(prog);
        return 0;
    }
    return prog;
}

static int s_logged[HOST_SHADER_COUNT]; /* a failure is logged once per process */

int present_shader_build(PresentShaderProg *sp, PresentGlsl d, int preset, const char *tag)
{
    const char *test_fail;

    if (!sp || preset <= HOST_SHADER_NONE || preset >= HOST_SHADER_COUNT)
        return 0;
    if (sp->failed)
        return 0;
    if (sp->prog)
        return 1;
    if (!tag)
        tag = "[HOST]";
    test_fail = getenv("RECOMP_SHADER_TEST_FAIL");   /* the fallback's test: a shader that fails */
    sp->prog = gl_load() ? link_program(d, preset, test_fail && !strcmp(test_fail, k_names[preset]) ? "not glsl;\n"
                                                                                                    : NULL, tag)
                         : 0;
    if (!sp->prog) {
        sp->failed = 1;
        if (!s_logged[preset]) {
            s_logged[preset] = 1;
            fprintf(stderr, "  %s picture shader %s does not build here; drawing without one\n", tag,
                    k_names[preset]);
        }
        return 0;
    }
    sp->u_tex = g.GetUniformLocation(sp->prog, "u_tex");
    sp->u_prev = g.GetUniformLocation(sp->prog, "u_prev");
    sp->u_flip = g.GetUniformLocation(sp->prog, "u_flip");
    sp->u_rect = g.GetUniformLocation(sp->prog, "u_rect");
    sp->u_swap = g.GetUniformLocation(sp->prog, "u_swap");
    sp->u_src = g.GetUniformLocation(sp->prog, "u_src");
    sp->u_out = g.GetUniformLocation(sp->prog, "u_out");
    sp->u_cells = g.GetUniformLocation(sp->prog, "u_cells");
    sp->u_strength = g.GetUniformLocation(sp->prog, "u_strength");
    sp->u_curve = g.GetUniformLocation(sp->prog, "u_curve");
    sp->u_ghost = g.GetUniformLocation(sp->prog, "u_ghost");
    fprintf(stderr, "  %s picture shader %s\n", tag, k_names[preset]);
    return 1;
}

void present_shader_release(PresentShaderProg *sp, int lost)
{
    if (!sp)
        return;
    if (sp->prog && !lost && g.loaded > 0)
        g.DeleteProgram(sp->prog);
    memset(sp, 0, sizeof *sp);
}

void present_shader_use(const PresentShaderProg *sp, const PresentShaderParams *p)
{
    g.UseProgram(sp->prog);
    g.Uniform1i(sp->u_tex, 0);
    g.Uniform1i(sp->u_prev, 1);
    g.Uniform1f(sp->u_flip, p->flip ? 1.0f : 0.0f);
    if (sp->u_rect >= 0) {
        if (p->rect[2] > 0.0f && p->rect[3] > 0.0f)
            g.Uniform4f(sp->u_rect, p->rect[0], p->rect[1], p->rect[2], p->rect[3]);
        else
            g.Uniform4f(sp->u_rect, 0.0f, 0.0f, 1.0f, 1.0f);
    }
    if (sp->u_swap >= 0)
        g.Uniform1f(sp->u_swap, p->swap ? 1.0f : 0.0f);
    g.Uniform2f(sp->u_src, p->src_w, p->src_h);
    g.Uniform2f(sp->u_out, p->out_w, p->out_h);
    g.Uniform2f(sp->u_cells, p->cells_w, p->cells_h);
    g.Uniform1f(sp->u_strength, (float)clamp100(p->strength) / 100.0f);
    g.Uniform1f(sp->u_curve, (float)clamp100(p->curvature) / 100.0f);
    g.Uniform1f(sp->u_ghost, (float)clamp100(p->ghosting) / 100.0f);
}

unsigned present_shader_draw_legacy(const PresentShaderProg *sp, const PresentShaderParams *p,
                                    int vx, int vy, int vw, int vh)
{
    static const float k_tri[6] = { 0.0f, 0.0f, 2.0f, 0.0f, 0.0f, 2.0f };
    int prog = 0, active = 0, abuf = 0, vp[4] = { 0, 0, 0, 0 }, en[3] = { 0, 0, 0 };
    int a0_buf = 0, a0_size = 4, a0_stride = 0, a0_type = PGL_FLOAT, a0_norm = 0;
    void *a0_ptr = NULL;
    unsigned char scissor, blend;
    unsigned err;

    if (!sp || !sp->prog || g.loaded <= 0)
        return 1;
    while (g.GetError())   /* earlier errors are not ours */
        ;
    g.GetIntegerv(PGL_CURRENT_PROGRAM, &prog);
    g.GetIntegerv(PGL_ACTIVE_TEXTURE, &active);
    g.GetIntegerv(PGL_ARRAY_BUFFER_BINDING, &abuf);
    g.GetIntegerv(PGL_VIEWPORT, vp);
    scissor = g.IsEnabled(PGL_SCISSOR_TEST);
    blend = g.IsEnabled(PGL_BLEND);
    for (unsigned i = 0; i < 3; i++)
        g.GetVertexAttribiv(i, PGL_ATTRIB_ENABLED, &en[i]);
    g.GetVertexAttribiv(0, PGL_ATTRIB_BUFFER_BINDING, &a0_buf);
    g.GetVertexAttribiv(0, PGL_ATTRIB_SIZE, &a0_size);
    g.GetVertexAttribiv(0, PGL_ATTRIB_STRIDE, &a0_stride);
    g.GetVertexAttribiv(0, PGL_ATTRIB_TYPE, &a0_type);
    g.GetVertexAttribiv(0, PGL_ATTRIB_NORMALIZED, &a0_norm);
    g.GetVertexAttribPointerv(0, PGL_ATTRIB_POINTER, &a0_ptr);

    if (scissor)
        g.Disable(PGL_SCISSOR_TEST);
    if (blend)
        g.Disable(PGL_BLEND);
    g.ActiveTexture(PGL_TEXTURE0);
    g.Viewport(vx, vy, vw, vh);
    present_shader_use(sp, p);
    g.BindBuffer(PGL_ARRAY_BUFFER, 0);
    if (en[1])
        g.DisableVertexAttribArray(1);
    if (en[2])
        g.DisableVertexAttribArray(2);
    g.EnableVertexAttribArray(0);
    g.VertexAttribPointer(0, 2, PGL_FLOAT, 0, 0, k_tri);
    g.DrawArrays(PGL_TRIANGLES, 0, 3);
    err = g.GetError();

    /* SDL's renderer caches these: put them back as they were. */
    g.BindBuffer(PGL_ARRAY_BUFFER, (unsigned)a0_buf);
    g.VertexAttribPointer(0, a0_size, (unsigned)a0_type, (unsigned char)(a0_norm != 0), a0_stride, a0_ptr);
    g.BindBuffer(PGL_ARRAY_BUFFER, (unsigned)abuf);
    if (!en[0])
        g.DisableVertexAttribArray(0);
    if (en[1])
        g.EnableVertexAttribArray(1);
    if (en[2])
        g.EnableVertexAttribArray(2);
    g.UseProgram((unsigned)prog);
    g.Viewport(vp[0], vp[1], vp[2], vp[3]);
    if (scissor)
        g.Enable(PGL_SCISSOR_TEST);
    if (blend)
        g.Enable(PGL_BLEND);
    g.ActiveTexture((unsigned)active);
    return err;
}

int present_shader_read_rgb(int w, int h, unsigned char *rgb)
{
    unsigned char *rgba;

    if (w <= 0 || h <= 0 || !rgb || !gl_load())
        return 0;
    rgba = malloc((size_t)w * h * 4);
    if (!rgba)
        return 0;
    g.PixelStorei(PGL_PACK_ALIGNMENT, 4);
    g.ReadPixels(0, 0, w, h, PGL_RGBA, PGL_UNSIGNED_BYTE, rgba);
    for (int y = 0; y < h; y++) {
        const unsigned char *s = rgba + (size_t)(h - 1 - y) * w * 4;   /* GL's row 0 is the bottom */
        unsigned char *d = rgb + (size_t)y * w * 3;
        for (int x = 0; x < w; x++) {
            d[x * 3 + 0] = s[x * 4 + 0];
            d[x * 3 + 1] = s[x * 4 + 1];
            d[x * 3 + 2] = s[x * 4 + 2];
        }
    }
    free(rgba);
    return 1;
}
