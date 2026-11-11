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

#ifdef __cplusplus
}
#endif

#endif /* XBA_PACK_LOADER_H */
