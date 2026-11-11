/* Title packs in the app shell: the forwarders and the loader
 * (pack_loader.h). No SDL, no JNI: tests/recomp_pack builds it into a fake
 * shell library on Linux and checks the routing. */
#include "pack_loader.h"

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

int xba_pack_load(const char *files_dir, const char *xbe_path, xba_title_main_fn *main_out,
                  char *reason, size_t reason_n)
{
    char tid[9], lib[RECOMP_PACK_PATH_MAX], why[512];
    recomp_pack_meta m;
    recomp_pack_syms syms;
    int rc;

    *main_out = NULL;
    why[0] = 0;
    if (recomp_pack_xbe_title_id(xbe_path, tid) != 0) {
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
