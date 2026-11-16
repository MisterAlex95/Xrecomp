/* gc_run -- runs a GameCube DOL headless on the spike's core (XBA-225) and dumps the XFB.
 *
 *   gc_run PROG.dol [--frames N] [--png OUT] [--pack LIB.so] [--missed FILE] [--stats] [--peek ADDR[:N]] [-v]
 *
 * --frames counts video fields (59.94 per second). --png: the framebuffer the video interface scans out after the
 * last field, YUV converted to RGB. --pack: a library built from gc_tools.recomp's C (gc_recomp_title_table); its
 * functions bind to this program's bus symbols. --missed: the text addresses the interpreter had to start at after a
 * jump, one hex per line, for gc_tools.recomp --discovered. Exit status: 0, 1 when the machine stopped (unsupported
 * instruction...), 2 on bad arguments or input. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gc_core.h"

static uint32_t crc_table[256];

static void crc_init(void)
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static uint32_t crc32_of(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--)
        crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static void put32(FILE *f, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    fwrite(b, 1, 4, f);
}

static void chunk(FILE *f, const char *type, const uint8_t *data, size_t n)
{
    put32(f, (uint32_t)n);
    fwrite(type, 1, 4, f);
    if (n)
        fwrite(data, 1, n, f);
    uint32_t c = crc32_of(0, (const uint8_t *)type, 4);
    c = crc32_of(c, data, n);
    put32(f, c);
}

/* A W x H RGB PNG, zlib with stored blocks (no compression: no dependency, deterministic bytes). */
static int write_png(const char *path, const uint8_t *rgb, int w, int h)
{
    size_t row = 1 + (size_t)w * 3, n = row * (size_t)h;
    uint8_t *raw = malloc(n);
    size_t nblk = (n + 65534) / 65535;
    size_t zn = 2 + n + nblk * 5 + 4;
    uint8_t *z = malloc(zn);
    if (!raw || !z) {
        free(raw);
        free(z);
        return -1;
    }
    for (int y = 0; y < h; y++) {
        raw[y * row] = 0;
        memcpy(raw + y * row + 1, rgb + (size_t)y * w * 3, (size_t)w * 3);
    }
    size_t o = 0;
    z[o++] = 0x78;
    z[o++] = 0x01;
    uint32_t s1 = 1, s2 = 0;
    for (size_t i = 0; i < n; i++) {
        s1 = (s1 + raw[i]) % 65521;
        s2 = (s2 + s1) % 65521;
    }
    for (size_t at = 0; at < n; at += 65535) {
        size_t len = n - at < 65535 ? n - at : 65535;
        z[o++] = at + len == n ? 1 : 0;
        z[o++] = (uint8_t)len;
        z[o++] = (uint8_t)(len >> 8);
        z[o++] = (uint8_t)~len;
        z[o++] = (uint8_t)(~len >> 8);
        memcpy(z + o, raw + at, len);
        o += len;
    }
    uint32_t ad = s2 << 16 | s1;
    z[o++] = (uint8_t)(ad >> 24);
    z[o++] = (uint8_t)(ad >> 16);
    z[o++] = (uint8_t)(ad >> 8);
    z[o++] = (uint8_t)ad;
    free(raw);
    FILE *f = fopen(path, "wb");
    if (!f) {
        free(z);
        return -1;
    }
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13] = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                        (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 2, 0, 0, 0};
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, o);
    chunk(f, "IEND", NULL, 0);
    free(z);
    return fclose(f) ? -1 : 0;
}

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = n > 0 ? malloc((size_t)n) : NULL;
    if (p && fread(p, 1, (size_t)n, f) != (size_t)n) {
        free(p);
        p = NULL;
    }
    fclose(f);
    *size = p ? (size_t)n : 0;
    return p;
}

