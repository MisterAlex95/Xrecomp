/* recomp_storage.h (XBA-23). */
#include "recomp_storage.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>

#define mkdir_one(p) mkdir((p), 0755)

static int fmt(char *dst, const char *a, const char *b)
{
    size_t la = strlen(a);
    int n;

    /* No trailing slash on either part: xbox_path_init appends its own. */
    while (la > 1 && a[la - 1] == '/')
        la--;
    n = b ? snprintf(dst, RECOMP_STORAGE_PATH_MAX, "%.*s/%s", (int)la, a, b)
          : snprintf(dst, RECOMP_STORAGE_PATH_MAX, "%.*s", (int)la, a);
    if (n < 0 || n >= RECOMP_STORAGE_PATH_MAX)
        return -1;
    while (n > 1 && dst[n - 1] == '/')
        dst[--n] = '\0';
    return 0;
}

static void mkdir_p(const char *path)
{
    char tmp[RECOMP_STORAGE_PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir_one(tmp);
            *p = '/';
        }
    }
    if (mkdir_one(tmp) != 0 && errno != EEXIST)
        fprintf(stderr, "storage: cannot create %s: %s\n", tmp, strerror(errno));
}

static int find_xbe(const char *dir, char *out)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int found = 0;

    out[0] = '\0';
    if (!d)
        return 0;
    while ((e = readdir(d)) != NULL) {
        if (strcasecmp(e->d_name, "default.xbe") == 0) {
            struct stat st;
            if (fmt(out, dir, e->d_name) == 0 && stat(out, &st) == 0 &&
                S_ISREG(st.st_mode)) {
                found = 1;
                break;
            }
            out[0] = '\0';
        }
    }
    closedir(d);
    return found;
}

int recomp_storage_resolve(const char *external, const char *internal,
                           recomp_storage *out)
{
    const char *ov = getenv("RECOMP_GAME_DIR");

    if (!out)
        return -1;
    memset(out, 0, sizeof *out);
    if (!internal || !internal[0])
        return -1;
    if (ov && ov[0]) {
        if (fmt(out->game_dir, ov, NULL) != 0)
            return -1;
    } else {
        if (!external || !external[0] || fmt(out->game_dir, external, "game") != 0)
            return -1;
    }
    if (fmt(out->save_dir, internal, "save") != 0)
        return -1;

    mkdir_p(out->game_dir);
    mkdir_p(out->save_dir);

    int found = find_xbe(out->game_dir, out->xbe);
    printf("storage: game=%s save=%s (default.xbe %s)\n",
           out->game_dir, out->save_dir, found ? "found" : "missing");
    fflush(stdout);
    return found;
}
