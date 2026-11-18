/* ps1_pack.c -- see ps1_pack.h (XBA-168). */
#define _GNU_SOURCE
#include "ps1_pack.h"

#include "ps1_devices.h"
#include "console_pack.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- What is shared with the other consoles: src/platform/console_pack.c ---------------------------------------- */

_Static_assert((int)PS1_PACK_LOADED == (int)CONSOLE_PACK_LOADED && (int)PS1_PACK_NONE == (int)CONSOLE_PACK_NONE &&
                   (int)PS1_PACK_STALE == (int)CONSOLE_PACK_STALE,
               "console_pack.h returns these");

static void read_table(void *entry, console_pack_table *out)
{
    const ps1_recomp_table *t = ((ps1_recomp_title_table_fn)entry)();
    if (!t)
        return;
    out->table = t;
    out->abi = t->abi;
    out->count = t->count;
    out->entries = t->entries;
    out->source_sha256 = t->source_sha256;
}

static const console_pack_desc pack_desc = {
    .console = "ps1",
    .source_field = "source_sha256",
    .source_noun = "boot EXE",
    .entry_symbol = PS1_RECOMP_ENTRY_SYMBOL,
    .abi_min = PS1_RECOMP_ABI,
    .abi_max = PS1_RECOMP_ABI,
    .abi_range = 0,
    .sha = NULL,
    .read_table = read_table,
};

void ps1_pack_sha256(const void *data, size_t n, uint8_t out[32])
{
    recomp_sha256(data, n, out, pack_desc.sha);
}

void ps1_pack_hex(const uint8_t sum[32], char hex[65])
{
    recomp_sha256_hex(sum, hex);
}

int ps1_pack_sha256_file(const char *path, char hex[65])
{
    return recomp_sha256_file(path, hex, pack_desc.sha);
}

const char *ps1_pack_host_abi(void)
{
    return console_pack_host_abi();
}

int ps1_pack_check(const char *pack_dir, const char *key, const char *source_sha256, long long shell_abi,
                   char *lib_path, size_t lib_path_n, char *why, size_t why_n)
{
    return console_pack_check(&pack_desc, pack_dir, key, source_sha256, shell_abi, lib_path, lib_path_n, why, why_n);
}

/* ---- The library ---------------------------------------------------------------------------------------------- */

int ps1_pack_open(const char *lib, const char *source_sha256, ps1_pack *out, char *why, size_t why_n)
{
    const void *table = NULL;
    memset(out, 0, sizeof *out);
    if (console_pack_open(&pack_desc, lib, source_sha256, &out->handle, &table, why, why_n) != 0)
        return -1;
    out->table = table;
    return 0;
}

void ps1_pack_close(ps1_pack *p)
{
    if (!p)
        return;
    console_pack_close(p->handle);
    memset(p, 0, sizeof *p);
}

int ps1_pack_load(ps1_machine *m, const char *pack_lib, const char *key, const char *source_sha256, ps1_pack *out)
{
    char dir[1100], json[1200], lib[1300], why[512] = "";
    memset(out, 0, sizeof *out);
    ps1_attach_recomp(m, NULL); /* whatever was attached before goes, loaded or not */
    if (!pack_lib || !*pack_lib)
        return PS1_PACK_NONE;
    snprintf(dir, sizeof dir, "%s", pack_lib);
    char *slash = strrchr(dir, '/');
    if (slash)
        *slash = 0;
    else
        snprintf(dir, sizeof dir, ".");
    snprintf(json, sizeof json, "%s/pack.json", dir);
    snprintf(lib, sizeof lib, "%s", pack_lib);
    int rc = PS1_PACK_LOADED;
    if (!source_sha256 || !*source_sha256) {
        rc = PS1_PACK_STALE;   /* nothing to check it against: never run code that may be another game's */
        snprintf(why, sizeof why, "the game's boot EXE cannot be read");
    } else if (access(json, F_OK) == 0) {   /* a published pack; a bare library (a lifter's own test) has no pack.json */
        char checked[1300];
        rc = ps1_pack_check(dir, key, source_sha256, PS1_PACK_SHELL_ABI, checked, sizeof checked, why, sizeof why);
        if (rc == PS1_PACK_LOADED && strcmp(checked, lib) != 0)
            snprintf(lib, sizeof lib, "%s", checked);   /* pack.json's lib, not the name handed over */
    } else if (access(lib, F_OK) != 0) {
        rc = PS1_PACK_STALE;
        snprintf(why, sizeof why, "no %.400s", lib);
    }
    if (rc == PS1_PACK_LOADED && ps1_pack_open(lib, source_sha256, out, why, sizeof why) != 0)
        rc = PS1_PACK_STALE;
    if (rc == PS1_PACK_LOADED && ps1_attach_recomp(m, out->table) != 0) {
        snprintf(why, sizeof why, "the core refused the table (abi %u)", out->table->abi);
        ps1_pack_close(out);
        rc = PS1_PACK_STALE;
    }
    if (rc == PS1_PACK_LOADED)
        printf("PACK_LOADED ps1 %s (%u functions)\n", key ? key : "?", out->table->count);
    else
        printf("PACK_STALE ps1 %s (%s), running interpreted\n", key ? key : "?", why);
    fflush(stdout);
    return rc == PS1_PACK_NONE ? PS1_PACK_STALE : rc;
}

