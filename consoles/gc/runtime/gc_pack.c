/* gc_pack.c -- see gc_pack.h (XBA-264). The N64's n64_pack.c with a GameCube main DOL as the source identity. */
#define _GNU_SOURCE
#include "gc_pack.h"

#include "console_pack.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ---- What is shared with the other consoles: src/platform/console_pack.c ---------------------------------------- */

_Static_assert((int)GC_PACK_LOADED == (int)CONSOLE_PACK_LOADED && (int)GC_PACK_NONE == (int)CONSOLE_PACK_NONE &&
                   (int)GC_PACK_STALE == (int)CONSOLE_PACK_STALE,
               "console_pack.h returns these");

/* The ARMv8 SHA-2 instructions when the CPU has them ($GC_PACK_SHA_C=1 forces plain C, for tests). */
static recomp_sha256_accel pack_sha = RECOMP_SHA256_ACCEL("GC_PACK_SHA_C");

static void read_table(void *entry, console_pack_table *out)
{
    const gc_recomp_table *t = ((gc_recomp_title_table_fn)entry)();
    if (!t)
        return;
    out->table = t;
    out->abi = t->abi;
    out->count = t->count;
    out->entries = t->entries;
    out->source_sha256 = t->dol_sha256;
}

static const console_pack_desc pack_desc = {
    .console = "gc",
    .source_field = "source_sha256",
    .source_noun = "main DOL",
    .entry_symbol = GC_RECOMP_ENTRY_SYMBOL,
    .abi_min = GC_RECOMP_ABI,
    .abi_max = GC_RECOMP_ABI,
    .abi_range = 0,
    .sha = &pack_sha,
    .read_table = read_table,
};

void gc_pack_sha256(const void *data, size_t n, uint8_t out[32])
{
    recomp_sha256(data, n, out, pack_desc.sha);
}

void gc_pack_hex(const uint8_t sum[32], char hex[65])
{
    recomp_sha256_hex(sum, hex);
}

int gc_pack_sha256_file(const char *path, char hex[65])
{
    return recomp_sha256_file(path, hex, pack_desc.sha);
}

const char *gc_pack_host_abi(void)
{
    return console_pack_host_abi();
}

int gc_pack_check(const char *pack_dir, const char *key, const char *source_sha256, long long shell_abi,
                   char *lib_path, size_t lib_path_n, char *why, size_t why_n)
{
    return console_pack_check(&pack_desc, pack_dir, key, source_sha256, shell_abi, lib_path, lib_path_n, why, why_n);
}

/* ---- The library ---------------------------------------------------------------------------------------------- */

int gc_pack_open(const char *lib, const char *source_sha256, gc_pack *out, char *why, size_t why_n)
{
    const void *table = NULL;
    memset(out, 0, sizeof *out);
    if (console_pack_open(&pack_desc, lib, source_sha256, &out->handle, &table, why, why_n) != 0)
        return -1;
    out->table = table;
    return 0;
}

void gc_pack_close(gc_pack *p)
{
    if (!p)
        return;
    console_pack_close(p->handle);
    memset(p, 0, sizeof *p);
}

int gc_pack_load(gc_machine *m, const char *pack_lib, const char *key, const char *source_sha256, gc_pack *out)
{
    char dir[1100], json[1200], lib[1300], why[512] = "";
    memset(out, 0, sizeof *out);
    gc_attach_recomp(m, NULL); /* whatever was attached before goes, loaded or not */
    if (!pack_lib || !*pack_lib)
        return GC_PACK_NONE;
    snprintf(dir, sizeof dir, "%s", pack_lib);
    char *slash = strrchr(dir, '/');
    if (slash)
        *slash = 0;
    else
        snprintf(dir, sizeof dir, ".");
    snprintf(json, sizeof json, "%s/pack.json", dir);
    snprintf(lib, sizeof lib, "%s", pack_lib);
    int rc = GC_PACK_LOADED;
    if (!source_sha256 || !*source_sha256) {
        rc = GC_PACK_STALE;   /* nothing to check it against: never run code that may be another game's */
        snprintf(why, sizeof why, "the main DOL's identity is unknown");
    } else if (access(json, F_OK) == 0) {   /* a published pack; a bare library (a lifter's own test) has no pack.json */
        char checked[1300];
        rc = gc_pack_check(dir, key, source_sha256, GC_PACK_SHELL_ABI, checked, sizeof checked, why, sizeof why);
        if (rc == GC_PACK_LOADED && strcmp(checked, lib) != 0)
            snprintf(lib, sizeof lib, "%s", checked);   /* pack.json's lib, not the name handed over */
    } else if (access(lib, F_OK) != 0) {
        rc = GC_PACK_STALE;
        snprintf(why, sizeof why, "no %.400s", lib);
    }
    if (rc == GC_PACK_LOADED && gc_pack_open(lib, source_sha256, out, why, sizeof why) != 0)
        rc = GC_PACK_STALE;
    if (rc == GC_PACK_LOADED && gc_attach_recomp(m, out->table) != 0) {
        snprintf(why, sizeof why, "the core refused the table (abi %u)", out->table->abi);
        gc_pack_close(out);
        rc = GC_PACK_STALE;
    }
    if (rc == GC_PACK_LOADED)
        printf("PACK_LOADED gc %s (%u resume points, %u spans)\n", key ? key : "?", out->table->count,
               out->table->nspans);
    else
        printf("PACK_STALE gc %s (%s), running interpreted\n", key ? key : "?", why);
    fflush(stdout);
    return rc == GC_PACK_NONE ? GC_PACK_STALE : rc;
}

/* ---- Discovered code ------------------------------------------------------------------------------------------ */

int gc_pack_save_discovered(gc_machine *m, const char *path)
{
    if (!m || !path || !*path)
        return -1;
    int added = gc_recomp_write_discovered(m, path);
    if (added < 0) {
        fprintf(stderr, "gc: cannot write the discovered code to %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (added > 0) { /* saved at every pause and every few minutes: quiet when nothing is new */
        printf("gc: %zu address(es) the pack did not cover, %zu REL module record(s), %d new line(s), in %s\n",
               gc_recomp_missed(m, NULL, 0), gc_recomp_rel_records(m, NULL, 0), added, path);
        fflush(stdout);
    }
    return added;
}
