/* xenos_glsl_run (XBA-270): runs the GLSL that x360_tools.xenos_glsl writes on a real GLES 3.0 implementation
 * (Mesa's llvmpipe through EGL's surfaceless platform, no display), for test_xenos_glsl.
 *
 *   xenos_glsl_run DIR
 *
 * DIR/N.in: 256 vec4 float constants, 8 uint32 boolean words, 16 vec4 (vertex attributes / interpolator values),
 * 32 uint32 loop constants, all little-endian. The constants go to the shader's uniform block (XeVertexConstants or
 * XePixelConstants) as one std140 buffer.
 *
 * For N = 0, 1, ...:
 * - DIR/N.vert is linked with transform feedback on its xe_o0 .. xe_o<k-1> outputs (k from DIR/N.nout), drawn as one
 *   point with rasterisation off, the attributes as the generic values of xe_v0..15; prints "N vert" and 4k hex words.
 * - DIR/N.frag runs behind a vertex shader that passes the 16 vec4 as interpolators xe_i0..15 on one point covering
 *   a 1x1 RGBA32F target cleared to a sentinel; prints "N frag" and the 4 hex words of xe_color0, or "N frag killed"
 *   when the target kept the sentinel (discard).
 * A shader that fails prints "N error" and the log. Stops at the first N with neither file. */
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IN_SIZE (4096 + 32 + 256 + 128)
#define SENTINEL (-12345.0f)

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        b = NULL;
    }
    fclose(f);
    if (b) {
        b[n] = 0;
        if (len)
            *len = (size_t)n;
    }
    return b;
}

static GLuint shader(GLenum type, const char *src, int n)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        printf("%d error\n%s\n", n, log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static int link(GLuint p, int n)
{
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        printf("%d error\n%s\n", n, log);
    }
    return ok;
}

static uint8_t *read_in(const char *dir, int n)
{
    char path[4096];
    snprintf(path, sizeof path, "%s/%d.in", dir, n);
    size_t len = 0;
    uint8_t *in = (uint8_t *)slurp(path, &len);
    if (!in || len < IN_SIZE) {
        printf("%d error\nbad %d.in\n", n, n);
        free(in);
        return NULL;
    }
    return in;
}

/* the constants as the std140 block: 256 vec4, 2 uvec4 of booleans, 8 uvec4 of loop constants */
static GLuint bind_constants(GLuint p, const uint8_t *in)
{
    uint8_t block[4096 + 32 + 128];
    memcpy(block, in, 4096 + 32);
    memcpy(block + 4096 + 32, in + 4096 + 32 + 256, 128);
    GLuint ubo;
    glGenBuffers(1, &ubo);
    glBindBuffer(GL_UNIFORM_BUFFER, ubo);
    glBufferData(GL_UNIFORM_BUFFER, sizeof block, block, GL_STATIC_DRAW);
    static const char *names[2] = {"XeVertexConstants", "XePixelConstants"};
    for (GLuint i = 0; i < 2; i++) {
        GLuint idx = glGetUniformBlockIndex(p, names[i]);
        if (idx != GL_INVALID_INDEX) {
            glUniformBlockBinding(p, idx, i);
            glBindBufferBase(GL_UNIFORM_BUFFER, i, ubo);
        }
    }
    return ubo;
}

static void run_vertex(const char *dir, int n, const char *src)
{
    char path[4096];
    uint8_t *in = read_in(dir, n);
    snprintf(path, sizeof path, "%s/%d.nout", dir, n);
    char *ns = slurp(path, NULL);
    int nout = ns ? atoi(ns) : 0;
    free(ns);
    if (!in || nout < 1 || nout > 16) {
        if (in)
            printf("%d error\nbad %d.nout\n", n, n);
        free(in);
        return;
    }
    static const char fs_src[] =
        "#version 300 es\nprecision mediump float;\nout vec4 c;\nvoid main() { c = vec4(0); }\n";
    GLuint vs = shader(GL_VERTEX_SHADER, src, n), fs = shader(GL_FRAGMENT_SHADER, fs_src, n);
    if (!vs || !fs) {
        free(in);
        return;
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    char names[16][8];
    const char *pn[16];
    for (int i = 0; i < nout; i++) {
        snprintf(names[i], sizeof names[i], "xe_o%d", i);
        pn[i] = names[i];
    }
    glTransformFeedbackVaryings(p, nout, pn, GL_INTERLEAVED_ATTRIBS);
    if (!link(p, n)) {
        free(in);
        return;
    }
    glUseProgram(p);
    GLuint ubo = bind_constants(p, in);
    for (int i = 0; i < 16; i++) {
        char an[16];
        snprintf(an, sizeof an, "xe_v%d", i);
        GLint loc = glGetAttribLocation(p, an);
        if (loc >= 0)
            glVertexAttrib4fv((GLuint)loc, (const GLfloat *)(in + 4096 + 32 + 16 * i));
    }
    GLuint buf;
    glGenBuffers(1, &buf);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buf);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, 16 * nout, NULL, GL_STATIC_READ);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, buf);
    glEnable(GL_RASTERIZER_DISCARD);
    glBeginTransformFeedback(GL_POINTS);
    glDrawArrays(GL_POINTS, 0, 1);
    glEndTransformFeedback();
    glDisable(GL_RASTERIZER_DISCARD);
    const uint32_t *out = glMapBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 16 * nout, GL_MAP_READ_BIT);
    if (!out) {
        printf("%d error\ncannot map the feedback buffer (0x%x)\n", n, glGetError());
    } else {
        printf("%d vert", n);
        for (int i = 0; i < 4 * nout; i++)
            printf(" %08x", out[i]);
        printf("\n");
        glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER);
    }
    glDeleteBuffers(1, &buf);
    glDeleteBuffers(1, &ubo);
    glDeleteProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    free(in);
}

