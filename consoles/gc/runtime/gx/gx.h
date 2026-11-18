/* gx.h -- the GameCube's Flipper GPU (GX) for the GameCube runtime (XBA-262, GC-4; epic XBA-219).
 *
 * Two halves:
 * - the GX core (gx_fifo.c, gx_vtx.c, gx_tex.c): the processor interface FIFO the CPU's write-gather pipe fills,
 *   the command processor (CP registers, display lists, BP / XF / CP register loads, draws), the vertex loader (every
 *   attribute format, direct and indexed, matrix indices), the transform unit (XF: matrices, lighting, texgen, run on
 *   the CPU), the pixel engine's registers (tokens, draw done, interrupts), TMEM for TLUTs, texture decoding. No GL:
 *   without a renderer the commands are parsed and acknowledged (interrupts, tokens) and nothing is drawn;
 * - the renderer (gx_gl.c, gx_shader.c): OpenGL ES 3.0 (decision of 2026-11-16 in « Decisions taken »). The EFB is a
 *   framebuffer object (internal resolution 1x..8x), TEV states become GLSL ES fragment shaders cached by state hash,
 *   with an ubershader that draws any state at once while the specialised shader compiles on a worker thread (shared
 *   EGL context), textures decoded and cached by content hash, EFB copies to textures and to the XFB in RAM (YUV),
 *   EFB peeks / pokes. GL entry points are loaded at run time (eglGetProcAddress): no build dependency.
 *
 * Threading: everything runs on the thread that runs the CPU (gc_run_frame); the GL context must be current on it.
 * See consoles/README.md « GameCube GX ». */
#ifndef GX_H
#define GX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gx_state gx_state;

/* PI interrupt causes GX raises (the processor interface's INTSR bits). */
enum { GX_INT_PE_TOKEN = 1u << 9, GX_INT_PE_FINISH = 1u << 10, GX_INT_CP = 1u << 11 };

/* irq(user, set, clear): GX asks the machine to set / clear PI interrupt causes (GX_INT_*). */
typedef void (*gx_irq_fn)(void *user, uint32_t set, uint32_t clear);

gx_state *gx_create(uint8_t *ram, uint32_t ram_size, gx_irq_fn irq, void *irq_user);
void gx_destroy(gx_state *gx);
void gx_set_verbose(gx_state *gx, int level);

/* ---- hooks for the machine's bus ---- */

/* A store to the write-gather pipe (0x0C008000..), size 1, 2 or 4 bytes, value right-aligned. */
void gx_fifo_write(gx_state *gx, uint32_t v, unsigned size);
/* The processor interface's FIFO registers: idx 3 base, 4 end, 5 write pointer (PI + 0x0C / 0x10 / 0x14). */
uint32_t gx_pi_read(gx_state *gx, unsigned idx);
void gx_pi_write(gx_state *gx, unsigned idx, uint32_t v);
/* Command processor (0x0C000000) and pixel engine (0x0C001000) registers, 16 bits, off = byte offset in the block. */
uint16_t gx_cp_read16(gx_state *gx, uint32_t off);
void gx_cp_write16(gx_state *gx, uint32_t off, uint16_t v);
uint16_t gx_pe_read16(gx_state *gx, uint32_t off);
void gx_pe_write16(gx_state *gx, uint32_t off, uint16_t v);
/* The EFB as the CPU sees it at physical 0x08000000: x in bits 2..11, y in 12..21, bit 22 = Z. 32-bit ARGB / Z24. */
uint32_t gx_efb_peek(gx_state *gx, uint32_t pa);
void gx_efb_poke(gx_state *gx, uint32_t pa, uint32_t v, unsigned size);

/* One video field went by (the machine calls it at each field end): the texture cache's content checks age by
 * field, statistics. */
void gx_field(gx_state *gx);

/* ---- renderer ---- */

