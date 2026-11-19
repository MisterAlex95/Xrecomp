/* The GL back end's generated combiner shaders against the CPU combiners.
 *
 * nv2a_gl_shader.c transcribes nv2a_rc_eval() into GLSL. This draws one
 * flat quad per combiner program -- vertex colours, fog and four 1x1
 * textures as the inputs -- reads the pixel back and compares it with what
 * nv2a_rc_eval() computes from the same inputs. Programs are random (every
 * input mapping, output scale/bias, dot products, the final combiner) plus a
 * few fixed ones D3D writes, so a rule that drifts between the two shows up
 * as a pixel off by more than rounding. Then shadow maps, bump-env, and
 * the stage modes 8 and up (XBA-311: dot products in every dot mapping,
 * DOT_ST, the reflections and cube / volume lookups, dependent reads,
 * DOT_ZW's depth) against values worked out by hand, and random stage
 * programs, illegal ones included, that must compile.
 *
 * Needs a GL context (headless CGL on macOS, EGL elsewhere); without one the
 * GPU half is skipped (exit 77) after the text checks. */
#include "nv2a_gl_api.h"
#include "nv2a_gl_shader.h"
#include "nv2a_combiner.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); s_fail = 1; } } while (0)

static uint32_t s_rng = 12345u;
static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return s_rng;
}

/* An input byte: a readable register, alpha bit, any mapping. */
static uint32_t rnd_in(void)
{
    static const uint32_t regs[] = { 0, 1, 2, 3, 4, 5, 8, 9, 10, 11, 12, 13 };
    return regs[rnd() % 12] | (rnd() & 0x10) | ((rnd() % 8) << 5);
}

static uint32_t rnd_out_reg(void)
{
    static const uint32_t regs[] = { 0, 4, 5, 8, 9, 10, 11, 12, 13 };
    return regs[rnd() % 9];
}

static GLuint compile(GLenum type, const char *body)
{
    const char *src[2] = { nv2a_gl_glsl_header(NV2A_GL_ES), body };
    GLuint sh = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(sh, 2, src, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(sh, sizeof log, NULL, log);
        printf("compile failed: %s\n%s\n", log, body);
        return 0;
    }
    return sh;
}

static float q8(float x) { return (float)(int)(x * 255.0f + 0.5f) / 255.0f; }

static GLuint s_fbo;

/* ---- stage modes 8 and up (XBA-311) ------------------------------------ *
 * Every expected pixel here is worked out by hand from the rule in
 * nv2a_backend.h (dot_mapping), not by running the same formula again:
 * a 4x4 gradient texture (texel (x, y) is (40x + 20, 40y + 20, 0)) that
 * says where a lookup landed, cube faces and volume slices of one colour
 * each. The combiners pass one stage through (r0 = t<stage>). */

/* A stage's coordinates over the quad: base + dx * X + dy * Y, X and Y
 * running 0..1 across the 4x4 target (pixel x's centre at (x + 0.5) / 4). */
typedef struct { float base[4], dx[4], dy[4]; } StageTc;

static GLuint s_vs;

/* Draw `ps` (kinds as given, the textures bound by the caller) over the
 * 4x4 target, combiners passing t<out_st> through; pixels into got. */
static int draw_stage_case(Nv2aPixelState *ps, const uint32_t kinds[4], const StageTc tc[4],
                           int out_st, const float eye[3], const float zpos, uint8_t got[64])
{
    static char body[32768];
    Nv2aRenderState rs;
    Nv2aGlShaderKey k;
    Nv2aVertexFull quad[6];
    float tscale[8] = {1, 1, 1, 1, 1, 1, 1, 1}, vp[4] = {0.5f, 0.5f, 1.0f / 8388607.5f, 0};
    GLuint fs, prog;
    GLint ok = 0;
    unsigned i, st, j;

    ps->shade = NV2A_SHADE_COMBINERS;
    ps->rc.control = 1;
    ps->rc.color_icw[0] = (uint32_t)(0x08 + out_st) << 24 | 0x20u << 16;   /* A = t, B = 1 */
    ps->rc.color_ocw[0] = 0x0Cu << 4;                                     /* r0 = A*B */
    ps->rc.alpha_icw[0] = 0x30u << 24 | 0x30u << 16;                      /* 1 * 1 */
    ps->rc.alpha_ocw[0] = 0x0Cu << 4;
    memset(&rs, 0, sizeof rs);
    nv2a_gl_shader_key(ps, &rs, kinds, &k);
    if (!nv2a_gl_fragment_body(&k, body, sizeof body))
        return 0;
    fs = compile(GL_FRAGMENT_SHADER, body);
    if (!fs)
        return 0;
    prog = glCreateProgram();
    glAttachShader(prog, s_vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        glDeleteProgram(prog);
        glDeleteShader(fs);
        return 0;
    }
    glUseProgram(prog);
    for (i = 0; i < 4; i++) {
        char nm[8];
        snprintf(nm, sizeof nm, "u_t%u", i);
        glUniform1i(glGetUniformLocation(prog, nm), (GLint)i);
    }
    glUniform4fv(glGetUniformLocation(prog, "u_vp"), 1, vp);
    glUniform2fv(glGetUniformLocation(prog, "u_tscale"), 4, tscale);
    if (eye)
        glUniform3fv(glGetUniformLocation(prog, "u_eye"), 1, eye);
    for (i = 0; i < 6; i++) {
        static const float xy[6][2] = { {0,0}, {4,0}, {4,4}, {0,0}, {4,4}, {0,4} };
        float X = xy[i][0] / 4.0f, Y = xy[i][1] / 4.0f;
        memset(&quad[i], 0, sizeof quad[i]);
        quad[i].pos[0] = xy[i][0]; quad[i].pos[1] = xy[i][1];
        quad[i].pos[2] = zpos;
        quad[i].pos[3] = 1.0f;
        for (st = 0; st < 4; st++)
            for (j = 0; j < 4; j++)
                quad[i].tex[st][j] = tc[st].base[j] + tc[st].dx[j] * X + tc[st].dy[j] * Y;
    }
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glReadPixels(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, got);
    glDeleteProgram(prog);
    glDeleteShader(fs);
    return 1;
}

static void tex2d(GLuint t, int unit, int w, int h, const uint8_t *px)
{
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
}

/* One byte off is rounding; more is a different texel. */
static int px_is(const uint8_t *g, int r, int gg, int b)
{
    return abs(g[0] - r) <= 2 && abs(g[1] - gg) <= 2 && abs(g[2] - b) <= 2;
}

