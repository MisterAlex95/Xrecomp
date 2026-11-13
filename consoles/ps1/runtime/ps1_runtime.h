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

/* Runs a title pack of this console (XBA-147): rom is the game file,
 * pack_lib the pack's library, title_key the game's key
 * (recomp_pack_rom_identify). The app shell's loader
 * (android/app/src/main/cpp/pack_loader.c) calls it for a pack whose
 * console is "ps1". A stub for now: logs "ps1 runtime ready (...)" on
 * stdout and returns 0. */
int ps1_runtime_start(const char *rom, const char *pack_lib, const char *title_key);

#ifdef __cplusplus
}
#endif

#endif /* XBR_PS1_RUNTIME_H */
