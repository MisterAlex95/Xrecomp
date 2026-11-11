/* Vertex programs through the GLSL translator (src/gpu/nv2a_gl_vsh.c) against
 * the CPU interpreter (src/kernel/nv2a_vsh_interp.c).
 *
 * Text checks first (no GL needed). Then, with a GL context:
 *   - edge cases: every ILU op on special values (0, -0, tiny, huge, pow(0, p)
 *     and LIT's power clamp), a0-relative reads in and out of c[];
 *   - 600 random programs (every MAC and ILU op, every source mux, swizzles,
 *     negates, a0-relative constants, MAC/ILU pairs, every output register)
 *     on 64 random vertices each.
 * Each program runs once through the interpreter and once as a GLSL vertex
 * shader whose raw outputs come back by transform feedback; every output
 * component must agree within a tolerance (infinities must match exactly; a
 * NaN reference is not compared). The GPU's float maths is not the CPU's to
 * the last bit, and a few ops are discontinuous (SLT/SGE, ARL's floor, EXP,
 * LOG's exponent, LIT's x > 0), so up to 1 % of random vertices may differ.
 *
 * Then the test proves it would notice a translator bug: a handful of
 * deliberate mutations of the generated text (DPH without + B.w, a0 off by
 * one, RCC's clamp, MIN as MAX, the colour clamp dropped, R1 writes going
 * elsewhere, SGE as SLT, RSQ without abs) must each make the comparison fail.
 *
 * Exit 77 without a GL context. */
#include "../../src/kernel/nv2a_vsh_interp.c"

#include "nv2a_gl_api.h"
#include "nv2a_gl_shader.h"
#include "nv2a_gl_vsh.h"

#include <stddef.h>

static int s_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); s_fail++; } } while (0)

static uint32_t s_rng = 0xC0FFEEu;

static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static float rndf(void)
{
    return ((float)(rnd() & 0xFFFF) / 65535.0f - 0.5f) * 8.0f;
}

/* ---- encoder (field table in nv2a_vsh_interp.c) ------------------------ */

typedef struct { uint32_t mux, reg, swz, neg; } Src;   /* mux 1 R, 2 v, 3 c */

#define SWZ_ID 0x1Bu

static void encode(uint32_t w[4], uint32_t mac, uint32_t ilu, Src a, Src b, Src c,
                   uint32_t cidx, uint32_t vidx, uint32_t mac_mask, uint32_t ilu_mask,
                   uint32_t tdst, uint32_t omask, uint32_t to_o, uint32_t oaddr,
                   uint32_t from_ilu, uint32_t rel, uint32_t final)
{
    w[0] = 0;
    w[1] = ilu << 25 | mac << 21 | cidx << 13 | vidx << 9 | a.neg << 8 | a.swz;
    w[2] = a.reg << 28 | a.mux << 26 | b.neg << 25 | b.swz << 17 | b.reg << 13
         | b.mux << 11 | c.neg << 10 | c.swz << 2 | (c.reg >> 2);
    w[3] = (c.reg & 3u) << 30 | c.mux << 28 | mac_mask << 24 | tdst << 20
         | ilu_mask << 16 | omask << 12 | to_o << 11 | oaddr << 3 | from_ilu << 2
         | rel << 1 | final;
}

static void random_ins(uint32_t w[4], int final)
{
    w[0] = 0;
    w[1] = rnd() & 0x0FFFFFFFu;
    w[2] = rnd();
    w[3] = rnd() & ~1u;
    /* a0-relative bases mostly inside c[]. */
    w[1] = (w[1] & ~(0xFFu << 13)) | ((rnd() % 100u) << 13);
    /* Outputs: mostly to o[], mostly to registers the rasteriser reads. */
    if (rnd() % 8)
        w[3] = (w[3] & ~(0xFFu << 3)) | ((rnd() % 13u) << 3) | (1u << 11);
    if (final)
        w[3] |= 1u;
}

