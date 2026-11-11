/* A small performance overlay drawn over the game picture by the SDL
 * frame-buffer window (fb_present_sdl.c): FPS, frame time, draws, memory,
 * CPU, battery and temperature, and the title's status line (the window
 * title it sets, e.g. a background preparation percentage, which a phone has
 * no title bar to show). On by default on Android; RECOMP_OVERLAY=0 turns it
 * off, =1 turns it on on a desktop. */
#ifndef PERF_OVERLAY_H
#define PERF_OVERLAY_H

#include <SDL.h>
#include <stdint.h>

int  perf_overlay_enabled(void);
/* Once in a while (every ~500 ms): new figures; returns 1 when the text
 * changed, so the window redraws even without a new guest frame. */
int  perf_overlay_tick(uint64_t now_ms, uint64_t flips, uint32_t draws, const char *status);
/* Inside the frame, after the game picture, before SDL_RenderPresent. */
void perf_overlay_draw(SDL_Renderer *ren);
void perf_overlay_reset(void);   /* the renderer's textures were lost */
/* Frame generation figures for the next tick's "LSFG" line: state 0 = no
 * line, 1 = generating (real and shown fps x 100, generation time in us),
 * 2 = fallen back ("LSFG OFF"). */
void perf_overlay_set_framegen(int state, uint32_t real_fps_centi, uint32_t shown_fps_centi,
                               uint32_t gen_us);

#endif
