/* gc_yuv_gl.c -- see gc_yuv_gl.h (XBA-259). */
#include "gc_yuv_gl.h"

#include "host_window.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef APIENTRY
#define APIENTRY
#endif

enum {
    XGL_TRIANGLES = 0x0004, XGL_TEXTURE_2D = 0x0DE1, XGL_UNSIGNED_BYTE = 0x1401, XGL_RGBA = 0x1908,
    XGL_RGBA8 = 0x8058, XGL_NEAREST = 0x2600, XGL_LINEAR = 0x2601, XGL_TEXTURE_MAG_FILTER = 0x2800,
    XGL_TEXTURE_MIN_FILTER = 0x2801, XGL_TEXTURE_WRAP_S = 0x2802, XGL_TEXTURE_WRAP_T = 0x2803,
    XGL_CLAMP_TO_EDGE = 0x812F, XGL_UNPACK_ROW_LENGTH = 0x0CF2, XGL_UNPACK_ALIGNMENT = 0x0CF5,
    XGL_TEXTURE0 = 0x84C0, XGL_FRAMEBUFFER = 0x8D40, XGL_COLOR_ATTACHMENT0 = 0x8CE0,
    XGL_FRAMEBUFFER_COMPLETE = 0x8CD5, XGL_FRAGMENT_SHADER = 0x8B30, XGL_VERTEX_SHADER = 0x8B31,
    XGL_COMPILE_STATUS = 0x8B81, XGL_LINK_STATUS = 0x8B82, XGL_SCISSOR_TEST = 0x0C11, XGL_BLEND = 0x0BE2,
    XGL_DEPTH_TEST = 0x0B71, XGL_CULL_FACE = 0x0B44, XGL_STENCIL_TEST = 0x0B90,
};

