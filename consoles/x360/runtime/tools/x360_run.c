/* x360_run -- runs an Xbox 360 title, recompiled, headless (XBA-270): what it prints through DbgPrint goes to stdout.
 *
 *   x360_run TITLE.xex --pack LIB.so [--disc IMAGE|DIR] [--data DIR] [--insns N] [--log FILE] [--dbg FILE]
 *            [--missed FILE] [--gpu-trace FILE] [--dump-shaders DIR] [--strict-imports]
 *            [--save-state FILE] [--load-state FILE] [--stats] [-v]
 *
 * TITLE.xex: an unencrypted XEX2, compression none or basic (python3 -m tools.xex FILE --plain OUT makes one).
 * LIB.so: the title's code, x360_tools.recomp's C built with -DXE_RUNTIME -shared (xe_rc_resume, xe_rc_image_crc).
 * --disc: the title's disc (game:, d:, \Device\Cdrom0), a disc image or a directory; --data: a writable directory for
 * cache: / hdd:. --save-state: write a save state when the run stops (budget or end); --load-state: start from one.
 * --pad-script FILE: timed controller states (see load_pad_script). --seconds N: stop after N seconds of the title's virtual time (default 3600; 0: no limit). --strict-imports: stop at the first import without an HLE (by name). Runs until the title ends or N instructions (default 2 billion). --missed: addresses reached without lifted code,
 * for x360_tools.recomp --discovered. Exit status: the title's exit code when it ended (0..255), 1 on an error, a
 * missed address or the budget running out, 2 on bad arguments or input. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "x360_core.h"

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

/* --pad-script FILE: one line per change, "MS USER BUTTONS [LT RT LX LY RX RY]" (BUTTONS in hex, XINPUT bits;
 * the rest decimal) or "MS USER -" to unplug; # starts a comment. Times are virtual milliseconds, in order. */
static int load_pad_script(x360_machine *x, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    x360_pad_event ev[1024];
    unsigned n = 0;
    char line[256];
    while (n < 1024 && fgets(line, sizeof line, f)) {
        char *h = strchr(line, '#');
        if (h)
            *h = 0;
        unsigned ms, user;
        char b[32];
        int lt = 0, rt = 0, lx = 0, ly = 0, rx = 0, ry = 0;
        int got = sscanf(line, "%u %u %31s %d %d %d %d %d %d", &ms, &user, b, &lt, &rt, &lx, &ly, &rx, &ry);
        if (got < 3)
            continue;
        x360_pad_event *e = &ev[n++];
        memset(e, 0, sizeof *e);
        e->ms = ms, e->user = (int)user;
        e->connected = strcmp(b, "-") != 0;
        if (e->connected) {
            e->pad.buttons = (uint16_t)strtoul(b, NULL, 16);
            e->pad.left_trigger = (uint8_t)lt, e->pad.right_trigger = (uint8_t)rt;
            e->pad.thumb_lx = (int16_t)lx, e->pad.thumb_ly = (int16_t)ly;
            e->pad.thumb_rx = (int16_t)rx, e->pad.thumb_ry = (int16_t)ry;
        }
    }
    fclose(f);
    return x360_set_pad_script(x, ev, n);
}

