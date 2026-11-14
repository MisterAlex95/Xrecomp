/* ps1_pack -- a PS1 title pack, the runtime's side (XBA-168; docs/android/prepare.md « PS1 packs », ps1_recomp.h
 * for the table it carries).
 *
 * tools.prepare builds <packs>/ps1-<KEY>/{libtitle.so, pack.json} from the player's disc (on the device with the
 * embedded toolchain, or on a desktop: tools/prepare/ps1.py). The runtime takes the pack when it is the right one and
 * plays the game in the interpreter otherwise -- a PS1 game always starts:
 *   - pack.json: format 1, console "ps1", title_key = the game's key, source_sha256 = the SHA-256 of the disc's boot
 *     EXE (SYSTEM.CNF's BOOT file, else PSX.EXE; an EXE game: the file), abi = this host's, shell_abi =
 *     PS1_PACK_SHELL_ABI, entry "ps1_recomp_title_table", lib a plain name with lib_size bytes and lib_sha256 (hashed
 *     at each start);
 *   - the library: dlopen, the entry's table, its abi (ps1_attach_recomp) and its source_sha256 = the boot EXE's.
 * The source identity is the boot EXE, not the image: 2 MB at most to read from a disc of hundreds, the same for a
 * .cue / .bin, an .iso or a .chd of one disc, and what the table records itself (ps1_recomp_table.source_sha256).
 * And the discovery loop: at exit the code the interpreter had to run (ps1_recomp_write_missed: address, hash,
 * bytes) is appended to a text file without repeats (address + hash), which the next Prepare hands to the lifter.
 *
 * No SDL: consoles/ps1/tests checks every case (ps1_pack). POSIX (dlopen). */
#ifndef PS1_PACK_H
#define PS1_PACK_H

#include <stddef.h>
#include <stdint.h>

#include "ps1_recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The shell ABI a PS1 pack is linked against: the app shell's xba_shell_abi (pack_loader.h XBA_SHELL_ABI) and
 * tools/prepare/pack.py SHELL_ABI; bumped together. */
#define PS1_PACK_SHELL_ABI 1
/* The boot EXE read for the source identity: its header and at most the whole RAM (ps1_runtime.c PS1_EXE_MAX). */
#define PS1_PACK_EXE_MAX (2u * 1024 * 1024 + 0x800u)

enum {
    PS1_PACK_LOADED = 0, /* the table is attached */
    PS1_PACK_NONE = 1,   /* no pack.json / no library: interpreter */
    PS1_PACK_STALE = 2   /* a pack that is not this disc's, this shell's or this host's, or does not load: interpreter */
};

/* SHA-256. */
void ps1_pack_sha256(const void *data, size_t n, uint8_t out[32]);
/* Lower-case hex of a file's SHA-256 into hex (65 bytes). Returns 0, or -1 when it cannot be read. */
int ps1_pack_sha256_file(const char *path, char hex[65]);
void ps1_pack_hex(const uint8_t sum[32], char hex[65]);

/* pack.json's "abi" for this host: "arm64-v8a", "x86_64", "x86", "armeabi-v7a", else "unknown". */
const char *ps1_pack_host_abi(void);

/* The boot EXE of a disc image (anything ps1_disc_open reads: .cue, .bin, .iso, .chd, "fd:N"), read from ISO 9660 as
 * consoles/ps1/tools/ps1_tools/exe.py does (SYSTEM.CNF's first BOOT = line, "cdrom:" dropped; without one PSX.EXE;
 * names case-insensitive, ";1" ignored). Fills hex with its SHA-256 and boot (may be NULL) with the path it read.
 * Returns 0, or -1 with why. */
int ps1_pack_disc_boot_sha256(const char *disc, char hex[65], char *boot, size_t boot_n, char *why, size_t why_n);
/* The same boot EXE's bytes (malloc'd, the caller frees; at most 2 MB + 2 KB): the runtime's fast boot of a disc
 * (XBA-168). Returns 0, or -1 with why. */
int ps1_pack_disc_boot_exe(const char *disc, uint8_t **exe, size_t *exe_n, char *boot, size_t boot_n, char *why,
                           size_t why_n);

/* The checks on <pack_dir>/pack.json for this game (key, the boot EXE's hex SHA-256), this shell (shell_abi; 0 skips
 * it) and this host. Fills lib_path. Returns PS1_PACK_LOADED (all good), PS1_PACK_NONE (no pack.json) or
 * PS1_PACK_STALE with a short reason in why ("built from another boot EXE", "shell_abi 2, this shell is 1"). */
int ps1_pack_check(const char *pack_dir, const char *key, const char *source_sha256, long long shell_abi,
                   char *lib_path, size_t lib_path_n, char *why, size_t why_n);

typedef struct ps1_pack {
    void *handle;
    const ps1_recomp_table *table;
} ps1_pack;

/* dlopen(lib, RTLD_NOW | RTLD_LOCAL), the table from PS1_RECOMP_ENTRY_SYMBOL, its abi and its source_sha256 (= the
 * boot EXE's; NULL skips that check). Returns 0, or -1 with why (the library is closed again). */
int ps1_pack_open(const char *lib, const char *source_sha256, ps1_pack *out, char *why, size_t why_n);
void ps1_pack_close(ps1_pack *p);

/* What ps1_runtime_run does with ps1_runtime_opts.pack_lib: when the library's folder has a pack.json, ps1_pack_check
 * (shell_abi PS1_PACK_SHELL_ABI) first; then ps1_pack_open and ps1_attach_recomp. source_sha256 NULL or "": the boot
 * EXE could not be read, the pack is refused. Logs one line on stdout: "PACK_LOADED ps1 <KEY> (<N> functions)" or
 * "PACK_STALE ps1 <KEY> (<why>), running interpreted". pack_lib NULL or "": PS1_PACK_NONE, nothing logged. out is
 * closed by the caller (ps1_pack_close) after ps1_destroy. */
int ps1_pack_load(ps1_machine *m, const char *pack_lib, const char *key, const char *source_sha256, ps1_pack *out);

/* ps1_recomp_write_missed(m) into path, its directory made first. Logs "ps1: code at <N> address(es) the pack did not
 * cover, <A> new record(s), in <path>" when there were any. Returns the number of records added, or -1. */
int ps1_pack_save_missed(ps1_machine *m, const char *path);

#ifdef __cplusplus
}
#endif

#endif