static void run_fragment(const char *dir, int n, const char *src, GLuint float_fbo)
{
    uint8_t *in = read_in(dir, n);
    if (!in)
        return;
    static const char vs_src[] =
        "#version 300 es\n"
        "in vec4 a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15;\n"
        "out vec4 xe_i0, xe_i1, xe_i2, xe_i3, xe_i4, xe_i5, xe_i6, xe_i7, xe_i8, xe_i9, xe_i10, xe_i11, xe_i12, "
        "xe_i13, xe_i14, xe_i15;\n"
        "void main() { xe_i0 = a0; xe_i1 = a1; xe_i2 = a2; xe_i3 = a3; xe_i4 = a4; xe_i5 = a5; xe_i6 = a6; "
        "xe_i7 = a7; xe_i8 = a8; xe_i9 = a9; xe_i10 = a10; xe_i11 = a11; xe_i12 = a12; xe_i13 = a13; xe_i14 = a14; "
        "xe_i15 = a15; gl_Position = vec4(0.0, 0.0, 0.0, 1.0); gl_PointSize = 1.0; }\n";
    GLuint vs = shader(GL_VERTEX_SHADER, vs_src, n), fs = shader(GL_FRAGMENT_SHADER, src, n);
    if (!vs || !fs) {
        free(in);
        return;
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    if (!link(p, n)) {
        free(in);
        return;
    }
    glUseProgram(p);
    GLuint ubo = bind_constants(p, in);
    for (int i = 0; i < 16; i++) {
        char an[8];
        snprintf(an, sizeof an, "a%d", i);
        GLint loc = glGetAttribLocation(p, an);
        if (loc >= 0)
            glVertexAttrib4fv((GLuint)loc, (const GLfloat *)(in + 4096 + 32 + 16 * i));
    }
    if (!float_fbo) {
        printf("%d frag linked\n", n); /* no float target: compiled and linked only */
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, float_fbo);
        glViewport(0, 0, 1, 1);
        static const GLfloat sentinel[4] = {SENTINEL, SENTINEL, SENTINEL, SENTINEL};
        glClearBufferfv(GL_COLOR, 0, sentinel);
        glDrawArrays(GL_POINTS, 0, 1);
        float px[4] = {0};
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, px);
        if (px[0] == SENTINEL && px[1] == SENTINEL && px[2] == SENTINEL && px[3] == SENTINEL) {
            printf("%d frag killed\n", n);
        } else {
            uint32_t w[4];
            memcpy(w, px, sizeof w);
            printf("%d frag %08x %08x %08x %08x\n", n, w[0], w[1], w[2], w[3]);
        }
    }
    glDeleteBuffers(1, &ubo);
    glDeleteProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    free(in);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: xenos_glsl_run DIR\n");
        return 2;
    }
    PFNEGLGETPLATFORMDISPLAYEXTPROC gpd =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay d = gpd ? gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : EGL_NO_DISPLAY;
    EGLint maj, min;
    if (d == EGL_NO_DISPLAY || !eglInitialize(d, &maj, &min)) {
        fprintf(stderr, "skipped: no surfaceless EGL display\n");
        return 77;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    static const EGLint cfg_attr[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
    EGLConfig cfg;
    EGLint ncfg = 0;
    eglChooseConfig(d, cfg_attr, &cfg, 1, &ncfg);
    static const EGLint ctx_attr[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE};
    EGLContext ctx = eglCreateContext(d, ncfg ? cfg : EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attr);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        fprintf(stderr, "skipped: no GLES 3.0 context (0x%x)\n", eglGetError());
        return 77;
    }
    fprintf(stderr, "xenos_glsl_run: %s\n", (const char *)glGetString(GL_RENDERER));
    /* no surface, so no default framebuffer: draws need a complete one of their own */
    GLuint fbo, rb;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 1, 1);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "xenos_glsl_run: incomplete framebuffer\n");
        return 1;
    }
    /* pixel shaders write into a float target when the implementation can render to one */
    GLuint ffbo = 0, frb;
    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    if (ext && strstr(ext, "GL_EXT_color_buffer_float")) {
        glGenFramebuffers(1, &ffbo);
        glBindFramebuffer(GL_FRAMEBUFFER, ffbo);
        glGenRenderbuffers(1, &frb);
        glBindRenderbuffer(GL_RENDERBUFFER, frb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA32F, 1, 1);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, frb);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            ffbo = 0;
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    }
    for (int n = 0;; n++) {
        char path[4096];
        snprintf(path, sizeof path, "%s/%d.vert", argv[1], n);
        char *v = slurp(path, NULL);
        snprintf(path, sizeof path, "%s/%d.frag", argv[1], n);
        char *f = slurp(path, NULL);
        if (!v && !f)
            break;
        if (v)
            run_vertex(argv[1], n, v);
        if (f)
            run_fragment(argv[1], n, f, ffbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        free(v);
        free(f);
    }
    eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(d, ctx);
    eglTerminate(d);
    return 0;
}
