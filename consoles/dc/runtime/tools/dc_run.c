/* dc_run -- runs a Dreamcast program headless on the spike's core (XBA-227) and dumps what it showed.
 *
 *   dc_run PROGRAM.elf|1ST_READ.BIN [--frames N] [--png OUT] [--png-when TEXT] [--serial OUT] [--pack LIB.so]
 *          [--missed FILE] [--stats] [--peek ADDR[:WORDS]] [-v] [-v]
 *   dc_run --decode-table OUT     the decoder's view of all 65536 opcodes (compared with dc_tools.sh4 --table)
 *
 * Runs until the program returns to the BIOS or N frames (default 600) have passed. --png: the framebuffer at the
 * end, or with --png-when at the end of the first frame whose serial output holds TEXT (programs clear the screen on
 * their way out); --serial: what the program wrote to the serial port (also on stdout after "serial:" with -v). --pack: a
 * library built from dc_tools.recomp's C (dc_recomp_title_table); its functions bind to this program's bus
 * symbols. --missed: the addresses the interpreter had to start at inside the lifted image, one hex per line, for
 * dc_tools.recomp --discovered. Exit status: 0, 1 when the program did not return within N frames or returned
 * non-zero, 2 on bad arguments or input. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dc_core.h"

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

/* An RGB PNG, zlib with stored blocks (no compression: no dependency, deterministic bytes). */
static int write_png(const char *path, const uint8_t *rgb, int w, int h)
{
    size_t row = 1 + (size_t)w * 3, n = row * (size_t)h;
    uint8_t *raw = malloc(n);
    size_t nblk = (n + 65534) / 65535, zn = 2 + n + nblk * 5 + 4;
    uint8_t *z = malloc(zn);
    if (!raw || !z) {
        free(raw);
        free(z);
        return -1;
    }
    for (int y = 0; y < h; y++) {
        raw[(size_t)y * row] = 0;
        memcpy(raw + (size_t)y * row + 1, rgb + (size_t)y * (size_t)w * 3, (size_t)w * 3);
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
    FILE *f = fopen(path, "wb");
    int rc = -1;
    if (f) {
        static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
        fwrite(sig, 1, 8, f);
        uint8_t ihdr[13] = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                            (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h,
                            8, 2, 0, 0, 0};
        chunk(f, "IHDR", ihdr, 13);
        chunk(f, "IDAT", z, o);
        chunk(f, "IEND", NULL, 0);
        rc = fclose(f) ? -1 : 0;
    }
    free(raw);
    free(z);
    return rc;
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

static int usage(void)
{
    fprintf(stderr, "usage: dc_run PROGRAM [--frames N] [--png OUT] [--serial OUT] [--pack LIB] [--missed FILE]"
                    " [--png-when TEXT] [--stats] [--peek ADDR[:WORDS]] [-v]\n       dc_run --decode-table OUT\n");
    return 2;
}

int main(int argc, char **argv)
{
    const char *path = NULL, *png = NULL, *png_when = NULL, *serial = NULL, *pack = NULL, *missed = NULL;
    int frames = 600, stats = 0, verbose = 0, npeek = 0;
    const char *peek[16];
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--decode-table") && i + 1 < argc) {
            FILE *f = fopen(argv[++i], "w");
            if (!f)
                return 2;
            dc_dump_decode_table(f);
            return fclose(f) ? 2 : 0;
        } else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--png") && i + 1 < argc)
            png = argv[++i];
        else if (!strcmp(argv[i], "--png-when") && i + 1 < argc)
            png_when = argv[++i];
        else if (!strcmp(argv[i], "--serial") && i + 1 < argc)
            serial = argv[++i];
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
        else if (argv[i][0] != '-' && !path)
            path = argv[i];
        else
            return usage();
    }
    if (!path)
        return usage();
    size_t size;
    uint8_t *data = read_file(path, &size);
    if (!data) {
        fprintf(stderr, "dc_run: cannot read %s\n", path);
        return 2;
    }
    dc_machine *m = dc_create();
    char why[160];
    if (!m || dc_load(m, data, size, why, sizeof why)) {
        fprintf(stderr, "dc_run: %s: %s\n", path, m ? why : "out of memory");
        free(data);
        dc_destroy(m);
        return 2;
    }
    free(data);
    dc_set_verbose(m, verbose);
    void *lib = NULL;
    if (pack) {
        lib = dlopen(pack, RTLD_NOW | RTLD_LOCAL);
        dc_recomp_title_table_fn get = lib ? (dc_recomp_title_table_fn)dlsym(lib, DC_RECOMP_ENTRY_SYMBOL) : NULL;
        if (!get || dc_attach_recomp(m, get())) {
            fprintf(stderr, "dc_run: pack %s: %s\n", pack, lib ? "no table, ABI or bytes mismatch" : dlerror());
            dc_destroy(m);
            return 2;
        }
    }
    uint8_t *rgb = png ? malloc((size_t)DC_FB_MAX_W * DC_FB_MAX_H * 3) : NULL;
    int w = 0, h = 0, have_png = 0, ran = 0, code = 0;
    if (png && !rgb) {
        dc_destroy(m);
        return 2;
    }
    while (ran < frames && !dc_exited(m, NULL)) {
        ran += dc_run_frames(m, 1);
        if (png_when && !have_png && strstr(dc_serial(m, NULL), png_when))
            have_png = dc_framebuffer_rgb(m, rgb, &w, &h) == 0 ? 1 : -1;
    }
    bool exited = dc_exited(m, &code);
    printf("frames %d%s\n", ran, exited ? "" : " (did not return)");
    if (exited)
        printf("exit %d\n", code);
    if (stats) {
        dc_stats s;
        dc_get_stats(m, &s);
        printf("stats: native %llu interpreted %llu calls %llu irqs %llu bios %llu\n", (unsigned long long)s.native,
               (unsigned long long)s.interp, (unsigned long long)s.calls, (unsigned long long)s.irqs,
               (unsigned long long)s.hle);
    }
    for (int i = 0; i < npeek; i++) {
        unsigned long a = strtoul(peek[i], NULL, 16), n = 1;
        const char *c = strchr(peek[i], ':');
        if (c)
            n = strtoul(c + 1, NULL, 0);
        for (unsigned long k = 0; k < n; k++)
            printf("%s%08lX", k % 8 ? " " : k ? "\n" : "peek ", (unsigned long)dc_peek32(m, (uint32_t)(a + 4 * k)));
        printf("\n");
    }
    int rc = exited && code == 0 ? 0 : 1;
    if (missed) {
        size_t n = dc_recomp_missed(m, NULL, 0);
        uint32_t *list = malloc((n ? n : 1) * sizeof *list);
        FILE *f = fopen(missed, "w");
        if (list && f) {
            dc_recomp_missed(m, list, n);
            fprintf(f, "# dc_run --missed: interpreter starts inside the lifted image\n");
            for (size_t i = 0; i < n; i++)
                fprintf(f, "%08X\n", list[i]);
            printf("missed %zu\n", n);
        }
        if (f)
            fclose(f);
        free(list);
    }
    if (serial) {
        size_t len;
        const char *s = dc_serial(m, &len);
        FILE *f = fopen(serial, "wb");
        if (!f || fwrite(s, 1, len, f) != len) {
            fprintf(stderr, "dc_run: cannot write %s\n", serial);
            rc = 2;
        }
        if (f)
            fclose(f);
    }
    if (png) {
        crc_init();
        if (!png_when)
            have_png = dc_framebuffer_rgb(m, rgb, &w, &h) == 0 ? 1 : -1;
        if (have_png != 1) {
            fprintf(stderr, "dc_run: %s\n", have_png ? "display off: no picture" : "--png-when text never came");
            rc = rc ? rc : 1;
        } else if (write_png(png, rgb, w, h)) {
            fprintf(stderr, "dc_run: cannot write %s\n", png);
            rc = 2;
        }
        free(rgb);
    }
    dc_destroy(m);
    if (lib)
        dlclose(lib);
    return rc;
}
