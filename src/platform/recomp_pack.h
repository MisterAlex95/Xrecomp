/* A title pack, the loader's side (docs/android/prepare.md, « The loader »).
 *
 * The on-device Prepare builds <files dir>/packs/<TITLEID>/{libtitle.so,
 * pack.json}; the app shell (android/app/src/main/cpp/pack_loader.c) finds
 * the pack for the game's default.xbe, checks it and dlopens it. This is the
 * part of that with no Android in it, so the Linux tests cover it
 * (tests/recomp_pack):
 *   - the XBE's title ID (certificate, 8 upper-case hex digits);
 *   - pack.json, read and checked the way tools/prepare/pack.py `check` and
 *     the app's Pack.check do (format, title_id, abi, shell_abi, lib,
 *     lib_size), and its env_force options, set over recomp.cfg;
 *   - dlopen + dlsym of the symbols a pack exports, each one checked to come
 *     from the pack itself, not from a library it depends on.
 *
 * lib_sha256 is not checked here: the pack sits in the app's private storage,
 * written by prepare (which hashed it) with an atomic publish, and hashing a
 * 100+ MB library at every start costs about a second on a phone. lib_size
 * catches a truncated file. An import from external storage checks the hash.
 *
 * POSIX hosts (dlopen). */
#ifndef XBOXRECOMP_RECOMP_PACK_H
#define XBOXRECOMP_RECOMP_PACK_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RECOMP_PACK_FORMAT 1
#define RECOMP_PACK_PATH_MAX 1024
/* env_force entries a pack.json may carry (tools/prepare/pack.py ENV_FORCE_MAX). */
#define RECOMP_PACK_ENV_FORCE_MAX 16

/* Results of recomp_pack_find / recomp_pack_open. */
enum {
    RECOMP_PACK_OK = 0,
    RECOMP_PACK_MISSING = 1,   /* no packs/<TID>/pack.json */
    RECOMP_PACK_INVALID = 2,   /* pack.json unreadable, or a check failed */
    RECOMP_PACK_DLOPEN = 3     /* dlopen / dlsym failed */
};

typedef struct recomp_pack_meta {
    long long format;
    char title_id[16];
    char title[128];
    char abi[32];
    long long shell_abi;
    char lib[256];
    long long lib_size;
    char lib_sha256[72];
    char entry[64];            /* "recomp_title_main" when absent */
    unsigned have;             /* RECOMP_PACK_HAVE_* bits: fields found */
    /* "env_force": {"RECOMP_LSFG": "0", ...}, the title profile's forced
     * options (title.json). Entries past RECOMP_PACK_ENV_FORCE_MAX, or with a
     * key or value too long for the arrays, are counted in env_force_dropped. */
    int env_force_n, env_force_dropped;
    struct { char key[64]; char value[256]; } env_force[RECOMP_PACK_ENV_FORCE_MAX];
} recomp_pack_meta;

#define RECOMP_PACK_HAVE_FORMAT    0x01u
#define RECOMP_PACK_HAVE_TITLE_ID  0x02u
#define RECOMP_PACK_HAVE_ABI       0x04u
#define RECOMP_PACK_HAVE_SHELL_ABI 0x08u
#define RECOMP_PACK_HAVE_LIB       0x10u
#define RECOMP_PACK_HAVE_LIB_SIZE  0x20u
#define RECOMP_PACK_REQUIRED       0x3fu

/* The `abi` a pack for this host carries: "arm64-v8a" on arm64, "x86_64",
 * "x86", "armeabi-v7a", else "unknown". */
const char *recomp_pack_host_abi(void);

/* The title ID of an XBE image in memory / on disk, as 8 upper-case hex
 * digits in out (9 bytes). Header: "XBEH", base address at 0x104,
 * certificate address at 0x118; the title ID is at certificate + 0x08.
 * Returns 0, or -1 when it is not an XBE (or the file cannot be read). */
int recomp_pack_xbe_title_id_mem(const void *xbe, size_t len, char out[9]);
int recomp_pack_xbe_title_id(const char *path, char out[9]);

/* "4D530004", "0x4d530004", "4d530004" -> "4D530004" in out (9 bytes).
 * Returns 0, or -1 when s is not 1 to 8 hex digits. */
int recomp_pack_norm_title_id(const char *s, char out[9]);

/* Parses pack.json (a flat JSON object, plus "env_force", an object of
 * strings; unknown keys and nested values are skipped). Returns 0, or -1 with
 * why in err when it is not JSON or a known field has the wrong type. Absent
 * fields are left out of m->have. */
int recomp_pack_parse(const char *json, size_t len, recomp_pack_meta *m, char *err, size_t errn);

/* Applies m->env_force with recomp_cfg_force (recomp_cfg.h): each entry set
 * over whatever recomp.cfg or the environment said, one line each,
 * "[TITLE] <tid> forces KEY=VALUE (title profile)". Call it after recomp.cfg
 * and before anything reads those options (the window, the frame generation
 * pacer). Returns the number of options set. */
int recomp_pack_apply_env_force(const recomp_pack_meta *m);

/* The checks on a parsed pack.json for title tid, this shell's ABI and the
 * pack directory: every required field, format 1, title_id = tid, abi = the
 * host's, shell_abi, lib a plain file name, the file there and lib_size
 * bytes long. Fills lib_path (pack_dir/lib). Returns 0, or -1 with a short
 * reason ("shell_abi 0, this shell is 1"). */
int recomp_pack_validate(const recomp_pack_meta *m, const char *tid, long long shell_abi,
                         const char *pack_dir, char *lib_path, size_t lib_path_n,
                         char *reason, size_t reason_n);

/* <files_dir>/packs/<tid>/pack.json, read, parsed and validated. Returns
 * RECOMP_PACK_OK (m and lib_path filled), RECOMP_PACK_MISSING (no pack.json
 * there) or RECOMP_PACK_INVALID (reason says why). */
int recomp_pack_find(const char *files_dir, const char *tid, long long shell_abi,
                     recomp_pack_meta *m, char *lib_path, size_t lib_path_n,
                     char *reason, size_t reason_n);

/* What a pack exports. Optional ones are NULL when the pack does not define
 * them (a dlsym that resolves to a library the pack depends on -- libmain.so
 * with its own stand-ins -- counts as not defined). */
typedef struct recomp_pack_syms {
    void *handle;
    void *title_main;            /* entry (recomp_title_main), required */
    void *lookup;                /* recomp_lookup */
    void *lookup_manual;         /* recomp_lookup_manual */
    void *fe_menu_get_pb_state;
    int *fe_cursor;              /* g_fe_cursor */
} recomp_pack_syms;

/* dlopen(lib_path, RTLD_NOW | RTLD_LOCAL), then the symbols above. entry
 * NULL or "" means "recomp_title_main". Returns RECOMP_PACK_OK, or
 * RECOMP_PACK_DLOPEN with dlerror() (or "no <entry> in the pack") in err;
 * the library is closed again on failure. */
int recomp_pack_open(const char *lib_path, const char *entry, recomp_pack_syms *out,
                     char *err, size_t errn);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_RECOMP_PACK_H */
