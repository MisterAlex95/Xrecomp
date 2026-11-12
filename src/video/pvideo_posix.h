/* The PVIDEO overlay on POSIX hosts: pvideo_posix.c (the scan-out) and
 * fb_present_sdl.c (the window it publishes through) talk through this.
 *
 * The overlay is composed at the console's 640x480 over the last frame the
 * window was given, as 0xFFRRGGBB words (XBOX_FB_HOST_BGRX8888). */
#ifndef XBOX_PVIDEO_POSIX_H
#define XBOX_PVIDEO_POSIX_H

#if !defined(_WIN32)
#include <stddef.h>
#include <stdint.h>

#define PVIDEO_W 640u
#define PVIDEO_H 480u

/* pvideo_posix.c */

/* Register the vblank hook and the RECOMP_FB_DUMP screen hook (once; from
 * xbox_HostWindowMain, before the guest starts). */
void pvideo_install(void);
/* A frame the title presented (format: SDL_PIXELFORMAT_*), with the present
 * lock held. While the overlay is up it becomes the picture under it, the
 * composition is published in its place and 1 is returned; otherwise 0 and
 * the caller presents the frame itself. */
int  pvideo_frame(const uint8_t *px, uint32_t w, uint32_t h, uint32_t pitch,
                  uint32_t format);
/* One vblank's scan-out, on the calling thread: the banks pending in
 * NV_PVIDEO_BUFFER are copied, composed, published and handed back (their
 * bits cleared); a STOP since the last call takes the overlay down. Returns
 * the number of frames published (0 or 1). The vblank hook runs it on the
 * overlay thread; tests call it directly. */
int  xbox_PvideoStep(void);
/* Tests: guest physical memory is `size` bytes at `phys` instead of the
 * contiguous window (NULL restores it). */
void xbox_PvideoTestMemory(const uint8_t *phys, size_t size);
/* Tests: the picture last composed (PVIDEO_W x PVIDEO_H) and whether the
 * overlay is up. Present lock not needed when nothing else runs. */
const uint32_t *xbox_PvideoPicture(int *up);

/* fb_present_sdl.c */

void fbwin_present_lock(void);
void fbwin_present_unlock(void);
/* w x h pixels in SDL format `format`, rows pitch bytes apart, resampled
 * (nearest) to PVIDEO_W x PVIDEO_H 0xFFRRGGBB words. */
void fbwin_to_xrgb(const uint8_t *src, uint32_t w, uint32_t h, uint32_t pitch,
                   uint32_t format, uint32_t *dst);
/* The newest frame the window has, as fbwin_to_xrgb; 0 if none. Present
 * lock held. */
int  fbwin_last_frame(uint32_t *dst);
/* Publish a composed picture as a host frame (aspect policy, frame
 * generation pacer); present lock held. 1 if a window took it. */
int  fbwin_publish_overlay(const uint32_t *px);
/* The picture as a 24-bit BMP; -1 if the file could not be written. */
int  fbwin_write_xrgb_bmp(const char *path, const uint32_t *px);
#endif

#endif
