/* Title packs in the app shell: the forwarders and the loader
 * (pack_loader.h). No SDL, no JNI: tests/recomp_pack builds it into a fake
 * shell library on Linux and checks the routing. */
#include "pack_loader.h"
#include "gba_runtime.h"
#include "n64_runtime.h"
#include "gc_runtime.h"
#include "x360_runtime.h"
#include "ps1_runtime.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Exported (default visibility, kept by `used`): see pack_loader.h. */
__attribute__((visibility("default"), used)) const int xba_shell_abi = XBA_SHELL_ABI;

typedef void (*recomp_func_t)(void);
typedef recomp_func_t (*lookup_fn)(uint32_t va);
typedef int (*pb_state_fn)(void);

static lookup_fn s_lookup, s_lookup_manual;
/* XBA-82: <files>/work/<tid>/image.next (the image the next start runs) and
 * the XBE of the image this start runs when it is not default.xbe. */
static char s_next_path[RECOMP_PACK_PATH_MAX + 64];
static char s_image_xbe[RECOMP_PACK_PATH_MAX + 300];
/* The loaded pack's images, for the image handler (xba_pack_image_state). */
static recomp_pack_meta s_meta;
static char s_pack_dir[RECOMP_PACK_PATH_MAX], s_pack_lib[RECOMP_PACK_PATH_MAX];
static pb_state_fn s_pb_state;
static int *s_fe_cursor;

/* The four symbols a title provides (its generated dispatch and one title's
 * front-end hooks), which the toolkit in libmain.so calls. Weak: a title
 * linked in statically defines them strong and wins. Without a title they
 * forward to the loaded pack, or stand in (0) when there is none. */
__attribute__((weak)) recomp_func_t recomp_lookup(uint32_t va)
{
    return s_lookup ? s_lookup(va) : 0;
}

__attribute__((weak)) recomp_func_t recomp_lookup_manual(uint32_t va)
{
    return s_lookup_manual ? s_lookup_manual(va) : 0;
}

__attribute__((weak)) int g_fe_cursor;

__attribute__((weak)) int fe_menu_get_pb_state(void)
{
    if (s_fe_cursor)
        g_fe_cursor = *s_fe_cursor;
    return s_pb_state ? s_pb_state() : 0;
}

/* XBA-126: the game may be a disc image read in place ("fd:N/default.xbe",
 * "<image>/default.xbe"): the kernel's reader (kernel_disc.c, linked into
 * libmain.so) takes the XBE out of it. Weak, so tests/recomp_pack's fake
 * shell links without the kernel. */
extern int xbox_disc_slurp(const char *host_path, void **data, size_t *size) __attribute__((weak));

static int xbe_title_id(const char *xbe_path, char tid[9])
{
    void *data = NULL;
    size_t len = 0;
    int rc;
    if (recomp_pack_xbe_title_id(xbe_path, tid) == 0)
        return 0;
    if (!xbox_disc_slurp || !xbe_path || xbox_disc_slurp(xbe_path, &data, &len) != 0)
        return -1;
    rc = recomp_pack_xbe_title_id_mem(data, len, tid);
    free(data);
    return rc;
}

