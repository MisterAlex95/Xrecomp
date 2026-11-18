/* gba_pack.c -- see gba_pack.h (XBA-157 / XBA-158). */
#define _GNU_SOURCE
#include "gba_pack.h"

#include "console_pack.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- What is shared with the other consoles: src/platform/console_pack.c ---------------------------------------- */

_Static_assert((int)GBA_PACK_LOADED == (int)CONSOLE_PACK_LOADED && (int)GBA_PACK_NONE == (int)CONSOLE_PACK_NONE &&
                   (int)GBA_PACK_STALE == (int)CONSOLE_PACK_STALE,
               "console_pack.h returns these");

static void read_table(void *entry, console_pack_table *out)
{
    const gba_recomp_table *t = ((gba_recomp_title_table_fn)entry)();
    if (!t)
        return;
    out->table = t;
    out->abi = t->abi;
    out->count = t->count;
    out->entries = t->entries;
    out->source_sha256 = t->rom_sha256;
}

static const console_pack_desc pack_desc = {
    .console = "gba",
    .source_field = "rom_sha256",
    .source_noun = "ROM",
    .entry_symbol = GBA_RECOMP_ENTRY_SYMBOL,
    .abi_min = GBA_RECOMP_ABI_MIN,
    .abi_max = GBA_RECOMP_ABI,
    .abi_range = 1,
    .sha = NULL,
    .read_table = read_table,
};

void gba_pack_sha256(const void *data, size_t n, uint8_t out[32])
{
    recomp_sha256(data, n, out, pack_desc.sha);
}

void gba_pack_hex(const uint8_t sum[32], char hex[65])
{
    recomp_sha256_hex(sum, hex);
}

int gba_pack_sha256_file(const char *path, char hex[65])
{
    return recomp_sha256_file(path, hex, pack_desc.sha);
}

const char *gba_pack_host_abi(void)
{
    return console_pack_host_abi();
}

int gba_pack_check(const char *pack_dir, const char *key, const char *rom_sha256, long long shell_abi,
                   char *lib_path, size_t lib_path_n, char *why, size_t why_n)
{
    return console_pack_check(&pack_desc, pack_dir, key, rom_sha256, shell_abi, lib_path, lib_path_n, why, why_n);
}

/* ---- The library ---------------------------------------------------------------------------------------------- */

int gba_pack_open(const char *lib, const char *rom_sha256, gba_pack *out, char *why, size_t why_n)
{
    const void *table = NULL;
    memset(out, 0, sizeof *out);
    if (console_pack_open(&pack_desc, lib, rom_sha256, &out->handle, &table, why, why_n) != 0)
        return -1;
    out->table = table;
    return 0;
}

void gba_pack_close(gba_pack *p)
{
    if (!p)
        return;
    console_pack_close(p->handle);
    memset(p, 0, sizeof *p);
}

int gba_pack_load(gba_machine *m, const char *pack_lib, const char *key, const char *rom_sha256, gba_pack *out)
{
    char dir[1100], json[1200], lib[1300], why[512] = "";
    memset(out, 0, sizeof *out);
    gba_attach_recomp(m, NULL); /* whatever was attached before goes, loaded or not */
    if (!pack_lib || !*pack_lib)
        return GBA_PACK_NONE;
    snprintf(dir, sizeof dir, "%s", pack_lib);
    char *slash = strrchr(dir, '/');
    if (slash)
        *slash = 0;
    else
        snprintf(dir, sizeof dir, ".");
    snprintf(json, sizeof json, "%s/pack.json", dir);
    snprintf(lib, sizeof lib, "%s", pack_lib);
    int rc = GBA_PACK_LOADED;
    if (access(json, F_OK) == 0) {   /* a published pack; a bare library (a lifter's own test) has no pack.json */
        char checked[1300];
        rc = gba_pack_check(dir, key, rom_sha256, GBA_PACK_SHELL_ABI, checked, sizeof checked, why, sizeof why);
        if (rc == GBA_PACK_LOADED && strcmp(checked, lib) != 0)
            snprintf(lib, sizeof lib, "%s", checked);   /* pack.json's lib, not the name handed over */
    } else if (access(lib, F_OK) != 0) {
        rc = GBA_PACK_STALE;
        snprintf(why, sizeof why, "no %.400s", lib);
    }
    if (rc == GBA_PACK_LOADED && gba_pack_open(lib, rom_sha256, out, why, sizeof why) != 0)
        rc = GBA_PACK_STALE;
    if (rc == GBA_PACK_LOADED && gba_attach_recomp(m, out->table) != 0) {
        snprintf(why, sizeof why, "the core refused the table (abi %u)", out->table->abi);
        gba_pack_close(out);
        rc = GBA_PACK_STALE;
    }
    if (rc == GBA_PACK_LOADED)
        printf("PACK_LOADED gba %s (%u functions)\n", key ? key : "?", out->table->count);
    else
        printf("PACK_STALE gba %s (%s), running interpreted\n", key ? key : "?", why);
    fflush(stdout);
    return rc == GBA_PACK_NONE ? GBA_PACK_STALE : rc;
}