/* ---- GL harness ---------------------------------------------------------- */

#define NV 64                           /* vertices per program */

static GLuint s_vao, s_vbo, s_tfb, s_fs;
static char s_body[131072], s_mut[131072];

static GLuint compile(GLenum type, const char *body)
{
    const char *src[2];
    GLuint sh = glCreateShader(type);
    GLint ok = 0;

    src[0] = nv2a_gl_glsl_header(NV2A_GL_ES);
    src[1] = body;
    glShaderSource(sh, 2, src, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(sh, sizeof log, NULL, log);
        printf("compile failed:\n%s\n--- source ---\n%s\n", log, body);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

/* Replace every `from` in s_body by `to`, into s_mut. Returns the count. */
static int mutate(const char *from, const char *to)
{
    const char *p = s_body, *q;
    size_t n = 0, lf = strlen(from), lt = strlen(to);
    int hits = 0;

    while ((q = strstr(p, from)) != NULL && n + (size_t)(q - p) + lt + 1 < sizeof s_mut) {
        memcpy(s_mut + n, p, (size_t)(q - p));
        n += (size_t)(q - p);
        memcpy(s_mut + n, to, lt);
        n += lt;
        p = q + lf;
        hits++;
    }
    strncpy(s_mut + n, p, sizeof s_mut - n - 1);
    s_mut[sizeof s_mut - 1] = 0;
    return hits;
}

/* Run `body` on in[NV][16][4]; outputs in out[NV] (Nv2aVshOutput layout:
 * pos, d0, d1, fog, t0..t3 -- the capture order). */
static int gpu_run(const char *body, const float (*in)[16][4], Nv2aVshOutput *out, int nv)
{
    GLuint vs = compile(GL_VERTEX_SHADER, body), prog;
    GLint ok = 0;
    uint32_t i;
    void *map;

    if (!vs)
        return 0;
    prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, s_fs);
    glTransformFeedbackVaryings(prog, 8, nv2a_gl_vsh_capture_names, GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(prog, sizeof log, NULL, log);
        printf("link failed: %s\n", log);
        glDeleteProgram(prog);
        return 0;
    }
    glUseProgram(prog);
    for (i = 0; i < NV2A_VSH_CONSTANTS; i++) {
        char name[16];
        GLint loc;
        snprintf(name, sizeof name, "c[%u]", i);
        loc = glGetUniformLocation(prog, name);
        if (loc >= 0)
            glUniform4fv(loc, 1, s_const[i]);
    }
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(nv * 16 * 4 * sizeof(float)), in, GL_STREAM_DRAW);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, s_tfb);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, (GLsizeiptr)(nv * sizeof(Nv2aVshOutput)),
                 NULL, GL_STREAM_READ);
    glEnable(GL_RASTERIZER_DISCARD);
    glBeginTransformFeedback(GL_POINTS);
    glDrawArrays(GL_POINTS, 0, nv);
    glEndTransformFeedback();
    glDisable(GL_RASTERIZER_DISCARD);
    map = glMapBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0,
                           (GLsizeiptr)(nv * sizeof(Nv2aVshOutput)), GL_MAP_READ_BIT);
    {
        GLenum e = glGetError();
        if (e != GL_NO_ERROR) {
            printf("FAIL: GL error 0x%X\n", e);
            s_fail++;
        }
    }
    if (map) {
        memcpy(out, map, (size_t)nv * sizeof(Nv2aVshOutput));
        glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER);
    }
    glDeleteProgram(prog);
    return map != NULL;
}

static unsigned long s_cmp, s_nan_skipped;

static int close_enough(float r, float g)
{
    s_cmp++;
    if (r != r) {
        s_nan_skipped++;
        return 1;
    }
    if (isinf(r))
        return g == r;
    if (!isfinite(g))
        return 0;
    return fabsf(g - r) <= 2e-3f * (1.0f + fabsf(r));
}

