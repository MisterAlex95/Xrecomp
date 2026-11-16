/* ps2_run -- runs a PS2 EE executable headless on the spike's core (XBA-226) and prints what it wrote to the SIO.
 *
 *   ps2_run ELF [--insns N] [--sio OUT] [--pack LIB.so] [--missed FILE] [--stats] [-v]
 *
 * Runs until the program exits (the Exit syscall, or the main thread returning) or N instructions have run
 * (default 200 million). The SIO text goes to stdout, or to OUT with --sio. --pack: a library built from
 * ps2_tools.recomp's C (ps2_recomp_title_table); its functions bind to this program's bus symbols. --missed: the
 * addresses the interpreter had to start at inside the executable's code, one hex per line, for ps2_tools.recomp
 * --discovered. Exit status: 0 (the program exited), 1 when the machine stopped on an error or the budget ran out,
 * 2 on bad arguments or input. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ps2_core.h"

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
    const char *path = NULL, *sio = NULL, *pack = NULL, *missed = NULL;
    unsigned long long insns = 200000000ull;
    int stats = 0, verbose = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--insns") && i + 1 < argc)
            insns = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--sio") && i + 1 < argc)
            sio = argv[++i];
        else if (!strcmp(argv[i], "--pack") && i + 1 < argc)
            pack = argv[++i];
        else if (!strcmp(argv[i], "--missed") && i + 1 < argc)
            missed = argv[++i];
        else if (!strcmp(argv[i], "--stats"))
            stats = 1;
        else if (!strcmp(argv[i], "-v"))
            verbose++;
        else if (argv[i][0] != '-' && !path)
            path = argv[i];
        else {
            fprintf(stderr, "usage: ps2_run ELF [--insns N] [--sio OUT] [--pack LIB] [--missed FILE] [--stats] [-v]\n");
            return 2;
        }
    }
    if (!path) {
        fprintf(stderr, "ps2_run: no executable\n");
        return 2;
    }
    size_t size;
    uint8_t *data = read_file(path, &size);
    if (!data) {
        fprintf(stderr, "ps2_run: cannot read %s\n", path);
        return 2;
    }
    char why[200];
    ps2_machine *m = ps2_create(data, size, why, sizeof why);
    free(data);
    if (!m) {
        fprintf(stderr, "ps2_run: %s: %s\n", path, why);
        return 2;
    }
    ps2_set_verbose(m, verbose);
    void *lib = NULL;
    if (pack) {
        lib = dlopen(pack, RTLD_NOW | RTLD_LOCAL);
        ps2_recomp_title_table_fn get = lib ? (ps2_recomp_title_table_fn)dlsym(lib, PS2_RECOMP_ENTRY_SYMBOL) : NULL;
        if (!get || ps2_attach_recomp(m, get())) {
            fprintf(stderr, "ps2_run: pack %s: %s\n", pack, lib ? "no table or ABI mismatch" : dlerror());
            ps2_destroy(m);
            return 2;
        }
    }
    int rc = 0;
    int r = ps2_run(m, insns);
    if (r == 1) {
        fprintf(stderr, "ps2_run: stopped: %s\n", ps2_error(m));
        ps2_dump_trace(m, stderr);
        rc = 1;
    } else if (r == 0) {
        fprintf(stderr, "ps2_run: still running after %llu instructions\n", insns);
        rc = 1;
    } else {
        fprintf(stderr, "ps2_run: exited with %d\n", ps2_exit_code(m));
    }
    if (verbose)
        ps2_dump_trace(m, stderr);
    if (sio) {
        FILE *f = fopen(sio, "w");
        if (!f || fputs(ps2_sio_output(m), f) < 0) {
            fprintf(stderr, "ps2_run: cannot write %s\n", sio);
            rc = 2;
        }
        if (f)
            fclose(f);
    } else {
        fputs(ps2_sio_output(m), stdout);
    }
    if (stats) {
        ps2_recomp_stats s;
        ps2_recomp_get_stats(m, &s);
        printf("stats: native %llu interpreted %llu calls %llu entries %u stale %u\n", (unsigned long long)s.native,
               (unsigned long long)s.interp, (unsigned long long)s.calls, s.entries, s.stale);
    }
    if (missed) {
        size_t n = ps2_recomp_missed(m, NULL, 0);
        uint32_t *list = malloc((n ? n : 1) * sizeof *list);
        FILE *f = fopen(missed, "w");
        if (list && f) {
            ps2_recomp_missed(m, list, n);
            fprintf(f, "# ps2_run --missed: interpreter starts inside the executable's code\n");
            for (size_t i = 0; i < n; i++)
                fprintf(f, "%08X\n", list[i]);
            printf("missed %zu\n", n);
        }
        if (f)
            fclose(f);
        free(list);
    }
    ps2_destroy(m);
    if (lib)
        dlclose(lib);
    return rc;
}
