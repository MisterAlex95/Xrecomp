/* The GL render back end for the pushbuffer executor (nv2a_gl.c, XBA-27). */
#ifndef XBOXRECOMP_NV2A_GL_H
#define XBOXRECOMP_NV2A_GL_H

/* With RECOMP_GPU set (and not "0"), register the GL back end with the
 * executor. Call before the title submits work, from any thread: the GL
 * context itself is created on the executor thread at its first call.
 * Returns 1 if the back end was registered. */
int nv2a_gl_install_from_env(void);

#endif
