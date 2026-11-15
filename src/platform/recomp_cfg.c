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

/* 1 when key is one of RECOMP_CFG_SKIP's names (comma- or space-separated). */
static int skipped(const char *key)
{
    const char *list = getenv(RECOMP_CFG_SKIP_ENV), *p;
    size_t n = strlen(key);
    if (!list)
        return 0;
    for (p = list; *p;) {
        size_t len;
        while (*p == ',' || isspace((unsigned char)*p)) p++;
        len = strcspn(p, ", \t");
        if (len == n && len > 0 && strncmp(p, key, n) == 0)
            return 1;
        p += len;
    }
    return 0;
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
        if (skipped(key)) {
            /* The launcher's per-game settings chose the runtime's default for it (XBA-188). */
            append(summary, sizeof summary, &used, "%s%s (skipped: the game's own setting)", count ? ", " : "", key);
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

int recomp_cfg_force_key_ok(const char *key)
{
    size_t n;
    if (!key || strncmp(key, "RECOMP_", 7) != 0)
        return 0;
    n = strlen(key);
    if (n == 7 || n >= RECOMP_CFG_FORCE_KEY_MAX)
        return 0;
    for (key += 7; *key; key++)
        if (!((*key >= 'A' && *key <= 'Z') || (*key >= '0' && *key <= '9') || *key == '_'))
            return 0;
    return 1;
}

int recomp_cfg_force_value_ok(const char *value)
{
    size_t n = 0;
    if (!value)
        return 0;
    for (; value[n]; n++)
        if ((unsigned char)value[n] < 0x20 || (unsigned char)value[n] > 0x7e)
            return 0;
    return n < RECOMP_CFG_FORCE_VALUE_MAX;
}

int recomp_cfg_force(const char *tid, const char *key, const char *value)
{
    if (!tid)
        tid = "?";
    if (!recomp_cfg_force_key_ok(key)) {
        fprintf(stderr, "[TITLE] %s: env_force \"%s\" ignored (only RECOMP_[A-Z0-9_]+ names)\n", tid,
                key ? key : "");
        return -1;
    }
    if (!recomp_cfg_force_value_ok(value)) {
        fprintf(stderr, "[TITLE] %s: env_force %s ignored (value not printable ASCII under %d bytes)\n", tid,
                key, RECOMP_CFG_FORCE_VALUE_MAX);
        return -1;
    }
    if (cfg_setenv(key, value) != 0) {
        fprintf(stderr, "[TITLE] %s: cannot set %s\n", tid, key);
        return -1;
    }
    printf("[TITLE] %s forces %s=%s (title profile)\n", tid, key, value);
    fflush(stdout);
    return 0;
}