/* ---- The boot EXE of a disc (consoles/ps1/tools/ps1_tools/exe.py, the same rules) ----------------------------- */

typedef struct {
    ps1_disc *d;
    uint8_t raw[2352];
} iso_rd;

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* The 2048 user bytes of sector lba (mode 2 form 1: after the subheader; mode 1: after the header), or NULL. */
static const uint8_t *iso_sector(iso_rd *r, uint32_t lba)
{
    if (ps1_disc_read_sector(r->d, lba, r->raw) != 0)
        return NULL;
    return r->raw[15] == 2 ? r->raw + 24 : r->raw + 16;
}

/* length bytes from lba into a fresh buffer (max bytes at most), or NULL. */
static uint8_t *iso_read(iso_rd *r, uint32_t lba, uint32_t length, uint32_t max)
{
    if (length > max)
        return NULL;
    uint8_t *out = malloc(length ? length : 1);
    for (uint32_t done = 0; out && done < length; done += 2048, lba++) {
        const uint8_t *u = iso_sector(r, lba);
        if (!u) {
            free(out);
            return NULL;
        }
        memcpy(out + done, u, length - done < 2048 ? length - done : 2048);
    }
    return out;
}

/* The entry `want` (no ";1", upper case) of the directory at (lba, size): its extent and size. 0, or -1. */
static int iso_dir_find(iso_rd *r, uint32_t lba, uint32_t size, const char *want, int dir, uint32_t *ext,
                        uint32_t *len)
{
    uint8_t *data = iso_read(r, lba, size, 1u << 20);
    int rc = -1;
    for (uint32_t pos = 0; data && pos < size && rc;) {
        uint32_t n = data[pos];
        if (n == 0) {
            pos = (pos / 2048 + 1) * 2048; /* records never cross a sector */
            continue;
        }
        if (n < 33 || pos + n > size)
            break;
        uint32_t nlen = data[pos + 32];
        if (33 + nlen > n)
            break;
        char name[256];
        memcpy(name, data + pos + 33, nlen);
        name[nlen] = 0;
        char *semi = strchr(name, ';');
        if (semi)
            *semi = 0;
        for (char *c = name; *c; c++)
            *c = (char)toupper((unsigned char)*c);
        int is_dir = (data[pos + 25] & 2) != 0;
        int dot = nlen == 1 && (data[pos + 33] == 0 || data[pos + 33] == 1); /* "." and ".." */
        if (!dot && !strcmp(name, want) && is_dir == dir) {
            *ext = le32(data + pos + 2);
            *len = le32(data + pos + 10);
            rc = 0;
        }
        pos += n;
    }
    free(data);
    return rc;
}

/* A file by path ("\\DIR\\NAME.EXT;1", '\\' or '/', case-insensitive) from the root of pvd: extent and size. */
static int iso_find(iso_rd *r, const uint8_t *pvd, const char *path, uint32_t *ext, uint32_t *len)
{
    uint32_t lba = le32(pvd + 156 + 2), size = le32(pvd + 156 + 10);
    char parts[16][256];
    int np = 0;
    for (const char *p = path; *p;) {
        while (*p == '\\' || *p == '/')
            p++;
        if (!*p)
            break;
        if (np == 16)
            return -1;
        size_t k = 0;
        while (*p && *p != '\\' && *p != '/') {
            if (k < 255)
                parts[np][k++] = (char)toupper((unsigned char)*p);
            p++;
        }
        parts[np][k] = 0;
        char *semi = strchr(parts[np], ';');
        if (semi)
            *semi = 0;
        np++;
    }
    for (int i = 0; i < np; i++)
        if (iso_dir_find(r, lba, size, parts[i], i < np - 1, &lba, &size) != 0)
            return -1;
    *ext = lba;
    *len = size;
    return np ? 0 : -1;
}