int xba_pack_load(const char *files_dir, const char *xbe_path, xba_title_main_fn *main_out,
                  char *reason, size_t reason_n)
{
    char tid[9], lib[RECOMP_PACK_PATH_MAX], why[512];
    recomp_pack_meta m;
    recomp_pack_syms syms;
    int rc;

    *main_out = NULL;
    why[0] = 0;
    if (xbe_title_id(xbe_path, tid) != 0) {
        snprintf(why, sizeof why, "cannot read the title ID of %s", xbe_path ? xbe_path : "(none)");
        printf("PACK_INVALID %s\n", why);
        rc = RECOMP_PACK_INVALID;
        goto out;
    }
    rc = recomp_pack_find(files_dir, tid, xba_shell_abi, &m, lib, sizeof lib, why, sizeof why);
    if (rc == RECOMP_PACK_MISSING) {
        snprintf(why, sizeof why, "%s", tid);
        printf("PACK_MISSING %s (no %s/packs/%s/pack.json)\n", tid, files_dir ? files_dir : "", tid);
        goto out;
    }
    if (rc != RECOMP_PACK_OK) {
        printf("PACK_INVALID %s\n", why);
        goto out;
    }
    /* The title profile's forced options (pack.json env_force): after
     * recomp.cfg (recomp_app_start), before the window and the frame
     * generation pacer read them, and before the title's own
     * xba_title_configure, which runs too late for those. */
    recomp_pack_apply_env_force(&m);
    /* What the run meets that the analysis missed (src/kernel/
     * recomp_discovery.h): in the title's Prepare work folder, where the next
     * Prepare reads it. recomp.cfg's RECOMP_DISCOVERY wins; =0 turns it off. */
    if (files_dir && !getenv("RECOMP_DISCOVERY")) {
        char disc[RECOMP_PACK_PATH_MAX + 64];
        snprintf(disc, sizeof disc, "%s/work/%s/discovered.jsonl", files_dir, tid);
        setenv("RECOMP_DISCOVERY", disc, 0);
    }
    /* The disc's other images (XBA-82): which one this start runs, and the
     * map the kernel hands a launch of another one over with. */
    if (files_dir) {
        char work[RECOMP_PACK_PATH_MAX + 32], want[300] = "", key[256] = "default.xbe";
        char pack_dir[RECOMP_PACK_PATH_MAX], img_lib[RECOMP_PACK_PATH_MAX + 300], ir[256];
        char *slash;
        FILE *f;

        snprintf(work, sizeof work, "%s/work/%s", files_dir, tid);
        snprintf(s_next_path, sizeof s_next_path, "%s/image.next", work);
        if ((f = fopen(s_next_path, "r")) != NULL) {
            if (!fgets(want, sizeof want, f))
                want[0] = 0;
            fclose(f);
            remove(s_next_path);
            want[strcspn(want, "\r\n")] = 0;
        }
        snprintf(pack_dir, sizeof pack_dir, "%s", lib);
        if ((slash = strrchr(pack_dir, '/')) != NULL)
            *slash = 0;
        s_meta = m;
        snprintf(s_pack_dir, sizeof s_pack_dir, "%s", pack_dir);
        snprintf(s_pack_lib, sizeof s_pack_lib, "%s", lib);
        if (want[0]) {
            if (recomp_pack_image_lib(&m, pack_dir, lib, want, img_lib, sizeof img_lib, key, sizeof key, ir,
                                      sizeof ir) == 0) {
                snprintf(lib, sizeof lib, "%s", img_lib);
                if (strcmp(key, "default.xbe")) {
                    snprintf(s_image_xbe, sizeof s_image_xbe, "%s", xbe_path);
                    if ((slash = strrchr(s_image_xbe, '/')) != NULL)
                        slash[1] = 0;
                    else
                        s_image_xbe[0] = 0;
                    strncat(s_image_xbe, key, sizeof s_image_xbe - strlen(s_image_xbe) - 1);
                    setenv("RECOMP_IMAGE", key, 1);
                }
                printf("PACK_IMAGE %s %s\n", key, lib);
            } else {
                printf("PACK_IMAGE_MISSING %s (%s), default.xbe instead\n", want, ir);
            }
        }
        if (m.images_n > 0 && !getenv("RECOMP_IMAGE_MAP")) {
            char map[RECOMP_PACK_PATH_MAX + 64];
            snprintf(map, sizeof map, "%s/images.map", work);
            if ((f = fopen(map, "w")) != NULL) {
                fprintf(f, "default.xbe\tdefault.xbe\n");
                for (int i = 0; i < m.images_n; i++)
                    fprintf(f, "%s\t%s\n", m.images[i].path, m.images[i].path);
                fclose(f);
                setenv("RECOMP_IMAGE_MAP", map, 1);
            }
        }
    }
    rc = recomp_pack_open(lib, m.entry, &syms, why, sizeof why);
    if (rc != RECOMP_PACK_OK) {
        printf("PACK_DLOPEN %s\n", why);
        goto out;
    }
    /* Before the title starts: no guest thread reads these yet. */
    s_lookup = (lookup_fn)syms.lookup;
    s_lookup_manual = (lookup_fn)syms.lookup_manual;
    s_pb_state = (pb_state_fn)syms.fe_menu_get_pb_state;
    s_fe_cursor = syms.fe_cursor;
    *main_out = (xba_title_main_fn)syms.title_main;
    printf("PACK_LOADED %s %s (%lld bytes, \"%s\", shell_abi %d; exports:%s%s%s%s)\n", tid, lib,
           m.lib_size, m.title, xba_shell_abi, s_lookup ? " recomp_lookup" : "",
           s_lookup_manual ? " recomp_lookup_manual" : "", s_pb_state ? " fe_menu_get_pb_state" : "",
           s_fe_cursor ? " g_fe_cursor" : "");
out:
    fflush(stdout);
    if (reason && reason_n)
        snprintf(reason, reason_n, "%s", why);
    return rc;
}

