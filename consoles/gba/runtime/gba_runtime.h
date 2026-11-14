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

/* Runs a title pack of this console (XBA-147): rom is the game file,
 * pack_lib the pack's library, title_key the game's key
 * (recomp_pack_rom_identify). The app shell's loader
 * (android/app/src/main/cpp/pack_loader.c) calls it for a pack whose
 * console is "gba". A stub for now: logs "gba runtime ready (...)" on
 * stdout and returns 0. */
int gba_runtime_start(const char *rom, const char *pack_lib, const char *title_key);

#ifdef __cplusplus
}
#endif

#endif /* XBR_GBA_RUNTIME_H */
