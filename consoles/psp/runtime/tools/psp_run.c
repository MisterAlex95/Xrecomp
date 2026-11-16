/* psp_run -- runs a PSP module headless on the spike's core (XBA-224) and dumps the framebuffer.
 *
 *   psp_run MODULE (EBOOT.PBP / .prx / .elf) [--frames N] [--png OUT] [--pack LIB.so] [--missed FILE] [--stats]
 *           [--buttons HEX] [-v]
 *
 * Runs N frames (default 60) or until the program exits (sceKernelExitGame). --pack: a library built from
 * psp_tools.recomp's C (psp_recomp_title_table); its functions bind to this program's bus symbols. --missed: the
 * addresses the interpreter had to start at inside the module's code, one hex per line, for psp_tools.recomp
 * --discovered. Exit status: 0 (ran, or the program exited), 1 when the machine stopped on an error, 2 on bad
 * arguments or input. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psp_core.h"

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

/* A 480x272 RGB PNG, zlib with stored blocks (no compression: no dependency, deterministic bytes). */
static int write_png(const char *path, const uint8_t *rgb)
{
    enum { W = PSP_SCREEN_W, H = PSP_SCREEN_H, ROW = 1 + W * 3 };
    static uint8_t raw[ROW * H];
    for (int y = 0; y < H; y++) {
        raw[y * ROW] = 0;
        memcpy(&raw[y * ROW + 1], rgb + (size_t)y * W * 3, W * 3);
    }
    size_t n = sizeof raw, nblk = (n + 65534) / 65535;
    size_t zn = 2 + n + nblk * 5 + 4;
    uint8_t *z = malloc(zn);
    if (!z)
        return -1;
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
    FILE *f = fopen(path, "wb");
    if (!f) {
        free(z);
        return -1;
    }
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13] = {0, 0, W >> 8, W & 0xFF, 0, 0, H >> 8, H & 0xFF, 8, 2, 0, 0, 0};
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
    const char *path = NULL, *png = NULL, *pack = NULL, *missed = NULL;
    int frames = 60, stats = 0, verbose = 0;
    uint32_t buttons = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--png") && i + 1 < argc)
            png = argv[++i];
        else if (!strcmp(argv[i], "--pack") && i + 1 < argc)
            pack = argv[++i];
        else if (!strcmp(argv[i], "--missed") && i + 1 < argc)
            missed = argv[++i];
        else if (!strcmp(argv[i], "--buttons") && i + 1 < argc)
            buttons = (uint32_t)strtoul(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--stats"))
            stats = 1;
        else if (!strcmp(argv[i], "-v"))
            verbose++;
        else if (argv[i][0] != '-' && !path)
            path = argv[i];
        else {
            fprintf(stderr, "usage: psp_run MODULE [--frames N] [--png OUT] [--pack LIB] [--missed FILE] [--stats]"
                            " [--buttons HEX] [-v]\n");
            return 2;
        }
    }
    if (!path) {
        fprintf(stderr, "psp_run: no module\n");
        return 2;
    }
    size_t size;
    uint8_t *data = read_file(path, &size);
    if (!data) {
        fprintf(stderr, "psp_run: cannot read %s\n", path);
        return 2;
    }
    char why[200];
    psp_machine *m = psp_create(data, size, why, sizeof why);
    free(data);
    if (!m) {
        fprintf(stderr, "psp_run: %s: %s\n", path, why);
        return 2;
    }
    psp_set_verbose(m, verbose);
    psp_set_buttons(m, buttons);
    void *lib = NULL;
    if (pack) {
        lib = dlopen(pack, RTLD_NOW | RTLD_LOCAL);
        psp_recomp_title_table_fn get = lib ? (psp_recomp_title_table_fn)dlsym(lib, PSP_RECOMP_ENTRY_SYMBOL) : NULL;
        if (!get || psp_attach_recomp(m, get())) {
            fprintf(stderr, "psp_run: pack %s: %s\n", pack, lib ? "no table, ABI or load address mismatch" : dlerror());
            psp_destroy(m);
            return 2;
        }
    }
    int rc = 0;
    for (int f = 0; f < frames; f++) {
        int r = psp_run_frame(m);
        if (r == 1) {
            fprintf(stderr, "psp_run: stopped in frame %d: %s\n", f, psp_error(m));
            psp_dump_trace(m, stderr);
            rc = 1;
            break;
        }
        if (r == 2) {
            printf("exited in frame %d\n", f);
            break;
        }
    }
    printf("frames %llu\n", (unsigned long long)psp_frames(m));
    if (verbose)
        psp_dump_trace(m, stderr);
    if (stats) {
        psp_recomp_stats s;
        psp_recomp_get_stats(m, &s);
        printf("stats: native %llu interpreted %llu calls %llu entries %u stale %u\n", (unsigned long long)s.native,
               (unsigned long long)s.interp, (unsigned long long)s.calls, s.entries, s.stale);
    }
    if (missed) {
        size_t n = psp_recomp_missed(m, NULL, 0);
        uint32_t *list = malloc((n ? n : 1) * sizeof *list);
        FILE *f = fopen(missed, "w");
        if (list && f) {
            psp_recomp_missed(m, list, n);
            fprintf(f, "# psp_run --missed: interpreter starts inside the module's code\n");
            for (size_t i = 0; i < n; i++)
                fprintf(f, "%08X\n", list[i]);
            printf("missed %zu\n", n);
        }
        if (f)
            fclose(f);
        free(list);
    }
    if (png) {
        static uint8_t rgb[PSP_SCREEN_W * PSP_SCREEN_H * 3];
        crc_init();
        if (psp_screen_rgb(m, rgb)) {
            fprintf(stderr, "psp_run: no framebuffer set (sceDisplaySetFrameBuf never called)\n");
            rc = rc ? rc : 1;
        } else if (write_png(png, rgb)) {
            fprintf(stderr, "psp_run: cannot write %s\n", png);
            rc = 2;
        }
    }
    psp_destroy(m);
    if (lib)
        dlclose(lib);
    return rc;
}
