/* present_shaders.h -- the picture shaders shared by the console window (host_window.c, XBA-195) and the Xbox
 * presenter (fb_present_sdl.c, XBA-203): presets, their RECOMP_SHADER* keys, the GLSL sources and the GL program
 * with its uniforms. src/platform/HOST_LAYER.md « Picture shaders ».
 *
 * Each preset is one fragment shader of our own (no third-party shader pack), a single pass at the output
 * resolution over one triangle that covers the viewport, so it runs after any scaling, upscale or generated frame.
 * The sources come in two dialects: GLSL ES 3.00 / GLSL 1.50 for a context of our own (host_window: GLES 3.0 or a
 * 3.2 core context, no vertex buffer, gl_VertexID), and GLSL ES 1.00 / GLSL 1.20 for SDL_Renderer's context (the
 * Xbox presenter: opengles2 or the legacy opengl renderer, a vertex attribute). The fragment bodies are the same
 * text in both; the dialect's header maps texture() and the output.
 *
 * GL entry points come from SDL_GL_GetProcAddress, loaded on the first build; every call needs the caller's GL
 * context current. */
#ifndef XBR_PRESENT_SHADERS_H
#define XBR_PRESENT_SHADERS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    HOST_SHADER_NONE = 0,
    HOST_SHADER_SHARP = 1,
    HOST_SHADER_SCANLINES = 2,
    HOST_SHADER_CRT = 3,
    HOST_SHADER_LCD = 4,
    HOST_SHADER_FXAA = 5,
    HOST_SHADER_CAS = 6,
    HOST_SHADER_COUNT = 7
};
/* Not a preset the user picks: the plain picture through the area filter, what host_window draws instead of the
 * plain program when the picture is minified (present_shader_taps). present_shader_build / _fs_src / _name take it
 * ("area"); present_shader_parse does not. */
#define PRESENT_SHADER_AREA HOST_SHADER_COUNT
typedef struct HostShader {
    int preset;      /* HOST_SHADER_* */
    int strength;    /* 0..100, -1: the preset's default */
    int curvature;   /* CRT: 0 (flat, the default)..100 */
    int ghosting;    /* LCD: 0 (none, the default)..100 */
} HostShader;

/*   present_shader_parse     "none" / "sharp" / "scanlines" / "crt" / "lcd" / "fxaa" / "cas" -> HOST_SHADER_* (case
 *                            ignored; -1 for NULL, "" or anything else)
 *   present_shader_name      the other way ("none" for an unknown preset)
 *   present_shader_strength  the strength drawn, 0..100: s->strength, or the preset's default when -1 (scanlines
 *                            50, CRT 60, LCD 50, CAS 50, none / sharp / FXAA 0)
 *   present_shader_clamp     a preset outside the list -> none; strength -1 or 0..100, curvature / ghosting 0..100
 *   present_shader_env       preset: $console_var (e.g. "RECOMP_GBA_SHADER", "RECOMP_XBOX_SHADER") when it parses,
 *                            else $RECOMP_SHADER, else def; strength $RECOMP_SHADER_STRENGTH, curvature
 *                            $RECOMP_SHADER_CURVATURE, ghosting $RECOMP_SHADER_GHOSTING (0..100, clamped; unset or
 *                            not a number: -1 / 0 / 0). console_var may be NULL. */
int present_shader_parse(const char *v);
const char *present_shader_name(int preset);
int present_shader_strength(const HostShader *s);
HostShader present_shader_clamp(const HostShader *s);
void present_shader_env(const char *console_var, int def, HostShader *out);

/* Console lines, the cells a scanline / grid is drawn per (u_cells' height):
 *   present_shader_console_cells  a console picture h lines tall, perhaps upscaled (XBA-184): h / round(h / 240),
 *                                 at least 1 (a 960-line PS1 picture: 240; a GBA's 160)
 *   present_shader_xbox_cells     an Xbox frame h lines tall (640x480 guest, or a GPU back end's multiple of it,
 *                                 RECOMP_GPU_SCALE): its guest lines are 480, 576 or 720, the first that divides h
 *                                 (else h); a standard-definition picture (480 / 576) draws one scanline per pair
 *                                 of lines, as a TV shows one field (240 / 288), a 720-line one per line */
int present_shader_console_cells(int h);
int present_shader_xbox_cells(int h);