typedef struct gl_fns {
    void (APIENTRY *GenTextures)(int, unsigned *);
    void (APIENTRY *DeleteTextures)(int, const unsigned *);
    void (APIENTRY *BindTexture)(unsigned, unsigned);
    void (APIENTRY *TexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
    void (APIENTRY *TexSubImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
    void (APIENTRY *TexParameteri)(unsigned, unsigned, int);
    void (APIENTRY *PixelStorei)(unsigned, int);
    void (APIENTRY *ActiveTexture)(unsigned);
    void (APIENTRY *GenFramebuffers)(int, unsigned *);
    void (APIENTRY *DeleteFramebuffers)(int, const unsigned *);
    void (APIENTRY *BindFramebuffer)(unsigned, unsigned);
    void (APIENTRY *FramebufferTexture2D)(unsigned, unsigned, unsigned, unsigned, int);
    unsigned (APIENTRY *CheckFramebufferStatus)(unsigned);
    unsigned (APIENTRY *CreateShader)(unsigned);
    void (APIENTRY *ShaderSource)(unsigned, int, const char *const *, const int *);
    void (APIENTRY *CompileShader)(unsigned);
    void (APIENTRY *GetShaderiv)(unsigned, unsigned, int *);
    void (APIENTRY *GetShaderInfoLog)(unsigned, int, int *, char *);
    void (APIENTRY *DeleteShader)(unsigned);
    unsigned (APIENTRY *CreateProgram)(void);
    void (APIENTRY *AttachShader)(unsigned, unsigned);
    void (APIENTRY *LinkProgram)(unsigned);
    void (APIENTRY *GetProgramiv)(unsigned, unsigned, int *);
    void (APIENTRY *GetProgramInfoLog)(unsigned, int, int *, char *);
    void (APIENTRY *DeleteProgram)(unsigned);
    void (APIENTRY *UseProgram)(unsigned);
    int (APIENTRY *GetUniformLocation)(unsigned, const char *);
    void (APIENTRY *Uniform1i)(int, int);
    void (APIENTRY *GenVertexArrays)(int, unsigned *);
    void (APIENTRY *DeleteVertexArrays)(int, const unsigned *);
    void (APIENTRY *BindVertexArray)(unsigned);
    void (APIENTRY *DrawArrays)(unsigned, int, int);
    void (APIENTRY *Viewport)(int, int, int, int);
    void (APIENTRY *Disable)(unsigned);
    void (APIENTRY *ColorMask)(unsigned char, unsigned char, unsigned char, unsigned char);
    unsigned (APIENTRY *GetError)(void);
} gl_fns;

struct gc_yuv_gl {
    gl_fns gl;
    unsigned prog, vao, src, dst, fbo;
    int u_tex;
    int w, h;   /* the textures' size (picture pixels) */
    int filter;
};

/* A full-screen triangle from gl_VertexID; each output pixel takes its texel pair's Y and the pair's U / V. */
static const char k_vs[] =
    "void main() {\n"
    "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";
static const char k_fs[] =
    "uniform sampler2D u_tex;\n"
    "out vec4 o_color;\n"
    "void main() {\n"
    "    ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "    vec4 t = texelFetch(u_tex, ivec2(p.x >> 1, p.y), 0) * 255.0;\n"
    "    float y = (p.x & 1) == 0 ? t.r : t.b;\n"
    "    float u = t.g - 128.0, v = t.a - 128.0;\n"
    "    float c = 1.164383 * (y - 16.0);\n"
    "    vec3 rgb = vec3(c + 1.596027 * v, c - 0.391762 * u - 0.812968 * v, c + 2.017232 * u);\n"
    "    o_color = vec4(clamp(rgb / 255.0, 0.0, 1.0), 1.0);\n"
    "}\n";

static int load(gl_fns *gl)
{
#define F(name)                                                                                                       \
    do {                                                                                                              \
        *(void **)&gl->name = host_window_gl_proc("gl" #name);                                                        \
        if (!gl->name) {                                                                                              \
            fprintf(stderr, "gc: GPU YUV: no gl" #name "\n");                                                         \
            return 0;                                                                                                 \
        }                                                                                                             \
    } while (0)
    F(GenTextures); F(DeleteTextures); F(BindTexture); F(TexImage2D); F(TexSubImage2D); F(TexParameteri);
    F(PixelStorei); F(ActiveTexture); F(GenFramebuffers); F(DeleteFramebuffers); F(BindFramebuffer);
    F(FramebufferTexture2D); F(CheckFramebufferStatus); F(CreateShader); F(ShaderSource); F(CompileShader);
    F(GetShaderiv); F(GetShaderInfoLog); F(DeleteShader); F(CreateProgram); F(AttachShader); F(LinkProgram);
    F(GetProgramiv); F(GetProgramInfoLog); F(DeleteProgram); F(UseProgram); F(GetUniformLocation); F(Uniform1i);
    F(GenVertexArrays); F(DeleteVertexArrays); F(BindVertexArray); F(DrawArrays); F(Viewport); F(Disable); F(ColorMask);
    F(GetError);
#undef F
    return 1;
}

static unsigned shader(gl_fns *gl, unsigned kind, const char *head, const char *body)
{
    unsigned s = gl->CreateShader(kind);
    const char *src[2] = {head, body};
    int ok = 0;
    gl->ShaderSource(s, 2, src, NULL);
    gl->CompileShader(s);
    gl->GetShaderiv(s, XGL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = "";
        gl->GetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "gc: GPU YUV: %s shader: %s\n", kind == XGL_VERTEX_SHADER ? "vertex" : "fragment", log);
        gl->DeleteShader(s);
        return 0;
    }
    return s;
}

gc_yuv_gl *gc_yuv_gl_create(int filter)
{
    gc_yuv_gl *g = calloc(1, sizeof *g);
    if (!g || !load(&g->gl)) {
        free(g);
        return NULL;
    }
    gl_fns *gl = &g->gl;
    HostWindowInfo info;
    host_window_info(&info);
    const char *head = info.gles ? "#version 300 es\nprecision highp float;\nprecision highp int;\n"
                                 : "#version 150\n";
    unsigned vs = shader(gl, XGL_VERTEX_SHADER, head, k_vs), fs = vs ? shader(gl, XGL_FRAGMENT_SHADER, head, k_fs) : 0;
    if (!fs) {
        if (vs)
            gl->DeleteShader(vs);
        free(g);
        return NULL;
    }
    g->prog = gl->CreateProgram();
    gl->AttachShader(g->prog, vs);
    gl->AttachShader(g->prog, fs);
    gl->LinkProgram(g->prog);
    gl->DeleteShader(vs);
    gl->DeleteShader(fs);
    int ok = 0;
    gl->GetProgramiv(g->prog, XGL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = "";
        gl->GetProgramInfoLog(g->prog, sizeof log, NULL, log);
        fprintf(stderr, "gc: GPU YUV: link: %s\n", log);
        gl->DeleteProgram(g->prog);
        free(g);
        return NULL;
    }
    g->u_tex = gl->GetUniformLocation(g->prog, "u_tex");
    gl->GenVertexArrays(1, &g->vao);
    gl->GenTextures(1, &g->src);
    gl->GenTextures(1, &g->dst);
    gl->GenFramebuffers(1, &g->fbo);
    g->filter = filter;
    gl->GetError(); /* clear */
    return g;
}

static void tex_params(gl_fns *gl, int filter)
{
    gl->TexParameteri(XGL_TEXTURE_2D, XGL_TEXTURE_MIN_FILTER, filter);
    gl->TexParameteri(XGL_TEXTURE_2D, XGL_TEXTURE_MAG_FILTER, filter);
    gl->TexParameteri(XGL_TEXTURE_2D, XGL_TEXTURE_WRAP_S, XGL_CLAMP_TO_EDGE);
    gl->TexParameteri(XGL_TEXTURE_2D, XGL_TEXTURE_WRAP_T, XGL_CLAMP_TO_EDGE);
}

int gc_yuv_gl_present(gc_yuv_gl *g, const uint8_t *yuv, int w, int h)
{
    gl_fns *gl = &g->gl;
    if (w < 2 || h < 1)
        return 0;
    gl->ActiveTexture(XGL_TEXTURE0);
    gl->PixelStorei(XGL_UNPACK_ALIGNMENT, 4);
    gl->PixelStorei(XGL_UNPACK_ROW_LENGTH, 0);
    gl->BindTexture(XGL_TEXTURE_2D, g->src);
    if (w != g->w || h != g->h) {
        /* the XFB as RGBA8 texels of two pixels each (Y0 U Y1 V), and the picture it becomes */
        tex_params(gl, XGL_NEAREST);
        gl->TexImage2D(XGL_TEXTURE_2D, 0, (int)XGL_RGBA8, w / 2, h, 0, XGL_RGBA, XGL_UNSIGNED_BYTE, yuv);
        gl->BindTexture(XGL_TEXTURE_2D, g->dst);
        tex_params(gl, g->filter == HOST_FILTER_NEAREST ? XGL_NEAREST : XGL_LINEAR);
        gl->TexImage2D(XGL_TEXTURE_2D, 0, (int)XGL_RGBA8, w, h, 0, XGL_RGBA, XGL_UNSIGNED_BYTE, NULL);
        gl->BindFramebuffer(XGL_FRAMEBUFFER, g->fbo);
        gl->FramebufferTexture2D(XGL_FRAMEBUFFER, XGL_COLOR_ATTACHMENT0, XGL_TEXTURE_2D, g->dst, 0);
        unsigned st = gl->CheckFramebufferStatus(XGL_FRAMEBUFFER);
        if (st != XGL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "gc: GPU YUV: framebuffer incomplete (0x%04x)\n", st);
            gl->BindFramebuffer(XGL_FRAMEBUFFER, 0);
            return 0;
        }
        g->w = w;
        g->h = h;
        gl->BindTexture(XGL_TEXTURE_2D, g->src);
    } else {
        gl->TexSubImage2D(XGL_TEXTURE_2D, 0, 0, 0, w / 2, h, XGL_RGBA, XGL_UNSIGNED_BYTE, yuv);
        gl->BindFramebuffer(XGL_FRAMEBUFFER, g->fbo);
    }
    /* line 0 of the XFB into row 0 of the picture: host_window_present_texture's top_down */
    /* GX draws on this context (XBA-262) and leaves its own state: culling, stencil, a color mask from its last
     * clear; any of them would keep this triangle from being drawn */
    gl->Disable(XGL_SCISSOR_TEST);
    gl->Disable(XGL_BLEND);
    gl->Disable(XGL_DEPTH_TEST);
    gl->Disable(XGL_CULL_FACE);
    gl->Disable(XGL_STENCIL_TEST);
    gl->ColorMask(1, 1, 1, 1);
    gl->Viewport(0, 0, w, h);
    gl->UseProgram(g->prog);
    gl->Uniform1i(g->u_tex, 0);
    gl->BindVertexArray(g->vao);
    gl->DrawArrays(XGL_TRIANGLES, 0, 3);
    gl->BindVertexArray(0);
    gl->BindFramebuffer(XGL_FRAMEBUFFER, 0);
    unsigned err = gl->GetError();
    if (err) {
        fprintf(stderr, "gc: GPU YUV: GL error 0x%04x\n", err);
        return 0;
    }
    return host_window_present_texture(g->dst, w, h, 1);
}

void gc_yuv_gl_destroy(gc_yuv_gl *g)
{
    if (!g)
        return;
    gl_fns *gl = &g->gl;
    gl->DeleteFramebuffers(1, &g->fbo);
    gl->DeleteTextures(1, &g->src);
    gl->DeleteTextures(1, &g->dst);
    gl->DeleteVertexArrays(1, &g->vao);
    gl->DeleteProgram(g->prog);
    free(g);
}
