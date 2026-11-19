/* NV2A coverage on the GL back end (XBA-120): pushbuffer methods into the
 * real executor (nv2a_pb_exec.c, linked), which hands its batches to the
 * real GL back end (src/gpu/nv2a_gl.c, included so the checks can read its
 * statistics and call its present hook), over a fake guest memory; the
 * pixels are read back into guest memory and checked. Built for desktop GL
 * and for the GLES 3.0 path Android runs (NV2A_GL_FORCE_ES); on Linux both
 * run on Mesa's llvmpipe through EGL_PLATFORM=surfaceless. Exit 77 without
 * a GL context.
 *
 * Points and lines:
 *   1. a screen-space line strip (no matrix, positions in pixels);
 *   2. fixed-function points of SET_POINT_SIZE 4 (6.3 fixed point: 32), each
 *      a 4x4 square of its vertex colour;
 *   3. a fixed-function line loop: its closing segment is drawn;
 *   4. a point sprite: a 2x2 texture over an 8x8 point, texel (0,0) in the
 *      point's top left quarter;
 *   5. vertex-program points and lines (the GLSL vertex program path);
 *   6. none of it is counted as unsupported; the CPU rasteriser draws no
 *      point and counts one.
 *
 * Mip chains:
 *   7. a 16x16 texture of five one-colour levels over 4x4 pixels: level 2
 *      with BOX_NEARESTLOD, level 3 with a LOD bias of +1, level 1 with the
 *      LOD clamped to 1, half level 2 half level 3 trilinear at LOD 2.5,
 *      level 0 with a LOD0 filter and with RECOMP_GPU_MIPS=0.
 *
 * Cube and volume textures:
 *   8. a cube map's six faces, one colour each, on a CUBEMAP stage;
 *   9. a 4x4x4 volume, one colour a slice: each slice by its r through the
 *      colour rule without combiners, then on a PROJECT3D stage with q = 2;
 *      the CPU rasteriser counts it as unsupported.
 *
 * Formats:
 *  10. swizzled A8Y8 and R4G4B4A4 (added), and SET_TEXTURE_FILTER's signed
 *      channels on an A8R8G8B8 texel, through the combiners.
 *
 * Fixed function on the GPU (XBA-134 item 1):
 *  11. the generated vertex shader against the CPU transform, output by
 *      output over random states, and the same picture both ways; then
 *      over random states of each part XBA-306 added (vertex blending,
 *      texgen, texture matrices, specular, two-sided, spot lights) alone
 *      and all together.
 *
 * Streamed uploads (XBA-302):
 *  12. 240 overlapping batches, CPU-transformed and vertex-program
 *      (gathered: vertices and indices), drawn with a glBufferData per draw
 *      (RECOMP_GPU_STREAM=off), then through 4 KB rings that wrap every few
 *      batches (unsynchronised maps, then glBufferSubData) and the default
 *      rings: every picture byte for byte the first. Every scene's 240
 *      batches reach the back end, each on the path asked for (XBA-315).
 *
 * Stage modes 8 and up (XBA-311):
 *  14. DOT_ST, the dependent reads, DOT_STR_CUBE, the two reflections and
 *      bump-env drawn by the GL back end and by the CPU rasteriser, every
 *      pixel compared (a few cube-face ties allowed), hand-worked pixels
 *      checked, none counted as unsupported.
 *
 * Uniform shadow (XBA-312):
 *  13. the scene with the shadow off (every uniform sent at every batch)
 *      and on: the same picture, calls left out; with the fixed-function
 *      batches on the CPU, and on the GPU (c[] changing in part). */
#include "../../src/gpu/nv2a_gl.c"
#include "nv2a_coverage.h"
#include "nv2a_vsh_interp.h"   /* Nv2aVshOutput: the capture layout */
#include <math.h>

#define MEM_BYTES (8u << 20)
#define FB      0x00100000u         /* 64x64 A8R8G8B8, pitch 256 */
#define VB      0x00200000u         /* vertices */
#define TEX     0x00300000u         /* textures */
#define W 64
#define H 64

static uint8_t *s_mem;
static int s_fail;

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); s_fail = 1; \
} } while (0)

/* ---- what the executor expects the runtime to provide ------------------ */
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)s_mem; }
const char *xbox_path_save_dir_utf8(void) { return NULL; }
void xbox_HostThreadFloat(const char *role) { (void)role; }   /* the shader worker's placement */
uint32_t g_xbox_image_lo, g_xbox_image_hi;
uint32_t g_xbox_stack_base, g_xbox_stack_size;
uint32_t xbox_ContiguousAllocatedBytes(void) { return 0; }
void xbox_FramebufferWindowSet(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_FramebufferWindowSetAA(uint32_t sy) { (void)sy; }
void xbox_FramebufferWindowSetFormat(uint32_t f) { (void)f; }
uint32_t xbox_Nv2aFenceWordVa(void) { return 0; }
int nv2a_pb_dma_walk(void) { return 1; }
void xbox_FramebufferWindowStart(void) {}
void xbox_FramebufferWindowFrameStats(uint32_t draws) { (void)draws; }
void xbox_FramebufferWindowFrameTris(uint32_t tris) { (void)tris; }
void xbox_FramebufferWindowPresent(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
int xbox_FramebufferWindowPresentHost(const void *px, uint32_t w, uint32_t h, uint32_t pitch,
                                      uint32_t format)
{ (void)px; (void)w; (void)h; (void)pitch; (void)format; return 0; }
void xbox_Nv2aFrameCounterFlip(void) {}
int nv2a_pb_scan_in_call(void) { return 0; }
uint32_t xbox_VblankCount(void) { return 0; }
uint64_t xbox_HostNowNs(void) { return 0; }
void xbox_HostSleepNs(uint64_t ns) { (void)ns; }
uint32_t xbox_Nv2aFrameCountersOwned(void) { return 0; }
volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset) { (void)offset; return NULL; }

/* ---- driving the executor ---------------------------------------------- */
extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
static void m(uint32_t method, uint32_t param) { nv2a_pb_exec_method(0, method, param); }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static void mf(uint32_t method, float f) { m(method, f2u(f)); }
static void put32(uint32_t at, uint32_t v) { memcpy(s_mem + at, &v, 4); }
static void putf(uint32_t at, float f) { memcpy(s_mem + at, &f, 4); }

static uint32_t px(uint32_t x, uint32_t y)
{
    uint32_t v;
    memcpy(&v, s_mem + FB + (size_t)y * 256 + x * 4u, 4);
    return v & 0x00FFFFFFu;
}

/* Pixels of colour c (RGB) in [x0,x1)x[y0,y1). */
static uint32_t count(uint32_t c, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    uint32_t x, y, n = 0;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++)
            n += px(x, y) == c;
    return n;
}

/* The surface back into guest memory, now (the flip's sync present). */
static void readback(void)
{
    Nv2aPresentSurface now, ready;
    memset(&now, 0, sizeof now);
    now.va = FB;
    now.pitch = 256;
    now.bpp = 4;
    now.format = 0x0008;
    now.w = W;
    now.h = H;
    gl_present(&now, 1, &ready);
    gl_flip();
}

static void clear_black(void)
{
    m(0x1D90, 0xFF000000u);                  /* SET_COLOR_CLEAR_VALUE */
    m(0x1D94, 0xF0);                         /* CLEAR_SURFACE: colour */
}

static void surface(void)
{
    m(0x0208, 0x0008);                       /* SET_SURFACE_FORMAT: A8R8G8B8, linear */
    m(0x0200, (uint32_t)W << 16);
    m(0x0204, (uint32_t)H << 16);
    m(0x020C, 256);
    m(0x0210, FB);
}

/* Fixed function: composite = the 64x64 viewport, y down. */
static void ffp(void)
{
    static const float M[16] = { 32, 0, 0, 32, 0, -32, 0, 32, 0, 0, 1, 0, 0, 0, 0, 1 };
    uint32_t i;
    for (i = 0; i < 16; i++)
        mf(0x0680 + 4 * i, M[i]);
    m(0x1E94, 0);
}

/* Vertex i at VB + 32 i: float3 position, D3DCOLOR, float2 texcoord. */
static void vtx(uint32_t i, float x, float y, float z, uint32_t argb, float u, float v)
{
    uint32_t at = VB + 32 * i;
    putf(at, x); putf(at + 4, y); putf(at + 8, z);
    put32(at + 12, argb);
    putf(at + 16, u); putf(at + 20, v);
}

static void arrays(void)
{
    uint32_t a;
    for (a = 0; a < 16; a++)
        m(0x1760 + 4 * a, 2);                /* size 0: off */
    m(0x1720, VB);      m(0x1760, 2 | (3 << 4) | (32 << 8));        /* v0: float3 */
    m(0x172C, VB + 12); m(0x176C, 0 | (4 << 4) | (32 << 8));        /* v3: D3DCOLOR */
    m(0x1744, VB + 16); m(0x1784, 2 | (2 << 4) | (32 << 8));        /* v9: float2 */
}

static void draw(uint32_t prim, uint32_t first, uint32_t n)
{
    m(0x17FC, prim);
    m(0x1810, first | ((n - 1) << 24));
    m(0x17FC, 0);
}