/* SYSTEM.CNF's first `BOOT \s* = \s* \S+` (case-insensitive, as re.search), "cdrom:" or "cdrom" dropped. */
static int cnf_boot(const char *cnf, char *out, size_t n)
{
    for (const char *p = cnf; *p; p++) {
        if (strncasecmp(p, "BOOT", 4))
            continue;
        const char *q = p + 4;
        while (*q && isspace((unsigned char)*q))
            q++;
        if (*q != '=')
            continue;
        q++;
        while (*q && isspace((unsigned char)*q))
            q++;
        const char *e = q;
        while (*e && !isspace((unsigned char)*e))
            e++;
        if (e == q)
            continue;
        if (e - q >= 5 && !strncasecmp(q, "cdrom", 5)) {
            q += 5;
            if (*q == ':')
                q++;
        }
        snprintf(out, n, "%.*s", (int)(e - q), q);
        return 0;
    }
    return -1;
}

int ps1_pack_disc_boot_sha256(const char *disc, char hex[65], char *boot, size_t boot_n, char *why, size_t why_n)
{
    uint8_t *exe = NULL;
    size_t len = 0;
    hex[0] = 0;
    if (ps1_pack_disc_boot_exe(disc, &exe, &len, boot, boot_n, why, why_n) != 0)
        return -1;
    uint8_t sum[32];
    ps1_pack_sha256(exe, len, sum);
    ps1_pack_hex(sum, hex);
    free(exe);
    return 0;
}

int ps1_pack_disc_boot_exe(const char *disc, uint8_t **exe_out, size_t *exe_n, char *boot, size_t boot_n, char *why,
                           size_t why_n)
{
    char err[256] = "", path[512] = "PSX.EXE";
    iso_rd r;
    uint32_t ext, len;
    uint8_t pvd[2048];
    const uint8_t *u;
    int rc = -1;
    *exe_out = NULL;
    *exe_n = 0;
    r.d = disc && *disc ? ps1_disc_open(disc, err, sizeof err) : NULL;
    if (!r.d) {
        snprintf(why, why_n, "the disc cannot be opened: %s", err[0] ? err : "no disc");
        return -1;
    }
    if (!(u = iso_sector(&r, 16)) || u[0] != 1 || memcmp(u + 1, "CD001", 5)) {
        snprintf(why, why_n, "no ISO 9660 volume on the disc");
        goto out;
    }
    memcpy(pvd, u, sizeof pvd);
    if (iso_find(&r, pvd, "SYSTEM.CNF", &ext, &len) == 0) {
        uint8_t *cnf = iso_read(&r, ext, len, 64u << 10);
        char *text = cnf ? malloc((size_t)len + 1) : NULL;
        if (text) {
            memcpy(text, cnf, len);
            text[len] = 0;
            for (uint32_t i = 0; i < len; i++)
                if (!text[i])
                    text[i] = ' '; /* latin-1 text: a NUL does not end it */
            if (cnf_boot(text, path, sizeof path) != 0)
                snprintf(path, sizeof path, "PSX.EXE");
        }
        free(text);
        free(cnf);
    }
    if (iso_find(&r, pvd, path, &ext, &len) != 0) {
        snprintf(why, why_n, "the boot EXE %s is not on the disc", path);
        goto out;
    }
    uint8_t *exe = iso_read(&r, ext, len, PS1_PACK_EXE_MAX);
    if (!exe) {
        snprintf(why, why_n, "the boot EXE %s cannot be read (%u bytes)", path, len);
        goto out;
    }
    *exe_out = exe;
    *exe_n = len;
    if (boot && boot_n)
        snprintf(boot, boot_n, "%s", path);
    rc = 0;
out:
    ps1_disc_close(r.d);
    return rc;
}

/* ---- Discovered code ------------------------------------------------------------------------------------------ */

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

int ps1_pack_save_missed(ps1_machine *m, const char *path)
{
    if (!m || !path || !*path)
        return -1;
    size_t total = ps1_recomp_missed(m, NULL, 0);
    if (!total)
        return 0;
    mkdirs_of(path);
    int added = ps1_recomp_write_missed(m, path);
    if (added < 0) {
        fprintf(stderr, "ps1: cannot write the discovered code to %s: %s\n", path, strerror(errno));
        return -1;
    }
    printf("ps1: code at %zu address(es) the pack did not cover, %d new record(s), in %s\n", total, added, path);
    fflush(stdout);
    return added;
}
