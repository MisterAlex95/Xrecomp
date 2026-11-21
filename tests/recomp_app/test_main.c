/* recomp_app (XBA-52): the start-up the Android shell runs for every title.
 *
 *   - the host defaults: RECOMP_FB_WINDOW and RECOMP_PB_EXEC on for Android
 *     unless the environment or recomp.cfg says otherwise, "=0" removing
 *     RECOMP_PB_EXEC (read by presence); nothing on other hosts;
 *   - recomp_app_start: recomp.cfg is read from <external> before storage
 *     (it may set RECOMP_GAME_DIR), the resolved directories go to
 *     xbox_path_init, the fault handler is installed, and the return value
 *     says whether default.xbe is there; no paths, no xbox_path_init;
 *   - "no usable storage paths" goes to stderr only when there are none (it
 *     used to be printed on every normal start: the else had moved to the
 *     disc-image check);
 *   - a disc image the kernel did not mount: "disc images are off" when
 *     RECOMP_DISC_IMAGE=0 refused it, "the disc image cannot be read"
 *     otherwise, and no game either way;
 *   - recomp_app_start_rom (another console's game file): 0, the file as the
 *     game directory and still a file, no default.xbe, the save directory as
 *     usual, and neither "cannot be read" nor "default.xbe" said. */
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
/* kernel_disc.c's (XBA-126): a regular file is an image, as there; never mounted here. */
int xbox_disc_spec_is_image(const char *spec)
{
    struct stat st;
    return spec && *spec && stat(spec, &st) == 0 && S_ISREG(st.st_mode);
}
int xbox_disc_mounted(void) { return 0; }
/* android_log.c's, stubbed: on a device it would move this test's output to logcat. */
static int s_log_calls;
int xbox_android_log_init(void) { s_log_calls++; return 0; }

static int env_is(const char *name, const char *want)
{
    const char *v = getenv(name);
    return want ? v && strcmp(v, want) == 0 : v == NULL;
}

/* recomp_app_start (recomp_app_start_rom when rom) with stdout and stderr
 * caught: 1 when what it printed has `needle`. */
static char s_said[16384];
static int start_said(int rom, const char *ext, const char *in, recomp_storage *s, int *ret, const char *needle)
{
    const char *tmp = getenv("TMPDIR");
    char path[512];
    snprintf(path, sizeof path, "%s/recomp_app_err_XXXXXX", tmp && tmp[0] ? tmp : "/tmp");
    int fd = mkstemp(path), saved_out, saved_err;
    size_t n = 0;
    if (fd < 0) { perror("mkstemp"); return -1; }
    fflush(stdout);
    fflush(stderr);
    saved_out = dup(1);
    saved_err = dup(2);
    dup2(fd, 1);
    dup2(fd, 2);
    *ret = rom ? recomp_app_start_rom(ext, in, s) : recomp_app_start(ext, in, s);
    fflush(stdout);
    fflush(stderr);
    dup2(saved_out, 1);
    dup2(saved_err, 2);
    close(saved_out);
    close(saved_err);
    lseek(fd, 0, SEEK_SET);
    ssize_t r;
    while (n < sizeof s_said - 1 && (r = read(fd, s_said + n, sizeof s_said - 1 - n)) > 0) n += (size_t)r;
    s_said[n] = '\0';
    close(fd);
    unlink(path);
    return strstr(s_said, needle) != NULL;
}

static int start_says(const char *ext, const char *in, recomp_storage *s, int *ret, const char *needle)
{
    return start_said(0, ext, in, s, ret, needle);
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
    {
        int r = -2;
        CHECK(start_says(ext, in, &s, &r, "no usable storage paths") == 0);   /* a normal start */
        CHECK(r == 1);
    }
    CHECK(strcmp(s.xbe, p) == 0);
    CHECK(s_path_calls == 2);

    /* 3. No paths: no storage, no xbox_path_init, the handler all the same. */
    {
        int r = -2;
        CHECK(start_says(NULL, NULL, &s, &r, "no usable storage paths") == 1);
        CHECK(r == -1);
    }
    CHECK(s.game_dir[0] == '\0' && s.xbe[0] == '\0');
    CHECK(s_path_calls == 2);
    CHECK(s_handler_calls == 3);

    /* 4. A disc image as the game directory, not mounted (the stub never
     *    mounts): refused by RECOMP_DISC_IMAGE=0, or unreadable. */
    unlink(p);
    snprintf(p, sizeof p, "%s/ext/recomp.cfg", root); unlink(p);   /* RECOMP_GAME_DIR from the environment now */
    {
        char img[1024];
        struct stat st;
        int r = -2;
        snprintf(img, sizeof img, "%s/game.iso", root);
        write_file(img, "not a disc");
        setenv("RECOMP_GAME_DIR", img, 1);

        setenv("RECOMP_DISC_IMAGE", "0", 1);
        CHECK(start_says(ext, in, &s, &r, "disc images are off (RECOMP_DISC_IMAGE=0)") == 1);
        CHECK(strstr(s_said, "cannot be read") == NULL);
        CHECK(r == 0 && s.xbe[0] == '\0' && strcmp(s.game_dir, img) == 0);

        unsetenv("RECOMP_DISC_IMAGE");
        r = -2;
        CHECK(start_says(ext, in, &s, &r, "the disc image cannot be read") == 1);
        CHECK(strstr(s_said, "disc images are off") == NULL);
        CHECK(r == 0 && s.xbe[0] == '\0');

        /* 5. Another console's game: the directories only. */
        setenv("RECOMP_DISC_IMAGE", "0", 1);   /* as the shell sets it */
        r = -2;
        int calls = s_path_calls;
        CHECK(start_said(1, ext, in, &s, &r, "cannot be read") == 0);
        CHECK(strstr(s_said, "default.xbe") == NULL);
        CHECK(strstr(s_said, "disc image") == NULL);
        CHECK(r == 0);
        CHECK(strcmp(s.game_dir, img) == 0 && s.xbe[0] == '\0');
        snprintf(p, sizeof p, "%s/save", in);
        CHECK(strcmp(s.save_dir, p) == 0);
        CHECK(s_path_calls == calls + 1 && strcmp(s_path_game, img) == 0 && strcmp(s_path_save, p) == 0);
        CHECK(stat(img, &st) == 0 && S_ISREG(st.st_mode));   /* still a file */

        /* No paths: -1, as recomp_app_start. */
        r = -2;
        CHECK(start_said(1, NULL, NULL, &s, &r, "no usable storage paths") == 1);
        CHECK(r == -1 && s.game_dir[0] == '\0');

        unsetenv("RECOMP_DISC_IMAGE");
        unsetenv("RECOMP_GAME_DIR");
        unlink(img);
    }

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