int main(int argc, char **argv)
{
    const char *dol_path = NULL, *png = NULL, *pack = NULL, *missed = NULL;
    int frames = 60, stats = 0, verbose = 0, npeek = 0;
    const char *peek[16];
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--png") && i + 1 < argc)
            png = argv[++i];
        else if (!strcmp(argv[i], "--pack") && i + 1 < argc)
            pack = argv[++i];
        else if (!strcmp(argv[i], "--missed") && i + 1 < argc)
            missed = argv[++i];
        else if (!strcmp(argv[i], "--peek") && i + 1 < argc && npeek < 16)
            peek[npeek++] = argv[++i];
        else if (!strcmp(argv[i], "--stats"))
            stats = 1;
        else if (!strcmp(argv[i], "-v"))
            verbose++;
        else if (argv[i][0] != '-' && !dol_path)
            dol_path = argv[i];
        else {
            fprintf(stderr, "usage: gc_run PROG.dol [--frames N] [--png OUT] [--pack LIB] [--missed FILE] [--stats]"
                            " [--peek ADDR[:N]] [-v]\n");
            return 2;
        }
    }
    if (!dol_path) {
        fprintf(stderr, "gc_run: no DOL\n");
        return 2;
    }
    size_t size;
    uint8_t *dol = read_file(dol_path, &size);
    if (!dol) {
        fprintf(stderr, "gc_run: cannot read %s\n", dol_path);
        return 2;
    }
    char why[160];
    gc_machine *m = gc_create(dol, size, why, sizeof why);
    free(dol);
    if (!m) {
        fprintf(stderr, "gc_run: %s: %s\n", dol_path, why);
        return 2;
    }
    gc_set_verbose(m, verbose);
    void *lib = NULL;
    if (pack) {
        lib = dlopen(pack, RTLD_NOW | RTLD_LOCAL);
        gc_recomp_title_table_fn get = lib ? (gc_recomp_title_table_fn)dlsym(lib, GC_RECOMP_ENTRY_SYMBOL) : NULL;
        if (!get || gc_attach_recomp(m, get())) {
            fprintf(stderr, "gc_run: pack %s: %s\n", pack, lib ? "no table or ABI mismatch" : dlerror());
            gc_destroy(m);
            return 2;
        }
    }
    int rc = 0;
    for (int f = 0; f < frames; f++) {
        if (gc_run_frame(m)) {
            fprintf(stderr, "gc_run: stopped in field %d: %s\n", f, gc_error(m));
            gc_dump_state(m, stderr);
            rc = 1;
            break;
        }
    }
    printf("frames %llu\n", (unsigned long long)gc_frames(m));
    if (verbose)
        gc_dump_state(m, stdout);
    if (stats) {
        gc_recomp_stats s;
        gc_recomp_get_stats(m, &s);
        printf("stats: native %llu interpreted %llu calls %llu span_checks %llu\n", (unsigned long long)s.native,
               (unsigned long long)s.interp, (unsigned long long)s.calls, (unsigned long long)s.span_checks);
    }
    for (int i = 0; i < npeek; i++) { /* --peek ADDR[:WORDS]: memory words at the end (debugging) */
        unsigned long a = strtoul(peek[i], NULL, 16), n = 1;
        const char *c = strchr(peek[i], ':');
        if (c)
            n = strtoul(c + 1, NULL, 0);
        for (unsigned long k = 0; k < n; k++)
            printf("%s%08lX", k % 8 ? " " : k ? "\n" : "peek ", (unsigned long)gc_peek32(m, (uint32_t)(a + 4 * k)));
        printf("\n");
    }
    if (missed) {
        size_t n = gc_recomp_missed(m, NULL, 0);
        uint32_t *list = malloc((n ? n : 1) * sizeof *list);
        FILE *f = fopen(missed, "w");
        if (list && f) {
            gc_recomp_missed(m, list, n);
            fprintf(f, "# gc_run --missed: text addresses the interpreter started at after a jump\n");
            for (size_t i = 0; i < n; i++)
                fprintf(f, "%08X\n", list[i]);
            printf("missed %zu\n", n);
        }
        if (f)
            fclose(f);
        free(list);
    }
    if (png) {
        static uint8_t rgb[GC_XFB_MAX_W * GC_XFB_MAX_H * 3];
        int w, h;
        crc_init();
        if (gc_xfb_rgb(m, rgb, &w, &h)) {
            fprintf(stderr, "gc_run: the video interface has no framebuffer\n");
            rc = rc ? rc : 1;
        } else if (write_png(png, rgb, w, h)) {
            fprintf(stderr, "gc_run: cannot write %s\n", png);
            rc = 2;
        } else {
            printf("xfb %dx%d\n", w, h);
        }
    }
    gc_destroy(m);
    if (lib)
        dlclose(lib);
    return rc;
}
