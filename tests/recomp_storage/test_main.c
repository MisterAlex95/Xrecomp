/* recomp_storage_resolve (XBA-23): the game and save directories the Android
 * shell hands to xbox_path_init, and the default.xbe check behind the
 * NO_GAME_DATA message. */
#define _GNU_SOURCE
#include "recomp_storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static int is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }

static void touch(const char *p) { FILE *f = fopen(p, "w"); if (f) { fputs("XBEH", f); fclose(f); } }

int main(void)
{
    /* $TMPDIR: /tmp does not exist on Android (the device bundle sets it). */
    const char *tmp = getenv("TMPDIR");
    char root[256], ext[512], in[512], p[2048];
    recomp_storage s;

    snprintf(root, sizeof root, "%s/recomp_storage_XXXXXX", tmp && tmp[0] ? tmp : "/tmp");
    if (!mkdtemp(root)) { perror("mkdtemp"); return 1; }
    snprintf(ext, sizeof ext, "%s/ext/files/", root);   /* trailing slash on purpose */
    snprintf(in, sizeof in, "%s/int/files", root);
    unsetenv("RECOMP_GAME_DIR");

    /* 1. Nothing there yet: both directories made, no game. */
    CHECK(recomp_storage_resolve(ext, in, &s) == 0);
    snprintf(p, sizeof p, "%s/ext/files/game", root);
    CHECK(strcmp(s.game_dir, p) == 0);
    CHECK(is_dir(p));
    snprintf(p, sizeof p, "%s/int/files/save", root);
    CHECK(strcmp(s.save_dir, p) == 0);
    CHECK(is_dir(p));
    CHECK(s.xbe[0] == '\0');

    /* 2. A directory named default.xbe is not the game. */
    snprintf(p, sizeof p, "%s/default.xbe", s.game_dir);
    mkdir(p, 0755);
    CHECK(recomp_storage_resolve(ext, in, &s) == 0);
    rmdir(p);

    /* 3. Found whatever its case, under the name it has on disk. */
    snprintf(p, sizeof p, "%s/DEFAULT.XBE", s.game_dir);
    touch(p);
    CHECK(recomp_storage_resolve(ext, in, &s) == 1);
    CHECK(strcmp(s.xbe, p) == 0);

    /* 4. RECOMP_GAME_DIR wins over <external>/game (recomp.cfg sets it). */
    snprintf(p, sizeof p, "%s/elsewhere/", root);
    setenv("RECOMP_GAME_DIR", p, 1);
    CHECK(recomp_storage_resolve(ext, in, &s) == 0);
    snprintf(p, sizeof p, "%s/elsewhere", root);
    CHECK(strcmp(s.game_dir, p) == 0);
    CHECK(is_dir(p));
    /* With it set, no external dir is needed. */
    CHECK(recomp_storage_resolve(NULL, in, &s) == 0);
    unsetenv("RECOMP_GAME_DIR");

    /* 4b. A disc image read in place (XBA-126): a file, or "fd:N", is not created and names default.xbe inside. */
    {
        char img[600], fdspec[32];
        struct stat st;
        snprintf(img, sizeof img, "%s/Game.iso", root);
        touch(img);
        setenv("RECOMP_GAME_DIR", img, 1);
        CHECK(recomp_storage_resolve(ext, in, &s) == 1);
        CHECK(strcmp(s.game_dir, img) == 0);
        snprintf(p, sizeof p, "%s/default.xbe", img);
        CHECK(strcmp(s.xbe, p) == 0);
        CHECK(stat(img, &st) == 0 && S_ISREG(st.st_mode));          /* still the file */
        FILE *f = fopen(img, "rb");
        snprintf(fdspec, sizeof fdspec, "fd:%d", f ? fileno(f) : -1);
        setenv("RECOMP_GAME_DIR", fdspec, 1);
        CHECK(recomp_storage_resolve(ext, in, &s) == 1);
        CHECK(strcmp(s.game_dir, fdspec) == 0 && strncmp(s.xbe, fdspec, strlen(fdspec)) == 0);
        if (f)
            fclose(f);
        CHECK(recomp_storage_resolve(ext, in, &s) == 0);             /* the descriptor is gone */
        CHECK(s.xbe[0] == '\0');
        unsetenv("RECOMP_GAME_DIR");
    }

    /* 5. Bad arguments. */
    CHECK(recomp_storage_resolve(ext, NULL, &s) == -1);
    CHECK(recomp_storage_resolve(NULL, in, &s) == -1);
    CHECK(recomp_storage_resolve(ext, in, NULL) == -1);
    {
        char longp[2000];
        memset(longp, 'a', sizeof longp - 1);
        longp[0] = '/';
        longp[sizeof longp - 1] = '\0';
        CHECK(recomp_storage_resolve(longp, in, &s) == -1);
    }

    {
        char cmd[600];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
        if (system(cmd) != 0) fprintf(stderr, "cleanup failed\n");
    }
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("recomp_storage: all checks passed\n");
    return 0;
}
