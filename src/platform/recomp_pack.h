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
 * Other consoles (XBA-147, consoles/README.md): pack.json may carry
 * "console" ("xbox" when absent, so every pack made before stays valid, same
 * format 1) and, for gba / ps1 / n64, "title_key" instead of title_id: the
 * generic title key recomp_pack_rom_identify reads from the game itself. Such
 * a pack lives in <files dir>/packs/<console>-<title_key>/; the loader hands
 * it to that console's runtime instead of dlopening it as an Xbox title.
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
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RECOMP_PACK_FORMAT 1
#define RECOMP_PACK_PATH_MAX 1024
/* env_force entries a pack.json may carry (tools/prepare/pack.py ENV_FORCE_MAX). */
#define RECOMP_PACK_ENV_FORCE_MAX 16
/* Other images of the disc a pack.json may carry (tools/prepare/pack.py). */
#define RECOMP_PACK_IMAGES_MAX 8

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
    /* "images": {"game.xbe": "libimage-game.xbe.so", ...}: the disc's other
     * images the title launches (XBA-82), each one's library in the pack
     * folder. default.xbe is "lib". Older shells skip the key. */
    int images_n, images_dropped;
    struct { char path[256]; char lib[256]; } images[RECOMP_PACK_IMAGES_MAX];
    /* XBA-147: "console" ("xbox" when absent) and, for another console,
     * "title_key" (recomp_pack_rom_identify). */
    char console[16];
    char title_key[48];
} recomp_pack_meta;

#define RECOMP_PACK_HAVE_FORMAT    0x01u
#define RECOMP_PACK_HAVE_TITLE_ID  0x02u
#define RECOMP_PACK_HAVE_ABI       0x04u
#define RECOMP_PACK_HAVE_SHELL_ABI 0x08u
#define RECOMP_PACK_HAVE_LIB       0x10u
#define RECOMP_PACK_HAVE_LIB_SIZE  0x20u
#define RECOMP_PACK_REQUIRED       0x3fu    /* an Xbox pack's */
#define RECOMP_PACK_HAVE_CONSOLE   0x40u
#define RECOMP_PACK_HAVE_TITLE_KEY 0x80u

/* The consoles a pack may be for (pack.json "console"). */
enum {
    RECOMP_CONSOLE_UNKNOWN = -1,
    RECOMP_CONSOLE_XBOX = 0,
    RECOMP_CONSOLE_GBA = 1,
    RECOMP_CONSOLE_PS1 = 2,
    RECOMP_CONSOLE_N64 = 3
};

/* "xbox", "gba", "ps1", "n64" <-> RECOMP_CONSOLE_*. NULL or "" is xbox (a
 * pack from before the field); anything else RECOMP_CONSOLE_UNKNOWN / "?". */
int recomp_pack_console_id(const char *name);
const char *recomp_pack_console_name(int console);

/* The console and generic title key of a game of another console than the
 * Xbox, from the file's content (its extension is not trusted):
 *   gba  "<game code>-<CRC32 of the file>"          AXVE-1A2B3C4D
 *        (header: 0x96 at 0xB2 and a valid complement check at 0xBD)
 *   n64  "<game code>-<header CRC1>"                NSME-635A2BFF
 *        (.z64 80371240, .v64 37804012, .n64 40123780: any byte order)
 *   ps1  "<serial>" from SYSTEM.CNF's BOOT line     SLUS-00594
 *        (ISO 9660 in 2048- or 2352-byte sectors, system ID "PLAYSTATION";
 *        no serial-shaped boot file: "PSX-<CRC32 of the volume descriptor>";
 *        a .cue: its first FILE)
 * A game code byte outside [A-Z0-9] becomes '_', so a key names a folder.
 * An Xbox image is none of these (the Xbox path reads it); that is how an
 * .iso is told apart: XISO magic vs ISO 9660 "PLAYSTATION". A CHD (MAME
 * compressed hunks, "MComprHD" at 0: a PlayStation disc) is read through the
 * disc hook below when one is installed, else it is not identified. key: 24
 * bytes or more. path may also be "fd:N", a descriptor the app shell opened
 * from a document (XBA-157): read from offset 0 through a dup, left open.
 * Returns 0, or -1 when the file is none of those. */
