/* The XDK switches as toolkit defaults (XBA-106).
 *
 * The switches every XDK title needs because the console works that way --
 * the one-CPU DISPATCH gate, vblanks on the display's schedule, the USB pad
 * (and the keyboard standing in for it), the AC'97 codec ready with the
 * emulated APU -- used to arrive only from the XDK profile layers
 * (profiles/xdk/{xapi,dsound,d3d8}, gone since), which tools.prepare picked
 * from the XBE's library table. A title whose table was odd (stripped,
 * renamed, an unknown build) got none of them: a black, silent, padless boot.
 *
 * recomp_xdk_defaults() sets each of them unless something already did (the
 * environment, recomp.cfg, a forced option, or a profile layer, whose
 * xba_title_configure runs first). RECOMP_XDK_DEFAULTS=0 sets none of them
 * (the A/B switch). The pushbuffer executor (RECOMP_PB_EXEC) was already a
 * default (templates/new-game main(), the Android shell); D3D's software
 * methods and the DMA walk are the D3D service's (recomp_d3d_service.h).
 *
 * Returns how many it set; prints one line naming them when any. */
#ifndef XBOXRECOMP_RECOMP_XDK_DEFAULTS_H
#define XBOXRECOMP_RECOMP_XDK_DEFAULTS_H

#ifdef __cplusplus
extern "C" {
#endif

int recomp_xdk_defaults(void);

/* The table, for tests and docs: name and value of entry i, NULL past the end. */
const char *recomp_xdk_default_name(int i, const char **value);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_RECOMP_XDK_DEFAULTS_H */