static int stage_mode_tests(void)
{
    static const uint8_t face[6][4] = {               /* +X -X +Y -Y +Z -Z */
        { 200, 0, 0, 255 }, { 0, 200, 0, 255 }, { 0, 0, 200, 255 },
        { 200, 200, 0, 255 }, { 0, 200, 200, 255 }, { 200, 0, 200, 255 },
    };
    uint8_t grad[64], got[64];
    GLuint tx[4], cube, vol;
    unsigned c, x, y, i;
    int bad = 0;

    for (y = 0; y < 4; y++)
        for (x = 0; x < 4; x++) {
            uint8_t *p = &grad[(y * 4 + x) * 4];
            p[0] = (uint8_t)(40 * x + 20); p[1] = (uint8_t)(40 * y + 20); p[2] = 0; p[3] = 255;
        }
    glGenTextures(4, tx);
    glGenTextures(1, &cube);
    glGenTextures(1, &vol);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_CUBE_MAP, cube);
    for (i = 0; i < 6; i++)
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, GL_RGBA8, 1, 1, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, face[i]);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, 0);

    /* 1. DOTPRODUCT then DOT_ST, every dot mapping. Stage 0 reads a 1x1
     *    "normal" N; each case's coordinates are chosen so that s, t, r
     *    dotted with map(N) is the coordinate c (X for stage 1, Y for
     *    stage 2), whatever else the components hold: the lookup lands on
     *    gradient texel (x, y) only if map(N) is what the rule says. */
    {
        static const struct {
            uint32_t map, sgn;            /* dot mapping; stage 0's signed channels */
            uint8_t n[4];                 /* N: r, g, b, a */
            float k[3], o[3];             /* coordinates: c * k + o */
        } cases[] = {
            { 0, 0, { 0x00, 0xFF, 0x00, 255 }, { 0, 1, 0 }, { 0.37f, 0, 0.2f } },   /* (0, 1, 0) */
            { 1, 0, { 0x80, 0xFF, 0x80, 255 }, { 0, 1, 0 }, { 0.37f, 0, 0.2f } },   /* (b-128)/127 */
            { 2, 0, { 0x7F, 0x80, 0x00, 255 }, { 1, 0, 0 }, { 0.25f, 0.25f, 0 } },  /* (1, -1, .) */
            { 3, 0, { 0x81, 0x00, 0x7F, 255 }, { 0, 0, 1 }, { 0.3f, 0.9f, 0.3f } }, /* (-1, 0, 1) */
            { 4, 0, { 0x12, 0xFF, 0x00, 255 }, { 1, 0, 0 }, { -0.25f, 0.6f, 0.25f } }, /* HILO (1, 0, 1) */
            { 5, 0, { 0x34, 0x80, 0x80, 255 }, { 0, 0, 1 }, { 0.4f, 0.7f, 0 } },   /* hemisphere (0, 0, 1) */
            { 7, 0, { 0x34, 0x00, 0x00, 255 }, { 0, 0, 1 }, { 0.4f, 0.7f, 0 } },   /* hemisphere (0, 0, 1) */
            /* The filter's signed channels: mapping 1 reads the bytes as
             * fetched (signed first, 0x80 would be -1.01); mapping 0 reads
             * them signed (0x7F is 1, not 0.5). */
            { 1, 0xE, { 0x80, 0xFF, 0x80, 255 }, { 0, 1, 0 }, { 0.37f, 0, 0.2f } },
            { 0, 0xE, { 0x00, 0x7F, 0x00, 255 }, { 0, 1, 0 }, { 0.37f, 0, 0.2f } },
        };
        const uint32_t kinds[4] = { NV2A_GL_TEX_2D, NV2A_GL_TEX_NONE, NV2A_GL_TEX_2D, NV2A_GL_TEX_NONE };
        tex2d(tx[2], 2, 4, 4, grad);
        for (c = 0; c < sizeof cases / sizeof cases[0]; c++) {
            Nv2aPixelState ps;
            StageTc tc[4];
            memset(&ps, 0, sizeof ps);
            memset(tc, 0, sizeof tc);
            tex2d(tx[0], 0, 1, 1, cases[c].n);
            ps.rc.stage_program = 0x1u | 0x11u << 5 | 0x09u << 10;
            ps.dot_mapping = cases[c].map | cases[c].map << 4;
            ps.stage[0].valid = 1;
            ps.stage[0].mode = 1;
            ps.stage[0].tex.filter = 0x01010000u | cases[c].sgn << 28;
            tc[0].base[0] = tc[0].base[1] = 0.5f;
            tc[0].base[3] = 1.0f;
            for (i = 0; i < 3; i++) {
                tc[1].base[i] = cases[c].o[i];
                tc[1].dx[i] = cases[c].k[i];
                tc[2].base[i] = cases[c].o[i];
                tc[2].dy[i] = cases[c].k[i];
            }
            tc[1].base[3] = tc[2].base[3] = 1.0f;
            if (!draw_stage_case(&ps, kinds, tc, 2, NULL, 0.0f, got)) {
                bad++;
                continue;
            }
            for (y = 0; y < 4; y++)
                for (x = 0; x < 4; x++)
                    if (!px_is(&got[(y * 4 + x) * 4], 40 * (int)x + 20, 40 * (int)y + 20, 0)) {
                        if (bad++ < 8)
                            printf("DOT_ST mapping %u case %u pixel %u,%u: got %d,%d\n",
                                   cases[c].map, c, x, y, got[(y * 4 + x) * 4],
                                   got[(y * 4 + x) * 4 + 1]);
                    }
        }
    }

    /* 2. Cube lookups on stage 3 (2 for DOT_RFLCT_DIFF), N = (0, 1, 0)
     *    with mapping 0, so each stage's dot is its t coordinate. */
    {
        static const struct {
            uint32_t prog;                /* stage modes 1..3 */
            float d[3], q[3];             /* each stage's dot (its t), q */
            int eye;                      /* u_eye = (1, 0, 0) */
            int out, face;                /* stage read, expected face */
        } cases[] = {
            { 0x11 | 0x11 << 5 | 0x0E << 10, { 1, 0.2f, -0.3f }, { 1, 1, 1 }, 0, 3, 0 },   /* STR_CUBE +X */
            { 0x11 | 0x11 << 5 | 0x0E << 10, { 0.2f, -1, 0.3f }, { 1, 1, 1 }, 0, 3, 3 },   /* -Y */
            { 0x11 | 0x11 << 5 | 0x0E << 10, { 0.1f, 0.2f, -1 }, { 1, 1, 1 }, 0, 3, 5 },   /* -Z */
            /* R = 2 N (N.E) / (N.N) - E: N (1.2, 1, 0), E (1, 0, 0) gives
             * (0.18, 0.98, 0), +Y (N alone is +X, -E -X). */
            { 0x11 | 0x11 << 5 | 0x0C << 10, { 1.2f, 1, 0 }, { 1, 0, 0 }, 0, 3, 2 },
            /* N (0, 0, 1), E (0.2, 0.5, 1): R (-0.2, -0.5, 1), +Z (-E is -Z). */
            { 0x11 | 0x11 << 5 | 0x0C << 10, { 0, 0, 1 }, { 0.2f, 0.5f, 1 }, 0, 3, 4 },
            /* CONST: E is u_eye (1, 0, 0), not the q (0, 0, 1, which would
             * give R = -E' = -Z). */
            { 0x11 | 0x11 << 5 | 0x12 << 10, { 1.2f, 1, 0 }, { 0, 0, 1 }, 1, 3, 2 },
            /* DOT_RFLCT_DIFF: stage 2 looks up (dot1, dot2, stage 3's dot). */
            { 0x11 | 0x0B << 5 | 0x0C << 10, { 0.2f, 0.3f, -1 }, { 1, 1, 1 }, 0, 2, 5 },
            { 0x11 | 0x0B << 5 | 0x0C << 10, { 0.2f, -1, 0.3f }, { 1, 1, 1 }, 0, 2, 3 },
        };
        static const uint8_t n[4] = { 0, 255, 0, 255 };
        static const float eye[3] = { 1, 0, 0 };
        for (c = 0; c < sizeof cases / sizeof cases[0]; c++) {
            Nv2aPixelState ps;
            StageTc tc[4];
            uint32_t kinds[4] = { NV2A_GL_TEX_2D, NV2A_GL_TEX_NONE, NV2A_GL_TEX_NONE, NV2A_GL_TEX_CUBE };
            memset(&ps, 0, sizeof ps);
            memset(tc, 0, sizeof tc);
            tex2d(tx[0], 0, 1, 1, n);
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_CUBE_MAP, cube);
            if (cases[c].out == 2)
                kinds[2] = NV2A_GL_TEX_CUBE;
            ps.rc.stage_program = 0x1u | cases[c].prog << 5;
            ps.stage[0].valid = 1;
            ps.stage[0].mode = 1;
            ps.stage[0].tex.filter = 0x01010000u;
            tc[0].base[0] = tc[0].base[1] = 0.5f;
            tc[0].base[3] = 1.0f;
            for (i = 1; i < 4; i++) {
                tc[i].base[0] = 0.77f;            /* times map(N).x = 0 */
                tc[i].base[1] = cases[c].d[i - 1];
                tc[i].base[2] = -0.4f;            /* times 0 */
                tc[i].base[3] = cases[c].q[i - 1];
            }
            if (!draw_stage_case(&ps, kinds, tc, cases[c].out, cases[c].eye ? eye : NULL,
                                 0.0f, got)) {
                bad++;
                continue;
            }
            for (i = 0; i < 16; i++) {
                const uint8_t *f = face[cases[c].face];
                if (!px_is(&got[i * 4], f[0], f[1], f[2])) {
                    if (bad++ < 8)
                        printf("cube case %u pixel %u: got %d,%d,%d want face %d\n", c, i,
                               got[i * 4], got[i * 4 + 1], got[i * 4 + 2], cases[c].face);
                    break;
                }
            }
        }
    }

    /* 3. DOT_STR_3D: a 1x1x2 volume, slice 0 red, slice 1 green, at
     *    r = dot3 = 0.25 and 0.75. */
    {
        static const uint8_t sl[8] = { 200, 0, 0, 255, 0, 200, 0, 255 };
        static const uint8_t n[4] = { 0, 255, 0, 255 };
        const uint32_t kinds[4] = { NV2A_GL_TEX_2D, NV2A_GL_TEX_NONE, NV2A_GL_TEX_NONE, NV2A_GL_TEX_3D };
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_3D, vol);
        glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, 1, 1, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, sl);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAX_LEVEL, 0);
        for (c = 0; c < 2; c++) {
            Nv2aPixelState ps;
            StageTc tc[4];
            memset(&ps, 0, sizeof ps);
            memset(tc, 0, sizeof tc);
            tex2d(tx[0], 0, 1, 1, n);
            ps.rc.stage_program = 0x1u | 0x11u << 5 | 0x11u << 10 | 0x0Du << 15;
            ps.stage[0].valid = 1;
            ps.stage[0].mode = 1;
            ps.stage[0].tex.filter = 0x01010000u;
            tc[0].base[0] = tc[0].base[1] = 0.5f;
            tc[0].base[3] = 1.0f;
            tc[1].base[1] = 0.5f;
            tc[2].base[1] = 0.5f;
            tc[3].base[1] = c ? 0.75f : 0.25f;
            if (!draw_stage_case(&ps, kinds, tc, 3, NULL, 0.0f, got)) {
                bad++;
                continue;
            }
            if (!px_is(&got[20], c ? 0 : 200, c ? 200 : 0, 0)) {
                printf("DOT_STR_3D slice %u: got %d,%d,%d\n", c, got[20], got[21], got[22]);
                bad++;
            }
        }
    }

    /* 4. Dependent reads: stage 1 samples the gradient at stage 0's
     *    (a, r) or (g, b). A 0x60 R 0xE0: texel (1, 3); G 0x20 B 0xA0:
     *    texel (0, 2). */
    {
        static const uint8_t n[4] = { 0xE0, 0x20, 0xA0, 0x60 };   /* r, g, b, a */
        const uint32_t kinds[4] = { NV2A_GL_TEX_2D, NV2A_GL_TEX_2D, NV2A_GL_TEX_NONE, NV2A_GL_TEX_NONE };
        for (c = 0; c < 2; c++) {
            Nv2aPixelState ps;
            StageTc tc[4];
            memset(&ps, 0, sizeof ps);
            memset(tc, 0, sizeof tc);
            tex2d(tx[0], 0, 1, 1, n);
            tex2d(tx[1], 1, 4, 4, grad);
            ps.rc.stage_program = 0x1u | (c ? 0x10u : 0x0Fu) << 5;
            ps.stage[0].valid = 1;
            ps.stage[0].mode = 1;
            ps.stage[0].tex.filter = 0x01010000u;
            tc[0].base[0] = tc[0].base[1] = 0.5f;
            tc[0].base[3] = 1.0f;
            if (!draw_stage_case(&ps, kinds, tc, 1, NULL, 0.0f, got)) {
                bad++;
                continue;
            }
            if (!px_is(&got[20], c ? 20 : 60, c ? 100 : 140, 0)) {
                printf("DPNDNT_%s: got %d,%d\n", c ? "GB" : "AR", got[20], got[21]);
                bad++;
            }
        }
    }

    /* 5. DOT_ZW: depth dot1 / dot2, dot2 = 1, dot1 = 2X: columns at depth
     *    0.25, 0.75, 1.25 and 1.75 -- the last two outside 0..1, dropped.
     *    Then a plain quad at depth 0.5 with LESS: it covers every column
     *    but the first. */
    {
        static const uint8_t n[4] = { 255, 0, 0, 255 };
        const uint32_t kinds[4] = { NV2A_GL_TEX_2D, NV2A_GL_TEX_NONE, NV2A_GL_TEX_NONE, NV2A_GL_TEX_NONE };
        GLuint zfbo, zrt, zrb;
        Nv2aPixelState ps;
        StageTc tc[4];
        uint8_t first[64];
        glGenTextures(1, &zrt);
        glBindTexture(GL_TEXTURE_2D, zrt);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glGenRenderbuffers(1, &zrb);
        glBindRenderbuffer(GL_RENDERBUFFER, zrb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, 4, 4);
        glGenFramebuffers(1, &zfbo);
        glBindFramebuffer(GL_FRAMEBUFFER, zfbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, zrt, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, zrb);
        CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
        glClearColor(0, 0, 1, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);   /* depth 1 */
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        memset(&ps, 0, sizeof ps);
        memset(tc, 0, sizeof tc);
        tex2d(tx[0], 0, 1, 1, n);                 /* N (1, 0, 0), mapping 0 */
        ps.rc.stage_program = 0x1u | 0x11u << 5 | 0x0Au << 10;
        ps.stage[0].valid = 1;
        ps.stage[0].mode = 1;
        ps.stage[0].tex.filter = 0x01010000u;
        tc[0].base[0] = tc[0].base[1] = 0.5f;
        tc[0].base[3] = 1.0f;
        tc[1].dx[0] = 2.0f;                       /* dot1 = 2X */
        tc[1].base[1] = 0.6f;                     /* times 0 */
        tc[2].base[0] = 1.0f;                     /* dot2 = 1 */
        tc[2].base[2] = 0.3f;
        /* stage 0 passed through: N, red (255, 0, 0). */
        if (!draw_stage_case(&ps, kinds, tc, 0, NULL, 0.0f, first)) {
            bad++;
        } else {
            memset(&ps, 0, sizeof ps);
            ps.rc.stage_program = 0x1u;
            tc[1].dx[0] = 0.0f;
            {
                static const uint8_t green[4] = { 0, 255, 0, 255 };
                tex2d(tx[0], 0, 1, 1, green);
            }
            /* Window depth z / 16777215: 0.5. */
            if (!draw_stage_case(&ps, kinds, tc, 0, NULL, 8388607.5f, got))
                bad++;
            for (x = 0; x < 4; x++) {
                const uint8_t *a = &first[(1 * 4 + x) * 4], *b = &got[(1 * 4 + x) * 4];
                int want_first_red = x < 2, want_red = x == 0;
                if (!px_is(a, want_first_red ? 255 : 0, 0, want_first_red ? 0 : 255)
                    || !px_is(b, want_red ? 255 : 0, want_red ? 0 : 255, 0)) {
                    printf("DOT_ZW column %u: first %d,%d,%d then %d,%d,%d\n", x,
                           a[0], a[1], a[2], b[0], b[1], b[2]);
                    bad++;
                }
            }
        }
        glDisable(GL_DEPTH_TEST);
        glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
        glDeleteFramebuffers(1, &zfbo);
        glDeleteRenderbuffers(1, &zrb);
        glDeleteTextures(1, &zrt);
    }

    /* 6. Every stage program compiles, illegal layouts included (a mode on
     *    a stage it is not valid on, inputs after the stage reading them,
     *    any dot mapping, any kind). */
    {
        static char body[32768];
        static const uint32_t kk[] = { NV2A_GL_TEX_NONE, NV2A_GL_TEX_2D, NV2A_GL_TEX_CUBE,
                                       NV2A_GL_TEX_3D };
        int fails = 0;
        for (c = 0; c < 150; c++) {
            Nv2aPixelState ps;
            Nv2aRenderState rs;
            Nv2aGlShaderKey k;
            uint32_t kinds[4];
            GLuint fs;
            memset(&ps, 0, sizeof ps);
            memset(&rs, 0, sizeof rs);
            ps.shade = NV2A_SHADE_COMBINERS;
            ps.rc.control = 1;
            for (i = 0; i < 4; i++) {
                uint32_t mode = rnd() % 0x14, kind = kk[rnd() % 4];
                /* Kinds as bind_stage_textures gives them: a cube only to
                 * a cube lookup, a volume only to PROJECT2D/3D, STR_3D. */
                if (kind == NV2A_GL_TEX_CUBE && mode != 3 && mode != 0x0B && mode != 0x0C
                    && mode != 0x0E && mode != 0x12)
                    kind = NV2A_GL_TEX_2D;
                if (kind == NV2A_GL_TEX_3D && mode != 1 && mode != 2 && mode != 0x0D)
                    kind = NV2A_GL_TEX_2D;
                ps.rc.stage_program |= mode << (i * 5);
                kinds[i] = kind;
                ps.stage[i].valid = 1;
                ps.stage[i].tex.filter = (rnd() & 0xF) << 28 | 0x01010000u;
            }
            ps.other_stage_input = rnd() & 0x00FF0000u;
            ps.dot_mapping = rnd() & 0xFFF;
            nv2a_gl_shader_key(&ps, &rs, kinds, &k);
            if (!nv2a_gl_fragment_body(&k, body, sizeof body)) {
                fails++;
                continue;
            }
            fs = compile(GL_FRAGMENT_SHADER, body);
            if (!fs) {
                if (fails++ < 2)
                    printf("stage program 0x%05X does not compile\n", ps.rc.stage_program);
                continue;
            }
            glDeleteShader(fs);
        }
        if (fails)
            printf("random stage programs: %d of 150 failed\n", fails);
        bad += fails;
    }

    glDeleteTextures(4, tx);
    glDeleteTextures(1, &cube);
    glDeleteTextures(1, &vol);
    printf("stage modes 8 and up: %s\n", bad ? "FAILED" : "ok");
    return bad;
}


