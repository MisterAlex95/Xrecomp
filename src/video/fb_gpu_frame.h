/* Zero-copy present (XBA-308): a GPU back end hands the SDL window a frame as a texture of a GL share group both
 * contexts are in, instead of pixels read back into host memory.
 *
 * The back end (src/gpu/nv2a_gl.c) puts a pointer to an XboxFbGpuFrame in Nv2aPresentSurface.host_px with
 * host_format NV2A_HOST_FORMAT_GPU (= XBOX_FB_HOST_GPU) and host_pitch 0; the executor passes it to
 * xbox_FramebufferWindowPresentHost as it passes pixels. There, on the executor thread with the back end's context
 * current, after the lifecycle hold and under the window's present lock, the window either
 *   - asks for the picture in the texture of its back slot (render), and publishes that slot; its main thread waits
 *     for the acquire fence and draws the texture into its own GL context (fb_present_sdl.c), or
 *   - when it cannot take a texture right now (frame generation, the video overlay, a letterbox zoom: whatever needs
 *     the pixels on the CPU), asks for the pixels (a synchronous read-back) and goes on as with any host frame.
 *
 * Fences are GL sync objects (shared across a share group). acquire: inserted by the back end after the frame's last
 * command, flushed; the window waits for it before its first draw of the slot. release: inserted (and flushed) by
 * the window after each draw from a slot's texture; the back end waits for it before writing that texture again.
 * Waits are bounded on both sides (a fence of a context that went away must not hang the other). */
#ifndef XBOXRECOMP_FB_GPU_FRAME_H
#define XBOXRECOMP_FB_GPU_FRAME_H

#include <stdint.h>

/* xbox_FramebufferWindowPresentHost's format for a host_px that is an XboxFbGpuFrame (pitch 0). */
#define XBOX_FB_HOST_GPU 3u
/* The window's slots: render's slot is 0 .. XBOX_FB_GPU_SLOTS - 1, one texture each on the back end's side. */
#define XBOX_FB_GPU_SLOTS 3

typedef struct XboxFbGpuFrame {
    uint32_t w, h;          /* the picture, top row first (GL row 0 is the top) */
    /* The picture into slot's texture (w x h RGBA8, created and sized by the back end, sampled by the window with
     * its own filter): wait for and delete release (may be NULL) first; on return *acquire is the fence the window
     * waits for. Returns the texture's name, 0 on failure (the window then asks for pixels). */
    unsigned (*render)(const struct XboxFbGpuFrame *f, int slot, void *release, void **acquire);
    /* The picture in host memory: rows *pitch bytes apart, *format XBOX_FB_HOST_BGRX8888 / RGBX8888; valid until
     * the back end's flip. NULL: none. */
    const void *(*pixels)(const struct XboxFbGpuFrame *f, uint32_t *pitch, uint32_t *format);
    /* Deletes a fence the window no longer needs (one never waited for: the frame was dropped unshown). */
    void (*drop)(void *fence);
} XboxFbGpuFrame;

#endif
