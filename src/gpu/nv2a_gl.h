/* The GL render back end for the pushbuffer executor (nv2a_gl.c, XBA-27). */
#ifndef XBOXRECOMP_NV2A_GL_H
#define XBOXRECOMP_NV2A_GL_H

#include <stdint.h>

/* With RECOMP_GPU set (and not "0"), or unset on Android (the GL back end
 * is the default there; RECOMP_GPU=0 keeps the CPU rasteriser), register
 * the GL back end with the executor. Call before the title submits work,
 * from any thread: the GL context itself is created on the executor thread
 * at its first call, and if it cannot be made the back end unregisters and
 * the CPU rasteriser draws. Returns 1 if the back end was registered. */
int nv2a_gl_install_from_env(void);

/* The window's drawable size in real (HiDPI) pixels. The SDL presenter
 * calls it once its renderer exists, before the guest starts; a headless
 * run never does. With it the back end may hand frames to the window at
 * its own size (host present: RECOMP_GPU_PRESENT=host, or an internal
 * resolution above 1) and RECOMP_GPU_SCALE=auto fits the drawable's
 * height. Read when the GL context is created. */
void nv2a_gl_set_display_size(uint32_t w, uint32_t h);

#endif