int main(void)
{
    static char body[32768];
    Nv2aPixelState ps;
    Nv2aRenderState rs;
    Nv2aGlShaderKey k1, k2;
    const uint32_t kinds[4] = { NV2A_GL_TEX_2D, NV2A_GL_TEX_2D, NV2A_GL_TEX_2D, NV2A_GL_TEX_2D };
    GLuint vs, vao, vbo, fbo, rt, tex[4];
    Nv2aVertexFull quad[6];
    int prog_i, mism = 0, tested = 0;

    /* ---- text: keys are canonical, the body names what it should ------ */
    memset(&ps, 0, sizeof ps);
    memset(&rs, 0, sizeof rs);
    ps.shade = NV2A_SHADE_COMBINERS;
    ps.rc.control = 1;
    ps.rc.color_icw[0] = 0x08040000u;
    ps.rc.color_icw[5] = 0xDEADBEEFu;              /* beyond the stage count */
    ps.rc.factor0[0] = 0xFF00FF00u;                /* a constant: not in the key */
    nv2a_gl_shader_key(&ps, &rs, kinds, &k1);
    ps.rc.color_icw[5] = 0;
    ps.rc.factor0[0] = 0x12345678u;
    nv2a_gl_shader_key(&ps, &rs, kinds, &k2);
    CHECK(memcmp(&k1, &k2, sizeof k1) == 0);
    CHECK(nv2a_gl_shader_key_hash(&k1) == nv2a_gl_shader_key_hash(&k2));
    rs.alpha_test_enable = 1;
    rs.alpha_func = 0x207;                          /* ALWAYS: no test */
    nv2a_gl_shader_key(&ps, &rs, kinds, &k2);
    CHECK(k2.alpha_func == 0);
    rs.alpha_func = 0x204;
    nv2a_gl_shader_key(&ps, &rs, kinds, &k2);
    CHECK(k2.alpha_func == 0x204);
    CHECK(nv2a_gl_fragment_body(&k2, body, sizeof body) > 0);
    CHECK(strstr(body, "discard") != NULL);
    CHECK(strstr(body, "R8.rgb") != NULL);           /* A = t0 */
    CHECK(nv2a_gl_fragment_body(&k2, body, 16) == 0); /* too small: refused */
    rs.alpha_test_enable = 0;

    /* NV2A_GL_TEST_NO_GPU: CMake sets it for a cross build run under
     * qemu-user, where llvmpipe is too slow for this half. */
    if (getenv("NV2A_GL_TEST_NO_GPU") || !nv2a_gl_context_create()) {
        printf("no GL context: GPU comparison skipped\n");
        return s_fail ? 1 : 77;
    }

    /* ---- GPU: one 4x4 target, one quad, four 1x1 textures ------------- */
    vs = compile(GL_VERTEX_SHADER, nv2a_gl_vertex_body());
    CHECK(vs != 0);
    s_vs = vs;
    glGenTextures(1, &rt);
    glBindTexture(GL_TEXTURE_2D, rt);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
    s_fbo = fbo;
    CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    glViewport(0, 0, 4, 4);
    glGenTextures(4, tex);
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    {
        const GLsizei st = (GLsizei)sizeof(Nv2aVertexFull);
        int t;
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, st, (void *)offsetof(Nv2aVertexFull, pos));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, st, (void *)offsetof(Nv2aVertexFull, d0));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, st, (void *)offsetof(Nv2aVertexFull, d1));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, st, (void *)offsetof(Nv2aVertexFull, fog));
        for (t = 0; t < 4; t++) {
            glEnableVertexAttribArray((GLuint)(4 + t));
            glVertexAttribPointer((GLuint)(4 + t), 4, GL_FLOAT, GL_FALSE, st,
                                  (void *)(offsetof(Nv2aVertexFull, tex) + (size_t)t * 16));
        }
    }

    for (prog_i = 0; prog_i < 400; prog_i++) {
        float v0[4], v1[4], fog[4], t[4][4], want[4], c0[32], c1[32], fc[8], fogc[4];
        float tscale[8] = {1, 1, 1, 1, 1, 1, 1, 1}, vp[4] = {0.5f, 0.5f, 1.0f / 8388607.5f, 0};
        uint8_t texel[4][4], got[4];
        uint32_t s, n, i;
        GLuint fs, prog;
        GLint ok = 0;

        /* Every other program through the vertex shader's depth-clamp
         * fallback (GLES without GL_EXT_depth_clamp): z is in range, so
         * the pixels must not change. */
        vp[3] = (float)(prog_i & 1);
        memset(&ps, 0, sizeof ps);
        ps.shade = NV2A_SHADE_COMBINERS;
        n = 1 + rnd() % 8;
        ps.rc.control = n | ((rnd() & 0x110) << 8);  /* per-stage factors or not; no mux */
        ps.rc.stage_program = 0x1 | 0x1 << 5 | 0x1 << 10 | 0x1 << 15;
        for (s = 0; s < n; s++) {
            uint32_t fl = (rnd() & 0x3) | (rnd() & 0x38) | (rnd() & 0xC0);
            uint32_t afl = rnd() & 0x38;
            if ((fl & 0x38) == 0x28 || (fl & 0x38) == 0x38) fl &= ~0x38u;
            if ((afl & 0x38) == 0x28 || (afl & 0x38) == 0x38) afl = 0;
            ps.rc.color_icw[s] = rnd_in() << 24 | rnd_in() << 16 | rnd_in() << 8 | rnd_in();
            ps.rc.alpha_icw[s] = rnd_in() << 24 | rnd_in() << 16 | rnd_in() << 8 | rnd_in();
            ps.rc.color_ocw[s] = fl << 12 | rnd_out_reg() << 8 | rnd_out_reg() << 4 | rnd_out_reg();
            ps.rc.alpha_ocw[s] = afl << 12 | rnd_out_reg() << 8 | rnd_out_reg() << 4 | rnd_out_reg();
            ps.rc.factor0[s] = rnd();
            ps.rc.factor1[s] = rnd();
        }
        if (prog_i % 4 != 3) {                    /* and sometimes no final combiner */
            /* Final: A..D from the plain registers (mapping bits are not
             * used there beyond invert), E/F products, G alpha. */
            static const uint32_t fr[] = { 0, 1, 2, 3, 4, 5, 8, 9, 12, 13, 14, 15 };
            uint32_t a = fr[rnd() % 12] | (rnd() & 0x30), b = fr[rnd() % 12] | (rnd() & 0x30);
            uint32_t c = fr[rnd() % 12] | (rnd() & 0x30), d = fr[rnd() % 12] | (rnd() & 0x30);
            uint32_t e = fr[rnd() % 10] | (rnd() & 0x30), f = fr[rnd() % 10] | (rnd() & 0x30);
            uint32_t g = fr[rnd() % 10] | (rnd() & 0x30);
            ps.rc.final0 = a << 24 | b << 16 | c << 8 | d;
            ps.rc.final1 = e << 24 | f << 16 | g << 8 | (rnd() & 0xE0);
            ps.rc.final_c0 = rnd();
            ps.rc.final_c1 = rnd();
        }
        ps.fog_color = rnd();

        for (i = 0; i < 4; i++) {
            v0[i] = q8((float)(rnd() % 256) / 255.0f);
            v1[i] = q8((float)(rnd() % 256) / 255.0f);
        }
        fog[3] = q8((float)(rnd() % 256) / 255.0f);
        nv2a_rc_unpack(ps.fog_color, fogc);
        fog[0] = fogc[0]; fog[1] = fogc[1]; fog[2] = fogc[2];
        for (i = 0; i < 4; i++) {
            uint32_t c = rnd();
            nv2a_rc_unpack(c, t[i]);
            texel[i][0] = (uint8_t)(c >> 16); texel[i][1] = (uint8_t)(c >> 8);
            texel[i][2] = (uint8_t)c;         texel[i][3] = (uint8_t)(c >> 24);
            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, tex[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        }
        nv2a_rc_eval(&ps.rc, v0, v1, fog, (const float (*)[4])t, want);

        memset(&rs, 0, sizeof rs);
        nv2a_gl_shader_key(&ps, &rs, kinds, &k1);
        if (!nv2a_gl_fragment_body(&k1, body, sizeof body)) {
            CHECK(0);
            continue;
        }
        fs = compile(GL_FRAGMENT_SHADER, body);
        if (!fs) {
            s_fail = 1;
            break;
        }
        prog = glCreateProgram();
        glAttachShader(prog, vs);
        glAttachShader(prog, fs);
        glLinkProgram(prog);
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        CHECK(ok);
        glUseProgram(prog);
        for (i = 0; i < 4; i++) {
            char nm[8];
            snprintf(nm, sizeof nm, "u_t%u", i);
            glUniform1i(glGetUniformLocation(prog, nm), (GLint)i);
        }
        for (i = 0; i < 8; i++) {
            nv2a_rc_unpack(ps.rc.factor0[i], &c0[i * 4]);
            nv2a_rc_unpack(ps.rc.factor1[i], &c1[i * 4]);
        }
        nv2a_rc_unpack(ps.rc.final_c0, &fc[0]);
        nv2a_rc_unpack(ps.rc.final_c1, &fc[4]);
        glUniform4fv(glGetUniformLocation(prog, "u_vp"), 1, vp);
        glUniform2fv(glGetUniformLocation(prog, "u_tscale"), 4, tscale);
        glUniform4fv(glGetUniformLocation(prog, "u_c0"), 8, c0);
        glUniform4fv(glGetUniformLocation(prog, "u_c1"), 8, c1);
        glUniform4fv(glGetUniformLocation(prog, "u_fc0"), 1, &fc[0]);
        glUniform4fv(glGetUniformLocation(prog, "u_fc1"), 1, &fc[4]);
        glUniform4fv(glGetUniformLocation(prog, "u_fogc"), 1, fogc);

        /* Two triangles over the 4x4 target, every attribute constant. */
        for (i = 0; i < 6; i++) {
            static const float xy[6][2] = { {0,0}, {4,0}, {4,4}, {0,0}, {4,4}, {0,4} };
            memset(&quad[i], 0, sizeof quad[i]);
            quad[i].pos[0] = xy[i][0]; quad[i].pos[1] = xy[i][1];
            quad[i].pos[2] = 0.0f; quad[i].pos[3] = 1.0f;
            memcpy(quad[i].d0, v0, sizeof v0);
            memcpy(quad[i].d1, v1, sizeof v1);
            quad[i].fog = fog[3];
            quad[i].tex[0][0] = quad[i].tex[1][0] = 0.5f;
            quad[i].tex[2][0] = quad[i].tex[3][0] = 0.5f;
            quad[i].tex[0][1] = quad[i].tex[1][1] = 0.5f;
            quad[i].tex[2][1] = quad[i].tex[3][1] = 0.5f;
            quad[i].tex[0][3] = quad[i].tex[1][3] = 1.0f;
            quad[i].tex[2][3] = quad[i].tex[3][3] = 1.0f;
        }
        glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, got);
        tested++;
        for (i = 0; i < 4; i++) {
            int w = (int)(want[i] * 255.0f + 0.5f);
            if (abs(w - (int)got[i]) > 2) {
                if (mism < 5)
                    printf("program %d channel %u: CPU %d, GPU %d\n", prog_i, i, w, got[i]);
                mism++;
                break;
            }
        }
        glDeleteProgram(prog);
        glDeleteShader(fs);
    }
    CHECK(glGetError() == GL_NO_ERROR);
    printf("%d combiner programs compared, %d mismatched\n", tested, mism);

    /* ---- shadow maps: a depth texture read with a depth compare ------- *
     * Stage 0 PROJECT3D on a 2x1 D24S8 texture holding depths 0.25 and
     * 0.75 (Y16 range: 16384, 49151), the combiners passing t0 through,
     * r/q = 32768 (0.5). The generated shader leaves the comparison to the
     * sampler; the GL compare functions here are what bind_shadow sets for
     * the NV2A's "stored depth <func> r/q" (xemu psh_append_shadowmap), so
     * GL_GEQUAL is the title's LEQUAL. PROJECT2D compares 0. */
    {
        static const uint32_t depth[2] = { 0x40000000u, 0xC0000000u };   /* D24 in the top bits */
        static const struct { uint32_t func, mode; uint8_t left, right; } cases[] = {
            { 3, 2, 0, 255 },   /* GL LEQUAL (title GEQUAL): 0.5 <= 0.25 no, <= 0.75 yes */
            { 4, 2, 255, 0 },   /* GL GREATER (title LESS) */
            { 0, 2, 0, 0 },     /* NEVER */
            { 7, 2, 255, 255 }, /* ALWAYS */
            { 3, 1, 255, 255 }, /* PROJECT2D: 0 <= both */
        };
        const uint32_t skinds[4] = { NV2A_GL_TEX_SHADOW16, 0, 0, 0 };
        float tscale[8] = {0.5f, 1, 1, 1, 1, 1, 1, 1}, vp[4] = {0.5f, 0.5f, 1.0f / 8388607.5f, 0};
        GLuint dtex, fs, prog;
        unsigned c, i;

        glGenTextures(1, &dtex);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, dtex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, 2, 1, 0, GL_DEPTH_STENCIL,
                     GL_UNSIGNED_INT_24_8, depth);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        for (c = 0; c < sizeof cases / sizeof cases[0]; c++) {
            uint8_t got[4 * 4 * 4];
            GLint ok = 0;
            memset(&ps, 0, sizeof ps);
            ps.shade = NV2A_SHADE_COMBINERS;
            ps.rc.control = 1;
            ps.rc.stage_program = cases[c].mode;
            /* stage 0: A = t0, B = 1 (zero inverted) -> r0 = t0 */
            ps.rc.color_icw[0] = 0x08u << 24 | 0x20u << 16;
            ps.rc.color_ocw[0] = 0x0Cu << 4;
            memset(&rs, 0, sizeof rs);
            nv2a_gl_shader_key(&ps, &rs, skinds, &k1);
            CHECK(nv2a_gl_fragment_body(&k1, body, sizeof body) > 0);
            CHECK(strstr(body, "sampler2DShadow u_t0") != NULL);
            fs = compile(GL_FRAGMENT_SHADER, body);
            CHECK(fs != 0);
            if (!fs)
                break;
            prog = glCreateProgram();
            glAttachShader(prog, vs);
            glAttachShader(prog, fs);
            glLinkProgram(prog);
            glGetProgramiv(prog, GL_LINK_STATUS, &ok);
            CHECK(ok);
            glUseProgram(prog);
            for (i = 0; i < 4; i++) {
                char nm[8];
                snprintf(nm, sizeof nm, "u_t%u", i);
                glUniform1i(glGetUniformLocation(prog, nm), (GLint)i);
            }
            glUniform4fv(glGetUniformLocation(prog, "u_vp"), 1, vp);
            glUniform2fv(glGetUniformLocation(prog, "u_tscale"), 4, tscale);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, (GLint)(GL_NEVER + cases[c].func));
            /* Texels 0..2 across the target (texel coordinates times
             * tscale 1/2), r = 32768 * q with q = 2 (projective). */
            for (i = 0; i < 6; i++) {
                static const float xy[6][2] = { {0,0}, {4,0}, {4,4}, {0,0}, {4,4}, {0,4} };
                memset(&quad[i], 0, sizeof quad[i]);
                quad[i].pos[0] = xy[i][0]; quad[i].pos[1] = xy[i][1];
                quad[i].pos[3] = 1.0f;
                quad[i].tex[0][0] = xy[i][0] / 2.0f * 2.0f;
                quad[i].tex[0][1] = 0.5f * 2.0f;
                quad[i].tex[0][2] = 32768.0f * 2.0f;
                quad[i].tex[0][3] = 2.0f;
            }
            glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STREAM_DRAW);
            glDrawArrays(GL_TRIANGLES, 0, 6);
            glReadPixels(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, got);
            /* Column 0 samples texel 0, column 3 texel 1. */
            if (got[1 * 16 + 0] != cases[c].left || got[1 * 16 + 12] != cases[c].right) {
                printf("shadow case %u: got %d/%d, want %d/%d\n", c, got[16], got[28],
                       cases[c].left, cases[c].right);
                s_fail = 1;
            }
            glDeleteProgram(prog);
            glDeleteShader(fs);
        }
        glDeleteTextures(1, &dtex);
        CHECK(glGetError() == GL_NO_ERROR);
        printf("shadow-map compares: %s\n", s_fail ? "FAILED" : "ok");
    }
    /* ---- bump-env (stage modes 6 and 7) -------------------------------- *
     * Stage 0 (PROJECT2D) samples a dU/dV/L texture, stage 1 a 4x4 colour
     * texture whose texel (x, y) is (40x + 20, 40y + 20, 0) at its own
     * coordinates plus M (dU, dV); the combiners pass t1 through. dU, dV
     * are the blue and green bytes as signed (0x7F = +1, 0x81 = -1), the
     * matrix m00, m01, m10, m11 in u_bm, D = (m00 dU + m10 dV,
     * m01 dU + m11 dV) (xemu psh.c), so 0.25 is one texel. */
    {
        static const struct {
            uint32_t mode;
            uint8_t b, g, r;                  /* dsdt texel: dU, dV, L */
            float m[4], scale, offset;
            int dx, dy;                       /* expected texel shift */
            float lum;                        /* expected colour factor */
            int lin;                          /* 2x1 bilinear source case */
        } cases[] = {
            { 6, 0x7F, 0x00, 0, { 0.25f, 0, 0, 0.25f }, 0, 0,  1,  0, 1.0f, 0 },
            { 6, 0x00, 0x81, 0, { 0.25f, 0, 0, 0.25f }, 0, 0,  0, -1, 1.0f, 0 },
            { 6, 0x7F, 0x00, 0, { 0, 0.25f, 0, 0 },     0, 0,  0,  1, 1.0f, 0 },  /* m01: dU moves v */
            { 6, 0x00, 0x7F, 0, { 0, 0, 0.5f, 0 },      0, 0,  2,  0, 1.0f, 0 },  /* m10: dV moves u */
            { 7, 0x81, 0x00, 0x80, { 0.25f, 0, 0, 0.25f }, 0.5f, 0.25f, -1, 0,
              0.5f * 128.0f / 255.0f + 0.25f, 0 },
            /* dU +2/127 and -2/127 side by side, sampled half way with a
             * bilinear filter: signed per texel they cancel (no shift);
             * blended as unsigned bytes first they would read 0x80, -1. */
            { 6, 0x02, 0x00, 0, { 0.25f * 127.0f / 2.0f, 0, 0, 0 }, 0, 0, 0, 0, 1.0f, 1 },
        };
        const uint32_t bkinds[4] = { NV2A_GL_TEX_2D, NV2A_GL_TEX_2D, 0, 0 };
        float tscale[8] = {1, 1, 1, 1, 1, 1, 1, 1}, vp[4] = {0.5f, 0.5f, 1.0f / 8388607.5f, 0};
        uint8_t col[4 * 4 * 4];
        GLuint btex[2], fs, prog;
        unsigned c, i, x, y;
        int bump_bad = 0;

        for (y = 0; y < 4; y++)
            for (x = 0; x < 4; x++) {
                uint8_t *p = &col[(y * 4 + x) * 4];
                p[0] = (uint8_t)(40 * x + 20); p[1] = (uint8_t)(40 * y + 20);
                p[2] = 0; p[3] = 255;
            }
        glGenTextures(2, btex);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, btex[1]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, col);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        for (c = 0; c < sizeof cases / sizeof cases[0]; c++) {
            uint8_t ds[4 * 4 * 4], got[4 * 4 * 4];
            float bm[16] = {0}, bso[8] = {0};
            GLint ok = 0;
            int dw = cases[c].lin ? 2 : 4, dh = cases[c].lin ? 1 : 4;
            for (i = 0; i < (unsigned)(dw * dh); i++) {
                ds[i * 4 + 0] = cases[c].r; ds[i * 4 + 1] = cases[c].g;
                ds[i * 4 + 2] = cases[c].b; ds[i * 4 + 3] = 255;
            }
            if (cases[c].lin)
                ds[4 + 2] = (uint8_t)(0x100 - cases[c].b);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, btex[0]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, dw, dh, 0, GL_RGBA, GL_UNSIGNED_BYTE, ds);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, cases[c].lin ? GL_LINEAR : GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, cases[c].lin ? GL_LINEAR : GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);

            memset(&ps, 0, sizeof ps);
            ps.shade = NV2A_SHADE_COMBINERS;
            ps.rc.control = 1;
            ps.rc.stage_program = 1u | cases[c].mode << 5;
            ps.rc.color_icw[0] = 0x09u << 24 | 0x20u << 16;    /* A = t1, B = 1 */
            ps.rc.color_ocw[0] = 0x0Cu << 4;                   /* r0 = A*B */
            ps.stage[0].valid = 1;
            ps.stage[0].mode = 1;
            ps.stage[0].tex.color = 0x06;
            ps.stage[0].tex.filter = cases[c].lin ? 0x02020000u : 0x01010000u;
            ps.stage[0].tex.addr_u = cases[c].lin ? 3 : 1;
            ps.stage[0].tex.addr_v = cases[c].lin ? 3 : 1;
            memset(&rs, 0, sizeof rs);
            nv2a_gl_shader_key(&ps, &rs, bkinds, &k1);
            CHECK(k1.bump_src[1] == 0x10);
            CHECK((k1.bump_fetch[0] & NV2A_GL_DSDT_TEXEL) != 0);
            CHECK(nv2a_gl_fragment_body(&k1, body, sizeof body) > 0);
            fs = compile(GL_FRAGMENT_SHADER, body);
            CHECK(fs != 0);
            if (!fs)
                break;
            prog = glCreateProgram();
            glAttachShader(prog, vs);
            glAttachShader(prog, fs);
            glLinkProgram(prog);
            glGetProgramiv(prog, GL_LINK_STATUS, &ok);
            CHECK(ok);
            glUseProgram(prog);
            for (i = 0; i < 4; i++) {
                char nm[8];
                snprintf(nm, sizeof nm, "u_t%u", i);
                glUniform1i(glGetUniformLocation(prog, nm), (GLint)i);
            }
            memcpy(&bm[4], cases[c].m, sizeof cases[c].m);
            bso[2] = cases[c].scale;
            bso[3] = cases[c].offset;
            glUniform4fv(glGetUniformLocation(prog, "u_vp"), 1, vp);
            glUniform2fv(glGetUniformLocation(prog, "u_tscale"), 4, tscale);
            glUniform4fv(glGetUniformLocation(prog, "u_bm"), 4, bm);
            glUniform2fv(glGetUniformLocation(prog, "u_bso"), 4, bso);
            /* Coordinates 0..1 over the 4x4 target: pixel (x, y) at texel
             * centre (x, y) of the colour texture; the 2x1 source is read
             * at its centre (half way between its texels). */
            for (i = 0; i < 6; i++) {
                static const float xy[6][2] = { {0,0}, {4,0}, {4,4}, {0,0}, {4,4}, {0,4} };
                memset(&quad[i], 0, sizeof quad[i]);
                quad[i].pos[0] = xy[i][0]; quad[i].pos[1] = xy[i][1];
                quad[i].pos[3] = 1.0f;
                quad[i].tex[0][0] = cases[c].lin ? 0.5f : xy[i][0] / 4.0f;
                quad[i].tex[0][1] = cases[c].lin ? 0.5f : xy[i][1] / 4.0f;
                quad[i].tex[0][3] = 1.0f;
                quad[i].tex[1][0] = xy[i][0] / 4.0f;
                quad[i].tex[1][1] = xy[i][1] / 4.0f;
                quad[i].tex[1][3] = 1.0f;
            }
            glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STREAM_DRAW);
            glDrawArrays(GL_TRIANGLES, 0, 6);
            glReadPixels(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, got);
            for (y = 0; y < 4; y++)
                for (x = 0; x < 4; x++) {
                    unsigned tx = (x + 4 + (unsigned)cases[c].dx) & 3;
                    unsigned ty = (y + 4 + (unsigned)cases[c].dy) & 3;
                    int wr = (int)((float)(40 * tx + 20) * cases[c].lum + 0.5f);
                    int wg = (int)((float)(40 * ty + 20) * cases[c].lum + 0.5f);
                    const uint8_t *g = &got[(y * 4 + x) * 4];
                    if (abs(g[0] - wr) > 2 || abs(g[1] - wg) > 2) {
                        if (bump_bad++ < 6)
                            printf("bump case %u pixel %u,%u: got %d,%d want %d,%d\n",
                                   c, x, y, g[0], g[1], wr, wg);
                    }
                }
            glDeleteProgram(prog);
            glDeleteShader(fs);
        }
        glDeleteTextures(2, btex);
        CHECK(glGetError() == GL_NO_ERROR);
        CHECK(bump_bad == 0);
        printf("bump-env stages: %s\n", bump_bad ? "FAILED" : "ok");
    }
    CHECK(stage_mode_tests() == 0);
    CHECK(glGetError() == GL_NO_ERROR);
    CHECK(tested == 400);
    CHECK(mism == 0);
    if (!s_fail)
        printf("nv2a GL combiner shaders match the CPU combiners: ok\n");
    return s_fail;
}
