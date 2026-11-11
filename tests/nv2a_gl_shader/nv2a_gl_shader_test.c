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

    if (!nv2a_gl_context_create()) {
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
    CHECK(tested == 400);
    CHECK(mism == 0);
    if (!s_fail)
        printf("nv2a GL combiner shaders match the CPU combiners: ok\n");
    return s_fail;
}