/* ---- XBA-134 item 1: the fixed-function unit as a generated shader ------
 * A spy around the GL back end records what each path hands it: the CPU
 * transform's vertices (draw) and the fixed-function program batch
 * (draw_program with ffp set), then passes the batch on. */
extern void nv2a_gpu_ffp_enable(int on);
#define REC_MAX 256
static struct {
    uint32_t count;                         /* CPU path: vertices */
    Nv2aVertexFull full[REC_MAX];
    int got_ffp;                            /* GPU path: a fixed-function batch */
    Nv2aFfpKey key;
    uint32_t rows, streams;
    float attrs[REC_MAX * 16 * 4];
    float cst[192][4];
} s_rec;
static Nv2aBackend s_spy;

/* Batches the back end received (XBA-315): CPU-transformed (draw with full
 * vertices), vertex-program and fixed-function program batches. */
static uint32_t s_got_cpu, s_got_prog, s_got_ffp;

static void spy_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    if (b->full)
        s_got_cpu++;
    if (b->full && b->count <= REC_MAX) {
        s_rec.count = b->count;
        memcpy(s_rec.full, b->full, b->count * sizeof *b->full);
    }
    gl_draw(s, b);
}

static int spy_draw_program(const Nv2aSurface *s, const Nv2aBatch *b)
{
    const Nv2aProgramBatch *pb = b->program;
    if (pb->ffp)
        s_got_ffp++;
    else
        s_got_prog++;
    if (pb->ffp && pb->attrs && pb->vertex_count <= REC_MAX) {
        uint32_t a, ns = 0;
        for (a = 0; a < 16; a++)
            ns += (pb->streams >> a) & 1;
        s_rec.got_ffp = 1;
        s_rec.key = *pb->ffp;
        s_rec.rows = pb->vertex_count;
        s_rec.streams = pb->streams;
        memcpy(s_rec.attrs, pb->attrs, (size_t)pb->vertex_count * ns * 4 * sizeof(float));
        memcpy(s_rec.cst, pb->constants, sizeof s_rec.cst);
    }
    return gl_draw_program(s, b);
}

static void spy_install(void)
{
    s_spy = s_backend;
    s_spy.caps &= ~NV2A_BACKEND_CAP_RAW_VERTEX;     /* gathered: the spy copies the rows */
    s_spy.draw = spy_draw;
    s_spy.draw_program = spy_draw_program;
    nv2a_backend_register_with_present(&s_spy, gl_present);
}

/* Run the fixed-function body for s_rec on the GPU, capturing what it
 * writes (transform feedback, Nv2aVshOutput order). */
static int ffp_capture(Nv2aVshOutput *out)
{
    static char body[65536];
    static const char *fs_body = "out vec4 o;\nvoid main() { o = vec4(0.0); }\n";
    const char *src[2];
    GLuint vs, fs, prog, vao, vbo, tfb;
    GLint ok = 0;
    uint32_t a, k, ns = 0, i;
    void *map;

    if (!nv2a_gl_vsh_ffp_body(&s_rec.key, NV2A_GL_VSH_CAPTURE, 0, body, sizeof body))
        return 0;
    src[0] = nv2a_gl_glsl_header(NV2A_GL_ES);
    vs = glCreateShader(GL_VERTEX_SHADER);
    src[1] = body;
    glShaderSource(vs, 2, src, NULL);
    glCompileShader(vs);
    glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(vs, sizeof log, NULL, log);
        printf("ffp body compile failed: %s\n%s\n", log, body);
        return 0;
    }
    fs = glCreateShader(GL_FRAGMENT_SHADER);
    src[1] = fs_body;
    glShaderSource(fs, 2, src, NULL);
    glCompileShader(fs);
    prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glTransformFeedbackVaryings(prog, 8, nv2a_gl_vsh_capture_names, GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok)
        return 0;
    sh_forget();                            /* raw GL below: the back end's shadow is stale */
    glUseProgram(prog);
    for (i = 0; i < 192; i++) {
        char name[12];
        GLint loc;
        snprintf(name, sizeof name, "c[%u]", i);
        if ((loc = glGetUniformLocation(prog, name)) >= 0)
            glUniform4fv(loc, 1, s_rec.cst[i]);
    }
    for (a = 0; a < 16; a++)
        ns += (s_rec.streams >> a) & 1;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glGenBuffers(1, &tfb);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(s_rec.rows * ns * 16), s_rec.attrs, GL_STREAM_DRAW);
    for (a = 0, k = 0; a < 16; a++)
        if (s_rec.streams & (1u << a)) {
            glEnableVertexAttribArray(a);
            glVertexAttribPointer(a, 4, GL_FLOAT, GL_FALSE, (GLsizei)(ns * 16),
                                  (const void *)((size_t)k++ * 16));
        }
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, tfb);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, (GLsizeiptr)(s_rec.rows * sizeof(Nv2aVshOutput)),
                 NULL, GL_STREAM_READ);
    glEnable(GL_RASTERIZER_DISCARD);
    glBeginTransformFeedback(GL_POINTS);
    glDrawArrays(GL_POINTS, 0, (GLsizei)s_rec.rows);
    glEndTransformFeedback();
    glDisable(GL_RASTERIZER_DISCARD);
    map = glMapBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0,
                           (GLsizeiptr)(s_rec.rows * sizeof(Nv2aVshOutput)), GL_MAP_READ_BIT);
    if (map) {
        memcpy(out, map, s_rec.rows * sizeof(Nv2aVshOutput));
        glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER);
    }
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
    glBindVertexArray(0);
    glDeleteBuffers(1, &vbo);
    glDeleteBuffers(1, &tfb);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(prog);
    sh_forget();
    return map != NULL;
}

static uint32_t s_rng = 12345;
static float frand(float lo, float hi)
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return lo + (hi - lo) * (float)(s_rng >> 8) / 16777216.0f;
}

/* XBA-306 features a trial turns on (ffp_trial's feat). */
#define FT_TEXGEN   1u
#define FT_TEXMAT   2u
#define FT_SKIN     4u
#define FT_SPECULAR 8u
#define FT_TWOSIDE  16u
#define FT_SPOT     32u
#define FT_ALL      63u
static uint32_t s_ft_seen;              /* FT_* the shader batches' keys carried */

/* Every XBA-306 register back off (the trials leave them set). */
static void ffp_trial_reset(void)
{
    uint32_t st, k;
    m(0x03B8, 0); m(0x0294, 0); m(0x17C4, 0); m(0x0328, 0); m(0x09CC, 0); m(0x0298, 0);
    for (st = 0; st < 4; st++) {
        for (k = 0; k < 4; k++)
            m(0x03C0 + 16 * st + 4 * k, 0);
        m(0x0420 + 4 * st, 0);
    }
}

/* One random fixed-function state and batch (trial t), drawn through the
 * CPU transform and through the generated shader; the vertices compared.
 * feat: the XBA-306 parts turned on at random settings (every register of
 * the others is written off). Returns the worst difference. */