int main(int argc, char **argv)
{
    const char *path = NULL, *pack = NULL, *logp = NULL, *dbgp = NULL, *missedp = NULL, *disc = NULL, *data_dir = NULL,
               *gpu_trace = NULL, *shaders = NULL, *save_state = NULL, *load_state = NULL, *pad_script = NULL;
    unsigned long long insns = 2000000000ull;
    int stats = 0, verbose = 0, strict = 0;
    uint32_t seconds = 3600;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--insns") && i + 1 < argc)
            insns = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--pack") && i + 1 < argc)
            pack = argv[++i];
        else if (!strcmp(argv[i], "--save-state") && i + 1 < argc)
            save_state = argv[++i];
        else if (!strcmp(argv[i], "--load-state") && i + 1 < argc)
            load_state = argv[++i];
        else if (!strcmp(argv[i], "--dump-shaders") && i + 1 < argc)
            shaders = argv[++i];
        else if (!strcmp(argv[i], "--pad-script") && i + 1 < argc)
            pad_script = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
            seconds = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--strict-imports"))
            strict = 1;
        else if (!strcmp(argv[i], "--gpu-trace") && i + 1 < argc)
            gpu_trace = argv[++i];
        else if (!strcmp(argv[i], "--disc") && i + 1 < argc)
            disc = argv[++i];
        else if (!strcmp(argv[i], "--data") && i + 1 < argc)
            data_dir = argv[++i];
        else if (!strcmp(argv[i], "--log") && i + 1 < argc)
            logp = argv[++i];
        else if (!strcmp(argv[i], "--dbg") && i + 1 < argc)
            dbgp = argv[++i];
        else if (!strcmp(argv[i], "--missed") && i + 1 < argc)
            missedp = argv[++i];
        else if (!strcmp(argv[i], "--stats"))
            stats = 1;
        else if (!strcmp(argv[i], "-v"))
            verbose++;
        else if (argv[i][0] != '-' && !path)
            path = argv[i];
        else {
            fprintf(stderr, "usage: x360_run TITLE.xex --pack LIB.so [--disc IMAGE|DIR] [--data DIR] [--insns N] "
                            "[--log FILE] [--dbg FILE] [--missed FILE] [--stats] [-v]\n");
            return 2;
        }
    }
    if (!path || !pack) {
        fprintf(stderr, "x360_run: a title and its pack (--pack) are needed\n");
        return 2;
    }
    size_t size;
    uint8_t *data = read_file(path, &size);
    if (!data) {
        fprintf(stderr, "x360_run: cannot read %s\n", path);
        return 2;
    }
    char why[256] = "";
    x360_machine *x = x360_create(data, size, why, sizeof why);
    free(data);
    if (!x) {
        fprintf(stderr, "x360_run: %s: %s\n", path, why);
        return 2;
    }
    FILE *log = logp ? fopen(logp, "w") : stderr;
    FILE *dbg = dbgp ? fopen(dbgp, "w") : stdout;
    if (!log || !dbg) {
        fprintf(stderr, "x360_run: cannot open the output files\n");
        return 2;
    }
    x360_set_output(x, dbg, log, verbose);
    x360_set_strict_imports(x, strict);
    x360_set_time_limit(x, seconds);
    if (pad_script && load_pad_script(x, pad_script)) {
        fprintf(stderr, "x360_run: %s: cannot read the pad script\n", pad_script);
        return 2;
    }
    if (disc && x360_set_disc(x, disc, why, sizeof why)) {
        fprintf(stderr, "x360_run: %s\n", why);
        return 2;
    }
    if (data_dir)
        x360_set_data_dir(x, data_dir);
    FILE *gtrace = gpu_trace ? fopen(gpu_trace, "w") : NULL;
    if (gtrace)
        x360_set_gpu_trace(x, gtrace);
    if (shaders)
        x360_set_shader_dump(x, shaders);
    void *lib = dlopen(pack, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        fprintf(stderr, "x360_run: %s\n", dlerror());
        return 2;
    }
    const void *resume = dlsym(lib, "xe_rc_resume");
    const unsigned *count = dlsym(lib, "xe_rc_resume_count");
    const uint32_t *crc = dlsym(lib, "xe_rc_image_crc");
    if (!resume || !count || !crc) {
        fprintf(stderr, "x360_run: %s is not an x360_tools.recomp pack\n", pack);
        return 2;
    }
    x360_pack p = {resume, *count, *crc};
    if (x360_bind(x, &p)) {
        fprintf(stderr, "x360_run: %s\n", x360_error(x));
        return 2;
    }
    if (load_state) {
        size_t n;
        uint8_t *blob = read_file(load_state, &n);
        char why2[256] = "";
        int lr = blob ? x360_state_load(x, NULL, blob, n, why2, sizeof why2) : X360_STATE_ECORRUPT;
        free(blob);
        if (lr != X360_STATE_OK) {
            fprintf(stderr, "x360_run: %s: %s\n", load_state, blob ? why2 : "cannot read");
            return 2;
        }
        fprintf(log, "x360_run: state loaded from %s\n", load_state);
    }
    int st = x360_run(x, insns);
    if (save_state && (st == X360_BUDGET || st == X360_EXITED)) {
        size_t cap = x360_state_size(x);
        uint8_t *blob = malloc(cap);
        size_t n = blob ? x360_state_save(x, NULL, blob, cap) : 0;
        FILE *f = n ? fopen(save_state, "wb") : NULL;
        if (!f || fwrite(blob, 1, n, f) != n) {
            fprintf(stderr, "x360_run: cannot write the state to %s\n", save_state);
            st = X360_ERROR;
        } else
            fprintf(log, "x360_run: state saved to %s (%zu bytes)\n", save_state, n);
        if (f)
            fclose(f);
        free(blob);
    }
    x360_stats s;
    x360_get_stats(x, &s);
    if (stats)
        fprintf(log, "stats: native %llu interpreted %llu hle %llu switches %llu threads %u unimplemented %u "
                     "gpu packets %llu draws %llu swaps %llu vblanks %u\n",
                (unsigned long long)s.native, (unsigned long long)s.interpreted, (unsigned long long)s.hle_calls,
                (unsigned long long)s.switches, s.threads, s.unimplemented, (unsigned long long)s.gpu_packets,
                (unsigned long long)s.gpu_draws, (unsigned long long)s.gpu_swaps, s.vblanks);
    if (stats)
        x360_gpu_opcode_histogram(x, log);
    if (missedp) {
        FILE *f = fopen(missedp, "w");
        if (f) {
            const uint32_t *m;
            unsigned n = x360_missed(x, &m);
            fprintf(f, "# x360_run: addresses reached without lifted code\n");
            for (unsigned k = 0; k < n; k++)
                fprintf(f, "%08X\n", m[k]);
            fclose(f);
        }
    }
    int rc;
    if (st == X360_EXITED) {
        fprintf(log, "x360_run: the title ended (exit code %u)\n", x360_exit_code(x));
        rc = (int)(x360_exit_code(x) & 0xFF);
    } else {
        fprintf(log, "x360_run: %s\n", st == X360_BUDGET ? "instruction or time budget ran out" : x360_error(x));
        rc = 1;
    }
    fflush(dbg);
    x360_destroy(x);
    dlclose(lib);
    return rc;
}
