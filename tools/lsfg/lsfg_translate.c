/* lsfg_translate: fills the LSFG shader cache from the user's own Lossless.dll (docs/technical/framegen.md).
 *
 *   lsfg_translate [-o CACHE_BASE] Lossless.dll    extract + translate into CACHE_BASE/<key> (default: the desktop
 *                                                  cache base, lsfg_cache_default_base) and make it current
 *   lsfg_translate --dir CACHE_DIR                 translate the .dxbc files of an existing cache directory (one
 *                                                  the Android app made from a picked DLL, pulled from the device)
 *
 * Extracts the LSFG 3.1 and 3.1 performance DXBC blobs (and any embedded SPIR-V), translates each DXBC shader to
 * SPIR-V with DXVK's compiler (lsfg_dxbc.c), renumbers the bindings to framegen's layout and checks them
 * (lsfg_spv.c), and writes the cache and its manifest. Prints counts and sizes only, never shader contents. The
 * DLL and everything written here belong to the user: never commit, bundle or upload them. */
#include "lsfg_cache.h"
#include "lsfg_dxbc.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void usage(void)
{
    fprintf(stderr, "usage: lsfg_translate [-o CACHE_BASE] Lossless.dll\n"
                    "       lsfg_translate --dir CACHE_DIR\n");
}

/* Number and total size of the files in dir ending in suffix (and not in a longer known suffix). */
static void tally(const char *dir, const char *suffix, unsigned *n, unsigned long long *bytes)
{
    *n = 0;
    *bytes = 0;
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    size_t sl = strlen(suffix);
    while ((e = readdir(d)) != NULL) {
        size_t l = strlen(e->d_name);
        if (l <= sl || strcmp(e->d_name + l - sl, suffix) != 0)
            continue;
        if (strcmp(suffix, ".spv") == 0 && (strstr(e->d_name, ".fp16.") || strstr(e->d_name, ".fp32.")))
            continue;
        char p[4096];
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (stat(p, &st) == 0) {
            (*n)++;
            *bytes += (unsigned long long)st.st_size;
        }
    }
    closedir(d);
}

static void report(const char *dir, const LsfgCacheInfo *info)
{
    printf("cache      %s\n", dir);
    printf("dll        sha256 %.16s..., %llu bytes, %u RT_RCDATA resources\n", info->dll_sha256,
           (unsigned long long)info->dll_size, info->rcdata);
    for (int v = 0; v < 2; v++)
        printf("%-10s %u/%u DXBC, %u/%u translated, %u/%u FP16, %u/%u FP32\n", v ? "lsfg_3_1p" : "lsfg_3_1",
               info->dxbc[v], info->needed[v], info->spv[v], info->needed[v], info->fp16[v], info->needed[v],
               info->fp32[v], info->needed[v]);
    static const char *const suffixes[] = { ".dxbc", ".spv", ".fp16.spv", ".fp32.spv" };
    for (int i = 0; i < 4; i++) {
        unsigned n;
        unsigned long long b;
        tally(dir, suffixes[i], &n, &b);
        if (n)
            printf("files      %2u x %-9s %llu bytes\n", n, suffixes[i], b);
    }
    if (info->translator[0])
        printf("translator %s\n", info->translator);
}

int main(int argc, char **argv)
{
    const char *base = NULL, *dll = NULL, *dir = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc)
            base = argv[++i];
        else if (!strcmp(argv[i], "--dir") && i + 1 < argc)
            dir = argv[++i];
        else if (argv[i][0] == '-') {
            usage();
            return 2;
        } else
            dll = argv[i];
    }
    if (!getenv("DXVK_LOG_LEVEL"))
        setenv("DXVK_LOG_LEVEL", "warn", 1);
    if (!getenv("DXVK_LOG_PATH"))
        setenv("DXVK_LOG_PATH", "none", 1);
    char err[1024] = "", out[4096], def[2048];
    LsfgCacheInfo info;
    int rc;
    if (dir) {
        rc = lsfg_cache_translate(dir, lsfg_dxbc_translator_name(), lsfg_dxbc_translate, NULL, &info, err, sizeof err);
        snprintf(out, sizeof out, "%s", dir);
    } else if (dll) {
        if (!base) {
            if (lsfg_cache_default_base(def, sizeof def)) {
                fprintf(stderr, "lsfg_translate: no HOME: pass -o CACHE_BASE\n");
                return 2;
            }
            base = def;
        }
        rc = lsfg_cache_import_path(dll, base, lsfg_dxbc_translator_name(), lsfg_dxbc_translate, NULL, &info, out,
                                    sizeof out, err, sizeof err);
    } else {
        usage();
        return 2;
    }
    if (rc && rc != LSFG_CACHE_ETRANSLATE) {
        fprintf(stderr, "lsfg_translate: %s%s%s\n", lsfg_cache_strerror(rc), err[0] ? ": " : "", err);
        return 1;
    }
    report(out, &info);
    if (rc) {
        fprintf(stderr, "lsfg_translate: %s\n", err);
        return 1;
    }
    return lsfg_cache_variant_ready(&info, 0, NULL, NULL, NULL) || lsfg_cache_variant_ready(&info, 1, NULL, NULL, NULL)
               ? 0 : 1;
}