const char *xba_pack_image_xbe(void)
{
    return s_image_xbe[0] ? s_image_xbe : NULL;
}

int xba_pack_set_next_image(const char *target)
{
    FILE *f;

    if (!s_next_path[0] || !target || !*target)
        return -1;
    if (!(f = fopen(s_next_path, "w")))
        return -1;
    fprintf(f, "%s\n", target);
    return fclose(f) == 0 ? 0 : -1;
}

int xba_pack_image_state(const char *target)
{
    char out[RECOMP_PACK_PATH_MAX + 300], why[256];

    if (!s_pack_dir[0] || !target || !*target)
        return -1;
    return recomp_pack_image_lib(&s_meta, s_pack_dir, s_pack_lib, target, out, sizeof out, NULL, 0, why,
                                 sizeof why);
}

int xba_pack_set_pending_image(const char *target)
{
    char path[sizeof s_next_path + 16], *slash;
    FILE *f;

    if (!s_next_path[0] || !target || !*target)
        return -1;
    snprintf(path, sizeof path, "%s", s_next_path);
    if (!(slash = strrchr(path, '/')))
        return -1;
    snprintf(slash + 1, sizeof path - (size_t)(slash + 1 - path), "image.pending");
    if (!(f = fopen(path, "w")))
        return -1;
    fprintf(f, "%s\n", target);
    return fclose(f) == 0 ? 0 : -1;
}

/* ---- Other consoles (XBA-147) ---------------------------------------------- */

int xba_pack_rom_console(const char *path)
{
    char key[64];
    int console = RECOMP_CONSOLE_UNKNOWN;
    if (!path || !path[0] || recomp_pack_rom_identify(path, &console, key, sizeof key) != 0)
        return RECOMP_CONSOLE_UNKNOWN;
    return console;
}

typedef int (*console_start_fn)(const char *rom, const char *pack_lib, const char *title_key);

/* The Xbox 360 (XBA-280): a pack is always needed (its code is the title's, recompiled ahead: bringup --app-pack on a
 * desktop for now). The pack holds the plain title (default.xex) and the shader cache (shaders/); the game file the
 * player imported is the disc (an image) or, an XEX, the title itself when the pack has no default.xex. */
static int x360_start(const char *rom, const char *pack_lib, const char *title_key)
{
    char xex[RECOMP_PACK_PATH_MAX + 16], shaders[RECOMP_PACK_PATH_MAX + 16];
    const char *slash = pack_lib ? strrchr(pack_lib, '/') : NULL;
    int dir = slash ? (int)(slash - pack_lib) : 0;
    snprintf(xex, sizeof xex, "%.*s/default.xex", dir, pack_lib ? pack_lib : ".");
    snprintf(shaders, sizeof shaders, "%.*s/shaders", dir, pack_lib ? pack_lib : ".");
    unsigned char magic[4] = {0};
    FILE *f = fopen(xex, "rb");
    int have_xex = f && fread(magic, 1, 4, f) == 4 && !memcmp(magic, "XEX2", 4);
    if (f)
        fclose(f);
    int rom_is_xex = 0;
    if (rom && strncmp(rom, "fd:", 3)) {
        f = fopen(rom, "rb");
        rom_is_xex = f && fread(magic, 1, 4, f) == 4 && !memcmp(magic, "XEX2", 4);
        if (f)
            fclose(f);
    }
    if (!getenv("RECOMP_X360_SHADERS"))
        setenv("RECOMP_X360_SHADERS", shaders, 1);
    if (!rom_is_xex && rom && !getenv("RECOMP_X360_DISC"))
        setenv("RECOMP_X360_DISC", rom, 1); /* the image the player imported is the disc */
    if (rom_is_xex && !getenv("RECOMP_X360_DISC")) { /* an XEX by path: its folder holds the title's files */
        char folder[RECOMP_PACK_PATH_MAX];
        const char *s = strrchr(rom, '/');
        snprintf(folder, sizeof folder, "%.*s", s ? (int)(s - rom) : 1, s ? rom : ".");
        setenv("RECOMP_X360_DISC", folder, 1);
    }
    return x360_runtime_start(have_xex ? xex : rom, pack_lib, title_key);
}