typedef struct gx_gl_options {
    int scale;              /* internal resolution: 1..8 (0: RECOMP_GC_UPSCALE, else RECOMP_UPSCALE, else 1) */
    int async_shaders;      /* 1: specialised shaders compile on a worker thread with the ubershader meanwhile (default);
                               0: compile on first use (blocking); -1: ubershader only; 2: specialised only, blocking */
    int efb_copy_ram;       /* 1: EFB copies to textures are also encoded into RAM (CPU readers, reinterpretation);
                               0: texture only, RAM content tracked by hash (default; env GC_GX_EFB_COPY_RAM) */
    const char *shader_list;/* per-title list of the TEV / vertex states met (read at attach and compiled ahead,
                               written back with the new ones at gx_gl_detach / gx_destroy); NULL: none */
    const char *program_cache;/* per-title program binaries of this device's driver (gx_progcache.h, XBA-264): loaded
                               at attach (states ready before the first frame), rewritten by gx_save_program_cache /
                               gx_gl_detach when programs were added; NULL: env GC_GX_PROGRAM_CACHE, else none */
} gx_gl_options;

/* Attaches the GLES 3.0 renderer to the context current on this thread. Returns 0, or -1 (why in the log) when the
 * context lacks what it needs; GX then runs without drawing. */
int gx_gl_attach(gx_state *gx, const gx_gl_options *opt);
void gx_gl_detach(gx_state *gx);
/* Writes the per-title shader list now when it has new states (an exit hook; no GL call). */
void gx_save_shader_list(gx_state *gx);
bool gx_gl_attached(const gx_state *gx);
/* Adds the programs compiled since attach to the program binary cache and rewrites it when it grew (gx_gl_detach does
 * it too). GL calls: on the renderer's thread with its context current (a pause, the end). */
void gx_save_program_cache(gx_state *gx);

/* ---- shader warm-up (XBA-264): the per-title lists compiled ahead into the program binary cache ---- */

typedef struct gx_warmup_result {
    int states;              /* distinct TEV states in the lists */
    int cached;              /* already in the cache file, of this driver */
    int compiled;            /* compiled now and added */
    int failed;              /* would not compile or gave no binary */
    double seconds;
} gx_warmup_result;

/* Compiles every TEV state of the lists (gx_shaders.txt files; missing ones are skipped) that the cache file does not
 * hold for the current context's driver and writes it. Needs a current GLES 3 (or GL 3.3 core) context. Returns 0
 * done, 1 the driver has no program binaries (nothing written), 2 no usable context, 3 the cache cannot be written. */
int gx_gl_warmup(const char *const *lists, size_t nlists, const char *cache, gx_warmup_result *r);
/* The same on a headless EGL context of its own (gx_egl_headless_create; software: prefer llvmpipe), destroyed after:
 * the app's warm-up process and gc_run --gx-warmup. */
int gx_warmup_headless(const char *const *lists, size_t nlists, const char *cache, bool software,
                       gx_warmup_result *r);

/* The last copy to the XFB at RAM address addr (the address the video interface scans out): its GL texture at the
 * internal resolution, for a presenter that shows it without going through RAM. 0 when there is none. */
unsigned gx_xfb_texture(gx_state *gx, uint32_t addr, int *w, int *h);

/* RECOMP_GC_GX_XFB_DIAG (docs/runtime/environment-variables.md): true when the presenter should report each field. */
bool gx_xfb_diag_enabled(const gx_state *gx);
/* One diagnostic line for the field just presented. vi_addr: the address the video interface scans; path: how the
 * field reached the window; presents: the swaps it took; busy_ns: the field's work, without the pacing sleep. */
void gx_xfb_diag_field(gx_state *gx, uint32_t vi_addr, const char *path, unsigned presents, uint64_t busy_ns);

typedef struct gx_stats {
    uint64_t fifo_bytes, draws, vertices, batches, efb_copies, xfb_copies, peeks, pokes;
    uint64_t tex_decodes, tex_hits, shaders_ready, shaders_pending, uber_draws, special_draws;
    uint64_t unknown_cmds;
} gx_stats;
void gx_get_stats(const gx_state *gx, gx_stats *s);

/* ---- headless GL (gx_egl.c): a surfaceless EGL context for gc_run and the tests ---- */

/* Creates a GLES 3 context with no surface (EGL_MESA_platform_surfaceless, else the default display with
 * EGL_KHR_surfaceless_context) and makes it current. 0, or -1 with why. software: prefer Mesa's llvmpipe. */
int gx_egl_headless_create(bool software, char *why, size_t why_len);
void gx_egl_headless_destroy(void);

#ifdef __cplusplus
}
#endif

#endif
