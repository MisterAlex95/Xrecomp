/* n64_runtime -- the Nintendo 64 runtime (XBA-146): empty for now.
 * consoles/README.md, Plane « Spec · Other consoles ». */
#ifndef XBR_N64_RUNTIME_H
#define XBR_N64_RUNTIME_H

#ifdef __cplusplus
extern "C" {
#endif

#define N64_RUNTIME_VERSION "0.0.0"

/* "n64_runtime <version>". */
const char *n64_runtime_version(void);

#ifdef __cplusplus
}
#endif

#endif /* XBR_N64_RUNTIME_H */
