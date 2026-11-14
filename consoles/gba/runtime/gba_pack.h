/* gba_pack -- a GBA title pack, the runtime's side (XBA-157 / XBA-158; docs/android/prepare.md « GBA packs »,
 * gba_recomp.h for the table it carries).
 *
 * tools.prepare builds <packs>/gba-<KEY>/{libtitle.so, pack.json} from the player's ROM (on the device with the
 * embedded toolchain, or on a desktop). The runtime takes the pack when it is the right one and plays the game in the
 * interpreter otherwise -- a GBA game always starts:
 *   - pack.json: format 1, console "gba", title_key = the game's key, rom_sha256 = the ROM's SHA-256, abi = this
 *     host's, shell_abi = GBA_PACK_SHELL_ABI, entry "gba_recomp_title_table", lib a plain name with lib_size bytes
 *     and lib_sha256 (hashed at each start: a GBA pack is a few MB);
 *   - the library: dlopen, the entry's table, its abi (gba_attach_recomp) and its rom_sha256 = the ROM's.
 * And the discovery loop: at exit, the ROM addresses the interpreter had to run (gba_recomp_missed) are merged into a
 * text file (one 0x%08X per line, sorted, bit 0 = Thumb) that the next Prepare hands to the lifter.
 *
 * No SDL: consoles/gba/tests checks every case. POSIX (dlopen). */
#ifndef GBA_PACK_H
#define GBA_PACK_H

#include <stddef.h>
#include <stdint.h>

#include "gba_recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The shell ABI a GBA pack is linked against: the app shell's xba_shell_abi (pack_loader.h XBA_SHELL_ABI) and
 * tools/prepare/pack.py SHELL_ABI; bumped together. */
#define GBA_PACK_SHELL_ABI 1
#define GBA_PACK_DISCOVERED_MAX 65536

enum {
    GBA_PACK_LOADED = 0, /* the table is attached */
    GBA_PACK_NONE = 1,   /* no pack.json / no library: interpreter */
    GBA_PACK_STALE = 2   /* a pack that is not this ROM's, this shell's or this host's, or does not load: interpreter */
};

/* SHA-256. */
void gba_pack_sha256(const void *data, size_t n, uint8_t out[32]);
/* Lower-case hex of a file's SHA-256 into hex (65 bytes). Returns 0, or -1 when it cannot be read. */
int gba_pack_sha256_file(const char *path, char hex[65]);
void gba_pack_hex(const uint8_t sum[32], char hex[65]);

/* pack.json's "abi" for this host: "arm64-v8a", "x86_64", "x86", "armeabi-v7a", else "unknown". */
const char *gba_pack_host_abi(void);

/* The checks on <pack_dir>/pack.json for this game (key, the ROM's hex SHA-256), this shell (shell_abi; 0 skips it)
 * and this host. Fills lib_path. Returns GBA_PACK_LOADED (all good), GBA_PACK_NONE (no pack.json) or
 * GBA_PACK_STALE with a short reason in why ("built from another ROM", "shell_abi 2, this shell is 1"). */
int gba_pack_check(const char *pack_dir, const char *key, const char *rom_sha256, long long shell_abi,
                   char *lib_path, size_t lib_path_n, char *why, size_t why_n);

typedef struct gba_pack {
    void *handle;
    const gba_recomp_table *table;
} gba_pack;

/* dlopen(lib, RTLD_NOW | RTLD_LOCAL), the table from GBA_RECOMP_ENTRY_SYMBOL, its abi and its rom_sha256 (= the
 * ROM's; NULL skips that check). Returns 0, or -1 with why (the library is closed again). */
int gba_pack_open(const char *lib, const char *rom_sha256, gba_pack *out, char *why, size_t why_n);
void gba_pack_close(gba_pack *p);

/* What gba_runtime_run does with gba_runtime_opts.pack_lib: when the library's folder has a pack.json, gba_pack_check
 * (shell_abi GBA_PACK_SHELL_ABI) first; then gba_pack_open and gba_attach_recomp. Logs one line on stdout:
 * "PACK_LOADED gba <KEY> (<N> functions)" or "PACK_STALE gba <KEY> (<why>), running interpreted". pack_lib NULL
 * or "": GBA_PACK_NONE, nothing logged. out is closed by the caller (gba_pack_close) after gba_destroy. */
int gba_pack_load(gba_machine *m, const char *pack_lib, const char *key, const char *rom_sha256, gba_pack *out);

/* Merges addrs into the discovered file path (created with its directory; through path.tmp + rename), sorted, no
 * repeats, at most GBA_PACK_DISCOVERED_MAX lines. *added (may be NULL): how many were new. Nothing new: the file is
 * not touched. Returns 0, or -1 when it cannot be written. */
int gba_pack_write_discovered(const char *path, const uint32_t *addrs, size_t n, size_t *added);

/* gba_recomp_missed(m) into path (gba_pack_write_discovered). Logs "gba: <N> address(es) the pack did not cover,
 * <A> new, in <path>" when there were any. Returns the number added, or -1. */
int gba_pack_save_missed(gba_machine *m, const char *path);

#ifdef __cplusplus
}
#endif

#endif
