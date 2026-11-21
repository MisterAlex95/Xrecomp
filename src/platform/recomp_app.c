/* recomp_app.h (XBA-52): what shell_main.c did inline, for every title. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L   /* setenv / unsetenv */
#endif
#include "recomp_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "android_log.h"
#include "host_cpus.h"
#include "recomp_cfg.h"

#if defined(__ANDROID__)
#include <android/log.h>
#endif

/* The kernel's; declared here rather than pulling kernel.h and the memory
 * layout header into the platform directory. */
extern void xbox_path_init(const char *game_dir, const char *save_dir);
extern int xbox_PosixInstallMmioFaultHandler(void);
extern int xbox_disc_spec_is_image(const char *spec);
extern int xbox_disc_mounted(void);

void recomp_app_set_defaults(int android)
{
    char set[128] = "";
    const char *pb;

    if (!android)
        return;
    if (!getenv("RECOMP_FB_WINDOW")) {
        setenv("RECOMP_FB_WINDOW", "1", 0);
        strcat(set, " RECOMP_FB_WINDOW=1");
    }
    pb = getenv("RECOMP_PB_EXEC");
    if (!pb) {
        setenv("RECOMP_PB_EXEC", "1", 0);
        strcat(set, " RECOMP_PB_EXEC=1");
    } else if (!*pb || strcmp(pb, "0") == 0) {
        /* Read by presence (getenv != NULL) in the kernel: "=0" has to go. */
        unsetenv("RECOMP_PB_EXEC");
        strcat(set, " RECOMP_PB_EXEC off");
    }
    if (set[0])
        printf("defaults:%s (Android; RECOMP_FB_WINDOW=0, RECOMP_PB_EXEC=0 or RECOMP_HEADLESS=1"
               " in recomp.cfg turn them off)\n", set);
}

/* rom: the game is another console's (recomp_app_start_rom), whose file is
 * no Xbox disc: the directories only, no default.xbe to look for. */
static int app_start(const char *external, const char *internal,
                     recomp_storage *store, int rom)
{
    int have_game = -1;

    if (xbox_android_log_init() != 0) {
#if defined(__ANDROID__)
        __android_log_write(ANDROID_LOG_ERROR, XBOX_ANDROID_LOG_TAG,
                            "stdout/stderr redirect failed, runtime messages will be lost");
#endif
    }

    /* RECOMP_* switches: no shell environment on Android, so a file in the
     * app's external files dir (adb push recomp.cfg /sdcard/Android/data/<id>/files/). */
    if (external) {
        char cfg[RECOMP_STORAGE_PATH_MAX + 16];
        snprintf(cfg, sizeof cfg, "%s/recomp.cfg", external);
        if (recomp_cfg_load(cfg) == 0)
            printf("recomp.cfg: none (read from %s when present)\n", cfg);
    }

#if defined(__ANDROID__)
    recomp_app_set_defaults(1);
#else
    recomp_app_set_defaults(0);
#endif

    /* Game data and saves, after recomp.cfg (which may set RECOMP_GAME_DIR)
     * and before any title code opens a file. */
    memset(store, 0, sizeof *store);
    if (external && internal)
        have_game = rom ? recomp_storage_resolve_entry(external, internal, NULL, store)
                        : recomp_storage_resolve(external, internal, store);
    if (have_game >= 0) {
        xbox_path_init(store->game_dir, store->save_dir);
        /* A disc image the kernel could not mount (kernel_disc.c said why): no
         * game, as for a folder without default.xbe. */
        if (have_game > 0 && xbox_disc_spec_is_image(store->game_dir) && !xbox_disc_mounted()) {
            const char *off = getenv("RECOMP_DISC_IMAGE");
            if (off && strcmp(off, "0") == 0)
                fprintf(stderr, "xboxrecomp: %s: disc images are off (RECOMP_DISC_IMAGE=0): extract it, "
                                "or remove the line from recomp.cfg\n", store->game_dir);
            else
                fprintf(stderr, "xboxrecomp: %s: the disc image cannot be read\n", store->game_dir);
            store->xbe[0] = '\0';
            have_game = 0;
        }
    } else {
        fprintf(stderr, "xboxrecomp: no usable storage paths (external %s)\n",
                external ? external : "none");
    }

    /* After recomp.cfg, which may say RECOMP_BIG_CORES=0; before SDL_Init,
     * whose threads should inherit the mask too. */
    recomp_host_cpus_init();

    if (xbox_PosixInstallMmioFaultHandler() != 0)
        fprintf(stderr, "xboxrecomp: could not install the MMIO fault handler\n");

    return have_game;
}

int recomp_app_start(const char *external, const char *internal,
                     recomp_storage *store)
{
    return app_start(external, internal, store, 0);
}

int recomp_app_start_rom(const char *external, const char *internal,
                         recomp_storage *store)
{
    return app_start(external, internal, store, 1);
}
