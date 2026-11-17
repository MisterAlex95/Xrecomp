/* gc_pack -- a GameCube title pack, the runtime's side (XBA-264; docs/android/prepare.md « GameCube packs »,
 * gc_recomp.h for the table it carries). The N64's n64_pack.h with the main DOL as the source identity.
 *
 * tools.prepare builds <packs>/gc-<KEY>/{libtitle.so, pack.json, gx_shaders.txt} from the player's disc or DOL (on the
 * device with the embedded toolchain, or on a desktop: tools/prepare/gc.py). The runtime takes the pack when it is
 * the right one and plays the game in the interpreter otherwise -- a GameCube game always starts:
 *   - pack.json: format 1, console "gc", title_key = the game's key (GALE01-0, DOL-<CRC32> for a bare DOL),
 *     source_sha256 = the SHA-256 of the main DOL (a disc's, read through its header; a DOL file's whole bytes),
 *     abi = this host's, shell_abi = GC_PACK_SHELL_ABI, entry "gc_recomp_title_table", lib a plain name with lib_size
 *     bytes and lib_sha256 (hashed at each start);
 *   - the library: dlopen, the entry's table, its abi (gc_attach_recomp) and its dol_sha256 = the main DOL's.
 * The REL modules and code copied to RAM are not part of the identity: every span is content-checked in memory
 * (gc_recomp.h), so code that is not what was lifted runs in the interpreter.
 * And the discovery loop: at exit, at every pause and every few minutes the addresses the interpreter had to start at
 * and the REL modules seen loaded go to a text file (gc_recomp_write_discovered: merged without repeats), which the
 * next Prepare hands to the lifter.
 *
 * No SDL: consoles/gc/tests checks every case (gc_pack). POSIX (dlopen). */
#ifndef GC_PACK_H
#define GC_PACK_H

#include <stddef.h>
#include <stdint.h>

#include "gc_core.h"
#include "gc_recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The shell ABI a GameCube pack is linked against: the app shell's xba_shell_abi (pack_loader.h XBA_SHELL_ABI) and
 * tools/prepare/pack.py SHELL_ABI; bumped together. */
#define GC_PACK_SHELL_ABI 1

enum {
    GC_PACK_LOADED = 0, /* the table is attached */
    GC_PACK_NONE = 1,   /* no pack.json / no library: interpreter */
    GC_PACK_STALE = 2   /* a pack that is not this game's, this shell's or this host's, or does not load: interpreter */
};

/* SHA-256. */
void gc_pack_sha256(const void *data, size_t n, uint8_t out[32]);
/* Lower-case hex of a file's SHA-256 into hex (65 bytes). Returns 0, or -1 when it cannot be read. */
int gc_pack_sha256_file(const char *path, char hex[65]);
void gc_pack_hex(const uint8_t sum[32], char hex[65]);

/* pack.json's "abi" for this host: "arm64-v8a", "x86_64", "x86", "armeabi-v7a", else "unknown". */
const char *gc_pack_host_abi(void);

/* The checks on <pack_dir>/pack.json for this game (key, the main DOL's hex SHA-256), this shell (shell_abi; 0 skips
 * it) and this host. Fills lib_path. Returns GC_PACK_LOADED (all good), GC_PACK_NONE (no pack.json) or GC_PACK_STALE
 * with a short reason in why ("built from another main DOL", "shell_abi 2, this shell is 1"). */
int gc_pack_check(const char *pack_dir, const char *key, const char *source_sha256, long long shell_abi,
                  char *lib_path, size_t lib_path_n, char *why, size_t why_n);

typedef struct gc_pack {
    void *handle;
    const gc_recomp_table *table;
} gc_pack;

/* dlopen(lib, RTLD_NOW | RTLD_LOCAL), the table from GC_RECOMP_ENTRY_SYMBOL, its abi and its dol_sha256 (= the main
 * DOL's; NULL skips that check). Returns 0, or -1 with why (the library is closed again). */
int gc_pack_open(const char *lib, const char *source_sha256, gc_pack *out, char *why, size_t why_n);
void gc_pack_close(gc_pack *p);

/* What gc_runtime_run does with gc_runtime_opts.pack_lib: when the library's folder has a pack.json, gc_pack_check
 * (shell_abi GC_PACK_SHELL_ABI) first; then gc_pack_open and gc_attach_recomp. source_sha256 NULL or "": the main
 * DOL's identity is unknown, the pack is refused. Logs one line on stdout: "PACK_LOADED gc <KEY> (<N> resume points,
 * <S> spans)" or "PACK_STALE gc <KEY> (<why>), running interpreted". pack_lib NULL or "": GC_PACK_NONE, nothing
 * logged. out is closed by the caller (gc_pack_close) after gc_destroy. */
int gc_pack_load(gc_machine *m, const char *pack_lib, const char *key, const char *source_sha256, gc_pack *out);

/* gc_recomp_write_discovered(m, path), logged ("gc: <N> address(es) the pack did not cover, <R> REL module
 * record(s), <A> new line(s), in <path>") when it added any. Returns the number of lines added, or -1. */
int gc_pack_save_discovered(gc_machine *m, const char *path);

#ifdef __cplusplus
}
#endif

#endif
