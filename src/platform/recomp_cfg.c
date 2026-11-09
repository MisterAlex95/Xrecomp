/* RECOMP_* options from a file (recomp_cfg.h). */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
#include "recomp_cfg.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
static int cfg_setenv(const char *k, const char *v) { return _putenv_s(k, v); }
#else
static int cfg_setenv(const char *k, const char *v) { return setenv(k, v, 1); }
#endif

static char *trim(char *s)
{
    char *e;
    while (*s && isspace((unsigned char)*s)) s++;
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

/* Appends to the summary line, truncating quietly when it is full. */
static void append(char *buf, size_t size, size_t *used, const char *fmt, const char *a, const char *b)
{
    int n;
    if (*used >= size - 1) return;
    n = snprintf(buf + *used, size - *used, fmt, a, b);
    if (n < 0) return;
    *used += (size_t)n < size - *used ? (size_t)n : size - 1 - *used;
}

static int valid_key(const char *k)
{
    if (!*k || !(isalpha((unsigned char)*k) || *k == '_')) return 0;
    for (; *k; k++)
        if (!(isalnum((unsigned char)*k) || *k == '_')) return 0;
    return 1;
}

int recomp_cfg_load(const char *path)
{
    FILE *f;
    char line[1024];
    char summary[2048];
    size_t used = 0;
    int count = 0, lineno = 0;

    if (!path) return 0;
    f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT) return 0;
        fprintf(stderr, "recomp.cfg: cannot read %s (%s)\n", path, strerror(errno));
        return -1;
    }
    summary[0] = '\0';
    while (fgets(line, sizeof line, f)) {
        char *s, *eq, *key, *val;
        lineno++;
        s = trim(line);
        if (!*s || *s == '#') continue;
        eq = strchr(s, '=');
        if (!eq) {
            fprintf(stderr, "recomp.cfg:%d: no '=' in \"%s\", line skipped\n", lineno, s);
            continue;
        }
        *eq = '\0';
        key = trim(s);
        val = trim(eq + 1);
        if (!valid_key(key)) {
            fprintf(stderr, "recomp.cfg:%d: \"%s\" is not a variable name, line skipped\n", lineno, key);
            continue;
        }
        if (getenv(key)) {
            /* Already set by the environment: that one wins. */
            append(summary, sizeof summary, &used, "%s%s (kept from the environment)", count ? ", " : "", key);
            count++;
            continue;
        }
        if (cfg_setenv(key, val) != 0) {
            fprintf(stderr, "recomp.cfg:%d: cannot set %s\n", lineno, key);
            continue;
        }
        append(summary, sizeof summary, &used, "%s%s", count ? ", " : "", "");
        append(summary, sizeof summary, &used, "%s=%s", key, val);
        count++;
    }
    fclose(f);
    printf("recomp.cfg: %d option(s) from %s%s%s\n", count, path, count ? ": " : "", summary);
    fflush(stdout);
    return count;
}
