/* recomp_cfg_load (XBA-26): RECOMP_* options from a file. */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
#include "recomp_cfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <process.h>
#define getpid _getpid
static void unset(const char *k) { _putenv_s(k, ""); }
static void set(const char *k, const char *v) { _putenv_s(k, v); }
#else
#include <unistd.h>
static void unset(const char *k) { unsetenv(k); }
static void set(const char *k, const char *v) { setenv(k, v, 1); }
#endif

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

static int env_is(const char *k, const char *want)
{
    const char *v = getenv(k);
    if (!want) return !v || !*v;
    return v && strcmp(v, want) == 0;
}

int main(void)
{
    char path[512];
    const char *tmp = getenv("TMPDIR");
    FILE *f;
    int n;

    if (!tmp || !*tmp) tmp = "/tmp";
#if defined(_WIN32)
    tmp = getenv("TEMP") ? getenv("TEMP") : ".";
#endif
    snprintf(path, sizeof path, "%s/recomp_cfg_test_%d.cfg", tmp, (int)getpid());

    /* 1. no file: 0, not an error */
    remove(path);
    CHECK(recomp_cfg_load(path) == 0, "missing file should return 0");
    CHECK(recomp_cfg_load(NULL) == 0, "NULL path should return 0");

    /* 2. the format */
    unset("RECOMP_CFG_A"); unset("RECOMP_CFG_B"); unset("RECOMP_CFG_C"); unset("RECOMP_CFG_EMPTY");
    set("RECOMP_CFG_KEPT", "from-env");
    f = fopen(path, "w");
    if (!f) { printf("cannot write %s\n", path); return 1; }
    fputs("# a comment\n"
          "\n"
          "RECOMP_CFG_A=1\n"
          "  RECOMP_CFG_B =  two words  \r\n"   /* trimmed, CRLF */
          "RECOMP_CFG_C=a=b\n"                  /* the first '=' splits */
          "RECOMP_CFG_KEPT=from-file\n"         /* the environment wins */
          "not a line\n"                        /* no '=': skipped */
          "9BAD=1\n"                            /* not a variable name: skipped */
          "=novalue\n"                          /* empty key: skipped */
          "RECOMP_CFG_EMPTY=\n",                /* empty value is a value */
          f);
    fclose(f);
    n = recomp_cfg_load(path);
    CHECK(n == 5, "expected 5 options (A, B, C, KEPT, EMPTY), got %d", n);
    CHECK(env_is("RECOMP_CFG_A", "1"), "RECOMP_CFG_A");
    CHECK(env_is("RECOMP_CFG_B", "two words"), "RECOMP_CFG_B trimmed, CRLF dropped: \"%s\"",
          getenv("RECOMP_CFG_B") ? getenv("RECOMP_CFG_B") : "(unset)");
    CHECK(env_is("RECOMP_CFG_C", "a=b"), "RECOMP_CFG_C keeps the second '='");
    CHECK(env_is("RECOMP_CFG_KEPT", "from-env"), "the environment must win over the file");
    CHECK(getenv("9BAD") == NULL, "9BAD must not be set");
#if !defined(_WIN32)   /* _putenv_s cannot hold an empty value */
    CHECK(env_is("RECOMP_CFG_EMPTY", "") && getenv("RECOMP_CFG_EMPTY") != NULL, "empty value");
#endif

    /* 3. a very long file still loads (the summary line truncates, nothing overflows) */
    f = fopen(path, "w");
    if (f) {
        int i;
        for (i = 0; i < 200; i++)
            fprintf(f, "RECOMP_CFG_LONG_%03d=%0100d\n", i, i);
        fclose(f);
        n = recomp_cfg_load(path);
        CHECK(n == 200, "expected 200 long options, got %d", n);
        CHECK(getenv("RECOMP_CFG_LONG_199") != NULL, "last long option");
    }

    /* 4. a title's forced options (env_force): they win over the environment and recomp.cfg */
    set("RECOMP_LSFG", "1");
    CHECK(recomp_cfg_force("4541005B", "RECOMP_LSFG", "0") == 0, "force RECOMP_LSFG=0");
    CHECK(env_is("RECOMP_LSFG", "0"), "a forced value wins over the environment");
    CHECK(recomp_cfg_force_key_ok("RECOMP_GPU_FPS_CAP") && recomp_cfg_force_key_ok("RECOMP_X9_"),
          "RECOMP_[A-Z0-9_]+ allowed");
    CHECK(!recomp_cfg_force_key_ok("RECOMP_") && !recomp_cfg_force_key_ok("LD_PRELOAD")
          && !recomp_cfg_force_key_ok("RECOMP_lsfg") && !recomp_cfg_force_key_ok("RECOMP_A-B")
          && !recomp_cfg_force_key_ok(NULL), "other names refused");
    CHECK(recomp_cfg_force_value_ok("") && recomp_cfg_force_value_ok("60") && !recomp_cfg_force_value_ok("a\nb")
          && !recomp_cfg_force_value_ok("\xc3\xa9"), "values: printable ASCII");
    unset("LD_PRELOAD_TEST");
    CHECK(recomp_cfg_force("T", "LD_PRELOAD_TEST", "x") == -1 && getenv("LD_PRELOAD_TEST") == NULL,
          "a refused key is not set");
    CHECK(recomp_cfg_force("T", "RECOMP_LSFG", "1\n") == -1 && env_is("RECOMP_LSFG", "0"),
          "a refused value leaves the old one");

    remove(path);
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    puts("PASS: recomp.cfg");
    return 0;
}