static float ffp_trial(int t, uint32_t feat)
{
    static Nv2aVshOutput gpu[REC_MAX];
    static const uint32_t tg_modes[6] = { 0, 0x2400, 0x2401, 0x2402, 0x8512, 0x8511 };
    uint32_t i, k, st, n = 48, nl = (uint32_t)t % 9, mask = 0;
    int force = (feat & (FT_SPECULAR | FT_TWOSIDE | FT_SPOT)) != 0;
    int lit = force || t % 4 != 3, col = t % 5 != 4, nrm = force || t % 7 != 6;
    float worst = 0.0f;

    if (force && !nl)
        nl = 1 + (uint32_t)t % 8;
    /* Composite: viewport-ish scale, a little perspective, w in [1, 2]. */
    {
        const float M[16] = { 32 * frand(0.5f, 1.5f), frand(-4, 4), frand(-4, 4), 32,
                              frand(-4, 4), -32 * frand(0.5f, 1.5f), frand(-4, 4), 32,
                              frand(-0.1f, 0.1f), frand(-0.1f, 0.1f), 0.2f, 0.5f,
                              frand(-0.2f, 0.2f), frand(-0.2f, 0.2f), frand(-0.2f, 0.2f), 1.5f };
        for (i = 0; i < 16; i++)
            mf(0x0680 + 4 * i, M[i]);
    }
    for (i = 0; i < 12; i++)                 /* model-view rows 0-2 */
        mf(0x0480 + 4 * i, (i % 5 == 0) ? frand(0.5f, 1.5f) : frand(-0.5f, 0.5f));
    for (i = 12; i < 16; i++)                /* row 3: (0, 0, 0, 1) */
        mf(0x0480 + 4 * i, i == 15 ? 1.0f : 0.0f);
    m(0x0314, (uint32_t)lit);
    for (i = 0; i < nl; i++) {
        uint32_t b = 0x1000 + i * 0x80, bb = 0x0C00 + i * 0x40;
        uint32_t kind = frand(0, 1) < 0.5f ? 1u : 2u;
        if (kind == 2 && (feat & FT_SPOT) && (t + i) % 2 == 0)
            kind = 3;
        mask |= kind << (2 * i);
        for (k = 0; k < 3; k++) {
            mf(b + 4 * k, frand(0, 0.2f));                 /* ambient */
            mf(b + 0x0C + 4 * k, frand(0, 0.6f));          /* diffuse */
            mf(b + 0x34 + 4 * k, frand(-1, 1));            /* direction */
            mf(b + 0x5C + 4 * k, frand(-3, 3));            /* position */
            mf(b + 0x68 + 4 * k, k == 0 ? frand(0.5f, 1.5f) : frand(0, 0.3f));
        }
        mf(b + 0x24, frand(0, 1) < 0.3f ? frand(0.5f, 2.0f) : 0.0f);   /* range */
        if (!feat)
            continue;
        for (k = 0; k < 3; k++) {
            mf(b + 0x18 + 4 * k, frand(0, 0.6f));          /* specular */
            mf(b + 0x28 + 4 * k, frand(-1, 1));            /* half vector */
            mf(b + 0x40 + 4 * k, k == 0 ? frand(-0.2f, 0.2f) : k == 1 ? frand(0, 1)
                                                               : frand(0.5f, 1.5f));
            mf(bb + 4 * k, frand(0, 0.2f));                /* back ambient */
            mf(bb + 0x0C + 4 * k, frand(0, 0.6f));         /* back diffuse */
            mf(bb + 0x18 + 4 * k, frand(0, 0.6f));         /* back specular */
        }
        for (k = 0; k < 4; k++)                            /* spot direction */
            mf(b + 0x4C + 4 * k, k < 3 ? frand(-2, 2) : frand(-1, 1));
    }
    m(0x03BC, mask);
    /* Colour material: ambient, diffuse from v[col]; specular 0..2. */
    m(0x0298, (uint32_t)(t & 3) << 2 | (uint32_t)((t >> 2) & 1) << 4
              | ((feat & FT_SPECULAR) ? (uint32_t)(t % 3) << 6 : 0u));
    for (k = 0; k < 3; k++)
        mf(0x0A10 + 4 * k, frand(0, 0.3f));
    mf(0x03B4, frand(0, 1));
    mf(0x0A20, frand(-2, 2)); mf(0x0A24, frand(-2, 2)); mf(0x0A28, frand(0, 1));
    ffp_trial_reset();
    if (feat) {
        for (k = 0; k < 3; k++) {
            mf(0x17A0 + 4 * k, frand(0, 0.3f));            /* back scene ambient */
            mf(0x09E0 + 4 * k, k == 0 ? frand(-0.5f, 0.0f) : k == 1 ? frand(0, 0.5f)
                                                             : frand(0.5f, 1.5f));
            mf(0x1E28 + 4 * k, k == 0 ? frand(-0.5f, 0.0f) : k == 1 ? frand(0, 0.5f)
                                                             : frand(0.5f, 1.5f));
        }
        mf(0x17AC, frand(0, 1));
        for (k = 0; k < 4; k++)
            mf(0x0A50 + 4 * k, k < 3 ? frand(-2, 2) : 1.0f);   /* eye position */
    }
    m(0x03B8, (feat & FT_SPECULAR) ? 1u : 0u);
    m(0x0294, (feat & FT_SPECULAR) ? (uint32_t)(t & 1) | ((uint32_t)((t >> 1) & 1) << 16) : 0u);
    m(0x17C4, (feat & FT_TWOSIDE) ? 1u : 0u);
    /* Vertex blending: a random mode and matrices 1..3. */
    m(0x0328, (feat & FT_SKIN) ? 1u + (uint32_t)t % 6 : 0u);
    for (k = 1; (feat & FT_SKIN) && k < 4; k++)
        for (i = 0; i < 16; i++)
            mf(0x0480 + 0x40 * k + 4 * i, i >= 12 ? (i == 15 ? 1.0f : 0.0f)
                                         : (i % 5 == 0) ? frand(0.5f, 1.5f) : frand(-0.5f, 0.5f));
    /* Texgen and texture matrices, every stage. */
    m(0x09CC, (uint32_t)t & 1);
    for (st = 0; (feat & (FT_TEXGEN | FT_TEXMAT)) && st < 4; st++) {
        for (k = 0; k < 4; k++) {
            uint32_t mode = 0;
            if ((feat & FT_TEXGEN) && frand(0, 1) < 0.7f) {
                mode = tg_modes[(uint32_t)frand(0, 5.99f)];
                if ((mode == 0x2402 && k >= 2) || ((mode & 0xFF00) == 0x8500 && k == 3))
                    mode = 0x2401;
            }
            m(0x03C0 + 16 * st + 4 * k, mode);
            for (i = 0; i < 4; i++)
                mf(0x0840 + 64 * st + 16 * k + 4 * i, frand(-1, 1));
        }
        m(0x0420 + 4 * st, (feat & FT_TEXMAT) && frand(0, 1) < 0.7f ? 1u : 0u);
        for (i = 0; i < 16; i++)
            mf(0x06C0 + 64 * st + 4 * i, (i % 5 == 0) ? frand(0.5f, 1.5f) : frand(-0.5f, 0.5f));
    }
    /* Vertices (80 bytes): position, D3DCOLOR, normal, texcoord 0 (float2)
     * and 1 (float4), weights (float3), specular (D3DCOLOR). */
    for (i = 0; i < n; i++) {
        uint32_t at = VB + 80 * i;
        putf(at, frand(-1, 1)); putf(at + 4, frand(-1, 1)); putf(at + 8, frand(0, 1));
        put32(at + 12, s_rng ^ (i * 0x9E3779B9u));
        putf(at + 16, frand(-1, 1)); putf(at + 20, frand(-1, 1)); putf(at + 24, frand(-1, 1));
        putf(at + 28, frand(0, 1)); putf(at + 32, frand(0, 1));
        putf(at + 36, frand(-2, 2)); putf(at + 40, frand(-2, 2)); putf(at + 44, frand(-2, 2));
        putf(at + 48, frand(0.5f, 2));
        if (feat) {
            putf(at + 52, frand(0, 0.6f)); putf(at + 56, frand(0, 0.4f));
            putf(at + 60, frand(0, 0.3f));
        }
        put32(at + 64, (s_rng >> 3) ^ (i * 0x85EBCA6Bu));
    }
    for (k = 0; k < 16; k++)
        m(0x1760 + 4 * k, 2);
    m(0x1720, VB);      m(0x1760, 2 | (3 << 4) | (80 << 8));
    if (col) { m(0x172C, VB + 12); m(0x176C, 0 | (4 << 4) | (80 << 8)); }
    if (nrm) { m(0x1728, VB + 16); m(0x1768, 2 | (3 << 4) | (80 << 8)); }
    m(0x1744, VB + 28); m(0x1784, 2 | (2 << 4) | (80 << 8));
    m(0x1748, VB + 36); m(0x1788, 2 | (4 << 4) | (80 << 8));
    if (feat & FT_SKIN) { m(0x1724, VB + 52); m(0x1764, 2 | (3 << 4) | (80 << 8)); }
    if (feat & FT_SPECULAR) { m(0x1730, VB + 64); m(0x1770, 0 | (4 << 4) | (80 << 8)); }

    memset(&s_rec, 0, sizeof s_rec);
    nv2a_gpu_ffp_enable(0);
    draw(5, 0, n);                           /* TRIANGLES: the CPU transform */
    nv2a_gpu_ffp_enable(1);
    draw(5, 0, n);                           /* the generated shader */
    nv2a_gpu_ffp_enable(0);
    if (s_rec.count != n || !s_rec.got_ffp || s_rec.rows != n || !ffp_capture(gpu)) {
        printf("  trial %d (features 0x%x): CPU %u vertices, shader batch %d with %u rows\n",
               t, feat, s_rec.count, s_rec.got_ffp, s_rec.rows);
        return 1e9f;
    }
    for (st = 0; st < 4; st++) {
        for (k = 0; k < 4; k++)
            s_ft_seen |= s_rec.key.texgen[st][k] ? FT_TEXGEN : 0u;
        s_ft_seen |= s_rec.key.texmat[st] ? FT_TEXMAT : 0u;
    }
    for (k = 0; k < s_rec.key.nlights; k++)
        s_ft_seen |= s_rec.key.light_kind[k] == 3 ? FT_SPOT : 0u;
    s_ft_seen |= (s_rec.key.skin ? FT_SKIN : 0u) | (s_rec.key.specular ? FT_SPECULAR : 0u)
               | (s_rec.key.two_side ? FT_TWOSIDE : 0u);
    for (i = 0; i < n; i++) {
        const Nv2aVertexFull *c = &s_rec.full[i];
        const Nv2aVshOutput *g = &gpu[i];
        for (k = 0; k < 4; k++) {
            float d = fabsf(c->pos[k] - g->pos[k]) / (1.0f + fabsf(c->pos[k]));
            float dc = fabsf(c->d0[k] - g->d0[k]), ds = fabsf(c->d1[k] - g->d1[k]);
            d = d > dc ? d : dc;
            d = d > ds ? d : ds;
            for (st = 0; st < 4; st++) {
                float dt = fabsf(c->tex[st][k] - g->tex[st][k]) / (1.0f + fabsf(c->tex[st][k]));
                d = d > dt ? d : dt;
            }
            if (d > worst)
                worst = d;
        }
    }
    return worst;
}

