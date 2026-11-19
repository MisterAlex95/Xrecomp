/* Shader compile progress, for the small badge at the top left of the game picture (XBA-314).
 *
 * While a renderer compiles shaders the window shows "SHADERS 12/304" (a known total: a warm-up list) or
 * "SHADERS: COMPILING... (3)" (compiles nobody announced: a first run, a new area), and nothing once done. It is
 * always on, unlike the performance overlay (RECOMP_OVERLAY), and drawn by both windows (perf_overlay.c draws it,
 * fb_present_sdl.c and host_window.c place it). Every renderer reports here:
 *
 *   Xbox      nv2a_gl.c: the program binary cache's warm-up and the program list compiled ahead (a known total),
 *             programs compiled at a draw (live)
 *   GameCube  gc_runtime.c: GX's pending / ready programs per field
 *   N64       n64_rdp_vk.cpp: the start's pipelines against the last start's count, later builds (live)
 *   Xbox 360  xenos_gl.c: programs linked at first use (live)
 *
 * A batch with a known total: host_shader_progress_expect(n) adds n programs to it, host_shader_progress_done(n)
 * marks n of them finished (compiled, loaded or failed: whatever the outcome, so the badge always closes); the batch
 * ends when done reaches the total, and the next expect starts a new one. host_shader_progress_set(done, total)
 * replaces both (a renderer that keeps its own counts; total 0 ends the batch). A live compile
 * (host_shader_progress_live) counts towards "COMPILING... (n)", shown until HOST_SHADER_PROGRESS_LINGER_MS after the
 * last one; n counts the compiles since the badge last went away. A batch in progress takes precedence.
 *
 * Plain C11 + atomics, no SDL, no console source: any thread may report, any thread may read. Clock: CLOCK_MONOTONIC
 * milliseconds (the *_at forms take it, for the tests). */
#ifndef XBOXRECOMP_HOST_SHADER_PROGRESS_H
#define XBOXRECOMP_HOST_SHADER_PROGRESS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HOST_SHADER_PROGRESS_LINGER_MS 1000u

void host_shader_progress_expect(uint32_t n);
void host_shader_progress_done(uint32_t n);
void host_shader_progress_set(uint32_t done, uint32_t total);
void host_shader_progress_live(void);
void host_shader_progress_live_at(uint64_t now_ms);
/* Everything forgotten: no batch, no live compile (a renderer torn down, its cache off). */
void host_shader_progress_reset(void);

/* The badge's text at now_ms into out (empty when nothing is compiling); returns 1 when the badge is shown. */
int host_shader_progress_text(char *out, size_t n);
int host_shader_progress_text_at(uint64_t now_ms, char *out, size_t n);

/* CLOCK_MONOTONIC in milliseconds, the clock the functions above use. */
uint64_t host_shader_progress_now_ms(void);

#ifdef __cplusplus
}
#endif

#endif