/* Compare nv vertices; returns how many differ. `show` prints the first. */
static int compare(const Nv2aVshOutput *ref, const Nv2aVshOutput *got, int nv, int show,
                   const char *what)
{
    static const char *const names[8] = { "pos", "d0", "d1", "fog", "t0", "t1", "t2", "t3" };
    int v, k, bad = 0;

    for (v = 0; v < nv; v++) {
        const float *r = (const float *)&ref[v], *g = (const float *)&got[v];
        for (k = 0; k < 32; k++)
            if (!close_enough(r[k], g[k])) {
                if (show-- > 0)
                    printf("  %s: vertex %d %s.%c: interpreter %.9g, GLSL %.9g\n", what, v,
                           names[k / 4], "xyzw"[k % 4], r[k], g[k]);
                bad++;
                break;
            }
    }
    return bad;
}

/* Load `len` instructions at slot 0, translate (CAPTURE), and run nv
 * vertices through both. Fills ref/got; returns 1 if both ran. */
static int run_both(const uint32_t (*prog)[4], uint32_t len, const float (*in)[16][4], int nv,
                    Nv2aVshOutput *ref, Nv2aVshOutput *got, int need_gpu)
{
    Nv2aGlVshInfo info;
    uint32_t i;
    int v;

    for (i = 0; i < len; i++)
        nv2a_vsh_set_instruction(i, prog[i]);
    nv2a_vsh_set_start_slot(0);
    for (v = 0; v < nv; v++)
        if (!vsh_run_decoded(in[v], &ref[v]))
            return 0;
    if (!nv2a_gl_vsh_body(prog, len, NV2A_GL_VSH_CAPTURE, s_body, sizeof s_body, &info)) {
        printf("FAIL: translation refused a program of %u instructions\n", len);
        s_fail++;
        return 0;
    }
    if (!need_gpu)
        return 1;
    return gpu_run(s_body, in, got, nv);
}

/* ---- random programs ----------------------------------------------------- */

#define NPROG 600
#define MAXLEN 24
static uint32_t s_progs[NPROG][MAXLEN][4];
static uint32_t s_lens[NPROG];
static float s_in[NPROG][NV][16][4];

static void make_programs(void)
{
    uint32_t p, i;
    int v, a, k;

    for (p = 0; p < NPROG; p++) {
        s_lens[p] = 1 + rnd() % MAXLEN;
        for (i = 0; i < s_lens[p]; i++)
            random_ins(s_progs[p][i], i + 1 == s_lens[p]);
        for (v = 0; v < NV; v++)
            for (a = 0; a < 16; a++)
                for (k = 0; k < 4; k++)
                    s_in[p][v][a][k] = rndf();
    }
}

/* All random programs through the GL path with `from` -> `to` applied to the
 * generated text (NULL: as generated). Returns differing vertices; *ran how
 * many programs the mutation touched. */
static long run_random(const char *from, const char *to, uint32_t nprog, int show,
                       int *touched)
{
    static Nv2aVshOutput ref[NV], got[NV];
    long bad = 0;
    uint32_t p;

    *touched = 0;
    for (p = 0; p < nprog; p++) {
        if (!run_both((const uint32_t (*)[4])s_progs[p], s_lens[p],
                      (const float (*)[16][4])s_in[p], NV, ref, got, 0))
            continue;
        if (from) {
            if (!mutate(from, to))
                continue;
            memcpy(s_body, s_mut, sizeof s_body);
        }
        if (!gpu_run(s_body, (const float (*)[16][4])s_in[p], got, NV)) {
            s_fail++;
            continue;
        }
        (*touched)++;
        bad += compare(ref, got, NV, show > 0 ? 1 : 0, "random");
        if (show > 0 && bad)
            show--;
    }
    return bad;
}