/* NDC of pixel position (x, y) on the 64x64 viewport above. */
static float nx(float x) { return x / 32.0f - 1.0f; }
static float ny(float y) { return 1.0f - y / 32.0f; }

/* Cases 12 and 13: 240 batches of 3 to 33 vertices at their own place in
 * VB (the executor may read them late), alpha tested against a reference
 * that changes at every batch (a uniform that changes), depth test off.
 * Every third goes through the vertex program (positions in pixels,
 * indexed with vertices shared at random, so that the index lists differ),
 * the others through the fixed-function unit: transformed on the CPU, or
 * with gpu_ffp by the generated vertex shader, from c[] (the composite
 * matrix's translation and x scale changing from batch to batch, back and
 * forth: part of the constants change). The picture into out. Returns the
 * ring wraps counted while drawing; s_scene_uni the uniform calls issued
 * and left out. */
static uint32_t s_scene_uni[2];
static uint32_t stream_scene(uint32_t *out, int gpu_ffp)
{
    Nv2aPresentSurface now, ready;
    uint32_t i, k, x, y, base = 0, rng = 777, wraps;

    s_got_cpu = s_got_prog = s_got_ffp = 0;
    nv2a_gpu_ffp_enable(gpu_ffp);
    m(0x030C, 0);                            /* no depth test */
    clear_black();
    wraps = s_st.strm_wraps;
    s_scene_uni[0] = s_st.uni_calls;
    s_scene_uni[1] = s_st.uni_skipped;
    m(0x0300, 1);                            /* alpha test, GREATER: the reference */
    m(0x033C, 0x0204);                       /* (a uniform) changes every batch */
    for (i = 0; i < 240; i++) {
        uint32_t nv = 3 * (1 + (i * 7) % 11);
        int prog = i % 3 == 0;
        m(0x1E94, prog ? 2 : 0);
        m(0x0340, (i * 37u) & 0x7F);
        if (gpu_ffp && !prog) {
            mf(0x0680 + 4 * 3, 32.0f + (float)(i % 5));      /* translation x */
            mf(0x0680 + 4 * 0, 32.0f - (float)(i % 3));      /* scale x */
        }
        for (k = 0; k < nv; k++) {
            float fx, fy;
            uint32_t c;
            rng = rng * 1103515245u + 12345u;
            fx = (float)((rng >> 8) % 72) - 4.0f;
            fy = (float)((rng >> 16) % 72) - 4.0f;
            c = rng * 2654435761u;
            if (prog)
                vtx(base + k, fx, fy, 100, c, 0, 0);
            else
                vtx(base + k, nx(fx), ny(fy), 0.5f, c, 0, 0);
        }
        if (prog) {
            /* Indexed (ARRAY_ELEMENT16), vertices shared at random: each
             * batch's gathered index list is its own. */
            uint32_t ni = 2 * nv;
            m(0x17FC, 5);
            for (k = 0; k < ni; k += 2) {
                uint32_t a, b;
                rng = rng * 1103515245u + 12345u;
                a = base + (rng >> 8) % nv;
                b = base + (rng >> 20) % nv;
                m(0x1800, a | b << 16);
            }
            m(0x17FC, 0);
        } else {
            draw(5, base, nv);               /* TRIANGLES */
        }
        base += nv;
    }
    m(0x1E94, 0);
    m(0x0300, 0);
    ffp();
    nv2a_gpu_ffp_enable(0);
    /* readback(), counted between the present (every batch drawn) and the
     * flip (which may clear the counters). */
    memset(&now, 0, sizeof now);
    now.va = FB;
    now.pitch = 256;
    now.bpp = 4;
    now.format = 0x0008;
    now.w = W;
    now.h = H;
    gl_present(&now, 1, &ready);
    wraps = s_st.strm_wraps - wraps;
    s_scene_uni[0] = s_st.uni_calls - s_scene_uni[0];
    s_scene_uni[1] = s_st.uni_skipped - s_scene_uni[1];
    gl_flip();
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            out[y * W + x] = px(x, y);
    /* Every batch reached the back end (XBA-315): 80 through the vertex
     * program, 160 fixed-function ones on the path asked for. */
    if (s_got_prog != 80 || s_got_cpu != (gpu_ffp ? 0u : 160u)
        || s_got_ffp != (gpu_ffp ? 160u : 0u))
        printf("    scene (fixed function on the %s): back end got %u CPU-transformed,"
               " %u fixed-function, %u vertex-program batches of 160 + 80\n",
               gpu_ffp ? "GPU" : "CPU", s_got_cpu, s_got_ffp, s_got_prog);
    CHECK(s_got_prog == 80);
    CHECK(s_got_cpu == (gpu_ffp ? 0u : 160u));
    CHECK(s_got_ffp == (gpu_ffp ? 160u : 0u));
    return wraps;
}

