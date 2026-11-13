/* gba_runtime -- the Game Boy Advance runtime (XBA-146): empty for now.
 * consoles/README.md, Plane « Spec · Other consoles ». */
#ifndef XBR_GBA_RUNTIME_H
#define XBR_GBA_RUNTIME_H

#ifdef __cplusplus
extern "C" {
#endif

#define GBA_RUNTIME_VERSION "0.0.0"

/* "gba_runtime <version>". */
const char *gba_runtime_version(void);

#ifdef __cplusplus
}
#endif

#endif /* XBR_GBA_RUNTIME_H */
