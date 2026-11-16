/* gc_run -- runs a GameCube DOL headless on the spike's core (XBA-225) and dumps the XFB.
 *
 *   gc_run PROG.dol [--frames N] [--png OUT] [--pack LIB.so] [--missed FILE] [--stats] [--peek ADDR[:N]] [--pal]
 *          [--pad PORT:BUTTONS[:SX:SY:CX:CY:L:R]]... [-v]
 *
 * --frames counts video fields (60 per second, 50 in PAL). --pal: the IPL's PAL state (gc_set_tv_mode). --pad: a
 * standard controller on port PORT (0..3) for the whole run, BUTTONS in hex (GC_PAD_*), then sticks and triggers in
 * decimal (0..255, sticks at rest 128; XBA-259). --png: the framebuffer the video interface scans out after the
 * last field, YUV converted to RGB. --pack: a library built from gc_tools.recomp's C (gc_recomp_title_table); its
 * functions bind to this program's bus symbols. --missed: the text addresses the interpreter had to start at after a
 * jump, one hex per line, for gc_tools.recomp --discovered. Exit status: 0, 1 when the machine stopped (unsupported
 * instruction...), 2 on bad arguments or input. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gc_core.h"
#include "gc_png.h"

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
    int pal = 0;
    gc_pad_state pads[4];
    memset(pads, 0, sizeof pads);
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
        else if (!strcmp(argv[i], "--pal"))
            pal = 1;
        else if (!strcmp(argv[i], "--pad") && i + 1 < argc) {
            unsigned port = 4, b = 0, v[6] = {128, 128, 128, 128, 0, 0};
            int n = sscanf(argv[++i], "%u:%x:%u:%u:%u:%u:%u:%u", &port, &b, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
            if (n < 2 || port > 3) {
                fprintf(stderr, "gc_run: --pad PORT:BUTTONS[:SX:SY:CX:CY:L:R]\n");
                return 2;
            }
            pads[port] = (gc_pad_state){true, (uint16_t)b, (uint8_t)v[0], (uint8_t)v[1], (uint8_t)v[2], (uint8_t)v[3],
                                        (uint8_t)v[4], (uint8_t)v[5]};
        } else if (!strcmp(argv[i], "--stats"))
            stats = 1;
        else if (!strcmp(argv[i], "-v"))
            verbose++;
        else if (argv[i][0] != '-' && !dol_path)
            dol_path = argv[i];
        else {
            fprintf(stderr, "usage: gc_run PROG.dol [--frames N] [--png OUT] [--pack LIB] [--missed FILE] [--stats]"
                            " [--peek ADDR[:N]] [--pal] [--pad PORT:BUTTONS[:SX:SY:CX:CY:L:R]] [-v]\n");
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
    if (pal)
        gc_set_tv_mode(m, GC_TV_PAL);
    for (int p = 0; p < 4; p++)
        gc_set_pad(m, p, &pads[p]);
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
