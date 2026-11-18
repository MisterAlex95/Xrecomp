/* n64_pack.c -- see n64_pack.h (XBA-174). The PS1's ps1_pack.c with a cartridge in place of a disc. */
#define _GNU_SOURCE
#include "n64_pack.h"

#include "console_pack.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- What is shared with the other consoles: src/platform/console_pack.c ---------------------------------------- */

_Static_assert((int)N64_PACK_LOADED == (int)CONSOLE_PACK_LOADED && (int)N64_PACK_NONE == (int)CONSOLE_PACK_NONE &&
                   (int)N64_PACK_STALE == (int)CONSOLE_PACK_STALE,
               "console_pack.h returns these");

/* The ARMv8 SHA-2 instructions when the CPU has them ($N64_PACK_SHA_C=1 forces plain C, for tests). */
static recomp_sha256_accel pack_sha = RECOMP_SHA256_ACCEL("N64_PACK_SHA_C");

static void read_table(void *entry, console_pack_table *out)
{
    const n64_recomp_table *t = ((n64_recomp_title_table_fn)entry)();
    if (!t)
        return;
    out->table = t;
    out->abi = t->abi;
    out->count = t->count;
    out->entries = t->entries;
    out->source_sha256 = t->source_sha256;
}

static const console_pack_desc pack_desc = {
    .console = "n64",
    .source_field = "source_sha256",
    .source_noun = "ROM",
    .entry_symbol = N64_RECOMP_ENTRY_SYMBOL,
    .abi_min = N64_RECOMP_ABI,
    .abi_max = N64_RECOMP_ABI,
    .abi_range = 0,
    .sha = &pack_sha,
    .read_table = read_table,
};

void n64_pack_sha256(const void *data, size_t n, uint8_t out[32])
{
    recomp_sha256(data, n, out, pack_desc.sha);
}

void n64_pack_hex(const uint8_t sum[32], char hex[65])
{
    recomp_sha256_hex(sum, hex);
}

int n64_pack_rom_sha256(const uint8_t *rom, size_t n, char hex[65])
{
    n64_rom_order order = n64_rom_detect(rom, n);
    if (order == N64_ROM_UNKNOWN) {
        hex[0] = 0;
        return -1;
    }
    recomp_sha256_ctx c;
    recomp_sha256_init(&c, pack_desc.sha);
    uint8_t buf[65536], sum[32];
    for (size_t off = 0; off < n;) {
        size_t k = n - off < sizeof buf ? n - off : sizeof buf; /* a multiple of 4 but the last piece */
        memcpy(buf, rom + off, k);
        if (order == N64_ROM_V64) {
            for (size_t i = 0; i + 1 < k; i += 2) {
                uint8_t t = buf[i];
                buf[i] = buf[i + 1];
                buf[i + 1] = t;
            }
        } else if (order == N64_ROM_N64) {
            for (size_t i = 0; i + 3 < k; i += 4) {
                uint8_t t0 = buf[i], t1 = buf[i + 1];
                buf[i] = buf[i + 3];
                buf[i + 1] = buf[i + 2];
                buf[i + 2] = t1;
                buf[i + 3] = t0;
            }
        }
        recomp_sha256_update(&c, buf, k);
        off += k;
    }
    recomp_sha256_final(&c, sum);
    n64_pack_hex(sum, hex);
    return 0;
}

int n64_pack_sha256_file(const char *path, char hex[65])
{
    return recomp_sha256_file(path, hex, pack_desc.sha);
}

const char *n64_pack_host_abi(void)
{
    return console_pack_host_abi();
}

int n64_pack_check(const char *pack_dir, const char *key, const char *source_sha256, long long shell_abi,
                   char *lib_path, size_t lib_path_n, char *why, size_t why_n)
{
    return console_pack_check(&pack_desc, pack_dir, key, source_sha256, shell_abi, lib_path, lib_path_n, why, why_n);
}

/* ---- The library ---------------------------------------------------------------------------------------------- */

int n64_pack_open(const char *lib, const char *source_sha256, n64_pack *out, char *why, size_t why_n)
{
    const void *table = NULL;
    memset(out, 0, sizeof *out);
    if (console_pack_open(&pack_desc, lib, source_sha256, &out->handle, &table, why, why_n) != 0)
        return -1;
    out->table = table;
    return 0;
}

void n64_pack_close(n64_pack *p)
{
    if (!p)
        return;
    console_pack_close(p->handle);
    memset(p, 0, sizeof *p);
}

int n64_pack_load(n64_machine *m, const char *pack_lib, const char *key, const char *source_sha256, n64_pack *out)
{
    char dir[1100], json[1200], lib[1300], why[512] = "";
    memset(out, 0, sizeof *out);
    n64_attach_recomp(m, NULL); /* whatever was attached before goes, loaded or not */
    if (!pack_lib || !*pack_lib)
        return N64_PACK_NONE;
    snprintf(dir, sizeof dir, "%s", pack_lib);
    char *slash = strrchr(dir, '/');
    if (slash)
        *slash = 0;
    else
        snprintf(dir, sizeof dir, ".");
    snprintf(json, sizeof json, "%s/pack.json", dir);
    snprintf(lib, sizeof lib, "%s", pack_lib);
    int rc = N64_PACK_LOADED;
    if (!source_sha256 || !*source_sha256) {
        rc = N64_PACK_STALE;   /* nothing to check it against: never run code that may be another game's */
        snprintf(why, sizeof why, "the ROM's identity is unknown");
    } else if (access(json, F_OK) == 0) {   /* a published pack; a bare library (a lifter's own test) has no pack.json */
        char checked[1300];
        rc = n64_pack_check(dir, key, source_sha256, N64_PACK_SHELL_ABI, checked, sizeof checked, why, sizeof why);
        if (rc == N64_PACK_LOADED && strcmp(checked, lib) != 0)
            snprintf(lib, sizeof lib, "%s", checked);   /* pack.json's lib, not the name handed over */
    } else if (access(lib, F_OK) != 0) {
        rc = N64_PACK_STALE;
        snprintf(why, sizeof why, "no %.400s", lib);
    }
    if (rc == N64_PACK_LOADED && n64_pack_open(lib, source_sha256, out, why, sizeof why) != 0)
        rc = N64_PACK_STALE;
    if (rc == N64_PACK_LOADED && n64_attach_recomp(m, out->table) != 0) {
        snprintf(why, sizeof why, "the core refused the table (abi %u)", out->table->abi);
        n64_pack_close(out);
        rc = N64_PACK_STALE;
    }
    if (rc == N64_PACK_LOADED)
        printf("PACK_LOADED n64 %s (%u functions)\n", key ? key : "?", out->table->count);
    else
        printf("PACK_STALE n64 %s (%s), running interpreted\n", key ? key : "?", why);
    fflush(stdout);
    return rc == N64_PACK_NONE ? N64_PACK_STALE : rc;
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

int n64_pack_save_missed(n64_machine *m, const char *path)
{
    if (!m || !path || !*path)
        return -1;
    size_t total = n64_recomp_missed(m, NULL, 0);
    if (!total)
        return 0;
    mkdirs_of(path);
    int added = n64_recomp_write_missed(m, path);
    if (added < 0) {
        fprintf(stderr, "n64: cannot write the discovered code to %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (added > 0) { /* saved at every pause and every few minutes: quiet when nothing is new */
        printf("n64: code at %zu address(es) the pack did not cover, %d new record(s), in %s\n", total, added, path);
        fflush(stdout);
    }
    return added;
}
