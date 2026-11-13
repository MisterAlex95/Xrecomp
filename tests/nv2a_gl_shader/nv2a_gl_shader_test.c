/* The GL back end's generated combiner shaders against the CPU combiners.
 *
 * nv2a_gl_shader.c transcribes nv2a_rc_eval() into GLSL. This draws one
 * flat quad per combiner program -- vertex colours, fog and four 1x1
 * textures as the inputs -- reads the pixel back and compares it with what
 * nv2a_rc_eval() computes from the same inputs. Programs are random (every
 * input mapping, output scale/bias, dot products, the final combiner) plus a
 * few fixed ones D3D writes, so a rule that drifts between the two shows up
 * as a pixel off by more than rounding.
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
    glGenTextures(1, &rt);
    glBindTexture(GL_TEXTURE_2D, rt);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
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
    CHECK(tested == 400);
    CHECK(mism == 0);
    if (!s_fail)
        printf("nv2a GL combiner shaders match the CPU combiners: ok\n");
    return s_fail;
}
