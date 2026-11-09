/* Where a title's data and saves live on a host without a shell (XBA-23).
 *
 * On Android the game data is pushed to <external files dir>/game/ (adb
 * push, or the import screen later) and the saves stay in
 * <internal files dir>/save/, which survives the app being killed and is
 * private to it. The app resolves both here and hands them to
 * xbox_path_init(game_dir, save_dir), which maps D:\ and the save drives.
 *
 * POSIX hosts; only the app shell calls it. */
#ifndef XBOXRECOMP_RECOMP_STORAGE_H
#define XBOXRECOMP_RECOMP_STORAGE_H

#ifdef __cplusplus
extern "C" {
#endif

#define RECOMP_STORAGE_PATH_MAX 1024

typedef struct recomp_storage {
    char game_dir[RECOMP_STORAGE_PATH_MAX];  /* D:\ */
    char save_dir[RECOMP_STORAGE_PATH_MAX];  /* save drives, partition images */
    char xbe[RECOMP_STORAGE_PATH_MAX];       /* default.xbe as found on disk, or "" */
} recomp_storage;

/* Resolves the two directories:
 *   game_dir = $RECOMP_GAME_DIR if set (recomp.cfg can set it), else
 *              <external>/game;
 *   save_dir = <internal>/save.
 * Both are created when missing (the game dir too, so the folder to push the
 * data into is there to see). default.xbe is looked up case-insensitively
 * in game_dir, as the kernel's path lookup does.
 *
 * Returns 1 when default.xbe was found, 0 when it was not (out->xbe is ""),
 * -1 on bad arguments or a path too long. Prints one line on stdout:
 * "storage: game=... save=... (default.xbe found|missing)". */
int recomp_storage_resolve(const char *external, const char *internal,
                           recomp_storage *out);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_RECOMP_STORAGE_H */
