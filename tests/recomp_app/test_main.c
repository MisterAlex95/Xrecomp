/* recomp_app (XBA-52): the start-up the Android shell runs for every title.
 *
 *   - the host defaults: RECOMP_FB_WINDOW and RECOMP_PB_EXEC on for Android
 *     unless the environment or recomp.cfg says otherwise, "=0" removing
 *     RECOMP_PB_EXEC (read by presence); nothing on other hosts;
 *   - recomp_app_start: recomp.cfg is read from <external> before storage
 *     (it may set RECOMP_GAME_DIR), the resolved directories go to
 *     xbox_path_init, the fault handler is installed, and the return value
 *     says whether default.xbe is there; no paths, no xbox_path_init. */
#define _GNU_SOURCE
#include "recomp_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* The kernel's two, stubbed. */
static int s_path_calls, s_handler_calls;
static char s_path_game[2048], s_path_save[2048];
void xbox_path_init(const char *game_dir, const char *save_dir)
{
    s_path_calls++;
    snprintf(s_path_game, sizeof s_path_game, "%s", game_dir ? game_dir : "(null)");
    snprintf(s_path_save, sizeof s_path_save, "%s", save_dir ? save_dir : "(null)");
}
int xbox_PosixInstallMmioFaultHandler(void) { s_handler_calls++; return 0; }
/* android_log.c's, stubbed: on a device it would move this test's output to logcat. */
static int s_log_calls;
int xbox_android_log_init(void) { s_log_calls++; return 0; }

static int env_is(const char *name, const char *want)
{
    const char *v = getenv(name);
    return want ? v && strcmp(v, want) == 0 : v == NULL;
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (f) { fputs(text, f); fclose(f); }
}

int main(void)
{
    const char *tmp = getenv("TMPDIR");
    char root[256], ext[512], in[512], p[2048];
    recomp_storage s;

    /* 1. Defaults. */
    unsetenv("RECOMP_FB_WINDOW");
    unsetenv("RECOMP_PB_EXEC");
    recomp_app_set_defaults(0);
    CHECK(env_is("RECOMP_FB_WINDOW", NULL));
    CHECK(env_is("RECOMP_PB_EXEC", NULL));
    recomp_app_set_defaults(1);
    CHECK(env_is("RECOMP_FB_WINDOW", "1"));
    CHECK(env_is("RECOMP_PB_EXEC", "1"));
    setenv("RECOMP_FB_WINDOW", "0", 1);
    setenv("RECOMP_PB_EXEC", "0", 1);
    recomp_app_set_defaults(1);
    CHECK(env_is("RECOMP_FB_WINDOW", "0"));          /* the window reads its value */
    CHECK(env_is("RECOMP_PB_EXEC", NULL));           /* the kernel reads presence */
    setenv("RECOMP_PB_EXEC", "verbose", 1);
    recomp_app_set_defaults(1);
    CHECK(env_is("RECOMP_PB_EXEC", "verbose"));      /* a value of the user's stays */

    /* 2. Start-up, with a recomp.cfg that moves the game directory. */
    snprintf(root, sizeof root, "%s/recomp_app_XXXXXX", tmp && tmp[0] ? tmp : "/tmp");
    if (!mkdtemp(root)) { perror("mkdtemp"); return 1; }
    snprintf(ext, sizeof ext, "%s/ext", root);
    snprintf(in, sizeof in, "%s/int", root);
    mkdir(ext, 0755);
    mkdir(in, 0755);
    unsetenv("RECOMP_GAME_DIR");
    snprintf(p, sizeof p, "%s/recomp.cfg", ext);
    {
        char cfg[1024];
        snprintf(cfg, sizeof cfg, "RECOMP_GAME_DIR=%s/disc\n", root);
        write_file(p, cfg);
    }
    CHECK(recomp_app_start(ext, in, &s) == 0);       /* no default.xbe yet */
    snprintf(p, sizeof p, "%s/disc", root);
    CHECK(strcmp(s.game_dir, p) == 0);
    CHECK(s_path_calls == 1 && strcmp(s_path_game, s.game_dir) == 0 && strcmp(s_path_save, s.save_dir) == 0);
    CHECK(s_handler_calls == 1);
    CHECK(s_log_calls == 1);

    snprintf(p, sizeof p, "%s/disc/default.xbe", root);
    write_file(p, "XBEH");
    CHECK(recomp_app_start(ext, in, &s) == 1);
    CHECK(strcmp(s.xbe, p) == 0);
    CHECK(s_path_calls == 2);

    /* 3. No paths: no storage, no xbox_path_init, the handler all the same. */
    CHECK(recomp_app_start(NULL, NULL, &s) == -1);
    CHECK(s.game_dir[0] == '\0' && s.xbe[0] == '\0');
    CHECK(s_path_calls == 2);
    CHECK(s_handler_calls == 3);

    unlink(p);
    snprintf(p, sizeof p, "%s/ext/recomp.cfg", root); unlink(p);
    snprintf(p, sizeof p, "%s/disc", root); rmdir(p);
    snprintf(p, sizeof p, "%s/ext", root); rmdir(p);
    snprintf(p, sizeof p, "%s/int/save", root);
    {
        /* recomp_storage makes save/; leave the tree to the temp dir's cleaner if anything else is in it. */
        rmdir(p);
    }
    snprintf(p, sizeof p, "%s/int", root); rmdir(p);
    rmdir(root);

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("PASS: recomp_app defaults and start-up");
    return 0;
}