/* Downscale-aware sampling. A picture larger than the rectangle it is drawn into (an upscaled N64 / PS1 scanout of
 * 2560 columns in a 1440-wide viewport) loses detail through one bilinear tap per output pixel: past ~1.5x a tap
 * skips texels, thin lines and text break up (nearest only drops whole columns). Each preset, and the area program
 * host_window draws instead of the plain one, averages instead the texels under the output pixel in an axis that is
 * minified by more than PRESENT_DOWNSCALE_MIN (a box filter over texel centres, exact up to 7 texels per pixel,
 * spread bilinear taps beyond); an axis that is not keeps today's single tap (the texture's filter, linear or
 * nearest).
 *   present_shader_taps      the taps per axis for a src_w x src_h picture in an out_w x out_h viewport: 1 (one
 *                            plain tap) unless enabled and src / out > PRESENT_DOWNSCALE_MIN in that axis, then
 *                            min(ceil(src / out) + 1, 8); taps[0] columns, taps[1] rows. Returns 1 when either > 1
 *   present_downscale_env    0 when $RECOMP_PRESENT_DOWNSCALE starts with '0' (off: one plain tap at any scale, the
 *                            look before), else 1 */
#define PRESENT_DOWNSCALE_MIN 1.25f
int present_shader_taps(int enabled, float src_w, float src_h, float out_w, float out_h, float taps[2]);
int present_downscale_env(void);

typedef enum {
    PRESENT_GLSL_ES300 = 0,   /* #version 300 es: host_window on GLES 3.0 */
    PRESENT_GLSL_150 = 1,     /* #version 150: host_window on a 3.2 core context */
    PRESENT_GLSL_ES100 = 2,   /* #version 100: SDL's opengles2 renderer */
    PRESENT_GLSL_120 = 3      /* #version 120: SDL's opengl renderer (a legacy / compatibility context) */
} PresentGlsl;

/* The sources, as the strings glShaderSource takes (*n of them, at most 4; *n 0 for HOST_SHADER_NONE or an unknown
 * preset). fs_body replaces the preset's body when not NULL (the fallback's test). */
void present_shader_vs_src(PresentGlsl d, const char **src, int *n);
void present_shader_fs_src(PresentGlsl d, int preset, const char *fs_body, const char **src, int *n);

/* A preset's program in one GL context. Legacy dialects (ES100 / 120) read the vertex attribute a_pos (location 0),
 * the corners (0,0) (2,0) (0,2) of the triangle; the others draw 3 vertices with no attribute. */
typedef struct PresentShaderProg {
    unsigned prog;
    int failed;   /* did not build in this context: none is drawn instead */
    int u_tex, u_prev, u_flip, u_rect, u_swap, u_src, u_out, u_cells, u_strength, u_curve, u_ghost, u_taps;
} PresentShaderProg;

/* Builds preset's program into *sp (once: a built or failed one is left as it is). $RECOMP_SHADER_TEST_FAIL naming
 * the preset builds a shader that does not compile instead. A failure is logged once per process and preset, as
 * "  <tag> picture shader <name> does not build here; drawing without one" (tag e.g. "[HOST] window:"); a success
 * as "  <tag> picture shader <name>". Returns 1 when sp->prog is usable. */
int present_shader_build(PresentShaderProg *sp, PresentGlsl d, int preset, const char *tag);
/* Deletes the program (the context still current), or forgets it (lost == 1: the context went, e.g. an Android
 * resume); either way the next build starts again. */
void present_shader_release(PresentShaderProg *sp, int lost);

/* What one draw feeds the program. */
typedef struct PresentShaderParams {
    float src_w, src_h;      /* the texture, texels */
    float out_w, out_h;      /* the viewport, pixels */
    float cells_w, cells_h;  /* console pixels the texture holds (present_shader_*_cells) */
    float rect[4];           /* legacy dialects: the texture's part shown, x0 y0 w h in 0..1 (0 0 1 1: all) */
    int flip;                /* the texture's rows are top-down (row 0 = the top) */
    int swap;                /* legacy dialects: the texels are B, G, R, A (SDL's opengles2 ARGB textures) */
    int strength;            /* 0..100, present_shader_strength */
    int curvature, ghosting; /* 0..100; ghosting 0 when there is no previous picture */
    int downscale;           /* average the texels under a minified output pixel (present_shader_taps); 0: one tap */
} PresentShaderParams;

/* glUseProgram(sp->prog) and every uniform; the caller binds the texture to unit 0 (the previous picture to unit 1),
 * sets the viewport and draws. */
void present_shader_use(const PresentShaderProg *sp, const PresentShaderParams *p);

/* Legacy dialects in SDL_Renderer's context: draws the triangle through attribute 0 from a client array and puts
 * back every GL state it touches that SDL caches (program, array buffer, attributes 0..2, viewport, scissor, blend,
 * active unit). The texture is the caller's, bound to unit 0 (SDL_GL_BindTexture). vx, vy, vw, vh: the viewport in
 * GL's window coordinates (y from the bottom). Returns the GL error after the draw (0: none). */
unsigned present_shader_draw_legacy(const PresentShaderProg *sp, const PresentShaderParams *p,
                                    int vx, int vy, int vw, int vh);

/* Reads w x h pixels of the current read framebuffer from (0, 0) into rgb (w * h * 3 bytes, row 0 at the top). */
int present_shader_read_rgb(int w, int h, unsigned char *rgb);

#ifdef __cplusplus
}
#endif

#endif