int xba_pack_load_rom(const char *files_dir, const char *rom_path, int *runtime_rc,
                      char *reason, size_t reason_n)
{
    char key[64], lib[RECOMP_PACK_PATH_MAX], why[512];
    recomp_pack_meta m;
    console_start_fn start = NULL;
    int console = RECOMP_CONSOLE_UNKNOWN, rc;

    if (runtime_rc)
        *runtime_rc = -1;
    why[0] = 0;
    if (!rom_path || recomp_pack_rom_identify(rom_path, &console, key, sizeof key) != 0) {
        snprintf(why, sizeof why, "%s is not a game of another console", rom_path ? rom_path : "(none)");
        printf("PACK_INVALID %s\n", why);
        rc = RECOMP_PACK_INVALID;
        goto out;
    }
    /* The runtime by the console: the game says which, pack.json must agree
     * (recomp_pack_validate_key). */
    switch (console) {
    case RECOMP_CONSOLE_GBA: start = gba_runtime_start; break;
    case RECOMP_CONSOLE_PS1: start = ps1_runtime_start; break;
    case RECOMP_CONSOLE_N64: start = n64_runtime_start; break;
    case RECOMP_CONSOLE_GC: start = gc_runtime_start; break;
    case RECOMP_CONSOLE_X360: start = x360_start; break;
    default: break;
    }
    rc = recomp_pack_find_key(files_dir, console, key, xba_shell_abi, &m, lib, sizeof lib, why, sizeof why);
    if (console == RECOMP_CONSOLE_GBA && start) {
        /* The GBA runtime is an interpreter that a pack speeds up (XBA-157 / XBA-158): the game always starts. No
         * pack: PACK_NONE. A pack this shell refuses (shell_abi, abi, title_key...): PACK_STALE, interpreted. A pack
         * that passes: the runtime checks the rest (rom_sha256, lib_sha256, its table: gba_pack.h) and logs
         * PACK_LOADED gba <KEY> (<N> functions) or PACK_STALE. The discovered addresses go to the Prepare's work
         * folder of the game, <files>/work/gba-<KEY>/discovered.txt. */
        char work[RECOMP_PACK_PATH_MAX + 8];
        snprintf(work, sizeof work, "%s/work", files_dir ? files_dir : ".");
        gba_runtime_set_work_dir(files_dir ? work : NULL);
        if (rc == RECOMP_PACK_MISSING)
            printf("PACK_NONE %s %s (not prepared, the interpreter runs it)\n", recomp_pack_console_name(console), key);
        else if (rc != RECOMP_PACK_OK)
            printf("PACK_STALE %s %s (%s), running interpreted\n", recomp_pack_console_name(console), key, why);
        fflush(stdout);
        why[0] = 0;
        rc = start(rom_path, rc == RECOMP_PACK_OK ? lib : NULL, key);
        if (runtime_rc)
            *runtime_rc = rc;
        rc = RECOMP_PACK_OK;
        goto out;
    }
    if (console == RECOMP_CONSOLE_PS1 && start) {
        /* The PS1 runtime is an interpreter that a pack speeds up too (XBA-168): the game always starts, from its
         * disc (or EXE) and the OpenBIOS the app ships. No pack: PACK_NONE. A pack this shell refuses: PACK_STALE,
         * interpreted. A pack that passes: the runtime checks the rest (source_sha256 = its boot EXE's, lib_sha256,
         * its table: ps1_pack.h) and logs PACK_LOADED ps1 <KEY> (<N> functions) or PACK_STALE. The discovered code
         * goes to the Prepare's work folder of the game, <files>/work/ps1-<KEY>/discovered.txt. */
        char work[RECOMP_PACK_PATH_MAX + 8];
        snprintf(work, sizeof work, "%s/work", files_dir ? files_dir : ".");
        ps1_runtime_set_work_dir(files_dir ? work : NULL);
        if (rc == RECOMP_PACK_MISSING)
            printf("PACK_NONE %s %s (not prepared, the interpreter runs it)\n", recomp_pack_console_name(console), key);
        else if (rc != RECOMP_PACK_OK)
            printf("PACK_STALE %s %s (%s), running interpreted\n", recomp_pack_console_name(console), key, why);
        fflush(stdout);
        why[0] = 0;
        rc = start(rom_path, rc == RECOMP_PACK_OK ? lib : NULL, key);
        if (runtime_rc)
            *runtime_rc = rc;
        rc = RECOMP_PACK_OK;
        goto out;
    }
    if (console == RECOMP_CONSOLE_N64 && start) {
        /* The N64 runtime is an interpreter that a pack speeds up too (XBA-174): the cartridge always starts, no BIOS
         * (HLE PIF). No pack: PACK_NONE. A pack this shell refuses: PACK_STALE, interpreted. A pack that passes: the
         * runtime checks the rest (source_sha256 = the ROM's in big-endian order, lib_sha256, its table: n64_pack.h)
         * and logs PACK_LOADED n64 <KEY> (<N> functions) or PACK_STALE. The discovered code goes to the Prepare's
         * work folder of the game, <files>/work/n64-<KEY>/discovered.txt. */
        char work[RECOMP_PACK_PATH_MAX + 8];
        snprintf(work, sizeof work, "%s/work", files_dir ? files_dir : ".");
        n64_runtime_set_work_dir(files_dir ? work : NULL);
        if (rc == RECOMP_PACK_MISSING)
            printf("PACK_NONE %s %s (not prepared, the interpreter runs it)\n", recomp_pack_console_name(console), key);
        else if (rc != RECOMP_PACK_OK)
            printf("PACK_STALE %s %s (%s), running interpreted\n", recomp_pack_console_name(console), key, why);
        fflush(stdout);
        why[0] = 0;
        rc = start(rom_path, rc == RECOMP_PACK_OK ? lib : NULL, key);
        if (runtime_rc)
            *runtime_rc = rc;
        rc = RECOMP_PACK_OK;
        goto out;
    }
    if (console == RECOMP_CONSOLE_GC && start) {
        /* The GameCube runtime (XBA-259) is an interpreter too, homebrew DOLs for now: the program always starts,
         * no IPL. No pack: PACK_NONE. A pack this shell refuses: PACK_STALE, interpreted. A pack that passes is
         * handed over; the runtime attaches its table (PACK_LOADED gc <KEY>), whose code runs only where memory
         * holds what it was lifted from. The interpreter's misses go to <files>/work/gc-<KEY>/discovered.txt. */
        char work[RECOMP_PACK_PATH_MAX + 8];
        snprintf(work, sizeof work, "%s/work", files_dir ? files_dir : ".");
        gc_runtime_set_work_dir(files_dir ? work : NULL);
        if (rc == RECOMP_PACK_MISSING)
            printf("PACK_NONE %s %s (not prepared, the interpreter runs it)\n", recomp_pack_console_name(console), key);
        else if (rc != RECOMP_PACK_OK)
            printf("PACK_STALE %s %s (%s), running interpreted\n", recomp_pack_console_name(console), key, why);
        fflush(stdout);
        why[0] = 0;
        rc = start(rom_path, rc == RECOMP_PACK_OK ? lib : NULL, key);
        if (runtime_rc)
            *runtime_rc = rc;
        rc = RECOMP_PACK_OK;
        goto out;
    }
    if (console == RECOMP_CONSOLE_X360) {
        /* XBA-280: the missed addresses and the shaders met go to <files>/work/x360-<KEY>/ (for the next pack) */
        char work[RECOMP_PACK_PATH_MAX + 8];
        snprintf(work, sizeof work, "%s/work", files_dir ? files_dir : ".");
        x360_runtime_set_work_dir(files_dir ? work : NULL);
    }
    if (rc == RECOMP_PACK_MISSING) {
        snprintf(why, sizeof why, "%s-%s", recomp_pack_console_name(console), key);
        printf("PACK_MISSING %s %s (no %s/packs/%s/pack.json)\n", recomp_pack_console_name(console), key,
               files_dir ? files_dir : "", why);
        goto out;
    }
    if (rc != RECOMP_PACK_OK || !start) {
        if (rc == RECOMP_PACK_OK)
            snprintf(why, sizeof why, "no %s runtime in this shell", recomp_pack_console_name(console));
        printf("PACK_INVALID %s\n", why);
        rc = RECOMP_PACK_INVALID;
        goto out;
    }
    recomp_pack_apply_env_force(&m);
    printf("PACK_LOADED %s %s %s (%lld bytes, \"%s\")\n", recomp_pack_console_name(console), key, lib, m.lib_size,
           m.title);
    fflush(stdout);
    rc = start(rom_path, lib, key);
    if (runtime_rc)
        *runtime_rc = rc;
    rc = RECOMP_PACK_OK;
out:
    fflush(stdout);
    if (reason && reason_n)
        snprintf(reason, reason_n, "%s", why);
    return rc;
}
