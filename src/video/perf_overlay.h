/* A small performance overlay drawn over the game picture by the Xbox
 * frame-buffer window (fb_present_sdl.c, SDL's renderer) and the consoles'
 * window (host_window.c, GL): FPS, frame time, draws (Xbox), the performance
 * line's figures every console shares (host_perf_line.h, XBA-243: speed, the
 * work per frame and its emulation / drawing split, the worst and slow frames,
 * pictures shown), memory, CPU, battery and temperature, and the title's
 * status line (the window title it sets, e.g. a background preparation
 * percentage, which a phone has no title bar to show). Off by default
 * everywhere; RECOMP_OVERLAY=1 turns it on (Android: Settings › Advanced ›
 * Performance overlay). */
#ifndef PERF_OVERLAY_H
#define PERF_OVERLAY_H

#include <SDL.h>
#include <stdint.h>

int  perf_overlay_enabled(void);
/* Once in a while (every ~500 ms): new figures; returns 1 when the text
 * changed, so the window redraws even without a new guest frame. now_ms on
 * the monotonic clock (the performance line's live figures are dated on it);
 * draws PERF_OVERLAY_NO_DRAWS: no draw count (the consoles). */
#define PERF_OVERLAY_NO_DRAWS 0xFFFFFFFFu
int  perf_overlay_tick(uint64_t now_ms, uint64_t flips, uint32_t draws, const char *status);
/* Inside the frame, after the game picture, before SDL_RenderPresent. */
void perf_overlay_draw(SDL_Renderer *ren);
/* The overlay as 0xAARRGGBB words, row 0 at the top, for a window that
 * draws it itself (host_window.c): NULL when there is nothing to show;
 * *serial changes whenever the pixels do. */
const uint32_t *perf_overlay_pixels(int *w, int *h, unsigned *serial);
/* The overlay's text, its lines joined with '\n' (tests). */
void perf_overlay_text(char *out, size_t n);
void perf_overlay_reset(void);   /* the renderer's textures were lost (the overlay's and the badge's) */
/* Frame generation figures for the next tick's "LSFG" line: state 0 = no
 * line, 1 = generating (real and shown fps x 100, generation time in us),
 * 2 = fallen back or kept off ("LSFG OFF"), 3 = waiting for the title's
 * lock verdict ("LSFG WAITING", WP4). */
/* The shader badge (host_shader_progress.h, XBA-314): "SHADERS 12/304" while shaders compile, at the top left of the
 * game picture, in the overlay's font; always on (not RECOMP_OVERLAY). perf_overlay_badge_update: its text at now_ms
 * (CLOCK_MONOTONIC ms, host_shader_progress_now_ms), pixels rebuilt only when it changed; returns 1 then (it
 * appeared, changed or went), so a window redraws a held picture. A few atomic loads when nothing changed: call it
 * every turn of the window's loop. perf_overlay_badge_draw: the SDL window, after the overlay; NULL from
 * perf_overlay_badge_pixels when hidden (a GL window draws it itself); perf_overlay_badge_text for the tests. */
int  perf_overlay_badge_update(uint64_t now_ms);
void perf_overlay_badge_draw(SDL_Renderer *ren);
const uint32_t *perf_overlay_badge_pixels(int *w, int *h, unsigned *serial);
void perf_overlay_badge_text(char *out, size_t n);
void perf_overlay_set_framegen(int state, uint32_t real_fps_centi, uint32_t shown_fps_centi,
                               uint32_t gen_us);

#endif