/* ---- Discovered addresses ------------------------------------------------------------------------------------- */

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static void mkdirs_of(const char *path)
{
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s", path);
    char *slash = strrchr(tmp, '/');
    if (!slash || slash == tmp)
        return;
    *slash = 0;
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    mkdir(tmp, 0755);
}

static size_t uniq(uint32_t *v, size_t n)
{
    size_t k = 0;
    qsort(v, n, sizeof *v, cmp_u32);
    for (size_t i = 0; i < n; i++)
        if (k == 0 || v[k - 1] != v[i])
            v[k++] = v[i];
    return k;
}

int gba_pack_write_discovered(const char *path, const uint32_t *addrs, size_t n, size_t *added)
{
    size_t cap = GBA_PACK_DISCOVERED_MAX + n, have = 0, len = 0;
    if (added)
        *added = 0;
    if (!path || !*path)
        return -1;
    uint32_t *v = malloc((cap ? cap : 1) * sizeof *v);
    if (!v)
        return -1;
    char *text = console_pack_read_text(path, 16u << 20, &len);
    if (text) {
        for (char *line = strtok(text, "\n"); line && have < GBA_PACK_DISCOVERED_MAX; line = strtok(NULL, "\n")) {
            char *hash = strchr(line, '#');
            if (hash)
                *hash = 0;
            char *end;
            while (isspace((unsigned char)*line))
                line++;
            if (!*line)
                continue;
            unsigned long long x = strtoull(line, &end, 16);
            if (end != line && x <= 0xFFFFFFFFull)
                v[have++] = (uint32_t)x;
        }
        free(text);
    }
    have = uniq(v, have);
    size_t before = have;
    memcpy(v + have, addrs, n * sizeof *v);
    size_t total = uniq(v, have + n);
    if (total > GBA_PACK_DISCOVERED_MAX)
        total = GBA_PACK_DISCOVERED_MAX;
    size_t fresh = total > before ? total - before : 0;
    if (added)
        *added = fresh;
    if (!fresh) {
        free(v);
        return 0;
    }
    char tmp[1200];
    mkdirs_of(path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    int ok = f != NULL;
    for (size_t i = 0; ok && i < total; i++)
        ok = fprintf(f, "0x%08X\n", (unsigned)v[i]) > 0;
    if (f) {
        ok &= fflush(f) == 0;
        ok &= fsync(fileno(f)) == 0;
        ok &= fclose(f) == 0;
    }
    free(v);
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

int gba_pack_save_missed(gba_machine *m, const char *path)
{
    if (!m || !path || !*path)
        return -1;
    size_t total = gba_recomp_missed(m, NULL, 0);
    if (!total)
        return 0;
    if (total > GBA_PACK_DISCOVERED_MAX)
        total = GBA_PACK_DISCOVERED_MAX;
    uint32_t *v = malloc(total * sizeof *v);
    if (!v)
        return -1;
    size_t got = gba_recomp_missed(m, v, total), added = 0;
    if (got > total)
        got = total;
    int rc = gba_pack_write_discovered(path, v, got, &added);
    free(v);
    if (rc != 0) {
        fprintf(stderr, "gba: cannot write the discovered addresses to %s: %s\n", path, strerror(errno));
        return -1;
    }
    printf("gba: %zu address(es) the pack did not cover, %zu new, in %s\n", got, added, path);
    fflush(stdout);
    return (int)added;
}
