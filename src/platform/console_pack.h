/* console_pack -- what the consoles' title packs share, the runtime's side: the checks on a pack's pack.json and the
 * loading of its library (consoles/<console>/runtime/<console>_pack.c keeps the attachment to its machine and its
 * discovered code). What differs from one console to the next is in a console_pack_desc. libc + dlopen, nothing of any
 * console here. */
#ifndef CONSOLE_PACK_H
#define CONSOLE_PACK_H

#include <stddef.h>
#include <stdint.h>

#include "recomp_sha256.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The same values as each console's <CONSOLE>_PACK_LOADED / _NONE / _STALE. */
enum {
    CONSOLE_PACK_LOADED = 0, /* all good */
    CONSOLE_PACK_NONE = 1,   /* no pack.json */
    CONSOLE_PACK_STALE = 2   /* a pack that is not this game's, this shell's or this host's, or does not load */
};

/* What a console's table says, for console_pack_open. table NULL: the entry returned none. */
typedef struct console_pack_table {
    const void *table;
    uint32_t abi, count;
    const void *entries;
    const char *source_sha256;
} console_pack_table;

typedef struct console_pack_desc {
    const char *console;      /* pack.json's "console": "gba" */
    const char *source_field; /* pack.json's key for the game's identity: "rom_sha256", "source_sha256" */
    const char *source_noun;  /* that identity in the reasons: "ROM", "boot EXE", "main DOL" */
    const char *entry_symbol; /* the library's table function */
    uint32_t abi_min, abi_max;
    int abi_range;            /* 1: tables from abi_min to abi_max are taken; 0: abi_max alone */
    recomp_sha256_accel *sha; /* how this console hashes (recomp_sha256.h) */
    /* Calls the table function entry (the symbol's address) and fills out; a NULL table leaves out as it is. */
    void (*read_table)(void *entry, console_pack_table *out);
} console_pack_desc;

/* pack.json's "abi" for this host: "arm64-v8a", "x86_64", "x86", "armeabi-v7a", else "unknown". */
const char *console_pack_host_abi(void);

/* A whole text file of at most max bytes, malloc'd and NUL-terminated, its size in *n; NULL when it cannot be read or
 * is longer. */
char *console_pack_read_text(const char *path, size_t max, size_t *n);

/* The checks on <pack_dir>/pack.json for this game (key, the source's hex SHA-256), this shell (shell_abi; 0 skips it)
 * and this host. Fills lib_path. Returns CONSOLE_PACK_LOADED, CONSOLE_PACK_NONE (no pack.json) or CONSOLE_PACK_STALE
 * with a short reason in why. */
int console_pack_check(const console_pack_desc *d, const char *pack_dir, const char *key, const char *source_sha256,
                       long long shell_abi, char *lib_path, size_t lib_path_n, char *why, size_t why_n);

/* dlopen(lib, RTLD_NOW | RTLD_LOCAL), the table from d->entry_symbol, its abi and its source's SHA-256 (NULL skips that
 * check). Returns 0 with *handle and *table, or -1 with why (the library is closed again, *handle and *table untouched). */
int console_pack_open(const console_pack_desc *d, const char *lib, const char *source_sha256, void **handle,
                      const void **table, char *why, size_t why_n);
/* handle may be NULL. */
void console_pack_close(void *handle);

#ifdef __cplusplus
}
#endif

#endif
