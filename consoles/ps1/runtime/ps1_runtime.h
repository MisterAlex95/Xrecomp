/* ps1_runtime -- the PlayStation runtime (XBA-146): empty for now.
 * consoles/README.md, Plane « Spec · Other consoles ». */
#ifndef XBR_PS1_RUNTIME_H
#define XBR_PS1_RUNTIME_H

#ifdef __cplusplus
extern "C" {
#endif

#define PS1_RUNTIME_VERSION "0.0.0"

/* "ps1_runtime <version>". */
const char *ps1_runtime_version(void);

#ifdef __cplusplus
}
#endif

#endif /* XBR_PS1_RUNTIME_H */