int recomp_pack_rom_identify(const char *path, int *console, char *key, size_t key_n);

/* A reader of the disc images recomp_pack_rom_identify cannot read itself
 * (CHD: compressed, XBA-159), so recomp_pack.c stays libc-only: whoever links
 * a disc reader installs it (the PS1 runtime's ps1_identify_install, built
 * with XBR_PS1_CHD). open: a path or "fd:N" (left open), returns a handle or
 * NULL; read: the 2048 user data bytes of data sector lba (LBA 0 = MSF
 * 00:02:00, the PVD at 16), 0 or -1; close: frees the handle. */
typedef struct recomp_pack_disc_hook {
    void *(*open)(const char *path);
    int (*read)(void *disc, uint32_t lba, unsigned char out[2048]);
    void (*close)(void *disc);
} recomp_pack_disc_hook;

/* Installs the hook (NULL removes it); the struct must outlive its use. */
void recomp_pack_set_disc_hook(const recomp_pack_disc_hook *hook);

/* The pack folder's name: "<console>-<key>" for another console, the
 * normalised title ID for xbox. Returns 0, or -1 when key is empty, longer
 * than 40 or has characters outside [A-Za-z0-9_-]. */
int recomp_pack_dir_name(int console, const char *key, char *out, size_t out_n);

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
 * pack directory: every required field, format 1, console xbox (or none),
 * title_id = tid, abi = the
 * host's, shell_abi, lib a plain file name, the file there and lib_size
 * bytes long. Fills lib_path (pack_dir/lib). Returns 0, or -1 with a short
 * reason ("shell_abi 0, this shell is 1"). */
int recomp_pack_validate(const recomp_pack_meta *m, const char *tid, long long shell_abi,
                         const char *pack_dir, char *lib_path, size_t lib_path_n,
                         char *reason, size_t reason_n);

/* The same for a pack of another console: format, console, title_key = key
 * (in place of title_id), abi, shell_abi, lib, lib_size. RECOMP_CONSOLE_XBOX
 * is recomp_pack_validate with key as the title ID. */
int recomp_pack_validate_key(const recomp_pack_meta *m, int console, const char *key, long long shell_abi,
                             const char *pack_dir, char *lib_path, size_t lib_path_n,
                             char *reason, size_t reason_n);

/* The library of image `path` (a disc path, any case, '\\' or '/', as
 * xbox_launch_rel_path gives it) in a validated pack: "default.xbe" (or
 * empty) is lib_path itself, another one an "images" entry, checked to be a
 * plain file name present in pack_dir. Fills out (pack_dir/lib) and, when
 * key is not NULL, the entry's path as pack.json spells it. Returns 0, or -1
 * with a reason (not in the pack, missing file). */
int recomp_pack_image_lib(const recomp_pack_meta *m, const char *pack_dir, const char *lib_path,
                          const char *path, char *out, size_t out_n, char *key, size_t key_n,
                          char *reason, size_t reason_n);

/* <files_dir>/packs/<tid>/pack.json, read, parsed and validated. Returns
 * RECOMP_PACK_OK (m and lib_path filled), RECOMP_PACK_MISSING (no pack.json
 * there) or RECOMP_PACK_INVALID (reason says why). */
int recomp_pack_find(const char *files_dir, const char *tid, long long shell_abi,
                     recomp_pack_meta *m, char *lib_path, size_t lib_path_n,
                     char *reason, size_t reason_n);

/* recomp_pack_find for a game of another console: <files_dir>/packs/
 * <console>-<key>/pack.json, checked with recomp_pack_validate_key. */
int recomp_pack_find_key(const char *files_dir, int console, const char *key, long long shell_abi,
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