int main(void)
{
    uint32_t n;

    s_mem = (uint8_t *)calloc(1, MEM_BYTES);
    setenv("RECOMP_GPU", "1", 1);
    if (!nv2a_gl_install_from_env()) {
        fprintf(stderr, "back end not registered\n");
        return 1;
    }
    surface();
    m(0x0358, 0x01010101u);                  /* SET_COLOR_MASK: all */
    clear_black();
    if (s_init <= 0) {
        puts("no GL context: GL coverage checks skipped");
        return 77;
    }
    printf("GL: %s | %s (%s path), points to %.0f px, lines to %.0f px\n",
           (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION),
           NV2A_GL_ES ? "GLES" : "desktop GL", (double)s_point_max, (double)s_line_max);
    arrays();

    /* 1. Screen space (no composite matrix yet): a line strip along row 10
     * (y 10.5, pixel centres) then down column 40. */
    vtx(0, 4.0f, 10.5f, 0, 0xFFFF0000u, 0, 0);
    vtx(1, 40.5f, 10.5f, 0, 0xFFFF0000u, 0, 0);
    vtx(2, 40.5f, 50.0f, 0, 0xFFFF0000u, 0, 0);
    draw(4, 0, 3);                           /* LINE_STRIP */
    readback();
    n = count(0xFF0000u, 6, 10, 38, 11);
    printf("1. screen-space line strip: %u of 32 row pixels, %u of 36 column pixels\n",
           n, count(0xFF0000u, 40, 12, 41, 48));
    CHECK(n == 32);
    CHECK(count(0xFF0000u, 40, 12, 41, 48) == 36);
    CHECK(count(0xFF0000u, 0, 20, 30, 64) == 0);

    /* 2. Fixed-function points, size 4: corners on whole pixels, so each
     * covers exactly 4x4. */
    surface();
    clear_black();
    ffp();
    m(0x043C, 32);                           /* SET_POINT_SIZE 4.0 */
    vtx(0, nx(10), ny(10), 0.5f, 0xFF00FF00u, 0, 0);
    vtx(1, nx(30), ny(20), 0.5f, 0xFF0000FFu, 0, 0);
    vtx(2, nx(50), ny(50), 0.5f, 0xFFFFFF00u, 0, 0);
    draw(1, 0, 3);                           /* POINTS */
    readback();
    printf("2. fixed-function points: %u, %u, %u pixels\n", count(0x00FF00u, 0, 0, W, H),
           count(0x0000FFu, 0, 0, W, H), count(0xFFFF00u, 0, 0, W, H));
    CHECK(count(0x00FF00u, 8, 8, 12, 12) == 16 && count(0x00FF00u, 0, 0, W, H) == 16);
    CHECK(count(0x0000FFu, 28, 18, 32, 22) == 16 && count(0x0000FFu, 0, 0, W, H) == 16);
    CHECK(count(0xFFFF00u, 48, 48, 52, 52) == 16);

    /* 3. A fixed-function line loop: a square whose closing (left) edge is
     * the loop's last segment. */
    clear_black();
    vtx(0, nx(20.5f), ny(20.5f), 0.5f, 0xFFFFFFFFu, 0, 0);
    vtx(1, nx(40.5f), ny(20.5f), 0.5f, 0xFFFFFFFFu, 0, 0);
    vtx(2, nx(40.5f), ny(40.5f), 0.5f, 0xFFFFFFFFu, 0, 0);
    vtx(3, nx(20.5f), ny(40.5f), 0.5f, 0xFFFFFFFFu, 0, 0);
    draw(3, 0, 4);                           /* LINE_LOOP */
    readback();
    n = count(0xFFFFFFu, 20, 22, 21, 39);
    printf("3. fixed-function line loop: closing edge %u of 17 pixels, %u in all\n", n,
           count(0xFFFFFFu, 0, 0, W, H));
    CHECK(n == 17);
    CHECK(count(0xFFFFFFu, 22, 20, 39, 21) == 17);   /* the first segment */
    CHECK(count(0xFFFFFFu, 22, 22, 39, 39) == 0);    /* inside: empty */

    /* 4. A point sprite: an 8x8 point at (32, 32) over a 2x2 linear
     * A8R8G8B8 texture (red, green / blue, white), nearest. */
    clear_black();
    put32(TEX + 0, 0xFFFF0000u); put32(TEX + 4, 0xFF00FF00u);
    put32(TEX + 8, 0xFF0000FFu); put32(TEX + 12, 0xFFFFFFFFu);
    m(0x1B00, TEX);
    m(0x1B04, (0x12u << 8) | (2u << 4) | (1u << 16));   /* LU_IMAGE_A8R8G8B8, 2D, 1 level */
    m(0x1B08, 0x0303);
    m(0x1B0C, 1u << 30);
    m(0x1B10, 8u << 16);                     /* pitch 8 */
    m(0x1B14, (1u << 16) | (1u << 24));      /* nearest */
    m(0x1B1C, (2u << 16) | 2u);
    m(0x043C, 64);                           /* size 8 */
    m(0x031C, 1);                            /* SET_POINT_SMOOTH_ENABLE: sprites */
    vtx(0, nx(32), ny(32), 0.5f, 0xFFFFFFFFu, 0, 0);
    draw(1, 0, 1);
    readback();
    printf("4. point sprite: quarters %06X %06X / %06X %06X\n", px(29, 29), px(34, 29),
           px(29, 34), px(34, 34));
    CHECK(count(0xFF0000u, 28, 28, 32, 32) == 16 && count(0x00FF00u, 32, 28, 36, 32) == 16);
    CHECK(count(0x0000FFu, 28, 32, 32, 36) == 16 && count(0xFFFFFFu, 32, 32, 36, 36) == 16);
    m(0x031C, 0);
    m(0x1B0C, 0);
    m(0x1B00, 0);                            /* no texture */

    /* 5. A vertex program (mov oPos, v0; mov oD0, v3): positions in pixels,
     * through the GLSL vertex program path. */
    {
        static const uint32_t prog[2][4] = {
            { 0, (1u << 21) | (0u << 9) | 0x1Bu, 2u << 26, (0xFu << 12) | (1u << 11) | (0u << 3) },
            { 0, (1u << 21) | (3u << 9) | 0x1Bu, 2u << 26, (0xFu << 12) | (1u << 11) | (3u << 3) | 1u },
        };
        uint32_t draws0 = s_st.vsh_draws, k, w;
        m(0x1E9C, 0);
        for (k = 0; k < 2; k++)
            for (w = 0; w < 4; w++)
                m(0x0B00 + 4 * w, prog[k][w]);
        m(0x1EA0, 0);
        m(0x1E94, 2);                        /* program */
        clear_black();
        m(0x043C, 16);                       /* size 2 */
        vtx(0, 10, 50, 100, 0xFF00FFFFu, 0, 0);
        vtx(1, 12.5f, 30.5f, 100, 0xFFFF00FFu, 0, 0);
        vtx(2, 52.5f, 30.5f, 100, 0xFFFF00FFu, 0, 0);
        draw(1, 0, 1);                       /* POINTS: one */
        draw(2, 1, 2);                       /* LINES: one segment */
        readback();
        printf("5. vertex-program point %u pixels, line %u of 38 pixels (%u GLSL draws)\n",
               count(0x00FFFFu, 0, 0, W, H), count(0xFF00FFu, 14, 30, 52, 31),
               s_st.vsh_draws - draws0);
        CHECK(count(0x00FFFFu, 9, 49, 11, 51) == 4 && count(0x00FFFFu, 0, 0, W, H) == 4);
        CHECK(count(0xFF00FFu, 14, 30, 52, 31) == 38);
        m(0x1E94, 0);
    }

    /* 7. A mip chain: a 16x16 swizzled A8R8G8B8 texture with 5 levels, each
     * level one colour, drawn over 4x4 pixels (LOD 2). */
    {
        static const uint32_t col[5] = { 0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu,
                                         0xFFFFFF00u, 0xFFFFFFFFu };
        uint32_t lv, off = 0, sz = 16, i, c[7];
        for (lv = 0; lv < 5; lv++, sz >>= 1)
            for (i = 0; i < sz * sz; i++, off += 4)
                put32(TEX + off, col[lv]);
        vtx(0, nx(8), ny(8), 0.5f, 0xFFFFFFFFu, 0, 0);
        vtx(1, nx(12), ny(8), 0.5f, 0xFFFFFFFFu, 1, 0);
        vtx(2, nx(12), ny(12), 0.5f, 0xFFFFFFFFu, 1, 1);
        vtx(3, nx(8), ny(12), 0.5f, 0xFFFFFFFFu, 0, 1);
        m(0x1B00, TEX);
        m(0x1B04, (0x06u << 8) | (2u << 4) | (5u << 16) | (4u << 20) | (4u << 24));
        m(0x1B08, 0x0303);
        m(0x1B0C, (1u << 30) | ((4u << 8) << 6));          /* LOD 0 .. 4 */
        m(0x1B14, (3u << 16) | (1u << 24));                 /* BOX_NEARESTLOD */
        clear_black();
        draw(8, 0, 4);                                      /* QUADS */
        readback();
        c[0] = px(10, 10);
        m(0x1B14, (3u << 16) | (1u << 24) | 256u);          /* LOD bias +1 */
        draw(8, 0, 4);
        readback();
        c[1] = px(10, 10);
        m(0x1B14, (3u << 16) | (1u << 24));
        m(0x1B0C, (1u << 30) | ((1u << 8) << 6));          /* LOD clamped to 0 .. 1 */
        draw(8, 0, 4);
        readback();
        c[2] = px(10, 10);
        m(0x1B0C, (1u << 30) | ((4u << 8) << 6));
        m(0x1B14, (6u << 16) | (2u << 24));                 /* TENT_TENT_LOD: trilinear */
        draw(8, 0, 4);
        readback();
        c[3] = px(10, 10);
        m(0x1B14, (6u << 16) | (2u << 24) | 128u);          /* trilinear, LOD 2.5 */
        draw(8, 0, 4);
        readback();
        c[6] = px(10, 10);
        m(0x1B14, (1u << 16) | (1u << 24));                 /* BOX_LOD0 */
        draw(8, 0, 4);
        readback();
        c[4] = px(10, 10);
        /* RECOMP_GPU_MIPS=0: level 0 whatever the filter. */
        s_mips = 0;
        tex_drop_all();
        m(0x1B14, (3u << 16) | (1u << 24));
        draw(8, 0, 4);
        readback();
        c[5] = px(10, 10);
        s_mips = 1;
        tex_drop_all();
        printf("7. mip chain: LOD 2 %06X, bias +1 %06X, clamp 1 %06X, trilinear %06X,"
               " LOD0 filter %06X, mips off %06X; trilinear at LOD 2.5 %06X\n",
               c[0], c[1], c[2], c[3], c[4], c[5], c[6]);
        CHECK(c[0] == 0x0000FFu && c[1] == 0xFFFF00u && c[2] == 0x00FF00u);
        CHECK(c[3] == 0x0000FFu);               /* exactly LOD 2: level 2 alone */
        CHECK(c[4] == 0xFF0000u && c[5] == 0xFF0000u);
        /* Half blue (level 2), half yellow (level 3). */
        CHECK((c[6] >> 16) >= 0x78 && (c[6] >> 16) <= 0x88 && ((c[6] >> 8) & 0xFF) >= 0x78
              && ((c[6] >> 8) & 0xFF) <= 0x88 && (c[6] & 0xFF) >= 0x78 && (c[6] & 0xFF) <= 0x88);
        m(0x1B0C, 0);
        m(0x1B00, 0);
    }

    /* 8, 9: a quad over [8,12)^2 with texcoords (s, t, r) as given, float3
     * texcoords from here on. */
    m(0x1784, 2 | (3 << 4) | (32 << 8));
    {
        /* 9a. A volume through the colour rule without combiners: 4x4x4
         * swizzled A8R8G8B8, slice z one colour, nearest; r picks the
         * slice. */
        static const uint32_t slice[4] = { 0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFFFFu };
        uint32_t x, y, z, k, c[4];
        for (z = 0; z < 4; z++)
            for (y = 0; y < 4; y++)
                for (x = 0; x < 4; x++)
                    put32(TEX + 4 * xbox_swizzle_offset_3d(x, y, z, 4, 4, 4), slice[z]);
        m(0x1B00, TEX);
        m(0x1B04, (0x06u << 8) | (3u << 4) | (1u << 16) | (2u << 20) | (2u << 24) | (2u << 28));
        m(0x1B08, 0x030303);
        m(0x1B0C, 1u << 30);
        m(0x1B14, (1u << 16) | (1u << 24));
        for (z = 0; z < 4; z++) {
            for (k = 0; k < 4; k++) {
                vtx(k, nx(k == 1 || k == 2 ? 12 : 8), ny(k >= 2 ? 12 : 8), 0.5f, 0xFFFFFFFFu,
                    0.5f, 0.5f);
                putf(VB + 32 * k + 24, ((float)z + 0.5f) / 4.0f);
            }
            clear_black();
            draw(8, 0, 4);
            readback();
            c[z] = px(10, 10);
        }
        printf("9a. volume, no combiners: slices %06X %06X %06X %06X\n", c[0], c[1], c[2], c[3]);
        for (z = 0; z < 4; z++)
            CHECK(c[z] == (slice[z] & 0xFFFFFFu));
        CHECK(nv2a_gap_count("volume texture") == 0);
    }
    {
        /* 8. A cube map (combiners, stage 0 CUBEMAP, final combiner = the
         * texel): 4x4 faces, one colour each, 128 bytes apart. */
        static const uint32_t face[6] = { 0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu,
                                          0xFFFFFF00u, 0xFFFFFFFFu, 0xFF00FFFFu };
        static const float dir[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 },
                                         { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
        uint32_t f, i, k, c[6], bad = 0;
        for (f = 0; f < 6; f++)
            for (i = 0; i < 16; i++)
                put32(TEX + 128 * f + 4 * i, face[f]);
        m(0x1E60, 1);                           /* COMBINER_CONTROL: 1 stage */
        m(0x0288, 0x08);                        /* final: D = texture 0 */
        m(0x028C, 0x18u << 8);                  /* G = texture 0 alpha */
        m(0x1E70, 3);                           /* stage 0 CUBEMAP */
        m(0x1B00, TEX);
        m(0x1B04, (0x06u << 8) | (2u << 4) | (1u << 2) | (1u << 16) | (2u << 20) | (2u << 24));
        for (f = 0; f < 6; f++) {
            for (k = 0; k < 4; k++) {
                vtx(k, nx(k == 1 || k == 2 ? 12 : 8), ny(k >= 2 ? 12 : 8), 0.5f, 0xFFFFFFFFu,
                    dir[f][0], dir[f][1]);
                putf(VB + 32 * k + 24, dir[f][2]);
            }
            clear_black();
            draw(8, 0, 4);
            readback();
            c[f] = px(10, 10);
            bad += c[f] != (face[f] & 0xFFFFFFu);
        }
        printf("8. cube map: faces %06X %06X %06X %06X %06X %06X\n", c[0], c[1], c[2], c[3],
               c[4], c[5]);
        CHECK(bad == 0);
    }
    {
        /* 9b. The volume on the combiners, PROJECT3D, and with q = 2. */
        static const uint32_t slice[4] = { 0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFFFFu };
        uint32_t x, y, z, k, c;
        for (z = 0; z < 4; z++)
            for (y = 0; y < 4; y++)
                for (x = 0; x < 4; x++)
                    put32(TEX + 4 * xbox_swizzle_offset_3d(x, y, z, 4, 4, 4), slice[z]);
        m(0x1E70, 2);                           /* stage 0 PROJECT3D */
        m(0x1B04, (0x06u << 8) | (3u << 4) | (1u << 16) | (2u << 20) | (2u << 24) | (2u << 28));
        m(0x1784, 2 | (4 << 4) | (32 << 8));    /* texcoord float4: q */
        for (k = 0; k < 4; k++) {
            vtx(k, nx(k == 1 || k == 2 ? 12 : 8), ny(k >= 2 ? 12 : 8), 0.5f, 0xFFFFFFFFu,
                1.0f, 1.0f);
            putf(VB + 32 * k + 24, 2.5f / 4.0f * 2.0f);   /* r/q: slice 2 */
            putf(VB + 32 * k + 28, 2.0f);
        }
        clear_black();
        draw(8, 0, 4);
        readback();
        c = px(10, 10);
        printf("9b. volume, PROJECT3D, q 2: %06X\n", c);
        CHECK(c == 0x0000FFu);
        /* The CPU rasteriser does not sample volumes: counted there. */
        nv2a_backend_register(NULL);
        draw(8, 0, 4);
        CHECK(nv2a_gap_count("volume texture") == 1);
        nv2a_gl_install_from_env();
        m(0x1784, 2 | (2 << 4) | (32 << 8));
        m(0x1E70, 0);
        m(0x1B00, 0);
    }

    /* 10. Formats XBA-120 added, on the GPU: swizzled A8Y8 and R4G4B4A4
     * through the combiners (final: D = texel), then signed channels. */
    {
        uint32_t k, c[4];
        uint16_t v16;
        m(0x1E70, 1);                           /* stage 0 PROJECT2D */
        m(0x1784, 2 | (2 << 4) | (32 << 8));
        for (k = 0; k < 4; k++)
            vtx(k, nx(k == 1 || k == 2 ? 12 : 8), ny(k >= 2 ? 12 : 8), 0.5f, 0xFFFFFFFFu,
                k == 1 || k == 2 ? 1.0f : 0.0f, k >= 2 ? 1.0f : 0.0f);
        m(0x1B00, TEX);
        m(0x1B08, 0x0303);
        m(0x1B0C, 1u << 30);
        v16 = 0xFF60;                           /* A8Y8: A 0xFF, Y 0x60 */
        for (k = 0; k < 4; k++)
            memcpy(s_mem + TEX + 2 * k, &v16, 2);
        m(0x1B04, (0x1Au << 8) | (2u << 4) | (1u << 16) | (1u << 20) | (1u << 24));
        m(0x1B14, (1u << 16) | (1u << 24));
        clear_black();
        draw(8, 0, 4);
        readback();
        c[0] = px(10, 10);
        v16 = 0x48CF;                           /* R4G4B4A4: 4, 8, C, F */
        for (k = 0; k < 4; k++)
            memcpy(s_mem + TEX + 2 * k, &v16, 2);
        m(0x1B04, (0x39u << 8) | (2u << 4) | (1u << 16) | (1u << 20) | (1u << 24));
        draw(8, 0, 4);
        readback();
        c[1] = px(10, 10);
        /* A8R8G8B8 0xFFC04080 with R, G, B signed: R -64/127 and B -128/127
         * clamp to 0 at the final combiner, G 64/127 reads 0x80. */
        for (k = 0; k < 4; k++)
            put32(TEX + 4 * k, 0xFFC04080u);
        m(0x1B04, (0x06u << 8) | (2u << 4) | (1u << 16) | (1u << 20) | (1u << 24));
        draw(8, 0, 4);
        readback();
        c[2] = px(10, 10);
        m(0x1B14, (1u << 16) | (1u << 24) | (0xEu << 28));
        draw(8, 0, 4);
        readback();
        c[3] = px(10, 10);
        printf("10. A8Y8 %06X, R4G4B4A4 %06X, A8R8G8B8 %06X, the same with R, G, B signed %06X\n",
               c[0], c[1], c[2], c[3]);
        CHECK(c[0] == 0x606060u && c[1] == 0x4488CCu && c[2] == 0xC04080u);
        CHECK((c[3] & 0xFF00FFu) == 0 && ((c[3] >> 8) & 0xFF) >= 0x80 && ((c[3] >> 8) & 0xFF) <= 0x81);
        CHECK(nv2a_gap_count("texture format 0x1A SZ_A8Y8") == 0);
        m(0x1B14, (1u << 16) | (1u << 24));
        m(0x1E70, 0);
        m(0x1B00, 0);
    }

    /* 14. Stage modes 8 and up (XBA-311) and bump-env, the CPU rasteriser
     * against the GL back end: the same batch drawn both ways over a 16x16
     * quad, every pixel compared. Stage 0 reads a one-colour normal N;
     * stages 1-3 get their own float4 coordinates (v10-v12), X and Y
     * running 0..1 across the quad. The final combiner shows one stage. */
    {
        enum { VB2 = VB + 0x1000, ST = 80, NTEX = TEX, GTEX = TEX + 0x100, CTEX = TEX + 0x200 };
        static const uint32_t face[6] = { 0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu,
                                          0xFFFFFF00u, 0xFFFFFFFFu, 0xFF00FFFFu };
        static const struct {
            const char *name;
            uint32_t prog, map, n, show;          /* stage modes, dot mapping, N, stage shown */
            float tc[3][4][3];                    /* stages 1-3: s t r q = base + X dx + Y dy */
            int cube, bump;                       /* the stage holding the cube (0: none) */
            uint32_t want;                        /* pixel (10, 10) by hand, or 0 */
        } cases[] = {
            /* map 3: N (-1, 0, 1); dot1 = X, dot2 = Y: texel (X * 4, Y * 4). */
            { "DOT_ST", 0x1 | 0x11 << 5 | 0x09 << 10, 0x33, 0xFF81007Fu, 2,
              { { { 0.3f, 0, 0 }, { 0.9f, 0, 0 }, { 0.3f, 1, 0 }, { 1, 0, 0 } },
                { { 0.3f, 0, 0 }, { 0.9f, 0, 0 }, { 0.3f, 0, 1 }, { 1, 0, 0 } } }, 0, 0,
              0x141400u },
            /* The same with only SET_DOT_RGBMAPPING changed (0: N as it is):
             * another picture (checked below), the same on both paths. */
            { "DOT_ST map 0", 0x1 | 0x11 << 5 | 0x09 << 10, 0x00, 0xFF81007Fu, 2,
              { { { 0.3f, 0, 0 }, { 0.9f, 0, 0 }, { 0.3f, 1, 0 }, { 1, 0, 0 } },
                { { 0.3f, 0, 0 }, { 0.9f, 0, 0 }, { 0.3f, 0, 1 }, { 1, 0, 0 } } }, 0, 0, 0 },
            /* A 0x60, R 0xE0: texel (1, 3). */
            { "DPNDNT_AR", 0x1 | 0x0F << 5, 0, 0x60E020A0u, 1, { { { 0 } } }, 0, 0, 0x3C8C00u },
            { "DPNDNT_GB", 0x1 | 0x10 << 5, 0, 0x60E020A0u, 1, { { { 0 } } }, 0, 0, 0x146400u },
            /* map 0, N (0, 1, 0): each dot is the stage's t. Directions
             * across the quad cross every face but -Z. */
            { "DOT_STR_CUBE", 0x1 | 0x11 << 5 | 0x11 << 10 | 0x0E << 15, 0, 0xFF00FF00u, 3,
              { { { 0.77f, 0, 0 }, { -1, 2, 0 }, { -0.4f, 0, 0 }, { 1, 0, 0 } },
                { { 0.77f, 0, 0 }, { -0.9f, 0, 1.7f }, { -0.4f, 0, 0 }, { 1, 0, 0 } },
                { { 0.77f, 0, 0 }, { 0.45f, 0, 0 }, { -0.4f, 0, 0 }, { 1, 0, 0 } } }, 3, 0, 0 },
            /* Eye from the q: E (0.3, 0.2 + Y, 1). */
            { "DOT_RFLCT_SPEC", 0x1 | 0x11 << 5 | 0x11 << 10 | 0x0C << 15, 0, 0xFF00FF00u, 3,
              { { { 0.77f, 0, 0 }, { -1, 2, 0 }, { -0.4f, 0, 0 }, { 0.3f, 0, 0 } },
                { { 0.77f, 0, 0 }, { -0.9f, 0, 1.7f }, { -0.4f, 0, 0 }, { 0.2f, 0, 1 } },
                { { 0.77f, 0, 0 }, { 0.45f, 0, 0 }, { -0.4f, 0, 0 }, { 1, 0, 0 } } }, 3, 0, 0 },
            /* The eye from SET_EYE_VECTOR (0.9, 0.2, -0.5), the q ignored. */
            { "DOT_RFLCT_SPEC_CONST", 0x1 | 0x11 << 5 | 0x11 << 10 | 0x12 << 15, 0, 0xFF00FF00u, 3,
              { { { 0.77f, 0, 0 }, { -1, 2, 0 }, { -0.4f, 0, 0 }, { 0.3f, 0, 0 } },
                { { 0.77f, 0, 0 }, { -0.9f, 0, 1.7f }, { -0.4f, 0, 0 }, { 0.2f, 0, 1 } },
                { { 0.77f, 0, 0 }, { 0.45f, 0, 0 }, { -0.4f, 0, 0 }, { 1, 0, 0 } } }, 3, 0, 0 },
            /* Stage 2 looks up (dot1, dot2, stage 3's dot). */
            { "DOT_RFLCT_DIFF", 0x1 | 0x11 << 5 | 0x0B << 10 | 0x0C << 15, 0, 0xFF00FF00u, 2,
              { { { 0.77f, 0, 0 }, { -1, 2, 0 }, { -0.4f, 0, 0 }, { 1, 0, 0 } },
                { { 0.77f, 0, 0 }, { -0.9f, 0, 1.7f }, { -0.4f, 0, 0 }, { 1, 0, 0 } },
                { { 0.77f, 0, 0 }, { -0.45f, 0, 0 }, { -0.4f, 0, 0 }, { 1, 0, 0 } } }, 2, 0, 0 },
            /* Bump-env: dU 1 (B 0x7F), dV -1 (G 0x81), matrix 0.25: one
             * texel right and up of the stage's own (X, Y). */
            { "BUMPENVMAP", 0x1 | 0x06 << 5, 0, 0xFF00817Fu, 1,
              { { { 0, 1, 0 }, { 0, 0, 1 }, { 0, 0, 0 }, { 1, 0, 0 } } }, 0, 1, 0 },
        };
        static uint32_t gl_px[16 * 16], first[16 * 16];
        uint32_t c, k, st, x, y;
        int total_bad = 0;

        m(0x1E60, 1);                           /* COMBINER_CONTROL: 1 stage */
        m(0x0288, 0);
        m(0x028C, 0x30u << 8);                  /* G = 1 */
        /* Vertices: float3, D3DCOLOR, then t0..t3 as float4, 80 bytes. */
        m(0x1720, VB2);      m(0x1760, 2 | (3 << 4) | (ST << 8));
        m(0x172C, VB2 + 12); m(0x176C, 0 | (4 << 4) | (ST << 8));
        for (st = 0; st < 4; st++) {
            m(0x1744 + 4 * st, VB2 + 16 + 16 * st);
            m(0x1784 + 4 * st, 2 | (4 << 4) | (ST << 8));
        }
        /* Textures: N 2x2, the 4x4 gradient (texel (x, y) is 40x + 20,
         * 40y + 20, 0), a cube of 4x4 faces 128 bytes apart. */
        for (y = 0; y < 4; y++)
            for (x = 0; x < 4; x++)
                put32(GTEX + 4 * xbox_swizzle_offset_3d(x, y, 0, 4, 4, 1),
                      0xFF000000u | (40 * x + 20) << 16 | (40 * y + 20) << 8);
        for (k = 0; k < 6; k++)
            for (x = 0; x < 16; x++)
                put32(CTEX + 128 * k + 4 * x, face[k]);
        for (c = 0; c < sizeof cases / sizeof cases[0]; c++) {
            uint32_t bad = 0, pass;
            for (k = 0; k < 4; k++)
                put32(NTEX + 4 * k, cases[c].n);
            for (st = 0; st < 4; st++) {
                uint32_t b = 0x40 * st, cube = cases[c].cube && cases[c].cube == (int)st;
                if (st == 0) {
                    m(0x1B00, NTEX);
                    m(0x1B04, (0x06u << 8) | (2u << 4) | (1u << 16) | (1u << 20) | (1u << 24));
                } else if (cube && cases[c].cube) {
                    m(0x1B00 + b, CTEX);
                    m(0x1B04 + b, (0x06u << 8) | (2u << 4) | (1u << 2) | (1u << 16) | (2u << 20)
                                  | (2u << 24));
                } else {
                    m(0x1B00 + b, GTEX);
                    m(0x1B04 + b, (0x06u << 8) | (2u << 4) | (1u << 16) | (2u << 20) | (2u << 24));
                }
                m(0x1B08 + b, 0x0303);          /* clamp */
                m(0x1B0C + b, 1u << 30);
                m(0x1B14 + b, (1u << 16) | (1u << 24));   /* nearest */
            }
            if (cases[c].bump) {
                mf(0x1B28 + 0x40, 0.25f);       /* m00 */
                mf(0x1B2C + 0x40, 0.0f);        /* m01 */
                mf(0x1B30 + 0x40, 0.25f);       /* m11 */
                mf(0x1B34 + 0x40, 0.0f);        /* m10 */
            }
            m(0x1E70, cases[c].prog);
            m(0x1E74, cases[c].map);            /* SET_DOT_RGBMAPPING */
            m(0x1E78, 0);                       /* every stage reads stage 0 */
            mf(0x181C, 0.9f);                   /* SET_EYE_VECTOR */
            mf(0x1820, 0.2f);
            mf(0x1824, -0.5f);
            m(0x0288, 0x08u + cases[c].show);   /* final: D = the stage shown */
            for (k = 0; k < 4; k++) {
                float X = (k == 1 || k == 2) ? 1.0f : 0.0f, Y = k >= 2 ? 1.0f : 0.0f;
                uint32_t at = VB2 + ST * k, j;
                putf(at, nx(k == 1 || k == 2 ? 24 : 8));
                putf(at + 4, ny(k >= 2 ? 24 : 8));
                putf(at + 8, 0.5f);
                put32(at + 12, 0xFFFFFFFFu);
                putf(at + 16, 0.5f); putf(at + 20, 0.5f); putf(at + 24, 0); putf(at + 28, 1);
                for (st = 1; st < 4; st++)
                    for (j = 0; j < 4; j++) {
                        const float *t = cases[c].tc[st - 1][j];
                        putf(at + 16 + 16 * st + 4 * j, t[0] + t[1] * X + t[2] * Y);
                    }
                if (cases[c].tc[0][3][0] == 0 && cases[c].tc[0][3][1] == 0)
                    for (st = 1; st < 4; st++)
                        putf(at + 16 + 16 * st + 12, 1.0f);   /* q 1 where none given */
            }
            for (pass = 0; pass < 2; pass++) {
                if (pass)
                    nv2a_backend_register(NULL);
                clear_black();
                draw(8, 0, 4);
                if (!pass)
                    readback();
                for (y = 0; y < 16; y++)
                    for (x = 0; x < 16; x++) {
                        uint32_t p = px(8 + x, 8 + y), q = gl_px[y * 16 + x], j;
                        if (!pass) {
                            gl_px[y * 16 + x] = p;
                            continue;
                        }
                        for (j = 0; j < 24; j += 8)
                            if (abs((int)((p >> j) & 0xFF) - (int)((q >> j) & 0xFF)) > 2) {
                                if (bad++ < 3)
                                    printf("   %s pixel %u,%u: GL %06X, CPU %06X\n",
                                           cases[c].name, x, y, q, p);
                                break;
                            }
                    }
                if (pass)
                    nv2a_gl_install_from_env();
            }
            /* Cube cases: a pixel whose direction ties two faces may land
             * on either; a handful at most. */
            if (c == 0)
                memcpy(first, gl_px, sizeof first);
            if (c == 1) {
                uint32_t same = 0;
                for (k = 0; k < 256; k++)
                    same += gl_px[k] == first[k];
                printf("14. DOT_ST, mapping 3 then 0: %u of 256 pixels the same\n", same);
                if (same > 128)
                    total_bad++;
            }
            printf("14. %s: %u of 256 pixels differ between GL and the CPU rasteriser, (10,10) %06X\n",
                   cases[c].name, bad, gl_px[2 * 16 + 2]);
            if (bad > (cases[c].cube ? 4u : 0u))
                total_bad++;
            if (cases[c].want && gl_px[2 * 16 + 2] != cases[c].want) {
                printf("   %s: (10,10) want %06X\n", cases[c].name, cases[c].want);
                total_bad++;
            }
            {
                /* Not counted as unsupported: the stage modes drawn. */
                uint32_t mo;
                for (st = 1; st < 4; st++)
                    if ((mo = (cases[c].prog >> (5 * st)) & 0x1F) != 0)
                        CHECK(nv2a_gap_count(nv2a_stage_mode_name(mo)) == 0);
            }
        }
        CHECK(total_bad == 0);
        for (st = 0; st < 4; st++) {
            m(0x1B00 + 0x40 * st, 0);
            m(0x1B0C + 0x40 * st, 0);
        }
        m(0x1E70, 0);
        m(0x1E74, 0);
        m(0x0288, 0);
        arrays();
    }

    /* 6. Nothing counted as unsupported; the CPU rasteriser draws none and
     * counts. */
    CHECK(nv2a_gap_count("points") == 0 && nv2a_gap_count("lines") == 0
          && nv2a_gap_count("line strip") == 0 && nv2a_gap_count("line loop") == 0);
    nv2a_backend_register(NULL);
    draw(1, 0, 3);
    CHECK(nv2a_gap_count("points") == 1);
    nv2a_gl_install_from_env();
    printf("6. gaps: points on the GPU 0, on the CPU rasteriser %llu\n",
           (unsigned long long)nv2a_gap_count("points"));

    /* 11. XBA-134 item 1: the fixed-function unit as a generated vertex
     * shader (RECOMP_GPU_FFP=1), against the CPU transform: 41 random
     * states (lit with 0-8 infinite and local lights, colour material,
     * ranges; unlit; no colour; no normal), 48 vertices each, every output
     * compared; then the same lit picture drawn both ways. */
    {
        float worst = 0.0f, w;
        int t, pdiff = 0, lit_px = 0;
        uint32_t x, y, ref[W * H];
        spy_install();
        m(0x0288, 0x04);                     /* final combiner: D = diffuse */
        m(0x028C, 0x14u << 8);
        surface();
        m(0x1E70, 0);
        m(0x1B00, 0);
        for (t = 0; t < 41; t++) {        /* the last (40): lit, 4 lights */
            w = ffp_trial(t, 0);
            if (w > worst)
                worst = w;
        }
        printf("11. fixed-function shader vs CPU transform: 41 states x 48 vertices,"
               " worst difference %.2g\n", (double)worst);
        CHECK(worst < 2e-4f);
        /* The picture: the last state, drawn by each path. */
        clear_black();
        nv2a_gpu_ffp_enable(0);
        draw(5, 0, 48);
        readback();
        for (y = 0; y < H; y++)
            for (x = 0; x < W; x++)
                lit_px += (ref[y * W + x] = px(x, y)) != 0;
        clear_black();
        nv2a_gpu_ffp_enable(1);
        draw(5, 0, 48);
        readback();
        nv2a_gpu_ffp_enable(0);
        for (y = 0; y < H; y++)
            for (x = 0; x < W; x++) {
                uint32_t a = ref[y * W + x], b = px(x, y), c;
                for (c = 0; c < 24; c += 8) {
                    int d = (int)((a >> c) & 0xFF) - (int)((b >> c) & 0xFF);
                    if (d > 2 || d < -2) {
                        pdiff++;
                        break;
                    }
                }
            }
        printf("    the same picture both ways: %d of 4096 pixels differ by more than 2"
               " (%d drawn)\n", pdiff, lit_px);
        CHECK(pdiff <= 4 && lit_px > 200);
        /* XBA-306: each part the generator gained, alone (12 states each),
         * then all of them together (24): vertex blending, texgen, texture
         * matrices, specular, two-sided lighting, spot lights. d1 and the
         * four coordinate sets compared too. */
        {
            static const struct { uint32_t feat; const char *name; } part[7] = {
                { FT_TEXGEN, "texgen" }, { FT_TEXMAT, "texture matrices" },
                { FT_SKIN, "vertex blending" }, { FT_SPECULAR, "specular" },
                { FT_TWOSIDE, "two-sided" }, { FT_SPOT, "spot lights" },
                { FT_ALL, "all together" },
            };
            int j;
            for (j = 0; j < 7; j++) {
                float pw = 0.0f;
                int count = part[j].feat == FT_ALL ? 24 : 12;
                s_ft_seen = 0;
                for (t = 0; t < count; t++) {
                    w = ffp_trial(t + 3 * j, part[j].feat);
                    if (w > pw)
                        pw = w;
                }
                printf("    %s: %d states x 48 vertices, worst difference %.2g\n",
                       part[j].name, count, (double)pw);
                CHECK(pw < 1e-3f);
                CHECK(s_ft_seen == part[j].feat);     /* the part reached the shader */
            }
            ffp_trial_reset();
        }
        m(0x0314, 0);
        nv2a_gl_install_from_env();
    }

    /* 12. XBA-302: streamed vertex and index uploads, byte for byte the
     * glBufferData-per-draw picture whatever the ring does. */
    {
        static const uint32_t prog[2][4] = {
            { 0, (1u << 21) | (0u << 9) | 0x1Bu, 2u << 26, (0xFu << 12) | (1u << 11) | (0u << 3) },
            { 0, (1u << 21) | (3u << 9) | 0x1Bu, 2u << 26, (0xFu << 12) | (1u << 11) | (3u << 3) | 1u },
        };
        static const struct { int mode; size_t kb; const char *name; } run[3] = {
            { NV_GL_STREAM_RING, 4, "4 KB rings, unsynchronised maps" },
            { NV_GL_STREAM_SUB, 4, "4 KB rings, glBufferSubData" },
            { NV_GL_STREAM_RING, 0, "default rings" },
        };
        static uint32_t ref[W * H], got[W * H];
        uint32_t k, w, lit_px = 0, wraps;
        int r, j;
        spy_install();                       /* gathered vertex-program batches */
        nv2a_gpu_ffp_enable(0);
        surface();
        ffp();
        arrays();                            /* case 11 left 80-byte strides (XBA-315) */
        m(0x0308, 0);                        /* no culling */
        m(0x1E9C, 0);
        for (k = 0; k < 2; k++)
            for (w = 0; w < 4; w++)
                m(0x0B00 + 4 * w, prog[k][w]);
        m(0x1EA0, 0);
        s_strm_mode = NV_GL_STREAM_OFF;
        strm_reset(0);
        stream_scene(ref, 0);
        for (k = 0; k < W * H; k++)
            lit_px += ref[k] != 0;
        for (r = 0; r < 3; r++) {
            uint32_t diff = 0;
            s_strm_mode = run[r].mode;
            for (j = 0; j < NV_GL_NSTRM; j++)
                s_strm_size[j] = run[r].kb ? run[r].kb << 10 : (j == NV_GL_STRM_IDX ? 2u : 8u) << 20;
            strm_reset(0);
            wraps = stream_scene(got, 0);
            for (k = 0; k < W * H; k++)
                diff += got[k] != ref[k];
            printf("12. streamed uploads, %s: %u wraps, %u of 4096 pixels differ from"
                   " glBufferData per draw (%u drawn)\n", run[r].name, wraps, diff, lit_px);
            CHECK(diff == 0);
            CHECK(run[r].kb == 0 || wraps >= 10);
        }
        CHECK(lit_px > 3500);
        /* 13. XBA-312: the uniform shadow changes nothing in the picture,
         * with the fixed-function batches on the CPU (pixel uniforms) and
         * on the GPU (c[] changing in part from batch to batch). */
        for (r = 0; r < 2; r++) {
            uint32_t diff = 0, on_calls, on_skipped, off_calls, off_skipped;
            s_ushadow = 0;
            stream_scene(ref, r);
            off_calls = s_scene_uni[0];
            off_skipped = s_scene_uni[1];
            s_ushadow = 1;
            stream_scene(got, r);
            on_calls = s_scene_uni[0];
            on_skipped = s_scene_uni[1];
            for (k = 0; k < W * H; k++)
                diff += got[k] != ref[k];
            printf("13. uniform shadow, fixed function on the %s: %u calls, %u left out"
                   " (off: %u calls); %u of 4096 pixels differ\n", r ? "GPU" : "CPU",
                   on_calls, on_skipped, off_calls, diff);
            CHECK(diff == 0);
            CHECK(on_calls > 0 && on_skipped > 0 && off_skipped == 0);
            CHECK(on_calls + on_skipped == off_calls);
        }
        nv2a_gl_install_from_env();
    }

    CHECK(glGetError() == GL_NO_ERROR);
    if (!s_fail)
        printf("GL coverage (%s): ok\n", NV2A_GL_ES ? "GLES" : "GL");
    return s_fail;
}
