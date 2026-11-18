/* xenos_glsl_run (XBA-270): runs the GLSL that x360_tools.xenos_glsl writes on a real GLES 3.0 implementation
 * (Mesa's llvmpipe through EGL's surfaceless platform, no display), for test_xenos_glsl.
 *
 *   xenos_glsl_run DIR
 *
 * For N = 0, 1, ...: DIR/N.vert with DIR/N.in (256 vec4 float constants, then 8 uint32 boolean words, then 16 vec4
 * vertex attributes, little-endian) is linked with transform feedback on its xe_o0 .. xe_o<k-1> outputs (k from
 * DIR/N.nout, a decimal number), drawn as one point with rasterisation off, and its outputs are printed as
 * "N vert" followed by 4k 32-bit hex words. DIR/N.frag is compiled (with a pass-through vertex shader, linked) and
 * printed as "N frag ok". A shader that fails prints "N error" and the log. Stops at the first N with neither. */
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void set_uniforms(GLuint p, const uint8_t *in)
{
    GLint c = glGetUniformLocation(p, "xe_c");
    if (c >= 0)
        glUniform4fv(c, 256, (const GLfloat *)in);
    GLint b = glGetUniformLocation(p, "xe_b");
    if (b >= 0)
        glUniform1uiv(b, 8, (const GLuint *)(in + 4096));
}

static void run_vertex(const char *dir, int n, const char *src)
{
    char path[4096];
    snprintf(path, sizeof path, "%s/%d.in", dir, n);
    size_t len = 0;
    uint8_t *in = (uint8_t *)slurp(path, &len);
    snprintf(path, sizeof path, "%s/%d.nout", dir, n);
    char *ns = slurp(path, NULL);
    int nout = ns ? atoi(ns) : 0;
    free(ns);
    if (!in || len < 4096 + 32 + 256 || nout < 1 || nout > 16) {
        printf("%d error\nbad %d.in / %d.nout\n", n, n, n);
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
    set_uniforms(p, in);
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
    glDeleteProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    free(in);
}

static void run_fragment(int n, const char *src)
{
    /* a vertex shader feeding sixteen interpolators, so a pixel shader with any count of xe_i links */
    static const char vs_src[] =
        "#version 300 es\n"
        "out vec4 xe_i0, xe_i1, xe_i2, xe_i3, xe_i4, xe_i5, xe_i6, xe_i7, xe_i8, xe_i9, xe_i10, xe_i11, xe_i12, "
        "xe_i13, xe_i14, xe_i15;\n"
        "void main() { xe_i0 = xe_i1 = xe_i2 = xe_i3 = xe_i4 = xe_i5 = xe_i6 = xe_i7 = xe_i8 = xe_i9 = xe_i10 = "
        "xe_i11 = xe_i12 = xe_i13 = xe_i14 = xe_i15 = vec4(0); gl_Position = vec4(0); }\n";
    GLuint vs = shader(GL_VERTEX_SHADER, vs_src, n), fs = shader(GL_FRAGMENT_SHADER, src, n);
    if (!vs || !fs)
        return;
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    if (link(p, n))
        printf("%d frag ok\n", n);
    glDeleteProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
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
            run_fragment(n, f);
        free(v);
        free(f);
    }
    eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(d, ctx);
    eglTerminate(d);
    return 0;
}