int main(void)
{
    Nv2aGlVshInfo info;
    uint32_t w[2][4], i;
    Src rv0 = { 2, 0, SWZ_ID, 0 }, rr0 = { 1, 0, SWZ_ID, 0 }, rc = { 3, 0, SWZ_ID, 0 };
    float fu[4];

    for (i = 0; i < NV2A_VSH_CONSTANTS; i++) {
        float c[4] = { rndf(), rndf(), rndf(), rndf() };
        nv2a_vsh_set_constant(i, c);
    }

    /* ---- text --------------------------------------------------------- */
    /* DP4 o[0] = v3 . c[100], FINAL. */
    {
        Src a = { 2, 0, SWZ_ID, 0 }, b = { 3, 0, SWZ_ID, 0 };
        encode(w[0], 7, 0, a, b, rr0, 100, 3, 0, 0, 0, 0xF, 1, 0, 0, 0, 1);
    }
    CHECK(nv2a_gl_vsh_body((const uint32_t (*)[4])w, 1, NV2A_GL_VSH_RENDER, s_body,
                           sizeof s_body, &info) > 0);
    CHECK(info.inputs == (1u << 3) && info.count == 1 && !info.relative);
    CHECK(strstr(s_body, "layout(location = 3) in vec4 v3;") != NULL);
    CHECK(strstr(s_body, "dot(A, B)") != NULL);
    CHECK(strstr(s_body, "c[100]") != NULL);
    CHECK(strstr(s_body, "v_fog = vsh_fog(o_fog.x)") != NULL);
    CHECK(strstr(s_body, "gl_ClipDistance") == NULL);
    CHECK(nv2a_gl_vsh_body((const uint32_t (*)[4])w, 1,
                           NV2A_GL_VSH_RENDER | NV2A_GL_VSH_CLIP_DISTANCE, s_body,
                           sizeof s_body, &info) > 0);
    CHECK(strstr(s_body, "gl_ClipDistance[0]") != NULL);
    CHECK(nv2a_gl_vsh_body((const uint32_t (*)[4])w, 1, 0, s_body, 64, &info) == 0);
    w[0][3] &= ~1u;                                   /* no FINAL: refused */
    CHECK(nv2a_gl_vsh_body((const uint32_t (*)[4])w, 1, 0, s_body, sizeof s_body, &info) == 0);
    nv2a_gl_vsh_fog_uniform(1, 0x803, 0.5f, 2.0f, fu);
    CHECK(fu[0] == 2.0f && fu[1] == 0.5f && fu[2] == 2.0f && fu[3] == 2.0f);
    nv2a_gl_vsh_fog_uniform(0, 0x800, 0.5f, 2.0f, fu);
    CHECK(fu[0] == 1.0f && fu[3] == 0.0f);
    /* The interpreter's program accessor sees the same words. */
    {
        const uint32_t (*pi)[4];
        uint64_t h1, h2;
        uint32_t inputs;
        w[0][3] |= 1u;
        nv2a_vsh_set_instruction(5, w[0]);
        nv2a_vsh_set_start_slot(5);
        CHECK(nv2a_vsh_program(&pi, &h1, &inputs) == 1);
        CHECK(memcmp(pi[0], w[0], 16) == 0 && inputs == (1u << 3));
        nv2a_vsh_set_instruction(9, w[0]);
        nv2a_vsh_set_start_slot(9);
        CHECK(nv2a_vsh_program(NULL, &h2, NULL) == 1 && h1 == h2);
        CHECK(!nv2a_vsh_program_writes_constants());
    }
    /* Constant stamps move with every kind of write. */
    {
        uint64_t s0, s1;
        const uint64_t *st = nv2a_vsh_constant_stamps(&s0);
        float c[4] = { 1, 2, 3, 4 };
        nv2a_vsh_set_constant(7, c);
        nv2a_vsh_constant_stamps(&s1);
        CHECK(s1 > s0 && st[7] == s1 && st[8] <= s0);
        nv2a_vsh_constant_component(59, 2, 0);
        CHECK(st[59] > s1);
        nv2a_vsh_set_constant_load(120);
        nv2a_vsh_constant_word(0);
        CHECK(st[120] > st[59]);
    }

    if (!nv2a_gl_context_create()) {
        printf("no GL context: GPU comparison skipped\n");
        return s_fail ? 1 : 77;
    }
    printf("GL: %s | %s\n", (const char *)glGetString(GL_RENDERER),
           (const char *)glGetString(GL_VERSION));
    s_fs = compile(GL_FRAGMENT_SHADER, "out vec4 o_c;\nvoid main() { o_c = vec4(1.0); }\n");
    CHECK(s_fs != 0);
    /* A headless context has no complete default framebuffer, and a draw
     * needs one even with the rasteriser off. */
    {
        GLuint rt, fbo;
        glGenTextures(1, &rt);
        glBindTexture(GL_TEXTURE_2D, rt);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
        CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    }
    glGenVertexArrays(1, &s_vao);
    glBindVertexArray(s_vao);
    glGenBuffers(1, &s_vbo);
    glGenBuffers(1, &s_tfb);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    for (i = 0; i < 16; i++) {
        glEnableVertexAttribArray(i);
        glVertexAttribPointer(i, 4, GL_FLOAT, GL_FALSE, 16 * 4 * sizeof(float),
                              (const void *)(size_t)(i * 4 * sizeof(float)));
    }

    /* ---- edge cases: each ILU op on special values ---------------------- */
    {
        static const float xs[] = { 0.0f, -0.0f, 1e-30f, -1e-30f, 1e30f, -1e30f, 1.0f, -1.0f,
                                    2.0f, 0.5f, 3.0f, -7.25f, 1e-19f, 1e19f, 100.0f, -100.0f };
        static const float lit[][4] = {
            { 1, 0, 0, 0 }, { 1, 0, 0, -2 }, { 1, 0, 0, 2 }, { -1, 2, 0, 1 }, { 1, 2, 0, 200 },
            { 1, 0.5f, 0, -200 }, { 0, 2, 0, 1 }, { 0.5f, -1, 0, 3 }, { 2, 3, 0, 127.9f },
            { 1, 1, 0, 0 }, { 3, 0.25f, 0, 0.5f }, { 1e-3f, 4, 0, 10 }, { 1, 0, 0, 1e-3f },
            { 1, 1e-3f, 0, 120 }, { -0.0f, 1, 0, 1 }, { 5, 5, 0, -5 } };
        static float in[16][16][4];
        static Nv2aVshOutput ref[16], got[16];
        uint32_t op;
        int v, bad;

        for (op = 1; op <= 7; op++) {
            /* ILU op on v0 to oT0, alone (MAC nop). */
            encode(w[0], 0, op, rr0, rr0, rv0, 0, 0, 0, 0, 0, 0xF, 1, 9, 1, 0, 1);
            memset(in, 0, sizeof in);
            for (v = 0; v < 16; v++) {
                if (op == 7)
                    memcpy(in[v][0], lit[v], sizeof lit[v]);
                else
                    in[v][0][0] = xs[v];
                in[v][0][1] = op == 7 ? in[v][0][1] : 1.0f;
            }
            if (run_both((const uint32_t (*)[4])w, 1, (const float (*)[16][4])in, 16, ref, got, 1)) {
                char what[32];
                snprintf(what, sizeof what, "ILU op %u", op);
                bad = compare(ref, got, 16, 4, what);
                CHECK(bad == 0);
            } else {
                CHECK(0);
            }
        }
        /* ARL a0 = v0.x, then o[9] = c[a0 + 5]: in range, below 0, past 191,
         * and floor(x + 0.001). */
        {
            static const float a0s[] = { 0, 3, -10, -5, -6, 186, 187, 2.9995f, 2.998f,
                                         -0.0005f, -0.002f, 100.4f, 190, 1, 50, 7 };
            Src cr = { 3, 0, SWZ_ID, 0 };
            encode(w[0], 13, 0, rv0, rr0, rr0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
            encode(w[1], 1, 0, cr, rr0, rr0, 5, 0, 0, 0, 0, 0xF, 1, 9, 0, 1, 1);
            memset(in, 0, sizeof in);
            for (v = 0; v < 16; v++)
                in[v][0][0] = a0s[v];
            if (run_both((const uint32_t (*)[4])w, 2, (const float (*)[16][4])in, 16, ref, got, 1)) {
                bad = compare(ref, got, 16, 4, "a0-relative");
                CHECK(bad == 0);
            } else {
                CHECK(0);
            }
        }
        /* MAC + ILU pair: the ILU writes R1 whatever the temp field says;
         * then oPos from R1 (the D3D epilogue's shape). */
        {
            encode(w[0], 2, 3, rv0, rv0, rv0, 0, 0, 0x8, 0x8, 7, 0, 0, 0, 0, 0, 0);
            encode(w[1], 1, 0, (Src){ 1, 1, SWZ_ID, 0 }, rr0, rr0, 0, 0, 0, 0, 0, 0xF, 1, 0, 0, 0, 1);
            memset(in, 0, sizeof in);
            for (v = 0; v < 16; v++)
                in[v][0][0] = xs[v];
            if (run_both((const uint32_t (*)[4])w, 2, (const float (*)[16][4])in, 16, ref, got, 1)) {
                bad = compare(ref, got, 16, 4, "MAC+ILU pair");
                CHECK(bad == 0);
            } else {
                CHECK(0);
            }
        }
        (void)rc;
    }

    /* ---- random programs ------------------------------------------------- */
    make_programs();
    {
        static const struct { const char *name, *from, *to; } muts[] = {
            { "DPH without + B.w", "dot(A.xyz, B.xyz) + B.w", "dot(A.xyz, B.xyz)" },
            { "a0 off by one", "vsh_ci(a0 + ", "vsh_ci(a0 + 1 + " },
            { "RCC clamp", "1.884467e19) f = f < 0.0 ? -1.884467e19 : 1.884467e19",
                           "1.884467e19) f = f < 0.0 ? -1.0e19 : 1.0e19" },
            { "MIN as MAX", "mix(B, A, lessThan(A, B))", "mix(A, B, lessThan(A, B))" },
            { "colour clamp dropped", "x_d0 = vsh_sat(o_d0)", "x_d0 = o_d0" },
            { "R1 writes go to R2", "  R1.", "  R2." },
            { "SGE as SLT", "vec4(greaterThanEqual(A, B))", "vec4(lessThan(A, B))" },
            { "RSQ without abs", "float a = abs(x); return a == 0.0", "float a = x; return a == 0.0" },
        };
        int touched, m;
        long bad = run_random(NULL, NULL, NPROG, 8, &touched);
        long verts = (long)touched * NV;

        printf("random: %d programs, %ld vertices, %ld differ (%.2f%%); %lu components"
               " compared, %lu NaN references skipped\n", touched, verts, bad,
               verts ? 100.0 * (double)bad / (double)verts : 0.0, s_cmp, s_nan_skipped);
        CHECK(touched == NPROG);
        CHECK(bad * 100 <= verts);
        bad = bad / 4 + 1;          /* what the quarter run may differ by anyway */
        for (m = 0; m < (int)(sizeof muts / sizeof muts[0]); m++) {
            /* The first quarter of the programs is plenty to show it. */
            long mb = run_random(muts[m].from, muts[m].to, NPROG / 4, 0, &touched);
            printf("mutation \"%s\": %d programs touched, %ld vertices differ -> %s\n",
                   muts[m].name, touched, mb, mb > bad + 10 ? "caught" : "MISSED");
            CHECK(mb > bad + 10);
        }
    }

    if (s_fail) {
        printf("FAIL: %d check(s)\n", s_fail);
        return 1;
    }
    puts("ok");
    return 0;
}
