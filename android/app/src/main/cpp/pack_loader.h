/* Title packs in the app shell (docs/android/app-shell.md « Title packs »,
 * docs/android/prepare.md « The loader »).
 *
 * The one app runs any title from a pack the on-device Prepare built:
 * <internal files dir>/packs/<TITLEID>/{libtitle.so, pack.json}. When no
 * title is linked into libmain.so, shell_main.c calls xba_pack_load() with
 * the game's default.xbe; it finds the pack for that title ID, checks it
 * (recomp_pack.h), dlopens it and routes the title's symbols:
 *
 * libmain.so defines recomp_lookup, recomp_lookup_manual,
 * fe_menu_get_pb_state and g_fe_cursor as WEAK forwarders (pack_loader.c).
 * The toolkit inside libmain.so (thread start, APCs, DPCs, the pushbuffer
 * replay) calls them; the dynamic linker bound those calls when libmain.so
 * was loaded, so a library dlopened later cannot take them over by symbol
 * interposition. Instead each forwarder calls through a pointer the loader
 * sets from dlsym on the pack (NULL until then: the stand-in result, 0).
 * g_fe_cursor is data: libmain's copy is refreshed from the pack's on each
 * fe_menu_get_pb_state call (the pushbuffer replay asks for the menu state
 * first, then reads the cursor in the same frame).
 *
 * A title linked in statically (-PxbaTitleDir, the stub APK) defines those
 * symbols STRONG: the static link picks them over the weak forwarders, and
 * shell_main.c never calls the loader (recomp_title_main is not NULL). */
#ifndef XBA_PACK_LOADER_H
#define XBA_PACK_LOADER_H

#include <stddef.h>

#include "recomp_pack.h"
#include "recomp_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The shell ABI: what a pack may link against in libmain.so. Exported, so a
 * pack (and a tool looking at libmain.so) can read it; pack.json's
 * shell_abi must equal it, and the app's Pack.SHELL_ABI too. Bump all three
 * (and tools/prepare/pack.py SHELL_ABI) together when libmain.so's exported
 * surface changes in a way packs built before cannot follow. */
#define XBA_SHELL_ABI 1
extern const int xba_shell_abi;

typedef int (*xba_title_main_fn)(const recomp_storage *store);

/* Finds, checks, dlopens the pack for the XBE at xbe_path under
 * <files_dir>/packs and installs its symbols behind the forwarders. Logs
 * one line on stdout (logcat):
 *   PACK_LOADED <tid> <lib path> (<size> bytes, "<title>", ...)
 *   PACK_MISSING <tid> (...)        no pack for this title
 *   PACK_INVALID <reason>           pack.json or the lib failed a check
 *   PACK_DLOPEN <dlerror>           dlopen or the entry's dlsym failed
 * Returns RECOMP_PACK_OK (*main_out = the pack's recomp_title_main),
 * RECOMP_PACK_MISSING, RECOMP_PACK_INVALID or RECOMP_PACK_DLOPEN, with why
 * in reason (for MISSING: the title ID). A valid pack's env_force options
 * are set before the dlopen, one "[TITLE] <tid> forces KEY=VALUE (title
 * profile)" line each (recomp_pack_apply_env_force). */
int xba_pack_load(const char *files_dir, const char *xbe_path, xba_title_main_fn *main_out,
                  char *reason, size_t reason_n);

/* The disc's other images (XBA-82, docs/technical/multi-xbe.md « Android »).
 * pack.json "images" names them; xba_pack_load writes <files>/work/<tid>/
 * images.map for the kernel (RECOMP_IMAGE_MAP) and, when <work>/image.next
 * names one (written by the last run's hand-over), loads that image's
 * library instead of default.xbe's and sets RECOMP_IMAGE ("PACK_IMAGE <path>
 * <lib>"; not in the pack: "PACK_IMAGE_MISSING", default.xbe runs).
 * xba_pack_image_xbe: that image's XBE (next to default.xbe), or NULL for
 * default.xbe. xba_pack_set_next_image: the image the next start runs (the
 * kernel's image handler, before the restart); 0 when written. */
const char *xba_pack_image_xbe(void);
int xba_pack_set_next_image(const char *target);

/* Other consoles (XBA-147): the game is a file of another console (a .gba,
 * a PS1 .bin / .cue / .iso, an N64 .z64 / .v64 / .n64), read by
 * recomp_pack_rom_identify. Its pack is <files_dir>/packs/<console>-<key>/,
 * whose pack.json says console and title_key; the loader checks it
 * (recomp_pack_find_key), sets its env_force options, and hands it to that
 * console's runtime (<console>_runtime_start, consoles/<console>/runtime),
 * whose return value goes to *runtime_rc. Logs one line on stdout:
 *   PACK_LOADED <console> <key> <lib path> (<size> bytes, "<title>")
 *   PACK_NONE <console> <key> (...)
 *   PACK_MISSING <console> <key> (...)
 *   PACK_INVALID <reason>
 * A console whose runtime plays without a pack (GBA: an interpreter,
 * XBA-157) runs the game when there is none (PACK_NONE, pack_lib NULL) and
 * that is RECOMP_PACK_OK; for the others no pack is PACK_MISSING.
 * Returns RECOMP_PACK_OK, RECOMP_PACK_MISSING or RECOMP_PACK_INVALID (not a
 * game of another console included), with why in reason.
 * The path may be "fd:N" (a document the app opened, recomp_pack_rom_identify
 * reads it from offset 0).
 * xba_pack_rom_console: the console of such a file (RECOMP_CONSOLE_GBA...),
 * or RECOMP_CONSOLE_UNKNOWN (an Xbox game, a folder, anything else). */
int xba_pack_rom_console(const char *path);
int xba_pack_load_rom(const char *files_dir, const char *rom_path, int *runtime_rc,
                      char *reason, size_t reason_n);

#ifdef __cplusplus
}
#endif

#endif /* XBA_PACK_LOADER_H */
